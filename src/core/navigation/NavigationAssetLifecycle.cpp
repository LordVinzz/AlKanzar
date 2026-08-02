#include "core/navigation/Navigation.hpp"
#include "core/navigation/NavigationDetailBake.hpp"
#include "core/navigation/NavigationDetailGeometry.hpp"
#include "core/navigation/NavigationDetailRuntime.hpp"
#include "core/navigation/NavigationDetailTypes.hpp"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string_view>
#include <unordered_map>

#include <SDL.h>

#include "render/resources/StaticGltfModel.hpp"

namespace core {
namespace navigation_detail {

int findClipContaining(const render::GltfModelData& model, std::string_view token) {
    const std::string loweredToken = lowercaseCopy(std::string(token));
    for (std::size_t clipIndex = 0; clipIndex < model.animations.size(); ++clipIndex) {
        std::string lowered = lowercaseCopy(model.animations[clipIndex].name);
        if (lowered.find(loweredToken) != std::string::npos) {
            return static_cast<int>(clipIndex);
        }
    }
    return -1;
}

std::string resolveNavAssetPath(const std::string& assetPath) {
    if (assetPath.empty()) {
        return {};
    }
    const std::filesystem::path path(assetPath);
    if (path.is_absolute()) {
        return path.string();
    }

    char* basePath = SDL_GetBasePath();
    std::string resolved = basePath ? (std::filesystem::path(basePath) / path).string() : path.string();
    if (basePath != nullptr) {
        SDL_free(basePath);
    }
    return resolved;
}

}  // namespace navigation_detail

using namespace navigation_detail;

bool NavigationSystem::initializeScene(const SceneBlueprint& blueprint, World& world, NavigationRuntime& runtime) const {
    runtime.assetPath = resolveNavAssetPath(blueprint.navMeshAssetPath);
    runtime.editor = NavigationEditorState{};
    const bool loaded = loadAsset(runtime);
    const ParentPathData paths = buildStableIdPaths(world);

    const std::unordered_map<std::string, NavSourceTag> overrideById = [&runtime]() {
        std::unordered_map<std::string, NavSourceTag> overrides{};
        overrides.reserve(runtime.asset.sourceTagOverrides.size());
        for (const NavSourceTagOverride& overrideRecord : runtime.asset.sourceTagOverrides) {
            overrides[overrideRecord.stableId] = overrideRecord.tag;
        }
        return overrides;
    }();

    for (EntityId entity : world.renderables.entities()) {
        const RenderableComponent& renderable = world.renderables.get(entity);
        const NavSourceTag defaultTag = defaultTagForLayer(renderable.layer);
        const auto pathIt = paths.pathByEntity.find(entity);
        const std::string stableId = pathIt != paths.pathByEntity.end()
            ? pathIt->second
            : ("Renderable/" + std::to_string(entity.index));
        const auto overrideIt = overrideById.find(stableId);
        const NavSourceTag effectiveTag = overrideIt != overrideById.end() ? overrideIt->second : defaultTag;
        world.navSources.emplace(entity, NavSourceComponent{stableId, defaultTag, effectiveTag});
    }

    for (EntityId entity : world.animatedModels.entities()) {
        const NameComponent* name = world.names.tryGet(entity);
        if (name == nullptr || name->value != "Character") {
            continue;
        }
        if (!world.navAgents.contains(entity)) {
            world.navAgents.emplace(entity, NavAgentComponent{});
        }
        const AnimatedModelComponent& animated = world.animatedModels.get(entity);
        if (animated.model) {
            const int idleClip = render::findDefaultAnimationClipIndex(*animated.model);
            int walkClip = findClipContaining(*animated.model, "walk");
            if (walkClip < 0) {
                walkClip = findClipContaining(*animated.model, "run");
            }
            world.locomotion.emplace(entity, LocomotionComponent{idleClip, walkClip});
        }
    }

    const bool rebuilt = rebuildRuntime(runtime);
    return loaded && rebuilt;
}

bool NavigationSystem::loadAsset(NavigationRuntime& runtime) const {
    invalidatePendingPathRequests();
    runtime.solveSnapshot.reset();
    runtime.asset = NavMeshAsset{};
    if (runtime.assetPath.empty()) {
        setRuntimeStatus(runtime, "No navmesh asset configured.", true);
        return false;
    }

    std::ifstream input(runtime.assetPath, std::ios::binary);
    if (!input.is_open()) {
        runtime.asset.version = kNavAssetVersion;
        setRuntimeStatus(runtime, "Navmesh asset missing; using an empty in-memory asset.", false);
        return true;
    }

    std::ostringstream buffer;
    buffer << input.rdbuf();
    std::string error{};
    if (!parseNavMeshAsset(buffer.str(), runtime.asset, &error)) {
        runtime.asset = NavMeshAsset{};
        setRuntimeStatus(runtime, error, true);
        return false;
    }
    setRuntimeStatus(runtime, "Loaded navmesh asset.", false);
    return true;
}

bool NavigationSystem::reloadAsset(NavigationRuntime& runtime) const {
    if (!loadAsset(runtime)) {
        return false;
    }
    return rebuildRuntime(runtime);
}

bool NavigationSystem::saveAsset(const NavigationRuntime& runtime, std::string* error) const {
    if (runtime.assetPath.empty()) {
        if (error) {
            *error = "No navmesh asset path configured.";
        }
        return false;
    }

    std::error_code createError{};
    const std::filesystem::path path(runtime.assetPath);
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path(), createError);
        if (createError) {
            if (error) {
                *error = createError.message();
            }
            return false;
        }
    }

    std::ofstream output(runtime.assetPath, std::ios::binary | std::ios::trunc);
    if (!output.is_open()) {
        if (error) {
            *error = "Failed to open navmesh asset for writing.";
        }
        return false;
    }
    output << serializeNavMeshAsset(runtime.asset);
    return output.good();
}

}  // namespace core
