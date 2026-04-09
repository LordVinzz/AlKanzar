#include "RenderEngine.hpp"

#include <SDL_opengl.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <limits>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_inverse.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <imgui.h>
#include <backends/imgui_impl_opengl3.h>
#include <backends/imgui_impl_sdl2.h>
#include <spdlog/spdlog.h>

#include "StaticGltfModel.hpp"

namespace {

constexpr float kIsoAngleX = 35.264f;
constexpr float kIsoAngleY = 45.0f;
constexpr float kBaseOrthoSize = 10.0f;
constexpr float kMinZoom = 0.2f;
constexpr float kMaxZoom = 5.0f;
constexpr float kNearPlane = 0.0f;
constexpr float kFarPlane = 100.0f;
constexpr float kOrbitSpeedDegPerSecond = 40.0f;
constexpr float kAxisLength = 1.0f;
constexpr float kAxisThickness = 0.035f;
constexpr float kAxisCenterHalfExtent = 0.055f;
constexpr float kLightGizmoScaleMin = 0.45f;
constexpr float kLightGizmoScaleMax = 1.25f;
constexpr float kLightGizmoScaleFactor = 0.12f;
constexpr float kDirectionalDebugAnchorDistance = 7.5f;
constexpr float kDebugVolumeAlpha = 0.85f;
constexpr float kSelectionBoundsAlpha = 0.95f;
constexpr float kSelectionAxisScaleMin = 0.55f;
constexpr float kSelectionAxisScaleMax = 1.35f;
constexpr float kSelectionScaleFactor = 0.2f;

struct Vertex {
    glm::vec3 position{0.0f};
    glm::vec3 normal{0.0f, 1.0f, 0.0f};
    glm::vec2 uv0{0.0f};
    glm::vec2 uv1{0.0f};
    glm::vec4 color{1.0f};
};

struct Ray {
    glm::vec3 origin{0.0f};
    glm::vec3 direction{0.0f, 0.0f, -1.0f};
};

void pushVertex(const Vertex& vertex, render::Mesh& outMesh) {
    outMesh.positions.push_back(vertex.position);
    outMesh.normals.push_back(vertex.normal);
    outMesh.colors.push_back(vertex.color);
    if (outMesh.uvSets.size() < 2) {
        outMesh.uvSets.resize(2);
    }
    outMesh.uvSets[0].push_back(vertex.uv0);
    outMesh.uvSets[1].push_back(vertex.uv1);
}

void addQuad(const std::array<Vertex, 4>& verts, render::Mesh& outMesh) {
    const unsigned int base = static_cast<unsigned int>(outMesh.positions.size());
    for (const auto& v : verts) {
        pushVertex(v, outMesh);
    }
    outMesh.indices.insert(outMesh.indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
}

void addBox(
    const glm::vec3& minCorner,
    const glm::vec3& maxCorner,
    const glm::vec4& color,
    render::Mesh& outMesh
) {
    const float minX = minCorner.x;
    const float minY = minCorner.y;
    const float minZ = minCorner.z;
    const float maxX = maxCorner.x;
    const float maxY = maxCorner.y;
    const float maxZ = maxCorner.z;
    const std::array<Vertex, 4> rightFace{{
        {glm::vec3(maxX, minY, minZ), glm::vec3(1.0f, 0.0f, 0.0f), glm::vec2(0.0f, 0.0f), glm::vec2(0.0f), color},
        {glm::vec3(maxX, maxY, minZ), glm::vec3(1.0f, 0.0f, 0.0f), glm::vec2(0.0f, 1.0f), glm::vec2(0.0f), color},
        {glm::vec3(maxX, maxY, maxZ), glm::vec3(1.0f, 0.0f, 0.0f), glm::vec2(1.0f, 1.0f), glm::vec2(0.0f), color},
        {glm::vec3(maxX, minY, maxZ), glm::vec3(1.0f, 0.0f, 0.0f), glm::vec2(1.0f, 0.0f), glm::vec2(0.0f), color},
    }};
    addQuad(rightFace, outMesh);

    const std::array<Vertex, 4> leftFace{{
        {glm::vec3(minX, minY, minZ), glm::vec3(-1.0f, 0.0f, 0.0f), glm::vec2(0.0f, 0.0f), glm::vec2(0.0f), color},
        {glm::vec3(minX, minY, maxZ), glm::vec3(-1.0f, 0.0f, 0.0f), glm::vec2(1.0f, 0.0f), glm::vec2(0.0f), color},
        {glm::vec3(minX, maxY, maxZ), glm::vec3(-1.0f, 0.0f, 0.0f), glm::vec2(1.0f, 1.0f), glm::vec2(0.0f), color},
        {glm::vec3(minX, maxY, minZ), glm::vec3(-1.0f, 0.0f, 0.0f), glm::vec2(0.0f, 1.0f), glm::vec2(0.0f), color},
    }};
    addQuad(leftFace, outMesh);

    const std::array<Vertex, 4> topFace{{
        {glm::vec3(minX, maxY, minZ), glm::vec3(0.0f, 1.0f, 0.0f), glm::vec2(0.0f, 0.0f), glm::vec2(0.0f), color},
        {glm::vec3(minX, maxY, maxZ), glm::vec3(0.0f, 1.0f, 0.0f), glm::vec2(0.0f, 1.0f), glm::vec2(0.0f), color},
        {glm::vec3(maxX, maxY, maxZ), glm::vec3(0.0f, 1.0f, 0.0f), glm::vec2(1.0f, 1.0f), glm::vec2(0.0f), color},
        {glm::vec3(maxX, maxY, minZ), glm::vec3(0.0f, 1.0f, 0.0f), glm::vec2(1.0f, 0.0f), glm::vec2(0.0f), color},
    }};
    addQuad(topFace, outMesh);

    const std::array<Vertex, 4> bottomFace{{
        {glm::vec3(minX, minY, minZ), glm::vec3(0.0f, -1.0f, 0.0f), glm::vec2(0.0f, 0.0f), glm::vec2(0.0f), color},
        {glm::vec3(maxX, minY, minZ), glm::vec3(0.0f, -1.0f, 0.0f), glm::vec2(1.0f, 0.0f), glm::vec2(0.0f), color},
        {glm::vec3(maxX, minY, maxZ), glm::vec3(0.0f, -1.0f, 0.0f), glm::vec2(1.0f, 1.0f), glm::vec2(0.0f), color},
        {glm::vec3(minX, minY, maxZ), glm::vec3(0.0f, -1.0f, 0.0f), glm::vec2(0.0f, 1.0f), glm::vec2(0.0f), color},
    }};
    addQuad(bottomFace, outMesh);

    const std::array<Vertex, 4> frontFace{{
        {glm::vec3(minX, minY, maxZ), glm::vec3(0.0f, 0.0f, 1.0f), glm::vec2(0.0f, 0.0f), glm::vec2(0.0f), color},
        {glm::vec3(maxX, minY, maxZ), glm::vec3(0.0f, 0.0f, 1.0f), glm::vec2(1.0f, 0.0f), glm::vec2(0.0f), color},
        {glm::vec3(maxX, maxY, maxZ), glm::vec3(0.0f, 0.0f, 1.0f), glm::vec2(1.0f, 1.0f), glm::vec2(0.0f), color},
        {glm::vec3(minX, maxY, maxZ), glm::vec3(0.0f, 0.0f, 1.0f), glm::vec2(0.0f, 1.0f), glm::vec2(0.0f), color},
    }};
    addQuad(frontFace, outMesh);

    const std::array<Vertex, 4> backFace{{
        {glm::vec3(minX, minY, minZ), glm::vec3(0.0f, 0.0f, -1.0f), glm::vec2(1.0f, 0.0f), glm::vec2(0.0f), color},
        {glm::vec3(minX, maxY, minZ), glm::vec3(0.0f, 0.0f, -1.0f), glm::vec2(1.0f, 1.0f), glm::vec2(0.0f), color},
        {glm::vec3(maxX, maxY, minZ), glm::vec3(0.0f, 0.0f, -1.0f), glm::vec2(0.0f, 1.0f), glm::vec2(0.0f), color},
        {glm::vec3(maxX, minY, minZ), glm::vec3(0.0f, 0.0f, -1.0f), glm::vec2(0.0f, 0.0f), glm::vec2(0.0f), color},
    }};
    addQuad(backFace, outMesh);
}

render::RenderEngine::Bounds3 computeMeshBounds(const render::Mesh& mesh) {
    render::RenderEngine::Bounds3 bounds{};
    if (mesh.positions.empty()) {
        return bounds;
    }

    bounds.min = mesh.positions.front();
    bounds.max = bounds.min;
    for (const glm::vec3& position : mesh.positions) {
        bounds.min = glm::min(bounds.min, position);
        bounds.max = glm::max(bounds.max, position);
    }
    return bounds;
}

render::RenderEngine::Bounds3 computeModelBounds(const render::StaticModelData& model) {
    render::RenderEngine::Bounds3 bounds{};
    bool hasBounds = false;
    for (const auto& section : model.sections) {
        if (section.mesh.positions.empty()) {
            continue;
        }
        const auto sectionBounds = computeMeshBounds(section.mesh);
        if (!hasBounds) {
            bounds = sectionBounds;
            hasBounds = true;
            continue;
        }
        bounds.min = glm::min(bounds.min, sectionBounds.min);
        bounds.max = glm::max(bounds.max, sectionBounds.max);
    }
    return bounds;
}

void transformMesh(render::Mesh& mesh, const glm::mat4& transform) {
    const glm::mat3 normalMatrix = glm::mat3(glm::transpose(glm::inverse(transform)));
    for (std::size_t vertexIndex = 0; vertexIndex < mesh.positions.size(); ++vertexIndex) {
        mesh.positions[vertexIndex] = glm::vec3(transform * glm::vec4(mesh.positions[vertexIndex], 1.0f));
        glm::vec3 transformedNormal = normalMatrix * mesh.normals[vertexIndex];
        if (glm::dot(transformedNormal, transformedNormal) > 1.0e-6f) {
            transformedNormal = glm::normalize(transformedNormal);
        }
        mesh.normals[vertexIndex] = transformedNormal;
    }
    render::computeTangents(mesh);
}

void transformModel(render::StaticModelData& model, const glm::mat4& transform) {
    for (auto& section : model.sections) {
        transformMesh(section.mesh, transform);
    }
}

bool fitModelToFootprint(render::StaticModelData& model, float targetFootprint) {
    const render::RenderEngine::Bounds3 bounds = computeModelBounds(model);
    const glm::vec3 size = bounds.max - bounds.min;
    const float footprint = std::max(size.x, size.z);
    if (footprint <= 1.0e-4f) {
        return false;
    }

    const float scale = targetFootprint / footprint;
    transformModel(model, glm::scale(glm::mat4(1.0f), glm::vec3(scale)));
    return true;
}

void buildSphereMesh(int stacks, int slices, render::Mesh& outMesh) {
    outMesh = render::Mesh{};
    outMesh.uvSets.resize(2);

    const float pi = 3.1415926535f;
    const float twoPi = pi * 2.0f;

    for (int stack = 0; stack <= stacks; ++stack) {
        const float v = static_cast<float>(stack) / static_cast<float>(stacks);
        const float phi = v * pi;
        const float y = std::cos(phi);
        const float r = std::sin(phi);

        for (int slice = 0; slice <= slices; ++slice) {
            const float u = static_cast<float>(slice) / static_cast<float>(slices);
            const float theta = u * twoPi;
            const float x = r * std::cos(theta);
            const float z = r * std::sin(theta);

            Vertex vert{};
            vert.position = glm::vec3(x, y, z);
            vert.normal = glm::vec3(x, y, z);
            vert.uv0 = glm::vec2(u, v);
            pushVertex(vert, outMesh);
        }
    }

    const int stride = slices + 1;
    for (int stack = 0; stack < stacks; ++stack) {
        for (int slice = 0; slice < slices; ++slice) {
            const unsigned int a = static_cast<unsigned int>(stack * stride + slice);
            const unsigned int b = static_cast<unsigned int>((stack + 1) * stride + slice);
            const unsigned int c = static_cast<unsigned int>((stack + 1) * stride + slice + 1);
            const unsigned int d = static_cast<unsigned int>(stack * stride + slice + 1);
            outMesh.indices.insert(outMesh.indices.end(), {a, b, c, a, c, d});
        }
    }
}

void buildConeMesh(int slices, render::Mesh& outMesh) {
    outMesh = render::Mesh{};
    outMesh.uvSets.resize(2);

    const float twoPi = 6.283185307f;

    Vertex apex{};
    apex.position = glm::vec3(0.0f, 0.0f, 0.0f);
    apex.normal = glm::vec3(0.0f, 0.0f, -1.0f);
    apex.uv0 = glm::vec2(0.5f, 0.0f);
    pushVertex(apex, outMesh);

    for (int i = 0; i <= slices; ++i) {
        const float u = static_cast<float>(i) / static_cast<float>(slices);
        const float theta = u * twoPi;
        const float x = std::cos(theta);
        const float y = std::sin(theta);

        Vertex vert{};
        vert.position = glm::vec3(x, y, 1.0f);
        vert.normal = glm::vec3(x, y, 0.0f);
        vert.uv0 = glm::vec2(u, 1.0f);
        pushVertex(vert, outMesh);
    }

    const unsigned int baseCenterIndex = static_cast<unsigned int>(outMesh.positions.size());
    Vertex center{};
    center.position = glm::vec3(0.0f, 0.0f, 1.0f);
    center.normal = glm::vec3(0.0f, 0.0f, 1.0f);
    center.uv0 = glm::vec2(0.5f, 0.5f);
    pushVertex(center, outMesh);

    for (int i = 0; i < slices; ++i) {
        const unsigned int i0 = 1u + static_cast<unsigned int>(i);
        const unsigned int i1 = 1u + static_cast<unsigned int>(i + 1);
        outMesh.indices.insert(outMesh.indices.end(), {0u, i1, i0});
    }

    for (int i = 0; i < slices; ++i) {
        const unsigned int i0 = 1u + static_cast<unsigned int>(i);
        const unsigned int i1 = 1u + static_cast<unsigned int>(i + 1);
        outMesh.indices.insert(outMesh.indices.end(), {baseCenterIndex, i0, i1});
    }
}

glm::vec3 stableUp(const glm::vec3& dir) {
    const glm::vec3 up(0.0f, 1.0f, 0.0f);
    if (std::abs(glm::dot(dir, up)) > 0.95f) {
        return glm::vec3(0.0f, 0.0f, 1.0f);
    }
    return up;
}

glm::mat4 makeOrientationFromDirection(const glm::vec3& direction) {
    if (glm::dot(direction, direction) < 1.0e-6f) {
        return glm::mat4(1.0f);
    }

    const glm::vec3 forward = glm::normalize(direction);
    const glm::vec3 up = stableUp(forward);
    const glm::vec3 right = glm::normalize(glm::cross(up, forward));
    const glm::vec3 actualUp = glm::normalize(glm::cross(forward, right));

    glm::mat4 basis(1.0f);
    basis[0] = glm::vec4(right, 0.0f);
    basis[1] = glm::vec4(actualUp, 0.0f);
    basis[2] = glm::vec4(forward, 0.0f);
    return basis;
}

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

std::string modelRootPath() {
    char* basePath = SDL_GetBasePath();
    if (!basePath) {
        spdlog::warn("RenderEngine: SDL_GetBasePath failed: {}", SDL_GetError());
        return "build/models/";
    }
    std::string root = std::string(basePath) + "models/";
    SDL_free(basePath);
    return root;
}

std::string textureRootPath() {
    char* basePath = SDL_GetBasePath();
    if (!basePath) {
        spdlog::warn("RenderEngine: SDL_GetBasePath failed: {}", SDL_GetError());
        return "build/textures/";
    }
    std::string root = std::string(basePath) + "textures/";
    SDL_free(basePath);
    return root;
}

bool intersectRaySphere(const Ray& ray, const glm::vec3& center, float radius, float& outT) {
    const glm::vec3 oc = ray.origin - center;
    const float a = glm::dot(ray.direction, ray.direction);
    const float b = 2.0f * glm::dot(oc, ray.direction);
    const float c = glm::dot(oc, oc) - radius * radius;
    const float discriminant = b * b - 4.0f * a * c;
    if (discriminant < 0.0f) {
        return false;
    }

    const float sqrtDisc = std::sqrt(discriminant);
    const float t0 = (-b - sqrtDisc) / (2.0f * a);
    const float t1 = (-b + sqrtDisc) / (2.0f * a);
    if (t0 >= 0.0f) {
        outT = t0;
        return true;
    }
    if (t1 >= 0.0f) {
        outT = t1;
        return true;
    }
    return false;
}

bool intersectRayAabb(const Ray& ray, const render::RenderEngine::Bounds3& bounds, float& outT) {
    float tMin = 0.0f;
    float tMax = std::numeric_limits<float>::max();

    for (int axis = 0; axis < 3; ++axis) {
        const float origin = ray.origin[axis];
        const float direction = ray.direction[axis];
        const float minValue = bounds.min[axis];
        const float maxValue = bounds.max[axis];

        if (std::abs(direction) < 1.0e-6f) {
            if (origin < minValue || origin > maxValue) {
                return false;
            }
            continue;
        }

        float t0 = (minValue - origin) / direction;
        float t1 = (maxValue - origin) / direction;
        if (t0 > t1) {
            std::swap(t0, t1);
        }
        tMin = std::max(tMin, t0);
        tMax = std::min(tMax, t1);
        if (tMin > tMax) {
            return false;
        }
    }

    outT = tMin;
    return true;
}

}  // namespace

namespace render {

void RenderEngine::LightInstance::setIndex(int index) {
    index_ = index;
}

void RenderEngine::LightInstance::setMovableChangedCallback(std::function<void(int, bool)> callback) {
    movableChangedCallback_ = std::move(callback);
}

void RenderEngine::LightInstance::setIsMovable(bool movable) {
    if (isMovable == movable) {
        return;
    }
    isMovable = movable;
    if (movableChangedCallback_) {
        movableChangedCallback_(index_, isMovable);
    }
}

RenderEngine::RenderEngine(int width, int height, std::string title)
    : width_(width),
      height_(height),
      title_(std::move(title)) {}

RenderEngine::~RenderEngine() {
    shutdownImGui();
    destroyDeferredResources();
    destroyTextureLibrary();
    shadowSystem_.destroy();
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
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
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

    detectLightingCapabilities();
    updateProjection();

    if (!initImGui()) {
        return false;
    }

    buildScene();
    return sceneReady_;
}

void RenderEngine::run() {
    if (!init()) {
        return;
    }

    spdlog::info("RenderEngine: starting main loop");
    bool running = true;
    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event) != 0) {
            if (imguiReady_) {
                ImGui_ImplSDL2_ProcessEvent(&event);
            }
            handleEvent(event, running);
        }

