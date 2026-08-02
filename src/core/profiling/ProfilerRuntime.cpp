#include "ProfilerService.hpp"

#ifndef GL_GLEXT_PROTOTYPES
#define GL_GLEXT_PROTOTYPES
#endif

#include <SDL_opengl.h>

#include <algorithm>
#include <chrono>
#include <functional>
#include <thread>
#include <unordered_map>
#include <utility>

namespace core {
namespace {

using Clock = std::chrono::steady_clock;

std::uint64_t nowNs() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count()
    );
}

double nsToMs(std::uint64_t durationNs) {
    return static_cast<double>(durationNs) / 1'000'000.0;
}

std::uint64_t currentThreadIdValue() {
    return static_cast<std::uint64_t>(std::hash<std::thread::id>{}(std::this_thread::get_id()));
}

struct ThreadCpuTlsEntry {
    void* state{nullptr};
    std::uint64_t frameSerial{0};
    std::uint64_t instanceId{0};
};

thread_local std::unordered_map<const ProfilerService*, ThreadCpuTlsEntry> g_threadCpuTls{};

}  // namespace

void ProfilerService::recordProfilerUiTime(double durationMs) {
    if (!currentFrameActive_.load(std::memory_order_acquire)) {
        return;
    }
    currentProfilerUiMs_ += std::max(durationMs, 0.0);
}

ProfilerService::CpuScopeHandle ProfilerService::scopedCpu(const char* name) {
    const CpuScopeToken token = pushCpuScope(name);
    return CpuScopeHandle(token.index == ProfilerRecordedScope::kRoot ? nullptr : this, token.state, token.index);
}

ProfilerService::GpuScopeHandle ProfilerService::scopedGpu(const char* name) {
    const std::uint32_t index = beginGpuScope(name);
    return GpuScopeHandle(index == ProfilerRecordedScope::kRoot ? nullptr : this, index);
}

ProfilerStats ProfilerService::stats() const {
    ProfilerStats snapshot{};
    snapshot.capturing = capturing_;
    snapshot.startPending = startRequested_;
    snapshot.stopPending = stopRequested_;
    snapshot.pendingGpuFrames = pendingGpuFrames_.size();
    snapshot.droppedCpuScopes = droppedCpuScopes_.load(std::memory_order_acquire);
    snapshot.droppedGpuScopes = droppedGpuScopes_;
    snapshot.droppedFrames = droppedFrames_;
    snapshot.currentFrameNumber = currentFrameNumber_;
    {
        std::lock_guard lock(snapshotMutex_);
        snapshot.bufferedFrames = snapshots_.size();
    }
    return snapshot;
}

std::vector<std::shared_ptr<const ProfilerFrameSnapshot>> ProfilerService::snapshots() const {
    std::lock_guard lock(snapshotMutex_);
    return std::vector<std::shared_ptr<const ProfilerFrameSnapshot>>(snapshots_.begin(), snapshots_.end());
}

ProfilerTraceCapture ProfilerService::rawCapture() const {
    std::lock_guard lock(snapshotMutex_);

    ProfilerTraceCapture capture{};
    capture.mainThreadId = mainThreadId_;
    capture.frames.assign(rawFrames_.begin(), rawFrames_.end());
    capture.sessionId = capture.frames.empty() ? activeSessionId_.load() : capture.frames.back().sessionId;
    return capture;
}

bool ProfilerService::exportPerfettoTrace(const std::string& path, std::string* error) {
    waitForWorkerIdle();
    return exportProfilerTraceCaptureToPerfetto(rawCapture(), path, error);
}

void ProfilerService::waitForWorkerIdle() {
    std::unique_lock lock(requestMutex_);
    idleCv_.wait(lock, [this]() {
        return requestQueue_.empty() && processedRequestId_ >= enqueuedRequestId_;
    });
}

