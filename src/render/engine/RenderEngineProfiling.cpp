#include "RenderEngine.hpp"

#include "RenderPaths.hpp"

namespace render {

std::vector<ResourceMemoryRecord> RenderEngine::profilingResources() const {
    std::vector<ResourceMemoryRecord> resources{};

    for (std::size_t index = 0; index < sceneMeshes_.size(); ++index) {
        const std::unique_ptr<MeshBuffer>& mesh = sceneMeshes_[index];
        if (!mesh || mesh->gpuBytes() == 0u) {
            continue;
        }
        resources.push_back(ResourceMemoryRecord{
            "Scene Mesh " + std::to_string(index),
            "Mesh Buffer",
            0u,
            mesh->gpuBytes()
        });
    }

    if (jointMatrixBuffer_ != 0 && jointMatrixBufferSize_ > 0) {
        resources.push_back(ResourceMemoryRecord{
            "Joint Matrix Buffer",
            "Animation Buffer",
            0u,
            static_cast<std::uint64_t>(jointMatrixBufferSize_)
        });
    }

    for (const std::shared_ptr<Texture>& texture : resourceRegistry_.profilingTextures()) {
        if (!texture || !texture->valid()) {
            continue;
        }
        resources.push_back(ResourceMemoryRecord{
            texture->name,
            "Texture",
            estimateTextureCpuBytes(*texture),
            estimateTextureGpuBytes(*texture)
        });
    }

    const std::vector<ResourceMemoryRecord> shadowResources = shadowSystem_.profilingResources();
    resources.insert(resources.end(), shadowResources.begin(), shadowResources.end());

    if (renderPath_) {
        const std::vector<ResourceMemoryRecord> pathResources = renderPath_->profilingResources();
        resources.insert(resources.end(), pathResources.begin(), pathResources.end());
    }

    return resources;
}

std::vector<FrameCounterRecord> RenderEngine::profilingCounters() const {
    return {
        FrameCounterRecord{"Total Renderables", static_cast<std::int64_t>(latestFrustumCullStats_.totalRenderables), "Frustum Culling"},
        FrameCounterRecord{"Visibility Hidden", static_cast<std::int64_t>(latestFrustumCullStats_.visibilityHidden), "Frustum Culling"},
        FrameCounterRecord{"Bounds Tested", static_cast<std::int64_t>(latestFrustumCullStats_.boundsTested), "Frustum Culling"},
        FrameCounterRecord{"Passed", static_cast<std::int64_t>(latestFrustumCullStats_.frustumPassed), "Frustum Culling"},
        FrameCounterRecord{"Culled", static_cast<std::int64_t>(latestFrustumCullStats_.frustumCulled), "Frustum Culling"},
        FrameCounterRecord{"No Bounds Bypass", static_cast<std::int64_t>(latestFrustumCullStats_.noBoundsBypass), "Frustum Culling"},
        FrameCounterRecord{"Main Pass Visible", static_cast<std::int64_t>(latestFrustumCullStats_.mainPassVisible), "Frustum Culling"},
        FrameCounterRecord{"Candidates", static_cast<std::int64_t>(latestOcclusionCullStats_.candidates), "Occlusion Culling"},
        FrameCounterRecord{"Query Issued", static_cast<std::int64_t>(latestOcclusionCullStats_.queryIssued), "Occlusion Culling"},
        FrameCounterRecord{"Visible", static_cast<std::int64_t>(latestOcclusionCullStats_.visible), "Occlusion Culling"},
        FrameCounterRecord{"Occluded", static_cast<std::int64_t>(latestOcclusionCullStats_.occluded), "Occlusion Culling"},
        FrameCounterRecord{"No Bounds Bypass", static_cast<std::int64_t>(latestOcclusionCullStats_.noBoundsBypass), "Occlusion Culling"},
        FrameCounterRecord{"Warmup Visible", static_cast<std::int64_t>(latestOcclusionCullStats_.warmupVisible), "Occlusion Culling"},
        FrameCounterRecord{"Pending Reused", static_cast<std::int64_t>(latestOcclusionCullStats_.pendingReused), "Occlusion Culling"},
    };
}

}  // namespace render

