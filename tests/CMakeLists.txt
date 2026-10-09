# Host tests of RAV's pure, REAPER-free helpers. A project of its own (the main build
# needs Windows and REAPER); any platform, any C++17 compiler:
#   cmake -S tests -B build-tests && cmake --build build-tests && ctest --test-dir build-tests
cmake_minimum_required(VERSION 3.20)
project(rav_host_tests LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

enable_testing()

add_executable(video_shot_timing_test video_shot_timing_test.cpp)
target_include_directories(video_shot_timing_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
add_test(NAME video_shot_timing COMMAND video_shot_timing_test)

add_executable(video_preview_lag_test video_preview_lag_test.cpp ../src/video_preview_lag.cpp)
target_include_directories(video_preview_lag_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
find_package(Threads REQUIRED)
target_link_libraries(video_preview_lag_test PRIVATE Threads::Threads)
add_test(NAME video_preview_lag COMMAND video_preview_lag_test)

# Spec 11-fb-3 -- the viewer's shortcut rules (src/shortcuts.h).
add_executable(shortcuts_test shortcuts_test.cpp)
target_include_directories(shortcuts_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
add_test(NAME shortcuts COMMAND shortcuts_test)

# Spec 11-fb-13 -- the Output size written into REAPER's render format (src/render_format_size.h).
add_executable(render_format_size_test render_format_size_test.cpp)
target_include_directories(render_format_size_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
add_test(NAME render_format_size COMMAND render_format_size_test)

# Spec 11-fb-15 -- shots belong to clips (src/video_clip_shots.h).
add_executable(video_clip_shots_test video_clip_shots_test.cpp)
target_include_directories(video_clip_shots_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
add_test(NAME video_clip_shots COMMAND video_clip_shots_test)

# Spec 11-fb-16 -- Render Current's source switch (src/render_current.h).
add_executable(render_current_test render_current_test.cpp)
target_include_directories(render_current_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
add_test(NAME render_current COMMAND render_current_test)

# Spec 10-3c -- the skeleton overlay's projection, pick and segments (src/skeleton_overlay.h).
add_executable(skeleton_overlay_test skeleton_overlay_test.cpp)
target_include_directories(skeleton_overlay_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
add_test(NAME skeleton_overlay COMMAND skeleton_overlay_test)

# Issue #1 -- "Locate textures...": file-name extraction + search/copy (src/texture_locate.h).
add_executable(texture_locate_test texture_locate_test.cpp ../src/texture_locate.cpp)
target_include_directories(texture_locate_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
add_test(NAME texture_locate COMMAND texture_locate_test)

# Spec 10-0 -- the bone-event rule engine and role mapping (src/bone_events.h, bone_roles.h).
add_executable(bone_events_test bone_events_test.cpp ../src/bone_events.cpp ../src/footstep_measure.cpp)
target_include_directories(bone_events_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
add_test(NAME bone_events COMMAND bone_events_test)

# Spike 10-8a -- foot events from body physics (src/motion_physics.h).
add_executable(motion_physics_test motion_physics_test.cpp ../src/motion_physics.cpp ../src/bone_events.cpp)
target_include_directories(motion_physics_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
add_test(NAME motion_physics COMMAND motion_physics_test)

# Story 10-8b -- auto detection: foot events from the physics as blocks of the item (src/auto_detect.h).
add_executable(auto_detect_test auto_detect_test.cpp ../src/auto_detect.cpp ../src/motion_physics.cpp ../src/bone_events.cpp
               ../src/rule_record.cpp ../src/event_list.cpp ../src/tagging_signal.cpp)
target_include_directories(auto_detect_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
add_test(NAME auto_detect COMMAND auto_detect_test)

add_executable(bone_roles_test bone_roles_test.cpp ../src/bone_events.cpp)
target_include_directories(bone_roles_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
add_test(NAME bone_roles COMMAND bone_roles_test)

# Spec 10-7a -- "REF <label>" reference markers and their CSV lines (src/ref_labels.h).
add_executable(ref_labels_test ref_labels_test.cpp ../src/bone_events.cpp ../src/role_map_store.cpp
               ../src/rule_record.cpp)
target_include_directories(ref_labels_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
add_test(NAME ref_labels COMMAND ref_labels_test)

# Spec 10-0 -- the offline evaluator: runs the footstep detection on RAV_detection_dump CSV
# files and prints the measure action's report (a tool, not a test):
#   detection_eval [--param=value ...] [--json] file.csv...
# Spike 10-7a step 1: --single measures Speed, Stillness and Relative drop alone per labelled bone.
# Spike 10-8a: --physics scores the foot events of motion_physics.h against the labelled REFs.
add_executable(detection_eval detection_eval.cpp ../src/bone_events.cpp ../src/footstep_measure.cpp
               ../src/tagging_signal.cpp ../src/motion_physics.cpp)
target_include_directories(detection_eval PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)

# Story 10-2 -- the rules record, presets and the role mapping (src/rule_record.h,
# preset_store.h, role_map_store.h). The factory presets are embedded as in the DLL.
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../cmake")
include(FactoryPresets)
rav_generate_factory_presets("${CMAKE_CURRENT_SOURCE_DIR}/../presets/factory"
                             "${CMAKE_CURRENT_SOURCE_DIR}/../src/factory_presets.h.in"
                             "${CMAKE_CURRENT_BINARY_DIR}/generated/factory_presets.h")

add_executable(rule_record_test rule_record_test.cpp ../src/rule_record.cpp)
target_include_directories(rule_record_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
target_compile_definitions(rule_record_test PRIVATE RAV_TEST_DATA="${CMAKE_CURRENT_SOURCE_DIR}/data")
add_test(NAME rule_record COMMAND rule_record_test)

add_executable(preset_store_test preset_store_test.cpp ../src/rule_record.cpp ../src/preset_store.cpp)
target_include_directories(preset_store_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src
                           ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_compile_definitions(preset_store_test PRIVATE RAV_TEST_DATA="${CMAKE_CURRENT_SOURCE_DIR}/data")
add_test(NAME preset_store COMMAND preset_store_test)

add_executable(role_map_store_test role_map_store_test.cpp ../src/role_map_store.cpp ../src/rule_record.cpp)
target_include_directories(role_map_store_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
target_compile_definitions(role_map_store_test PRIVATE RAV_TEST_DATA="${CMAKE_CURRENT_SOURCE_DIR}/data")
add_test(NAME role_map_store COMMAND role_map_store_test)

# Story 10-3 -- the Tagging view's units, menus and clip mapping (src/tagging_signal.h).
add_executable(tagging_signal_test tagging_signal_test.cpp ../src/tagging_signal.cpp)
target_include_directories(tagging_signal_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
add_test(NAME tagging_signal COMMAND tagging_signal_test)

# Story 10-4 -- the item's event list, Apply's plan, the first pass (src/event_list.h).
add_executable(event_list_test event_list_test.cpp ../src/event_list.cpp ../src/rule_record.cpp ../src/tagging_signal.cpp)
target_include_directories(event_list_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
add_test(NAME event_list COMMAND event_list_test)

# Story 10-3b -- the Tagging view's preset menu logic (src/preset_menu_model.h).
add_executable(preset_menu_model_test preset_menu_model_test.cpp ../src/preset_menu_model.cpp)
target_include_directories(preset_menu_model_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
add_test(NAME preset_menu_model COMMAND preset_menu_model_test)
