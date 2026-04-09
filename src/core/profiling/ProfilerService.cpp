#include "ProfilerService.hpp"

#ifndef GL_GLEXT_PROTOTYPES
#define GL_GLEXT_PROTOTYPES
#endif

#include <SDL_opengl.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <sstream>
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

std::uint64_t nextProfilerInstanceId() {
    static std::atomic<std::uint64_t> nextId{1u};
    return nextId.fetch_add(1u, std::memory_order_acq_rel);
}

std::uint64_t currentThreadIdValue() {
    return static_cast<std::uint64_t>(std::hash<std::thread::id>{}(std::this_thread::get_id()));
}

std::string threadLabel(std::uint64_t threadId, std::uint64_t mainThreadId) {
    if (threadId == mainThreadId) {
        return "Main";
    }

    std::ostringstream stream;
    stream << "T" << threadId;
    return stream.str();
}

struct ThreadCpuTlsEntry {
    void* state{nullptr};
    std::uint64_t frameSerial{0};
    std::uint64_t instanceId{0};
};

thread_local std::unordered_map<const ProfilerService*, ThreadCpuTlsEntry> g_threadCpuTls{};

struct AggregatedScopeNode {
    std::string name{};
    double totalMs{0.0};
    double selfMs{0.0};
    std::uint32_t callCount{0};
    std::string thread{};
    std::vector<AggregatedScopeNode> children{};
};

ProfilerScopeNode toSnapshotNode(AggregatedScopeNode node) {
    ProfilerScopeNode snapshot{};
    snapshot.name = std::move(node.name);
    snapshot.totalMs = node.totalMs;
    snapshot.selfMs = node.selfMs;
    snapshot.callCount = node.callCount;
    snapshot.thread = std::move(node.thread);
    snapshot.children.reserve(node.children.size());
    for (AggregatedScopeNode& child : node.children) {
        snapshot.children.push_back(toSnapshotNode(std::move(child)));
    }
    return snapshot;
}

AggregatedScopeNode* findOrAppendNode(
    std::vector<AggregatedScopeNode>& nodes,
    const std::string& name,
    const std::string& thread
) {
    for (AggregatedScopeNode& node : nodes) {
        if (node.name == name && node.thread == thread) {
            return &node;
        }
    }

    nodes.push_back(AggregatedScopeNode{
        name,
        0.0,
        0.0,
        0u,
        thread,
        {}
    });
    return &nodes.back();
}

void accumulateScopeTree(
    std::uint32_t scopeIndex,
    const std::vector<ProfilerRecordedScope>& scopes,
    const std::vector<std::vector<std::uint32_t>>& childrenByParent,
    const std::vector<double>& selfMs,
    std::vector<AggregatedScopeNode>& destination,
    std::uint64_t mainThreadId
) {
    const ProfilerRecordedScope& scope = scopes[scopeIndex];
    const std::uint64_t durationNs = scope.endNs > scope.startNs ? scope.endNs - scope.startNs : 0u;
    AggregatedScopeNode* target = findOrAppendNode(
        destination,
        scope.name,
        threadLabel(scope.threadId, mainThreadId)
    );
    target->totalMs += nsToMs(durationNs);
    target->selfMs += selfMs[scopeIndex];
    target->callCount += 1u;

    for (std::uint32_t childIndex : childrenByParent[scopeIndex]) {
        accumulateScopeTree(childIndex, scopes, childrenByParent, selfMs, target->children, mainThreadId);
    }
}

}  // namespace

