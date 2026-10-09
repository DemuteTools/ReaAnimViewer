// SPDX-License-Identifier: MIT
//
// Linux stand-in for <windows.h>: just enough for the plugin's GL-free load path
// (asset_loader.cpp, gltf_skin.cpp, bone_sampling.cpp) to compile under the _WIN32
// guard. Nothing here is a working Win32 API.
#pragma once

#ifndef APIENTRY
#define APIENTRY
#endif
#ifndef WINAPI
#define WINAPI
#endif
#ifndef __declspec
#define __declspec(x)
#endif
#ifndef __stdcall
#define __stdcall
#endif

#include <cstdio>
#include <cwchar>

// stb_image's STBI_WINDOWS_UTF8 path (asset_loader.cpp turns it on): byte copies,
// so ASCII paths still open; non-ASCII texture paths are irrelevant to a track dump.
extern "C" FILE* _wfopen(const wchar_t* name, const wchar_t* mode);
