#include "RenderEngine.hpp"

#include <SDL_opengl.h>

#include <array>
#include <cstdint>
#include <limits>
#include <utility>

#include <glm/geometric.hpp>
#include <glm/ext/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <imgui.h>
#include <backends/imgui_impl_opengl3.h>
#include <backends/imgui_impl_sdl2.h>
#include <spdlog/spdlog.h>

#include "core/profiling/ProfilerService.hpp"
#include "RenderPaths.hpp"
#include "render/pipeline/SceneOverlayRenderer.hpp"

namespace {

std::string shaderRootPath() {
    char* basePath = SDL_GetBasePath();
    if (!basePath) {
        spdlog::warn("RenderEngine: SDL_GetBasePath failed: {}", SDL_GetError());
        return "build/shaders/";
    }
    std::string root = std::string(basePath) + "shaders/";
    SDL_free(basePath);
    return root;
}

struct BoundsBoxVertex {
    glm::vec3 position{0.0f};
    glm::vec3 normal{0.0f, 1.0f, 0.0f};
    glm::vec2 uv0{0.0f};
    glm::vec2 uv1{0.0f};
    glm::vec4 color{1.0f};
};

void pushBoundsBoxVertex(const BoundsBoxVertex& vertex, render::Mesh& outMesh) {
    outMesh.positions.push_back(vertex.position);
    outMesh.normals.push_back(vertex.normal);
    outMesh.colors.push_back(vertex.color);
    if (outMesh.uvSets.size() < 2) {
        outMesh.uvSets.resize(2);
    }
    outMesh.uvSets[0].push_back(vertex.uv0);
    outMesh.uvSets[1].push_back(vertex.uv1);
}

void addBoundsBoxQuad(const std::array<BoundsBoxVertex, 4>& verts, render::Mesh& outMesh) {
    const unsigned int base = static_cast<unsigned int>(outMesh.positions.size());
    for (const auto& vertex : verts) {
        pushBoundsBoxVertex(vertex, outMesh);
    }
    outMesh.indices.insert(outMesh.indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
}

void addBoundsBox(render::Mesh& outMesh) {
    const glm::vec3 minCorner(-0.5f);
    const glm::vec3 maxCorner(0.5f);
    const glm::vec4 color(1.0f);
    const float minX = minCorner.x;
    const float minY = minCorner.y;
    const float minZ = minCorner.z;
    const float maxX = maxCorner.x;
    const float maxY = maxCorner.y;
    const float maxZ = maxCorner.z;

    addBoundsBoxQuad({{
        {glm::vec3(maxX, minY, minZ), glm::vec3(1.0f, 0.0f, 0.0f), {}, {}, color},
        {glm::vec3(maxX, maxY, minZ), glm::vec3(1.0f, 0.0f, 0.0f), {}, {}, color},
        {glm::vec3(maxX, maxY, maxZ), glm::vec3(1.0f, 0.0f, 0.0f), {}, {}, color},
        {glm::vec3(maxX, minY, maxZ), glm::vec3(1.0f, 0.0f, 0.0f), {}, {}, color},
    }}, outMesh);
    addBoundsBoxQuad({{
        {glm::vec3(minX, minY, minZ), glm::vec3(-1.0f, 0.0f, 0.0f), {}, {}, color},
        {glm::vec3(minX, minY, maxZ), glm::vec3(-1.0f, 0.0f, 0.0f), {}, {}, color},
        {glm::vec3(minX, maxY, maxZ), glm::vec3(-1.0f, 0.0f, 0.0f), {}, {}, color},
        {glm::vec3(minX, maxY, minZ), glm::vec3(-1.0f, 0.0f, 0.0f), {}, {}, color},
    }}, outMesh);
    addBoundsBoxQuad({{
        {glm::vec3(minX, maxY, minZ), glm::vec3(0.0f, 1.0f, 0.0f), {}, {}, color},
        {glm::vec3(minX, maxY, maxZ), glm::vec3(0.0f, 1.0f, 0.0f), {}, {}, color},
        {glm::vec3(maxX, maxY, maxZ), glm::vec3(0.0f, 1.0f, 0.0f), {}, {}, color},
        {glm::vec3(maxX, maxY, minZ), glm::vec3(0.0f, 1.0f, 0.0f), {}, {}, color},
    }}, outMesh);
    addBoundsBoxQuad({{
        {glm::vec3(minX, minY, minZ), glm::vec3(0.0f, -1.0f, 0.0f), {}, {}, color},
        {glm::vec3(maxX, minY, minZ), glm::vec3(0.0f, -1.0f, 0.0f), {}, {}, color},
        {glm::vec3(maxX, minY, maxZ), glm::vec3(0.0f, -1.0f, 0.0f), {}, {}, color},
        {glm::vec3(minX, minY, maxZ), glm::vec3(0.0f, -1.0f, 0.0f), {}, {}, color},
    }}, outMesh);
    addBoundsBoxQuad({{
        {glm::vec3(minX, minY, maxZ), glm::vec3(0.0f, 0.0f, 1.0f), {}, {}, color},
        {glm::vec3(maxX, minY, maxZ), glm::vec3(0.0f, 0.0f, 1.0f), {}, {}, color},
        {glm::vec3(maxX, maxY, maxZ), glm::vec3(0.0f, 0.0f, 1.0f), {}, {}, color},
        {glm::vec3(minX, maxY, maxZ), glm::vec3(0.0f, 0.0f, 1.0f), {}, {}, color},
    }}, outMesh);
    addBoundsBoxQuad({{
        {glm::vec3(minX, minY, minZ), glm::vec3(0.0f, 0.0f, -1.0f), {}, {}, color},
        {glm::vec3(minX, maxY, minZ), glm::vec3(0.0f, 0.0f, -1.0f), {}, {}, color},
        {glm::vec3(maxX, maxY, minZ), glm::vec3(0.0f, 0.0f, -1.0f), {}, {}, color},
        {glm::vec3(maxX, minY, minZ), glm::vec3(0.0f, 0.0f, -1.0f), {}, {}, color},
    }}, outMesh);
}

}  // namespace

namespace render {

RenderEngine::RenderEngine(int width, int height, std::string title)
    : width_(width),
      height_(height),
      title_(std::move(title)),
      directionalLightDirection_(glm::normalize(glm::vec3(-0.3f, -1.0f, -0.4f))) {}

RenderEngine::~RenderEngine() {
    shutdownImGui();
    renderPath_.reset();
    overlayRenderer_.reset();
    shadowSystem_.destroy();
    for (auto& [entity, state] : occlusionQueryStates_) {
        if (state.queryId != 0) {
            glDeleteQueries(1, &state.queryId);
            state.queryId = 0;
        }
    }
    for (GLuint queryId : freeOcclusionQueries_) {
        if (queryId != 0) {
            glDeleteQueries(1, &queryId);
        }
    }
    resourceRegistry_.destroy();
    sceneMeshes_.clear();
    if (jointMatrixTexture_ != 0) {
        glDeleteTextures(1, &jointMatrixTexture_);
        jointMatrixTexture_ = 0;
    }
    if (jointMatrixBuffer_ != 0) {
        glDeleteBuffers(1, &jointMatrixBuffer_);
        jointMatrixBuffer_ = 0;
    }
    jointMatrixBufferSize_ = 0;

    if (glContext_) {
        SDL_GL_DeleteContext(glContext_);
        glContext_ = nullptr;
    }
    if (window_) {
        SDL_DestroyWindow(window_);
        window_ = nullptr;
    }
}

bool RenderEngine::initImGui() {
    if (imguiReady_) {
        return true;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();

    if (!ImGui_ImplSDL2_InitForOpenGL(window_, glContext_)) {
        spdlog::error("RenderEngine: failed to init ImGui SDL2 backend");
        ImGui::DestroyContext();
        return false;
    }
    if (!ImGui_ImplOpenGL3_Init("#version 410")) {
        spdlog::error("RenderEngine: failed to init ImGui OpenGL backend");
        ImGui_ImplSDL2_Shutdown();
        ImGui::DestroyContext();
        return false;
    }

    imguiReady_ = true;
    return true;
}

void RenderEngine::shutdownImGui() {
    if (!imguiReady_) {
        return;
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    imguiReady_ = false;
}

bool RenderEngine::initializeOcclusionResources(const std::string& shaderRoot) {
    if (!occlusionShader_.buildFromFiles(shaderRoot + "debug_color.vert", shaderRoot + "debug_color.frag")) {
        spdlog::error("RenderEngine: failed to initialize occlusion shader");
        return false;
    }

    render::Mesh boundsMesh;
    boundsMesh.uvSets.resize(2);
    addBoundsBox(boundsMesh);
    if (!occlusionBoundsMesh_.upload(boundsMesh)) {
        spdlog::error("RenderEngine: failed to upload occlusion bounds mesh");
        return false;
    }

    occlusionMvpLocation_ = occlusionShader_.uniformLocation("uMVP");
    occlusionColorLocation_ = occlusionShader_.uniformLocation("uColor");
    return true;
}

void RenderEngine::beginImGuiFrame() {
    if (!imguiReady_) {
        return;
    }

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplSDL2_NewFrame();
    ImGui::NewFrame();
}

void RenderEngine::renderImGui() {
    if (!imguiReady_) {
        return;
    }

    if (profiler_) {
        ALKANZAR_PROFILE_SCOPE(*profiler_, "ImGui Draw");
        ImGui::Render();
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        return;
    }

    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

void RenderEngine::processEvent(const SDL_Event& event) {
    if (imguiReady_) {
        ImGui_ImplSDL2_ProcessEvent(&event);
    }
}

void RenderEngine::resize(int width, int height) {
    width_ = width;
    height_ = height;
    glViewport(0, 0, width_, height_);
    if (renderPath_) {
        renderPath_->resize(width_, height_);
    }
}

void RenderEngine::present() const {
    if (window_) {
        SDL_GL_SwapWindow(window_);
    }
}

bool RenderEngine::wantsMouse() const {
    return imguiReady_ && ImGui::GetIO().WantCaptureMouse;
}

bool RenderEngine::wantsKeyboard() const {
    return imguiReady_ && ImGui::GetIO().WantCaptureKeyboard;
}

bool RenderEngine::init() {
    if (window_) {
        return true;
    }

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);

    window_ = SDL_CreateWindow(
        title_.c_str(),
        SDL_WINDOWPOS_CENTERED,
        SDL_WINDOWPOS_CENTERED,
        width_,
        height_,
        SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE
    );
    if (!window_) {
        spdlog::error("RenderEngine: unable to create window: {}", SDL_GetError());
        return false;
    }

    glContext_ = SDL_GL_CreateContext(window_);
    if (!glContext_) {
        spdlog::error("RenderEngine: unable to create GL 4.1 context: {}", SDL_GetError());
        SDL_DestroyWindow(window_);
        window_ = nullptr;
        return false;
    }

    SDL_GL_MakeCurrent(window_, glContext_);
    SDL_GL_SetSwapInterval(1);

    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    glEnable(GL_TEXTURE_CUBE_MAP_SEAMLESS);
    glClearColor(0.10f, 0.10f, 0.12f, 1.0f);

    if (!initImGui()) {
        return false;
    }
    if (!resourceRegistry_.initializeDefaults()) {
        spdlog::error("RenderEngine: failed to initialize default material resources");
        return false;
    }

    overlayRenderer_ = std::make_unique<SceneOverlayRenderer>();
    if (!overlayRenderer_->init(shaderRootPath())) {
        spdlog::error("RenderEngine: failed to initialize overlay renderer");
        overlayRenderer_.reset();
        return false;
    }
    if (!initializeOcclusionResources(shaderRootPath())) {
        return false;
    }

    GLint major = 0;
    GLint minor = 0;
    glGetIntegerv(GL_MAJOR_VERSION, &major);
    glGetIntegerv(GL_MINOR_VERSION, &minor);

    if (major > 4 || (major == 4 && minor >= 1)) {
        renderPath_ = std::make_unique<DeferredRenderPath>();
        spdlog::info("RenderEngine: using deferred path (GL {}.{})", major, minor);
    } else {
        renderPath_ = std::make_unique<SimpleForwardPath>();
        spdlog::warn("RenderEngine: deferred path unavailable (GL {}.{})", major, minor);
    }

    if (!renderPath_->init(shaderRootPath(), materialBinder_, shadowSystem_)) {
        spdlog::error("RenderEngine: failed to initialize render path '{}'", renderPath_->name());
        renderPath_.reset();
        return false;
    }

    materialBinder_.prebindDefaults(resourceRegistry_);
    renderPath_->resize(width_, height_);
    return true;
}

MeshBuffer* RenderEngine::createSceneMesh(const Mesh& mesh) {
    auto buffer = std::make_unique<MeshBuffer>();
    if (!buffer->upload(mesh)) {
        return nullptr;
    }

    MeshBuffer* raw = buffer.get();
    sceneMeshes_.push_back(std::move(buffer));
    return raw;
}

MeshHandle RenderEngine::uploadMesh(const Mesh& mesh) {
    MeshBuffer* uploaded = createSceneMesh(mesh);
    if (!uploaded) {
        return {};
    }
    return MeshHandle{sceneMeshes_.size() - 1u};
}

void RenderEngine::uploadJointMatrices(const std::vector<glm::mat4>& jointMatrices) {
    const GLsizeiptr bufferSize = static_cast<GLsizeiptr>(jointMatrices.size() * sizeof(glm::mat4));
    if (bufferSize == 0) {
        jointMatrixBufferSize_ = 0;
        return;
    }

    if (jointMatrixBuffer_ == 0) {
        glGenBuffers(1, &jointMatrixBuffer_);
    }
    if (jointMatrixTexture_ == 0) {
        glGenTextures(1, &jointMatrixTexture_);
    }

    glBindBuffer(GL_TEXTURE_BUFFER, jointMatrixBuffer_);
    if (jointMatrixBufferSize_ != bufferSize) {
        glBufferData(GL_TEXTURE_BUFFER, bufferSize, jointMatrices.data(), GL_DYNAMIC_DRAW);
        jointMatrixBufferSize_ = bufferSize;
    } else {
        glBufferSubData(GL_TEXTURE_BUFFER, 0, bufferSize, jointMatrices.data());
    }

    glBindTexture(GL_TEXTURE_BUFFER, jointMatrixTexture_);
    glTexBuffer(GL_TEXTURE_BUFFER, GL_RGBA32F, jointMatrixBuffer_);
}

std::shared_ptr<Texture> RenderEngine::registerTexture(const std::shared_ptr<Texture>& texture) {
    return resourceRegistry_.registerTexture(texture);
}

std::shared_ptr<Sampler> RenderEngine::registerSampler(const std::shared_ptr<Sampler>& sampler) {
    return resourceRegistry_.registerSampler(sampler);
}

std::vector<std::shared_ptr<Texture>> RenderEngine::textureCatalog(TextureSemantic preferredSemantic) const {
    return resourceRegistry_.textureCatalog(preferredSemantic);
}

void* RenderEngine::texturePreviewId(const std::shared_ptr<Texture>& texture) {
    return resourceRegistry_.texturePreviewId(texture);
}

void RenderEngine::pollOcclusionQueries() {
    for (auto& [entity, state] : occlusionQueryStates_) {
        (void)entity;
        if (!state.queryInFlight || state.queryId == 0) {
            continue;
        }

        GLuint available = 0;
        glGetQueryObjectuiv(state.queryId, GL_QUERY_RESULT_AVAILABLE, &available);
        if (!available) {
            continue;
        }

        GLuint passed = 0;
        glGetQueryObjectuiv(state.queryId, GL_QUERY_RESULT, &passed);
        if (passed != 0) {
            state.lastVisible = true;
            state.occludedFrameStreak = 0;
        } else {
            state.lastVisible = false;
            if (state.occludedFrameStreak < std::numeric_limits<std::uint8_t>::max()) {
                state.occludedFrameStreak++;
            }
        }
        state.hasLastResult = true;
        state.queryInFlight = false;
        recycleOcclusionQuery(state.queryId);
        state.queryId = 0;
    }
}

void RenderEngine::cleanupOcclusionStates() {
    constexpr std::uint64_t kMaxInactiveFrames = 3u;
    for (auto it = occlusionQueryStates_.begin(); it != occlusionQueryStates_.end();) {
        if (renderFrameNumber_ > it->second.lastFrameTouched &&
            renderFrameNumber_ - it->second.lastFrameTouched > kMaxInactiveFrames) {
            if (it->second.queryId != 0) {
                recycleOcclusionQuery(it->second.queryId);
            }
            it = occlusionQueryStates_.erase(it);
            continue;
        }
        ++it;
    }
}

void RenderEngine::recycleOcclusionQuery(GLuint queryId) {
    if (queryId != 0) {
        freeOcclusionQueries_.push_back(queryId);
    }
}

void RenderEngine::issueOcclusionQueries(const RenderPathContext& context) {
    if (occlusionShader_.id() == 0 || !occlusionBoundsMesh_.valid()) {
        latestOcclusionCullStats_.queryIssued = 0u;
        return;
    }

    renderPath_->prepareOcclusionQueryPass(context);
    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
    glDepthMask(GL_FALSE);
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);

    const glm::mat4 viewProjection = context.camera.projection * context.camera.view;
    occlusionShader_.use();
    glUniform4f(occlusionColorLocation_, 1.0f, 1.0f, 1.0f, 1.0f);

    std::uint32_t issued = 0u;
    for (const RenderSceneObjectView& object : context.scene.objects) {
        if (!object.visible || !object.frustumVisible || !object.hasWorldBounds || object.layer == RenderLayer::Ground) {
            continue;
        }

        OcclusionQueryState& state = occlusionQueryStates_[object.entity];
        state.lastFrameTouched = renderFrameNumber_;
        if (state.queryInFlight) {
            continue;
        }

        if (state.queryId == 0) {
            if (!freeOcclusionQueries_.empty()) {
                state.queryId = freeOcclusionQueries_.back();
                freeOcclusionQueries_.pop_back();
            } else {
                glGenQueries(1, &state.queryId);
            }
        }
        if (state.queryId == 0) {
            continue;
        }

        glm::mat4 model(1.0f);
        const glm::vec3 center = (object.worldBounds.min + object.worldBounds.max) * 0.5f;
        const glm::vec3 extents = glm::max((object.worldBounds.max - object.worldBounds.min) * 0.5f, glm::vec3(0.01f));
        model = glm::translate(model, center);
        model = glm::scale(model, extents * 2.0f);
        const glm::mat4 mvp = viewProjection * model;
        glUniformMatrix4fv(occlusionMvpLocation_, 1, GL_FALSE, glm::value_ptr(mvp));

        glBeginQuery(GL_ANY_SAMPLES_PASSED, state.queryId);
        occlusionBoundsMesh_.draw();
        glEndQuery(GL_ANY_SAMPLES_PASSED);

        state.queryInFlight = true;
        issued++;
    }

    sceneView_.occlusionCullStats.queryIssued = issued;
    latestOcclusionCullStats_.queryIssued = issued;
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDepthMask(GL_TRUE);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, context.width, context.height);
}

void RenderEngine::renderFrame(
    const core::FrameSceneData& frame,
    const CameraMatrices& camera,
    const RenderFrameOptions& options,
    core::TaskScheduler& scheduler,
    bool useParallelSceneView
) {
    renderFrameNumber_++;
    if (!renderPath_ || !overlayRenderer_) {
        latestFrustumCullStats_ = FrustumCullStats{};
        latestOcclusionCullStats_ = OcclusionCullStats{};
        return;
    }

    std::vector<const MeshBuffer*> meshLookup;
    meshLookup.reserve(sceneMeshes_.size());
    for (const auto& mesh : sceneMeshes_) {
        meshLookup.push_back(mesh.get());
    }

    const auto buildOcclusionCache = [&]() {
        std::unordered_map<core::EntityId, OcclusionCullCacheState> cache;
        cache.reserve(occlusionQueryStates_.size());
        for (const auto& [entity, state] : occlusionQueryStates_) {
            cache.emplace(entity, OcclusionCullCacheState{
                state.queryInFlight,
                state.hasLastResult,
                state.lastVisible,
                state.occludedFrameStreak
            });
        }
        return cache;
    };

    if (profiler_) {
        {
            ALKANZAR_PROFILE_SCOPE(*profiler_, "Scene View Build");
            sceneView_ = buildRenderSceneView(frame, meshLookup, scheduler, useParallelSceneView);
        }
        {
            ALKANZAR_PROFILE_SCOPE(*profiler_, "Frustum Cull");
            applyCameraFrustumCulling(sceneView_, camera);
            latestFrustumCullStats_ = sceneView_.frustumCullStats;
        }
        {
            ALKANZAR_PROFILE_SCOPE(*profiler_, "Occlusion Cull");
            pollOcclusionQueries();
            const auto occlusionCache = buildOcclusionCache();
            applyLastKnownOcclusionVisibility(sceneView_, occlusionCache);
            latestOcclusionCullStats_ = sceneView_.occlusionCullStats;
        }
        {
            ALKANZAR_PROFILE_SCOPE(*profiler_, "Joint Palette Upload");
            uploadJointMatrices(frame.jointMatrices);
        }
        {
            ALKANZAR_PROFILE_SCOPE(*profiler_, "Light Frame Build");
            lightFrame_ = lightPipeline_.buildFrame(sceneView_, camera.view, shadowSystem_, renderPath_->usesDeferredLighting());
        }
    } else {
        sceneView_ = buildRenderSceneView(frame, meshLookup, scheduler, useParallelSceneView);
        applyCameraFrustumCulling(sceneView_, camera);
        latestFrustumCullStats_ = sceneView_.frustumCullStats;
        pollOcclusionQueries();
        const auto occlusionCache = buildOcclusionCache();
        applyLastKnownOcclusionVisibility(sceneView_, occlusionCache);
        latestOcclusionCullStats_ = sceneView_.occlusionCullStats;
        uploadJointMatrices(frame.jointMatrices);
        lightFrame_ = lightPipeline_.buildFrame(sceneView_, camera.view, shadowSystem_, renderPath_->usesDeferredLighting());
    }

    for (const RenderSceneObjectView& object : sceneView_.objects) {
        if (object.layer == RenderLayer::Ground) {
            continue;
        }
        OcclusionQueryState& state = occlusionQueryStates_[object.entity];
        state.lastFrameTouched = renderFrameNumber_;
    }
    cleanupOcclusionStates();

    const RenderPathContext context{
        width_,
        height_,
        camera,
        options,
        sceneView_,
        lightFrame_,
        lightPipeline_,
        resourceRegistry_,
        materialBinder_,
        geometryRenderer_,
        *overlayRenderer_,
        shadowSystem_,
        jointMatrixTexture_,
        profiler_,
        directionalLightDirection_,
        directionalLightColor_,
        directionalLightIntensity_,
    };
    if (profiler_) {
        {
            ALKANZAR_PROFILE_SCOPE(*profiler_, "Render Path");
            renderPath_->render(context);
        }
        {
            ALKANZAR_PROFILE_SCOPE(*profiler_, "Occlusion Cull");
            issueOcclusionQueries(context);
        }
        return;
    }

    renderPath_->render(context);
    issueOcclusionQueries(context);
}

std::vector<ResourceMemoryRecord> RenderEngine::profilingResources() const {
    std::vector<ResourceMemoryRecord> resources{};

    for (std::size_t index = 0; index < sceneMeshes_.size(); ++index) {
        const std::unique_ptr<MeshBuffer>& mesh = sceneMeshes_[index];
        if (!mesh || mesh->gpuBytes() == 0u) {
            continue;
        }
        resources.push_back(ResourceMemoryRecord{
            "Scene Mesh " + std::to_string(index),
            "Mesh Buffer",
            0u,
            mesh->gpuBytes()
        });
    }

    if (jointMatrixBuffer_ != 0 && jointMatrixBufferSize_ > 0) {
        resources.push_back(ResourceMemoryRecord{
            "Joint Matrix Buffer",
            "Animation Buffer",
            0u,
            static_cast<std::uint64_t>(jointMatrixBufferSize_)
        });
    }

    for (const std::shared_ptr<Texture>& texture : resourceRegistry_.profilingTextures()) {
        if (!texture || !texture->valid()) {
            continue;
        }
        resources.push_back(ResourceMemoryRecord{
            texture->name,
            "Texture",
            estimateTextureCpuBytes(*texture),
            estimateTextureGpuBytes(*texture)
        });
    }

    const std::vector<ResourceMemoryRecord> shadowResources = shadowSystem_.profilingResources();
    resources.insert(resources.end(), shadowResources.begin(), shadowResources.end());

    if (renderPath_) {
        const std::vector<ResourceMemoryRecord> pathResources = renderPath_->profilingResources();
        resources.insert(resources.end(), pathResources.begin(), pathResources.end());
    }

    return resources;
}

std::vector<FrameCounterRecord> RenderEngine::profilingCounters() const {
    return {
        FrameCounterRecord{"Total Renderables", static_cast<std::int64_t>(latestFrustumCullStats_.totalRenderables), "Frustum Culling"},
        FrameCounterRecord{"Visibility Hidden", static_cast<std::int64_t>(latestFrustumCullStats_.visibilityHidden), "Frustum Culling"},
        FrameCounterRecord{"Bounds Tested", static_cast<std::int64_t>(latestFrustumCullStats_.boundsTested), "Frustum Culling"},
        FrameCounterRecord{"Passed", static_cast<std::int64_t>(latestFrustumCullStats_.frustumPassed), "Frustum Culling"},
        FrameCounterRecord{"Culled", static_cast<std::int64_t>(latestFrustumCullStats_.frustumCulled), "Frustum Culling"},
        FrameCounterRecord{"No Bounds Bypass", static_cast<std::int64_t>(latestFrustumCullStats_.noBoundsBypass), "Frustum Culling"},
        FrameCounterRecord{"Main Pass Visible", static_cast<std::int64_t>(latestFrustumCullStats_.mainPassVisible), "Frustum Culling"},
        FrameCounterRecord{"Candidates", static_cast<std::int64_t>(latestOcclusionCullStats_.candidates), "Occlusion Culling"},
        FrameCounterRecord{"Query Issued", static_cast<std::int64_t>(latestOcclusionCullStats_.queryIssued), "Occlusion Culling"},
        FrameCounterRecord{"Visible", static_cast<std::int64_t>(latestOcclusionCullStats_.visible), "Occlusion Culling"},
        FrameCounterRecord{"Occluded", static_cast<std::int64_t>(latestOcclusionCullStats_.occluded), "Occlusion Culling"},
        FrameCounterRecord{"No Bounds Bypass", static_cast<std::int64_t>(latestOcclusionCullStats_.noBoundsBypass), "Occlusion Culling"},
        FrameCounterRecord{"Warmup Visible", static_cast<std::int64_t>(latestOcclusionCullStats_.warmupVisible), "Occlusion Culling"},
        FrameCounterRecord{"Pending Reused", static_cast<std::int64_t>(latestOcclusionCullStats_.pendingReused), "Occlusion Culling"},
    };
}

}  // namespace render
