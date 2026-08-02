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
    services.currentScene = services.sceneRegistry.defaultScene();
    services.sceneLoaded = services.sceneFactory.buildScene(
        services.currentScene,
        services.world,
        services.renderer
    );
    if (!services.sceneLoaded) {
        spdlog::error("Application: failed to build default scene");
        services.requestedMode = AppMode::Shutdown;
        return;
    }
    if (!services.navigationSystem.initializeScene(services.currentScene, services.world, services.navigation)) {
        spdlog::error("Application: navigation init failed: {}", services.navigation.statusMessage);
    }
    services.requestedMode = AppMode::Gameplay;
}

void BootstrapState::onExit(EngineServices&) {}
void BootstrapState::update(EngineServices&) {}
void BootstrapState::renderUi(EngineServices&) {}

void GameplayState::onEnter(EngineServices&) {}

void GameplayState::onExit(EngineServices&) {}

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

void ShutdownState::onEnter(EngineServices& services) {
    services.running = false;
}

void ShutdownState::onExit(EngineServices&) {}
void ShutdownState::update(EngineServices&) {}
void ShutdownState::renderUi(EngineServices&) {}

}  // namespace core
