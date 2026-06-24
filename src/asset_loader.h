// SPDX-License-Identifier: MIT
//
// The assimp boundary (D5/D6). LoadAsset reads a glTF/GLB/FBX/Collada file, parses
// it into the D1 `Asset` model, and uploads its geometry to the GPU. This header
// pulls in NO assimp or GL headers — consumers see only `scene.h` types; all
// parse-exception risk is contained in asset_loader.cpp's single try/catch (AR18).
//
// PRECONDITION: a current GL context. LoadAsset creates VBOs/IBOs, so it must be
// called on the main thread after the viewer's WGL context is made current.

#pragma once

#ifdef _WIN32

#include <optional>
#include <string>

#include "scene.h"

namespace rav {

// Why a category enum and not just a bool: the console funnel (D7) names the
// failure class, and the viewport degrades the same way for every class — log one
// line, keep the window alive (AR16/AR17). Ok is unused in the failure return.
enum class LoadErrorCategory {
    Ok,
    FileNotFound,
    ParseFailed,
    UnsupportedFormat,
    GpuUploadFailed,
    OutOfMemory,
    Unknown,
};

// Human-readable category label for the console line.
const char* LoadErrorCategoryName(LoadErrorCategory category);

struct LoadResult {
    std::optional<Asset> asset;
    LoadErrorCategory    category = LoadErrorCategory::Ok;
    std::string          detail;   // human-readable, goes into the console line
};

// No-throw across this boundary (assimp throws std::exception-derived internally;
// nothing escapes). On success: result.asset has a value, category == Ok. On
// failure: result.asset is empty and category/detail describe the cause.
LoadResult LoadAsset(const std::string& path);

}  // namespace rav

#endif  // _WIN32
