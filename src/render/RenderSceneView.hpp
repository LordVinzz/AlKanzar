#pragma once

#include <memory>
#include <optional>
#include <vector>

#include <glm/mat4x4.hpp>

#include "core/FrameData.hpp"
#include "MeshBuffer.hpp"
#include "ShadowSystem.hpp"

namespace core {
class TaskScheduler;
}

namespace render {

struct RenderSceneObjectView {
    int sourceIndex{-1};
    const MeshBuffer* mesh{nullptr};
    std::shared_ptr<Material> material{};
    RenderLayer layer{RenderLayer::Geometry};
    Bounds3 localBounds{};
    Bounds3 worldBounds{};
    glm::mat4 modelMatrix{1.0f};
    bool visible{true};
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

struct RenderSceneView {
    std::vector<RenderSceneObjectView> objects{};
    std::vector<core::FrameLight> lights{};
    std::vector<core::FrameLightVolume> lightVolumes{};
    RenderSelectionView selection{};
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
 * Collects the visible renderables used by the shadow passes.
 */
std::vector<ShadowSystem::ShadowRenderable> collectShadowRenderables(const RenderSceneView& scene);

}  // namespace render
