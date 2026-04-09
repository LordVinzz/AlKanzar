#pragma once

#include <atomic>
#include <cstddef>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "render/Profiling.hpp"

namespace core {

struct ProfilerConfig {
    std::size_t maxFrames{180u};
    std::size_t maxCpuScopesPerFrame{512u};
    std::size_t maxGpuScopesPerFrame{32u};
    std::size_t maxPendingGpuFrames{24u};
    std::uint64_t gpuReadbackDelayFrames{3u};
};

struct ProfilerRecordedScope {
    static constexpr std::uint32_t kRoot = static_cast<std::uint32_t>(-1);

    std::string name{};
    std::uint32_t parentIndex{kRoot};
    std::uint64_t startNs{0};
    std::uint64_t endNs{0};
    std::uint64_t threadId{0};
};

struct ProfilerScopeNode {
    std::string name{};
    double totalMs{0.0};
    double selfMs{0.0};
    std::uint32_t callCount{0};
    std::string thread{};
    std::vector<ProfilerScopeNode> children{};
};

struct GpuPassSample {
    std::string name{};
    double durationMs{0.0};
    bool available{false};
    bool pending{false};
};

struct ResourceMemoryEntry {
    std::string name{};
    std::string category{};
    std::uint64_t cpuBytes{0};
    std::uint64_t gpuBytes{0};
};

struct ProfilerFrameSnapshot {
    std::uint64_t sessionId{0};
    std::uint64_t frameNumber{0};
    double cpuFrameMs{0.0};
    double gpuFrameMs{0.0};
    double profilerUiMs{0.0};
    bool gpuComplete{false};
    std::vector<ProfilerScopeNode> cpuScopes{};
    std::vector<GpuPassSample> gpuPasses{};
    std::vector<ResourceMemoryEntry> resources{};
};

struct ProfilerStats {
    bool capturing{false};
    bool startPending{false};
    bool stopPending{false};
    std::size_t bufferedFrames{0};
    std::size_t pendingGpuFrames{0};
    std::uint64_t droppedCpuScopes{0};
    std::uint64_t droppedGpuScopes{0};
    std::uint64_t droppedFrames{0};
    std::uint64_t currentFrameNumber{0};
};

[[nodiscard]] std::vector<ProfilerScopeNode> buildProfilerScopeTree(
    const std::vector<ProfilerRecordedScope>& scopes,
    std::uint64_t mainThreadId
);

class ProfilerService {
public:
    class CpuScopeHandle {
    public:
        CpuScopeHandle() = default;
        CpuScopeHandle(ProfilerService* profiler, std::uint32_t index);
        ~CpuScopeHandle();

        CpuScopeHandle(const CpuScopeHandle&) = delete;
        CpuScopeHandle& operator=(const CpuScopeHandle&) = delete;
        CpuScopeHandle(CpuScopeHandle&& other) noexcept;
        CpuScopeHandle& operator=(CpuScopeHandle&& other) noexcept;

    private:
        void reset() noexcept;

        ProfilerService* profiler_{nullptr};
        std::uint32_t index_{ProfilerRecordedScope::kRoot};
    };

    class GpuScopeHandle {
    public:
        GpuScopeHandle() = default;
        GpuScopeHandle(ProfilerService* profiler, std::uint32_t index);
        ~GpuScopeHandle();

        GpuScopeHandle(const GpuScopeHandle&) = delete;
        GpuScopeHandle& operator=(const GpuScopeHandle&) = delete;
        GpuScopeHandle(GpuScopeHandle&& other) noexcept;
        GpuScopeHandle& operator=(GpuScopeHandle&& other) noexcept;

    private:
        void reset() noexcept;

        ProfilerService* profiler_{nullptr};
        std::uint32_t index_{ProfilerRecordedScope::kRoot};
    };

    explicit ProfilerService(ProfilerConfig config = {});
    ~ProfilerService();

    ProfilerService(const ProfilerService&) = delete;
    ProfilerService& operator=(const ProfilerService&) = delete;

    void startCapture();
    void stopCapture();
    void beginFrame();
    void endFrame(const std::vector<render::ResourceMemoryRecord>& resources);
    void recordProfilerUiTime(double durationMs);

    [[nodiscard]] CpuScopeHandle scopedCpu(const char* name);
    [[nodiscard]] GpuScopeHandle scopedGpu(const char* name);

