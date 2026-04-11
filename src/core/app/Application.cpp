#include "Application.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <string_view>

#include "core/editor/EditorSessionImGuiSettings.hpp"
#include "core/scene/Camera.hpp"
#include <spdlog/spdlog.h>

namespace core {

namespace {

struct FrameStageDiagnosticsConfig {
    bool enableParallelTransformUpdate{false};
    bool forceSequentialTransformUpdate{false};
    bool enableParallelLightUpdate{false};
    bool forceSequentialLightUpdate{false};
    bool forceSequentialRenderExtraction{false};
    bool enableParallelRenderExtraction{false};
    bool enableParallelSceneView{false};
    bool forceSequentialSceneView{false};
    bool disableProfilerFrame{false};
    bool disableSchedulerProfiling{false};
    bool logFrameStages{false};
    std::uint64_t logFrameStageLimit{8u};

    [[nodiscard]] bool hasRuntimeOverrides() const {
        return enableParallelTransformUpdate ||
            forceSequentialTransformUpdate ||
            enableParallelLightUpdate ||
            forceSequentialLightUpdate ||
            forceSequentialRenderExtraction ||
            enableParallelRenderExtraction ||
            enableParallelSceneView ||
            forceSequentialSceneView ||
            disableProfilerFrame ||
            disableSchedulerProfiling;
    }

