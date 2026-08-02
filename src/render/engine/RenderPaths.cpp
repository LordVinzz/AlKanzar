#include "RenderPaths.hpp"

#include <algorithm>

#include <glm/geometric.hpp>
#include <glm/matrix.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/trigonometric.hpp>

#include "core/profiling/ProfilerService.hpp"
#include "RenderSceneView.hpp"

namespace {

constexpr float kNearPlane = 0.0f;
constexpr float kFarPlane = 100.0f;

}  // namespace

namespace render {



DeferredRenderPath::~DeferredRenderPath() {
    destroyResources();
}

bool DeferredRenderPath::init(const std::string& shaderRoot, MaterialBinder& materialBinder, ShadowSystem& shadowSystem) {
    destroyResources();

    if (!geometryShader_.buildFromFiles(shaderRoot + "deferred_gbuffer.vert", shaderRoot + "deferred_gbuffer.frag") ||
        !dirLightShader_.buildFromFiles(shaderRoot + "fullscreen_tri.vert", shaderRoot + "deferred_dir_light.frag") ||
        !volumeShader_.buildFromFiles(shaderRoot + "deferred_volume.vert", shaderRoot + "deferred_volume.frag") ||
        !compositeShader_.buildFromFiles(shaderRoot + "fullscreen_tri.vert", shaderRoot + "deferred_composite.frag")) {
        return false;
    }

    geometryContext_.modelLocation = geometryShader_.uniformLocation("uModel");
    geometryContext_.normalMatrixLocation = geometryShader_.uniformLocation("uNormalMatrix");
    geometryContext_.skinnedLocation = geometryShader_.uniformLocation("uSkinned");
    geometryContext_.jointBaseIndexLocation = geometryShader_.uniformLocation("uJointBaseIndex");
    geometryContext_.jointCountLocation = geometryShader_.uniformLocation("uJointCount");
    geometryContext_.materialLocations = materialBinder.captureUniformLocations(geometryShader_);
    gbufferViewLocation_ = geometryShader_.uniformLocation("uView");
    gbufferProjLocation_ = geometryShader_.uniformLocation("uProj");

    deferredInvProjLocation_ = dirLightShader_.uniformLocation("uInvProj");
    deferredDirLightDirLocation_ = dirLightShader_.uniformLocation("uDirLightDir");
    deferredDirLightColorLocation_ = dirLightShader_.uniformLocation("uDirLightColor");
    deferredDirLightIntensityLocation_ = dirLightShader_.uniformLocation("uDirLightIntensity");
    deferredAmbientLocation_ = dirLightShader_.uniformLocation("uAmbient");
    deferredShadowMapLocation_ = dirLightShader_.uniformLocation("uShadowMap");
    deferredShadowMatrixLocation_ = dirLightShader_.uniformLocation("uShadowMatrices[0]");
    deferredCascadeSplitsLocation_ = dirLightShader_.uniformLocation("uCascadeSplits[0]");
    deferredCascadeCountLocation_ = dirLightShader_.uniformLocation("uCascadeCount");
    deferredShadowTexelSizeLocation_ = dirLightShader_.uniformLocation("uShadowTexelSize");
    deferredShadowBiasMinLocation_ = dirLightShader_.uniformLocation("uShadowBiasMin");
    deferredShadowBiasSlopeLocation_ = dirLightShader_.uniformLocation("uShadowBiasSlope");
    deferredShadowPcfRadiusLocation_ = dirLightShader_.uniformLocation("uShadowPcfRadius");

    volumeProjLocation_ = volumeShader_.uniformLocation("uProj");
    volumeInvProjLocation_ = volumeShader_.uniformLocation("uInvProj");
    volumeScreenSizeLocation_ = volumeShader_.uniformLocation("uScreenSize");
    volumeLightOffsetLocation_ = volumeShader_.uniformLocation("uLightOffset");
    volumeIsSpotLocation_ = volumeShader_.uniformLocation("uIsSpot");
    volumeRenderFullscreenLocation_ = volumeShader_.uniformLocation("uRenderFullscreen");
    volumeBoundsMinLocation_ = volumeShader_.uniformLocation("uVolumeMin");
    volumeBoundsMaxLocation_ = volumeShader_.uniformLocation("uVolumeMax");
    volumeInvViewLocation_ = volumeShader_.uniformLocation("uInvView");
    volumeSpotShadowMatrixLocation_ = volumeShader_.uniformLocation("uSpotShadowMatrices[0]");
    volumeSpotShadowPositionLocation_ = volumeShader_.uniformLocation("uSpotShadowPositions[0]");
    volumeSpotShadowFarPlaneLocation_ = volumeShader_.uniformLocation("uSpotShadowFarPlanes[0]");
    volumeSpotShadowCountLocation_ = volumeShader_.uniformLocation("uSpotShadowCount");
    volumeSpotShadowTexelSizeLocation_ = volumeShader_.uniformLocation("uSpotShadowTexelSize");
    volumeSpotShadowPcfRadiusLocation_ = volumeShader_.uniformLocation("uSpotShadowPcfRadius");
    volumePointShadowCountLocation_ = volumeShader_.uniformLocation("uPointShadowCount");
    volumePointShadowDiskRadiusLocation_ = volumeShader_.uniformLocation("uPointShadowDiskRadius");
    volumePointShadowPcfRadiusLocation_ = volumeShader_.uniformLocation("uPointShadowPcfRadius");

    compositeDebugModeLocation_ = compositeShader_.uniformLocation("uDebugMode");
    compositeShadowMapLocation_ = compositeShader_.uniformLocation("uShadowMap");
    compositeShadowMatrixLocation_ = compositeShader_.uniformLocation("uShadowMatrices[0]");
    compositeCascadeSplitsLocation_ = compositeShader_.uniformLocation("uCascadeSplits[0]");
    compositeCascadeCountLocation_ = compositeShader_.uniformLocation("uCascadeCount");
    compositeShadowTexelSizeLocation_ = compositeShader_.uniformLocation("uShadowTexelSize");
    compositeShadowPcfRadiusLocation_ = compositeShader_.uniformLocation("uShadowPcfRadius");
    compositeInvProjLocation_ = compositeShader_.uniformLocation("uInvProj");
    compositeShadowBiasMinLocation_ = compositeShader_.uniformLocation("uShadowBiasMin");
    compositeShadowBiasSlopeLocation_ = compositeShader_.uniformLocation("uShadowBiasSlope");
    compositeShadowDebugCascadeLocation_ = compositeShader_.uniformLocation("uShadowDebugCascade");
    compositeDirLightDirLocation_ = compositeShader_.uniformLocation("uDirLightDir");

    dirLightShader_.use();
    glUniform1i(dirLightShader_.uniformLocation("uGAlbedoMetal"), 0);
    glUniform1i(dirLightShader_.uniformLocation("uGNormalRough"), 1);
    glUniform1i(dirLightShader_.uniformLocation("uGEmissiveAo"), 2);
    glUniform1i(dirLightShader_.uniformLocation("uGClearcoat"), 3);
    glUniform1i(dirLightShader_.uniformLocation("uDepth"), 4);
    glUniform1i(deferredShadowMapLocation_, 5);

    volumeShader_.use();
    glUniform1i(volumeShader_.uniformLocation("uGAlbedoMetal"), 0);
    glUniform1i(volumeShader_.uniformLocation("uGNormalRough"), 1);
    glUniform1i(volumeShader_.uniformLocation("uGEmissiveAo"), 2);
    glUniform1i(volumeShader_.uniformLocation("uGClearcoat"), 3);
    glUniform1i(volumeShader_.uniformLocation("uDepth"), 4);
    glUniform1i(volumeShader_.uniformLocation("uLightBuffer"), 5);
    glUniform1i(volumeShader_.uniformLocation("uSpotShadowMap"), 6);
    glUniform1i(volumeShader_.uniformLocation("uPointShadowMap"), 7);

    compositeShader_.use();
    glUniform1i(compositeShader_.uniformLocation("uLightBuffer"), 0);
    glUniform1i(compositeShader_.uniformLocation("uGAlbedoMetal"), 1);
    glUniform1i(compositeShader_.uniformLocation("uGNormalRough"), 2);
    glUniform1i(compositeShader_.uniformLocation("uGEmissiveAo"), 3);
    glUniform1i(compositeShader_.uniformLocation("uGClearcoat"), 4);
    glUniform1i(compositeShader_.uniformLocation("uDepth"), 5);
    glUniform1i(compositeShadowMapLocation_, 6);

    geometryShader_.use();
    glUniform1i(geometryShader_.uniformLocation("uBaseColorTexture"), 0);
    glUniform1i(geometryShader_.uniformLocation("uMetallicRoughnessTexture"), 1);
    glUniform1i(geometryShader_.uniformLocation("uNormalTexture"), 2);
    glUniform1i(geometryShader_.uniformLocation("uAoTexture"), 3);
    glUniform1i(geometryShader_.uniformLocation("uEmissiveTexture"), 4);
    glUniform1i(geometryShader_.uniformLocation("uAlphaTexture"), 5);
    glUniform1i(geometryShader_.uniformLocation("uClearcoatTexture"), 6);
    glUniform1i(geometryShader_.uniformLocation("uDetailNormalTexture"), 7);
    glUniform1i(geometryShader_.uniformLocation("uHeightTexture"), 8);
    glUniform1i(geometryShader_.uniformLocation("uJointBuffer"), 9);

    return shadowSystem.init(shaderRoot);
}

void DeferredRenderPath::resize(int width, int height) {
    ensureResources(width, height);
}

void DeferredRenderPath::prepareOcclusionQueryPass(const RenderPathContext& context) const {
    glBindFramebuffer(GL_FRAMEBUFFER, lightFbo_);
    glViewport(0, 0, context.width, context.height);
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glDisable(GL_BLEND);
}

bool DeferredRenderPath::beginFrame(const RenderPathContext& context) {
    ensureResources(context.width, context.height);
    if (gbufferFbo_ == 0 || lightFbo_ == 0) {
        return false;
    }

    if (context.profiler) {
        ALKANZAR_PROFILE_SCOPE(*context.profiler, "Deferred Light Upload");
        context.lightPipeline.uploadDeferredLights(context.lights, lightBuffers_);
    } else {
        context.lightPipeline.uploadDeferredLights(context.lights, lightBuffers_);
    }

    const auto shadowRenderables = collectShadowRenderables(context.scene);
    const glm::vec3 dirLightWorld = glm::normalize(context.directionalLightDirection);
    context.shadowSystem.updateDirectional(
        context.camera.view,
        context.camera.projection,
        dirLightWorld,
        kNearPlane,
        kFarPlane
    );
    if (context.profiler) {
        {
            ALKANZAR_PROFILE_SCOPE(*context.profiler, "Directional Shadow Pass");
            ALKANZAR_PROFILE_GPU_SCOPE(*context.profiler, "Directional Shadow Pass");
            context.shadowSystem.renderDirectionalShadows(shadowRenderables, context.jointTextureBuffer);
        }
        {
            ALKANZAR_PROFILE_SCOPE(*context.profiler, "Spot Shadow Pass");
            ALKANZAR_PROFILE_GPU_SCOPE(*context.profiler, "Spot Shadow Pass");
            context.shadowSystem.renderSpotShadows(shadowRenderables, context.jointTextureBuffer);
        }
        {
            ALKANZAR_PROFILE_SCOPE(*context.profiler, "Point Shadow Pass");
            ALKANZAR_PROFILE_GPU_SCOPE(*context.profiler, "Point Shadow Pass");
            context.shadowSystem.renderPointShadows(shadowRenderables, context.jointTextureBuffer);
        }
    } else {
        context.shadowSystem.renderDirectionalShadows(shadowRenderables, context.jointTextureBuffer);
        context.shadowSystem.renderSpotShadows(shadowRenderables, context.jointTextureBuffer);
        context.shadowSystem.renderPointShadows(shadowRenderables, context.jointTextureBuffer);
    }
    return true;
}

void DeferredRenderPath::prepareTargets(const RenderPathContext& context) {
    glBindFramebuffer(GL_FRAMEBUFFER, gbufferFbo_);
    glViewport(0, 0, context.width, context.height);
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);

