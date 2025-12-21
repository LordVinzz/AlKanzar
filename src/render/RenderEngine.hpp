#pragma once

#include <SDL.h>
#include <SDL_opengl.h>

#include <string>

namespace render {

class RenderEngine {
public:
    RenderEngine(int width, int height, std::string title = "AlKanzar - Render Preview");
    ~RenderEngine();

    RenderEngine(const RenderEngine&) = delete;
    RenderEngine& operator=(const RenderEngine&) = delete;

    bool init();
    void run();

private:
    void handleEvent(const SDL_Event& event, bool& running);
    void updateProjection() const;
    void renderScene() const;
    void drawGround() const;
    void drawWalls() const;

    SDL_Window* window_{nullptr};
    SDL_GLContext glContext_{nullptr};
    int width_;
    int height_;
    float zoom_{1.0f};
    float cameraX_{0.0f};
    float cameraZ_{0.0f};
    bool middleDragging_{false};
    int lastMouseX_{0};
    int lastMouseY_{0};
    std::string title_;
};

}  // namespace render
