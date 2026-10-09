// SPDX-License-Identifier: MIT
//
// The plugin's load + sampling sources are wrapped in #ifdef _WIN32 (the plugin is
// Windows-only). To compile them unchanged on Linux, every system / third-party header
// they use is included HERE first, without _WIN32 (so libstdc++, assimp, glm, stb and
// rapidjson keep their Linux configuration; their include guards then skip the later
// includes), and only then is _WIN32 defined for the plugin's own code.
#pragma once

#ifdef _WIN32
#error "win32_prelude.h is for non-Windows hosts"
#endif

#include <algorithm>
#include <array>
#include <cctype>
#include <climits>
#include <cmath>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <assimp/Importer.hpp>
#include <assimp/config.h>
#include <assimp/postprocess.h>
#include <assimp/scene.h>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_inverse.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <rapidjson/document.h>

#include <stb_image.h>  // declarations only; asset_loader.cpp compiles the implementation

#include "windows.h"  // the stub (stubs/), for __declspec/__stdcall in stb's UTF-8 path

#define _WIN32 1