    const GLfloat clearAlbedo[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    const GLfloat clearNormal[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    const GLfloat clearEmissiveAo[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    const GLfloat clearClearcoat[4] = {0.0f, 1.0f, 0.0f, 0.0f};
    const GLfloat clearDepth[4] = {1.0f, 0.0f, 0.0f, 0.0f};
    glClearBufferfv(GL_COLOR, 0, clearAlbedo);
    glClearBufferfv(GL_COLOR, 1, clearNormal);
    glClearBufferfv(GL_COLOR, 2, clearEmissiveAo);
    glClearBufferfv(GL_COLOR, 3, clearClearcoat);
    glClearBufferfv(GL_COLOR, 4, clearDepth);
    glClearBufferfi(GL_DEPTH_STENCIL, 0, 1.0f, 0);
}

void DeferredRenderPath::drawGeometry(const RenderPathContext& context) {
    if (context.profiler) {
        ALKANZAR_PROFILE_SCOPE(*context.profiler, "GBuffer Pass");
        ALKANZAR_PROFILE_GPU_SCOPE(*context.profiler, "GBuffer Pass");
        geometryShader_.use();
        glUniformMatrix4fv(gbufferViewLocation_, 1, GL_FALSE, glm::value_ptr(context.camera.view));
        glUniformMatrix4fv(gbufferProjLocation_, 1, GL_FALSE, glm::value_ptr(context.camera.projection));
        drawStandardSceneLayers(context, geometryContext_);
        return;
    }

    geometryShader_.use();
    glUniformMatrix4fv(gbufferViewLocation_, 1, GL_FALSE, glm::value_ptr(context.camera.view));
    glUniformMatrix4fv(gbufferProjLocation_, 1, GL_FALSE, glm::value_ptr(context.camera.projection));
    drawStandardSceneLayers(context, geometryContext_);
}

void DeferredRenderPath::composeFrame(const RenderPathContext& context) {
    if (context.profiler) {
        ALKANZAR_PROFILE_SCOPE(*context.profiler, "Lighting & Composition");
        ALKANZAR_PROFILE_GPU_SCOPE(*context.profiler, "Lighting & Composition");
    }

    const auto clearSamplers = [](int maxUnitExclusive) {
        for (int unit = 0; unit < maxUnitExclusive; ++unit) {
            glBindSampler(unit, 0);
        }
    };

    const glm::vec3 dirLightWorld = glm::normalize(context.directionalLightDirection);
    const glm::vec3 dirLightView = glm::normalize(glm::mat3(context.camera.view) * dirLightWorld);
    const glm::mat4 invView = glm::inverse(context.camera.view);
    const int shadowDebugCascade = std::clamp(
        context.options.shadowDebugCascade,
        0,
        context.shadowSystem.directionalCascadeCount() - 1
    );

    glBindFramebuffer(GL_FRAMEBUFFER, lightFbo_);
    glViewport(0, 0, context.width, context.height);
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glDisable(GL_BLEND);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    dirLightShader_.use();
    glUniformMatrix4fv(deferredInvProjLocation_, 1, GL_FALSE, glm::value_ptr(context.camera.invProjection));
    glUniform3fv(deferredDirLightDirLocation_, 1, glm::value_ptr(dirLightView));
    glUniform3fv(deferredDirLightColorLocation_, 1, glm::value_ptr(context.directionalLightColor));
    glUniform1f(deferredDirLightIntensityLocation_, context.directionalLightIntensity);
    glUniform3f(deferredAmbientLocation_, 0.0f, 0.0f, 0.0f);
    glUniformMatrix4fv(
        deferredShadowMatrixLocation_,
        context.shadowSystem.directionalCascadeCount(),
        GL_FALSE,
        glm::value_ptr(context.shadowSystem.directionalMatrices().front())
    );
    glUniform1fv(
        deferredCascadeSplitsLocation_,
        context.shadowSystem.directionalCascadeCount(),
        context.shadowSystem.directionalSplits().data()
    );
    glUniform1i(deferredCascadeCountLocation_, context.shadowSystem.directionalCascadeCount());
    glUniform2f(
        deferredShadowTexelSizeLocation_,
        context.shadowSystem.directionalTexelSize().x,
        context.shadowSystem.directionalTexelSize().y
    );
    glUniform1f(deferredShadowBiasMinLocation_, context.shadowSystem.directionalBiasMin());
    glUniform1f(deferredShadowBiasSlopeLocation_, context.shadowSystem.directionalBiasSlope());
    glUniform1i(deferredShadowPcfRadiusLocation_, context.shadowSystem.directionalPcfRadius());

    glActiveTexture(GL_TEXTURE0 + 0);
    glBindTexture(GL_TEXTURE_2D, gbufferAlbedo_);
    glActiveTexture(GL_TEXTURE0 + 1);
    glBindTexture(GL_TEXTURE_2D, gbufferNormal_);
    glActiveTexture(GL_TEXTURE0 + 2);
    glBindTexture(GL_TEXTURE_2D, gbufferEmissiveAo_);
    glActiveTexture(GL_TEXTURE0 + 3);
    glBindTexture(GL_TEXTURE_2D, gbufferClearcoat_);
    glActiveTexture(GL_TEXTURE0 + 4);
    glBindTexture(GL_TEXTURE_2D, gbufferDepthColor_);
    glActiveTexture(GL_TEXTURE0 + 5);
    glBindTexture(GL_TEXTURE_2D_ARRAY, context.shadowSystem.directionalShadowMap());
    clearSamplers(6);

    glBindVertexArray(fullscreenVao_);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);

    if (context.lights.lightCount > 0 && lightBuffers_.texture != 0) {
        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE);
        glDisable(GL_DEPTH_TEST);
        glDepthMask(GL_FALSE);
        glDisable(GL_CULL_FACE);

        volumeShader_.use();
        glUniformMatrix4fv(volumeProjLocation_, 1, GL_FALSE, glm::value_ptr(context.camera.projection));
        glUniformMatrix4fv(volumeInvProjLocation_, 1, GL_FALSE, glm::value_ptr(context.camera.invProjection));
        glUniform2f(volumeScreenSizeLocation_, static_cast<float>(context.width), static_cast<float>(context.height));
        glUniformMatrix4fv(volumeInvViewLocation_, 1, GL_FALSE, glm::value_ptr(invView));
        glUniformMatrix4fv(
            volumeSpotShadowMatrixLocation_,
            context.shadowSystem.spotShadowCount(),
            GL_FALSE,
            glm::value_ptr(context.shadowSystem.spotShadowMatrices().front())
        );
        glUniform3fv(
            volumeSpotShadowPositionLocation_,
            context.shadowSystem.spotShadowCount(),
            glm::value_ptr(context.shadowSystem.spotShadowPositions().front())
        );
        glUniform1fv(
            volumeSpotShadowFarPlaneLocation_,
            context.shadowSystem.spotShadowCount(),
            context.shadowSystem.spotShadowFarPlanes().data()
        );
        glUniform1i(volumeSpotShadowCountLocation_, context.shadowSystem.spotShadowCount());
        glUniform2f(
            volumeSpotShadowTexelSizeLocation_,
            context.shadowSystem.spotTexelSize().x,
            context.shadowSystem.spotTexelSize().y
        );
        glUniform1i(volumeSpotShadowPcfRadiusLocation_, context.shadowSystem.spotPcfRadius());
        glUniform1i(volumePointShadowCountLocation_, context.shadowSystem.pointShadowCount());
        glUniform1f(volumePointShadowDiskRadiusLocation_, context.shadowSystem.pointShadowDiskRadius());
        glUniform1i(volumePointShadowPcfRadiusLocation_, context.shadowSystem.pointPcfRadius());
        glUniform1i(volumeRenderFullscreenLocation_, 1);

        glActiveTexture(GL_TEXTURE0 + 0);
        glBindTexture(GL_TEXTURE_2D, gbufferAlbedo_);
        glActiveTexture(GL_TEXTURE0 + 1);
        glBindTexture(GL_TEXTURE_2D, gbufferNormal_);
        glActiveTexture(GL_TEXTURE0 + 2);
        glBindTexture(GL_TEXTURE_2D, gbufferEmissiveAo_);
        glActiveTexture(GL_TEXTURE0 + 3);
        glBindTexture(GL_TEXTURE_2D, gbufferClearcoat_);
        glActiveTexture(GL_TEXTURE0 + 4);
        glBindTexture(GL_TEXTURE_2D, gbufferDepthColor_);
        glActiveTexture(GL_TEXTURE0 + 5);
        glBindTexture(GL_TEXTURE_BUFFER, lightBuffers_.texture);
        glActiveTexture(GL_TEXTURE0 + 6);
        glBindTexture(GL_TEXTURE_2D_ARRAY, context.shadowSystem.spotShadowMap());
        glActiveTexture(GL_TEXTURE0 + 7);
        glBindTexture(GL_TEXTURE_CUBE_MAP_ARRAY, context.shadowSystem.pointShadowMap());
        clearSamplers(8);

        glBindVertexArray(fullscreenVao_);
        for (const core::FrameLightVolume& volume : context.scene.lightVolumes) {
            glUniform3fv(volumeBoundsMinLocation_, 1, glm::value_ptr(volume.minCorner));
            glUniform3fv(volumeBoundsMaxLocation_, 1, glm::value_ptr(volume.maxCorner));
            for (int lightIndex : volume.staticLightIndices) {
                if (lightIndex < 0 || lightIndex >= static_cast<int>(context.scene.lights.size())) {
                    continue;
                }
                glUniform1i(volumeIsSpotLocation_, context.scene.lights[lightIndex].type == LightType::Spot ? 1 : 0);
                glUniform1i(volumeLightOffsetLocation_, lightIndex);
                glDrawArrays(GL_TRIANGLES, 0, 3);
            }
            for (int lightIndex : volume.movableLightIndices) {
                if (lightIndex < 0 || lightIndex >= static_cast<int>(context.scene.lights.size())) {
                    continue;
                }
                glUniform1i(volumeIsSpotLocation_, context.scene.lights[lightIndex].type == LightType::Spot ? 1 : 0);
                glUniform1i(volumeLightOffsetLocation_, lightIndex);
                glDrawArrays(GL_TRIANGLES, 0, 3);
            }
        }
        glBindVertexArray(0);

        glDisable(GL_BLEND);
        glEnable(GL_DEPTH_TEST);
        glEnable(GL_CULL_FACE);
        glDepthMask(GL_TRUE);
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, context.width, context.height);
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glDisable(GL_BLEND);

    compositeShader_.use();
    glUniform1i(compositeDebugModeLocation_, static_cast<int>(context.options.debugView));
    glUniformMatrix4fv(
        compositeShadowMatrixLocation_,
        context.shadowSystem.directionalCascadeCount(),
        GL_FALSE,
        glm::value_ptr(context.shadowSystem.directionalMatrices().front())
    );
    glUniform1fv(
        compositeCascadeSplitsLocation_,
        context.shadowSystem.directionalCascadeCount(),
        context.shadowSystem.directionalSplits().data()
    );
    glUniform1i(compositeCascadeCountLocation_, context.shadowSystem.directionalCascadeCount());
    glUniform2f(
        compositeShadowTexelSizeLocation_,
        context.shadowSystem.directionalTexelSize().x,
        context.shadowSystem.directionalTexelSize().y
    );
    glUniform1i(compositeShadowPcfRadiusLocation_, context.shadowSystem.directionalPcfRadius());
    glUniformMatrix4fv(compositeInvProjLocation_, 1, GL_FALSE, glm::value_ptr(context.camera.invProjection));
    glUniform3fv(compositeDirLightDirLocation_, 1, glm::value_ptr(dirLightView));
    glUniform1f(compositeShadowBiasMinLocation_, context.shadowSystem.directionalBiasMin());
    glUniform1f(compositeShadowBiasSlopeLocation_, context.shadowSystem.directionalBiasSlope());
    glUniform1i(compositeShadowDebugCascadeLocation_, shadowDebugCascade);

    glActiveTexture(GL_TEXTURE0 + 0);
    glBindTexture(GL_TEXTURE_2D, lightColor_);
    glActiveTexture(GL_TEXTURE0 + 1);
    glBindTexture(GL_TEXTURE_2D, gbufferAlbedo_);
    glActiveTexture(GL_TEXTURE0 + 2);
    glBindTexture(GL_TEXTURE_2D, gbufferNormal_);
    glActiveTexture(GL_TEXTURE0 + 3);
    glBindTexture(GL_TEXTURE_2D, gbufferEmissiveAo_);
    glActiveTexture(GL_TEXTURE0 + 4);
    glBindTexture(GL_TEXTURE_2D, gbufferClearcoat_);
    glActiveTexture(GL_TEXTURE0 + 5);
    glBindTexture(GL_TEXTURE_2D, gbufferDepthColor_);
    glActiveTexture(GL_TEXTURE0 + 6);
    glBindTexture(GL_TEXTURE_2D_ARRAY, context.shadowSystem.directionalShadowMap());
    clearSamplers(7);

    glBindVertexArray(fullscreenVao_);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);
    glDepthMask(GL_TRUE);
}

}  // namespace render
