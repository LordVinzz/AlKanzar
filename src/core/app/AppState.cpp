#include "AppState.hpp"
#include "core/editor/EditorUi.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <imgui.h>
#include <glm/gtc/type_ptr.hpp>
#include <spdlog/spdlog.h>

#include "core/editor/Command.hpp"
#include "core/editor/EditorSessionImGuiSettings.hpp"
#include "EngineServices.hpp"
#include "render/resources/StaticGltfModel.hpp"

namespace core {


void BootstrapState::onEnter(EngineServices& services) {
    if (!services.renderer.init()) {
        services.requestedMode = AppMode::Shutdown;
        return;
    }
    registerEditorSessionImGuiSettings(services.editorSession);
    std::string sceneError{};
    services.currentScene = appModeCapabilities(services.startupMode).usesDeterministicScene
        ? services.sceneRegistry.deterministicTestScene(&sceneError)
        : services.sceneRegistry.defaultScene(&sceneError);
    if (!sceneError.empty()) {
        spdlog::error("Application: failed to load scene asset: {}", sceneError);
        services.requestedMode = AppMode::Shutdown;
        return;
    }
    services.sceneLoaded = services.sceneFactory.buildScene(
        services.currentScene,
        services.world,
        services.renderer
    );
    if (!services.sceneLoaded) {
        spdlog::error(
            "Application: failed to build scene for {} mode",
            appModeName(services.startupMode)
        );
        services.requestedMode = AppMode::Shutdown;
        return;
    }
    if (!services.navigationSystem.initializeScene(services.currentScene, services.world, services.navigation)) {
        spdlog::error("Application: navigation init failed: {}", services.navigation.statusMessage);
    }
    services.editorSelection.clear();
    services.partySelection.clear();
    for (const EntityId entity : services.world.characters.entities()) {
        if (services.world.characters.get(entity).affiliation == CharacterAffiliation::Player) {
            services.partySelection.setLeader(entity);
            break;
        }
    }
    services.requestedMode = services.startupMode;
}

void BootstrapState::onExit(EngineServices&) {}
void BootstrapState::update(EngineServices&) {}
void BootstrapState::renderUi(EngineServices&) {}

void GameplayState::onEnter(EngineServices&) {}

void GameplayState::onExit(EngineServices& services) {
    services.input.partySelectionDrag.reset();
}

void GameplayState::update(EngineServices& services) {
    updateOrbitCamera(services.camera, services.time);
}

void GameplayState::renderUi(EngineServices&) {}

void EditorState::onEnter(EngineServices& services) {
    if (!services.editorSession.mainWindowVisible) {
        services.editorSession.openMainWindow();
    }
}

void EditorState::onExit(EngineServices& services) {
    services.editorSession.suspendEditorUi();
}

void EditorState::update(EngineServices& services) {
    updateOrbitCamera(services.camera, services.time);
}

void EditorState::renderUi(EngineServices& services) {
    drawEditorMainWindow(services);

    const auto profilerWindowStart = std::chrono::steady_clock::now();
    drawProfilerWindow(services);
    const auto profilerWindowEnd = std::chrono::steady_clock::now();
    services.profiler.recordProfilerUiTime(
        std::chrono::duration<double, std::milli>(profilerWindowEnd - profilerWindowStart).count()
    );

    ALKANZAR_PROFILE_SCOPE(services.profiler, "State UI Render");
    drawSceneHierarchyWindow(services);
    drawNavMeshWindow(services);
    drawInspectorWindow(services);
}

void TestToolState::onEnter(EngineServices& services) {
    services.time.paused = false;
    services.time.timeScale = 1.0f;
}

void TestToolState::onExit(EngineServices&) {}
void TestToolState::update(EngineServices&) {}
void TestToolState::renderUi(EngineServices&) {}

void ShutdownState::onEnter(EngineServices& services) {
    services.running = false;
}

void ShutdownState::onExit(EngineServices&) {}
void ShutdownState::update(EngineServices&) {}
void ShutdownState::renderUi(EngineServices&) {}

IAppState& AppStateCollection::forMode(AppMode mode) {
    switch (mode) {
        case AppMode::Bootstrap:
            return bootstrap_;
        case AppMode::Gameplay:
            return gameplay_;
        case AppMode::Editor:
            return editor_;
        case AppMode::TestTool:
            return testTool_;
        case AppMode::Shutdown:
        default:
            return shutdown_;
    }
}

}  // namespace core
