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
#include "ShaderProgram.hpp"

namespace render {

enum class RenderLayer {
    Ground,
    Geometry,
    Actors,
};

class RenderEngine {
public:
    RenderEngine(int width, int height, std::string title = "AlKanzar - Render Preview");
    ~RenderEngine();

    RenderEngine(const RenderEngine&) = delete;
    RenderEngine& operator=(const RenderEngine&) = delete;

    bool init();
    void run();

private:
    enum class RendererPath {
        SimpleForward,
        TiledCompute,
        Deferred41,
    };

    enum class DebugView : int {
        Final = 0,
        Albedo = 1,
        Normal = 2,
        RoughMetal = 3,
        Depth = 4,
        Light = 5,
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
    };

    struct GpuLight {
        glm::vec4 positionRadius;
        glm::vec4 colorIntensity;
        glm::vec4 directionType;
        glm::vec4 spotParams;
    };

    struct TileMeta {
        uint32_t offset;
        uint32_t count;
        uint32_t pad0;
        uint32_t pad1;
    };

    void handleEvent(const SDL_Event& event, bool& running);
    void updateProjection();
    void detectLightingCapabilities();
    void renderScene();
    void buildScene();
    void buildLights();
    void updateLights();
    void ensureLightingResources();
    void destroyLightingResources();
    void ensureDeferredResources();
    void destroyDeferredResources();
    bool buildVolumeMeshes();
    void renderDepthPrepass();
    void dispatchDepthMinMax();
    void dispatchLightCulling();
    void renderLitScene();
    void renderDeferredScene();
    void renderSimpleScene();

    void setLightingUniforms() const;
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

    using DispatchComputeFn = void (*)(GLuint, GLuint, GLuint);
    using MemoryBarrierFn = void (*)(GLbitfield);

    ShaderProgram lightingShader_;
    ShaderProgram depthShader_;
    ShaderProgram depthMinMaxCompute_;
    ShaderProgram lightCullCompute_;
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
    GLint lightingMvpLocation_{-1};
    GLint lightingViewLocation_{-1};
    GLint lightingTileCountLocation_{-1};
    GLint lightingTileSizeLocation_{-1};
    GLint dirLightDirLocation_{-1};
    GLint dirLightColorLocation_{-1};
    GLint dirLightIntensityLocation_{-1};
    GLint depthMvpLocation_{-1};
    GLint depthScreenSizeLocation_{-1};
    GLint depthTileCountLocation_{-1};
    GLint cullScreenSizeLocation_{-1};
    GLint cullTileCountLocation_{-1};
    GLint cullTileSizeLocation_{-1};
    GLint cullLightCountLocation_{-1};
    GLint cullMaxLightsLocation_{-1};
    GLint cullInvProjLocation_{-1};
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
    GLint compositeDebugModeLocation_{-1};

    GLuint depthFbo_{0};
    GLuint depthTexture_{0};
    GLuint lightsSsbo_{0};
    GLuint tileMetaSsbo_{0};
    GLuint tileIndexSsbo_{0};
    GLuint tileDepthSsbo_{0};
    GLuint gbufferFbo_{0};
    GLuint gbufferAlbedo_{0};
    GLuint gbufferNormal_{0};
    GLuint gbufferDepth_{0};
    GLuint lightFbo_{0};
    GLuint lightColor_{0};
    GLuint lightsTbo_{0};
    GLuint lightsTboTex_{0};
    GLuint fullscreenVao_{0};

    int tileSize_{16};
    int maxLightsPerTile_{128};
    int tilesX_{0};
    int tilesY_{0};
    int resourceWidth_{0};
    int resourceHeight_{0};
    int deferredWidth_{0};
    int deferredHeight_{0};
    int lightCount_{0};
    int pointLightCount_{0};
    int spotLightCount_{0};
    GLsizeiptr lightBufferSize_{0};
    GLsizeiptr lightTboSize_{0};
    DispatchComputeFn dispatchCompute_{nullptr};
    MemoryBarrierFn memoryBarrier_{nullptr};

    RendererPath rendererPath_{RendererPath::SimpleForward};
    DebugView debugView_{DebugView::Final};
    bool cameraInsideLightVolume_{false};

    std::vector<LightInstance> lights_;
    std::vector<GpuLight> gpuLights_;

    glm::mat4 projection_{1.0f};
    glm::mat4 invProjection_{1.0f};
    glm::mat4 view_{1.0f};
    bool sceneReady_{false};
};

}  // namespace render
