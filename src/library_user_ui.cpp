// SPDX-License-Identifier: MIT
//
// See library_user_ui.h. Spec 13-11 (decisions 2, 5, 8 to 13).
//
// 13-77a: a library is linked to a .ravlib of the user folder or not; nothing writes a .ravlib but an explicit
// gesture (Save to a file, Import, Save it again, Export, the sync of 13-77b/c): no autosave.
//
// Threads: the UI thread draws, runs the pickers and the first-sight pass from this module's REAPER timer
// and applies a published Locate result; the Locate worker (StartLocateJob) runs RelocateFolder or
// LocateUnderRoot through the folder cache (media_db.h), the only recursive folder scans: none runs
// on the UI thread, and Cancel / unload stop them (13-50, 13-5 review). Listing RAV's own Libraries folder (non-recursive) is done on the UI
// thread on first draw, after this module writes a file, and on Refresh (decision 2).

#include "library_user_ui.h"
#include "watchdog.h"  // freeze audit: breadcrumbs and scopes
#include "busy_progress_ui.h"  // 13-59: the busy card of a long gesture

#ifdef _WIN32

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <ctime>
#include <set>
#include <sstream>
#include <string>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <imgui.h>

#include <windows.h>  // GetAsyncKeyState (no re-check during a drag)

#include "file_dialogs.h"
#include "library_icons.h"  // 13-77a: DrawLinkGlyph, DrawToolGlyphAt
#include "library_tree_edit.h"
#include "library_media_ui.h"  // freeze audit #2: MediaReachAsk / MediaReachLast (the probe worker)
#include "library_sync.h"
#include "library_tree_ui.h"
#include "library_view_ui.h"
#include "media_db.h"
#include "ravlib.h"
#include "sound_peaks.h"
#include "sound_place.h"
#include "sound_place_plan.h"
#include "sound_populate.h"
#include "sound_project.h"
#include "sound_record.h"
#include "sound_tracks.h"
#include "texture_locate.h"
#include "timed_join.h"  // freeze audit #8
#include "ui_flow.h"
#include "ui_flow_math.h"
#include "ui_theme.h"

namespace rav {

namespace {

namespace fs = std::filesystem;

using InsertTrackAtIndexFn = void (*)(int idx, bool want_defaults);
using SetMediaTrackInfoValueFn = bool (*)(MediaTrack* tr, const char* parm, double v);
using GetParentTrackFn = MediaTrack* (*)(MediaTrack* track);
using CreateFromFileFn = PCM_source* (*)(const char* filename);
using GetMainHwndFn = HWND (*)();
using CountTakesFn = int (*)(MediaItem* item);
using GetMediaItemTakeFn = MediaItem_Take* (*)(MediaItem* item, int tk);

CountTakesFn             f_count_takes = nullptr;  // 13-24
GetMediaItemTakeFn       f_get_take = nullptr;
InsertTrackAtIndexFn     f_insert_track = nullptr;
SetMediaTrackInfoValueFn f_set_track = nullptr;
GetParentTrackFn         f_parent = nullptr;
CreateFromFileFn         f_create = nullptr;
GetMainHwndFn            f_main_hwnd = nullptr;

int (*g_register)(const char*, void*) = nullptr;
bool g_timer_on = false;


const FileDialogFilter kRavlibFilter = {L"ReaAnimViewer User Library", L"*.ravlib", L"ravlib"};
const FileDialogFilter kAudioFilter = {L"Audio files", L"*.wav;*.aif;*.aiff;*.flac;*.mp3;*.ogg;*.opus;*.wv;*.m4a",
                                       L"wav"};

// One .ravlib of the user folder.
struct UserFile {
    std::string file;  // file name
    std::string uid;   // its library's uid ("" = unreadable)
    std::string name;
    uint32_t    color = 0;
    SoundStore  store;  // parsed
    std::string text;   // 13-77a: as read (the first-sight pass compares it with the project's copy)
};

enum class PickKind { None, Import, Export, Reveal, Locate, LocateFolder, Relink, SaveFile, SaveAgain };

struct LocateJob {
    std::thread                                       worker;
    std::atomic<bool>                                 running{false};
    std::atomic<bool>                                 done{false};
    std::atomic<bool>                                 cancel{false};  // 13-38: set at unload
    std::mutex                                        mu;
    std::vector<std::pair<AssetIdentity, std::string>> found;  // under mu
    int                                               missing = 0;
    int                                               ambiguous = 0;  // under mu
    AssetIdentity                                     picked;  // the file the user picked (UI thread)
    ReaProject*                                       proj = nullptr;  // the project it was started in (UI thread)
    // 13-50 review: the picked Sound's file was renamed: that Sound takes `relink_now` whole (UI thread).
    bool          relink = false;
    std::string   relink_lib, relink_node;
    AssetIdentity relink_old, relink_now;
    // Spec missing-sounds-window (UI thread): a folder search (its root, for the toast), the picked Sound's name
    // and file (a Locate's toast), and what was found by the Sound's name only (under mu).
    bool                     search = false;
    std::string              root;
    std::string              self_name, self_file;
    std::string              self_lib, self_node, self_was;  // the picked Sound and its file's path before
    std::vector<FoundByName> by_name;  // under mu
};

// Spec missing-sounds-window: an older item Sound that never remembered its file, looked for by its name.
struct ByNameTarget {
    std::string lib_uid, node_uid, name;
};

struct State {
    // Folder listing.
    bool                  listed = false;
    std::vector<UserFile> files;
    // Pending picker (run by the timer, never inside the ImGui frame).
    PickKind    pick = PickKind::None;
    std::string pick_lib;
    std::string pick_node;
    bool        pick_rename = false;  // Locate: a file of another name relinks the Sound (13-50 review)
    bool        in_timer = false;
    // 13-77a: the (project, file + library uid) pairs the first-sight pass has looked at (SavedKey).
    std::set<std::pair<ReaProject*, std::string>> sighted;
    // 13-77a: the folder listing as LinkedFileStateOf reads it (file, library uid).
    std::vector<FolderRavlib> folder;
    int         last_state = -1;
    ReaProject* last_proj = nullptr;
    // Missing caches (decision 10).
    const MediaDbIndex*                          idx_seen = nullptr;
    std::unordered_map<std::string, std::string> resolved;  // asset key -> path ("" = missing)
    // The project's items (lower GUID -> its active take's file when on disk, "" when offline).
    std::unordered_map<std::string, std::string> live_files;
    int                                          live_state = -1;
    // Freeze audit #2: the items' files are checked on the probe worker, never on the UI thread. The items read at the
    // last project change (REAPER API only), their set's signature (from_items survives a change that left it alone),
    // and whether an answer is still awaited (the next frame builds live_files again from what came back).
    struct LiveItem {
        std::string guid, path;
    };
    std::vector<LiveItem> live_items;
    size_t                live_sig = 0;
    bool                  live_dirty = false;
    // A missing file found in another project item playing it (asset key -> path, "" = none): read
    // again when the project changes.
    std::unordered_map<std::string, std::string> from_items;
    ReaProject*                                  live_proj = nullptr;
    // Spec missing-sounds-window: the folder searched last; the Sounds found by their name only (lib + "\n" + node).
    std::string           last_searched;
    std::set<std::string> by_name_found;
    // Lands in "New folder..." field.
    bool        new_folder = false;
    std::string new_folder_node;
    char        buf[256] = {};
};

State     g;
LocateJob g_job;

std::string LowerAscii(std::string s)
{
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string ResourcePath()
{
    const char* p = GetResourcePath();
    return p ? std::string(p) : std::string();
}

fs::path UserFolder()
{
    return fs::u8path(ResourcePath()) / "ReaAnimViewer" / "Libraries";
}

std::string UserFolderText()
{
    return UserFolder().u8string();
}

std::string BaseNameOf(const std::string& path)
{
    const size_t s = path.find_last_of("/\\");
    return s == std::string::npos ? path : path.substr(s + 1);
}

bool ReadText(const fs::path& p, std::string* out)
{
    std::ifstream in(p, std::ios::binary);
    if (!in) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    *out = ss.str();
    return true;
}

// Temp file + rename.
bool WriteText(const fs::path& p, const std::string& text)
{
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    fs::path tmp = p;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out << text;
        if (!out.good()) return false;
    }
    fs::rename(tmp, p, ec);
    if (ec) {
        fs::remove(tmp, ec);
        return false;
    }
    return true;
}

int64_t FileSize(const std::string& path)
{
    if (path.empty()) return -1;
    std::error_code ec;
    const fs::path  p = fs::u8path(path);
    if (!fs::is_regular_file(p, ec)) return -1;
    const auto sz = fs::file_size(p, ec);
    return ec ? -1 : static_cast<int64_t>(sz);
}

void ListUserFolder()
{
    g.listed = true;
    g.files.clear();
    std::error_code ec;
    fs::directory_iterator it(UserFolder(), ec), end;
    for (; !ec && it != end; it.increment(ec)) {
        std::error_code e2;
        if (!it->is_regular_file(e2)) continue;
        const fs::path& p = it->path();
        if (LowerAscii(p.extension().u8string()) != kRavlibExt) continue;
        UserFile f;
        f.file = p.filename().u8string();
        std::string text;
        if (ReadText(p, &text) && ParseRavlib(text, &f.store)) {
            f.text = text;
            const SoundLibrary& lib = f.store.libraries.front();
            f.uid = lib.uid;
            f.name = lib.name;
            f.color = lib.color;
        } else {
            f.name = p.stem().u8string();
        }
        g.files.push_back(std::move(f));
    }
    std::sort(g.files.begin(), g.files.end(),
              [](const UserFile& a, const UserFile& b) { return LowerAscii(a.name) < LowerAscii(b.name); });
    g.folder.clear();
    for (const UserFile& f : g.files) g.folder.push_back({f.file, f.uid});
}

std::vector<std::string> FolderFileNames()
{
    std::vector<std::string> out;
    for (const UserFile& f : g.files) out.push_back(f.file);
    return out;
}

void ShowToastNoUndo(const std::string& text)
{
    ShowLibraryToast(text, false);
}

// One gesture = one undo point + a toast offering Undo.
bool Gesture(const std::string& name, const std::function<bool(SoundStore&)>& edit, const std::string& toast = {})
{
    const std::string desc = "RAV: " + name;
    if (!ModifySoundStore(nullptr, desc.c_str(), edit)) return false;
    ShowLibraryToast(toast.empty() ? name : toast, true, desc);
    return true;
}

// ---- Missing (decision 10) ----------------------------------------------------------------------

std::string AssetKey(const AssetIdentity& a)
{
    char buf[64];
    std::snprintf(buf, sizeof buf, "|%lld|%.4f|", static_cast<long long>(a.size), a.length);
    return LowerAscii(a.name) + buf + a.last_path;
}

std::string ProjectFolder();  // below

// Freeze audit #2 (a project on Google Drive or a share that stalls): a file's presence is asked of the probe worker
// and the last answer kept. "Not asked yet" counts as present (never "missing" for a moment), an answer older than
// kRecheckS is asked again (at most that often, and not while the mouse button is down: a drag edits the project at
// every step) without losing the old one meanwhile.
enum class Disk { Yes, No, Unknown };
constexpr double kRecheckS = 3.0;

Disk DiskState(const std::string& path, long long* size)
{
    bool      ok = false;
    long long sz = -1;
    double    age = 0.0;
    const bool known = MediaReachLast(path, &ok, &sz, &age);
    if (!known || age > kRecheckS) {
        g.live_dirty = true;  // an answer is awaited (or the drag ends and it is asked)
        if ((GetAsyncKeyState(VK_LBUTTON) & 0x8000) == 0) MediaReachAsk(path, /*urgent=*/false);
    }
    if (!known) return Disk::Unknown;
    if (size) *size = sz;
    return ok ? Disk::Yes : Disk::No;
}

void RebuildLiveFiles()
{
    g.live_dirty = false;
    g.live_files.clear();
    for (const State::LiveItem& li : g.live_items) {
        std::string path = li.path;
        if (!path.empty() && DiskState(path, nullptr) == Disk::No) path.clear();
        g.live_files[li.guid] = path;
    }
    if (MediaReachInflight() > 0) g.live_dirty = true;
}

void RefreshCaches()
{
    const std::shared_ptr<const MediaDbIndex> idx = MediaDbGet();
    if (idx.get() != g.idx_seen) {
        g.idx_seen = idx.get();
        g.resolved.clear();
    }
    ReaProject* proj = EnumProjects(-1, nullptr, 0);
    const int   st = GetProjectStateChangeCount(proj);
    if (st != g.live_state || proj != g.live_proj) {
        g.live_state = st;
        g.live_proj = proj;
        g.live_items.clear();
        const std::string dir = ProjectFolder();
        const int         n = CountMediaItems(proj);
        RAV_QUIET_SCOPE("RefreshCaches items", n);  // freeze audit (REAPER API only: the disk is asked of the worker)
        size_t sig = static_cast<size_t>(n);
        for (int i = 0; i < n; ++i) {
            MediaItem* it = GetMediaItem(proj, i);
            if (!it) continue;
            MediaItem_Take* t = GetActiveTake(it);
            PCM_source*     src = t ? GetMediaItemTake_Source(t) : nullptr;
            const char*     fn = src ? src->GetFileName() : nullptr;
            State::LiveItem li;
            li.path = fn && fn[0] ? AbsoluteMediaPath(fn, dir) : std::string();
            li.guid = LowerAscii(ItemGuidOf(it));
            sig = sig * 1099511628211ULL ^ std::hash<std::string>()(li.path);
            g.live_items.push_back(std::move(li));
        }
        // The files other items play only change with the set of items: kept when the edit left it alone.
        if (sig != g.live_sig) {
            g.live_sig = sig;
            g.from_items.clear();
        }
        RebuildLiveFiles();
    } else if (g.live_dirty) {
        MediaReachPoll();  // the worker's answers
        RebuildLiveFiles();
    }
}

// The file of a project item that plays the same file (same name, same size when known, still on
// disk), "" when none: a Sound whose own item was deleted finds its audio in a copy kept elsewhere.
// `*settled` false: a candidate's file was not answered yet; it is taken as present for now (not missing) and the
// result is not kept, so the next frame decides again.
std::string FileFromProjectItems(const AssetIdentity& a, bool* settled)
{
    *settled = true;
    if (a.name.empty()) return {};
    RAV_QUIET_SCOPE("FileFromProjectItems", a.name);  // freeze audit
    const std::string name = LowerAscii(a.name);
    const std::string dir = ProjectFolder();
    const int         n = CountMediaItems(nullptr);
    std::string       maybe;
    for (int i = 0; i < n; ++i) {
        MediaItem*      it = GetMediaItem(nullptr, i);
        MediaItem_Take* t = it ? GetActiveTake(it) : nullptr;
        PCM_source*     src = t ? GetMediaItemTake_Source(t) : nullptr;
        const char*     fn = src ? src->GetFileName() : nullptr;
        if (!fn || !fn[0] || LowerAscii(BaseNameOf(fn)) != name) continue;
        const std::string path = AbsoluteMediaPath(fn, dir);
        long long         size = -1;
        const Disk        d = DiskState(path, &size);
        if (d == Disk::No) continue;
        if (d == Disk::Unknown) {
            *settled = false;
            if (maybe.empty()) maybe = path;
            continue;
        }
        if (a.size >= 0 && size != a.size) continue;
        *settled = true;
        return path;
    }
    return maybe;  // "" when no candidate; else the first one not answered yet
}

// `*unsettled` (review M3): the only candidate is a same-name item file whose existence is not answered yet. It is
// neither "this file" (it may be another folder's file of that name, or an offline one) nor "missing": "" with the flag.
std::string ResolvedPath(const AssetIdentity& a, bool* unsettled = nullptr)
{
    const std::string key = AssetKey(a);
    std::string       path;
    const auto        it = g.resolved.find(key);
    if (it != g.resolved.end()) {
        path = it->second;
    } else {
        const std::shared_ptr<const MediaDbIndex> idx = MediaDbGet();
        // Review M5: the sizes come from the probe worker's answers, never from a stat on this thread. A file not
        // answered yet makes the whole resolution unsettled: not kept, asked again next frame.
        bool        unknown = false;
        const auto  size_of = [&unknown](const std::string& p) -> int64_t {
            long long  sz = -1;
            const Disk d = DiskState(p, &sz);
            if (d == Disk::Unknown) unknown = true;
            return d == Disk::Yes ? static_cast<int64_t>(sz) : -1;
        };
        path = ResolveAssetPath(a, size_of, idx.get());
        if (unknown) {
            if (unsettled) *unsettled = true;
            return {};
        }
        g.resolved[key] = path;
    }
    if (!path.empty()) return path;
    // Not at its last path nor in the Media Explorer: another item of the project that plays it.
    const auto hit = g.from_items.find(key);
    if (hit != g.from_items.end()) return hit->second;
    bool        settled = true;
    std::string found = FileFromProjectItems(a, &settled);
    if (settled) g.from_items[key] = found;
    else {
        if (unsettled) *unsettled = true;
        return {};
    }
    return found;
}

// Where a File Sound's audio is on disk ("" = missing): its file (last path, Media Explorer, another
// item playing it), else the file its own project item plays. The missing check, the panel, Edit and
// audition all ask this, so a Sound is never "not missing" in one and "Source not found" in another.
std::string SoundFilePath(const SoundSource& s, bool* unsettled = nullptr)
{
    if (unsettled) *unsettled = false;
    RefreshCaches();
    std::string path = ResolvedPath(s.asset, unsettled);
    if (path.empty() && !s.item_guid.empty()) {
        const auto it = g.live_files.find(LowerAscii(s.item_guid));
        if (it != g.live_files.end()) {
            path = it->second;
            if (!path.empty() && unsettled) *unsettled = false;
        }
    }
    return path;
}

// The resolver Populate and the auditions read a moved file with (ReadSoundMedia): a file still being checked is
// waited for a moment (the probe answers a metadata check in milliseconds); if it stays unsettled the sound is not
// available yet, never another same-name file.
std::string SoundFilePathForPlay(const SoundSource& s)
{
    bool        unsettled = false;
    std::string path = SoundFilePath(s, &unsettled);
    for (int i = 0; unsettled && path.empty() && i < 60; ++i) {  // at most ~300 ms, only for a never-asked candidate
        Sleep(5);
        MediaReachPoll();
        path = SoundFilePath(s, &unsettled);
    }
    return path;
}

void ApplyHooks()
{
    static bool resolver_set = false;
    if (!resolver_set) {
        resolver_set = true;
        SetFileSoundResolver([](const SoundSource& s) { return SoundFilePathForPlay(s); });
    }
    const LibraryTreeHooks& h = GetLibraryTreeHooks();
    if (h.is_missing) return;
    LibraryTreeHooks n = h;
    n.is_missing = [](const SoundNode& s) { return SoundNodeMissing(s); };  // spec missing-sounds-window: the one rule
    SetLibraryTreeHooks(n);
}

// ---- The user folder's files (13-77a: no autosave) ----------------------------------------------

fs::path FileOf(const std::string& name)
{
    return UserFolder() / fs::u8path(name);
}

// The first-sight key: REAPER reuses a project tab's ReaProject* when another project is opened in it,
// so the project's file name is part of the key (a newly opened project is a first sight).
std::pair<ReaProject*, std::string> SavedKey(ReaProject* proj, const std::string& lib_uid)
{
    char fn[4096] = {};
    if (!proj) proj = EnumProjects(-1, fn, sizeof fn);
    else
        for (int i = 0; ReaProject* p = EnumProjects(i, fn, sizeof fn); ++i)
            if (p == proj) break;
    return std::make_pair(proj, std::string(fn) + "\n" + lib_uid);
}

// The uid of the library in a .ravlib file ("" = none / unreadable).
std::string UidInFile(const fs::path& p)
{
    std::string text;
    SoundStore  s;
    if (!ReadText(p, &text) || !ParseRavlib(text, &s)) return {};
    return s.libraries.front().uid;
}

// RavlibFileName, also skipping a name whose file exists: the folder list compares ASCII case only,
// while the file system also folds accented capitals ("Ete" / "ete" with accents = one file).
std::string FreeOnDisk(const std::string& lib_name, std::vector<std::string> taken)
{
    std::string     name = RavlibFileName(lib_name, taken);
    std::error_code ec;
    while (fs::exists(FileOf(name), ec)) {
        taken.push_back(name);
        name = RavlibFileName(lib_name, taken);
    }
    return name;
}

// A file name for `lib_uid` that no other library uses (store, folder and disk).
std::string FreeFileName(const SoundStore& store, const std::string& lib_uid, const std::string& lib_name)
{
    std::vector<std::string> taken = FolderFileNames();
    for (const SoundLibrary& l : store.libraries)
        if (l.uid != lib_uid && !l.ravlib_path.empty()) taken.push_back(l.ravlib_path);
    return FreeOnDisk(lib_name, taken);
}

// ---- Writing a .ravlib (13-77a: only on an explicit gesture) ------------------------------------------

int64_t NowS()
{
    return static_cast<int64_t>(std::time(nullptr));
}

// The open project's file name (the `by` of the files this project writes).
std::string ProjectFileName()
{
    char fn[4096] = {};
    EnumProjects(-1, fn, sizeof fn);
    const std::string b = BaseNameOf(fn);
    return b.empty() ? std::string("unsaved project") : b;
}

// Review 13-77a M1: file names compare the way Windows does (accented capitals too): the host's built-in fold stands in
// for the file system's upcase table; the DLL uses the system's own.
std::string Win32FoldFileName(const std::string& s)
{
    if (s.empty()) return s;
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return BuiltinFoldFileName(s);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), &w[0], n);
    CharUpperBuffW(&w[0], static_cast<DWORD>(n));
    const int m = WideCharToMultiByte(CP_UTF8, 0, w.data(), n, nullptr, 0, nullptr, nullptr);
    if (m <= 0) return BuiltinFoldFileName(s);
    std::string out(static_cast<size_t>(m), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), n, &out[0], m, nullptr, nullptr);
    return out;
}

