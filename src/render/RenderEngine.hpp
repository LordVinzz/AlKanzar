#ifndef GL_GLEXT_PROTOTYPES
#define GL_GLEXT_PROTOTYPES
#endif
#pragma once

#include <SDL.h>
#include <SDL_opengl.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <glm/mat3x3.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include "Geometry.hpp"
#include "Material.hpp"
#include "MeshBuffer.hpp"
#include "LightExecutionVolume.hpp"
#include "ShadowSystem.hpp"
#include "ShaderProgram.hpp"
#include "StaticGltfModel.hpp"

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
        Deferred41,
    };

    enum class MaterialTextureSlot {
        BaseColor = 0,
        MetallicRoughness,
        Normal,
        Ao,
        Emissive,
        Alpha,
        Clearcoat,
        DetailNormal,
        Height,
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

public:
    struct Bounds3 {
        glm::vec3 min{0.0f};
        glm::vec3 max{0.0f};
    };

private:
    struct TransformState {
        glm::vec3 position{0.0f};
        glm::vec3 rotationDeg{0.0f};
        glm::vec3 scale{1.0f};
    };

    enum class SceneObjectKind {
        Ground,
        Wall,
        Model,
    };

    struct SceneObject {
        int id{0};
        std::string name;
        std::string materialLabel;
        SceneObjectKind kind{SceneObjectKind::Model};
        RenderLayer renderLayer{RenderLayer::Geometry};
        MeshBuffer* mesh{nullptr};
        std::shared_ptr<Material> material;
        Bounds3 localBounds{};
        std::shared_ptr<TransformState> transform;
        bool visible{true};
    };

    enum class SelectedEntityType {
        SceneObject,
        Light,
    };

    enum class InspectorTab {
        Selection = 0,
        TextureBrowser,
    };

    struct SelectedEntity {
        SelectedEntityType type{SelectedEntityType::SceneObject};
        int index{-1};
    };

    struct EditorState {
        bool enabled{false};
        std::optional<SelectedEntity> selection{};
        InspectorTab activeInspectorTab{InspectorTab::Selection};
        MaterialTextureSlot textureBrowserSlot{MaterialTextureSlot::BaseColor};
        bool textureBrowserFocusRequested{false};
        char textureBrowserSearch[128]{};
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
        bool isMovable{false};
        bool castsShadow{false};
        float shadowBiasMin{0.0f};
        float shadowBiasSlope{0.0f};

        void setIndex(int index);
        void setMovableChangedCallback(std::function<void(int, bool)> callback);
        void setIsMovable(bool movable);

    private:
        int index_{-1};
        std::function<void(int, bool)> movableChangedCallback_{};
    };

    struct GpuLight {
        glm::vec4 positionRadius;
        glm::vec4 colorIntensity;
        glm::vec4 directionType;
        glm::vec4 spotParams;
        glm::vec4 shadowInfo;
    };

    struct DirectionalLightDebug {
        glm::vec3 direction{0.0f, -1.0f, 0.0f};
        glm::vec3 color{1.0f};
        float intensity{1.0f};
    };

    struct ActiveLightDebug {
        glm::vec3 position{0.0f};
        float radius{0.0f};
        glm::vec3 color{1.0f};
        float outerAngle{0.0f};
        glm::vec3 direction{0.0f, 0.0f, 1.0f};
        LightType type{LightType::Point};
    };

    struct MaterialUniformLocations {
        GLint baseColorFactor{-1};
        GLint metallicFactor{-1};
        GLint roughnessFactor{-1};
        GLint normalScale{-1};
        GLint aoStrength{-1};
        GLint emissiveFactor{-1};
        GLint emissiveStrength{-1};
        GLint alphaFactor{-1};
        GLint alphaMode{-1};
        GLint alphaCutoff{-1};
        GLint clearcoatFactor{-1};
        GLint clearcoatRoughness{-1};
        GLint detailNormalScale{-1};
        GLint heightScale{-1};
        GLint baseColorUvSet{-1};
        GLint metallicRoughnessUvSet{-1};
        GLint normalUvSet{-1};
        GLint aoUvSet{-1};
        GLint emissiveUvSet{-1};
        GLint alphaUvSet{-1};
        GLint clearcoatUvSet{-1};
        GLint detailNormalUvSet{-1};
        GLint heightUvSet{-1};
        GLint baseColorUvTransform{-1};
        GLint metallicRoughnessUvTransform{-1};
        GLint normalUvTransform{-1};
        GLint aoUvTransform{-1};
        GLint emissiveUvTransform{-1};
        GLint alphaUvTransform{-1};
        GLint clearcoatUvTransform{-1};
        GLint detailNormalUvTransform{-1};
        GLint heightUvTransform{-1};
    };

    bool initImGui();
    void shutdownImGui();
    void beginImGuiFrame();
    void renderEditorUi();
    void renderImGui();

    void handleEvent(const SDL_Event& event, bool& running);
    void updateProjection();
    void updateOrbitCamera();
    void detectLightingCapabilities();
    void renderScene();
    void buildScene();
    void buildLights();
    void updateLights();
    void ensureDeferredResources();
    void destroyDeferredResources();
    bool buildVolumeMeshes();
    bool buildDebugMeshes();
    void renderDeferredScene();
    void renderSimpleScene();
    void renderLightDebugOverlay(bool includeSelectedLight) const;
    void renderSelectionOverlay() const;
    void drawDebugMesh(const MeshBuffer& mesh, const glm::mat4& model, const glm::vec4& color, bool wireframe) const;
    void drawSceneObjectSimple(const SceneObject& object) const;
    void drawSceneObjectDeferred(const SceneObject& object) const;
    void drawSceneLayerSimple(RenderLayer layer) const;
    void drawSceneLayerDeferred(RenderLayer layer) const;
    std::vector<ShadowSystem::ShadowRenderable> collectShadowRenderables() const;
    void assignStaticLightToVolume(int lightIndex);
    void removeStaticLightFromVolumes(int lightIndex);
    void rebuildMovableAssignments(float timeSeconds);
    void handleLightMovableChanged(int lightIndex, bool isMovable);
    void evaluateLightTransform(const LightInstance& light, float timeSeconds, glm::vec3& position, glm::vec3& direction) const;
    glm::mat4 composeTransform(const TransformState& transform) const;
    glm::mat3 normalMatrixFromModel(const glm::mat4& model) const;
    Bounds3 transformBounds(const Bounds3& bounds, const glm::mat4& model) const;
    Bounds3 sceneObjectWorldBounds(const SceneObject& object) const;
    bool pickSceneEntity(int mouseX, int mouseY, SelectedEntity& outSelection) const;
    void handleViewportClick(int mouseX, int mouseY);
    const char* rendererPathName() const;
    std::string selectionSummary() const;
    float currentTimeSeconds() const;
    bool buildTextureLibrary();
    void destroyTextureLibrary();
    MeshBuffer* createSceneMesh(const Mesh& mesh);
    std::shared_ptr<Texture> registerTexture(const std::shared_ptr<Texture>& texture);
    std::shared_ptr<Sampler> registerSampler(const std::shared_ptr<Sampler>& sampler);
    bool ensureTextureUploaded(Texture& texture) const;
    bool ensureSamplerUploaded(Sampler& sampler) const;
    void bindTextureRef(int unit, const TextureRef& ref) const;
    const TextureRef& defaultTextureForSlot(MaterialTextureSlot slot) const;
    const TextureRef& defaultTextureForUnit(int unit) const;
    void prebindMaterialDefaults() const;
    std::vector<std::shared_ptr<Texture>> runtimeTextureCatalog(TextureSemantic preferredSemantic) const;
    void ensureInlineTexture(TextureRef& ref, const std::string& name, TextureSemantic semantic, const glm::vec4& value);
    TextureSemantic textureSemanticForSlot(MaterialTextureSlot slot) const;
    glm::vec4 defaultInlineValueForSlot(MaterialTextureSlot slot) const;
    const char* materialTextureSlotName(MaterialTextureSlot slot) const;
    TextureRef* textureRefForSlot(Material& material, MaterialTextureSlot slot) const;
    void openTextureBrowser(MaterialTextureSlot slot);
    void renderTextureBrowserTab(Material& material);
    bool drawTextureSlotEditor(
        const char* label,
        const std::string& materialName,
        MaterialTextureSlot slot,
        TextureRef& ref,
        const TextureRef& resolved
    );
    ShaderInputs resolveMaterialInputs(const Material& material) const;
    void bindMaterialUniforms(const ShaderInputs& inputs, const MaterialUniformLocations& locations) const;
    void configureMaterialRasterState(const ShaderInputs& inputs) const;
    std::shared_ptr<Material> createProceduralMaterial(
        const std::string& name,
        const std::shared_ptr<Texture>& baseColor,
        const std::shared_ptr<Texture>& normal,
        const std::shared_ptr<Texture>& metallicRoughness,
        const std::shared_ptr<Texture>& ao,
        const std::shared_ptr<Texture>& height,
        const glm::vec3& tint,
        const glm::vec2& uvScale,
        float clearcoatFactor,
        float clearcoatRoughness,
        float detailNormalScale
    );
    void appendModelObjects(
        const std::string& modelName,
        SceneObjectKind kind,
        RenderLayer layer,
        const StaticModelData& model,
        const std::shared_ptr<TransformState>& transformState,
        int& nextId
    );

    SDL_Window* window_{nullptr};
    SDL_GLContext glContext_{nullptr};
    int width_;
    int height_;
    float zoom_{1.0f};
    float panX_{0.0f};
    float panY_{0.0f};
    float cameraDistance_{15.0f};
    float orbitYawDeg_{45.0f};
    bool orbitCameraEnabled_{false};
    Uint32 lastOrbitTickMs_{0};
    bool middleDragging_{false};
    int lastMouseX_{0};
    int lastMouseY_{0};
    std::string title_;
    bool imguiReady_{false};

    ShaderProgram simpleShader_;
    ShaderProgram deferredGeometryShader_;
    ShaderProgram deferredDirLightShader_;
    ShaderProgram deferredVolumeShader_;
    ShaderProgram deferredCompositeShader_;
    ShaderProgram debugColorShader_;
    MeshBuffer lightSphere_;
    MeshBuffer lightCone_;
    MeshBuffer axisGizmo_;
    MeshBuffer selectionBox_;
    GLint simpleModelLocation_{-1};
    GLint simpleViewLocation_{-1};
    GLint simpleProjLocation_{-1};
    GLint simpleNormalMatrixLocation_{-1};
    GLint simpleLightDirLocation_{-1};
    MaterialUniformLocations simpleMaterialLocations_{};
    GLint gbufferModelLocation_{-1};
    GLint gbufferViewLocation_{-1};
    GLint gbufferProjLocation_{-1};
    GLint gbufferNormalMatrixLocation_{-1};
    MaterialUniformLocations gbufferMaterialLocations_{};
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
    GLint debugMvpLocation_{-1};
    GLint debugColorLocation_{-1};

    GLuint gbufferFbo_{0};
    GLuint gbufferAlbedo_{0};
    GLuint gbufferNormal_{0};
    GLuint gbufferEmissiveAo_{0};
    GLuint gbufferClearcoat_{0};
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
    bool showLightDebug_{false};
    bool cameraInsideLightVolume_{false};
    int shadowDebugCascade_{0};

    ShadowSystem shadowSystem_{};
    DirectionalLightDebug directionalLight_{};

    std::vector<std::shared_ptr<Texture>> textures_;
    std::vector<std::shared_ptr<Sampler>> samplers_;
    std::vector<std::unique_ptr<MeshBuffer>> sceneMeshes_;
    std::shared_ptr<Sampler> defaultSampler_{};
    TextureRef defaultBaseColorTexture_{};
    TextureRef defaultNormalTexture_{};
    TextureRef defaultMetallicRoughnessTexture_{};
    TextureRef defaultAoTexture_{};
    TextureRef defaultEmissiveTexture_{};
    TextureRef defaultAlphaTexture_{};
    TextureRef defaultClearcoatTexture_{};
    TextureRef defaultDetailNormalTexture_{};
    TextureRef defaultHeightTexture_{};

    std::vector<LightInstance> lights_;
    std::vector<GpuLight> gpuLights_;
    std::vector<ActiveLightDebug> lightDebugInstances_;
    std::vector<int> activeLightIndices_;
    std::vector<LightExecutionVolume> lightVolumes_;
    std::vector<SceneObject> sceneObjects_;
    EditorState editorState_{};
    bool movableAssignmentsDirty_{false};

    glm::mat4 projection_{1.0f};
    glm::mat4 invProjection_{1.0f};
    glm::mat4 view_{1.0f};
    bool sceneReady_{false};
};

}  // namespace render
