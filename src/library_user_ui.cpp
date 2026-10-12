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
#include "sound_place.h"
#include "sound_place_plan.h"
#include "sound_populate.h"
#include "sound_project.h"
#include "sound_record.h"
#include "sound_tracks.h"
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
    ShowLibraryToast("Located " + std::to_string(changed) + " of " + std::to_string(m) + " sounds", changed > 0,
                     "RAV: Locate sounds");
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

// ---- Lands-in tracks (decision 12) ----------------------------------------------------------------

// TrackName, TrackIndex, DirectChildren, AddTrackAt, AddChildTrack: sound_tracks.h (13-13).

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
    if (layer < 1) return nullptr;
    for (int guard = 0; guard < 64; ++guard) {
        const int fi = TrackIndex(proj, folder);
        if (fi < 0) return nullptr;
        int              last = fi, beyond = 0;
        std::vector<int> kids = DirectChildren(proj, fi, &last, &beyond);
        // 13-13: "layer N" by name, else by position among the children that are no Switch-tree
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

}  // namespace

// ---- Public -------------------------------------------------------------------------------------

bool FileSoundMissing(const SoundNode& n)
{
    if (n.type != SoundNodeType::Sound || n.sound.source.kind != SoundSourceKind::File) return false;
    RefreshCaches();
    if (LiveItem(n.sound.source.item_guid)) return false;
    return ResolvedPath(n.sound.source.asset).empty();
}

bool PrepareFileSound(ReaProject* proj, const std::string& lib_uid, const std::string& node_uid,
                      FileSoundTarget* out, std::string* why, bool make_track)
{
    if (!out) return false;
    *out = FileSoundTarget{};
    const SoundStore    store = GetSoundStore(proj);
    const SoundLibrary* lib = FindLibrary(store, lib_uid);
    const SoundNode*    n = lib ? FindNode(*lib, node_uid) : nullptr;
    if (!n || n->type != SoundNodeType::Sound || n->sound.source.kind != SoundSourceKind::File) {
        if (why) *why = "Not a file sound";
        return false;
    }
    const std::string& name = n->sound.source.asset.name;
    RefreshCaches();
    const std::string path = ResolvedPath(n->sound.source.asset);
    if (path.empty()) {
        // An older build imported the file as a muted source item: still placed from it.
        if (!n->sound.source.item_guid.empty() && FindItemByGuid(n->sound.source.item_guid)) {
            out->item_guid = n->sound.source.item_guid;
            return true;
        }
        if (why) *why = "File not found: " + name;
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
    const int         layer = BlendLayerOf(*lib, node_uid) + 1;
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

void RequestUserLibraryImport() { g.pick = PickKind::Import; }

void RequestUserLibraryExport(const std::string& lib_uid)
{
    g.pick = PickKind::Export;
    g.pick_lib = lib_uid;
}

int UserLibraryMissing(const std::string& lib_uid, std::string* first)
{
    const SoundLibrary* lib = FindLibrary(GetSoundStore(nullptr), lib_uid);
    return lib ? MissingIn(*lib, first) : 0;
}

void RequestUserLibraryLocate(const std::string& lib_uid, const std::string& node_uid) { RequestLocate(lib_uid, node_uid); }

void RefreshUserLibraries()
{
    ListUserFolder();
    g.resolved.clear();
}

std::string UserLibraryFolderText() { return UserFolderText(); }

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
            ImGui::SetNextItemWidth(ui::LabelRow("Lands in", 80.0f, ui::TextWidth("Lands in") + ImGui::GetStyle().ItemSpacing.x));
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
                ImGui::SetTooltip("The folder track its files are placed under (one layer track per Blend layer)");
            if (g.new_folder && g.new_folder_node == n->uid) {
                const float okw = ui::ButtonWidth("OK");
                bool        beside = true;
                ImGui::SetNextItemWidth(flow::StretchBeside(ImGui::GetContentRegionAvail().x, 60.0f, okw,
                                                            ImGui::GetStyle().ItemSpacing.x, &beside));
                const bool enter = ImGui::InputTextWithHint("##newfolder", "Folder name", g.buf, sizeof g.buf,
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
    InitSoundTracks(get);  // 13-13: the Lands-in and Switch-named tracks
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
