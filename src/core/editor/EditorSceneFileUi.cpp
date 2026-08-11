#include "core/editor/EditorUi.hpp"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <imgui.h>

#include "EditorSceneActions.hpp"
#include "core/app/EngineServices.hpp"

namespace core {
namespace {

std::optional<std::filesystem::path> safeSceneName(
    const char* input,
    std::string& error
) {
    std::filesystem::path relative(input != nullptr ? input : "");
    if (relative.empty()) {
        error = "Enter a scene file name.";
        return std::nullopt;
    }
    if (relative.is_absolute() || relative.has_root_name() || relative.has_root_directory() ||
        relative != relative.filename()) {
        error = "Scene files must be direct children of assets/scenes.";
        return std::nullopt;
    }
    if (relative.extension().empty()) {
        relative += ".scene";
    }
    if (relative.extension() != ".scene") {
        error = "Scene files must use the .scene extension.";
        return std::nullopt;
    }
    error.clear();
    return relative;
}

void clearEditorAfterDocumentReplacement(EngineServices& services) {
    services.commands.clear();
    services.editorSelection.clear();
    services.editorSession.pendingDeleteObjectId.clear();
}

bool activateLoadedDocument(
    EngineServices& services,
    SceneDocument candidate,
    std::string* error
) {
    SceneDocument previous = services.sceneDocument;
    services.sceneDocument = std::move(candidate);
    if (rebuildEditorScene(services, std::nullopt, error)) {
        clearEditorAfterDocumentReplacement(services);
        return true;
    }
    services.sceneDocument = std::move(previous);
    std::string restoreError{};
    (void)rebuildEditorScene(services, std::nullopt, &restoreError);
    return false;
}

bool openSceneByName(EngineServices& services, const std::filesystem::path& relative) {
    const std::filesystem::path source = services.sceneRegistry.sourceSceneDirectory() / relative;
    const std::filesystem::path staged = services.sceneRegistry.stagedSceneDirectory() / relative;
    SceneDocument candidate{};
    std::string error{};
    if (!candidate.load(source, staged, &error) ||
        !activateLoadedDocument(services, std::move(candidate), &error)) {
        setEditorSceneStatus(services, std::move(error), true);
        return false;
    }
    setEditorSceneStatus(services, "Opened " + relative.string() + ".", false);
    return true;
}

bool saveCurrentScene(EngineServices& services) {
    services.sceneDocument.captureRuntimeWorld(services.world);
    std::string error{};
    if (!services.sceneDocument.save(&error)) {
        setEditorSceneStatus(services, std::move(error), true);
        return false;
    }
    setEditorSceneStatus(services, "Saved " + services.sceneDocument.displayName() + ".", false);
    return true;
}

bool saveCurrentSceneAs(EngineServices& services, const std::filesystem::path& relative) {
    services.sceneDocument.captureRuntimeWorld(services.world);
    const std::filesystem::path source = services.sceneRegistry.sourceSceneDirectory() / relative;
    const std::filesystem::path staged = services.sceneRegistry.stagedSceneDirectory() / relative;
    std::string error{};
    if (!services.sceneDocument.saveAs(source, staged, &error)) {
        setEditorSceneStatus(services, std::move(error), true);
        return false;
    }
    setEditorSceneStatus(services, "Saved " + relative.string() + ".", false);
    return true;
}

void performPendingAction(EngineServices& services) {
    const SceneDocumentAction action = services.editorSession.pendingSceneAction;
    services.editorSession.pendingSceneAction = SceneDocumentAction::None;
    switch (action) {
        case SceneDocumentAction::NewScene: {
            SceneDocument candidate{};
            candidate.createEmpty();
            std::string error{};
            if (activateLoadedDocument(services, std::move(candidate), &error)) {
                setEditorSceneStatus(services, "Created a new untitled SCN V2 scene.", false);
            } else {
                setEditorSceneStatus(services, std::move(error), true);
            }
            break;
        }
        case SceneDocumentAction::OpenScene:
            services.editorSession.openSceneDialogRequested = true;
            break;
        case SceneDocumentAction::ReloadScene: {
            if (!services.sceneDocument.hasPath()) {
                setEditorSceneStatus(services, "Untitled scenes cannot be reloaded.", true);
                break;
            }
            SceneDocument candidate{};
            std::string error{};
            if (!candidate.load(
                    services.sceneDocument.sourcePath(),
                    services.sceneDocument.stagedPath(),
                    &error) ||
                !activateLoadedDocument(services, std::move(candidate), &error)) {
                setEditorSceneStatus(services, std::move(error), true);
            } else {
                setEditorSceneStatus(services, "Reloaded the scene from disk.", false);
            }
            break;
        }
        case SceneDocumentAction::Quit:
            services.requestedMode = AppMode::Shutdown;
            break;
        case SceneDocumentAction::None:
            break;
    }
}

void beginDocumentAction(EngineServices& services, SceneDocumentAction action) {
    const bool needsConfirmation = services.sceneDocument.dirty();
    requestSceneDocumentAction(services, action);
    if (!needsConfirmation && action != SceneDocumentAction::OpenScene) {
        performPendingAction(services);
    }
}

void drawCreateMenu(EngineServices& services) {
    if (!ImGui::BeginMenu("Create")) return;
    if (ImGui::MenuItem("Plane Primitive")) {
        (void)createEditorSceneObject(
            services,
            SceneObjectType::Primitive,
            {},
            ScenePrimitiveShape::Plane
        );
    }
    if (ImGui::MenuItem("Box Primitive")) {
        (void)createEditorSceneObject(
            services,
            SceneObjectType::Primitive,
            {},
            ScenePrimitiveShape::Box
        );
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Adventurer Model")) {
        (void)createEditorSceneObject(services, SceneObjectType::Model, "Adventurer.glb");
    }
    if (ImGui::MenuItem("House Model")) {
        (void)createEditorSceneObject(services, SceneObjectType::Model, "FantasyHouse.glb");
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Directional Light")) {
        (void)createEditorSceneObject(services, SceneObjectType::DirectionalLight);
    }
    if (ImGui::MenuItem("Point Light")) {
        (void)createEditorSceneObject(services, SceneObjectType::PointLight);
    }
    if (ImGui::MenuItem("Spot Light")) {
        (void)createEditorSceneObject(services, SceneObjectType::SpotLight);
    }
    if (ImGui::MenuItem("Light Volume")) {
        (void)createEditorSceneObject(services, SceneObjectType::LightVolume);
    }
    ImGui::EndMenu();
}

void commitSceneSettings(
    EngineServices& services,
    SceneBlueprint after,
    const char* label
) {
    executeEditorSceneSnapshot(
        services,
        label,
        EditorSceneSnapshot{services.sceneDocument.blueprint(), selectedAuthoredObject(services)},
        EditorSceneSnapshot{std::move(after), selectedAuthoredObject(services)}
    );
}

void drawUnsavedChangesDialog(EngineServices& services) {
    if (services.editorSession.unsavedChangesDialogRequested) {
        ImGui::OpenPopup("Unsaved Scene Changes");
        services.editorSession.unsavedChangesDialogRequested = false;
    }
    if (!ImGui::BeginPopupModal(
            "Unsaved Scene Changes",
            nullptr,
            ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }
    ImGui::TextWrapped(
        "%s has unsaved changes. Save them before continuing?",
        services.sceneDocument.displayName().c_str()
    );
    if (ImGui::Button("Save")) {
        bool closeDialog = false;
        if (services.sceneDocument.hasPath()) {
            if (saveCurrentScene(services)) {
                performPendingAction(services);
                closeDialog = true;
            }
        } else {
            services.editorSession.resumePendingActionAfterSaveAs = true;
            services.editorSession.saveAsSceneDialogRequested = true;
            closeDialog = true;
        }
        if (closeDialog) {
            ImGui::CloseCurrentPopup();
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Discard")) {
        performPendingAction(services);
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) {
        services.editorSession.pendingSceneAction = SceneDocumentAction::None;
        services.editorSession.resumePendingActionAfterSaveAs = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void drawDeleteObjectDialog(EngineServices& services) {
    if (!services.editorSession.pendingDeleteObjectId.empty()) {
        ImGui::OpenPopup("Delete Authored Object?");
    }
    if (!ImGui::BeginPopupModal(
            "Delete Authored Object?",
            nullptr,
            ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }

    const std::string objectId = services.editorSession.pendingDeleteObjectId;
    ImGui::TextWrapped(
        "Delete '%s' and every authored child below it? This operation can be undone.",
        objectId.c_str()
    );
    if (ImGui::Button("Delete")) {
        (void)deleteEditorSceneObject(services, objectId);
        services.editorSession.pendingDeleteObjectId.clear();
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) {
        services.editorSession.pendingDeleteObjectId.clear();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void drawOpenSceneDialog(EngineServices& services) {
    if (services.editorSession.openSceneDialogRequested) {
        ImGui::OpenPopup("Open SCN Scene");
        services.editorSession.openSceneDialogRequested = false;
        services.editorSession.scenePathInput.fill('\0');
    }
    if (!ImGui::BeginPopupModal("Open SCN Scene", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;

    std::vector<std::filesystem::path> scenes{};
    std::error_code iteratorError{};
    for (std::filesystem::directory_iterator it(
             services.sceneRegistry.sourceSceneDirectory(),
             iteratorError), end;
         !iteratorError && it != end;
         it.increment(iteratorError)) {
        if (it->is_regular_file() && it->path().extension() == ".scene") {
            scenes.push_back(it->path().filename());
        }
    }
    std::sort(scenes.begin(), scenes.end());
    ImGui::TextDisabled("assets/scenes");
    if (ImGui::BeginListBox("##SceneFiles", ImVec2(420.0f, 180.0f))) {
        for (const auto& scene : scenes) {
            const bool selected = scene.string() == services.editorSession.scenePathInput.data();
            if (ImGui::Selectable(scene.string().c_str(), selected)) {
                std::strncpy(
                    services.editorSession.scenePathInput.data(),
                    scene.string().c_str(),
                    services.editorSession.scenePathInput.size() - 1u
                );
            }
        }
        ImGui::EndListBox();
    }
    ImGui::InputText("File", services.editorSession.scenePathInput.data(), services.editorSession.scenePathInput.size());
    if (ImGui::Button("Open")) {
        std::string error{};
        const auto relative = safeSceneName(services.editorSession.scenePathInput.data(), error);
        if (relative.has_value() && openSceneByName(services, *relative)) {
            services.editorSession.pendingSceneAction = SceneDocumentAction::None;
            ImGui::CloseCurrentPopup();
        } else if (!relative.has_value()) {
            setEditorSceneStatus(services, std::move(error), true);
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) {
        services.editorSession.pendingSceneAction = SceneDocumentAction::None;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void drawSaveAsDialog(EngineServices& services) {
    if (services.editorSession.saveAsSceneDialogRequested) {
        ImGui::OpenPopup("Save SCN Scene As");
        services.editorSession.saveAsSceneDialogRequested = false;
        services.editorSession.scenePathInput.fill('\0');
        const std::string initial = services.sceneDocument.displayName();
        std::strncpy(
            services.editorSession.scenePathInput.data(),
            initial.c_str(),
            services.editorSession.scenePathInput.size() - 1u
        );
    }
    if (!ImGui::BeginPopupModal("Save SCN Scene As", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    ImGui::TextDisabled("Saved atomically to assets/scenes and mirrored to build/scenes.");
    ImGui::InputText("File", services.editorSession.scenePathInput.data(), services.editorSession.scenePathInput.size());
    std::string error{};
    const auto relative = safeSceneName(services.editorSession.scenePathInput.data(), error);
    const bool exists = relative.has_value() && std::filesystem::exists(
        services.sceneRegistry.sourceSceneDirectory() / *relative
    );
    if (exists) {
        ImGui::TextColored(ImVec4(0.95f, 0.65f, 0.20f, 1.0f), "This file exists. Overwrite is explicit.");
    }
    if (ImGui::Button(exists ? "Overwrite" : "Save")) {
        if (!relative.has_value()) {
            setEditorSceneStatus(services, std::move(error), true);
        } else if (saveCurrentSceneAs(services, *relative)) {
            const bool resume = services.editorSession.resumePendingActionAfterSaveAs;
            services.editorSession.resumePendingActionAfterSaveAs = false;
            ImGui::CloseCurrentPopup();
            if (resume) performPendingAction(services);
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) {
        services.editorSession.resumePendingActionAfterSaveAs = false;
        services.editorSession.pendingSceneAction = SceneDocumentAction::None;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

}  // namespace

void drawEditorMenuBar(EngineServices& services) {
    if (!ImGui::BeginMenuBar()) return;
    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("New", "Ctrl+Shift+N")) beginDocumentAction(services, SceneDocumentAction::NewScene);
        if (ImGui::MenuItem("Open...", "Ctrl+O")) beginDocumentAction(services, SceneDocumentAction::OpenScene);
        if (ImGui::MenuItem("Reload")) beginDocumentAction(services, SceneDocumentAction::ReloadScene);
        ImGui::Separator();
        if (ImGui::MenuItem("Save", "Ctrl+S")) {
            if (services.sceneDocument.hasPath()) (void)saveCurrentScene(services);
            else services.editorSession.saveAsSceneDialogRequested = true;
        }
        if (ImGui::MenuItem("Save As...", "Ctrl+Shift+S")) {
            services.editorSession.saveAsSceneDialogRequested = true;
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Quit")) beginDocumentAction(services, SceneDocumentAction::Quit);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Edit")) {
        if (ImGui::MenuItem("Undo", "Ctrl+Z", false, services.commands.canUndo())) services.commands.undo();
        if (ImGui::MenuItem("Redo", "Ctrl+Shift+Z", false, services.commands.canRedo())) services.commands.redo();
        const std::optional<SceneObjectId> selected = selectedAuthoredObject(services);
        ImGui::Separator();
        if (ImGui::MenuItem("Duplicate", "Ctrl+D", false, selected.has_value())) {
            (void)duplicateEditorSceneObject(services, *selected);
        }
        if (ImGui::MenuItem("Delete...", "Delete", false, selected.has_value())) {
            services.editorSession.pendingDeleteObjectId = *selected;
        }
        ImGui::EndMenu();
    }
    drawCreateMenu(services);
    ImGui::EndMenuBar();
}

void beginEditorSceneDocumentAction(EngineServices& services, SceneDocumentAction action) {
    beginDocumentAction(services, action);
}

void drawSceneDocumentDialogs(EngineServices& services) {
    drawUnsavedChangesDialog(services);
    drawOpenSceneDialog(services);
    drawSaveAsDialog(services);
    drawDeleteObjectDialog(services);
}

void drawSceneSettingsEditor(EngineServices& services) {
    if (!ImGui::CollapsingHeader("Persistent Scene Settings", ImGuiTreeNodeFlags_DefaultOpen)) return;
    SceneBlueprint after = services.sceneDocument.blueprint();
    bool commit = false;
    char navMesh[256]{};
    std::strncpy(navMesh, after.navMeshAssetPath.c_str(), sizeof(navMesh) - 1u);
    if (ImGui::InputText("NavMesh Asset", navMesh, sizeof(navMesh), ImGuiInputTextFlags_EnterReturnsTrue)) {
        after.navMeshAssetPath = navMesh;
        commit = true;
    }
    if (commit) {
        commitSceneSettings(services, std::move(after), "Edit Scene Settings");
    }
}

}  // namespace core
