// SPDX-License-Identifier: MIT
//
// See self_update.h. Windows allows RENAMING a loaded DLL but not overwriting it,
// so a swap is: copy the Toolkit copy to <dll>.new and check it, rename our file to
// <dll>.old, move <dll>.new in place. The leftovers are deleted at the next startup
// (or by the launcher).

#include "self_update.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <optional>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

#include "console_log.h"
#include "rav_version.h"

namespace rav {
namespace {

constexpr wchar_t kToolkitDir[]    = L"\\Scripts\\ReaAnimViewer\\Scripts\\";
constexpr wchar_t kLauncherName[]  = L"RAV_Launcher.lua";
// The video FX (Story 11-1): installed in UserPlugins\FX, shipped next to the DLL.
constexpr wchar_t kClapName[]      = L"rav_video_fx.clap";

// Embedded in every build so the version of a DLL FILE can be read without loading
// it (ReadEmbeddedVersion). Referenced at runtime (OwnVersion) so the linker keeps it.
const char kVersionMarker[] = "RAV_VERSION_MARKER:" RAV_VERSION_STRING;

enum class Ownership { Unknown, ReaPack, NotReaPack };

REAPER_PLUGIN_HINSTANCE g_module      = nullptr;
void* (*g_get_func)(const char*)      = nullptr;
int (*g_register)(const char*, void*) = nullptr;
bool      g_timer_registered = false;
bool      g_swapped          = false;
Ownership g_ownership        = Ownership::Unknown;

std::string OwnVersion()
{
    const std::string marker(kVersionMarker);
    return marker.substr(marker.find(':') + 1);
}

std::wstring Widen(const std::string& utf8)
{
    if (utf8.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
    if (n <= 0) return {};
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, out.data(), n);
    out.resize(static_cast<size_t>(n - 1));
    return out;
}

std::string Narrow(const std::wstring& wide)
{
    if (wide.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, out.data(), n, nullptr, nullptr);
    out.resize(static_cast<size_t>(n - 1));
    return out;
}

bool FileExists(const std::wstring& path)
{
    const DWORD attr = GetFileAttributesW(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

// Backslashes, absolute, no trailing separator: comparable with another path.
std::wstring NormalizePath(std::wstring path)
{
    std::replace(path.begin(), path.end(), L'/', L'\\');
    const DWORD n = GetFullPathNameW(path.c_str(), 0, nullptr, nullptr);
    if (n > 0) {
        std::wstring full(n, L'\0');
        const DWORD written = GetFullPathNameW(path.c_str(), n, full.data(), nullptr);
        if (written > 0 && written < n) path = full.substr(0, written);
    }
    while (path.size() > 3 && path.back() == L'\\') path.pop_back();
    return path;
}

std::wstring ResourcePath()
{
    return NormalizePath(Widen(GetResourcePath()));
}

std::wstring OwnPath()
{
    std::vector<wchar_t> buf(MAX_PATH);
    for (;;) {
        const DWORD n = GetModuleFileNameW(g_module, buf.data(), static_cast<DWORD>(buf.size()));
        if (n == 0) return {};
        if (n < buf.size()) return std::wstring(buf.data(), n);
        buf.resize(buf.size() * 2);
    }
}

std::wstring FileName(const std::wstring& path)
{
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

// <ResourcePath>\UserPlugins\<our file name>, the path REAPER and ReaPack use for
// our DLL. Empty when we were loaded from anywhere else (then we never touch it).
std::wstring UserPluginsPath()
{
    const std::wstring own = OwnPath();
    if (own.empty()) return {};
    const std::wstring expected = ResourcePath() + L"\\UserPlugins\\" + FileName(own);
    if (CompareStringOrdinal(NormalizePath(own).c_str(), -1, expected.c_str(), -1, TRUE)
        != CSTR_EQUAL) {
        LogInfo("self-update: not loaded from UserPlugins, disabled");
        return {};
    }
    return expected;
}

// ---- Versions --------------------------------------------------------------

// Digit runs compared as numbers: "beta2" < "beta10".
int NaturalCompare(const std::string& a, const std::string& b)
{
    auto digit = [](char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; };
    size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        if (digit(a[i]) && digit(b[j])) {
            size_t i2 = i, j2 = j;
            while (i2 < a.size() && digit(a[i2])) ++i2;
            while (j2 < b.size() && digit(b[j2])) ++j2;
            const unsigned long long x = std::strtoull(a.substr(i, i2 - i).c_str(), nullptr, 10);
            const unsigned long long y = std::strtoull(b.substr(j, j2 - j).c_str(), nullptr, 10);
            if (x != y) return x < y ? -1 : 1;
            i = i2;
            j = j2;
        } else {
            if (a[i] != b[j]) return a[i] < b[j] ? -1 : 1;
            ++i;
            ++j;
        }
    }
    if (i < a.size()) return 1;
    if (j < b.size()) return -1;
    return 0;
}

// Numeric segments compared as numbers; "0.2.0" > "0.2.0-beta". nullopt when a
// version cannot be parsed (the caller then does nothing).
std::optional<int> CompareVersionsLocal(const std::string& a, const std::string& b)
{
    struct Parsed { std::vector<unsigned long long> nums; std::string suffix; };
    auto parse = [](const std::string& v) -> std::optional<Parsed> {
        Parsed p;
        const size_t dash = v.find('-');
        const std::string core = v.substr(0, dash);
        if (dash != std::string::npos) p.suffix = v.substr(dash + 1);
        std::stringstream ss(core);
        std::string seg;
        while (std::getline(ss, seg, '.')) {
            if (seg.empty() || seg.find_first_not_of("0123456789") != std::string::npos) {
                return std::nullopt;
            }
            p.nums.push_back(std::strtoull(seg.c_str(), nullptr, 10));
        }
        if (p.nums.empty()) return std::nullopt;
        return p;
    };
    const auto pa = parse(a);
    const auto pb = parse(b);
    if (!pa || !pb) return std::nullopt;

    const size_t count = (std::max)(pa->nums.size(), pb->nums.size());
    for (size_t i = 0; i < count; ++i) {
        const unsigned long long x = i < pa->nums.size() ? pa->nums[i] : 0;
        const unsigned long long y = i < pb->nums.size() ? pb->nums[i] : 0;
        if (x != y) return x < y ? -1 : 1;
    }
    if (pa->suffix == pb->suffix) return 0;
    if (pa->suffix.empty()) return 1;
    if (pb->suffix.empty()) return -1;
    return NaturalCompare(pa->suffix, pb->suffix);
}

// ReaPack's comparator only while ReaPack is surely loaded (first tick). At quit it
// may already be unloaded, and calling into it would crash: local comparator only.
std::optional<int> CompareVersions(const std::string& a, const std::string& b, bool allow_reapack)
{
    using CompareFn = int (*)(const char*, const char*, char*, int);
    if (allow_reapack && g_get_func) {
        if (auto fn = reinterpret_cast<CompareFn>(g_get_func("ReaPack_CompareVersions"))) {
            char error[256] = {};
            const int result = fn(a.c_str(), b.c_str(), error, sizeof(error));
            if (error[0] == '\0') return result < 0 ? -1 : (result > 0 ? 1 : 0);
        }
    }
    return CompareVersionsLocal(a, b);
}

// @version of the launcher the Toolkit installed next to the DLL copy. A release
// keeps it equal to the version of the DLL shipped with it.
std::optional<std::string> ToolkitCopyVersion(const std::wstring& resource)
{
    std::ifstream file(resource + kToolkitDir + kLauncherName);
    if (!file) return std::nullopt;
    const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    std::smatch m;
    static const std::regex kVersion(R"(@version[ \t]+(\S+))");
    if (!std::regex_search(text, m, kVersion)) return std::nullopt;
    return m[1].str();
}

// Version compiled into a DLL file (kVersionMarker), read from its bytes.
std::optional<std::string> ReadEmbeddedVersion(const std::wstring& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file) return std::nullopt;
    const std::string bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    // Built in two parts so this needle is not itself a full marker in our binary.
    const std::string needle = std::string("RAV_VERSION_") + "MARKER:";
    for (size_t pos = bytes.find(needle); pos != std::string::npos;
         pos = bytes.find(needle, pos + 1)) {
        const size_t start = pos + needle.size();
        const size_t end   = bytes.find('\0', start);
        if (end == std::string::npos || end == start || end - start > 64) continue;
        return bytes.substr(start, end - start);
    }
    return std::nullopt;
}

// A complete PE image (a truncated download fails to map). Loaded as a resource
// image: no code runs, nothing is initialized.
bool IsCompleteImage(const std::wstring& path)
{
    HMODULE image = LoadLibraryExW(path.c_str(), nullptr,
        LOAD_LIBRARY_AS_IMAGE_RESOURCE | LOAD_LIBRARY_AS_DATAFILE);
    if (!image) return false;
    FreeLibrary(image);
    return true;
}

// ---- ReaPack ownership -----------------------------------------------------

Ownership CheckOwnership(const std::wstring& dll)
{
    using GetOwnerFn  = void* (*)(const char*, char*, int);
    using FreeEntryFn = bool (*)(void*);
    auto get_owner  = reinterpret_cast<GetOwnerFn>(g_get_func("ReaPack_GetOwner"));
    auto free_entry = reinterpret_cast<FreeEntryFn>(g_get_func("ReaPack_FreeEntry"));
    if (!get_owner) return Ownership::NotReaPack;  // ReaPack not installed

    char error[256] = {};
    void* entry = get_owner(Narrow(dll).c_str(), error, sizeof(error));
    if (entry) {
        if (free_entry) free_entry(entry);
        return Ownership::ReaPack;
    }
    if (error[0] != '\0') {
        // ReaPack answers "no" with an error too ("the file is not owned by any
        // package entry", reapack src/api_package.cpp). 0.2.x took it for "cannot
        // tell" below and never updated wherever ReaPack is installed.
        if (std::strstr(error, "not owned")) return Ownership::NotReaPack;
        // ReaPack could not tell (e.g. its registry is busy): assume it owns us.
        LogWarn("self-update: ReaPack_GetOwner failed: %s", error);
        return Ownership::Unknown;
    }
    return Ownership::NotReaPack;
}

// ---- Files -----------------------------------------------------------------

void DeleteLeftovers(const std::wstring& dll)
{
    const std::wstring dir = dll.substr(0, dll.find_last_of(L'\\') + 1);
    for (const wchar_t* pattern : {L".old*", L".new"}) {
        WIN32_FIND_DATAW data;
        HANDLE find = FindFirstFileW((dll + pattern).c_str(), &data);
        if (find == INVALID_HANDLE_VALUE) continue;
        do {
            DeleteFileW((dir + data.cFileName).c_str());  // may still be locked: ignored
        } while (FindNextFileW(find, &data));
        FindClose(find);
    }
}

// <ResourcePath>\UserPlugins\FX\rav_video_fx.clap, where REAPER scans CLAP plug-ins
// and where ReaPack's "extension" type puts FX/rav_video_fx.clap.
std::wstring ClapInstallPath(const std::wstring& resource)
{
    return resource + L"\\UserPlugins\\FX\\" + kClapName;
}

// The video FX next to the Toolkit's DLL copy. The Toolkit keeps the package's
// FX/ sub-folder, or flattens it: both are accepted.
std::wstring ToolkitClapPath(const std::wstring& resource)
{
    const std::wstring nested = resource + kToolkitDir + L"FX\\" + kClapName;
    if (FileExists(nested)) return nested;
    const std::wstring flat = resource + kToolkitDir + kClapName;
    if (FileExists(flat)) return flat;
    return {};
}

// Brings the installed video FX to `version` from the Toolkit copy, the same way as
// the DLL (a loaded .clap can be renamed, not overwritten). The DLL and the CLAP
// share the frame API version, so they must move together; on any failure the CLAP
// stays as it is (the FX then shows a "do not match" console line) and the next
// SyncClapWithDll call (next quit or startup) tries again. Never throws.
void SwapClapToVersion(const std::wstring& resource, const std::string& version)
{
    const std::wstring candidate = ToolkitClapPath(resource);
    if (candidate.empty()) return;
    const std::wstring target = ClapInstallPath(resource);
    if (FileExists(target) && ReadEmbeddedVersion(target) == version) return;  // already there
    if (ReadEmbeddedVersion(candidate) != version) {
        LogWarn("self-update: the Toolkit video FX is not version %s, left as is", version.c_str());
        return;
    }

    CreateDirectoryW((resource + L"\\UserPlugins\\FX").c_str(), nullptr);  // may already exist
    const std::wstring staged = target + L".new";
    if (!CopyFileW(candidate.c_str(), staged.c_str(), FALSE)) {
        LogWarn("self-update: could not stage the new video FX (error %lu)", GetLastError());
        return;
    }
    if (ReadEmbeddedVersion(staged) != version || !IsCompleteImage(staged)) {
        LogWarn("self-update: Toolkit video FX copy is incomplete");
        DeleteFileW(staged.c_str());
        return;
    }
    std::wstring old;
    if (FileExists(target)) {
        for (int i = 0; i < 10 && old.empty(); ++i) {
            const std::wstring name = target + L".old" + (i == 0 ? L"" : std::to_wstring(i));
            if (MoveFileExW(target.c_str(), name.c_str(), MOVEFILE_REPLACE_EXISTING)) old = name;
        }
        if (old.empty()) {
            LogWarn("self-update: could not rename the current video FX (error %lu)", GetLastError());
            DeleteFileW(staged.c_str());
            return;
        }
    }
    if (!MoveFileExW(staged.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        LogWarn("self-update: could not move the new video FX in place (error %lu), restoring",
                GetLastError());
        if (!old.empty() && !MoveFileExW(old.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING)) {
            CopyFileW(old.c_str(), target.c_str(), FALSE);
        }
        DeleteFileW(staged.c_str());
        return;
    }
    LogInfo("self-update: video FX -> %s", version.c_str());
}

// Keeps the video FX at the version of the DLL that REAPER will load next (the file
// in UserPlugins, which a swap may just have replaced). Only for a Toolkit install
// (ReaPack updates both files itself) of a release build.
void SyncClapWithDll()
{
    if (g_ownership != Ownership::NotReaPack) return;
    if (ToolkitClapPath(ResourcePath()).empty()) return;  // no Toolkit copy: nothing to sync
    const std::wstring dll = UserPluginsPath();
    if (dll.empty()) return;
    const auto version = ReadEmbeddedVersion(dll);
    if (!version || *version == "dev") return;
    SwapClapToVersion(ResourcePath(), *version);
}

// Replaces our DLL with the Toolkit copy when that copy is newer. Returns the
// installed version, or nullopt when nothing was done.
std::optional<std::string> SwapIfNewer(bool allow_reapack_compare)
{
    if (g_swapped || g_ownership != Ownership::NotReaPack) return std::nullopt;

    const std::wstring dll = UserPluginsPath();
    if (dll.empty()) return std::nullopt;
    const std::wstring resource  = ResourcePath();
    const std::wstring candidate = resource + kToolkitDir + FileName(dll);
    if (!FileExists(candidate)) return std::nullopt;

    const auto version = ToolkitCopyVersion(resource);
    if (!version) return std::nullopt;
    const auto cmp = CompareVersions(*version, OwnVersion(), allow_reapack_compare);
    if (!cmp || *cmp <= 0) return std::nullopt;

    // Another REAPER instance already swapped it in.
    if (ReadEmbeddedVersion(dll) == version) return std::nullopt;

    // Stage and check the copy first: the Toolkit may still be writing it, or the
    // download may have been cut, or the launcher may not match the DLL.
    const std::wstring staged = dll + L".new";
    if (!CopyFileW(candidate.c_str(), staged.c_str(), FALSE)) {
        LogWarn("self-update: could not stage the new DLL (error %lu)", GetLastError());
        return std::nullopt;
    }
    if (ReadEmbeddedVersion(staged) != version || !IsCompleteImage(staged)) {
        LogWarn("self-update: Toolkit copy is incomplete or not version %s", version->c_str());
        DeleteFileW(staged.c_str());
        return std::nullopt;
    }

    // A previous .old may still be locked by a REAPER that has not exited yet.
    std::wstring old;
    for (int i = 0; i < 10 && old.empty(); ++i) {
        const std::wstring name = dll + L".old" + (i == 0 ? L"" : std::to_wstring(i));
        if (MoveFileExW(dll.c_str(), name.c_str(), MOVEFILE_REPLACE_EXISTING)) old = name;
    }
    if (old.empty()) {
        LogWarn("self-update: could not rename the current DLL (error %lu)", GetLastError());
        DeleteFileW(staged.c_str());
        return std::nullopt;
    }
    if (!MoveFileExW(staged.c_str(), dll.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        LogWarn("self-update: could not move the new DLL in place (error %lu), restoring",
                GetLastError());
        if (!MoveFileExW(old.c_str(), dll.c_str(), MOVEFILE_REPLACE_EXISTING)
            && !CopyFileW(old.c_str(), dll.c_str(), FALSE)) {
            LogError("self-update: restore failed (error %lu), the extension will be missing "
                     "at next start: run RAV_Launcher.lua to reinstall it", GetLastError());
        }
        DeleteFileW(staged.c_str());
        return std::nullopt;
    }
    g_swapped = true;
    LogInfo("self-update: %s -> %s", OwnVersion().c_str(), version->c_str());
    return version;
}

// ---- Entry points ----------------------------------------------------------

// One-shot: runs on the first tick, once every extension (ReaPack included,
// which loads after us) is loaded.
void OnFirstTick()
{
    try {
        if (g_timer_registered && g_register) {
            g_register("-timer", (void*)&OnFirstTick);
            g_timer_registered = false;
        }
        const std::wstring dll = UserPluginsPath();
        if (dll.empty()) return;
        DeleteLeftovers(dll);
        DeleteLeftovers(ClapInstallPath(ResourcePath()));
        g_ownership = CheckOwnership(dll);
        const auto version = SwapIfNewer(true);
        SyncClapWithDll();
        if (version) {
            const std::string text = "ReaAnimViewer was updated to " + *version
                + ".\n\nRestart REAPER to use the new version.";
            ShowMessageBox(text.c_str(), "ReaAnimViewer", 0);
        }
    } catch (...) {
        LogWarn("self-update: startup check failed");
    }
}

}  // namespace

void SelfUpdateInit(REAPER_PLUGIN_HINSTANCE module,
                    void* (*get_func)(const char*),
                    int (*register_fn)(const char*, void*))
{
    try {
        if (OwnVersion() == "dev") return;  // never on dev builds
        g_module   = module;
        g_get_func = get_func;
        g_register = register_fn;
        if (g_get_func && g_register && g_register("timer", (void*)&OnFirstTick)) {
            g_timer_registered = true;
        } else {
            LogWarn("self-update: could not register the startup timer, disabled");
        }
    } catch (...) {
        LogWarn("self-update: init failed");
    }
}

void SelfUpdateOnQuit()
{
    try {
        if (g_timer_registered && g_register) {
            // Quit before the first tick: ownership unknown, so do nothing.
            g_register("-timer", (void*)&OnFirstTick);
            g_timer_registered = false;
            return;
        }
        SwapIfNewer(false);
        SyncClapWithDll();
    } catch (...) {
        LogWarn("self-update: quit swap failed");
    }
}

}  // namespace rav
