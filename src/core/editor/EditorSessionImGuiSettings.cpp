#include "core/editor/EditorSessionImGuiSettings.hpp"

#include <cstdio>
#include <cstring>

#include <imgui.h>
#include <imgui_internal.h>

#include "core/editor/EditorSession.hpp"

namespace core {

namespace {

constexpr const char* kSettingsTypeName = "AlKanzar";
constexpr const char* kSettingsSectionName = "EditorSession";

void loadEditorSessionBoolLine(EditorSession& session, const char* line) {
    int value = 0;
    if (std::sscanf(line, "SceneHierarchyVisible=%d", &value) == 1) {
        session.sceneHierarchyVisible = value != 0;
    } else if (std::sscanf(line, "InspectorWindowVisible=%d", &value) == 1) {
        session.inspectorWindowVisible = value != 0;
    } else if (std::sscanf(line, "ProfilerWindowVisible=%d", &value) == 1) {
        session.profilerWindowVisible = value != 0;
    } else if (std::sscanf(line, "ProfilerFollowLatest=%d", &value) == 1) {
        session.profilerFollowLatest = value != 0;
    }
}

void* editorSessionSettingsReadOpen(ImGuiContext*, ImGuiSettingsHandler* handler, const char* name) {
    if (std::strcmp(name, kSettingsSectionName) != 0) {
        return nullptr;
    }

    return handler->UserData;
}

void editorSessionSettingsReadLine(ImGuiContext*, ImGuiSettingsHandler*, void* entry, const char* line) {
    if (entry == nullptr || line == nullptr) {
        return;
    }

    loadEditorSessionBoolLine(*static_cast<EditorSession*>(entry), line);
}

void editorSessionSettingsWriteAll(ImGuiContext*, ImGuiSettingsHandler* handler, ImGuiTextBuffer* outBuffer) {
    if (handler->UserData == nullptr || outBuffer == nullptr) {
        return;
    }

    const EditorSession& session = *static_cast<const EditorSession*>(handler->UserData);
    outBuffer->appendf("[%s][%s]\n", handler->TypeName, kSettingsSectionName);
    outBuffer->appendf("SceneHierarchyVisible=%d\n", session.sceneHierarchyVisible ? 1 : 0);
    outBuffer->appendf("InspectorWindowVisible=%d\n", session.inspectorWindowVisible ? 1 : 0);
    outBuffer->appendf("ProfilerWindowVisible=%d\n", session.profilerWindowVisible ? 1 : 0);
    outBuffer->appendf("ProfilerFollowLatest=%d\n", session.profilerFollowLatest ? 1 : 0);
    outBuffer->append("\n");
}

}  // namespace

void registerEditorSessionImGuiSettings(EditorSession& session) {
    ImGuiContext* context = ImGui::GetCurrentContext();
    if (context == nullptr) {
        return;
    }

    if (ImGuiSettingsHandler* existingHandler = ImGui::FindSettingsHandler(kSettingsTypeName);
        existingHandler != nullptr) {
        existingHandler->UserData = &session;
        return;
    }

    ImGuiSettingsHandler handler{};
    handler.TypeName = kSettingsTypeName;
    handler.TypeHash = ImHashStr(kSettingsTypeName);
    handler.UserData = &session;
    handler.ReadOpenFn = editorSessionSettingsReadOpen;
    handler.ReadLineFn = editorSessionSettingsReadLine;
    handler.WriteAllFn = editorSessionSettingsWriteAll;
    ImGui::AddSettingsHandler(&handler);
}

void markEditorSessionImGuiSettingsDirty() {
    if (ImGui::GetCurrentContext() == nullptr) {
        return;
    }

    ImGui::MarkIniSettingsDirty();
}

}  // namespace core