// First sight (decision 2): a linked library with no common version whose own text equals its file takes the file's
// as the common version, silently (no undo point). Once per (project, library) per session: a file that differs
// stays without a common version (13-77b shows each difference as changed on both sides).
void FirstSightPass(ReaProject* proj)
{
    if (ResourcePath().empty()) return;
    if (!g.listed) ListUserFolder();
    std::vector<FolderRavlibText> files;
    for (const UserFile& f : g.files) files.push_back({f.file, f.uid, f.text});
    const std::vector<std::pair<std::string, std::string>> todo =  // lib uid, file text (the decision is ravlib.cpp's)
        FirstSightTargets(GetSoundStore(proj), files,
                          [&](const std::string& uid) { return g.sighted.insert(SavedKey(proj, uid)).second; });
    if (todo.empty()) return;
    const int64_t now = NowS();
    ModifySoundStore(proj, nullptr, [&](SoundStore& s) {
        bool any = false;
        for (const auto& t : todo) any = AdoptFileAsBase(s, t.first, t.second, now) || any;
        return any;
    });
}

// ---- Locate (decision 11) -----------------------------------------------------------------------

void JoinLocate()
{
    if (g_job.worker.joinable()) g_job.worker.join();
    g_job.running = false;
}

// The identities of every missing sound whose file is known (a File Sound's, an item Sound's remembered one).
std::vector<AssetIdentity> MissingIdentities()
{
    std::vector<AssetIdentity> out;
    RefreshCaches();
    for (const SoundLibrary& lib : GetSoundStore(nullptr).libraries)
        for (const SoundNode& n : lib.nodes) {
            MissingWhy why = MissingWhy::FileNotFound;
            if (SoundNodeMissing(n, &why) && why != MissingWhy::ItemGoneFileUnknown) out.push_back(n.sound.source.asset);
        }
    return out;
}

// Spec missing-sounds-window: the missing item Sounds that never remembered their file (looked for by name).
std::vector<ByNameTarget> MissingByName(const std::string& skip_lib = {}, const std::string& skip_node = {})
{
    std::vector<ByNameTarget> out;
    RefreshCaches();
    for (const SoundLibrary& lib : GetSoundStore(nullptr).libraries)
        for (const SoundNode& n : lib.nodes) {
            MissingWhy why = MissingWhy::FileNotFound;
            if (lib.uid == skip_lib && n.uid == skip_node) continue;
            if (!SoundNodeMissing(n, &why) || why != MissingWhy::ItemGoneFileUnknown || n.name.empty()) continue;
            out.push_back({lib.uid, n.uid, n.name});
        }
    return out;
}

// The folder a path is in ("" = none).
std::string FolderOf(const std::string& path)
{
    const size_t cut = path.find_last_of("/\\");
    return cut == std::string::npos ? std::string() : path.substr(0, cut);
}

// One search of the Locate worker: one result per missing identity, through the folder lister;
// returns early when `cancel` is set.
using FolderLister = std::function<std::vector<DbFile>(const std::string&)>;
using LocateSearch = std::function<std::vector<LookupResult>(const std::vector<AssetIdentity>& missing,
                                                             const FolderLister& list_folder,
                                                             const std::atomic<bool>* cancel)>;

// 13-50 (13-38 review): the one Locate worker, for the single-file Locate and "Locate in a folder".
// `picked` = the file the user picked (ApplyLocated gets it; empty last_path = none); `self` = the
// picked Sound, put first in the found pairs (null = a folder locate). False when nothing started.
// Spec missing-sounds-window: `by_name` (older item Sounds with no remembered file) are looked for under
// `by_name_dir` (and its subfolders) by their name; each folder is listed once per run.
bool StartLocateJob(const std::vector<AssetIdentity>& missing, const AssetIdentity& picked,
                    const AssetIdentity* self, const LocateSearch& search,
                    const std::vector<ByNameTarget>& by_name = {}, const std::string& by_name_dir = {})
{
    if (g_job.running) {
        ShowToastNoUndo("Already locating...");
        return false;
    }
    JoinLocate();
    g_job.missing = static_cast<int>(missing.size());
    g_job.picked = picked;
    g_job.proj = EnumProjects(-1, nullptr, 0);
    g_job.relink = false;  // set by StartLocate once started
    g_job.search = false;  // set by StartLocateFolder once started
    g_job.root.clear();
    g_job.self_name.clear();
    g_job.self_file.clear();
    g_job.self_lib.clear();
    g_job.self_node.clear();
    g_job.self_was.clear();
    g_job.done = false;
    g_job.cancel = false;
    g_job.running = true;
    {
        std::lock_guard<std::mutex> lk(g_job.mu);
        g_job.found.clear();
        g_job.ambiguous = 0;
        g_job.by_name.clear();
    }
    const std::string   resource = ResourcePath();
    const bool          has_self = self != nullptr;
    const AssetIdentity self_id = self ? *self : AssetIdentity{};
    const std::string   self_path = picked.last_path;
    try {
        g_job.worker = std::thread([missing, has_self, self_id, self_path, search, resource, by_name, by_name_dir]() {
            const int slot = RAV_WORKER_SLOT("Locate", 0);  // freeze audit
            (void)slot;
            RAV_WORKER_BUSY(slot, "searching", self_path);
            LocateOutcome            out;
            std::vector<FoundByName> named;
            if (has_self) out.found.push_back({self_id, self_path});
            try {
                // Each folder listed once in this run (the identity search and the by-name one share a root).
                const FolderLister base = MakeCachedFolderLister(resource, nullptr, &g_job.cancel);
                std::map<std::string, std::vector<DbFile>> listed;
                const FolderLister lister = [&base, &listed](const std::string& folder) {
                    const auto it = listed.find(folder);
                    if (it != listed.end()) return it->second;
                    return listed[folder] = base(folder);
                };
                const std::vector<LookupResult> res = search(missing, lister, &g_job.cancel);
                out = ClassifyLocate(missing, res, has_self ? &self_id : nullptr, self_path);
                if (!by_name.empty() && !by_name_dir.empty() && !g_job.cancel) {
                    const std::vector<DbFile> files = lister(by_name_dir);
                    std::vector<std::string>  paths;
                    for (const DbFile& f : files) paths.push_back(f.path);
                    for (const ByNameTarget& t : by_name) {
                        const std::string p = FirstByStem(paths, t.name);
                        if (p.empty()) continue;
                        FoundByName fb;
                        fb.lib_uid = t.lib_uid;
                        fb.node_uid = t.node_uid;
                        fb.found.last_path = p;
                        fb.found.name = BaseNameOf(p);
                        for (const DbFile& f : files)
                            if (f.path == p) {
                                fb.found.size = f.size;
                                fb.found.length = f.length;
                                break;
                            }
                        named.push_back(std::move(fb));
                    }
                }
            } catch (...) {
            }
            {
                std::lock_guard<std::mutex> lk(g_job.mu);
                g_job.found = std::move(out.found);
                g_job.ambiguous = out.ambiguous;
                g_job.by_name = std::move(named);
            }
            RAV_WORKER_IDLE(slot);
            g_job.done = true;
        });
    } catch (...) {
        g_job.running = false;
        ShowToastNoUndo("Could not start locating");
        return false;
    }
    return true;
}

// Spec missing-sounds-window ("Locate...: one file, then its folder for the others"): the results RelocateFolder did
// not find are looked for under the picked file's folder (and its subfolders).
std::vector<LookupResult> NearPicked(std::vector<LookupResult> res, const std::string& dir,
                                     const std::vector<AssetIdentity>& missing, const FolderLister& lister,
                                     const std::atomic<bool>* cancel)
{
    if (dir.empty()) return res;
    std::vector<AssetIdentity> rest;
    std::vector<size_t>        at;
    for (size_t i = 0; i < missing.size() && i < res.size(); ++i)
        if (res[i].status != LookupStatus::Found) {
            rest.push_back(missing[i]);
            at.push_back(i);
        }
    if (rest.empty()) return res;
    const std::vector<LookupResult> more = LocateUnderRoot(dir, rest, lister, cancel);
    for (size_t k = 0; k < more.size() && k < at.size(); ++k)
        if (more[k].status == LookupStatus::Found) res[at[k]] = more[k];
    return res;
}

// `allow_rename` (the Sound's own Locate, 13-50 review): a file of another name is that Sound's file
// renamed: the Sound takes it whole (name, path, size, length) in the same undo point as the others
// found through its old folder. Otherwise (the library menu's "first missing sound") it is refused.
void StartLocate(const std::string& lib_uid, const std::string& node_uid, const std::string& picked, bool allow_rename)
{
    const SoundLibrary* lib = FindLibrary(GetSoundStore(nullptr), lib_uid);
    const SoundNode*    n = lib ? FindNode(*lib, node_uid) : nullptr;
    if (!n || n->type != SoundNodeType::Sound) return;
    // Spec missing-sounds-window: an item Sound whose item is gone takes the picked file as its remembered one
    // (its kind kept); one whose item is there plays the item.
    if (n->sound.source.kind == SoundSourceKind::Item && !ItemSoundGone(*n)) {
        ShowToastNoUndo("Its item is in the project: it plays the item");
        return;
    }
    const std::string   self_name = n->name.empty() ? std::string("Sound") : n->name;
    const AssetIdentity stored = n->sound.source.asset;
    const bool unknown = stored.name.empty() && stored.last_path.empty();  // an older item Sound
    const bool renamed = unknown || LowerAscii(BaseNameOf(picked)) != LowerAscii(BaseNameOf(stored.name));
    if (renamed && !allow_rename) {
        ShowToastNoUndo("That is not " + (stored.name.empty() ? self_name : stored.name));
        return;
    }
    if (g_job.running) {  // before the PCM_source below
        ShowToastNoUndo("Already locating...");
        return;
    }
    AssetIdentity located;
    located.name = BaseNameOf(picked);
    located.size = FileSize(picked);
    located.last_path = picked;
    if (f_create) {
        if (PCM_source* src = f_create(picked.c_str())) {
            bool qn = false;
            const double len = GetMediaSourceLength(src, &qn);
            if (!qn && len > 0.0) located.length = len;
            delete src;
        }
    }
    if (renamed) {
        // The others: those sharing the Sound's old file are relinked with it (ApplyRelinked), not
        // looked for; the rest through the old folder mapped to the picked one.
        std::vector<AssetIdentity> others;
        for (const AssetIdentity& id : MissingIdentities())
            if (!(LowerAscii(id.last_path) == LowerAscii(stored.last_path) && SameAssetIdentity(id, stored)))
                others.push_back(id);
        const AssetIdentity key = RenamedLocateKey(stored.last_path, located);
        const std::string   dir = FolderOf(picked);
        const LocateSearch  search = [key, picked, dir](const std::vector<AssetIdentity>& missing,
                                                       const FolderLister& lister, const std::atomic<bool>* cancel) {
            return NearPicked(RelocateFolder(key, picked, missing, lister), dir, missing, lister, cancel);
        };
        if (!StartLocateJob(others, located, nullptr, search, MissingByName(lib_uid, node_uid), dir)) return;
        g_job.self_name = self_name;
        g_job.self_file = located.name;
        g_job.self_lib = lib_uid;
        g_job.self_node = node_uid;
        g_job.self_was = stored.last_path;
        g_job.relink = true;
        g_job.relink_lib = lib_uid;
        g_job.relink_node = node_uid;
        g_job.relink_old = stored;
        g_job.relink_now = located;
        ShowToastNoUndo("Locating...");
        return;
    }
    AssetIdentity self = stored;  // the picked Sound itself: its unknown fields filled
    if (self.size < 0) self.size = located.size;
    if (self.length < 0.0) self.length = located.length;

    // 13-38: RelocateFolder maps the stored OLD folder to the picked one (the other missing sounds
    // are then looked for in their mapped folders); ApplyLocated still gets the picked path (and sets
    // the Sound's size and length from it, so a file of another size no longer stays red, 13-7b).
    AssetIdentity old_id = located;
    old_id.last_path = stored.last_path;
    const std::string  dir = FolderOf(picked);
    const LocateSearch search = [old_id, picked, dir](const std::vector<AssetIdentity>& missing,
                                                      const FolderLister& lister, const std::atomic<bool>* cancel) {
        // the lister stops on cancel; spec missing-sounds-window: the others not found so are looked for in its folder
        return NearPicked(RelocateFolder(old_id, picked, missing, lister), dir, missing, lister, cancel);
    };
    if (StartLocateJob(MissingIdentities(), located, &self, search, MissingByName(lib_uid, node_uid), dir)) {
        g_job.self_name = self_name;
        g_job.self_file = located.name;
        g_job.self_lib = lib_uid;
        g_job.self_node = node_uid;
        g_job.self_was = stored.last_path;
        ShowToastNoUndo("Locating...");
    }
}