    [[nodiscard]] bool shouldLogFrameStage(std::uint64_t frameIndex) const {
        return logFrameStages && frameIndex < logFrameStageLimit;
    }
};

bool readBoolEnv(const char* name) {
    const char* value = std::getenv(name);
    if (value == nullptr) {
        return false;
    }

    const std::string_view token(value);
    return !token.empty() &&
        token != "0" &&
        token != "false" &&
        token != "FALSE" &&
        token != "off" &&
        token != "OFF" &&
        token != "no" &&
        token != "NO";
}

std::uint64_t readUintEnv(const char* name, std::uint64_t fallback) {
    const char* value = std::getenv(name);
    if (value == nullptr || *value == '\0') {
        return fallback;
    }

    char* end = nullptr;
    const unsigned long long parsed = std::strtoull(value, &end, 10);
    if (end == value || (end != nullptr && *end != '\0')) {
        return fallback;
    }

    return static_cast<std::uint64_t>(parsed);
}

FrameStageDiagnosticsConfig loadFrameStageDiagnosticsConfig() {
    FrameStageDiagnosticsConfig config{};
    config.enableParallelTransformUpdate = readBoolEnv("ALKANZAR_ENABLE_PARALLEL_TRANSFORM_UPDATE");
    config.forceSequentialTransformUpdate = readBoolEnv("ALKANZAR_FORCE_SEQUENTIAL_TRANSFORM_UPDATE");
    config.enableParallelLightUpdate = readBoolEnv("ALKANZAR_ENABLE_PARALLEL_LIGHT_UPDATE");
    config.forceSequentialLightUpdate = readBoolEnv("ALKANZAR_FORCE_SEQUENTIAL_LIGHT_UPDATE");
    config.forceSequentialRenderExtraction = readBoolEnv("ALKANZAR_FORCE_SEQUENTIAL_RENDER_EXTRACTION");
    config.enableParallelRenderExtraction = readBoolEnv("ALKANZAR_ENABLE_PARALLEL_RENDER_EXTRACTION");
    config.enableParallelSceneView = readBoolEnv("ALKANZAR_ENABLE_PARALLEL_SCENE_VIEW");
    config.forceSequentialSceneView = readBoolEnv("ALKANZAR_FORCE_SEQUENTIAL_SCENE_VIEW");
    config.disableProfilerFrame = readBoolEnv("ALKANZAR_DISABLE_PROFILER_FRAME");
    config.disableSchedulerProfiling = readBoolEnv("ALKANZAR_DISABLE_SCHEDULER_PROFILING");
    config.logFrameStages = readBoolEnv("ALKANZAR_LOG_FRAME_STAGES");
    config.logFrameStageLimit = readUintEnv("ALKANZAR_LOG_FRAME_STAGE_LIMIT", 8u);
    return config;
}

void logFrameStageBoundary(
    std::string_view boundary,
    std::string_view stage,
    std::uint64_t frameIndex,
    std::string_view mode
) {
    spdlog::info(
        "Application: frame {} {} {} [{}]",
        frameIndex,
        boundary,
        stage,
        mode
    );
}

void openEditorMainWindow(EditorSession& session) {
    session.openMainWindow();
}

}  // namespace

Application::Application(int width, int height)
    : services_(width, height) {
    bindEventHandlers();
}

void Application::run() {
    transitionTo(AppMode::Bootstrap);
    const FrameStageDiagnosticsConfig diagnostics = loadFrameStageDiagnosticsConfig();
    const bool useParallelTransformUpdate =
        diagnostics.enableParallelTransformUpdate && !diagnostics.forceSequentialTransformUpdate;
    const bool useParallelLightUpdate =
        diagnostics.enableParallelLightUpdate && !diagnostics.forceSequentialLightUpdate;
    const bool useParallelRenderExtraction =
        diagnostics.enableParallelRenderExtraction && !diagnostics.forceSequentialRenderExtraction;
    const bool useParallelSceneView =
        diagnostics.enableParallelSceneView && !diagnostics.forceSequentialSceneView;
    if (diagnostics.disableSchedulerProfiling) {
        services_.scheduler.setProfiler(nullptr);
    }
    spdlog::info(
        "Application: frame diagnostics "
        "(transform={}, light={}, extraction={}, scene_view={}, profiler_frame={}, scheduler_profiling={}, frame_logs={}, log_limit={})",
        useParallelTransformUpdate ? "parallel" : "sequential",
        useParallelLightUpdate ? "parallel" : "sequential",
        useParallelRenderExtraction ? "parallel" : "sequential",
        useParallelSceneView ? "parallel" : "sequential",
        diagnostics.disableProfilerFrame ? "disabled" : "enabled",
        diagnostics.disableSchedulerProfiling ? "disabled" : "enabled",
        diagnostics.logFrameStages,
        diagnostics.logFrameStageLimit
    );

    Uint32 previousTicks = SDL_GetTicks();
    std::uint64_t frameIndex = 0u;
    while (services_.running) {
        if (!diagnostics.disableProfilerFrame) {
            services_.profiler.beginFrame();
        }

        {
            ALKANZAR_PROFILE_SCOPE(services_.profiler, "Event Pump");
            if (diagnostics.shouldLogFrameStage(frameIndex)) {
                logFrameStageBoundary("begin", "Event Pump", frameIndex, "main");
            }
            SDL_Event event;
            while (SDL_PollEvent(&event) != 0) {
                services_.renderer.processEvent(event);
                translateSdlEvent(event);
            }
            if (diagnostics.shouldLogFrameStage(frameIndex)) {
                logFrameStageBoundary("end", "Event Pump", frameIndex, "main");
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

        if (diagnostics.shouldLogFrameStage(frameIndex)) {
            logFrameStageBoundary("begin", "Begin ImGui Frame", frameIndex, "main");
        }
        services_.renderer.beginImGuiFrame();
        if (diagnostics.shouldLogFrameStage(frameIndex)) {
            logFrameStageBoundary("end", "Begin ImGui Frame", frameIndex, "main");
        }
        if (currentState_ != nullptr) {
            ALKANZAR_PROFILE_SCOPE(services_.profiler, "State Update");
            if (diagnostics.shouldLogFrameStage(frameIndex)) {
                logFrameStageBoundary("begin", "State Update", frameIndex, "main");
            }
            currentState_->update(services_);
            if (diagnostics.shouldLogFrameStage(frameIndex)) {
                logFrameStageBoundary("end", "State Update", frameIndex, "main");
            }
        }

        {
            ALKANZAR_PROFILE_SCOPE(services_.profiler, "Animation Update");
            if (diagnostics.shouldLogFrameStage(frameIndex)) {
                logFrameStageBoundary("begin", "Animation Update", frameIndex, "main");
            }
            services_.animationSystem.update(services_.world, services_.time, services_.scheduler, true);
            if (diagnostics.shouldLogFrameStage(frameIndex)) {
                logFrameStageBoundary("end", "Animation Update", frameIndex, "main");
            }
        }
        {
            ALKANZAR_PROFILE_SCOPE(services_.profiler, "Transform Update");
            if (diagnostics.shouldLogFrameStage(frameIndex)) {
                logFrameStageBoundary(
                    "begin",
                    "Transform Update",
                    frameIndex,
                    useParallelTransformUpdate ? "parallel" : "sequential"
                );
            }
            services_.transformSystem.update(services_.world, services_.scheduler, useParallelTransformUpdate);
            if (diagnostics.shouldLogFrameStage(frameIndex)) {
                logFrameStageBoundary(
                    "end",
                    "Transform Update",
                    frameIndex,
                    useParallelTransformUpdate ? "parallel" : "sequential"
                );
            }
        }
        {
            ALKANZAR_PROFILE_SCOPE(services_.profiler, "Light Update");
            if (diagnostics.shouldLogFrameStage(frameIndex)) {
                logFrameStageBoundary(
                    "begin",
                    "Light Update",
                    frameIndex,
                    useParallelLightUpdate ? "parallel" : "sequential"
                );
            }
            services_.lightSystem.update(
                services_.world,
                services_.time,
                services_.scheduler,
                useParallelLightUpdate
            );
            if (diagnostics.shouldLogFrameStage(frameIndex)) {
                logFrameStageBoundary(
                    "end",
                    "Light Update",
                    frameIndex,
                    useParallelLightUpdate ? "parallel" : "sequential"
                );
            }
        }
        {
            ALKANZAR_PROFILE_SCOPE(services_.profiler, "Render Extraction");
            if (diagnostics.shouldLogFrameStage(frameIndex)) {
                logFrameStageBoundary(
                    "begin",
                    "Render Extraction",
                    frameIndex,
                    useParallelRenderExtraction ? "parallel" : "sequential"
                );
            }
            services_.renderExtractionSystem.extract(
                services_.world,
                services_.materials,
                services_.selection,
                services_.frame,
                services_.scheduler,
                useParallelRenderExtraction
            );
            if (diagnostics.shouldLogFrameStage(frameIndex)) {
                logFrameStageBoundary(
                    "end",
                    "Render Extraction",
                    frameIndex,
                    useParallelRenderExtraction ? "parallel" : "sequential"
                );
            }
        }

        if (currentState_ != nullptr) {
            if (diagnostics.shouldLogFrameStage(frameIndex)) {
                logFrameStageBoundary("begin", "State UI", frameIndex, "main");
            }
            currentState_->renderUi(services_);
            if (diagnostics.shouldLogFrameStage(frameIndex)) {
                logFrameStageBoundary("end", "State UI", frameIndex, "main");
            }
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
            currentMode_ == AppMode::Editor,
        };
        {
            ALKANZAR_PROFILE_SCOPE(services_.profiler, "Render Frame");
            if (diagnostics.shouldLogFrameStage(frameIndex)) {
                logFrameStageBoundary(
                    "begin",
                    "Render Frame",
                    frameIndex,
                    useParallelSceneView ? "parallel" : "sequential"
                );
            }
            services_.renderer.renderFrame(
                services_.frame,
                camera,
                renderOptions,
                services_.scheduler,
                useParallelSceneView
            );
            if (diagnostics.shouldLogFrameStage(frameIndex)) {
                logFrameStageBoundary(
                    "end",
                    "Render Frame",
                    frameIndex,
                    useParallelSceneView ? "parallel" : "sequential"
                );
            }
        }
        {
            ALKANZAR_PROFILE_SCOPE(services_.profiler, "ImGui Render");
            if (diagnostics.shouldLogFrameStage(frameIndex)) {
                logFrameStageBoundary("begin", "ImGui Render", frameIndex, "main");
            }
            services_.renderer.renderImGui();
            if (diagnostics.shouldLogFrameStage(frameIndex)) {
                logFrameStageBoundary("end", "ImGui Render", frameIndex, "main");
            }
        }
        {
            ALKANZAR_PROFILE_SCOPE(services_.profiler, "Present");
            if (diagnostics.shouldLogFrameStage(frameIndex)) {
                logFrameStageBoundary("begin", "Present", frameIndex, "main");
            }
            services_.renderer.present();
            if (diagnostics.shouldLogFrameStage(frameIndex)) {
                logFrameStageBoundary("end", "Present", frameIndex, "main");
            }
        }
        if (diagnostics.shouldLogFrameStage(frameIndex)) {
            logFrameStageBoundary("begin", "Collect Profiling Resources", frameIndex, diagnostics.disableProfilerFrame ? "disabled" : "enabled");
        }
        const std::vector<render::ResourceMemoryRecord> profilingResources =
            diagnostics.disableProfilerFrame ? std::vector<render::ResourceMemoryRecord>{}
                                             : services_.renderer.profilingResources();
        if (diagnostics.shouldLogFrameStage(frameIndex)) {
            logFrameStageBoundary("end", "Collect Profiling Resources", frameIndex, diagnostics.disableProfilerFrame ? "disabled" : "enabled");
        }
        if (diagnostics.shouldLogFrameStage(frameIndex)) {
            logFrameStageBoundary("begin", "Profiler End Frame", frameIndex, diagnostics.disableProfilerFrame ? "disabled" : "enabled");
        }
        if (!diagnostics.disableProfilerFrame) {
            services_.profiler.endFrame(profilingResources);
        }
        if (diagnostics.shouldLogFrameStage(frameIndex)) {
            logFrameStageBoundary("end", "Profiler End Frame", frameIndex, diagnostics.disableProfilerFrame ? "disabled" : "enabled");
        }

        if (services_.requestedMode.has_value()) {
            transitionTo(*services_.requestedMode);
            services_.requestedMode.reset();
        }

        ++frameIndex;
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
            services_.editorSession.suspendEditorUi();
        } else {
            openEditorMainWindow(services_.editorSession);
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