        beginImGuiFrame();
        updateOrbitCamera();
        renderScene();
        if (editorState_.enabled) {
            renderEditorUi();
        }
        renderImGui();
        SDL_GL_SwapWindow(window_);
    }
}

void RenderEngine::handleEvent(const SDL_Event& event, bool& running) {
    const bool captureMouse = imguiReady_ && editorState_.enabled && ImGui::GetIO().WantCaptureMouse;
    const bool captureKeyboard = imguiReady_ && editorState_.enabled && ImGui::GetIO().WantCaptureKeyboard;

    switch (event.type) {
        case SDL_QUIT:
            running = false;
            break;
        case SDL_KEYDOWN:
            if (event.key.keysym.sym == SDLK_ESCAPE) {
                running = false;
                break;
            }
            if (event.key.keysym.sym == SDLK_e && event.key.repeat == 0) {
                editorState_.enabled = !editorState_.enabled;
                break;
            }
            if (captureKeyboard) {
                break;
            }
            switch (event.key.keysym.sym) {
                case SDLK_0:
                    debugView_ = DebugView::Final;
                    break;
                case SDLK_1:
                    debugView_ = DebugView::Albedo;
                    break;
                case SDLK_2:
                    debugView_ = DebugView::Normal;
                    break;
                case SDLK_3:
                    debugView_ = DebugView::RoughMetal;
                    break;
                case SDLK_4:
                    debugView_ = DebugView::Depth;
                    break;
                case SDLK_5:
                    debugView_ = DebugView::Light;
                    break;
                case SDLK_6:
                    debugView_ = DebugView::ShadowMap;
                    break;
                case SDLK_7:
                    debugView_ = DebugView::ShadowFactor;
                    break;
                case SDLK_8:
                    debugView_ = DebugView::ShadowCascade;
                    break;
                case SDLK_LEFTBRACKET:
                    shadowDebugCascade_ = std::max(0, shadowDebugCascade_ - 1);
                    break;
                case SDLK_RIGHTBRACKET:
                    shadowDebugCascade_ = std::min(shadowSystem_.directionalCascadeCount() - 1, shadowDebugCascade_ + 1);
                    break;
                case SDLK_c:
                    if (event.key.repeat == 0) {
                        orbitCameraEnabled_ = !orbitCameraEnabled_;
                        if (orbitCameraEnabled_) {
                            orbitYawDeg_ = kIsoAngleY;
                            panX_ = 0.0f;
                            panY_ = 0.0f;
                            lastOrbitTickMs_ = SDL_GetTicks();
                        } else {
                            lastOrbitTickMs_ = 0;
                        }
                        updateProjection();
                    }
                    break;
                case SDLK_l:
                    if (event.key.repeat == 0) {
                        showLightDebug_ = !showLightDebug_;
                    }
                    break;
                default:
                    break;
            }
            break;
        case SDL_MOUSEWHEEL:
            if (!captureMouse && event.wheel.y != 0) {
                const float factor = event.wheel.y > 0 ? 0.9f : 1.1f;
                zoom_ = std::clamp(zoom_ * factor, kMinZoom, kMaxZoom);
                updateProjection();
            }
            break;
        case SDL_MOUSEBUTTONDOWN:
            if (event.button.button == SDL_BUTTON_LEFT && editorState_.enabled && !captureMouse) {
                handleViewportClick(event.button.x, event.button.y);
            }
            if (event.button.button == SDL_BUTTON_MIDDLE && !captureMouse) {
                middleDragging_ = true;
                lastMouseX_ = event.button.x;
                lastMouseY_ = event.button.y;
            }
            break;
        case SDL_MOUSEBUTTONUP:
            if (event.button.button == SDL_BUTTON_MIDDLE) {
                middleDragging_ = false;
            }
            break;
        case SDL_MOUSEMOTION:
            if (!captureMouse && middleDragging_ && !orbitCameraEnabled_) {
                constexpr float panSpeed = 0.01f;
                panX_ -= static_cast<float>(event.motion.xrel) * panSpeed / zoom_;
                panY_ += static_cast<float>(event.motion.yrel) * panSpeed / zoom_;
                lastMouseX_ = event.motion.x;
                lastMouseY_ = event.motion.y;
                updateProjection();
            }
            break;
        case SDL_WINDOWEVENT:
            if (event.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
                width_ = event.window.data1;
                height_ = event.window.data2;
                updateProjection();
            }
            break;
        default:
            break;
    }
}

void RenderEngine::updateProjection() {
    glViewport(0, 0, width_, height_);

    const float aspect = static_cast<float>(width_) / static_cast<float>(height_ > 0 ? height_ : 1);
    const float halfSize = kBaseOrthoSize / zoom_;
    const float yawDeg = orbitCameraEnabled_ ? orbitYawDeg_ : kIsoAngleY;
    const float viewPanX = orbitCameraEnabled_ ? 0.0f : panX_;
    const float viewPanY = orbitCameraEnabled_ ? 0.0f : panY_;

    projection_ = glm::ortho(-halfSize * aspect, halfSize * aspect, -halfSize, halfSize, kNearPlane, kFarPlane);
    invProjection_ = glm::inverse(projection_);
    const glm::mat4 rx = glm::rotate(glm::mat4(1.0f), glm::radians(kIsoAngleX), glm::vec3(1.0f, 0.0f, 0.0f));
    const glm::mat4 ry = glm::rotate(glm::mat4(1.0f), glm::radians(-yawDeg), glm::vec3(0.0f, 1.0f, 0.0f));
    const glm::mat4 t = glm::translate(glm::mat4(1.0f), glm::vec3(-viewPanX, -viewPanY, -cameraDistance_));
    view_ = t * rx * ry;

    if (rendererPath_ == RendererPath::Deferred41) {
        ensureDeferredResources();
    }
}

void RenderEngine::updateOrbitCamera() {
    if (!orbitCameraEnabled_) {
        return;
    }

    const Uint32 nowMs = SDL_GetTicks();
    if (lastOrbitTickMs_ == 0) {
        lastOrbitTickMs_ = nowMs;
        return;
    }

    const float deltaSeconds = static_cast<float>(nowMs - lastOrbitTickMs_) * 0.001f;
    lastOrbitTickMs_ = nowMs;
    orbitYawDeg_ = std::fmod(orbitYawDeg_ + kOrbitSpeedDegPerSecond * deltaSeconds, 360.0f);
    updateProjection();
}

void RenderEngine::detectLightingCapabilities() {
    GLint major = 0;
    GLint minor = 0;
    glGetIntegerv(GL_MAJOR_VERSION, &major);
    glGetIntegerv(GL_MINOR_VERSION, &minor);

    if (major > 4 || (major == 4 && minor >= 1)) {
        rendererPath_ = RendererPath::Deferred41;
        spdlog::info("RenderEngine: using deferred path (GL 4.1 compatible)");
    } else {
        rendererPath_ = RendererPath::SimpleForward;
        spdlog::warn("RenderEngine: deferred path unavailable (GL {}.{})", major, minor);
    }
}

float RenderEngine::currentTimeSeconds() const {
    return static_cast<float>(SDL_GetTicks()) * 0.001f;
}

std::shared_ptr<Texture> RenderEngine::registerTexture(const std::shared_ptr<Texture>& texture) {
    if (texture) {
        textures_.push_back(texture);
    }
    return texture;
}

std::shared_ptr<Sampler> RenderEngine::registerSampler(const std::shared_ptr<Sampler>& sampler) {
    if (sampler) {
        samplers_.push_back(sampler);
    }
    return sampler;
}

void RenderEngine::destroyTextureLibrary() {
    if (glContext_) {
        for (const auto& texture : textures_) {
            if (texture && texture->gpuHandle != 0) {
                const GLuint handle = static_cast<GLuint>(texture->gpuHandle);
                glDeleteTextures(1, &handle);
                texture->gpuHandle = 0;
            }
        }

        for (const auto& sampler : samplers_) {
            if (sampler && sampler->gpuHandle != 0) {
                const GLuint handle = static_cast<GLuint>(sampler->gpuHandle);
                glDeleteSamplers(1, &handle);
                sampler->gpuHandle = 0;
            }
        }
    }

    defaultSampler_.reset();
    defaultBaseColorTexture_ = {};
    defaultNormalTexture_ = {};
    defaultMetallicRoughnessTexture_ = {};
    defaultAoTexture_ = {};
    defaultEmissiveTexture_ = {};
    defaultAlphaTexture_ = {};
    defaultClearcoatTexture_ = {};
    defaultDetailNormalTexture_ = {};
    defaultHeightTexture_ = {};
    textures_.clear();
    samplers_.clear();
    sceneMeshes_.clear();
}

bool RenderEngine::ensureTextureUploaded(Texture& texture) const {
    if (texture.gpuHandle != 0) {
        return true;
    }
    if (!texture.valid()) {
        return false;
    }

    GLenum internalFormat = GL_RGBA8;
    GLenum format = GL_RGBA;
    GLenum type = GL_UNSIGNED_BYTE;

    switch (texture.format) {
        case Format::R8:
            internalFormat = GL_R8;
            format = GL_RED;
            break;
        case Format::RGB8:
            internalFormat = texture.srgb ? GL_SRGB8 : GL_RGB8;
            format = GL_RGB;
            break;
        case Format::RGBA16F:
            internalFormat = GL_RGBA16F;
            format = GL_RGBA;
            type = GL_HALF_FLOAT;
            break;
        case Format::RGBA8:
        case Format::Unknown:
        default:
            internalFormat = texture.srgb ? GL_SRGB8_ALPHA8 : GL_RGBA8;
            format = GL_RGBA;
            break;
    }

    GLuint handle = 0;
    glGenTextures(1, &handle);
    glBindTexture(GL_TEXTURE_2D, handle);
    glTexImage2D(
        GL_TEXTURE_2D,
        0,
        internalFormat,
        texture.width,
        texture.height,
        0,
        format,
        type,
        texture.bytes.data()
    );
    glGenerateMipmap(GL_TEXTURE_2D);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
    const int mipLevels = 1 + static_cast<int>(std::floor(std::log2(static_cast<float>(std::max(texture.width, texture.height)))));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, std::max(mipLevels - 1, 0));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glBindTexture(GL_TEXTURE_2D, 0);

    texture.gpuHandle = handle;
    texture.mipLevels = mipLevels;
    return true;
}

bool RenderEngine::ensureSamplerUploaded(Sampler& sampler) const {
    if (sampler.gpuHandle != 0) {
        return true;
    }

    GLuint handle = 0;
    glGenSamplers(1, &handle);

    const auto toWrap = [](WrapMode mode) {
        switch (mode) {
            case WrapMode::Clamp:
                return static_cast<GLint>(GL_CLAMP_TO_EDGE);
            case WrapMode::Mirror:
                return static_cast<GLint>(GL_MIRRORED_REPEAT);
            case WrapMode::Repeat:
            default:
                return static_cast<GLint>(GL_REPEAT);
        }
    };

    const auto toMagFilter = [](Filter filter) {
        return filter == Filter::Nearest ? static_cast<GLint>(GL_NEAREST) : static_cast<GLint>(GL_LINEAR);
    };

    const auto toMinFilter = [](Filter minFilter, Filter mipFilter) {
        if (mipFilter == Filter::None) {
            return minFilter == Filter::Nearest ? static_cast<GLint>(GL_NEAREST) : static_cast<GLint>(GL_LINEAR);
        }
        if (minFilter == Filter::Nearest && mipFilter == Filter::Nearest) {
            return static_cast<GLint>(GL_NEAREST_MIPMAP_NEAREST);
        }
        if (minFilter == Filter::Nearest && mipFilter == Filter::Linear) {
            return static_cast<GLint>(GL_NEAREST_MIPMAP_LINEAR);
        }
        if (minFilter == Filter::Linear && mipFilter == Filter::Nearest) {
            return static_cast<GLint>(GL_LINEAR_MIPMAP_NEAREST);
        }
        return static_cast<GLint>(GL_LINEAR_MIPMAP_LINEAR);
    };

    glSamplerParameteri(handle, GL_TEXTURE_WRAP_S, toWrap(sampler.wrapU));
    glSamplerParameteri(handle, GL_TEXTURE_WRAP_T, toWrap(sampler.wrapV));
    glSamplerParameteri(handle, GL_TEXTURE_WRAP_R, toWrap(sampler.wrapW));
    glSamplerParameteri(handle, GL_TEXTURE_MIN_FILTER, toMinFilter(sampler.minFilter, sampler.mipFilter));
    glSamplerParameteri(handle, GL_TEXTURE_MAG_FILTER, toMagFilter(sampler.magFilter));

#ifdef GL_TEXTURE_MAX_ANISOTROPY_EXT
    GLfloat maxAnisotropy = 1.0f;
    glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT, &maxAnisotropy);
    glSamplerParameterf(handle, GL_TEXTURE_MAX_ANISOTROPY_EXT, std::min(maxAnisotropy, sampler.anisotropy));
#endif

    sampler.gpuHandle = handle;
    return true;
}

const TextureRef& RenderEngine::defaultTextureForSlot(MaterialTextureSlot slot) const {
    switch (slot) {
        case MaterialTextureSlot::BaseColor:
            return defaultBaseColorTexture_;
        case MaterialTextureSlot::MetallicRoughness:
            return defaultMetallicRoughnessTexture_;
        case MaterialTextureSlot::Normal:
            return defaultNormalTexture_;
        case MaterialTextureSlot::Ao:
            return defaultAoTexture_;
        case MaterialTextureSlot::Emissive:
            return defaultEmissiveTexture_;
        case MaterialTextureSlot::Alpha:
            return defaultAlphaTexture_;
        case MaterialTextureSlot::Clearcoat:
            return defaultClearcoatTexture_;
        case MaterialTextureSlot::DetailNormal:
            return defaultDetailNormalTexture_;
        case MaterialTextureSlot::Height:
        default:
            return defaultHeightTexture_;
    }
}

const TextureRef& RenderEngine::defaultTextureForUnit(int unit) const {
    switch (unit) {
        case 0:
            return defaultBaseColorTexture_;
        case 1:
            return defaultMetallicRoughnessTexture_;
        case 2:
            return defaultNormalTexture_;
        case 3:
            return defaultAoTexture_;
        case 4:
            return defaultEmissiveTexture_;
        case 5:
            return defaultAlphaTexture_;
        case 6:
            return defaultClearcoatTexture_;
        case 7:
            return defaultDetailNormalTexture_;
        case 8:
        default:
            return defaultHeightTexture_;
    }
}

void RenderEngine::bindTextureRef(int unit, const TextureRef& ref) const {
    const TextureRef* selected = &ref;
    if (!selected->valid() || !ensureTextureUploaded(*selected->texture)) {
        selected = &defaultTextureForUnit(unit);
    }

    if (!selected->texture || !selected->texture->valid() || !ensureTextureUploaded(*selected->texture)) {
        glActiveTexture(GL_TEXTURE0 + unit);
        glBindTexture(GL_TEXTURE_2D, 0);
        glBindSampler(unit, 0);
        return;
    }

    if (selected->sampler) {
        ensureSamplerUploaded(*selected->sampler);
    }

    glActiveTexture(GL_TEXTURE0 + unit);
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(selected->texture->gpuHandle));
    glBindSampler(unit, selected->sampler ? static_cast<GLuint>(selected->sampler->gpuHandle) : 0);
}

void RenderEngine::prebindMaterialDefaults() const {
    for (int unit = 0; unit <= 8; ++unit) {
        bindTextureRef(unit, defaultTextureForUnit(unit));
    }
}

std::vector<std::shared_ptr<Texture>> RenderEngine::runtimeTextureCatalog(TextureSemantic preferredSemantic) const {
    std::vector<std::shared_ptr<Texture>> catalog;
    catalog.reserve(textures_.size());
    for (const auto& texture : textures_) {
        if (!texture || !texture->valid() || texture->origin == TextureOrigin::InlinePrivate) {
            continue;
        }
        catalog.push_back(texture);
    }

    std::stable_sort(catalog.begin(), catalog.end(), [preferredSemantic](const auto& lhs, const auto& rhs) {
        const bool lhsPreferred = lhs->semantic == preferredSemantic;
        const bool rhsPreferred = rhs->semantic == preferredSemantic;
        if (lhsPreferred != rhsPreferred) {
            return lhsPreferred && !rhsPreferred;
        }
        if (lhs->origin != rhs->origin) {
            return static_cast<int>(lhs->origin) < static_cast<int>(rhs->origin);
        }
        if (lhs->name != rhs->name) {
            return lhs->name < rhs->name;
        }
        if (lhs->width != rhs->width) {
            return lhs->width < rhs->width;
        }
        return lhs->height < rhs->height;
    });

    return catalog;
}

TextureSemantic RenderEngine::textureSemanticForSlot(MaterialTextureSlot slot) const {
    switch (slot) {
        case MaterialTextureSlot::BaseColor:
            return TextureSemantic::BaseColor;
        case MaterialTextureSlot::MetallicRoughness:
            return TextureSemantic::ORM;
        case MaterialTextureSlot::Normal:
        case MaterialTextureSlot::DetailNormal:
            return TextureSemantic::Normal;
        case MaterialTextureSlot::Ao:
            return TextureSemantic::AO;
        case MaterialTextureSlot::Emissive:
            return TextureSemantic::Emissive;
        case MaterialTextureSlot::Alpha:
            return TextureSemantic::Alpha;
        case MaterialTextureSlot::Clearcoat:
            return TextureSemantic::Clearcoat;
        case MaterialTextureSlot::Height:
        default:
            return TextureSemantic::Height;
    }
}

