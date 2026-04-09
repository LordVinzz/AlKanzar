#include "TaskScheduler.hpp"

#include <chrono>
#include <cstdlib>
#include <string_view>

#include "ProfilerService.hpp"
#include <spdlog/spdlog.h>

namespace core {

namespace {

std::size_t resolveWorkerCount(const TaskSchedulerConfig& config) {
    if (config.workerCount > 0u) {
        return config.workerCount;
    }

    const unsigned int hardwareThreads = std::thread::hardware_concurrency();
    if (hardwareThreads <= 1u) {
        return 1u;
    }
    return static_cast<std::size_t>(hardwareThreads - 1u);
}

bool readBoolEnv(const char* name) {
    const char* value = std::getenv(name);
    if (value == nullptr) {
        return false;
    }

    const std::string_view token(value);
    return !token.empty() &&
        token != "0" &&
        token != "false" &&
        token != "FALSE" &&
        token != "off" &&
        token != "OFF" &&
        token != "no" &&
        token != "NO";
}

double readDoubleEnv(const char* name, double fallback) {
    const char* value = std::getenv(name);
    if (value == nullptr || *value == '\0') {
        return fallback;
    }

    char* end = nullptr;
    const double parsed = std::strtod(value, &end);
    if (end == value || (end != nullptr && *end != '\0')) {
        return fallback;
    }
    return parsed;
}

bool shouldLogTaskDuration(double durationMs) {
    static const bool logAllTasks = readBoolEnv("ALKANZAR_LOG_SCHEDULER_TASKS");
    static const double slowTaskThresholdMs = readDoubleEnv("ALKANZAR_LOG_SCHEDULER_TASK_THRESHOLD_MS", 100.0);
    return logAllTasks || durationMs >= slowTaskThresholdMs;
}

bool shouldLogTaskStarts() {
    static const bool logTaskStarts = readBoolEnv("ALKANZAR_LOG_SCHEDULER_TASK_STARTS");
    return logTaskStarts;
}

bool shouldLogSchedulerWaits() {
    static const bool logSchedulerWaits = readBoolEnv("ALKANZAR_LOG_SCHEDULER_WAITS");
    return logSchedulerWaits;
}

std::string describeCurrentException() {
    try {
        throw;
    } catch (const std::exception& error) {
        return error.what();
    } catch (...) {
        return "non-std exception";
    }
}

}  // namespace

TaskScheduler::TaskScheduler(TaskSchedulerConfig config)
    : config_(config) {
    const std::size_t resolvedWorkerCount = resolveWorkerCount(config_);
    workers_.reserve(resolvedWorkerCount);
    for (std::size_t index = 0u; index < resolvedWorkerCount; ++index) {
        workers_.emplace_back([this]() { workerLoop(); });
    }
}

TaskScheduler::~TaskScheduler() {
    {
        std::lock_guard lock(queueMutex_);
        stopRequested_ = true;
    }
    queueCv_.notify_all();

    for (std::thread& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
}

void TaskScheduler::setProfiler(ProfilerService* profiler) {
    profiler_ = profiler;
}

void TaskScheduler::wait(TaskGroup& group) {
    const bool logWaits = shouldLogSchedulerWaits();
    while (!group.empty()) {
        if (tryExecuteQueuedTask(false)) {
            continue;
        }

        const std::size_t pending = group.pending_.load(std::memory_order_acquire);
        if (pending == 0u) {
            break;
        }

        if (logWaits) {
            std::size_t frameQueueSize = 0u;
            std::size_t backgroundQueueSize = 0u;
            {
                std::lock_guard lock(queueMutex_);
                frameQueueSize = frameQueue_.size();
                backgroundQueueSize = backgroundQueue_.size();
            }

            spdlog::info(
                "TaskScheduler: wait pending={} frame_queue={} background_queue={}",
                pending,
                frameQueueSize,
                backgroundQueueSize
            );
        }

        group.pending_.wait(pending, std::memory_order_acquire);
    }

    if (std::optional<TaskFailure> failure = consumeFailure()) {
        try {
            std::rethrow_exception(failure->error);
        } catch (const std::exception& error) {
            throw std::runtime_error("TaskScheduler task '" + failure->taskName + "' failed: " + error.what());
        } catch (...) {
            throw std::runtime_error("TaskScheduler task '" + failure->taskName + "' failed with a non-std exception");
        }
    }
}

std::deque<TaskScheduler::QueuedTask>& TaskScheduler::queueFor(TaskKind kind) {
    return kind == TaskKind::Frame ? frameQueue_ : backgroundQueue_;
}

bool TaskScheduler::tryExecuteQueuedTask(bool allowBackground) {
    QueuedTask task{};
    {
        std::lock_guard lock(queueMutex_);
        if (!frameQueue_.empty()) {
            task = std::move(frameQueue_.front());
            frameQueue_.pop_front();
        } else if (allowBackground && !backgroundQueue_.empty()) {
            task = std::move(backgroundQueue_.front());
            backgroundQueue_.pop_front();
        } else {
            return false;
        }
    }

    executeTask(std::move(task));
    return true;
}

void TaskScheduler::executeTask(QueuedTask task) {
    const auto startedAt = std::chrono::steady_clock::now();
    std::exception_ptr failure{};
    if (shouldLogTaskStarts()) {
        spdlog::info(
            "TaskScheduler: task '{}' start",
            task.name.empty() ? "<unnamed>" : task.name
        );
    }

    try {
        if (task.profile && profiler_ != nullptr && !task.name.empty()) {
            auto scope = profiler_->scopedCpu(task.name.c_str());
            (void)scope;
            task.fn();
        } else {
            task.fn();
        }
    } catch (...) {
        failure = std::current_exception();
        recordFailure(failure, task.name);
    }

    const auto finishedAt = std::chrono::steady_clock::now();
    const double durationMs =
        std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(finishedAt - startedAt).count();
    if (shouldLogTaskDuration(durationMs)) {
        spdlog::info("TaskScheduler: task '{}' finished in {:.3f} ms", task.name, durationMs);
    }
    if (failure != nullptr) {
        try {
            std::rethrow_exception(failure);
        } catch (...) {
            spdlog::error(
                "TaskScheduler: task '{}' threw {}",
                task.name.empty() ? "<unnamed>" : task.name,
                describeCurrentException()
            );
        }
    }

    if (task.group != nullptr) {
        task.group->pending_.fetch_sub(1u, std::memory_order_acq_rel);
        task.group->pending_.notify_all();
    }
}

void TaskScheduler::workerLoop() {
    while (true) {
        QueuedTask task{};
        {
            std::unique_lock lock(queueMutex_);
            queueCv_.wait(lock, [this]() {
                return stopRequested_ || !frameQueue_.empty() || !backgroundQueue_.empty();
            });

            if (frameQueue_.empty() && backgroundQueue_.empty() && stopRequested_) {
                return;
            }

            if (!frameQueue_.empty()) {
                task = std::move(frameQueue_.front());
                frameQueue_.pop_front();
            } else if (!backgroundQueue_.empty()) {
                task = std::move(backgroundQueue_.front());
                backgroundQueue_.pop_front();
            } else {
                continue;
            }
        }

        executeTask(std::move(task));
    }
}

void TaskScheduler::recordFailure(std::exception_ptr error, std::string taskName) {
    if (error == nullptr) {
        return;
    }

    std::lock_guard lock(failureMutex_);
    if (!pendingFailure_.has_value()) {
        pendingFailure_ = TaskFailure{error, std::move(taskName)};
    }
}

std::optional<TaskScheduler::TaskFailure> TaskScheduler::consumeFailure() {
    std::lock_guard lock(failureMutex_);
    std::optional<TaskFailure> failure = std::move(pendingFailure_);
    pendingFailure_.reset();
    return failure;
}

}  // namespace core
