# SPDX-License-Identifier: MIT
cmake_minimum_required(VERSION 3.20)

project(reaanimviewer
    VERSION 0.2.2
    DESCRIPTION "Reaper extension that loads glTF/FBX rigs synced to the transport"
    LANGUAGES CXX
)

# Link the MSVC runtime statically (/MT, /MTd in Debug) for EVERY target, including
# the FetchContent dependencies (assimp, imgui, zlib...). Without this the DLL imports
# MSVCP140.dll / VCRUNTIME140.dll and silently fails to load in REAPER on machines
# without the VC++ Redistributable. Set before FetchContent so dependencies inherit it
# (CMP0091 is NEW: our minimum is 3.20, assimp's is 3.22). assimp's own USE_STATIC_CRT
# option stays OFF on purpose: it injects raw /MT flags that would duplicate this.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")
include(ReaperPlugin)

# Version compiled into the DLL (src/self_update.cpp). Only a release build passes it
# (-DRAV_VERSION=X.Y.Z through build.bat). It is dropped from the cache right away,
# so any later configure without it (build.bat, IDE, manual cmake) builds "dev",
# and a dev build never self-updates.
if(DEFINED RAV_VERSION AND NOT RAV_VERSION STREQUAL "")
    set(RAV_VERSION_EFFECTIVE "${RAV_VERSION}")
else()
    set(RAV_VERSION_EFFECTIVE "dev")
endif()
unset(RAV_VERSION CACHE)

# Version SHOWN to the user (viewer menu, copied error log). Release builds show the
# release version; dev builds show "<last release>-dev+<commit>[-dirty]" so a report
# from a dev build still says what it was built from. RAV_VERSION_EFFECTIVE above
# stays "dev" for the self-update. build.bat reconfigures on every run, so the commit
# is current; without git (or outside a checkout) it is just "<last release>-dev".
if(RAV_VERSION_EFFECTIVE STREQUAL "dev")
    set(RAV_DISPLAY_VERSION "${PROJECT_VERSION}-dev")
    find_package(Git QUIET)
    if(GIT_FOUND)
        execute_process(COMMAND "${GIT_EXECUTABLE}" rev-parse --short=7 HEAD
                        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
                        OUTPUT_VARIABLE _rav_git_hash OUTPUT_STRIP_TRAILING_WHITESPACE
                        RESULT_VARIABLE _rav_git_rc ERROR_QUIET)
        if(_rav_git_rc EQUAL 0 AND NOT _rav_git_hash STREQUAL "")
            set(RAV_DISPLAY_VERSION "${RAV_DISPLAY_VERSION}+${_rav_git_hash}")
            # Tracked changes only: build output and untracked scratch files don't count.
            execute_process(COMMAND "${GIT_EXECUTABLE}" status --porcelain --untracked-files=no
                            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
                            OUTPUT_VARIABLE _rav_git_changes ERROR_QUIET)
            if(NOT _rav_git_changes STREQUAL "")
                set(RAV_DISPLAY_VERSION "${RAV_DISPLAY_VERSION}-dirty")
            endif()
        endif()
    endif()
else()
    set(RAV_DISPLAY_VERSION "${RAV_VERSION_EFFECTIVE}")
endif()
message(STATUS "ReaAnimViewer version: ${RAV_DISPLAY_VERSION}")

# configure_file only rewrites the header when the version changes, so switching
# between release and dev builds only recompiles its includer.
configure_file("${CMAKE_SOURCE_DIR}/src/rav_version.h.in"
               "${CMAKE_BINARY_DIR}/generated/rav_version.h" @ONLY)

