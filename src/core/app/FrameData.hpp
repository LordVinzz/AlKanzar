#pragma once

#include <memory>
#include <optional>
#include <vector>

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include "core/ecs/Entity.hpp"
#include "core/ecs/ComponentRegistry.hpp"
#include "render/resources/Material.hpp"
#include "render/engine/RenderTypes.hpp"

namespace core {

struct FrameRenderable {
    EntityId entity{};
    render::MeshHandle mesh{};
    std::shared_ptr<render::Material> material{};
    render::RenderLayer layer{render::RenderLayer::Geometry};
    render::Bounds3 localBounds{};
    render::Bounds3 worldBounds{};
    bool hasWorldBounds{false};
    glm::mat4 modelMatrix{1.0f};
    bool visible{true};
    bool skinned{false};
    int jointMatrixBase{0};
    int jointMatrixCount{0};
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
    EntityId entity{};
    glm::vec3 minCorner{0.0f};
    glm::vec3 maxCorner{0.0f};
    std::vector<int> staticLightIndices{};
    std::vector<int> movableLightIndices{};
};

enum class FrameColliderShape {
    Box = 0,
    Sphere,
};

struct FrameColliderDebug {
    EntityId entity{};
    FrameColliderShape shape{FrameColliderShape::Box};
    render::Bounds3 bounds{};
    glm::mat4 modelMatrix{1.0f};
    glm::vec3 center{0.0f};
    float radius{0.0f};
};

struct FrameSelection {
    std::optional<EntityId> entity{};
    std::optional<ComponentKind> component{};
    bool isLight{false};
    render::Bounds3 worldBounds{};
    bool hasWorldBounds{false};
    glm::mat4 transformMatrix{1.0f};
    glm::mat4 boundsModelMatrix{1.0f};
    bool hasBoundsModelMatrix{false};
};

struct FrameSkeletonDebug {
    std::optional<EntityId> owner{};
    int skinIndex{-1};
    bool showOverlay{false};
    std::vector<std::string> jointNames{};
    std::vector<int> parentIndices{};
    std::vector<glm::vec3> jointWorldPositions{};
    std::vector<glm::mat4> jointMatrices{};

    void clear() {
        owner.reset();
        skinIndex = -1;
        showOverlay = false;
        jointNames.clear();
        parentIndices.clear();
        jointWorldPositions.clear();
        jointMatrices.clear();
    }
};

struct FrameNavDebugPolygon {
    int id{-1};
    float elevationY{0.0f};
    std::vector<glm::vec3> vertices{};
    glm::vec4 color{1.0f};
};

struct FrameNavDebugLink {
    int id{-1};
    glm::vec3 fromPoint{0.0f};
    glm::vec3 toPoint{0.0f};
    bool bidirectional{true};
};

struct FrameNavigationDebug {
    std::vector<FrameNavDebugPolygon> polygons{};
    std::vector<FrameNavDebugLink> links{};
    std::vector<glm::vec3> path{};
    std::optional<glm::vec3> destination{};
    std::vector<glm::vec3> captureVertices{};
    float captureElevationY{0.0f};

    void clear() {
        polygons.clear();
        links.clear();
        path.clear();
        destination.reset();
        captureVertices.clear();
        captureElevationY = 0.0f;
    }
};

struct FrameSceneData {
    std::vector<FrameRenderable> renderables{};
    std::vector<FrameLight> lights{};
    std::vector<FrameLightVolume> lightVolumes{};
    std::vector<FrameColliderDebug> colliderDebug{};
    std::vector<glm::mat4> jointMatrices{};
    FrameSelection selection{};
    FrameSkeletonDebug selectionSkeleton{};
    FrameNavigationDebug navigation{};

    void reserve(std::size_t renderableCount, std::size_t lightCount, std::size_t volumeCount, std::size_t colliderCount = 0u) {
        renderables.reserve(renderableCount);
        lights.reserve(lightCount);
        lightVolumes.reserve(volumeCount);
        colliderDebug.reserve(colliderCount);
        jointMatrices.reserve(renderableCount * 64u);
    }

    void clear() {
        renderables.clear();
        lights.clear();
        lightVolumes.clear();
        colliderDebug.clear();
        jointMatrices.clear();
        selection = FrameSelection{};
        selectionSkeleton.clear();
        navigation.clear();
    }
};

}  // namespace core
