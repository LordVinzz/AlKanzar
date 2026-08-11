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
#include "core/scene/SceneAsset.hpp"
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
    ImGui::SetNextWindowSize(ImVec2(420.0f, 520.0f), ImGuiCond_FirstUseEver);

    bool open = services.editorSession.mainWindowVisible;
    std::string title = "Editor - " + services.sceneDocument.displayName();
    if (services.sceneDocument.dirty()) {
        title += " *";
    }
    title += "###Editor";
    if (ImGui::Begin(title.c_str(), &open, ImGuiWindowFlags_MenuBar)) {
        drawEditorMenuBar(services);
        ImGui::Text("SCN V%u", core::kSceneAssetVersion);
        ImGui::SameLine();
        ImGui::TextDisabled("%s", services.sceneDocument.hasPath()
            ? services.sceneDocument.sourcePath().string().c_str()
            : "Unsaved document");
        if (!services.editorSession.sceneDocumentStatus.empty()) {
            const ImVec4 color = services.editorSession.sceneDocumentStatusIsError
                ? ImVec4(0.95f, 0.35f, 0.30f, 1.0f)
                : ImVec4(0.35f, 0.85f, 0.45f, 1.0f);
            ImGui::TextColored(color, "%s", services.editorSession.sceneDocumentStatus.c_str());
        }

        drawSceneSettingsEditor(services);
        ImGui::SeparatorText("Window Toggles");

        bool sceneHierarchyVisible = services.editorSession.sceneHierarchyVisible;
        if (ImGui::Checkbox("Scene Hierarchy", &sceneHierarchyVisible)) {
            setPersistedEditorSessionFlag(services.editorSession.sceneHierarchyVisible, sceneHierarchyVisible);
            if (!sceneHierarchyVisible) {
                services.editorSession.sceneHierarchyFocusRequested = false;
            }
        }
        ImGui::SameLine();
        ImGui::TextDisabled("Ctrl+H");

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
