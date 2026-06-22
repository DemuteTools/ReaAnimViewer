# SPDX-License-Identifier: MIT
#
# THROWAWAY SPIKE (Spike 0 / Story 0-1) build — NOT production, do not merge to main.
# Builds reaper_fbxav_spike.dll: a feasibility prototype (assimp -> GPU skinning
# -> FBO -> CPU readback -> ReaImGui panel + fps). Windows x64 / MSVC only.
#
# Prereq: drop the generated ReaImGui binding header at
#   extern/reaimgui/include/reaper_imgui_functions.h
# (from https://github.com/cfillion/reaimgui/releases). It is generated, not in
# the ReaImGui source tree, so it is not vendored.

cmake_minimum_required(VERSION 3.20)

project(fbxanimationviewer
    VERSION 0.0.1
    DESCRIPTION "Feasibility spike: glTF skinned rig in a Reaper ReaImGui panel"
    LANGUAGES CXX
)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")
include(ReaperPlugin)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

if(NOT WIN32)
    message(STATUS "fbxanimationviewer spike: Windows-only. Build target stubbed on this platform.")
    add_custom_target(fbxav_spike ALL
        COMMAND ${CMAKE_COMMAND} -E echo
            "[fbxav_spike] Spike 0 supports Windows x64/MSVC only — nothing to build here."
        VERBATIM)
    return()
endif()

include(FetchContent)

# --- assimp v6.0.5, importers narrowed to glTF only (D15; FBX is a spike non-goal) ---
set(ASSIMP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_SAMPLES OFF CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_ASSIMP_TOOLS OFF CACHE BOOL "" FORCE)
set(ASSIMP_INSTALL OFF CACHE BOOL "" FORCE)
set(ASSIMP_WARNINGS_AS_ERRORS OFF CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_ALL_IMPORTERS_BY_DEFAULT OFF CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_ALL_EXPORTERS_BY_DEFAULT OFF CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_GLTF_IMPORTER ON CACHE BOOL "" FORCE)
set(ASSIMP_NO_EXPORT ON CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(assimp
    GIT_REPOSITORY https://github.com/assimp/assimp.git
    GIT_TAG v6.0.5
    GIT_SHALLOW TRUE)

# --- GLM 1.0.3 (header-only) ---
FetchContent_Declare(glm
    GIT_REPOSITORY https://github.com/g-truc/glm.git
    GIT_TAG 1.0.3
    GIT_SHALLOW TRUE)

FetchContent_MakeAvailable(assimp glm)

add_reaper_extension(fbxav_spike
    SOURCES
        src/spike_main.cpp
        src/spike_gl.cpp
        src/spike_loader.cpp
        src/spike_renderer.cpp
        src/reaper_api.cpp
)

target_include_directories(fbxav_spike PRIVATE
    "${CMAKE_SOURCE_DIR}/src"
    "${CMAKE_SOURCE_DIR}/extern/reaimgui/include"
    "${CMAKE_SOURCE_DIR}/extern/sokol"          # vendored but unused by the spike (see findings)
)

target_link_libraries(fbxav_spike PRIVATE
    assimp::assimp
    glm::glm
    opengl32 gdi32 user32
)

# Spike hygiene is relaxed (throwaway): assimp + GLM headers can trip /W3, so the
# spike does not gate on warnings the way production does (NFR-R5 applies on main).
