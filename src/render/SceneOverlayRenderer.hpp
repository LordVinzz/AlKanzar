#pragma once

#include "RenderLightPipeline.hpp"
#include "RenderTypes.hpp"
#include "ShaderProgram.hpp"

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
    void drawDebugMesh(
        const MeshBuffer& mesh,
        const glm::mat4& projection,
        const glm::mat4& view,
        const glm::mat4& model,
        const glm::vec4& color,
        bool wireframe
    ) const;

    ShaderProgram debugColorShader_{};
    MeshBuffer lightSphere_{};
    MeshBuffer lightCone_{};
    MeshBuffer axisGizmo_{};
    MeshBuffer selectionBox_{};
    GLint debugMvpLocation_{-1};
    GLint debugColorLocation_{-1};
};

}  // namespace render
