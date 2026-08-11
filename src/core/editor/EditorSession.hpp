#pragma once

#include <array>
#include <optional>
#include <string>

#include "core/ecs/Components.hpp"
#include "render/resources/Material.hpp"

namespace core {

enum class InspectorTab {
    Selection = 0,
    TextureBrowser,
};

enum class SceneDocumentAction {
    None = 0,
    NewScene,
    OpenScene,
    ReloadScene,
    Quit,
};

enum class EditorGizmoOperation {
    Translate = 0,
    Rotate,
    Scale,
};

enum class EditorGizmoSpace {
    Local = 0,
    World,
};

struct EditorSession {
    InspectorTab activeInspectorTab{InspectorTab::Selection};
    render::MaterialTextureSlot textureBrowserSlot{render::MaterialTextureSlot::BaseColor};
    bool mainWindowVisible{false};
    bool mainWindowFocusRequested{false};
    bool textureBrowserFocusRequested{false};
    bool sceneHierarchyVisible{true};
    bool sceneHierarchyFocusRequested{false};
    bool inspectorWindowVisible{true};
    bool inspectorWindowFocusRequested{false};
    bool profilerWindowVisible{true};
    bool profilerWindowFocusRequested{false};
    bool navMeshWindowVisible{true};
    bool navMeshWindowFocusRequested{false};
    bool navMeshOverlayVisible{true};
    bool navMeshPolygonWireframeVisible{false};
    bool profilerFollowLatest{true};
    int profilerSelectedFrame{0};
    int animationInspectorSkinIndex{0};
    std::string profilerExportStatus{};
    bool profilerExportStatusIsError{false};
    std::array<char, 128> textureBrowserSearch{};
    std::array<char, 128> animationSkeletonSearch{};
    std::array<char, 256> scenePathInput{};
    std::string sceneDocumentStatus{};
    bool sceneDocumentStatusIsError{false};
    SceneDocumentAction pendingSceneAction{SceneDocumentAction::None};
    bool unsavedChangesDialogRequested{false};
    bool openSceneDialogRequested{false};
    bool saveAsSceneDialogRequested{false};
    bool resumePendingActionAfterSaveAs{false};
    EditorGizmoOperation gizmoOperation{EditorGizmoOperation::Translate};
    EditorGizmoSpace gizmoSpace{EditorGizmoSpace::World};
    bool gizmoSnapEnabled{false};
    bool gizmoWasUsing{false};
    TransformComponent gizmoStartTransform{};
    std::string gizmoActiveObjectId{};
    std::string pendingDeleteObjectId{};

    [[nodiscard]] bool anyToolWindowVisible() const {
        return sceneHierarchyVisible || inspectorWindowVisible || profilerWindowVisible || navMeshWindowVisible;
    }

    void clearFocusRequests() {
        mainWindowFocusRequested = false;
        textureBrowserFocusRequested = false;
        sceneHierarchyFocusRequested = false;
        inspectorWindowFocusRequested = false;
        profilerWindowFocusRequested = false;
        navMeshWindowFocusRequested = false;
    }

    void setToolWindowsVisible(bool visible) {
        sceneHierarchyVisible = visible;
        inspectorWindowVisible = visible;
        profilerWindowVisible = visible;
        navMeshWindowVisible = visible;
        sceneHierarchyFocusRequested = false;
        inspectorWindowFocusRequested = false;
        profilerWindowFocusRequested = false;
        navMeshWindowFocusRequested = false;
    }

    void ensureToolWindowsVisible() {
        if (anyToolWindowVisible()) {
            return;
        }

        setToolWindowsVisible(true);
    }

    void showAllWindows() {
        setToolWindowsVisible(true);
        mainWindowVisible = true;
        mainWindowFocusRequested = true;
        textureBrowserFocusRequested = false;
    }

    void openMainWindow() {
        mainWindowVisible = true;
        mainWindowFocusRequested = true;
    }

    void suspendEditorUi() {
        mainWindowVisible = false;
        clearFocusRequests();
    }
};

}  // namespace core
