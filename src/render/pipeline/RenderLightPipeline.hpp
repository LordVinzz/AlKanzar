#ifndef GL_GLEXT_PROTOTYPES
#define GL_GLEXT_PROTOTYPES
#endif
#pragma once

#include <SDL_opengl.h>

#include <vector>

#include <glm/vec4.hpp>

#include "render/resources/Profiling.hpp"
#include "render/engine/RenderSceneView.hpp"

namespace render {

struct ActiveLightDebug {
    glm::vec3 position{0.0f};
    float radius{0.0f};
    glm::vec3 color{1.0f};
    float outerAngle{0.0f};
    glm::vec3 direction{0.0f, 0.0f, 1.0f};
    LightType type{LightType::Point};
};

struct GpuLight {
    glm::vec4 positionRadius;
    glm::vec4 colorIntensity;
    glm::vec4 directionType;
    glm::vec4 spotParams;
    glm::vec4 shadowInfo;
};

struct ActiveLightSelection {
    std::vector<int> indices{};
    int pointLightCount{0};
    int spotLightCount{0};
};

ActiveLightSelection selectActiveLights(
    const std::vector<core::FrameLight>& lights,
    const std::vector<core::FrameLightVolume>& lightVolumes
);

class RenderLightPipeline {
public:
    struct DeferredBuffers {
        GLuint buffer{0};
        GLuint texture{0};
        GLsizeiptr size{0};
    };

    struct FrameState {
        std::vector<int> activeLightIndices{};
        std::vector<ActiveLightDebug> debugLights{};
        std::vector<GpuLight> gpuLights{};
        int lightCount{0};
        int pointLightCount{0};
        int spotLightCount{0};
        bool cameraInsideLightVolume{false};
    };

    [[nodiscard]] FrameState buildFrame(
        const RenderSceneView& scene,
        const glm::mat4& view,
        ShadowSystem& shadowSystem,
        bool deferred
    ) const;

    void uploadDeferredLights(const FrameState& frame, DeferredBuffers& buffers) const;
    [[nodiscard]] std::vector<ResourceMemoryRecord> profilingResources(const DeferredBuffers& buffers) const;
};

}  // namespace render
