#pragma once

#include <string>
#include <vector>

#ifndef GL_GLEXT_PROTOTYPES
#define GL_GLEXT_PROTOTYPES
#endif
#include <SDL_opengl.h>

#include "RenderLightPipeline.hpp"
#include "render/engine/RenderTypes.hpp"
#include "render/resources/ShaderProgram.hpp"

namespace render {

class SceneOverlayRenderer {
public:
    ~SceneOverlayRenderer();

    bool init(const std::string& shaderRoot);
    void destroy();

    void renderSelectionOverlay(
        const RenderSceneView& scene,
        const CameraMatrices& camera,
        const RenderFrameOptions& options,
        int width,
        int height
    ) const;

    void renderLightDebugOverlay(
        const RenderSceneView& scene,
        const RenderLightPipeline::FrameState& lights,
        const CameraMatrices& camera,
        const RenderFrameOptions& options,
        int width,
        int height,
        const glm::vec3& directionalLightDirection
    ) const;

private:
    bool buildVolumeMeshes();
    bool buildDebugMeshes();
    bool buildLightIconResources(const std::string& shaderRoot);
    bool loadLightIconTextures();
    void drawDebugMesh(
        const MeshBuffer& mesh,
        const glm::mat4& projection,
        const glm::mat4& view,
        const glm::mat4& model,
        const glm::vec4& color,
        bool wireframe
    ) const;
    void drawLightIcon(
        const glm::vec4& clipCenter,
        GLuint textureHandle,
        float opacity,
        int width,
        int height
    ) const;
    void drawSkeletonLines(
        const std::vector<glm::vec3>& jointWorldPositions,
        const std::vector<int>& parentIndices,
        const glm::mat4& projection,
        const glm::mat4& view
    ) const;

    ShaderProgram debugColorShader_{};
    ShaderProgram lightIconShader_{};
    MeshBuffer lightSphere_{};
    MeshBuffer lightCone_{};
    MeshBuffer axisGizmo_{};
    MeshBuffer selectionBox_{};
    MeshBuffer lightIconQuad_{};
    GLint debugMvpLocation_{-1};
    GLint debugColorLocation_{-1};
    GLint lightIconClipCenterLocation_{-1};
    GLint lightIconSizeLocation_{-1};
    GLint lightIconOpacityLocation_{-1};
    GLuint pointLightIconTexture_{0};
    GLuint spotLightIconTexture_{0};
    mutable GLuint skeletonLineVao_{0};
    mutable GLuint skeletonLineVbo_{0};
};

}  // namespace render