glm::vec4 RenderEngine::defaultInlineValueForSlot(MaterialTextureSlot slot) const {
    switch (slot) {
        case MaterialTextureSlot::BaseColor:
            return glm::vec4(1.0f);
        case MaterialTextureSlot::MetallicRoughness:
            return glm::vec4(1.0f, 1.0f, 0.0f, 1.0f);
        case MaterialTextureSlot::Normal:
        case MaterialTextureSlot::DetailNormal:
            return glm::vec4(0.5f, 0.5f, 1.0f, 1.0f);
        case MaterialTextureSlot::Ao:
        case MaterialTextureSlot::Alpha:
            return glm::vec4(1.0f);
        case MaterialTextureSlot::Emissive:
            return glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
        case MaterialTextureSlot::Clearcoat:
            return glm::vec4(1.0f, 1.0f, 0.0f, 1.0f);
        case MaterialTextureSlot::Height:
        default:
            return glm::vec4(0.5f, 0.5f, 0.5f, 1.0f);
    }
}

const char* RenderEngine::materialTextureSlotName(MaterialTextureSlot slot) const {
    switch (slot) {
        case MaterialTextureSlot::BaseColor:
            return "Base Color Texture";
        case MaterialTextureSlot::MetallicRoughness:
            return "Metallic Roughness Texture";
        case MaterialTextureSlot::Normal:
            return "Normal Texture";
        case MaterialTextureSlot::Ao:
            return "AO Texture";
        case MaterialTextureSlot::Emissive:
            return "Emissive Texture";
        case MaterialTextureSlot::Alpha:
            return "Alpha Texture";
        case MaterialTextureSlot::Clearcoat:
            return "Clearcoat Texture";
        case MaterialTextureSlot::DetailNormal:
            return "Detail Normal Texture";
        case MaterialTextureSlot::Height:
        default:
            return "Height Texture";
    }
}

TextureRef* RenderEngine::textureRefForSlot(Material& material, MaterialTextureSlot slot) const {
    switch (slot) {
        case MaterialTextureSlot::BaseColor:
            return &material.baseColor;
        case MaterialTextureSlot::MetallicRoughness:
            return &material.metallicRoughness.texture;
        case MaterialTextureSlot::Normal:
            return &material.normal;
        case MaterialTextureSlot::Ao:
            return &material.ao;
        case MaterialTextureSlot::Emissive:
            return &material.emissive;
        case MaterialTextureSlot::Alpha:
            return &material.alpha;
        case MaterialTextureSlot::Clearcoat:
            return &material.clearcoat.texture;
        case MaterialTextureSlot::DetailNormal:
            return &material.detailNormal.texture;
        case MaterialTextureSlot::Height:
        default:
            return &material.height.texture;
    }
}

void RenderEngine::ensureInlineTexture(
    TextureRef& ref,
    const std::string& name,
    TextureSemantic semantic,
    const glm::vec4& value
) {
    ref.inlineValue = glm::clamp(value, glm::vec4(0.0f), glm::vec4(1.0f));
    if (!ref.texture || ref.texture->origin != TextureOrigin::InlinePrivate) {
        ref.texture = registerTexture(makeSolidTexture(
            name,
            ref.inlineValue,
            false,
            semantic,
            TextureOrigin::InlinePrivate
        ));
    }

    if (!ref.texture) {
        return;
    }

    if (ref.texture->gpuHandle != 0 && glContext_) {
        const GLuint handle = static_cast<GLuint>(ref.texture->gpuHandle);
        glDeleteTextures(1, &handle);
        ref.texture->gpuHandle = 0;
    }

    ref.texture->name = name;
    ref.texture->width = 1;
    ref.texture->height = 1;
    ref.texture->mipLevels = 1;
    ref.texture->format = Format::RGBA8;
    ref.texture->srgb = false;
    ref.texture->generated = true;
    ref.texture->semantic = semantic;
    ref.texture->origin = TextureOrigin::InlinePrivate;
    ref.texture->bytes.resize(4);
    ref.texture->bytes[0] = static_cast<std::uint8_t>(ref.inlineValue.r * 255.0f);
    ref.texture->bytes[1] = static_cast<std::uint8_t>(ref.inlineValue.g * 255.0f);
    ref.texture->bytes[2] = static_cast<std::uint8_t>(ref.inlineValue.b * 255.0f);
    ref.texture->bytes[3] = static_cast<std::uint8_t>(ref.inlineValue.a * 255.0f);
    ref.texture->data = ref.texture->bytes.data();
    ref.bindingMode = TextureBindingMode::InlineValue;
    if (!ref.sampler) {
        ref.sampler = defaultSampler_;
    }
}

void RenderEngine::openTextureBrowser(MaterialTextureSlot slot) {
    editorState_.textureBrowserSlot = slot;
    editorState_.activeInspectorTab = InspectorTab::TextureBrowser;
    editorState_.textureBrowserFocusRequested = true;
}