    [[nodiscard]] ProfilerStats stats() const;
    [[nodiscard]] std::vector<std::shared_ptr<const ProfilerFrameSnapshot>> snapshots() const;
    [[nodiscard]] std::uint64_t mainThreadId() const { return mainThreadId_; }

    void waitForWorkerIdle();

private:
    struct RawCpuScope {
        const char* name{nullptr};
        std::uint32_t parentIndex{ProfilerRecordedScope::kRoot};
        std::uint64_t startNs{0};
        std::uint64_t endNs{0};
        std::uint64_t threadId{0};
    };

    struct RawGpuPassState {
        const char* name{nullptr};
        unsigned int queryId{0};
    };

    struct RawGpuPass {
        std::string name{};
        unsigned int queryId{0};
        double durationMs{0.0};
        bool available{false};
        bool pending{false};
    };

    struct RawFrameData {
        std::uint64_t sessionId{0};
        std::uint64_t frameNumber{0};
        std::uint64_t startNs{0};
        std::uint64_t endNs{0};
        double profilerUiMs{0.0};
        std::vector<ProfilerRecordedScope> cpuScopes{};
        std::vector<RawGpuPass> gpuPasses{};
        std::vector<render::ResourceMemoryRecord> resources{};
    };

    struct RawGpuFrame {
        RawFrameData frame{};
    };

    struct BuildRequest {
        std::uint64_t requestId{0};
        RawFrameData frame{};
    };

    std::uint32_t pushCpuScope(const char* name);
    void popCpuScope(std::uint32_t index);
    std::uint32_t beginGpuScope(const char* name);
    void endGpuScope(std::uint32_t index);

    void applyStartCapture();
    void pollPendingGpuFrames();
    void clearPendingGpuFrames();
    void recycleGpuQuery(unsigned int queryId);
    void enqueueBuildRequest(BuildRequest request);
    void workerLoop();
    static ProfilerFrameSnapshot buildSnapshot(const RawFrameData& rawFrame, std::uint64_t mainThreadId);

    ProfilerConfig config_{};
    std::uint64_t mainThreadId_{0};
    std::uint64_t currentFrameNumber_{0};
    std::atomic<std::uint64_t> activeSessionId_{0};
    bool capturing_{false};
    bool startRequested_{false};
    bool stopRequested_{false};
    bool currentFrameActive_{false};
    bool gpuScopeOpen_{false};
    std::uint64_t currentFrameStartNs_{0};
    double currentProfilerUiMs_{0.0};
    std::uint64_t droppedCpuScopes_{0};
    std::uint64_t droppedGpuScopes_{0};
    std::uint64_t droppedFrames_{0};
    std::vector<RawCpuScope> currentCpuScopes_{};
    std::vector<std::uint32_t> currentCpuStack_{};
    std::vector<RawGpuPassState> currentGpuPasses_{};
    std::deque<RawGpuFrame> pendingGpuFrames_{};
    std::vector<unsigned int> freeGpuQueries_{};

    mutable std::mutex snapshotMutex_{};
    std::deque<std::shared_ptr<const ProfilerFrameSnapshot>> snapshots_{};

    std::mutex requestMutex_{};
    std::condition_variable requestCv_{};
    std::condition_variable idleCv_{};
    std::deque<BuildRequest> requestQueue_{};
    bool workerStopRequested_{false};
    std::uint64_t enqueuedRequestId_{0};
    std::uint64_t processedRequestId_{0};
    std::thread worker_{};
};

#define ALKANZAR_PROFILE_CONCAT_INNER(lhs, rhs) lhs##rhs
#define ALKANZAR_PROFILE_CONCAT(lhs, rhs) ALKANZAR_PROFILE_CONCAT_INNER(lhs, rhs)
#define ALKANZAR_PROFILE_SCOPE(profiler, name) \
    auto ALKANZAR_PROFILE_CONCAT(_alkanzarProfileScope_, __LINE__) = (profiler).scopedCpu(name)
#define ALKANZAR_PROFILE_GPU_SCOPE(profiler, name) \
    auto ALKANZAR_PROFILE_CONCAT(_alkanzarGpuScope_, __LINE__) = (profiler).scopedGpu(name)

}  // namespace core
