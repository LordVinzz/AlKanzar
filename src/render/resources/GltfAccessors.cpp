#include "GltfImportDetail.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

namespace render::gltf_detail {

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

}  // namespace render::gltf_detail

