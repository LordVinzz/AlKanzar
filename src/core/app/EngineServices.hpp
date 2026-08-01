#pragma once

#include <optional>

#include "AppState.hpp"
#include "core/animation/AnimationSystem.hpp"
#include "core/ecs/ComponentRegistry.hpp"
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

struct InputSession {
    bool middleDragging{false};
    bool rightMouseLooking{false};
    int lastMouseX{0};
    int lastMouseY{0};
};

struct EngineServices {
    explicit EngineServices(int width, int height)
        : renderer(width, height),
          scheduler(),
          events(512u) {
        renderer.setProfiler(&profiler);
        scheduler.setProfiler(&profiler);
        navigationSystem.setProfiler(&profiler);
    }

    render::RenderEngine renderer;
    ProfilerService profiler;
    TaskScheduler scheduler;
    EventBus<AppEvent> events;
    CommandHistory commands;
    SelectionModel selection;
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
    std::optional<AppMode> requestedMode{};
};

}  // namespace core
