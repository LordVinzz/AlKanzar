cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED SOURCE_ROOT OR NOT IS_DIRECTORY "${SOURCE_ROOT}")
    message(FATAL_ERROR "SOURCE_ROOT must point to the project's src directory")
endif()

# A file containing exactly 500 lines already violates the rule: every
# project-owned source file must contain strictly fewer than 500 lines.
set(LINE_LIMIT 500)

file(GLOB_RECURSE SOURCE_FILES LIST_DIRECTORIES false
    "${SOURCE_ROOT}/*.c"
    "${SOURCE_ROOT}/*.cc"
    "${SOURCE_ROOT}/*.cpp"
    "${SOURCE_ROOT}/*.cxx"
    "${SOURCE_ROOT}/*.h"
    "${SOURCE_ROOT}/*.hh"
    "${SOURCE_ROOT}/*.hpp"
    "${SOURCE_ROOT}/*.hxx"
    "${SOURCE_ROOT}/*.inl"
    "${SOURCE_ROOT}/*.ipp"
    "${SOURCE_ROOT}/*.glsl"
    "${SOURCE_ROOT}/*.vert"
    "${SOURCE_ROOT}/*.frag"
    "${SOURCE_ROOT}/*.geom"
    "${SOURCE_ROOT}/*.comp"
)
list(SORT SOURCE_FILES)

set(VIOLATIONS "")
foreach(SOURCE_FILE IN LISTS SOURCE_FILES)
    file(READ "${SOURCE_FILE}" CONTENTS)
    if(CONTENTS STREQUAL "")
        set(LINE_COUNT 0)
    else()
        string(REGEX MATCHALL "\n" LINE_BREAKS "${CONTENTS}")
        list(LENGTH LINE_BREAKS LINE_COUNT)
        string(LENGTH "${CONTENTS}" CONTENT_LENGTH)
        math(EXPR LAST_CHARACTER_INDEX "${CONTENT_LENGTH} - 1")
        string(SUBSTRING
            "${CONTENTS}"
            ${LAST_CHARACTER_INDEX}
            1
            LAST_CHARACTER
        )
        if(NOT LAST_CHARACTER STREQUAL "\n")
            math(EXPR LINE_COUNT "${LINE_COUNT} + 1")
        endif()
    endif()

    if(LINE_COUNT GREATER_EQUAL LINE_LIMIT)
        file(RELATIVE_PATH RELATIVE_FILE "${SOURCE_ROOT}" "${SOURCE_FILE}")
        list(APPEND VIOLATIONS "  ${RELATIVE_FILE}: ${LINE_COUNT} lines")
    endif()
endforeach()

if(VIOLATIONS)
    list(JOIN VIOLATIONS "\n" VIOLATION_REPORT)
    message(FATAL_ERROR
        "Project source files must contain fewer than ${LINE_LIMIT} lines.\n"
        "Split the following files along business boundaries:\n"
        "${VIOLATION_REPORT}"
    )
endif()

message(STATUS
    "All source files contain fewer than ${LINE_LIMIT} lines"
)
