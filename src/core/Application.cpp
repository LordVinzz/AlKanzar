#include "Application.hpp"

#include <algorithm>

#include "Camera.hpp"

namespace core {

Application::Application(int width, int height)
    : services_(width, height) {
    bindEventHandlers();
}

void Application::run() {
    transitionTo(AppMode::Bootstrap);

    Uint32 previousTicks = SDL_GetTicks();
    while (services_.running) {
        services_.profiler.beginFrame();

        {
            ALKANZAR_PROFILE_SCOPE(services_.profiler, "Event Pump");
            SDL_Event event;
            while (SDL_PollEvent(&event) != 0) {
                services_.renderer.processEvent(event);
                translateSdlEvent(event);
            }
        }

        services_.events.dispatch();
        if (services_.requestedMode.has_value()) {
            transitionTo(*services_.requestedMode);
            services_.requestedMode.reset();
        }

        const Uint32 currentTicks = SDL_GetTicks();
        services_.time.deltaSeconds = static_cast<float>(currentTicks - previousTicks) * 0.001f;
        services_.time.totalSeconds = static_cast<float>(currentTicks) * 0.001f;
        previousTicks = currentTicks;

        services_.renderer.beginImGuiFrame();
        if (currentState_ != nullptr) {
            ALKANZAR_PROFILE_SCOPE(services_.profiler, "State Update");
            currentState_->update(services_);
        }

        {
            ALKANZAR_PROFILE_SCOPE(services_.profiler, "Transform Update");
            services_.transformSystem.update(services_.world);
        }
        {
            ALKANZAR_PROFILE_SCOPE(services_.profiler, "Light Update");
            services_.lightSystem.update(services_.world, services_.time);
        }
        {
            ALKANZAR_PROFILE_SCOPE(services_.profiler, "Render Extraction");
            services_.renderExtractionSystem.extract(
                services_.world,
                services_.materials,
                services_.selection,
                services_.frame
            );
        }

        if (currentState_ != nullptr) {
            currentState_->renderUi(services_);
        }

        const render::CameraMatrices camera = computeCameraMatrices(
            services_.camera,
            services_.renderer.width(),
            services_.renderer.height()
        );
        const render::RenderFrameOptions renderOptions{
            services_.debugView,
            services_.shadowDebugCascade,
            services_.showLightDebug,
            currentMode_ == AppMode::Editor && services_.editorSession.sceneHierarchyVisible,
        };
        {
            ALKANZAR_PROFILE_SCOPE(services_.profiler, "Render Frame");
            services_.renderer.renderFrame(
                services_.frame,
                camera,
                renderOptions
            );
        }
        {
            ALKANZAR_PROFILE_SCOPE(services_.profiler, "ImGui Render");
            services_.renderer.renderImGui();
        }
        {
            ALKANZAR_PROFILE_SCOPE(services_.profiler, "Present");
            services_.renderer.present();
        }
        services_.profiler.endFrame(services_.renderer.profilingResources());

        if (services_.requestedMode.has_value()) {
            transitionTo(*services_.requestedMode);
            services_.requestedMode.reset();
        }
    }
}

void Application::transitionTo(AppMode mode) {
    if (currentState_ != nullptr && currentMode_ == mode) {
        return;
    }

    if (currentState_ != nullptr) {
        currentState_->onExit(services_);
    }

    currentMode_ = mode;
    switch (mode) {
        case AppMode::Bootstrap:
            currentState_ = &bootstrapState_;
            break;
        case AppMode::Gameplay:
            currentState_ = &gameplayState_;
            break;
        case AppMode::Editor:
            currentState_ = &editorState_;
            break;
        case AppMode::Shutdown:
        default:
            currentState_ = &shutdownState_;
            break;
    }

    if (currentState_ != nullptr) {
        currentState_->onEnter(services_);
    }
}

void Application::bindEventHandlers() {
    services_.selection.changed().connect([this](const std::optional<EntityId>& selection) {
        services_.events.publish(SelectionChangedEvent{selection});
    });

    services_.events.subscribe<QuitRequestedEvent>([this](const QuitRequestedEvent&) {
        services_.requestedMode = AppMode::Shutdown;
    });
    services_.events.subscribe<ToggleEditorEvent>([this](const ToggleEditorEvent&) {
        if (currentMode_ == AppMode::Editor) {
            services_.editorSession.sceneHierarchyVisible = false;
            services_.editorSession.sceneHierarchyFocusRequested = false;
            services_.editorSession.profilerWindowVisible = false;
        } else {
            services_.editorSession.profilerWindowVisible = true;
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
        const float factor = event.delta > 0 ? 0.9f : 1.1f;
        services_.camera.zoom = std::clamp(services_.camera.zoom * factor, 0.2f, 5.0f);
    });
    services_.events.subscribe<ViewportPanEvent>([this](const ViewportPanEvent& event) {
        if (services_.renderer.wantsMouse() || services_.camera.orbitEnabled) {
            return;
        }
        constexpr float panSpeed = 0.01f;
        services_.camera.panX -= static_cast<float>(event.dx) * panSpeed / services_.camera.zoom;
        services_.camera.panY += static_cast<float>(event.dy) * panSpeed / services_.camera.zoom;
    });
    services_.events.subscribe<ToggleOrbitCameraEvent>([this](const ToggleOrbitCameraEvent&) {
        services_.camera.orbitEnabled = !services_.camera.orbitEnabled;
        if (services_.camera.orbitEnabled) {
            services_.camera.orbitYawDeg = 45.0f;
            services_.camera.panX = 0.0f;
            services_.camera.panY = 0.0f;
        }
    });
    services_.events.subscribe<ToggleLightDebugEvent>([this](const ToggleLightDebugEvent&) {
        services_.showLightDebug = !services_.showLightDebug;
    });
    services_.events.subscribe<DebugViewSelectedEvent>([this](const DebugViewSelectedEvent& event) {
        services_.debugView = event.view;
    });
    services_.events.subscribe<ShadowCascadeStepEvent>([this](const ShadowCascadeStepEvent& event) {
        services_.shadowDebugCascade = std::max(0, services_.shadowDebugCascade + event.delta);
    });
    services_.events.subscribe<ViewportClickedEvent>([this](const ViewportClickedEvent& event) {
        if (currentMode_ != AppMode::Editor || services_.renderer.wantsMouse()) {
            return;
        }
        const render::CameraMatrices camera = computeCameraMatrices(
            services_.camera,
            services_.renderer.width(),
            services_.renderer.height()
        );
        services_.selection.set(services_.pickingSystem.pick(
            services_.frame,
            camera,
            services_.renderer.width(),
            services_.renderer.height(),
            event.x,
            event.y,
            false
        ));
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
                if (currentMode_ != AppMode::Editor) {
                    services_.requestedMode = AppMode::Editor;
                    services_.editorSession.sceneHierarchyVisible = true;
                    services_.editorSession.profilerWindowVisible = true;
                } else {
                    services_.editorSession.sceneHierarchyVisible = !services_.editorSession.sceneHierarchyVisible;
                    services_.editorSession.profilerWindowVisible = services_.editorSession.sceneHierarchyVisible;
                }
                services_.editorSession.sceneHierarchyFocusRequested = services_.editorSession.sceneHierarchyVisible;
                break;
            }

            const SDL_Keymod modifiers = SDL_GetModState();
            const bool primaryModifier = (modifiers & KMOD_CTRL) != 0 || (modifiers & KMOD_GUI) != 0;
            if (primaryModifier && event.key.repeat == 0) {
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
                        services_.events.publish(ToggleOrbitCameraEvent{});
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
            if (event.button.button == SDL_BUTTON_LEFT && currentMode_ == AppMode::Editor) {
                services_.events.publish(ViewportClickedEvent{event.button.x, event.button.y});
            }
            if (event.button.button == SDL_BUTTON_MIDDLE) {
                services_.input.middleDragging = true;
                services_.input.lastMouseX = event.button.x;
                services_.input.lastMouseY = event.button.y;
            }
            break;
        case SDL_MOUSEBUTTONUP:
            if (event.button.button == SDL_BUTTON_MIDDLE) {
                services_.input.middleDragging = false;
            }
            break;
        case SDL_MOUSEMOTION:
            if (services_.input.middleDragging) {
                services_.events.publish(ViewportPanEvent{event.motion.xrel, event.motion.yrel});
                services_.input.lastMouseX = event.motion.x;
                services_.input.lastMouseY = event.motion.y;
            }
            break;
        case SDL_WINDOWEVENT:
            if (event.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
                services_.events.publish(WindowResizedEvent{event.window.data1, event.window.data2});
            }
            break;
        default:
            break;
    }
}

}  // namespace core