void RenderEngine::renderTextureBrowserTab(Material& material) {
    TextureRef* targetRef = textureRefForSlot(material, editorState_.textureBrowserSlot);
    if (!targetRef) {
        ImGui::TextUnformatted("No texture slot selected.");
        return;
    }

    const TextureSemantic semantic = textureSemanticForSlot(editorState_.textureBrowserSlot);
    auto catalog = runtimeTextureCatalog(semantic);
    const auto matchesSearch = [this](const Texture& texture) {
        if (editorState_.textureBrowserSearch[0] == '\0') {
            return true;
        }

        std::string haystack = texture.name;
        haystack += " ";
        haystack += textureSemanticName(texture.semantic);
        haystack += " ";
        haystack += textureOriginName(texture.origin);
        std::transform(haystack.begin(), haystack.end(), haystack.begin(), [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });

        std::string needle(editorState_.textureBrowserSearch);
        std::transform(needle.begin(), needle.end(), needle.begin(), [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });
        return haystack.find(needle) != std::string::npos;
    };

    std::vector<std::shared_ptr<Texture>> filtered;
    filtered.reserve(catalog.size());
    for (const auto& texture : catalog) {
        if (texture && matchesSearch(*texture)) {
            filtered.push_back(texture);
        }
    }

    ImGui::Text("Target Material: %s", material.name.c_str());
    ImGui::Text("Target Slot: %s", materialTextureSlotName(editorState_.textureBrowserSlot));
    if (ImGui::Button("Back To Selection")) {
        editorState_.activeInspectorTab = InspectorTab::Selection;
        editorState_.textureBrowserFocusRequested = true;
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint(
        "##TextureSearch",
        "Search textures...",
        editorState_.textureBrowserSearch,
        IM_ARRAYSIZE(editorState_.textureBrowserSearch)
    );
    ImGui::Separator();
    ImGui::Text("%d textures", static_cast<int>(filtered.size()));

    ImGui::BeginChild("TextureBrowserResults", ImVec2(0.0f, 420.0f), false);
    const float cellWidth = 180.0f;
    const float availableWidth = std::max(ImGui::GetContentRegionAvail().x, cellWidth);
    const int columnCount = std::max(1, static_cast<int>(availableWidth / cellWidth));

    if (ImGui::BeginTable("TextureBrowserGrid", columnCount, ImGuiTableFlags_SizingFixedFit)) {
        for (const auto& texture : filtered) {
            ImGui::TableNextColumn();
            ImGui::PushID(texture.get());

            ensureTextureUploaded(*texture);
            if (texture->gpuHandle != 0) {
                ImGui::Image(
                    static_cast<ImTextureID>(texture->gpuHandle),
                    ImVec2(96.0f, 96.0f),
                    ImVec2(0.0f, 1.0f),
                    ImVec2(1.0f, 0.0f)
                );
            } else {
                ImGui::BeginChild("NoPreview", ImVec2(96.0f, 96.0f), true);
                ImGui::TextUnformatted("No");
                ImGui::TextUnformatted("Preview");
                ImGui::EndChild();
            }

            ImGui::TextWrapped("%s", texture->name.c_str());
            ImGui::Text("%s | %s", textureOriginName(texture->origin), textureSemanticName(texture->semantic));
            ImGui::Text("%dx%d | %s", texture->width, texture->height, formatName(texture->format));

            if (ImGui::Button("Use Texture", ImVec2(-1.0f, 0.0f))) {
                targetRef->texture = texture;
                targetRef->bindingMode = TextureBindingMode::ProjectTexture;
                if (!targetRef->sampler) {
                    targetRef->sampler = defaultSampler_;
                }
                editorState_.activeInspectorTab = InspectorTab::Selection;
                editorState_.textureBrowserFocusRequested = true;
            }

            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::EndChild();
}

ShaderInputs RenderEngine::resolveMaterialInputs(const Material& material) const {
    return resolveShaderInputs(
        material,
        defaultBaseColorTexture_,
        defaultNormalTexture_,
        defaultMetallicRoughnessTexture_,
        defaultAoTexture_,
        defaultEmissiveTexture_,
        defaultAlphaTexture_,
        defaultClearcoatTexture_,
        defaultDetailNormalTexture_,
        defaultHeightTexture_
    );
}

void RenderEngine::bindMaterialUniforms(const ShaderInputs& inputs, const MaterialUniformLocations& locations) const {
    glUniform3fv(locations.baseColorFactor, 1, glm::value_ptr(inputs.baseColorFactor));
    glUniform1f(locations.metallicFactor, inputs.metallicFactor);
    glUniform1f(locations.roughnessFactor, inputs.roughnessFactor);
    glUniform1f(locations.normalScale, inputs.normalScale);
    glUniform1f(locations.aoStrength, inputs.aoStrength);
    glUniform3fv(locations.emissiveFactor, 1, glm::value_ptr(inputs.emissiveFactor));
    glUniform1f(locations.emissiveStrength, inputs.emissiveStrength);
    glUniform1f(locations.alphaFactor, inputs.alphaFactor);
    glUniform1i(locations.alphaMode, static_cast<int>(inputs.alphaMode));
    glUniform1f(locations.alphaCutoff, inputs.alphaCutoff);
    glUniform1f(locations.clearcoatFactor, inputs.clearcoatFactor);
    glUniform1f(locations.clearcoatRoughness, inputs.clearcoatRoughness);
    glUniform1f(locations.detailNormalScale, inputs.detailNormalScale);
    glUniform1f(locations.heightScale, inputs.heightScale);

    const auto bindTextureMeta = [&](const TextureRef& textureRef, GLint uvSetLocation, GLint transformLocation) {
        glUniform1i(uvSetLocation, textureRef.uvSet);
        const glm::mat3 uvTransform = uvTransformMatrix(textureRef.transform);
        glUniformMatrix3fv(transformLocation, 1, GL_FALSE, glm::value_ptr(uvTransform));
    };

    bindTextureMeta(inputs.baseColorTexture, locations.baseColorUvSet, locations.baseColorUvTransform);
    bindTextureMeta(inputs.metallicRoughnessTexture, locations.metallicRoughnessUvSet, locations.metallicRoughnessUvTransform);
    bindTextureMeta(inputs.normalTexture, locations.normalUvSet, locations.normalUvTransform);
    bindTextureMeta(inputs.aoTexture, locations.aoUvSet, locations.aoUvTransform);
    bindTextureMeta(inputs.emissiveTexture, locations.emissiveUvSet, locations.emissiveUvTransform);
    bindTextureMeta(inputs.alphaTexture, locations.alphaUvSet, locations.alphaUvTransform);
    bindTextureMeta(inputs.clearcoatTexture, locations.clearcoatUvSet, locations.clearcoatUvTransform);
    bindTextureMeta(inputs.detailNormalTexture, locations.detailNormalUvSet, locations.detailNormalUvTransform);
    bindTextureMeta(inputs.heightTexture, locations.heightUvSet, locations.heightUvTransform);
}

void RenderEngine::configureMaterialRasterState(const ShaderInputs& inputs) const {
    if (inputs.doubleSided) {
        glDisable(GL_CULL_FACE);
    } else {
        glEnable(GL_CULL_FACE);
        glCullFace(GL_BACK);
    }

    if (inputs.alphaMode == AlphaMode::Blend) {
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    } else {
        glDisable(GL_BLEND);
    }
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

bool RenderEngine::buildTextureLibrary() {
    destroyTextureLibrary();

    auto sampler = std::make_shared<Sampler>();
    sampler->minFilter = Filter::Linear;
    sampler->magFilter = Filter::Linear;
    sampler->mipFilter = Filter::Linear;
    sampler->wrapU = WrapMode::Repeat;
    sampler->wrapV = WrapMode::Repeat;
    sampler->wrapW = WrapMode::Repeat;
    sampler->anisotropy = 8.0f;
    defaultSampler_ = registerSampler(sampler);

    defaultBaseColorTexture_ = makeTextureRef(registerTexture(makeSolidTexture(
        "DefaultWhite",
        glm::vec4(1.0f),
        true,
        TextureSemantic::BaseColor,
        TextureOrigin::Default
    )), defaultSampler_);
    defaultNormalTexture_ = makeTextureRef(registerTexture(makeSolidTexture(
        "DefaultNormal",
        glm::vec4(0.5f, 0.5f, 1.0f, 1.0f),
        false,
        TextureSemantic::Normal,
        TextureOrigin::Default
    )), defaultSampler_);
    defaultMetallicRoughnessTexture_ = makeTextureRef(registerTexture(makeSolidTexture(
        "DefaultORM",
        glm::vec4(1.0f, 1.0f, 0.0f, 1.0f),
        false,
        TextureSemantic::ORM,
        TextureOrigin::Default
    )), defaultSampler_);
    defaultAoTexture_ = makeTextureRef(registerTexture(makeSolidTexture(
        "DefaultAO",
        glm::vec4(1.0f),
        false,
        TextureSemantic::AO,
        TextureOrigin::Default
    )), defaultSampler_);
    defaultEmissiveTexture_ = makeTextureRef(registerTexture(makeSolidTexture(
        "DefaultBlack",
        glm::vec4(0.0f, 0.0f, 0.0f, 1.0f),
        false,
        TextureSemantic::Emissive,
        TextureOrigin::Default
    )), defaultSampler_);
    defaultAlphaTexture_ = makeTextureRef(registerTexture(makeSolidTexture(
        "DefaultAlpha",
        glm::vec4(1.0f),
        false,
        TextureSemantic::Alpha,
        TextureOrigin::Default
    )), defaultSampler_);
    defaultClearcoatTexture_ = makeTextureRef(registerTexture(makeSolidTexture(
        "DefaultClearcoat",
        glm::vec4(1.0f, 1.0f, 0.0f, 1.0f),
        false,
        TextureSemantic::Clearcoat,
        TextureOrigin::Default
    )), defaultSampler_);
    defaultDetailNormalTexture_ = defaultNormalTexture_;
    defaultHeightTexture_ = makeTextureRef(registerTexture(makeSolidTexture(
        "DefaultHeight",
        glm::vec4(0.5f, 0.5f, 0.5f, 1.0f),
        false,
        TextureSemantic::Height,
        TextureOrigin::Default
    )), defaultSampler_);

    return true;
}

std::shared_ptr<Material> RenderEngine::createProceduralMaterial(
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
) {
    auto material = std::make_shared<Material>();
    material->name = name;
    material->baseColorFactor = tint;
    material->metallicFactor = 1.0f;
    material->roughnessFactor = 1.0f;
    material->normalScale = 1.0f;
    material->aoStrength = 1.0f;
    material->alphaFactor = 1.0f;
    material->emissiveStrength = 1.0f;
    material->clearcoat.factor = clearcoatFactor;
    material->clearcoat.roughness = clearcoatRoughness;
    material->detailNormal.scale = detailNormalScale;
    material->height.scale = 0.03f;

    UVTransform transform{};
    transform.scale = uvScale;
    material->baseColor = makeTextureRef(baseColor, defaultSampler_, 0, transform);
    material->normal = makeTextureRef(normal, defaultSampler_, 0, transform);
    material->metallicRoughness.texture = makeTextureRef(metallicRoughness, defaultSampler_, 0, transform);
    material->ao = makeTextureRef(ao, defaultSampler_, 0, transform);
    material->height.texture = makeTextureRef(height, defaultSampler_, 0, transform);
    material->detailNormal.texture = makeTextureRef(normal, defaultSampler_, 0, transform);
    return material;
}

void RenderEngine::appendModelObjects(
    const std::string& modelName,
    SceneObjectKind kind,
    RenderLayer layer,
    const StaticModelData& model,
    const std::shared_ptr<TransformState>& transformState,
    int& nextId
) {
    for (const auto& section : model.sections) {
        MeshBuffer* mesh = createSceneMesh(section.mesh);
        if (!mesh) {
            continue;
        }

        sceneObjects_.push_back(SceneObject{
            nextId++,
            modelName + " / " + section.name,
            section.material ? section.material->name : std::string("Unassigned"),
            kind,
            layer,
            mesh,
            section.material,
            computeMeshBounds(section.mesh),
            transformState,
            true
        });
    }
}

glm::mat4 RenderEngine::composeTransform(const TransformState& transform) const {
    glm::mat4 model(1.0f);
    model = glm::translate(model, transform.position);
    model = glm::rotate(model, glm::radians(transform.rotationDeg.x), glm::vec3(1.0f, 0.0f, 0.0f));
    model = glm::rotate(model, glm::radians(transform.rotationDeg.y), glm::vec3(0.0f, 1.0f, 0.0f));
    model = glm::rotate(model, glm::radians(transform.rotationDeg.z), glm::vec3(0.0f, 0.0f, 1.0f));
    model = glm::scale(model, transform.scale);
    return model;
}

glm::mat3 RenderEngine::normalMatrixFromModel(const glm::mat4& model) const {
    return glm::mat3(glm::transpose(glm::inverse(model)));
}

RenderEngine::Bounds3 RenderEngine::transformBounds(const Bounds3& bounds, const glm::mat4& model) const {
    std::array<glm::vec3, 8> corners = {{
        {bounds.min.x, bounds.min.y, bounds.min.z},
        {bounds.min.x, bounds.min.y, bounds.max.z},
        {bounds.min.x, bounds.max.y, bounds.min.z},
        {bounds.min.x, bounds.max.y, bounds.max.z},
        {bounds.max.x, bounds.min.y, bounds.min.z},
        {bounds.max.x, bounds.min.y, bounds.max.z},
        {bounds.max.x, bounds.max.y, bounds.min.z},
        {bounds.max.x, bounds.max.y, bounds.max.z},
    }};

    Bounds3 out{};
    out.min = glm::vec3(model * glm::vec4(corners[0], 1.0f));
    out.max = out.min;
    for (const glm::vec3& corner : corners) {
        const glm::vec3 transformed = glm::vec3(model * glm::vec4(corner, 1.0f));
        out.min = glm::min(out.min, transformed);
        out.max = glm::max(out.max, transformed);
    }
    return out;
}

RenderEngine::Bounds3 RenderEngine::sceneObjectWorldBounds(const SceneObject& object) const {
    if (!object.transform) {
        return object.localBounds;
    }
    return transformBounds(object.localBounds, composeTransform(*object.transform));
}

bool RenderEngine::pickSceneEntity(int mouseX, int mouseY, SelectedEntity& outSelection) const {
    if (width_ <= 0 || height_ <= 0) {
        return false;
    }

    const float ndcX = (2.0f * static_cast<float>(mouseX) / static_cast<float>(width_)) - 1.0f;
    const float ndcY = 1.0f - (2.0f * static_cast<float>(mouseY) / static_cast<float>(height_));
    const glm::mat4 invViewProj = glm::inverse(projection_ * view_);
    const glm::vec4 nearClip(ndcX, ndcY, -1.0f, 1.0f);
    const glm::vec4 farClip(ndcX, ndcY, 1.0f, 1.0f);
    const glm::vec4 nearWorld4 = invViewProj * nearClip;
    const glm::vec4 farWorld4 = invViewProj * farClip;
    const glm::vec3 nearWorld = glm::vec3(nearWorld4) / nearWorld4.w;
    const glm::vec3 farWorld = glm::vec3(farWorld4) / farWorld4.w;
    const glm::vec3 direction = farWorld - nearWorld;
    if (glm::dot(direction, direction) < 1.0e-6f) {
        return false;
    }

    const Ray ray{nearWorld, glm::normalize(direction)};
    bool hit = false;
    float bestT = std::numeric_limits<float>::max();

    for (int index = 0; index < static_cast<int>(sceneObjects_.size()); ++index) {
        const SceneObject& object = sceneObjects_[index];
        if (!object.visible || object.mesh == nullptr || !object.mesh->valid()) {
            continue;
        }

        float tHit = 0.0f;
        if (!intersectRayAabb(ray, sceneObjectWorldBounds(object), tHit)) {
            continue;
        }
        if (tHit < bestT) {
            bestT = tHit;
            outSelection = SelectedEntity{SelectedEntityType::SceneObject, index};
            hit = true;
        }
    }

    const float timeSeconds = currentTimeSeconds();
    for (int index = 0; index < static_cast<int>(lights_.size()); ++index) {
        glm::vec3 position(0.0f);
        glm::vec3 directionVec(0.0f);
        evaluateLightTransform(lights_[index], timeSeconds, position, directionVec);

        glm::vec3 sphereCenter = position;
        float sphereRadius = lights_[index].radius;
        if (lights_[index].type == LightType::Spot) {
            const float coneRadius = lights_[index].radius * std::tan(glm::radians(lights_[index].outerAngle));
            sphereCenter = position + directionVec * (lights_[index].radius * 0.5f);
            sphereRadius = std::sqrt((lights_[index].radius * 0.5f) * (lights_[index].radius * 0.5f) + coneRadius * coneRadius);
        }

        float tHit = 0.0f;
        if (!intersectRaySphere(ray, sphereCenter, sphereRadius, tHit)) {
            continue;
        }
        if (tHit < bestT) {
            bestT = tHit;
            outSelection = SelectedEntity{SelectedEntityType::Light, index};
            hit = true;
        }
    }

    return hit;
}

void RenderEngine::handleViewportClick(int mouseX, int mouseY) {
    SelectedEntity selection{};
    if (pickSceneEntity(mouseX, mouseY, selection)) {
        editorState_.selection = selection;
    } else {
        editorState_.selection.reset();
    }
}

const char* RenderEngine::rendererPathName() const {
    return rendererPath_ == RendererPath::Deferred41 ? "Deferred 4.1" : "Simple Forward";
}

std::string RenderEngine::selectionSummary() const {
    if (!editorState_.selection.has_value()) {
        return "None";
    }

    const SelectedEntity selection = *editorState_.selection;
    if (selection.type == SelectedEntityType::SceneObject &&
        selection.index >= 0 &&
        selection.index < static_cast<int>(sceneObjects_.size())) {
        return sceneObjects_[selection.index].name;
    }
    if (selection.type == SelectedEntityType::Light &&
        selection.index >= 0 &&
        selection.index < static_cast<int>(lights_.size())) {
        const char* typeName = lights_[selection.index].type == LightType::Point ? "Point Light" : "Spot Light";
        return std::string(typeName) + " " + std::to_string(selection.index);
    }
    return "None";
}

bool RenderEngine::drawTextureSlotEditor(
    const char* label,
    const std::string& materialName,
    MaterialTextureSlot slot,
    TextureRef& ref,
    const TextureRef& resolved
) {
    const auto textureLabel = [](const Texture& texture) {
        return texture.name + " [" +
               textureOriginName(texture.origin) + " | " +
               textureSemanticName(texture.semantic) + " | " +
               std::to_string(texture.width) + "x" + std::to_string(texture.height) + " | " +
               formatName(texture.format) + "]";
    };

    const TextureSemantic semantic = textureSemanticForSlot(slot);
    bool changed = false;

    ImGui::PushID(label);
    const bool open = ImGui::TreeNodeEx("slot", ImGuiTreeNodeFlags_DefaultOpen, "%s", label);
    if (!open) {
        ImGui::PopID();
        return false;
    }

    if (resolved.texture && ensureTextureUploaded(*resolved.texture)) {
        ImGui::Image(
            static_cast<ImTextureID>(resolved.texture->gpuHandle),
            ImVec2(64.0f, 64.0f),
            ImVec2(0.0f, 1.0f),
            ImVec2(1.0f, 0.0f)
        );
    } else {
        ImGui::BeginChild("preview", ImVec2(64.0f, 64.0f), true);
        ImGui::TextUnformatted("No");
        ImGui::TextUnformatted("Preview");
        ImGui::EndChild();
    }

    ImGui::SameLine();
    ImGui::BeginGroup();
    ImGui::Text("Mode: %s", textureBindingModeName(ref.bindingMode));
    ImGui::Text(
        "Resolved: %s",
        resolved.texture ? resolved.texture->name.c_str() : "None"
    );
    if (resolved.texture) {
        ImGui::Text(
            "Meta: %s | %s | %dx%d | %s",
            textureOriginName(resolved.texture->origin),
            textureSemanticName(resolved.texture->semantic),
            resolved.texture->width,
            resolved.texture->height,
            formatName(resolved.texture->format)
        );
        ImGui::Text(
            "Mip Levels: %d | sRGB: %s",
            resolved.texture->mipLevels,
            resolved.texture->srgb ? "Yes" : "No"
        );
    }
    ImGui::EndGroup();

    int bindingMode = static_cast<int>(ref.bindingMode);
    if (ImGui::Combo("Binding Mode", &bindingMode, "Default\0Project Texture\0Inline Value\0")) {
        const TextureBindingMode previousMode = ref.bindingMode;
        ref.bindingMode = static_cast<TextureBindingMode>(bindingMode);
        changed = true;

        if (ref.bindingMode == TextureBindingMode::ProjectTexture) {
            auto catalog = runtimeTextureCatalog(semantic);
            if (!ref.texture || ref.texture->origin == TextureOrigin::InlinePrivate || !ref.texture->valid()) {
                ref.texture = catalog.empty() ? nullptr : catalog.front();
            }
            if (!ref.sampler) {
                ref.sampler = defaultSampler_;
            }
        } else if (ref.bindingMode == TextureBindingMode::InlineValue) {
            if (previousMode != TextureBindingMode::InlineValue) {
                ref.inlineValue = defaultInlineValueForSlot(slot);
            }
            ensureInlineTexture(ref, materialName + " / " + label + " Inline", semantic, ref.inlineValue);
        }
    }

    if (ref.bindingMode == TextureBindingMode::ProjectTexture) {
        auto catalog = runtimeTextureCatalog(semantic);
        if ((!ref.texture || ref.texture->origin == TextureOrigin::InlinePrivate || !ref.texture->valid()) && !catalog.empty()) {
            ref.texture = catalog.front();
            changed = true;
        }
        if (!ref.sampler) {
            ref.sampler = defaultSampler_;
        }

        ImGui::TextWrapped(
            "Selected Texture: %s",
            ref.texture ? textureLabel(*ref.texture).c_str() : "None"
        );
        if (ImGui::Button("Browse Texture Library")) {
            openTextureBrowser(slot);
        }
    } else if (ref.bindingMode == TextureBindingMode::InlineValue) {
        if (!ref.texture || ref.texture->origin != TextureOrigin::InlinePrivate || !ref.texture->valid()) {
            ensureInlineTexture(ref, materialName + " / " + label + " Inline", semantic, ref.inlineValue);
            changed = true;
        }

        bool inlineEdited = false;
        switch (slot) {
            case MaterialTextureSlot::BaseColor:
                inlineEdited = ImGui::ColorEdit4("Inline Value", glm::value_ptr(ref.inlineValue));
                break;
            case MaterialTextureSlot::MetallicRoughness: {
                glm::vec3 orm(ref.inlineValue);
                if (ImGui::DragFloat3("Inline AO/Rough/Metal", glm::value_ptr(orm), 0.01f, 0.0f, 1.0f, "%.2f")) {
                    ref.inlineValue = glm::vec4(orm, 1.0f);
                    inlineEdited = true;
                }
                break;
            }
            case MaterialTextureSlot::Normal:
            case MaterialTextureSlot::DetailNormal: {
                glm::vec3 normalValue(ref.inlineValue);
                if (ImGui::DragFloat3("Inline Normal", glm::value_ptr(normalValue), 0.01f, 0.0f, 1.0f, "%.2f")) {
                    ref.inlineValue = glm::vec4(normalValue, 1.0f);
                    inlineEdited = true;
                }
                break;
            }
            case MaterialTextureSlot::Ao:
            case MaterialTextureSlot::Alpha:
            case MaterialTextureSlot::Height: {
                float scalar = ref.inlineValue.x;
                if (ImGui::DragFloat("Inline Value", &scalar, 0.01f, 0.0f, 1.0f, "%.2f")) {
                    ref.inlineValue = glm::vec4(scalar, scalar, scalar, 1.0f);
                    inlineEdited = true;
                }
                break;
            }
            case MaterialTextureSlot::Emissive: {
                glm::vec3 emissiveValue(ref.inlineValue);
                if (ImGui::ColorEdit3("Inline Value", glm::value_ptr(emissiveValue))) {
                    ref.inlineValue = glm::vec4(emissiveValue, 1.0f);
                    inlineEdited = true;
                }
                break;
            }
            case MaterialTextureSlot::Clearcoat: {
                glm::vec2 clearcoatValue(ref.inlineValue.x, ref.inlineValue.y);
                if (ImGui::DragFloat2("Inline Factor/Rough", glm::value_ptr(clearcoatValue), 0.01f, 0.0f, 1.0f, "%.2f")) {
                    ref.inlineValue = glm::vec4(clearcoatValue, 0.0f, 1.0f);
                    inlineEdited = true;
                }
                break;
            }
        }

        if (inlineEdited) {
            ensureInlineTexture(ref, materialName + " / " + label + " Inline", semantic, ref.inlineValue);
            changed = true;
        }
    }

    const Sampler* sampler = resolved.sampler.get();
    if (sampler) {
        ImGui::Text(
            "Sampler: min %s, mag %s, mip %s",
            filterName(sampler->minFilter),
            filterName(sampler->magFilter),
            filterName(sampler->mipFilter)
        );
        ImGui::Text(
            "Wrap: %s / %s / %s | Aniso %.1f",
            wrapModeName(sampler->wrapU),
            wrapModeName(sampler->wrapV),
            wrapModeName(sampler->wrapW),
            sampler->anisotropy
        );
    } else {
        ImGui::TextUnformatted("Sampler: Texture defaults");
    }

    int uvSet = std::clamp(ref.uvSet, 0, 1);
    if (ImGui::SliderInt("UV Set", &uvSet, 0, 1)) {
        ref.uvSet = uvSet;
        changed = true;
    }
    if (ImGui::DragFloat2("Offset", glm::value_ptr(ref.transform.offset), 0.01f)) {
        changed = true;
    }
    if (ImGui::DragFloat2("Scale", glm::value_ptr(ref.transform.scale), 0.01f, 0.0f, 128.0f)) {
        changed = true;
    }
    if (ImGui::DragFloat("Rotation", &ref.transform.rotation, 0.01f, -6.2831f, 6.2831f)) {
        changed = true;
    }

    ImGui::TreePop();
    ImGui::PopID();
    return changed;
}

void RenderEngine::renderEditorUi() {
    ImGui::SetNextWindowBgAlpha(0.92f);
    std::string inspectorTitle = "Inspector";
    if (editorState_.selection.has_value()) {
        inspectorTitle += " - " + selectionSummary();
    }
    inspectorTitle += "###Inspector";
    ImGui::Begin(inspectorTitle.c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize);
    if (!editorState_.selection.has_value()) {
        ImGui::TextUnformatted("Click an object or light to inspect it.");
        ImGui::End();
        return;
    }

    const SelectedEntity selection = *editorState_.selection;
    const ImGuiTabItemFlags selectionTabFlags =
        (editorState_.textureBrowserFocusRequested && editorState_.activeInspectorTab == InspectorTab::Selection)
            ? ImGuiTabItemFlags_SetSelected
            : 0;
    const ImGuiTabItemFlags browserTabFlags =
        (editorState_.textureBrowserFocusRequested && editorState_.activeInspectorTab == InspectorTab::TextureBrowser)
            ? ImGuiTabItemFlags_SetSelected
            : 0;

    if (ImGui::BeginTabBar("InspectorTabs")) {
        if (ImGui::BeginTabItem("Selection", nullptr, selectionTabFlags)) {
            editorState_.activeInspectorTab = InspectorTab::Selection;

            if (selection.type == SelectedEntityType::SceneObject &&
                selection.index >= 0 &&
                selection.index < static_cast<int>(sceneObjects_.size())) {
                SceneObject& object = sceneObjects_[selection.index];
                Bounds3 worldBounds = sceneObjectWorldBounds(object);
                if (!object.transform) {
                    ImGui::TextUnformatted("Object transform is unavailable.");
                } else {
                    ImGui::Text("Object: %s", object.name.c_str());
                    ImGui::Text("Kind: %s", object.kind == SceneObjectKind::Ground ? "Ground" :
                                            object.kind == SceneObjectKind::Wall ? "Wall" : "Model");
                    ImGui::Text("Material: %s", object.materialLabel.c_str());
                    ImGui::DragFloat3("Position", glm::value_ptr(object.transform->position), 0.05f);
                    ImGui::DragFloat3("Rotation", glm::value_ptr(object.transform->rotationDeg), 0.5f);
                    if (ImGui::DragFloat3("Scale", glm::value_ptr(object.transform->scale), 0.02f)) {
                        object.transform->scale = glm::max(object.transform->scale, glm::vec3(0.01f));
                    }
                    ImGui::Separator();
                    ImGui::Text("World Bounds Min: %.2f %.2f %.2f", worldBounds.min.x, worldBounds.min.y, worldBounds.min.z);
                    ImGui::Text("World Bounds Max: %.2f %.2f %.2f", worldBounds.max.x, worldBounds.max.y, worldBounds.max.z);
                    if (object.material) {
                        Material& material = *object.material;
                        ImGui::Separator();
                        ImGui::Text("Material Asset: %s", material.name.c_str());
                        ImGui::ColorEdit3("Base Color Factor", glm::value_ptr(material.baseColorFactor));
                        ImGui::DragFloat("Metallic Factor", &material.metallicFactor, 0.01f, 0.0f, 1.0f);
                        ImGui::DragFloat("Roughness Factor", &material.roughnessFactor, 0.01f, 0.0f, 1.0f);
                        ImGui::DragFloat("Normal Scale", &material.normalScale, 0.01f, 0.0f, 8.0f);
                        ImGui::DragFloat("AO Strength", &material.aoStrength, 0.01f, 0.0f, 1.0f);
                        ImGui::ColorEdit3("Emissive Factor", glm::value_ptr(material.emissiveFactor));
                        ImGui::DragFloat("Emissive Strength", &material.emissiveStrength, 0.01f, 0.0f, 8.0f);
                        ImGui::DragFloat("Alpha Factor", &material.alphaFactor, 0.01f, 0.0f, 1.0f);
                        ImGui::Text("Alpha Mode: %s", alphaModeName(material.alphaMode));
                        ImGui::DragFloat("Alpha Cutoff", &material.alphaCutoff, 0.01f, 0.0f, 1.0f);
                        ImGui::Checkbox("Double Sided", &material.doubleSided);
                        ImGui::DragFloat("Clearcoat Factor", &material.clearcoat.factor, 0.01f, 0.0f, 1.0f);
                        ImGui::DragFloat("Clearcoat Roughness", &material.clearcoat.roughness, 0.01f, 0.0f, 1.0f);
                        ImGui::DragFloat("Detail Normal Scale", &material.detailNormal.scale, 0.01f, 0.0f, 8.0f);
                        ImGui::DragFloat("Height Scale", &material.height.scale, 0.001f, 0.0f, 0.25f, "%.3f");
                        const ShaderInputs resolvedInputs = resolveMaterialInputs(material);
                        drawTextureSlotEditor(
                            "Base Color Texture",
                            material.name,
                            MaterialTextureSlot::BaseColor,
                            material.baseColor,
                            resolvedInputs.baseColorTexture
                        );
                        drawTextureSlotEditor(
                            "Metallic Roughness Texture",
                            material.name,
                            MaterialTextureSlot::MetallicRoughness,
                            material.metallicRoughness.texture,
                            resolvedInputs.metallicRoughnessTexture
                        );
                        drawTextureSlotEditor(
                            "Normal Texture",
                            material.name,
                            MaterialTextureSlot::Normal,
                            material.normal,
                            resolvedInputs.normalTexture
                        );
                        drawTextureSlotEditor(
                            "AO Texture",
                            material.name,
                            MaterialTextureSlot::Ao,
                            material.ao,
                            resolvedInputs.aoTexture
                        );
                        drawTextureSlotEditor(
                            "Emissive Texture",
                            material.name,
                            MaterialTextureSlot::Emissive,
                            material.emissive,
                            resolvedInputs.emissiveTexture
                        );
                        drawTextureSlotEditor(
                            "Alpha Texture",
                            material.name,
                            MaterialTextureSlot::Alpha,
                            material.alpha,
                            resolvedInputs.alphaTexture
                        );
                        drawTextureSlotEditor(
                            "Clearcoat Texture",
                            material.name,
                            MaterialTextureSlot::Clearcoat,
                            material.clearcoat.texture,
                            resolvedInputs.clearcoatTexture
                        );
                        drawTextureSlotEditor(
                            "Detail Normal Texture",
                            material.name,
                            MaterialTextureSlot::DetailNormal,
                            material.detailNormal.texture,
                            resolvedInputs.detailNormalTexture
                        );
                        drawTextureSlotEditor(
                            "Height Texture",
                            material.name,
                            MaterialTextureSlot::Height,
                            material.height.texture,
                            resolvedInputs.heightTexture
                        );

                        if (ImGui::TreeNode("Shader Inputs")) {
                            const ShaderInputs inputs = resolveMaterialInputs(material);
                            ImGui::Text("Base Color Factor: %.2f %.2f %.2f", inputs.baseColorFactor.x, inputs.baseColorFactor.y, inputs.baseColorFactor.z);
                            ImGui::Text("Metallic / Roughness: %.2f / %.2f", inputs.metallicFactor, inputs.roughnessFactor);
                            ImGui::Text("Normal Scale: %.2f", inputs.normalScale);
                            ImGui::Text("AO Strength: %.2f", inputs.aoStrength);
                            ImGui::Text("Emissive Strength: %.2f", inputs.emissiveStrength);
                            ImGui::Text("Clearcoat: %.2f roughness %.2f", inputs.clearcoatFactor, inputs.clearcoatRoughness);
                            ImGui::Text("Height Scale: %.3f", inputs.heightScale);
                            ImGui::Text("Resolved Base: %s", inputs.baseColorTexture.texture ? inputs.baseColorTexture.texture->name.c_str() : "None");
                            ImGui::Text("Resolved Normal: %s", inputs.normalTexture.texture ? inputs.normalTexture.texture->name.c_str() : "None");
                            ImGui::Text(
                                "Resolved ORM: %s",
                                inputs.metallicRoughnessTexture.texture ? inputs.metallicRoughnessTexture.texture->name.c_str() : "None"
                            );
                            ImGui::Text("Resolved AO: %s", inputs.aoTexture.texture ? inputs.aoTexture.texture->name.c_str() : "None");
                            ImGui::Text("Resolved Emissive: %s", inputs.emissiveTexture.texture ? inputs.emissiveTexture.texture->name.c_str() : "None");
                            ImGui::TreePop();
                        }
                    }
                }
            } else if (selection.type == SelectedEntityType::Light &&
                       selection.index >= 0 &&
                       selection.index < static_cast<int>(lights_.size())) {
                LightInstance& light = lights_[selection.index];
                glm::vec3 currentPosition(0.0f);
                glm::vec3 currentDirection(0.0f);
                evaluateLightTransform(light, currentTimeSeconds(), currentPosition, currentDirection);

                ImGui::Text("Light: %s %d", light.type == LightType::Point ? "Point" : "Spot", selection.index);
                ImGui::DragFloat3("Position", glm::value_ptr(light.basePosition), 0.05f);
                ImGui::ColorEdit3("Color", glm::value_ptr(light.color));
                ImGui::DragFloat("Intensity", &light.intensity, 0.05f, 0.0f, 100.0f);
                if (ImGui::DragFloat("Radius", &light.radius, 0.1f, 0.1f, 250.0f)) {
                    light.radius = std::max(light.radius, 0.1f);
                    movableAssignmentsDirty_ = true;
                }
                bool isMovable = light.isMovable;
                if (ImGui::Checkbox("Movable", &isMovable)) {
                    light.setIsMovable(isMovable);
                }
                ImGui::Checkbox("Casts Shadow", &light.castsShadow);
                ImGui::DragFloat("Shadow Bias Min", &light.shadowBiasMin, 0.00005f, 0.0f, 0.05f, "%.5f");
                ImGui::DragFloat("Shadow Bias Slope", &light.shadowBiasSlope, 0.0001f, 0.0f, 0.05f, "%.5f");
                if (light.type == LightType::Spot) {
                    ImGui::DragFloat3("Target", glm::value_ptr(light.target), 0.05f);
                    if (ImGui::DragFloat("Inner Angle", &light.innerAngle, 0.25f, 0.1f, 89.0f)) {
                        light.innerAngle = std::clamp(light.innerAngle, 0.1f, 89.0f);
                        light.outerAngle = std::max(light.outerAngle, light.innerAngle + 0.1f);
                    }
                    if (ImGui::DragFloat("Outer Angle", &light.outerAngle, 0.25f, 0.1f, 89.0f)) {
                        light.outerAngle = std::clamp(light.outerAngle, light.innerAngle + 0.1f, 89.0f);
                    }
                }
                ImGui::Separator();
                ImGui::Text("Current Position: %.2f %.2f %.2f", currentPosition.x, currentPosition.y, currentPosition.z);
                if (light.type == LightType::Spot) {
                    ImGui::Text("Current Direction: %.2f %.2f %.2f", currentDirection.x, currentDirection.y, currentDirection.z);
                }
            } else {
                ImGui::TextUnformatted("Selection is no longer valid.");
            }
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Texture Browser", nullptr, browserTabFlags)) {
            editorState_.activeInspectorTab = InspectorTab::TextureBrowser;

            if (selection.type == SelectedEntityType::SceneObject &&
                selection.index >= 0 &&
                selection.index < static_cast<int>(sceneObjects_.size()) &&
                sceneObjects_[selection.index].material) {
                renderTextureBrowserTab(*sceneObjects_[selection.index].material);
            } else {
                ImGui::TextUnformatted("Select an object with a material to browse textures.");
            }
            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
    }
    editorState_.textureBrowserFocusRequested = false;

    ImGui::End();
}

void RenderEngine::drawDebugMesh(const MeshBuffer& mesh, const glm::mat4& model, const glm::vec4& color, bool wireframe) const {
    if (!mesh.valid() || debugColorShader_.id() == 0) {
        return;
    }

    const glm::mat4 mvp = projection_ * view_ * model;
    glUniformMatrix4fv(debugMvpLocation_, 1, GL_FALSE, glm::value_ptr(mvp));
    glUniform4fv(debugColorLocation_, 1, glm::value_ptr(color));
    if (wireframe) {
        glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
    }
    mesh.draw();
    if (wireframe) {
        glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    }
}

void RenderEngine::renderSelectionOverlay() const {
    if (!editorState_.enabled || !editorState_.selection.has_value() || debugColorShader_.id() == 0) {
        return;
    }

    const SelectedEntity selection = *editorState_.selection;
    if (selection.type != SelectedEntityType::SceneObject ||
        selection.index < 0 ||
        selection.index >= static_cast<int>(sceneObjects_.size()) ||
        !axisGizmo_.valid() ||
        !selectionBox_.valid()) {
        return;
    }

    const SceneObject& object = sceneObjects_[selection.index];
    Bounds3 worldBounds = sceneObjectWorldBounds(object);
    const glm::vec3 center = (worldBounds.min + worldBounds.max) * 0.5f;
    const glm::vec3 extents = glm::max((worldBounds.max - worldBounds.min) * 0.5f, glm::vec3(0.01f));
    const float axisScale = std::clamp(glm::length(worldBounds.max - worldBounds.min) * kSelectionScaleFactor, kSelectionAxisScaleMin, kSelectionAxisScaleMax);

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, width_, height_);
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_CULL_FACE);

    debugColorShader_.use();
    if (!object.transform) {
        return;
    }

    glm::mat4 axisModel = composeTransform(*object.transform);
    axisModel = glm::translate(glm::mat4(1.0f), object.transform->position) *
                glm::rotate(glm::mat4(1.0f), glm::radians(object.transform->rotationDeg.x), glm::vec3(1.0f, 0.0f, 0.0f)) *
                glm::rotate(glm::mat4(1.0f), glm::radians(object.transform->rotationDeg.y), glm::vec3(0.0f, 1.0f, 0.0f)) *
                glm::rotate(glm::mat4(1.0f), glm::radians(object.transform->rotationDeg.z), glm::vec3(0.0f, 0.0f, 1.0f)) *
                glm::scale(glm::mat4(1.0f), glm::vec3(axisScale));
    drawDebugMesh(axisGizmo_, axisModel, glm::vec4(1.0f), false);

    glm::mat4 boxModel(1.0f);
    boxModel = glm::translate(boxModel, center);
    boxModel = glm::scale(boxModel, extents);
    drawDebugMesh(selectionBox_, boxModel, glm::vec4(0.98f, 0.85f, 0.30f, kSelectionBoundsAlpha), true);

    glDisable(GL_BLEND);
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    glDepthMask(GL_TRUE);
}

void RenderEngine::renderLightDebugOverlay(bool includeSelectedLight) const {
    int selectedLightIndex = -1;
    if (includeSelectedLight &&
        editorState_.selection.has_value() &&
        editorState_.selection->type == SelectedEntityType::Light) {
        selectedLightIndex = editorState_.selection->index;
    }

    if ((!showLightDebug_ && selectedLightIndex < 0) || debugColorShader_.id() == 0 || !axisGizmo_.valid()) {
        return;
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, width_, height_);
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_CULL_FACE);

    debugColorShader_.use();
    for (int lightIndex : activeLightIndices_) {
        const bool selected = lightIndex == selectedLightIndex;
        if (!showLightDebug_ && !selected) {
            continue;
        }

        const ActiveLightDebug& light = lightDebugInstances_[lightIndex];
        glm::mat4 axisModel(1.0f);
        axisModel = glm::translate(axisModel, light.position);
        if (light.type == LightType::Spot) {
            axisModel *= makeOrientationFromDirection(light.direction);
        }
        const float axisScale = std::clamp(light.radius * kLightGizmoScaleFactor, kLightGizmoScaleMin, kLightGizmoScaleMax);
        axisModel = glm::scale(axisModel, glm::vec3(axisScale));
        drawDebugMesh(axisGizmo_, axisModel, selected ? glm::vec4(1.0f, 0.92f, 0.40f, 1.0f) : glm::vec4(1.0f), false);

        if (light.type == LightType::Point && lightSphere_.valid()) {
            glm::mat4 sphereModel(1.0f);
            sphereModel = glm::translate(sphereModel, light.position);
            sphereModel = glm::scale(sphereModel, glm::vec3(light.radius));
            drawDebugMesh(lightSphere_, sphereModel, glm::vec4(light.color, selected ? 1.0f : kDebugVolumeAlpha), true);
        } else if (light.type == LightType::Spot && lightCone_.valid()) {
            glm::mat4 coneModel(1.0f);
            coneModel = glm::translate(coneModel, light.position);
            coneModel *= makeOrientationFromDirection(light.direction);
            const float coneRadius = light.radius * std::tan(glm::radians(light.outerAngle));
            coneModel = glm::scale(coneModel, glm::vec3(coneRadius, coneRadius, light.radius));
            drawDebugMesh(lightCone_, coneModel, glm::vec4(light.color, selected ? 1.0f : kDebugVolumeAlpha), true);
        }
    }

    if (showLightDebug_) {
        const glm::vec3 dir = glm::normalize(directionalLight_.direction);
        glm::mat4 directionalModel(1.0f);
        directionalModel = glm::translate(directionalModel, -dir * kDirectionalDebugAnchorDistance);
        directionalModel *= makeOrientationFromDirection(dir);
        directionalModel = glm::scale(directionalModel, glm::vec3(1.1f));
        drawDebugMesh(axisGizmo_, directionalModel, glm::vec4(1.0f), false);
    }

    glDisable(GL_BLEND);
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    glDepthMask(GL_TRUE);
}

void RenderEngine::drawSceneObjectSimple(const SceneObject& object) const {
    if (!object.visible || !object.mesh || !object.mesh->valid()) {
        return;
    }
    if (!object.transform) {
        return;
    }

    static const Material defaultMaterial{};
    const ShaderInputs inputs = resolveMaterialInputs(object.material ? *object.material : defaultMaterial);
    configureMaterialRasterState(inputs);

    const glm::mat4 model = composeTransform(*object.transform);
    const glm::mat3 normalMatrix = normalMatrixFromModel(model);
    glUniformMatrix4fv(simpleModelLocation_, 1, GL_FALSE, glm::value_ptr(model));
    glUniformMatrix3fv(simpleNormalMatrixLocation_, 1, GL_FALSE, glm::value_ptr(normalMatrix));
    bindMaterialUniforms(inputs, simpleMaterialLocations_);
    bindTextureRef(0, inputs.baseColorTexture);
    bindTextureRef(1, inputs.metallicRoughnessTexture);
    bindTextureRef(2, inputs.normalTexture);
    bindTextureRef(3, inputs.aoTexture);
    bindTextureRef(4, inputs.emissiveTexture);
    bindTextureRef(5, inputs.alphaTexture);
    bindTextureRef(6, inputs.clearcoatTexture);
    bindTextureRef(7, inputs.detailNormalTexture);
    bindTextureRef(8, inputs.heightTexture);
    object.mesh->draw();
}

void RenderEngine::drawSceneObjectDeferred(const SceneObject& object) const {
    if (!object.visible || !object.mesh || !object.mesh->valid()) {
        return;
    }
    if (!object.transform) {
        return;
    }

    static const Material defaultMaterial{};
    const ShaderInputs inputs = resolveMaterialInputs(object.material ? *object.material : defaultMaterial);
    configureMaterialRasterState(inputs);

    const glm::mat4 model = composeTransform(*object.transform);
    const glm::mat3 normalMatrix = normalMatrixFromModel(model);
    glUniformMatrix4fv(gbufferModelLocation_, 1, GL_FALSE, glm::value_ptr(model));
    glUniformMatrix3fv(gbufferNormalMatrixLocation_, 1, GL_FALSE, glm::value_ptr(normalMatrix));
    bindMaterialUniforms(inputs, gbufferMaterialLocations_);
    bindTextureRef(0, inputs.baseColorTexture);
    bindTextureRef(1, inputs.metallicRoughnessTexture);
    bindTextureRef(2, inputs.normalTexture);
    bindTextureRef(3, inputs.aoTexture);
    bindTextureRef(4, inputs.emissiveTexture);
    bindTextureRef(5, inputs.alphaTexture);
    bindTextureRef(6, inputs.clearcoatTexture);
    bindTextureRef(7, inputs.detailNormalTexture);
    bindTextureRef(8, inputs.heightTexture);
    object.mesh->draw();
}

void RenderEngine::drawSceneLayerSimple(RenderLayer layer) const {
    const GLboolean depthWrite = layer == RenderLayer::Ground ? GL_FALSE : GL_TRUE;
    glDepthMask(depthWrite);
    for (const SceneObject& object : sceneObjects_) {
        if (object.renderLayer == layer) {
            drawSceneObjectSimple(object);
        }
    }
    glDisable(GL_BLEND);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    glDepthMask(GL_TRUE);
}

void RenderEngine::drawSceneLayerDeferred(RenderLayer layer) const {
    const GLboolean depthWrite = layer == RenderLayer::Ground ? GL_FALSE : GL_TRUE;
    glDepthMask(depthWrite);
    for (const SceneObject& object : sceneObjects_) {
        if (object.renderLayer == layer) {
            drawSceneObjectDeferred(object);
        }
    }
    glDisable(GL_BLEND);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    glDepthMask(GL_TRUE);
}

std::vector<ShadowSystem::ShadowRenderable> RenderEngine::collectShadowRenderables() const {
    std::vector<ShadowSystem::ShadowRenderable> renderables;
    renderables.reserve(sceneObjects_.size());
    for (const SceneObject& object : sceneObjects_) {
        if (!object.visible || !object.mesh || !object.mesh->valid()) {
            continue;
        }
        if (!object.transform) {
            continue;
        }
        renderables.push_back(ShadowSystem::ShadowRenderable{object.mesh, composeTransform(*object.transform)});
    }
    return renderables;
}

void RenderEngine::evaluateLightTransform(
    const LightInstance& light,
    float timeSeconds,
    glm::vec3& position,
    glm::vec3& direction
) const {
    position = light.basePosition;
    if (light.isMovable) {
        const float phase = light.phase + timeSeconds;
        const float orbitScale = light.type == LightType::Spot ? 2.25f : 0.55f;
        const float bobScale = light.type == LightType::Spot ? 2.15f : 0.35f;

        position.x += orbitScale * std::cos(phase * 0.7f);
        position.z += orbitScale * std::sin(phase * 0.9f);
        position.y += bobScale * std::sin(phase * 1.3f);
    }

    direction = glm::vec3(0.0f);
    if (light.type == LightType::Spot) {
        direction = glm::normalize(light.target - position);
    }
}

void RenderEngine::assignStaticLightToVolume(int lightIndex) {
    if (lightIndex < 0 || lightIndex >= static_cast<int>(lights_.size())) {
        return;
    }

    const LightInstance& light = lights_[lightIndex];
    for (auto& volume : lightVolumes_) {
        if (volume.intersectsSphere(light.basePosition, light.radius)) {
            volume.attachStaticLight(lightIndex);
            return;
        }
    }
}

void RenderEngine::removeStaticLightFromVolumes(int lightIndex) {
    for (auto& volume : lightVolumes_) {
        volume.detachStaticLight(lightIndex);
    }
}

void RenderEngine::rebuildMovableAssignments(float timeSeconds) {
    for (auto& volume : lightVolumes_) {
        volume.clearMovableLights();
    }

    for (int lightIndex = 0; lightIndex < static_cast<int>(lights_.size()); ++lightIndex) {
        const LightInstance& light = lights_[lightIndex];
        if (!light.isMovable) {
            continue;
        }

        glm::vec3 position(0.0f);
        glm::vec3 direction(0.0f);
        evaluateLightTransform(light, timeSeconds, position, direction);
        for (auto& volume : lightVolumes_) {
            if (volume.intersectsSphere(position, light.radius)) {
                volume.addMovableLight(lightIndex);
            }
        }
    }

    movableAssignmentsDirty_ = false;
}

void RenderEngine::handleLightMovableChanged(int lightIndex, bool isMovable) {
    if (lightIndex < 0 || lightIndex >= static_cast<int>(lights_.size())) {
        return;
    }

    removeStaticLightFromVolumes(lightIndex);
    movableAssignmentsDirty_ = true;
    rebuildMovableAssignments(currentTimeSeconds());
    if (!isMovable) {
        assignStaticLightToVolume(lightIndex);
    }
}

void RenderEngine::buildLights() {
    lights_.clear();
    lightDebugInstances_.clear();
    activeLightIndices_.clear();
    lightVolumes_.clear();
    movableAssignmentsDirty_ = false;

    directionalLight_.direction = glm::normalize(glm::vec3(-0.3f, -1.0f, -0.4f));
    directionalLight_.color = glm::vec3(0.0f);
    directionalLight_.intensity = 0.0f;
    lightVolumes_.emplace_back(glm::vec3(-100.0f), glm::vec3(100.0f));

    auto registerLight = [this](LightInstance light) {
        const int lightIndex = static_cast<int>(lights_.size());
        light.setIndex(lightIndex);
        light.setMovableChangedCallback([this](int changedLightIndex, bool isMovable) {
            handleLightMovableChanged(changedLightIndex, isMovable);
        });
        lights_.push_back(std::move(light));
        if (!lights_.back().isMovable) {
            assignStaticLightToVolume(lightIndex);
        }
    };

    LightInstance point{};
    point.basePosition = glm::vec3(1.5f, 1.2f, 0.0f);
    point.radius = 40.0f;
    point.color = glm::vec3(0.9f, 0.7f, 1.0f);
    point.intensity = 20.0f;
    point.target = glm::vec3(0.0f);
    point.type = LightType::Point;
    point.phase = 0.0f;
    point.isMovable = false;
    point.castsShadow = true;
    point.shadowBiasMin = 0.000015f;
    point.shadowBiasSlope = 0.0045f;
    registerLight(point);

    LightInstance spot{};
    spot.basePosition = glm::vec3(5.5f, 10.2f, 0.0f);
    spot.radius = 32.0f;
    spot.color = glm::vec3(0.55f, 0.70f, 0.95f);
    spot.intensity = 1.4f;
    spot.target = glm::vec3(-3.0f, 1.2f, -8.0f);
    spot.innerAngle = 15.0f;
    spot.outerAngle = 25.0f;
    spot.type = LightType::Spot;
    spot.phase = 0.0f;
    spot.isMovable = false;
    spot.castsShadow = true;
    spot.shadowBiasMin = 0.0012f;
    spot.shadowBiasSlope = 0.004f;
    registerLight(spot);

    gpuLights_.resize(lights_.size());
    lightDebugInstances_.resize(lights_.size());
    movableAssignmentsDirty_ = true;
}

void RenderEngine::updateLights() {
    const bool deferred = rendererPath_ == RendererPath::Deferred41;
    activeLightIndices_.clear();
    lightDebugInstances_.assign(lights_.size(), ActiveLightDebug{});
    lightCount_ = 0;
    pointLightCount_ = 0;
    spotLightCount_ = 0;
    cameraInsideLightVolume_ = false;

    if (deferred) {
        shadowSystem_.beginFrame();
    }
    if (lights_.empty()) {
        return;
    }

    const float time = currentTimeSeconds();
    const glm::mat4 invView = deferred ? glm::inverse(view_) : glm::mat4(1.0f);
    if (movableAssignmentsDirty_) {
        rebuildMovableAssignments(time);
    }
    if (deferred) {
        gpuLights_.assign(lights_.size(), GpuLight{});
    }

    std::vector<bool> processed(lights_.size(), false);
    auto appendLight = [&](int lightIndex) {
        if (lightIndex < 0 || lightIndex >= static_cast<int>(lights_.size()) || processed[lightIndex]) {
            return;
        }
        processed[lightIndex] = true;

        const LightInstance& light = lights_[lightIndex];
        glm::vec3 position(0.0f);
        glm::vec3 direction(0.0f);
        evaluateLightTransform(light, time, position, direction);

        activeLightIndices_.push_back(lightIndex);
        ActiveLightDebug debug{};
        debug.position = position;
        debug.radius = light.radius;
        debug.color = light.color;
        debug.outerAngle = light.outerAngle;
        debug.direction = direction;
        debug.type = light.type;
        lightDebugInstances_[lightIndex] = debug;

        if (light.type == LightType::Point) {
            pointLightCount_++;
        } else {
            spotLightCount_++;
        }

        if (!deferred) {
            return;
        }

        const glm::vec3 viewPos = glm::vec3(view_ * glm::vec4(position, 1.0f));
        if (glm::length(viewPos) < light.radius) {
            cameraInsideLightVolume_ = true;
        }
        glm::vec3 viewDir(0.0f);
        if (light.type == LightType::Spot) {
            viewDir = glm::normalize(glm::mat3(view_) * direction);
        }

        int shadowType = 0;
        int shadowIndex = -1;
        if (light.castsShadow) {
            if (light.type == LightType::Spot) {
                ShadowSystem::SpotShadowDesc desc{
                    position,
                    direction,
                    light.radius,
                    light.outerAngle,
                    light.shadowBiasMin,
                    light.shadowBiasSlope
                };
                shadowIndex = shadowSystem_.registerSpotShadow(desc, invView);
                if (shadowIndex >= 0) {
                    shadowType = 1;
                }
            } else {
                ShadowSystem::PointShadowDesc desc{
                    position,
                    light.radius,
                    light.shadowBiasMin,
                    light.shadowBiasSlope
                };
                shadowIndex = shadowSystem_.registerPointShadow(desc);
                if (shadowIndex >= 0) {
                    shadowType = 2;
                }
            }
        }
        if (shadowType == 0) {
            shadowIndex = 0;
        }

        GpuLight gpu{};
        gpu.positionRadius = glm::vec4(viewPos, light.radius);
        gpu.colorIntensity = glm::vec4(light.color, light.intensity);
        gpu.directionType = glm::vec4(viewDir, static_cast<float>(light.type));
        gpu.shadowInfo = glm::vec4(
            static_cast<float>(shadowType),
            static_cast<float>(shadowIndex),
            light.shadowBiasMin,
            light.shadowBiasSlope
        );

        if (light.type == LightType::Spot) {
            const float inner = std::cos(glm::radians(light.innerAngle));
            const float outer = std::cos(glm::radians(light.outerAngle));
            const float tanOuter = std::tan(glm::radians(light.outerAngle));
            gpu.spotParams = glm::vec4(inner, outer, light.radius, tanOuter);
        } else {
            gpu.spotParams = glm::vec4(0.0f);
        }

        gpuLights_[lightIndex] = gpu;
    };

    for (const auto& volume : lightVolumes_) {
        for (int lightIndex : volume.staticLightIndices()) {
            appendLight(lightIndex);
        }
        for (int lightIndex : volume.movableLightIndices()) {
            appendLight(lightIndex);
        }
    }

    lightCount_ = static_cast<int>(activeLightIndices_.size());
    if (!deferred) {
        return;
    }

    if (lightsTbo_ == 0) {
        glGenBuffers(1, &lightsTbo_);
    }
    if (lightsTboTex_ == 0) {
        glGenTextures(1, &lightsTboTex_);
    }

    const GLsizeiptr bufferSize = static_cast<GLsizeiptr>(gpuLights_.size() * sizeof(GpuLight));
    if (bufferSize == 0) {
        lightTboSize_ = 0;
        return;
    }

    glBindBuffer(GL_TEXTURE_BUFFER, lightsTbo_);
    if (bufferSize != lightTboSize_) {
        glBufferData(GL_TEXTURE_BUFFER, bufferSize, gpuLights_.data(), GL_DYNAMIC_DRAW);
        lightTboSize_ = bufferSize;
    } else {
        glBufferSubData(GL_TEXTURE_BUFFER, 0, bufferSize, gpuLights_.data());
    }

    glBindTexture(GL_TEXTURE_BUFFER, lightsTboTex_);
    glTexBuffer(GL_TEXTURE_BUFFER, GL_RGBA32F, lightsTbo_);
}

bool RenderEngine::buildVolumeMeshes() {
    Mesh sphereMesh;
    buildSphereMesh(16, 24, sphereMesh);

    Mesh coneMesh;
    buildConeMesh(24, coneMesh);

    return lightSphere_.upload(sphereMesh) && lightCone_.upload(coneMesh);
}

bool RenderEngine::buildDebugMeshes() {
    Mesh axisMesh;
    addBox(
        glm::vec3(-kAxisCenterHalfExtent),
        glm::vec3(kAxisCenterHalfExtent),
        glm::vec4(0.95f, 0.95f, 0.95f, 1.0f),
        axisMesh
    );
    addBox(
        glm::vec3(0.0f, -kAxisThickness, -kAxisThickness),
        glm::vec3(kAxisLength, kAxisThickness, kAxisThickness),
        glm::vec4(0.95f, 0.20f, 0.18f, 1.0f),
        axisMesh
    );
    addBox(
        glm::vec3(-kAxisThickness, 0.0f, -kAxisThickness),
        glm::vec3(kAxisThickness, kAxisLength, kAxisThickness),
        glm::vec4(0.20f, 0.92f, 0.24f, 1.0f),
        axisMesh
    );
    addBox(
        glm::vec3(-kAxisThickness, -kAxisThickness, 0.0f),
        glm::vec3(kAxisThickness, kAxisThickness, kAxisLength),
        glm::vec4(0.18f, 0.48f, 0.96f, 1.0f),
        axisMesh
    );

    Mesh selectionMesh;
    addBox(
        glm::vec3(-1.0f, -1.0f, -1.0f),
        glm::vec3(1.0f, 1.0f, 1.0f),
        glm::vec4(1.0f),
        selectionMesh
    );

    return axisGizmo_.upload(axisMesh) &&
           selectionBox_.upload(selectionMesh);
}

void RenderEngine::buildScene() {
    sceneReady_ = false;
    sceneObjects_.clear();

    const std::string shaderRoot = shaderRootPath();
    if (!debugColorShader_.buildFromFiles(shaderRoot + "debug_color.vert", shaderRoot + "debug_color.frag")) {
        spdlog::error("RenderEngine: failed to build debug overlay shader");
        return;
    }
    debugMvpLocation_ = debugColorShader_.uniformLocation("uMVP");
    debugColorLocation_ = debugColorShader_.uniformLocation("uColor");

    if (rendererPath_ == RendererPath::SimpleForward) {
        if (!simpleShader_.buildFromFiles(shaderRoot + "simple.vert", shaderRoot + "simple.frag")) {
            spdlog::error("RenderEngine: failed to build simple shaders");
            return;
        }
        simpleModelLocation_ = simpleShader_.uniformLocation("uModel");
        simpleViewLocation_ = simpleShader_.uniformLocation("uView");
        simpleProjLocation_ = simpleShader_.uniformLocation("uProj");
        simpleNormalMatrixLocation_ = simpleShader_.uniformLocation("uNormalMatrix");
        simpleLightDirLocation_ = simpleShader_.uniformLocation("uLightDir");
        const auto captureSimpleMaterialLocations = [this](ShaderProgram& shader, MaterialUniformLocations& locations) {
            locations.baseColorFactor = shader.uniformLocation("uBaseColorFactor");
            locations.metallicFactor = shader.uniformLocation("uMetallicFactor");
            locations.roughnessFactor = shader.uniformLocation("uRoughnessFactor");
            locations.normalScale = shader.uniformLocation("uNormalScale");
            locations.aoStrength = shader.uniformLocation("uAoStrength");
            locations.emissiveFactor = shader.uniformLocation("uEmissiveFactor");
            locations.emissiveStrength = shader.uniformLocation("uEmissiveStrength");
            locations.alphaFactor = shader.uniformLocation("uAlphaFactor");
            locations.alphaMode = shader.uniformLocation("uAlphaMode");
            locations.alphaCutoff = shader.uniformLocation("uAlphaCutoff");
            locations.clearcoatFactor = shader.uniformLocation("uClearcoatFactor");
            locations.clearcoatRoughness = shader.uniformLocation("uClearcoatRoughness");
            locations.detailNormalScale = shader.uniformLocation("uDetailNormalScale");
            locations.heightScale = shader.uniformLocation("uHeightScale");
            locations.baseColorUvSet = shader.uniformLocation("uBaseColorUvSet");
            locations.metallicRoughnessUvSet = shader.uniformLocation("uMetallicRoughnessUvSet");
            locations.normalUvSet = shader.uniformLocation("uNormalUvSet");
            locations.aoUvSet = shader.uniformLocation("uAoUvSet");
            locations.emissiveUvSet = shader.uniformLocation("uEmissiveUvSet");
            locations.alphaUvSet = shader.uniformLocation("uAlphaUvSet");
            locations.clearcoatUvSet = shader.uniformLocation("uClearcoatUvSet");
            locations.detailNormalUvSet = shader.uniformLocation("uDetailNormalUvSet");
            locations.heightUvSet = shader.uniformLocation("uHeightUvSet");
            locations.baseColorUvTransform = shader.uniformLocation("uBaseColorUvTransform");
            locations.metallicRoughnessUvTransform = shader.uniformLocation("uMetallicRoughnessUvTransform");
            locations.normalUvTransform = shader.uniformLocation("uNormalUvTransform");
            locations.aoUvTransform = shader.uniformLocation("uAoUvTransform");
            locations.emissiveUvTransform = shader.uniformLocation("uEmissiveUvTransform");
            locations.alphaUvTransform = shader.uniformLocation("uAlphaUvTransform");
            locations.clearcoatUvTransform = shader.uniformLocation("uClearcoatUvTransform");
            locations.detailNormalUvTransform = shader.uniformLocation("uDetailNormalUvTransform");
            locations.heightUvTransform = shader.uniformLocation("uHeightUvTransform");
        };
        captureSimpleMaterialLocations(simpleShader_, simpleMaterialLocations_);

        if (!buildTextureLibrary()) {
            spdlog::error("RenderEngine: failed to build default texture library");
            return;
        }
        prebindMaterialDefaults();

        simpleShader_.use();
        glUniform1i(simpleShader_.uniformLocation("uBaseColorTexture"), 0);
        glUniform1i(simpleShader_.uniformLocation("uMetallicRoughnessTexture"), 1);
        glUniform1i(simpleShader_.uniformLocation("uNormalTexture"), 2);
        glUniform1i(simpleShader_.uniformLocation("uAoTexture"), 3);
        glUniform1i(simpleShader_.uniformLocation("uEmissiveTexture"), 4);
        glUniform1i(simpleShader_.uniformLocation("uAlphaTexture"), 5);
        glUniform1i(simpleShader_.uniformLocation("uClearcoatTexture"), 6);
        glUniform1i(simpleShader_.uniformLocation("uDetailNormalTexture"), 7);
        glUniform1i(simpleShader_.uniformLocation("uHeightTexture"), 8);
    } else {
        if (!deferredGeometryShader_.buildFromFiles(shaderRoot + "deferred_gbuffer.vert", shaderRoot + "deferred_gbuffer.frag") ||
            !deferredDirLightShader_.buildFromFiles(shaderRoot + "fullscreen_tri.vert", shaderRoot + "deferred_dir_light.frag") ||
            !deferredVolumeShader_.buildFromFiles(shaderRoot + "deferred_volume.vert", shaderRoot + "deferred_volume.frag") ||
            !deferredCompositeShader_.buildFromFiles(shaderRoot + "fullscreen_tri.vert", shaderRoot + "deferred_composite.frag")) {
            spdlog::error("RenderEngine: failed to build deferred shaders");
            return;
        }

        gbufferModelLocation_ = deferredGeometryShader_.uniformLocation("uModel");
        gbufferViewLocation_ = deferredGeometryShader_.uniformLocation("uView");
        gbufferProjLocation_ = deferredGeometryShader_.uniformLocation("uProj");
        gbufferNormalMatrixLocation_ = deferredGeometryShader_.uniformLocation("uNormalMatrix");
        const auto captureGeometryMaterialLocations = [this](ShaderProgram& shader, MaterialUniformLocations& locations) {
            locations.baseColorFactor = shader.uniformLocation("uBaseColorFactor");
            locations.metallicFactor = shader.uniformLocation("uMetallicFactor");
            locations.roughnessFactor = shader.uniformLocation("uRoughnessFactor");
            locations.normalScale = shader.uniformLocation("uNormalScale");
            locations.aoStrength = shader.uniformLocation("uAoStrength");
            locations.emissiveFactor = shader.uniformLocation("uEmissiveFactor");
            locations.emissiveStrength = shader.uniformLocation("uEmissiveStrength");
            locations.alphaFactor = shader.uniformLocation("uAlphaFactor");
            locations.alphaMode = shader.uniformLocation("uAlphaMode");
            locations.alphaCutoff = shader.uniformLocation("uAlphaCutoff");
            locations.clearcoatFactor = shader.uniformLocation("uClearcoatFactor");
            locations.clearcoatRoughness = shader.uniformLocation("uClearcoatRoughness");
            locations.detailNormalScale = shader.uniformLocation("uDetailNormalScale");
            locations.heightScale = shader.uniformLocation("uHeightScale");
            locations.baseColorUvSet = shader.uniformLocation("uBaseColorUvSet");
            locations.metallicRoughnessUvSet = shader.uniformLocation("uMetallicRoughnessUvSet");
            locations.normalUvSet = shader.uniformLocation("uNormalUvSet");
            locations.aoUvSet = shader.uniformLocation("uAoUvSet");
            locations.emissiveUvSet = shader.uniformLocation("uEmissiveUvSet");
            locations.alphaUvSet = shader.uniformLocation("uAlphaUvSet");
            locations.clearcoatUvSet = shader.uniformLocation("uClearcoatUvSet");
            locations.detailNormalUvSet = shader.uniformLocation("uDetailNormalUvSet");
            locations.heightUvSet = shader.uniformLocation("uHeightUvSet");
            locations.baseColorUvTransform = shader.uniformLocation("uBaseColorUvTransform");
            locations.metallicRoughnessUvTransform = shader.uniformLocation("uMetallicRoughnessUvTransform");
            locations.normalUvTransform = shader.uniformLocation("uNormalUvTransform");
            locations.aoUvTransform = shader.uniformLocation("uAoUvTransform");
            locations.emissiveUvTransform = shader.uniformLocation("uEmissiveUvTransform");
            locations.alphaUvTransform = shader.uniformLocation("uAlphaUvTransform");
            locations.clearcoatUvTransform = shader.uniformLocation("uClearcoatUvTransform");
            locations.detailNormalUvTransform = shader.uniformLocation("uDetailNormalUvTransform");
            locations.heightUvTransform = shader.uniformLocation("uHeightUvTransform");
        };
        captureGeometryMaterialLocations(deferredGeometryShader_, gbufferMaterialLocations_);
        deferredInvProjLocation_ = deferredDirLightShader_.uniformLocation("uInvProj");
        deferredDirLightDirLocation_ = deferredDirLightShader_.uniformLocation("uDirLightDir");
        deferredDirLightColorLocation_ = deferredDirLightShader_.uniformLocation("uDirLightColor");
        deferredDirLightIntensityLocation_ = deferredDirLightShader_.uniformLocation("uDirLightIntensity");
        deferredAmbientLocation_ = deferredDirLightShader_.uniformLocation("uAmbient");
        volumeProjLocation_ = deferredVolumeShader_.uniformLocation("uProj");
        volumeInvProjLocation_ = deferredVolumeShader_.uniformLocation("uInvProj");
        volumeScreenSizeLocation_ = deferredVolumeShader_.uniformLocation("uScreenSize");
        volumeLightOffsetLocation_ = deferredVolumeShader_.uniformLocation("uLightOffset");
        volumeIsSpotLocation_ = deferredVolumeShader_.uniformLocation("uIsSpot");
        volumeRenderFullscreenLocation_ = deferredVolumeShader_.uniformLocation("uRenderFullscreen");
        volumeBoundsMinLocation_ = deferredVolumeShader_.uniformLocation("uVolumeMin");
        volumeBoundsMaxLocation_ = deferredVolumeShader_.uniformLocation("uVolumeMax");
        volumeInvViewLocation_ = deferredVolumeShader_.uniformLocation("uInvView");
        volumeSpotShadowMatrixLocation_ = deferredVolumeShader_.uniformLocation("uSpotShadowMatrices[0]");
        volumeSpotShadowCountLocation_ = deferredVolumeShader_.uniformLocation("uSpotShadowCount");
        volumeSpotShadowTexelSizeLocation_ = deferredVolumeShader_.uniformLocation("uSpotShadowTexelSize");
        volumeSpotShadowPcfRadiusLocation_ = deferredVolumeShader_.uniformLocation("uSpotShadowPcfRadius");
        volumePointShadowCountLocation_ = deferredVolumeShader_.uniformLocation("uPointShadowCount");
        volumePointShadowDiskRadiusLocation_ = deferredVolumeShader_.uniformLocation("uPointShadowDiskRadius");
        volumePointShadowPcfRadiusLocation_ = deferredVolumeShader_.uniformLocation("uPointShadowPcfRadius");
        compositeDebugModeLocation_ = deferredCompositeShader_.uniformLocation("uDebugMode");
        deferredShadowMapLocation_ = deferredDirLightShader_.uniformLocation("uShadowMap");
        deferredShadowMatrixLocation_ = deferredDirLightShader_.uniformLocation("uShadowMatrices[0]");
        deferredCascadeSplitsLocation_ = deferredDirLightShader_.uniformLocation("uCascadeSplits[0]");
        deferredCascadeCountLocation_ = deferredDirLightShader_.uniformLocation("uCascadeCount");
        deferredShadowTexelSizeLocation_ = deferredDirLightShader_.uniformLocation("uShadowTexelSize");
        deferredShadowBiasMinLocation_ = deferredDirLightShader_.uniformLocation("uShadowBiasMin");
        deferredShadowBiasSlopeLocation_ = deferredDirLightShader_.uniformLocation("uShadowBiasSlope");
        deferredShadowPcfRadiusLocation_ = deferredDirLightShader_.uniformLocation("uShadowPcfRadius");
        compositeShadowMapLocation_ = deferredCompositeShader_.uniformLocation("uShadowMap");
        compositeShadowMatrixLocation_ = deferredCompositeShader_.uniformLocation("uShadowMatrices[0]");
        compositeCascadeSplitsLocation_ = deferredCompositeShader_.uniformLocation("uCascadeSplits[0]");
        compositeCascadeCountLocation_ = deferredCompositeShader_.uniformLocation("uCascadeCount");
        compositeShadowTexelSizeLocation_ = deferredCompositeShader_.uniformLocation("uShadowTexelSize");
        compositeShadowPcfRadiusLocation_ = deferredCompositeShader_.uniformLocation("uShadowPcfRadius");
        compositeInvProjLocation_ = deferredCompositeShader_.uniformLocation("uInvProj");
        compositeShadowBiasMinLocation_ = deferredCompositeShader_.uniformLocation("uShadowBiasMin");
        compositeShadowBiasSlopeLocation_ = deferredCompositeShader_.uniformLocation("uShadowBiasSlope");
        compositeShadowDebugCascadeLocation_ = deferredCompositeShader_.uniformLocation("uShadowDebugCascade");
        compositeDirLightDirLocation_ = deferredCompositeShader_.uniformLocation("uDirLightDir");

        if (!buildTextureLibrary()) {
            spdlog::error("RenderEngine: failed to build default texture library");
            return;
        }
        prebindMaterialDefaults();

        deferredDirLightShader_.use();
        glUniform1i(deferredDirLightShader_.uniformLocation("uGAlbedoMetal"), 0);
        glUniform1i(deferredDirLightShader_.uniformLocation("uGNormalRough"), 1);
        glUniform1i(deferredDirLightShader_.uniformLocation("uGEmissiveAo"), 2);
        glUniform1i(deferredDirLightShader_.uniformLocation("uGClearcoat"), 3);
        glUniform1i(deferredDirLightShader_.uniformLocation("uDepth"), 4);
        glUniform1i(deferredShadowMapLocation_, 5);

        deferredVolumeShader_.use();
        glUniform1i(deferredVolumeShader_.uniformLocation("uGAlbedoMetal"), 0);
        glUniform1i(deferredVolumeShader_.uniformLocation("uGNormalRough"), 1);
        glUniform1i(deferredVolumeShader_.uniformLocation("uGEmissiveAo"), 2);
        glUniform1i(deferredVolumeShader_.uniformLocation("uGClearcoat"), 3);
        glUniform1i(deferredVolumeShader_.uniformLocation("uDepth"), 4);
        glUniform1i(deferredVolumeShader_.uniformLocation("uLightBuffer"), 5);
        glUniform1i(deferredVolumeShader_.uniformLocation("uSpotShadowMap"), 6);
        glUniform1i(deferredVolumeShader_.uniformLocation("uPointShadowMap"), 7);

        deferredCompositeShader_.use();
        glUniform1i(deferredCompositeShader_.uniformLocation("uLightBuffer"), 0);
        glUniform1i(deferredCompositeShader_.uniformLocation("uGAlbedoMetal"), 1);
        glUniform1i(deferredCompositeShader_.uniformLocation("uGNormalRough"), 2);
        glUniform1i(deferredCompositeShader_.uniformLocation("uGEmissiveAo"), 3);
        glUniform1i(deferredCompositeShader_.uniformLocation("uGClearcoat"), 4);
        glUniform1i(deferredCompositeShader_.uniformLocation("uDepth"), 5);
        glUniform1i(compositeShadowMapLocation_, 6);

        deferredGeometryShader_.use();
        glUniform1i(deferredGeometryShader_.uniformLocation("uBaseColorTexture"), 0);
        glUniform1i(deferredGeometryShader_.uniformLocation("uMetallicRoughnessTexture"), 1);
        glUniform1i(deferredGeometryShader_.uniformLocation("uNormalTexture"), 2);
        glUniform1i(deferredGeometryShader_.uniformLocation("uAoTexture"), 3);
        glUniform1i(deferredGeometryShader_.uniformLocation("uEmissiveTexture"), 4);
        glUniform1i(deferredGeometryShader_.uniformLocation("uAlphaTexture"), 5);
        glUniform1i(deferredGeometryShader_.uniformLocation("uClearcoatTexture"), 6);
        glUniform1i(deferredGeometryShader_.uniformLocation("uDetailNormalTexture"), 7);
        glUniform1i(deferredGeometryShader_.uniformLocation("uHeightTexture"), 8);

        if (!shadowSystem_.init(shaderRoot)) {
            spdlog::error("RenderEngine: failed to init shadow system");
            return;
        }
    }

    buildLights();
    if (!buildVolumeMeshes() || !buildDebugMeshes()) {
        spdlog::error("RenderEngine: failed to build debug geometry");
        return;
    }

    const std::string texturesRoot = textureRootPath();
    auto soilBase = registerTexture(loadTextureFromFile(
        texturesRoot + "soil.jpg",
        "SoilBaseColor",
        true,
        TextureSemantic::BaseColor
    ));
    auto woodBase = registerTexture(loadTextureFromFile(
        texturesRoot + "wood.jpg",
        "WoodBaseColor",
        true,
        TextureSemantic::BaseColor
    ));
    auto rockBase = registerTexture(loadTextureFromFile(
        texturesRoot + "rock.jpg",
        "RockBaseColor",
        true,
        TextureSemantic::BaseColor
    ));

    auto soilNormal = soilBase ? registerTexture(generateNormalTexture(*soilBase, "SoilNormal")) : nullptr;
    auto soilOrm = soilBase ? registerTexture(generateMetallicRoughnessTexture(*soilBase, "SoilORM", 0.0f, 0.10f)) : nullptr;
    auto soilAo = soilBase ? registerTexture(generateOcclusionTexture(*soilBase, "SoilAO")) : nullptr;
    auto soilHeight = soilBase ? registerTexture(generateHeightTexture(*soilBase, "SoilHeight")) : nullptr;

    auto woodNormal = woodBase ? registerTexture(generateNormalTexture(*woodBase, "WoodNormal", 5.0f)) : nullptr;
    auto woodOrm = woodBase ? registerTexture(generateMetallicRoughnessTexture(*woodBase, "WoodORM", 0.0f, -0.08f)) : nullptr;
    auto woodAo = woodBase ? registerTexture(generateOcclusionTexture(*woodBase, "WoodAO")) : nullptr;
    auto woodHeight = woodBase ? registerTexture(generateHeightTexture(*woodBase, "WoodHeight")) : nullptr;

    auto rockNormal = rockBase ? registerTexture(generateNormalTexture(*rockBase, "RockNormal", 6.0f)) : nullptr;
    auto rockOrm = rockBase ? registerTexture(generateMetallicRoughnessTexture(*rockBase, "RockORM", 0.0f, 0.14f)) : nullptr;
    auto rockAo = rockBase ? registerTexture(generateOcclusionTexture(*rockBase, "RockAO")) : nullptr;
    auto rockHeight = rockBase ? registerTexture(generateHeightTexture(*rockBase, "RockHeight")) : nullptr;

    auto applyTextureSet = [this](
        Material& material,
        const std::shared_ptr<Texture>& baseColor,
        const std::shared_ptr<Texture>& normal,
        const std::shared_ptr<Texture>& orm,
        const std::shared_ptr<Texture>& ao,
        const std::shared_ptr<Texture>& height,
        const glm::vec2& uvScale,
        float clearcoatFactor,
        float clearcoatRoughness,
        float detailNormalScale,
        float heightScale
    ) {
        UVTransform transform{};
        transform.scale = uvScale;
        material.baseColor = makeTextureRef(baseColor, defaultSampler_, 0, transform);
        material.normal = makeTextureRef(normal, defaultSampler_, 0, transform);
        material.metallicRoughness.texture = makeTextureRef(orm, defaultSampler_, 0, transform);
        material.ao = makeTextureRef(ao, defaultSampler_, 0, transform);
        material.height.texture = makeTextureRef(height, defaultSampler_, 0, transform);
        material.detailNormal.texture = makeTextureRef(normal, defaultSampler_, 0, transform);
        material.clearcoat.factor = clearcoatFactor;
        material.clearcoat.roughness = clearcoatRoughness;
        material.detailNormal.scale = detailNormalScale;
        material.height.scale = heightScale;
    };

    auto groundMaterial = createProceduralMaterial(
        "Ground Soil",
        soilBase,
        soilNormal,
        soilOrm,
        soilAo,
        soilHeight,
        glm::vec3(0.92f, 0.96f, 0.90f),
        glm::vec2(120.0f, 120.0f),
        0.0f,
        0.0f,
        0.85f
    );
    auto wallRockMaterial = createProceduralMaterial(
        "Wall Rock",
        rockBase,
        rockNormal,
        rockOrm,
        rockAo,
        rockHeight,
        glm::vec3(0.86f, 0.80f, 0.76f),
        glm::vec2(3.5f, 1.4f),
        0.0f,
        0.0f,
        1.15f
    );
    auto wallWoodMaterial = createProceduralMaterial(
        "Wall Wood",
        woodBase,
        woodNormal,
        woodOrm,
        woodAo,
        woodHeight,
        glm::vec3(0.98f, 0.94f, 0.86f),
        glm::vec2(2.5f, 1.25f),
        0.20f,
        0.25f,
        0.45f
    );

    Mesh groundMesh;
    groundMesh.uvSets.resize(2);
    const float g = 500.0f;
    addQuad(
        {{
            {glm::vec3(-g, 0.0f, -g), glm::vec3(0.0f, 1.0f, 0.0f), glm::vec2(0.0f, 0.0f), glm::vec2(0.0f), glm::vec4(1.0f)},
            {glm::vec3(-g, 0.0f, g), glm::vec3(0.0f, 1.0f, 0.0f), glm::vec2(0.0f, 1.0f), glm::vec2(0.0f), glm::vec4(1.0f)},
            {glm::vec3(g, 0.0f, g), glm::vec3(0.0f, 1.0f, 0.0f), glm::vec2(1.0f, 1.0f), glm::vec2(0.0f), glm::vec4(1.0f)},
            {glm::vec3(g, 0.0f, -g), glm::vec3(0.0f, 1.0f, 0.0f), glm::vec2(1.0f, 0.0f), glm::vec2(0.0f), glm::vec4(1.0f)},
        }},
        groundMesh
    );

    Mesh wallAMesh;
    Mesh wallBMesh;
    constexpr float wallHeight = 2.5f;
    constexpr float wallOffset = 3.0f;
    constexpr float wallLength = 5.0f;
    constexpr float wallThickness = 0.5f;
    addBox(
        glm::vec3(-wallThickness * 0.5f, 0.0f, -wallLength),
        glm::vec3(wallThickness * 0.5f, wallHeight, wallLength),
        glm::vec4(1.0f),
        wallAMesh
    );
    addBox(
        glm::vec3(-wallThickness * 0.5f, 0.0f, -wallLength),
        glm::vec3(wallThickness * 0.5f, wallHeight, wallLength),
        glm::vec4(1.0f),
        wallBMesh
    );

    StaticModelData characterModel;
    if (!loadStaticGltfModel(modelRootPath() + "Adventurer.glb", characterModel)) {
        spdlog::error("RenderEngine: failed to load character model");
        return;
    }

    StaticModelData houseModel;
    if (!loadStaticGltfModel(modelRootPath() + "FantasyHouse.glb", houseModel)) {
        spdlog::error("RenderEngine: failed to load house model");
        return;
    }

    constexpr float kHouseFootprint = 7.5f;
    if (!fitModelToFootprint(houseModel, kHouseFootprint)) {
        spdlog::error("RenderEngine: failed to fit house model to target footprint");
        return;
    }

    for (auto& section : houseModel.sections) {
        if (!section.material) {
            section.material = std::make_shared<Material>();
            section.material->name = section.name;
        }

        std::string lowered = section.material->name;
        std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });

        if (lowered.find("wood") != std::string::npos) {
            applyTextureSet(*section.material, woodBase, woodNormal, woodOrm, woodAo, woodHeight, glm::vec2(1.6f), 0.22f, 0.20f, 0.40f, 0.02f);
        } else if (lowered.find("stone") != std::string::npos) {
            applyTextureSet(*section.material, rockBase, rockNormal, rockOrm, rockAo, rockHeight, glm::vec2(1.5f), 0.0f, 0.0f, 1.15f, 0.04f);
        } else if (lowered.find("plaster") != std::string::npos) {
            applyTextureSet(*section.material, soilBase, soilNormal, soilOrm, soilAo, soilHeight, glm::vec2(1.1f), 0.03f, 0.45f, 0.35f, 0.015f);
        } else if (lowered.find("roof") != std::string::npos) {
            applyTextureSet(*section.material, rockBase, rockNormal, rockOrm, rockAo, rockHeight, glm::vec2(2.3f), 0.0f, 0.0f, 0.95f, 0.03f);
            section.material->baseColorFactor *= glm::vec3(1.05f, 0.82f, 0.72f);
        } else if (lowered.find("window") != std::string::npos) {
            applyTextureSet(*section.material, woodBase, woodNormal, woodOrm, woodAo, woodHeight, glm::vec2(1.2f), 0.12f, 0.18f, 0.25f, 0.01f);
            section.material->baseColorFactor *= glm::vec3(0.88f, 0.92f, 1.0f);
        }
    }

    MeshBuffer* groundBuffer = createSceneMesh(groundMesh);
    MeshBuffer* wallABuffer = createSceneMesh(wallAMesh);
    MeshBuffer* wallBBuffer = createSceneMesh(wallBMesh);
    if (!groundBuffer || !wallABuffer || !wallBBuffer) {
        spdlog::error("RenderEngine: failed to upload procedural scene meshes");
        return;
    }

    int nextId = 0;
    auto groundTransform = std::make_shared<TransformState>();
    auto wallATransform = std::make_shared<TransformState>();
    wallATransform->position = glm::vec3(-wallOffset, 0.0f, 0.0f);
    auto wallBTransform = std::make_shared<TransformState>();
    wallBTransform->position = glm::vec3(wallOffset, 0.0f, 0.0f);
    auto characterTransform = std::make_shared<TransformState>();
    auto houseTransform = std::make_shared<TransformState>();
    houseTransform->position = glm::vec3(-3.0f, 0.0f, -8.0f);
    houseTransform->rotationDeg = glm::vec3(0.0f, -35.0f, 0.0f);

    sceneObjects_.push_back(SceneObject{
        nextId++, "Ground", groundMaterial ? groundMaterial->name : std::string("Ground"), SceneObjectKind::Ground, RenderLayer::Ground,
        groundBuffer, groundMaterial, computeMeshBounds(groundMesh), groundTransform, true
    });
    sceneObjects_.push_back(SceneObject{
        nextId++, "Wall A", wallRockMaterial ? wallRockMaterial->name : std::string("Wall A"), SceneObjectKind::Wall, RenderLayer::Geometry,
        wallABuffer, wallRockMaterial, computeMeshBounds(wallAMesh), wallATransform, true
    });
    sceneObjects_.push_back(SceneObject{
        nextId++, "Wall B", wallWoodMaterial ? wallWoodMaterial->name : std::string("Wall B"), SceneObjectKind::Wall, RenderLayer::Geometry,
        wallBBuffer, wallWoodMaterial, computeMeshBounds(wallBMesh), wallBTransform, true
    });

    appendModelObjects("Character", SceneObjectKind::Model, RenderLayer::Actors, characterModel, characterTransform, nextId);
    appendModelObjects("House", SceneObjectKind::Model, RenderLayer::Geometry, houseModel, houseTransform, nextId);

    sceneReady_ = !sceneObjects_.empty();
}

