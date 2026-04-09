#ifndef GL_GLEXT_PROTOTYPES
#define GL_GLEXT_PROTOTYPES
#endif
#pragma once

#include <SDL.h>
#include <SDL_opengl.h>

#include <memory>
#include <string>
#include <vector>

#include <glm/vec3.hpp>

#include "core/FrameData.hpp"
#include "Geometry.hpp"
#include "Material.hpp"
#include "MeshBuffer.hpp"
#include "RenderLightPipeline.hpp"
#include "RenderResourceRegistry.hpp"
#include "RenderSceneView.hpp"
#include "RenderTypes.hpp"
#include "SceneGeometryRenderer.hpp"
#include "ShadowSystem.hpp"

namespace render {

class IRenderPath;
class SceneOverlayRenderer;

class RenderEngine {
public:
    RenderEngine(int width, int height, std::string title = "AlKanzar - Render Preview");
    ~RenderEngine();

    RenderEngine(const RenderEngine&) = delete;
    RenderEngine& operator=(const RenderEngine&) = delete;

    bool init();
    void processEvent(const SDL_Event& event);
    void resize(int width, int height);
    void beginImGuiFrame();
    void renderImGui();
    void present() const;
    [[nodiscard]] bool wantsMouse() const;
    [[nodiscard]] bool wantsKeyboard() const;
    void renderFrame(
        const core::FrameSceneData& frame,
        const CameraMatrices& camera,
        const RenderFrameOptions& options
    );
    [[nodiscard]] MeshHandle uploadMesh(const Mesh& mesh);
    std::shared_ptr<Texture> registerTexture(const std::shared_ptr<Texture>& texture);
    std::shared_ptr<Sampler> registerSampler(const std::shared_ptr<Sampler>& sampler);
    [[nodiscard]] const std::shared_ptr<Sampler>& defaultSampler() const { return resourceRegistry_.defaultSampler(); }
    [[nodiscard]] std::vector<std::shared_ptr<Texture>> textureCatalog(TextureSemantic preferredSemantic) const;
    [[nodiscard]] void* texturePreviewId(const std::shared_ptr<Texture>& texture);
    [[nodiscard]] int width() const { return width_; }
    [[nodiscard]] int height() const { return height_; }

private:
    bool initImGui();
    void shutdownImGui();
    MeshBuffer* createSceneMesh(const Mesh& mesh);

    SDL_Window* window_{nullptr};
    SDL_GLContext glContext_{nullptr};
    int width_;
    int height_;
    std::string title_;
    bool imguiReady_{false};

    std::vector<std::unique_ptr<MeshBuffer>> sceneMeshes_{};
    RenderResourceRegistry resourceRegistry_{};
    MaterialBinder materialBinder_{};
    SceneGeometryRenderer geometryRenderer_{};
    RenderLightPipeline lightPipeline_{};
    RenderSceneView sceneView_{};
    RenderLightPipeline::FrameState lightFrame_{};
    ShadowSystem shadowSystem_{};
    std::unique_ptr<SceneOverlayRenderer> overlayRenderer_{};
    std::unique_ptr<IRenderPath> renderPath_{};

    glm::vec3 directionalLightDirection_{0.0f, -1.0f, 0.0f};
    glm::vec3 directionalLightColor_{0.0f};
    float directionalLightIntensity_{0.0f};
};

}  // namespace render
