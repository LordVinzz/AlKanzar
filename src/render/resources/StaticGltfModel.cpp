#include "StaticGltfModel.hpp"
#include "GltfImportDetail.hpp"

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
#include "cgltf/cgltf.h"

namespace {

using namespace render::gltf_detail;


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
    render::buildJointInfluenceBounds(section);
    section.material = std::move(material);
    outModel.sections.push_back(std::move(section));
    return true;
}

}  // namespace

namespace render {

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
