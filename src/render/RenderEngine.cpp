#include "RenderEngine.hpp"

#include <SDL_opengl.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <utility>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_inverse.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <spdlog/spdlog.h>

#include "StaticGltfModel.hpp"

namespace {
constexpr float kIsoAngleX = 35.264f;  // atan(sqrt(1/2)) in degrees
constexpr float kIsoAngleY = 45.0f;
constexpr float kBaseOrthoSize = 10.0f;
constexpr float kMinZoom = 0.2f;
constexpr float kMaxZoom = 5.0f;
constexpr float kNearPlane = 0.10f;
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

struct Vertex {
    float px, py, pz;
    float nx, ny, nz;
    float r, g, b;
};

struct MeshBounds {
    glm::vec3 min{0.0f};
    glm::vec3 max{0.0f};
};

void addQuad(const std::array<Vertex, 4>& verts, std::vector<float>& outVerts, std::vector<unsigned int>& outIndices) {
    const unsigned int base = static_cast<unsigned int>(outVerts.size() / 9);
    for (const auto& v : verts) {
        outVerts.insert(outVerts.end(), {v.px, v.py, v.pz, v.nx, v.ny, v.nz, v.r, v.g, v.b});
    }
    outIndices.insert(outIndices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
}

void addBox(
    const glm::vec3& minCorner,
    const glm::vec3& maxCorner,
    const glm::vec3& color,
    std::vector<float>& outVerts,
    std::vector<unsigned int>& outIndices
) {
    const float minX = minCorner.x;
    const float minY = minCorner.y;
    const float minZ = minCorner.z;
    const float maxX = maxCorner.x;
    const float maxY = maxCorner.y;
    const float maxZ = maxCorner.z;
    const float r = color.r;
    const float g = color.g;
    const float b = color.b;

    const std::array<Vertex, 4> rightFace{{
        {maxX, minY, minZ, 1.0f, 0.0f, 0.0f, r, g, b},
        {maxX, maxY, minZ, 1.0f, 0.0f, 0.0f, r, g, b},
        {maxX, maxY, maxZ, 1.0f, 0.0f, 0.0f, r, g, b},
        {maxX, minY, maxZ, 1.0f, 0.0f, 0.0f, r, g, b},
    }};
    addQuad(rightFace, outVerts, outIndices);

    const std::array<Vertex, 4> leftFace{{
        {minX, minY, minZ, -1.0f, 0.0f, 0.0f, r, g, b},
        {minX, minY, maxZ, -1.0f, 0.0f, 0.0f, r, g, b},
        {minX, maxY, maxZ, -1.0f, 0.0f, 0.0f, r, g, b},
        {minX, maxY, minZ, -1.0f, 0.0f, 0.0f, r, g, b},
    }};
    addQuad(leftFace, outVerts, outIndices);

    const std::array<Vertex, 4> topFace{{
        {minX, maxY, minZ, 0.0f, 1.0f, 0.0f, r, g, b},
        {minX, maxY, maxZ, 0.0f, 1.0f, 0.0f, r, g, b},
        {maxX, maxY, maxZ, 0.0f, 1.0f, 0.0f, r, g, b},
        {maxX, maxY, minZ, 0.0f, 1.0f, 0.0f, r, g, b},
    }};
    addQuad(topFace, outVerts, outIndices);

    const std::array<Vertex, 4> bottomFace{{
        {minX, minY, minZ, 0.0f, -1.0f, 0.0f, r, g, b},
        {maxX, minY, minZ, 0.0f, -1.0f, 0.0f, r, g, b},
        {maxX, minY, maxZ, 0.0f, -1.0f, 0.0f, r, g, b},
        {minX, minY, maxZ, 0.0f, -1.0f, 0.0f, r, g, b},
    }};
    addQuad(bottomFace, outVerts, outIndices);

    const std::array<Vertex, 4> frontFace{{
        {minX, minY, maxZ, 0.0f, 0.0f, 1.0f, r, g, b},
        {maxX, minY, maxZ, 0.0f, 0.0f, 1.0f, r, g, b},
        {maxX, maxY, maxZ, 0.0f, 0.0f, 1.0f, r, g, b},
        {minX, maxY, maxZ, 0.0f, 0.0f, 1.0f, r, g, b},
    }};
    addQuad(frontFace, outVerts, outIndices);

    const std::array<Vertex, 4> backFace{{
        {minX, minY, minZ, 0.0f, 0.0f, -1.0f, r, g, b},
        {minX, maxY, minZ, 0.0f, 0.0f, -1.0f, r, g, b},
        {maxX, maxY, minZ, 0.0f, 0.0f, -1.0f, r, g, b},
        {maxX, minY, minZ, 0.0f, 0.0f, -1.0f, r, g, b},
    }};
    addQuad(backFace, outVerts, outIndices);
}

void pushVertex(const Vertex& v, std::vector<float>& outVerts) {
    outVerts.insert(outVerts.end(), {v.px, v.py, v.pz, v.nx, v.ny, v.nz, v.r, v.g, v.b});
}

MeshBounds computeMeshBounds(const render::StaticMeshData& mesh) {
    MeshBounds bounds{};
    if (mesh.vertices.size() < 9) {
        return bounds;
    }

    bounds.min = glm::vec3(mesh.vertices[0], mesh.vertices[1], mesh.vertices[2]);
    bounds.max = bounds.min;

    for (std::size_t offset = 0; offset + 8 < mesh.vertices.size(); offset += 9) {
        const glm::vec3 position(mesh.vertices[offset + 0], mesh.vertices[offset + 1], mesh.vertices[offset + 2]);
        bounds.min = glm::min(bounds.min, position);
        bounds.max = glm::max(bounds.max, position);
    }

    return bounds;
}

void transformStaticMesh(render::StaticMeshData& mesh, const glm::mat4& transform) {
    const glm::mat3 normalMatrix = glm::mat3(glm::transpose(glm::inverse(transform)));
    for (std::size_t offset = 0; offset + 8 < mesh.vertices.size(); offset += 9) {
        const glm::vec3 position(mesh.vertices[offset + 0], mesh.vertices[offset + 1], mesh.vertices[offset + 2]);
        const glm::vec3 normal(mesh.vertices[offset + 3], mesh.vertices[offset + 4], mesh.vertices[offset + 5]);

        const glm::vec3 transformedPosition = glm::vec3(transform * glm::vec4(position, 1.0f));
        glm::vec3 transformedNormal = normalMatrix * normal;
        if (glm::dot(transformedNormal, transformedNormal) > 1.0e-6f) {
            transformedNormal = glm::normalize(transformedNormal);
        }

        mesh.vertices[offset + 0] = transformedPosition.x;
        mesh.vertices[offset + 1] = transformedPosition.y;
        mesh.vertices[offset + 2] = transformedPosition.z;
        mesh.vertices[offset + 3] = transformedNormal.x;
        mesh.vertices[offset + 4] = transformedNormal.y;
        mesh.vertices[offset + 5] = transformedNormal.z;
    }
}

bool fitMeshToFootprint(render::StaticMeshData& mesh, float targetFootprint) {
    const MeshBounds bounds = computeMeshBounds(mesh);
    const glm::vec3 size = bounds.max - bounds.min;
    const float footprint = std::max(size.x, size.z);
    if (footprint <= 1.0e-4f) {
        return false;
    }

    const float scale = targetFootprint / footprint;
    transformStaticMesh(mesh, glm::scale(glm::mat4(1.0f), glm::vec3(scale)));
    return true;
}

void buildSphereMesh(int stacks, int slices, std::vector<float>& outVerts, std::vector<unsigned int>& outIndices) {
    outVerts.clear();
    outIndices.clear();

    const float pi = 3.1415926535f;
    const float twoPi = pi * 2.0f;

    for (int stack = 0; stack <= stacks; ++stack) {
        float v = static_cast<float>(stack) / static_cast<float>(stacks);
        float phi = v * pi;
        float y = std::cos(phi);
        float r = std::sin(phi);

        for (int slice = 0; slice <= slices; ++slice) {
            float u = static_cast<float>(slice) / static_cast<float>(slices);
            float theta = u * twoPi;
            float x = r * std::cos(theta);
            float z = r * std::sin(theta);

            Vertex vert{};
            vert.px = x;
            vert.py = y;
            vert.pz = z;
            vert.nx = x;
            vert.ny = y;
            vert.nz = z;
            vert.r = 1.0f;
            vert.g = 1.0f;
            vert.b = 1.0f;
            pushVertex(vert, outVerts);
        }
    }

    const int stride = slices + 1;
    for (int stack = 0; stack < stacks; ++stack) {
        for (int slice = 0; slice < slices; ++slice) {
            unsigned int a = static_cast<unsigned int>(stack * stride + slice);
            unsigned int b = static_cast<unsigned int>((stack + 1) * stride + slice);
            unsigned int c = static_cast<unsigned int>((stack + 1) * stride + slice + 1);
            unsigned int d = static_cast<unsigned int>(stack * stride + slice + 1);
            outIndices.insert(outIndices.end(), {a, b, c, a, c, d});
        }
    }
}

void buildConeMesh(int slices, std::vector<float>& outVerts, std::vector<unsigned int>& outIndices) {
    outVerts.clear();
    outIndices.clear();

    const float twoPi = 6.283185307f;

    Vertex apex{};
    apex.px = 0.0f;
    apex.py = 0.0f;
    apex.pz = 0.0f;
    apex.nx = 0.0f;
    apex.ny = 0.0f;
    apex.nz = -1.0f;
    apex.r = 1.0f;
    apex.g = 1.0f;
    apex.b = 1.0f;
    pushVertex(apex, outVerts);

    for (int i = 0; i <= slices; ++i) {
        float u = static_cast<float>(i) / static_cast<float>(slices);
        float theta = u * twoPi;
        float x = std::cos(theta);
        float y = std::sin(theta);

        Vertex vert{};
        vert.px = x;
        vert.py = y;
        vert.pz = 1.0f;
        vert.nx = x;
        vert.ny = y;
        vert.nz = 0.0f;
        vert.r = 1.0f;
        vert.g = 1.0f;
        vert.b = 1.0f;
        pushVertex(vert, outVerts);
    }

    const unsigned int baseCenterIndex = static_cast<unsigned int>(outVerts.size() / 9);
    Vertex center{};
    center.px = 0.0f;
    center.py = 0.0f;
    center.pz = 1.0f;
    center.nx = 0.0f;
    center.ny = 0.0f;
    center.nz = 1.0f;
    center.r = 1.0f;
    center.g = 1.0f;
    center.b = 1.0f;
    pushVertex(center, outVerts);

    for (int i = 0; i < slices; ++i) {
        unsigned int i0 = 1u + static_cast<unsigned int>(i);
        unsigned int i1 = 1u + static_cast<unsigned int>(i + 1);
        outIndices.insert(outIndices.end(), {0u, i1, i0});
    }

    for (int i = 0; i < slices; ++i) {
        unsigned int i0 = 1u + static_cast<unsigned int>(i);
        unsigned int i1 = 1u + static_cast<unsigned int>(i + 1);
        outIndices.insert(outIndices.end(), {baseCenterIndex, i0, i1});
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
    destroyDeferredResources();
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
            handleEvent(event, running);
        }

        updateOrbitCamera();
        renderScene();
        SDL_GL_SwapWindow(window_);
    }
}

void RenderEngine::handleEvent(const SDL_Event& event, bool& running) {
    switch (event.type) {
        case SDL_QUIT:
            running = false;
            break;
        case SDL_KEYDOWN:
            switch (event.key.keysym.sym) {
                case SDLK_ESCAPE:
                    running = false;
                    break;
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
        case SDL_MOUSEWHEEL: {
            if (event.wheel.y != 0) {
                const float factor = event.wheel.y > 0 ? 0.9f : 1.1f;
                zoom_ = std::clamp(zoom_ * factor, kMinZoom, kMaxZoom);
                updateProjection();
            }
            break;
        }
        case SDL_MOUSEBUTTONDOWN:
            if (event.button.button == SDL_BUTTON_MIDDLE) {
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
            if (middleDragging_ && !orbitCameraEnabled_) {
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

void RenderEngine::drawLayer(RenderLayer layer, std::initializer_list<const MeshBuffer*> meshes) const {
    // Ground doesn't write depth so vertical geometry isn't occluded in isometric view.
    const GLboolean depthWrite = layer == RenderLayer::Ground ? GL_FALSE : GL_TRUE;
    glDepthMask(depthWrite);
    for (const MeshBuffer* mesh : meshes) {
        mesh->draw();
    }
}

void RenderEngine::drawCharacter() const {
    character_.draw();
}

void RenderEngine::drawHouse() const {
    house_.draw();
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
    rebuildMovableAssignments(static_cast<float>(SDL_GetTicks()) * 0.001f);
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
    directionalLight_.color = glm::vec3(0., 0., 0.);
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

    constexpr int kPointLights = 1;
    constexpr int kSpotLights = 1;
    constexpr float kTwoPi = 6.283185307f;

    
        const float r = 0.9f;
        const float g = 0.7f;
        const float b = 1.f;

    LightInstance light{};
    light.basePosition = glm::vec3(1.5f, 1.2f, 0.f);
    light.radius = 40.0f;
    light.color = glm::vec3(r, g, b);
    light.intensity = 20.0f;
    light.target = glm::vec3(0.0f);
    light.innerAngle = 0.0f;
    light.outerAngle = 0.0f;
    light.type = LightType::Point;
    light.phase = 0.f;
    light.isMovable = false;
    light.castsShadow = true;
    light.shadowBiasMin = 0.000015f;
    light.shadowBiasSlope = 0.0045f;
    registerLight(light);
    
    LightInstance light2{};
    light2.basePosition = glm::vec3(5.5f, 10.2f, 0.f);
    light2.radius = 32.0f;
    light2.color = glm::vec3(0.55f, 0.70f, 0.95f);
    light2.intensity = 1.4f;
    light2.target = glm::vec3(-3.f, 1.2f, -8.f);
    light2.innerAngle = 15.0f;
    light2.outerAngle = 25.0f;
    light2.type = LightType::Spot;
    light2.phase = 0;
    light2.isMovable = false;
    light2.castsShadow = true;
    light2.shadowBiasMin = 0.0012f;
    light2.shadowBiasSlope = 0.004f;
    registerLight(light2);
    

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

    const float time = static_cast<float>(SDL_GetTicks()) * 0.001f;
    const glm::mat4 invView = deferred ? glm::inverse(view_) : glm::mat4(1.0f);
    rebuildMovableAssignments(time);
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
        } else if (light.type == LightType::Spot) {
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
    std::vector<float> sphereVerts;
    std::vector<unsigned int> sphereIdx;
    buildSphereMesh(16, 24, sphereVerts, sphereIdx);

    std::vector<float> coneVerts;
    std::vector<unsigned int> coneIdx;
    buildConeMesh(24, coneVerts, coneIdx);

    const bool sphereReady = lightSphere_.upload(sphereVerts, sphereIdx);
    const bool coneReady = lightCone_.upload(coneVerts, coneIdx);
    return sphereReady && coneReady;
}

bool RenderEngine::buildDebugMeshes() {
    std::vector<float> axisVerts;
    std::vector<unsigned int> axisIdx;

    addBox(
        glm::vec3(-kAxisCenterHalfExtent),
        glm::vec3(kAxisCenterHalfExtent),
        glm::vec3(0.95f),
        axisVerts,
        axisIdx
    );
    addBox(
        glm::vec3(0.0f, -kAxisThickness, -kAxisThickness),
        glm::vec3(kAxisLength, kAxisThickness, kAxisThickness),
        glm::vec3(0.95f, 0.20f, 0.18f),
        axisVerts,
        axisIdx
    );
    addBox(
        glm::vec3(-kAxisThickness, 0.0f, -kAxisThickness),
        glm::vec3(kAxisThickness, kAxisLength, kAxisThickness),
        glm::vec3(0.20f, 0.92f, 0.24f),
        axisVerts,
        axisIdx
    );
    addBox(
        glm::vec3(-kAxisThickness, -kAxisThickness, 0.0f),
        glm::vec3(kAxisThickness, kAxisThickness, kAxisLength),
        glm::vec3(0.18f, 0.48f, 0.96f),
        axisVerts,
        axisIdx
    );

    return axisGizmo_.upload(axisVerts, axisIdx);
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

void RenderEngine::renderLightDebugOverlay() {
    if (!showLightDebug_ || debugColorShader_.id() == 0 || !axisGizmo_.valid()) {
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
        const ActiveLightDebug& light = lightDebugInstances_[lightIndex];
        glm::mat4 axisModel(1.0f);
        axisModel = glm::translate(axisModel, light.position);
        if (light.type == LightType::Spot) {
            axisModel *= makeOrientationFromDirection(light.direction);
        }
        const float axisScale = std::clamp(light.radius * kLightGizmoScaleFactor, kLightGizmoScaleMin, kLightGizmoScaleMax);
        axisModel = glm::scale(axisModel, glm::vec3(axisScale));
        drawDebugMesh(axisGizmo_, axisModel, glm::vec4(1.0f), false);

        if (light.type == LightType::Point && lightSphere_.valid()) {
            glm::mat4 sphereModel(1.0f);
            sphereModel = glm::translate(sphereModel, light.position);
            sphereModel = glm::scale(sphereModel, glm::vec3(light.radius));
            drawDebugMesh(lightSphere_, sphereModel, glm::vec4(light.color, kDebugVolumeAlpha), true);
        } else if (light.type == LightType::Spot && lightCone_.valid()) {
            glm::mat4 coneModel(1.0f);
            coneModel = glm::translate(coneModel, light.position);
            coneModel *= makeOrientationFromDirection(light.direction);
            const float coneRadius = light.radius * std::tan(glm::radians(light.outerAngle));
            coneModel = glm::scale(coneModel, glm::vec3(coneRadius, coneRadius, light.radius));
            drawDebugMesh(lightCone_, coneModel, glm::vec4(light.color, kDebugVolumeAlpha), true);
        }
    }

    const glm::vec3 dir = glm::normalize(directionalLight_.direction);
    glm::mat4 directionalModel(1.0f);
    directionalModel = glm::translate(directionalModel, -dir * kDirectionalDebugAnchorDistance);
    directionalModel *= makeOrientationFromDirection(dir);
    directionalModel = glm::scale(directionalModel, glm::vec3(1.1f));
    drawDebugMesh(axisGizmo_, directionalModel, glm::vec4(1.0f), false);

    glDisable(GL_BLEND);
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    glDepthMask(GL_TRUE);
}

bool RenderEngine::buildCharacterMesh() {
    StaticMeshData characterMesh;
    const std::string characterPath = modelRootPath() + "Adventurer.glb";
    if (!loadStaticGltfModel(characterPath, characterMesh)) {
        spdlog::error("RenderEngine: failed to load character model '{}'", characterPath);
        return false;
    }

    return character_.upload(characterMesh.vertices, characterMesh.indices);
}

bool RenderEngine::buildHouseMesh() {
    StaticMeshData houseMesh;
    const std::string housePath = modelRootPath() + "FantasyHouse.glb";
    if (!loadStaticGltfModel(housePath, houseMesh)) {
        spdlog::error("RenderEngine: failed to load house model '{}'", housePath);
        return false;
    }

    constexpr float kHouseFootprint = 7.5f;
    constexpr glm::vec3 kHousePosition(-3.0f, 0.0f, -8.0f);
    constexpr float kHouseYawDeg = -35.0f;

    if (!fitMeshToFootprint(houseMesh, kHouseFootprint)) {
        spdlog::error("RenderEngine: failed to fit house model '{}'", housePath);
        return false;
    }

    glm::mat4 houseTransform(1.0f);
    houseTransform = glm::translate(houseTransform, kHousePosition);
    houseTransform *= glm::rotate(glm::mat4(1.0f), glm::radians(kHouseYawDeg), glm::vec3(0.0f, 1.0f, 0.0f));
    transformStaticMesh(houseMesh, houseTransform);

    return house_.upload(houseMesh.vertices, houseMesh.indices);
}

void RenderEngine::ensureDeferredResources() {
    if (rendererPath_ != RendererPath::Deferred41) {
        return;
    }
    if (width_ <= 0 || height_ <= 0) {
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
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glGenTextures(1, &gbufferNormal_);
    glBindTexture(GL_TEXTURE_2D, gbufferNormal_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, width_, height_, 0, GL_RGBA, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glGenTextures(1, &gbufferDepthColor_);
    glBindTexture(GL_TEXTURE_2D, gbufferDepthColor_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R32F, width_, height_, 0, GL_RED, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glGenTextures(1, &gbufferDepth_);
    glBindTexture(GL_TEXTURE_2D, gbufferDepth_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH24_STENCIL8, width_, height_, 0, GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_NONE);
    glTexParameteri(GL_TEXTURE_2D, GL_DEPTH_STENCIL_TEXTURE_MODE, GL_DEPTH_COMPONENT);

    glGenFramebuffers(1, &gbufferFbo_);
    glBindFramebuffer(GL_FRAMEBUFFER, gbufferFbo_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, gbufferAlbedo_, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, gbufferNormal_, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT2, GL_TEXTURE_2D, gbufferDepthColor_, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, gbufferDepth_, 0);
    const GLenum gbufferAttachments[3] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1, GL_COLOR_ATTACHMENT2};
    glDrawBuffers(3, gbufferAttachments);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        spdlog::error("RenderEngine: gbuffer framebuffer is incomplete");
    }

    glGenTextures(1, &lightColor_);
    glBindTexture(GL_TEXTURE_2D, lightColor_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, width_, height_, 0, GL_RGBA, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glGenFramebuffers(1, &lightFbo_);
    glBindFramebuffer(GL_FRAMEBUFFER, lightFbo_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, lightColor_, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, gbufferDepth_, 0);
    glDrawBuffer(GL_COLOR_ATTACHMENT0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        spdlog::error("RenderEngine: light framebuffer is incomplete");
    }

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
    if (gbufferFbo_ != 0) {
        glDeleteFramebuffers(1, &gbufferFbo_);
        gbufferFbo_ = 0;
    }
    if (lightFbo_ != 0) {
        glDeleteFramebuffers(1, &lightFbo_);
        lightFbo_ = 0;
    }
    if (gbufferAlbedo_ != 0) {
        glDeleteTextures(1, &gbufferAlbedo_);
        gbufferAlbedo_ = 0;
    }
    if (gbufferNormal_ != 0) {
        glDeleteTextures(1, &gbufferNormal_);
        gbufferNormal_ = 0;
    }
    if (gbufferDepthColor_ != 0) {
        glDeleteTextures(1, &gbufferDepthColor_);
        gbufferDepthColor_ = 0;
    }
    if (gbufferDepth_ != 0) {
        glDeleteTextures(1, &gbufferDepth_);
        gbufferDepth_ = 0;
    }
    if (lightColor_ != 0) {
        glDeleteTextures(1, &lightColor_);
        lightColor_ = 0;
    }
    if (lightsTboTex_ != 0) {
        glDeleteTextures(1, &lightsTboTex_);
        lightsTboTex_ = 0;
    }
    if (lightsTbo_ != 0) {
        glDeleteBuffers(1, &lightsTbo_);
        lightsTbo_ = 0;
    }
    if (fullscreenVao_ != 0) {
        glDeleteVertexArrays(1, &fullscreenVao_);
        fullscreenVao_ = 0;
    }

    deferredWidth_ = 0;
    deferredHeight_ = 0;
    lightTboSize_ = 0;
}

void RenderEngine::renderDeferredScene() {
    if (rendererPath_ != RendererPath::Deferred41) {
        return;
    }
    if (gbufferFbo_ == 0 || lightFbo_ == 0) {
        return;
    }

    updateLights();

    const glm::mat4 mvp = projection_ * view_;
    const glm::vec3 dirLightWorld = glm::normalize(directionalLight_.direction);
    const glm::vec3 dirLightView = glm::normalize(glm::mat3(view_) * dirLightWorld);
    const glm::mat4 invView = glm::inverse(view_);

    shadowSystem_.updateDirectional(view_, projection_, dirLightWorld, kNearPlane, kFarPlane);
    shadowDebugCascade_ = std::clamp(shadowDebugCascade_, 0, shadowSystem_.directionalCascadeCount() - 1);
    shadowSystem_.renderDirectionalShadows({&ground_, &wallA_, &wallB_, &character_, &house_});
    shadowSystem_.renderSpotShadows({&ground_, &wallA_, &wallB_, &character_, &house_});
    shadowSystem_.renderPointShadows({&ground_, &wallA_, &wallB_, &character_, &house_});

    glBindFramebuffer(GL_FRAMEBUFFER, gbufferFbo_);
    glViewport(0, 0, width_, height_);
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
    const GLfloat clearAlbedo[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    const GLfloat clearNormal[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    const GLfloat clearDepth[4] = {1.0f, 0.0f, 0.0f, 0.0f};
    glClearBufferfv(GL_COLOR, 0, clearAlbedo);
    glClearBufferfv(GL_COLOR, 1, clearNormal);
    glClearBufferfv(GL_COLOR, 2, clearDepth);
    glClearBufferfi(GL_DEPTH_STENCIL, 0, 1.0f, 0);

    deferredGeometryShader_.use();
    glUniformMatrix4fv(gbufferMvpLocation_, 1, GL_FALSE, glm::value_ptr(mvp));
    glUniformMatrix4fv(gbufferViewLocation_, 1, GL_FALSE, glm::value_ptr(view_));
    glUniform1f(gbufferMetallicLocation_, 0.0f);
    glUniform1f(gbufferRoughnessLocation_, 0.6f);

    glDepthMask(GL_FALSE);
    ground_.draw();
    glDepthMask(GL_TRUE);
    wallA_.draw();
    wallB_.draw();
    drawCharacter();
    drawHouse();

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
    glBindTexture(GL_TEXTURE_2D, gbufferDepthColor_);
    glActiveTexture(GL_TEXTURE0 + 3);
    glBindTexture(GL_TEXTURE_2D_ARRAY, shadowSystem_.directionalShadowMap());

    glBindVertexArray(fullscreenVao_);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);

    if (lightCount_ > 0) {
        glEnable(GL_DEPTH_TEST);
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
        glBindTexture(GL_TEXTURE_2D, gbufferDepthColor_);
        glActiveTexture(GL_TEXTURE0 + 3);
        glBindTexture(GL_TEXTURE_BUFFER, lightsTboTex_);
        glActiveTexture(GL_TEXTURE0 + 4);
        glBindTexture(GL_TEXTURE_2D_ARRAY, shadowSystem_.spotShadowMap());
        glActiveTexture(GL_TEXTURE0 + 5);
        glBindTexture(GL_TEXTURE_CUBE_MAP_ARRAY, shadowSystem_.pointShadowMap());

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
    glBindTexture(GL_TEXTURE_2D, gbufferDepthColor_);
    glActiveTexture(GL_TEXTURE0 + 4);
    glBindTexture(GL_TEXTURE_2D_ARRAY, shadowSystem_.directionalShadowMap());

    glBindVertexArray(fullscreenVao_);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);

    renderLightDebugOverlay();

    glDepthMask(GL_TRUE);
}

void RenderEngine::renderSimpleScene() {
    updateLights();

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, width_, height_);
    glDepthMask(GL_TRUE);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    simpleShader_.use();
    const glm::mat4 mvp = projection_ * view_;
    glUniformMatrix4fv(simpleMvpLocation_, 1, GL_FALSE, glm::value_ptr(mvp));
    glUniform3fv(simpleLightDirLocation_, 1, glm::value_ptr(directionalLight_.direction));

    drawLayer(RenderLayer::Ground, {&ground_});
    drawLayer(RenderLayer::Geometry, {&wallA_, &wallB_, &house_});
    drawCharacter();
    renderLightDebugOverlay();
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

void RenderEngine::buildScene() {
    bool shadersReady = false;
    bool volumeReady = true;
    bool debugReady = false;
    const std::string shaderRoot = shaderRootPath();
    const std::string debugVertexShader = shaderRoot + "debug_color.vert";
    const std::string debugFragmentShader = shaderRoot + "debug_color.frag";

    if (!debugColorShader_.buildFromFiles(debugVertexShader, debugFragmentShader)) {
        spdlog::error("RenderEngine: failed to build debug overlay shader");
        sceneReady_ = false;
        return;
    }

    debugMvpLocation_ = debugColorShader_.uniformLocation("uMVP");
    debugColorLocation_ = debugColorShader_.uniformLocation("uColor");
    debugReady = debugColorShader_.id() != 0;

    if (rendererPath_ == RendererPath::SimpleForward) {
        const std::string simpleVertexShader = shaderRoot + "simple.vert";
        const std::string simpleFragmentShader = shaderRoot + "simple.frag";

        if (!simpleShader_.buildFromFiles(simpleVertexShader, simpleFragmentShader)) {
            spdlog::error("RenderEngine: failed to build simple shaders");
            sceneReady_ = false;
            return;
        }

        simpleMvpLocation_ = simpleShader_.uniformLocation("uMVP");
        simpleLightDirLocation_ = simpleShader_.uniformLocation("uLightDir");
        shadersReady = simpleShader_.id() != 0;
    } else if (rendererPath_ == RendererPath::Deferred41) {
        const std::string gbufferVertexShader = shaderRoot + "deferred_gbuffer.vert";
        const std::string gbufferFragmentShader = shaderRoot + "deferred_gbuffer.frag";
        const std::string fullscreenVertexShader = shaderRoot + "fullscreen_tri.vert";
        const std::string dirLightFragmentShader = shaderRoot + "deferred_dir_light.frag";
        const std::string volumeVertexShader = shaderRoot + "deferred_volume.vert";
        const std::string volumeFragmentShader = shaderRoot + "deferred_volume.frag";
        const std::string compositeFragmentShader = shaderRoot + "deferred_composite.frag";

        if (!deferredGeometryShader_.buildFromFiles(gbufferVertexShader, gbufferFragmentShader)) {
            spdlog::error("RenderEngine: failed to build deferred geometry shaders");
            sceneReady_ = false;
            return;
        }

        if (!deferredDirLightShader_.buildFromFiles(fullscreenVertexShader, dirLightFragmentShader)) {
            spdlog::error("RenderEngine: failed to build deferred directional shader");
            sceneReady_ = false;
            return;
        }

        if (!deferredVolumeShader_.buildFromFiles(volumeVertexShader, volumeFragmentShader)) {
            spdlog::error("RenderEngine: failed to build deferred volume shaders");
            sceneReady_ = false;
            return;
        }

        if (!deferredCompositeShader_.buildFromFiles(fullscreenVertexShader, compositeFragmentShader)) {
            spdlog::error("RenderEngine: failed to build deferred composite shader");
            sceneReady_ = false;
            return;
        }

        gbufferMvpLocation_ = deferredGeometryShader_.uniformLocation("uMVP");
        gbufferViewLocation_ = deferredGeometryShader_.uniformLocation("uView");
        gbufferMetallicLocation_ = deferredGeometryShader_.uniformLocation("uMetallic");
        gbufferRoughnessLocation_ = deferredGeometryShader_.uniformLocation("uRoughness");
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
        compositeDebugModeLocation_ = deferredCompositeShader_.uniformLocation("uDebugMode");
        volumeInvViewLocation_ = deferredVolumeShader_.uniformLocation("uInvView");
        volumeSpotShadowMatrixLocation_ = deferredVolumeShader_.uniformLocation("uSpotShadowMatrices[0]");
        volumeSpotShadowCountLocation_ = deferredVolumeShader_.uniformLocation("uSpotShadowCount");
        volumeSpotShadowTexelSizeLocation_ = deferredVolumeShader_.uniformLocation("uSpotShadowTexelSize");
        volumeSpotShadowPcfRadiusLocation_ = deferredVolumeShader_.uniformLocation("uSpotShadowPcfRadius");
        volumePointShadowCountLocation_ = deferredVolumeShader_.uniformLocation("uPointShadowCount");
        volumePointShadowDiskRadiusLocation_ = deferredVolumeShader_.uniformLocation("uPointShadowDiskRadius");
        volumePointShadowPcfRadiusLocation_ = deferredVolumeShader_.uniformLocation("uPointShadowPcfRadius");
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

        deferredDirLightShader_.use();
        glUniform1i(deferredDirLightShader_.uniformLocation("uGAlbedoMetal"), 0);
        glUniform1i(deferredDirLightShader_.uniformLocation("uGNormalRough"), 1);
        glUniform1i(deferredDirLightShader_.uniformLocation("uDepth"), 2);
        glUniform1i(deferredShadowMapLocation_, 3);

        deferredVolumeShader_.use();
        glUniform1i(deferredVolumeShader_.uniformLocation("uGAlbedoMetal"), 0);
        glUniform1i(deferredVolumeShader_.uniformLocation("uGNormalRough"), 1);
        glUniform1i(deferredVolumeShader_.uniformLocation("uDepth"), 2);
        glUniform1i(deferredVolumeShader_.uniformLocation("uLightBuffer"), 3);
        glUniform1i(deferredVolumeShader_.uniformLocation("uSpotShadowMap"), 4);
        glUniform1i(deferredVolumeShader_.uniformLocation("uPointShadowMap"), 5);

        deferredCompositeShader_.use();
        glUniform1i(deferredCompositeShader_.uniformLocation("uLightBuffer"), 0);
        glUniform1i(deferredCompositeShader_.uniformLocation("uGAlbedoMetal"), 1);
        glUniform1i(deferredCompositeShader_.uniformLocation("uGNormalRough"), 2);
        glUniform1i(deferredCompositeShader_.uniformLocation("uDepth"), 3);
        glUniform1i(compositeShadowMapLocation_, 4);

        if (!shadowSystem_.init(shaderRoot)) {
            spdlog::error("RenderEngine: failed to init shadow system");
            sceneReady_ = false;
            return;
        }

        shadersReady = deferredGeometryShader_.id() != 0 &&
                       deferredDirLightShader_.id() != 0 &&
                       deferredVolumeShader_.id() != 0 &&
                       deferredCompositeShader_.id() != 0;
    }

    buildLights();
    volumeReady = buildVolumeMeshes() && buildDebugMeshes();

    std::vector<float> groundVerts;
    std::vector<unsigned int> groundIdx;
    const float g = 500.0f;
    const std::array<Vertex, 4> groundQuad{{
        {-g, 0.0f, -g, 0.0f, 1.0f, 0.0f, 0.18f, 0.36f, 0.20f},
        {-g, 0.0f, g, 0.0f, 1.0f, 0.0f, 0.18f, 0.36f, 0.20f},
        {g, 0.0f, g, 0.0f, 1.0f, 0.0f, 0.18f, 0.36f, 0.20f},
        {g, 0.0f, -g, 0.0f, 1.0f, 0.0f, 0.18f, 0.36f, 0.20f},
    }};
    addQuad(groundQuad, groundVerts, groundIdx);

    std::vector<float> wallVerts;
    std::vector<unsigned int> wallIdx;
    const float wallHeight = 2.5f;
    const float wallOffset = 3.0f;
    const float wallLength = 5.0f;
    const float wallThickness = 0.5f;
    addBox(
        glm::vec3(-wallOffset - wallThickness * 0.5f, 0.0f, -wallLength),
        glm::vec3(-wallOffset + wallThickness * 0.5f, wallHeight, wallLength),
        glm::vec3(0.70f, 0.25f, 0.25f),
        wallVerts,
        wallIdx
    );

    std::vector<float> wallBVerts;
    std::vector<unsigned int> wallBIdx;
    addBox(
        glm::vec3(wallOffset - wallThickness * 0.5f, 0.0f, -wallLength),
        glm::vec3(wallOffset + wallThickness * 0.5f, wallHeight, wallLength),
        glm::vec3(0.25f, 0.45f, 0.70f),
        wallBVerts,
        wallBIdx
    );

    sceneReady_ = shadersReady &&
                  volumeReady &&
                  debugReady &&
                  ground_.upload(groundVerts, groundIdx) &&
                  wallA_.upload(wallVerts, wallIdx) &&
                  wallB_.upload(wallBVerts, wallBIdx) &&
                  buildCharacterMesh() &&
                  buildHouseMesh();
}

}  // namespace render
