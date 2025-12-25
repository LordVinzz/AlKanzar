#include "RenderEngine.hpp"

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

namespace {
constexpr float kIsoAngleX = 35.264f;  // atan(sqrt(1/2)) in degrees
constexpr float kIsoAngleY = 45.0f;
constexpr float kBaseOrthoSize = 10.0f;
constexpr float kMinZoom = 0.2f;
constexpr float kMaxZoom = 5.0f;
constexpr int kTileSize = 16;
constexpr int kMaxLightsPerTile = 128;
constexpr float kDepthEpsilon = 0.999999f;

constexpr GLuint kLightsBinding = 0;
constexpr GLuint kTileMetaBinding = 1;
constexpr GLuint kTileIndexBinding = 2;
constexpr GLuint kTileDepthBinding = 3;

struct Vertex {
    float px, py, pz;
    float nx, ny, nz;
    float r, g, b;
};

void addQuad(const std::array<Vertex, 4>& verts, std::vector<float>& outVerts, std::vector<unsigned int>& outIndices) {
    const unsigned int base = static_cast<unsigned int>(outVerts.size() / 9);
    for (const auto& v : verts) {
        outVerts.insert(outVerts.end(), {v.px, v.py, v.pz, v.nx, v.ny, v.nz, v.r, v.g, v.b});
    }
    outIndices.insert(outIndices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
}

void pushVertex(const Vertex& v, std::vector<float>& outVerts) {
    outVerts.insert(outVerts.end(), {v.px, v.py, v.pz, v.nx, v.ny, v.nz, v.r, v.g, v.b});
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

}  // namespace

namespace render {

RenderEngine::RenderEngine(int width, int height, std::string title)
    : width_(width),
      height_(height),
      tileSize_(kTileSize),
      maxLightsPerTile_(kMaxLightsPerTile),
      title_(std::move(title)) {}

RenderEngine::~RenderEngine() {
    destroyLightingResources();
    destroyDeferredResources();
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
            if (middleDragging_) {
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

    projection_ = glm::ortho(-halfSize * aspect, halfSize * aspect, -halfSize, halfSize, 1.0f, 100.0f);
    invProjection_ = glm::inverse(projection_);
    // We want an isometric view that looks down onto the scene. The standard
    // approach is to rotate the world by -35.264° around the X axis (to tip
    // the view downward) and +45° around the Y axis (to rotate the view
    // diagonally across the X/Z plane).  In the previous version the
    // Y‑rotation used a negative angle which effectively flipped the depth
    // ordering, causing the walls to appear behind the ground even though
    // they are closer to the camera.  Swapping the sign on the Y rotation
    // fixes the depth ordering and places the walls in front of the ground as
    // intended.
    const glm::mat4 rx = glm::rotate(glm::mat4(1.0f), glm::radians(-kIsoAngleX), glm::vec3(1.0f, 0.0f, 0.0f));
    const glm::mat4 ry = glm::rotate(glm::mat4(1.0f), glm::radians(kIsoAngleY), glm::vec3(0.0f, 1.0f, 0.0f));
    const glm::mat4 t = glm::translate(glm::mat4(1.0f), glm::vec3(-panX_, -panY_, -cameraDistance_));

    view_ = t * rx * ry;

    if (rendererPath_ == RendererPath::TiledCompute) {
        ensureLightingResources();
    } else if (rendererPath_ == RendererPath::Deferred41) {
        ensureDeferredResources();
    }
}

void RenderEngine::detectLightingCapabilities() {
    GLint major = 0;
    GLint minor = 0;
    glGetIntegerv(GL_MAJOR_VERSION, &major);
    glGetIntegerv(GL_MINOR_VERSION, &minor);

    dispatchCompute_ = reinterpret_cast<DispatchComputeFn>(SDL_GL_GetProcAddress("glDispatchCompute"));
    memoryBarrier_ = reinterpret_cast<MemoryBarrierFn>(SDL_GL_GetProcAddress("glMemoryBarrier"));

    const bool supportsCompute = dispatchCompute_ != nullptr &&
                                 memoryBarrier_ != nullptr &&
                                 (major > 4 || (major == 4 && minor >= 3));

    if (supportsCompute) {
        rendererPath_ = RendererPath::TiledCompute;
        spdlog::info("RenderEngine: compute/SSBO path enabled (GL {}.{})", major, minor);
        return;
    }

    if (major > 4 || (major == 4 && minor >= 1)) {
        rendererPath_ = RendererPath::Deferred41;
        spdlog::warn("RenderEngine: compute/SSBO path unavailable (GL {}.{})", major, minor);
        spdlog::info("RenderEngine: using deferred path (GL 4.1 compatible)");
    } else {
        rendererPath_ = RendererPath::SimpleForward;
        spdlog::warn("RenderEngine: deferred path unavailable (GL {}.{})", major, minor);
    }
}

void RenderEngine::setLightingUniforms() const {
    const glm::mat4 mvp = projection_ * view_;
    glUniformMatrix4fv(lightingMvpLocation_, 1, GL_FALSE, glm::value_ptr(mvp));
    glUniformMatrix4fv(lightingViewLocation_, 1, GL_FALSE, glm::value_ptr(view_));
}

void RenderEngine::drawLayer(RenderLayer layer, std::initializer_list<const MeshBuffer*> meshes) const {
    // Ground doesn't write depth so vertical geometry isn't occluded in isometric view.
    const GLboolean depthWrite = layer == RenderLayer::Ground ? GL_FALSE : GL_TRUE;
    glDepthMask(depthWrite);
    for (const MeshBuffer* mesh : meshes) {
        mesh->draw();
    }
}

void RenderEngine::buildLights() {
    lights_.clear();

    constexpr int kPointLights = 32;
    constexpr int kSpotLights = 8;
    constexpr float kTwoPi = 6.283185307f;

    for (int i = 0; i < kPointLights; ++i) {
        const float angle = kTwoPi * static_cast<float>(i) / static_cast<float>(kPointLights);
        const float r = 0.4f + 0.6f * static_cast<float>(std::sin(angle));
        const float g = 0.4f + 0.6f * static_cast<float>(std::sin(angle + 2.1f));
        const float b = 0.4f + 0.6f * static_cast<float>(std::sin(angle + 4.2f));

        LightInstance light{};
        light.basePosition = glm::vec3(std::cos(angle) * 4.5f, 1.2f, std::sin(angle) * 4.5f);
        light.radius = 6.0f;
        light.color = glm::vec3(r, g, b);
        light.intensity = 1.0f;
        light.target = glm::vec3(0.0f);
        light.innerAngle = 0.0f;
        light.outerAngle = 0.0f;
        light.type = LightType::Point;
        light.phase = angle;
        lights_.push_back(light);
    }

    for (int i = 0; i < kSpotLights; ++i) {
        const float angle = kTwoPi * static_cast<float>(i) / static_cast<float>(kSpotLights);
        LightInstance light{};
        light.basePosition = glm::vec3(std::cos(angle) * 2.5f, 4.0f, std::sin(angle) * 2.5f);
        light.radius = 8.0f;
        light.color = glm::vec3(0.55f, 0.70f, 0.95f);
        light.intensity = 1.4f;
        light.target = glm::vec3(0.0f, 0.0f, 0.0f);
        light.innerAngle = 15.0f;
        light.outerAngle = 25.0f;
        light.type = LightType::Spot;
        light.phase = angle;
        lights_.push_back(light);
    }

    gpuLights_.resize(lights_.size());
}

void RenderEngine::updateLights() {
    if (rendererPath_ == RendererPath::SimpleForward) {
        return;
    }
    if (lights_.empty()) {
        lightCount_ = 0;
        pointLightCount_ = 0;
        spotLightCount_ = 0;
        return;
    }

    const float time = static_cast<float>(SDL_GetTicks()) * 0.001f;
    gpuLights_.clear();
    gpuLights_.reserve(lights_.size());
    pointLightCount_ = 0;
    spotLightCount_ = 0;
    cameraInsideLightVolume_ = false;

    auto appendLight = [&](const LightInstance& light) {
        glm::vec3 position = light.basePosition;
        const float phase = light.phase + time;
        const float orbitScale = light.type == LightType::Spot ? 2.25f : 0.55f;
        const float bobScale = light.type == LightType::Spot ? 2.15f : 0.35f;

        position.x += orbitScale * std::cos(phase * 0.7f);
        position.z += orbitScale * std::sin(phase * 0.9f);
        position.y += bobScale * std::sin(phase * 1.3f);

        glm::vec3 direction(0.0f);
        if (light.type == LightType::Spot) {
            direction = glm::normalize(light.target - position);
        }

        const glm::vec3 viewPos = glm::vec3(view_ * glm::vec4(position, 1.0f));
        if (glm::length(viewPos) < light.radius) {
            cameraInsideLightVolume_ = true;
        }
        glm::vec3 viewDir(0.0f);
        if (light.type == LightType::Spot) {
            viewDir = glm::normalize(glm::mat3(view_) * direction);
        }

        GpuLight gpu{};
        gpu.positionRadius = glm::vec4(viewPos, light.radius);
        gpu.colorIntensity = glm::vec4(light.color, light.intensity);
        gpu.directionType = glm::vec4(viewDir, static_cast<float>(light.type));

        if (light.type == LightType::Spot) {
            const float inner = std::cos(glm::radians(light.innerAngle));
            const float outer = std::cos(glm::radians(light.outerAngle));
            const float tanOuter = std::tan(glm::radians(light.outerAngle));
            gpu.spotParams = glm::vec4(inner, outer, light.radius, tanOuter);
        } else {
            gpu.spotParams = glm::vec4(0.0f);
        }

        gpuLights_.push_back(gpu);
    };

    for (const auto& light : lights_) {
        if (light.type == LightType::Point) {
            appendLight(light);
            pointLightCount_++;
        }
    }

    for (const auto& light : lights_) {
        if (light.type == LightType::Spot) {
            appendLight(light);
            spotLightCount_++;
        }
    }

    lightCount_ = static_cast<int>(gpuLights_.size());

    if (rendererPath_ == RendererPath::TiledCompute) {
        if (lightsSsbo_ == 0) {
            glGenBuffers(1, &lightsSsbo_);
        }

        const GLsizeiptr bufferSize = static_cast<GLsizeiptr>(gpuLights_.size() * sizeof(GpuLight));
        if (bufferSize == 0) {
            lightBufferSize_ = 0;
            return;
        }

        glBindBuffer(GL_SHADER_STORAGE_BUFFER, lightsSsbo_);
        if (bufferSize != lightBufferSize_) {
            glBufferData(GL_SHADER_STORAGE_BUFFER, bufferSize, gpuLights_.data(), GL_DYNAMIC_DRAW);
            lightBufferSize_ = bufferSize;
        } else {
            glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, bufferSize, gpuLights_.data());
        }
    } else if (rendererPath_ == RendererPath::Deferred41) {
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

void RenderEngine::ensureLightingResources() {
    if (rendererPath_ != RendererPath::TiledCompute) {
        return;
    }
    if (width_ <= 0 || height_ <= 0) {
        return;
    }
    if (width_ == resourceWidth_ && height_ == resourceHeight_) {
        return;
    }

    resourceWidth_ = width_;
    resourceHeight_ = height_;

    if (depthFbo_ != 0) {
        glDeleteFramebuffers(1, &depthFbo_);
        depthFbo_ = 0;
    }
    if (depthTexture_ != 0) {
        glDeleteTextures(1, &depthTexture_);
        depthTexture_ = 0;
    }
    if (tileMetaSsbo_ != 0) {
        glDeleteBuffers(1, &tileMetaSsbo_);
        tileMetaSsbo_ = 0;
    }
    if (tileIndexSsbo_ != 0) {
        glDeleteBuffers(1, &tileIndexSsbo_);
        tileIndexSsbo_ = 0;
    }
    if (tileDepthSsbo_ != 0) {
        glDeleteBuffers(1, &tileDepthSsbo_);
        tileDepthSsbo_ = 0;
    }

    tilesX_ = (width_ + tileSize_ - 1) / tileSize_;
    tilesY_ = (height_ + tileSize_ - 1) / tileSize_;
    const int tileCount = tilesX_ * tilesY_;

    glGenTextures(1, &depthTexture_);
    glBindTexture(GL_TEXTURE_2D, depthTexture_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, width_, height_, 0, GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_NONE);

    glGenFramebuffers(1, &depthFbo_);
    glBindFramebuffer(GL_FRAMEBUFFER, depthFbo_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, depthTexture_, 0);
    glDrawBuffer(GL_NONE);
    glReadBuffer(GL_NONE);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        spdlog::error("RenderEngine: depth framebuffer is incomplete");
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    std::vector<TileMeta> tileMeta(static_cast<size_t>(tileCount));
    for (int i = 0; i < tileCount; ++i) {
        tileMeta[static_cast<size_t>(i)] = {
            static_cast<uint32_t>(i * maxLightsPerTile_),
            0u,
            0u,
            0u
        };
    }

    glGenBuffers(1, &tileMetaSsbo_);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, tileMetaSsbo_);
    glBufferData(GL_SHADER_STORAGE_BUFFER,
                 static_cast<GLsizeiptr>(tileMeta.size() * sizeof(TileMeta)),
                 tileMeta.data(),
                 GL_DYNAMIC_DRAW);

    glGenBuffers(1, &tileIndexSsbo_);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, tileIndexSsbo_);
    glBufferData(GL_SHADER_STORAGE_BUFFER,
                 static_cast<GLsizeiptr>(tileCount) * maxLightsPerTile_ * sizeof(uint32_t),
                 nullptr,
                 GL_DYNAMIC_DRAW);

    glGenBuffers(1, &tileDepthSsbo_);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, tileDepthSsbo_);
    glBufferData(GL_SHADER_STORAGE_BUFFER,
                 static_cast<GLsizeiptr>(tileCount) * sizeof(glm::vec2),
                 nullptr,
                 GL_DYNAMIC_DRAW);
}

void RenderEngine::destroyLightingResources() {
    if (!glContext_) {
        return;
    }
    if (depthFbo_ != 0) {
        glDeleteFramebuffers(1, &depthFbo_);
        depthFbo_ = 0;
    }
    if (depthTexture_ != 0) {
        glDeleteTextures(1, &depthTexture_);
        depthTexture_ = 0;
    }
    if (lightsSsbo_ != 0) {
        glDeleteBuffers(1, &lightsSsbo_);
        lightsSsbo_ = 0;
    }
    if (tileMetaSsbo_ != 0) {
        glDeleteBuffers(1, &tileMetaSsbo_);
        tileMetaSsbo_ = 0;
    }
    if (tileIndexSsbo_ != 0) {
        glDeleteBuffers(1, &tileIndexSsbo_);
        tileIndexSsbo_ = 0;
    }
    if (tileDepthSsbo_ != 0) {
        glDeleteBuffers(1, &tileDepthSsbo_);
        tileDepthSsbo_ = 0;
    }

    resourceWidth_ = 0;
    resourceHeight_ = 0;
    tilesX_ = 0;
    tilesY_ = 0;
    lightBufferSize_ = 0;
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

void RenderEngine::renderDepthPrepass() {
    if (rendererPath_ != RendererPath::TiledCompute) {
        return;
    }
    if (depthFbo_ == 0 || depthTexture_ == 0) {
        return;
    }

    glBindFramebuffer(GL_FRAMEBUFFER, depthFbo_);
    glViewport(0, 0, width_, height_);
    glDepthMask(GL_TRUE);
    glClear(GL_DEPTH_BUFFER_BIT);
    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);

    depthShader_.use();
    const glm::mat4 mvp = projection_ * view_;
    glUniformMatrix4fv(depthMvpLocation_, 1, GL_FALSE, glm::value_ptr(mvp));

    ground_.draw();
    wallA_.draw();
    wallB_.draw();

    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (memoryBarrier_) {
        memoryBarrier_(GL_TEXTURE_FETCH_BARRIER_BIT);
    }
}

void RenderEngine::dispatchDepthMinMax() {
    if (rendererPath_ != RendererPath::TiledCompute || !dispatchCompute_) {
        return;
    }
    if (tilesX_ <= 0 || tilesY_ <= 0 || tileDepthSsbo_ == 0 || depthTexture_ == 0) {
        return;
    }

    depthMinMaxCompute_.use();
    glUniform2i(depthScreenSizeLocation_, width_, height_);
    glUniform2i(depthTileCountLocation_, tilesX_, tilesY_);

    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, kTileDepthBinding, tileDepthSsbo_);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, depthTexture_);

