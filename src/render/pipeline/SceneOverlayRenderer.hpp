#pragma once

#include <array>
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

namespace detail {

enum class OverlayIconBatchKind {
    UnselectedPoint = 0,
    UnselectedSpot,
    SelectedPoint,
    SelectedSpot,
};

struct OverlayIconBatch {
    OverlayIconBatchKind kind{OverlayIconBatchKind::UnselectedPoint};
    LightType type{LightType::Point};
    bool selected{false};
    float opacity{1.0f};
    std::vector<glm::vec4> clipCenters{};

    [[nodiscard]] bool empty() const { return clipCenters.empty(); }
};

struct OverlayWork {
    bool drawSelection{false};
    glm::vec3 selectionCenter{0.0f};
    glm::vec3 selectionExtents{0.01f};
    float selectionAxisScale{1.0f};
    bool drawSkeleton{false};
    std::array<OverlayIconBatch, 4> iconBatches{};
    std::vector<int> debugLightIndices{};
    bool drawDirectionalMarker{false};

    [[nodiscard]] bool hasWork() const;
};

[[nodiscard]] OverlayWork buildOverlayWork(
    const RenderSceneView& scene,
    const RenderLightPipeline::FrameState& lights,
    const CameraMatrices& camera,
    const RenderFrameOptions& options
);

}  // namespace detail

class SceneOverlayRenderer {
public:
    ~SceneOverlayRenderer();

    bool init(const std::string& shaderRoot);
    void destroy();

    void renderOverlays(
        const RenderSceneView& scene,
        const RenderLightPipeline::FrameState& lights,
        const CameraMatrices& camera,
        const RenderFrameOptions& options,
        int width,
        int height,
        const glm::vec3& directionalLightDirection
    ) const;

    void renderGroundIndicators(
        const RenderSceneView& scene,
        const CameraMatrices& camera
    ) const;

private:
    bool buildVolumeMeshes();
    bool buildDebugMeshes();
    bool buildGroundIndicatorResources();
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
    void drawLightIconBatch(
        const detail::OverlayIconBatch& batch,
        GLuint textureHandle,
        int width,
        int height
    ) const;
    void drawLineVertices(
        const std::vector<glm::vec3>& vertices,
        const glm::mat4& projection,
        const glm::mat4& view,
        const glm::vec4& color,
        GLenum primitive
    ) const;
    void drawFilledPolygon(
        const std::vector<glm::vec3>& vertices,
        const glm::mat4& projection,
        const glm::mat4& view,
        const glm::vec4& color
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
    MeshBuffer groundIndicatorRing_{};
    MeshBuffer lightIconQuad_{};
    GLint debugMvpLocation_{-1};
    GLint debugColorLocation_{-1};
    GLint lightIconSizeLocation_{-1};
    GLuint pointLightIconTexture_{0};
    GLuint spotLightIconTexture_{0};
    mutable GLuint lightIconInstanceVbo_{0};
    mutable GLuint skeletonLineVao_{0};
    mutable GLuint skeletonLineVbo_{0};
};

}  // namespace render
