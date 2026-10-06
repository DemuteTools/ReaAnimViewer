// SPDX-License-Identifier: MIT
//
// Native Windows file pickers (IFileOpenDialog / IFileSaveDialog), for the Tagging view's
// preset Import / Export (story 10-3b). Paths are UTF-8. Each one runs a modal loop: call
// it from the window procedure, never inside the ImGui frame. No-throw.

#pragma once

#ifdef _WIN32

#include <string>

struct HWND__;  // HWND under <windows.h> STRICT (the default), without pulling it in here

namespace rav {

// A file type the picker offers, e.g. {L"ReaAnimViewer preset", L"*.ravpreset", L"ravpreset"}.
struct FileDialogFilter {
    const wchar_t* label;
    const wchar_t* pattern;
    const wchar_t* default_ext;  // without the dot
};

// Picks an existing file. True with its path in out_utf8; false when cancelled or unavailable.
bool PickOpenFile(HWND__* owner, const wchar_t* title, const FileDialogFilter& filter, std::string& out_utf8);

// Picks where to write a file, `default_name_utf8` proposed (the picker asks before
// replacing a file). True with its path in out_utf8; false when cancelled or unavailable.
bool PickSaveFile(HWND__* owner, const wchar_t* title, const FileDialogFilter& filter,
                  const std::string& default_name_utf8, std::string& out_utf8);

// The same with several file types (story 10-3f): `type_index` (0-based) is the one
// preselected on entry and the one the user left selected on return (when true).
bool PickSaveFile(HWND__* owner, const wchar_t* title, const FileDialogFilter* filters, int filter_count,
                  int* type_index, const std::string& default_name_utf8, std::string& out_utf8);

}  // namespace rav

#endif  // _WIN32
