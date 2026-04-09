#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace core {

class ProfilerService;

struct TaskSchedulerConfig {
    std::size_t workerCount{0};
};

/**
 * Tracks frame-bound work that must finish before the caller continues.
 */
class TaskGroup {
public:
    /**
     * Returns true when all tasks scheduled into the group have completed.
     */
    [[nodiscard]] bool empty() const {
        return pending_.load(std::memory_order_acquire) == 0u;
    }

private:
    friend class TaskScheduler;

    std::atomic<std::size_t> pending_{0u};
};

namespace detail {

template <typename T>
struct AsyncTaskState {
    mutable std::mutex mutex{};
    std::optional<T> value{};
    std::exception_ptr error{};
    bool ready{false};
    bool taken{false};
};

template <>
struct AsyncTaskState<void> {
    mutable std::mutex mutex{};
    std::exception_ptr error{};
    bool ready{false};
    bool taken{false};
};

}  // namespace detail

template <typename T>
class AsyncTaskHandle {
public:
    AsyncTaskHandle() = default;
    explicit AsyncTaskHandle(std::shared_ptr<detail::AsyncTaskState<T>> state)
        : state_(std::move(state)) {}

    [[nodiscard]] bool valid() const {
        return static_cast<bool>(state_);
    }

    [[nodiscard]] bool ready() const {
        if (!state_) {
            return false;
        }

        std::lock_guard lock(state_->mutex);
        return state_->ready;
    }

    [[nodiscard]] std::optional<T> take() {
        if (!state_) {
            return std::nullopt;
        }

        std::lock_guard lock(state_->mutex);
        if (!state_->ready || state_->taken) {
            return std::nullopt;
        }

        state_->taken = true;
        if (state_->error) {
            std::rethrow_exception(state_->error);
        }

        std::optional<T> value = std::move(state_->value);
        state_.reset();
        return value;
    }

private:
    std::shared_ptr<detail::AsyncTaskState<T>> state_{};
};

template <>
class AsyncTaskHandle<void> {
public:
    AsyncTaskHandle() = default;
    explicit AsyncTaskHandle(std::shared_ptr<detail::AsyncTaskState<void>> state)
        : state_(std::move(state)) {}

    [[nodiscard]] bool valid() const {
        return static_cast<bool>(state_);
    }

    [[nodiscard]] bool ready() const {
        if (!state_) {
            return false;
        }

        std::lock_guard lock(state_->mutex);
        return state_->ready;
    }

    bool take() {
        if (!state_) {
            return false;
        }

        std::lock_guard lock(state_->mutex);
        if (!state_->ready || state_->taken) {
            return false;
        }

        state_->taken = true;
        if (state_->error) {
            std::rethrow_exception(state_->error);
        }

        state_.reset();
        return true;
    }

private:
    std::shared_ptr<detail::AsyncTaskState<void>> state_{};
};

class TaskScheduler {
public:
    /**
     * Creates the worker pool used by frame and background tasks.
     */
    explicit TaskScheduler(TaskSchedulerConfig config = {});
    ~TaskScheduler();

    TaskScheduler(const TaskScheduler&) = delete;
    TaskScheduler& operator=(const TaskScheduler&) = delete;

    /**
     * Enables CPU profiler scopes for named frame tasks.
     */
    void setProfiler(ProfilerService* profiler);
    [[nodiscard]] std::size_t workerCount() const { return workers_.size(); }

    /**
     * Queues work for the current frame.
     * Long tasks still stall the frame once the caller waits on the group.
     */
    template <typename Fn>
    void schedule(TaskGroup& group, std::string name, Fn&& fn) {
        enqueueTask(TaskKind::Frame, &group, std::move(name), std::forward<Fn>(fn), true);
    }

    /**
     * Splits a frame task into chunks and schedules them into the same group.
     */
    template <typename Fn>
    void parallelFor(TaskGroup& group, std::size_t count, std::size_t grain, std::string name, Fn&& fn) {
        if (count == 0u) {
            return;
        }

        const std::size_t chunkSize = std::max<std::size_t>(1u, grain);
        for (std::size_t begin = 0u; begin < count; begin += chunkSize) {
            const std::size_t end = std::min(count, begin + chunkSize);
            schedule(group, name, [begin, end, fn = std::forward<Fn>(fn)]() mutable {
                fn(begin, end);
            });
        }
    }

    /**
     * Blocks until a frame task group is done.
     * Waits on the group's counter directly to avoid the missed queue wake that caused startup stalls.
     */
    void wait(TaskGroup& group);

    /**
     * Queues background work that may complete in later frames.
     * Use this when the result is not required before the current frame presents.
     */
    template <typename Fn>
    auto submitAsync(std::string name, Fn&& fn) {
        using Result = std::invoke_result_t<std::decay_t<Fn>>;
        auto state = std::make_shared<detail::AsyncTaskState<Result>>();
        enqueueTask(TaskKind::Background, nullptr, std::move(name), [state, fn = std::forward<Fn>(fn)]() mutable {
            if constexpr (std::is_void_v<Result>) {
                try {
                    fn();
                    std::lock_guard lock(state->mutex);
                    state->ready = true;
                } catch (...) {
                    std::lock_guard lock(state->mutex);
                    state->error = std::current_exception();
                    state->ready = true;
                }
            } else {
                try {
                    Result value = fn();
                    std::lock_guard lock(state->mutex);
                    state->value = std::move(value);
                    state->ready = true;
                } catch (...) {
                    std::lock_guard lock(state->mutex);
                    state->error = std::current_exception();
                    state->ready = true;
                }
            }
        }, false);
        return AsyncTaskHandle<Result>(std::move(state));
    }

private:
    enum class TaskKind {
        Frame,
        Background,
    };

    struct QueuedTask {
        TaskKind kind{TaskKind::Frame};
        TaskGroup* group{nullptr};
        std::string name{};
        std::function<void()> fn{};
        bool profile{false};
    };

    struct TaskFailure {
        std::exception_ptr error{};
        std::string taskName{};
    };

    template <typename Fn>
    void enqueueTask(TaskKind kind, TaskGroup* group, std::string name, Fn&& fn, bool profile) {
        if (group != nullptr) {
            group->pending_.fetch_add(1u, std::memory_order_release);
        }

        {
            std::lock_guard lock(queueMutex_);
            queueFor(kind).push_back(QueuedTask{
                kind,
                group,
                std::move(name),
                std::forward<Fn>(fn),
                profile
            });
        }
        queueCv_.notify_one();
    }

    [[nodiscard]] std::deque<QueuedTask>& queueFor(TaskKind kind);
    bool tryExecuteQueuedTask(bool allowBackground);
    void executeTask(QueuedTask task);
    void workerLoop();
    void recordFailure(std::exception_ptr error, std::string taskName);
    [[nodiscard]] std::optional<TaskFailure> consumeFailure();

    TaskSchedulerConfig config_{};
    ProfilerService* profiler_{nullptr};
    std::mutex queueMutex_{};
    std::condition_variable queueCv_{};
    std::deque<QueuedTask> frameQueue_{};
    std::deque<QueuedTask> backgroundQueue_{};
    bool stopRequested_{false};
    std::vector<std::thread> workers_{};
    std::mutex failureMutex_{};
    std::optional<TaskFailure> pendingFailure_{};
};

}  // namespace core
