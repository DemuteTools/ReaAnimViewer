# SPDX-License-Identifier: MIT
#
# add_reaper_extension(<target> SOURCES <sources...>)
#
# Defines a SHARED library target that produces a Reaper extension DLL
# with the mandatory `reaper_` filename prefix that Reaper's loader scans for.
#
# On Windows: <target>.dll → reaper_<target>.dll
# On Linux:   lib<target>.so → reaper_<target>.so
# On macOS:   lib<target>.dylib → reaper_<target>.dylib
#
# Usage:
#   add_reaper_extension(fbxanimationviewer
#       SOURCES src/plugin_main.cpp src/viewer_window.cpp ...)

function(add_reaper_extension target)
    cmake_parse_arguments(ARG "" "" "SOURCES" ${ARGN})
    if(NOT ARG_SOURCES)
        message(FATAL_ERROR "add_reaper_extension(${target}): SOURCES is required")
    endif()

    add_library(${target} SHARED ${ARG_SOURCES})

    set_target_properties(${target} PROPERTIES
        PREFIX ""
        OUTPUT_NAME "reaper_${target}"
        CXX_STANDARD 17
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
    )

    target_include_directories(${target} PRIVATE
        "${CMAKE_SOURCE_DIR}/extern/reaper-sdk/sdk"
    )

    if(MSVC)
        target_compile_options(${target} PRIVATE /W3 /permissive-)
        target_compile_definitions(${target} PRIVATE
            _CRT_SECURE_NO_WARNINGS
            NOMINMAX
            WIN32_LEAN_AND_MEAN
            UNICODE
            _UNICODE
        )
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra)
    endif()
endfunction()