// 13-38: "Locate in a folder": every missing File Sound (all libraries) is looked for under `root`
// and its subfolders, on the worker (LocateUnderRoot; no PCM_source there, lengths stay unknown).
void StartLocateFolder(const std::string& root)
{
    if (root.empty()) return;
    if (g_job.running) {
        ShowToastNoUndo("Already locating...");
        return;
    }
    g.last_searched = root;
    const std::vector<AssetIdentity> missing = MissingIdentities();
    const std::vector<ByNameTarget>  by_name = MissingByName();
    if (missing.empty() && by_name.empty()) {
        ShowToastNoUndo("No missing sound");
        return;
    }
    const LocateSearch search = [root](const std::vector<AssetIdentity>& m, const FolderLister& lister,
                                       const std::atomic<bool>* cancel) {
        return LocateUnderRoot(root, m, lister, cancel);
    };
    if (StartLocateJob(missing, AssetIdentity{}, nullptr, search, by_name, root)) {
        g_job.search = true;
        g_job.root = root;
        ShowToastNoUndo("Searching " + root + "...");
    }
}

void ApplyLocateResult()
{
    if (!g_job.running || !g_job.done) return;
    JoinLocate();
    std::vector<std::pair<AssetIdentity, std::string>> found;
    std::vector<FoundByName>                           named;
    int ambiguous = 0;
    {
        std::lock_guard<std::mutex> lk(g_job.mu);
        found.swap(g_job.found);
        named.swap(g_job.by_name);
        ambiguous = g_job.ambiguous;
    }
    if (g_job.cancel) {  // 13-50 (13-11): Cancel keeps nothing, not even what was found before it
        ShowToastNoUndo("Locate cancelled: nothing changed");
        return;
    }
    // 13-50 review: the result belongs to the project the scan started in; another tab now shown
    // (or that project closed) drops it rather than writing it into the wrong project.
    if (g_job.proj != EnumProjects(-1, nullptr, 0)) {
        ShowToastNoUndo("Locate dropped: the project changed while it ran (nothing changed)");
        return;
    }
    int changed = 0, relinked = 0, by_name = 0;
    const AssetIdentity* picked = g_job.picked.last_path.empty() ? nullptr : &g_job.picked;
    // Spec missing-sounds-window: an item Sound whose item is gone has its remembered file relinked (kind kept).
    const std::function<bool(const SoundNode&)> item_ok = [](const SoundNode& n) { return ItemSoundGone(n); };
    const char* desc = g_job.search ? "RAV: Search a folder for missing sounds" : "RAV: Locate sounds";
    RefreshCaches();
    ModifySoundStore(nullptr, desc, [&](SoundStore& s) {
        if (g_job.relink)
            relinked = ApplyRelinked(s, g_job.relink_lib, g_job.relink_node, g_job.relink_old, g_job.relink_now, item_ok);
        changed = ApplyLocated(s, found, picked, item_ok);
        by_name = ApplyFoundByName(s, named);
        return changed + relinked + by_name > 0;
    });
    g.resolved.clear();
    g.from_items.clear();
    // Only the Sounds that now play the file found by their name (one that already remembered a file, or was
    // deleted, was left as it was).
    bool self_now = false;
    {
        const SoundStore& now = GetSoundStore(nullptr);
        auto asset_of = [&now](const std::string& lib_uid, const std::string& node_uid) -> const AssetIdentity* {
            const SoundLibrary* l = FindLibrary(now, lib_uid);
            const SoundNode*    n = l ? FindNode(*l, node_uid) : nullptr;
            return n ? &n->sound.source.asset : nullptr;
        };
        for (const FoundByName& f : named)
            if (const AssetIdentity* a = asset_of(f.lib_uid, f.node_uid))
                if (a->last_path == f.found.last_path) g.by_name_found.insert(f.lib_uid + "\n" + f.node_uid);
        if (!g_job.self_node.empty() && !g_job.picked.last_path.empty())
            if (const AssetIdentity* a = asset_of(g_job.self_lib, g_job.self_node))
                self_now = a->last_path == g_job.picked.last_path && g_job.self_was != g_job.picked.last_path;
    }
    const bool any = changed + relinked + by_name > 0;
    std::string msg;
    if (g_job.search) {
        msg = SearchToastText(changed + by_name, by_name, g_job.root, static_cast<int>(ListAllMissingSounds().size()), ambiguous);
    } else if (self_now) {
        // The mockup's Locate toast: the picked Sound, then what was found next to it.
        const int others = changed + relinked + by_name - 1;
        msg = "\"" + g_job.self_name + "\" now plays " + g_job.self_file + ". The other missing sounds were looked for in its folder";
        if (others > 0) msg += " (" + std::to_string(others) + " found" + (by_name > 0 ? ", " + std::to_string(by_name) + " by name: check them" : std::string()) + ")";
        msg += AmbiguousText(ambiguous) + ". Ctrl+Z undoes.";
    } else {
        msg = relinked > 0 ? RelinkToastText(g_job.relink_old.name, g_job.relink_now.name, changed, g_job.missing, ambiguous)
                           : LocateToastText(changed, g_job.missing, ambiguous);
    }
    ShowLibraryToast(msg, any, desc);
}

// ---- Pickers (run by the timer) -----------------------------------------------------------------

HWND__* Owner()
{
    return f_main_hwnd ? f_main_hwnd() : nullptr;
}

void RunImport()
{
    std::string path;
    if (!PickOpenFile(Owner(), L"Import a User Library", kRavlibFilter, path)) return;
    std::string text;
    SoundStore  file;
    if (!ReadText(fs::u8path(path), &text) || !ParseRavlib(text, &file)) {
        ShowToastNoUndo("Not a User Library file");
        return;
    }
    const SoundLibrary& flib = file.libraries.front();
    ListUserFolder();
    // 13-77a (reverses 13-11 B7): a library whose file is in the folder already is not copied over it (a shared file
    // is never overwritten silently): it is added from the list. The decision is ravlib.cpp's (DecideImport).
    const ImportDecision imp = DecideImport(GetSoundStore(nullptr), g.folder, flib.uid);
    if (imp.action == ImportAction::AlreadyInProject) {
        SelectLibraryRow(flib.uid, LibraryRow{"", FindLibrary(GetSoundStore(nullptr), flib.uid)->root});
        ShowToastNoUndo("Already in this project");
        return;
    }
    if (imp.action == ImportAction::Refuse) {
        ShowToastNoUndo(ImportRefusedToastText(imp.file));
        return;
    }
    const std::string name = FreeOnDisk(flib.name, FolderFileNames());
    if (!WriteText(FileOf(name), text)) {
        ShowToastNoUndo("Could not copy the file into " + UserFolderText());
        return;
    }
    ListUserFolder();
    std::string uid, why;
    const int64_t now = NowS();
    const char*   desc = "RAV: Import user library";
    if (ModifySoundStore(nullptr, desc, [&](SoundStore& s) {
            std::vector<std::pair<std::string, std::string>> map;
            LinkBase                                         base;
            if (!AddRavlibLibrary(s, file, name, &uid, &why, &map, &base)) return false;
            base.uid_map = map;
            base.synced_at = now;
            base.file_base = TranslateUidsText(text, map, true);  // review 13-77b H3: the file as read
            return SetLinkBase(s, base);
        })) {
        const SoundLibrary* l = FindLibrary(GetSoundStore(nullptr), uid);
        if (l) {
            SelectLibraryRow(uid, LibraryRow{"", l->root});
            ShowLibraryToast(l->name + " added: this project's own copy, linked to " + name, true, desc);
        }
    } else if (!why.empty()) {
        ShowToastNoUndo(why);
    }
}

void RunExport(const std::string& lib_uid)
{
    const SoundStore&   store = GetSoundStore(nullptr);
    const SoundLibrary* lib = FindLibrary(store, lib_uid);
    if (!lib) return;
    int               items = 0;
    const std::string text = SerializeRavlib(RavlibOf(store, lib_uid, &items));
    std::string       path;
    if (!PickSaveFile(Owner(), L"Export a User Library", kRavlibFilter, RavlibFileName(lib->name, {}), path)) return;
    if (!WriteText(fs::u8path(path), text)) {
        ShowToastNoUndo("Could not write " + path);
        return;
    }
    std::string msg = "Exported " + BaseNameOf(path);
    if (items > 0)
        msg += ": " + std::to_string(items) + " sound(s) come from project items and will be missing elsewhere";
    ShowToastNoUndo(msg);
}

