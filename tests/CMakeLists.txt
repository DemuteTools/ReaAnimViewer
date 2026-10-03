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
