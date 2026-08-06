#include "SceneAssetDetail.hpp"

#include <string>
#include <string_view>
#include <utility>

namespace core::scene_asset_detail {
namespace {

bool parseLightVolume(
    lua_State* state,
    int objectIndex,
    int parametersIndex,
    SceneBlueprint& blueprint,
    std::string* error,
    std::string_view path
) {
    if (!validateStringFields(
            state,
            parametersIndex,
            {"type", "name", "half_extents"},
            error,
            path)) {
        return false;
    }
    LightVolumeBlueprint volume{};
    if (!readStringField(state, parametersIndex, "name", volume.name, true, error, path) ||
        !readVec3Field(state, parametersIndex, "half_extents", volume.halfExtents, false, error, path) ||
        !readTransform(state, objectIndex, volume.transform, error, path)) {
        return false;
    }
    if (volume.name.empty() ||
        volume.halfExtents.x <= 0.0f ||
        volume.halfExtents.y <= 0.0f ||
        volume.halfExtents.z <= 0.0f) {
        return fail(error, path, "name must not be empty and half_extents must be positive");
    }
    blueprint.lightVolumes.push_back(std::move(volume));
    return true;
}

bool parsePointLight(
    lua_State* state,
    int objectIndex,
    int parametersIndex,
    SceneBlueprint& blueprint,
    std::string* error,
    std::string_view path
) {
    if (!validateStringFields(
            state,
            parametersIndex,
            {
                "type", "name", "radius", "color", "intensity", "phase",
                "movable", "casts_shadow", "shadow_bias_min", "shadow_bias_slope"
            },
            error,
            path)) {
        return false;
    }
    PointLightBlueprint light{};
    if (!readStringField(state, parametersIndex, "name", light.name, true, error, path) ||
        !readFloatField(state, parametersIndex, "radius", light.radius, false, error, path) ||
        !readVec3Field(state, parametersIndex, "color", light.color, false, error, path) ||
        !readFloatField(state, parametersIndex, "intensity", light.intensity, false, error, path) ||
        !readFloatField(state, parametersIndex, "phase", light.phase, false, error, path) ||
        !readBoolField(state, parametersIndex, "movable", light.isMovable, false, error, path) ||
        !readBoolField(state, parametersIndex, "casts_shadow", light.castsShadow, false, error, path) ||
        !readFloatField(state, parametersIndex, "shadow_bias_min", light.shadowBiasMin, false, error, path) ||
        !readFloatField(state, parametersIndex, "shadow_bias_slope", light.shadowBiasSlope, false, error, path) ||
        !readTransform(state, objectIndex, light.transform, error, path)) {
        return false;
    }
    if (light.name.empty() || light.radius <= 0.0f || light.intensity < 0.0f ||
        light.shadowBiasMin < 0.0f || light.shadowBiasSlope < 0.0f) {
        return fail(error, path, "light name/radius/intensity/shadow bias values are invalid");
    }
    blueprint.pointLights.push_back(std::move(light));
    return true;
}

bool parseSpotLight(
    lua_State* state,
    int objectIndex,
    int parametersIndex,
    SceneBlueprint& blueprint,
    std::string* error,
    std::string_view path
) {
    if (!validateStringFields(
            state,
            parametersIndex,
            {
                "type", "name", "radius", "color", "intensity", "target",
                "inner_angle", "outer_angle", "phase", "movable",
                "casts_shadow", "shadow_bias_min", "shadow_bias_slope"
            },
            error,
            path)) {
        return false;
    }
    SpotLightBlueprint light{};
    if (!readStringField(state, parametersIndex, "name", light.name, true, error, path) ||
        !readFloatField(state, parametersIndex, "radius", light.radius, false, error, path) ||
        !readVec3Field(state, parametersIndex, "color", light.color, false, error, path) ||
        !readFloatField(state, parametersIndex, "intensity", light.intensity, false, error, path) ||
        !readVec3Field(state, parametersIndex, "target", light.target, false, error, path) ||
        !readFloatField(state, parametersIndex, "inner_angle", light.innerAngle, false, error, path) ||
        !readFloatField(state, parametersIndex, "outer_angle", light.outerAngle, false, error, path) ||
        !readFloatField(state, parametersIndex, "phase", light.phase, false, error, path) ||
        !readBoolField(state, parametersIndex, "movable", light.isMovable, false, error, path) ||
        !readBoolField(state, parametersIndex, "casts_shadow", light.castsShadow, false, error, path) ||
        !readFloatField(state, parametersIndex, "shadow_bias_min", light.shadowBiasMin, false, error, path) ||
        !readFloatField(state, parametersIndex, "shadow_bias_slope", light.shadowBiasSlope, false, error, path) ||
        !readTransform(state, objectIndex, light.transform, error, path)) {
        return false;
    }
    if (light.name.empty() || light.radius <= 0.0f || light.intensity < 0.0f ||
        light.innerAngle < 0.0f || light.outerAngle < light.innerAngle || light.outerAngle >= 180.0f ||
        light.shadowBiasMin < 0.0f || light.shadowBiasSlope < 0.0f) {
        return fail(error, path, "spot-light radius, intensity, angles or shadow bias values are invalid");
    }
    blueprint.spotLights.push_back(std::move(light));
    return true;
}

}  // namespace

bool parseLightObject(
    lua_State* state,
    int objectIndex,
    int parametersIndex,
    std::string_view type,
    SceneBlueprint& blueprint,
    std::string* error,
    std::string_view path
) {
    if (type == "LightVolume") {
        return parseLightVolume(state, objectIndex, parametersIndex, blueprint, error, path);
    }
    if (type == "PointLight") {
        return parsePointLight(state, objectIndex, parametersIndex, blueprint, error, path);
    }
    if (type == "SpotLight") {
        return parseSpotLight(state, objectIndex, parametersIndex, blueprint, error, path);
    }
    return fail(error, std::string(path) + ".type", "contains an unknown object type");
}

}  // namespace core::scene_asset_detail
