# SPDX-License-Identifier: MIT
cmake_minimum_required(VERSION 3.20)

project(fbxanimationviewer
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
    add_reaper_extension(fbxanimationviewer
        SOURCES
            src/plugin_main.cpp
            src/reaper_api.cpp
            src/viewer_window.cpp
    )
    target_link_libraries(fbxanimationviewer PRIVATE opengl32 gdi32 user32)
else()
    message(STATUS
        "fbxanimationviewer: non-Windows platform detected — Phase 0 only "
        "supports Windows. Build target stubbed."
    )
    # Echo the same notice at build time so a developer running just
    # `cmake --build` (without re-configure) cannot mistake the no-op for a
    # successful Windows build.
    add_custom_target(fbxanimationviewer ALL
        COMMAND ${CMAKE_COMMAND} -E echo
            "[fbxanimationviewer] Phase 0 supports Windows only — nothing to build on this platform."
        VERBATIM
    )
endif()
