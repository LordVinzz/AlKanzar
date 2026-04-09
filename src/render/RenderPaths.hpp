#ifndef GL_GLEXT_PROTOTYPES
#define GL_GLEXT_PROTOTYPES
#endif
#pragma once

#include <SDL_opengl.h>

#include <memory>
#include <string>

#include "Profiling.hpp"
#include "RenderLightPipeline.hpp"
#include "SceneGeometryRenderer.hpp"
#include "SceneOverlayRenderer.hpp"

namespace core {
class ProfilerService;
}

namespace render {

struct RenderPathContext {
    int width{0};
    int height{0};
    const CameraMatrices& camera;
    const RenderFrameOptions& options;
    const RenderSceneView& scene;
    const RenderLightPipeline::FrameState& lights;
    RenderLightPipeline& lightPipeline;
    RenderResourceRegistry& resources;
    MaterialBinder& materialBinder;
    SceneGeometryRenderer& geometryRenderer;
    SceneOverlayRenderer& overlayRenderer;
    ShadowSystem& shadowSystem;
    core::ProfilerService* profiler{nullptr};
    glm::vec3 directionalLightDirection{0.0f, -1.0f, 0.0f};
    glm::vec3 directionalLightColor{1.0f};
    float directionalLightIntensity{0.0f};
};

class IRenderPath {
public:
    virtual ~IRenderPath() = default;

    virtual bool init(const std::string& shaderRoot, MaterialBinder& materialBinder, ShadowSystem& shadowSystem) = 0;
    virtual void resize(int width, int height) = 0;
    virtual void render(const RenderPathContext& context) = 0;
    [[nodiscard]] virtual const char* name() const = 0;
    [[nodiscard]] virtual bool usesDeferredLighting() const = 0;
    [[nodiscard]] virtual std::vector<ResourceMemoryRecord> profilingResources() const = 0;
};

class RenderPathBase : public IRenderPath {
public:
    void render(const RenderPathContext& context) final;

protected:
    void drawStandardSceneLayers(const RenderPathContext& context, const SceneGeometryShaderContext& shaderContext) const;

    virtual bool beginFrame(const RenderPathContext& context) = 0;
    virtual void prepareTargets(const RenderPathContext& context) = 0;
    virtual void drawGeometry(const RenderPathContext& context) = 0;
    virtual void composeFrame(const RenderPathContext& context) = 0;
};

class SimpleForwardPath final : public RenderPathBase {
public:
    bool init(const std::string& shaderRoot, MaterialBinder& materialBinder, ShadowSystem& shadowSystem) override;
    void resize(int width, int height) override;
    [[nodiscard]] const char* name() const override { return "Simple Forward"; }
    [[nodiscard]] bool usesDeferredLighting() const override { return false; }
    [[nodiscard]] std::vector<ResourceMemoryRecord> profilingResources() const override;

protected:
    bool beginFrame(const RenderPathContext& context) override;
    void prepareTargets(const RenderPathContext& context) override;
    void drawGeometry(const RenderPathContext& context) override;
    void composeFrame(const RenderPathContext& context) override;

private:
    ShaderProgram shader_{};
    GLint modelLocation_{-1};
    GLint viewLocation_{-1};
    GLint projLocation_{-1};
    GLint normalMatrixLocation_{-1};
    GLint lightDirLocation_{-1};
    SceneGeometryShaderContext geometryContext_{};
};

class DeferredRenderPath final : public RenderPathBase {
public:
    ~DeferredRenderPath() override;

    bool init(const std::string& shaderRoot, MaterialBinder& materialBinder, ShadowSystem& shadowSystem) override;
    void resize(int width, int height) override;
    [[nodiscard]] const char* name() const override { return "Deferred 4.1"; }
    [[nodiscard]] bool usesDeferredLighting() const override { return true; }
    [[nodiscard]] std::vector<ResourceMemoryRecord> profilingResources() const override;

protected:
    bool beginFrame(const RenderPathContext& context) override;
    void prepareTargets(const RenderPathContext& context) override;
    void drawGeometry(const RenderPathContext& context) override;
    void composeFrame(const RenderPathContext& context) override;

private:
    void destroyResources();
    void ensureResources(int width, int height);

    ShaderProgram geometryShader_{};
    ShaderProgram dirLightShader_{};
    ShaderProgram volumeShader_{};
    ShaderProgram compositeShader_{};
    SceneGeometryShaderContext geometryContext_{};

    GLint gbufferViewLocation_{-1};
    GLint gbufferProjLocation_{-1};
    GLint deferredInvProjLocation_{-1};
    GLint deferredDirLightDirLocation_{-1};
    GLint deferredDirLightColorLocation_{-1};
    GLint deferredDirLightIntensityLocation_{-1};
    GLint deferredAmbientLocation_{-1};
    GLint volumeProjLocation_{-1};
    GLint volumeInvProjLocation_{-1};
    GLint volumeScreenSizeLocation_{-1};
    GLint volumeLightOffsetLocation_{-1};
    GLint volumeIsSpotLocation_{-1};
    GLint volumeRenderFullscreenLocation_{-1};
    GLint volumeBoundsMinLocation_{-1};
    GLint volumeBoundsMaxLocation_{-1};
    GLint volumeInvViewLocation_{-1};
    GLint volumeSpotShadowMatrixLocation_{-1};
    GLint volumeSpotShadowCountLocation_{-1};
    GLint volumeSpotShadowTexelSizeLocation_{-1};
    GLint volumeSpotShadowPcfRadiusLocation_{-1};
    GLint volumePointShadowCountLocation_{-1};
    GLint volumePointShadowDiskRadiusLocation_{-1};
    GLint volumePointShadowPcfRadiusLocation_{-1};
    GLint compositeDebugModeLocation_{-1};
    GLint deferredShadowMapLocation_{-1};
    GLint deferredShadowMatrixLocation_{-1};
    GLint deferredCascadeSplitsLocation_{-1};
    GLint deferredCascadeCountLocation_{-1};
    GLint deferredShadowTexelSizeLocation_{-1};
    GLint deferredShadowBiasMinLocation_{-1};
    GLint deferredShadowBiasSlopeLocation_{-1};
    GLint deferredShadowPcfRadiusLocation_{-1};
    GLint compositeShadowMapLocation_{-1};
    GLint compositeShadowMatrixLocation_{-1};
    GLint compositeCascadeSplitsLocation_{-1};
    GLint compositeCascadeCountLocation_{-1};
    GLint compositeShadowTexelSizeLocation_{-1};
    GLint compositeShadowPcfRadiusLocation_{-1};
    GLint compositeInvProjLocation_{-1};
    GLint compositeShadowBiasMinLocation_{-1};
    GLint compositeShadowBiasSlopeLocation_{-1};
    GLint compositeShadowDebugCascadeLocation_{-1};
    GLint compositeDirLightDirLocation_{-1};

    GLuint gbufferFbo_{0};
    GLuint gbufferAlbedo_{0};
    GLuint gbufferNormal_{0};
    GLuint gbufferEmissiveAo_{0};
    GLuint gbufferClearcoat_{0};
    GLuint gbufferDepthColor_{0};
    GLuint gbufferDepth_{0};
    GLuint lightFbo_{0};
    GLuint lightColor_{0};
    GLuint fullscreenVao_{0};
    int deferredWidth_{0};
    int deferredHeight_{0};

    RenderLightPipeline::DeferredBuffers lightBuffers_{};
};

}  // namespace render
