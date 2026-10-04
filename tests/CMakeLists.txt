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

# Issue #1 -- "Locate textures...": file-name extraction + search/copy (src/texture_locate.h).
add_executable(texture_locate_test texture_locate_test.cpp ../src/texture_locate.cpp)
target_include_directories(texture_locate_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
add_test(NAME texture_locate COMMAND texture_locate_test)

# Spec 10-0 -- the bone-event rule engine and role mapping (src/bone_events.h, bone_roles.h).
add_executable(bone_events_test bone_events_test.cpp ../src/bone_events.cpp ../src/footstep_measure.cpp)
target_include_directories(bone_events_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
add_test(NAME bone_events COMMAND bone_events_test)

add_executable(bone_roles_test bone_roles_test.cpp ../src/bone_events.cpp)
target_include_directories(bone_roles_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
add_test(NAME bone_roles COMMAND bone_roles_test)

# Spec 10-0 -- the offline evaluator: runs the footstep detection on RAV_detection_dump CSV
# files and prints the measure action's report (a tool, not a test):
#   detection_eval [--param=value ...] [--json] file.csv...
add_executable(detection_eval detection_eval.cpp ../src/bone_events.cpp ../src/footstep_measure.cpp)
target_include_directories(detection_eval PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
