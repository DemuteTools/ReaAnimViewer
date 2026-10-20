// SPDX-License-Identifier: MIT
//
// See library_user_ui.h. Spec 13-11 (decisions 2, 5, 8 to 13).
//
// Threads: the UI thread draws, runs the pickers and the autosave from this module's REAPER timer
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
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
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
#include "library_tree_edit.h"
#include "library_media_ui.h"  // freeze audit #2: MediaReachAsk / MediaReachLast (the probe worker)
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
};

enum class PickKind { None, Import, Export, Reveal, Locate, LocateFolder };

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
    // Autosave: per (project, its file + library uid) the text last written / seen (SavedKey).
    std::map<std::pair<ReaProject*, std::string>, std::string> saved;
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

// ---- Autosave (decision 9) ----------------------------------------------------------------------

fs::path FileOf(const std::string& name)
{
    return UserFolder() / fs::u8path(name);
}

// The autosave key: REAPER reuses a project tab's ReaProject* when another project is opened in it,
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

void Autosave(ReaProject* proj)
{
    if (ResourcePath().empty()) return;
    bool wrote = false;
    const SoundStore store = GetSoundStore(proj);
    for (const SoundLibrary& lib : store.libraries) {
        if (!lib.user) continue;
        std::string file = lib.ravlib_path;
        const fs::path p0 = file.empty() ? fs::path() : FileOf(file);
        std::error_code ec;
        bool clash = false;
        if (!file.empty() && fs::exists(p0, ec)) {
            const std::string other = UidInFile(p0);
            clash = !other.empty() && other != lib.uid;
        }
        if (file.empty() || clash) {  // a name of its own, kept in the project (no undo point)
            if (!g.listed) ListUserFolder();
            file = FreeFileName(store, lib.uid, lib.name);
            const std::string uid = lib.uid;
            ModifySoundStore(proj, nullptr, [&](SoundStore& s) {
                SoundLibrary* l = FindLibrary(s, uid);
                if (!l || l->ravlib_path == file) return false;
                l->ravlib_path = file;
                return true;
            });
        }
        const fs::path    p = FileOf(file);
        const std::string text = SerializeRavlib(RavlibOf(store, lib.uid, nullptr));
        const auto        key = SavedKey(proj, lib.uid);
        const auto        it = g.saved.find(key);
        if (it == g.saved.end()) {  // first sight: write only when the file is absent
            g.saved[key] = text;
            if (!fs::exists(p, ec) && WriteText(p, text)) wrote = true;
            continue;
        }
        if (it->second == text) continue;
        if (WriteText(p, text)) {
            it->second = text;
            wrote = true;
        }
    }
    if (wrote) ListUserFolder();
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
    if (FindLibrary(GetSoundStore(nullptr), flib.uid)) {
        SelectLibraryRow(flib.uid, LibraryRow{"", FindLibrary(GetSoundStore(nullptr), flib.uid)->root});
        ShowToastNoUndo("Already in this project");
        return;
    }
    // Copied into the user folder; a file of that library already there takes the imported text, so
    // the folder and the project agree.
    ListUserFolder();
    std::string name;
    for (const UserFile& f : g.files)
        if (f.uid == flib.uid) name = f.file;
    if (name.empty()) name = FreeOnDisk(flib.name, FolderFileNames());
    if (!WriteText(FileOf(name), text)) {
        ShowToastNoUndo("Could not copy the file into " + UserFolderText());
        return;
    }
    ListUserFolder();
    std::string uid, why;
    if (Gesture("Import user library", [&](SoundStore& s) { return AddRavlibLibrary(s, file, name, &uid, &why); })) {
        const SoundLibrary* l = FindLibrary(GetSoundStore(nullptr), uid);
        if (l) SelectLibraryRow(uid, LibraryRow{"", l->root});
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
// is not written yet). The autosave runs first, so the file shown holds the latest change.
void RunReveal(const std::string& lib_uid)
{
    Autosave(EnumProjects(-1, nullptr, 0));
    const SoundLibrary* lib = FindLibrary(GetSoundStore(nullptr), lib_uid);
    if (!lib || !lib->user) return;
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
        else if (k == PickKind::Locate) RunLocatePick(lib, node, rename);
        else if (k == PickKind::LocateFolder) RunLocateFolderPick();

        ApplyLocateResult();

        ReaProject* proj = EnumProjects(-1, nullptr, 0);
        const int   st = GetProjectStateChangeCount(proj);
        if (st != g.last_state || proj != g.last_proj) {
            g.last_state = st;
            g.last_proj = proj;
            RefreshItemAssets(proj);  // spec missing-sounds-window: item Sounds remember their item's file
            Autosave(proj);
        }
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

// ---- 13-24: Convert to User Library -------------------------------------------------------------

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
// (library_tree_ui.cpp), with "+ User Library" and "Import .ravlib..." under the list; Export,
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
        if (Gesture("Add user library", [&](SoundStore& s) { return AddRavlibLibrary(s, src, file, &uid, &why); })) {
            const SoundLibrary* l = FindLibrary(GetSoundStore(nullptr), uid);
            if (l) SelectLibraryRow(uid, LibraryRow{"", l->root});
        } else if (!why.empty()) {
            ShowToastNoUndo(why);
        }
        return;
    }
}

void NewUserLibrary()
{
    std::string       uid;
    const std::string file_dir_ok = ResourcePath();
    if (!Gesture("New user library", [&](SoundStore& s) {
            if (!NewLibrary(s, &uid)) return false;
            SoundLibrary* l = FindLibrary(s, uid);
            if (!l) return false;
            std::vector<std::string> names;
            for (const SoundLibrary& o : s.libraries)
                if (o.uid != uid) names.push_back(o.name);
            l->name = UniqueName("New User Library", names);
            if (SoundNode* root = FindNode(*l, l->root)) root->name = l->name;
            l->user = true;
            l->ravlib_path = FreeFileName(s, uid, l->name);
            return true;
        }))
        return;
    const SoundStore&   now = GetSoundStore(nullptr);
    const SoundLibrary* l = FindLibrary(now, uid);
    if (l && !file_dir_ok.empty()) {
        const std::string text = SerializeRavlib(RavlibOf(now, uid, nullptr));
        if (WriteText(FileOf(l->ravlib_path), text)) g.saved[SavedKey(nullptr, uid)] = text;
        ListUserFolder();
        SelectLibraryRow(uid, LibraryRow{"", l->root});
    }
}

void ConvertToUserLibrary(const std::string& lib_uid)
{
    const SoundStore    store = GetSoundStore(nullptr);
    const SoundLibrary* lib = FindLibrary(store, lib_uid);
    if (!lib || lib->user) return;
    if (ResourcePath().empty()) {
        ShowToastNoUndo("No REAPER resource folder: a User Library cannot be saved");
        return;
    }
    // REAPER read once, before the gesture: every Item Sound's item.
    BusyScope                            busy("Converting to User Library");  // 13-59: many sounds
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
    // 13-24 review: the file this library was already saved to (a conversion undone, then made
    // again) is reused, never a second "<name> 2" file with the same uid.
    std::string file;
    for (const UserFile& f : g.files)
        if (!f.uid.empty() && f.uid == lib_uid) {
            file = f.file;
            break;
        }
    if (file.empty()) file = FreeFileName(store, lib_uid, lib->name);
    ConvertReport     rep;
    busy.Phase("Writing");
    const char*       desc = "RAV: Convert to user library";
    const bool        ok = ModifySoundStore(nullptr, desc, [&](SoundStore& s) {
        return ConvertLibraryToUser(
            s, lib_uid, file,
            [&facts](const std::string& guid) {
                const auto it = facts.find(guid);
                return it == facts.end() ? ItemFileFacts{} : it->second;
            },
            &rep);
    });
    if (!ok) return;
    std::string         toast = ConvertReportText(rep, file);
    const SoundStore&   now = GetSoundStore(nullptr);
    const SoundLibrary* l = FindLibrary(now, lib_uid);
    if (l) {
        // Written at once (as New); on failure the autosave tries again (first sight: file absent).
        const std::string text = SerializeRavlib(RavlibOf(now, lib_uid, nullptr));
        if (WriteText(FileOf(l->ravlib_path), text)) g.saved[SavedKey(nullptr, lib_uid)] = text;
        else toast += ". Could not write it to " + UserFolderText() + " yet";
        ListUserFolder();
        SelectLibraryRow(lib_uid, LibraryRow{"", l->root});
    }
    ShowLibraryToast(toast, true, desc);
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

void DrawUserLibraryNodeSection()
{
    const SoundStore&   store = GetSoundStore(nullptr);
    const SoundLibrary* lib = FindLibrary(store, SelectedLibraryUid());
    const std::string   node_uid = LibraryRowNode();
    const SoundNode*    n = lib && !node_uid.empty() ? FindNode(*lib, node_uid) : nullptr;
    // 13-50 (13-7b review): a missing File Sound's Locate buttons sit on the Sound panel's red Source
    // line only (DrawLocateButtons), never twice in the column.
    if (!lib || !n || !lib->user) return;
    ui::Section sec;
    if (ui::BeginSection(sec, "User Library", ImTextureID(), false)) {
        {
            const bool        is_root = n->uid == lib->root || n->parent.empty();
            if (is_root) {
                // 13-36: its file, to share: a copy saved anywhere, or the file shown in File Explorer.
                // Not saved yet (no file name given): said so, never a guessed name (13-50 review).
                const std::string file = lib->ravlib_path.empty() ? std::string("(not saved yet)") : lib->ravlib_path;
                const float       fw = ui::LabelRow("File", ui::Dp(80.0f));
                ui::FitText(file, ui::kText, fw);
                ui::ItemTipText(("In " + UserFolderText()).c_str());
                if (ui::SolidButton("Export .ravlib...")) RequestUserLibraryExport(lib->uid);
                ui::ItemTipText("Save a copy of this library's file anywhere, to share it");
                ui::SameLineIfFits(ui::ButtonWidth("Show in Explorer"));
                if (ui::SolidButton("Show in Explorer")) RequestUserLibraryReveal(lib->uid);
                ui::ItemTipText("Open its folder in File Explorer, its file selected");
            }
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
    f_count_takes = reinterpret_cast<CountTakesFn>(get("CountTakes"));
    f_get_take = reinterpret_cast<GetMediaItemTakeFn>(get("GetMediaItemTake"));
    InitSoundTracks(get);  // 13-13: the Lands-in and Switch-named tracks
    ApplyHooks();  // the tree's missing marks, even before column 1 draws
    g_register = rec->Register;
    if (g_register) g_timer_on = g_register("timer", reinterpret_cast<void*>(&OnTimer)) != 0;
}

void QuitLibraryUser()
{
    SetFileSoundResolver(nullptr);
    if (g_timer_on && g_register) g_register("-timer", reinterpret_cast<void*>(&OnTimer));
    g_timer_on = false;
    g_job.cancel = true;  // 13-38: a scan of a huge folder stops at once
    // Freeze audit #8: a scan stuck in a folder of a dead share is given up on after a while, not waited for for ever.
    JoinWithin(g_job.worker, [] { return g_job.done.load(); });
    g_job.running = false;
}

}  // namespace rav

#endif  // _WIN32
