#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <glm/mat4x4.hpp>
#include <glm/gtc/quaternion.hpp>

#include "Geometry.hpp"
#include "Material.hpp"

namespace render {

enum class AnimationInterpolation {
    Step = 0,
    Linear,
    CubicSpline,
};

struct NodeTransform {
    glm::vec3 translation{0.0f};
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 scale{1.0f};
};

struct Vec3AnimationTrack {
    AnimationInterpolation interpolation{AnimationInterpolation::Linear};
    std::vector<float> times{};
    std::vector<glm::vec3> values{};
};

struct QuatAnimationTrack {
    AnimationInterpolation interpolation{AnimationInterpolation::Linear};
    std::vector<float> times{};
    std::vector<glm::quat> values{};
};

struct NodeAnimationChannels {
    std::optional<Vec3AnimationTrack> translation{};
    std::optional<QuatAnimationTrack> rotation{};
    std::optional<Vec3AnimationTrack> scale{};
};

struct AnimationClip {
    std::string name{};
    float duration{0.0f};
    std::vector<NodeAnimationChannels> nodeChannels{};
};

struct SkinData {
    std::string name{};
    int skeletonRootNode{-1};
    std::vector<int> jointNodeIndices{};
    std::vector<int> parentJointIndices{};
    std::vector<std::string> jointNames{};
    std::vector<glm::mat4> inverseBindMatrices{};
    std::vector<int> skeletonNodeIndices{};
    std::vector<int> skeletonParentIndices{};
    std::vector<int> skeletonJointIndices{};
};

struct GltfNode {
    std::string name{};
    int parentIndex{-1};
    int meshIndex{-1};
    int skinIndex{-1};
    NodeTransform localTransform{};
    glm::mat4 bindGlobalMatrix{1.0f};
};

struct GltfMeshSection {
    std::string name{};
    int nodeIndex{-1};
    int skinIndex{-1};
    Mesh mesh{};
    std::shared_ptr<Material> material{};

    [[nodiscard]] bool skinned() const {
        return skinIndex >= 0;
    }
};

struct GltfModelData {
    std::vector<GltfMeshSection> sections{};
    std::vector<GltfNode> nodes{};
    std::vector<int> sceneRootNodes{};
    std::vector<SkinData> skins{};
    std::vector<AnimationClip> animations{};

    [[nodiscard]] bool animated() const {
        return !skins.empty() || !animations.empty();
    }
};

using StaticMeshSection = GltfMeshSection;
using StaticModelData = GltfModelData;

glm::mat4 composeNodeTransform(const NodeTransform& transform);
bool decomposeNodeTransform(const glm::mat4& matrix, NodeTransform& outTransform);
void refreshModelBindPose(GltfModelData& model);
int findDefaultAnimationClipIndex(const GltfModelData& model);

bool loadGltfModel(const std::string& path, GltfModelData& outModel);
bool loadStaticGltfModel(const std::string& path, StaticModelData& outModel);
bool loadStaticCharacterModel(const std::string& path, StaticModelData& outModel);

}  // namespace render
