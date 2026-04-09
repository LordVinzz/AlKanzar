#pragma once

#include <optional>

#include "AppState.hpp"
#include "Camera.hpp"
#include "CommandHistory.hpp"
#include "EditorSession.hpp"
#include "EventBus.hpp"
#include "Events.hpp"
#include "FrameData.hpp"
#include "LightSystem.hpp"
#include "MaterialLibrary.hpp"
#include "PickingSystem.hpp"
#include "RenderExtractionSystem.hpp"
#include "SceneFactory.hpp"
#include "SceneRegistry.hpp"
#include "SelectionModel.hpp"
#include "TimeContext.hpp"
#include "TransformSystem.hpp"
#include "World.hpp"
#include "render/RenderEngine.hpp"

namespace core {

struct InputSession {
    bool middleDragging{false};
    int lastMouseX{0};
    int lastMouseY{0};
};

struct EngineServices {
    explicit EngineServices(int width, int height)
        : renderer(width, height),
          events(512u) {}

    render::RenderEngine renderer;
    EventBus<AppEvent> events;
    CommandHistory commands;
    SelectionModel selection;
    SceneRegistry sceneRegistry;
    SceneFactory sceneFactory;
    MaterialLibrary materials;
    World world;
    TimeContext time;
    CameraState camera;
    EditorSession editorSession;
    FrameSceneData frame;
    TransformSystem transformSystem;
    LightSystem lightSystem;
    RenderExtractionSystem renderExtractionSystem;
    PickingSystem pickingSystem;
    InputSession input;
    render::DebugView debugView{render::DebugView::Final};
    int shadowDebugCascade{0};
    bool showLightDebug{false};
    bool sceneLoaded{false};
    bool running{true};
    std::optional<AppMode> requestedMode{};
};

}  // namespace core
