# fbx_dump: a Linux/host command-line twin of the plugin's detection dump.
# Compiles the plugin's own load + sampling sources (src/, unchanged) against a
# static assimp built from the same vendored source and importer set as the plugin.
#
#   cmake -S tools/fbx_dump -B <build dir> -DCMAKE_BUILD_TYPE=Release
#   cmake --build <build dir> -j8
#   <build dir>/fbx_dump <in.fbx> <out.csv>
cmake_minimum_required(VERSION 3.22)
project(fbx_dump CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

set(RAV_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")
set(RAV_DEPS "${RAV_ROOT}/build/_deps" CACHE PATH "Folder holding assimp-src, glm-src, stb-src")

# Same importer set as the plugin's CMakeLists.txt (glTF + FBX + Collada, no export).
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
set(ASSIMP_BUILD_ZLIB                     ON  CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS                     OFF CACHE BOOL "" FORCE)
add_subdirectory("${RAV_DEPS}/assimp-src" assimp EXCLUDE_FROM_ALL)

add_executable(fbx_dump
    fbx_dump.cpp
    stubs/plugin_load.cpp
    stubs/plugin_gltf_skin.cpp
    stubs/plugin_bone_sampling.cpp
    stubs/linux_stubs.cpp
    "${RAV_ROOT}/src/texture_locate.cpp")
# stubs/ first: it supplies <windows.h> and <gl/GL.h>.
target_include_directories(fbx_dump PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/stubs"
    "${RAV_ROOT}/src")
target_include_directories(fbx_dump SYSTEM PRIVATE
    "${RAV_DEPS}/glm-src"
    "${RAV_DEPS}/stb-src"
    "${RAV_DEPS}/assimp-src/contrib/rapidjson/include")
target_link_libraries(fbx_dump PRIVATE assimp)
