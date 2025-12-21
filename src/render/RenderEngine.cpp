#include "RenderEngine.hpp"

#include <algorithm>
#include <array>
#include <utility>

#include <spdlog/spdlog.h>

namespace {
struct Vec3 {
    float x;
    float y;
    float z;
};

constexpr float kIsoAngleX = 35.264f;  // atan(sqrt(1/2)) in degrees
constexpr float kIsoAngleY = 45.0f;
constexpr float kBaseOrthoSize = 10.0f;
constexpr float kMinZoom = 0.2f;
constexpr float kMaxZoom = 5.0f;

void drawQuad(const Vec3& a, const Vec3& b, const Vec3& c, const Vec3& d, const Vec3& normal, const std::array<float, 3>& color) {
    glBegin(GL_QUADS);
    glColor3f(color[0], color[1], color[2]);
    glNormal3f(normal.x, normal.y, normal.z);
    glVertex3f(a.x, a.y, a.z);
    glVertex3f(b.x, b.y, b.z);
    glVertex3f(c.x, c.y, c.z);
    glVertex3f(d.x, d.y, d.z);
    glEnd();
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

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_COMPATIBILITY);
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
    return true;
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
                cameraX_ -= static_cast<float>(event.motion.xrel) * panSpeed / zoom_;
                cameraZ_ += static_cast<float>(event.motion.yrel) * panSpeed / zoom_;
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

void RenderEngine::updateProjection() const {
    glViewport(0, 0, width_, height_);

    const float aspect = static_cast<float>(width_) / static_cast<float>(height_ > 0 ? height_ : 1);
    const float halfSize = kBaseOrthoSize / zoom_;

    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(-halfSize * aspect, halfSize * aspect, -halfSize, halfSize, -50.0f, 50.0f);

    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glRotatef(kIsoAngleX, 1.0f, 0.0f, 0.0f);
    glRotatef(kIsoAngleY, 0.0f, 1.0f, 0.0f);
    glTranslatef(-cameraX_, 0.0f, -cameraZ_);
}

void RenderEngine::renderScene() const {
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    updateProjection();
    drawGround();
    drawWalls();
}

void RenderEngine::drawGround() const {
    const float size = 5.0f;
    const Vec3 normal{0.0f, 1.0f, 0.0f};
    const std::array<float, 3> color{0.18f, 0.36f, 0.20f};

    drawQuad(
        Vec3{-size, 0.0f, -size},
        Vec3{size, 0.0f, -size},
        Vec3{size, 0.0f, size},
        Vec3{-size, 0.0f, size},
        normal,
        color
    );
}

void RenderEngine::drawWalls() const {
    const float wallHeight = 2.5f;
    const float wallDepth = 0.1f;
    const float wallLength = 5.0f;

    const Vec3 normal{0.0f, 0.0f, 1.0f};
    const std::array<float, 3> wallColorA{0.70f, 0.25f, 0.25f};
    const std::array<float, 3> wallColorB{0.25f, 0.45f, 0.70f};

    drawQuad(
        Vec3{-wallLength, 0.0f, -wallDepth},
        Vec3{-wallLength, wallHeight, -wallDepth},
        Vec3{-wallLength, wallHeight, wallDepth},
        Vec3{-wallLength, 0.0f, wallDepth},
        normal,
        wallColorA
    );

    drawQuad(
        Vec3{wallLength, 0.0f, -wallDepth},
        Vec3{wallLength, wallHeight, -wallDepth},
        Vec3{wallLength, wallHeight, wallDepth},
        Vec3{wallLength, 0.0f, wallDepth},
        normal,
        wallColorB
    );
}

}  // namespace render
