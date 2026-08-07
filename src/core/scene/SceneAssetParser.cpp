#include "SceneAssetDetail.hpp"

#include <array>
#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>

extern "C" {
#include <lua.h>
}

namespace core::scene_asset_detail {
namespace {

bool validAssetReference(const std::string& reference, bool allowEmpty) {
    if (reference.empty()) {
        return allowEmpty;
    }
    if (reference.find('\0') != std::string::npos) {
        return false;
    }
    const std::filesystem::path path(reference);
    if (path.is_absolute() || path.has_root_name() || path.has_root_directory()) {
        return false;
    }
    for (const std::filesystem::path& component : path) {
        if (component == "..") {
            return false;
        }
    }
    return true;
}

bool parseRenderLayer(
    std::string_view token,
    render::RenderLayer& outLayer,
    std::string* error,
    std::string_view path
) {
    if (token == "Ground") {
        outLayer = render::RenderLayer::Ground;
        return true;
    }
    if (token == "Geometry") {
        outLayer = render::RenderLayer::Geometry;
        return true;
    }
    if (token == "Actors") {
        outLayer = render::RenderLayer::Actors;
        return true;
    }
    return fail(error, std::string(path) + ".layer", "contains an unknown render layer");
}

bool parseSceneParameters(
    lua_State* state,
    int parametersIndex,
    SceneBlueprint& blueprint,
    std::string* error
) {
    constexpr std::string_view path = "scene";
    if (!validateStringFields(
            state,
            parametersIndex,
            {
                "type", "ground_half_extent", "wall_height", "wall_offset",
                "wall_length", "wall_thickness", "navmesh"
            },
            error,
            path)) {
        return false;
    }

    std::string type{};
    if (!readStringField(state, parametersIndex, "type", type, true, error, path) ||
        !readFloatField(state, parametersIndex, "ground_half_extent", blueprint.groundHalfExtent, false, error, path) ||
        !readFloatField(state, parametersIndex, "wall_height", blueprint.wallHeight, false, error, path) ||
        !readFloatField(state, parametersIndex, "wall_offset", blueprint.wallOffset, false, error, path) ||
        !readFloatField(state, parametersIndex, "wall_length", blueprint.wallLength, false, error, path) ||
        !readFloatField(state, parametersIndex, "wall_thickness", blueprint.wallThickness, false, error, path) ||
        !readStringField(state, parametersIndex, "navmesh", blueprint.navMeshAssetPath, false, error, path)) {
        return false;
    }
    if (type != "Scene") {
        return fail(error, "scene.type", "must be Scene");
    }
    if (blueprint.groundHalfExtent <= 0.0f || blueprint.wallHeight <= 0.0f ||
        blueprint.wallOffset < 0.0f || blueprint.wallLength <= 0.0f ||
        blueprint.wallThickness <= 0.0f) {
        return fail(error, path, "ground and wall dimensions are invalid");
    }
    if (!validAssetReference(blueprint.navMeshAssetPath, true)) {
        return fail(error, "scene.navmesh", "must be a portable relative asset path");
    }
    return true;
}

bool parseModelObject(
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
            {"type", "name", "asset", "layer"},
            error,
            path)) {
        return false;
    }

    ModelInstanceBlueprint model{};
    std::string layer = "Geometry";
    if (!readStringField(state, parametersIndex, "name", model.name, true, error, path) ||
        !readStringField(state, parametersIndex, "asset", model.path, true, error, path) ||
        !readStringField(state, parametersIndex, "layer", layer, false, error, path) ||
        !readTransform(state, objectIndex, model.transform, error, path) ||
        !readFloatField(state, objectIndex, "fit_footprint", model.footprint, false, error, path) ||
        !parseRenderLayer(layer, model.layer, error, path)) {
        return false;
    }
    if (model.name.empty() || !validAssetReference(model.path, false)) {
        return fail(error, path, "model name must not be empty and asset must be a portable relative path");
    }
    if (model.footprint < 0.0f) {
        return fail(error, std::string(path) + ".fit_footprint", "must be positive");
    }
    model.fitToFootprint = model.footprint > 0.0f;

    const FieldStatus characterStatus = pushTableField(
        state,
        objectIndex,
        "character_data",
        false,
        error,
        path
    );
    if (characterStatus == FieldStatus::Error) {
        return false;
    }
    if (characterStatus == FieldStatus::Present) {
        CharacterBlueprint character{};
        const std::string characterPath = std::string(path) + ".character";
        const bool valid = parseCharacterTable(state, -1, character, error, characterPath);
        lua_pop(state, 1);
        if (!valid) {
            return false;
        }
        model.character = std::move(character);
    }

