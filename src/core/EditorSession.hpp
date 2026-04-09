#pragma once

#include <array>
#include <string>

#include "render/Material.hpp"

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
    bool sceneHierarchyVisible{false};
    bool sceneHierarchyFocusRequested{false};
    bool inspectorWindowVisible{false};
    bool inspectorWindowFocusRequested{false};
    bool profilerWindowVisible{false};
    bool profilerWindowFocusRequested{false};
    bool profilerFollowLatest{true};
    int profilerSelectedFrame{0};
    std::string profilerExportStatus{};
    bool profilerExportStatusIsError{false};
    std::array<char, 128> textureBrowserSearch{};
};

}  // namespace core
