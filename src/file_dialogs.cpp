// SPDX-License-Identifier: MIT
//
// See file_dialogs.h.

#include "file_dialogs.h"

#ifdef _WIN32

#include <filesystem>

#include <windows.h>
#include <shobjidl.h>  // IFileOpenDialog, IFileSaveDialog

namespace rav {
namespace {

// The picked item's file-system path (UTF-8).
bool ResultPath(IFileDialog* dlg, std::string& out_utf8)
{
    bool ok = false;
    IShellItem* item = nullptr;
    if (SUCCEEDED(dlg->GetResult(&item))) {
        PWSTR wide = nullptr;
        if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &wide)) && wide) {
            try {
                out_utf8 = std::filesystem::path(wide).u8string();
                ok = !out_utf8.empty();
            } catch (...) {
                ok = false;
            }
            CoTaskMemFree(wide);
        }
        item->Release();
    }
    return ok;
}

void SetUp(IFileDialog* dlg, const wchar_t* title, const FileDialogFilter& filter, FILEOPENDIALOGOPTIONS extra)
{
    DWORD opts = 0;
    if (SUCCEEDED(dlg->GetOptions(&opts))) dlg->SetOptions(opts | FOS_FORCEFILESYSTEM | extra);
    if (title) dlg->SetTitle(title);
    if (filter.pattern) {
        const COMDLG_FILTERSPEC specs[] = {{filter.label ? filter.label : L"", filter.pattern}, {L"All files", L"*.*"}};
        dlg->SetFileTypes(2, specs);
        dlg->SetFileTypeIndex(1);
    }
    if (filter.default_ext) dlg->SetDefaultExtension(filter.default_ext);
}

}  // namespace

bool PickOpenFile(HWND__* owner, const wchar_t* title, const FileDialogFilter& filter, std::string& out_utf8)
{
    // Balanced: S_OK and S_FALSE both need a CoUninitialize. RPC_E_CHANGED_MODE (the thread
    // is already multithreaded) is used as it is, without one.
    const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    bool ok = false;
    IFileOpenDialog* dlg = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) {
        SetUp(dlg, title, filter, FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST);
        if (SUCCEEDED(dlg->Show(owner))) ok = ResultPath(dlg, out_utf8);
        dlg->Release();
    }
    if (SUCCEEDED(init)) CoUninitialize();
    return ok;
}

bool PickSaveFile(HWND__* owner, const wchar_t* title, const FileDialogFilter& filter,
                  const std::string& default_name_utf8, std::string& out_utf8)
{
    // The single type plus "All files", as the open picker offers them.
    const FileDialogFilter filters[] = {filter, {L"All files", L"*.*", nullptr}};
    int                    index = 0;
    return PickSaveFile(owner, title, filters, filter.pattern ? 2 : 0, &index, default_name_utf8, out_utf8);
}

bool PickSaveFile(HWND__* owner, const wchar_t* title, const FileDialogFilter* filters, int filter_count,
                  int* type_index, const std::string& default_name_utf8, std::string& out_utf8)
{
    const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    bool ok = false;
    IFileSaveDialog* dlg = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) {
        DWORD opts = 0;
        if (SUCCEEDED(dlg->GetOptions(&opts)))
            dlg->SetOptions(opts | FOS_FORCEFILESYSTEM | FOS_OVERWRITEPROMPT | FOS_PATHMUSTEXIST);
        if (title) dlg->SetTitle(title);
        const int n = filters && filter_count > 0 ? (filter_count < 16 ? filter_count : 16) : 0;
        int       sel = type_index ? *type_index : 0;
        if (sel < 0 || sel >= n) sel = 0;
        if (n > 0) {
            COMDLG_FILTERSPEC specs[16];
            for (int i = 0; i < n; ++i)
                specs[i] = {filters[i].label ? filters[i].label : L"", filters[i].pattern ? filters[i].pattern : L"*.*"};
            dlg->SetFileTypes(static_cast<UINT>(n), specs);
            dlg->SetFileTypeIndex(static_cast<UINT>(sel + 1));  // 1-based
            const wchar_t* ext = filters[sel].default_ext ? filters[sel].default_ext : filters[0].default_ext;
            if (ext) dlg->SetDefaultExtension(ext);
        }
        if (!default_name_utf8.empty()) {
            try {
                const std::wstring wide = std::filesystem::u8path(default_name_utf8).wstring();
                dlg->SetFileName(wide.c_str());
            } catch (...) {
                // A name that does not convert: the picker starts empty.
            }
        }
        if (SUCCEEDED(dlg->Show(owner))) {
            ok = ResultPath(dlg, out_utf8);
            UINT chosen = 0;
            if (ok && type_index && n > 0 && SUCCEEDED(dlg->GetFileTypeIndex(&chosen)) && chosen >= 1 &&
                chosen <= static_cast<UINT>(n))
                *type_index = static_cast<int>(chosen) - 1;
        }
        dlg->Release();
    }
    if (SUCCEEDED(init)) CoUninitialize();
    return ok;
}

}  // namespace rav

#endif  // _WIN32