// 13-36: the library's .ravlib selected in a File Explorer window (the folder alone when the file
// is not written yet).
void RunReveal(const std::string& lib_uid)
{
    const SoundLibrary* lib = FindLibrary(GetSoundStore(nullptr), lib_uid);
    if (!lib || !LibraryLinked(*lib)) return;
    std::error_code ec;
    fs::create_directories(UserFolder(), ec);
    const fs::path file = lib->ravlib_path.empty() ? fs::path() : FileOf(lib->ravlib_path);
    const bool     has_file = !file.empty() && fs::exists(file, ec);
    // 13-50 review: explorer.exe by its full path (Windows folder), never searched for on the PATH.
    wchar_t      win[MAX_PATH] = {};
    const UINT   wn = GetWindowsDirectoryW(win, MAX_PATH);
    if (wn == 0 || wn >= MAX_PATH) {
        ShowToastNoUndo("Could not open " + UserFolderText());
        return;
    }
    const std::wstring exe = std::wstring(win, wn) + L"\\explorer.exe";
    std::wstring       cmd = L"\"" + exe + L"\" ";
    if (has_file) cmd += L"/select,\"" + file.lexically_normal().wstring() + L"\"";
    else cmd += L"\"" + UserFolder().lexically_normal().wstring() + L"\"";
    STARTUPINFOW        si = {};
    PROCESS_INFORMATION pi = {};
    si.cb = sizeof si;
    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(L'\0');
    if (!CreateProcessW(exe.c_str(), buf.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        ShowToastNoUndo("Could not open " + UserFolderText());
        return;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
}

// 13-50 review: titled "Locate <its name>", opened in the folder it was in (or its nearest parent
// still there).
void RunLocatePick(const std::string& lib_uid, const std::string& node_uid, bool allow_rename)
{
    std::string name, folder;
    {
        const SoundLibrary* lib = FindLibrary(GetSoundStore(nullptr), lib_uid);
        const SoundNode*    n = lib ? FindNode(*lib, node_uid) : nullptr;
        if (!n) return;
        name = n->sound.source.asset.name.empty() ? BaseNameOf(n->sound.source.asset.last_path) : n->sound.source.asset.name;
        if (name.empty()) name = n->name;  // spec missing-sounds-window: an older item Sound (file unknown): its name
        try {
            folder = fs::u8path(n->sound.source.asset.last_path).parent_path().u8string();
        } catch (...) {
        }
    }  // no store reference across the dialog (REAPER's timers run while it is open)
    std::wstring title = L"Locate a missing sound";
    try {
        if (!name.empty()) title = L"Locate " + fs::u8path(name).wstring();
    } catch (...) {
    }
    std::string path;
    if (!PickOpenFile(Owner(), title.c_str(), kAudioFilter, folder, path)) return;
    StartLocate(lib_uid, node_uid, path, allow_rename);
}

void RunLocateFolderPick()
{
    std::string root;
    if (!PickFolder(Owner(), root)) return;  // cancelled
    StartLocateFolder(root);
}

// 13-77a: Locate... on a linked library whose file is gone: any .ravlib, which must hold this library. Outside the
// folder it is copied in. One undo point; the common version is unchanged (13-77b compares three ways).
void RunRelinkPick(const std::string& lib_uid)
{
    std::string lib_name;
    {
        const SoundLibrary* lib = FindLibrary(GetSoundStore(nullptr), lib_uid);
        if (!lib || !LibraryLinked(*lib)) return;
        lib_name = lib->name;
    }  // no store reference across the dialog
    std::string path;
    if (!PickOpenFile(Owner(), L"Locate the library file", kRavlibFilter, UserFolderText(), path)) return;
    std::string text;
    SoundStore  file;
    if (!ReadText(fs::u8path(path), &text) || !ParseRavlib(text, &file)) {
        ShowToastNoUndo("Not a User Library file");
        return;
    }
    if (file.libraries.front().uid != lib_uid) {
        ShowToastNoUndo("That file holds another library");
        return;
    }
    ListUserFolder();
    std::string name = BaseNameOf(path);
    bool        inside = false;
    try {
        std::error_code ec;
        inside = fs::equivalent(fs::u8path(path).parent_path(), UserFolder(), ec) && !ec;
    } catch (...) {
    }
    // Review 13-77a M1: picked outside the folder while the folder already holds this library: link to that file, no copy.
    const RelinkDecision dec = DecideRelink(g.folder, lib_uid, inside, name);
    if (dec.action == RelinkAction::LinkExisting) {
        name = dec.file;
    } else if (dec.action == RelinkAction::CopyIn) {
        name = FreeOnDisk(lib_name, FolderFileNames());
        if (!WriteText(FileOf(name), text)) {
            ShowToastNoUndo("Could not copy the file into " + UserFolderText());
            return;
        }
        ListUserFolder();
    }
    if (!Gesture("Relink library file", [&](SoundStore& s) { return RelinkLibraryFile(s, lib_uid, name); },
                 lib_name + " linked to " + name))
        ShowToastNoUndo(lib_name + " is linked to " + name + " already");
}

// Spec missing-sounds-window: every item Sound whose item is in the project remembers the file its active take plays
// (name, size, length, last path), so it still plays that file once the item is gone. Written silently (no undo
// point), only when it differs; run from the timer (never inside a frame: the store is replaced).
void RefreshItemAssets(ReaProject* proj)
{
    struct Update {
        std::string   lib, node;
        AssetIdentity asset;
    };
    std::vector<Update> updates;
    {
        std::unordered_map<std::string, MediaItem*> items;  // lower GUID -> item (one pass over the project)
        const int ni = CountMediaItems(proj);
        for (int i = 0; i < ni; ++i)
            if (MediaItem* it = GetMediaItem(proj, i)) items[LowerAscii(ItemGuidOf(it))] = it;
        const SoundStore& store = GetSoundStore(proj);
        for (const SoundLibrary& lib : store.libraries)
            for (const SoundNode& n : lib.nodes) {
                const SoundSource& src = n.sound.source;
                if (n.type != SoundNodeType::Sound || src.kind != SoundSourceKind::Item || src.item_guid.empty()) continue;
                const auto hit = items.find(LowerAscii(src.item_guid));
                MediaItem* item = hit == items.end() ? nullptr : hit->second;
                if (!item || IsFolderItem(item)) continue;
                MediaItem_Take* t = GetActiveTake(item);
                PCM_source*     pcm = t ? GetMediaItemTake_Source(t) : nullptr;
                const char*     fn = pcm ? pcm->GetFileName() : nullptr;
                if (!fn || !fn[0]) continue;
                const std::string path = AbsoluteMediaPath(fn, ProjectFolder());
                const double      len = pcm->GetLength();
                const AssetIdentity& a = src.asset;
                const bool same_len = !(len > 0.0) ? a.length < 0.0 : std::fabs(a.length - len) < 1e-6;
                if (a.last_path == path && a.name == BaseNameOf(path) && same_len && a.size >= 0) continue;  // cheap: no disk
                AssetIdentity now = a;
                now.name = BaseNameOf(path);
                now.last_path = path;
                now.length = len > 0.0 ? len : -1.0;
                now.size = FileSize(path);
                if (now.name == a.name && now.last_path == a.last_path && now.size == a.size && now.length == a.length) continue;
                updates.push_back({lib.uid, n.uid, now});
            }
    }
    if (updates.empty()) return;
    ModifySoundStore(proj, nullptr, [&](SoundStore& s) {
        bool any = false;
        for (const Update& u : updates) {
            SoundLibrary* l = FindLibrary(s, u.lib);
            SoundNode*    x = l ? FindNode(*l, u.node) : nullptr;
            if (!x || x->sound.source.kind != SoundSourceKind::Item) continue;
            AssetIdentity& a = x->sound.source.asset;
            if (a.name == u.asset.name && a.last_path == u.asset.last_path && a.size == u.asset.size && a.length == u.asset.length)
                continue;
            a = u.asset;
            any = true;
        }
        return any;
    });
}

// ---- 13-77b: the file sync (a worker for every disk read, the caches, Apply, Ctrl+Z of the file) ---------------------
// Every stat / read / write of a .ravlib of the sync runs on ONE worker thread (the folder may be a cloud drive or a
// network share): the UI thread copies the library (RavlibOf), posts a job, and takes the answer in this module's timer;
// the worker builds the texts (review 13-77b M8: no serialization on the UI thread per edit). The file is read again on
// project open / tab switch, when REAPER's window comes back to the foreground, after this module writes, and before each
// Apply; a read is a stat first and parses the file only when its size or time moved. Everything is kept per project tab
// (ReaProject* + its file name, review M5): a tab switch keeps the other project's journals and holds an Apply's answer
// until its project is shown again.

constexpr const char kSyncUndo[] = "RAV: Sync with file";

enum class SyncKind { Diff, Apply, Journal };
enum class SyncSt { Ok, Gone, Unreadable, OtherLibrary, FileChanged, WriteFailed, Failed };

using UidMap = std::vector<std::pair<std::string, std::string>>;
using ProjKey = std::pair<ReaProject*, std::string>;  // a tab, and the project file it shows

struct SyncJob {
    SyncKind    kind = SyncKind::Diff;
    ProjKey     proj;
    std::string lib_uid, file, path;
    SoundStore  one;  // the library and its groups (RavlibOf), the worker serializes it
    std::string base, file_base;  // the common versions (project uid space)
    UidMap      uid_map;
    std::vector<SwitchGroup> groups;  // the project's groups (the name merge, review M4)
    uint64_t    seq = 0;
    bool        have = false;  // Diff: the slot holds a diff (else the worker never answers "same")
    std::map<std::string, SyncDir> picks;
    SyncFileStamp listed;
    std::string   by;
    int64_t       now = 0;
    // Journal
    std::vector<SyncJournal> journals;
    std::vector<bool>        undone;
    uint64_t                 state_now = 0;
    bool                     undo_gate = false, redo_gate = false;
};

struct SyncOut {
    SyncKind      kind = SyncKind::Diff;
    SyncSt        st = SyncSt::Ok;
    ProjKey       proj;
    std::string   lib_uid, file;
    uint64_t      seq = 0;
    bool          same = false;  // Diff: nothing moved since the last answer (the slot keeps its diff)
    SyncDiff      diff;
    SyncFileStamp stamp;
    std::string   by;  // the file's header (after a write: this project, at the job's time; review H4)
    int64_t       at = 0;
    UidMap        uid_map;  // the link's map grown by the name merge
    // Apply
    std::map<std::string, SyncDir> picks;  // closed
    std::string mine_job;                           // the project copy the job worked on (M2: edits made since are carried)
    SyncResult  result;
    std::string file_proj_after;                    // the file as written, project uids
    std::string before_text, after_text;            // as read / as written, file uids
    bool        file_written = false;
    // Journal
    JournalPlan              plan;
    std::vector<SyncJournal> journals;
};

struct SyncCached {
    SyncFileStamp st;
    std::string   text;
};

// What the worker last answered for a library (review M8: a Diff that would give the same answer is skipped).
struct SyncSeen {
    uint64_t      mine = 0, base = 0, fb = 0, map = 0;
    SyncFileStamp st;
};

struct SyncWorker {
    std::thread             th;
    std::mutex              mu;
    std::condition_variable cv;
    std::deque<SyncJob>     jobs;
    std::deque<SyncOut>     outs;
    bool                    quit = false;
    bool                    started = false;
    std::atomic<bool>       finished{false};
};
SyncWorker g_sw;

SyncFileStamp SyncStatOf(const fs::path& p)
{
    SyncFileStamp s;
    std::error_code ec;
    if (!fs::is_regular_file(p, ec) || ec) return s;
    const auto sz = fs::file_size(p, ec);
    if (ec) return s;
    s.size = static_cast<int64_t>(sz);
    const auto t = fs::last_write_time(p, ec);
    s.mtime = ec ? 0 : static_cast<int64_t>(t.time_since_epoch().count());
    return s;
}

// Review 13-77b M6: a temp file of its own (two REAPER instances never share one), and a stat right before it replaces
// the file: when the file moved since it was read (`expect`), nothing is replaced (FileChanged).
SyncSt SyncWriteFile(const fs::path& p, const std::string& text, const SyncFileStamp* expect)
{
    static std::atomic<unsigned> counter{0};
    std::error_code              ec;
    fs::path                     tmp = p;
    tmp += ".rav" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(++counter) + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return SyncSt::WriteFailed;
        out << text;
        if (!out.good()) {
            out.close();
            fs::remove(tmp, ec);
            return SyncSt::WriteFailed;
        }
    }
    if (expect) {
        SyncFileStamp now = SyncStatOf(p);
        now.hash = 0;
        SyncFileStamp was = *expect;
        was.hash = 0;
        if (DecideApplyGate(was, now) != ApplyGate::Go) {
            fs::remove(tmp, ec);
            return SyncSt::FileChanged;
        }
    }
    fs::rename(tmp, p, ec);
    if (ec) {
        fs::remove(tmp, ec);
        return SyncSt::WriteFailed;
    }
    return SyncSt::Ok;
}

uint64_t MapHash(const UidMap& m)
{
    std::string s;
    for (const auto& kv : m) s += kv.first + ':' + kv.second + ',';
    return SyncTextHash(s);
}

// Worker: reads the file (the cache answers when size and time did not move, unless `fresh`), checks it is this
// library's, translates it to project uids (the link's map plus the name merge). False with out->st set otherwise.
bool SyncLoadFile(const SyncJob& job, bool fresh, std::map<std::string, SyncCached>& cache, SyncOut* out, std::string* file_text,
                  std::string* proj_text)
{
    const fs::path p = fs::u8path(job.path);
    SyncFileStamp  st = SyncStatOf(p);
    if (st.size < 0) {
        out->st = SyncSt::Gone;
        return false;
    }
    const auto it = cache.find(job.path);
    if (!fresh && it != cache.end() && it->second.st.size == st.size && it->second.st.mtime == st.mtime) {
        st = it->second.st;
        *file_text = it->second.text;
    } else {
        if (!ReadText(p, file_text)) {
            out->st = SyncSt::Unreadable;
            return false;
        }
        st.hash = SyncTextHash(*file_text);
        cache[job.path] = SyncCached{st, *file_text};
    }
    out->stamp = st;
    SoundStore parsed;
    if (!ParseRavlib(*file_text, &parsed)) {
        out->st = SyncSt::Unreadable;
        return false;
    }
    if (parsed.libraries.front().uid != job.lib_uid) {
        out->st = SyncSt::OtherLibrary;
        return false;
    }
    RavlibWriter(parsed, &out->by, &out->at);
    out->uid_map = job.uid_map;
    for (const auto& kv : NameMergeMap(job.groups, TranslateUidsText(*file_text, job.uid_map, true), job.uid_map))
        out->uid_map.push_back(kv);
    *proj_text = TranslateUidsText(*file_text, out->uid_map, true);
    return true;
}

std::string SeenKey(const SyncJob& job)
{
    return std::to_string(reinterpret_cast<uintptr_t>(job.proj.first)) + "\n" + job.proj.second + "\n" + job.lib_uid;
}

void SyncRunJob(const SyncJob& job, std::map<std::string, SyncCached>& cache, std::map<std::string, SyncSeen>& seen, SyncOut* out)
{
    out->kind = job.kind;
    out->proj = job.proj;
    out->lib_uid = job.lib_uid;
    out->file = job.file;
    out->seq = job.seq;
    if (job.kind == SyncKind::Journal) {
        out->journals = job.journals;
        const fs::path      p = fs::u8path(job.path);
        const SyncFileStamp st = SyncStatOf(p);
        std::string         text;
        const bool          exists = st.size >= 0 && ReadText(p, &text);
        out->plan = DecideJournalChain(job.journals, job.undone, job.state_now, job.undo_gate, job.redo_gate, exists,
                                       exists ? SyncTextHash(text) : 0);
        if (out->plan.step == JournalStep::RestoreBefore || out->plan.step == JournalStep::RestoreAfter) {
            if (SyncWriteFile(p, out->plan.write, nullptr) != SyncSt::Ok) out->st = SyncSt::WriteFailed;
            cache.erase(job.path);
        }
        return;
    }
    const std::string mine = SerializeRavlib(job.one);
    const bool        apply = job.kind == SyncKind::Apply;
    std::string       file_text, proj_text;
    if (!SyncLoadFile(job, apply, cache, out, &file_text, &proj_text)) {
        seen.erase(SeenKey(job));
        return;
    }
    // Second review L1: a name match found just now (file V3 = project V7) also translates the two common versions,
    // which may still hold the file's uid
    std::string base = job.base, file_base = job.file_base;
    if (out->uid_map.size() > job.uid_map.size()) {
        const UidMap fresh(out->uid_map.begin() + static_cast<long>(job.uid_map.size()), out->uid_map.end());
        base = TranslateUidsText(base, fresh, true);
        file_base = TranslateUidsText(file_base, fresh, true);
    }
    if (!apply) {
        SyncSeen  now{SyncTextHash(mine), SyncTextHash(job.base), SyncTextHash(job.file_base), MapHash(out->uid_map), out->stamp};
        SyncSeen& was = seen[SeenKey(job)];
        if (job.have && was.mine == now.mine && was.base == now.base && was.fb == now.fb && was.map == now.map && was.st == now.st) {
            out->same = true;
            return;
        }
        was = now;
        out->diff = DiffLibrary(base, file_base, mine, proj_text);
        return;
    }
    out->diff = DiffLibrary(base, file_base, mine, proj_text);
    out->mine_job = mine;
    out->file_proj_after = proj_text;
    out->before_text = file_text;
    if (DecideApplyGate(job.listed, out->stamp) != ApplyGate::Go) {  // the file moved since the list was built
        out->st = SyncSt::FileChanged;
        return;
    }
    out->picks = CloseSelection(out->diff, job.picks);
    if (!ApplySync(base, file_base, mine, proj_text, out->diff, out->picks, &out->result)) {
        out->st = SyncSt::Failed;
        return;
    }
    if (!out->result.file_changed) return;
    const std::string back = TranslateUidsText(out->result.file, out->uid_map, false);
    SoundStore        s2;
    if (!ParseRavlib(back, &s2)) {
        out->st = SyncSt::Failed;
        return;
    }
    const std::string text = SerializeRavlib(s2, job.by, job.now);
    const fs::path    p = fs::u8path(job.path);
    std::error_code   ec;
    fs::path          bak = p;
    bak += ".bak";
    fs::copy_file(p, bak, fs::copy_options::overwrite_existing, ec);  // one .bak per file, the version before
    if (ec) {
        out->st = SyncSt::WriteFailed;
        return;
    }
    const SyncSt w = SyncWriteFile(p, text, &out->stamp);
    if (w != SyncSt::Ok) {
        out->st = w;
        cache.erase(job.path);
        return;
    }
    out->after_text = text;
    out->file_proj_after = TranslateUidsText(text, out->uid_map, true);
    // second review H1: the file base stays built key by key; only what writing the text changed in it (nothing, or a
    // normalisation) is carried onto it, never the file's unpicked changes
    std::string fb_written;
    if (!out->result.file_base.empty() && CarryChanges(out->result.file, out->file_proj_after, out->result.file_base, &fb_written))
        out->result.file_base = fb_written;
    out->file_written = true;
    out->by = job.by;  // review H4: the header this Apply wrote
    out->at = job.now;
    cache.erase(job.path);
}

void SyncWorkerMain()
{
    std::map<std::string, SyncCached> cache;
    std::map<std::string, SyncSeen>   seen;
    for (;;) {
        SyncJob job;
        {
            std::unique_lock<std::mutex> lk(g_sw.mu);
            g_sw.cv.wait(lk, [] { return g_sw.quit || !g_sw.jobs.empty(); });
            if (g_sw.quit) break;
            job = std::move(g_sw.jobs.front());
            g_sw.jobs.pop_front();
        }
        SyncOut out;
        try {
            SyncRunJob(job, cache, seen, &out);
        } catch (...) {
            out.kind = job.kind;
            out.proj = job.proj;
            out.lib_uid = job.lib_uid;
            out.file = job.file;
            out.seq = job.seq;
            out.st = SyncSt::Failed;
        }
        std::lock_guard<std::mutex> lk(g_sw.mu);
        g_sw.outs.push_back(std::move(out));
    }
    g_sw.finished = true;
}

void PostSyncJob(SyncJob job)
{
    std::lock_guard<std::mutex> lk(g_sw.mu);
    if (!g_sw.started) {
        g_sw.started = true;
        g_sw.th = std::thread(SyncWorkerMain);
    }
    g_sw.jobs.push_back(std::move(job));
    g_sw.cv.notify_one();
}

// ---- the UI side ----

struct SyncSlot {
    bool          ready = false;  // diff is valid
    bool          bad = false;    // the file could not be read just now (or holds another library)
    bool          busy = false;   // a Diff job is in flight
    bool          force = true;   // read the file again (stat first) at the next chance
    bool          dirty = true;
    uint64_t      seq = 0;
    uint64_t      gen_seen = 0;
    double        quiet_until = 0.0;
    std::string   file_name;
    SyncDiff      diff;
    SyncFileStamp stamp;
    std::string   by;
    int64_t       at = 0;
};

struct SyncJournalRec {
    SyncJournal j;
    bool        undone = false;
};

struct SyncReq {
    std::string                    lib;
    std::map<std::string, SyncDir> picks;
    uint64_t                       ticket = 0;
    std::function<void(SyncApplyOutcome)> done;
};

struct SyncApplied {  // L5: the last Apply of a library, for 13-77c
    uint64_t         seq = 0;
    SyncApplyOutcome last = SyncApplyOutcome::None;
};

struct ProjSync {
    std::map<std::string, SyncSlot>                    slots;
    std::map<std::string, std::vector<SyncJournalRec>> journals;  // per library, oldest first
    std::set<std::string>                              journal_busy;
    std::vector<SyncReq>                               reqs;
    std::deque<SyncOut>                                held;  // answers that came while another project was shown
    std::map<std::string, SyncApplied>                 applied;
    bool                                               hist_empty = false;
    uint64_t                                           checked_gen = 0;
};

struct LibSyncState {
    std::map<ProjKey, ProjSync> projs;
    ProjKey                     cur;
    bool                        fg_in = false;
    bool                        apply_busy = false;  // an Apply is in flight (one at a time, any project)
    std::map<uint64_t, SyncReq> waiting;             // Applies posted, by job seq (the outcome goes back to them)
    uint64_t                    seq = 0;
    double                      next_prune = 0.0;
};
LibSyncState gs;

using CanRedoFn = const char* (*)(ReaProject*);
CanRedoFn f_can_redo = nullptr;  // Undo_CanRedo2 (resolved at init)

ProjSync& CurSync()
{
    return gs.projs[gs.cur];
}

// The tab shown now (second review H3).
ProjKey ShownProjKey()
{
    char        fn[4096] = {};
    ReaProject* p = EnumProjects(-1, fn, sizeof fn);
    return ProjKey(p, fn);
}

double MonoS()
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::string MineTextOf(const SoundStore& store, const std::string& uid)
{
    return SerializeRavlib(RavlibOf(store, uid, nullptr));
}

uint64_t LinkStateOf(const SoundStore& store, const std::string& lib_uid)
{
    const LinkBase* b = FindLinkBase(store, lib_uid);
    return LinkStateHash(b ? *b : LinkBase());
}

void FillJobFromStore(SyncJob* j, const SoundStore& store, const SoundLibrary& lib)
{
    j->proj = gs.cur;
    j->lib_uid = lib.uid;
    j->file = lib.ravlib_path;
    j->path = FileOf(lib.ravlib_path).u8string();
    j->one = RavlibOf(store, lib.uid, nullptr);
    j->groups = store.groups;
    if (const LinkBase* b = FindLinkBase(store, lib.uid)) {
        j->base = b->text;
        j->file_base = b->file_base;
        j->uid_map = b->uid_map;
    }
}

void SlotFromOut(SyncSlot& sl, SyncOut& o)
{
    if (o.same && sl.ready) return;
    sl.stamp = o.stamp;
    sl.by = o.by;
    sl.at = o.at;
    if ((o.st == SyncSt::Ok || o.st == SyncSt::FileChanged) && !o.same) {
        sl.diff = std::move(o.diff);
        sl.ready = true;
        sl.bad = false;
    } else if (o.st != SyncSt::Ok && o.st != SyncSt::FileChanged) {
        sl.ready = false;
        sl.bad = true;
    }
}

void ForceSync(ProjSync& ps, const std::string& lib_uid)
{
    auto it = ps.slots.find(lib_uid);
    if (it != ps.slots.end()) it->second.force = true;
}

void FinishApply(ProjSync& ps, const SyncOut& o, SyncApplyOutcome what)
{
    SyncApplied& a = ps.applied[o.lib_uid];
    ++a.seq;
    a.last = what;
    const auto it = gs.waiting.find(o.seq);
    if (it == gs.waiting.end()) return;
    const std::function<void(SyncApplyOutcome)> done = it->second.done;
    gs.waiting.erase(it);
    if (done) done(what);
}

void HandleApplyOut(ProjSync& ps, SyncOut& o)
{
    if (RouteSyncAnswer(true, o.proj == ShownProjKey()) == SyncRoute::Hold) {  // second review H3
        ps.held.push_back(std::move(o));
        return;
    }
    auto it = ps.slots.find(o.lib_uid);
    if (o.st == SyncSt::FileChanged) {
        if (it != ps.slots.end()) {
            SlotFromOut(it->second, o);
            it->second.force = true;
        }
        ShowToastNoUndo("The file changed meanwhile: check the changes again");
        FinishApply(ps, o, SyncApplyOutcome::Stopped);
        return;
    }
    if (it != ps.slots.end()) it->second.force = true;
    if (o.st != SyncSt::Ok) {
        if (o.st == SyncSt::Gone) ShowToastNoUndo(o.file + " is not in the User Library folder any more: nothing written");
        else if (o.st == SyncSt::Unreadable || o.st == SyncSt::OtherLibrary) ShowToastNoUndo(o.file + " cannot be read as this library's file: nothing written");
        else if (o.st == SyncSt::WriteFailed) ShowToastNoUndo("Could not write " + o.file + " in " + UserFolderText() + ": nothing changed");
        else ShowToastNoUndo("Sync with file failed: nothing changed");
        FinishApply(ps, o, SyncApplyOutcome::Failed);
        return;
    }
    const std::string   uid = o.lib_uid;
    ReaProject* const   proj = o.proj.first;  // second review H3: the project that asked, never "the current one"
    const SoundStore&   store = GetSoundStore(proj);
    const SoundLibrary* lib = FindLibrary(store, uid);
    if (!lib || !LibraryLinked(*lib) || lib->ravlib_path != o.file) {
        if (o.file_written) ShowToastNoUndo(o.file + " was written, but this library is no longer linked to it here");
        FinishApply(ps, o, SyncApplyOutcome::Failed);
        return;
    }
    // Second review M2: the library was edited during the round trip. The worker's two common versions describe what
    // was really written and stay; only the edits made meanwhile (the project copy the job took -> as it is now) are
    // carried onto the worker's merged copy.
    SyncResult        r = o.result;
    const std::string mine_now = MineTextOf(store, uid);
    if (mine_now != o.mine_job) {
        std::string carried;
        if (!CarryChanges(o.mine_job, mine_now, r.mine, &carried)) {
            ShowToastNoUndo(o.file_written ? o.file + " was written, but this project's copy could not be updated: check the changes again"
                                           : "This library changed while syncing: check the changes again");
            FinishApply(ps, o, SyncApplyOutcome::Failed);
            return;
        }
        r.mine = carried;
        r.mine_changed = r.mine != mine_now;
    }
    const int  sent_shown = o.file_written ? r.sent : 0;
    const bool anything = o.file_written || r.mine_changed || r.sent > 0 || r.taken > 0;
    if (!anything) {
        ShowToastNoUndo(r.open.empty() ? "Nothing to apply" : SyncAppliedToast(0, 0, o.file, static_cast<int>(r.open.size())));
        FinishApply(ps, o, SyncApplyOutcome::Nothing);
        return;
    }
    SoundStore merged, before;
    if (!ParseRavlib(r.mine, &merged)) {
        ShowToastNoUndo(o.file_written ? o.file + " was written, but this project's copy could not be updated: check the changes again"
                                       : "Sync with file failed: nothing changed");
        FinishApply(ps, o, SyncApplyOutcome::Failed);
        return;
    }
    const bool        has_before = ParseRavlib(mine_now, &before);
    uint64_t          state_before = 0, state_after = 0;
    const int64_t     now = NowS();
    const bool        ok = ModifySoundStore(proj, kSyncUndo, [&](SoundStore& s) {
        const LinkBase* ob = FindLinkBase(s, uid);
        state_before = LinkStateHash(ob ? *ob : LinkBase());
        if (!ReplaceLibraryFromMerge(s, uid, merged, has_before ? &before : nullptr)) return false;
        // Second review H2: what the merge adapted (a role 13-23 filtered, the project's group name / default, a
        // library name made unique) goes into the base only: never listed as "edited here", never sent to the file
        SyncResult rs = r;
        std::string absorbed;
        if (!rs.base.empty() && CarryChanges(rs.mine, MineTextOf(s, uid), rs.base, &absorbed)) rs.base = absorbed;
        const LinkBase nb = NextLinkBase(ob, uid, rs, o.uid_map, now, o.by, o.at);
        state_after = LinkStateHash(nb);
        SetLinkBase(s, nb);
        return true;
    });
    if (!ok) {
        ShowToastNoUndo(o.file_written ? o.file + " was written, but this project's copy could not be updated: check the changes again"
                                       : "Sync with file failed: nothing changed");
        FinishApply(ps, o, SyncApplyOutcome::Failed);
        return;
    }
    if (o.file_written) {  // review M3: always journalled once the file is written
        std::vector<SyncJournalRec>& js = ps.journals[uid];
        js.erase(std::remove_if(js.begin(), js.end(), [](const SyncJournalRec& x) { return x.undone; }), js.end());  // no redo past a new Apply
        SyncJournalRec jr;
        jr.j.lib_uid = uid;
        jr.j.file = o.file;
        jr.j.before = o.before_text;
        jr.j.after = o.after_text;
        jr.j.base_before = state_before;
        jr.j.base_after = state_after;
        js.push_back(jr);
        if (js.size() > 20) js.erase(js.begin());
    }
    std::string toast;
    if (sent_shown == 0 && r.taken == 0)
        toast = "Applied: " + std::to_string(r.sent) + (r.sent == 1 ? " change settled" : " changes settled") + ", " + o.file + " kept as it is";
    else
        toast = SyncAppliedToast(sent_shown, r.taken, o.file, static_cast<int>(r.open.size()));
    ShowLibraryToast(toast, true, kSyncUndo);
    FinishApply(ps, o, r.open.empty() ? SyncApplyOutcome::Applied : SyncApplyOutcome::Partly);
}

void HandleJournalOut(ProjSync& ps, SyncOut& o)
{
    ps.journal_busy.erase(o.lib_uid);
    ps.checked_gen = 0;  // second review M3: a Ctrl+Z made while this job ran is checked at the next tick
    ForceSync(ps, o.lib_uid);
    auto it = ps.journals.find(o.lib_uid);
    if (it == ps.journals.end()) return;
    std::vector<SyncJournalRec>& js = it->second;
    bool same = js.size() == o.journals.size();
    for (size_t i = 0; same && i < js.size(); ++i) same = js[i].j.after == o.journals[i].after && js[i].j.before == o.journals[i].before;
    const std::string f = o.file;
    if (o.st == SyncSt::WriteFailed) {
        ShowToastNoUndo((o.plan.step == JournalStep::RestoreAfter ? "Redo: the project is forward again, but " : "Undo: the project is back, but ") + f +
                        " could not be restored");
        ps.journals.erase(it);
        return;
    }
    if (o.plan.step == JournalStep::RestoreBefore)
        ShowToastNoUndo("Undo: the project is back, and " + f + " too (nobody wrote it since). " + f + ".bak stays as a backup");
    else if (o.plan.step == JournalStep::FileWrittenSince)
        ShowToastNoUndo("Undo: only the project is back: " + f + " was written since. " + f + ".bak keeps the version before");
    else if (o.plan.step == JournalStep::RestoreAfter)
        ShowToastNoUndo("Redo: the project is forward again, and " + f + " too");
    if (!same) return;  // another Apply came meanwhile: its journal stands as it is
    if (o.plan.drop) {
        ps.journals.erase(it);
        return;
    }
    for (size_t i = 0; i < js.size() && i < o.plan.undone.size(); ++i) js[i].undone = o.plan.undone[i];
}

void HandleOut(ProjSync& ps, SyncOut& o)
{
    if (o.kind == SyncKind::Apply) HandleApplyOut(ps, o);
    else if (o.kind == SyncKind::Journal) HandleJournalOut(ps, o);
}

// Second review H3: `shown` is the tab shown now, taken before any answer is handled.
void DrainSyncOuts(const ProjKey& shown)
{
    std::deque<SyncOut> outs;
    {
        std::lock_guard<std::mutex> lk(g_sw.mu);
        outs.swap(g_sw.outs);
    }
    for (SyncOut& o : outs) {
        if (o.kind == SyncKind::Apply) gs.apply_busy = false;
        auto            pit = gs.projs.find(o.proj);
        const SyncRoute route = RouteSyncAnswer(pit != gs.projs.end(), o.proj == shown);
        if (route == SyncRoute::Drop) {  // that tab is gone
            if (o.kind == SyncKind::Apply) gs.waiting.erase(o.seq);
            continue;
        }
        ProjSync& ps = pit->second;
        if (o.kind == SyncKind::Diff) {
            auto it = ps.slots.find(o.lib_uid);
            if (it == ps.slots.end() || it->second.seq != o.seq || it->second.file_name != o.file) continue;
            it->second.busy = false;
            SlotFromOut(it->second, o);
        } else if (route == SyncRoute::Handle) {
            HandleOut(ps, o);
        } else {
            if (o.kind == SyncKind::Journal) {
                ps.journal_busy.erase(o.lib_uid);
                ps.checked_gen = 0;  // M3
            }
            ps.held.push_back(std::move(o));  // review M5: taken when its project is shown again
        }
    }
}

bool StartSyncApply(const SyncReq& req)
{
    const SoundStore&   store = GetSoundStore(nullptr);
    const SoundLibrary* lib = FindLibrary(store, req.lib);
    ProjSync&           ps = CurSync();
    auto                it = ps.slots.find(req.lib);
    if (!lib || !LibraryLinked(*lib) || req.picks.empty()) {
        if (req.done) req.done(SyncApplyOutcome::Failed);
        return false;
    }
    if (gs.apply_busy || it == ps.slots.end() || !it->second.ready) {
        ShowToastNoUndo(gs.apply_busy ? "Another sync is running: try again in a moment" : "Still reading " + lib->ravlib_path + ": try again in a moment");
        if (req.done) req.done(SyncApplyOutcome::Failed);
        return false;
    }
    SyncJob j;
    j.kind = SyncKind::Apply;
    FillJobFromStore(&j, store, *lib);
    j.picks = req.picks;
    j.listed = it->second.stamp;
    j.by = ProjectFileName();
    j.now = NowS();
    j.seq = ++gs.seq;
    gs.apply_busy = true;
    gs.waiting[j.seq] = req;
    PostSyncJob(std::move(j));
    return true;
}

bool EmptyName(const char* s)
{
    return !s || !*s;
}

// Drops the projects whose tab is closed or shows another file now.
void PruneSyncProjects()
{
    std::set<ProjKey> open;
    char              fn[4096] = {};
    for (int i = 0;; ++i) {
        fn[0] = '\0';
        ReaProject* p = EnumProjects(i, fn, sizeof fn);
        if (!p) break;
        open.insert(ProjKey(p, fn));
    }
    for (auto it = gs.projs.begin(); it != gs.projs.end();) it = open.count(it->first) ? std::next(it) : gs.projs.erase(it);
}

void SyncTick(ReaProject* proj)
{
    char fn[4096] = {};
    EnumProjects(-1, fn, sizeof fn);
    const ProjKey key(proj, fn);
    DrainSyncOuts(key);  // second review H3: answers are routed against the tab shown now, not last tick's
    if (ResourcePath().empty() || !g.listed) return;
    const bool    switched = key != gs.cur;
    gs.cur = key;
    ProjSync& ps = gs.projs[key];
    const double now = MonoS();
    if (now >= gs.next_prune) {
        gs.next_prune = now + 2.0;
        PruneSyncProjects();
    }
    if (switched) {  // a tab shown again: its files are read again (stat first), its held answers taken
        for (auto& kv : ps.slots) kv.second.force = true;
        std::deque<SyncOut> held;
        held.swap(ps.held);
        for (SyncOut& o : held) HandleOut(ps, o);
    }
    // Review H2: a project opened, reverted or reopened in this tab starts with an empty undo history: the journals of
    // the project that was there before must never act on it.
    const char* can_undo = Undo_CanUndo2(proj);
    const char* can_redo = f_can_redo ? f_can_redo(proj) : nullptr;
    const bool  hist_empty = EmptyName(can_undo) && EmptyName(can_redo);
    if (hist_empty && !ps.hist_empty) {
        ps.journals.clear();
        ps.held.clear();
        for (auto& kv : ps.slots) kv.second.force = true;
    }
    ps.hist_empty = hist_empty;
    HWND       main = f_main_hwnd ? f_main_hwnd() : nullptr;
    HWND       fg = GetForegroundWindow();
    const bool in = main && fg && (fg == main || GetAncestor(fg, GA_ROOTOWNER) == main);
    if (in && !gs.fg_in)
        for (auto& kv : ps.slots) kv.second.force = true;
    gs.fg_in = in;

    const SoundStore& store = GetSoundStore(proj);
    const uint64_t    gen = SoundStoreGeneration();
    std::set<std::string> live;
    for (const SoundLibrary& lib : store.libraries) {
        if (lib.attached || !LibraryLinked(lib)) continue;
        live.insert(lib.uid);
        SyncSlot& sl = ps.slots[lib.uid];
        if (sl.file_name != lib.ravlib_path) {
            sl = SyncSlot();
            sl.file_name = lib.ravlib_path;
        }
        if (LinkedFileOf(lib) != LinkedFileState::Ok) {  // the File section shows its own bar
            sl.ready = false;
            sl.bad = false;
            sl.force = true;
            continue;
        }
        if (gen != sl.gen_seen) {
            sl.gen_seen = gen;
            sl.quiet_until = now + 0.35;  // an edit in progress (a drag) is not read on every tick
            sl.dirty = true;
        }
        if (sl.busy || gs.apply_busy) continue;
        if (!sl.force && !sl.dirty) continue;
        if (!sl.force && now < sl.quiet_until) continue;
        SyncJob j;
        j.kind = SyncKind::Diff;
        FillJobFromStore(&j, store, lib);
        j.have = sl.ready;
        sl.force = false;
        sl.dirty = false;
        sl.busy = true;
        sl.seq = j.seq = ++gs.seq;
        PostSyncJob(std::move(j));
    }
    for (auto it = ps.slots.begin(); it != ps.slots.end();) it = live.count(it->first) ? std::next(it) : ps.slots.erase(it);

    // A gesture of the frame (Send mine / Get file's): one at a time.
    if (!ps.reqs.empty() && !gs.apply_busy) {
        SyncReq r = std::move(ps.reqs.front());
        ps.reqs.erase(ps.reqs.begin());
        StartSyncApply(r);
    }

    // Ctrl+Z / redo of an Apply (review H2 / M7): only when REAPER's next redo (an undo just happened) or next undo (a
    // redo) is the sync, and the link's state is one an Apply left or found.
    if (gen != ps.checked_gen) {
        ps.checked_gen = gen;
        const bool undo_gate = !EmptyName(can_redo) && std::string(can_redo) == kSyncUndo;
        const bool redo_gate = !EmptyName(can_undo) && std::string(can_undo) == kSyncUndo;
        for (auto& kv : ps.journals) {
            if (kv.second.empty() || ps.journal_busy.count(kv.first)) continue;
            std::vector<SyncJournal> js;
            std::vector<bool>        undone;
            for (const SyncJournalRec& r : kv.second) {
                js.push_back(r.j);
                undone.push_back(r.undone);
            }
            const uint64_t state_now = LinkStateOf(store, kv.first);
            if (!JournalChainCandidate(js, undone, state_now, undo_gate, redo_gate)) continue;
            const SoundLibrary* lib = FindLibrary(store, kv.first);
            if (!lib || lib->ravlib_path != js.back().file) continue;
            SyncJob j;
            j.kind = SyncKind::Journal;
            j.proj = key;
            j.lib_uid = kv.first;
            j.file = js.back().file;
            j.path = FileOf(j.file).u8string();
            j.journals = js;
            j.undone = undone;
            j.state_now = state_now;
            j.undo_gate = undo_gate;
            j.redo_gate = redo_gate;
            j.seq = ++gs.seq;
            ps.journal_busy.insert(kv.first);
            PostSyncJob(std::move(j));
        }
    }
}

void OnTimer()
{
    RAV_QUIET_SCOPE("User Library timer", nullptr);  // freeze audit
    if (g.in_timer) return;
    g.in_timer = true;
    try {
        const PickKind    k = g.pick;
        const std::string lib = g.pick_lib, node = g.pick_node;
        const bool        rename = g.pick_rename;
        g.pick = PickKind::None;
        if (k == PickKind::Import) RunImport();
        else if (k == PickKind::Export) RunExport(lib);
        else if (k == PickKind::Reveal) RunReveal(lib);
        else if (k == PickKind::Relink) RunRelinkPick(lib);
        else if (k == PickKind::SaveFile) SaveLibraryToFile(lib);
        else if (k == PickKind::SaveAgain) SaveLibraryFileAgain(lib);
        else if (k == PickKind::Locate) RunLocatePick(lib, node, rename);
        else if (k == PickKind::LocateFolder) RunLocateFolderPick();

        ApplyLocateResult();

        ReaProject* proj = EnumProjects(-1, nullptr, 0);
        const int   st = GetProjectStateChangeCount(proj);
        if (st != g.last_state || proj != g.last_proj) {
            g.last_state = st;
            g.last_proj = proj;
            RefreshItemAssets(proj);  // spec missing-sounds-window: item Sounds remember their item's file
            FirstSightPass(proj);
        }
        SyncTick(proj);  // 13-77b: the file sync (worker answers, triggers, Apply requests, Ctrl+Z of the file)
    } catch (...) {
    }
    g.in_timer = false;
}

void RequestLocate(const std::string& lib_uid, const std::string& node_uid, bool allow_rename)
{
    g.pick = PickKind::Locate;
    g.pick_lib = lib_uid;
    g.pick_node = node_uid;
    g.pick_rename = allow_rename;
}

// ---- Lands-in tracks (decision 12) ----------------------------------------------------------------

// TrackName, TrackIndex, DirectChildren, AddTrackAt, AddChildTrack: sound_tracks.h (13-13).

bool OldLibraryItem(MediaItem* it);  // 13-24, below

MediaTrack* FolderFor(ReaProject* proj, const SoundStore& store, const std::string& dest)
{
    // 1. The parent folder of an already-materialised Sound with the same destination.
    if (f_parent)
        for (const SoundLibrary& lib : store.libraries)
            for (const SoundNode& n : lib.nodes) {
                if (n.type != SoundNodeType::Sound || n.sound.source.kind != SoundSourceKind::File) continue;
                if (n.sound.source.item_guid.empty() || EffectiveLandsIn(lib, n.uid) != dest) continue;
                MediaItem* it = FindItemByGuid(n.sound.source.item_guid);
                if (!OldLibraryItem(it)) continue;  // 13-24: a converted Sound's own item sits anywhere
                MediaTrack* tr = it ? GetMediaItem_Track(it) : nullptr;
                MediaTrack* par = tr ? f_parent(tr) : nullptr;
                if (par && GetMediaTrackInfo_Value(par, "I_FOLDERDEPTH") >= 1.0) return par;
            }
    // 2. A folder track named exactly so.
    const int n = CountTracks(proj);
    for (int i = 0; i < n; ++i) {
        MediaTrack* t = GetTrack(proj, i);
        if (t && GetMediaTrackInfo_Value(t, "I_FOLDERDEPTH") >= 1.0 && TrackName(t) == dest) return t;
    }
    // 3. A new folder at the bottom.
    MediaTrack* f = AddTrackAt(proj, n, dest);
    if (f) f_set_track(f, "I_FOLDERDEPTH", 1.0);
    return f;
}

MediaTrack* LayerTrack(ReaProject* proj, MediaTrack* folder, int layer)
{
    if (layer < 1) return nullptr;
    for (int guard = 0; guard < 64; ++guard) {
        const int fi = TrackIndex(proj, folder);
        if (fi < 0) return nullptr;
        int              last = fi, beyond = 0;
        std::vector<int> kids = DirectChildren(proj, fi, &last, &beyond);
        // 13-13: by position among the children that are no Switch-tree
        // folder (a Switch tree in this folder is never taken as a layer).
        std::vector<std::string> names;
        std::vector<bool>        tree;
        for (int j : kids) {
            MediaTrack* t = GetTrack(proj, j);
            names.push_back(TrackName(t));
            tree.push_back(IsSwitchTreeTrack(t));
        }
        int       next = 1;
        const int pick = PickLayerChild(names, tree, layer, &next);
        if (pick >= 0) return GetTrack(proj, kids[static_cast<size_t>(pick)]);
        // A new last child, the folder depths fixed (AddChildTrack).
        if (!AddChildTrack(proj, folder, "layer " + std::to_string(next))) return nullptr;
    }
    return nullptr;
}

// ---- 13-24: the conversion of Item Sounds (Save to a file) -------------------------------------------------------------

// An older build's imported User Library source item (kLibSourceTagKey). A File Sound's item that
// is not one is the project item a converted Sound came from (spec 13-24 C3).
bool OldLibraryItem(MediaItem* it)
{
    char mark[64] = {};
    return it && GetSetMediaItemInfo_String(it, kLibSourceTagKey, mark, false) && mark[0];
}

// The folder of the current project's file ("" = not saved yet).
std::string ProjectFolder()
{
    char fn[4096] = {};
    EnumProjects(-1, fn, sizeof fn);
    const std::string p = fn;
    const size_t      s = p.find_last_of("/\\");
    return s == std::string::npos ? std::string() : p.substr(0, s);
}

// What REAPER says of an Item Sound's item: its takes' source types, files, lengths and sizes.
ItemFileFacts ReadItemFileFacts(const std::string& guid, const std::string& project_dir)
{
    ItemFileFacts f;
    MediaItem* item = guid.empty() ? nullptr : FindItemByGuid(guid);
    if (!item) return f;
    f.found = true;
    f.pile = IsFolderItem(item);
    MediaItem_Take* active = GetActiveTake(item);
    const bool      all = f_count_takes && f_get_take;
    const int       n = all ? f_count_takes(item) : (active ? 1 : 0);
    for (int k = 0; k < n; ++k) {
        MediaItem_Take* t = all ? f_get_take(item, k) : active;
        PCM_source*     src = t ? GetMediaItemTake_Source(t) : nullptr;
        ItemTakeFile    tf;
        if (src) {
            const char* type = src->GetType();
            tf.type = type ? type : "";
            const char* fn = src->GetFileName();
            tf.path = AbsoluteMediaPath(fn ? std::string(fn) : std::string(), project_dir);
            tf.length = src->GetLength();
            tf.size = FileSize(tf.path);
        } else {
            tf.type = "EMPTY";
        }
        if (t && t == active) f.active = k;
        f.takes.push_back(tf);
    }
    // 13-24 review: detection's facts, so the conversion freezes the item's frames (C2).
    SoundMedia m;
    f.entry_ok = ReadItemMedia(item, &m);
    if (f.entry_ok) f.entry = m.facts;
    return f;
}

}  // namespace

