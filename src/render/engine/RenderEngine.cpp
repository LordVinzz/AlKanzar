#include "RenderEngine.hpp"

#include <SDL_opengl.h>

#include <utility>

#include <glm/geometric.hpp>
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
    resourceRegistry_.destroy();
    sceneMeshes_.clear();

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
        ALKANZAR_PROFILE_GPU_SCOPE(*profiler_, "ImGui Draw");
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

void RenderEngine::renderFrame(
    const core::FrameSceneData& frame,
    const CameraMatrices& camera,
    const RenderFrameOptions& options,
    core::TaskScheduler& scheduler,
    bool useParallelSceneView
) {
    if (!renderPath_ || !overlayRenderer_) {
        return;
    }

    std::vector<const MeshBuffer*> meshLookup;
    meshLookup.reserve(sceneMeshes_.size());
    for (const auto& mesh : sceneMeshes_) {
        meshLookup.push_back(mesh.get());
    }

    if (profiler_) {
        {
            ALKANZAR_PROFILE_SCOPE(*profiler_, "Scene View Build");
            sceneView_ = buildRenderSceneView(frame, meshLookup, scheduler, useParallelSceneView);
        }
        {
            ALKANZAR_PROFILE_SCOPE(*profiler_, "Light Frame Build");
            lightFrame_ = lightPipeline_.buildFrame(sceneView_, camera.view, shadowSystem_, renderPath_->usesDeferredLighting());
        }
    } else {
        sceneView_ = buildRenderSceneView(frame, meshLookup, scheduler, useParallelSceneView);
        lightFrame_ = lightPipeline_.buildFrame(sceneView_, camera.view, shadowSystem_, renderPath_->usesDeferredLighting());
    }

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
        profiler_,
        directionalLightDirection_,
        directionalLightColor_,
        directionalLightIntensity_,
    };
    if (profiler_) {
        ALKANZAR_PROFILE_SCOPE(*profiler_, "Render Path");
        renderPath_->render(context);
        return;
    }

    renderPath_->render(context);
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

}  // namespace render