std::vector<ProfilerScopeNode> buildProfilerScopeTree(
    const std::vector<ProfilerRecordedScope>& scopes,
    std::uint64_t mainThreadId
) {
    if (scopes.empty()) {
        return {};
    }

    std::vector<std::vector<std::uint32_t>> childrenByParent(scopes.size());
    std::vector<double> selfMs(scopes.size(), 0.0);
    std::vector<std::uint32_t> roots{};
    roots.reserve(scopes.size());

    for (std::size_t index = 0; index < scopes.size(); ++index) {
        const ProfilerRecordedScope& scope = scopes[index];
        const std::uint64_t durationNs = scope.endNs > scope.startNs ? scope.endNs - scope.startNs : 0u;
        selfMs[index] = nsToMs(durationNs);
        if (scope.parentIndex == ProfilerRecordedScope::kRoot || scope.parentIndex >= scopes.size()) {
            roots.push_back(static_cast<std::uint32_t>(index));
        } else {
            childrenByParent[scope.parentIndex].push_back(static_cast<std::uint32_t>(index));
            selfMs[scope.parentIndex] -= selfMs[index];
        }
    }

    for (double& value : selfMs) {
        value = std::max(value, 0.0);
    }

    std::vector<AggregatedScopeNode> aggregatedRoots{};
    aggregatedRoots.reserve(roots.size());
    for (std::uint32_t rootIndex : roots) {
        accumulateScopeTree(rootIndex, scopes, childrenByParent, selfMs, aggregatedRoots, mainThreadId);
    }

    std::vector<ProfilerScopeNode> snapshotRoots{};
    snapshotRoots.reserve(aggregatedRoots.size());
    for (AggregatedScopeNode& root : aggregatedRoots) {
        snapshotRoots.push_back(toSnapshotNode(std::move(root)));
    }
    return snapshotRoots;
}

ProfilerFrameSnapshot ProfilerService::buildSnapshot(const RawFrameData& rawFrame, std::uint64_t mainThreadId) {
    ProfilerFrameSnapshot snapshot{};
    snapshot.sessionId = rawFrame.sessionId;
    snapshot.frameNumber = rawFrame.frameNumber;
    const std::uint64_t frameDurationNs = rawFrame.endNs > rawFrame.startNs ? rawFrame.endNs - rawFrame.startNs : 0u;
    snapshot.cpuFrameMs = std::max(nsToMs(frameDurationNs) - rawFrame.profilerUiMs, 0.0);
    snapshot.profilerUiMs = rawFrame.profilerUiMs;
    snapshot.cpuScopes = buildProfilerScopeTree(rawFrame.cpuScopes, mainThreadId);
    snapshot.gpuPasses.reserve(rawFrame.gpuPasses.size());
    snapshot.resources.reserve(rawFrame.resources.size());

    snapshot.gpuComplete = !rawFrame.gpuPasses.empty();
    for (const RawGpuPass& pass : rawFrame.gpuPasses) {
        snapshot.gpuPasses.push_back(GpuPassSample{
            pass.name,
            pass.durationMs,
            pass.available,
            pass.pending
        });
        if (pass.available) {
            snapshot.gpuFrameMs += pass.durationMs;
        }
        if (!pass.available || pass.pending) {
            snapshot.gpuComplete = false;
        }
    }

    for (const render::ResourceMemoryRecord& record : rawFrame.resources) {
        snapshot.resources.push_back(ResourceMemoryEntry{
            record.name,
            record.category,
            record.cpuBytes,
            record.gpuBytes
        });
    }

    return snapshot;
}

ProfilerTraceFrame ProfilerService::buildTraceFrame(const RawFrameData& rawFrame) {
    ProfilerTraceFrame traceFrame{};
    traceFrame.sessionId = rawFrame.sessionId;
    traceFrame.frameNumber = rawFrame.frameNumber;
    traceFrame.startNs = rawFrame.startNs;
    traceFrame.endNs = rawFrame.endNs;
    traceFrame.profilerUiMs = rawFrame.profilerUiMs;
    traceFrame.cpuScopes = rawFrame.cpuScopes;
    traceFrame.gpuPasses.reserve(rawFrame.gpuPasses.size());
    traceFrame.resources.reserve(rawFrame.resources.size());

    for (const RawGpuPass& pass : rawFrame.gpuPasses) {
        traceFrame.gpuPasses.push_back(GpuPassSample{
            pass.name,
            pass.durationMs,
            pass.available,
            pass.pending
        });
    }

    for (const render::ResourceMemoryRecord& record : rawFrame.resources) {
        traceFrame.resources.push_back(ResourceMemoryEntry{
            record.name,
            record.category,
            record.cpuBytes,
            record.gpuBytes
        });
    }

    return traceFrame;
}

