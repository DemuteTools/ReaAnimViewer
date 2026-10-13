// SPDX-License-Identifier: MIT
//
// See sound_place.h (13-6). The engine copies items through their state chunk (a file source:
// makes the item from the file, 13-11), never runs a REAPER action, never touches a folder item,
// never writes RAV_SOUNDS and records no undo.

#include "sound_place.h"

#ifdef _WIN32

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <system_error>
#include <vector>

#include "sound_project.h"
#include "sound_record.h"
#include "sound_tracks.h"

namespace rav {
namespace {

using PluginRegisterFn = int (*)(const char* name, void* infostruct);
using AddMediaItemToTrackFn = MediaItem* (*)(MediaTrack* tr);
using DeleteTrackMediaItemFn = bool (*)(MediaTrack* tr, MediaItem* it);
using GetItemStateChunkFn = bool (*)(MediaItem* item, char* strNeedBig, int strNeedBig_sz, bool isundo);
using PreventUIRefreshFn = void (*)(int prevent_count);
using SetItemStateChunkFn = bool (*)(MediaItem* item, const char* str, bool isundo);
using SetMediaItemInfo_ValueFn = bool (*)(MediaItem* item, const char* parmname, double newvalue);
using SetMediaItemTakeInfo_ValueFn = bool (*)(MediaItem_Take* take, const char* parmname, double newvalue);
using GetMediaItemTakeFn = MediaItem_Take* (*)(MediaItem* item, int tk);
using CountTakesFn = int (*)(MediaItem* item);
using SetActiveTakeFn = void (*)(MediaItem_Take* take);
using GetMediaSourceTypeFn = void (*)(PCM_source* source, char* typebuf, int typebuf_sz);
using MoveMediaItemToTrackFn = bool (*)(MediaItem* item, MediaTrack* desttr);
using GetConfigVarStringFn = bool (*)(const char* name, char* bufOut, int bufOut_sz);
using GetNumTakeMarkersFn = int (*)(MediaItem_Take* take);
using GetTakeMarkerFn = double (*)(MediaItem_Take* take, int idx, char* nameOut, int nameOut_sz, int* colorOutOptional);
using GetItemProjectContextFn = ReaProject* (*)(MediaItem* item);
using AddTakeToMediaItemFn = MediaItem_Take* (*)(MediaItem* item);
using PCMSourceCreateFromFileFn = PCM_source* (*)(const char* filename);
using PCMSourceDestroyFn = void (*)(PCM_source* src);
using SetMediaItemTakeSourceFn = bool (*)(MediaItem_Take* take, PCM_source* source);
using GetProjectPathExFn = void (*)(ReaProject* proj, char* buf, int buf_sz);

PluginRegisterFn             f_plugin_register = nullptr;
AddMediaItemToTrackFn        f_add_item = nullptr;
DeleteTrackMediaItemFn       f_delete_item = nullptr;
GetItemStateChunkFn          f_get_chunk = nullptr;
SetItemStateChunkFn          f_set_chunk = nullptr;
PreventUIRefreshFn           f_prevent_refresh = nullptr;
int                          g_batch = 0;  // PlaceBatch depth
std::map<MediaItem*, std::string> g_batch_chunks;  // a batch's source chunks, read once
std::map<MediaItem*, bool>        g_batch_folders;  // 13-19: a batch's IsFolderItem answers, decided once
SetMediaItemInfo_ValueFn     f_set_item = nullptr;
SetMediaItemTakeInfo_ValueFn f_set_take = nullptr;
GetMediaItemTakeFn           f_get_take = nullptr;
CountTakesFn                 f_count_takes = nullptr;
SetActiveTakeFn              f_set_active_take = nullptr;
GetMediaSourceTypeFn         f_source_type = nullptr;
MoveMediaItemToTrackFn       f_move_to_track = nullptr;
GetConfigVarStringFn         f_config_string = nullptr;
GetNumTakeMarkersFn          f_num_take_markers = nullptr;
GetTakeMarkerFn              f_get_take_marker = nullptr;
GetItemProjectContextFn      f_item_project = nullptr;
AddTakeToMediaItemFn         f_add_take = nullptr;
PCMSourceCreateFromFileFn    f_create_source = nullptr;
PCMSourceDestroyFn           f_destroy_source = nullptr;
SetMediaItemTakeSourceFn     f_set_take_source = nullptr;
GetProjectPathExFn           f_project_path = nullptr;

constexpr const char kMissingFns[] = "a REAPER function is missing";

bool EngineReady()
{
    return f_add_item && f_delete_item && f_get_chunk && f_set_chunk && f_set_item && f_set_take
        && f_get_take && f_count_takes && f_set_active_take && f_source_type && f_move_to_track;
}

std::string ItemGuid(MediaItem* item)
{
    char buf[64] = {};
    if (!item || !GetSetMediaItemInfo_String(item, "GUID", buf, false)) return {};
    return buf;
}

std::string TrackGuid(MediaTrack* track)
{
    char buf[64] = {};
    if (!track || !GetSetMediaTrackInfo_String(track, "GUID", buf, false)) return {};
    return buf;
}

bool SameGuid(const std::string& a, const std::string& b)
{
    if (a.size() != b.size() || a.empty()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        char x = a[i], y = b[i];
        if (x >= 'a' && x <= 'z') x = static_cast<char>(x - 32);
        if (y >= 'a' && y <= 'z') y = static_cast<char>(y - 32);
        if (x != y) return false;
    }
    return true;
}

std::string NewGuidString()
{
    GUID g;
    genGuid(&g);
    char buf[64] = {};
    guidToString(&g, buf);
    return buf;
}

MediaItem* (*g_item_finder)(const std::string& guid) = nullptr;

// 13-19: an item the gesture is about to delete leaves its caches (a later item may reuse the pointer).
void ForgetDeleted(MediaItem* item)
{
    ForgetLiveItem(item);
    g_batch_folders.erase(item);
}

// E11b: the active gesture's finder first (a hit still checked live), else scan the project's items.
MediaItem* FindItem(ReaProject* proj, const std::string& guid)
{
    if (guid.empty()) return nullptr;
    if (g_item_finder)
        if (MediaItem* hit = g_item_finder(guid))
            // 13-19: the gesture's live-item cache for the current project (ValidatePtr2 walks its items).
            if ((proj ? ValidatePtr2(proj, hit, "MediaItem*") : IsLiveItem(hit)) && SameGuid(ItemGuid(hit), guid))
                return hit;
    const int n = CountMediaItems(proj);
    for (int i = 0; i < n; ++i) {
        MediaItem* it = GetMediaItem(proj, i);
        if (it && SameGuid(ItemGuid(it), guid)) return it;
    }
    return nullptr;
}

// The item's state chunk: a doubled buffer from 64 KB to 64 MB while the text fills it.
// True when both files hold the same bytes (read in 64 KB blocks; false on any read error).
bool SameBytes(const std::filesystem::path& a, const std::filesystem::path& b)
{
    std::ifstream fa(a, std::ios::binary), fb(b, std::ios::binary);
    if (!fa || !fb) return false;
    std::vector<char> ba(65536), bb(65536);
    for (;;) {
        fa.read(ba.data(), static_cast<std::streamsize>(ba.size()));
        fb.read(bb.data(), static_cast<std::streamsize>(bb.size()));
        const std::streamsize na = fa.gcount(), nb = fb.gcount();
        if (na != nb || std::memcmp(ba.data(), bb.data(), static_cast<size_t>(na)) != 0) return false;
        if (na == 0 || !fa || !fb) return na == nb && fa.eof() == fb.eof();
    }
}

std::string ReadChunk(MediaItem* item)
{
    if (!f_get_chunk || !item) return {};
    for (int size = 64 * 1024; size <= 64 * 1024 * 1024; size *= 2) {
        std::vector<char> buf(static_cast<size_t>(size), '\0');
        if (!f_get_chunk(item, buf.data(), size, false)) return {};
        const size_t len = std::strlen(buf.data());
        if (len + 1 < static_cast<size_t>(size)) return std::string(buf.data(), len);
    }
    return {};
}

// The child tracks of a folder track (until its folder closes).
std::vector<MediaTrack*> ChildTracks(ReaProject* proj, MediaTrack* folder)
{
    std::vector<MediaTrack*> out;
    if (!folder) return out;
    const int idx = static_cast<int>(GetMediaTrackInfo_Value(folder, "IP_TRACKNUMBER")) - 1;
    if (idx < 0) return out;
    int       depth = 1;
    const int n = CountTracks(proj);
    for (int j = idx + 1; j < n && depth > 0; ++j) {
        MediaTrack* t = GetTrack(proj, j);
        if (!t) break;
        out.push_back(t);
        depth += static_cast<int>(GetMediaTrackInfo_Value(t, "I_FOLDERDEPTH"));
    }
    return out;
}

bool EmptySource(MediaItem* item)
{
    MediaItem_Take* take = GetActiveTake(item);
    if (!take) return true;
    PCM_source* src = GetMediaItemTake_Source(take);
    if (!src) return true;
    if (!f_source_type) return false;
    char type[64] = {};
    f_source_type(src, type, sizeof(type));
    return type[0] == '\0' || std::strcmp(type, "EMPTY") == 0;
}

FolderItemFacts ReadFolderFacts(MediaItem* item)
{
    FolderItemFacts f;
    MediaTrack* track = GetMediaItem_Track(item);
    if (!track) return f;
    f.on_folder_track = GetMediaTrackInfo_Value(track, "I_FOLDERDEPTH") == 1.0;
    if (!f.on_folder_track) return f;
    f.empty_source = EmptySource(item);
    if (!f.empty_source) return f;
    const double pos = GetMediaItemInfo_Value(item, "D_POSITION");
    const double len = GetMediaItemInfo_Value(item, "D_LENGTH");
    // The item's own project (the active tab only when REAPER cannot tell).
    ReaProject*  proj = f_item_project ? f_item_project(item) : nullptr;
    if (!proj) proj = EnumProjects(-1, nullptr, 0);
    for (MediaTrack* t : ChildTracks(proj, track)) {
        const int n = CountTrackMediaItems(t);
        for (int i = 0; i < n && !f.children_inside; ++i) {
            MediaItem* c = GetTrackMediaItem(t, i);
            if (c && SpanInside(pos, len, GetMediaItemInfo_Value(c, "D_POSITION"), GetMediaItemInfo_Value(c, "D_LENGTH")))
                f.children_inside = true;
        }
        if (f.children_inside) break;
    }
    f.nvk_item_type = NvkItemTypeFromChunk(ReadChunk(item));
    return f;
}

// One item to copy and where.
struct Member {
    MediaItem*  src = nullptr;
    MediaTrack* track = nullptr;
    std::string mark;  // 13-16: the RAV track's mark ("" = no spill, no record)
};

// E1: the items an entry places (the source, or a folder item's children inside its span).
bool CollectMembers(ReaProject* proj, const PlaceRequest& req, MediaItem** anchor_out, bool* folder_out,
                    std::vector<Member>* out, std::string* error)
{
    if (req.source.kind != SoundSourceKind::Item) {
        *error = "a file source is placed from its file";
        return false;
    }
    MediaItem* src = FindItem(proj, req.source.item_guid);
    if (!src) {
        *error = "source missing";
        return false;
    }
    *anchor_out = src;
    *folder_out = IsFolderItem(src);
    if (!*folder_out) {
        if (req.track) out->push_back({src, req.track, req.track_mark});
        else out->push_back({src, GetMediaItem_Track(src), {}});
        return true;
    }
    const double pos = GetMediaItemInfo_Value(src, "D_POSITION");
    const double len = GetMediaItemInfo_Value(src, "D_LENGTH");
    for (MediaTrack* t : ChildTracks(proj, GetMediaItem_Track(src))) {
        const int n = CountTrackMediaItems(t);
        for (int i = 0; i < n; ++i) {
            MediaItem* c = GetTrackMediaItem(t, i);
            if (!c || IsFolderItem(c)) continue;  // nested folder items skipped
            PlacedTag placed_tag;
            if (ReadPlacedTag(c, &placed_tag)) continue;  // an earlier placement, not part of the source
            if (!SpanInside(pos, len, GetMediaItemInfo_Value(c, "D_POSITION"), GetMediaItemInfo_Value(c, "D_LENGTH")))
                continue;
            Member m{c, t, {}};
            if (req.pile_track) {  // 13-16 B5: the member's own child of the layer track
                std::string mark;
                if (MediaTrack* to = req.pile_track(t, &mark)) {
                    m.track = to;
                    m.mark = mark;
                }
            } else if (req.track) {
                m.track = req.track;
                m.mark = req.track_mark;
            }
            out->push_back(m);
        }
    }
    if (out->empty()) {
        *error = "folder item has no child items";
        return false;
    }
    return true;
}

// E3: the variation on a copied plain item.
void ApplyVariation(MediaItem* item, const PlaceRequest& req)
{
    if (req.slice.set) {
        MediaItem_Take* take = GetActiveTake(item);
        if (take) {
            double rate = GetMediaItemTakeInfo_Value(take, "D_PLAYRATE");
            if (!(rate > 0.0)) rate = 1.0;
            f_set_take(take, "D_STARTOFFS", req.slice.start_offs);
            f_set_item(item, "D_LENGTH", req.slice.length / rate);
        }
        f_set_item(item, "D_SNAPOFFSET", req.slice.snap_offset);
        return;
    }
    if (req.variation >= 0 && req.variation < f_count_takes(item)) {
        MediaItem_Take* take = f_get_take(item, req.variation);
        if (take) f_set_active_take(take);
    }
}

// Antho 2026-10-12: the variation's fades on a copied plain item (after ApplyVariation: the length is final).
void ApplyFades(MediaItem* item, const FadeApply& fa)
{
    if (!fa.set_in && !fa.set_out) return;
    MediaItem_Take* take = GetActiveTake(item);
    const double    rate = take ? GetMediaItemTakeInfo_Value(take, "D_PLAYRATE") : 1.0;
    double          in = 0.0, out = 0.0;
    FadesOnItem(fa, rate, GetMediaItemInfo_Value(item, "D_LENGTH"), GetMediaItemInfo_Value(item, "D_FADEINLEN"),
                GetMediaItemInfo_Value(item, "D_FADEOUTLEN"), &in, &out);
    f_set_item(item, "D_FADEINLEN", in);
    f_set_item(item, "D_FADEOUTLEN", out);
    if (fa.set_in) {
        f_set_item(item, "D_FADEINLEN_AUTO", -1.0);  // the source's auto-fade would override the length
        f_set_item(item, "D_FADEINDIR", 0.0);
        if (fa.shape_in) f_set_item(item, "C_FADEINSHAPE", static_cast<double>(fa.in_shape));
    }
    if (fa.set_out) {
        f_set_item(item, "D_FADEOUTLEN_AUTO", -1.0);
        f_set_item(item, "D_FADEOUTDIR", 0.0);
        if (fa.shape_out) f_set_item(item, "C_FADEOUTSHAPE", static_cast<double>(fa.out_shape));
    }
}

// Writes `src`'s chunk into `dst` with the given IGUID.
bool WriteCopy(MediaItem* src, MediaItem* dst, const std::string& guid)
{
    std::string chunk;
    if (g_batch > 0) {
        auto hit = g_batch_chunks.find(src);
        if (hit == g_batch_chunks.end()) hit = g_batch_chunks.emplace(src, ReadChunk(src)).first;
        chunk = hit->second;
    } else {
        chunk = ReadChunk(src);
    }
    if (chunk.empty()) return false;
    const std::string copy = PrepareCopyChunk(chunk, guid, &NewGuidString);
    return f_set_chunk(dst, copy.c_str(), false);
}

// 13-16 (B7b): a kept item whose tag records a RAV track it is not on (the track RAV uses for that
// mark: a copy of it carrying the same mark is not it) was moved by the user.
bool MovedByUser(ReaProject* proj, MediaItem* kept, const std::string& kept_tk)
{
    return kept && PlacedSpotOf(kept_tk, GetMediaItem_Track(kept) == FindTrackByMark(proj, kept_tk)) ==
                       PlacedSpot::MovedByUser;
}

// 13-16 (B6): where a placed item lands: settled on its RAV track's lanes (its mark returned), or
// the lane cache told of it ("" or the kept mark returned).
std::string Land(ReaProject* proj, MediaItem* item, MediaTrack* track, const std::string& mark, bool user_moved,
                 const std::string& kept_tk)
{
    if (user_moved) {
        RefreshLaneItem(item);
        return kept_tk;
    }
    if (mark.empty()) {
        RefreshLaneItem(item);
        return {};
    }
    const std::string landed = SettleOnLanes(proj, item, track, mark);
    return landed.empty() ? mark : landed;
}

std::string OwnerGuid(const PlaceRequest& req, const std::string& fallback)
{
    if (req.owner) return ItemGuid(req.owner);
    return fallback;
}

// Antho 2026-10-12 (13-11): a File Sound's item, made straight from its file on `req.track` (no
// source item). `kept` non-null = rewritten in place (its GUID kept, its takes replaced). 13-20 / 13-23:
// one item per piece (req.pieces; none = one item from req.slice), all positioned from the anchor
// piece; `kept` becomes the first.
PlaceResult PlaceFileImpl(ReaProject* proj, const PlaceRequest& req, MediaItem* kept, const std::string& kept_owner,
                          const std::string& kept_tk)
{
    PlaceResult r;
    if (!f_add_take || !f_create_source || !f_set_take_source) {
        r.error = kMissingFns;
        return r;
    }
    if (req.file_path.empty() || !req.track || !ValidatePtr2(proj, req.track, "MediaTrack*")) {
        r.error = req.file_path.empty() ? "no file to place" : "no track to place the file on";
        return r;
    }
    std::vector<PieceApply> pieces = req.pieces;
    if (pieces.empty()) {
        PieceApply one;
        one.slice = req.slice;
        one.fades = req.fades;
        one.anchor = true;
        pieces.push_back(one);
    }
    double file_len = 0.0;
    {
        PCM_source* probe = f_create_source(req.file_path.c_str());
        if (!probe) {
            r.error = "could not read the file";
            return r;
        }
        bool qn = false;
        file_len = GetMediaSourceLength(probe, &qn);
        if (qn) file_len = 0.0;
        if (f_destroy_source) f_destroy_source(probe);
    }
    std::vector<FileItemFields> fields;
    size_t                      anchor = 0;
    for (size_t k = 0; k < pieces.size(); ++k) {
        fields.push_back(FileItemFieldsFor(file_len, pieces[k].slice, pieces[k].fades, req.gain_db, req.project_t,
                                           req.blend_ms, req.offset_s));
        if (pieces[k].anchor) anchor = k;
    }
    if (!fields[anchor].ok) {
        r.error = "the variation lies outside the file";
        return r;
    }
    const double anchor_pos = fields[anchor].position;
    const double sync = fields[anchor].snap_offset;
    const bool   user_moved = MovedByUser(proj, kept, kept_tk);
    const std::string owner = kept_owner.empty() ? OwnerGuid(req, {}) : OwnerGuid(req, kept_owner);
    std::string name = std::filesystem::u8path(req.file_path).filename().u8string();
    if (name.empty()) name = req.file_path;

    for (size_t k = 0; k < pieces.size(); ++k) {
        const FileItemFields& fld = fields[k];
        if (!fld.ok) continue;  // a piece outside the file: the others still play
        PCM_source* src = f_create_source(req.file_path.c_str());
        if (!src) {
            r.error = "could not read the file";
            return r;
        }
        auto drop_source = [&src]() {
            if (src && f_destroy_source) f_destroy_source(src);
            src = nullptr;
        };
        MediaItem*  reuse = r.items.empty() ? kept : nullptr;  // the kept item becomes the first piece
        MediaItem*  item = reuse;
        std::string saved;
        if (reuse) {
            if (!user_moved && GetMediaItem_Track(reuse) != req.track && !f_move_to_track(reuse, req.track)) {
                drop_source();
                r.error = "could not move the kept item to its track";
                return r;
            }
            // A fresh item with the same GUID: the old takes, their sources and FX go. The old
            // chunk is kept so a failure below puts the item back as it was.
            saved = ReadChunk(reuse);
            const std::string fresh = "<ITEM\nPOSITION 0\nSNAPOFFS 0\nLENGTH 1\nLOOP 0\nALLTAKES 0\nMUTE 0 0\nSEL 0\nIGUID " +
                                      ItemGuid(reuse) + "\n>\n";
            if (!f_set_chunk(reuse, fresh.c_str(), false)) {
                drop_source();
                r.error = "could not rewrite the kept item";
                return r;
            }
        } else {
            item = f_add_item(req.track);
            if (!item) {
                drop_source();
                r.error = "could not add an item";
                return r;
            }
        }
        MediaItem_Take* take = f_add_take(item);
        if (!take || !f_set_take_source(take, src)) {
            drop_source();
            if (!reuse) {
                ForgetDeleted(item);
                f_delete_item(req.track, item);
            } else if (!saved.empty()) {
                f_set_chunk(reuse, saved.c_str(), false);  // back as it was
            }
            r.error = "could not give the item its file";
            return r;
        }
        src = nullptr;  // the take owns it now
        f_set_active_take(take);
        {
            std::vector<char> buf(name.begin(), name.end());
            buf.push_back('\0');
            GetSetMediaItemTakeInfo_String(take, "P_NAME", buf.data(), true);
        }
        const double pos = k == anchor ? anchor_pos : PiecePosition(anchor_pos, pieces[k].from_anchor, 1.0);
        f_set_take(take, "D_STARTOFFS", fld.start_offs);
        f_set_take(take, "D_PLAYRATE", 1.0);
        f_set_item(item, "B_LOOPSRC", 0.0);
        f_set_item(item, "B_MUTE", 0.0);
        f_set_item(item, "D_LENGTH", fld.length);
        f_set_item(item, "D_SNAPOFFSET", k == anchor ? fld.snap_offset : 0.0);
        f_set_item(item, "D_POSITION", pos);
        f_set_item(item, "D_VOL", fld.volume);
        f_set_item(item, "D_FADEINLEN", fld.fade_in);
        f_set_item(item, "D_FADEOUTLEN", fld.fade_out);
        f_set_item(item, "D_FADEINLEN_AUTO", -1.0);  // REAPER's auto-fade would override the length
        f_set_item(item, "D_FADEOUTLEN_AUTO", -1.0);
        f_set_item(item, "D_FADEINDIR", 0.0);
        f_set_item(item, "D_FADEOUTDIR", 0.0);
        if (pieces[k].fades.shape_in) f_set_item(item, "C_FADEINSHAPE", static_cast<double>(pieces[k].fades.in_shape));
        if (pieces[k].fades.shape_out) f_set_item(item, "C_FADEOUTSHAPE", static_cast<double>(pieces[k].fades.out_shape));

        PlacedTag tag;
        tag.owner = owner;
        tag.rid = req.rid;
        tag.t = req.event_t;
        tag.node = req.node;
        tag.own = req.own;
        tag.has_anchor = true;
        tag.ao = pos - anchor_pos;
        tag.sy = sync;
        tag.pt = req.part;
        tag.tk = Land(proj, item, req.track, req.track_mark, reuse != nullptr && user_moved, kept_tk);
        WritePlacedTag(item, tag);
        r.item_guids.push_back(ItemGuid(item));
        r.items.push_back(item);
    }
    r.ok = !r.items.empty();
    if (!r.ok && r.error.empty()) r.error = "the variation lies outside the file";
    return r;
}

// 13-20 / 13-23: one piece of a variation on a copied plain item: its span (D_STARTOFFS, D_LENGTH in
// item seconds), its snap (the anchor piece only) and its fades.
void ApplyPiece(MediaItem* item, const PieceApply& piece)
{
    MediaItem_Take* take = GetActiveTake(item);
    double          rate = take ? GetMediaItemTakeInfo_Value(take, "D_PLAYRATE") : 1.0;
    if (!(rate > 0.0) || !std::isfinite(rate)) rate = 1.0;
    if (take) {
        f_set_take(take, "D_STARTOFFS", piece.slice.start_offs);
        f_set_item(item, "D_LENGTH", piece.slice.length / rate);
    }
    f_set_item(item, "D_SNAPOFFSET", piece.anchor ? piece.slice.snap_offset / rate : 0.0);
    ApplyFades(item, piece.fades);
}

// The shared place/replace body. `kept` non-null = rewrite it as the first member.
PlaceResult PlaceImpl(ReaProject* proj, const PlaceRequest& req, MediaItem* kept, const std::string& kept_owner,
                      const std::string& kept_tk)
{
    PlaceResult r;
    if (!EngineReady()) {
        r.error = kMissingFns;
        return r;
    }
    if (req.source.kind == SoundSourceKind::File) return PlaceFileImpl(proj, req, kept, kept_owner, kept_tk);
    const bool user_moved = MovedByUser(proj, kept, kept_tk);  // 13-16 B7b: the kept item stays on its track
    MediaItem*          anchor_src = nullptr;
    bool                folder = false;
    std::vector<Member> members;
    if (!CollectMembers(proj, req, &anchor_src, &folder, &members, &r.error)) return r;
    // 13-20 / 13-23: a plain item is placed as its pieces (one copy each); a folder pile ignores them.
    const bool by_pieces = !folder && !req.pieces.empty();
    if (by_pieces) members.assign(req.pieces.size(), members.front());

    const double src_anchor_pos = GetMediaItemInfo_Value(anchor_src, "D_POSITION");
    const std::string owner = kept_owner.empty() ? OwnerGuid(req, req.source.item_guid) : OwnerGuid(req, kept_owner);
    const double gain = ItemGainFor(req.gain_db);

    // Copy every member first, apply the variation, then position from the anchor's snap.
    std::vector<MediaItem*> placed;
    for (size_t i = 0; i < members.size(); ++i) {
        const Member& m = members[i];
        MediaItem*    item = nullptr;
        std::string   guid;
        if (i == 0 && kept) {
            item = kept;
            guid = ItemGuid(kept);
            if (!user_moved && GetMediaItem_Track(kept) != m.track && !f_move_to_track(kept, m.track)) {
                r.error = "could not move the kept item to the source track";
                return r;
            }
        } else {
            item = f_add_item(m.track);
            guid = NewGuidString();
        }
        if (!item) {
            r.error = "could not add an item";
            return r;
        }
        if (!WriteCopy(m.src, item, guid)) {
            if (!(i == 0 && kept)) {
                ForgetDeleted(item);
                f_delete_item(m.track, item);
            }
            r.error = "could not copy the source item";
            return r;
        }
        if (by_pieces) {
            ApplyPiece(item, req.pieces[i]);
        } else if (!folder) {
            ApplyVariation(item, req);
            ApplyFades(item, req.fades);
        }
        {  // 13-11 (B9): a library source is muted; its copy plays (and does not carry the mark)
            char mark[64] = {};
            const bool lib_src = GetSetMediaItemInfo_String(m.src, kLibSourceTagKey, mark, false) && mark[0];
            if (lib_src) {
                char none[1] = {};
                GetSetMediaItemInfo_String(item, kLibSourceTagKey, none, true);
                f_set_item(item, "B_MUTE",
                           PlacedCopyMuted(GetMediaItemInfo_Value(m.src, "B_MUTE") != 0.0, true) ? 1.0 : 0.0);
            }
        }
        // Antho 2026-10-13: the level heard in RAV is the level placed, so a copied item plays at the
        // library's gain alone, never on top of its source item's own volume (an item placed earlier and
        // taken as a source gave +24.3 dB in the library, +47 dB on the track). A folder pile keeps its
        // members' volumes: they are the balance inside the pile.
        f_set_item(item, "D_VOL", folder ? GetMediaItemInfo_Value(m.src, "D_VOL") * gain : gain);
        placed.push_back(item);
        r.item_guids.push_back(ItemGuid(item));
        r.items.push_back(item);
    }

    // E2: sync = the source (topmost) item's snap, read after the variation. 13-20 / 13-23: the anchor
    // piece's (the one holding the sync point); every piece is placed from it.
    size_t anchor_piece = 0;
    if (by_pieces)
        for (size_t i = 0; i < req.pieces.size(); ++i)
            if (req.pieces[i].anchor) anchor_piece = i;
    const double sync = folder ? GetMediaItemInfo_Value(anchor_src, "D_SNAPOFFSET")
                               : GetMediaItemInfo_Value(placed[anchor_piece], "D_SNAPOFFSET");
    const double anchor = PlacementPosition(req.project_t, sync, req.blend_ms, req.offset_s);
    for (size_t i = 0; i < placed.size(); ++i) {
        const double src_pos = GetMediaItemInfo_Value(members[i].src, "D_POSITION");
        double       pos = anchor;
        if (folder) {
            pos = std::max(0.0, PileMemberPosition(anchor, src_anchor_pos, src_pos));
        } else if (by_pieces && i != anchor_piece) {
            MediaItem_Take* take = GetActiveTake(placed[i]);
            pos = PiecePosition(anchor, req.pieces[i].from_anchor, take ? GetMediaItemTakeInfo_Value(take, "D_PLAYRATE") : 1.0);
        }
        f_set_item(placed[i], "D_POSITION", pos);
        PlacedTag tag;
        tag.owner = owner;
        tag.rid = req.rid;
        tag.t = req.event_t;
        tag.node = req.node;
        tag.own = req.own;
        tag.has_anchor = true;
        tag.ao = pos - anchor;  // as placed: a member clamped at 0 still gives the anchor back
        tag.sy = sync;
        tag.pt = req.part;
        tag.tk = Land(proj, placed[i], members[i].track, members[i].mark, i == 0 && user_moved, kept_tk);
        WritePlacedTag(placed[i], tag);
    }
    r.ok = true;
    return r;
}

// E7: a placed item we may change. `edit` = replace/remove (attached refused).
MediaItem* PlacedItemFor(ReaProject* proj, const std::string& guid, bool edit, PlacedTag* tag_out, std::string* error)
{
    MediaItem*        item = FindItem(proj, guid);
    PlacedTag         tag;
    const bool        has_tag = item && ReadPlacedTag(item, &tag);
    const std::string why = PlacedItemRefusal(item != nullptr, has_tag, has_tag && IsFolderItem(item), tag.own, edit);
    if (!why.empty()) {
        *error = why;
        return nullptr;
    }
    if (tag_out) *tag_out = tag;
    return item;
}

// ---- Dev action "RAV: Placement engine test" ------------------------------------------------

int               g_test_cmd = 0;
gaccel_register_t g_test_accel = {};
std::string       g_last_inputs = "place,0,0,0,0";

std::string Fmt(const char* fmt, double v)
{
    char buf[64];
    std::snprintf(buf, sizeof(buf), fmt, v);
    return buf;
}

std::string DescribeItem(ReaProject* proj, const std::string& guid, const char* note)
{
    MediaItem* item = FindItem(proj, guid);
    if (!item) return guid + ": gone\n";
    std::string s = guid;
    if (note && *note) s += std::string(" (") + note + ")";
    MediaTrack* tr = GetMediaItem_Track(item);
    s += "  track " + std::to_string(tr ? static_cast<int>(GetMediaTrackInfo_Value(tr, "IP_TRACKNUMBER")) : 0);
    s += "  pos " + Fmt("%.3f", GetMediaItemInfo_Value(item, "D_POSITION"));
    s += "  take " + std::to_string(static_cast<int>(GetMediaItemInfo_Value(item, "I_CURTAKE")) + 1);
    MediaItem_Take* take = GetActiveTake(item);
    s += "  startoffs " + Fmt("%.3f", take ? GetMediaItemTakeInfo_Value(take, "D_STARTOFFS") : 0.0);
    s += "  len " + Fmt("%.3f", GetMediaItemInfo_Value(item, "D_LENGTH"));
    const double vol = GetMediaItemInfo_Value(item, "D_VOL");
    s += "  vol " + (vol > 0.0 ? Fmt("%.2f dB", 20.0 * std::log10(vol)) : std::string("-inf dB"));
    char tag[1024] = {};
    if (GetSetMediaItemInfo_String(item, kPlacedTagKey, tag, false) && tag[0]) s += std::string("\n   tag: ") + tag;
    return s + "\n";
}

std::vector<std::string> SplitCsv(const std::string& s)
{
    std::vector<std::string> out;
    size_t                   start = 0;
    for (;;) {
        const size_t c = s.find(',', start);
        out.push_back(s.substr(start, c == std::string::npos ? std::string::npos : c - start));
        if (c == std::string::npos) break;
        start = c + 1;
    }
    while (out.size() < 5) out.push_back("0");
    for (std::string& v : out) {
        while (!v.empty() && v.front() == ' ') v.erase(v.begin());
        while (!v.empty() && v.back() == ' ') v.pop_back();
    }
    return out;
}

// [take marker n, marker n+1 or source end) of the item's active take.
SliceApply SliceForMarker(MediaItem* item, int n)
{
    SliceApply none;
    if (n <= 0 || !f_num_take_markers || !f_get_take_marker) return none;
    MediaItem_Take* take = GetActiveTake(item);
    if (!take) return none;
    const int count = f_num_take_markers(take);
    if (n > count) return none;
    char         name[256];
    const double start = f_get_take_marker(take, n - 1, name, sizeof(name), nullptr);
    double       end = 0.0;
    if (n < count) {
        end = f_get_take_marker(take, n, name, sizeof(name), nullptr);
    } else {
        PCM_source* src = GetMediaItemTake_Source(take);
        bool        qn = false;
        end = src ? GetMediaSourceLength(src, &qn) : 0.0;
    }
    SoundFrame f;
    f.start = start;
    f.length = end - start;
    return SliceApplyFor(f);
}

void RunPlacementTest()
{
    ReaProject* proj = EnumProjects(-1, nullptr, 0);
    char        values[1024];
    std::snprintf(values, sizeof(values), "%s", g_last_inputs.c_str());
    if (!GetUserInputs("RAV placement engine test", 5,
                       "Step (place/move/replace/remove),Take # (1..n; 0 = keep),Take-marker slice # (0 = none),"
                       "Gain dB,Blend offset ms",
                       values, sizeof(values)))
        return;
    g_last_inputs = values;
    const std::vector<std::string> in = SplitCsv(values);
    const std::string step = in[0];
    const int    take_no = std::atoi(in[1].c_str());
    const int    slice_no = std::atoi(in[2].c_str());
    const double gain_db = std::atof(in[3].c_str());
    const double blend_ms = std::atof(in[4].c_str());

    std::vector<MediaItem*> placed, sources;
    const int               nsel = CountSelectedMediaItems(proj);
    for (int i = 0; i < nsel; ++i) {
        MediaItem* it = GetSelectedMediaItem(proj, i);
        if (!it) continue;
        PlacedTag tag;
        (ReadPlacedTag(it, &tag) ? placed : sources).push_back(it);
    }

    std::string report = std::string("copyimpmedia ") + (ReadCopyImportedMedia() ? "on" : "off") + "\n";
    report += "step: " + step + "\n\n";
    {
        const std::string undo = "RAV: Placement engine test (" + step + ")";
        SoundUndoScope    scope(proj, undo.c_str());
        if (step == "place") {
            if (sources.empty()) {
                report += "select a source item (one without a RAV_SND tag)\n";
            } else {
                PlaceRequest req;
                req.rid = "test";
                req.node = "TEST";
                req.own = Ownership::Populated;
                req.event_t = req.project_t = GetCursorPositionEx(proj);
                req.source.kind = SoundSourceKind::Item;
                req.source.item_guid = ItemGuid(sources.front());
                req.variation = take_no - 1;
                req.slice = SliceForMarker(sources.front(), slice_no);
                req.gain_db = gain_db;
                req.blend_ms = blend_ms;
                const PlaceResult r = PlaceSounds(proj, req);
                if (!r.ok) report += "refused: " + r.error + "\n";
                for (const std::string& g : r.item_guids) report += DescribeItem(proj, g, "new");
            }
        } else if (step == "move") {
            if (placed.empty()) {
                report += "select a placed item\n";
            } else {
                const double delta = GetCursorPositionEx(proj) - GetMediaItemInfo_Value(placed.front(), "D_POSITION");
                for (MediaItem* it : placed) {
                    const std::string g = ItemGuid(it);
                    const bool ok = MovePlacement(proj, g, GetMediaItemInfo_Value(it, "D_POSITION") + delta);
                    report += ok ? DescribeItem(proj, g, "same GUID") : g + ": refused\n";
                }
            }
        } else if (step == "replace") {
            if (placed.empty() || sources.empty()) {
                report += "select a placed item and a source item\n";
            } else {
                MediaItem*        old = placed.front();
                const std::string g = ItemGuid(old);
                PlacedTag         tag;
                ReadPlacedTag(old, &tag);
                PlaceRequest req;
                req.rid = tag.rid;
                req.node = tag.node;
                req.own = tag.own;
                req.event_t = req.project_t = tag.t;
                req.source.kind = SoundSourceKind::Item;
                req.source.item_guid = ItemGuid(sources.front());
                req.variation = take_no - 1;
                req.slice = SliceForMarker(sources.front(), slice_no);
                req.gain_db = gain_db;
                req.blend_ms = blend_ms;
                req.offset_s = (GetMediaItemInfo_Value(old, "D_POSITION") + GetMediaItemInfo_Value(old, "D_SNAPOFFSET"))
                             - (tag.t + blend_ms / 1000.0);
                const PlaceResult r = ReplacePlacement(proj, g, req);
                if (!r.ok) report += g + ": refused: " + r.error + "\n";
                for (size_t i = 0; i < r.item_guids.size(); ++i)
                    report += DescribeItem(proj, r.item_guids[i], i == 0 && SameGuid(r.item_guids[i], g) ? "same GUID" : "new");
            }
        } else if (step == "remove") {
            std::vector<std::string> guids;
            for (int i = 0; i < nsel; ++i) guids.push_back(ItemGuid(GetSelectedMediaItem(proj, i)));
            for (const std::string& g : guids) {
                PlacedTag   tag;
                std::string why;
                if (!PlacedItemFor(proj, g, true, &tag, &why)) {
                    report += g + ": refused: " + why + "\n";
                    continue;
                }
                report += RemovePlacement(proj, g) ? g + ": removed\n" : g + ": refused\n";
            }
        } else {
            report += "unknown step (place / move / replace / remove)\n";
        }
    }
    UpdateArrange();
    ShowMessageBox(report.c_str(), "RAV placement engine test", 0);
}

bool OnPlaceTestCommand(int command, int /*flag*/)
{
    if (command == 0 || command != g_test_cmd) return false;
    try {
        RunPlacementTest();
    } catch (...) {
    }
    return true;
}

}  // namespace

void PlaceBatch(bool on)
{
    if (on) {
        TracksBatch(true);     // 13-16: the gesture's track marks and lane items, read once
        LiveItemsBatch(true);  // 13-19: each item checked live once
        if (g_batch++ == 0 && f_prevent_refresh) f_prevent_refresh(1);
        return;
    }
    if (g_batch <= 0) return;
    TracksBatch(false);
    LiveItemsBatch(false);
    if (--g_batch == 0) {
        g_batch_chunks.clear();
        g_batch_folders.clear();
        if (f_prevent_refresh) f_prevent_refresh(-1);
    }
}

void InitSoundPlace(void* (*get_func)(const char* name))
{
    if (!get_func) return;
    f_plugin_register = reinterpret_cast<PluginRegisterFn>(get_func("plugin_register"));
    f_add_item = reinterpret_cast<AddMediaItemToTrackFn>(get_func("AddMediaItemToTrack"));
    f_delete_item = reinterpret_cast<DeleteTrackMediaItemFn>(get_func("DeleteTrackMediaItem"));
    f_get_chunk = reinterpret_cast<GetItemStateChunkFn>(get_func("GetItemStateChunk"));
    f_set_chunk = reinterpret_cast<SetItemStateChunkFn>(get_func("SetItemStateChunk"));
    f_prevent_refresh = reinterpret_cast<PreventUIRefreshFn>(get_func("PreventUIRefresh"));
    f_set_item = reinterpret_cast<SetMediaItemInfo_ValueFn>(get_func("SetMediaItemInfo_Value"));
    f_set_take = reinterpret_cast<SetMediaItemTakeInfo_ValueFn>(get_func("SetMediaItemTakeInfo_Value"));
    f_get_take = reinterpret_cast<GetMediaItemTakeFn>(get_func("GetMediaItemTake"));
    f_count_takes = reinterpret_cast<CountTakesFn>(get_func("CountTakes"));
    f_set_active_take = reinterpret_cast<SetActiveTakeFn>(get_func("SetActiveTake"));
    f_source_type = reinterpret_cast<GetMediaSourceTypeFn>(get_func("GetMediaSourceType"));
    f_move_to_track = reinterpret_cast<MoveMediaItemToTrackFn>(get_func("MoveMediaItemToTrack"));
    f_config_string = reinterpret_cast<GetConfigVarStringFn>(get_func("get_config_var_string"));
    f_num_take_markers = reinterpret_cast<GetNumTakeMarkersFn>(get_func("GetNumTakeMarkers"));
    f_get_take_marker = reinterpret_cast<GetTakeMarkerFn>(get_func("GetTakeMarker"));
    f_item_project = reinterpret_cast<GetItemProjectContextFn>(get_func("GetItemProjectContext"));
    f_add_take = reinterpret_cast<AddTakeToMediaItemFn>(get_func("AddTakeToMediaItem"));
    f_create_source = reinterpret_cast<PCMSourceCreateFromFileFn>(get_func("PCM_Source_CreateFromFile"));
    f_destroy_source = reinterpret_cast<PCMSourceDestroyFn>(get_func("PCM_Source_Destroy"));
    f_set_take_source = reinterpret_cast<SetMediaItemTakeSourceFn>(get_func("SetMediaItemTake_Source"));
    f_project_path = reinterpret_cast<GetProjectPathExFn>(get_func("GetProjectPathEx"));

    // E11: the dev action, registered here (no frozen file changes).
    if (f_plugin_register && g_test_cmd == 0) {
        g_test_cmd = f_plugin_register("command_id", const_cast<char*>("RAV_SOUND_PLACE_TEST"));
        if (g_test_cmd) {
            g_test_accel.accel.cmd = static_cast<WORD>(g_test_cmd);
            g_test_accel.desc = "RAV: Placement engine test";
            f_plugin_register("gaccel", &g_test_accel);
            f_plugin_register("hookcommand", reinterpret_cast<void*>(&OnPlaceTestCommand));
        }
    }
}

bool ReadItemFacts(MediaItem* item, ItemFacts* out)
{
    if (!out) return false;
    *out = ItemFacts{};
    if (!item) return false;
    try {
        out->guid = ItemGuid(item);
        out->track_guid = TrackGuid(GetMediaItem_Track(item));
        out->position = GetMediaItemInfo_Value(item, "D_POSITION");
        out->length = GetMediaItemInfo_Value(item, "D_LENGTH");
        out->snap_offset = GetMediaItemInfo_Value(item, "D_SNAPOFFSET");
        out->volume = GetMediaItemInfo_Value(item, "D_VOL");
        MediaItem_Take* take = GetActiveTake(item);
        out->playrate = take ? GetMediaItemTakeInfo_Value(take, "D_PLAYRATE") : 1.0;
        out->takes = f_count_takes ? f_count_takes(item) : 0;
        out->cur_take = static_cast<int>(GetMediaItemInfo_Value(item, "I_CURTAKE"));
        out->folder = ReadFolderFacts(item);
        return !out->guid.empty();
    } catch (...) {
        return false;
    }
}

bool IsFolderItem(MediaItem* item)
{
    if (!item) return false;
    try {
        // 13-19: decided once a batch (a folder pile's check scans its child tracks and reads its chunk).
        if (g_batch > 0) {
            auto hit = g_batch_folders.find(item);
            if (hit != g_batch_folders.end()) return hit->second;
            const bool folder = IsFolderItemRule(ReadFolderFacts(item));
            g_batch_folders.emplace(item, folder);
            return folder;
        }
        return IsFolderItemRule(ReadFolderFacts(item));
    } catch (...) {
        return false;
    }
}

void SetPlaceItemFinder(MediaItem* (*finder)(const std::string& guid))
{
    g_item_finder = finder;
}

bool PlacedPileAnchor(MediaItem* item, double* anchor_pos, double* sync)
{
    if (!item || !anchor_pos || !sync) return false;
    try {
        PlacedTag tag;
        if (!ReadPlacedTag(item, &tag)) tag = PlacedTag();  // untagged: its own position and snap
        PlacedAnchorOf(tag, GetMediaItemInfo_Value(item, "D_POSITION"), GetMediaItemInfo_Value(item, "D_SNAPOFFSET"),
                       anchor_pos, sync);
        return true;
    } catch (...) {
        return false;
    }
}

PlaceResult PlaceSounds(ReaProject* proj, const PlaceRequest& req)
{
    try {
        return PlaceImpl(proj, req, nullptr, {}, {});
    } catch (...) {
        PlaceResult r;
        r.error = "placement failed";
        return r;
    }
}

bool MovePlacement(ReaProject* proj, const std::string& item_guid, double project_t)
{
    try {
        if (!f_set_item) return false;
        std::string why;
        MediaItem*  item = PlacedItemFor(proj, item_guid, false, nullptr, &why);
        if (!item) return false;
        const bool moved = f_set_item(item, "D_POSITION", project_t);
        RefreshLaneItem(item);  // 13-16: the gesture's lane cache in step
        return moved;
    } catch (...) {
        return false;
    }
}

PlaceResult ReplacePlacement(ReaProject* proj, const std::string& item_guid, const PlaceRequest& req)
{
    PlaceResult r;
    try {
        if (!EngineReady()) {
            r.error = kMissingFns;
            return r;
        }
        PlacedTag  tag;
        MediaItem* item = PlacedItemFor(proj, item_guid, true, &tag, &r.error);
        if (!item) return r;
        return PlaceImpl(proj, req, item, tag.owner, tag.tk);
    } catch (...) {
        r.error = "replace failed";
        return r;
    }
}

bool RemovePlacement(ReaProject* proj, const std::string& item_guid)
{
    try {
        if (!f_delete_item) return false;
        std::string why;
        MediaItem*  item = PlacedItemFor(proj, item_guid, true, nullptr, &why);
        if (!item) return false;
        ForgetLaneItem(item);  // 13-16: out of the gesture's lane cache
        ForgetDeleted(item);   // 13-19: out of the gesture's live and folder caches
        return f_delete_item(GetMediaItem_Track(item), item);
    } catch (...) {
        return false;
    }
}

bool RemovePlacedAny(ReaProject* proj, MediaItem* item)
{
    try {
        if (!f_delete_item || !item) return false;
        if (!(proj ? ValidatePtr2(proj, item, "MediaItem*") : IsLiveItem(item))) return false;
        PlacedTag  tag;
        const bool has_tag = ReadPlacedTag(item, &tag);
        // The refusals of PlacedItemFor, without the "attached" one (edit = false).
        if (!PlacedItemRefusal(true, has_tag, has_tag && IsFolderItem(item), tag.own, false).empty()) return false;
        ForgetLaneItem(item);  // 13-16: out of the gesture's lane cache
        ForgetDeleted(item);   // 13-19: out of the gesture's live and folder caches
        return f_delete_item(GetMediaItem_Track(item), item);
    } catch (...) {
        return false;
    }
}

bool ReadCopyImportedMedia()
{
    if (!f_config_string) return true;
    char buf[64] = {};
    if (!f_config_string("copyimpmedia", buf, sizeof(buf)) || !buf[0]) return true;
    return (std::atoi(buf) & 1) != 0;
}

std::string FileForProject(ReaProject* proj, const std::string& path)
{
    namespace fs = std::filesystem;
    try {
        if (path.empty() || !ReadCopyImportedMedia() || !f_project_path) return path;
        char dir_buf[4096] = {};
        f_project_path(proj, dir_buf, sizeof(dir_buf));
        if (!dir_buf[0]) return path;
        std::error_code ec;
        const fs::path  src = fs::u8path(path);
        const fs::path  dir = fs::u8path(dir_buf);
        const auto      size = fs::file_size(src, ec);
        if (ec) return path;
        if (fs::equivalent(src.parent_path(), dir, ec)) return path;  // already in the media folder
        ec.clear();
        fs::create_directories(dir, ec);
        ec.clear();
        if (!fs::is_directory(dir, ec)) return path;
        const std::string name = src.filename().u8string();
        for (int n = 0; n < 1000; ++n) {
            const fs::path dst = dir / fs::u8path(MediaCopyName(name, n));
            ec.clear();
            if (fs::exists(dst, ec)) {
                if (fs::equivalent(src, dst, ec)) return path;
                ec.clear();
                const auto dsize = fs::file_size(dst, ec);
                // Copied before only if it is the same file: same size and same bytes (a
                // re-rendered take or another folder's file of that name is not).
                if (!ec && dsize == size && SameBytes(src, dst)) return dst.u8string();
                continue;  // another file of that name
            }
            ec.clear();
            if (fs::copy_file(src, dst, ec) && !ec) return dst.u8string();
            return path;
        }
    } catch (...) {
    }
    return path;
}

}  // namespace rav

#endif  // _WIN32