    dispatchCompute_(static_cast<GLuint>(tilesX_), static_cast<GLuint>(tilesY_), 1);
    if (memoryBarrier_) {
        memoryBarrier_(GL_SHADER_STORAGE_BARRIER_BIT);
    }
}

void RenderEngine::dispatchLightCulling() {
    if (rendererPath_ != RendererPath::TiledCompute || !dispatchCompute_) {
        return;
    }
    if (tilesX_ <= 0 || tilesY_ <= 0 || tileMetaSsbo_ == 0 || tileIndexSsbo_ == 0 || tileDepthSsbo_ == 0) {
        return;
    }

    lightCullCompute_.use();
    glUniform2i(cullScreenSizeLocation_, width_, height_);
    glUniform2i(cullTileCountLocation_, tilesX_, tilesY_);
    glUniform1i(cullTileSizeLocation_, tileSize_);
    glUniform1i(cullLightCountLocation_, lightCount_);
    glUniform1i(cullMaxLightsLocation_, maxLightsPerTile_);
    glUniformMatrix4fv(cullInvProjLocation_, 1, GL_FALSE, glm::value_ptr(invProjection_));

    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, kLightsBinding, lightsSsbo_);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, kTileMetaBinding, tileMetaSsbo_);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, kTileIndexBinding, tileIndexSsbo_);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, kTileDepthBinding, tileDepthSsbo_);

    dispatchCompute_(static_cast<GLuint>(tilesX_), static_cast<GLuint>(tilesY_), 1);
    if (memoryBarrier_) {
        memoryBarrier_(GL_SHADER_STORAGE_BARRIER_BIT);
    }
}

