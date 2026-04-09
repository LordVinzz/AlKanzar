#pragma once

#include <memory>
#include <optional>
#include <vector>

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

#include "Entity.hpp"
#include "MaterialLibrary.hpp"
#include "render/Material.hpp"
#include "render/RenderTypes.hpp"

namespace core {

struct FrameRenderable {
    EntityId entity{};
    render::MeshHandle mesh{};
    MaterialHandle materialHandle{};
    std::shared_ptr<render::Material> material{};
    render::RenderLayer layer{render::RenderLayer::Geometry};
    render::Bounds3 localBounds{};
    render::Bounds3 worldBounds{};
    glm::mat4 modelMatrix{1.0f};
    bool visible{true};
};

struct FrameLight {
    EntityId entity{};
    render::LightType type{render::LightType::Point};
    glm::vec3 position{0.0f};
    float radius{0.0f};
    glm::vec3 color{1.0f};
    float intensity{1.0f};
    glm::vec3 direction{0.0f, 0.0f, -1.0f};
    float innerAngle{0.0f};
    float outerAngle{0.0f};
    bool isMovable{false};
    bool castsShadow{false};
    float shadowBiasMin{0.0f};
    float shadowBiasSlope{0.0f};
};

struct FrameLightVolume {
    glm::vec3 minCorner{0.0f};
    glm::vec3 maxCorner{0.0f};
    std::vector<int> staticLightIndices{};
    std::vector<int> movableLightIndices{};
};

struct FrameSelection {
    std::optional<EntityId> entity{};
    bool isLight{false};
    render::Bounds3 worldBounds{};
    glm::mat4 transformMatrix{1.0f};
};

struct FrameSceneData {
    std::vector<FrameRenderable> renderables{};
    std::vector<FrameLight> lights{};
    std::vector<FrameLightVolume> lightVolumes{};
    FrameSelection selection{};

    void reserve(std::size_t renderableCount, std::size_t lightCount, std::size_t volumeCount) {
        renderables.reserve(renderableCount);
        lights.reserve(lightCount);
        lightVolumes.reserve(volumeCount);
    }

    void clear() {
        renderables.clear();
        lights.clear();
        lightVolumes.clear();
        selection = FrameSelection{};
    }
};

}  // namespace core
