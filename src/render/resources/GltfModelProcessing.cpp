#include "GltfImportDetail.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <unordered_map>

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/matrix_decompose.hpp>
#include <spdlog/spdlog.h>

namespace render::gltf_detail {
namespace {

render::Bounds3 computeModelBounds(const render::GltfModelData& model) {
    render::Bounds3 bounds{};
    bool hasVertex = false;
    for (const render::GltfMeshSection& section : model.sections) {
        if (section.nodeIndex < 0 || section.nodeIndex >= static_cast<int>(model.nodes.size())) {
            continue;
        }

        const glm::mat4 nodeMatrix = model.nodes[section.nodeIndex].bindGlobalMatrix;
        for (const glm::vec3& position : section.mesh.positions) {
            const glm::vec3 worldPosition = glm::vec3(nodeMatrix * glm::vec4(position, 1.0f));
            if (!hasVertex) {
                bounds.min = worldPosition;
                bounds.max = worldPosition;
                hasVertex = true;
                continue;
            }
            bounds.min = glm::min(bounds.min, worldPosition);
            bounds.max = glm::max(bounds.max, worldPosition);
        }
    }
    return bounds;
}

}  // namespace

bool centerAndGroundModel(render::GltfModelData& model) {
    if (model.sections.empty()) {
        spdlog::error("StaticGltfModel: generated model data is empty");
        return false;
    }

    const render::Bounds3 bounds = computeModelBounds(model);
    const glm::vec3 center = (bounds.min + bounds.max) * 0.5f;
    const glm::vec3 translation(-center.x, -bounds.min.y, -center.z);

    for (int rootIndex : model.sceneRootNodes) {
        if (rootIndex < 0 || rootIndex >= static_cast<int>(model.nodes.size())) {
            continue;
        }
        model.nodes[rootIndex].localTransform.translation += translation;
    }
    render::refreshModelBindPose(model);
    return true;
}

void buildSkinSkeletonHierarchy(
    const std::vector<std::vector<int>>& childrenByParent,
    render::SkinData& skin
) {
    skin.skeletonNodeIndices.clear();
    skin.skeletonParentIndices.clear();
    skin.skeletonJointIndices.clear();

    if (skin.skeletonRootNode < 0) {
        skin.skeletonNodeIndices = skin.jointNodeIndices;
        skin.skeletonParentIndices = skin.parentJointIndices;
        skin.skeletonJointIndices.resize(skin.jointNodeIndices.size(), -1);
        for (std::size_t jointIndex = 0; jointIndex < skin.jointNodeIndices.size(); ++jointIndex) {
            skin.skeletonJointIndices[jointIndex] = static_cast<int>(jointIndex);
        }
        return;
    }

    std::vector<int> jointIndexByNode(childrenByParent.size(), -1);
    for (std::size_t jointIndex = 0; jointIndex < skin.jointNodeIndices.size(); ++jointIndex) {
        const int nodeIndex = skin.jointNodeIndices[jointIndex];
        if (nodeIndex >= 0 && nodeIndex < static_cast<int>(jointIndexByNode.size())) {
            jointIndexByNode[static_cast<std::size_t>(nodeIndex)] = static_cast<int>(jointIndex);
        }
    }

    const auto appendNode = [&](const auto& self, int nodeIndex, int parentDisplayIndex) -> void {
        if (nodeIndex < 0 || nodeIndex >= static_cast<int>(childrenByParent.size())) {
            return;
        }

        const int displayIndex = static_cast<int>(skin.skeletonNodeIndices.size());
        skin.skeletonNodeIndices.push_back(nodeIndex);
        skin.skeletonParentIndices.push_back(parentDisplayIndex);
        skin.skeletonJointIndices.push_back(jointIndexByNode[static_cast<std::size_t>(nodeIndex)]);

        for (int childNodeIndex : childrenByParent[static_cast<std::size_t>(nodeIndex)]) {
            self(self, childNodeIndex, displayIndex);
        }
    };

    appendNode(appendNode, skin.skeletonRootNode, -1);
}

}  // namespace render::gltf_detail

