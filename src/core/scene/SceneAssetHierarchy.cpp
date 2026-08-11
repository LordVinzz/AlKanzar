#include "SceneAssetDetail.hpp"

#include <algorithm>
#include <string>
#include <string_view>
#include <utility>

extern "C" {
#include <lua.h>
}

namespace core::scene_asset_detail {
namespace {

bool validSceneObjectId(std::string_view id) {
    if (id.empty() || id.size() > 128u) {
        return false;
    }
    const auto validFirst = [](unsigned char value) {
        return (value >= 'A' && value <= 'Z') ||
            (value >= 'a' && value <= 'z') ||
            value == '_';
    };
    const auto validRest = [](unsigned char value) {
        return (value >= 'A' && value <= 'Z') ||
            (value >= 'a' && value <= 'z') ||
            (value >= '0' && value <= '9') ||
            value == '_' || value == '-' || value == '.';
    };
    if (!validFirst(static_cast<unsigned char>(id.front()))) {
        return false;
    }
    for (char value : id.substr(1u)) {
        if (!validRest(static_cast<unsigned char>(value))) {
            return false;
        }
    }
    return true;
}

std::string legacyObjectId(std::string_view type, const SceneBlueprint& blueprint) {
    std::string prefix{};
    std::size_t ordinal = 1u;
    if (type == "Primitive") {
        prefix = "primitive";
        ordinal += blueprint.primitives.size();
    } else if (type == "Model") {
        prefix = "model";
        ordinal += blueprint.models.size();
    } else if (type == "DirectionalLight") {
        prefix = "directional_light";
    } else if (type == "LightVolume") {
        prefix = "light_volume";
        ordinal += blueprint.lightVolumes.size();
    } else if (type == "PointLight") {
        prefix = "point_light";
        ordinal += blueprint.pointLights.size();
    } else {
        prefix = "spot_light";
        ordinal += blueprint.spotLights.size();
    }
    const std::string digits = std::to_string(ordinal);
    return prefix + "_" + std::string(3u - std::min<std::size_t>(3u, digits.size()), '0') + digits;
}

}  // namespace

bool readSceneObjectIdentity(
    lua_State* state,
    int objectIndex,
    int parametersIndex,
    std::string_view type,
    std::uint32_t version,
    SceneBlueprint& blueprint,
    SceneObjectId& outId,
    std::optional<SceneObjectId>& outParentId,
    std::string* error,
    std::string_view path
) {
    SceneObjectId id{};
    if (!readStringField(
            state,
            parametersIndex,
            "id",
            id,
            version >= 2u,
            error,
            path)) {
        return false;
    }
    if (version < 2u) {
        if (!id.empty()) {
            return fail(error, std::string(path) + ".id", "requires SCN V2");
        }
        id = legacyObjectId(type, blueprint);
    }
    if (!validSceneObjectId(id)) {
        return fail(
            error,
            std::string(path) + ".id",
            "must start with a letter or underscore and contain only letters, digits, '_', '-' or '.'"
        );
    }
    if (std::find(blueprint.objectOrder.begin(), blueprint.objectOrder.end(), id) !=
        blueprint.objectOrder.end()) {
        return fail(error, std::string(path) + ".id", "duplicates scene object id '" + id + "'");
    }

    std::optional<SceneObjectId> parentId{};
    const FieldStatus parentStatus = pushTableField(
        state,
        objectIndex,
        "parent_data",
        false,
        error,
        path
    );
    if (parentStatus == FieldStatus::Error) {
        return false;
    }
    if (parentStatus == FieldStatus::Present) {
        if (version < 2u) {
            lua_pop(state, 1);
            return fail(error, std::string(path) + ".parent", "requires SCN V2");
        }
        const int parentObjectIndex = lua_gettop(state);
        const FieldStatus parametersStatus = pushTableField(
            state,
            parentObjectIndex,
            "parameters",
            true,
            error,
            std::string(path) + ".parent"
        );
        if (parametersStatus != FieldStatus::Present) {
            lua_pop(state, 1);
            return false;
        }
        SceneObjectId parsedParent{};
        const bool valid = readStringField(
            state,
            -1,
            "id",
            parsedParent,
            true,
            error,
            std::string(path) + ".parent"
        );
        lua_pop(state, 2);
        if (!valid) {
            return false;
        }
        parentId = std::move(parsedParent);
    }

    outId = std::move(id);
    outParentId = std::move(parentId);
    blueprint.objectOrder.push_back(outId);
    return true;
}

}  // namespace core::scene_asset_detail
