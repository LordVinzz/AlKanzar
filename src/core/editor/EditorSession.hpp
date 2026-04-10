#pragma once

#include <array>
#include <string>

#include "render/resources/Material.hpp"

namespace core {

enum class InspectorTab {
    Selection = 0,
    TextureBrowser,
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
    bool profilerFollowLatest{true};
    int profilerSelectedFrame{0};
    int animationInspectorSkinIndex{0};
    std::string profilerExportStatus{};
    bool profilerExportStatusIsError{false};
    std::array<char, 128> textureBrowserSearch{};
    std::array<char, 128> animationSkeletonSearch{};

    [[nodiscard]] bool anyToolWindowVisible() const {
        return sceneHierarchyVisible || inspectorWindowVisible || profilerWindowVisible;
    }

    void clearFocusRequests() {
        mainWindowFocusRequested = false;
        textureBrowserFocusRequested = false;
        sceneHierarchyFocusRequested = false;
        inspectorWindowFocusRequested = false;
        profilerWindowFocusRequested = false;
    }

    void setToolWindowsVisible(bool visible) {
        sceneHierarchyVisible = visible;
        inspectorWindowVisible = visible;
        profilerWindowVisible = visible;
        sceneHierarchyFocusRequested = false;
        inspectorWindowFocusRequested = false;
        profilerWindowFocusRequested = false;
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