namespace render {
namespace {

constexpr std::size_t kJointInfluenceCount = 4u;

}  // namespace

void buildJointInfluenceBounds(GltfMeshSection& section) {
    section.jointInfluenceBounds.clear();
    if (!section.skinned() || section.mesh.positions.empty() || !section.mesh.skinned()) {
        return;
    }

    if (section.mesh.jointIndices.size() < section.mesh.positions.size() ||
        section.mesh.jointWeights.size() < section.mesh.positions.size()) {
        return;
    }

    std::unordered_map<int, std::size_t> boundsIndexByJoint{};
    boundsIndexByJoint.reserve(section.mesh.positions.size());
    for (std::size_t vertexIndex = 0; vertexIndex < section.mesh.positions.size(); ++vertexIndex) {
        const glm::vec3& position = section.mesh.positions[vertexIndex];
        const glm::uvec4 joints = section.mesh.jointIndices[vertexIndex];
        const glm::vec4 weights = section.mesh.jointWeights[vertexIndex];
        const std::array<int, kJointInfluenceCount> jointValues{
            static_cast<int>(joints.x),
            static_cast<int>(joints.y),
            static_cast<int>(joints.z),
            static_cast<int>(joints.w),
        };
        const std::array<float, kJointInfluenceCount> weightValues{
            weights.x,
            weights.y,
            weights.z,
            weights.w,
        };

        for (std::size_t influenceIndex = 0; influenceIndex < kJointInfluenceCount; ++influenceIndex) {
            if (weightValues[influenceIndex] <= 0.0f) {
                continue;
            }

            const int jointIndex = jointValues[influenceIndex];
            auto [it, inserted] = boundsIndexByJoint.try_emplace(jointIndex, section.jointInfluenceBounds.size());
            if (inserted) {
                section.jointInfluenceBounds.push_back(JointInfluenceBounds{
                    jointIndex,
                    Bounds3{position, position},
                });
                continue;
            }

            JointInfluenceBounds& jointBounds = section.jointInfluenceBounds[it->second];
            jointBounds.localBounds.min = glm::min(jointBounds.localBounds.min, position);
            jointBounds.localBounds.max = glm::max(jointBounds.localBounds.max, position);
        }
    }
}

glm::mat4 composeNodeTransform(const NodeTransform& transform) {
    glm::mat4 matrix(1.0f);
    matrix = glm::translate(matrix, transform.translation);
    matrix *= glm::mat4_cast(transform.rotation);
    matrix = glm::scale(matrix, transform.scale);
    return matrix;
}

bool decomposeNodeTransform(const glm::mat4& matrix, NodeTransform& outTransform) {
    glm::vec3 skew(0.0f);
    glm::vec4 perspective(0.0f);
    glm::quat rotation;
    glm::vec3 translation(0.0f);
    glm::vec3 scale(1.0f);
    if (!glm::decompose(matrix, scale, rotation, translation, skew, perspective)) {
        return false;
    }

    outTransform.translation = translation;
    outTransform.rotation = glm::normalize(rotation);
    outTransform.scale = scale;
    return true;
}

void refreshModelBindPose(GltfModelData& model) {
    std::vector<std::uint8_t> resolved(model.nodes.size(), 0u);

    const auto resolveNode = [&](const auto& self, int nodeIndex) -> glm::mat4 {
        if (nodeIndex < 0 || nodeIndex >= static_cast<int>(model.nodes.size())) {
            return glm::mat4(1.0f);
        }

        GltfNode& node = model.nodes[static_cast<std::size_t>(nodeIndex)];
        if (resolved[static_cast<std::size_t>(nodeIndex)] != 0u) {
            return node.bindGlobalMatrix;
        }

        glm::mat4 matrix = composeNodeTransform(node.localTransform);
        if (node.parentIndex >= 0) {
            matrix = self(self, node.parentIndex) * matrix;
        }

        node.bindGlobalMatrix = matrix;
        resolved[static_cast<std::size_t>(nodeIndex)] = 1u;
        return matrix;
    };

    for (int nodeIndex = 0; nodeIndex < static_cast<int>(model.nodes.size()); ++nodeIndex) {
        resolveNode(resolveNode, nodeIndex);
    }
}

int findDefaultAnimationClipIndex(const GltfModelData& model) {
    for (std::size_t clipIndex = 0; clipIndex < model.animations.size(); ++clipIndex) {
        std::string lowered = model.animations[clipIndex].name;
        std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char value) {
            return static_cast<char>(std::tolower(value));
        });
        if (lowered.find("idle") != std::string::npos) {
            return static_cast<int>(clipIndex);
        }
    }

    return model.animations.empty() ? -1 : 0;
}

}  // namespace render
