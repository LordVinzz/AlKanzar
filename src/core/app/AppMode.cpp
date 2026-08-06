#include "AppMode.hpp"

#include <string_view>

namespace core {

namespace {

constexpr std::uint32_t systemMask(AppRuntimeSystem system) {
    return static_cast<std::uint32_t>(system);
}

constexpr std::uint32_t kRuntimeSystems =
    systemMask(AppRuntimeSystem::StateUpdate) |
    systemMask(AppRuntimeSystem::Navigation) |
    systemMask(AppRuntimeSystem::Animation) |
    systemMask(AppRuntimeSystem::Physics) |
    systemMask(AppRuntimeSystem::Transforms) |
    systemMask(AppRuntimeSystem::Lighting) |
    systemMask(AppRuntimeSystem::RenderExtraction);

constexpr AppModeCapabilities kInactiveCapabilities{};

constexpr AppModeCapabilities kGameplayCapabilities{
    .fixedStepSystems = kRuntimeSystems,
    .rendersWorld = true,
    .acceptsGameplayOrders = true,
    .acceptsPartySelection = true,
    .acceptsTimeControls = true,
    .acceptsCameraInput = true,
};

constexpr AppModeCapabilities kEditorCapabilities{
    .fixedStepSystems = kRuntimeSystems,
    .rendersWorld = true,
    .rendersEditorUi = true,
    .acceptsEditorInput = true,
    .acceptsTimeControls = true,
    .acceptsCameraInput = true,
    .usesEditorSelection = true,
    .showsEditorOverlays = true,
    .syncsNavigationDebug = true,
};

constexpr AppModeCapabilities kTestToolCapabilities{
    .fixedStepSystems = kRuntimeSystems,
    .rendersWorld = true,
    .usesDeterministicScene = true,
};

bool isPersistentRuntimeMode(AppMode mode) {
    return mode == AppMode::Gameplay || mode == AppMode::TestTool;
}

}  // namespace

const AppModeCapabilities& appModeCapabilities(AppMode mode) {
    switch (mode) {
        case AppMode::Gameplay:
            return kGameplayCapabilities;
        case AppMode::Editor:
            return kEditorCapabilities;
        case AppMode::TestTool:
            return kTestToolCapabilities;
        case AppMode::Bootstrap:
        case AppMode::Shutdown:
        default:
            return kInactiveCapabilities;
    }
}

const char* appModeName(AppMode mode) {
    switch (mode) {
        case AppMode::Bootstrap:
            return "Bootstrap";
        case AppMode::Gameplay:
            return "Gameplay";
        case AppMode::Editor:
            return "Editor";
        case AppMode::TestTool:
            return "TestTool";
        case AppMode::Shutdown:
        default:
            return "Shutdown";
    }
}

AppMode normalizeStartupMode(AppMode mode) {
    switch (mode) {
        case AppMode::Gameplay:
        case AppMode::Editor:
        case AppMode::TestTool:
            return mode;
        case AppMode::Bootstrap:
        case AppMode::Shutdown:
        default:
            return AppMode::Gameplay;
    }
}

AppMode startupModeFromArguments(int argc, const char* const* argv) {
    AppMode mode = AppMode::Gameplay;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        if (argument == "--test-tool") {
            mode = AppMode::TestTool;
        } else if (argument == "--editor") {
            mode = AppMode::Editor;
        } else if (argument == "--gameplay") {
            mode = AppMode::Gameplay;
        }
    }
    return mode;
}

AppMode AppModeSession::editorToggleTarget() const {
    if (current_ == AppMode::Editor) {
        return runtimeModeBeforeEditor_;
    }
    if (isPersistentRuntimeMode(current_)) {
        return AppMode::Editor;
    }
    return current_;
}

bool AppModeSession::transitionTo(AppMode mode) {
    if (current_ == mode) {
        return false;
    }
    current_ = mode;
    if (isPersistentRuntimeMode(mode)) {
        runtimeModeBeforeEditor_ = mode;
    }
    return true;
}

}  // namespace core
