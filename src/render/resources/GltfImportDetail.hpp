#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include "StaticGltfModel.hpp"
#include "cgltf/cgltf.h"

namespace render::gltf_detail {

[[nodiscard]] const char* cgltfResultName(cgltf_result result);
[[nodiscard]] glm::mat4 toGlmMat4(const cgltf_float* values);
[[nodiscard]] AlphaMode alphaModeFromCgltf(cgltf_alpha_mode mode);
[[nodiscard]] AnimationInterpolation interpolationFromCgltf(cgltf_interpolation_type type);
[[nodiscard]] std::shared_ptr<Material> buildMaterial(
    const cgltf_material* material,
    std::size_t fallbackIndex
);
bool readVec2(const cgltf_accessor* accessor, cgltf_size index, glm::vec2& outValue);
bool readVec3(const cgltf_accessor* accessor, cgltf_size index, glm::vec3& outValue);
bool readColor(const cgltf_accessor* accessor, cgltf_size index, glm::vec4& outValue);
bool readVec4Float(const cgltf_accessor* accessor, cgltf_size index, glm::vec4& outValue);
bool readVec4UInt(const cgltf_accessor* accessor, cgltf_size index, glm::uvec4& outValue);
[[nodiscard]] std::string primitiveName(
    const cgltf_node* node,
    const cgltf_material* material,
    std::size_t primitiveIndex,
    std::size_t materialIndex
);
void gatherMeshNodes(const cgltf_node* node, std::vector<const cgltf_node*>& outNodes);
[[nodiscard]] NodeTransform nodeTransformFromCgltf(const cgltf_node& node);
bool readTimeTrack(const cgltf_accessor* accessor, std::vector<float>& outTimes);
bool readVec3TrackValues(
    const cgltf_accessor* accessor,
    AnimationInterpolation interpolation,
    std::size_t keyCount,
    std::vector<glm::vec3>& outValues
);
bool readQuatTrackValues(
    const cgltf_accessor* accessor,
    AnimationInterpolation interpolation,
    std::size_t keyCount,
    std::vector<glm::quat>& outValues
);
bool centerAndGroundModel(GltfModelData& model);
void buildSkinSkeletonHierarchy(
    const std::vector<std::vector<int>>& childrenByParent,
    SkinData& skin
);

}  // namespace render::gltf_detail