// ---- Public -------------------------------------------------------------------------------------

bool FileSoundMissing(const SoundNode& n)
{
    if (n.type != SoundNodeType::Sound || n.sound.source.kind != SoundSourceKind::File) return false;
    bool unsettled = false;
    return SoundFilePath(n.sound.source, &unsettled).empty() && !unsettled;  // unsettled: not missing for the count
}

bool ItemSoundGone(const SoundNode& n)
{
    if (n.type != SoundNodeType::Sound || n.sound.source.kind != SoundSourceKind::Item) return false;
    if (n.sound.source.item_guid.empty()) return true;
    RefreshCaches();
    return !g.live_files.count(LowerAscii(n.sound.source.item_guid));
}

namespace {
std::string NodeFilePath(const SoundNode& n, bool* unsettled)
{
    if (unsettled) *unsettled = false;
    if (n.type != SoundNodeType::Sound) return {};
    const AssetIdentity& a = n.sound.source.asset;
    if (n.sound.source.kind == SoundSourceKind::Item && a.name.empty() && a.last_path.empty()) return {};
    return SoundFilePath(n.sound.source, unsettled);
}
// The missing rule's file test: a file still being checked (review M3) is not missing yet.
bool NodeFileAbsent(const SoundNode& x)
{
    bool unsettled = false;
    return NodeFilePath(x, &unsettled).empty() && !unsettled;
}
}  // namespace

