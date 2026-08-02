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
    snapshot.startNs = rawFrame.startNs;
    snapshot.endNs = rawFrame.endNs;
    const std::uint64_t frameDurationNs = rawFrame.endNs > rawFrame.startNs ? rawFrame.endNs - rawFrame.startNs : 0u;
    snapshot.cpuFrameMs = std::max(nsToMs(frameDurationNs) - rawFrame.profilerUiMs, 0.0);
    snapshot.profilerUiMs = rawFrame.profilerUiMs;
    snapshot.cpuScopes = buildProfilerScopeTree(rawFrame.cpuScopes, mainThreadId);
    snapshot.gpuPasses.reserve(rawFrame.gpuPasses.size());
    snapshot.resources.reserve(rawFrame.resources.size());
    snapshot.counters = rawFrame.counters;

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
    traceFrame.counters = rawFrame.counters;

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

void ProfilerService::endFrame(
    const std::vector<render::ResourceMemoryRecord>& resources,
    const std::vector<render::FrameCounterRecord>& counters
) {
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
    rawFrame.counters = counters;
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


}  // namespace core
