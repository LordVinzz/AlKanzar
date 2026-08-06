#pragma once

#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>

#include <glm/vec3.hpp>

#include "SceneBlueprint.hpp"

struct lua_State;

namespace core::scene_asset_detail {

enum class FieldStatus {
    Missing,
    Present,
    Error,
};

[[nodiscard]] bool fail(
    std::string* error,
    std::string_view path,
    std::string_view message
);

[[nodiscard]] bool validateStringFields(
    lua_State* state,
    int tableIndex,
    std::initializer_list<std::string_view> allowedFields,
    std::string* error,
    std::string_view path
);

[[nodiscard]] bool validateArray(lua_State* state, int tableIndex, std::string* error, std::string_view path);

[[nodiscard]] FieldStatus pushTableField(
    lua_State* state,
    int tableIndex,
    const char* field,
    bool required,
    std::string* error,
    std::string_view path
);

[[nodiscard]] bool readStringField(
    lua_State* state,
    int tableIndex,
    const char* field,
    std::string& outValue,
    bool required,
    std::string* error,
    std::string_view path
);

[[nodiscard]] bool readFloatField(
    lua_State* state,
    int tableIndex,
    const char* field,
    float& outValue,
    bool required,
    std::string* error,
    std::string_view path
);

[[nodiscard]] bool readIntegerField(
    lua_State* state,
    int tableIndex,
    const char* field,
    std::int64_t& outValue,
    bool required,
    std::string* error,
    std::string_view path
);

[[nodiscard]] bool readBoolField(
    lua_State* state,
    int tableIndex,
    const char* field,
    bool& outValue,
    bool required,
    std::string* error,
    std::string_view path
);

[[nodiscard]] bool readVec3Field(
    lua_State* state,
    int tableIndex,
    const char* field,
    glm::vec3& outValue,
    bool required,
    std::string* error,
    std::string_view path
);

[[nodiscard]] bool readTransform(
    lua_State* state,
    int objectIndex,
    TransformComponent& outTransform,
    std::string* error,
    std::string_view path
);

[[nodiscard]] bool parseCharacterTable(
    lua_State* state,
    int tableIndex,
    CharacterBlueprint& outCharacter,
    std::string* error,
    std::string_view path
);

[[nodiscard]] bool parseLightObject(
    lua_State* state,
    int objectIndex,
    int parametersIndex,
    std::string_view type,
    SceneBlueprint& blueprint,
    std::string* error,
    std::string_view path
);

[[nodiscard]] bool parseSceneTable(
    lua_State* state,
    int tableIndex,
    SceneBlueprint& outBlueprint,
    std::string* error
);

}  // namespace core::scene_asset_detail
