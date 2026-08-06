#include "core/editor/EditorUi.hpp"
#include "core/editor/EditorUiCommands.hpp"

#include <algorithm>
#include <array>
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

#include "core/app/EngineServices.hpp"
#include "core/editor/EditorSessionImGuiSettings.hpp"
#include "core/editor/ComponentRegistry.hpp"
#include "render/resources/StaticGltfModel.hpp"


namespace core {

void drawEditorMainWindow(EngineServices& services) {
    if (!services.editorSession.mainWindowVisible) {
        services.editorSession.mainWindowFocusRequested = false;
        return;
    }

    if (services.editorSession.mainWindowFocusRequested) {
        ImGui::SetNextWindowFocus();
    }
    ImGui::SetNextWindowSize(ImVec2(360.0f, 260.0f), ImGuiCond_FirstUseEver);

    bool open = services.editorSession.mainWindowVisible;
    if (ImGui::Begin("Editor", &open)) {
        ImGui::TextUnformatted("Window Toggles");
        ImGui::Separator();

        bool sceneHierarchyVisible = services.editorSession.sceneHierarchyVisible;
        if (ImGui::Checkbox("Scene Hierarchy", &sceneHierarchyVisible)) {
            setPersistedEditorSessionFlag(services.editorSession.sceneHierarchyVisible, sceneHierarchyVisible);
            if (!sceneHierarchyVisible) {
                services.editorSession.sceneHierarchyFocusRequested = false;
            }
        }
        ImGui::SameLine();
        ImGui::TextDisabled("Ctrl+S");

        bool inspectorVisible = services.editorSession.inspectorWindowVisible;
        if (ImGui::Checkbox("Inspector", &inspectorVisible)) {
            setPersistedEditorSessionFlag(services.editorSession.inspectorWindowVisible, inspectorVisible);
            if (!inspectorVisible) {
                services.editorSession.inspectorWindowFocusRequested = false;
            }
        }
        ImGui::SameLine();
        ImGui::TextDisabled("Ctrl+I");

        bool profilerVisible = services.editorSession.profilerWindowVisible;
        if (ImGui::Checkbox("Profiler", &profilerVisible)) {
            setPersistedEditorSessionFlag(services.editorSession.profilerWindowVisible, profilerVisible);
            if (!profilerVisible) {
                services.editorSession.profilerWindowFocusRequested = false;
            }
        }
        ImGui::SameLine();
        ImGui::TextDisabled("Ctrl+P");

        bool navMeshVisible = services.editorSession.navMeshWindowVisible;
        if (ImGui::Checkbox("NavMesh", &navMeshVisible)) {
            setPersistedEditorSessionFlag(services.editorSession.navMeshWindowVisible, navMeshVisible);
            if (!navMeshVisible) {
                services.editorSession.navMeshWindowFocusRequested = false;
            }
        }
        ImGui::SameLine();
        ImGui::TextDisabled("Ctrl+N");

        ImGui::Separator();
        if (ImGui::Button("Show All")) {
            const bool wasSceneHierarchyVisible = services.editorSession.sceneHierarchyVisible;
            const bool wasInspectorVisible = services.editorSession.inspectorWindowVisible;
            const bool wasProfilerVisible = services.editorSession.profilerWindowVisible;
            const bool wasNavMeshVisible = services.editorSession.navMeshWindowVisible;
            services.editorSession.setToolWindowsVisible(true);
            if (wasSceneHierarchyVisible != services.editorSession.sceneHierarchyVisible ||
                wasInspectorVisible != services.editorSession.inspectorWindowVisible ||
                wasProfilerVisible != services.editorSession.profilerWindowVisible ||
                wasNavMeshVisible != services.editorSession.navMeshWindowVisible) {
                markEditorSessionImGuiSettingsDirty();
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Hide All")) {
            const bool wasSceneHierarchyVisible = services.editorSession.sceneHierarchyVisible;
            const bool wasInspectorVisible = services.editorSession.inspectorWindowVisible;
            const bool wasProfilerVisible = services.editorSession.profilerWindowVisible;
            const bool wasNavMeshVisible = services.editorSession.navMeshWindowVisible;
            services.editorSession.setToolWindowsVisible(false);
            if (wasSceneHierarchyVisible != services.editorSession.sceneHierarchyVisible ||
                wasInspectorVisible != services.editorSession.inspectorWindowVisible ||
                wasProfilerVisible != services.editorSession.profilerWindowVisible ||
                wasNavMeshVisible != services.editorSession.navMeshWindowVisible) {
                markEditorSessionImGuiSettingsDirty();
            }
        }

        ImGui::Separator();
        ImGui::TextDisabled("Press E to toggle all editor features.");
    }
    ImGui::End();

    services.editorSession.mainWindowVisible = open;
    services.editorSession.mainWindowFocusRequested = false;
}

}  // namespace core
