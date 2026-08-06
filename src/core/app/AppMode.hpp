#pragma once

#include <cstdint>

namespace core {

enum class AppMode {
    Bootstrap,
    Gameplay,
    Editor,
    TestTool,
    Shutdown,
};

enum class AppRuntimeSystem : std::uint32_t {
    StateUpdate = 1u << 0u,
    Navigation = 1u << 1u,
    Animation = 1u << 2u,
    Physics = 1u << 3u,
    Transforms = 1u << 4u,
    Lighting = 1u << 5u,
    RenderExtraction = 1u << 6u,
};

struct AppModeCapabilities {
    std::uint32_t fixedStepSystems{0u};
    bool rendersWorld{false};
    bool rendersEditorUi{false};
    bool acceptsEditorInput{false};
    bool acceptsGameplayOrders{false};
    bool acceptsPartySelection{false};
    bool acceptsTimeControls{false};
    bool acceptsCameraInput{false};
    bool usesEditorSelection{false};
    bool showsEditorOverlays{false};
    bool syncsNavigationDebug{false};
    bool usesDeterministicScene{false};

    [[nodiscard]] bool runs(AppRuntimeSystem system) const {
        return (fixedStepSystems & static_cast<std::uint32_t>(system)) != 0u;
    }
};

[[nodiscard]] const AppModeCapabilities& appModeCapabilities(AppMode mode);
[[nodiscard]] const char* appModeName(AppMode mode);
[[nodiscard]] AppMode normalizeStartupMode(AppMode mode);
[[nodiscard]] AppMode startupModeFromArguments(int argc, const char* const* argv);

class AppModeSession {
public:
    [[nodiscard]] AppMode current() const { return current_; }
    [[nodiscard]] AppMode editorToggleTarget() const;
    [[nodiscard]] const AppModeCapabilities& capabilities() const {
        return appModeCapabilities(current_);
    }

    bool transitionTo(AppMode mode);

private:
    AppMode current_{AppMode::Shutdown};
    AppMode runtimeModeBeforeEditor_{AppMode::Gameplay};
};

}  // namespace core
