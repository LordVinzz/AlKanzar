#pragma once

#include <array>

#include "render/Material.hpp"

namespace core {

enum class InspectorTab {
    Selection = 0,
    TextureBrowser,
};

struct EditorSession {
    InspectorTab activeInspectorTab{InspectorTab::Selection};
    render::MaterialTextureSlot textureBrowserSlot{render::MaterialTextureSlot::BaseColor};
    bool textureBrowserFocusRequested{false};
    bool sceneHierarchyVisible{false};
    bool sceneHierarchyFocusRequested{false};
    std::array<char, 128> textureBrowserSearch{};
};

}  // namespace core