ProfilerService::CpuScopeToken ProfilerService::pushCpuScope(const char* name) {
    if (!currentFrameActive_.load(std::memory_order_acquire) || name == nullptr) {
        return {};
    }

    const std::size_t reservation = reservedCpuScopes_.fetch_add(1u, std::memory_order_acq_rel);
    if (reservation >= config_.maxCpuScopesPerFrame) {
        droppedCpuScopes_.fetch_add(1u, std::memory_order_acq_rel);
        return {};
    }

    ThreadCpuTlsEntry& tls = g_threadCpuTls[this];
    if (tls.instanceId != instanceId_) {
        tls = ThreadCpuTlsEntry{};
        tls.instanceId = instanceId_;
    }
    const std::uint64_t frameSerial = cpuFrameSerial_.load(std::memory_order_acquire);
    ThreadCpuState* state = static_cast<ThreadCpuState*>(tls.state);
    if (state == nullptr) {
        std::lock_guard lock(cpuStateMutex_);
        currentCpuThreadStates_.push_back(std::make_unique<ThreadCpuState>());
        state = currentCpuThreadStates_.back().get();
        state->threadId = currentThreadIdValue();
        tls.state = state;
    }

    if (tls.frameSerial != frameSerial) {
        state->frameSerial = frameSerial;
        state->scopes.clear();
        state->stack.clear();
        state->scopes.reserve(config_.maxCpuScopesPerFrame);
        state->stack.reserve(config_.maxCpuScopesPerFrame);
        tls.frameSerial = frameSerial;
    }

    const std::uint32_t index = static_cast<std::uint32_t>(state->scopes.size());
    state->scopes.push_back(RawCpuScope{
        name,
        state->stack.empty() ? ProfilerRecordedScope::kRoot : state->stack.back(),
        nowNs(),
        0u,
        state->threadId
    });
    state->stack.push_back(index);
    return CpuScopeToken{state, index};
}

void ProfilerService::popCpuScope(void* statePtr, std::uint32_t index) {
    if (statePtr == nullptr || index == ProfilerRecordedScope::kRoot) {
        return;
    }

    auto* state = static_cast<ThreadCpuState*>(statePtr);
    if (index >= state->scopes.size()) {
        return;
    }

    state->scopes[index].endNs = nowNs();
    if (!state->stack.empty() && state->stack.back() == index) {
        state->stack.pop_back();
    }
}

std::uint32_t ProfilerService::beginGpuScope(const char* name) {
    if (!currentFrameActive_.load(std::memory_order_acquire) || name == nullptr) {
        return ProfilerRecordedScope::kRoot;
    }
    if (gpuScopeOpen_ || currentGpuPasses_.size() >= config_.maxGpuScopesPerFrame) {
        ++droppedGpuScopes_;
        return ProfilerRecordedScope::kRoot;
    }

    unsigned int queryId = 0;
    if (!freeGpuQueries_.empty()) {
        queryId = freeGpuQueries_.back();
        freeGpuQueries_.pop_back();
    } else {
        glGenQueries(1, &queryId);
    }

    if (queryId == 0) {
        ++droppedGpuScopes_;
        return ProfilerRecordedScope::kRoot;
    }

    const std::uint32_t index = static_cast<std::uint32_t>(currentGpuPasses_.size());
    currentGpuPasses_.push_back(RawGpuPassState{name, queryId});
    glBeginQuery(GL_TIME_ELAPSED, queryId);
    gpuScopeOpen_ = true;
    return index;
}

void ProfilerService::endGpuScope(std::uint32_t index) {
    if (!currentFrameActive_.load(std::memory_order_acquire) ||
        index == ProfilerRecordedScope::kRoot ||
        index >= currentGpuPasses_.size() ||
        !gpuScopeOpen_) {
        return;
    }

    glEndQuery(GL_TIME_ELAPSED);
    gpuScopeOpen_ = false;
}

void ProfilerService::applyStartCapture() {
    activeSessionId_.fetch_add(1u);
    capturing_ = true;
    startRequested_ = false;
    stopRequested_ = false;
    droppedCpuScopes_.store(0u, std::memory_order_release);
    reservedCpuScopes_.store(0u, std::memory_order_release);
    droppedGpuScopes_ = 0;
    droppedFrames_ = 0;
    clearPendingGpuFrames();

    {
        std::lock_guard lock(snapshotMutex_);
        snapshots_.clear();
        rawFrames_.clear();
    }
    {
        std::lock_guard lock(requestMutex_);
        requestQueue_.clear();
        processedRequestId_ = enqueuedRequestId_;
    }
}

