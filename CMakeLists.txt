# SPDX-License-Identifier: MIT
cmake_minimum_required(VERSION 3.20)

project(reaanimviewer
    VERSION 0.0.1
    DESCRIPTION "Reaper extension that loads glTF/FBX rigs synced to the transport"
    LANGUAGES CXX
)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")
include(ReaperPlugin)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

if(WIN32)
    # GLM 1.0.3 (header-only) — first matrix-math consumer arrives with the
    # renderer (AR6). Vendoring the full GLM header tree is impractical on the
    # drvfs/WSL mount, so we use the sanctioned FetchContent fallback pinned to
    # the 1.0.3 tag (the spike's proven approach). Recorded in extern/VENDORED.md.
    include(FetchContent)
    FetchContent_Declare(glm
        GIT_REPOSITORY https://github.com/g-truc/glm.git
        GIT_TAG 1.0.3
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(glm)

    add_reaper_extension(animviewer
        SOURCES
            src/plugin_main.cpp
            src/reaper_api.cpp
            src/viewer_window.cpp
            src/gl_loader.cpp
            src/renderer.cpp
            src/console_log.cpp
    )
    # GLM as a SYSTEM include so its headers cannot trip /W3 /permissive- (NFR-R5).
    target_include_directories(animviewer SYSTEM PRIVATE "${glm_SOURCE_DIR}")
    target_link_libraries(animviewer PRIVATE opengl32 gdi32 user32)
else()
    message(STATUS
        "animviewer: non-Windows platform detected — Phase 0 only "
        "supports Windows. Build target stubbed."
    )
    # Echo the same notice at build time so a developer running just
    # `cmake --build` (without re-configure) cannot mistake the no-op for a
    # successful Windows build.
    add_custom_target(animviewer ALL
        COMMAND ${CMAKE_COMMAND} -E echo
            "[ReaAnimViewer] Phase 0 supports Windows only — nothing to build on this platform."
        VERBATIM
    )
endif()
