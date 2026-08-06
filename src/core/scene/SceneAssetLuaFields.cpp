#include "SceneAssetDetail.hpp"

#include <algorithm>
#include <cmath>
#include <string>

extern "C" {
#include <lua.h>
}

namespace core::scene_asset_detail {
namespace {

std::string childPath(std::string_view path, std::string_view field) {
    std::string result(path);
    if (!result.empty()) {
        result += ".";
    }
    result += field;
    return result;
}

bool missingField(
    std::string* error,
    std::string_view path,
    const char* field
) {
    return fail(error, childPath(path, field), "is required");
}

bool wrongFieldType(
    std::string* error,
    std::string_view path,
    const char* field,
    std::string_view expected
) {
    return fail(error, childPath(path, field), std::string("must be ") + std::string(expected));
}

}  // namespace

bool fail(std::string* error, std::string_view path, std::string_view message) {
    if (error != nullptr) {
        error->assign(path);
        if (!path.empty() && !message.empty()) {
            *error += ": ";
        }
        error->append(message);
    }
    return false;
}

bool validateStringFields(
    lua_State* state,
    int tableIndex,
    std::initializer_list<std::string_view> allowedFields,
    std::string* error,
    std::string_view path
) {
    const int absoluteIndex = lua_absindex(state, tableIndex);
    lua_pushnil(state);
    while (lua_next(state, absoluteIndex) != 0) {
        if (lua_type(state, -2) != LUA_TSTRING) {
            lua_pop(state, 2);
            return fail(error, path, "contains a non-string field name");
        }
        std::size_t keySize = 0u;
        const char* keyData = lua_tolstring(state, -2, &keySize);
        const std::string_view key(keyData, keySize);
        const bool allowed = std::find(allowedFields.begin(), allowedFields.end(), key) !=
            allowedFields.end();
        if (!allowed) {
            lua_pop(state, 2);
            return fail(error, childPath(path, key), "is not supported by SCN V1");
        }
        lua_pop(state, 1);
    }
    return true;
}

bool validateArray(lua_State* state, int tableIndex, std::string* error, std::string_view path) {
    const int absoluteIndex = lua_absindex(state, tableIndex);
    const lua_Integer length = static_cast<lua_Integer>(lua_rawlen(state, absoluteIndex));
    lua_Integer fieldCount = 0;
    lua_pushnil(state);
    while (lua_next(state, absoluteIndex) != 0) {
        const bool validIndex = lua_isinteger(state, -2) &&
            lua_tointeger(state, -2) >= 1 &&
            lua_tointeger(state, -2) <= length;
        if (!validIndex) {
            lua_pop(state, 2);
            return fail(error, path, "must be a contiguous array starting at index 1");
        }
        ++fieldCount;
        lua_pop(state, 1);
    }
    if (fieldCount != length) {
        return fail(error, path, "must not contain holes or duplicate numeric keys");
    }
    for (lua_Integer index = 1; index <= length; ++index) {
        lua_rawgeti(state, absoluteIndex, index);
        const bool present = !lua_isnil(state, -1);
        lua_pop(state, 1);
        if (!present) {
            return fail(error, path, "must be a contiguous array starting at index 1");
        }
    }
    return true;
}

FieldStatus pushTableField(
    lua_State* state,
    int tableIndex,
    const char* field,
    bool required,
    std::string* error,
    std::string_view path
) {
    lua_getfield(state, lua_absindex(state, tableIndex), field);
    if (lua_isnil(state, -1)) {
        lua_pop(state, 1);
        if (required) {
            missingField(error, path, field);
            return FieldStatus::Error;
        }
        return FieldStatus::Missing;
    }
    if (!lua_istable(state, -1)) {
        lua_pop(state, 1);
        wrongFieldType(error, path, field, "a table");
        return FieldStatus::Error;
    }
    return FieldStatus::Present;
}

bool readStringField(
    lua_State* state,
    int tableIndex,
    const char* field,
    std::string& outValue,
    bool required,
    std::string* error,
    std::string_view path
) {
    lua_getfield(state, lua_absindex(state, tableIndex), field);
    if (lua_isnil(state, -1)) {
        lua_pop(state, 1);
        return !required || missingField(error, path, field);
    }
    if (lua_type(state, -1) != LUA_TSTRING) {
        lua_pop(state, 1);
        return wrongFieldType(error, path, field, "a string");
    }
    std::size_t size = 0u;
    const char* data = lua_tolstring(state, -1, &size);
    outValue.assign(data, size);
    lua_pop(state, 1);
    return true;
}

bool readFloatField(
    lua_State* state,
    int tableIndex,
    const char* field,
    float& outValue,
    bool required,
    std::string* error,
    std::string_view path
) {
    lua_getfield(state, lua_absindex(state, tableIndex), field);
    if (lua_isnil(state, -1)) {
        lua_pop(state, 1);
        return !required || missingField(error, path, field);
    }
    if (lua_type(state, -1) != LUA_TNUMBER) {
        lua_pop(state, 1);
        return wrongFieldType(error, path, field, "a finite number");
    }
    const double value = static_cast<double>(lua_tonumber(state, -1));
    lua_pop(state, 1);
    const float converted = static_cast<float>(value);
    if (!std::isfinite(value) || !std::isfinite(converted)) {
        return wrongFieldType(error, path, field, "a finite number");
    }
    outValue = converted;
    return true;
}

bool readIntegerField(
    lua_State* state,
    int tableIndex,
    const char* field,
    std::int64_t& outValue,
    bool required,
    std::string* error,
    std::string_view path
) {
    lua_getfield(state, lua_absindex(state, tableIndex), field);
    if (lua_isnil(state, -1)) {
        lua_pop(state, 1);
        return !required || missingField(error, path, field);
    }
    if (!lua_isinteger(state, -1)) {
        lua_pop(state, 1);
        return wrongFieldType(error, path, field, "an integer");
    }
    outValue = static_cast<std::int64_t>(lua_tointeger(state, -1));
    lua_pop(state, 1);
    return true;
}

bool readBoolField(
    lua_State* state,
    int tableIndex,
    const char* field,
    bool& outValue,
    bool required,
    std::string* error,
    std::string_view path
) {
    lua_getfield(state, lua_absindex(state, tableIndex), field);
    if (lua_isnil(state, -1)) {
        lua_pop(state, 1);
        return !required || missingField(error, path, field);
    }
    if (lua_type(state, -1) != LUA_TBOOLEAN) {
        lua_pop(state, 1);
        return wrongFieldType(error, path, field, "a boolean");
    }
    outValue = lua_toboolean(state, -1) != 0;
    lua_pop(state, 1);
    return true;
}

bool readVec3Field(
    lua_State* state,
    int tableIndex,
    const char* field,
    glm::vec3& outValue,
    bool required,
    std::string* error,
    std::string_view path
) {
    const FieldStatus status = pushTableField(
        state,
        tableIndex,
        field,
        required,
        error,
        path
    );
    if (status == FieldStatus::Missing) {
        return true;
    }
    if (status == FieldStatus::Error) {
        return false;
    }

    const std::string vectorPath = childPath(path, field);
    const int vectorIndex = lua_gettop(state);
    glm::vec3 value = outValue;
    const bool valid = validateStringFields(
        state,
        vectorIndex,
        {"x", "y", "z"},
        error,
        vectorPath
    ) &&
        readFloatField(state, vectorIndex, "x", value.x, true, error, vectorPath) &&
        readFloatField(state, vectorIndex, "y", value.y, true, error, vectorPath) &&
        readFloatField(state, vectorIndex, "z", value.z, true, error, vectorPath);
    lua_pop(state, 1);
    if (valid) {
        outValue = value;
    }
    return valid;
}

bool readTransform(
    lua_State* state,
    int objectIndex,
    TransformComponent& outTransform,
    std::string* error,
    std::string_view path
) {
    const FieldStatus status = pushTableField(
        state,
        objectIndex,
        "transform_data",
        false,
        error,
        path
    );
    if (status == FieldStatus::Missing) {
        return true;
    }
    if (status == FieldStatus::Error) {
        return false;
    }

    const int transformIndex = lua_gettop(state);
    const std::string transformPath = childPath(path, "transform");
    TransformComponent transform = outTransform;
    const bool valid = validateStringFields(
        state,
        transformIndex,
        {"position", "rotation", "scale"},
        error,
        transformPath
    ) &&
        readVec3Field(state, transformIndex, "position", transform.position, false, error, transformPath) &&
        readVec3Field(state, transformIndex, "rotation", transform.rotationDeg, false, error, transformPath) &&
        readVec3Field(state, transformIndex, "scale", transform.scale, false, error, transformPath);
    lua_pop(state, 1);
    if (valid) {
        outTransform = transform;
    }
    return valid;
}

}  // namespace core::scene_asset_detail
