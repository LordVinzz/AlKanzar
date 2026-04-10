#pragma once

#include <memory>
#include <string>
#include <vector>

#include <glm/mat4x4.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/vec3.hpp>

#include "Entity.hpp"
#include "core/lighting/MaterialLibrary.hpp"
#include "render/engine/RenderTypes.hpp"

namespace render {
struct GltfModelData;
}

namespace core {

struct AnimationLocalPose {
    glm::vec3 translation{0.0f};
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 scale{1.0f};
};

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

struct AnimatedModelComponent {
    std::shared_ptr<render::GltfModelData> model{};
    int currentClip{-1};
    int nextClip{-1};
    int requestedClip{-1};
    float currentTime{0.0f};
    float nextTime{0.0f};
    bool playing{true};
    bool loop{true};
    float speed{1.0f};
    float blendDuration{0.2f};
    float blendElapsed{0.0f};
    bool showSkeletonOverlay{true};
    std::vector<AnimationLocalPose> localPose{};
    std::vector<glm::mat4> globalNodeMatrices{};
    std::vector<std::vector<glm::mat4>> skinJointMatrices{};
};

struct SkinnedRenderableComponent {
    EntityId animationOwner{};
    int skinIndex{-1};
    int nodeIndex{-1};
    int sectionIndex{-1};
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
