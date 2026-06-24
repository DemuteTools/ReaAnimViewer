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

    # assimp 6.0.5 (Spike-0 proven pin) — the glTF/FBX/Collada loader (D5/D15).
    # Static link (BUILD_SHARED_LIBS OFF) so the deliverable stays a single
    # reaper_animviewer.dll with NO assimp side-DLL (D15/D17). The importer set is
    # narrowed to glTF + FBX + Collada (AR5): ~38 other importers off, which is also
    # an attack-surface reduction (the v6.0.x CVEs sit in parsers we don't compile).
    # ASSIMP_WARNINGS_AS_ERRORS OFF: assimp's own tree warns under MSVC and would
    # otherwise fail its build; our extension's /W3 /permissive- is unaffected
    # because assimp headers are added as a SYSTEM include below.
    set(ASSIMP_BUILD_TESTS                    OFF CACHE BOOL "" FORCE)
    set(ASSIMP_BUILD_SAMPLES                  OFF CACHE BOOL "" FORCE)
    set(ASSIMP_BUILD_ASSIMP_TOOLS             OFF CACHE BOOL "" FORCE)
    set(ASSIMP_INSTALL                        OFF CACHE BOOL "" FORCE)
    set(ASSIMP_WARNINGS_AS_ERRORS             OFF CACHE BOOL "" FORCE)
    set(ASSIMP_NO_EXPORT                      ON  CACHE BOOL "" FORCE)
    set(ASSIMP_BUILD_ALL_IMPORTERS_BY_DEFAULT OFF CACHE BOOL "" FORCE)
    set(ASSIMP_BUILD_ALL_EXPORTERS_BY_DEFAULT OFF CACHE BOOL "" FORCE)
    set(ASSIMP_BUILD_GLTF_IMPORTER            ON  CACHE BOOL "" FORCE)
    set(ASSIMP_BUILD_FBX_IMPORTER             ON  CACHE BOOL "" FORCE)
    set(ASSIMP_BUILD_COLLADA_IMPORTER         ON  CACHE BOOL "" FORCE)
    set(BUILD_SHARED_LIBS                     OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(assimp
        GIT_REPOSITORY https://github.com/assimp/assimp.git
        GIT_TAG        v6.0.5
        GIT_SHALLOW    TRUE)

    # stb_image (image decoder — header-only, public domain). assimp leaves embedded
    # glTF textures COMPRESSED (aiTexture::mHeight==0, raw PNG/JPG bytes) and exposes
    # no decode API, so Story 2.2 needs its own decoder. FetchContent (not in-tree)
    # for the same drvfs/WSL reason as GLM/assimp; pinned by commit (stb is untagged).
    # The implementation is compiled into asset_loader.cpp via STB_IMAGE_IMPLEMENTATION.
    FetchContent_Declare(stb
        GIT_REPOSITORY https://github.com/nothings/stb.git
        GIT_TAG        31c1ad37456438565541f4919958214b6e762fb4  # recorded in extern/VENDORED.md
        GIT_SHALLOW    TRUE)

    FetchContent_MakeAvailable(glm assimp stb)

    add_reaper_extension(animviewer
        SOURCES
            src/plugin_main.cpp
            src/reaper_api.cpp
            src/viewer_window.cpp
            src/gl_loader.cpp
            src/renderer.cpp
            src/asset_loader.cpp
            src/console_log.cpp
    )
    # GLM + assimp as SYSTEM includes so their headers cannot trip /W3 /permissive-
    # (NFR-R5). assimp's generated config headers live in its BINARY dir.
    target_include_directories(animviewer SYSTEM PRIVATE
        "${glm_SOURCE_DIR}"
        "${assimp_SOURCE_DIR}/include"
        "${assimp_BINARY_DIR}/include"
        "${stb_SOURCE_DIR}")  # stb_image.h — SYSTEM so it can't trip /W3 /permissive-
    # comdlg32: GetOpenFileNameW — the native "choose a model" dialog (Story 2.1).
    target_link_libraries(animviewer PRIVATE assimp opengl32 gdi32 user32 comdlg32)
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
