#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

#include <glm/mat4x4.hpp>

#include "core/app/FrameData.hpp"
#include "render/resources/MeshBuffer.hpp"
#include "render/pipeline/ShadowSystem.hpp"

namespace core {
class TaskScheduler;
}

namespace render {

struct RenderSceneObjectView {
    core::EntityId entity{};
    int sourceIndex{-1};
    const MeshBuffer* mesh{nullptr};
    std::shared_ptr<Material> material{};
    RenderLayer layer{RenderLayer::Geometry};
    Bounds3 localBounds{};
    Bounds3 worldBounds{};
    bool hasWorldBounds{false};
    glm::mat4 modelMatrix{1.0f};
    bool skinned{false};
    int jointMatrixBase{0};
    int jointMatrixCount{0};
    bool visible{true};
    bool frustumVisible{true};
    bool occlusionVisible{true};
};

struct FrustumCullStats {
    std::uint32_t totalRenderables{0};
    std::uint32_t visibilityHidden{0};
    std::uint32_t boundsTested{0};
    std::uint32_t frustumPassed{0};
    std::uint32_t frustumCulled{0};
    std::uint32_t noBoundsBypass{0};
    std::uint32_t mainPassVisible{0};
};

struct OcclusionCullCacheState {
    bool queryInFlight{false};
    bool hasLastResult{false};
    bool lastVisible{true};
    std::uint8_t occludedFrameStreak{0};
};

struct OcclusionCullStats {
    std::uint32_t candidates{0};
    std::uint32_t queryIssued{0};
    std::uint32_t visible{0};
    std::uint32_t occluded{0};
    std::uint32_t noBoundsBypass{0};
    std::uint32_t warmupVisible{0};
    std::uint32_t pendingReused{0};
};

enum class RenderSelectionKind {
    None = 0,
    Renderable,
    Light,
    Node,
};

struct RenderSelectionView {
    RenderSelectionKind kind{RenderSelectionKind::None};
    int index{-1};
    Bounds3 worldBounds{};
    bool hasWorldBounds{false};
    glm::mat4 transformMatrix{1.0f};
};

struct RenderSelectionSkeletonView {
    bool showOverlay{false};
    std::vector<int> parentIndices{};
    std::vector<glm::vec3> jointWorldPositions{};
};

struct RenderSceneView {
    std::vector<RenderSceneObjectView> objects{};
    std::vector<core::FrameLight> lights{};
    std::vector<core::FrameLightVolume> lightVolumes{};
    RenderSelectionView selection{};
    RenderSelectionSkeletonView selectionSkeleton{};
    FrustumCullStats frustumCullStats{};
    OcclusionCullStats occlusionCullStats{};
};

/**
 * Resolves the editor selection into render-scene indices for the current frame.
 */
RenderSelectionView resolveRenderSelection(const core::FrameSceneData& frame);

/**
 * Builds the renderer-facing scene view from extracted frame data.
 * Parallel mode is still frame-bound and waits for object build tasks to finish immediately.
 */
RenderSceneView buildRenderSceneView(
    const core::FrameSceneData& frame,
    const std::vector<const MeshBuffer*>& meshLookup,
    core::TaskScheduler& scheduler,
    bool useParallel = true
);

/**
 * Applies camera-driven frustum visibility to scene objects using their world bounds.
 * Objects without valid world bounds fail open and remain visible to the main pass.
 */
void applyCameraFrustumCulling(RenderSceneView& scene, const CameraMatrices& camera);

/**
 * Applies last-known occlusion visibility to camera-visible scene objects.
 * Objects without valid bounds or without a completed query fail open.
 */
void applyLastKnownOcclusionVisibility(
    RenderSceneView& scene,
    const std::unordered_map<core::EntityId, OcclusionCullCacheState>& cache
);

/**
 * Collects the visible renderables used by the shadow passes.
 */
std::vector<ShadowSystem::ShadowRenderable> collectShadowRenderables(const RenderSceneView& scene);

}  // namespace render