    blueprint.models.push_back(std::move(model));
    return true;
}

bool parseObject(
    lua_State* state,
    int objectIndex,
    SceneBlueprint& blueprint,
    std::string* error,
    std::string_view path
) {
    const int absoluteObjectIndex = lua_absindex(state, objectIndex);
    const FieldStatus parametersStatus = pushTableField(
        state,
        absoluteObjectIndex,
        "parameters",
        true,
        error,
        path
    );
    if (parametersStatus != FieldStatus::Present) {
        return false;
    }
    const int parametersIndex = lua_gettop(state);
    std::string type{};
    if (!readStringField(state, parametersIndex, "type", type, true, error, path)) {
        lua_pop(state, 1);
        return false;
    }

    bool valid = false;
    if (type == "Model") {
        valid = parseModelObject(
            state,
            absoluteObjectIndex,
            parametersIndex,
            blueprint,
            error,
            path
        );
    } else {
        valid = parseLightObject(
            state,
            absoluteObjectIndex,
            parametersIndex,
            type,
            blueprint,
            error,
            path
        );
    }
    lua_pop(state, 1);
    return valid;
}

}  // namespace

bool parseSceneTable(
    lua_State* state,
    int tableIndex,
    SceneBlueprint& outBlueprint,
    std::string* error
) {
    const int sceneIndex = lua_absindex(state, tableIndex);
    bool built = false;
    if (!readBoolField(state, sceneIndex, "built", built, true, error, "scene") || !built) {
        if (error != nullptr && error->empty()) {
            *error = "scene.build(): must be called exactly once at the end of the asset";
        }
        return false;
    }

    const FieldStatus parametersStatus = pushTableField(
        state,
        sceneIndex,
        "parameters",
        true,
        error,
        "scene"
    );
    if (parametersStatus != FieldStatus::Present) {
        return false;
    }
    SceneBlueprint blueprint{};
    const bool parametersValid = parseSceneParameters(state, -1, blueprint, error);
    lua_pop(state, 1);
    if (!parametersValid) {
        return false;
    }

    const FieldStatus objectsStatus = pushTableField(
        state,
        sceneIndex,
        "objects",
        true,
        error,
        "scene"
    );
    if (objectsStatus != FieldStatus::Present) {
        return false;
    }
    const int objectsIndex = lua_gettop(state);
    if (!validateArray(state, objectsIndex, error, "scene.objects")) {
        lua_pop(state, 1);
        return false;
    }

    std::unordered_set<const void*> objectIdentities{};
    const std::size_t objectCount = lua_rawlen(state, objectsIndex);
    objectIdentities.reserve(objectCount);
    for (std::size_t index = 1u; index <= objectCount; ++index) {
        lua_rawgeti(state, objectsIndex, static_cast<lua_Integer>(index));
        if (!lua_istable(state, -1)) {
            lua_pop(state, 2);
            return fail(
                error,
                "scene.objects[" + std::to_string(index) + "]",
                "must be an object returned by Create"
            );
        }
        if (!objectIdentities.insert(lua_topointer(state, -1)).second) {
            lua_pop(state, 2);
            return fail(
                error,
                "scene.objects[" + std::to_string(index) + "]",
                "was added to the scene more than once"
            );
        }
        const std::string objectPath = "scene.objects[" + std::to_string(index) + "]";
        const bool valid = parseObject(state, -1, blueprint, error, objectPath);
        lua_pop(state, 1);
        if (!valid) {
            lua_pop(state, 1);
            return false;
        }
    }
    lua_pop(state, 1);

    std::array<bool, kMaximumPartySize> occupiedPartySlots{};
    for (const ModelInstanceBlueprint& model : blueprint.models) {
        if (!model.character.has_value() || !model.character->partyMember.has_value()) {
            continue;
        }
        const std::size_t slot = model.character->partyMember->slot;
        if (occupiedPartySlots[slot]) {
            return fail(
                error,
                "scene.objects",
                "contains more than one character in the same party_slot"
            );
        }
        occupiedPartySlots[slot] = true;
    }

    outBlueprint = std::move(blueprint);
    return true;
}

}  // namespace core::scene_asset_detail
