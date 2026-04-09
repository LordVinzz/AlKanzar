#pragma once

#include <memory>
#include <optional>
#include <vector>

#include <glm/mat4x4.hpp>

#include "core/FrameData.hpp"
#include "MeshBuffer.hpp"
#include "ShadowSystem.hpp"

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
};

struct RenderSelectionView {
    RenderSelectionKind kind{RenderSelectionKind::None};
    int index{-1};
    Bounds3 worldBounds{};
    glm::mat4 transformMatrix{1.0f};
};

struct RenderSceneView {
    std::vector<RenderSceneObjectView> objects{};
    std::vector<core::FrameLight> lights{};
    std::vector<core::FrameLightVolume> lightVolumes{};
    RenderSelectionView selection{};
};

RenderSelectionView resolveRenderSelection(const core::FrameSceneData& frame);
RenderSceneView buildRenderSceneView(const core::FrameSceneData& frame, const std::vector<const MeshBuffer*>& meshLookup);
std::vector<ShadowSystem::ShadowRenderable> collectShadowRenderables(const RenderSceneView& scene);

}  // namespace render
