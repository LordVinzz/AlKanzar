#include "Application.hpp"

#include <algorithm>
#include <array>
#include <optional>

#include "core/editor/EditorSessionImGuiSettings.hpp"
#include "core/scene/Camera.hpp"

namespace core {

void Application::bindEventHandlers() {
    services_.selection.changed().connect([this](const std::optional<SelectionTarget>& selection) {
        services_.events.publish(SelectionChangedEvent{selection});
    });

    services_.events.subscribe<QuitRequestedEvent>([this](const QuitRequestedEvent&) {
        services_.requestedMode = AppMode::Shutdown;
    });
    services_.events.subscribe<ToggleEditorEvent>([this](const ToggleEditorEvent&) {
        if (currentMode_ == AppMode::Editor) {
            services_.editorSession.suspendEditorUi();
        } else {
            services_.editorSession.openMainWindow();
        }
        services_.requestedMode = currentMode_ == AppMode::Editor ? AppMode::Gameplay : AppMode::Editor;
    });
    services_.events.subscribe<UndoRequestedEvent>([this](const UndoRequestedEvent&) {
        services_.commands.undo();
    });
    services_.events.subscribe<RedoRequestedEvent>([this](const RedoRequestedEvent&) {
        services_.commands.redo();
    });
    services_.events.subscribe<WindowResizedEvent>([this](const WindowResizedEvent& event) {
        services_.renderer.resize(event.width, event.height);
    });
    services_.events.subscribe<ViewportWheelEvent>([this](const ViewportWheelEvent& event) {
        if (services_.renderer.wantsMouse() || event.delta == 0) {
            return;
        }
        if (services_.camera.freeCameraEnabled) {
            adjustFreeCameraSpeed(services_.camera, event.delta);
            return;
        }
        const float factor = event.delta > 0 ? 0.9f : 1.1f;
        services_.camera.zoom = std::clamp(services_.camera.zoom * factor, 0.2f, 5.0f);
    });
    services_.events.subscribe<ViewportPanEvent>([this](const ViewportPanEvent& event) {
        if (services_.renderer.wantsMouse() ||
            services_.camera.orbitEnabled ||
            services_.camera.freeCameraEnabled) {
            return;
        }
        constexpr float panSpeed = 0.01f;
        services_.camera.panX -= static_cast<float>(event.dx) * panSpeed / services_.camera.zoom;
        services_.camera.panY += static_cast<float>(event.dy) * panSpeed / services_.camera.zoom;
    });
    services_.events.subscribe<ToggleFreeCameraEvent>([this](const ToggleFreeCameraEvent&) {
        const bool enable = !services_.camera.freeCameraEnabled;
        if (!enable) {
            releaseFreeCameraMouse();
        }
        setFreeCameraEnabled(services_.camera, enable);
    });
    services_.events.subscribe<ToggleLightDebugEvent>([this](const ToggleLightDebugEvent&) {
        services_.showLightDebug = !services_.showLightDebug;
    });
    services_.events.subscribe<ToggleSimulationPauseEvent>([this](const ToggleSimulationPauseEvent&) {
        services_.time.paused = !services_.time.paused;
    });
    services_.events.subscribe<AdjustSimulationSpeedEvent>([this](const AdjustSimulationSpeedEvent& event) {
        constexpr std::array<float, 4> speeds{0.5f, 1.0f, 2.0f, 4.0f};
        const auto current = std::lower_bound(speeds.begin(), speeds.end(), services_.time.timeScale);
        const std::size_t index = current == speeds.end()
            ? speeds.size() - 1u
            : static_cast<std::size_t>(std::distance(speeds.begin(), current));
        const int next = std::clamp(static_cast<int>(index) + event.direction, 0, static_cast<int>(speeds.size() - 1u));
        services_.time.timeScale = speeds[static_cast<std::size_t>(next)];
    });
    services_.events.subscribe<DebugViewSelectedEvent>([this](const DebugViewSelectedEvent& event) {
        services_.debugView = event.view;
    });
    services_.events.subscribe<ShadowCascadeStepEvent>([this](const ShadowCascadeStepEvent& event) {
        services_.shadowDebugCascade = std::max(0, services_.shadowDebugCascade + event.delta);
    });
    services_.events.subscribe<ViewportClickedEvent>([this](const ViewportClickedEvent& event) {
        ALKANZAR_PROFILE_SCOPE(services_.profiler, "Viewport Click");
        if (services_.renderer.wantsMouse()) {
            return;
        }
        const render::CameraMatrices camera = computeCameraMatrices(
            services_.camera,
            services_.renderer.width(),
            services_.renderer.height()
        );
        const auto moveControlledAgent = [&]() {
            if (services_.world.navAgents.entities().empty()) {
                return false;
            }
            std::optional<NavHitResult> hit{};
            {
                ALKANZAR_PROFILE_SCOPE(services_.profiler, "Navigation Hit Test");
                hit = services_.navigationSystem.hitTest(
                    services_.navigation,
                    camera,
                    services_.renderer.width(),
                    services_.renderer.height(),
                    event.x,
                    event.y
                );
            }
            if (!hit.has_value()) {
                return false;
            }
            {
                ALKANZAR_PROFILE_SCOPE(services_.profiler, "Navigation Path Request");
                return services_.navigationSystem.requestAgentDestination(
                    services_.world,
                    services_.navigation,
                    services_.scheduler,
                    services_.world.navAgents.entities().front(),
                    hit->position
                );
            }
        };

        if (currentMode_ == AppMode::Gameplay) {
            ALKANZAR_PROFILE_SCOPE(services_.profiler, "Gameplay Click Move");
            moveControlledAgent();
            return;
        }
        if (currentMode_ != AppMode::Editor) {
            return;
        }
        if (services_.navigation.editor.polygonCaptureActive) {
            ALKANZAR_PROFILE_SCOPE(services_.profiler, "Navigation Polygon Capture");
            services_.navigationSystem.capturePolygonClick(
                services_.navigation,
                camera,
                services_.renderer.width(),
                services_.renderer.height(),
                event.x,
                event.y
            );
            return;
        }
        if (services_.navigation.editor.testMoveMode) {
            ALKANZAR_PROFILE_SCOPE(services_.profiler, "Editor Test Move");
            moveControlledAgent();
            return;
        }

        {
            ALKANZAR_PROFILE_SCOPE(services_.profiler, "Viewport Selection");
            services_.selection.set(services_.pickingSystem.pick(
                services_.frame,
                camera,
                services_.renderer.width(),
                services_.renderer.height(),
                event.x,
                event.y,
                false
            ));
        }
    });
}

void Application::translateSdlEvent(const SDL_Event& event) {
    switch (event.type) {
        case SDL_QUIT:
            services_.events.publish(QuitRequestedEvent{});
            break;
        case SDL_KEYDOWN: {
            if (event.key.keysym.sym == SDLK_ESCAPE) {
                services_.events.publish(QuitRequestedEvent{});
                break;
            }
            if (event.key.keysym.sym == SDLK_e && event.key.repeat == 0) {
                services_.events.publish(ToggleEditorEvent{});
                break;
            }

            const SDL_Keymod modifiers = SDL_GetModState();
            const bool primaryModifier = (modifiers & KMOD_CTRL) != 0 || (modifiers & KMOD_GUI) != 0;
            if (primaryModifier && event.key.repeat == 0) {
                if (currentMode_ == AppMode::Editor) {
                    switch (event.key.keysym.sym) {
                        case SDLK_i:
                            setPersistedEditorSessionFlag(
                                services_.editorSession.inspectorWindowVisible,
                                !services_.editorSession.inspectorWindowVisible
                            );
                            services_.editorSession.inspectorWindowFocusRequested =
                                services_.editorSession.inspectorWindowVisible;
                            break;
                        case SDLK_p:
                            setPersistedEditorSessionFlag(
                                services_.editorSession.profilerWindowVisible,
                                !services_.editorSession.profilerWindowVisible
                            );
                            services_.editorSession.profilerWindowFocusRequested =
                                services_.editorSession.profilerWindowVisible;
                            break;
                        case SDLK_n:
                            setPersistedEditorSessionFlag(
                                services_.editorSession.navMeshWindowVisible,
                                !services_.editorSession.navMeshWindowVisible
                            );
                            services_.editorSession.navMeshWindowFocusRequested =
                                services_.editorSession.navMeshWindowVisible;
                            break;
                        case SDLK_s:
                            setPersistedEditorSessionFlag(
                                services_.editorSession.sceneHierarchyVisible,
                                !services_.editorSession.sceneHierarchyVisible
                            );
                            services_.editorSession.sceneHierarchyFocusRequested =
                                services_.editorSession.sceneHierarchyVisible;
                            break;
                        default:
                            break;
                    }

                    if (event.key.keysym.sym == SDLK_i ||
                        event.key.keysym.sym == SDLK_n ||
                        event.key.keysym.sym == SDLK_p ||
                        event.key.keysym.sym == SDLK_s) {
                        break;
                    }
                }

                if (event.key.keysym.sym == SDLK_z) {
                    if ((modifiers & KMOD_SHIFT) != 0) {
                        services_.events.publish(RedoRequestedEvent{});
                    } else {
                        services_.events.publish(UndoRequestedEvent{});
                    }
                    break;
                }
                if (event.key.keysym.sym == SDLK_y) {
                    services_.events.publish(RedoRequestedEvent{});
                    break;
                }
            }

            if (services_.renderer.wantsKeyboard()) {
                break;
            }

            switch (event.key.keysym.sym) {
                case SDLK_SPACE:
                    if (event.key.repeat == 0) {
                        services_.events.publish(ToggleSimulationPauseEvent{});
                    }
                    break;
                case SDLK_MINUS:
                    if (event.key.repeat == 0) {
                        services_.events.publish(AdjustSimulationSpeedEvent{-1});
                    }
                    break;
                case SDLK_EQUALS:
                case SDLK_KP_PLUS:
                    if (event.key.repeat == 0) {
                        services_.events.publish(AdjustSimulationSpeedEvent{1});
                    }
                    break;
                case SDLK_F1:
                    if (event.key.repeat == 0) {
                        services_.events.publish(ToggleEditorEvent{});
                    }
                    break;
                case SDLK_0:
                    services_.events.publish(DebugViewSelectedEvent{render::DebugView::Final});
                    break;
                case SDLK_1:
                    services_.events.publish(DebugViewSelectedEvent{render::DebugView::Albedo});
                    break;
                case SDLK_2:
                    services_.events.publish(DebugViewSelectedEvent{render::DebugView::Normal});
                    break;
                case SDLK_3:
                    services_.events.publish(DebugViewSelectedEvent{render::DebugView::RoughMetal});
                    break;
                case SDLK_4:
                    services_.events.publish(DebugViewSelectedEvent{render::DebugView::Depth});
                    break;
                case SDLK_5:
                    services_.events.publish(DebugViewSelectedEvent{render::DebugView::Light});
                    break;
                case SDLK_6:
                    services_.events.publish(DebugViewSelectedEvent{render::DebugView::ShadowMap});
                    break;
                case SDLK_7:
                    services_.events.publish(DebugViewSelectedEvent{render::DebugView::ShadowFactor});
                    break;
                case SDLK_8:
                    services_.events.publish(DebugViewSelectedEvent{render::DebugView::ShadowCascade});
                    break;
                case SDLK_LEFTBRACKET:
                    services_.events.publish(ShadowCascadeStepEvent{-1});
                    break;
                case SDLK_RIGHTBRACKET:
                    services_.events.publish(ShadowCascadeStepEvent{1});
                    break;
                case SDLK_c:
                    if (event.key.repeat == 0) {
                        services_.events.publish(ToggleFreeCameraEvent{});
                    }
                    break;
                default:
                    break;
            }
            break;
        }
        case SDL_MOUSEWHEEL:
            services_.events.publish(ViewportWheelEvent{event.wheel.y});
            break;
        case SDL_MOUSEBUTTONDOWN:
            if (event.button.button == SDL_BUTTON_LEFT) {
                services_.events.publish(ViewportClickedEvent{event.button.x, event.button.y});
            }
            if (event.button.button == SDL_BUTTON_MIDDLE) {
                services_.input.middleDragging = true;
                services_.input.lastMouseX = event.button.x;
                services_.input.lastMouseY = event.button.y;
            }
            if (event.button.button == SDL_BUTTON_RIGHT &&
                services_.camera.freeCameraEnabled &&
                !services_.renderer.wantsMouse()) {
                services_.input.rightMouseLooking = true;
                SDL_SetRelativeMouseMode(SDL_TRUE);
            }
            break;
        case SDL_MOUSEBUTTONUP:
            if (event.button.button == SDL_BUTTON_MIDDLE) {
                services_.input.middleDragging = false;
            }
            if (event.button.button == SDL_BUTTON_RIGHT) {
                releaseFreeCameraMouse();
            }
            break;
        case SDL_MOUSEMOTION:
            if (services_.input.rightMouseLooking &&
                services_.camera.freeCameraEnabled) {
                rotateFreeCamera(
                    services_.camera,
                    static_cast<float>(event.motion.xrel),
                    static_cast<float>(event.motion.yrel)
                );
            } else if (services_.input.middleDragging) {
                services_.events.publish(ViewportPanEvent{event.motion.xrel, event.motion.yrel});
                services_.input.lastMouseX = event.motion.x;
                services_.input.lastMouseY = event.motion.y;
            }
            break;
        case SDL_WINDOWEVENT:
            if (event.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
                services_.events.publish(WindowResizedEvent{event.window.data1, event.window.data2});
            }
            if (event.window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
                services_.input.middleDragging = false;
                releaseFreeCameraMouse();
            }
            break;
        default:
            break;
    }
}

void Application::updateFreeCameraControls() {
    if (!services_.camera.freeCameraEnabled ||
        services_.renderer.wantsKeyboard()) {
        return;
    }
    const Uint8* keys = SDL_GetKeyboardState(nullptr);
    const FreeCameraInput input{
        keys[SDL_SCANCODE_W] != 0,
        keys[SDL_SCANCODE_S] != 0,
        keys[SDL_SCANCODE_A] != 0,
        keys[SDL_SCANCODE_D] != 0,
        keys[SDL_SCANCODE_Q] != 0,
        keys[SDL_SCANCODE_E] != 0,
        keys[SDL_SCANCODE_LSHIFT] != 0 ||
            keys[SDL_SCANCODE_RSHIFT] != 0,
    };
    TimeContext frameTime = services_.time;
    frameTime.deltaSeconds = std::min(services_.time.frameDeltaSeconds, 0.1f);
    updateFreeCamera(services_.camera, input, frameTime);
}

void Application::releaseFreeCameraMouse() {
    if (!services_.input.rightMouseLooking) {
        return;
    }
    services_.input.rightMouseLooking = false;
    SDL_SetRelativeMouseMode(SDL_FALSE);
}

}  // namespace core
