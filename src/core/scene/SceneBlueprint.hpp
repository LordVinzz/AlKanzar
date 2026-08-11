#pragma once

#include <optional>
#include <cstdint>
#include <string>
#include <vector>

#include <glm/vec3.hpp>

#include "core/ecs/Components.hpp"

namespace core {

using SceneObjectId = std::string;

enum class ScenePrimitiveShape {
    Plane = 0,
    Box,
};

enum class SceneMaterialPreset {
    Soil = 0,
    Rock,
    Wood,
};

enum class SceneModelMaterialProfile {
    Imported = 0,
    House,
};

struct CharacterBlueprint {
    CharacterComponent character{};
    CharacterControllerComponent controller{};
    std::optional<PartyMemberComponent> partyMember{};
    AbilityScoresComponent abilities{};
    SkillRanksComponent skills{};
    CharacterVitalsComponent vitals{};
};

struct ModelInstanceBlueprint {
    std::string name;
    std::string path;
    render::RenderLayer layer{render::RenderLayer::Geometry};
    SceneModelMaterialProfile materialProfile{SceneModelMaterialProfile::Imported};
    TransformComponent transform{};
    bool fitToFootprint{false};
    float footprint{0.0f};
    std::optional<CharacterBlueprint> character{};
    SceneObjectId id{};
    std::optional<SceneObjectId> parentId{};
};

struct ScenePrimitiveBlueprint {
    std::string name;
    ScenePrimitiveShape shape{ScenePrimitiveShape::Box};
    SceneMaterialPreset material{SceneMaterialPreset::Rock};
    render::RenderLayer layer{render::RenderLayer::Geometry};
    TransformComponent transform{};
    SceneObjectId id{};
    std::optional<SceneObjectId> parentId{};
};

struct DirectionalLightBlueprint {
    std::string name;
    glm::vec3 direction{-0.3f, -1.0f, -0.4f};
    glm::vec3 color{1.0f};
    float intensity{1.0f};
    SceneObjectId id{};
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
    SceneObjectId id{};
    std::optional<SceneObjectId> parentId{};
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
    SceneObjectId id{};
    std::optional<SceneObjectId> parentId{};
};

struct LightVolumeBlueprint {
    std::string name;
    TransformComponent transform{};
    glm::vec3 halfExtents{1.0f};
    SceneObjectId id{};
    std::optional<SceneObjectId> parentId{};
};

struct SceneBlueprint {
    // Retained only to migrate SCN V1 assets whose procedural layout was
    // described by scene-wide values instead of explicit authored objects.
    float groundHalfExtent{500.0f};
    float wallHeight{2.5f};
    float wallOffset{3.0f};
    float wallLength{5.0f};
    float wallThickness{0.5f};
    std::string navMeshAssetPath{};
    std::vector<ScenePrimitiveBlueprint> primitives{};
    std::vector<ModelInstanceBlueprint> models{};
    std::optional<DirectionalLightBlueprint> directionalLight{};
    std::vector<LightVolumeBlueprint> lightVolumes{};
    std::vector<PointLightBlueprint> pointLights{};
    std::vector<SpotLightBlueprint> spotLights{};
    std::vector<SceneObjectId> objectOrder{};
    std::uint32_t sourceVersion{2u};
    bool requiresExplicitObjectMigration{false};
};

}  // namespace core