ProfilerService::CpuScopeHandle::CpuScopeHandle(ProfilerService* profiler, void* state, std::uint32_t index)
    : profiler_(profiler),
      state_(state),
      index_(index) {}

ProfilerService::CpuScopeHandle::~CpuScopeHandle() {
    reset();
}

ProfilerService::CpuScopeHandle::CpuScopeHandle(CpuScopeHandle&& other) noexcept
    : profiler_(other.profiler_),
      state_(other.state_),
      index_(other.index_) {
    other.profiler_ = nullptr;
    other.state_ = nullptr;
    other.index_ = ProfilerRecordedScope::kRoot;
}

ProfilerService::CpuScopeHandle& ProfilerService::CpuScopeHandle::operator=(CpuScopeHandle&& other) noexcept {
    if (this == &other) {
        return *this;
    }
    reset();
    profiler_ = other.profiler_;
    state_ = other.state_;
    index_ = other.index_;
    other.profiler_ = nullptr;
    other.state_ = nullptr;
    other.index_ = ProfilerRecordedScope::kRoot;
    return *this;
}

void ProfilerService::CpuScopeHandle::reset() noexcept {
    if (profiler_ != nullptr && index_ != ProfilerRecordedScope::kRoot) {
        profiler_->popCpuScope(state_, index_);
    }
    profiler_ = nullptr;
    state_ = nullptr;
    index_ = ProfilerRecordedScope::kRoot;
}

ProfilerService::GpuScopeHandle::GpuScopeHandle(ProfilerService* profiler, std::uint32_t index)
    : profiler_(profiler),
      index_(index) {}

ProfilerService::GpuScopeHandle::~GpuScopeHandle() {
    reset();
}

ProfilerService::GpuScopeHandle::GpuScopeHandle(GpuScopeHandle&& other) noexcept
    : profiler_(other.profiler_),
      index_(other.index_) {
    other.profiler_ = nullptr;
    other.index_ = ProfilerRecordedScope::kRoot;
}

ProfilerService::GpuScopeHandle& ProfilerService::GpuScopeHandle::operator=(GpuScopeHandle&& other) noexcept {
    if (this == &other) {
        return *this;
    }
    reset();
    profiler_ = other.profiler_;
    index_ = other.index_;
    other.profiler_ = nullptr;
    other.index_ = ProfilerRecordedScope::kRoot;
    return *this;
}

void ProfilerService::GpuScopeHandle::reset() noexcept {
    if (profiler_ != nullptr && index_ != ProfilerRecordedScope::kRoot) {
        profiler_->endGpuScope(index_);
    }
    profiler_ = nullptr;
    index_ = ProfilerRecordedScope::kRoot;
}

ProfilerService::ProfilerService(ProfilerConfig config)
    : config_(config),
      mainThreadId_(currentThreadIdValue()),
      instanceId_(nextProfilerInstanceId()) {
    worker_ = std::thread([this]() { workerLoop(); });
}

ProfilerService::~ProfilerService() {
    waitForWorkerIdle();
    {
        std::lock_guard lock(requestMutex_);
        workerStopRequested_ = true;
    }
    requestCv_.notify_all();
    if (worker_.joinable()) {
        worker_.join();
    }
    clearPendingGpuFrames();
    for (unsigned int queryId : freeGpuQueries_) {
        glDeleteQueries(1, &queryId);
    }
}

void ProfilerService::startCapture() {
    startRequested_ = true;
    if (!capturing_) {
        stopRequested_ = false;
    }
}

void ProfilerService::stopCapture() {
    if (capturing_) {
        stopRequested_ = true;
        return;
    }
    startRequested_ = false;
}

void ProfilerService::beginFrame() {
    ++currentFrameNumber_;
    pollPendingGpuFrames();

    if (startRequested_) {
        applyStartCapture();
    }
    if (stopRequested_) {
        capturing_ = false;
        stopRequested_ = false;
    }

    currentFrameActive_.store(capturing_, std::memory_order_release);
    currentProfilerUiMs_ = 0.0;
    gpuScopeOpen_ = false;
    currentGpuPasses_.clear();
    reservedCpuScopes_.store(0u, std::memory_order_release);

    if (!currentFrameActive_.load(std::memory_order_acquire)) {
        return;
    }

    cpuFrameSerial_.fetch_add(1u, std::memory_order_acq_rel);
    currentFrameStartNs_ = nowNs();
    currentGpuPasses_.reserve(config_.maxGpuScopesPerFrame);
}