void ProfilerService::pollPendingGpuFrames() {
    while (!pendingGpuFrames_.empty()) {
        RawFrameData& pendingFrame = pendingGpuFrames_.front().frame;
        if (pendingFrame.frameNumber + config_.gpuReadbackDelayFrames > currentFrameNumber_) {
            break;
        }

        bool allResolved = true;
        bool anyResolved = false;
        for (RawGpuPass& pass : pendingFrame.gpuPasses) {
            if (!pass.pending || pass.queryId == 0) {
                continue;
            }

            GLuint available = 0;
            glGetQueryObjectuiv(pass.queryId, GL_QUERY_RESULT_AVAILABLE, &available);
            if (available == 0) {
                allResolved = false;
                continue;
            }

            GLuint64 elapsedNs = 0;
            glGetQueryObjectui64v(pass.queryId, GL_QUERY_RESULT, &elapsedNs);
            pass.durationMs = nsToMs(static_cast<std::uint64_t>(elapsedNs));
            pass.available = true;
            pass.pending = false;
            recycleGpuQuery(pass.queryId);
            pass.queryId = 0;
            anyResolved = true;
        }

        if (anyResolved || allResolved) {
            enqueueBuildRequest(BuildRequest{0u, pendingFrame});
        }
        if (!allResolved) {
            break;
        }

        pendingGpuFrames_.pop_front();
    }
}

void ProfilerService::clearPendingGpuFrames() {
    for (RawGpuFrame& pendingFrame : pendingGpuFrames_) {
        for (RawGpuPass& pass : pendingFrame.frame.gpuPasses) {
            if (pass.queryId != 0) {
                recycleGpuQuery(pass.queryId);
                pass.queryId = 0;
            }
        }
    }
    pendingGpuFrames_.clear();
}

void ProfilerService::recycleGpuQuery(unsigned int queryId) {
    if (queryId != 0) {
        freeGpuQueries_.push_back(queryId);
    }
}

void ProfilerService::enqueueBuildRequest(BuildRequest request) {
    std::lock_guard lock(requestMutex_);
    request.requestId = ++enqueuedRequestId_;
    requestQueue_.push_back(std::move(request));
    requestCv_.notify_one();
}

void ProfilerService::workerLoop() {
    while (true) {
        BuildRequest request{};
        {
            std::unique_lock lock(requestMutex_);
            requestCv_.wait(lock, [this]() {
                return workerStopRequested_ || !requestQueue_.empty();
            });
            if (workerStopRequested_ && requestQueue_.empty()) {
                return;
            }

            request = std::move(requestQueue_.front());
            requestQueue_.pop_front();
        }

        if (request.frame.sessionId == activeSessionId_.load()) {
            auto snapshot = std::make_shared<ProfilerFrameSnapshot>(buildSnapshot(request.frame, mainThreadId_));
            ProfilerTraceFrame traceFrame = buildTraceFrame(request.frame);
            std::lock_guard lock(snapshotMutex_);
            auto existing = std::find_if(snapshots_.begin(), snapshots_.end(), [&](const auto& candidate) {
                return candidate->sessionId == snapshot->sessionId && candidate->frameNumber == snapshot->frameNumber;
            });
            if (existing != snapshots_.end()) {
                *existing = snapshot;
            } else {
                snapshots_.push_back(snapshot);
                while (snapshots_.size() > config_.maxFrames) {
                    snapshots_.pop_front();
                }
            }

            auto rawExisting = std::find_if(rawFrames_.begin(), rawFrames_.end(), [&](const ProfilerTraceFrame& candidate) {
                return candidate.sessionId == traceFrame.sessionId && candidate.frameNumber == traceFrame.frameNumber;
            });
            if (rawExisting != rawFrames_.end()) {
                *rawExisting = std::move(traceFrame);
            } else {
                rawFrames_.push_back(std::move(traceFrame));
                while (rawFrames_.size() > config_.maxFrames) {
                    rawFrames_.pop_front();
                }
            }
        }

        {
            std::lock_guard lock(requestMutex_);
            processedRequestId_ = std::max(processedRequestId_, request.requestId);
            if (requestQueue_.empty() && processedRequestId_ >= enqueuedRequestId_) {
                idleCv_.notify_all();
            }
        }
    }
}

}  // namespace core

