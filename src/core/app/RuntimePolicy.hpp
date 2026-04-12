#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "render/resources/Profiling.hpp"

namespace core {

struct RuntimePolicyFrameContext {
    std::size_t workerCount{0u};
    bool transformsDirty{false};
    bool lightsDirty{false};
    std::size_t transformCount{0u};
    std::size_t boundsCount{0u};
    std::size_t renderableCount{0u};
    std::size_t parentCount{0u};
    std::size_t pointLightCount{0u};
    std::size_t spotLightCount{0u};
    std::size_t lightVolumeCount{0u};
};

struct RuntimePolicyDecision {
    bool parallelTransformUpdate{false};
    bool parallelLightUpdate{false};
    bool parallelRenderExtraction{false};
    bool parallelSceneView{false};
};

class RuntimePolicy {
public:
    [[nodiscard]] const RuntimePolicyDecision& evaluate(const RuntimePolicyFrameContext& context);
    [[nodiscard]] const RuntimePolicyDecision& decision() const { return decision_; }
    [[nodiscard]] std::vector<render::FrameCounterRecord> profilingCounters() const;

private:
    struct StageTelemetry {
        std::int64_t workload{0};
        std::int64_t enableThreshold{0};
        std::int64_t disableThreshold{0};
    };

    [[nodiscard]] static bool resolveParallelDecision(
        bool current,
        bool hasWork,
        std::size_t workerCount,
        std::size_t workload,
        std::size_t enableThreshold,
        std::size_t disableThreshold
    );

    [[nodiscard]] static std::size_t totalLightCount(const RuntimePolicyFrameContext& context);
    [[nodiscard]] static std::size_t stageThreshold(std::size_t minimum, std::size_t perWorker, std::size_t workerCount);

    RuntimePolicyFrameContext context_{};
    RuntimePolicyDecision decision_{};
    StageTelemetry transformTelemetry_{};
    StageTelemetry lightTelemetry_{};
    StageTelemetry extractionTelemetry_{};
    StageTelemetry sceneViewTelemetry_{};
};

}  // namespace core