void ProfilerService::endFrame(const std::vector<render::ResourceMemoryRecord>& resources) {
    if (!currentFrameActive_.load(std::memory_order_acquire)) {
        return;
    }

    if (gpuScopeOpen_) {
        glEndQuery(GL_TIME_ELAPSED);
        gpuScopeOpen_ = false;
    }

    RawFrameData rawFrame{};
    rawFrame.sessionId = activeSessionId_.load();
    rawFrame.frameNumber = currentFrameNumber_;
    rawFrame.startNs = currentFrameStartNs_;
    rawFrame.endNs = nowNs();
    rawFrame.profilerUiMs = currentProfilerUiMs_;
    rawFrame.resources = resources;
    rawFrame.gpuPasses.reserve(currentGpuPasses_.size());
    const std::uint64_t frameSerial = cpuFrameSerial_.load(std::memory_order_acquire);

    std::vector<const ThreadCpuState*> threadStates{};
    {
        std::lock_guard lock(cpuStateMutex_);
        threadStates.reserve(currentCpuThreadStates_.size());
        for (const std::unique_ptr<ThreadCpuState>& state : currentCpuThreadStates_) {
            if (state && state->frameSerial == frameSerial && !state->scopes.empty()) {
                threadStates.push_back(state.get());
            }
        }
    }

    std::sort(threadStates.begin(), threadStates.end(), [this](const ThreadCpuState* lhs, const ThreadCpuState* rhs) {
        const bool lhsMain = lhs->threadId == mainThreadId_;
        const bool rhsMain = rhs->threadId == mainThreadId_;
        if (lhsMain != rhsMain) {
            return lhsMain;
        }
        return lhs->threadId < rhs->threadId;
    });

    std::size_t totalCpuScopes = 0u;
    for (const ThreadCpuState* state : threadStates) {
        totalCpuScopes += state->scopes.size();
    }
    rawFrame.cpuScopes.reserve(totalCpuScopes);

    for (const ThreadCpuState* state : threadStates) {
        const std::uint32_t scopeOffset = static_cast<std::uint32_t>(rawFrame.cpuScopes.size());
        for (const RawCpuScope& scope : state->scopes) {
            rawFrame.cpuScopes.push_back(ProfilerRecordedScope{
                scope.name,
                scope.parentIndex == ProfilerRecordedScope::kRoot ? ProfilerRecordedScope::kRoot
                                                                  : scopeOffset + scope.parentIndex,
                scope.startNs,
                scope.endNs,
                scope.threadId
            });
        }
    }

    for (RawGpuPassState& pass : currentGpuPasses_) {
        rawFrame.gpuPasses.push_back(RawGpuPass{
            pass.name,
            pass.queryId,
            0.0,
            false,
            pass.queryId != 0
        });
    }

    enqueueBuildRequest(BuildRequest{0u, rawFrame});
    if (!rawFrame.gpuPasses.empty()) {
        pendingGpuFrames_.push_back(RawGpuFrame{std::move(rawFrame)});
        while (pendingGpuFrames_.size() > config_.maxPendingGpuFrames) {
            RawFrameData droppedFrame = std::move(pendingGpuFrames_.front().frame);
            pendingGpuFrames_.pop_front();
            for (RawGpuPass& pass : droppedFrame.gpuPasses) {
                if (pass.queryId != 0) {
                    recycleGpuQuery(pass.queryId);
                    pass.queryId = 0;
                }
                pass.pending = false;
                pass.available = false;
            }
            ++droppedFrames_;
            enqueueBuildRequest(BuildRequest{0u, droppedFrame});
        }
    }

    currentFrameActive_.store(false, std::memory_order_release);
    if (stopRequested_) {
        capturing_ = false;
        stopRequested_ = false;
    }
}

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
