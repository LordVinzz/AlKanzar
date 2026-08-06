if(NOT DEFINED SOURCE_ROOT)
    message(FATAL_ERROR "SOURCE_ROOT is required")
endif()

function(assert_layer_dependencies layer_name layer_directory)
    file(GLOB_RECURSE layer_files
        "${SOURCE_ROOT}/${layer_directory}/*.hpp"
        "${SOURCE_ROOT}/${layer_directory}/*.cpp"
    )
    if(NOT layer_files)
        message(FATAL_ERROR "Architecture layer '${layer_name}' has no source files")
    endif()

    foreach(layer_file IN LISTS layer_files)
        file(READ "${layer_file}" layer_source)
        foreach(forbidden_dependency IN LISTS ARGN)
            string(FIND "${layer_source}" "${forbidden_dependency}" dependency_position)
            if(NOT dependency_position EQUAL -1)
                file(RELATIVE_PATH relative_file "${SOURCE_ROOT}" "${layer_file}")
                message(FATAL_ERROR
                    "Layer '${layer_name}' may not reference '${forbidden_dependency}' (${relative_file})"
                )
            endif()
        endforeach()
    endforeach()
endfunction()

set(presentation_dependencies
    "core/editor/"
    "render/"
    "<imgui"
    "<SDL"
    "<GL/"
    "<OpenGL/"
)

assert_layer_dependencies(
    "content"
    "core/content"
    "core/rules/"
    "core/simulation/"
    "core/ecs/"
    "core/systems/"
    "core/app/"
    ${presentation_dependencies}
)

assert_layer_dependencies(
    "rules"
    "core/rules"
    "core/simulation/"
    "core/ecs/"
    "core/systems/"
    "core/app/"
    ${presentation_dependencies}
)

assert_layer_dependencies(
    "simulation"
    "core/simulation"
    "core/ecs/"
    "core/systems/"
    "core/app/"
    ${presentation_dependencies}
)

assert_layer_dependencies(
    "ecs infrastructure"
    "core/ecs"
    "core/editor/"
    "<imgui"
)
