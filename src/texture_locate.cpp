// SPDX-License-Identifier: MIT
//
// See texture_locate.h.

#include "texture_locate.h"

#include <cstddef>
#include <deque>
#include <filesystem>
#include <system_error>
#include <utility>

#ifdef _WIN32
#include <windows.h>
#include <shobjidl.h>  // IFileOpenDialog
#endif

namespace rav {
namespace {

namespace fs = std::filesystem;

char LowerAscii(char c)
{
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

std::string LowerAscii(const std::string& s)
{
    std::string out = s;
    for (char& c : out) c = LowerAscii(c);
    return out;
}

// Safety cap on the entries one search visits, so picking a drive root cannot hang the
// viewer for minutes (depth 4 of C:\ is a lot of folders).
constexpr std::size_t kMaxEntriesVisited = 200000;

}  // namespace

std::string TextureFileName(const std::string& stored)
{
    const std::size_t sep = stored.find_last_of("\\/");
    return sep == std::string::npos ? stored : stored.substr(sep + 1);
}

std::vector<std::string> TextureCandidates(const std::string& stored)
{
    std::vector<std::string> out{stored};
    const std::string name = TextureFileName(stored);
    if (!name.empty() && name != stored) out.push_back(name);
    return out;
}

bool SameFileNameNoCase(const std::string& a, const std::string& b)
{
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (LowerAscii(a[i]) != LowerAscii(b[i])) return false;
    return true;
}

LocateResult LocateTextures(const std::string& dest_dir,
                            const std::vector<std::string>& missing_names,
                            const std::string& search_root)
{
    LocateResult res;
    try {
        const fs::path dest = fs::u8path(dest_dir);
        const fs::path root = fs::u8path(search_root);
        std::error_code ec;

        // The names still to find (distinct, case-insensitive), minus those already present
        // next to the model (never overwritten, so not searched).
        std::vector<std::string> wanted;       // as the loader spells them
        std::vector<std::string> wanted_low;   // lower-cased, same order
        std::vector<fs::path>    found;        // same order; empty = not found yet
        for (const std::string& name : missing_names) {
            if (name.empty()) continue;
            const std::string low = LowerAscii(name);
            bool dup = false;
            for (const std::string& w : wanted_low) dup = dup || w == low;
            for (const std::string& a : res.already_present) dup = dup || SameFileNameNoCase(a, name);
            if (dup) continue;
            if (fs::exists(dest / fs::u8path(name), ec)) {
                res.already_present.push_back(name);
                continue;
            }
            wanted.push_back(name);
            wanted_low.push_back(low);
            found.emplace_back();
        }

        // Breadth-first walk: the shallowest match wins.
        std::size_t left = wanted.size();
        std::size_t visited = 0;
        std::deque<std::pair<fs::path, int>> queue;
        if (left > 0 && fs::is_directory(root, ec)) queue.emplace_back(root, 0);
        while (!queue.empty() && left > 0 && visited < kMaxEntriesVisited) {
            const fs::path dir   = queue.front().first;
            const int      depth = queue.front().second;
            queue.pop_front();
            fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec);
            if (ec) { ec.clear(); continue; }
            for (; it != fs::directory_iterator() && left > 0 && visited < kMaxEntriesVisited;
                 it.increment(ec)) {
                if (ec) break;
                ++visited;
                const fs::directory_entry& e = *it;
                std::error_code sec;
                const fs::file_status st = e.symlink_status(sec);
                if (sec) continue;
                if (fs::is_directory(st)) {
                    if (depth < kLocateMaxDepth) queue.emplace_back(e.path(), depth + 1);
                    continue;
                }
                if (!fs::is_regular_file(st)) continue;
                const std::string low = LowerAscii(e.path().filename().u8string());
                for (std::size_t i = 0; i < wanted.size(); ++i) {
                    if (found[i].empty() && wanted_low[i] == low) {
                        found[i] = e.path();
                        --left;
                        break;
                    }
                }
            }
            ec.clear();
        }

        // Copy what was found, under the loader's spelling. copy_options::none fails on an
        // existing file, so a file that appeared meanwhile is never overwritten either.
        for (std::size_t i = 0; i < wanted.size(); ++i) {
            if (found[i].empty()) {
                res.still_missing.push_back(wanted[i]);
                continue;
            }
            const fs::path to = dest / fs::u8path(wanted[i]);
            std::error_code cec;
            if (fs::exists(to, cec)) {
                res.already_present.push_back(wanted[i]);
            } else if (fs::copy_file(found[i], to, fs::copy_options::none, cec) && !cec) {
                res.copied.push_back(wanted[i]);
            } else {
                // A copy that failed partway may leave a partial file, which would later
                // read as "already present": remove it (it did not exist before the copy).
                std::error_code rec;
                fs::remove(to, rec);
                res.failed.push_back(wanted[i]);
            }
        }
    } catch (...) {
        // u8path on a malformed string, or bad_alloc: report what was not handled as missing.
        for (const std::string& name : missing_names) {
            bool listed = false;
            for (const auto* v : {&res.copied, &res.already_present, &res.failed, &res.still_missing})
                for (const std::string& n : *v) listed = listed || SameFileNameNoCase(n, name);
            if (!listed && !name.empty()) {
                try { res.still_missing.push_back(name); } catch (...) {}
            }
        }
    }
    return res;
}

#ifdef _WIN32
bool PickFolder(HWND__* owner, std::string& out_utf8)
{
    // Balanced: S_OK and S_FALSE both need a CoUninitialize. RPC_E_CHANGED_MODE (the thread
    // is already multithreaded) is used as it is, without one.
    const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    bool ok = false;
    IFileOpenDialog* dlg = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_PPV_ARGS(&dlg)))) {
        DWORD opts = 0;
        if (SUCCEEDED(dlg->GetOptions(&opts)))
            dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
        dlg->SetTitle(L"Folder holding the missing texture files");
        if (SUCCEEDED(dlg->Show(owner))) {
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
        }
        dlg->Release();
    }
    if (SUCCEEDED(init)) CoUninitialize();
    return ok;
}
#endif

}  // namespace rav
