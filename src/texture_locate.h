// SPDX-License-Identifier: MIT
//
// Missing external textures (GitHub issue #1). An FBX stores its texture paths from the
// exporter's own folder tree (e.g. "..\..\sourceimages\T.png"); when REAPER copies only
// the .fbx into the project media folder, those files are not found. The loader also
// looks for a texture by FILE NAME next to the model (TextureFileName), so copying the
// files beside the .fbx fixes it. LocateTextures does that copy from a folder the user
// picks (the "Locate textures..." button of the viewer's notice).
//
// TextureFileName and LocateTextures are portable std::filesystem code (host-tested in
// tests/texture_locate_test.cpp); only PickFolder is Win32. Paths are UTF-8. No-throw.

#pragma once

#include <string>
#include <vector>

#ifdef _WIN32
struct HWND__;  // HWND under <windows.h> STRICT (the default), without pulling it in here
#endif

namespace rav {

// The file name part of a stored texture path, split on BOTH '\' and '/' (an FBX written
// on Windows keeps backslashes whatever the platform). "" when the path ends in a
// separator or is empty.
std::string TextureFileName(const std::string& stored);

// The paths ResolveTexture tries for an external texture, in order (relative to the
// model folder): the stored path, then its file name alone when that is non-empty and
// different (issue #1: a copy beside the .fbx is found whatever folder the exporter wrote).
std::vector<std::string> TextureCandidates(const std::string& stored);

// ASCII case-insensitive equality of two file names (non-ASCII bytes compare exactly).
bool SameFileNameNoCase(const std::string& a, const std::string& b);

// How deep LocateTextures looks under the picked folder: files of the folder itself are
// depth 0, files of its sub-folders depth 1, ... up to this depth.
constexpr int kLocateMaxDepth = 4;

struct LocateResult {
    std::vector<std::string> copied;           // file names now copied next to the model
    std::vector<std::string> already_present;  // a file of that name was there: not overwritten
    std::vector<std::string> failed;           // found, but the copy failed (e.g. read-only folder)
    std::vector<std::string> still_missing;    // not found under search_root
};

// For each name in missing_names, finds a file of that name (case-insensitive) under
// search_root (breadth-first, depth <= kLocateMaxDepth: the shallowest match wins) and
// copies it into dest_dir under that name. Never overwrites a file: a name already present
// in dest_dir is reported in already_present and not searched. Symbolic-link folders are
// not followed. Empty names are skipped and names differing only in case count once;
// every other name lands in exactly one list.
LocateResult LocateTextures(const std::string& dest_dir,
                            const std::vector<std::string>& missing_names,
                            const std::string& search_root);

#ifdef _WIN32
// The native folder picker (IFileOpenDialog, FOS_PICKFOLDERS), modal to `owner`. True with
// the picked folder in out_utf8; false when cancelled or unavailable. Runs a modal loop:
// call it from the window procedure, never inside the ImGui frame.
bool PickFolder(HWND__* owner, std::string& out_utf8);
#endif

}  // namespace rav
