cmake_minimum_required(VERSION 3.16)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

project(AlKanzar VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(CTest)

find_package(SDL2 REQUIRED)
find_package(OpenGL REQUIRED)
find_package(Threads REQUIRED)
find_package(spdlog REQUIRED)
find_package(glm CONFIG REQUIRED)

add_subdirectory(src/render)

add_library(alkanzar_core STATIC
    src/core/animation/AnimationSystem.cpp
    src/core/app/AppState.cpp
    src/core/app/Application.cpp
    src/core/scene/Camera.cpp
    src/core/scene/SceneFactory.cpp
    src/core/scene/SceneRegistry.cpp
    src/core/lighting/LightSystem.cpp
    src/core/transform/TransformMath.cpp
    src/core/transform/TransformSystem.cpp
    src/core/profiling/PerfettoTraceExporter.cpp
    src/core/profiling/ProfilerService.cpp
    src/core/systems/PickingSystem.cpp
    src/core/systems/RenderExtractionSystem.cpp
    src/core/systems/TaskScheduler.cpp
)

target_include_directories(alkanzar_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(alkanzar_core
    PUBLIC
        alkanzar_render
        spdlog::spdlog
        Threads::Threads
)

add_executable(AlKanzar
    src/main.cpp
)

target_link_libraries(AlKanzar
    PRIVATE
        alkanzar_core
)

if(BUILD_TESTING)
    add_executable(alkanzar_core_tests
        tests/core_tests.cpp
    )

    target_include_directories(alkanzar_core_tests
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src
    )

    target_link_libraries(alkanzar_core_tests
        PRIVATE
            alkanzar_core
    )

    add_test(NAME alkanzar_core_tests COMMAND alkanzar_core_tests)
endif()
