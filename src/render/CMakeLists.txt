add_library(alkanzar_render STATIC
    RenderEngine.cpp
    ShadowSystem.cpp
    ShaderProgram.cpp
    MeshBuffer.cpp
    LightExecutionVolume.cpp
    StaticGltfModel.cpp
)

set(SHADER_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/shaders)
set(SHADER_OUTPUT_DIR ${CMAKE_BINARY_DIR}/shaders)
set(SHADER_FILES
    ${SHADER_SOURCE_DIR}/simple.vert
    ${SHADER_SOURCE_DIR}/simple.frag
    ${SHADER_SOURCE_DIR}/debug_color.vert
    ${SHADER_SOURCE_DIR}/debug_color.frag
    ${SHADER_SOURCE_DIR}/deferred_gbuffer.vert
    ${SHADER_SOURCE_DIR}/deferred_gbuffer.frag
    ${SHADER_SOURCE_DIR}/fullscreen_tri.vert
    ${SHADER_SOURCE_DIR}/deferred_dir_light.frag
    ${SHADER_SOURCE_DIR}/deferred_volume.vert
    ${SHADER_SOURCE_DIR}/deferred_volume.frag
    ${SHADER_SOURCE_DIR}/deferred_composite.frag
    ${SHADER_SOURCE_DIR}/shadow_depth.vert
    ${SHADER_SOURCE_DIR}/shadow_depth.frag
)

add_custom_target(copy_shaders ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory ${SHADER_OUTPUT_DIR}
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${SHADER_FILES} ${SHADER_OUTPUT_DIR}
    DEPENDS ${SHADER_FILES}
    COMMENT "Copying shaders to build/shaders"
)

set(MODEL_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/models)
set(MODEL_OUTPUT_DIR ${CMAKE_BINARY_DIR}/models)
set(MODEL_FILES
    ${MODEL_SOURCE_DIR}/Adventurer.glb
)

add_custom_target(copy_models ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory ${MODEL_OUTPUT_DIR}
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${MODEL_FILES} ${MODEL_OUTPUT_DIR}
    DEPENDS ${MODEL_FILES}
    COMMENT "Copying models to build/models"
)

add_dependencies(alkanzar_render copy_shaders copy_models)

target_include_directories(alkanzar_render
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/..
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/vendor
)

target_link_libraries(alkanzar_render
    PUBLIC
        SDL2::SDL2
        OpenGL::GL
        glm::glm
    PRIVATE
        spdlog::spdlog
)
