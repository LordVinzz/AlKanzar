#include "RenderLightPipeline.hpp"

#include <algorithm>
#include <cmath>

#include <glm/geometric.hpp>
#include <glm/matrix.hpp>
#include <glm/trigonometric.hpp>

namespace render {

ActiveLightSelection selectActiveLights(
    const std::vector<core::FrameLight>& lights,
    const std::vector<core::FrameLightVolume>& lightVolumes
) {
    ActiveLightSelection selection{};
    if (lights.empty()) {
        return selection;
    }

    std::vector<bool> seen(lights.size(), false);
    auto appendLight = [&](int index) {
        if (index < 0 || index >= static_cast<int>(lights.size()) || seen[index]) {
            return;
        }
        seen[index] = true;
        selection.indices.push_back(index);
        if (lights[index].type == LightType::Point) {
            selection.pointLightCount++;
        } else {
            selection.spotLightCount++;
        }
    };

    if (lightVolumes.empty()) {
        for (int index = 0; index < static_cast<int>(lights.size()); ++index) {
            appendLight(index);
        }
        return selection;
    }

    for (const core::FrameLightVolume& volume : lightVolumes) {
        for (int index : volume.staticLightIndices) {
            appendLight(index);
        }
        for (int index : volume.movableLightIndices) {
            appendLight(index);
        }
    }

    return selection;
}

RenderLightPipeline::FrameState RenderLightPipeline::buildFrame(
    const RenderSceneView& scene,
    const glm::mat4& view,
    ShadowSystem& shadowSystem,
    bool deferred
) const {
    FrameState frame{};
    const ActiveLightSelection selection = selectActiveLights(scene.lights, scene.lightVolumes);
    frame.activeLightIndices = selection.indices;
    frame.lightCount = static_cast<int>(selection.indices.size());
    frame.pointLightCount = selection.pointLightCount;
    frame.spotLightCount = selection.spotLightCount;
    frame.debugLights.assign(scene.lights.size(), ActiveLightDebug{});
    if (deferred) {
        frame.gpuLights.assign(scene.lights.size(), GpuLight{});
        shadowSystem.beginFrame();
    }

    const glm::mat4 invView = deferred ? glm::inverse(view) : glm::mat4(1.0f);
    for (int lightIndex : frame.activeLightIndices) {
        const core::FrameLight& light = scene.lights[lightIndex];

        ActiveLightDebug debug{};
        debug.position = light.position;
        debug.radius = light.radius;
        debug.color = light.color;
        debug.outerAngle = light.outerAngle;
        debug.direction = light.direction;
        debug.type = light.type;
        frame.debugLights[lightIndex] = debug;

        if (!deferred) {
            continue;
        }

        const glm::vec3 viewPos = glm::vec3(view * glm::vec4(light.position, 1.0f));
        if (glm::length(viewPos) < light.radius) {
            frame.cameraInsideLightVolume = true;
        }

        glm::vec3 viewDir(0.0f);
        if (light.type == LightType::Spot) {
            viewDir = glm::normalize(glm::mat3(view) * light.direction);
        }

        int shadowType = 0;
        int shadowIndex = 0;
        if (light.castsShadow) {
            if (light.type == LightType::Spot) {
                ShadowSystem::SpotShadowDesc desc{
                    light.position,
                    light.direction,
                    light.radius,
                    light.outerAngle,
                    light.shadowBiasMin,
                    light.shadowBiasSlope,
                };
                const int registeredIndex = shadowSystem.registerSpotShadow(desc, invView);
                if (registeredIndex >= 0) {
                    shadowType = 1;
                    shadowIndex = registeredIndex;
                }
            } else {
                ShadowSystem::PointShadowDesc desc{
                    light.position,
                    light.radius,
                    light.shadowBiasMin,
                    light.shadowBiasSlope,
                };
                const int registeredIndex = shadowSystem.registerPointShadow(desc);
                if (registeredIndex >= 0) {
                    shadowType = 2;
                    shadowIndex = registeredIndex;
                }
            }
        }

        GpuLight gpu{};
        gpu.positionRadius = glm::vec4(viewPos, light.radius);
        gpu.colorIntensity = glm::vec4(light.color, light.intensity);
        gpu.directionType = glm::vec4(viewDir, static_cast<float>(light.type));
        gpu.shadowInfo = glm::vec4(
            static_cast<float>(shadowType),
            static_cast<float>(shadowIndex),
            light.shadowBiasMin,
            light.shadowBiasSlope
        );

        if (light.type == LightType::Spot) {
            const float inner = std::cos(glm::radians(light.innerAngle));
            const float outer = std::cos(glm::radians(light.outerAngle));
            const float tanOuter = std::tan(glm::radians(light.outerAngle));
            gpu.spotParams = glm::vec4(inner, outer, light.radius, tanOuter);
        } else {
            gpu.spotParams = glm::vec4(0.0f);
        }

        frame.gpuLights[lightIndex] = gpu;
    }

    return frame;
}

void RenderLightPipeline::uploadDeferredLights(const FrameState& frame, DeferredBuffers& buffers) const {
    const GLsizeiptr bufferSize = static_cast<GLsizeiptr>(frame.gpuLights.size() * sizeof(GpuLight));
    if (bufferSize == 0) {
        buffers.size = 0;
        return;
    }

    if (buffers.buffer == 0) {
        glGenBuffers(1, &buffers.buffer);
    }
    if (buffers.texture == 0) {
        glGenTextures(1, &buffers.texture);
    }

    glBindBuffer(GL_TEXTURE_BUFFER, buffers.buffer);
    if (bufferSize != buffers.size) {
        glBufferData(GL_TEXTURE_BUFFER, bufferSize, frame.gpuLights.data(), GL_DYNAMIC_DRAW);
        buffers.size = bufferSize;
    } else {
        glBufferSubData(GL_TEXTURE_BUFFER, 0, bufferSize, frame.gpuLights.data());
    }

    glBindTexture(GL_TEXTURE_BUFFER, buffers.texture);
    glTexBuffer(GL_TEXTURE_BUFFER, GL_RGBA32F, buffers.buffer);
}

}  // namespace render