std::string SoundNodeFilePath(const SoundNode& n) { return NodeFilePath(n, nullptr); }

bool SoundNodeMissing(const SoundNode& n, MissingWhy* why)
{
    return SoundIsMissing(
        n, [](const SoundNode& x) { return ItemSoundGone(x); }, [](const SoundNode& x) { return NodeFileAbsent(x); }, why);
}

std::vector<MissingSound> ListMissingSounds(const SoundLibrary& lib)
{
    return ListMissing(
        lib, [](const SoundNode& x) { return ItemSoundGone(x); }, [](const SoundNode& x) { return NodeFileAbsent(x); });
}

std::vector<MissingSound> ListAllMissingSounds()
{
    std::vector<MissingSound> out;
    for (const SoundLibrary& lib : GetSoundStore(nullptr).libraries) {
        std::vector<MissingSound> m = ListMissingSounds(lib);
        out.insert(out.end(), m.begin(), m.end());
    }
    return out;
}

AssetIdentity ItemFileAsset(MediaItem* item)
{
    AssetIdentity a;
    if (!item || IsFolderItem(item)) return a;
    MediaItem_Take* t = GetActiveTake(item);
    PCM_source*     src = t ? GetMediaItemTake_Source(t) : nullptr;
    const char*     fn = src ? src->GetFileName() : nullptr;
    if (!fn || !fn[0]) return a;
    a.last_path = AbsoluteMediaPath(fn, ProjectFolder());
    a.name = BaseNameOf(a.last_path);
    a.size = FileSize(a.last_path);
    const double len = src->GetLength();
    a.length = len > 0.0 ? len : -1.0;
    return a;
}

void RequestMissingSearchFolder()
{
    RequestLibraryLocateFolder();
}

const std::string& MissingLastSearchedFolder()
{
    return g.last_searched;
}

bool SoundFoundByName(const std::string& lib_uid, const std::string& node_uid)
{
    return g.by_name_found.count(lib_uid + "\n" + node_uid) > 0;
}

void ForgetFoundByName()
{
    g.by_name_found.clear();
}

bool PrepareFileSound(ReaProject* proj, const std::string& lib_uid, const std::string& node_uid,
                      FileSoundTarget* out, std::string* why, bool make_track)
{
    if (!out) return false;
    *out = FileSoundTarget{};
    const SoundStore    store = GetSoundStore(proj);
    const SoundLibrary* lib = FindLibrary(store, lib_uid);
    const SoundNode*    n = lib ? FindNode(*lib, node_uid) : nullptr;
    // Spec missing-sounds-window: an item Sound whose item is gone is placed from the file it remembers.
    const bool gone_item = n && n->type == SoundNodeType::Sound && n->sound.source.kind == SoundSourceKind::Item &&
                           ItemSoundGone(*n);
    if (!n || n->type != SoundNodeType::Sound || (n->sound.source.kind != SoundSourceKind::File && !gone_item)) {
        if (why) *why = "Not a file sound";
        return false;
    }
    // 13-24 (C3): a Sound converted from a Project Library is still placed from its own project item,
    // as before the conversion (an older build's imported library item keeps the file-first rule).
    // An offline own item gives way to the file once Locate has found it again.
    if (!gone_item && !n->sound.source.item_guid.empty()) {
        MediaItem* own = FindItemByGuid(n->sound.source.item_guid);
        RefreshCaches();
        const auto f = g.live_files.find(LowerAscii(n->sound.source.item_guid));
        const bool offline = f != g.live_files.end() && f->second.empty();
        if (own && !OldLibraryItem(own) && !(offline && !ResolvedPath(n->sound.source.asset).empty())) {
            out->item_guid = n->sound.source.item_guid;
            return true;
        }
    }
    const std::string& name = n->sound.source.asset.name;
    RefreshCaches();
    const std::string path = ResolvedPath(n->sound.source.asset);
    if (path.empty()) {
        // An older build imported the file as a muted source item: still placed from it.
        if (!gone_item && !n->sound.source.item_guid.empty() && FindItemByGuid(n->sound.source.item_guid)) {
            out->item_guid = n->sound.source.item_guid;
            return true;
        }
        if (why) {
            // Explicit: the item it was placed from is gone (13-24), and where the file was last seen.
            *why = "File not found: " + name;
            if (!n->sound.source.item_guid.empty()) *why += " (its item in this project was deleted)";
            const std::string& last = n->sound.source.asset.last_path;
            const size_t       cut = last.find_last_of("\\/");
            if (cut != std::string::npos) *why += ", last seen in " + last.substr(0, cut);
        }
        return false;
    }
    if (!make_track) {  // 13-13: the caller places it on its Switch-named track
        out->path = FileForProject(proj, path);
        return true;
    }
    if (!f_insert_track || !f_set_track) {
        if (why) *why = "This REAPER cannot make tracks for RAV";
        return false;
    }
    const std::string dest = EffectiveLandsIn(*lib, node_uid);
    // 13-16 (B4): a Blend's layers land on their own layer tracks (SoundTrackPath); "layer N" is
    // left for the sounds outside any Blend, so always the first one.
    const int         layer = 1;
    MediaTrack*       folder = FolderFor(proj, store, dest);
    MediaTrack*       track = folder ? LayerTrack(proj, folder, layer) : nullptr;
    if (!track) {
        if (why) *why = "Could not make the track for " + dest;
        return false;
    }
    out->path = FileForProject(proj, path);
    out->track = track;
    return true;
}

// ---- 13-7a (audit C1.4): the User Library pieces of the Libraries list -------------------------
// The mockup has no User Library section: user libraries are listed under USER in Libraries
// (library_tree_ui.cpp), with "+ Library" and "Import .ravlib..." under the list; Export,
// Locate and Refresh sit in the tree header's menu. This module keeps the folder, the files and
// the pickers; the list asks it through these functions.

void PrepareUserLibraries()
{
    ApplyHooks();
    RefreshCaches();
    if (!g.listed) ListUserFolder();
    // The folder is listed again when a file is added, removed or renamed there (its time
    // changes), looked at about once a second.
    static double             next_look = 0.0;
    static fs::file_time_type seen{};
    static bool               seen_set = false;
    const double              now = ImGui::GetTime();
    if (now < next_look) return;
    next_look = now + 1.0;
    std::error_code ec;
    const fs::file_time_type t = fs::last_write_time(UserFolder(), ec);
    if (ec) return;
    if (seen_set && t != seen) ListUserFolder();
    seen = t;
    seen_set = true;
}

