#ifndef GL_GLEXT_PROTOTYPES
#define GL_GLEXT_PROTOTYPES
#endif
#pragma once

#include <SDL.h>
#include <SDL_opengl.h>

#include <cstdint>
#include <initializer_list>
#include <string>
#include <vector>

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include "MeshBuffer.hpp"
#include "ShadowSystem.hpp"
#include "ShaderProgram.hpp"

namespace render {

enum class RenderLayer {
    Ground,
    Geometry,
    Actors,
};

class RenderEngine {
public:
    /**
     * Creates a render engine with the requested window size and title.
     * @param width Initial window width in pixels.
     * @param height Initial window height in pixels.
     * @param title Window title string.
     */
    RenderEngine(int width, int height, std::string title = "AlKanzar - Render Preview");
    /**
     * Releases GL resources and destroys the SDL window/context.
     */
    ~RenderEngine();

    /**
     * Non-copyable to avoid duplicating SDL/GL resources.
     */
    RenderEngine(const RenderEngine&) = delete;
    /**
     * Non-copyable assignment to avoid duplicating SDL/GL resources.
     */
    RenderEngine& operator=(const RenderEngine&) = delete;

    /**
     * Initializes the SDL window, GL context, and scene resources.
     * @return true when initialization succeeds or was already done.
     */
    bool init();
    /**
     * Runs the main event/render loop until quit.
     */
    void run();

private:
    enum class RendererPath {
        SimpleForward,
        Deferred41,
    };

    enum class DebugView : int {
        Final = 0,
        Albedo = 1,
        Normal = 2,
        RoughMetal = 3,
        Depth = 4,
        Light = 5,
        ShadowMap = 6,
        ShadowFactor = 7,
        ShadowCascade = 8,
    };

    enum class LightType : uint32_t {
        Point = 0,
        Spot = 1,
    };

    struct LightInstance {
        glm::vec3 basePosition;
        float radius;
        glm::vec3 color;
        float intensity;
        glm::vec3 target;
        float innerAngle;
        float outerAngle;
        LightType type;
        float phase;
        bool castsShadow{false};
        float shadowBiasMin{0.0f};
        float shadowBiasSlope{0.0f};
    };

    struct GpuLight {
        glm::vec4 positionRadius;
        glm::vec4 colorIntensity;
        glm::vec4 directionType;
        glm::vec4 spotParams;
        glm::vec4 shadowInfo;
    };

    /**
     * Handles input/window events and updates camera controls and debug view.
     * @param event SDL event to process.
     * @param running Set to false to exit the main loop.
     */
    void handleEvent(const SDL_Event& event, bool& running);
    /**
     * Updates viewport, projection, and view matrices from current camera state.
     */
    void updateProjection();
    /**
     * Chooses the rendering path based on GL version support.
     */
    void detectLightingCapabilities();
    /**
     * Renders a single frame using the active renderer path.
     */
    void renderScene();
    /**
     * Builds shaders, meshes, and uploads scene geometry.
     */
    void buildScene();
    /**
     * Initializes the light list used by deferred rendering.
     */
    void buildLights();
    /**
     * Animates lights, updates GPU buffers, and counts light types.
     */
    void updateLights();
    /**
     * Allocates or resizes deferred G-buffer and light targets.
     */
    void ensureDeferredResources();
    /**
     * Releases deferred rendering GPU resources.
     */
    void destroyDeferredResources();
    /**
     * Builds sphere/cone meshes for light volumes.
     * @return true when both meshes upload successfully.
     */
    bool buildVolumeMeshes();
    /**
     * Renders the scene using the deferred 4.1 path.
     */
    void renderDeferredScene();
    /**
     * Renders the scene using the simple forward path.
     */
    void renderSimpleScene();

    /**
     * Draws meshes for a layer with appropriate depth mask behavior.
     * @param layer Render layer to determine depth writes.
     * @param meshes Mesh buffers to draw.
     */
    void drawLayer(RenderLayer layer, std::initializer_list<const MeshBuffer*> meshes) const;

    SDL_Window* window_{nullptr};
    SDL_GLContext glContext_{nullptr};
    int width_;
    int height_;
    float zoom_{1.0f};
    float panX_{0.0f};
    float panY_{0.0f};
    float cameraDistance_{15.0f};
    bool middleDragging_{false};
    int lastMouseX_{0};
    int lastMouseY_{0};
    std::string title_;

    ShaderProgram simpleShader_;
    ShaderProgram deferredGeometryShader_;
    ShaderProgram deferredDirLightShader_;
    ShaderProgram deferredVolumeShader_;
    ShaderProgram deferredCompositeShader_;
    MeshBuffer ground_;
    MeshBuffer wallA_;
    MeshBuffer wallB_;
    MeshBuffer lightSphere_;
    MeshBuffer lightCone_;
    GLint simpleMvpLocation_{-1};
    GLint simpleLightDirLocation_{-1};
    GLint gbufferMvpLocation_{-1};
    GLint gbufferViewLocation_{-1};
    GLint gbufferMetallicLocation_{-1};
    GLint gbufferRoughnessLocation_{-1};
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
    GLuint gbufferDepthColor_{0};
    GLuint gbufferDepth_{0};
    GLuint lightFbo_{0};
    GLuint lightColor_{0};
    GLuint lightsTbo_{0};
    GLuint lightsTboTex_{0};
    GLuint fullscreenVao_{0};

    int deferredWidth_{0};
    int deferredHeight_{0};
    int lightCount_{0};
    int pointLightCount_{0};
    int spotLightCount_{0};
    GLsizeiptr lightTboSize_{0};

    RendererPath rendererPath_{RendererPath::SimpleForward};
    DebugView debugView_{DebugView::Final};
    bool cameraInsideLightVolume_{false};
    int shadowDebugCascade_{0};

    ShadowSystem shadowSystem_{};

    std::vector<LightInstance> lights_;
    std::vector<GpuLight> gpuLights_;

    glm::mat4 projection_{1.0f};
    glm::mat4 invProjection_{1.0f};
    glm::mat4 view_{1.0f};
    bool sceneReady_{false};
};

}  // namespace render
