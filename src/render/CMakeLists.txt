add_library(alkanzar_render STATIC
    RenderEngine.cpp
    ShaderProgram.cpp
    MeshBuffer.cpp
)

target_include_directories(alkanzar_render
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(alkanzar_render
    PUBLIC
        SDL2::SDL2
        OpenGL::GL
        glm::glm
    PRIVATE
        spdlog::spdlog
)