std::vector<UserLibraryFile> UserFilesNotInProject()
{
    std::vector<UserLibraryFile> out;
    const SoundStore&            store = GetSoundStore(nullptr);
    for (const UserFile& f : g.files) {
        if (!f.uid.empty() && FindLibrary(store, f.uid)) continue;
        UserLibraryFile u;
        u.file = f.file;
        u.name = f.name;
        u.color = f.color;
        u.readable = !f.uid.empty();
        out.push_back(u);
    }
    return out;
}

void AddUserLibraryFile(const std::string& file)
{
    for (const UserFile& f : g.files) {
        if (f.file != file || f.uid.empty()) continue;
        std::string      uid, why;
        const SoundStore src = f.store;
        const std::string ftext = f.text;
        const int64_t    now = NowS();
        const char*      desc = "RAV: Add user library";
        if (ModifySoundStore(nullptr, desc, [&](SoundStore& s) {
                std::vector<std::pair<std::string, std::string>> map;
                LinkBase                                         base;
                if (!AddRavlibLibrary(s, src, file, &uid, &why, &map, &base)) return false;
                base.uid_map = map;
                base.synced_at = now;
                base.file_base = TranslateUidsText(ftext, map, true);  // review 13-77b H3: the file as read
                return SetLinkBase(s, base);
            })) {
            const SoundLibrary* l = FindLibrary(GetSoundStore(nullptr), uid);
            if (l) {
                SelectLibraryRow(uid, LibraryRow{"", l->root});
                ShowLibraryToast(l->name + " added: this project's own copy, linked to " + file, true, desc);
            }
        } else if (!why.empty()) {
            ShowToastNoUndo(why);
        }
        return;
    }
}

// 13-77a: "Save to a file..." on an unlinked library: no dialog; the file name comes from the library's name (the
// file this library was saved to before, when its link was undone, is reused). The 13-24 conversion runs first
// (Item Sounds that play a plain media file become File Sounds, the rest stay project-only). The file is written
// first, then the store changes (one undo point): a failed write changes nothing.
void SaveLibraryToFile(const std::string& lib_uid)
{
    const SoundStore    store = GetSoundStore(nullptr);
    const SoundLibrary* lib = FindLibrary(store, lib_uid);
    if (!lib || lib->attached || LibraryLinked(*lib)) return;
    if (ResourcePath().empty()) {
        ShowToastNoUndo("No REAPER resource folder: a library cannot be saved to a file");
        return;
    }
    // REAPER read once, before the gesture: every Item Sound's item.
    BusyScope busy("Saving library to a file");  // 13-59: many sounds
    busy.Phase("Reading sounds");
    const std::string                    dir = ProjectFolder();
    std::map<std::string, ItemFileFacts> facts;
    for (size_t k = 0; k < lib->nodes.size(); ++k) {
        const SoundNode& n = lib->nodes[k];
        BusyStep(static_cast<int>(k), static_cast<int>(lib->nodes.size()), "sound");
        if (n.type == SoundNodeType::Sound && n.sound.source.kind == SoundSourceKind::Item && !n.sound.source.item_guid.empty() &&
            !facts.count(n.sound.source.item_guid))
            facts[n.sound.source.item_guid] = ReadItemFileFacts(n.sound.source.item_guid, dir);
    }
    ListUserFolder();
    // Review 13-77a H1: a file of the folder that already holds this library is never written (other projects use
    // it): the library is linked to it without a common version, and 13-77b shows the differences as both-sides.
    const SaveFileDecision dec = DecideSaveToFile(*lib, g.folder);
    if (dec.action == SaveFileAction::Refuse) return;
    if (dec.action == SaveFileAction::LinkExisting) {
        const bool linked = Gesture("Save library to a file",
                                    [&](SoundStore& s) { return LinkLibraryWithoutBase(s, lib_uid, dec.file); },
                                    LinkedExistingToastText(dec.file));
        if (linked)
            if (const SoundLibrary* l = FindLibrary(GetSoundStore(nullptr), lib_uid)) SelectLibraryRow(lib_uid, LibraryRow{"", l->root});
        return;
    }
    const std::string file = FreeFileName(store, lib_uid, lib->name);
    const auto get_facts = [&facts](const std::string& guid) {
        const auto it = facts.find(guid);
        return it == facts.end() ? ItemFileFacts{} : it->second;
    };
    const std::string by = ProjectFileName();
    const int64_t     at = NowS();
    // The text the file will hold: the same conversion on a copy.
    SoundStore    copy = store;
    ConvertReport rep;
    if (!ConvertLibraryToUser(copy, lib_uid, file, get_facts, &rep)) return;
    busy.Phase("Writing");
    const std::string written = SerializeRavlib(RavlibOf(copy, lib_uid, nullptr), by, at);
    if (!WriteText(FileOf(file), written)) {
        ShowToastNoUndo("Could not write " + file + " in " + UserFolderText() + ": nothing changed");
        return;
    }
    const std::string name = lib->name;
    const char*       desc = "RAV: Save library to a file";
    const bool        ok = ModifySoundStore(nullptr, desc, [&](SoundStore& s) {
        ConvertReport r2;
        if (!ConvertLibraryToUser(s, lib_uid, file, get_facts, &r2)) return false;
        LinkBase b = MakeLinkBase(s, lib_uid, by, at, at);
        b.file_base = written;  // review 13-77b H3: the file as written (no uid renewed)
        return SetLinkBase(s, b);
    });
    ListUserFolder();
    if (!ok) return;
    if (const SoundLibrary* l = FindLibrary(GetSoundStore(nullptr), lib_uid)) SelectLibraryRow(lib_uid, LibraryRow{"", l->root});
    ShowLibraryToast(SaveToFileToastText(name, file, rep), true, desc);
}

// 13-77a: "Unlink from file": the library stays in the project with all its sounds; the file is not touched.
void UnlinkLibrary(const std::string& lib_uid)
{
    const SoundLibrary* lib = FindLibrary(GetSoundStore(nullptr), lib_uid);
    if (!lib || !LibraryLinked(*lib)) return;
    const std::string name = lib->name;
    int unsent = 0;  // 13-77b: the changes made here that the file never got
    if (const SyncDiff* sd = LibrarySyncDiff(lib_uid)) unsent = sd->here + sd->both;
    Gesture("Unlink library from file", [&](SoundStore& s) { return UnlinkLibraryFromFile(s, lib_uid); },
            name + " unlinked: it stays in this project with all its sounds" +
                (unsent > 0 ? ", your " + std::to_string(unsent) + " unsent " + (unsent == 1 ? "change" : "changes") + " included" : std::string()) +
                ". The file is not touched.");
}

void RequestSaveLibraryToFile(const std::string& lib_uid)
{
    g.pick = PickKind::SaveFile;
    g.pick_lib = lib_uid;
}

void RequestSaveLibraryFileAgain(const std::string& lib_uid)
{
    g.pick = PickKind::SaveAgain;
    g.pick_lib = lib_uid;
}

void RequestRelinkLibrary(const std::string& lib_uid)
{
    g.pick = PickKind::Relink;
    g.pick_lib = lib_uid;
}

// 13-77a: "Save it again" on a library whose file is gone: this project's copy is written as the file. When the name
// now holds another library, the copy goes to a free name and the library is linked to it (that file is never
// overwritten).
void SaveLibraryFileAgain(const std::string& lib_uid)
{
    const SoundStore    store = GetSoundStore(nullptr);
    const SoundLibrary* lib = FindLibrary(store, lib_uid);
    if (!lib || !LibraryLinked(*lib) || ResourcePath().empty()) return;
    ListUserFolder();
    // Review 13-77a M1: decided on the folder as it is now and on the disk right before the write (never an overwrite,
    // never a second file for one uid; a stale click on a file that is fine again does nothing).
    std::error_code ec;
    const bool      exists = fs::exists(FileOf(lib->ravlib_path), ec);
    const RewriteDecision dec = DecideSaveAgain(*lib, g.folder, exists);
    if (dec.action == RewriteAction::Nothing) return;
    if (dec.action == RewriteAction::LinkExisting) {
        Gesture("Relink library file", [&](SoundStore& s) { return RelinkLibraryFile(s, lib_uid, dec.file); },
                lib->name + " is in " + dec.file + ": linked to it, nothing written");
        ListUserFolder();
        return;
    }
    const bool        other = dec.action == RewriteAction::WriteNew;
    const std::string file = other ? FreeFileName(store, lib_uid, lib->name) : lib->ravlib_path;
    const std::string by = ProjectFileName();
    const int64_t     at = NowS();
    const std::string written = SerializeRavlib(RavlibOf(store, lib_uid, nullptr), by, at);
    if (!WriteText(FileOf(file), written)) {
        ShowToastNoUndo("Could not write " + file + " in " + UserFolderText() + ": nothing changed");
        return;
    }
    const std::string name = lib->name;
    Gesture("Save library file again",
            [&](SoundStore& s) {
                bool     any = other && RelinkLibraryFile(s, lib_uid, file);
                LinkBase b = MakeLinkBase(s, lib_uid, by, at, at);
                b.file_base = written;  // review 13-77b H3: the file as written (no uid renewed)
                if (const LinkBase* old = FindLinkBase(s, lib_uid)) b.syncs = old->syncs + 1;
                any = SetLinkBase(s, b) || any;
                return any;
            },
            name + " saved again as " + file + " in the User Library folder");
    ListUserFolder();
}

void RequestUserLibraryImport() { g.pick = PickKind::Import; }

void RequestUserLibraryExport(const std::string& lib_uid)
{
    g.pick = PickKind::Export;
    g.pick_lib = lib_uid;
}

void RequestUserLibraryReveal(const std::string& lib_uid)
{
    g.pick = PickKind::Reveal;
    g.pick_lib = lib_uid;
}

bool UserLibraryLocating()
{
    return g_job.running;
}

void CancelUserLibraryLocate()
{
    if (g_job.running) g_job.cancel = true;  // the worker returns early; the timer then discards it
}

void DrawLibraryLocateStatus()
{
    if (!g_job.running) return;
    const bool  stopping = g_job.cancel.load();
    const char* text = stopping ? "Cancelling..." : "Locating missing sounds...";
    const float bw = ui::ButtonWidth("Cancel");
    bool        beside = true;
    const float room = flow::StretchBeside(ImGui::GetContentRegionAvail().x, ui::Dp(60.0f), bw,
                                           ImGui::GetStyle().ItemSpacing.x, &beside);
    ui::FitText(text, ui::kMuted, room);
    if (beside) ImGui::SameLine();
    ImGui::BeginDisabled(stopping);
    if (ui::SolidButton("Cancel##locate")) CancelUserLibraryLocate();
    ImGui::EndDisabled();
    ui::ItemTipText("Stop looking for the missing sounds: nothing is changed");
    DrawBusyBar(-1.0f);  // 13-59: the busy card's bar (how many folders are left is not known)
}

void RequestUserLibraryLocate(const std::string& lib_uid, const std::string& node_uid, bool allow_rename)
{
    RequestLocate(lib_uid, node_uid, allow_rename);
}

void RequestLibraryLocateFolder()
{
    g.pick = PickKind::LocateFolder;
    g.pick_lib.clear();
    g.pick_node.clear();
}

void RefreshUserLibraries()
{
    ListUserFolder();
    g.resolved.clear();
}

std::string UserLibraryFolderText() { return UserFolderText(); }

void DrawLocateButtons(const std::string& lib_uid, const std::string& node_uid)
{
    if (g_job.running) {  // 13-50 (13-11): the scan in progress, with its Cancel
        DrawLibraryLocateStatus();
        return;
    }
    if (ui::SolidButton("Locate file...")) RequestLocate(lib_uid, node_uid, true);
    ui::ItemTipText("Pick this file where it is now (renamed too: this Sound then takes the new file): the other "
                    "missing sounds are looked for in the same folders");
    ui::SameLineIfFits(ui::ButtonWidth("Locate in a folder..."));
    if (ui::SolidButton("Locate in a folder...")) RequestLibraryLocateFolder();
    ui::ItemTipText("Pick the folder that holds the sounds (for example the soundbank's parent): RAV searches "
                    "it and its subfolders for the missing sounds of every library");
}

LinkedFileState LinkedFileOf(const SoundLibrary& lib)
{
    if (!g.listed) return LibraryLinked(lib) ? LinkedFileState::Ok : LinkedFileState::Unlinked;  // not listed yet: no red flash
    return LinkedFileStateOf(lib, g.folder);
}

namespace {

// A status line of the File section: a drawn glyph (0 = the open circle "only here", 1 = the green tick) and its text.
void FileStatusLine(int glyph, const std::string& text)
{
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float  gw = ui::Dp(18.0f);
    ImDrawList*  dl = ImGui::GetWindowDrawList();
    const float  cy = p.y + ImGui::GetTextLineHeight() * 0.5f;
    if (glyph == 1) DrawToolGlyphAt(dl, ImVec2(p.x + ui::Dp(6.0f), cy), ui::Dp(5.0f), kToolCheck, ui::kOk);
    else dl->AddCircle(ImVec2(p.x + ui::Dp(6.0f), cy), ui::Dp(3.5f), ui::kFaint, 0, std::max(1.0f, ui::Dp(1.2f)));
    ImGui::SetCursorScreenPos(ImVec2(p.x + gw, p.y));
    ui::ToneText(text, glyph == 1 ? ui::kMuted : ui::kMuted, true, std::max(ui::Dp(60.0f), ImGui::GetContentRegionAvail().x - gw));
}

}  // namespace

// ---- 13-77b: the sync bars of the File section ------------------------------------------------------------------

const SyncDiff* LibrarySyncDiff(const std::string& lib_uid)
{
    const auto pit = gs.projs.find(gs.cur);
    if (pit == gs.projs.end()) return nullptr;
    const auto it = pit->second.slots.find(lib_uid);
    return it != pit->second.slots.end() && it->second.ready ? &it->second.diff : nullptr;
}

LibrarySyncInfo LibrarySyncInfoOf(const std::string& lib_uid)
{
    LibrarySyncInfo info;
    const auto      pit = gs.projs.find(gs.cur);
    if (pit != gs.projs.end()) {
        const auto it = pit->second.slots.find(lib_uid);
        if (it != pit->second.slots.end()) {
            info.ready = it->second.ready;
            info.bad = it->second.bad;
            info.reading = it->second.busy || (!it->second.ready && !it->second.bad);
            info.by = it->second.by;
            info.at = it->second.at;
        }
        const auto ap = pit->second.applied.find(lib_uid);
        if (ap != pit->second.applied.end()) {
            info.apply_seq = ap->second.seq;
            info.last_apply = ap->second.last;
        }
    }
    if (const LinkBase* b = FindLinkBase(GetSoundStore(nullptr), lib_uid)) info.synced_at = b->synced_at;
    return info;
}

void RequestSyncApply(const std::string& lib_uid, const std::map<std::string, SyncDir>& picks,
                      std::function<void(SyncApplyOutcome)> done)
{
    if (picks.empty()) {
        if (done) done(SyncApplyOutcome::Nothing);
        return;
    }
    SyncReq r;
    r.lib = lib_uid;
    r.picks = picks;
    r.done = std::move(done);
    CurSync().reqs.push_back(std::move(r));
}

// The changes a row's File section lists and its gesture takes: a Sound's own (its subject), a container's row.
static std::vector<const SyncChange*> RowChanges(const SyncDiff& d, const SoundNode& n)
{
    std::vector<const SyncChange*> own;
    for (const SyncChange& c : d.changes)
        if (n.type == SoundNodeType::Sound ? c.subject == n.uid : c.row == n.uid) own.push_back(&c);
    return own;
}

void RequestNodeSync(const std::string& lib_uid, const std::string& node_uid, SyncDir dir)
{
    const SyncDiff* d = LibrarySyncDiff(lib_uid);
    if (!d) {
        ShowToastNoUndo("Still reading the file: try again in a moment");
        return;
    }
    const SoundLibrary* lib = FindLibrary(GetSoundStore(nullptr), lib_uid);
    const SoundNode*    n = lib ? FindNode(*lib, node_uid) : nullptr;
    if (!n) return;
    std::map<std::string, SyncDir> picks;
    for (const SyncChange* c : RowChanges(*d, *n)) picks[c->key] = dir;
    RequestSyncApply(lib_uid, CloseSelection(*d, picks));
}

