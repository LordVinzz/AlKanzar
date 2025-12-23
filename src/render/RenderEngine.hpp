#ifndef GL_GLEXT_PROTOTYPES
#define GL_GLEXT_PROTOTYPES
#endif
#pragma once

#include <SDL.h>
#include <SDL_opengl.h>

#include <array>
#include <initializer_list>
#include <string>

#include "MeshBuffer.hpp"
#include "ShaderProgram.hpp"

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
    void handleEvent(const SDL_Event& event, bool& running);
    void updateProjection();
    void renderScene();
    void buildScene();

    void setMVPUniform() const;
    void drawLayer(RenderLayer layer, std::initializer_list<const MeshBuffer*> meshes) const;

    SDL_Window* window_{nullptr};
    SDL_GLContext glContext_{nullptr};
    int width_;
    int height_;
    float zoom_{1.0f};
    float panX_{0.0f};
    float panY_{0.0f};
    float cameraDistance_{15.0f};
    bool middleDragging_{false};
    int lastMouseX_{0};
    int lastMouseY_{0};
    std::string title_;

    ShaderProgram shader_;
    MeshBuffer ground_;
    MeshBuffer wallA_;
    MeshBuffer wallB_;
    GLint mvpLocation_{-1};
    GLint lightDirLocation_{-1};

    std::array<float, 16> projection_{};
    std::array<float, 16> view_{};
    bool sceneReady_{false};
};

}  // namespace render