void RenderEngine::renderLitScene() {
    if (rendererPath_ != RendererPath::TiledCompute) {
        return;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, width_, height_);
    glDepthMask(GL_TRUE);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    lightingShader_.use();
    setLightingUniforms();
    glUniform2i(lightingTileCountLocation_, tilesX_, tilesY_);
    glUniform1i(lightingTileSizeLocation_, tileSize_);
    const glm::vec3 dirLightWorld(-0.3f, -1.0f, -0.4f);
    const glm::vec3 dirLightView = glm::normalize(glm::mat3(view_) * dirLightWorld);
    glUniform3fv(dirLightDirLocation_, 1, glm::value_ptr(dirLightView));
    glUniform3f(dirLightColorLocation_, 1.0f, 1.0f, 1.0f);
    glUniform1f(dirLightIntensityLocation_, 0.7f);

    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, kLightsBinding, lightsSsbo_);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, kTileMetaBinding, tileMetaSsbo_);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, kTileIndexBinding, tileIndexSsbo_);

    drawLayer(RenderLayer::Ground, {&ground_});
    drawLayer(RenderLayer::Geometry, {&wallA_, &wallB_});
    drawLayer(RenderLayer::Actors, {});
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
    const glm::vec3 dirLightWorld(-0.3f, -1.0f, -0.4f);
    const glm::vec3 dirLightView = glm::normalize(glm::mat3(view_) * dirLightWorld);

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
    glUniform3f(deferredDirLightColorLocation_, 1.0f, 1.0f, 1.0f);
    glUniform1f(deferredDirLightIntensityLocation_, 0.7f);
    glUniform3f(deferredAmbientLocation_, 0.06f, 0.06f, 0.07f);

    glActiveTexture(GL_TEXTURE0 + 0);
    glBindTexture(GL_TEXTURE_2D, gbufferAlbedo_);
    glActiveTexture(GL_TEXTURE0 + 1);
    glBindTexture(GL_TEXTURE_2D, gbufferNormal_);
    glActiveTexture(GL_TEXTURE0 + 2);
    glBindTexture(GL_TEXTURE_2D, gbufferDepthColor_);

    glBindVertexArray(fullscreenVao_);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);

    if (lightCount_ > 0) {
        glEnable(GL_DEPTH_TEST);
        glDepthMask(GL_FALSE);
        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE);
        glEnable(GL_CULL_FACE);
        glCullFace(cameraInsideLightVolume_ ? GL_FRONT : GL_BACK);

        deferredVolumeShader_.use();
        glUniformMatrix4fv(volumeProjLocation_, 1, GL_FALSE, glm::value_ptr(projection_));
        glUniformMatrix4fv(volumeInvProjLocation_, 1, GL_FALSE, glm::value_ptr(invProjection_));
        glUniform2f(volumeScreenSizeLocation_, static_cast<float>(width_), static_cast<float>(height_));

        glActiveTexture(GL_TEXTURE0 + 0);
        glBindTexture(GL_TEXTURE_2D, gbufferAlbedo_);
        glActiveTexture(GL_TEXTURE0 + 1);
        glBindTexture(GL_TEXTURE_2D, gbufferNormal_);
        glActiveTexture(GL_TEXTURE0 + 2);
        glBindTexture(GL_TEXTURE_2D, gbufferDepthColor_);
        glActiveTexture(GL_TEXTURE0 + 3);
        glBindTexture(GL_TEXTURE_BUFFER, lightsTboTex_);

        if (pointLightCount_ > 0) {
            glUniform1i(volumeIsSpotLocation_, 0);
            glUniform1i(volumeLightOffsetLocation_, 0);
            lightSphere_.drawInstanced(pointLightCount_);
        }
        if (spotLightCount_ > 0) {
            glUniform1i(volumeIsSpotLocation_, 1);
            glUniform1i(volumeLightOffsetLocation_, pointLightCount_);
            lightCone_.drawInstanced(spotLightCount_);
        }

        glDisable(GL_BLEND);
        glDepthMask(GL_TRUE);
        glCullFace(GL_BACK);
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, width_, height_);
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glDisable(GL_BLEND);

    deferredCompositeShader_.use();
    glUniform1i(compositeDebugModeLocation_, static_cast<int>(debugView_));

    glActiveTexture(GL_TEXTURE0 + 0);
    glBindTexture(GL_TEXTURE_2D, lightColor_);
    glActiveTexture(GL_TEXTURE0 + 1);
    glBindTexture(GL_TEXTURE_2D, gbufferAlbedo_);
    glActiveTexture(GL_TEXTURE0 + 2);
    glBindTexture(GL_TEXTURE_2D, gbufferNormal_);
    glActiveTexture(GL_TEXTURE0 + 3);
    glBindTexture(GL_TEXTURE_2D, gbufferDepthColor_);

    glBindVertexArray(fullscreenVao_);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);

    glDepthMask(GL_TRUE);
}

