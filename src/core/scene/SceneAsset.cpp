#include "SceneAsset.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

#include "SceneAssetDetail.hpp"
#include "core/content/ContentFileHeader.hpp"

namespace core {
namespace {

constexpr std::size_t kMaximumSceneAssetBytes = 4u * 1024u * 1024u;
constexpr std::size_t kMaximumLuaMemoryBytes = 16u * 1024u * 1024u;
constexpr int kMaximumLuaInstructions = 500'000;

constexpr std::string_view kSceneDslBootstrap = R"lua(
local fail = __alkanzar_scene_error
__alkanzar_scene_error = nil

local created = {}
local active_scene = nil
local sealed = false

local function ensure_mutable()
    if sealed then
        fail("scene.build() must be the final scene command")
    end
end

function Create(parameters)
    ensure_mutable()
    if parameters == nil then
        fail("Create expects a parameter table")
    end

    local object = { parameters = parameters, added = false }
    created[#created + 1] = object

    function object.transform(value)
        ensure_mutable()
        object.transform_data = value
        return object
    end

    function object.character(value)
        ensure_mutable()
        object.character_data = value
        return object
    end

    function object.fit_to_footprint(value)
        ensure_mutable()
        object.fit_footprint = value
        return object
    end

    if parameters.type == "Scene" then
        if active_scene ~= nil then
            fail("a SCN V1 asset must create exactly one Scene")
        end
        active_scene = object
        object.objects = {}

        function object.add(child)
            ensure_mutable()
            if child == nil or child == object then
                fail("scene.add expects an object returned by Create")
            end
            if child.added then
                fail("an object cannot be added to the scene twice")
            end
            child.added = true
            object.objects[#object.objects + 1] = child
            return object
        end

        function object.build()
            ensure_mutable()
            for index = 1, #created do
                local candidate = created[index]
                if candidate ~= object and not candidate.added then
                    fail("every object returned by Create must be passed to scene.add")
                end
            end
            sealed = true
            object.built = true
            __ALKANZAR_SCENE_RESULT = object
            return object
        end
    end

    return object
end
)lua";

struct LuaMemoryBudget {
    std::size_t usedBytes{0u};
};

void* allocateLuaMemory(void* userData, void* pointer, std::size_t oldSize, std::size_t newSize) {
    auto& budget = *static_cast<LuaMemoryBudget*>(userData);
    const std::size_t accountedOldSize = pointer != nullptr ? oldSize : 0u;
    if (newSize == 0u) {
        std::free(pointer);
        budget.usedBytes -= std::min(budget.usedBytes, accountedOldSize);
        return nullptr;
    }
    if (newSize > accountedOldSize &&
        newSize - accountedOldSize > kMaximumLuaMemoryBytes -
            std::min(kMaximumLuaMemoryBytes, budget.usedBytes)) {
        return nullptr;
    }
    void* resized = std::realloc(pointer, newSize);
    if (resized != nullptr) {
        budget.usedBytes -= std::min(budget.usedBytes, accountedOldSize);
        budget.usedBytes += newSize;
    }
    return resized;
}

struct LuaStateCloser {
    void operator()(lua_State* state) const {
        if (state != nullptr) {
            lua_close(state);
        }
    }
};

int raiseSceneDslError(lua_State* state) {
    const char* message = lua_tostring(state, 1);
    return luaL_error(state, "%s", message != nullptr ? message : "invalid scene command");
}

void stopLongRunningScene(lua_State* state, lua_Debug*) {
    luaL_error(state, "scene script exceeded its instruction budget");
}

bool captureLuaError(lua_State* state, std::string_view stage, std::string* error) {
    const char* message = lua_tostring(state, -1);
    if (error != nullptr) {
        *error = std::string(stage) + ": " +
            (message != nullptr ? message : "unknown Lua error");
    }
    lua_pop(state, 1);
    return false;
}

bool runLuaChunk(
    lua_State* state,
    std::string_view source,
    const std::string& chunkName,
    std::string_view stage,
    bool enforceInstructionBudget,
    std::string* error
) {
    if (luaL_loadbufferx(
            state,
            source.data(),
            source.size(),
            chunkName.c_str(),
            "t") != LUA_OK) {
        return captureLuaError(state, stage, error);
    }
    if (enforceInstructionBudget) {
        lua_sethook(state, stopLongRunningScene, LUA_MASKCOUNT, kMaximumLuaInstructions);
    }
    const int status = lua_pcall(state, 0, 0, 0);
    lua_sethook(state, nullptr, 0, 0);
    if (status != LUA_OK) {
        return captureLuaError(state, stage, error);
    }
    return true;
}

bool validateSceneHeader(std::string_view bytes, std::string* error) {
    ContentFileHeader header{};
    if (!decodeContentFileHeader(bytes, header, error)) {
        return false;
    }
    if (header.type != kSceneContentType) {
        if (error != nullptr) {
            *error = "Expected content type SCN, got " + header.type + ".";
        }
        return false;
    }
    if (header.version != kSceneAssetVersion) {
        if (error != nullptr) {
            *error = "Unsupported scene version " + std::to_string(header.version) + ".";
        }
        return false;
    }

    std::array<char, kContentFileHeaderSize> expected{};
    std::string headerError{};
    if (!encodeTextContentFileHeader(header.version, header.type, expected, &headerError) ||
        bytes.substr(0u, expected.size()) != std::string_view(expected.data(), expected.size())) {
        if (error != nullptr) {
            *error = "SCN assets must use the visible 10-byte header V1SCN-----.";
        }
        return false;
    }
    return true;
}

}  // namespace

bool parseSceneAsset(
    std::string_view bytes,
    SceneBlueprint& outBlueprint,
    std::string* error,
    std::string_view chunkName
) {
    if (error != nullptr) {
        error->clear();
    }
    if (bytes.size() > kMaximumSceneAssetBytes) {
        return scene_asset_detail::fail(error, chunkName, "exceeds the 4 MiB SCN V1 limit");
    }
    if (!validateSceneHeader(bytes, error)) {
        return false;
    }
    const std::string_view payload = bytes.substr(kContentFileHeaderSize);
    if (payload.empty() || (payload.front() != '\n' && payload.front() != '\r')) {
        return scene_asset_detail::fail(
            error,
            chunkName,
            "must place the Lua payload on the line after the 10-byte header"
        );
    }

    LuaMemoryBudget memoryBudget{};
    std::unique_ptr<lua_State, LuaStateCloser> state(
        lua_newstate(allocateLuaMemory, &memoryBudget, 0u)
    );
    if (state == nullptr) {
        return scene_asset_detail::fail(error, chunkName, "could not allocate the Lua scene state");
    }

    lua_pushcfunction(state.get(), raiseSceneDslError);
    lua_setglobal(state.get(), "__alkanzar_scene_error");
    if (!runLuaChunk(
            state.get(),
            kSceneDslBootstrap,
            "=AlKanzar SCN V1 DSL",
            "Failed to initialize the SCN V1 DSL",
            false,
            error)) {
        return false;
    }

    const std::string luaChunkName = "@" + std::string(chunkName);
    if (!runLuaChunk(
            state.get(),
            payload,
            luaChunkName,
            "Failed to execute scene asset",
            true,
            error)) {
        return false;
    }

    lua_getglobal(state.get(), "__ALKANZAR_SCENE_RESULT");
    if (!lua_istable(state.get(), -1)) {
        lua_pop(state.get(), 1);
        return scene_asset_detail::fail(
            error,
            chunkName,
            "did not call scene.build()"
        );
    }
    SceneBlueprint blueprint{};
    const bool valid = scene_asset_detail::parseSceneTable(
        state.get(),
        -1,
        blueprint,
        error
    );
    lua_pop(state.get(), 1);
    if (!valid) {
        return false;
    }

    outBlueprint = std::move(blueprint);
    if (error != nullptr) {
        error->clear();
    }
    return true;
}

bool loadSceneAsset(
    const std::filesystem::path& path,
    SceneBlueprint& outBlueprint,
    std::string* error
) {
    std::error_code sizeError{};
    const std::uintmax_t fileSize = std::filesystem::file_size(path, sizeError);
    if (!sizeError && fileSize > kMaximumSceneAssetBytes) {
        return scene_asset_detail::fail(error, path.string(), "exceeds the 4 MiB SCN V1 limit");
    }
    std::ifstream input(path, std::ios::binary);
    if (!input.is_open()) {
        return scene_asset_detail::fail(error, path.string(), "could not open the scene asset");
    }
    const std::string bytes{
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>()
    };
    if (!input.good() && !input.eof()) {
        return scene_asset_detail::fail(error, path.string(), "could not read the scene asset");
    }
    return parseSceneAsset(bytes, outBlueprint, error, path.string());
}

}  // namespace core