# Story 10-2 -- the factory presets (presets/factory/*.ravpreset) compiled into the DLL
# as generated/factory_presets.h (src/preset_store.cpp). Editing one reconfigures.
include(FactoryPresets)
rav_generate_factory_presets("${CMAKE_SOURCE_DIR}/presets/factory"
                             "${CMAKE_SOURCE_DIR}/src/factory_presets.h.in"
                             "${CMAKE_BINARY_DIR}/generated/factory_presets.h")

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
        # NOT shallow: a shallow clone only fetches the branch tip, so checking out a
        # pinned commit hash fails ("unable to read tree") once upstream moves on.
        GIT_SHALLOW    FALSE)

    # Dear ImGui (the immediate-mode GUI behind ReaImGui) — vendored INTO the plugin
    # (Story 6.5.3 review revision) so the in-viewport tool UI is real widgets (sliders,
    # colour picker, buttons) drawn in our own GL context, with NO ReaImGui runtime
    # dependency for users. Pinned tag; recorded in extern/VENDORED.md.
    FetchContent_Declare(imgui
        GIT_REPOSITORY https://github.com/ocornut/imgui.git
        GIT_TAG        v1.91.5
        GIT_SHALLOW    TRUE)

    FetchContent_MakeAvailable(glm assimp stb imgui)

    # Dear ImGui has no CMakeLists of its own; compile the core + the Win32/OpenGL3
    # backends as a SEPARATE static lib so our strict /W3 /permissive- flags do NOT
    # apply to upstream code (it would warn). Our target includes its headers SYSTEM.
    add_library(imgui STATIC
        "${imgui_SOURCE_DIR}/imgui.cpp"
        "${imgui_SOURCE_DIR}/imgui_draw.cpp"
        "${imgui_SOURCE_DIR}/imgui_tables.cpp"
        "${imgui_SOURCE_DIR}/imgui_widgets.cpp"
        "${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp"
        "${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp")
    target_include_directories(imgui PUBLIC
        "${imgui_SOURCE_DIR}" "${imgui_SOURCE_DIR}/backends")
    target_link_libraries(imgui PUBLIC opengl32 gdi32 user32 dwmapi imm32)

    add_reaper_extension(animviewer
        SOURCES
            src/plugin_main.cpp
            src/reaper_api.cpp
            src/viewer_window.cpp
            src/viewer_fullscreen.cpp
            src/clip_list_ui.cpp
            src/clip_list_plan.cpp
            src/gl_loader.cpp
            src/renderer.cpp
            src/asset_loader.cpp
            src/gltf_skin.cpp
            src/console_log.cpp
            src/watchdog.cpp  # freeze audit: breadcrumbs, heartbeats, the freeze report
            src/pcm_source_anim.cpp
            src/asset_cache.cpp
            src/display_settings.cpp
            src/video_camera_seam.cpp
            src/video_render.cpp
            src/video_timeline.cpp
            src/self_update.cpp
            src/video_fx_host.cpp
            src/video_preview_lag.cpp
            src/video_fx_track.cpp
            src/video_gl_context.cpp
            src/gl_switch.cpp
            src/video_test_pattern.cpp
            src/video_camera.cpp
            src/video_shots.cpp
            src/ui_theme.cpp
            src/ui_flow.cpp
            src/ui_flow_math.cpp
            src/ui_fold_all.cpp
            src/video_view.cpp
            src/video_view_ui.cpp
            src/reaper_actions.cpp
            src/shortcuts.cpp
            src/shortcuts_ui.cpp
            src/shortcut_key_index.cpp
            src/texture_locate.cpp
            src/bone_events.cpp
            src/bone_sampling.cpp
            src/footstep_measure.cpp
            src/motion_physics.cpp
            src/auto_detect.cpp
            src/auto_types.cpp  # 10-9-0
            src/auto_types_legs.cpp
            src/auto_types_arms.cpp
            src/auto_types_jolt.cpp  # 10-9g
            src/auto_types_spin.cpp  # 10-9f
            src/detection_measure.cpp
            src/rule_record.cpp
            src/preset_store.cpp
            src/preset_menu_model.cpp
            src/file_dialogs.cpp
            src/role_map_store.cpp
            src/item_rules.cpp
            src/strip_view.cpp
            src/tagging_signal.cpp
            src/play_item_plan.cpp  # 11-7
            src/tagging_session.cpp
            src/tagging_view_ui.cpp
            src/skeleton_view.cpp
            src/event_list.cpp
            src/tag_markers.cpp
            src/item_clear_plan.cpp
            src/item_clear.cpp
            # Epic 13 (D20): the sound library and Populate.
            src/sound_record.cpp
            src/sound_project.cpp
            src/sound_entries.cpp
            src/sound_zones.cpp  # 13-62h
            src/normalize_presets.cpp  # 13-28
            src/sound_draw.cpp
            src/sound_switch.cpp
            src/library_tree_edit.cpp
            src/library_tree_nav.cpp
            src/sound_audition_plan.cpp
            src/media_db.cpp
            src/reaper_item_read.cpp  # 13-51: shared GUID / chunk readers
            src/sound_place_plan.cpp
            src/sound_place.cpp
            src/library_view_ui.cpp
            src/library_icons.cpp
            src/library_tree_ui.cpp
            src/library_node_ui.cpp
            src/zone_hit.cpp  # 13-62h (Lot E1)
            src/zone_wave_ui.cpp  # 13-62h (Lot E1)
            src/wave_draw_ui.cpp  # 13-71: the one waveform drawer
            src/sound_peaks.cpp
            src/reaper_view_keys.cpp
            src/view_nav.cpp  # 13-45: every timeline's scroll / zoom
            src/view_nav_live.cpp
            src/sound_audition.cpp
            src/library_media_ui.cpp
            src/library_media_plan.cpp
            src/tagging_populate_ui.cpp
            src/assign_view_model.cpp
            src/tagging_assign_ui.cpp
            src/float_ui_window.cpp
            src/sound_populate.cpp
            src/busy_progress_ui.cpp  # 13-59: the shared busy card of a long gesture
            src/sound_populate_plan.cpp
            src/sound_auto_assign.cpp
            src/sound_assign.cpp
            src/sound_follow.cpp
            src/sound_follow_plan.cpp
            src/tagging_attach_ui.cpp
            src/sound_attach.cpp
            src/sound_attach_plan.cpp
            src/sound_spread_selection.cpp  # 13-68
            src/sound_spread_plan.cpp       # 13-68 v2
            src/attach_manager_plan.cpp     # 13-68e
            src/attach_manager_ui.cpp       # 13-68e
            src/missing_sounds_ui.cpp       # spec missing-sounds-window
            src/sync_file_ui.cpp            # 13-77c
            src/library_sync_select.cpp     # 13-77c
            src/sync_rows.cpp               # 13-77c readability
            src/sound_sync_plan.cpp         # 13-35: Sync placed sounds
            src/sound_sync.cpp
            src/sound_sync_ui.cpp
            src/reaper_toast.cpp
            src/library_user_ui.cpp
            src/ravlib.cpp
            src/library_sync.cpp
            src/sound_tracks.cpp
            src/sound_tracks_plan.cpp
    )
    # Generated rav_version.h (see RAV_VERSION above).
    target_include_directories(animviewer PRIVATE "${CMAKE_BINARY_DIR}/generated")
    # GLM + assimp + Dear ImGui as SYSTEM includes so their headers cannot trip
    # /W3 /permissive- (NFR-R5). assimp's generated config headers live in its BINARY dir.
    target_include_directories(animviewer SYSTEM PRIVATE
        "${glm_SOURCE_DIR}"
        "${assimp_SOURCE_DIR}/include"
        "${assimp_BINARY_DIR}/include"
        # rapidjson ships in assimp's contrib tree; gltf_skin.cpp reuses it to parse the
        # glTF JSON directly (no new dependency). SYSTEM so it can't trip /W3 /permissive-.
        "${assimp_SOURCE_DIR}/contrib/rapidjson/include"
        "${stb_SOURCE_DIR}"      # stb_image.h — SYSTEM so it can't trip /W3 /permissive-
        "${imgui_SOURCE_DIR}"
        "${imgui_SOURCE_DIR}/backends")
    # comdlg32: GetOpenFileNameW — the native "choose a model" dialog (Story 2.1).
    # imgui: vendored in-viewport tool UI (Story 6.5.3 rev).
    # ole32 + uuid: IFileOpenDialog, the "Locate textures..." folder picker (issue #1); shell32:
    # SHCreateItemFromParsingName (a picker opened in a given folder, 13-50).
    target_link_libraries(animviewer PRIVATE assimp opengl32 gdi32 user32 comdlg32 ole32 shell32 uuid imgui)

    # The video FX (Epic 11, Story 11-1): rav_video_fx.clap, a thin CLAP shim that
    # hands REAPER's video frames to the frame-render API reaper_animviewer.dll
    # registers (src/video_fx_api.h). Header-only dependencies (CLAP 1.2.7, reaper-sdk
    # video headers, both vendored in extern/); no renderer, no assimp, no GL. Built
    # into the same output folder as the DLL (build/Release/), installed by build.bat
    # into UserPlugins\FX and published next to the DLL by a release.
    add_library(rav_video_fx SHARED src/fx/rav_video_fx_clap.cpp)
    set_target_properties(rav_video_fx PROPERTIES
        PREFIX ""
        OUTPUT_NAME "rav_video_fx"
        SUFFIX ".clap"
        CXX_STANDARD 17
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF)
    target_include_directories(rav_video_fx PRIVATE
        "${CMAKE_SOURCE_DIR}/src"
        "${CMAKE_SOURCE_DIR}/extern/reaper-sdk/sdk"
        "${CMAKE_BINARY_DIR}/generated")
    target_include_directories(rav_video_fx SYSTEM PRIVATE "${CMAKE_SOURCE_DIR}/extern/clap/include")
    if(MSVC)
        target_compile_options(rav_video_fx PRIVATE /W3 /permissive- /EHsc)
        target_compile_definitions(rav_video_fx PRIVATE
            _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
    endif()
    target_link_libraries(rav_video_fx PRIVATE user32)
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
