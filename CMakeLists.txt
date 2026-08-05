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
    src/core/app/ApplicationInput.cpp
    src/core/app/RuntimePolicy.cpp
    src/core/ecs/ComponentRegistry.cpp
    src/core/ecs/ComponentDescriptors.cpp
    src/core/ecs/CharacterComponentDescriptor.cpp
    src/core/ecs/ComponentInspector.cpp
    src/core/gameplay/CharacterInspector.cpp
    src/core/gameplay/CharacterRules.cpp
    src/core/editor/EditorSessionImGuiSettings.cpp
    src/core/editor/EditorUiAnimation.cpp
    src/core/editor/EditorUiHierarchy.cpp
    src/core/editor/EditorUiInspector.cpp
    src/core/editor/EditorUiMainWindow.cpp
    src/core/editor/EditorUiNavigation.cpp
    src/core/editor/EditorUiProfiler.cpp
    src/core/editor/EditorUiSupport.cpp
    src/core/editor/EditorUiTextureBrowser.cpp
    src/core/physics/PhysicsSystem.cpp
    src/core/navigation/Navigation.cpp
    src/core/navigation/NavigationAgentMotion.cpp
    src/core/navigation/NavigationAgentRequests.cpp
    src/core/navigation/NavigationAgentResults.cpp
    src/core/navigation/NavigationAgentSupport.cpp
    src/core/navigation/NavigationAssetSerialization.cpp
    src/core/navigation/NavigationAssetLifecycle.cpp
    src/core/navigation/NavigationEditor.cpp
    src/core/navigation/NavigationEditorSupport.cpp
    src/core/navigation/NavigationFrameSync.cpp
    src/core/navigation/NavigationDetailGeometry.cpp
    src/core/navigation/NavigationDetailRuntime.cpp
    src/core/navigation/NavigationRuntimeSupport.cpp
    src/core/navigation/NavigationRuntimeBuilder.cpp
    src/core/navigation/NavigationBakeSources.cpp
    src/core/navigation/NavigationDelaunayRemesh.cpp
    src/core/navigation/NavigationDelaunayCells.cpp
    src/core/navigation/NavigationBakeRuntimeCells.cpp
    src/core/navigation/NavigationSurfaceCoverage.cpp
    src/core/navigation/NavigationSurfaceSweep.cpp
    src/core/navigation/NavigationRuntimeQueries.cpp
    src/core/navigation/NavigationSolveEndpoints.cpp
    src/core/navigation/NavigationCellMerge.cpp
    src/core/navigation/NavigationCorridorSearch.cpp
    src/core/navigation/NavigationCorridorVisibility.cpp
    src/core/navigation/NavigationVisibilityNodes.cpp
    src/core/navigation/NavigationFunnel.cpp
    src/core/navigation/NavigationLinkedPath.cpp
    src/core/navigation/NavigationGenerator.cpp
    src/core/navigation/NavigationHitTest.cpp
    src/core/navigation/NavigationPathSolver.cpp
    src/core/navigation/NavigationPolygonOperations.cpp
    src/core/navigation/NavigationPolygonTopology.cpp
    src/core/navigation/NavigationDetailClearance.cpp
    src/core/navigation/NavigationEndpointResolution.cpp
    src/core/navigation/Polyanya.cpp
    src/core/navigation/PolyanyaMeshBuilder.cpp
    src/core/navigation/PolyanyaSearch.cpp
    third_party/polyanya/expansion.cpp
    third_party/polyanya/geometry.cpp
    src/core/scene/Camera.cpp
    src/core/scene/SceneFactory.cpp
    src/core/scene/SceneMeshFactory.cpp
    src/core/scene/SceneModelFactory.cpp
    src/core/scene/SceneRegistry.cpp
    src/core/lighting/LightSystem.cpp
    src/core/transform/TransformMath.cpp
    src/core/transform/TransformSystem.cpp
    src/core/profiling/PerfettoTraceExporter.cpp
    src/core/profiling/PerfettoProto.cpp
    src/core/profiling/ProfilerService.cpp
    src/core/profiling/ProfilerRuntime.cpp
    src/core/systems/PickingSystem.cpp
    src/core/systems/RenderExtractionAnimation.cpp
    src/core/systems/RenderExtractionSystem.cpp
    src/core/systems/TaskScheduler.cpp
)

target_include_directories(alkanzar_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/cdt/include
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

    add_executable(alkanzar_character_rules_tests
        tests/character_rules_tests.cpp
    )

    target_include_directories(alkanzar_core_tests
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src
    )

    target_link_libraries(alkanzar_core_tests
        PRIVATE
            alkanzar_core
    )

    target_link_libraries(alkanzar_character_rules_tests
        PRIVATE
            alkanzar_core
    )

    add_test(NAME alkanzar_core_tests COMMAND alkanzar_core_tests)
    add_test(NAME alkanzar_character_rules_tests COMMAND alkanzar_character_rules_tests)

    add_test(
        NAME alkanzar_src_file_length
        COMMAND
            ${CMAKE_COMMAND}
            -DSOURCE_ROOT=${CMAKE_CURRENT_SOURCE_DIR}/src
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_src_file_length.cmake
    )
    set_tests_properties(
        alkanzar_src_file_length
        PROPERTIES LABELS "architecture;quality"
    )
endif()
