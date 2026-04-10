#include "StaticGltfModel.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <limits>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/matrix_decompose.hpp>
#include <spdlog/spdlog.h>

#include "render/engine/RenderTypes.hpp"

#define CGLTF_IMPLEMENTATION
#include "cgltf.h"

namespace {

constexpr std::size_t kJointInfluenceCount = 4u;

const char* cgltfResultName(cgltf_result result) {
    switch (result) {
        case cgltf_result_success:
            return "success";
        case cgltf_result_data_too_short:
            return "data_too_short";
        case cgltf_result_unknown_format:
            return "unknown_format";
        case cgltf_result_invalid_json:
            return "invalid_json";
        case cgltf_result_invalid_gltf:
            return "invalid_gltf";
        case cgltf_result_invalid_options:
            return "invalid_options";
        case cgltf_result_file_not_found:
            return "file_not_found";
        case cgltf_result_io_error:
            return "io_error";
        case cgltf_result_out_of_memory:
            return "out_of_memory";
        case cgltf_result_legacy_gltf:
            return "legacy_gltf";
        default:
            return "unknown";
    }
}

glm::mat4 toGlmMat4(const cgltf_float* values) {
    return glm::make_mat4(values);
}

render::AlphaMode alphaModeFromCgltf(cgltf_alpha_mode mode) {
    switch (mode) {
        case cgltf_alpha_mode_mask:
            return render::AlphaMode::Mask;
        case cgltf_alpha_mode_blend:
            return render::AlphaMode::Blend;
        case cgltf_alpha_mode_opaque:
        case cgltf_alpha_mode_max_enum:
        default:
            return render::AlphaMode::Opaque;
    }
}

render::AnimationInterpolation interpolationFromCgltf(cgltf_interpolation_type interpolation) {
    switch (interpolation) {
        case cgltf_interpolation_type_step:
            return render::AnimationInterpolation::Step;
        case cgltf_interpolation_type_cubic_spline:
            return render::AnimationInterpolation::CubicSpline;
        case cgltf_interpolation_type_linear:
        default:
            return render::AnimationInterpolation::Linear;
    }
}

std::string materialName(const cgltf_material* material, std::size_t fallbackIndex) {
    if (material != nullptr && material->name != nullptr && material->name[0] != '\0') {
        return material->name;
    }
    return "Material " + std::to_string(fallbackIndex);
}

std::shared_ptr<render::Material> buildMaterial(
    const cgltf_material* material,
    std::size_t fallbackIndex
) {
    auto outMaterial = std::make_shared<render::Material>();
    outMaterial->name = materialName(material, fallbackIndex);

    if (material == nullptr) {
        return outMaterial;
    }

    if (material->has_pbr_metallic_roughness) {
        outMaterial->baseColorFactor = glm::vec3(
            material->pbr_metallic_roughness.base_color_factor[0],
            material->pbr_metallic_roughness.base_color_factor[1],
            material->pbr_metallic_roughness.base_color_factor[2]
        );
        outMaterial->alphaFactor = material->pbr_metallic_roughness.base_color_factor[3];
        outMaterial->metallicFactor = material->pbr_metallic_roughness.metallic_factor;
        outMaterial->roughnessFactor = material->pbr_metallic_roughness.roughness_factor;
    }

    outMaterial->normalScale = material->normal_texture.scale;
    outMaterial->aoStrength = material->occlusion_texture.scale;
    outMaterial->emissiveFactor = glm::vec3(
        material->emissive_factor[0],
        material->emissive_factor[1],
        material->emissive_factor[2]
    );
    outMaterial->alphaMode = alphaModeFromCgltf(material->alpha_mode);
    outMaterial->alphaCutoff = material->alpha_cutoff;
    outMaterial->doubleSided = material->double_sided != 0;

    if (material->has_clearcoat) {
        outMaterial->clearcoat.factor = material->clearcoat.clearcoat_factor;
        outMaterial->clearcoat.roughness = material->clearcoat.clearcoat_roughness_factor;
    }

    return outMaterial;
}

bool readVec2(const cgltf_accessor* accessor, cgltf_size index, glm::vec2& outValue) {
    if (accessor == nullptr || accessor->type != cgltf_type_vec2) {
        return false;
    }

    cgltf_float values[2]{};
    if (!cgltf_accessor_read_float(accessor, index, values, 2)) {
        return false;
    }

    outValue = glm::vec2(values[0], values[1]);
    return true;
}

bool readVec3(const cgltf_accessor* accessor, cgltf_size index, glm::vec3& outValue) {
    if (accessor == nullptr || accessor->type != cgltf_type_vec3) {
        return false;
    }

    cgltf_float values[3]{};
    if (!cgltf_accessor_read_float(accessor, index, values, 3)) {
        return false;
    }

    outValue = glm::vec3(values[0], values[1], values[2]);
    return true;
}

bool readColor(const cgltf_accessor* accessor, cgltf_size index, glm::vec4& outValue) {
    if (accessor == nullptr) {
        return false;
    }

    if (accessor->type == cgltf_type_vec3) {
        cgltf_float values[3]{};
        if (!cgltf_accessor_read_float(accessor, index, values, 3)) {
            return false;
        }
        outValue = glm::vec4(values[0], values[1], values[2], 1.0f);
        return true;
    }

    if (accessor->type == cgltf_type_vec4) {
        cgltf_float values[4]{};
        if (!cgltf_accessor_read_float(accessor, index, values, 4)) {
            return false;
        }
        outValue = glm::vec4(values[0], values[1], values[2], values[3]);
        return true;
    }

    return false;
}

bool readVec4Float(const cgltf_accessor* accessor, cgltf_size index, glm::vec4& outValue) {
    if (accessor == nullptr || accessor->type != cgltf_type_vec4) {
        return false;
    }

    cgltf_float values[4]{};
    if (!cgltf_accessor_read_float(accessor, index, values, 4)) {
        return false;
    }

    outValue = glm::vec4(values[0], values[1], values[2], values[3]);
    return true;
}

bool readVec4UInt(const cgltf_accessor* accessor, cgltf_size index, glm::uvec4& outValue) {
    if (accessor == nullptr || accessor->type != cgltf_type_vec4) {
        return false;
    }

    cgltf_uint values[4]{};
    if (!cgltf_accessor_read_uint(accessor, index, values, 4)) {
        return false;
    }

    outValue = glm::uvec4(values[0], values[1], values[2], values[3]);
    return true;
}

std::string primitiveName(
    const cgltf_node* node,
    const cgltf_material* material,
    std::size_t primitiveIndex,
    std::size_t materialIndex
) {
    std::string baseName;
    if (node != nullptr && node->name != nullptr && node->name[0] != '\0') {
        baseName = node->name;
    } else {
        baseName = "Mesh Section";
    }

    std::string matName = materialName(material, materialIndex);
    if (primitiveIndex == 0u) {
        return baseName + " / " + matName;
    }
    return baseName + " / " + matName + " / Primitive " + std::to_string(primitiveIndex);
}

void gatherMeshNodes(const cgltf_node* node, std::vector<const cgltf_node*>& outNodes) {
    if (node == nullptr) {
        return;
    }

    if (node->mesh != nullptr) {
        outNodes.push_back(node);
    }

    for (cgltf_size i = 0; i < node->children_count; ++i) {
        gatherMeshNodes(node->children[i], outNodes);
    }
}

render::NodeTransform nodeTransformFromCgltf(const cgltf_node& node) {
    render::NodeTransform transform{};
    if (node.has_matrix != 0) {
        render::decomposeNodeTransform(toGlmMat4(node.matrix), transform);
        return transform;
    }

    if (node.has_translation != 0) {
        transform.translation = glm::vec3(
            node.translation[0],
            node.translation[1],
            node.translation[2]
        );
    }
    if (node.has_rotation != 0) {
        transform.rotation = glm::normalize(glm::quat(
            node.rotation[3],
            node.rotation[0],
            node.rotation[1],
            node.rotation[2]
        ));
    }
    if (node.has_scale != 0) {
        transform.scale = glm::vec3(node.scale[0], node.scale[1], node.scale[2]);
    }
    return transform;
}

bool readTimeTrack(const cgltf_accessor* accessor, std::vector<float>& outTimes) {
    if (accessor == nullptr || accessor->type != cgltf_type_scalar) {
        return false;
    }

    outTimes.resize(accessor->count);
    for (cgltf_size index = 0; index < accessor->count; ++index) {
        cgltf_float value = 0.0f;
        if (!cgltf_accessor_read_float(accessor, index, &value, 1)) {
            return false;
        }
        outTimes[index] = value;
    }
    return true;
}

bool readVec3TrackValues(
    const cgltf_accessor* accessor,
    render::AnimationInterpolation interpolation,
    std::size_t keyCount,
    std::vector<glm::vec3>& outValues
) {
    if (accessor == nullptr || accessor->type != cgltf_type_vec3) {
        return false;
    }

    outValues.clear();
    outValues.reserve(keyCount);

    const bool cubicSpline = interpolation == render::AnimationInterpolation::CubicSpline;
    const std::size_t sampleStride = cubicSpline ? 3u : 1u;
    if (accessor->count < keyCount * sampleStride) {
        return false;
    }

    for (std::size_t keyIndex = 0; keyIndex < keyCount; ++keyIndex) {
        const cgltf_size sampleIndex = static_cast<cgltf_size>(keyIndex * sampleStride + (cubicSpline ? 1u : 0u));
        glm::vec3 value(0.0f);
        if (!readVec3(accessor, sampleIndex, value)) {
            return false;
        }
        outValues.push_back(value);
    }
    return true;
}

bool readQuatTrackValues(
    const cgltf_accessor* accessor,
    render::AnimationInterpolation interpolation,
    std::size_t keyCount,
    std::vector<glm::quat>& outValues
) {
    if (accessor == nullptr || accessor->type != cgltf_type_vec4) {
        return false;
    }

    outValues.clear();
    outValues.reserve(keyCount);

    const bool cubicSpline = interpolation == render::AnimationInterpolation::CubicSpline;
    const std::size_t sampleStride = cubicSpline ? 3u : 1u;
    if (accessor->count < keyCount * sampleStride) {
        return false;
    }

    for (std::size_t keyIndex = 0; keyIndex < keyCount; ++keyIndex) {
        const cgltf_size sampleIndex = static_cast<cgltf_size>(keyIndex * sampleStride + (cubicSpline ? 1u : 0u));
        glm::vec4 value(0.0f);
        if (!readVec4Float(accessor, sampleIndex, value)) {
            return false;
        }
        outValues.push_back(glm::normalize(glm::quat(value.w, value.x, value.y, value.z)));
    }
    return true;
}

bool appendPrimitive(
    const cgltf_node* node,
    const cgltf_primitive& primitive,
    int nodeIndex,
    int skinIndex,
    std::unordered_map<const cgltf_material*, std::shared_ptr<render::Material>>& materialCache,
    std::size_t materialIndex,
    std::size_t primitiveIndex,
    render::GltfModelData& outModel
) {
    if (primitive.type != cgltf_primitive_type_triangles) {
        spdlog::error("StaticGltfModel: only triangle primitives are supported");
        return false;
    }

    const cgltf_accessor* positionAccessor = cgltf_find_accessor(&primitive, cgltf_attribute_type_position, 0);
    const cgltf_accessor* normalAccessor = cgltf_find_accessor(&primitive, cgltf_attribute_type_normal, 0);
    if (positionAccessor == nullptr || normalAccessor == nullptr) {
        spdlog::error("StaticGltfModel: primitive is missing POSITION or NORMAL data");
        return false;
    }
    if (positionAccessor->count != normalAccessor->count) {
        spdlog::error("StaticGltfModel: POSITION/NORMAL count mismatch");
        return false;
    }

    const cgltf_accessor* uv0Accessor = cgltf_find_accessor(&primitive, cgltf_attribute_type_texcoord, 0);
    const cgltf_accessor* uv1Accessor = cgltf_find_accessor(&primitive, cgltf_attribute_type_texcoord, 1);
    const cgltf_accessor* colorAccessor = cgltf_find_accessor(&primitive, cgltf_attribute_type_color, 0);
    const cgltf_accessor* jointsAccessor = cgltf_find_accessor(&primitive, cgltf_attribute_type_joints, 0);
    const cgltf_accessor* weightsAccessor = cgltf_find_accessor(&primitive, cgltf_attribute_type_weights, 0);

    const bool isSkinned = skinIndex >= 0;
    if (isSkinned) {
        if (jointsAccessor == nullptr || weightsAccessor == nullptr) {
            spdlog::error("StaticGltfModel: skinned primitive is missing JOINTS_0 or WEIGHTS_0");
            return false;
        }
        if (jointsAccessor->count != positionAccessor->count || weightsAccessor->count != positionAccessor->count) {
            spdlog::error("StaticGltfModel: skinned attribute counts do not match POSITION count");
            return false;
        }
    }

    render::Mesh mesh{};
    mesh.positions.reserve(positionAccessor->count);
    mesh.normals.reserve(positionAccessor->count);
    mesh.colors.reserve(positionAccessor->count);
    mesh.uvSets.resize(2);
    mesh.uvSets[0].reserve(positionAccessor->count);
    mesh.uvSets[1].reserve(positionAccessor->count);
    if (isSkinned) {
        mesh.jointIndices.reserve(positionAccessor->count);
        mesh.jointWeights.reserve(positionAccessor->count);
    }

    for (cgltf_size vertexIndex = 0; vertexIndex < positionAccessor->count; ++vertexIndex) {
        glm::vec3 position(0.0f);
        glm::vec3 normal(0.0f, 1.0f, 0.0f);
        if (!readVec3(positionAccessor, vertexIndex, position) || !readVec3(normalAccessor, vertexIndex, normal)) {
            spdlog::error("StaticGltfModel: failed to read vertex {}", vertexIndex);
            return false;
        }

        glm::vec2 uv0(0.0f);
        glm::vec2 uv1(0.0f);
        glm::vec4 color(1.0f);
        if (uv0Accessor != nullptr) {
            readVec2(uv0Accessor, vertexIndex, uv0);
        }
        if (uv1Accessor != nullptr) {
            readVec2(uv1Accessor, vertexIndex, uv1);
        }
        if (colorAccessor != nullptr) {
            readColor(colorAccessor, vertexIndex, color);
        }

        mesh.positions.push_back(position);
        mesh.normals.push_back(normal);
        mesh.uvSets[0].push_back(uv0);
        mesh.uvSets[1].push_back(uv1);
        mesh.colors.push_back(color);

        if (isSkinned) {
            glm::uvec4 joints(0u);
            glm::vec4 weights(0.0f);
            if (!readVec4UInt(jointsAccessor, vertexIndex, joints) ||
                !readVec4Float(weightsAccessor, vertexIndex, weights)) {
                spdlog::error("StaticGltfModel: failed to read skinning data for vertex {}", vertexIndex);
                return false;
            }

            const float weightSum = weights.x + weights.y + weights.z + weights.w;
            if (weightSum > 0.0f) {
                weights /= weightSum;
            }

            mesh.jointIndices.push_back(joints);
            mesh.jointWeights.push_back(weights);
        }
    }

    if (primitive.indices != nullptr) {
        mesh.indices.reserve(static_cast<std::size_t>(primitive.indices->count));
        for (cgltf_size index = 0; index < primitive.indices->count; ++index) {
            const cgltf_size primitiveIndexValue = cgltf_accessor_read_index(primitive.indices, index);
            if (primitiveIndexValue >= positionAccessor->count) {
                spdlog::error("StaticGltfModel: index {} is out of range", primitiveIndexValue);
                return false;
            }
            mesh.indices.push_back(static_cast<unsigned int>(primitiveIndexValue));
        }
    } else {
        if ((positionAccessor->count % 3u) != 0u) {
            spdlog::error("StaticGltfModel: non-indexed triangle primitive has invalid vertex count");
            return false;
        }

        mesh.indices.reserve(static_cast<std::size_t>(positionAccessor->count));
        for (cgltf_size index = 0; index < positionAccessor->count; ++index) {
            mesh.indices.push_back(static_cast<unsigned int>(index));
        }
    }

    render::ensureMeshAttributeSizes(mesh, 2);
    render::computeTangents(mesh);

    std::shared_ptr<render::Material> material;
    auto cached = materialCache.find(primitive.material);
    if (cached != materialCache.end()) {
        material = cached->second;
    } else {
        material = buildMaterial(primitive.material, materialIndex);
        materialCache.emplace(primitive.material, material);
    }

    render::GltfMeshSection section{};
    section.name = primitiveName(node, primitive.material, primitiveIndex, materialIndex);
    section.nodeIndex = nodeIndex;
    section.skinIndex = skinIndex;
    section.mesh = std::move(mesh);
    section.material = std::move(material);
    outModel.sections.push_back(std::move(section));
    return true;
}

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

}  // namespace

