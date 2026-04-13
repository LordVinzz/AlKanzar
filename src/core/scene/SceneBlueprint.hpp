#pragma once

#include <string>
#include <vector>

#include <glm/vec3.hpp>

#include "core/ecs/Components.hpp"

namespace core {

struct ModelInstanceBlueprint {
    std::string name;
    std::string path;
    render::RenderLayer layer{render::RenderLayer::Geometry};
    TransformComponent transform{};
    bool fitToFootprint{false};
    float footprint{0.0f};
};

struct PointLightBlueprint {
    std::string name;
    TransformComponent transform{};
    float radius{1.0f};
    glm::vec3 color{1.0f};
    float intensity{1.0f};
    float phase{0.0f};
    bool isMovable{false};
    bool castsShadow{false};
    float shadowBiasMin{0.0f};
    float shadowBiasSlope{0.0f};
};

struct SpotLightBlueprint {
    std::string name;
    TransformComponent transform{};
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

struct LightVolumeBlueprint {
    std::string name;
    TransformComponent transform{};
    glm::vec3 halfExtents{1.0f};
};

struct SceneBlueprint {
    float groundHalfExtent{500.0f};
    float wallHeight{2.5f};
    float wallOffset{3.0f};
    float wallLength{5.0f};
    float wallThickness{0.5f};
    std::string navMeshAssetPath{};
    std::vector<ModelInstanceBlueprint> models{};
    std::vector<LightVolumeBlueprint> lightVolumes{};
    std::vector<PointLightBlueprint> pointLights{};
    std::vector<SpotLightBlueprint> spotLights{};
};

}  // namespace core
