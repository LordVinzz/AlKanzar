#pragma once

#include <optional>

#include "AppState.hpp"
#include "core/animation/AnimationSystem.hpp"
#include "core/editor/ComponentRegistry.hpp"
#include "core/scene/Camera.hpp"
#include "core/editor/CommandHistory.hpp"
#include "core/editor/EditorSession.hpp"
#include "core/events/EventBus.hpp"
#include "core/events/Events.hpp"
#include "FrameData.hpp"
#include "core/lighting/LightSystem.hpp"
#include "core/navigation/Navigation.hpp"
#include "core/physics/PhysicsSystem.hpp"
#include "core/systems/PickingSystem.hpp"
#include "core/systems/PartySelectionModel.hpp"
#include "core/systems/PartySelectionSystem.hpp"
#include "core/profiling/ProfilerService.hpp"
#include "core/systems/RenderExtractionSystem.hpp"
#include "core/scene/SceneFactory.hpp"
#include "core/scene/SceneRegistry.hpp"
#include "core/editor/SelectionModel.hpp"
#include "core/systems/TaskScheduler.hpp"
#include "TimeContext.hpp"
#include "core/transform/TransformSystem.hpp"
#include "core/ecs/World.hpp"
#include "render/engine/RenderEngine.hpp"

namespace core {

struct PartySelectionDragSession {
    bool active{false};
    int startX{0};
    int startY{0};
    int currentX{0};
    int currentY{0};

    void begin(int x, int y) {
        active = true;
        startX = x;
        startY = y;
        currentX = x;
        currentY = y;
    }

    void update(int x, int y) {
        currentX = x;
        currentY = y;
    }

    void reset() {
        *this = PartySelectionDragSession{};
    }
};

struct InputSession {
    bool middleDragging{false};
    bool rightMouseLooking{false};
    int lastMouseX{0};
    int lastMouseY{0};
    PartySelectionDragSession partySelectionDrag{};
};

struct EngineServices {
    explicit EngineServices(int width, int height, AppMode initialMode = AppMode::Gameplay)
        : renderer(width, height),
          scheduler(),
          events(512u),
          startupMode(normalizeStartupMode(initialMode)) {
        renderer.setProfiler(&profiler);
        scheduler.setProfiler(&profiler);
        navigationSystem.setProfiler(&profiler);
    }

    render::RenderEngine renderer;
    ProfilerService profiler;
    TaskScheduler scheduler;
    EventBus<AppEvent> events;
    CommandHistory commands;
    SelectionModel editorSelection;
    PartySelectionModel partySelection;
    PartySelectionSystem partySelectionSystem;
    SceneRegistry sceneRegistry;
    SceneFactory sceneFactory;
    SceneBlueprint currentScene{};
    World world;
    TimeContext time;
    CameraState camera;
    EditorSession editorSession;
    FrameSceneData frame;
    AnimationSystem animationSystem;
    TransformSystem transformSystem;
    LightSystem lightSystem;
    NavigationRuntime navigation;
    NavigationSystem navigationSystem;
    PhysicsSystem physicsSystem;
    RenderExtractionSystem renderExtractionSystem;
    PickingSystem pickingSystem;
    ComponentRegistry componentRegistry;
    InputSession input;
    render::DebugView debugView{render::DebugView::Final};
    int shadowDebugCascade{0};
    bool showLightDebug{false};
    bool sceneLoaded{false};
    bool running{true};
    AppMode startupMode{AppMode::Gameplay};
    std::optional<AppMode> requestedMode{};
};

}  // namespace core