namespace {

std::string SyncWhen(int64_t t)
{
    if (t <= 0) return {};
    const std::time_t tt = static_cast<std::time_t>(t);
    std::tm           tmv{};
    localtime_s(&tmv, &tt);
    char buf[64];
    std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M", &tmv);
    return buf;
}

enum class BarTone { Here, File, Both };

void BeginSyncBar(ui::Card& bar)
{
    ui::BeginCard(bar, ui::Dp(4.0f));
}

void EndSyncBar(ui::Card& bar, BarTone t)
{
    if (t == BarTone::Here) ui::EndCard(bar, ui::kAccentSoft, ui::kAccentLine);
    else if (t == BarTone::File) ui::EndCard(bar, ui::kWarnSoft, ui::kWarnBarLine);
    else ui::EndCard(bar, ui::kBothSoft, ui::kBothLine);
}

ImU32 BarInk(BarTone t)
{
    return t == BarTone::Here ? ui::kAccentText : t == BarTone::File ? ui::kWarnInk : ui::kBothInk;
}

BarTone ToneOf(const LibSyncCounts& c)
{
    return c.both > 0 || (c.here > 0 && c.file > 0) ? BarTone::Both : c.here > 0 ? BarTone::Here : BarTone::File;
}

std::string ChangesWord(int n)
{
    return std::to_string(n) + (n == 1 ? " change" : " changes");
}

// The root bar (mockup .bar): what differs between this project's copy and the file, who wrote it, when it was synced.
void DrawRootSyncBar(const SyncDiff& d, const LibrarySyncInfo& info)
{
    const LibSyncCounts c = TotalCounts(d);
    const BarTone    tone = ToneOf(c);
    ui::Card         bar;
    BeginSyncBar(bar);
    const ImU32 ink = BarInk(tone);
    if (c.here > 0) ui::ToneText(SyncCountLine(c.here, SyncSide::Here), ink, true);
    if (c.file > 0) {
        std::string t = ChangesWord(c.file) + " in the file";
        if (!info.by.empty()) t += " \xC2\xB7 by " + info.by + (info.at > 0 ? ", " + SyncWhen(info.at) : std::string());
        ui::ToneText(t, ink, true);
    }
    if (c.both > 0) ui::ToneText(ChangesWord(c.both) + " changed on both sides: you choose", ink, true);
    if (info.synced_at > 0) ui::ToneText("Last synced " + SyncWhen(info.synced_at), ui::kMuted, true);
    EndSyncBar(bar, tone);
}

// A container's (or a Sound's) bar: its own changes listed, Send mine / Get file's for all of them.
void DrawOwnSyncBar(const std::string& lib_uid, const std::string& node_uid, const std::vector<const SyncChange*>& own,
                    const std::string& file_by)
{
    LibSyncCounts c;
    for (const SyncChange* ch : own) (ch->side == SyncSide::Here ? c.here : ch->side == SyncSide::File ? c.file : c.both)++;
    const BarTone tone = ToneOf(c);
    const bool    mixed = tone == BarTone::Both && (c.here > 0 || c.file > 0) && c.both != c.Total();
    ui::Card      bar;
    BeginSyncBar(bar);
    const ImU32 ink = BarInk(tone);
    std::string head = ChangesWord(c.Total());
    if (tone == BarTone::Here) head += " made here";
    else if (tone == BarTone::File) head += " in the file" + (file_by.empty() ? std::string() : std::string(" \xC2\xB7 ") + file_by);
    ui::ToneText(head, ink, true);
    const size_t kMaxLines = 12;
    for (size_t i = 0; i < own.size() && i < kMaxLines; ++i) {
        const SyncChange* ch = own[i];
        std::string       t = "\xE2\x80\xA2 " + ch->text;
        if (ch->side == SyncSide::Here && mixed) t += " (here)";
        else if (ch->side == SyncSide::File && mixed) t += " (file)";
        ui::ToneText(t, ui::kMuted, true);
    }
    if (own.size() > kMaxLines) ui::ToneText("... and " + std::to_string(own.size() - kMaxLines) + " more", ui::kFaint, true);
    const bool up_primary = tone == BarTone::Here, down_primary = tone == BarTone::File;
    if (up_primary ? ui::PrimaryButton("\xE2\x86\x91 Send mine") : ui::SolidButton("\xE2\x86\x91 Send mine"))
        RequestNodeSync(lib_uid, node_uid, SyncDir::Up);
    ui::ItemTipText("Write these changes, as they are here, into the file. What they need goes with them.");
    ui::SameLineIfFits(ui::ButtonWidth("\xE2\x86\x93 Get file's"));
    if (down_primary ? ui::PrimaryButton("\xE2\x86\x93 Get file's") : ui::SolidButton("\xE2\x86\x93 Get file's"))
        RequestNodeSync(lib_uid, node_uid, SyncDir::Down);
    ui::ItemTipText("Take the file's version of this container here. What it needs goes with it.");
    EndSyncBar(bar, tone);
}

}  // namespace

// 13-77a: the mockup's File section (fileSection): where this library's file stands; 13-77b: its change bars.
void DrawLibraryFileSection()
{
    const SoundStore&   store = GetSoundStore(nullptr);
    const SoundLibrary* lib = FindLibrary(store, SelectedLibraryUid());
    const std::string   node_uid = LibraryRowNode();
    const SoundNode*    n = lib && !node_uid.empty() ? FindNode(*lib, node_uid) : nullptr;
    if (!lib || !n || lib->attached) return;  // an Attached library is never saved to a file
    const bool            is_root = n->uid == lib->root || n->parent.empty();
    const LinkedFileState st = LinkedFileOf(*lib);
    if (st == LinkedFileState::Unlinked && !is_root) return;
    if (st != LinkedFileState::Unlinked && st != LinkedFileState::Ok && !is_root) return;
    ui::Section sec;
    if (ui::BeginSection(sec, "File", ImTextureID(), false)) {
        const std::string lib_uid = lib->uid;
        if (st == LinkedFileState::Unlinked) {
            FileStatusLine(0, "Only in this project. Save it to a file to use it in other projects.");
            if (ui::SolidButton("Save to a file...")) RequestSaveLibraryToFile(lib_uid);
            ui::ItemTipText("Write this library as a .ravlib in the User Library folder: other projects can Add it");
            ui::SameLineIfFits(ui::ButtonWidth("Export .ravlib..."));
            if (ui::SolidButton("Export .ravlib...")) RequestUserLibraryExport(lib_uid);
            ui::ItemTipText("Save a copy anywhere, without a link");
        } else if (st == LinkedFileState::Ok) {
            if (is_root) {
                const float fw = ui::LabelRow("File", ui::Dp(80.0f));
                const ImVec2 p = ImGui::GetCursorScreenPos();
                const float  gs = ui::Dp(14.0f);
                DrawLinkGlyph(ImGui::GetWindowDrawList(), ImVec2(p.x, p.y + (ImGui::GetTextLineHeight() - gs) * 0.5f), gs, ui::kMuted);
                ImGui::Dummy(ImVec2(gs, ImGui::GetTextLineHeight()));
                ImGui::SameLine(0.0f, ui::Dp(6.0f));
                ui::FitText(lib->ravlib_path, ui::kText, std::max(ui::Dp(20.0f), fw - gs - ui::Dp(6.0f)));
                ui::ItemTipText(("In " + UserFolderText()).c_str());
                const SyncDiff*       sd = LibrarySyncDiff(lib_uid);
                const LibrarySyncInfo info = LibrarySyncInfoOf(lib_uid);
                if (sd && TotalCounts(*sd).Total() > 0) {
                    DrawRootSyncBar(*sd, info);
                    // review H5: the root's own changes (library settings, Switch Groups, containers new in the file
                    // right under it ...) with both buttons, until 13-77c's window
                    const std::vector<const SyncChange*> own = RowChanges(*sd, *n);
                    if (!own.empty()) DrawOwnSyncBar(lib_uid, n->uid, own, info.by);
                } else if (sd) {
                    FileStatusLine(1, "Same as the file. Changes made here stay here until you send them.");
                }
                else if (info.bad) FileStatusLine(0, "Could not read the file just now: it is read again when REAPER comes back to the front.");
                else FileStatusLine(0, "Reading the file...");
                if (ui::SolidButton("Export .ravlib...")) RequestUserLibraryExport(lib_uid);
                ui::ItemTipText("Save a copy of this library's file anywhere, to share it");
                ui::SameLineIfFits(ui::ButtonWidth("Show in Explorer"));
                if (ui::SolidButton("Show in Explorer")) RequestUserLibraryReveal(lib_uid);
                ui::ItemTipText("Open its folder in File Explorer, its file selected");
            } else {
                const SyncDiff* sd = LibrarySyncDiff(lib_uid);
                std::vector<const SyncChange*> own;
                if (sd) own = RowChanges(*sd, *n);
                if (!sd) FileStatusLine(0, "Reading the file...");
                else if (own.empty()) FileStatusLine(1, "Same as in " + lib->ravlib_path);
                else DrawOwnSyncBar(lib_uid, n->uid, own, LibrarySyncInfoOf(lib_uid).by);
            }
        } else {
            // Gone, renamed, or the name now holds another library: the red bar of the mockup.
            ui::Card bar;
            ui::BeginCard(bar, ui::Dp(4.0f));
            const LinkedFileWords words = LinkedFileWordsOf(st, lib->ravlib_path);
            ui::ToneText(words.title, ui::kBadInk, true);
            ui::ToneText(words.detail, ui::kMuted, true);
            if (ui::SolidButton("Locate...")) RequestRelinkLibrary(lib_uid);
            ui::ItemTipText("Pick the .ravlib of this library: it is linked again (copied into the folder when elsewhere)");
            ui::SameLineIfFits(ui::ButtonWidth("Save it again"));
            if (ui::SolidButton("Save it again")) RequestSaveLibraryFileAgain(lib_uid);
            ui::ItemTipText(words.save_again_tip.c_str());
            ui::EndCard(bar, ui::kBadSoft, ui::kBadLine);
        }
    }
    ui::EndSection(sec);
}

// 13-77a: the Tracks section: Lands in, now on every library (the Attached ones keep their sources' tracks).
void DrawLibraryTracksSection()
{
    const SoundStore&   store = GetSoundStore(nullptr);
    const SoundLibrary* lib = FindLibrary(store, SelectedLibraryUid());
    const std::string   node_uid = LibraryRowNode();
    const SoundNode*    n = lib && !node_uid.empty() ? FindNode(*lib, node_uid) : nullptr;
    // 13-50 (13-7b review): a missing File Sound's Locate buttons sit on the Sound panel's red Source
    // line only (DrawLocateButtons), never twice in the column.
    if (!lib || !n || lib->attached) return;
    ui::Section sec;
    if (ui::BeginSection(sec, "Tracks", ImTextureID(), false)) {
        {
            const bool        is_root = n->uid == lib->root || n->parent.empty();
            const std::string inherit =
                is_root ? std::string() : "Inherit (" + EffectiveLandsIn(*lib, n->parent) + ")";
            std::string shown = n->lands_in.empty() ? (is_root ? std::string(kDefaultLandsIn) : inherit) : n->lands_in;
            // The choices: RAV Library, the project's folder tracks, the destinations used in the store.
            std::vector<std::string> names = {kDefaultLandsIn};
            auto add = [&](const std::string& s) {
                if (!s.empty() && std::find(names.begin(), names.end(), s) == names.end()) names.push_back(s);
            };
            for (int i = 0, c = CountTracks(nullptr); i < c; ++i) {
                MediaTrack* t = GetTrack(nullptr, i);
                if (t && GetMediaTrackInfo_Value(t, "I_FOLDERDEPTH") >= 1.0) add(TrackName(t));
            }
            for (const SoundLibrary& l : store.libraries)
                for (const SoundNode& x : l.nodes) add(x.lands_in);
            std::string pick;
            bool        picked = false;
            // 13-14 (N2): the .row (.k label in the 80 px column) and the themed select.
            const float field_w = ui::LabelRow("Lands in", ui::Dp(80.0f));
            if (ui::BeginComboStyled("##landsin", shown.c_str(), field_w, ImGuiComboFlags_HeightLargest)) {
                // The tracks scroll in their own child (10 rows at most), so "New folder..." below them
                // is always seen, however long the project's track list.
                const int   rows = static_cast<int>(names.size()) + (is_root ? 0 : 1);
                const float list_h = static_cast<float>(std::clamp(rows, 1, 10)) * ImGui::GetTextLineHeightWithSpacing();
                ImGui::BeginChild("##landsinlist", ImVec2(0.0f, list_h), ImGuiChildFlags_None);
                if (!is_root && ImGui::Selectable(inherit.c_str(), n->lands_in.empty())) {
                    pick.clear();
                    picked = true;
                }
                for (const std::string& s : names)
                    if (ImGui::Selectable(s.c_str(), n->lands_in == s || (is_root && n->lands_in.empty() && s == kDefaultLandsIn))) {
                        pick = s;
                        picked = true;
                    }
                ImGui::EndChild();
                if (picked) ImGui::CloseCurrentPopup();  // a Selectable in a child does not close the combo
                ui::MenuSeparator();
                if (ImGui::Selectable("New folder...")) {
                    g.new_folder = true;
                    g.new_folder_node = n->uid;
                    g.buf[0] = '\0';
                }
                ui::EndComboStyled();
            }
            ui::ItemTipText("The folder track its files are placed under (one layer track per Blend layer)");
            if (g.new_folder && g.new_folder_node == n->uid) {
                const float okw = ui::ButtonWidth("OK");
                bool        beside = true;
                const float fw = flow::StretchBeside(ImGui::GetContentRegionAvail().x, ui::Dp(60.0f), okw,
                                                     ImGui::GetStyle().ItemSpacing.x, &beside);
                const bool  enter = ui::TextField("##newfolder", "Folder name", g.buf, sizeof g.buf, fw,
                                                  ImGuiInputTextFlags_EnterReturnsTrue);
                if (beside) ImGui::SameLine();
                const bool ok = ui::SolidButton("OK");
                if ((enter || ok) && g.buf[0]) {
                    pick = g.buf;
                    picked = true;
                    g.new_folder = false;
                }
                if (ImGui::IsKeyPressed(ImGuiKey_Escape)) g.new_folder = false;
            }
            if (picked) {
                const std::string lib_uid = lib->uid, nu = n->uid;
                std::string       value = pick;
                if (is_root && value == kDefaultLandsIn) value.clear();
                Gesture("Set Lands in", [&](SoundStore& s) {
                    SoundLibrary* l = FindLibrary(s, lib_uid);
                    SoundNode*    x = l ? FindNode(*l, nu) : nullptr;
                    if (!x || x->lands_in == value) return false;
                    x->lands_in = value;
                    return true;
                });
            }
        }
    }
    ui::EndSection(sec);
}

void InitLibraryUser(reaper_plugin_info_t* rec)
{
    if (!rec || !rec->GetFunc) return;
    auto get = rec->GetFunc;
    f_insert_track = reinterpret_cast<InsertTrackAtIndexFn>(get("InsertTrackAtIndex"));
    f_set_track = reinterpret_cast<SetMediaTrackInfoValueFn>(get("SetMediaTrackInfo_Value"));
    f_parent = reinterpret_cast<GetParentTrackFn>(get("GetParentTrack"));
    f_create = reinterpret_cast<CreateFromFileFn>(get("PCM_Source_CreateFromFile"));
    f_main_hwnd = reinterpret_cast<GetMainHwndFn>(get("GetMainHwnd"));
    f_can_redo = reinterpret_cast<CanRedoFn>(get("Undo_CanRedo2"));  // review 13-77b H2
    f_count_takes = reinterpret_cast<CountTakesFn>(get("CountTakes"));
    f_get_take = reinterpret_cast<GetMediaItemTakeFn>(get("GetMediaItemTake"));
    InitSoundTracks(get);  // 13-13: the Lands-in and Switch-named tracks
    ApplyHooks();  // the tree's missing marks, even before column 1 draws
    SetFileNameFold(&Win32FoldFileName);  // review 13-77a M1: names compare as the file system does
    g_register = rec->Register;
    if (g_register) g_timer_on = g_register("timer", reinterpret_cast<void*>(&OnTimer)) != 0;
}

void QuitLibraryUser()
{
    SetFileSoundResolver(nullptr);
    if (g_timer_on && g_register) g_register("-timer", reinterpret_cast<void*>(&OnTimer));
    g_timer_on = false;
    {  // 13-77b: the sync worker
        std::lock_guard<std::mutex> lk(g_sw.mu);
        g_sw.quit = true;
        g_sw.cv.notify_all();
    }
    if (g_sw.started) JoinWithin(g_sw.th, [] { return g_sw.finished.load(); });
    g_job.cancel = true;  // 13-38: a scan of a huge folder stops at once
    // Freeze audit #8: a scan stuck in a folder of a dead share is given up on after a while, not waited for for ever.
    JoinWithin(g_job.worker, [] { return g_job.done.load(); });
    g_job.running = false;
}

}  // namespace rav

#endif  // _WIN32