namespace render {

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

bool loadGltfModel(const std::string& path, GltfModelData& outModel) {
    outModel = GltfModelData{};

    cgltf_options options{};
    cgltf_data* data = nullptr;

    cgltf_result result = cgltf_parse_file(&options, path.c_str(), &data);
    if (result != cgltf_result_success) {
        spdlog::error("StaticGltfModel: failed to parse '{}': {}", path, cgltfResultName(result));
        return false;
    }

    const auto cleanup = [&]() {
        if (data != nullptr) {
            cgltf_free(data);
            data = nullptr;
        }
    };

    result = cgltf_load_buffers(&options, data, path.c_str());
    if (result != cgltf_result_success) {
        spdlog::error("StaticGltfModel: failed to load buffers for '{}': {}", path, cgltfResultName(result));
        cleanup();
        return false;
    }

    result = cgltf_validate(data);
    if (result != cgltf_result_success) {
        spdlog::error("StaticGltfModel: validation failed for '{}': {}", path, cgltfResultName(result));
        cleanup();
        return false;
    }

    outModel.nodes.resize(data->nodes_count);
    for (cgltf_size nodeIndex = 0; nodeIndex < data->nodes_count; ++nodeIndex) {
        const cgltf_node& node = data->nodes[nodeIndex];
        GltfNode& outNode = outModel.nodes[nodeIndex];
        if (node.name != nullptr && node.name[0] != '\0') {
            outNode.name = node.name;
        } else {
            outNode.name = "Node " + std::to_string(nodeIndex);
        }
        outNode.parentIndex = -1;
        outNode.meshIndex = node.mesh != nullptr ? static_cast<int>(node.mesh - data->meshes) : -1;
        outNode.skinIndex = node.skin != nullptr ? static_cast<int>(node.skin - data->skins) : -1;
        outNode.localTransform = nodeTransformFromCgltf(node);
    }

    for (cgltf_size parentIndex = 0; parentIndex < data->nodes_count; ++parentIndex) {
        const cgltf_node& node = data->nodes[parentIndex];
        for (cgltf_size childIndex = 0; childIndex < node.children_count; ++childIndex) {
            const int resolvedChildIndex = static_cast<int>(node.children[childIndex] - data->nodes);
            if (resolvedChildIndex >= 0 && resolvedChildIndex < static_cast<int>(outModel.nodes.size())) {
                outModel.nodes[static_cast<std::size_t>(resolvedChildIndex)].parentIndex = static_cast<int>(parentIndex);
            }
        }
    }

    std::vector<std::vector<int>> childrenByParent(outModel.nodes.size());
    for (std::size_t nodeIndex = 0; nodeIndex < outModel.nodes.size(); ++nodeIndex) {
        const int parentIndex = outModel.nodes[nodeIndex].parentIndex;
        if (parentIndex >= 0 && parentIndex < static_cast<int>(childrenByParent.size())) {
            childrenByParent[static_cast<std::size_t>(parentIndex)].push_back(static_cast<int>(nodeIndex));
        }
    }

    const cgltf_scene* scene = data->scene != nullptr
        ? data->scene
        : (data->scenes_count > 0 ? &data->scenes[0] : nullptr);

    if (scene != nullptr) {
        outModel.sceneRootNodes.reserve(scene->nodes_count);
        for (cgltf_size index = 0; index < scene->nodes_count; ++index) {
            outModel.sceneRootNodes.push_back(static_cast<int>(scene->nodes[index] - data->nodes));
        }
    } else {
        for (cgltf_size nodeIndex = 0; nodeIndex < data->nodes_count; ++nodeIndex) {
            if (outModel.nodes[nodeIndex].parentIndex < 0) {
                outModel.sceneRootNodes.push_back(static_cast<int>(nodeIndex));
            }
        }
    }

    outModel.skins.reserve(data->skins_count);
    for (cgltf_size skinIndex = 0; skinIndex < data->skins_count; ++skinIndex) {
        const cgltf_skin& skin = data->skins[skinIndex];
        SkinData outSkin{};
        if (skin.name != nullptr && skin.name[0] != '\0') {
            outSkin.name = skin.name;
        } else {
            outSkin.name = "Skin " + std::to_string(skinIndex);
        }
        outSkin.skeletonRootNode = skin.skeleton != nullptr ? static_cast<int>(skin.skeleton - data->nodes) : -1;
        outSkin.jointNodeIndices.reserve(skin.joints_count);
        outSkin.parentJointIndices.reserve(skin.joints_count);
        outSkin.jointNames.reserve(skin.joints_count);
        outSkin.inverseBindMatrices.reserve(skin.joints_count);

        for (cgltf_size jointIndex = 0; jointIndex < skin.joints_count; ++jointIndex) {
            const int nodeIndex = static_cast<int>(skin.joints[jointIndex] - data->nodes);
            outSkin.jointNodeIndices.push_back(nodeIndex);
            outSkin.jointNames.push_back(
                nodeIndex >= 0 && nodeIndex < static_cast<int>(outModel.nodes.size())
                    ? outModel.nodes[static_cast<std::size_t>(nodeIndex)].name
                    : ("Joint " + std::to_string(jointIndex))
            );

            int parentJointIndex = -1;
            if (nodeIndex >= 0 && nodeIndex < static_cast<int>(outModel.nodes.size())) {
                const int parentNodeIndex = outModel.nodes[static_cast<std::size_t>(nodeIndex)].parentIndex;
                for (std::size_t searchIndex = 0; searchIndex < outSkin.jointNodeIndices.size(); ++searchIndex) {
                    if (outSkin.jointNodeIndices[searchIndex] == parentNodeIndex) {
                        parentJointIndex = static_cast<int>(searchIndex);
                        break;
                    }
                }
            }
            outSkin.parentJointIndices.push_back(parentJointIndex);

            glm::mat4 inverseBind(1.0f);
            if (skin.inverse_bind_matrices != nullptr) {
                cgltf_float values[16]{};
                if (!cgltf_accessor_read_float(skin.inverse_bind_matrices, jointIndex, values, 16)) {
                    spdlog::error("StaticGltfModel: failed to read inverse bind matrix {}", jointIndex);
                    cleanup();
                    return false;
                }
                inverseBind = toGlmMat4(values);
            }
            outSkin.inverseBindMatrices.push_back(inverseBind);
        }

        buildSkinSkeletonHierarchy(childrenByParent, outSkin);
        outModel.skins.push_back(std::move(outSkin));
    }

    refreshModelBindPose(outModel);

    outModel.animations.reserve(data->animations_count);
    for (cgltf_size animationIndex = 0; animationIndex < data->animations_count; ++animationIndex) {
        const cgltf_animation& animation = data->animations[animationIndex];
        AnimationClip clip{};
        if (animation.name != nullptr && animation.name[0] != '\0') {
            clip.name = animation.name;
        } else {
            clip.name = "Animation " + std::to_string(animationIndex);
        }
        clip.nodeChannels.resize(outModel.nodes.size());

        for (cgltf_size channelIndex = 0; channelIndex < animation.channels_count; ++channelIndex) {
            const cgltf_animation_channel& channel = animation.channels[channelIndex];
            if (channel.target_node == nullptr || channel.sampler == nullptr) {
                continue;
            }

            const int nodeIndex = static_cast<int>(channel.target_node - data->nodes);
            if (nodeIndex < 0 || nodeIndex >= static_cast<int>(clip.nodeChannels.size())) {
                continue;
            }

            const render::AnimationInterpolation interpolation =
                interpolationFromCgltf(channel.sampler->interpolation);

            std::vector<float> times{};
            if (!readTimeTrack(channel.sampler->input, times) || times.empty()) {
                spdlog::error("StaticGltfModel: failed to read animation times for '{}'", clip.name);
                cleanup();
                return false;
            }

            clip.duration = std::max(clip.duration, times.back());
            NodeAnimationChannels& nodeChannels = clip.nodeChannels[static_cast<std::size_t>(nodeIndex)];

            switch (channel.target_path) {
                case cgltf_animation_path_type_translation: {
                    std::vector<glm::vec3> values{};
                    if (!readVec3TrackValues(channel.sampler->output, interpolation, times.size(), values)) {
                        spdlog::error("StaticGltfModel: failed to read translation track for '{}'", clip.name);
                        cleanup();
                        return false;
                    }
                    nodeChannels.translation = Vec3AnimationTrack{interpolation, std::move(times), std::move(values)};
                    break;
                }
                case cgltf_animation_path_type_rotation: {
                    std::vector<glm::quat> values{};
                    if (!readQuatTrackValues(channel.sampler->output, interpolation, times.size(), values)) {
                        spdlog::error("StaticGltfModel: failed to read rotation track for '{}'", clip.name);
                        cleanup();
                        return false;
                    }
                    nodeChannels.rotation = QuatAnimationTrack{interpolation, std::move(times), std::move(values)};
                    break;
                }
                case cgltf_animation_path_type_scale: {
                    std::vector<glm::vec3> values{};
                    if (!readVec3TrackValues(channel.sampler->output, interpolation, times.size(), values)) {
                        spdlog::error("StaticGltfModel: failed to read scale track for '{}'", clip.name);
                        cleanup();
                        return false;
                    }
                    nodeChannels.scale = Vec3AnimationTrack{interpolation, std::move(times), std::move(values)};
                    break;
                }
                default:
                    break;
            }
        }

        outModel.animations.push_back(std::move(clip));
    }

    std::vector<const cgltf_node*> meshNodes;
    if (scene != nullptr) {
        for (cgltf_size index = 0; index < scene->nodes_count; ++index) {
            gatherMeshNodes(scene->nodes[index], meshNodes);
        }
    } else {
        for (cgltf_size index = 0; index < data->nodes_count; ++index) {
            if (data->nodes[index].mesh != nullptr) {
                meshNodes.push_back(&data->nodes[index]);
            }
        }
    }

    if (meshNodes.empty()) {
        spdlog::error("StaticGltfModel: no mesh nodes found in '{}'", path);
        cleanup();
        return false;
    }

    std::unordered_map<const cgltf_material*, std::shared_ptr<Material>> materialCache;
    std::size_t materialIndex = 0u;
    for (const cgltf_node* node : meshNodes) {
        const int nodeIndex = static_cast<int>(node - data->nodes);
        const int skinIndex = node->skin != nullptr ? static_cast<int>(node->skin - data->skins) : -1;
        for (cgltf_size primitiveIndex = 0; primitiveIndex < node->mesh->primitives_count; ++primitiveIndex) {
            if (!appendPrimitive(
                    node,
                    node->mesh->primitives[primitiveIndex],
                    nodeIndex,
                    skinIndex,
                    materialCache,
                    materialIndex++,
                    primitiveIndex,
                    outModel
                )) {
                cleanup();
                return false;
            }
        }
    }

    if (!outModel.animated() && !centerAndGroundModel(outModel)) {
        cleanup();
        return false;
    }

    cleanup();
    return true;
}

bool loadStaticGltfModel(const std::string& path, StaticModelData& outModel) {
    return loadGltfModel(path, outModel);
}

bool loadStaticCharacterModel(const std::string& path, StaticModelData& outModel) {
    return loadGltfModel(path, outModel);
}

}  // namespace render
