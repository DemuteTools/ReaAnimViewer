// SPDX-License-Identifier: MIT
//
// See library_user_ui.h. Spec 13-11 (decisions 2, 5, 8 to 13).
//
// Threads: the UI thread draws, runs the pickers and the autosave from this module's REAPER timer
// and applies a published Locate result; the Locate worker only runs RelocateFolder through the
// folder cache (media_db.h). Listing RAV's own Libraries folder (non-recursive) is done on the UI
// thread on first draw, after this module writes a file, and on Refresh (decision 2).

#include "library_user_ui.h"

#ifdef _WIN32

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
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

#include "file_dialogs.h"
#include "library_tree_edit.h"
#include "library_tree_ui.h"
#include "library_view_ui.h"
#include "media_db.h"
#include "ravlib.h"
#include "sound_populate.h"
#include "sound_project.h"
#include "sound_record.h"
#include "ui_theme.h"

namespace rav {

namespace {

namespace fs = std::filesystem;

using InsertMediaFn = int (*)(const char* file, int mode);
using SetOnlyTrackSelectedFn = void (*)(MediaTrack* track);
using SetTrackSelectedFn = void (*)(MediaTrack* track, bool selected);
using InsertTrackAtIndexFn = void (*)(int idx, bool want_defaults);
using SetMediaTrackInfoValueFn = bool (*)(MediaTrack* tr, const char* parm, double v);
using SetMediaItemInfoValueFn = bool (*)(MediaItem* item, const char* parm, double v);
using MoveMediaItemToTrackFn = bool (*)(MediaItem* item, MediaTrack* tr);
using PreventUIRefreshFn = void (*)(int prevent_count);
using SetMediaItemSelectedFn = void (*)(MediaItem* item, bool selected);
using GetParentTrackFn = MediaTrack* (*)(MediaTrack* track);
using CreateFromFileFn = PCM_source* (*)(const char* filename);
using GetMainHwndFn = HWND (*)();

InsertMediaFn            f_insert_media = nullptr;
SetOnlyTrackSelectedFn   f_only_track = nullptr;
SetTrackSelectedFn       f_track_sel = nullptr;
InsertTrackAtIndexFn     f_insert_track = nullptr;
SetMediaTrackInfoValueFn f_set_track = nullptr;
SetMediaItemInfoValueFn  f_set_item = nullptr;
MoveMediaItemToTrackFn   f_move_item = nullptr;
PreventUIRefreshFn       f_prevent = nullptr;
SetMediaItemSelectedFn   f_item_sel = nullptr;
GetParentTrackFn         f_parent = nullptr;
CreateFromFileFn         f_create = nullptr;
GetMainHwndFn            f_main_hwnd = nullptr;

int (*g_register)(const char*, void*) = nullptr;
bool g_timer_on = false;

constexpr int kSetLastTouchedTrack = 40914;  // Track: Set first selected track as last touched track
constexpr int kUnselectAllItems = 40289;     // Item: Unselect (clear selection of) all items

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

enum class PickKind { None, Import, Export, Locate };

struct LocateJob {
    std::thread                                       worker;
    std::atomic<bool>                                 running{false};
    std::atomic<bool>                                 done{false};
    std::mutex                                        mu;
    std::vector<std::pair<AssetIdentity, std::string>> found;  // under mu
    int                                               missing = 0;
    AssetIdentity                                     picked;  // the file the user picked (UI thread)
};

struct State {
    // Folder listing.
    bool                  listed = false;
    std::vector<UserFile> files;
    // Pending picker (run by the timer, never inside the ImGui frame).
    PickKind    pick = PickKind::None;
    std::string pick_lib;
    std::string pick_node;
    bool        in_timer = false;
    // Autosave: per (project, its file + library uid) the text last written / seen (SavedKey).
    std::map<std::pair<ReaProject*, std::string>, std::string> saved;
    int         last_state = -1;
    ReaProject* last_proj = nullptr;
    // Missing caches (decision 10).
    const MediaDbIndex*                          idx_seen = nullptr;
    std::unordered_map<std::string, std::string> resolved;  // asset key -> path ("" = missing)
    std::unordered_set<std::string>              live_guids;
    int                                          live_state = -1;
    ReaProject*                                  live_proj = nullptr;
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
    ShowLibraryToast(toast.empty() ? name : toast);
    return true;
}

// ---- Missing (decision 10) ----------------------------------------------------------------------

std::string AssetKey(const AssetIdentity& a)
{
    char buf[64];
    std::snprintf(buf, sizeof buf, "|%lld|%.4f|", static_cast<long long>(a.size), a.length);
    return LowerAscii(a.name) + buf + a.last_path;
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
        g.live_guids.clear();
        const int n = CountMediaItems(proj);
        for (int i = 0; i < n; ++i)
            if (MediaItem* it = GetMediaItem(proj, i)) g.live_guids.insert(LowerAscii(ItemGuidOf(it)));
    }
}

std::string ResolvedPath(const AssetIdentity& a)
{
    const std::string key = AssetKey(a);
    const auto it = g.resolved.find(key);
    if (it != g.resolved.end()) return it->second;
    const std::shared_ptr<const MediaDbIndex> idx = MediaDbGet();
    const std::string path = ResolveAssetPath(a, &FileSize, idx.get());
    g.resolved[key] = path;
    return path;
}

bool LiveItem(const std::string& guid)
{
    return !guid.empty() && g.live_guids.count(LowerAscii(guid)) != 0;
}

void ApplyHooks()
{
    const LibraryTreeHooks& h = GetLibraryTreeHooks();
    if (h.is_missing) return;
    LibraryTreeHooks n = h;
    n.is_missing = [](const SoundNode& s) { return FileSoundMissing(s); };
    SetLibraryTreeHooks(n);
}

int MissingIn(const SoundLibrary& lib, std::string* first)
{
    int n = 0;
    for (const SoundNode& s : lib.nodes)
        if (s.type == SoundNodeType::Sound && FileSoundMissing(s)) {
            if (n == 0 && first) *first = s.uid;
            ++n;
        }
    return n;
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

std::vector<AssetIdentity> MissingIdentities()
{
    std::vector<AssetIdentity> out;
    RefreshCaches();
    for (const SoundLibrary& lib : GetSoundStore(nullptr).libraries)
        for (const SoundNode& n : lib.nodes)
            if (n.type == SoundNodeType::Sound && FileSoundMissing(n)) out.push_back(n.sound.source.asset);
    return out;
}

void StartLocate(const std::string& lib_uid, const std::string& node_uid, const std::string& picked)
{
    const SoundLibrary* lib = FindLibrary(GetSoundStore(nullptr), lib_uid);
    const SoundNode*    n = lib ? FindNode(*lib, node_uid) : nullptr;
    if (!n || n->sound.source.kind != SoundSourceKind::File) return;
    const AssetIdentity stored = n->sound.source.asset;
    if (LowerAscii(BaseNameOf(picked)) != LowerAscii(BaseNameOf(stored.name))) {
        ShowToastNoUndo("That is not " + stored.name);
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
    AssetIdentity self = stored;  // the picked Sound itself: its unknown fields filled
    if (self.size < 0) self.size = located.size;
    if (self.length < 0.0) self.length = located.length;

    if (g_job.running) {
        ShowToastNoUndo("Already locating...");
        return;
    }
    JoinLocate();
    std::vector<AssetIdentity> missing = MissingIdentities();
    g_job.missing = static_cast<int>(missing.size());
    g_job.picked = located;
    g_job.done = false;
    g_job.running = true;
    {
        std::lock_guard<std::mutex> lk(g_job.mu);
        g_job.found.clear();
    }
    const std::string resource = ResourcePath();
    try {
        g_job.worker = std::thread([located, picked, self, missing, resource]() {
            std::vector<std::pair<AssetIdentity, std::string>> found;
            found.push_back({self, picked});
            try {
                const std::vector<LookupResult> res =
                    RelocateFolder(located, picked, missing, MakeCachedFolderLister(resource, nullptr));
                for (size_t i = 0; i < res.size() && i < missing.size(); ++i)
                    if (res[i].status == LookupStatus::Found) found.push_back({missing[i], res[i].path});
            } catch (...) {
            }
            {
                std::lock_guard<std::mutex> lk(g_job.mu);
                g_job.found = std::move(found);
            }
            g_job.done = true;
        });
    } catch (...) {
        g_job.running = false;
        ShowToastNoUndo("Could not start locating");
        return;
    }
    ShowToastNoUndo("Locating...");
}

void ApplyLocateResult()
{
    if (!g_job.running || !g_job.done) return;
    JoinLocate();
    std::vector<std::pair<AssetIdentity, std::string>> found;
    {
        std::lock_guard<std::mutex> lk(g_job.mu);
        found.swap(g_job.found);
    }
    int changed = 0;
    ModifySoundStore(nullptr, "RAV: Locate sounds", [&](SoundStore& s) {
        changed = ApplyLocated(s, found, &g_job.picked);
        return changed > 0;
    });
    g.resolved.clear();
    const int m = std::max(g_job.missing, changed);
    ShowLibraryToast("Located " + std::to_string(changed) + " of " + std::to_string(m) + " sounds", changed > 0);
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

void RunLocatePick(const std::string& lib_uid, const std::string& node_uid)
{
    std::string path;
    if (!PickOpenFile(Owner(), L"Locate a missing sound", kAudioFilter, path)) return;
    StartLocate(lib_uid, node_uid, path);
}

void OnTimer()
{
    if (g.in_timer) return;
    g.in_timer = true;
    try {
        const PickKind    k = g.pick;
        const std::string lib = g.pick_lib, node = g.pick_node;
        g.pick = PickKind::None;
        if (k == PickKind::Import) RunImport();
        else if (k == PickKind::Export) RunExport(lib);
        else if (k == PickKind::Locate) RunLocatePick(lib, node);

        ApplyLocateResult();

        ReaProject* proj = EnumProjects(-1, nullptr, 0);
        const int   st = GetProjectStateChangeCount(proj);
        if (st != g.last_state || proj != g.last_proj) {
            g.last_state = st;
            g.last_proj = proj;
            Autosave(proj);
        }
    } catch (...) {
    }
    g.in_timer = false;
}

void RequestLocate(const std::string& lib_uid, const std::string& node_uid)
{
    g.pick = PickKind::Locate;
    g.pick_lib = lib_uid;
    g.pick_node = node_uid;
}

// ---- Materialise (decision 12) ------------------------------------------------------------------

std::string TrackName(MediaTrack* tr)
{
    char buf[512] = {};
    if (!tr || !GetSetMediaTrackInfo_String(tr, "P_NAME", buf, false)) return {};
    return buf;
}

int TrackIndex(ReaProject* proj, MediaTrack* tr)
{
    const int n = CountTracks(proj);
    for (int i = 0; i < n; ++i)
        if (GetTrack(proj, i) == tr) return i;
    return -1;
}

// The direct children of the folder at `fi`, its last descendant, and how many folders above `fi`
// that one also closes (FolderChildren, ravlib.h).
std::vector<int> DirectChildren(ReaProject* proj, int fi, int* last, int* beyond)
{
    std::vector<int> depths;
    const int        n = CountTracks(proj);
    for (int j = 0; j < n; ++j) {
        MediaTrack* t = GetTrack(proj, j);
        depths.push_back(t ? static_cast<int>(GetMediaTrackInfo_Value(t, "I_FOLDERDEPTH")) : 0);
    }
    std::vector<int> kids;
    *beyond = FolderChildren(depths, fi, &kids, last);
    return kids;
}

MediaTrack* AddTrackAt(ReaProject* proj, int idx, const std::string& name)
{
    f_insert_track(idx, false);
    MediaTrack* tr = GetTrack(proj, idx);
    if (!tr) return nullptr;
    std::string n = name;
    std::vector<char> buf(n.begin(), n.end());
    buf.push_back('\0');
    GetSetMediaTrackInfo_String(tr, "P_NAME", buf.data(), true);
    return tr;
}

MediaTrack* FolderFor(ReaProject* proj, const SoundStore& store, const std::string& dest)
{
    // 1. The parent folder of an already-materialised Sound with the same destination.
    if (f_parent)
        for (const SoundLibrary& lib : store.libraries)
            for (const SoundNode& n : lib.nodes) {
                if (n.type != SoundNodeType::Sound || n.sound.source.kind != SoundSourceKind::File) continue;
                if (n.sound.source.item_guid.empty() || EffectiveLandsIn(lib, n.uid) != dest) continue;
                MediaItem* it = FindItemByGuid(n.sound.source.item_guid);
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
    for (int guard = 0; guard < 64; ++guard) {
        const int fi = TrackIndex(proj, folder);
        if (fi < 0) return nullptr;
        int              last = fi, beyond = 0;
        std::vector<int> kids = DirectChildren(proj, fi, &last, &beyond);
        if (static_cast<int>(kids.size()) >= layer) return GetTrack(proj, kids[static_cast<size_t>(layer - 1)]);
        // A new child after the last descendant, closing the folder and the folders the old last one
        // closed above it (a nested folder).
        MediaTrack* old_last = GetTrack(proj, last);
        const double d_last = last == fi ? 0.0 : GetMediaTrackInfo_Value(old_last, "I_FOLDERDEPTH");
        MediaTrack* t = AddTrackAt(proj, last + 1, "layer " + std::to_string(kids.size() + 1));
        if (!t) return nullptr;
        if (last != fi) f_set_track(old_last, "I_FOLDERDEPTH", d_last + 1.0 + beyond);
        f_set_track(t, "I_FOLDERDEPTH", -1.0 - beyond);
    }
    return nullptr;
}

bool ImportAvailable()
{
    return f_insert_media && f_only_track && f_track_sel && f_insert_track && f_set_track && f_set_item && f_move_item &&
           f_item_sel;
}

}  // namespace

// ---- Public -------------------------------------------------------------------------------------

bool FileSoundMissing(const SoundNode& n)
{
    if (n.type != SoundNodeType::Sound || n.sound.source.kind != SoundSourceKind::File) return false;
    RefreshCaches();
    if (LiveItem(n.sound.source.item_guid)) return false;
    return ResolvedPath(n.sound.source.asset).empty();
}

std::string MaterialiseFileSound(ReaProject* proj, const std::string& lib_uid, const std::string& node_uid,
                                 std::string* why)
{
    const SoundStore    store = GetSoundStore(proj);
    const SoundLibrary* lib = FindLibrary(store, lib_uid);
    const SoundNode*    n = lib ? FindNode(*lib, node_uid) : nullptr;
    if (!n || n->type != SoundNodeType::Sound || n->sound.source.kind != SoundSourceKind::File) {
        if (why) *why = "Not a file sound";
        return {};
    }
    const std::string& name = n->sound.source.asset.name;
    if (!n->sound.source.item_guid.empty() && FindItemByGuid(n->sound.source.item_guid)) return n->sound.source.item_guid;
    RefreshCaches();
    const std::string path = ResolvedPath(n->sound.source.asset);
    if (path.empty()) {
        if (why) *why = "File not found: " + name;
        return {};
    }
    if (!ImportAvailable()) {
        if (why) *why = "This REAPER cannot import files for RAV";
        return {};
    }
    const std::string dest = EffectiveLandsIn(*lib, node_uid);
    const int         layer = BlendLayerOf(*lib, node_uid) + 1;

    if (f_prevent) f_prevent(1);
    std::string guid;
    // Saved state: edit cursor, track and item selection.
    const double             cursor = GetCursorPositionEx(proj);
    std::vector<MediaTrack*> sel_tracks;
    for (int i = 0, c = CountSelectedTracks(proj); i < c; ++i) sel_tracks.push_back(GetSelectedTrack(proj, i));
    std::vector<MediaItem*> sel_items;
    for (int i = 0, c = CountSelectedMediaItems(proj); i < c; ++i) sel_items.push_back(GetSelectedMediaItem(proj, i));

    MediaTrack* folder = FolderFor(proj, store, dest);
    MediaTrack* track = folder ? LayerTrack(proj, folder, layer) : nullptr;
    if (track) {
        double end = 0.0;
        for (int i = 0, c = CountMediaItems(proj); i < c; ++i)
            if (MediaItem* it = GetMediaItem(proj, i))
                end = std::max(end, GetMediaItemInfo_Value(it, "D_POSITION") + GetMediaItemInfo_Value(it, "D_LENGTH"));
        const double pos = end + 1.0;
        f_only_track(track);
        Main_OnCommand(kSetLastTouchedTrack, 0);
        Main_OnCommand(kUnselectAllItems, 0);
        SetEditCurPos(pos, false, false);
        f_insert_media(path.c_str(), 0);
        MediaItem* item = CountSelectedMediaItems(proj) > 0 ? GetSelectedMediaItem(proj, 0) : nullptr;
        if (item) {
            if (GetMediaItem_Track(item) != track) f_move_item(item, track);
            f_set_item(item, "D_POSITION", pos);
            guid = ItemGuidOf(item);
        }
    }
    // Restore.
    SetEditCurPos(cursor, false, false);
    for (int i = 0, c = CountTracks(proj); i < c; ++i)
        if (MediaTrack* t = GetTrack(proj, i)) f_track_sel(t, false);
    for (MediaTrack* t : sel_tracks)
        if (t) f_track_sel(t, true);
    Main_OnCommand(kUnselectAllItems, 0);
    for (MediaItem* it : sel_items)
        if (it && ValidatePtr2(proj, it, "MediaItem*")) f_item_sel(it, true);
    if (f_prevent) f_prevent(-1);
    UpdateArrange();

    if (guid.empty()) {
        if (why) *why = track ? "Import failed: " + name : "Could not make the track for " + dest;
        return {};
    }
    ModifySoundStore(proj, nullptr, [&](SoundStore& s) {
        SoundLibrary* l = FindLibrary(s, lib_uid);
        SoundNode*    x = l ? FindNode(*l, node_uid) : nullptr;
        if (!x || x->sound.source.item_guid == guid) return false;
        x->sound.source.item_guid = guid;
        return true;
    });
    g.live_state = -1;  // the new item is live
    return guid;
}

void DrawUserLibrarySection()
{
    ApplyHooks();
    RefreshCaches();
    if (!g.listed) ListUserFolder();
    const SoundStore& store = GetSoundStore(nullptr);
    ui::Section       sec;
    const bool        open = ui::BeginSection(sec, "User Library");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Folder: %s", UserFolderText().c_str());
    if (open) {
        std::string add_file;
        for (const UserFile& f : g.files) {
            ImGui::PushID(f.file.c_str());
            const float  rh = ImGui::GetFrameHeight();
            const ImVec2 p = ImGui::GetCursorScreenPos();
            const float  w = std::max(1.0f, ImGui::GetContentRegionAvail().x);
            ImDrawList*  dl = ImGui::GetWindowDrawList();
            const float  ty = p.y + (rh - ImGui::GetTextLineHeight()) * 0.5f;
            const SoundLibrary* in = f.uid.empty() ? nullptr : FindLibrary(store, f.uid);
            ImGui::SetNextItemAllowOverlap();
            if (ImGui::Selectable("##ufile", in && SelectedLibraryUid() == f.uid, 0, ImVec2(w, rh)) && in)
                SelectLibraryRow(in->uid, LibraryRow{"", in->root});
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", f.file.c_str());
            ImU32 dot = ui::kAccent;
            if (f.color & 0x1000000u) dot = IM_COL32((f.color >> 16) & 0xFF, (f.color >> 8) & 0xFF, f.color & 0xFF, 0xFF);
            if (!f.uid.empty()) dl->AddCircleFilled(ImVec2(p.x + 6.0f, p.y + rh * 0.5f), 4.0f, dot);
            float rx = p.x + w - 4.0f;
            if (f.uid.empty()) {
                const char* t = "unreadable";
                rx -= ImGui::CalcTextSize(t).x;
                dl->AddText(ImVec2(rx, ty), ui::kMuted, t);
            } else if (in) {
                std::string first;
                const int   miss = MissingIn(*in, &first);
                if (miss > 0) {
                    rx -= 64.0f;
                    ImGui::SetCursorScreenPos(ImVec2(rx, p.y));
                    if (ui::SolidButton("Locate...", ImVec2(62.0f, rh))) RequestLocate(in->uid, first);
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Pick the file of its first missing sound: the others are looked for near it");
                    const std::string t = std::to_string(miss);
                    rx -= ImGui::CalcTextSize(t.c_str()).x + 6.0f;
                    dl->AddText(ImVec2(rx, ty), kLibMissing, t.c_str());
                }
                const char* t = "in project";
                rx -= ImGui::CalcTextSize(t).x + 6.0f;
                dl->AddText(ImVec2(rx, ty), ui::kMuted, t);
            } else {
                rx -= 42.0f;
                ImGui::SetCursorScreenPos(ImVec2(rx, p.y));
                if (ui::SolidButton("Add", ImVec2(40.0f, rh))) add_file = f.file;
            }
            ui::EllipsisText(dl, ImVec2(p.x + 16.0f, ty), rx - 4.0f, f.uid.empty() ? ui::kMuted : ui::kText, f.name);
            ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + rh));
            ImGui::Dummy(ImVec2(0.0f, 0.0f));
            ImGui::PopID();
        }
        if (g.files.empty()) ImGui::TextDisabled("No User Library yet");
        if (!add_file.empty()) {
            for (const UserFile& f : g.files)
                if (f.file == add_file) {
                    std::string uid, why;
                    const SoundStore file = f.store;
                    if (Gesture("Add user library", [&](SoundStore& s) { return AddRavlibLibrary(s, file, add_file, &uid, &why); })) {
                        const SoundLibrary* l = FindLibrary(GetSoundStore(nullptr), uid);
                        if (l) SelectLibraryRow(uid, LibraryRow{"", l->root});
                    } else if (!why.empty()) {
                        ShowToastNoUndo(why);
                    }
                    break;
                }
        }
        if (ui::SolidButton("+ User Library")) {
            std::string uid;
            const std::string file_dir_ok = ResourcePath();
            if (Gesture("New user library", [&](SoundStore& s) {
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
                })) {
                const SoundStore&   now = GetSoundStore(nullptr);
                const SoundLibrary* l = FindLibrary(now, uid);
                if (l && !file_dir_ok.empty()) {
                    const std::string text = SerializeRavlib(RavlibOf(now, uid, nullptr));
                    if (WriteText(FileOf(l->ravlib_path), text)) g.saved[SavedKey(nullptr, uid)] = text;
                    ListUserFolder();
                    SelectLibraryRow(uid, LibraryRow{"", l->root});
                }
            }
        }
        ImGui::SameLine();
        if (ui::SolidButton("Import...")) g.pick = PickKind::Import;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Add a .ravlib file: it is copied into the folder");
        const SoundLibrary* sel = FindLibrary(store, SelectedLibraryUid());
        const bool          can_export = sel && sel->user;
        ImGui::SameLine();
        ImGui::BeginDisabled(!can_export);
        if (ui::SolidButton("Export...") && can_export) {
            g.pick = PickKind::Export;
            g.pick_lib = sel->uid;
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip(can_export ? "Save the selected User Library where you choose"
                                         : "Select a User Library to export it");
        ImGui::SameLine();
        if (ui::SolidButton("Refresh")) {
            ListUserFolder();
            g.resolved.clear();
        }
    }
    ui::EndSection(sec);
}

void DrawUserLibraryNodeSection()
{
    const SoundStore&   store = GetSoundStore(nullptr);
    const SoundLibrary* lib = FindLibrary(store, SelectedLibraryUid());
    const std::string   node_uid = LibraryRowNode();
    const SoundNode*    n = lib && !node_uid.empty() ? FindNode(*lib, node_uid) : nullptr;
    if (!lib || !n) return;
    const bool file_missing = FileSoundMissing(*n);
    if (!lib->user && !file_missing) return;
    ui::Section sec;
    if (ui::BeginSection(sec, lib->user ? "User Library" : "File", ImTextureID(), false)) {
        if (file_missing) {
            ImGui::PushStyleColor(ImGuiCol_Text, ui::Col(kLibMissing));
            ImGui::TextWrapped("File not found: %s", n->sound.source.asset.name.c_str());
            ImGui::PopStyleColor();
            if (ui::SolidButton("Locate folder...")) RequestLocate(lib->uid, n->uid);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Pick this file: the other missing sounds are looked for in the same folders");
        }
        if (lib->user) {
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
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted("Lands in");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(std::max(80.0f, ImGui::GetContentRegionAvail().x));
            if (ImGui::BeginCombo("##landsin", shown.c_str())) {
                if (!is_root && ImGui::Selectable(inherit.c_str(), n->lands_in.empty())) {
                    pick.clear();
                    picked = true;
                }
                for (const std::string& s : names)
                    if (ImGui::Selectable(s.c_str(), n->lands_in == s || (is_root && n->lands_in.empty() && s == kDefaultLandsIn))) {
                        pick = s;
                        picked = true;
                    }
                ImGui::Separator();
                if (ImGui::Selectable("New folder...")) {
                    g.new_folder = true;
                    g.new_folder_node = n->uid;
                    g.buf[0] = '\0';
                }
                ImGui::EndCombo();
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("The folder track its files are imported under the first time they are used");
            if (g.new_folder && g.new_folder_node == n->uid) {
                ImGui::SetNextItemWidth(std::max(60.0f, ImGui::GetContentRegionAvail().x - 70.0f));
                const bool enter = ImGui::InputTextWithHint("##newfolder", "Folder name", g.buf, sizeof g.buf,
                                                            ImGuiInputTextFlags_EnterReturnsTrue);
                ImGui::SameLine();
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
    f_insert_media = reinterpret_cast<InsertMediaFn>(get("InsertMedia"));
    f_only_track = reinterpret_cast<SetOnlyTrackSelectedFn>(get("SetOnlyTrackSelected"));
    f_track_sel = reinterpret_cast<SetTrackSelectedFn>(get("SetTrackSelected"));
    f_insert_track = reinterpret_cast<InsertTrackAtIndexFn>(get("InsertTrackAtIndex"));
    f_set_track = reinterpret_cast<SetMediaTrackInfoValueFn>(get("SetMediaTrackInfo_Value"));
    f_set_item = reinterpret_cast<SetMediaItemInfoValueFn>(get("SetMediaItemInfo_Value"));
    f_move_item = reinterpret_cast<MoveMediaItemToTrackFn>(get("MoveMediaItemToTrack"));
    f_prevent = reinterpret_cast<PreventUIRefreshFn>(get("PreventUIRefresh"));
    f_item_sel = reinterpret_cast<SetMediaItemSelectedFn>(get("SetMediaItemSelected"));
    f_parent = reinterpret_cast<GetParentTrackFn>(get("GetParentTrack"));
    f_create = reinterpret_cast<CreateFromFileFn>(get("PCM_Source_CreateFromFile"));
    f_main_hwnd = reinterpret_cast<GetMainHwndFn>(get("GetMainHwnd"));
    ApplyHooks();  // the tree's missing marks, even before column 1 draws
    g_register = rec->Register;
    if (g_register) g_timer_on = g_register("timer", reinterpret_cast<void*>(&OnTimer)) != 0;
}

void QuitLibraryUser()
{
    if (g_timer_on && g_register) g_register("-timer", reinterpret_cast<void*>(&OnTimer));
    g_timer_on = false;
    JoinLocate();
}

}  // namespace rav

#endif  // _WIN32
