#include "RuntimePolicy.hpp"

#include <algorithm>
#include <array>
#include <utility>

namespace core {

namespace {

constexpr std::size_t kMinParallelWorkers = 2u;

constexpr std::size_t kTransformEnableMinimum = 384u;
constexpr std::size_t kTransformEnablePerWorker = 128u;
constexpr std::size_t kTransformDisableMinimum = 192u;
constexpr std::size_t kTransformDisablePerWorker = 64u;

constexpr std::size_t kLightEnableMinimum = 256u;
constexpr std::size_t kLightEnablePerWorker = 96u;
constexpr std::size_t kLightDisableMinimum = 128u;
constexpr std::size_t kLightDisablePerWorker = 48u;

constexpr std::size_t kExtractionEnableMinimum = 320u;
constexpr std::size_t kExtractionEnablePerWorker = 128u;
constexpr std::size_t kExtractionDisableMinimum = 160u;
constexpr std::size_t kExtractionDisablePerWorker = 64u;

constexpr std::size_t kSceneViewEnableMinimum = 384u;
constexpr std::size_t kSceneViewEnablePerWorker = 128u;
constexpr std::size_t kSceneViewDisableMinimum = 192u;
constexpr std::size_t kSceneViewDisablePerWorker = 64u;

render::FrameCounterRecord makeCounter(const char* group, const char* name, std::int64_t value) {
    return render::FrameCounterRecord{name, value, group};
}

}  // namespace

const RuntimePolicyDecision& RuntimePolicy::evaluate(const RuntimePolicyFrameContext& context) {
    context_ = context;

    const std::size_t workers = context.workerCount;
    const std::size_t lights = totalLightCount(context);

    const std::size_t transformWorkload =
        context.transformCount +
        context.renderableCount +
        context.boundsCount +
        context.parentCount;
    transformTelemetry_ = StageTelemetry{
        static_cast<std::int64_t>(transformWorkload),
        static_cast<std::int64_t>(stageThreshold(kTransformEnableMinimum, kTransformEnablePerWorker, workers)),
        static_cast<std::int64_t>(stageThreshold(kTransformDisableMinimum, kTransformDisablePerWorker, workers)),
    };
    decision_.parallelTransformUpdate = resolveParallelDecision(
        decision_.parallelTransformUpdate,
        context.transformsDirty,
        workers,
        transformWorkload,
        static_cast<std::size_t>(transformTelemetry_.enableThreshold),
        static_cast<std::size_t>(transformTelemetry_.disableThreshold)
    );

    const std::size_t lightWorkload = lights * (1u + std::max<std::size_t>(1u, context.lightVolumeCount));
    lightTelemetry_ = StageTelemetry{
        static_cast<std::int64_t>(lightWorkload),
        static_cast<std::int64_t>(stageThreshold(kLightEnableMinimum, kLightEnablePerWorker, workers)),
        static_cast<std::int64_t>(stageThreshold(kLightDisableMinimum, kLightDisablePerWorker, workers)),
    };
    decision_.parallelLightUpdate = resolveParallelDecision(
        decision_.parallelLightUpdate,
        context.lightsDirty && lights > 0u,
        workers,
        lightWorkload,
        static_cast<std::size_t>(lightTelemetry_.enableThreshold),
        static_cast<std::size_t>(lightTelemetry_.disableThreshold)
    );

    const std::size_t extractionWorkload = context.renderableCount + lights + context.lightVolumeCount;
    extractionTelemetry_ = StageTelemetry{
        static_cast<std::int64_t>(extractionWorkload),
        static_cast<std::int64_t>(stageThreshold(kExtractionEnableMinimum, kExtractionEnablePerWorker, workers)),
        static_cast<std::int64_t>(stageThreshold(kExtractionDisableMinimum, kExtractionDisablePerWorker, workers)),
    };
    decision_.parallelRenderExtraction = resolveParallelDecision(
        decision_.parallelRenderExtraction,
        extractionWorkload > 0u,
        workers,
        extractionWorkload,
        static_cast<std::size_t>(extractionTelemetry_.enableThreshold),
        static_cast<std::size_t>(extractionTelemetry_.disableThreshold)
    );

    const std::size_t sceneViewWorkload = context.renderableCount;
    sceneViewTelemetry_ = StageTelemetry{
        static_cast<std::int64_t>(sceneViewWorkload),
        static_cast<std::int64_t>(stageThreshold(kSceneViewEnableMinimum, kSceneViewEnablePerWorker, workers)),
        static_cast<std::int64_t>(stageThreshold(kSceneViewDisableMinimum, kSceneViewDisablePerWorker, workers)),
    };
    decision_.parallelSceneView = resolveParallelDecision(
        decision_.parallelSceneView,
        sceneViewWorkload > 0u,
        workers,
        sceneViewWorkload,
        static_cast<std::size_t>(sceneViewTelemetry_.enableThreshold),
        static_cast<std::size_t>(sceneViewTelemetry_.disableThreshold)
    );

    return decision_;
}

std::vector<render::FrameCounterRecord> RuntimePolicy::profilingCounters() const {
    return {
        makeCounter("Runtime Policy", "Worker Threads", static_cast<std::int64_t>(context_.workerCount)),
        makeCounter("Runtime Policy", "Transforms Dirty", context_.transformsDirty ? 1 : 0),
        makeCounter("Runtime Policy", "Lights Dirty", context_.lightsDirty ? 1 : 0),

        makeCounter("Runtime Policy", "Transform Update Enabled", decision_.parallelTransformUpdate ? 1 : 0),
        makeCounter("Runtime Policy", "Transform Update Workload", transformTelemetry_.workload),
        makeCounter("Runtime Policy", "Transform Update Enable Threshold", transformTelemetry_.enableThreshold),
        makeCounter("Runtime Policy", "Transform Update Disable Threshold", transformTelemetry_.disableThreshold),

        makeCounter("Runtime Policy", "Light Update Enabled", decision_.parallelLightUpdate ? 1 : 0),
        makeCounter("Runtime Policy", "Light Update Workload", lightTelemetry_.workload),
        makeCounter("Runtime Policy", "Light Update Enable Threshold", lightTelemetry_.enableThreshold),
        makeCounter("Runtime Policy", "Light Update Disable Threshold", lightTelemetry_.disableThreshold),

        makeCounter("Runtime Policy", "Render Extraction Enabled", decision_.parallelRenderExtraction ? 1 : 0),
        makeCounter("Runtime Policy", "Render Extraction Workload", extractionTelemetry_.workload),
        makeCounter("Runtime Policy", "Render Extraction Enable Threshold", extractionTelemetry_.enableThreshold),
        makeCounter("Runtime Policy", "Render Extraction Disable Threshold", extractionTelemetry_.disableThreshold),

        makeCounter("Runtime Policy", "Scene View Enabled", decision_.parallelSceneView ? 1 : 0),
        makeCounter("Runtime Policy", "Scene View Workload", sceneViewTelemetry_.workload),
        makeCounter("Runtime Policy", "Scene View Enable Threshold", sceneViewTelemetry_.enableThreshold),
        makeCounter("Runtime Policy", "Scene View Disable Threshold", sceneViewTelemetry_.disableThreshold),
    };
}

bool RuntimePolicy::resolveParallelDecision(
    bool current,
    bool hasWork,
    std::size_t workerCount,
    std::size_t workload,
    std::size_t enableThreshold,
    std::size_t disableThreshold
) {
    if (!hasWork || workerCount < kMinParallelWorkers) {
        return false;
    }

    if (!current) {
        return workload >= enableThreshold;
    }
    return workload >= disableThreshold;
}

std::size_t RuntimePolicy::totalLightCount(const RuntimePolicyFrameContext& context) {
    return context.pointLightCount + context.spotLightCount;
}

std::size_t RuntimePolicy::stageThreshold(std::size_t minimum, std::size_t perWorker, std::size_t workerCount) {
    return std::max(minimum, workerCount * perWorker);
}

}  // namespace core
