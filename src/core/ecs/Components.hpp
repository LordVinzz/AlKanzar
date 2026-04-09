#pragma once

#include <string>

#include <glm/vec3.hpp>

#include "Entity.hpp"
#include "core/lighting/MaterialLibrary.hpp"
#include "render/engine/RenderTypes.hpp"

namespace core {

struct NameComponent {
    std::string value;
};

struct TransformComponent {
    glm::vec3 position{0.0f};
    glm::vec3 rotationDeg{0.0f};
    glm::vec3 scale{1.0f};
};

struct ParentComponent {
    EntityId parent{};
};

struct VisibilityComponent {
    bool visible{true};
};

struct BoundsComponent {
    render::Bounds3 localBounds{};
};

struct RenderableComponent {
    render::MeshHandle mesh{};
    MaterialHandle material{};
    render::RenderLayer layer{render::RenderLayer::Geometry};
};

struct PointLightComponent {
    float radius{1.0f};
    glm::vec3 color{1.0f};
    float intensity{1.0f};
    float phase{0.0f};
    bool isMovable{false};
    bool castsShadow{false};
    float shadowBiasMin{0.0f};
    float shadowBiasSlope{0.0f};
};

struct SpotLightComponent {
    float radius{1.0f};
    glm::vec3 color{1.0f};
    float intensity{1.0f};
    glm::vec3 target{0.0f};
    float innerAngle{15.0f};
    float outerAngle{25.0f};
    float phase{0.0f};
    bool isMovable{false};
    bool castsShadow{false};
    float shadowBiasMin{0.0f};
    float shadowBiasSlope{0.0f};
};

struct TransformCacheEntry {
    glm::mat4 worldMatrix{1.0f};
    render::Bounds3 worldBounds{};
    bool hasWorldBounds{false};
    bool valid{false};
};

struct LightRuntime {
    glm::vec3 position{0.0f};
    glm::vec3 direction{0.0f, 0.0f, -1.0f};
    render::LightType type{render::LightType::Point};
    bool isMovable{false};
    bool valid{false};
};

}  // namespace core
