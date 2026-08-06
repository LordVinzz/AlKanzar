#include "Application.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <iterator>
#include <string_view>

#include "RuntimePolicy.hpp"
#include "SimulationClock.hpp"
#include "core/editor/EditorSessionImGuiSettings.hpp"
#include "core/scene/Camera.hpp"
#include <spdlog/spdlog.h>

namespace core {

namespace {

constexpr double kMaxFrameRateHz = 600.0;
constexpr double kTargetFrameSeconds = 1.0 / kMaxFrameRateHz;

struct FrameDiagnosticsConfig {
    bool disableProfilerFrame{false};
    bool disableSchedulerProfiling{false};
    bool logFrameStages{false};
    std::uint64_t logFrameStageLimit{8u};

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

FrameDiagnosticsConfig loadFrameDiagnosticsConfig() {
    FrameDiagnosticsConfig config{};
    config.disableProfilerFrame = readBoolEnv("ALKANZAR_DISABLE_PROFILER_FRAME");
    config.disableSchedulerProfiling = readBoolEnv("ALKANZAR_DISABLE_SCHEDULER_PROFILING");
    config.logFrameStages = readBoolEnv("ALKANZAR_LOG_FRAME_STAGES");
    config.logFrameStageLimit = readUintEnv("ALKANZAR_LOG_FRAME_STAGE_LIMIT", 8u);
    return config;
}

RuntimePolicyFrameContext buildRuntimePolicyFrameContext(const EngineServices& services) {
    return RuntimePolicyFrameContext{
        services.scheduler.workerCount(),
        services.world.transformsDirty(),
        services.world.lightsDirty(),
        services.world.transforms.size(),
        services.world.bounds.size(),
        services.world.renderables.size(),
        services.world.parents.size(),
        services.world.pointLights.size(),
        services.world.spotLights.size(),
        services.world.lightVolumes.size(),
    };
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

double secondsBetween(Uint64 startCounter, Uint64 endCounter, Uint64 frequency) {
    if (endCounter <= startCounter) {
        return 0.0;
    }

    return static_cast<double>(endCounter - startCounter) / static_cast<double>(frequency);
}

void throttleFrameRate(Uint64 frameStartCounter, Uint64 performanceFrequency) {
    double elapsedSeconds = secondsBetween(frameStartCounter, SDL_GetPerformanceCounter(), performanceFrequency);
    if (elapsedSeconds >= kTargetFrameSeconds) {
        return;
    }

    const double remainingSeconds = kTargetFrameSeconds - elapsedSeconds;
    const Uint32 remainingMilliseconds = static_cast<Uint32>(remainingSeconds * 1000.0);
    if (remainingMilliseconds > 1u) {
        SDL_Delay(remainingMilliseconds - 1u);
    }

    while ((elapsedSeconds = secondsBetween(frameStartCounter, SDL_GetPerformanceCounter(), performanceFrequency)) <
           kTargetFrameSeconds) {
    }
}

}  // namespace

Application::Application(int width, int height, AppMode startupMode)
    : services_(width, height, startupMode) {
    bindEventHandlers();
}

void Application::run() {
    transitionTo(AppMode::Bootstrap);
    const FrameDiagnosticsConfig diagnostics = loadFrameDiagnosticsConfig();
    RuntimePolicy runtimePolicy;
    if (diagnostics.disableSchedulerProfiling) {
        services_.scheduler.setProfiler(nullptr);
    }
    spdlog::info(
        "Application: frame diagnostics "
        "(runtime_policy=engine-managed, profiler_frame={}, scheduler_profiling={}, frame_logs={}, log_limit={})",
        diagnostics.disableProfilerFrame ? "disabled" : "enabled",
        diagnostics.disableSchedulerProfiling ? "disabled" : "enabled",
        diagnostics.logFrameStages,
        diagnostics.logFrameStageLimit
    );

    const Uint64 performanceFrequency = SDL_GetPerformanceFrequency();
    const Uint64 startupCounter = SDL_GetPerformanceCounter();
    Uint64 previousCounter = startupCounter;
    SimulationClock simulationClock;
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

        {
            ALKANZAR_PROFILE_SCOPE(services_.profiler, "Event Dispatch");
            services_.events.dispatch();
        }
        if (services_.requestedMode.has_value()) {
            transitionTo(*services_.requestedMode);
            services_.requestedMode.reset();
        }
        const AppModeCapabilities& capabilities = modeSession_.capabilities();

        const Uint64 currentCounter = SDL_GetPerformanceCounter();
        const double frameSeconds = secondsBetween(previousCounter, currentCounter, performanceFrequency);
        previousCounter = currentCounter;
        services_.time.frameDeltaSeconds = static_cast<float>(simulationClock.advance(
            frameSeconds,
            services_.time.paused,
            services_.time.timeScale
        ));
        services_.time.interpolationAlpha = static_cast<float>(simulationClock.interpolationAlpha());
        const RuntimePolicyDecision* runtimeDecision = &runtimePolicy.decision();

        if (diagnostics.shouldLogFrameStage(frameIndex)) {
            logFrameStageBoundary("begin", "Begin ImGui Frame", frameIndex, "main");
        }
        services_.renderer.beginImGuiFrame();
        if (diagnostics.shouldLogFrameStage(frameIndex)) {
            logFrameStageBoundary("end", "Begin ImGui Frame", frameIndex, "main");
        }
        updateFreeCameraControls();
        while (simulationClock.consumeStep()) {
            services_.time.deltaSeconds = static_cast<float>(simulationClock.fixedStepSeconds());
            services_.time.totalSeconds += services_.time.deltaSeconds;
            services_.time.simulationTick = simulationClock.tickCount();
            runtimeDecision = &runtimePolicy.evaluate(buildRuntimePolicyFrameContext(services_));

            if (capabilities.runs(AppRuntimeSystem::StateUpdate) && currentState_ != nullptr) {
                ALKANZAR_PROFILE_SCOPE(services_.profiler, "State Update");
                if (diagnostics.shouldLogFrameStage(frameIndex)) {
                    logFrameStageBoundary("begin", "State Update", frameIndex, "main");
                }
                currentState_->update(services_);
                if (diagnostics.shouldLogFrameStage(frameIndex)) {
                    logFrameStageBoundary("end", "State Update", frameIndex, "main");
                }
            }

            if (capabilities.runs(AppRuntimeSystem::Navigation)) {
                {
                    ALKANZAR_PROFILE_SCOPE(services_.profiler, "Navigation Path Apply");
                    services_.navigationSystem.applyCompletedPathRequests(services_.world, services_.navigation);
                }
                {
                    ALKANZAR_PROFILE_SCOPE(services_.profiler, "Navigation Update");
                    services_.navigationSystem.updateAgents(services_.world, services_.navigation, services_.time);
                }
            }

            if (capabilities.runs(AppRuntimeSystem::Animation)) {
                ALKANZAR_PROFILE_SCOPE(services_.profiler, "Animation Update");
                if (diagnostics.shouldLogFrameStage(frameIndex)) {
                    logFrameStageBoundary("begin", "Animation Update", frameIndex, "main");
                }
                services_.animationSystem.update(services_.world, services_.time, services_.scheduler, true);
                if (diagnostics.shouldLogFrameStage(frameIndex)) {
                    logFrameStageBoundary("end", "Animation Update", frameIndex, "main");
                }
            }

            if (capabilities.runs(AppRuntimeSystem::Physics)) {
                ALKANZAR_PROFILE_SCOPE(services_.profiler, "Physics Update");
                services_.physicsSystem.update(services_.world, services_.time, services_.scheduler, true);
            }

            if (capabilities.runs(AppRuntimeSystem::Transforms)) {
                ALKANZAR_PROFILE_SCOPE(services_.profiler, "Transform Update");
                if (diagnostics.shouldLogFrameStage(frameIndex)) {
                    logFrameStageBoundary(
                        "begin",
                        "Transform Update",
                        frameIndex,
                        runtimeDecision->parallelTransformUpdate ? "parallel" : "sequential"
                    );
                }
                services_.transformSystem.update(
                    services_.world,
                    services_.scheduler,
                    runtimeDecision->parallelTransformUpdate
                );
                if (diagnostics.shouldLogFrameStage(frameIndex)) {
                    logFrameStageBoundary(
                        "end",
                        "Transform Update",
                        frameIndex,
                        runtimeDecision->parallelTransformUpdate ? "parallel" : "sequential"
                    );
                }
            }

            if (capabilities.runs(AppRuntimeSystem::Lighting)) {
                ALKANZAR_PROFILE_SCOPE(services_.profiler, "Light Update");
                if (diagnostics.shouldLogFrameStage(frameIndex)) {
                    logFrameStageBoundary(
                        "begin",
                        "Light Update",
                        frameIndex,
                        runtimeDecision->parallelLightUpdate ? "parallel" : "sequential"
                    );
                }
                services_.lightSystem.update(
                    services_.world,
                    services_.time,
                    services_.scheduler,
                    runtimeDecision->parallelLightUpdate
                );
                if (diagnostics.shouldLogFrameStage(frameIndex)) {
                    logFrameStageBoundary(
                        "end",
                        "Light Update",
                        frameIndex,
                        runtimeDecision->parallelLightUpdate ? "parallel" : "sequential"
                    );
                }
            }

            if (capabilities.runs(AppRuntimeSystem::RenderExtraction)) {
                ALKANZAR_PROFILE_SCOPE(services_.profiler, "Render Extraction");
                if (diagnostics.shouldLogFrameStage(frameIndex)) {
                    logFrameStageBoundary(
                        "begin",
                        "Render Extraction",
                        frameIndex,
                        runtimeDecision->parallelRenderExtraction ? "parallel" : "sequential"
                    );
                }
                services_.renderExtractionSystem.extract(
                    services_.world,
                    capabilities.usesEditorSelection ? &services_.editorSelection : nullptr,
                    services_.frame,
                    services_.scheduler,
                    runtimeDecision->parallelRenderExtraction
                );
                if (diagnostics.shouldLogFrameStage(frameIndex)) {
                    logFrameStageBoundary(
                        "end",
                        "Render Extraction",
                        frameIndex,
                        runtimeDecision->parallelRenderExtraction ? "parallel" : "sequential"
                    );
                }
            }
        }
        services_.time.interpolationAlpha = static_cast<float>(simulationClock.interpolationAlpha());

        if (capabilities.rendersEditorUi && currentState_ != nullptr) {
            if (diagnostics.shouldLogFrameStage(frameIndex)) {
                logFrameStageBoundary("begin", "State UI", frameIndex, "main");
            }
            currentState_->renderUi(services_);
            if (diagnostics.shouldLogFrameStage(frameIndex)) {
                logFrameStageBoundary("end", "State UI", frameIndex, "main");
            }
        }

        if (capabilities.syncsNavigationDebug) {
            services_.navigationSystem.syncFrame(services_.world, services_.navigation, services_.frame);
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
            capabilities.showsEditorOverlays,
            capabilities.showsEditorOverlays && services_.editorSession.navMeshOverlayVisible,
            capabilities.showsEditorOverlays && services_.editorSession.navMeshPolygonWireframeVisible,
        };
        if (capabilities.rendersWorld) {
            ALKANZAR_PROFILE_SCOPE(services_.profiler, "Render Frame");
            if (diagnostics.shouldLogFrameStage(frameIndex)) {
                logFrameStageBoundary(
                    "begin",
                    "Render Frame",
                    frameIndex,
                    runtimeDecision->parallelSceneView ? "parallel" : "sequential"
                );
            }
            services_.renderer.renderFrame(
                services_.frame,
                camera,
                renderOptions,
                services_.scheduler,
                runtimeDecision->parallelSceneView
            );
            if (diagnostics.shouldLogFrameStage(frameIndex)) {
                logFrameStageBoundary(
                    "end",
                    "Render Frame",
                    frameIndex,
                    runtimeDecision->parallelSceneView ? "parallel" : "sequential"
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
        std::vector<render::FrameCounterRecord> profilingCounters =
            diagnostics.disableProfilerFrame ? std::vector<render::FrameCounterRecord>{}
                                             : services_.renderer.profilingCounters();
        if (!diagnostics.disableProfilerFrame) {
            std::vector<render::FrameCounterRecord> runtimeCounters = runtimePolicy.profilingCounters();
            profilingCounters.insert(
                profilingCounters.end(),
                std::make_move_iterator(runtimeCounters.begin()),
                std::make_move_iterator(runtimeCounters.end())
            );
            std::vector<render::FrameCounterRecord> navigationCounters = services_.navigationSystem.profilingCounters();
            profilingCounters.insert(
                profilingCounters.end(),
                std::make_move_iterator(navigationCounters.begin()),
                std::make_move_iterator(navigationCounters.end())
            );
        }
        if (diagnostics.shouldLogFrameStage(frameIndex)) {
            logFrameStageBoundary("end", "Collect Profiling Resources", frameIndex, diagnostics.disableProfilerFrame ? "disabled" : "enabled");
        }
        if (diagnostics.shouldLogFrameStage(frameIndex)) {
            logFrameStageBoundary("begin", "Profiler End Frame", frameIndex, diagnostics.disableProfilerFrame ? "disabled" : "enabled");
        }
        if (!diagnostics.disableProfilerFrame) {
            services_.profiler.endFrame(profilingResources, profilingCounters);
        }
        if (diagnostics.shouldLogFrameStage(frameIndex)) {
            logFrameStageBoundary("end", "Profiler End Frame", frameIndex, diagnostics.disableProfilerFrame ? "disabled" : "enabled");
        }

        if (services_.requestedMode.has_value()) {
            transitionTo(*services_.requestedMode);
            services_.requestedMode.reset();
        }

        throttleFrameRate(currentCounter, performanceFrequency);
        ++frameIndex;
    }
}

void Application::transitionTo(AppMode mode) {
    if (currentState_ != nullptr && modeSession_.current() == mode) {
        return;
    }

    if (currentState_ != nullptr) {
        currentState_->onExit(services_);
    }

    if (!appModeCapabilities(mode).acceptsCameraInput) {
        releaseFreeCameraMouse();
        setFreeCameraEnabled(services_.camera, false);
    }

    modeSession_.transitionTo(mode);
    currentState_ = &states_.forMode(mode);
    spdlog::info("Application: entering {} mode", appModeName(mode));
    currentState_->onEnter(services_);
}

}  // namespace core
