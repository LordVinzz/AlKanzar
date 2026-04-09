#include "StaticGltfModel.hpp"

#include <array>
#include <cstddef>
#include <limits>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_inverse.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <spdlog/spdlog.h>

#define CGLTF_IMPLEMENTATION
#include "vendor/cgltf.h"

namespace {

constexpr std::size_t kVertexStride = 9;
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

glm::vec3 materialColor(const cgltf_material* material) {
    if (material == nullptr || !material->has_pbr_metallic_roughness) {
        return glm::vec3(1.0f);
    }

    return glm::vec3(
        material->pbr_metallic_roughness.base_color_factor[0],
        material->pbr_metallic_roughness.base_color_factor[1],
        material->pbr_metallic_roughness.base_color_factor[2]
    );
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
    render::StaticMeshData& outMesh
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

    const glm::vec3 color = materialColor(primitive.material);
    const cgltf_size vertexCount = positionAccessor->count;
    const unsigned int baseVertex = static_cast<unsigned int>(outMesh.vertices.size() / kVertexStride);

    outMesh.vertices.reserve(outMesh.vertices.size() + static_cast<std::size_t>(vertexCount) * kVertexStride);

    for (cgltf_size vertexIndex = 0; vertexIndex < vertexCount; ++vertexIndex) {
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

        outMesh.vertices.insert(
            outMesh.vertices.end(),
            {
                worldPosition.x, worldPosition.y, worldPosition.z,
                worldNormal.x, worldNormal.y, worldNormal.z,
                color.r, color.g, color.b
            }
        );
    }

    if (primitive.indices != nullptr) {
        outMesh.indices.reserve(outMesh.indices.size() + static_cast<std::size_t>(primitive.indices->count));
        for (cgltf_size index = 0; index < primitive.indices->count; ++index) {
            const cgltf_size primitiveIndex = cgltf_accessor_read_index(primitive.indices, index);
            if (primitiveIndex >= vertexCount) {
                spdlog::error("StaticGltfModel: index {} is out of range", primitiveIndex);
                return false;
            }
            outMesh.indices.push_back(baseVertex + static_cast<unsigned int>(primitiveIndex));
        }
    } else {
        if ((vertexCount % 3u) != 0u) {
            spdlog::error("StaticGltfModel: non-indexed triangle primitive has invalid vertex count");
            return false;
        }

        outMesh.indices.reserve(outMesh.indices.size() + static_cast<std::size_t>(vertexCount));
        for (cgltf_size index = 0; index < vertexCount; ++index) {
            outMesh.indices.push_back(baseVertex + static_cast<unsigned int>(index));
        }
    }

    return true;
}

bool centerAndGroundMesh(render::StaticMeshData& mesh) {
    if (mesh.vertices.empty() || mesh.indices.empty()) {
        spdlog::error("StaticGltfModel: generated mesh data is empty");
        return false;
    }

    glm::vec3 minBounds(std::numeric_limits<float>::max());
    glm::vec3 maxBounds(std::numeric_limits<float>::lowest());

    for (std::size_t offset = 0; offset < mesh.vertices.size(); offset += kVertexStride) {
        const glm::vec3 position(
            mesh.vertices[offset + 0],
            mesh.vertices[offset + 1],
            mesh.vertices[offset + 2]
        );
        minBounds = glm::min(minBounds, position);
        maxBounds = glm::max(maxBounds, position);
    }

    const glm::vec3 center = (minBounds + maxBounds) * 0.5f;
    const glm::vec3 translation(-center.x, -minBounds.y, -center.z);

    for (std::size_t offset = 0; offset < mesh.vertices.size(); offset += kVertexStride) {
        mesh.vertices[offset + 0] += translation.x;
        mesh.vertices[offset + 1] += translation.y;
        mesh.vertices[offset + 2] += translation.z;
    }

    spdlog::info(
        "StaticGltfModel: grounded character bounds min ({:.3f}, {:.3f}, {:.3f}) max ({:.3f}, {:.3f}, {:.3f})",
        minBounds.x, minBounds.y, minBounds.z,
        maxBounds.x, maxBounds.y, maxBounds.z
    );

    return true;
}

}  // namespace

namespace render {

bool loadStaticCharacterModel(const std::string& path, StaticMeshData& outMesh) {
    outMesh.vertices.clear();
    outMesh.indices.clear();

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
                    outMesh
                )) {
                cleanup();
                return false;
            }
        }
    }

    const bool ready = centerAndGroundMesh(outMesh);
    cleanup();
    return ready;
}

}  // namespace render
