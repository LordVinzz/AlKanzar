#include "StaticGltfModel.hpp"

#include <array>
#include <cstddef>
#include <limits>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_inverse.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <spdlog/spdlog.h>

#define CGLTF_IMPLEMENTATION
#include "vendor/cgltf.h"

namespace {

constexpr std::size_t kJointInfluenceCount = 4;

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

bool readVec4UInt(const cgltf_accessor* accessor, cgltf_size index, std::array<cgltf_uint, 4>& outValue) {
    if (accessor == nullptr || accessor->type != cgltf_type_vec4) {
        return false;
    }

    cgltf_uint values[4]{};
    if (!cgltf_accessor_read_uint(accessor, index, values, 4)) {
        return false;
    }

    outValue = {values[0], values[1], values[2], values[3]};
    return true;
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

bool buildSkinMatrices(
    const cgltf_skin* skin,
    std::vector<glm::mat4>& outJointMatrices,
    std::vector<glm::mat3>& outJointNormalMatrices
) {
    if (skin == nullptr) {
        return false;
    }

    outJointMatrices.resize(skin->joints_count, glm::mat4(1.0f));
    outJointNormalMatrices.resize(skin->joints_count, glm::mat3(1.0f));

    for (cgltf_size i = 0; i < skin->joints_count; ++i) {
        cgltf_float worldMatrixValues[16]{};
        cgltf_node_transform_world(skin->joints[i], worldMatrixValues);
        glm::mat4 jointWorld = toGlmMat4(worldMatrixValues);

        glm::mat4 inverseBind(1.0f);
        if (skin->inverse_bind_matrices != nullptr) {
            cgltf_float inverseBindValues[16]{};
            if (!cgltf_accessor_read_float(skin->inverse_bind_matrices, i, inverseBindValues, 16)) {
                spdlog::error("StaticGltfModel: failed to read inverse bind matrix {}", i);
                return false;
            }
            inverseBind = toGlmMat4(inverseBindValues);
        }

        const glm::mat4 jointMatrix = jointWorld * inverseBind;
        outJointMatrices[i] = jointMatrix;
        outJointNormalMatrices[i] = glm::mat3(glm::transpose(glm::inverse(jointMatrix)));
    }

    return true;
}

bool appendPrimitive(
    const cgltf_primitive& primitive,
    const glm::mat4& nodeWorld,
    const glm::mat3& nodeNormalMatrix,
    const std::vector<glm::mat4>* jointMatrices,
    const std::vector<glm::mat3>* jointNormalMatrices,
    std::unordered_map<const cgltf_material*, std::shared_ptr<render::Material>>& materialCache,
    std::size_t materialIndex,
    render::StaticModelData& outModel
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

    const bool isSkinned = jointMatrices != nullptr && jointNormalMatrices != nullptr;
    const cgltf_accessor* jointsAccessor = cgltf_find_accessor(&primitive, cgltf_attribute_type_joints, 0);
    const cgltf_accessor* weightsAccessor = cgltf_find_accessor(&primitive, cgltf_attribute_type_weights, 0);
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

    for (cgltf_size vertexIndex = 0; vertexIndex < positionAccessor->count; ++vertexIndex) {
        glm::vec3 position(0.0f);
        glm::vec3 normal(0.0f);
        if (!readVec3(positionAccessor, vertexIndex, position) || !readVec3(normalAccessor, vertexIndex, normal)) {
            spdlog::error("StaticGltfModel: failed to read vertex {}", vertexIndex);
            return false;
        }

        glm::vec3 worldPosition(0.0f);
        glm::vec3 worldNormal(0.0f);

        if (isSkinned) {
            std::array<cgltf_uint, kJointInfluenceCount> joints{};
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

            for (std::size_t jointSlot = 0; jointSlot < kJointInfluenceCount; ++jointSlot) {
                const float weight = weights[static_cast<glm::length_t>(jointSlot)];
                if (weight <= 0.0f) {
                    continue;
                }

                const std::size_t jointIndex = static_cast<std::size_t>(joints[jointSlot]);
                if (jointIndex >= jointMatrices->size() || jointIndex >= jointNormalMatrices->size()) {
                    spdlog::error("StaticGltfModel: joint index {} is out of range", jointIndex);
                    return false;
                }

                worldPosition += glm::vec3((*jointMatrices)[jointIndex] * glm::vec4(position, 1.0f)) * weight;
                worldNormal += (*jointNormalMatrices)[jointIndex] * normal * weight;
            }

            if (weightSum <= 0.0f) {
                worldPosition = glm::vec3(nodeWorld * glm::vec4(position, 1.0f));
                worldNormal = nodeNormalMatrix * normal;
            }
        } else {
            worldPosition = glm::vec3(nodeWorld * glm::vec4(position, 1.0f));
            worldNormal = nodeNormalMatrix * normal;
        }

        worldNormal = glm::normalize(worldNormal);

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

        mesh.positions.push_back(worldPosition);
        mesh.normals.push_back(worldNormal);
        mesh.uvSets[0].push_back(uv0);
        mesh.uvSets[1].push_back(uv1);
        mesh.colors.push_back(color);
    }

    if (primitive.indices != nullptr) {
        mesh.indices.reserve(static_cast<std::size_t>(primitive.indices->count));
        for (cgltf_size index = 0; index < primitive.indices->count; ++index) {
            const cgltf_size primitiveIndex = cgltf_accessor_read_index(primitive.indices, index);
            if (primitiveIndex >= positionAccessor->count) {
                spdlog::error("StaticGltfModel: index {} is out of range", primitiveIndex);
                return false;
            }
            mesh.indices.push_back(static_cast<unsigned int>(primitiveIndex));
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

    render::StaticMeshSection section{};
    section.name = material->name;
    section.mesh = std::move(mesh);
    section.material = std::move(material);
    outModel.sections.push_back(std::move(section));
    return true;
}

bool centerAndGroundModel(render::StaticModelData& model) {
    bool hasVertex = false;
    glm::vec3 minBounds(std::numeric_limits<float>::max());
    glm::vec3 maxBounds(std::numeric_limits<float>::lowest());

    for (const render::StaticMeshSection& section : model.sections) {
        for (const glm::vec3& position : section.mesh.positions) {
            minBounds = glm::min(minBounds, position);
            maxBounds = glm::max(maxBounds, position);
            hasVertex = true;
        }
    }

    if (!hasVertex) {
        spdlog::error("StaticGltfModel: generated model data is empty");
        return false;
    }

    const glm::vec3 center = (minBounds + maxBounds) * 0.5f;
    const glm::vec3 translation(-center.x, -minBounds.y, -center.z);

    for (render::StaticMeshSection& section : model.sections) {
        for (glm::vec3& position : section.mesh.positions) {
            position += translation;
        }
    }

    spdlog::info(
        "StaticGltfModel: grounded model bounds min ({:.3f}, {:.3f}, {:.3f}) max ({:.3f}, {:.3f}, {:.3f})",
        minBounds.x, minBounds.y, minBounds.z,
        maxBounds.x, maxBounds.y, maxBounds.z
    );

    return true;
}

}  // namespace

namespace render {

bool loadStaticGltfModel(const std::string& path, StaticModelData& outModel) {
    outModel.sections.clear();

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

    std::vector<const cgltf_node*> meshNodes;
    const cgltf_scene* scene = data->scene != nullptr
        ? data->scene
        : (data->scenes_count > 0 ? &data->scenes[0] : nullptr);

    if (scene != nullptr) {
        for (cgltf_size i = 0; i < scene->nodes_count; ++i) {
            gatherMeshNodes(scene->nodes[i], meshNodes);
        }
    } else {
        for (cgltf_size i = 0; i < data->nodes_count; ++i) {
            if (data->nodes[i].mesh != nullptr) {
                meshNodes.push_back(&data->nodes[i]);
            }
        }
    }

    if (meshNodes.empty()) {
        spdlog::error("StaticGltfModel: no mesh nodes found in '{}'", path);
        cleanup();
        return false;
    }

    std::unordered_map<const cgltf_material*, std::shared_ptr<Material>> materialCache;
    std::size_t materialIndex = 0;
    for (const cgltf_node* node : meshNodes) {
        cgltf_float nodeMatrixValues[16]{};
        cgltf_node_transform_world(node, nodeMatrixValues);
        const glm::mat4 nodeWorld = toGlmMat4(nodeMatrixValues);
        const glm::mat3 nodeNormalMatrix = glm::mat3(glm::transpose(glm::inverse(nodeWorld)));

        std::vector<glm::mat4> jointMatrices;
        std::vector<glm::mat3> jointNormalMatrices;
        const std::vector<glm::mat4>* jointMatrixPtr = nullptr;
        const std::vector<glm::mat3>* jointNormalMatrixPtr = nullptr;

        if (node->skin != nullptr) {
            if (!buildSkinMatrices(node->skin, jointMatrices, jointNormalMatrices)) {
                cleanup();
                return false;
            }
            jointMatrixPtr = &jointMatrices;
            jointNormalMatrixPtr = &jointNormalMatrices;
        }

        for (cgltf_size primitiveIndex = 0; primitiveIndex < node->mesh->primitives_count; ++primitiveIndex) {
            if (!appendPrimitive(
                    node->mesh->primitives[primitiveIndex],
                    nodeWorld,
                    nodeNormalMatrix,
                    jointMatrixPtr,
                    jointNormalMatrixPtr,
                    materialCache,
                    materialIndex++,
                    outModel
                )) {
                cleanup();
                return false;
            }
        }
    }

    const bool ready = centerAndGroundModel(outModel);
    cleanup();
    return ready;
}

bool loadStaticCharacterModel(const std::string& path, StaticModelData& outModel) {
    return loadStaticGltfModel(path, outModel);
}

}  // namespace render
