#include "RenderEngine.hpp"

#include <algorithm>
#include <array>
#include <utility>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <spdlog/spdlog.h>

namespace {
constexpr float kIsoAngleX = 35.264f;  // atan(sqrt(1/2)) in degrees
constexpr float kIsoAngleY = 45.0f;
constexpr float kBaseOrthoSize = 10.0f;
constexpr float kMinZoom = 0.2f;
constexpr float kMaxZoom = 5.0f;

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

}  // namespace

namespace render {

RenderEngine::RenderEngine(int width, int height, std::string title)
    : width_(width),
      height_(height),
      title_(std::move(title)) {}

RenderEngine::~RenderEngine() {
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

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
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
        spdlog::error("RenderEngine: unable to create GL context: {}", SDL_GetError());
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
            if (event.key.keysym.sym == SDLK_ESCAPE) {
                running = false;
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
}

void RenderEngine::setMVPUniform() const {
    const glm::mat4 mvp = projection_ * view_;
    glUniformMatrix4fv(mvpLocation_, 1, GL_FALSE, glm::value_ptr(mvp));
}

void RenderEngine::drawLayer(RenderLayer layer, std::initializer_list<const MeshBuffer*> meshes) const {
    // Ground doesn't write depth so vertical geometry isn't occluded in isometric view.
    const GLboolean depthWrite = layer == RenderLayer::Ground ? GL_FALSE : GL_TRUE;
    glDepthMask(depthWrite);
    for (const MeshBuffer* mesh : meshes) {
        mesh->draw();
    }
}

void RenderEngine::renderScene() {
    if (!sceneReady_) {
        return;
    }

    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    shader_.use();
    setMVPUniform();
    glUniform3f(lightDirLocation_, -0.3f, -1.0f, -0.4f);

    drawLayer(RenderLayer::Ground, {&ground_});
    drawLayer(RenderLayer::Geometry, {&wallA_, &wallB_});
    drawLayer(RenderLayer::Actors, {});
}

void RenderEngine::buildScene() {
    static const std::string vertexShader = R"(
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

    static const std::string fragmentShader = R"(
        #version 330 core
        in vec3 vColor;
        out vec4 FragColor;

        void main() {
            FragColor = vec4(vColor, 1.0);
        }
    )";

    if (!shader_.buildFromSource(vertexShader, fragmentShader)) {
        spdlog::error("RenderEngine: failed to build shaders");
        sceneReady_ = false;
        return;
    }

    mvpLocation_ = shader_.uniformLocation("uMVP");
    lightDirLocation_ = shader_.uniformLocation("uLightDir");

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

    sceneReady_ = shader_.id() != 0 &&
                  ground_.upload(groundVerts, groundIdx) &&
                  wallA_.upload(wallVerts, wallIdx) &&
                  wallB_.upload(wallBVerts, wallBIdx);
}

}  // namespace render