void RenderEngine::renderSimpleScene() {
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, width_, height_);
    glDepthMask(GL_TRUE);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    simpleShader_.use();
    const glm::mat4 mvp = projection_ * view_;
    glUniformMatrix4fv(simpleMvpLocation_, 1, GL_FALSE, glm::value_ptr(mvp));
    glUniform3f(simpleLightDirLocation_, -0.3f, -1.0f, -0.4f);

    drawLayer(RenderLayer::Ground, {&ground_});
    drawLayer(RenderLayer::Geometry, {&wallA_, &wallB_});
    drawLayer(RenderLayer::Actors, {});
}

void RenderEngine::renderScene() {
    if (!sceneReady_) {
        return;
    }
    if (rendererPath_ == RendererPath::SimpleForward) {
        renderSimpleScene();
        return;
    }
    if (rendererPath_ == RendererPath::TiledCompute) {
        updateLights();
        renderDepthPrepass();
        dispatchDepthMinMax();
        dispatchLightCulling();
        renderLitScene();
    } else if (rendererPath_ == RendererPath::Deferred41) {
        renderDeferredScene();
    }
}

void RenderEngine::buildScene() {
    bool shadersReady = false;
    bool volumeReady = true;

    if (rendererPath_ == RendererPath::SimpleForward) {
        static const std::string simpleVertexShader = R"(
            #version 330 core
            layout (location = 0) in vec3 aPos;
            layout (location = 1) in vec3 aNormal;
            layout (location = 2) in vec3 aColor;

            uniform mat4 uMVP;
            uniform vec3 uLightDir;

            out vec3 vColor;

            void main() {
                gl_Position = uMVP * vec4(aPos, 1.0);
                float ndotl = max(dot(normalize(aNormal), -normalize(uLightDir)), 0.2);
                vColor = aColor * ndotl;
            }
        )";

        static const std::string simpleFragmentShader = R"(
            #version 330 core
            in vec3 vColor;
            out vec4 FragColor;

            void main() {
                FragColor = vec4(vColor, 1.0);
            }
        )";

        if (!simpleShader_.buildFromSource(simpleVertexShader, simpleFragmentShader)) {
            spdlog::error("RenderEngine: failed to build simple shaders");
            sceneReady_ = false;
            return;
        }

        simpleMvpLocation_ = simpleShader_.uniformLocation("uMVP");
        simpleLightDirLocation_ = simpleShader_.uniformLocation("uLightDir");
        shadersReady = simpleShader_.id() != 0;
    } else if (rendererPath_ == RendererPath::TiledCompute) {
        static const std::string lightingVertexShader = R"(
        #version 430 core
        layout (location = 0) in vec3 aPos;
        layout (location = 1) in vec3 aNormal;
        layout (location = 2) in vec3 aColor;

        uniform mat4 uMVP;
        uniform mat4 uView;

        out vec3 vColor;
        out vec3 vViewPos;
        out vec3 vViewNormal;

        void main() {
            vec4 viewPos = uView * vec4(aPos, 1.0);
            vViewPos = viewPos.xyz;
            vViewNormal = mat3(uView) * aNormal;
            vColor = aColor;
            gl_Position = uMVP * vec4(aPos, 1.0);
        }
    )";

    static const std::string lightingFragmentShader = R"(
        #version 430 core
        struct Light {
            vec4 positionRadius;
            vec4 colorIntensity;
            vec4 directionType;
            vec4 spotParams;
        };

        struct TileMeta {
            uint offset;
            uint count;
            uint pad0;
            uint pad1;
        };

        layout(std430, binding = 0) readonly buffer Lights {
            Light lights[];
        };

        layout(std430, binding = 1) readonly buffer TileMetaBuffer {
            TileMeta tiles[];
        };

        layout(std430, binding = 2) readonly buffer TileIndices {
            uint lightIndices[];
        };

        uniform ivec2 uTileCount;
        uniform int uTileSize;
        uniform vec3 uDirLightDir;
        uniform vec3 uDirLightColor;
        uniform float uDirLightIntensity;

        in vec3 vColor;
        in vec3 vViewPos;
        in vec3 vViewNormal;
        out vec4 FragColor;

        void main() {
            ivec2 tileCoord = ivec2(gl_FragCoord.xy) / uTileSize;
            tileCoord = clamp(tileCoord, ivec2(0), uTileCount - ivec2(1));
            int tileIndex = tileCoord.y * uTileCount.x + tileCoord.x;
            TileMeta meta = tiles[tileIndex];

            vec3 normal = normalize(vViewNormal);
            vec3 color = vColor * 0.05;

            vec3 dir = normalize(-uDirLightDir);
            float ndotl = max(dot(normal, dir), 0.0);
            color += vColor * uDirLightColor * uDirLightIntensity * ndotl;

            for (uint i = 0u; i < meta.count; ++i) {
                uint lightIndex = lightIndices[meta.offset + i];
                Light light = lights[lightIndex];
                vec3 lightPos = light.positionRadius.xyz;
                float radius = light.positionRadius.w;
                vec3 toLight = lightPos - vViewPos;
                float dist2 = dot(toLight, toLight);
                if (dist2 > radius * radius) {
                    continue;
                }
                float dist = sqrt(dist2);
                vec3 L = toLight / max(dist, 0.0001);
                float attenuation = clamp(1.0 - dist / radius, 0.0, 1.0);
                attenuation *= attenuation;
                float diffuse = max(dot(normal, L), 0.0);
                float intensity = light.colorIntensity.w;
                vec3 lightColor = light.colorIntensity.rgb;

                if (light.directionType.w > 0.5) {
                    vec3 spotDir = normalize(light.directionType.xyz);
                    float cosTheta = dot(normalize(-L), spotDir);
                    float inner = light.spotParams.x;
                    float outer = light.spotParams.y;
                    float denom = max(inner - outer, 0.0001);
                    float spot = clamp((cosTheta - outer) / denom, 0.0, 1.0);
                    attenuation *= spot;
                }

                color += vColor * lightColor * intensity * diffuse * attenuation;
            }

            FragColor = vec4(color, 1.0);
        }
    )";

    const std::string depthVertexShader = R"(
        #version 430 core
        layout (location = 0) in vec3 aPos;

        uniform mat4 uMVP;

        void main() {
            gl_Position = uMVP * vec4(aPos, 1.0);
        }
    )";

    const std::string depthFragmentShader = R"(
        #version 430 core
        void main() { }
    )";

    const std::string depthMinMaxShader = std::string("#version 430 core\n#define TILE_SIZE ") +
        std::to_string(tileSize_) + "\n#define DEPTH_EPSILON " + std::to_string(kDepthEpsilon) + R"(
        #define TILE_PIXELS (TILE_SIZE * TILE_SIZE)
        layout(local_size_x = TILE_SIZE, local_size_y = TILE_SIZE, local_size_z = 1) in;

        layout(binding = 0) uniform sampler2D uDepthTex;
        layout(std430, binding = 3) buffer TileDepth {
            vec2 tileDepth[];
        };

        uniform ivec2 uScreenSize;
        uniform ivec2 uTileCount;

        shared float sMin[TILE_PIXELS];
        shared float sMax[TILE_PIXELS];

        void main() {
            ivec2 tile = ivec2(gl_WorkGroupID.xy);
            if (tile.x >= uTileCount.x || tile.y >= uTileCount.y) {
                return;
            }

            ivec2 tileOrigin = tile * TILE_SIZE;
            ivec2 pixel = tileOrigin + ivec2(gl_LocalInvocationID.xy);
            float depth = 1.0;
            bool valid = false;
            if (pixel.x < uScreenSize.x && pixel.y < uScreenSize.y) {
                depth = texelFetch(uDepthTex, pixel, 0).r;
                valid = depth < DEPTH_EPSILON;
            }

            uint idx = gl_LocalInvocationIndex;
            sMin[idx] = valid ? depth : 1.0;
            sMax[idx] = valid ? depth : 0.0;
            barrier();

            for (uint stride = TILE_PIXELS / 2u; stride > 0u; stride >>= 1u) {
                if (idx < stride) {
                    sMin[idx] = min(sMin[idx], sMin[idx + stride]);
                    sMax[idx] = max(sMax[idx], sMax[idx + stride]);
                }
                barrier();
            }

            if (idx == 0u) {
                uint tileIndex = uint(tile.y * uTileCount.x + tile.x);
                tileDepth[tileIndex] = vec2(sMin[0], sMax[0]);
            }
        }
    )";

    const std::string lightCullShader = R"(
        #version 430 core
        layout(local_size_x = 1, local_size_y = 1, local_size_z = 1) in;

        struct Light {
            vec4 positionRadius;
            vec4 colorIntensity;
            vec4 directionType;
            vec4 spotParams;
        };

        struct TileMeta {
            uint offset;
            uint count;
            uint pad0;
            uint pad1;
        };

        layout(std430, binding = 0) readonly buffer Lights {
            Light lights[];
        };

        layout(std430, binding = 1) buffer TileMetaBuffer {
            TileMeta tiles[];
        };

        layout(std430, binding = 2) buffer TileIndices {
            uint lightIndices[];
        };

        layout(std430, binding = 3) readonly buffer TileDepth {
            vec2 tileDepth[];
        };

        uniform ivec2 uScreenSize;
        uniform ivec2 uTileCount;
        uniform int uTileSize;
        uniform int uLightCount;
        uniform int uMaxLightsPerTile;
        uniform mat4 uInvProj;

        void expandAabb(inout vec3 minV, inout vec3 maxV, vec4 p) {
            vec3 v = p.xyz / p.w;
            minV = min(minV, v);
            maxV = max(maxV, v);
        }

        void main() {
            ivec2 tile = ivec2(gl_GlobalInvocationID.xy);
            if (tile.x >= uTileCount.x || tile.y >= uTileCount.y) {
                return;
            }

            uint tileIndex = uint(tile.y * uTileCount.x + tile.x);
            vec2 depthRange = tileDepth[tileIndex];
            if (depthRange.x > depthRange.y) {
                tiles[tileIndex].count = 0u;
                return;
            }

            vec2 pixelMin = vec2(tile * uTileSize);
            vec2 pixelMax = min(pixelMin + vec2(uTileSize), vec2(uScreenSize));
            vec2 ndcMin = (pixelMin / vec2(uScreenSize)) * 2.0 - 1.0;
            vec2 ndcMax = (pixelMax / vec2(uScreenSize)) * 2.0 - 1.0;

            float zMin = depthRange.x * 2.0 - 1.0;
            float zMax = depthRange.y * 2.0 - 1.0;

            vec3 minV = vec3(1e20);
            vec3 maxV = vec3(-1e20);

            expandAabb(minV, maxV, uInvProj * vec4(ndcMin.x, ndcMin.y, zMin, 1.0));
            expandAabb(minV, maxV, uInvProj * vec4(ndcMax.x, ndcMin.y, zMin, 1.0));
            expandAabb(minV, maxV, uInvProj * vec4(ndcMin.x, ndcMax.y, zMin, 1.0));
            expandAabb(minV, maxV, uInvProj * vec4(ndcMax.x, ndcMax.y, zMin, 1.0));
            expandAabb(minV, maxV, uInvProj * vec4(ndcMin.x, ndcMin.y, zMax, 1.0));
            expandAabb(minV, maxV, uInvProj * vec4(ndcMax.x, ndcMin.y, zMax, 1.0));
            expandAabb(minV, maxV, uInvProj * vec4(ndcMin.x, ndcMax.y, zMax, 1.0));
            expandAabb(minV, maxV, uInvProj * vec4(ndcMax.x, ndcMax.y, zMax, 1.0));

            uint offset = tiles[tileIndex].offset;
            uint count = 0u;

            for (int i = 0; i < uLightCount; ++i) {
                Light light = lights[i];
                vec3 center = light.positionRadius.xyz;
                float radius = light.positionRadius.w;
                vec3 closest = clamp(center, minV, maxV);
                vec3 delta = center - closest;
                if (dot(delta, delta) <= radius * radius) {
                    if (count < uint(uMaxLightsPerTile)) {
                        lightIndices[offset + count] = uint(i);
                        count++;
                    }
                }
            }

            tiles[tileIndex].count = count;
        }
    )";

    if (!lightingShader_.buildFromSource(lightingVertexShader, lightingFragmentShader)) {
        spdlog::error("RenderEngine: failed to build lighting shaders");
        sceneReady_ = false;
        return;
    }

    if (!depthShader_.buildFromSource(depthVertexShader, depthFragmentShader)) {
        spdlog::error("RenderEngine: failed to build depth shaders");
        sceneReady_ = false;
        return;
    }

    if (!depthMinMaxCompute_.buildComputeFromSource(depthMinMaxShader)) {
        spdlog::error("RenderEngine: failed to build depth compute shader");
        sceneReady_ = false;
        return;
    }

    if (!lightCullCompute_.buildComputeFromSource(lightCullShader)) {
        spdlog::error("RenderEngine: failed to build light culling shader");
        sceneReady_ = false;
        return;
    }

    lightingMvpLocation_ = lightingShader_.uniformLocation("uMVP");
    lightingViewLocation_ = lightingShader_.uniformLocation("uView");
    lightingTileCountLocation_ = lightingShader_.uniformLocation("uTileCount");
    lightingTileSizeLocation_ = lightingShader_.uniformLocation("uTileSize");
    dirLightDirLocation_ = lightingShader_.uniformLocation("uDirLightDir");
    dirLightColorLocation_ = lightingShader_.uniformLocation("uDirLightColor");
    dirLightIntensityLocation_ = lightingShader_.uniformLocation("uDirLightIntensity");
    depthMvpLocation_ = depthShader_.uniformLocation("uMVP");
    depthScreenSizeLocation_ = depthMinMaxCompute_.uniformLocation("uScreenSize");
    depthTileCountLocation_ = depthMinMaxCompute_.uniformLocation("uTileCount");
    cullScreenSizeLocation_ = lightCullCompute_.uniformLocation("uScreenSize");
    cullTileCountLocation_ = lightCullCompute_.uniformLocation("uTileCount");
    cullTileSizeLocation_ = lightCullCompute_.uniformLocation("uTileSize");
    cullLightCountLocation_ = lightCullCompute_.uniformLocation("uLightCount");
    cullMaxLightsLocation_ = lightCullCompute_.uniformLocation("uMaxLightsPerTile");
    cullInvProjLocation_ = lightCullCompute_.uniformLocation("uInvProj");

    buildLights();

    shadersReady = lightingShader_.id() != 0 &&
                   depthShader_.id() != 0 &&
                   depthMinMaxCompute_.id() != 0 &&
                   lightCullCompute_.id() != 0;
    } else if (rendererPath_ == RendererPath::Deferred41) {
        static const std::string gbufferVertexShader = R"(
            #version 410 core
            layout (location = 0) in vec3 aPos;
            layout (location = 1) in vec3 aNormal;
            layout (location = 2) in vec3 aColor;

            uniform mat4 uMVP;
            uniform mat4 uView;

            out vec3 vNormal;
            out vec3 vAlbedo;

            void main() {
                vNormal = mat3(uView) * aNormal;
                vAlbedo = aColor;
                gl_Position = uMVP * vec4(aPos, 1.0);
            }
        )";

        static const std::string gbufferFragmentShader = R"(
            #version 410 core
            in vec3 vNormal;
            in vec3 vAlbedo;

            layout(location = 0) out vec4 gAlbedoMetal;
            layout(location = 1) out vec4 gNormalRough;
            layout(location = 2) out float gDepth;

            uniform float uMetallic;
            uniform float uRoughness;

            void main() {
                vec3 normal = normalize(vNormal);
                gAlbedoMetal = vec4(vAlbedo, uMetallic);
                gNormalRough = vec4(normal, uRoughness);
                gDepth = gl_FragCoord.z;
            }
        )";

        static const std::string fullscreenVertexShader = R"(
            #version 410 core
            out vec2 vUv;
            const vec2 kVerts[3] = vec2[3](
                vec2(-1.0, -1.0),
                vec2(3.0, -1.0),
                vec2(-1.0, 3.0)
            );

            void main() {
                vec2 pos = kVerts[gl_VertexID];
                vUv = pos * 0.5 + 0.5;
                gl_Position = vec4(pos, 0.0, 1.0);
            }
        )";

        static const std::string dirLightFragmentShader = R"(
            #version 410 core
            in vec2 vUv;
            out vec4 FragColor;

            uniform sampler2D uGAlbedoMetal;
            uniform sampler2D uGNormalRough;
            uniform sampler2D uDepth;
            uniform mat4 uInvProj;
            uniform vec3 uDirLightDir;
            uniform vec3 uDirLightColor;
            uniform float uDirLightIntensity;
            uniform vec3 uAmbient;

            vec3 reconstructViewPos(vec2 uv, float depth) {
                vec4 ndc = vec4(uv * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
                vec4 view = uInvProj * ndc;
                return view.xyz / view.w;
            }

            void main() {
                float depth = texture(uDepth, vUv).r;
                if (depth >= 0.99999) {
                    FragColor = vec4(0.0);
                    return;
                }

                vec4 albedoMetal = texture(uGAlbedoMetal, vUv);
                vec4 normalRough = texture(uGNormalRough, vUv);

                vec3 albedo = albedoMetal.rgb;
                float metallic = albedoMetal.a;
                vec3 normal = normalize(normalRough.xyz);
                float roughness = normalRough.a;

                vec3 viewPos = reconstructViewPos(vUv, depth);
                vec3 V = normalize(-viewPos);
                vec3 L = normalize(-uDirLightDir);
                vec3 H = normalize(L + V);

                float ndotl = max(dot(normal, L), 0.0);
                float specPower = mix(64.0, 4.0, roughness);
                float spec = pow(max(dot(normal, H), 0.0), specPower);

                vec3 F0 = mix(vec3(0.04), albedo, metallic);
                vec3 diffuse = (1.0 - metallic) * albedo / 3.14159265;
                vec3 specular = F0 * spec;

                vec3 color = uAmbient * albedo;
                color += (diffuse + specular) * uDirLightColor * uDirLightIntensity * ndotl;

                FragColor = vec4(color, 1.0);
            }
        )";

        static const std::string volumeVertexShader = R"(
            #version 410 core
            layout (location = 0) in vec3 aPos;

            uniform mat4 uProj;
            uniform samplerBuffer uLightBuffer;
            uniform int uLightOffset;
            uniform int uIsSpot;

            flat out int vLightIndex;

            void main() {
                int lightIndex = uLightOffset + gl_InstanceID;
                vLightIndex = lightIndex;
                int base = lightIndex * 4;

                vec4 posRadius = texelFetch(uLightBuffer, base);
                vec4 dirType = texelFetch(uLightBuffer, base + 2);
                vec4 spotParams = texelFetch(uLightBuffer, base + 3);

                vec3 lightPos = posRadius.xyz;
                float radius = posRadius.w;

                vec3 viewPos;
                if (uIsSpot == 1) {
                    vec3 dir = normalize(dirType.xyz);
                    float coneLength = spotParams.z;
                    float coneRadius = spotParams.w * coneLength;
                    vec3 scaled = vec3(aPos.x * coneRadius, aPos.y * coneRadius, aPos.z * coneLength);

                    vec3 up = abs(dir.z) < 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(0.0, 1.0, 0.0);
                    vec3 right = normalize(cross(up, dir));
                    vec3 up2 = cross(dir, right);
                    vec3 rotated = right * scaled.x + up2 * scaled.y + dir * scaled.z;
                    viewPos = lightPos + rotated;
                } else {
                    viewPos = lightPos + aPos * radius;
                }

                gl_Position = uProj * vec4(viewPos, 1.0);
            }
        )";

        static const std::string volumeFragmentShader = R"(
            #version 410 core
            flat in int vLightIndex;
            out vec4 FragColor;

            uniform sampler2D uGAlbedoMetal;
            uniform sampler2D uGNormalRough;
            uniform sampler2D uDepth;
            uniform samplerBuffer uLightBuffer;
            uniform mat4 uInvProj;
            uniform vec2 uScreenSize;
            uniform int uIsSpot;

            vec3 reconstructViewPos(vec2 uv, float depth) {
                vec4 ndc = vec4(uv * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
                vec4 view = uInvProj * ndc;
                return view.xyz / view.w;
            }

            void main() {
                vec2 uv = gl_FragCoord.xy / uScreenSize;
                float depth = texture(uDepth, uv).r;
                if (depth >= 0.99999) {
                    discard;
                }

                vec4 albedoMetal = texture(uGAlbedoMetal, uv);
                vec4 normalRough = texture(uGNormalRough, uv);

                vec3 albedo = albedoMetal.rgb;
                float metallic = albedoMetal.a;
                vec3 normal = normalize(normalRough.xyz);
                float roughness = normalRough.a;

                vec3 viewPos = reconstructViewPos(uv, depth);

                int base = vLightIndex * 4;
                vec4 posRadius = texelFetch(uLightBuffer, base);
                vec4 colorIntensity = texelFetch(uLightBuffer, base + 1);
                vec4 dirType = texelFetch(uLightBuffer, base + 2);
                vec4 spotParams = texelFetch(uLightBuffer, base + 3);

                vec3 lightPos = posRadius.xyz;
                float radius = posRadius.w;
                vec3 toLight = lightPos - viewPos;
                float dist2 = dot(toLight, toLight);
                if (dist2 > radius * radius) {
                    discard;
                }

                float dist = sqrt(dist2);
                vec3 L = toLight / max(dist, 0.0001);
                float attenuation = clamp(1.0 - dist / radius, 0.0, 1.0);
                attenuation *= attenuation;

                if (uIsSpot == 1) {
                    vec3 spotDir = normalize(dirType.xyz);
                    float cosTheta = dot(normalize(-L), spotDir);
                    float inner = spotParams.x;
                    float outer = spotParams.y;
                    float spot = smoothstep(outer, inner, cosTheta);
                    attenuation *= spot;
                }

                float ndotl = max(dot(normal, L), 0.0);
                if (ndotl <= 0.0) {
                    discard;
                }

                vec3 V = normalize(-viewPos);
                vec3 H = normalize(L + V);
                float specPower = mix(64.0, 4.0, roughness);
                float spec = pow(max(dot(normal, H), 0.0), specPower);

                vec3 F0 = mix(vec3(0.04), albedo, metallic);
                vec3 diffuse = (1.0 - metallic) * albedo / 3.14159265;
                vec3 specular = F0 * spec;

                vec3 lightColor = colorIntensity.rgb * colorIntensity.w;
                vec3 color = (diffuse + specular) * lightColor * ndotl * attenuation;

                FragColor = vec4(color, 1.0);
            }
        )";

        static const std::string compositeFragmentShader = R"(
            #version 410 core
            in vec2 vUv;
            out vec4 FragColor;

            uniform sampler2D uLightBuffer;
            uniform sampler2D uGAlbedoMetal;
            uniform sampler2D uGNormalRough;
            uniform sampler2D uDepth;
            uniform int uDebugMode;

            vec3 tonemap(vec3 color) {
                return color / (color + vec3(1.0));
            }

            void main() {
                vec3 color;
                if (uDebugMode == 0) {
                    vec3 hdr = texture(uLightBuffer, vUv).rgb;
                    color = tonemap(hdr);
                } else if (uDebugMode == 1) {
                    color = texture(uGAlbedoMetal, vUv).rgb;
                } else if (uDebugMode == 2) {
                    vec3 normal = normalize(texture(uGNormalRough, vUv).xyz);
                    color = normal * 0.5 + 0.5;
                } else if (uDebugMode == 3) {
                    float rough = texture(uGNormalRough, vUv).a;
                    float metal = texture(uGAlbedoMetal, vUv).a;
                    color = vec3(rough, metal, 0.0);
                } else if (uDebugMode == 4) {
                    float depth = texture(uDepth, vUv).r;
                    color = vec3(depth);
                } else {
                    color = texture(uLightBuffer, vUv).rgb;
                }

                color = pow(clamp(color, 0.0, 1.0), vec3(1.0 / 2.2));
                FragColor = vec4(color, 1.0);
            }
        )";

        if (!deferredGeometryShader_.buildFromSource(gbufferVertexShader, gbufferFragmentShader)) {
            spdlog::error("RenderEngine: failed to build deferred geometry shaders");
            sceneReady_ = false;
            return;
        }

        if (!deferredDirLightShader_.buildFromSource(fullscreenVertexShader, dirLightFragmentShader)) {
            spdlog::error("RenderEngine: failed to build deferred directional shader");
            sceneReady_ = false;
            return;
        }

        if (!deferredVolumeShader_.buildFromSource(volumeVertexShader, volumeFragmentShader)) {
            spdlog::error("RenderEngine: failed to build deferred volume shaders");
            sceneReady_ = false;
            return;
        }

        if (!deferredCompositeShader_.buildFromSource(fullscreenVertexShader, compositeFragmentShader)) {
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
        compositeDebugModeLocation_ = deferredCompositeShader_.uniformLocation("uDebugMode");

        deferredDirLightShader_.use();
        glUniform1i(deferredDirLightShader_.uniformLocation("uGAlbedoMetal"), 0);
        glUniform1i(deferredDirLightShader_.uniformLocation("uGNormalRough"), 1);
        glUniform1i(deferredDirLightShader_.uniformLocation("uDepth"), 2);

        deferredVolumeShader_.use();
        glUniform1i(deferredVolumeShader_.uniformLocation("uGAlbedoMetal"), 0);
        glUniform1i(deferredVolumeShader_.uniformLocation("uGNormalRough"), 1);
        glUniform1i(deferredVolumeShader_.uniformLocation("uDepth"), 2);
        glUniform1i(deferredVolumeShader_.uniformLocation("uLightBuffer"), 3);

        deferredCompositeShader_.use();
        glUniform1i(deferredCompositeShader_.uniformLocation("uLightBuffer"), 0);
        glUniform1i(deferredCompositeShader_.uniformLocation("uGAlbedoMetal"), 1);
        glUniform1i(deferredCompositeShader_.uniformLocation("uGNormalRough"), 2);
        glUniform1i(deferredCompositeShader_.uniformLocation("uDepth"), 3);

        buildLights();
        volumeReady = buildVolumeMeshes();

        shadersReady = deferredGeometryShader_.id() != 0 &&
                       deferredDirLightShader_.id() != 0 &&
                       deferredVolumeShader_.id() != 0 &&
                       deferredCompositeShader_.id() != 0;
    }

    std::vector<float> groundVerts;
    std::vector<unsigned int> groundIdx;
    const float g = 5.0f;
    const std::array<Vertex, 4> groundQuad{{
        {-g, 0.0f, -g, 0.0f, 1.0f, 0.0f, 0.18f, 0.36f, 0.20f},
        {g, 0.0f, -g, 0.0f, 1.0f, 0.0f, 0.18f, 0.36f, 0.20f},
        {g, 0.0f, g, 0.0f, 1.0f, 0.0f, 0.18f, 0.36f, 0.20f},
        {-g, 0.0f, g, 0.0f, 1.0f, 0.0f, 0.18f, 0.36f, 0.20f},
    }};
    addQuad(groundQuad, groundVerts, groundIdx);

    std::vector<float> wallVerts;
    std::vector<unsigned int> wallIdx;
    const float wallHeight = 2.5f;
    const float wallOffset = 3.0f;
    const float wallLength = 5.0f;

    // The vertical walls must be defined in a counter‑clockwise order from the
    // camera’s point of view in our isometric projection.  When we flipped the
    // Y‑rotation to +45° the original vertex order resulted in a clockwise
    // winding for the walls, so they were culled as back faces.  Reordering
    // the vertices here restores a CCW winding without disabling face
    // culling.  The normals remain the same; we still want wallA facing +X
    // and wallB facing −X.

    // Wall A (x = -wallOffset) ordered: bottom‑near, bottom‑far, top‑far, top‑near
    const std::array<Vertex, 4> wallAQuad{{
        {-wallOffset, 0.0f, -wallLength, 1.0f, 0.0f, 0.0f, 0.70f, 0.25f, 0.25f},   // bottom near
        {-wallOffset, 0.0f,  wallLength, 1.0f, 0.0f, 0.0f, 0.70f, 0.25f, 0.25f},   // bottom far
        {-wallOffset, wallHeight,  wallLength, 1.0f, 0.0f, 0.0f, 0.70f, 0.25f, 0.25f}, // top far
        {-wallOffset, wallHeight, -wallLength, 1.0f, 0.0f, 0.0f, 0.70f, 0.25f, 0.25f}, // top near
    }};
    addQuad(wallAQuad, wallVerts, wallIdx);

    // Wall B (x = +wallOffset) ordered similarly
    const std::array<Vertex, 4> wallBQuad{{
        { wallOffset, 0.0f, -wallLength, -1.0f, 0.0f, 0.0f, 0.25f, 0.45f, 0.70f},   // bottom near
        { wallOffset, 0.0f,  wallLength, -1.0f, 0.0f, 0.0f, 0.25f, 0.45f, 0.70f},   // bottom far
        { wallOffset, wallHeight,  wallLength, -1.0f, 0.0f, 0.0f, 0.25f, 0.45f, 0.70f}, // top far
        { wallOffset, wallHeight, -wallLength, -1.0f, 0.0f, 0.0f, 0.25f, 0.45f, 0.70f}, // top near
    }};
    std::vector<float> wallBVerts;
    std::vector<unsigned int> wallBIdx;
    addQuad(wallBQuad, wallBVerts, wallBIdx);

    sceneReady_ = shadersReady &&
                  volumeReady &&
                  ground_.upload(groundVerts, groundIdx) &&
                  wallA_.upload(wallVerts, wallIdx) &&
                  wallB_.upload(wallBVerts, wallBIdx);
}

}  // namespace render