void RenderEngine::ensureDeferredResources() {
    if (rendererPath_ != RendererPath::Deferred41 || width_ <= 0 || height_ <= 0) {
        return;
    }
    if (width_ == deferredWidth_ && height_ == deferredHeight_ && gbufferFbo_ != 0 && lightFbo_ != 0) {
        return;
    }

    destroyDeferredResources();
    deferredWidth_ = width_;
    deferredHeight_ = height_;

    glGenTextures(1, &gbufferAlbedo_);
    glBindTexture(GL_TEXTURE_2D, gbufferAlbedo_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width_, height_, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glGenTextures(1, &gbufferNormal_);
    glBindTexture(GL_TEXTURE_2D, gbufferNormal_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, width_, height_, 0, GL_RGBA, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glGenTextures(1, &gbufferEmissiveAo_);
    glBindTexture(GL_TEXTURE_2D, gbufferEmissiveAo_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, width_, height_, 0, GL_RGBA, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glGenTextures(1, &gbufferClearcoat_);
    glBindTexture(GL_TEXTURE_2D, gbufferClearcoat_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, width_, height_, 0, GL_RGBA, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glGenTextures(1, &gbufferDepthColor_);
    glBindTexture(GL_TEXTURE_2D, gbufferDepthColor_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R32F, width_, height_, 0, GL_RED, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glGenTextures(1, &gbufferDepth_);
    glBindTexture(GL_TEXTURE_2D, gbufferDepth_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH24_STENCIL8, width_, height_, 0, GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_NONE);
    glTexParameteri(GL_TEXTURE_2D, GL_DEPTH_STENCIL_TEXTURE_MODE, GL_DEPTH_COMPONENT);

    glGenFramebuffers(1, &gbufferFbo_);
    glBindFramebuffer(GL_FRAMEBUFFER, gbufferFbo_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, gbufferAlbedo_, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, gbufferNormal_, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT2, GL_TEXTURE_2D, gbufferEmissiveAo_, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT3, GL_TEXTURE_2D, gbufferClearcoat_, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT4, GL_TEXTURE_2D, gbufferDepthColor_, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, gbufferDepth_, 0);
    const GLenum gbufferAttachments[5] = {
        GL_COLOR_ATTACHMENT0,
        GL_COLOR_ATTACHMENT1,
        GL_COLOR_ATTACHMENT2,
        GL_COLOR_ATTACHMENT3,
        GL_COLOR_ATTACHMENT4
    };
    glDrawBuffers(5, gbufferAttachments);

    glGenTextures(1, &lightColor_);
    glBindTexture(GL_TEXTURE_2D, lightColor_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, width_, height_, 0, GL_RGBA, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glGenFramebuffers(1, &lightFbo_);
    glBindFramebuffer(GL_FRAMEBUFFER, lightFbo_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, lightColor_, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, gbufferDepth_, 0);
    glDrawBuffer(GL_COLOR_ATTACHMENT0);

    if (fullscreenVao_ == 0) {
        glGenVertexArrays(1, &fullscreenVao_);
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
}

void RenderEngine::destroyDeferredResources() {
    if (!glContext_) {
        return;
    }
    if (gbufferFbo_ != 0) glDeleteFramebuffers(1, &gbufferFbo_);
    if (lightFbo_ != 0) glDeleteFramebuffers(1, &lightFbo_);
    if (gbufferAlbedo_ != 0) glDeleteTextures(1, &gbufferAlbedo_);
    if (gbufferNormal_ != 0) glDeleteTextures(1, &gbufferNormal_);
    if (gbufferEmissiveAo_ != 0) glDeleteTextures(1, &gbufferEmissiveAo_);
    if (gbufferClearcoat_ != 0) glDeleteTextures(1, &gbufferClearcoat_);
    if (gbufferDepthColor_ != 0) glDeleteTextures(1, &gbufferDepthColor_);
    if (gbufferDepth_ != 0) glDeleteTextures(1, &gbufferDepth_);
    if (lightColor_ != 0) glDeleteTextures(1, &lightColor_);
    if (lightsTboTex_ != 0) glDeleteTextures(1, &lightsTboTex_);
    if (lightsTbo_ != 0) glDeleteBuffers(1, &lightsTbo_);
    if (fullscreenVao_ != 0) glDeleteVertexArrays(1, &fullscreenVao_);

    gbufferFbo_ = 0;
    lightFbo_ = 0;
    gbufferAlbedo_ = 0;
    gbufferNormal_ = 0;
    gbufferEmissiveAo_ = 0;
    gbufferClearcoat_ = 0;
    gbufferDepthColor_ = 0;
    gbufferDepth_ = 0;
    lightColor_ = 0;
    lightsTboTex_ = 0;
    lightsTbo_ = 0;
    fullscreenVao_ = 0;
    deferredWidth_ = 0;
    deferredHeight_ = 0;
    lightTboSize_ = 0;
}

void RenderEngine::renderDeferredScene() {
    if (gbufferFbo_ == 0 || lightFbo_ == 0) {
        return;
    }

    const auto clearSamplers = [](int maxUnitExclusive) {
        for (int unit = 0; unit < maxUnitExclusive; ++unit) {
            glBindSampler(unit, 0);
        }
    };

    updateLights();
    const auto shadowRenderables = collectShadowRenderables();
    const glm::vec3 dirLightWorld = glm::normalize(directionalLight_.direction);
    const glm::vec3 dirLightView = glm::normalize(glm::mat3(view_) * dirLightWorld);
    const glm::mat4 invView = glm::inverse(view_);

    shadowSystem_.updateDirectional(view_, projection_, dirLightWorld, kNearPlane, kFarPlane);
    shadowDebugCascade_ = std::clamp(shadowDebugCascade_, 0, shadowSystem_.directionalCascadeCount() - 1);
    shadowSystem_.renderDirectionalShadows(shadowRenderables);
    shadowSystem_.renderSpotShadows(shadowRenderables);
    shadowSystem_.renderPointShadows(shadowRenderables);

    glBindFramebuffer(GL_FRAMEBUFFER, gbufferFbo_);
    glViewport(0, 0, width_, height_);
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
    const GLfloat clearAlbedo[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    const GLfloat clearNormal[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    const GLfloat clearEmissiveAo[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    const GLfloat clearClearcoat[4] = {0.0f, 1.0f, 0.0f, 0.0f};
    const GLfloat clearDepth[4] = {1.0f, 0.0f, 0.0f, 0.0f};
    glClearBufferfv(GL_COLOR, 0, clearAlbedo);
    glClearBufferfv(GL_COLOR, 1, clearNormal);
    glClearBufferfv(GL_COLOR, 2, clearEmissiveAo);
    glClearBufferfv(GL_COLOR, 3, clearClearcoat);
    glClearBufferfv(GL_COLOR, 4, clearDepth);
    glClearBufferfi(GL_DEPTH_STENCIL, 0, 1.0f, 0);

    deferredGeometryShader_.use();
    glUniformMatrix4fv(gbufferViewLocation_, 1, GL_FALSE, glm::value_ptr(view_));
    glUniformMatrix4fv(gbufferProjLocation_, 1, GL_FALSE, glm::value_ptr(projection_));
    drawSceneLayerDeferred(RenderLayer::Ground);
    drawSceneLayerDeferred(RenderLayer::Geometry);
    drawSceneLayerDeferred(RenderLayer::Actors);

    glBindFramebuffer(GL_FRAMEBUFFER, lightFbo_);
    glViewport(0, 0, width_, height_);
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glDisable(GL_BLEND);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    deferredDirLightShader_.use();
    glUniformMatrix4fv(deferredInvProjLocation_, 1, GL_FALSE, glm::value_ptr(invProjection_));
    glUniform3fv(deferredDirLightDirLocation_, 1, glm::value_ptr(dirLightView));
    glUniform3fv(deferredDirLightColorLocation_, 1, glm::value_ptr(directionalLight_.color));
    glUniform1f(deferredDirLightIntensityLocation_, directionalLight_.intensity);
    glUniform3f(deferredAmbientLocation_, 0.0f, 0.0f, 0.0f);
    glUniformMatrix4fv(
        deferredShadowMatrixLocation_,
        shadowSystem_.directionalCascadeCount(),
        GL_FALSE,
        glm::value_ptr(shadowSystem_.directionalMatrices().front())
    );
    glUniform1fv(
        deferredCascadeSplitsLocation_,
        shadowSystem_.directionalCascadeCount(),
        shadowSystem_.directionalSplits().data()
    );
    glUniform1i(deferredCascadeCountLocation_, shadowSystem_.directionalCascadeCount());
    glUniform2f(
        deferredShadowTexelSizeLocation_,
        shadowSystem_.directionalTexelSize().x,
        shadowSystem_.directionalTexelSize().y
    );
    glUniform1f(deferredShadowBiasMinLocation_, shadowSystem_.directionalBiasMin());
    glUniform1f(deferredShadowBiasSlopeLocation_, shadowSystem_.directionalBiasSlope());
    glUniform1i(deferredShadowPcfRadiusLocation_, shadowSystem_.directionalPcfRadius());

    glActiveTexture(GL_TEXTURE0 + 0);
    glBindTexture(GL_TEXTURE_2D, gbufferAlbedo_);
    glActiveTexture(GL_TEXTURE0 + 1);
    glBindTexture(GL_TEXTURE_2D, gbufferNormal_);
    glActiveTexture(GL_TEXTURE0 + 2);
    glBindTexture(GL_TEXTURE_2D, gbufferEmissiveAo_);
    glActiveTexture(GL_TEXTURE0 + 3);
    glBindTexture(GL_TEXTURE_2D, gbufferClearcoat_);
    glActiveTexture(GL_TEXTURE0 + 4);
    glBindTexture(GL_TEXTURE_2D, gbufferDepthColor_);
    glActiveTexture(GL_TEXTURE0 + 5);
    glBindTexture(GL_TEXTURE_2D_ARRAY, shadowSystem_.directionalShadowMap());
    clearSamplers(6);

    glBindVertexArray(fullscreenVao_);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);

    if (lightCount_ > 0) {
        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE);
        glDisable(GL_DEPTH_TEST);
        glDepthMask(GL_FALSE);
        glDisable(GL_CULL_FACE);

        deferredVolumeShader_.use();
        glUniformMatrix4fv(volumeProjLocation_, 1, GL_FALSE, glm::value_ptr(projection_));
        glUniformMatrix4fv(volumeInvProjLocation_, 1, GL_FALSE, glm::value_ptr(invProjection_));
        glUniform2f(volumeScreenSizeLocation_, static_cast<float>(width_), static_cast<float>(height_));
        glUniformMatrix4fv(volumeInvViewLocation_, 1, GL_FALSE, glm::value_ptr(invView));
        glUniformMatrix4fv(
            volumeSpotShadowMatrixLocation_,
            shadowSystem_.spotShadowCount(),
            GL_FALSE,
            glm::value_ptr(shadowSystem_.spotShadowMatrices().front())
        );
        glUniform1i(volumeSpotShadowCountLocation_, shadowSystem_.spotShadowCount());
        glUniform2f(
            volumeSpotShadowTexelSizeLocation_,
            shadowSystem_.spotTexelSize().x,
            shadowSystem_.spotTexelSize().y
        );
        glUniform1i(volumeSpotShadowPcfRadiusLocation_, shadowSystem_.spotPcfRadius());
        glUniform1i(volumePointShadowCountLocation_, shadowSystem_.pointShadowCount());
        glUniform1f(volumePointShadowDiskRadiusLocation_, shadowSystem_.pointShadowDiskRadius());
        glUniform1i(volumePointShadowPcfRadiusLocation_, shadowSystem_.pointPcfRadius());
        glUniform1i(volumeRenderFullscreenLocation_, 1);

        glActiveTexture(GL_TEXTURE0 + 0);
        glBindTexture(GL_TEXTURE_2D, gbufferAlbedo_);
        glActiveTexture(GL_TEXTURE0 + 1);
        glBindTexture(GL_TEXTURE_2D, gbufferNormal_);
        glActiveTexture(GL_TEXTURE0 + 2);
        glBindTexture(GL_TEXTURE_2D, gbufferEmissiveAo_);
        glActiveTexture(GL_TEXTURE0 + 3);
        glBindTexture(GL_TEXTURE_2D, gbufferClearcoat_);
        glActiveTexture(GL_TEXTURE0 + 4);
        glBindTexture(GL_TEXTURE_2D, gbufferDepthColor_);
        glActiveTexture(GL_TEXTURE0 + 5);
        glBindTexture(GL_TEXTURE_BUFFER, lightsTboTex_);
        glActiveTexture(GL_TEXTURE0 + 6);
        glBindTexture(GL_TEXTURE_2D_ARRAY, shadowSystem_.spotShadowMap());
        glActiveTexture(GL_TEXTURE0 + 7);
        glBindTexture(GL_TEXTURE_CUBE_MAP_ARRAY, shadowSystem_.pointShadowMap());
        clearSamplers(8);

        glBindVertexArray(fullscreenVao_);
        for (const auto& volume : lightVolumes_) {
            glUniform3fv(volumeBoundsMinLocation_, 1, glm::value_ptr(volume.minCorner()));
            glUniform3fv(volumeBoundsMaxLocation_, 1, glm::value_ptr(volume.maxCorner()));
            for (int lightIndex : volume.staticLightIndices()) {
                glUniform1i(volumeIsSpotLocation_, lights_[lightIndex].type == LightType::Spot ? 1 : 0);
                glUniform1i(volumeLightOffsetLocation_, lightIndex);
                glDrawArrays(GL_TRIANGLES, 0, 3);
            }
            for (int lightIndex : volume.movableLightIndices()) {
                glUniform1i(volumeIsSpotLocation_, lights_[lightIndex].type == LightType::Spot ? 1 : 0);
                glUniform1i(volumeLightOffsetLocation_, lightIndex);
                glDrawArrays(GL_TRIANGLES, 0, 3);
            }
        }
        glBindVertexArray(0);

        glDisable(GL_BLEND);
        glEnable(GL_DEPTH_TEST);
        glEnable(GL_CULL_FACE);
        glDepthMask(GL_TRUE);
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, width_, height_);
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glDisable(GL_BLEND);

    deferredCompositeShader_.use();
    glUniform1i(compositeDebugModeLocation_, static_cast<int>(debugView_));
    glUniformMatrix4fv(
        compositeShadowMatrixLocation_,
        shadowSystem_.directionalCascadeCount(),
        GL_FALSE,
        glm::value_ptr(shadowSystem_.directionalMatrices().front())
    );
    glUniform1fv(
        compositeCascadeSplitsLocation_,
        shadowSystem_.directionalCascadeCount(),
        shadowSystem_.directionalSplits().data()
    );
    glUniform1i(compositeCascadeCountLocation_, shadowSystem_.directionalCascadeCount());
    glUniform2f(
        compositeShadowTexelSizeLocation_,
        shadowSystem_.directionalTexelSize().x,
        shadowSystem_.directionalTexelSize().y
    );
    glUniform1i(compositeShadowPcfRadiusLocation_, shadowSystem_.directionalPcfRadius());
    glUniformMatrix4fv(compositeInvProjLocation_, 1, GL_FALSE, glm::value_ptr(invProjection_));
    glUniform3fv(compositeDirLightDirLocation_, 1, glm::value_ptr(dirLightView));
    glUniform1f(compositeShadowBiasMinLocation_, shadowSystem_.directionalBiasMin());
    glUniform1f(compositeShadowBiasSlopeLocation_, shadowSystem_.directionalBiasSlope());
    glUniform1i(compositeShadowDebugCascadeLocation_, shadowDebugCascade_);

    glActiveTexture(GL_TEXTURE0 + 0);
    glBindTexture(GL_TEXTURE_2D, lightColor_);
    glActiveTexture(GL_TEXTURE0 + 1);
    glBindTexture(GL_TEXTURE_2D, gbufferAlbedo_);
    glActiveTexture(GL_TEXTURE0 + 2);
    glBindTexture(GL_TEXTURE_2D, gbufferNormal_);
    glActiveTexture(GL_TEXTURE0 + 3);
    glBindTexture(GL_TEXTURE_2D, gbufferEmissiveAo_);
    glActiveTexture(GL_TEXTURE0 + 4);
    glBindTexture(GL_TEXTURE_2D, gbufferClearcoat_);
    glActiveTexture(GL_TEXTURE0 + 5);
    glBindTexture(GL_TEXTURE_2D, gbufferDepthColor_);
    glActiveTexture(GL_TEXTURE0 + 6);
    glBindTexture(GL_TEXTURE_2D_ARRAY, shadowSystem_.directionalShadowMap());
    clearSamplers(7);

    glBindVertexArray(fullscreenVao_);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);

    renderSelectionOverlay();
    renderLightDebugOverlay(true);
    glDepthMask(GL_TRUE);
}

void RenderEngine::renderSimpleScene() {
    updateLights();

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, width_, height_);
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    simpleShader_.use();
    glUniformMatrix4fv(simpleViewLocation_, 1, GL_FALSE, glm::value_ptr(view_));
    glUniformMatrix4fv(simpleProjLocation_, 1, GL_FALSE, glm::value_ptr(projection_));
    const glm::vec3 dirLightView = glm::normalize(glm::mat3(view_) * glm::normalize(directionalLight_.direction));
    glUniform3fv(simpleLightDirLocation_, 1, glm::value_ptr(dirLightView));

    drawSceneLayerSimple(RenderLayer::Ground);
    drawSceneLayerSimple(RenderLayer::Geometry);
    drawSceneLayerSimple(RenderLayer::Actors);
    renderSelectionOverlay();
    renderLightDebugOverlay(true);
}

void RenderEngine::renderScene() {
    if (!sceneReady_) {
        return;
    }

    if (rendererPath_ == RendererPath::Deferred41) {
        renderDeferredScene();
    } else {
        renderSimpleScene();
    }
}

}  // namespace render
