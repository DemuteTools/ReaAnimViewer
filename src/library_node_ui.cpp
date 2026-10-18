// SPDX-License-Identifier: MIT
//
// See library_node_ui.h. Every gesture is one ModifySoundStore undo point ("RAV: <gesture>")
// followed by the Library toast with Undo; live drags (faders, offsets, trims, dB, detection
// fields, the threshold line) preview a local draft and write once on release. A gesture is only
// picked while the panel draws (the 13-8 rule, picked_gestures.h: it holds copies of what it
// writes) and runs once the panel is drawn, so no SoundLibrary / SoundNode reference the panel
// holds is read after the store it points into was replaced. Picking a gesture does not stop the
// drawing (the rest of the panel stayed blank for a frame: Antho 2026-10-14, everything below a
// changed setting flickered); only a store really replaced meanwhile does (StoreReplaced). The audition / pre-edit logic is
// sound_audition_plan.h (pure).

#include "library_node_ui.h"
#include "watchdog.h"  // freeze audit: breadcrumbs and scopes
#include "busy_progress_ui.h"  // 13-59: the busy card's bar under Normalize while it measures
#include "dash_plan.h"         // freeze audit #1: hatches only where seen

#ifdef _WIN32

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>  // 13-62h (Lot E2): the panels' zone edits
#include <iterator>
#include <limits>
#include <map>
#include <optional>  // 13-62: PartSettings' own values
#include <set>
#include <string>
#include <utility>  // std::exchange (13-61)
#include <vector>

#include <imgui_internal.h>  // BeginDragDropTargetCustom, ImRect

#include "library_tree_edit.h"
#include "normalize_presets.h"
#include "library_icons.h"
#include "library_tree_ui.h"
#include "library_user_ui.h"  // 13-50: DrawLocateButtons (a missing File Sound)
#include "library_view_ui.h"
#include "live_draft_phase.h"
#include "picked_gestures.h"
#include "reaper_api.h"
#include "shortcuts.h"
#include "view_nav.h"  // 13-45: Edit's wheel and zoom keys, as every timeline's
#include "zone_wave_ui.h"  // 13-62h (Lot E1): a zoned Sound's waveform
#include "wave_draw_ui.h"  // 13-71: the one waveform drawer
#include "sound_assign.h"  // RoleKeyLabel (13-23 role maps)
#include "sound_audition.h"
#include "sound_audition_plan.h"
#include "sound_draw.h"
#include "sound_entries.h"
#include "sound_peaks.h"
#include "sound_place.h"
#include "sound_place_plan.h"
#include "sound_populate.h"
#include "sound_project.h"
#include "sound_record.h"
#include "sound_switch.h"
#include "ui_flow.h"
#include "ui_flow_math.h"
#include "ui_fold_all.h"
#include "ui_theme.h"

namespace rav {
namespace {

constexpr double kMinDb = -60.0, kMaxDb = 12.0;
constexpr const char kNoMediaTip[] = "Arrives with the Media Explorer";

// 13-28: a Normalize measured by REAPER (every mode but Peak), time-sliced: one span per call, ~8 ms per
// frame. Its sources (one per file) are deleted at its end or cancel; nothing is written before its end.
struct NormJob {
    bool                             active = false;
    std::string                      lib, uid, label;
    NormScope                        scope = NormScope::Variation;  // the effective one
    std::vector<NormRegion>          regions;   // 13-62: each with its own mode and target
    std::vector<double>              peak_adj;  // 13-62: per region, a Peak region's adjustment (cached peaks)
    std::vector<char>                by_peaks;  // 13-62: per region, 1 = Peak (not measured by the job)
    int                              pump_frame = -1;  // 13-62: pumped once per frame (two buttons may)
    std::vector<std::vector<double>> span_adj;  // per region, per span (dB, NaN = silent)
    size_t                           region = 0, span = 0, done_spans = 0, total_spans = 0;
    std::map<std::string, PCM_source*> sources;
    SoundData                        data0;  // the Sound measured: a change of its frames, zones, divide, roles
                                             // or mode meanwhile cancels
    // 13-62h (Lot E2): a zoned Sound's Normalize -- one region per group of zones (NormGroups), each group's gain
    // written into its zones (ApplyZoneNormalize); `reset_faders`: the parts' faders back to 0 (not for one zone).
    bool                              zoned = false;
    std::vector<std::vector<ZoneRef>> groups;
    bool                              reset_faders = false;
};

struct PanelState {
    AuditionHistory       hist;
    AuditionPlan          plan;
    std::string           plan_lib;
    NodePanelHooks        hooks;
    uint64_t              gen = 0;  // SoundStoreGeneration when this frame's drawing began
    // The shown Sound's media, re-read on a project change.
    std::string           media_uid;
    int                   media_scc = -1;
    SoundSource           media_src;
    SoundMedia            media;
    // 13-51 review (File Sound): the watched file's date + size when the media was read, when the disk
    // was last asked, and the length cache's generation then (a file renamed back or re-rendered
    // outside REAPER refreshes the panel within a second).
    FileVersion           media_ver;
    double                media_checked_s = 0.0;
    uint64_t              media_len_gen = 0;
    // A live sound edit: `draft` is shown instead of the store until it is written (13-53: until the
    // write has run, and on the frame its drag is released: LiveDraftPhase).
    LiveDraftPhase        phase;
    SoundData             snap, draft;
    // A live volume (fader / dB field) or Blend offset.
    std::string           vol_key;
    double                vol_live = 0.0;
    // A volume fader's gesture (0 = a dB field's): its id and its library, to enter its value on its
    // own node when the node it belongs to is no longer drawn (SettleLostFaders).
    ImGuiID               vol_gid = 0;
    std::string           vol_lib;
    int                   vol_seen = -1;  // the last frame its fader was drawn active
    std::string           off_uid;
    double                off0 = 0.0, off_live = 0.0;
    double                off_scale = 0.0;  // the lanes' ms at the drag's start (fixed while dragging)
    float                 off_dx = 0.0f;
    // 13-45: the Blend lanes' view window (ms) of the Blend shown: span 0 = the whole Blend; a
    // middle-drag pans it. A view state (no undo point), dropped when another Blend shows.
    std::string           blend_uid;
    double                blend_v0 = 0.0, blend_span = 0.0;
    bool                  blend_pan = false;
    bool                  blend_focus = false;  // 13-45 review: clicked in, the plain wheel is the lanes' (not the panel's)
    // The Edit overlay.
    bool                  edit_open = false;
    bool                  edit_focus = false;
    bool                  edit_has_focus = false;  // the Edit overlay window is focused (its zoom keys)
    bool                  edit_divided = false;    // 13-62h: the Sound it shows is zoned (divided, not Takes) and read: Delete is Edit's
    int                   edit_opened = -1;  // the frame Edit opened (its E is not read again then)
    std::string           edit_lib, edit_uid;
    double                view_start = 0.0, view_len = 0.0;
    int                   sound_frame = -10, edit_frame = -10;
    std::set<std::string> auto_tried;
    double                frame_start = 0.0;  // the frame playing ("frame:<uid>")
    char                  name_buf[256] = {};
    std::string           name_uid;
    // The lit nodes, once per frame.
    int                   lit_frame = -1;
    std::set<std::string> lit;
    // Detected-count cache (the "D" of "D -> N variations").
    std::string           det_key;
    int                   det_count = 0;
    // 13-62h: a settings row's fader being dragged (SettingsNumberRow): its row's key, its live value, whether it
    // has moved in this gesture, its id and the last frame it was drawn active (SettleLostFaders).
    std::string           numf_key;
    double                numf_live = 0.0;
    bool                  numf_moved = false;
    ImGuiID               numf_gid = 0;
    int                   numf_seen = -1;
    // A Detection row's fader (Threshold / Min gap): its library, node and field, to enter it when its row goes
    // away mid-gesture (SettleLostFaders). detf_field nullptr = a Settings of row's (dropped then).
    std::string           detf_lib, detf_uid;
    double SilenceParams::*detf_field = nullptr;
    // 13-28: Normalize's current settings (the user's, ExtState), the user's presets (reloaded when Edit
    // opens: another project may have changed them), the preset name popup and the measuring job.
    bool                  norm_loaded = false;
    NormScope             norm_scope = NormScope::Variation;
    NormMode              norm_mode = NormMode::Peak;
    double                norm_db = -1.0;
    NormJob               norm_job;
    // Edit's waveform: its x span on screen and the source length (key zoom around the mouse).
    float                 wave_x0 = 0.0f, wave_x1 = 0.0f;
    double                wave_len = 0.0;
    ImGuiID               wave_press_id = 0;  // 13-62i: the read-only waveform pressed (a click = released where pressed)
    ImVec2                wave_press;
    double                vzoom = 1.0;  // 13-45: Edit's lanes taller than its box (Zoom vertically), 1 = fit
    // 13-61: the edit cursor (where the last click on the waveform landed; < 0 = none) and a trim row's key
    // waiting for the overlay's next frame (a kShortcutEditTrim* id; 13-63: or kShortcutEditSplit; -1 = none).
    // View state: no undo point.
    double                edit_cursor = -1.0;
    int                   trim_request = -1;
    // 13-62: the selected part ("" = the Sound), the parts muted / soloed for listening and a Solo / Mute key
    // waiting for the overlay (1 = solo, 2 = mute, 3 = exclusive solo (13-67a), 0 = none). View state: never saved, no undo point; reset with
    // the shown Sound (C5).
    std::string           part_sel;
    std::set<std::string> part_mute, part_solo;
    // 13-62h (Lot E1): a zoned Sound's Edit view state -- the selected zones, the time selection, Loop, the drag,
    // the zone audition (zone_wave_ui.h). Never saved, no undo point; reset in OpenEdit.
    ZoneEditView          zv;
    // 13-62h (Lot E2): a number of the panels being dragged on a zoned Sound -- the zone it edits and the selection
    // when it began (the live draft restarts from the store each frame, so does the gesture). View state.
    bool                  zedit_on = false;
    ZoneRef               zedit_ref;
    ZoneSel               zedit_sel;
    int                   listen_request = 0;
    int                   nav_request = 0;  // 13-62F (F5): a Next (+1) / Previous (-1) variation key waiting (0 = none)
    // 13-62h: a Heal / Loop key waiting for the overlay's next frame (kShortcutEditHeal / kShortcutEditLoop;
    // -1 = none). Lot E consumes it: Heal = HealZones on the selected zones (one undo point, or a toast when they
    // are not edge to edge); Loop = the Loop toggle (view state, no undo point).
    int                   zone_request = -1;
    // 13-74: the gain rows' key presses waiting for the overlay (net notches, + = up; 0 = none). Not held back by a
    // live draft: notches in a row merge into one (ZoneGainNudge).
    int                   gain_request = 0;
    bool                  roles_more = false;
    bool                  layer_roles_more = false;  // 13-31: the "plays on" list's own "More roles"
    unsigned              draft_rev = 0;  // bumped each time the live draft is rewritten or ends
    // 13-40: the Mix row selected (its parent and the child): Play / Space plays that child.
    std::string           mix_parent, mix_child;
};
PanelState P;

// The store was replaced since this frame's drawing began: every SoundLibrary / SoundNode
// reference taken before is stale, so the drawing stops (the next frame draws again).
bool StoreReplaced() { return SoundStoreGeneration() != P.gen; }

void CancelNormJob();  // 13-28 (below)

// 13-7a / 13-7b: the layout helpers are the shared ones (ui_flow.h): labelled rows, text
// ellipsized with a tooltip, items wrapping when they do not fit beside the last one.
using ui::FitText;
using ui::SameLineIfFits;

double DbToGain(double db) { return std::pow(10.0, db / 20.0); }

using ui::WithAlpha;

void ClearLaneGain();  // 13-62 (below): the lane header's gain drag forgotten

void OpenEdit(const std::string& lib_uid, const std::string& uid)
{
    ClearLaneGain();
    P.edit_open = true;
    P.edit_focus = true;
    P.vzoom = 1.0;
    P.wave_len = 0.0;  // set by a Sound's waveform; Takes mode leaves it 0 (no key zoom)
    P.wave_x0 = P.wave_x1 = 0.0f;
    P.edit_opened = ImGui::GetFrameCount();
    P.edit_lib = lib_uid;
    P.edit_uid = uid;
    P.view_start = 0.0;
    P.view_len = 0.0;
    P.edit_cursor = -1.0;  // 13-61
    P.trim_request = -1;
    P.part_sel.clear();  // 13-62 (C5): listening and selection are per Sound
    P.part_mute.clear();
    P.part_solo.clear();
    P.listen_request = 0;
    P.nav_request = 0;
    P.zone_request = -1;  // 13-62h
    P.gain_request = 0;   // 13-74
    ZoneGainSettle(true);
    ZoneEditReset(P.zv);  // 13-62h (Lot E1)
    P.zedit_on = false;   // 13-62h (Lot E2)
    P.zedit_sel.clear();
    P.roles_more = false;
    P.layer_roles_more = false;
    CancelNormJob();          // 13-28: switching Sound cancels a measuring
}

// ---- gestures -------------------------------------------------------------------------------------

// The gestures picked while the panel draws, run by DrawNodePanel once it is drawn.
PickedGestures g_picks;

// The write itself: one undo point and the toast. False when nothing changed.
bool WriteNow(const std::string& name, const std::function<bool(SoundStore&)>& edit, const std::string& toast)
{
    RAV_SCOPE("Write", name);  // freeze audit
    const std::string desc = "RAV: " + name;
    if (!ModifySoundStore(nullptr, desc.c_str(), edit)) return false;
    ShowLibraryToast(toast.empty() ? name : toast, true, desc);
    return true;
}

// Picks the write (`edit` holds copies: it runs after the panel, the caller's locals gone).
void Gesture(const std::string& name, std::function<bool(SoundStore&)> edit, const std::string& toast = {})
{
    if (!edit) return;
    RAV_CRUMB("Gesture picked", name);  // freeze audit
    g_picks.Pick([name, edit, toast]() { WriteNow(name, edit, toast); });
}

// The store edit of one node: `f` on the node `uid` of the library `lib_uid` (copies of all three).
std::function<bool(SoundStore&)> NodeEdit(const std::string& lib_uid, const std::string& uid,
                                          std::function<bool(SoundNode&)> f)
{
    return [lib_uid, uid, f](SoundStore& s) {
        SoundLibrary* l = FindLibrary(s, lib_uid);
        SoundNode*    n = l ? FindNode(*l, uid) : nullptr;
        return n && f && f(*n);
    };
}

void EditNode(const std::string& name, const std::string& lib_uid, const std::string& uid,
              std::function<bool(SoundNode&)> f, const std::string& toast = {})
{
    Gesture(name, NodeEdit(lib_uid, uid, std::move(f)), toast);
}

const SoundData& Shown(const SoundNode& n) { return P.phase.Shows(n.uid) ? P.draft : n.sound; }

// Starts (or continues) a live edit of the Sound: the draft restarts from the snapshot each call.
SoundData& Live(const SoundNode& n)
{
    // After a commit this frame its write has not run yet: the next edit starts from the committed
    // draft, not from the store.
    const bool after_commit = P.phase.Committing() && P.phase.Uid() == n.uid;
    if (P.phase.Touch(n.uid)) P.snap = after_commit ? P.draft : n.sound;
    P.draft = P.snap;
    ++P.draft_rev;
    return P.draft;
}

void CommitLive(const std::string& name, const std::string& lib_uid, const std::string& uid, const std::string& toast = {})
{
    const unsigned token = P.phase.Commit(uid);
    if (!token) return;
    const SoundData d = P.draft;
    // 13-53: the draft stays drawn until this write has run (the rest of this frame drew the old store:
    // what was dragged jumped back for a frame on release).
    auto edit = NodeEdit(lib_uid, uid, [d](SoundNode& n) {
        n.sound = d;
        return true;
    });
    g_picks.Pick([name, edit, toast, token]() {
        WriteNow(name, edit, toast);
        P.phase.Written(token);
        ++P.draft_rev;
    });
}

void CancelLive()
{
    P.phase.Cancel();
    ++P.draft_rev;
}

// ---- media and detection --------------------------------------------------------------------------

bool SameSource(const SoundSource& a, const SoundSource& b)
{
    return a.kind == b.kind && a.item_guid == b.item_guid && a.asset.last_path == b.asset.last_path;
}

// The file a File Sound's panel watches: the one read, else (missing) its last path.
std::string WatchedFile(const SoundMedia& m, const SoundSource& src)
{
    if (src.kind != SoundSourceKind::File) return {};
    return m.ok ? m.path : src.asset.last_path;
}

double PanelNowSeconds()
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

const SoundMedia& MediaOf(const SoundNode& n)
{
    const int scc = GetProjectStateChangeCount(nullptr);
    bool      reread = P.media_uid != n.uid || P.media_scc != scc || !SameSource(P.media_src, n.sound.source);
    // 13-51 review: a File Sound's file changed outside REAPER (renamed back, re-rendered with another
    // length) or its cached length was dropped (the peak cache saw a change): read it again. Only the
    // shown Sound, the disk asked at most once a second.
    if (!reread && n.sound.source.kind == SoundSourceKind::File) {
        if (P.media_len_gen != SourceLengthGeneration()) {
            reread = true;
        } else {
            const double now = PanelNowSeconds();
            if (FileRecheckDue(P.media_checked_s, now)) {
                P.media_checked_s = now;
                const std::string f = WatchedFile(P.media, n.sound.source);
                if (FileReadStale(P.media_ver, SourceFileVersion(f))) {
                    ForgetSourceFileLength(f);  // its length is read again
                    reread = true;
                }
            }
        }
    }
    if (reread) {
        P.media_uid = n.uid;
        P.media_scc = scc;
        P.media_src = n.sound.source;
        const bool        file = n.sound.source.kind == SoundSourceKind::File;
        const std::string last = file ? n.sound.source.asset.last_path : std::string();
        const FileVersion before = SourceFileVersion(last);  // before the read: a write during it is seen after
        ReadSoundMedia(n.sound.source, &P.media);
        const std::string watched = WatchedFile(P.media, n.sound.source);
        P.media_ver = watched == last ? before : SourceFileVersion(watched);
        P.media_len_gen = SourceLengthGeneration();
        P.media_checked_s = PanelNowSeconds();
    }
    return P.media;
}

std::vector<SoundFrame> Detected(const SoundMedia& m, DetectMode mode, const SilenceParams& sp)
{
    PeakData   pk;
    const bool has = mode == DetectMode::Silence && RequestPeaks(m.path, &pk) == PeakState::Ready;
    return DetectFrames(m.facts, mode, sp, has ? &pk : nullptr);
}

// 13-20 / 13-23 / 13-25: the parts' cuts and sync points measured again on the file's peaks (without them
// while they are read: the stored ones kept, else measured at placement). Every write that changes
// frames, detection, divide, sync settings or zones runs it. RefreshOne: one variation (a cut dragged).
void RefreshWith(SoundData& d, const std::string& path)
{
    // 13-62i: a Takes-mode Sound's takes each laid out on their own file's peaks (the shown Sound's media, read).
    if (d.mode == DetectMode::Takes && P.media.ok && SameSource(P.media_src, d.source) && !d.frames.empty()) {
        for (size_t k = 0; k < d.frames.size(); ++k) {
            PeakData          pk;
            const std::string p = VariationPath(P.media, d, static_cast<int>(k));
            RefreshParts(d, !p.empty() && RequestPeaks(p, &pk) == PeakState::Ready ? &pk : nullptr, static_cast<int>(k));
        }
        return;
    }
    PeakData pk;
    RefreshParts(d, !path.empty() && RequestPeaks(path, &pk) == PeakState::Ready ? &pk : nullptr);
}

// 13-62h (Lot E2): the file's peaks for this frame's zone gestures (null while they are read), and the context a
// zone gesture of the panels runs in. 13-62h review: kept between frames (copied again only when the file's peaks
// change, as the zone waveform's PeaksOf), never copied each frame.
const PeakView* EditPeaks(const std::string& path)
{
    static PeakData    pk;
    static std::string at;
    static FileVersion file;
    static int         frame = -1;
    static bool        ok = false;
    const int          f = ImGui::GetFrameCount();
    if (frame != f || at != path) {
        frame = f;
        const bool ready = !path.empty() && RequestPeaks(path, nullptr) == PeakState::Ready;
        if (!ready) {
            ok = false;
        } else {
            const FileVersion fv = PeaksFileVersion(path);
            if (!ok || at != path || fv != file) {
                ok = RequestPeaks(path, &pk) == PeakState::Ready;
                file = fv;
            }
        }
        at = path;
    }
    return ok ? &pk : nullptr;
}

ZoneContext ZoneCtxOf(const SoundMedia& m) { return ZoneContext{EditPeaks(m.path), m.source_len}; }

// 13-62h review: a write of the panels is on its way (the zone waveform does not carry the selection over again when
// the store moves: the panel already resolved it).
void MarkZoneOwn()
{
    P.zv.own_pending = true;
    P.zv.own_frame = ImGui::GetFrameCount();
}

bool SameZoneEdit(const SoundData& x, const SoundData& y);  // (below)

// 13-62h review: a write of the panels that lays the zones out again without a selection of its own (a part's
// settings back to the Sound's, the Sync point, Normalize): `probe` = that write run on a copy of `before`; the
// selection carried over by time and part (SelFollow) and the write marked ours.
void ZoneSelAfter(const SoundData& before, const SoundData& probe)
{
    if (!UsesZones(before) || SameZoneEdit(before, probe)) return;
    P.zv.sel = SelFollow(before, P.zv.sel, probe);
    MarkZoneOwn();
}

// Two zones alike as the user sees them (edges, part, cuts, hand flags, own snap / fades / gain, crossfade).
bool SameEditZone(const EditZone& x, const EditZone& y)
{
    return x.a == y.a && x.b == y.b && x.part == y.part && x.slot_a == y.slot_a && x.slot_b == y.slot_b && x.a_hand == y.a_hand &&
           x.b_hand == y.b_hand && x.snap == y.snap && x.fade_in == y.fade_in && x.fade_out == y.fade_out && x.db == y.db &&
           x.xl == y.xl && x.xr == y.xr;
}

// A zone edit changed nothing the user sees (every zone, every part's fader): no undo point.
bool SameZoneEdit(const SoundData& x, const SoundData& y)
{
    if (x.frames.size() != y.frames.size()) return false;
    for (size_t i = 0; i < x.frames.size(); ++i) {
        const std::vector<EditZone>& a = x.frames[i].zones;
        const std::vector<EditZone>& b = y.frames[i].zones;
        if (a.size() != b.size() || x.frames[i].on != y.frames[i].on) return false;  // a variation turned off meanwhile
        for (size_t k = 0; k < a.size(); ++k)
            if (!SameEditZone(a[k], b[k])) return false;
    }
    for (const std::string& v : PartKeys(x))  // 13-62i: the implicit part's fader too
        if (PartFaderDb(x, v) != PartFaderDb(y, v)) return false;
    return true;
}

// 13-62h (decision 7): "· 2 edges dragged by hand follow it again (⚙ Options)" / "... kept (⚙ Options)" after a
// Pre-roll / Tail change; "" when no edge was dragged by hand.
std::string PreTailToastExtra(int hand, bool tail_wins)
{
    if (hand <= 0) return std::string();
    const std::string n = std::to_string(hand) + (hand > 1 ? " edges" : " edge") + " dragged by hand ";
    return " \xC2\xB7 " + n + (tail_wins ? (hand > 1 ? "follow it again" : "follows it again") : std::string("kept")) + " (\xE2\x9A\x99 Options)";
}

// A re-detection that keeps the hand frames (decision 7), their hand sync points and cuts carried to the
// new frames that hold them (A7), the parts measured again.
void Redetect(SoundData& d, const SoundMedia& m, const std::vector<SoundFrame>& previous)
{
    d.frames = MergeHandFrames(Detected(m, d.mode, d.silence), previous, d.mode == DetectMode::Takes);
    CarryHandMarks(d.frames, previous);
    CarrySides(d.frames, previous, d.side_pattern, d.sides, d.mode == DetectMode::Takes);  // 13-73: each keeps its side
    RefreshWith(d, m.path);
}

int DetectedCount(const std::string& uid, const SoundMedia& m, const SoundData& d)
{
    const bool ready = RequestPeaks(m.path, nullptr) == PeakState::Ready;
    char       key[256];
    std::snprintf(key, sizeof(key), "%s|%d|%g|%g|%g|%g|%d|%d", uid.c_str(), static_cast<int>(d.mode),
                  d.silence.threshold_db, d.silence.min_gap_ms, d.silence.preroll_ms, d.silence.tail_ms, ready ? 1 : 0,
                  P.media_scc);
    if (P.det_key != key) {
        P.det_key = key;
        P.det_count = static_cast<int>(Detected(m, d.mode, d.silence).size());
    }
    return P.det_count;
}

// ---- audition -------------------------------------------------------------------------------------

// A Sound whose source is an NVK folder item: its pile is placed as it is, without fades.
bool FolderSourced(const SoundData& d)
{
    if (d.source.kind != SoundSourceKind::Item || d.source.item_guid.empty()) return false;
    MediaItem* it = FindItemByGuid(d.source.item_guid);
    return it && IsFolderItem(it);
}

// A voice of a plan as PlayVoices plays it, from `path`.
PlayVoice ToPlayVoice(const AuditionVoice& v, const std::string& path)
{
    PlayVoice p;
    p.path = path;
    p.start_s = v.start_s;
    p.length_s = v.length_s;
    p.delay_s = v.delay_ms / 1000.0;
    p.gain = DbToGain(v.gain_db);
    p.fade_in_s = v.fade_in_s;
    p.fade_out_s = v.fade_out_s;
    p.fade_in_shape = v.fade_in_shape;
    p.fade_out_shape = v.fade_out_shape;
    p.fade_from_s = v.fade_from_s;
    p.fade_span_s = v.fade_span_s;
    p.mono = v.mono;
    return p;
}

// Plays `plan` tagged `tag`: the voices whose file is read; P.plan then names only the voices that play
// (13-7b review: a file PlayVoices could not open is not named). False, with `why`, when nothing plays.
bool PlayPlan(const std::string& lib_uid, const SoundLibrary& lib, AuditionPlan plan, const std::string& tag,
              std::string* why)
{
    std::vector<PlayVoice>     pv;
    std::vector<AuditionVoice> read;
    for (const AuditionVoice& v : plan.voices) {
        const SoundNode* s = FindNode(lib, v.node);
        SoundMedia       m;
        if (!s || !ReadSoundMedia(s->sound.source, &m)) continue;
        PlayVoice p = ToPlayVoice(v, VariationPath(m, s->sound, v.variation));
        if (FolderSourced(s->sound)) p.fade_in_s = p.fade_out_s = 0.0;  // placed without fades
        pv.push_back(p);
        read.push_back(v);
    }
    std::string reason = plan.voices.empty() ? std::string("nothing to play")
                         : pv.empty()        ? std::string("the sound files are not found")
                                             : std::string();
    std::vector<int> kept;
    const bool       ok = !pv.empty() && PlayVoices(pv, tag, &reason, &kept);
    if (why) *why = reason;
    if (!ok) return false;
    plan.voices.clear();
    for (int k : kept)
        if (k >= 0 && static_cast<size_t>(k) < read.size()) plan.voices.push_back(read[static_cast<size_t>(k)]);
    plan.played = PlayedText(lib, plan.voices);  // only the voices that play
    P.plan = plan;
    P.plan_lib = lib_uid;
    return true;
}

void PlayNode(const std::string& lib_uid, const std::string& uid)
{
    const SoundStore&   store = GetSoundStore(nullptr);
    const SoundLibrary* lib = FindLibrary(store, lib_uid);
    if (!lib) return;
    std::string why;
    if (PlayPlan(lib_uid, *lib, PlanAudition(store, *lib, uid, P.hist.Picker()), uid, &why)) return;
    // Fallback (decision 2): the selected Sound's one variation.
    const SoundNode* n = FindNode(*lib, uid);
    if (n && n->type == SoundNodeType::Sound) {
        const std::vector<int> act = ActiveVariations(n->sound);
        SoundMedia             m;
        if (!act.empty() && ReadSoundMedia(n->sound.source, &m)) {
            // 13-62i: as Populate places it (its zones).
            std::vector<AuditionVoice> vs = PartAuditionVoices(*lib, *n, act.front(), std::string());
            std::vector<PlayVoice>     pv;
            for (AuditionVoice& v : vs) {
                if (FolderSourced(n->sound)) v.fade_in_s = v.fade_out_s = 0.0;
                pv.push_back(ToPlayVoice(v, VariationPath(m, n->sound, act.front())));
            }
            if (!pv.empty() && PlayVoices(pv, uid)) {
                AuditionPlan one;
                one.root = uid;
                one.voices = vs;
                one.played = n->name;
                P.plan = one;
                P.plan_lib = lib_uid;
                return;
            }
        }
    }
    ShowLibraryToast("Cannot play: " + (why.empty() ? std::string("unknown reason") : why), false);
}

// 13-40: the Mix row selected under `n` (still one of its children), "" = none. Only a Random or a Switch
// has a Mix section.
std::string MixSelected(const SoundLibrary& lib, const SoundNode& n)
{
    if (n.type != SoundNodeType::Random && n.type != SoundNodeType::Switch) return {};
    if (P.mix_parent != n.uid || P.mix_child.empty()) return {};
    const SoundNode* k = FindNode(lib, P.mix_child);
    return k && k->parent == n.uid ? P.mix_child : std::string();
}

// 13-40: the panel's Play / Space: the Mix row selected (that child, resolved as itself, under the
// panel's node), else the node.
void PlayPanel(const SoundLibrary& lib, const SoundNode& n)
{
    const std::string child = MixSelected(lib, n);
    if (child.empty()) {
        PlayNode(lib.uid, n.uid);
        return;
    }
    std::string why;
    if (!PlayPlan(lib.uid, lib, PlanChildAudition(GetSoundStore(nullptr), lib, n.uid, child, P.hist.Picker()), n.uid, &why))
        ShowLibraryToast("Cannot play: " + (why.empty() ? std::string("unknown reason") : why), false);
}

void StopPlay() { StopAudition(); }

bool NodePlaying(const std::string& uid) { return AuditionPlaying() && AuditionTag() == uid; }

bool IsLit(const std::string& node)
{
    if (!AuditionPlaying() || AuditionTag() != P.plan.root) return false;
    const int f = ImGui::GetFrameCount();
    if (P.lit_frame != f) {
        P.lit_frame = f;
        P.lit = LitAt(P.plan, AuditionPosition() * 1000.0);
        P.lit.insert(P.plan.root);
    }
    return P.lit.count(node) > 0;
}

void MergeTreeHooks()
{
    const LibraryTreeHooks& h = GetLibraryTreeHooks();
    if (h.play && h.stop && h.is_lit) return;
    LibraryTreeHooks m = h;  // 13-7c's members are kept
    m.play = [](const std::string& lib, const std::string& node) { PlayNode(lib, node); };
    m.stop = [] { StopPlay(); };
    m.is_lit = [](const std::string& node) { return IsLit(node); };
    SetLibraryTreeHooks(m);
}

// 13-20 / 13-23: plays audition voices of one Sound (a part's pieces, a zone) as "frame:<uid>", so the
// playhead follows them in the file (the pieces are delayed by their distance from the first).
void PlaySoundVoices(const SoundNode& n, const SoundMedia& m, const std::vector<AuditionVoice>& voices)
{
    std::vector<PlayVoice> pv;
    for (const AuditionVoice& v : voices) {
        if (!(v.length_s > 0.0)) continue;
        pv.push_back(ToPlayVoice(v, v.variation >= 0 ? VariationPath(m, n.sound, v.variation) : m.path));
    }
    if (pv.empty()) {
        ShowLibraryToast("Nothing to play there", false);
        return;
    }
    if (!PlayVoices(pv, "frame:" + n.uid)) ShowLibraryToast("Cannot play: the sound file is not read", false);
    else P.frame_start = pv.front().start_s;
}

// A part of variation i ("" = the whole variation minus its exclusions), as Populate places it.
void PlayPart(const SoundLibrary& lib, const SoundNode& n, const SoundMedia& m, int i, const std::string& part)
{
    PlaySoundVoices(n, m, PartAuditionVoices(lib, n, i, part));  // 13-62i: its zones, as placed
}

// The playhead in a Sound's source seconds (or < 0): its frame, or its whole file.
double SoundPlayhead(const SoundNode& n)
{
    if (!AuditionPlaying()) return -1.0;
    const std::string& tag = AuditionTag();
    if (tag == "frame:" + n.uid) return P.frame_start + AuditionPosition();
    return -1.0;
}

// ---- small controls -------------------------------------------------------------------------------

const char* TypeName(SoundNodeType t)
{
    switch (t) {
    case SoundNodeType::Random: return "Random";
    case SoundNodeType::Blend: return "Blend";
    case SoundNodeType::Switch: return "Switch";
    default: return "Sound";
    }
}

// The mockup's contName: the node's name, else "Switch on <group>" / its type.
std::string NodeName(const SoundStore& store, const SoundNode& n)
{
    if (!n.name.empty()) return n.name;
    if (n.type == SoundNodeType::Switch) {
        const SwitchGroup* g = FindGroup(store, n.group_uid);
        return std::string("Switch on ") + (g ? g->name : std::string("?"));
    }
    return TypeName(n.type);
}

// The panel's title (the mockup's slotLabel): a container's name, "Blend (2)" when it has none.
std::string NodeTitle(const SoundStore& store, const SoundLibrary& lib, const SoundNode& n)
{
    if (!n.name.empty() || n.type == SoundNodeType::Sound || n.type == SoundNodeType::Switch) return NodeName(store, n);
    return std::string(TypeName(n.type)) + " (" + std::to_string(ChildNodes(lib, n).size()) + ")";
}

// The breadcrumb beside the title (the mockup's crumbs): the library, then the node's parents only,
// a Switch parent as "<group> = <value(s)>" (the value(s) the path plays for).
std::string Crumbs(const SoundStore& store, const SoundLibrary& lib, const SoundNode& n)
{
    std::vector<std::string> segs;
    const SoundNode*         child = &n;
    for (int guard = 0; child && !child->parent.empty() && guard < 64; ++guard) {
        const SoundNode* p = FindNode(lib, child->parent);
        if (!p) break;
        std::string seg = NodeName(store, *p);
        if (p->type == SoundNodeType::Switch) {
            const SwitchGroup* g = FindGroup(store, p->group_uid);
            std::string        vals;
            for (const SwitchCase& c : p->cases) {
                if (c.child_uid != child->uid || !g) continue;
                for (const SwitchValue& v : g->values)
                    if (v.uid == c.value_uid) vals += (vals.empty() ? "" : ", ") + v.name;
            }
            const bool unassigned =
                std::find(p->unassigned.begin(), p->unassigned.end(), child->uid) != p->unassigned.end();
            if (g && !vals.empty()) seg = g->name + " = " + vals;
            else if (g && unassigned) seg = g->name + " \xC2\xB7 unassigned";
        }
        segs.push_back(seg);
        child = p;
    }
    std::string s = lib.name;
    for (auto it = segs.rbegin(); it != segs.rend(); ++it) s += " \xE2\x80\xBA " + *it;
    return s;
}

// A run of text in one colour (and font) for RichText.
struct TextRun {
    std::string text;
    ImU32       col = ui::kMuted;
    ImFont*     font = nullptr;  // nullptr = the current font
};

// Runs of text in their colours, wrapped word by word within the room left (the mockup's .sub /
// .hint paragraphs with their .hl parts). One item of the height it takes.
void RichText(const std::vector<TextRun>& runs)
{
    ImDrawList*  dl = ImGui::GetWindowDrawList();
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const float  right = std::max(p0.x + 40.0f, ui::LineRight());
    const float  line_h = ImGui::GetTextLineHeight();
    float        x = p0.x, y = p0.y, max_x = p0.x;
    for (const TextRun& r : runs) {
        ImFont*     f = r.font ? r.font : ImGui::GetFont();
        const char* s = r.text.c_str();
        const char* end = s + r.text.size();
        while (s < end) {
            const char* w = s;  // a word with its trailing spaces
            while (w < end && *w != ' ') ++w;
            while (w < end && *w == ' ') ++w;
            const char* t = s;  // the word without its spaces: it goes on the next line when it does not fit
            while (t < w && *t != ' ') ++t;
            if (x > p0.x && x + ui::TextSizeIn(f, s, t).x > right) {
                x = p0.x;
                y += line_h;
            }
            const float ww = ui::TextSizeIn(f, s, w).x;
            ui::AddTextIn(dl, f, ImVec2(x, y + std::floor((line_h - f->FontSize) * 0.5f)), r.col, s, w);
            x += ww;
            max_x = std::max(max_x, x);
            s = w;
        }
    }
    ImGui::Dummy(ImVec2(std::max(1.0f, std::min(max_x, right) - p0.x), y + line_h - p0.y));
}

// The children of a container whose sound is missing (an Item gone from the project, a File not
// found), cached a second per node and project state.
int MissingChildren(const SoundLibrary& lib, const SoundNode& n)
{
    static std::string key;
    static int         count = 0, frame = -1000;
    const int          scc = GetProjectStateChangeCount(nullptr);
    const std::string  k = lib.uid + "|" + n.uid + "|" + std::to_string(scc);
    if (k == key && ImGui::GetFrameCount() - frame < 60) return count;
    key = k;
    frame = ImGui::GetFrameCount();
    count = 0;
    const LibraryTreeHooks& h = GetLibraryTreeHooks();
    for (const SoundNode* c : ChildNodes(lib, n)) {
        if (c->type != SoundNodeType::Sound) continue;
        const SoundSource& s = c->sound.source;
        if (s.kind == SoundSourceKind::Item) count += (s.item_guid.empty() || !FindItemByGuid(s.item_guid)) ? 1 : 0;
        else if (h.is_missing && h.is_missing(*c)) ++count;
    }
    return count;
}

// A volume fader -60..+12 dB with its dB field; true when a value is to be written (*out).
bool VolumeControl(const char* id, const std::string& lib_uid, const std::string& key, double stored, double* out,
                   float fader_w)
{
    ImGui::PushID(id);
    const bool live = P.vol_key == key;
    float      v = static_cast<float>(live ? P.vol_live : stored);
    bool       commit = false;
    const float kDbField = ui::Dp(56.0f);  // the mockup's .fader .lnum
    const float db_w = ui::NumberFieldWidth(kDbField, "dB");  // the dB field with its unit, measured
    bool        beside = true;
    const float room = flow::StretchBeside(ImGui::GetContentRegionAvail().x, ui::Dp(60.0f), db_w,
                                           ImGui::GetStyle().ItemSpacing.x, &beside);
    ImGui::SetNextItemWidth(std::max(1.0f, std::min(fader_w, room)));
    ui::Fader("##fader", &v, static_cast<float>(kMinDb), static_cast<float>(kMaxDb));  // 13-7 (B5): the mockup's fader
    if (ImGui::IsItemActive()) {
        P.vol_key = key;
        P.vol_live = v;
        P.vol_gid = ImGui::GetItemID();
        P.vol_lib = lib_uid;
        P.vol_seen = ImGui::GetFrameCount();
    }
    if (ImGui::IsItemDeactivated() && P.vol_key == key) {
        P.vol_key.clear();
        P.vol_gid = 0;
        // Esc after typing: the value is back as it was (ui::FaderCancelled), no undo point.
        if (!ui::FaderCancelled() && std::fabs(v - stored) > 1e-4) {
            *out = v;
            commit = true;
        }
    }
    if (beside) ImGui::SameLine();
    double d = P.vol_key == key ? P.vol_live : stored;
    switch (ui::DragNumber("##db", &d, 0.1, 1, "dB", kDbField, /*trim=*/true)) {  // "0", "-14.5"
    case ui::DragNumberEvent::Live:
        P.vol_key = key;
        P.vol_live = std::clamp(d, kMinDb, kMaxDb);
        P.vol_gid = 0;
        break;
    case ui::DragNumberEvent::Commit:
        P.vol_key.clear();
        *out = std::clamp(d, kMinDb, kMaxDb);
        commit = std::fabs(*out - stored) > 1e-6;
        break;
    case ui::DragNumberEvent::Cancel: P.vol_key.clear(); break;
    default: break;
    }
    ImGui::PopID();
    return commit;
}

// The default open state of a Section (decision 8), seeded once in ImGui's storage.
void SeedSection(const char* title, bool open)
{
    ImGui::PushID(title);
    ImGuiStorage* st = ImGui::GetStateStorage();
    const ImGuiID id = ImGui::GetID("##open");
    if (st->GetInt(id, 2) == 2) st->SetBool(id, open);
    ImGui::PopID();
}

// "Avoid repeating [the last 2 v]" (the mockup's select: off, the last 1..3; a larger stored value
// is listed too). Returns the value picked (one undo point), else -1.
int AvoidControl(const char* id, int stored)
{
    auto label = [](int k) { return k <= 0 ? std::string("off") : "the last " + std::to_string(k); };
    FitText("Avoid repeating", ui::kMuted);
    const std::string cur = label(stored);
    const float       w = ImGui::CalcTextSize("the last 3").x + ui::Dp(12.0f + 16.0f + 6.0f);
    SameLineIfFits(w);
    int picked = -1;
    if (ui::BeginComboStyled(id, cur.c_str(), w)) {
        const int top = std::max(3, std::min(stored, 32));
        for (int k = 0; k <= top; ++k) {
            if (k > 3 && k != stored) continue;
            if (ImGui::Selectable(label(k).c_str(), k == stored) && k != stored) picked = k;
        }
        ui::EndComboStyled();
    }
    return picked;
}

void Hatch(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 col)
{
    dl->PushClipRect(a, b, true);
    // Freeze audit #1: only the strokes inside the clip rect, on the hatch's own phase, never more than a cap.
    const float     H = b.y - a.y;
    const dash::Run r = dash::Visible(a.x - H, b.x, 6.0, H, dl->GetClipRectMin().x, dl->GetClipRectMax().x);
    for (int i = 0; i < r.count; ++i) {
        const float x = static_cast<float>(r.first + i * r.step);
        dl->AddLine(ImVec2(x, b.y), ImVec2(x + H, a.y), col);
    }
    dl->PopClipRect();
}

// ---- waveform -------------------------------------------------------------------------------------

int FrameAt(const std::vector<SoundFrame>& frames, double t)
{
    for (size_t i = 0; i < frames.size(); ++i)
        if (t >= frames[i].start && t < frames[i].start + frames[i].length) return static_cast<int>(i);
    return -1;
}


}  // namespace

WaveformEvents DrawWaveform(const char* id, const std::string& path, const std::vector<SoundFrame>& frames,
                            const std::vector<int>& selection, double playhead_s, const WaveformOptions& o)
{
    // 13-62i: read only (the Sound panel, the Media Explorer preview): its variations framed, their peaks at their
    // zones' gains, their fades as placed, the playhead. A click reports the variation under it; Edit edits zones
    // (zone_wave_ui.h), never here.
    WaveformEvents ev;
    ImGui::PushID(id);
    const ImGuiID  gid = ImGui::GetID("##wave");
    const ImGuiIO& io = ImGui::GetIO();
    const float    w = std::max(8.0f, o.size.x > 0.0f ? o.size.x : ImGui::GetContentRegionAvail().x);
    const float    h = std::max(16.0f, o.size.y);
    const float    rh = o.ruler ? ui::Dp(18.0f + 4.0f) : 0.0f;  // .edruler: 18 px + 4 px under it
    const ImVec2   p0 = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##wave", ImVec2(w, h + rh), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    const bool   hovered = ImGui::IsItemHovered();
    const ImVec2 a(p0.x, p0.y + rh), b(p0.x + w, p0.y + rh + h);
    ev.min = a;
    ev.max = b;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    // 13-7b (audit C5.4): the mockup's .swave: --bg, a 1 px stroke edge, r5.
    dl->AddRectFilled(a, b, ui::kBg, ui::kRadiusSm);
    const double vs = o.view_start;
    const double vl = o.view_len > 0.0 ? o.view_len : o.source_len;
    const float  text_y = std::floor((a.y + b.y - ImGui::GetTextLineHeight()) * 0.5f);
    if (!(vl > 0.0)) {
        dl->AddText(ImVec2(a.x + ui::Dp(8.0f), text_y), ui::kMuted, "No audio");
        dl->AddRect(a, b, ui::kStroke, ui::kRadiusSm);
        ImGui::PopID();
        return ev;
    }
    // Freeze audit #1: kept within 4096 px of the waveform (deep zoom on a long file).
    auto X = [&](double t) { return dash::ClampCoord(a.x + (t - vs) / vl * w, a.x, static_cast<double>(a.x) + w); };
    auto T = [&](float x) { return vs + static_cast<double>(x - a.x) / w * vl; };
    // 13-14 (C5.4) / 13-71: a lane per channel (L above R); 13-41: a Sound forced to mono one lane, the channel
    // (or L + R) heard. 13-57: one column per pixel, or the samples as a line when a pixel is shorter.
    const int   ncols = std::max(1, static_cast<int>(std::floor(w)));
    WaveColumns wc;
    const bool  have_peaks = ReadWaveColumns(path, vs, vl, ncols, &wc);
    const ImVec2 m = io.MousePos;
    const int    under = hovered ? FrameAt(frames, T(m.x)) : -1;
    const int    playing_frame = playhead_s >= 0.0 ? FrameAt(frames, playhead_s) : -1;
    dl->PushClipRect(a, b, true);
    // Frames (.sbox): a box inset 2 px, accent 9 % (13 % under the mouse, 18 % selected / playing); an off frame
    // hatched.
    const float by0 = a.y + 2.0f, by1 = b.y - 2.0f;
    for (size_t i = 0; i < frames.size(); ++i) {
        const SoundFrame& f = frames[i];
        const ImVec2 fa(X(f.start), by0), fb(std::max(X(f.start + f.length), X(f.start) + 1.0f), by1);
        if (fb.x < a.x || fa.x > b.x) continue;
        const bool sel = std::find(selection.begin(), selection.end(), static_cast<int>(i)) != selection.end();
        const bool hot = static_cast<int>(i) == playing_frame;
        if (!f.on) {
            dl->AddRectFilled(fa, fb, WithAlpha(ui::kMuted, 0x14), 3.0f);
            Hatch(dl, fa, fb, ui::kHatch);
        } else {
            const int alpha = (sel || hot) ? 0x2E : (static_cast<int>(i) == under ? 0x21 : 0x17);
            dl->AddRectFilled(fa, fb, WithAlpha(ui::kAccent, alpha), 3.0f);
        }
    }
    // Peaks, each column at the gain it is placed at: its zone's (13-62i: every Sound's gains are its zones'),
    // accent inside a part zone of an on frame, muted elsewhere (outside, a Gap, empty space); past full scale in --bad.
    if (have_peaks) {
        struct Own {
            bool   in = false;
            double g = 1.0;
        };
        auto OwnAt = [&](double t) {
            Own r;
            const int fi = FrameAt(frames, t);
            if (fi < 0 || !frames[static_cast<size_t>(fi)].on) return r;
            const SoundFrame& f = frames[static_cast<size_t>(fi)];
            if (f.zones.empty()) {
                r.in = true;
                r.g = DbToGain(f.db);
                return r;
            }
            for (const EditZone& z : f.zones)
                if (!z.IsGap() && t >= z.a && t < z.b) {
                    r.in = true;
                    r.g = DbToGain(z.db);
                }
            return r;
        };
        WaveDrawStyle st;
        st.px = w / static_cast<float>(ncols);
        st.line_w = ui::Dp(1.5f);
        st.split = ui::kStrokeStrong;
        DrawWaveLanes(dl, wc, o.mono, a.x, a.y, b.y, st, [&](double pos, double, bool, WaveInk* out) {
            const Own r = OwnAt(vs + pos / ncols * vl);
            out[0].col = r.in ? ui::kAccent : ui::kMuted;
            out[0].g = r.g;
            out[0].clip = r.in ? ui::kBad : 0;
            return 1;
        });
    } else {
        ev.reading = RequestPeaks(path, nullptr) == PeakState::Building;
        dl->AddText(ImVec2(a.x + ui::Dp(8.0f), text_y), ui::kMuted, ev.reading ? "Reading waveform\xE2\x80\xA6" : "Waveform unavailable");
    }
    // The frames' edges and numbers over the peaks (accent when selected or playing; dashed when off), the number
    // struck through when off.
    ImFont* nf = o.big_numbers ? ui::FontMonoSmall() : ui::FontMonoMicro();
    for (size_t i = 0; i < frames.size(); ++i) {
        const SoundFrame& f = frames[i];
        const ImVec2 fa(X(f.start), by0), fb(std::max(X(f.start + f.length), X(f.start) + 1.0f), by1);
        if (fb.x < a.x || fa.x > b.x) continue;
        const bool sel = std::find(selection.begin(), selection.end(), static_cast<int>(i)) != selection.end();
        if (!f.on) ui::DashedRect(dl, fa, fb, ui::kStrokeStrong, 3.0f, 3.0f, 2.0f);
        else dl->AddRect(fa, fb, (sel || static_cast<int>(i) == playing_frame) ? ui::kAccent : ui::kStrokeStrong, 3.0f);
        if (fb.x - fa.x >= nf->FontSize + 4.0f) {
            char num[16];
            std::snprintf(num, sizeof(num), "%d", static_cast<int>(i) + 1);
            const ImVec2 np(fa.x + 3.0f, fa.y + 1.0f);
            ui::AddTextIn(dl, nf, np, sel ? ui::kAccentText : ui::kMuted, num);
            if (!f.on) {
                const ImVec2 ns = ui::TextSizeIn(nf, num);
                const float  sy = std::floor(np.y + ns.y * 0.55f) + 0.5f;
                dl->AddLine(ImVec2(np.x, sy), ImVec2(np.x + ns.x, sy), ui::kMuted);
            }
        }
    }
    if (playhead_s >= vs && playhead_s <= vs + vl) dl->AddLine(ImVec2(X(playhead_s), a.y), ImVec2(X(playhead_s), b.y), ui::kText, 1.0f);
    dl->PopClipRect();
    // Antho 2026-10-12: the fades, as placed (drawn as REAPER draws an item's).
    if (!o.fades.empty()) {
        const ImU32 shade = WithAlpha(ui::kBg, 0x90), line = WithAlpha(ui::kText, 0xB0);
        dl->PushClipRect(a, b, true);
        for (size_t i = 0; i < frames.size() && i < o.fades.size(); ++i) {
            const SoundFrame& f = frames[i];
            const FrameFades& fd = o.fades[i];
            if (f.start + f.length < vs || f.start > vs + vl) continue;
            constexpr int kSteps = 12;
            for (int side = -1; side <= 1; side += 2) {
                const double len = side < 0 ? fd.in : fd.out;
                if (!(len > 0.0)) continue;
                const double t0 = side < 0 ? f.start : f.start + f.length - len;
                ImVec2       prev;
                for (int k = 0; k <= kSteps; ++k) {
                    const double t = t0 + len * k / kSteps;
                    const double g = FadeGainAt(t - f.start, f.length, side < 0 ? fd.in : 0.0, side > 0 ? fd.out : 0.0, fd.in_shape,
                                                fd.out_shape);
                    const ImVec2 p(X(t), b.y - static_cast<float>(g) * (b.y - a.y));
                    if (k > 0) {
                        dl->AddQuadFilled(ImVec2(prev.x, a.y), ImVec2(p.x, a.y), p, prev, shade);
                        dl->AddLine(prev, p, line);
                    }
                    prev = p;
                }
            }
        }
        dl->PopClipRect();
    }
    dl->AddRect(a, b, ui::kStroke, ui::kRadiusSm);
    // The ruler (.edruler): ms below 2 s shown, else s.
    if (o.ruler) {
        const double steps[] = {0.001, 0.005, 0.01, 0.02, 0.05, 0.1, 0.2, 0.25, 0.5, 1, 2, 5, 10, 30, 60};
        double       st = steps[0];
        for (double s : steps) {
            st = s;
            if (s / vl * w >= 70.0) break;
        }
        ImFont*     rf = ui::FontMonoTiny();
        const float base = p0.y + rh - 4.0f;
        dl->AddLine(ImVec2(a.x, base - 0.5f), ImVec2(a.x + w, base - 0.5f), ui::kStroke);
        const double t0 = std::ceil(vs / st) * st;
        const int    nt = dash::TickCount(t0, vs + vl, st);  // freeze audit #8: capped, never endless
        for (int i = 0; i < nt; ++i) {
            const double t = t0 + i * st;
            const float  x = std::floor(X(t)) + 0.5f;
            dl->AddLine(ImVec2(x, base - 7.0f), ImVec2(x, base - 1.0f), ui::kStrokeStrong);
            char lab[32];
            if (st < 1.0) std::snprintf(lab, sizeof(lab), "%.0f ms", t * 1000.0);
            else std::snprintf(lab, sizeof(lab), "%g s", t);
            ui::AddTextIn(dl, rf, ImVec2(x + 2.0f, p0.y + 2.0f), ui::kFaint, lab);
        }
    }
    // A click (released where it was pressed): the variation under it; a right-click is the caller's.
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        P.wave_press_id = gid;
        P.wave_press = m;
    }
    if (P.wave_press_id == gid && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        if (hovered && std::fabs(m.x - P.wave_press.x) <= 4.0f && std::fabs(m.y - P.wave_press.y) <= 4.0f) {
            ev.clicked = FrameAt(frames, T(P.wave_press.x));
            ev.clicked_empty = ev.clicked < 0;
            ev.click_at = T(P.wave_press.x);
            ev.ctrl = io.KeyCtrl;
        }
        P.wave_press_id = 0;
    }
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        ev.right_clicked = true;
        ev.right_at = T(m.x);
    }
    ImGui::PopID();
    return ev;
}

void SetNodePanelHooks(const NodePanelHooks& hooks) { P.hooks = hooks; }

bool LibraryNodeTakesKey(unsigned vk, bool ctrl, bool shift, bool alt)
{
    const int  f = ImGui::GetCurrentContext() ? ImGui::GetFrameCount() : -100;
    const bool edit = P.edit_open && P.edit_frame >= f - 1;
    if (edit && P.edit_has_focus) {  // the view keys (Up / Down zoom too) zoom Edit, not the arrange view
        if (ViewNavRowForKeyDown(vk, ctrl, shift, alt) >= 0) return true;  // 13-45 (a character binding too)
    }
    // 13-62h: Delete removes the selected zones while Edit has the focus on a zoned Sound (else REAPER's).
    if (edit && P.edit_has_focus && P.edit_divided && !ctrl && !shift && !alt && vk == 0x2E) return true;
    if (ctrl || shift || alt) return false;
    // 13-63: S is free (the split is the "Split at cursor" Shortcuts row, KEYS: SOUND EDIT).
    if (edit) return vk == 0x20 || vk == 0x1B || vk == 'E';
    const bool sound = P.sound_frame >= f - 1;
    return sound && LibraryViewFocused() && vk == 'E';
}

bool LibraryEditUsesEsc()
{
    const int f = ImGui::GetCurrentContext() ? ImGui::GetFrameCount() : -100;
    return P.edit_open && P.edit_frame >= f - 1 && P.edit_has_focus;
}

bool LibraryEditKeysActive()
{
    return LibraryEditUsesEsc();  // the same: open, drawn last frame, focused
}

void RequestLibraryEditKey(int shortcut_id)
{
    if (!P.edit_open) return;
    if (shortcut_id == kShortcutEditTrimLeft || shortcut_id == kShortcutEditTrimRight || shortcut_id == kShortcutEditTrimZone ||
        shortcut_id == kShortcutEditSplit)
        P.trim_request = shortcut_id;
    else if (shortcut_id == kShortcutEditSolo || shortcut_id == kShortcutEditMute || shortcut_id == kShortcutEditExclSolo)  // 13-62 (D)
        P.listen_request = shortcut_id == kShortcutEditSolo ? 1 : shortcut_id == kShortcutEditMute ? 2 : 3;  // 13-67a: 3 exclusive
    else if (shortcut_id == kShortcutEditNextVar || shortcut_id == kShortcutEditPrevVar)  // 13-62F (F5)
        P.nav_request = shortcut_id == kShortcutEditNextVar ? 1 : -1;
    else if (shortcut_id == kShortcutEditHeal || shortcut_id == kShortcutEditLoop ||  // 13-62h (Lot D; Lot E does them)
             shortcut_id == kShortcutEditSnapOffset || shortcut_id == kShortcutEditPrevStop ||  // 13-67a
             shortcut_id == kShortcutEditSelectAll ||                                         // Sound Edit evening (A)
             (shortcut_id >= kShortcutEditMonoOff && shortcut_id <= kShortcutEditSideRule))  // 13-73: Mono, Left / Right
        P.zone_request = shortcut_id;
    else if (shortcut_id == kShortcutEditGainUp || shortcut_id == kShortcutEditGainDown)  // 13-74
        P.gain_request += shortcut_id == kShortcutEditGainUp ? 1 : -1;
}

namespace {

// 13-74: the Edit waveform under the mouse (its frame), the wheel's part of a notch not given yet (a trackpad's).
int   g_gain_wave_frame = -100;
float g_gain_wheel_acc = 0.0f;

// The gain row the wheel up / down runs with these modifiers (Shortcuts: its keys in force), -1 none.
int GainWheelRow(bool up, bool ctrl, bool shift, bool alt)
{
    const int id = ShortcutActionFor(CurrentShortcutKeymap(), up ? vk::kWheelUp : vk::kWheelDown, ctrl, shift, alt, false, false, true);
    return id == kShortcutEditGainUp || id == kShortcutEditGainDown ? id : -1;
}

// This frame's gain notches from the wheel over the Edit waveform (`hovered`): + = up. *taken: the wheel is the gain
// rows' (the view's zoom / scroll does not get it; Dear ImGui does not scroll with it from the next frame).
int EditGainWheel(bool hovered, ImGuiID owner, bool* taken)
{
    *taken = false;
    if (!hovered) {
        g_gain_wheel_acc = 0.0f;
        return 0;
    }
    g_gain_wave_frame = ImGui::GetFrameCount();
    const ImGuiIO& io = ImGui::GetIO();
    const int      up = GainWheelRow(true, io.KeyCtrl, io.KeyShift, io.KeyAlt);
    const int      dn = GainWheelRow(false, io.KeyCtrl, io.KeyShift, io.KeyAlt);
    if (up < 0 && dn < 0) {
        g_gain_wheel_acc = 0.0f;
        return 0;
    }
    ImGui::SetKeyOwner(ImGuiKey_MouseWheelY, owner);
    ImGui::SetKeyOwner(ImGuiKey_MouseWheelX, owner);  // Dear ImGui turns a Shift+wheel into a horizontal scroll
    *taken = io.MouseWheel > 0.0f ? up >= 0 : io.MouseWheel < 0.0f ? dn >= 0 : true;
    if (!*taken) return 0;
    g_gain_wheel_acc += io.MouseWheel;
    int n = 0;
    for (; g_gain_wheel_acc >= 1.0f; g_gain_wheel_acc -= 1.0f) n += up == kShortcutEditGainUp ? 1 : -1;
    for (; g_gain_wheel_acc <= -1.0f; g_gain_wheel_acc += 1.0f) n += dn == kShortcutEditGainUp ? 1 : -1;
    return n;
}

}  // namespace

bool LibraryEditClaimsWheel(bool horizontal, bool ctrl, bool shift, bool alt)
{
    if (horizontal || !ImGui::GetCurrentContext() || !P.edit_open || g_gain_wave_frame < ImGui::GetFrameCount() - 1) return false;
    return GainWheelRow(true, ctrl, shift, alt) >= 0 || GainWheelRow(false, ctrl, shift, alt) >= 0;
}

namespace {

// ---- Sound panel pieces ---------------------------------------------------------------------------

struct SoundCtx {
    std::string lib_uid, uid;
};

// ---- fades (Antho 2026-10-12: fade in / out on each variation, against clicks) ---------------------

// Each frame's fades as drawn: as placed and auditioned (a Whole-item Sound only shows a fade it set
// by hand: its placed copy keeps the source item's own fades).
std::vector<FrameFades> ShownFades(const SoundData& d)
{
    std::vector<FrameFades> out;
    if (FolderSourced(d)) return out;  // no fade handles: none is placed
    out.reserve(d.frames.size());
    for (size_t i = 0; i < d.frames.size(); ++i) {
        FrameFades f = EffectiveFades(d, d.frames[i]);
        // 13-62i: a variation laid out as zones fades as its first part zone starts and its last one ends.
        const std::vector<EditZone>& zs = d.frames[i].zones;
        int first = -1, last = -1;
        for (size_t k = 0; k < zs.size(); ++k)
            if (!zs[k].IsGap()) {
                if (first < 0) first = static_cast<int>(k);
                last = static_cast<int>(k);
            }
        if (first >= 0) {
            const ZoneFadeInfo fi = ZoneFade(d, {static_cast<int>(i), first}, -1), fo = ZoneFade(d, {static_cast<int>(i), last}, 1);
            f.in = fi.s;
            f.out = fo.s;
            f.in_shape = fi.src == FadeSrc::Own ? zs[static_cast<size_t>(first)].fade_in_shape : 0;
            f.out_shape = fo.src == FadeSrc::Own ? zs[static_cast<size_t>(last)].fade_out_shape : 0;
            f.own_in = fi.src == FadeSrc::Own;
            f.own_out = fo.src == FadeSrc::Own;
        }
        out.push_back(f);
    }
    return out;
}

std::string PartName(const SoundData& d, const std::string& value);  // 13-62F (below)
std::string PartNumberToast(const std::string& pname, const char* label, PartField f, double v, const char* unit);
std::string LaneValue(const SoundData& d, int lane);

// "Fades in [ms] out [ms]": the Sound's default fades, for every variation with none of its own.
// A drag previews, its release (or a typed value) is one undo point.
void DefaultFadeControls(const SoundLibrary& lib, const SoundNode& n)
{
    const SoundData& d = Shown(n);
    if (FolderSourced(d)) return;  // a folder pile is placed without fades
    // 13-7b (audit C5.5): a .row -- the muted 80 px label, then "in [ms] out [ms]" as .lnum fields.
    const float fw = ui::Dp(52.0f);
    ui::LabelRow("Fades", ui::TextWidth("out") + ImGui::GetStyle().ItemSpacing.x + ui::NumberFieldWidth(fw, "ms"));
    ui::ItemTipText("Every variation fades in and out by this much (against clicks), unless it has its own fade\n"
        "(drag a fade handle at a variation's top corner in the waveform; Alt-click it for this default)");
    for (int side = -1; side <= 1; side += 2) {
        if (side > 0) SameLineIfFits(ui::TextWidth("out") + ImGui::GetStyle().ItemSpacing.x + ui::NumberFieldWidth(fw, "ms"));
        FitText(side < 0 ? "in" : "out", ui::kMuted);
        ImGui::SameLine();
        double v = (side < 0 ? d.fade_in : d.fade_out) * 1000.0;
        const ui::DragNumberEvent e = ui::DragNumber(side < 0 ? "##fadein" : "##fadeout", &v, 0.1, 1, "ms", fw, /*trim=*/true);
        if (e == ui::DragNumberEvent::Live || e == ui::DragNumberEvent::Commit) {
            SoundData&   dr = Live(n);
            const double sec = std::clamp(v, 0.0, 10000.0) / 1000.0;
            SetDefaultFades(dr, side < 0 ? sec : dr.fade_in, side < 0 ? dr.fade_out : sec);
            if (e == ui::DragNumberEvent::Commit) {
                if (dr.fade_in == n.sound.fade_in && dr.fade_out == n.sound.fade_out) CancelLive();  // no empty undo point
                else CommitLive("Default fades", lib.uid, n.uid);
                if (StoreReplaced()) return;
            }
        } else if (e == ui::DragNumberEvent::Cancel) {
            CancelLive();
        }
    }
}

// ---- the Edit view: zoom and scroll ---------------------------------------------------------------

constexpr float kScrollbarH = 10.0f;

// Multiplies the shown length by `factor` keeping the source time `at` where it is.
void ZoomView(double factor, double at, double src_len)
{
    if (!(src_len > 0.0)) return;
    const double vl = P.view_len > 0.0 ? P.view_len : src_len;
    const double nl = std::clamp(vl * factor, 0.01, std::max(0.01, src_len));
    P.view_start = std::clamp(at - (at - P.view_start) * nl / vl, 0.0, std::max(0.0, src_len - nl));
    P.view_len = nl >= src_len ? 0.0 : nl;
}

void ScrollView(double ds, double src_len)
{
    const double vl = P.view_len > 0.0 ? P.view_len : src_len;
    P.view_start = std::clamp(P.view_start + ds, 0.0, std::max(0.0, src_len - vl));
}

// A horizontal scrollbar under Edit's waveform: drag the thumb, or click the track to page.
void ViewScrollbar(double src_len)
{
    const float  w = std::max(8.0f, ImGui::GetContentRegionAvail().x);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##hscroll", ImVec2(w, kScrollbarH));
    if (!(src_len > 0.0)) return;
    const double vl = P.view_len > 0.0 ? P.view_len : src_len;
    const float  t0 = p.x + static_cast<float>(P.view_start / src_len) * w;
    const float  t1 = std::max(t0 + 12.0f, p.x + static_cast<float>((P.view_start + vl) / src_len) * w);
    ImDrawList*  dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + kScrollbarH), ui::kBg, kScrollbarH * 0.5f);
    const bool on_thumb = ImGui::IsItemHovered() && ImGui::GetIO().MousePos.x >= t0 && ImGui::GetIO().MousePos.x <= t1;
    dl->AddRectFilled(ImVec2(t0, p.y + 1.0f), ImVec2(std::min(t1, p.x + w), p.y + kScrollbarH - 1.0f),
                      ImGui::IsItemActive() || on_thumb ? ui::kMuted : ui::kStrokeStrong, kScrollbarH * 0.5f);
    if (ImGui::IsItemClicked() && !on_thumb) ScrollView(ImGui::GetIO().MousePos.x < t0 ? -vl : vl, src_len);
    else if (ImGui::IsItemActive() && ImGui::GetIO().MouseDelta.x != 0.0f)
        ScrollView(static_cast<double>(ImGui::GetIO().MouseDelta.x) / w * src_len, src_len);
}

// A zoom (13-45: any view action, wheel or key): around the mouse when it is over the waveform, else
// the view's middle.
void KeyZoom(double factor)
{
    const double len = P.wave_len;
    if (!(len > 0.0)) return;
    const double vl = P.view_len > 0.0 ? P.view_len : len;
    const float  mx = ImGui::GetIO().MousePos.x;
    double       at = P.view_start + vl * 0.5;
    if (P.wave_x1 > P.wave_x0 && mx >= P.wave_x0 && mx <= P.wave_x1)
        at = P.view_start + static_cast<double>(mx - P.wave_x0) / (P.wave_x1 - P.wave_x0) * vl;
    ZoomView(factor, at, len);
}

// 13-45: one frame's view actions in Edit (view_nav.h, the same for every timeline). Horizontal: the
// source view, zoomed around the mouse (1.25 per notch) and scrolled by 10 % of it per notch (a page:
// all of it) -- not in Takes mode, where each take shows whole. Vertical: the lanes taller than the box
// (Zoom vertically, up to 8 x) and the box scrolled (Scroll vertically). Call inside ##editwave.
void EditNavigate(const ViewNavStep& st)
{
    const double len = P.wave_len;
    if (len > 0.0) {
        if (st.zoom_h != 0.0) KeyZoom(std::pow(0.8, st.zoom_h));
        const double vl = P.view_len > 0.0 ? P.view_len : len;
        const double ds = (st.scroll_h * 0.1 + st.scroll_h_pages) * vl;
        if (ds != 0.0) ScrollView(ds, len);
    }
    if (st.zoom_v != 0.0) P.vzoom = std::clamp(P.vzoom * std::pow(1.25, st.zoom_v), 1.0, 8.0);
    const double dy = st.scroll_v * 0.15 + st.scroll_v_pages;
    if (dy != 0.0) ImGui::SetScrollY(ImGui::GetScrollY() + static_cast<float>(dy) * ImGui::GetWindowHeight());
}

// ---- 13-20 / 13-23 / 13-25: parts, zones and sync points in Edit ---------------------------------------

// A value's name in a group ("?" when gone).
std::string ValueName(const SoundStore& store, const std::string& group_uid, const std::string& value_uid)
{
    if (const SwitchGroup* g = FindGroup(store, group_uid))
        for (const SwitchValue& v : g->values)
            if (v.uid == value_uid) return v.name.empty() ? std::string("(unnamed value)") : v.name;
    return "?";
}

// A part's colour: the curve palette by the value's index (ui::kCurvePalette).
ImU32 PartColor(int k) { return ui::kCurvePalette[static_cast<size_t>(std::max(0, k)) % 8]; }

// ---- 13-62: per-part controls (stubs of Lot A; the next lots fill them, their signatures are fixed) -----

// ---- 13-62 (Lot E2): the per-part settings UI ---------------------------------------------------------
// A divided Sound's part owns a setting by touching it (decision 2): its row then shows a bar in the part's
// colour and a ↺ (back to the Sound's); an untouched row shows the Sound's value, muted, "= Sound". Every
// change is one undo point (EditNode; a drag: Live + CommitLive). Mute / Solo / the selected part are view
// state (P.part_*): never saved, no undo point. The part rows and the lane headers write to the Sound of the
// Edit window's library (P.edit_lib): the Contract's `int lib` carries nothing.

void LoadNormalizePrefs();  // 13-28 (below)
void PartDot(ImU32 col);    // (below)
static void PartNormalizeButton(int lib, const std::string& node_uid, const std::string& value);  // Lot C (below)

// The Sound `uid` of the Edit window's library and that library (nullptr when gone).
const SoundNode* EditSoundOf(const std::string& uid, const SoundLibrary** lib_out)
{
    const SoundLibrary* lib = FindLibrary(GetSoundStore(nullptr), P.edit_lib);
    const SoundNode*    n = lib ? FindNode(*lib, uid) : nullptr;
    if (lib_out) *lib_out = lib;
    return n && n->type == SoundNodeType::Sound ? n : nullptr;
}

// The Edit window's library's index in the store (-1 when gone).
int EditLibIndex()
{
    const SoundStore& store = GetSoundStore(nullptr);
    for (size_t i = 0; i < store.libraries.size(); ++i)
        if (store.libraries[i].uid == P.edit_lib) return static_cast<int>(i);
    return -1;
}

std::string PartName(const SoundData& d, const std::string& value)
{
    if (value == kWholePart) return "Sound";  // 13-62i: an undivided Sound's one part
    return ValueName(GetSoundStore(nullptr), d.divide_group, value);
}

// C1: a part's Normalize mode / target: its own, else the Sound's (the user's Normalize settings, Level).
NormMode PartNormModeOf(const SoundData& d, const std::string& value) { return PartNormMode(d, value, P.norm_mode); }
double   PartNormTargetOf(const SoundData& d, const std::string& value) { return PartNormTarget(d, value, P.norm_db); }

// 13-62h: a settings number row (the Detection card's Threshold / Min gap, every number row of Settings of): what
// it shows and takes. The field takes lo..hi, the fader spans flo..fhi (a value past it shows the fader at its end).
struct NumRow {
    const char* id;
    const char* label;
    const char* unit;
    double      step, lo, hi;  // what the field takes
    float       flo, fhi;      // the fader's span (the mockup's ranges)
    const char* tip;
};

// How a caller draws it: its label's column and colour, its field's tone (0 = the text's), a "+" on a positive
// value, a faint hint between label and fader ("before (-) / after"), and the room it draws after the field on the
// same line (a pill, a reset button).
struct NumRowLook {
    float       label_w = 0.0f;
    ImU32       label_col = ui::kText;
    ImU32       text_col = 0;
    bool        sign = false;
    const char* hint = nullptr;
    float       after_w = 0.0f;
    // 13-74 (the v1 mockup 13-67): a yellow tick on the fader at this value (< 0 or off its span: none), its tip when
    // the mouse is on it (the Tail's: where the first zone starts to be stopped).
    double      mark = -1.0;
    std::string mark_tip;
};

// What the row did this frame: the event (ui::DragNumberEvent's meaning, from the fader or the field) and the value
// (within lo..hi) to preview (Live) or enter (Commit).
struct NumRowOut {
    ui::DragNumberEvent e = ui::DragNumberEvent::None;
    double              v = 0.0;
    bool                fader = false;  // from the fader (else the field)
};

// 13-62h: the one settings number row (the Detection card's design, everywhere): its label, a fader that edits
// live, then the typed / dragged field beside it (under it when the column is narrow: flow::StretchBeside). The
// fader reports Live once it has moved, Commit on release, Cancel on Esc after typing; a click that never moved it
// reports nothing (it changes nothing). The field's events are passed on. `key` names the row (node + row + part):
// its fader's live value is shown on that row only. `value`: what the row shows when not dragged. The caller
// applies Live on its draft (Live) and enters Commit once (CommitLive; CancelLive when nothing changed).
NumRowOut SettingsNumberRow(const std::string& key, const NumRow& f, double value, const NumRowLook& k)
{
    NumRowOut   out;
    const float sp = ImGui::GetStyle().ItemSpacing.x, fw = ui::Dp(52.0f), nw = ui::NumberFieldWidth(fw, f.unit);
    const float fader_min = ui::Dp(40.0f), tail = nw + (k.after_w > 0.0f ? k.after_w + sp : 0.0f);
    ImGui::PushID(f.id);
    ui::LabelRow(f.label, nw + fader_min, k.label_w, k.label_col);
    if (k.hint) {  // left out when the fader would get under its least
        const float hw = std::min(ImGui::CalcTextSize(k.hint).x, ImGui::GetContentRegionAvail().x - fader_min - tail - 2.0f * sp);
        if (hw >= ui::Dp(24.0f)) {
            FitText(k.hint, ui::kMuted, hw);
            ImGui::SameLine();
        }
    }
    float       fv = static_cast<float>(P.numf_key == key ? P.numf_live : value);
    bool        beside = true;
    const float room = flow::StretchBeside(ImGui::GetContentRegionAvail().x, fader_min, tail, sp, &beside);
    ImGui::SetNextItemWidth(std::max(1.0f, room));
    ui::FaderOpts fo;
    const bool    marked = k.mark >= 0.0 && k.mark >= f.flo && k.mark <= f.fhi;  // 13-74
    if (marked) {
        fo.mark_on = true;
        fo.mark = static_cast<float>(k.mark);
    }
    const bool moved = ui::Fader("##fader", &fv, f.flo, f.fhi, 0.0f, fo);
    const bool active = ImGui::IsItemActive(), released = ImGui::IsItemDeactivated();
    bool       on_mark = false;
    if (marked && !k.mark_tip.empty() && ImGui::IsItemHovered() && !active) {
        const float mx = flow::FaderThumbX(fo.mark, ImGui::GetItemRectMin().x, ImGui::GetItemRectMax().x, ui::Dp(8.0f), f.flo, f.fhi);
        on_mark = std::fabs(ImGui::GetIO().MousePos.x - mx) <= ui::Dp(4.0f);
    }
    if (on_mark) ui::ShowTip(k.mark_tip.c_str());
    else if (f.tip) ui::ItemTipText(f.tip);
    if (active && P.numf_key != key) {  // a gesture begins: from the value itself (not its float)
        P.numf_key = key;
        P.numf_live = value;
        P.numf_moved = false;
        P.detf_field = nullptr;
    }
    if (P.numf_key == key && (active || released)) {
        if (moved) {
            P.numf_live = std::clamp(static_cast<double>(fv), f.lo, f.hi);
            P.numf_moved = true;
        }
        if (active) {
            P.numf_gid = ImGui::GetItemID();
            P.numf_seen = ImGui::GetFrameCount();
        }
        if (P.numf_moved) {
            out.e = !released                ? ui::DragNumberEvent::Live
                    : ui::FaderCancelled()   ? ui::DragNumberEvent::Cancel  // Esc after typing: nothing entered
                                             : ui::DragNumberEvent::Commit;
            out.v = P.numf_live;
            out.fader = true;
        }
        if (released) {
            P.numf_key.clear();
            P.numf_gid = 0;
        }
    }
    if (beside) ImGui::SameLine();
    double                    v = out.fader ? out.v : P.numf_key == key ? P.numf_live : value;
    const ui::DragNumberEvent e = ui::DragNumber("##num", &v, f.step, 1, f.unit, fw, /*trim=*/true, k.text_col, k.sign);
    if (f.tip) ui::ItemTipText(f.tip);
    ImGui::PopID();
    if (out.e == ui::DragNumberEvent::None && e != ui::DragNumberEvent::None) {
        out.e = e;
        out.v = std::clamp(v, f.lo, f.hi);
    }
    return out;
}

// 13-74: the Tail fader's tick tip (the v1 mockup's): "From 42 ms the first zone is stopped short".
std::string TailMarkTip(double ms)
{
    char buf[96];
    std::snprintf(buf, sizeof(buf), "From %.0f ms the first zone is stopped short", ms);
    return buf;
}

// A number row (the mockup's FIELDS, part side). `unit` nullptr = the Normalize mode's.
struct PartNum {
    PartField   field;
    NumRow      row;
};

// What a part follows on a row it does not own: the Sound's (gain: 0 dB; Normalize target: C1).
double SoundPartNumber(const SoundData& d, PartField f)
{
    switch (f) {
        case PartField::Preroll: return d.silence.preroll_ms;
        case PartField::Tail: return d.silence.tail_ms;
        case PartField::SyncOffset: return d.sync_offset_ms;
        case PartField::NormTarget: return P.norm_db;
        case PartField::FadeIn: return d.fade_in * 1000.0;
        case PartField::FadeOut: return d.fade_out * 1000.0;
        default: return 0.0;
    }
}

// The row's bar (.prow .bar: 4 px in the part's colour, left of its label) when the part owns the row.
void PartRowBar(bool own, ImU32 col)
{
    if (!own) return;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float  h = ImGui::GetFrameHeight();
    ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(p.x - ui::Dp(7.0f), p.y + 1.0f), ImVec2(p.x - ui::Dp(3.0f), p.y + h - 1.0f),
                                              col, ui::Dp(2.0f));
}

// A faint pill (.inh) after a row's value: "= Sound", "at the Gap", "▼ by hand" (left out when its line has no
// room; ellipsized when only part of it fits). `tip`: its tooltip.
void RowPill(const std::string& text, ImU32 ink, const char* tip)
{
    const float full = ImGui::CalcTextSize(text.c_str()).x + ui::Dp(12.0f), h = ImGui::GetFrameHeight();
    const float w = std::min(full, std::max(ui::Dp(30.0f), ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x));
    if (!ui::SameLineIfFits(std::min(full, ui::Dp(30.0f)))) return;  // its line has no room: the value says it already
    const float  room = std::min(w, std::max(1.0f, ui::LineRight() - ImGui::GetCursorScreenPos().x));
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList*  dl = ImGui::GetWindowDrawList();
    const float  th = ImGui::GetTextLineHeight(), ph = th + ui::Dp(2.0f);
    const ImVec2 a(p.x, p.y + std::floor((h - ph) * 0.5f)), b(p.x + room, a.y + ph);
    dl->AddRect(a, b, ink == ui::kWarn ? ui::kWarnLine : ui::kStroke, ph * 0.5f);
    ui::EllipsisText(dl, ImVec2(p.x + ui::Dp(6.0f), a.y + ui::Dp(1.0f)), b.x - ui::Dp(5.0f), ink, text);
    ImGui::Dummy(ImVec2(room, h));
    if (tip) ui::ItemTipText(tip);
}

// "= Sound" (.inh): a faint pill after a value the part follows (left out when its line has no room).
void InheritHint(const std::string& pname)
{
    const std::string tip = "Follows the Sound: type here to give " + pname + " its own";
    RowPill("= Sound", ui::kFaint, tip.c_str());
}

// The row's ↺ at the right end of its line, the part owning the row. True when clicked. `tip` nullptr = "Same as
// the Sound again".
bool PartResetButton(const char* id, bool own, const char* tip = nullptr)
{
    if (!own) return false;
    const float side = ui::ControlH();
    const bool  beside = ui::SameLineIfFits(side);
    const float y = ImGui::GetCursorScreenPos().y;
    const float x = beside ? std::max(ImGui::GetCursorScreenPos().x, ui::LineRight() - side) : ui::LineRight() - side;
    ImGui::SetCursorScreenPos(ImVec2(x, y));
    const bool clicked = ui::SolidButton((std::string("\xE2\x86\xBA##rst") + id).c_str(), ImVec2(side, side));
    ui::ItemTipText(tip ? tip : "Same as the Sound again");
    return clicked;
}

// ↺: one setting of the part back to the Sound's (one undo point; none when nothing changes).
void ResetPartField(const SoundLibrary& lib, const SoundNode& n, const std::string& path, const std::string& value,
                    PartField f, const std::string& pname)
{
    const bool remeasure = PartFieldRemeasures(f);
    // 13-62h (decision 7): on a zoned Sound a Pre-roll / Tail back to the Sound's moves the hand edges again (option).
    const bool pretail = (f == PartField::Preroll || f == PartField::Tail) && UsesZones(n.sound);
    const bool wins = ZoneTailWins();
    int        hand = 0;
    if (UsesZones(n.sound)) {  // 13-62h review: the write probed on a copy, the selection carried over
        SoundData probe = n.sound;
        ResetPartSetting(probe, value, f);
        if (remeasure) RefreshWith(probe, path);
        if (pretail) hand = ApplyPreTailChange(probe, value, wins, f, ZoneCtxOf(MediaOf(n)));
        ZoneSelAfter(n.sound, probe);
    }
    const double len = MediaOf(n).source_len;
    EditNode("Part setting", lib.uid, n.uid, [value, f, path, remeasure, pretail, wins, len](SoundNode& x) {
        const PartSettings* p = PartSettingsOf(x.sound, value);
        const PartSettings  before = p ? *p : PartSettings{};
        const bool          had = p != nullptr;
        ResetPartSetting(x.sound, value, f);
        if (SamePartSettings(had ? &before : nullptr, PartSettingsOf(x.sound, value))) return false;
        if (remeasure) RefreshWith(x.sound, path);
        if (pretail) {
            PeakData   pk;
            const bool has = !path.empty() && RequestPeaks(path, &pk) == PeakState::Ready;
            ApplyPreTailChange(x.sound, value, wins, f, ZoneContext{has ? &pk : nullptr, len});
        }
        return true;
    }, pname + ": same as the Sound again" + PreTailToastExtra(hand, wins));
}

// A picked setting (a segment, the Normalize mode): the part's own from now on (one undo point).
void SetPartNow(const SoundLibrary& lib, const SoundNode& n, const std::string& path, const std::string& value,
                const PartSettings& patch, bool remeasure, const std::string& toast)
{
    EditNode("Part setting", lib.uid, n.uid, [value, patch, path, remeasure](SoundNode& x) {
        const PartSettings* p = PartSettingsOf(x.sound, value);
        const PartSettings  before = p ? *p : PartSettings{};
        const bool          had = p != nullptr;
        SetPartSetting(x.sound, value, patch);
        if (SamePartSettings(had ? &before : nullptr, PartSettingsOf(x.sound, value))) return false;
        if (remeasure) RefreshWith(x.sound, path);
        return true;
    }, toast);
}

// A dragged / typed number of a part (`v` in the row's unit): live while dragged, one undo point at its end
// (none when nothing changed). True when the drawing must stop (the store replaced).
bool PartNumberEdit(ui::DragNumberEvent e, double v, const SoundLibrary& lib, const SoundNode& n, const std::string& path,
                    const std::string& value, PartField f, const std::string& toast)
{
    if (e == ui::DragNumberEvent::Live || e == ui::DragNumberEvent::Commit) {
        if (!P.zedit_on) {  // the selection when the gesture began (a relayout may re-resolve it)
            P.zedit_on = true;
            P.zedit_sel = P.zv.sel;
        }
        SoundData& dr = Live(n);
        SetPartSetting(dr, value, PartNumberPatch(f, v));
        if (PartFieldRemeasures(f)) RefreshWith(dr, path);
        // 13-62h (decision 7): a zoned Sound's Pre-roll / Tail moves its hand edges again (option), in the same write.
        int hand = 0;
        if ((f == PartField::Preroll || f == PartField::Tail) && UsesZones(dr))
            hand = ApplyPreTailChange(dr, value, ZoneTailWins(), f, ZoneCtxOf(MediaOf(n)));
        // 13-62h review: the selection of the gesture's start carried over by time and part (the relayout may drop or
        // shift zones).
        if (UsesZones(dr)) P.zv.sel = SelFollow(n.sound, P.zedit_sel, dr);
        if (e == ui::DragNumberEvent::Commit) {
            P.zedit_on = false;
            if (SamePartSettings(PartSettingsOf(dr, value), PartSettingsOf(n.sound, value))) {
                CancelLive();
            } else {
                if (UsesZones(dr)) MarkZoneOwn();
                CommitLive("Part setting", lib.uid, n.uid, toast + PreTailToastExtra(hand, ZoneTailWins()));
            }
            return StoreReplaced();
        }
    } else if (e == ui::DragNumberEvent::Cancel) {
        if (P.zedit_on) P.zv.sel = P.zedit_sel;
        P.zedit_on = false;
        CancelLive();
    }
    return false;
}

// "Toe · Tail: 40 ms (its own)" (the mockup's toast).
std::string PartNumberToast(const std::string& pname, const char* label, PartField f, double v, const char* unit)
{
    return pname + " \xC2\xB7 " + label + ": " + RoundedNumberText(v, f == PartField::Gain) + " " + unit + " (its own)";
}

// One number row of a part (the mockup's .prow): its own-value bar, the shared row (SettingsNumberRow), then
// "= Sound" while it follows the Sound, else its ↺. True when the drawing must stop.
bool PartNumberRow(const SoundLibrary& lib, const SoundNode& n, const std::string& path, const std::string& value,
                   const std::string& pname, ImU32 col, const PartNum& f)
{
    const SoundData&            d = Shown(n);
    const std::optional<double> own = OwnPartNumber(PartSettingsOf(d, value), f.field);
    NumRow                      row = f.row;
    if (!row.unit) row.unit = NormModeUnit(PartNormModeOf(d, value));
    NumRowLook look;
    look.label_w = ui::kLabelCol;
    look.label_col = own ? ui::kText : ui::kMuted;
    look.text_col = own ? 0 : ui::kMuted;
    look.sign = f.field == PartField::Gain;
    // Offset: "before (−) / after" the attack or peak, "after the start" (the Sound's row), before its fader.
    look.hint = f.field != PartField::SyncOffset                 ? nullptr
                : PartSyncMode(d, value) == SyncMode::SliceStart ? "after the start"
                                                                 : "before (\xE2\x88\x92) / after";
    look.after_w = own ? ui::ControlH() : std::min(ImGui::CalcTextSize("= Sound").x + ui::Dp(12.0f), ui::Dp(56.0f));
    if (f.field == PartField::Tail && UsesZones(d)) {  // 13-74: where its first zone starts to be stopped
        look.mark = TailCappedZones(d, ZoneCtxOf(MediaOf(n)), value).first_ms;
        if (look.mark >= 0.0) look.mark_tip = TailMarkTip(look.mark);
    }
    PartRowBar(own.has_value(), col);
    const NumRowOut o = SettingsNumberRow(n.uid + "/" + value + row.id, row, own ? *own : SoundPartNumber(d, f.field), look);
    if (PartNumberEdit(o.e, o.v, lib, n, path, value, f.field, PartNumberToast(pname, row.label, f.field, o.v, row.unit)))
        return true;
    if (!own) InheritHint(pname);
    if (PartResetButton(row.id, own.has_value())) ResetPartField(lib, n, path, value, f.field, pname);
    return false;
}

// "Heel fader +2 dB: every Heel zone +2.0 dB (they keep their differences)" (the mockup's toast).
std::string FaderToast(const std::string& pname, double value, double delta)
{
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%s%.1f dB", delta > 0.0 ? "+" : "", delta);
    return pname + " fader " + RoundedNumberText(value, true) + " dB: every " + pname + " zone " + buf + " (they keep their differences)";
}

// 13-62h: a part's fader dragged / typed (dB): every zone of the part moved by the same amount, live while dragged
// (SetPartFader on the draft, from the stored value), one undo point at its end (none when nothing changed). True
// when the drawing must stop. 13-62h review: one range for every box of it (the lane header's, Settings of's Gain):
// -60..+24 dB, a zone's gain range; the lane fader itself drags over -24..+12 and shows its end pinned past it.
bool PartFaderEdit(ui::DragNumberEvent e, double v, const SoundLibrary& lib, const SoundNode& n, const std::string& value,
                   const std::string& pname)
{
    v = std::clamp(v, -60.0, 24.0);
    if (e == ui::DragNumberEvent::Live || e == ui::DragNumberEvent::Commit) {
        SoundData& dr = Live(n);
        SetPartFader(dr, value, v, ZoneCtxOf(MediaOf(n)));
        if (e == ui::DragNumberEvent::Commit) {
            const double was = PartFaderDb(n.sound, value), now = PartFaderDb(dr, value);
            if (SameZoneEdit(dr, n.sound)) CancelLive();
            else CommitLive("Part fader", lib.uid, n.uid, FaderToast(pname, now, now - was));
            return StoreReplaced();
        }
    } else if (e == ui::DragNumberEvent::Cancel) {
        CancelLive();
    }
    return false;
}

// A segment row of a part (Sync point, Mono). `*picked` = the option picked (-1 = none): a pick makes it the
// part's own, even the value it follows (decision 2). True when its ↺ was clicked.
bool PartSegmentRow(const char* id, const char* label, const char* const* labels, const char* const* tips, int count,
                    int current, bool own, ImU32 col, int* picked)
{
    ImGui::PushID(id);
    PartRowBar(own, col);
    ui::LabelRow(label, ui::Dp(120.0f), ui::kLabelCol, own ? ui::kText : ui::kMuted);
    int        sel = current;
    const bool hit = ui::Segmented("##seg", labels, count, &sel, 0.0f, -1, nullptr, nullptr, /*follow=*/!own);
    const int  hov = ui::SegmentedHovered();
    if (hov >= 0 && hov < count) ui::ShowTip(tips[hov]);
    const bool reset = PartResetButton(id, own);
    ImGui::PopID();
    *picked = hit && sel >= 0 && sel < count && (sel != current || !own) ? sel : -1;
    return reset;
}

// The rows of part `value` (the mockup's partRowsHTML, part side): Detection (Pre-roll and Tail in Silence
// mode, where the Sound has its margins; Sync point, Offset), Level (Gain, Normalize, Target, Mono, then
// its Normalize button), Fades (Fade in, Fade out). Threshold, Min gap, Divide by and At cuts stay the
// Sound's.
static void PartSettingsRows(const char* id, int lib, const std::string& node_uid, const std::string& value)
{
    (void)lib;  // the Edit window's library (see above)
    const SoundLibrary* l = nullptr;
    const SoundNode*    n = EditSoundOf(node_uid, &l);
    if (!n || !l) return;
    // Each row reads the Sound again (Shown(*n), PartSettingsOf): a row's Live() write before it may have
    // moved what Shown returns, and a pointer into the part list kept across it would be stale.
    auto own_of = [n, &value]() { return PartSettingsOf(Shown(*n), value); };
    {
        const SoundData& d0 = Shown(*n);
        if (PartIndex(d0, value) < 0 || value == kWholePart) return;  // 13-62i: Takes mode too
    }
    LoadNormalizePrefs();  // C1: the Normalize rows' "= Sound" are the Level section's settings
    const SoundMedia&  m = MediaOf(*n);
    const std::string  path = m.path, pname = PartName(Shown(*n), value);
    const ImU32        col = PartColor(PartIndex(Shown(*n), value));
    const DetectMode   mode = Shown(*n).mode;
    ImGui::PushID(id);
    ImGui::PushID(value.c_str());
    auto stop = [] {
        ImGui::PopID();
        ImGui::PopID();
    };

    ui::GroupCaption("Detection", ui::kMuted);
    (void)mode;  // 13-62h: a zoned Sound's zone edges follow Pre-roll / Tail in every mode
    {
        static const PartNum kPre = {PartField::Preroll, {"##pre", "Pre-roll", "ms", 0.5, 0.0, 1000.0, 0.0f, 50.0f,
                                     "Automatic zone starts: this much before the detected onset (never before a cut)"}};
        static const PartNum kTail = {PartField::Tail, {"##tail", "Tail", "ms", 1.0, 0.0, 5000.0, 0.0f, 300.0f,
                                      "Automatic zone ends: this much after the sound dies (never past a cut)"}};
        if (PartNumberRow(*l, *n, path, value, pname, col, kPre) || PartNumberRow(*l, *n, path, value, pname, col, kTail))
            return stop();
        // 13-67b: why a longer Tail stops lengthening some of this part's zones
        const std::string capped = TailCapsText(TailCappedZones(Shown(*n), ZoneCtxOf(m), value));
        if (!capped.empty()) ui::ToneText(capped.c_str(), ui::kMuted, true);
    }
    {
        static const char* const kModes[3] = {"Attack", "Peak", "Start"};
        static const char* const kTips[3] = {"Where the first transient crosses the threshold", "The loudest transient's peak",
                                             "The slice start, pre-roll included"};
        const PartSettings* ps = own_of();
        const bool          own = ps && ps->sync_mode;
        int                 picked = -1;
        if (PartSegmentRow("##sync", "Snap offset", kModes, kTips, 3, static_cast<int>(PartSyncMode(Shown(*n), value)), own, col,
                           &picked))
            ResetPartField(*l, *n, path, value, PartField::SyncMode, pname);
        else if (picked >= 0) {
            PartSettings patch;
            patch.sync_mode = static_cast<SyncMode>(picked);
            SetPartNow(*l, *n, path, value, patch, true, pname + " \xC2\xB7 Snap offset at " + kModes[picked] + " (its own)");
        }
        if (StoreReplaced()) return stop();
    }
    {
        static const PartNum kOff = {PartField::SyncOffset, {"##soff", "Offset", "ms", 0.1, -50.0, 50.0, -50.0f, 50.0f,
                                     "Moves the snap offset from the attack, the peak or the zone start (ms)"}};
        if (PartNumberRow(*l, *n, path, value, pname, col, kOff)) return stop();
    }

    ImGui::Dummy(ImVec2(1.0f, ui::Dp(4.0f)));
    ui::GroupCaption("Level", ui::kMuted);
    {  // 13-62h: Gain = the part's fader (what it has applied to every zone of the part), no setting of its own; its
       // fader spans the lane fader's -24..+12 dB, the field the zone gains' -60..+24
        static const NumRow kGain = {"##gain", "Gain", "dB", 0.1, -60.0, 24.0, -24.0f, 12.0f,
                                     "The fader: moves the gain of every zone of this part by the same amount (they keep "
                                     "their differences); shows what it has applied"};
        NumRowLook look;
        look.label_w = ui::kLabelCol;
        look.sign = true;
        const NumRowOut o = SettingsNumberRow(n->uid + "/" + value + kGain.id, kGain, PartFaderDb(Shown(*n), value), look);
        if (PartFaderEdit(o.e, o.v, *l, *n, value, pname)) return stop();
    }
    {
        const PartSettings* ps = own_of();
        const bool          own = ps && ps->norm_mode;
        const NormMode      cur = PartNormModeOf(Shown(*n), value);
        PartRowBar(own, col);
        const float w = ui::LabelRow("Normalize", ui::Dp(110.0f), ui::kLabelCol, own ? ui::kText : ui::kMuted);
        const float side = own ? ui::ControlH() + ImGui::GetStyle().ItemSpacing.x : 0.0f;
        if (ui::BeginComboStyled("##nmode", NormModeLabel(cur), std::max(ui::Dp(60.0f), w - side), 0, ui::TagTone::Neutral,
                                 /*follow=*/!own)) {
            for (int j = 0; j < kNormModeCount; ++j) {
                const NormMode md = static_cast<NormMode>(j);
                if (ImGui::Selectable(NormModeLabel(md), md == cur) && (md != cur || !own)) {
                    PartSettings patch;
                    patch.norm_mode = j;
                    SetPartNow(*l, *n, path, value, patch, false, pname + " \xC2\xB7 Normalize: " + NormModeLabel(md) + " (its own)");
                }
            }
            ui::EndComboStyled();
        }
        if (StoreReplaced()) return stop();
        ui::ItemTipText(own ? "Its own Normalize mode (REAPER's)" : "Normalize mode (REAPER's): the Sound's; pick one to give it its own");
        if (PartResetButton("##nmode", own)) ResetPartField(*l, *n, path, value, PartField::NormMode, pname);
        if (StoreReplaced()) return stop();
    }
    {
        static const PartNum kTgt = {PartField::NormTarget, {"##ntgt", "Target", nullptr, 0.1, kNormTargetMin, kNormTargetMax,
                                     -60.0f, 0.0f, "Its Normalize target"}};
        if (PartNumberRow(*l, *n, path, value, pname, col, kTgt)) return stop();
    }
    {
        static const char* const kLabels[4] = {"Off", "L", "R", "Sum"};
        static const char* const kTips[4] = {"As the file: a stereo file stays stereo", "The left channel alone, on both sides",
                                             "The right channel alone, on both sides", "Left + right mixed into one channel"};
        const PartSettings* ps = own_of();
        const bool          own = ps && ps->mono;
        int                 picked = -1;
        if (PartSegmentRow("##mono", "Mono", kLabels, kTips, 4, static_cast<int>(PartMono(Shown(*n), value)), own, col, &picked))
            ResetPartField(*l, *n, path, value, PartField::Mono, pname);
        else if (picked >= 0) {
            PartSettings patch;
            patch.mono = static_cast<MonoMode>(picked);
            SetPartNow(*l, *n, path, value, patch, false, pname + " \xC2\xB7 Mono: " + kLabels[picked] + " (its own)");
        }
        if (StoreReplaced()) return stop();
    }
    // Lot C: "Normalize <Part>" (the library's real index: Edit's).
    PartNormalizeButton(EditLibIndex(), node_uid, value);
    if (StoreReplaced()) return stop();

    if (!FolderSourced(Shown(*n))) {  // a folder pile is placed without fades
        ImGui::Dummy(ImVec2(1.0f, ui::Dp(4.0f)));
        ui::GroupCaption("Fades", ui::kMuted);
        static const PartNum kIn = {PartField::FadeIn, {"##fin", "Fade in", "ms", 0.1, 0.0, 10000.0, 0.0f, 50.0f,
                                                    "Default fade in of its zones"}};
        static const PartNum kOut = {PartField::FadeOut, {"##fout", "Fade out", "ms", 0.1, 0.0, 10000.0, 0.0f, 50.0f,
                                                      "Default fade out of its zones"}};
        if (PartNumberRow(*l, *n, path, value, pname, col, kIn) || PartNumberRow(*l, *n, path, value, pname, col, kOut))
            return stop();
    }
    stop();
}

// Mute / Solo of a part (decision 5): listening only, never saved, no undo point; Solo adds to the other
// soloed parts (REAPER).
void ToggleListen(bool solo, const SoundData& d, const std::string& value)
{
    std::set<std::string>& s = solo ? P.part_solo : P.part_mute;
    const bool             on = s.insert(value).second;
    if (!on) s.erase(value);
    const std::string name = PartName(d, value);
    std::string       toast;
    if (solo && on) {
        std::string all;
        for (const std::string& v : d.divide_values)
            if (P.part_solo.count(v)) all += (all.empty() ? "" : " + ") + PartName(d, v);
        toast = "Solo " + name + ": Play hears " + all + " only (listening, not saved)";
    } else if (solo) {
        toast = name + " unsoloed";
    } else {
        toast = on ? name + " muted on Play (listening, not saved)" : name + " heard again";
    }
    ShowLibraryToast(toast, false);
}

// The M / S (Mute / Solo) button (.btn.mute / .btn.solo): raised; on = tinted red (mute) / amber (solo).
bool ListenButton(const char* id, const char* label, bool solo, bool on, ImVec2 size)
{
    const float  w = size.x > 0.0f ? size.x : ui::ButtonWidth(label), h = size.y > 0.0f ? size.y : ui::ControlH();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const bool   clicked = ImGui::InvisibleButton(id, ImVec2(w, h));
    const bool   hov = ImGui::IsItemHovered();
    ImU32        fill = hov ? ui::kHover : ui::kRaised, edge = ui::kStrokeStrong, ink = ui::kText;
    if (on) {
        fill = solo ? WithAlpha(ui::kWarn, 0x2E) : ui::kBadSoft;
        edge = solo ? WithAlpha(ui::kWarn, 0x99) : WithAlpha(ui::kBad, 0x99);
        ink = solo ? ui::kWarn : ui::kBadInk;
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), fill, ui::Dp(ui::kRadiusSm));
    dl->AddRect(p, ImVec2(p.x + w, p.y + h), edge, ui::Dp(ui::kRadiusSm));
    const ImVec2 ts = ImGui::CalcTextSize(label);
    ui::EllipsisText(dl, ImVec2(p.x + std::max(ui::Dp(2.0f), std::floor((w - ts.x) * 0.5f)), p.y + std::floor((h - ts.y) * 0.5f)),
                     p.x + w - ui::Dp(2.0f), ink, label);
    // REAPER's key named only while the Shortcuts row follows REAPER (Lot D's rule), else "your key".
    const bool reaper = ShortcutFollowsReaper(solo ? kShortcutEditSolo : kShortcutEditMute);
    const std::string tip = std::string(solo ? "Solo" : "Mute") + " the selected part (" +
                            (reaper ? std::string("your REAPER key for ") + (solo ? "40281" : "40280") : std::string("your key")) +
                            "). Listening only, not saved.";
    ui::ItemTipText(tip.c_str());
    return clicked;
}

// A "Settings of" chip (.pchip): the part's dot, its name, S / M when soloed / muted, its own-setting count
// (amber). `on` = selected, tinted in its colour. PartChipWidth = its natural width.
struct PartChipLook {
    bool dot = false, solo = false, mute = false;
    int  count = 0;
};

float PartChipExtras(const PartChipLook& k, float* right)
{
    const float gap = ui::Dp(6.0f), r = ui::Dp(4.0f);
    const float bw = ui::TextSizeIn(ui::FontMonoMicro(), "S").x + ui::Dp(6.0f);
    const float cw = k.count > 0 ? ui::TextSizeIn(ui::FontMonoTiny(), std::to_string(k.count).c_str()).x + ui::Dp(10.0f) : 0.0f;
    const float rx = (k.solo ? gap + bw : 0.0f) + (k.mute ? gap + bw : 0.0f) + (k.count > 0 ? gap + cw : 0.0f);
    if (right) *right = rx;
    return (k.dot ? r * 2.0f + gap : 0.0f) + rx;
}

float PartChipWidth(const std::string& name, const PartChipLook& k)
{
    return ui::Dp(17.0f) + PartChipExtras(k, nullptr) + ImGui::CalcTextSize(name.c_str()).x;
}

bool PartChip(const char* id, const std::string& name, ImU32 col, const PartChipLook& k, bool on, const char* tip)
{
    float       right_x = 0.0f;
    const float extras = PartChipExtras(k, &right_x);
    const float h = ui::FieldH(), padl = ui::Dp(8.0f), padr = ui::Dp(9.0f), gap = ui::Dp(6.0f), r = ui::Dp(4.0f);
    const float w = std::max(padl + extras + padr + ui::Dp(12.0f),
                             std::min(PartChipWidth(name, k), std::max(1.0f, ImGui::GetContentRegionAvail().x)));
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const bool   clicked = ImGui::InvisibleButton(id, ImVec2(w, h));
    const bool   hov = ImGui::IsItemHovered();
    ImDrawList*  dl = ImGui::GetWindowDrawList();
    const ImVec2 b(p.x + w, p.y + h);
    dl->AddRectFilled(p, b, on ? WithAlpha(col, 0x2E) : hov ? ui::kHover : ui::kRaised, h * 0.5f);
    dl->AddRect(p, b, on ? col : ui::kStrokeStrong, h * 0.5f);
    float       x = p.x + padl;
    const float cy = p.y + h * 0.5f;
    if (k.dot) {
        dl->AddCircleFilled(ImVec2(x + r, cy), r, col);
        x += r * 2.0f + gap;
    }
    const float name_max = b.x - padr - right_x;
    ui::EllipsisText(dl, ImVec2(x, std::floor(cy - ImGui::GetTextLineHeight() * 0.5f)), name_max, ui::kText, name);
    x = std::min(x + ImGui::CalcTextSize(name.c_str()).x, name_max);
    ImFont* badge = ui::FontMonoMicro();
    auto    badge_at = [&](const char* t, ImU32 bg, ImU32 ink) {
        x += gap;
        const float bw = ui::TextSizeIn(badge, t).x + ui::Dp(6.0f), bh = badge->FontSize + ui::Dp(4.0f);
        dl->AddRectFilled(ImVec2(x, cy - bh * 0.5f), ImVec2(x + bw, cy + bh * 0.5f), bg, ui::Dp(3.0f));
        ui::AddTextIn(dl, badge, ImVec2(x + ui::Dp(3.0f), std::floor(cy - badge->FontSize * 0.5f)), ink, t);
        x += bw;
    };
    if (k.solo) badge_at("S", WithAlpha(ui::kWarn, 0x40), ui::kWarn);
    if (k.mute) badge_at("M", ui::kBadSoft, ui::kBadInk);
    if (k.count > 0) {
        ImFont*           cf = ui::FontMonoTiny();
        const std::string cs = std::to_string(k.count);
        const float       cw = ui::TextSizeIn(cf, cs.c_str()).x + ui::Dp(10.0f), ch = cf->FontSize + ui::Dp(4.0f);
        x += gap;
        dl->AddRectFilled(ImVec2(x, cy - ch * 0.5f), ImVec2(x + cw, cy + ch * 0.5f), ui::kWarn, ch * 0.5f);
        ui::AddTextIn(dl, cf, ImVec2(x + ui::Dp(5.0f), std::floor(cy - cf->FontSize * 0.5f)), IM_COL32(0x10, 0x13, 0x1A, 0xFF),
                      cs.c_str());
    }
    if (tip) ui::ItemTipText(tip);
    return clicked;
}

// The lane header's fader being dragged (13-62h: the part's fader): its id, Sound, part, live value, whether it
// moved, the last frame it was drawn (SettleLostFaders ends a drag whose header went away).
struct LaneGainDrag {
    ImGuiID     gid = 0;
    std::string lib, uid, value, name;
    double      live = 0.0;
    bool        moved = false;
    int         seen = -1;
};
LaneGainDrag g_lane_gain;

void ClearLaneGain() { g_lane_gain = LaneGainDrag{}; }

ZoneWaveHost ZoneHost(const SoundLibrary& lib, const SoundNode& n, const SoundMedia& m, int take = -1);  // 13-62h (below)
void ZoneNormalizeGo(const SoundLibrary& lib, const SoundNode& n, const SoundMedia& m, const std::vector<std::vector<ZoneRef>>& groups,
                     const std::string& label, bool reset_faders);  // 13-62h (below)

// Split view: the lane header of part `part_k` (the mockup's laneHeadHTML), drawn in [min, max]: its colour
// bar, the dot, the name, M, S; then (wide) the gain fader + its dB field and the Normalize target ("= -1
// dBFS" while it follows the Sound) with "⋯ n" (every setting of the part in a popup: PartSettingsRows).
// `narrow` = the mockup's .narrow .lhead (104-112 px): everything but the fader (name, M, S, dB field, target,
// "⋯ n"); under ~92 px, name over M S only. A click elsewhere on it selects the part (like a REAPER track).
// Rows that do not fit the lane's height are left out. The cursor is left where it was. Returns the width it used.
// Its items are submitted here: an item of the caller over [min, max] submitted before would take the mouse.
[[maybe_unused]] static float PartLaneHeader(const char* id, const SoundData& d, int part_k, ImVec2 min, ImVec2 max,
                                             bool narrow)
{
    if (!IsDivided(d) || part_k < 0 || part_k >= PartCount(d) || max.x - min.x < 4.0f || max.y - min.y < 4.0f)
        return 0.0f;
    const SoundLibrary* lib = nullptr;
    const SoundNode*    n = EditSoundOf(P.edit_uid, &lib);
    if (!n || !lib) return 0.0f;
    const std::string value = d.divide_values[static_cast<size_t>(part_k)];
    const std::string name = PartName(d, value);
    const ImU32       col = PartColor(part_k);
    const bool        sel = P.part_sel == value;
    const ImVec2      cursor0 = ImGui::GetCursorScreenPos();
    ImDrawList*       dl = ImGui::GetWindowDrawList();
    ImGui::PushID(id);
    ImGui::PushID(part_k);
    // .lhead: surface (selected: #26324c), its colour bar on the left (3 px at .55; selected 4 px), its right edge.
    dl->AddRectFilled(min, max, sel ? IM_COL32(0x26, 0x32, 0x4C, 0xFF) : ui::kSurface);
    dl->AddRectFilled(min, ImVec2(min.x + ui::Dp(sel ? 4.0f : 3.0f), max.y), sel ? col : WithAlpha(col, 0x8C));
    dl->AddLine(ImVec2(max.x - 0.5f, min.y), ImVec2(max.x - 0.5f, max.y), ui::kStrokeStrong);
    const float padl = ui::Dp(9.0f), padr = ui::Dp(6.0f), padt = ui::Dp(5.0f), gap = ui::Dp(4.0f);
    const float x0 = min.x + padl, x1 = std::max(x0 + 1.0f, max.x - padr), bottom = max.y - ui::Dp(3.0f);
    const float bw = ui::Dp(22.0f), bh = ui::Dp(20.0f), ms_w = bw * 2.0f + ui::Dp(3.0f), th = ImGui::GetTextLineHeight();
    float       y = min.y + padt;
    bool        mute = false, solo = false;
    auto        name_at = [&](float ny, float right) {
        const float r = ui::Dp(4.0f);
        dl->AddCircleFilled(ImVec2(x0 + r, ny + th * 0.5f), r, col);
        ui::FontScope fs(ui::FontSemibold());
        ui::EllipsisTextTip(dl, ImVec2(x0 + r * 2.0f + ui::Dp(5.0f), ny), right, ui::kText, name);
    };
    auto ms_at = [&](float mx, float my) {
        ImGui::SetCursorScreenPos(ImVec2(mx, my));
        mute = ListenButton("##lmute", "M", false, P.part_mute.count(value) > 0, ImVec2(bw, bh));
        ImGui::SetCursorScreenPos(ImVec2(mx + bw + ui::Dp(3.0f), my));
        solo = ListenButton("##lsolo", "S", true, P.part_solo.count(value) > 0, ImVec2(bw, bh));
    };
    if (x1 - x0 < ms_w + ui::Dp(30.0f)) {
        // Narrower than the mockup's narrowest (104 px): the name over M S when both fit the lane's height,
        // else M S alone (the colour bar names the lane).
        if (y + th + gap + bh <= bottom) {
            name_at(y, x1);
            y += th + gap;
        }
        ms_at(x0, y);
    } else {
        name_at(y + std::floor((bh - th) * 0.5f), x1 - ms_w - gap);
        ms_at(x1 - ms_w, y);
        y += bh + gap;
        const float row_h = ui::ControlH();
        // Gain: the fader (-24..+12 dB, live, one undo point on release) and its dB field ("+3"). `narrow`
        // (the mockup's .narrow .lhead) hides only the fader.
        if (y + row_h <= bottom) {
            // 13-62h: the part's fader (-24..+12 dB) -- not a gain of its own: it moves every zone of the part by the
            // same amount and shows what it has applied (PartFaderDb). Live while dragged (SetPartFader on the draft,
            // from the stored value), one undo point on release; double-click: back to 0 dB (takes it off again).
            const float  fw = ui::Dp(46.0f), fader_w = narrow ? 0.0f : x1 - x0 - fw - gap;
            const double stored = PartFaderDb(d, value);
            const bool   live = g_lane_gain.gid != 0 && g_lane_gain.uid == n->uid && g_lane_gain.value == value;
            const SoundMedia&  med = MediaOf(*n);
            const std::string  fader_tip = "Every " + name + " zone together (= select them all and drag their gain): they keep their "
                                          "differences. Shows what it has applied to them (-24..+12 dB here; type in its box for -60..+24). "
                                          "Double-click: back to 0 dB (takes it off again)";
            bool         zero = false;
            if (fader_w >= ui::Dp(24.0f)) {
                float fv = static_cast<float>(live ? g_lane_gain.live : stored);
                ImGui::SetCursorScreenPos(ImVec2(x0, y));
                const bool moved = ui::Fader("##lgain", &fv, -24.0f, 12.0f, fader_w);
                const ImGuiID gid = ImGui::GetItemID();
                const bool active = ImGui::IsItemActive(), released = ImGui::IsItemDeactivated();
                const bool cancelled = released && ui::FaderCancelled();
                zero = ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
                ui::ItemTipText(fader_tip.c_str());
                if (live && g_lane_gain.gid == gid) g_lane_gain.seen = ImGui::GetFrameCount();
                if (zero) {  // the double-click's own drag gives way to the 0 dB write below
                    if (live) CancelLive();
                    ClearLaneGain();
                } else if (cancelled && live) {
                    ClearLaneGain();
                    CancelLive();
                } else if (active && !live) {  // a new gesture: nothing written until it moves
                    g_lane_gain = LaneGainDrag{gid, lib->uid, n->uid, value, name, PartFaderDb(n->sound, value), false, ImGui::GetFrameCount()};
                }
                if (!zero && !cancelled && (active || released) && g_lane_gain.gid == gid && g_lane_gain.uid == n->uid &&
                    g_lane_gain.value == value) {
                    // 13-62h review: the fader jumps to a click; only a drag past ImGui's threshold (or a typed value:
                    // no button held) counts, so a plain click writes nothing and a double-click is one undo point (0 dB).
                    const bool counts = ImGui::IsMouseDragging(ImGuiMouseButton_Left) || !ImGui::IsMouseDown(ImGuiMouseButton_Left);
                    if (moved && counts) {
                        g_lane_gain.live = std::clamp(std::round(static_cast<double>(fv) * 2.0) / 2.0, -24.0, 12.0);
                        g_lane_gain.moved = true;
                    }
                    if (g_lane_gain.moved) SetPartFader(Live(*n), value, g_lane_gain.live, ZoneCtxOf(med));
                    if (released) {
                        const bool wrote = g_lane_gain.moved;
                        ClearLaneGain();
                        if (wrote) {
                            const SoundData& dr = Shown(*n);
                            const double     was = PartFaderDb(n->sound, value), now = PartFaderDb(dr, value);
                            if (SameZoneEdit(dr, n->sound)) CancelLive();
                            else CommitLive("Part fader", lib->uid, n->uid, FaderToast(name, now, now - was));
                        }
                    }
                }
            }
            if (!zero && !StoreReplaced()) {
                const bool still = g_lane_gain.gid != 0 && g_lane_gain.uid == n->uid && g_lane_gain.value == value;
                double     v = still ? g_lane_gain.live : stored;
                ImGui::SetCursorScreenPos(ImVec2(fader_w >= ui::Dp(24.0f) ? x1 - fw : x0, y));
                const ui::DragNumberEvent e = ui::DragNumber("##lgaindb", &v, 0.1, 1, "", fw, /*trim=*/true, 0, /*sign=*/true);
                zero = ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && e == ui::DragNumberEvent::None;
                const std::string box_tip = "What this fader has applied to every " + name + " zone (-60..+24 dB, as a zone's gain)";
                ui::ItemTipText(box_tip.c_str());
                if (!zero && PartFaderEdit(e, v, *lib, *n, value, name)) {
                    ImGui::PopID();
                    ImGui::PopID();
                    ImGui::SetCursorScreenPos(cursor0);
                    return max.x - min.x;
                }
            }
            if (zero && std::fabs(PartFaderDb(n->sound, value)) >= 0.05) {
                const std::string v = value, nm = name;
                const double      len = med.source_len;
                const std::string path = med.path;
                EditNode("Part fader", lib->uid, n->uid, [v, len, path](SoundNode& x) {
                    PeakData   pk;
                    const bool has = !path.empty() && RequestPeaks(path, &pk) == PeakState::Ready;
                    return SetPartFader(x.sound, v, 0.0, ZoneContext{has ? &pk : nullptr, len});
                }, nm + " fader 0 dB: taken off every " + nm + " zone (they keep their own gains)");
            }
            if (StoreReplaced()) {
                ImGui::PopID();
                ImGui::PopID();
                ImGui::SetCursorScreenPos(cursor0);
                return max.x - min.x;
            }
            y += row_h + gap;
        }
        // "n zones · = -1 dBFS": its zone count and its Normalize target ("= " while it follows the Sound), then "⋯ n":
        // every setting in a popup.
        if (y + row_h <= bottom) {
            LoadNormalizePrefs();
            const SoundData&    dn = Shown(*n);
            const PartSettings* ps = PartSettingsOf(dn, value);
            const bool          nown = ps && (ps->norm_mode || ps->norm_target);
            const int           cnt = OwnSettingCount(dn, value);
            const int           nz = static_cast<int>(ZonesOfPart(dn, value).size());
            char                tgt[96];
            std::snprintf(tgt, sizeof(tgt), "%d %s \xC2\xB7 %s%s %s", nz, nz == 1 ? "zone" : "zones", nown ? "" : "= ",
                          FormatNormTarget(PartNormTargetOf(dn, value)).c_str(), NormModeUnit(PartNormModeOf(dn, value)));
            const std::string more = "\xE2\x8B\xAF" + (cnt > 0 ? " " + std::to_string(cnt) : std::string());
            const float       mw = std::min(ui::ButtonWidth(more.c_str()), x1 - x0);
            ui::EllipsisTextTip(dl, ImVec2(x0, y + std::floor((row_h - th) * 0.5f)), x1 - mw - gap, ui::kMuted, tgt,
                                nown ? "Normalize target (its own)" : "Normalize target (= Sound)");
            ImGui::SetCursorScreenPos(ImVec2(x1 - mw, y));
            if (ui::SolidButton((more + "##lmore").c_str(), ImVec2(mw, 0.0f))) {
                P.part_sel = value;
                ImGui::OpenPopup("##lanepop");
            }
            ui::ItemTip("All of %s's settings", name.c_str());
        }
    }
    // A click on the header elsewhere selects the part (its items above take the mouse first).
    ImGui::SetCursorScreenPos(min);
    if (ImGui::InvisibleButton("##lhead", ImVec2(max.x - min.x, max.y - min.y))) P.part_sel = value;
    // 13-62h: double-click a track header = every zone of its part selected (view state).
    const bool all_zones = ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
    if (ImGui::IsItemHovered()) ui::ItemTip("%s: click to pick it; double-click: select all its zones", name.c_str());
    if (all_zones && UsesZones(d)) ZoneSelectPart(ZoneHost(*lib, *n, MediaOf(*n)), value);
    if (mute) ToggleListen(false, d, value);
    if (solo) ToggleListen(true, d, value);
    // "⋯": the part's settings (the mockup's cpopHTML), the rows of the side strip.
    ImGui::SetNextWindowSizeConstraints(ImVec2(ui::Dp(290.0f), 0.0f), ImVec2(ui::Dp(290.0f), std::numeric_limits<float>::max()));
    if (ui::BeginPanelPopup("##lanepop")) {
        const float side = ui::ControlH();
        PartDot(col);
        FitText(name + " \xC2\xB7 its settings", ui::kText,
                std::max(1.0f, ImGui::GetContentRegionAvail().x - side - ImGui::GetStyle().ItemSpacing.x));
        ImGui::SameLine();
        ImGui::SetCursorScreenPos(ImVec2(std::max(ImGui::GetCursorScreenPos().x, ui::LineRight() - side), ImGui::GetCursorScreenPos().y));
        if (ui::IconOnlyButton("##lpclose", "\xE2\x9C\x95", "Close", side)) ImGui::CloseCurrentPopup();
        ImGui::Dummy(ImVec2(1.0f, ui::Dp(2.0f)));
        ImGui::Indent(ui::Dp(8.0f));  // room for the own bars left of the labels
        PartSettingsRows("lanepop", 0, n->uid, value);
        ImGui::Unindent(ui::Dp(8.0f));
        ui::EndPanelPopup();
    }
    ImGui::PopID();
    ImGui::PopID();
    ImGui::SetCursorScreenPos(cursor0);
    return max.x - min.x;
}

// The Normalize button of a part's Level rows: "" = the Sound's "Normalize per part" button, else
// "Normalize <Part>" (Lot C fills it, Lot E2 calls it). It draws nothing unless `node_uid` is the Sound
// shown in Edit (it acts on Edit's selected variations and Edit's measuring job).
void PartNormalizeNow(int lib, const std::string& node_uid, const std::string& value);  // 13-62 C (LEVEL, below)
[[maybe_unused]] static void PartNormalizeButton(int lib, const std::string& node_uid, const std::string& value)
{
    PartNormalizeNow(lib, node_uid, value);
}

// 13-62F (F3) / 13-62h (Antho): a panel size the user drags by its edge (logical px), remembered per user (a pref,
// like the Normalize ones): view state, no undo point. The lane headers' width, the Selection panel's height and
// the right column's width.
struct PanelSize {
    const char* pref;
    float       def, lo;
    float       v = -1.0f;  // < 0 = not read yet
    // 13-62F review: a drag of the edge: the size and the mouse at the grab, and whether it moved yet.
    bool  on = false, moved = false;
    float v0 = 0.0f, m0 = 0.0f;
    float Get()
    {
        if (v < 0.0f) v = std::max(lo, LoadPrefFloat(pref, def));
        return v;
    }
};
constexpr float kLaneHeadDefault = 160.0f, kLaneHeadMin = 104.0f;
PanelSize       g_lane_head{"lib_lane_head_w", kLaneHeadDefault, kLaneHeadMin};
PanelSize       g_zsel_h{"lib_zone_sel_h", 230.0f, 90.0f};      // the Selection panel under the waveform
PanelSize       g_side_w{"lib_edit_side_w", 270.0f, 200.0f};    // Edit's right column (.edmain: 1fr 270px)

float LaneHeadPref()
{
    return g_lane_head.Get();
}

// The edge of a panel (a ui::SplitterBar of `size`, the cursor at its top-left): drag = its size by the mouse's
// travel along the bar's axis (`sign` -1: the panel grows when the mouse goes left / up), kept in [lo, hi];
// double-click = back to its default. 13-62F review: it moves only once the mouse really moved (a plain click, or a
// double-click's first click, never changes nor saves it); saved on release.
void PanelSizeBar(const char* id, ImVec2 size, bool vertical, float sign, PanelSize& s, float hi, const char* tip)
{
    const ui::SplitterState sp = ui::SplitterBar(id, size, vertical);
    const float             scale = std::max(0.01f, ui::Dp(1.0f));
    const float             m = vertical ? ImGui::GetIO().MousePos.x : ImGui::GetIO().MousePos.y;
    hi = std::max(s.lo, hi);
    s.Get();
    auto grab = [&]() {
        s.on = true;
        s.moved = false;
        s.v0 = s.v;
        s.m0 = m;
    };
    if (sp.double_clicked) {
        s.v = s.def;
        SavePrefFloat(s.pref, s.v);
        grab();
    } else if (sp.active) {
        if (!s.on) grab();
        if (std::fabs(m - s.m0) > ui::Dp(3.0f)) s.moved = true;
        if (s.moved) s.v = std::clamp(s.v0 + sign * (m - s.m0) / scale, s.lo, hi);
    }
    if (sp.released || (!sp.active && s.on)) {
        if (s.moved) SavePrefFloat(s.pref, s.v);
        s.on = false;
        s.moved = false;
    }
    if (sp.hovered && !sp.active) ui::ItemTipText(tip);
}

// What Edit draws of a Sound's parts.
WaveParts EditParts(const SoundStore& store, const SoundData& d)
{
    WaveParts w;
    w.data = &d;
    if (IsDivided(d))
        for (size_t k = 0; k < d.divide_values.size(); ++k) {
            w.colors.push_back(PartColor(static_cast<int>(k)));
            w.names.push_back(ValueName(store, d.divide_group, d.divide_values[k]));
        }
    w.split = d.lanes_split;
    w.head_px = LaneHeadPref();  // 13-62F (F3)
    // 13-62: the selected part and who Play hears (Mute / Solo add up as in REAPER).
    if (IsDivided(d)) {
        if (PartIndex(d, P.part_sel) >= 0) w.sel_part = P.part_sel;
        for (const std::string& v : d.divide_values)
            w.heard.push_back(!P.part_mute.count(v) && (P.part_solo.empty() || P.part_solo.count(v)));
    }
    return w;
}

// ---- 13-62 (decisions 9-11): comping on Edit's waveform ------------------------------------------------

// The part of a Split lane ("" for the Combined waveform, the comp strip or a bad index).
std::string LaneValue(const SoundData& d, int lane)
{
    return lane >= 0 && static_cast<size_t>(lane) < d.divide_values.size() ? d.divide_values[static_cast<size_t>(lane)] : std::string();
}

// 13-62 (decision 1, C6): Split's lane headers, one per part in the column DrawWaveform left on the left of
// its lanes. PartLaneHeader draws each (its card, name, M, S, gain, Normalize target, "...", and its own
// click-to-select); while it draws nothing, a base card with a dot and the name, a click on it selecting the
// part like selecting a REAPER track (no undo).
void LaneHeaders(const WaveformEvents& ev, const SoundData& d, const WaveParts& wp)
{
    const ImVec2 back = ImGui::GetCursorScreenPos();
    ImDrawList*  dl = ImGui::GetWindowDrawList();
    const float  gap = ui::Dp(4.0f), r = ui::Dp(3.5f);
    for (int k = 0; k < PartCount(d) && static_cast<size_t>(k) < d.divide_values.size(); ++k) {
        const std::string& v = d.divide_values[static_cast<size_t>(k)];
        const ImVec2       mn(ev.head_x, ev.lanes_y + ev.lane_h * static_cast<float>(k));
        const ImVec2       mx(ev.head_x + ev.head_w - gap, mn.y + ev.lane_h - (k + 1 < PartCount(d) ? 1.0f : 0.0f));
        if (mx.y - mn.y < 4.0f || mx.x - mn.x < 4.0f) continue;
        ImGui::PushID(k);
        const std::string id = "##lanehead" + std::to_string(k);
        if (PartLaneHeader(id.c_str(), d, k, mn, mx, ev.head_narrow) <= 0.0f) {
            // Not drawn by its header: a base card (surface, the part's colour on the left, the accent edge when
            // selected), its dot and name, and a click on it selects the part.
            const bool  sel = P.part_sel == v;
            const ImU32 col = static_cast<size_t>(k) < wp.colors.size() ? wp.colors[static_cast<size_t>(k)] : ui::kAccent;
            dl->AddRectFilled(mn, mx, sel ? ui::kHover : ui::kSurface, ui::kRadiusSm);
            dl->AddRectFilled(mn, ImVec2(mn.x + ui::Dp(3.0f), mx.y), col, ui::kRadiusSm, ImDrawFlags_RoundCornersLeft);
            if (sel) dl->AddRect(mn, mx, ui::kAccentLine, ui::kRadiusSm);
            const float  ty = mn.y + ui::Dp(5.0f);
            const ImVec2 c(mn.x + ui::Dp(12.0f), ty + ImGui::GetTextLineHeight() * 0.5f);
            dl->AddCircleFilled(c, r, col);
            const std::string nm = static_cast<size_t>(k) < wp.names.size() ? wp.names[static_cast<size_t>(k)] : std::string("?");
            ui::EllipsisTextTip(dl, ImVec2(c.x + r + ui::Dp(5.0f), ty), mx.x - ui::Dp(4.0f), ui::kText, nm);
            ImGui::SetCursorScreenPos(mn);
            if (ImGui::InvisibleButton("##lanesel", ImVec2(mx.x - mn.x, mx.y - mn.y))) P.part_sel = v;
            // 13-62h (E1 review): double-click = every zone of its part selected, as its chip.
            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && UsesZones(d)) {
                const SoundLibrary* l = nullptr;
                if (const SoundNode* sn = EditSoundOf(P.edit_uid, &l); sn && l) ZoneSelectPart(ZoneHost(*l, *sn, MediaOf(*sn)), v);
            }
            ui::ItemTipText((nm + ": click to pick it; double-click: select all its zones").c_str());
        }
        ImGui::PopID();
    }
    // 13-62F (F3): the column's right edge (the 4 px between the headers and the lanes) resizes it: drag, double-click
    // = back to 160 px. Kept between 104 px and half the waveform's width; saved on release (view state, no undo).
    const float lanes_h = ev.lane_h * static_cast<float>(PartCount(d));
    if (lanes_h >= 4.0f) {
        ImGui::SetCursorScreenPos(ImVec2(ev.head_x + ev.head_w - gap, ev.lanes_y));
        const float scale = std::max(0.01f, ui::Dp(1.0f));
        PanelSizeBar("##laneheadw", ImVec2(gap, lanes_h), true, 1.0f, g_lane_head, (ev.max.x - ev.head_x) * 0.5f / scale,
                     "Drag: the lane headers' width. Double-click: back to the default");
    }
    ImGui::SetCursorScreenPos(back);
}

// 13-62h (Lot E1): what the zone waveform of a zoned Sound needs of the Edit window (zone_wave_ui.h): the view
// state in P.zv, the edit cursor, Settings of, Mute / Solo, the view, the Live / CommitLive / CancelLive drafts and
// one-write gestures (EditNode, one undo point each). Built each frame; `n` stays valid for that frame.
// 13-62i: a Takes-mode Sound's active take in Edit (the one its cursor, time selection and keys act on: the last one
// pressed, else the first one on); -1 for any other Sound.
int ActiveTakeOf(const SoundData& d)
{
    if (d.mode != DetectMode::Takes || d.frames.empty()) return -1;
    const int n = static_cast<int>(d.frames.size());
    if (P.zv.cur_tl >= 0 && P.zv.cur_tl < n) return P.zv.cur_tl;
    for (int k = 0; k < n; ++k)
        if (d.frames[static_cast<size_t>(k)].on) return k;
    return 0;
}

// The zone waveform's host of Sound `n`: its zones, its file, its part names and colours (13-62i: an undivided Sound's
// one part is "Sound"). A Takes-mode Sound: take `take` (< 0 = the active one) on its own file and timeline.
ZoneWaveHost ZoneHost(const SoundLibrary& lib, const SoundNode& n, const SoundMedia& m, int take)
{
    ZoneWaveHost h;
    h.view = &P.zv;
    h.lib = &lib;
    h.node = &n;
    const SoundNode* np = &n;
    h.shown = [np]() -> const SoundData& { return Shown(*np); };
    h.path = m.path;
    h.file_len = m.source_len;
    if (n.sound.mode == DetectMode::Takes) {  // 13-62i: each take on its own source
        h.tl = take >= 0 ? take : ActiveTakeOf(Shown(n));
        if (h.tl >= 0) {
            h.path = VariationPath(m, n.sound, h.tl);
            if (static_cast<size_t>(h.tl) < m.take_lens.size() && h.path != m.path) h.file_len = m.take_lens[static_cast<size_t>(h.tl)];
        }
    }
    const SoundStore& store = GetSoundStore(nullptr);
    for (size_t k = 0; k < n.sound.divide_values.size(); ++k) {
        h.names.push_back(ValueName(store, n.sound.divide_group, n.sound.divide_values[k]));
        h.colors.push_back(PartColor(static_cast<int>(k)));
    }
    if (!IsDivided(n.sound)) {  // 13-62i: the implicit part (kWholePart, part 0)
        h.names.push_back("Sound");
        h.colors.push_back(PartColor(0));
    }
    h.cursor = &P.edit_cursor;
    h.part_sel = &P.part_sel;
    h.mute = &P.part_mute;
    h.solo = &P.part_solo;
    h.view_start = &P.view_start;
    h.view_len = &P.view_len;
    h.head_px = LaneHeadPref();
    const std::string lib_uid = lib.uid, uid = n.uid;
    h.live = [np]() -> SoundData& { return Live(*np); };
    h.commit = [lib_uid, uid](const std::string& name, const std::string& toast) { CommitLive(name, lib_uid, uid, toast); };
    h.cancel = [] { CancelLive(); };
    h.live_now = [uid] { return P.phase.Shows(uid); };
    h.draft = [uid]() -> SoundData* { return P.phase.Shows(uid) ? &P.draft : nullptr; };
    h.edit = [lib_uid, uid](const std::string& name, std::function<bool(SoundData&)> f, const std::string& toast) {
        EditNode(name, lib_uid, uid, [f](SoundNode& x) { return f(x.sound); }, toast);
    };
    h.store_replaced = [] { return StoreReplaced(); };
    h.threshold = [np, lib_uid, uid, m](double db, bool done) {
        SoundData& dr = Live(*np);
        dr.silence.threshold_db = db;
        Redetect(dr, m, P.snap.frames);
        if (done) CommitLive("Detection", lib_uid, uid, std::string());
    };
    // 13-62h (v5): the item menu's Normalize (one gain): the Selection panel's rule (NormGroups), faders kept.
    const SoundLibrary* lp = &lib;
    h.normalize = [lp, np, m](const ZoneSel& sel) { ZoneNormalizeGo(*lp, *np, m, NormGroups(np->sound, sel), "Normalize", false); };
    return h;
}

// 13-62h (Lot E1): Edit's waveform of a zoned Sound: the zone rows (DrawZoneWave), the lane headers in its
// column (Lane), the scrollbar.
void DrawZoneSoundWave(const char* id, const SoundLibrary& lib, const SoundNode& n, const SoundMedia& m, float height)
{
    // 13-62i: a Takes-mode Sound draws one zone waveform per take (each on its own file and timeline), stacked.
    const SoundData& d0 = Shown(n);
    const bool       takes = d0.mode == DetectMode::Takes && !d0.frames.empty();
    if (takes) P.zv.cur_tl = ActiveTakeOf(d0);  // 13-62i: the take shown active is the one keys and Settings act on
    const int        rows = takes ? static_cast<int>(d0.frames.size()) : 1;
    const float      total = std::max(16.0f, height - kScrollbarH - 4.0f);
    const float      each = takes ? std::max(ui::Dp(120.0f), std::floor(total / static_cast<float>(rows))) : total;
    double           longest = m.source_len;
    double           scroll = 0.0;
    // 13-62i: takes that do not fit at their least height scroll vertically in their own region (its scrollbar; the
    // wheel stays the view's zoom / scroll, EditNavigate).
    const bool       scrolls = takes && each * static_cast<float>(rows) > total + 0.5f;
    if (scrolls) ImGui::BeginChild("##takerows", ImVec2(0.0f, total), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollWithMouse);
    auto end_rows = [&] {
        if (scrolls) ImGui::EndChild();
    };
    for (int r = 0; r < rows; ++r) {
        ImGui::PushID(r);
        const ZoneWaveHost   zh = ZoneHost(lib, n, m, takes ? r : -1);
        longest = std::max(longest, zh.file_len);
        if (takes) {
            ui::FontScope fs(ui::FontSmall());
            FitText("Take " + std::to_string(r + 1) + (d0.frames[static_cast<size_t>(r)].on ? std::string() : std::string(" (off)")),
                    P.zv.cur_tl == r ? ui::kText : ui::kMuted);
        }
        const ZoneWaveEvents ev = DrawZoneWave(id, zh, takes ? std::max(16.0f, each - ImGui::GetTextLineHeightWithSpacing()) : each);
        if (r == 0 || zh.tl == P.zv.cur_tl) {
            P.wave_x0 = ev.min.x;
            P.wave_x1 = ev.max.x;
        }
        if (StoreReplaced()) {
            ImGui::PopID();
            end_rows();
            return;
        }
        if (ev.lanes > 0) {  // the lane headers (M S, the fader, ...: PartLaneHeader) and the column's splitter
            WaveformEvents wev;
            wev.head_x = ev.head_x;
            wev.head_w = ev.head_w;
            wev.head_narrow = ev.head_narrow;
            wev.lanes_y = ev.lanes_y;
            wev.lane_h = ev.lane_h;
            wev.min = ev.min;
            wev.max = ev.max;
            const SoundData& d = Shown(n);
            LaneHeaders(wev, d, EditParts(GetSoundStore(nullptr), d));
            if (StoreReplaced()) {
                ImGui::PopID();
                end_rows();
                return;
            }
        }
        if (ev.scroll_s != 0.0) scroll = ev.scroll_s;
        ImGui::PopID();
    }
    end_rows();
    P.wave_len = longest;
    if (scroll != 0.0) ScrollView(scroll, longest);
    ViewScrollbar(longest);
}

// The Sound's waveform(s). Edit (`in_edit`): its zones (DrawZoneSoundWave; 13-62i: every Sound). The panel: its
// variations read only (13-62i, P4: no trim, dB drag or split there for any Sound), one lane per take in Takes mode; a
// click plays the variation as Populate places it, a right-click is the caller's.
void DrawSoundWaves(const char* id, const SoundLibrary& lib, const SoundNode& n, const SoundMedia& m, float height,
                    bool in_edit, WaveformEvents* first_ev)
{
    if (in_edit) {
        DrawZoneSoundWave(id, lib, n, m, height);
        return;
    }
    const SoundData&              d = Shown(n);
    const double                  ph = SoundPlayhead(n);
    const std::vector<FrameFades> fades = ShownFades(d);
    auto click = [&](const WaveformEvents& ev, int frame) {
        if (frame >= 0 && static_cast<size_t>(frame) < d.frames.size()) PlayPart(lib, n, m, frame, std::string());
    };
    if (d.mode == DetectMode::Takes && m.take_paths.size() >= 2) {
        const float lane_h = std::max(24.0f, height / static_cast<float>(std::max<size_t>(1, d.frames.size())));
        for (size_t i = 0; i < d.frames.size() && !StoreReplaced(); ++i) {
            const std::string path = i < m.take_paths.size() ? m.take_paths[i] : m.path;
            WaveformOptions   o;
            o.size = ImVec2(0.0f, lane_h - 2.0f);
            o.source_len = i < m.take_lens.size() ? m.take_lens[i] : m.source_len;
            if (i < fades.size()) o.fades = {fades[i]};
            o.mono = d.mono;  // 13-41
            char lid[64];
            std::snprintf(lid, sizeof(lid), "%s_t%d", id, static_cast<int>(i));
            const WaveformEvents ev = DrawWaveform(lid, path, {d.frames[i]}, {}, -1.0, o);
            if (i == 0 && first_ev) *first_ev = ev;
            if (ev.right_clicked && first_ev) first_ev->right_clicked = true;
            if (ev.clicked >= 0) click(ev, static_cast<int>(i));
        }
        return;
    }
    WaveformOptions o;
    o.size = ImVec2(0.0f, height);
    o.source_len = m.source_len;
    o.fades = fades;
    o.mono = d.mono;  // 13-41
    const WaveformEvents ev = DrawWaveform(id, m.path, d.frames, {}, ph, o);
    if (first_ev) *first_ev = ev;
    if (ev.clicked >= 0) click(ev, ev.clicked);
}

// Replace / Set from selected item: the first selected non-folder item (decision 12).
void ReplaceFromSelected(const std::string& lib_uid, const std::string& uid, const char* gesture)
{
    const int  n = CountSelectedMediaItems(nullptr);
    MediaItem* pick = nullptr;
    for (int i = 0; i < n && !pick; ++i) {
        MediaItem* it = GetSelectedMediaItem(nullptr, i);
        if (it && !IsFolderItem(it)) pick = it;
    }
    if (!pick) {
        ShowLibraryToast("Select an item in REAPER first", false);
        return;
    }
    SoundMedia m;
    if (!ReadItemMedia(pick, &m)) {
        ShowLibraryToast("That item has no audio", false);
        return;
    }
    PeakData   pk;
    const bool ready = RequestPeaks(m.path, &pk) == PeakState::Ready;
    const std::string guid = ItemGuidOf(pick);
    const DetectMode  mode = DefaultDetectMode(m.facts, ready);
    std::vector<SoundFrame> frames;
    SilenceParams           sp;
    {
        const SoundStore& store = GetSoundStore(nullptr);
        const SoundLibrary* lib = FindLibrary(store, lib_uid);
        const SoundNode*    node = lib ? FindNode(*lib, uid) : nullptr;
        if (node) sp = node->sound.silence;
    }
    frames = DetectFrames(m.facts, mode, sp, ready ? &pk : nullptr);
    std::string toast = "Replaced audio";
    if (n > 1) toast += " (the first of " + std::to_string(n) + " selected items)";
    const std::string path = m.path;
    const double      item_db = m.facts.item_db;
    const MonoMode    item_mono = MonoForChanMode(m.chan_mode);  // 13-49 review: a take that plays mono
    EditNode(
        gesture, lib_uid, uid,
        [guid, mode, frames, path, item_db, item_mono](SoundNode& x) {
            x.sound.source = SoundSource{};
            x.sound.source.kind = SoundSourceKind::Item;
            x.sound.source.item_guid = guid;
            x.sound.source.item_db = item_db;
            x.sound.mode = mode;
            x.sound.frames = frames;
            x.sound.edited = false;
            if (x.sound.mono == MonoMode::Off) x.sound.mono = item_mono;  // heard as the copy will play it
            RefreshWith(x.sound, path);  // 13-20 / 13-23 / 13-25: the parts on the new audio
            return true;
        },
        toast);
}

void ReplaceFromDrop(const std::string& lib_uid, const std::string& uid, const SoundSource& src,
                     const std::vector<SoundFrame>& dropped)
{
    SoundMedia m;
    std::vector<SoundFrame> frames = dropped;
    DetectMode              mode = DetectMode::Whole;
    const bool              read = ReadSoundMedia(src, &m);
    const std::string       path = read ? m.path : std::string();
    // 13-67c: only variations picked by hand are kept as dropped; a whole file is detected again with the Sound's own
    // Threshold / Min gap (the preview detected it with a new Sound's), never another value.
    bool picked = false;
    for (const SoundFrame& f : dropped) picked = picked || f.hand;
    if (!picked && read) frames.clear();  // 13-67c review: an unreadable file keeps the dropped variations
    if (frames.empty() && read) {
        SilenceParams sp;
        {
            const SoundStore&   store = GetSoundStore(nullptr);
            const SoundLibrary* lib = FindLibrary(store, lib_uid);
            const SoundNode*    node = lib ? FindNode(*lib, uid) : nullptr;
            if (node) sp = node->sound.silence;
        }
        PeakData   pk;
        const bool ready = RequestPeaks(m.path, &pk) == PeakState::Ready;
        mode = DefaultDetectMode(m.facts, ready);
        frames = DetectFrames(m.facts, mode, sp, ready ? &pk : nullptr);
    }
    EditNode(
        "Replace audio", lib_uid, uid,
        [src, mode, frames, path, edited = picked](SoundNode& x) {
            x.sound.source = src;
            x.sound.mode = mode;
            x.sound.frames = frames;
            x.sound.edited = edited;
            RefreshWith(x.sound, path);  // 13-20 / 13-23 / 13-25: the parts on the new audio
            return true;
        },
        "Replaced audio");
}

// What a Media Explorer drop does on a target (decision 6).
enum class MediaDropKind {
    Use,      // the node panel: media_drop (UseMediaAt: a Sound becomes a Random, Random / Blend add)
    Replace,  // a Source waveform: replaces the Sound's audio (decode_media, one undo point)
    Refused,  // a Switch: shown, never taken (the caption says why)
};

// A Media Explorer drop target over a rectangle (LibraryTreeHooks' payload contract, 13-7c): an
// accent frame while a media drag is over it, and the drop handed to media_drop with the node's
// row, or decoded and replacing the Sound's audio. True when a drop was taken (its write runs once
// the panel is drawn).
bool MediaDropTarget(ImGuiID id, ImVec2 a, ImVec2 b, const SoundLibrary& lib, const SoundNode& n, const char* caption,
                     MediaDropKind kind)
{
    const LibraryTreeHooks& h = GetLibraryTreeHooks();
    const ImGuiPayload*     drag = ImGui::GetDragDropPayload();
    if (h.media_payload_type.empty() || !drag || !drag->IsDataType(h.media_payload_type.c_str())) return false;
    if (kind == MediaDropKind::Use && !h.media_drop) return false;
    if (kind == MediaDropKind::Replace && !P.hooks.decode_media) return false;
    const bool refused = kind == MediaDropKind::Refused;
    bool dropped = false;
    // While a media drag runs, every target that takes it shows a thin frame.
    if (!refused) ImGui::GetWindowDrawList()->AddRect(a, b, ui::kAccentLine, ui::kRadiusSm);
    if (ImGui::BeginDragDropTargetCustom(ImRect(a, b), id)) {
        // The innermost target under the mouse takes the drag (ImGui keeps the smallest one).
        const ImGuiPayload* pl = ImGui::AcceptDragDropPayload(
            h.media_payload_type.c_str(), ImGuiDragDropFlags_AcceptBeforeDelivery | ImGuiDragDropFlags_AcceptNoDrawDefaultRect);
        if (pl && kind == MediaDropKind::Use && h.media_hover) h.media_hover(lib.uid, RowOfNode(lib, n.uid));  // 13-69
        if (pl) {
            // Drawn on the foreground: the frame shows over the panel's children too.
            ImDrawList* dl = ImGui::GetForegroundDrawList();
            if (refused) {
                ui::EllipsisText(dl, ImVec2(a.x + ui::Dp(6.0f), a.y + ui::Dp(4.0f)), b.x - ui::Dp(6.0f), ui::kMuted, caption);
            } else if (kind == MediaDropKind::Replace) {
                // The mockup's .swave.drop: a 2 px accent outline, accent 12 % over the wave, a
                // centred accent pill "Replace" (semibold 12, white).
                dl->AddRectFilled(a, b, WithAlpha(ui::kAccent, 0x1F), ui::kRadiusSm);
                dl->AddRect(a, b, ui::kAccent, ui::kRadiusSm, 0, 2.0f);
                ImFont*      pf = ui::FontSemibold();
                const ImVec2 ts = ui::TextSizeIn(pf, caption);
                const ImVec2 c((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f);
                const float  pw = std::min(ts.x + ui::Dp(20.0f), b.x - a.x - ui::Dp(4.0f)), ph = ts.y + ui::Dp(4.0f);
                const ImVec2 q0(c.x - pw * 0.5f, c.y - ph * 0.5f), q1(c.x + pw * 0.5f, c.y + ph * 0.5f);
                dl->AddRectFilled(q0, q1, ui::kAccent, ph * 0.5f);
                ui::FontScope f(pf);
                ui::EllipsisText(dl, ImVec2(std::max(q0.x + ui::Dp(10.0f), c.x - ts.x * 0.5f), q0.y + ui::Dp(2.0f)),
                                 q1.x - ui::Dp(8.0f), ui::kWhite, caption);
            } else {
                dl->AddRectFilled(a, b, WithAlpha(ui::kAccent, 0x1E), ui::kRadiusSm);
                dl->AddRect(a, b, ui::kAccent, ui::kRadiusSm, 0, 2.0f);
                ui::EllipsisText(dl, ImVec2(a.x + 6.0f, a.y + 4.0f), b.x - 6.0f, ui::kAccent, caption);
            }
        }
        if (pl && pl->IsDelivery() && !refused) {
            const std::string lib_uid = lib.uid;
            const std::vector<char> data(static_cast<const char*>(pl->Data), static_cast<const char*>(pl->Data) + pl->DataSize);
            if (kind == MediaDropKind::Replace) {
                SoundSource             src;
                std::vector<SoundFrame> frames;
                const auto decode = P.hooks.decode_media;  // the hooks may be replaced by the call
                if (decode(data.data(), static_cast<int>(data.size()), &src, &frames))
                    ReplaceFromDrop(lib_uid, n.uid, src, frames);
            } else {
                const LibraryRow row = RowOfNode(lib, n.uid);
                const auto drop = h.media_drop;  // the hooks may be replaced by the call
                // It may write the store (UseMediaAt): run once the panel is drawn, as a gesture.
                g_picks.Pick([drop, lib_uid, row, data]() { drop(lib_uid, row, data.data(), static_cast<int>(data.size())); });
            }
            dropped = true;
        }
        ImGui::EndDragDropTarget();
    }
    return dropped;
}

std::string FileName(const std::string& path)
{
    const size_t k = path.find_last_of("/\\");
    return k == std::string::npos ? path : path.substr(k + 1);
}

void ShowInReaper(const SoundSource& src)
{
    MediaItem* it = src.item_guid.empty() ? nullptr : FindItemByGuid(src.item_guid);
    if (!it) return;
    SelectOnlyItem(it);
    SetEditCurPos(GetMediaItemInfo_Value(it, "D_POSITION"), true, false);
    UpdateArrange();
}

// ---- 13-20 / 13-23 / 13-25: Edit's DETECTION rows for parts and sync points ---------------------------


// "Front ← toe, toe end · Back ← heel" (the values in text, their roles muted).
std::vector<TextRun> RolesRuns(const SoundStore& store, const SoundData& d, const std::vector<std::vector<std::string>>& by)
{
    std::vector<TextRun> runs;
    for (size_t k = 0; k < d.divide_values.size(); ++k) {
        if (k > 0) runs.push_back({" \xC2\xB7 ", ui::kMuted});
        runs.push_back({ValueName(store, d.divide_group, d.divide_values[k]), ui::kText});
        std::string keys;
        if (k < by.size())
            for (const std::string& r : by[k]) keys += (keys.empty() ? "" : ", ") + RoleKeyLabel(r);
        runs.push_back({" \xE2\x86\x90 " + (keys.empty() ? std::string("\xE2\x80\x94") : keys), ui::kMuted});
    }
    return runs;
}

// A coloured dot on the text line at the cursor (a part's colour), the cursor moved past it.
void PartDot(ImU32 col)
{
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float  h = ImGui::GetFrameHeight(), r = ui::Dp(4.0f);
    ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(p.x + r, p.y + h * 0.5f), r, col);
    ImGui::Dummy(ImVec2(r * 2.0f + ui::Dp(4.0f), h));
    ImGui::SameLine(0.0f, 0.0f);
}

// The Roles... popup (D9): per value, its role keys as chips (on = this value's; "→ Back" = another
// value's, moved on click), "More roles", "Use the group's roles". One undo point per change.
void RolesPopup(const SoundStore& store, const SoundLibrary& lib, const SoundNode& n)
{
    if (!ui::BeginPanelPopup("##rolespop", 0, ImVec2(10.0f, 10.0f))) return;
    const SoundData& d = n.sound;
    if (!IsDivided(d)) {
        ImGui::CloseCurrentPopup();
        ui::EndPanelPopup();
        return;
    }
    bool                                        own = false;
    const std::vector<std::vector<std::string>> by = RolesByValue(store, d, &own);
    const std::vector<std::string>&             all = LibraryRoleKeys();
    const size_t                                nb = std::min(LibraryBuiltinRoleCount(), all.size());
    static const char* const                    kCommon[] = {"heel", "toe", "toe_end", "hand", "knee"};
    std::vector<std::string>                    keys;
    if (P.roles_more) keys.assign(all.begin(), all.begin() + static_cast<long>(nb));
    else keys.assign(std::begin(kCommon), std::end(kCommon));
    if (!P.roles_more)  // a role already mapped outside the common list stays visible (and removable)
        for (const std::vector<std::string>& rs : by)
            for (const std::string& r : rs)
                if (std::find(keys.begin(), keys.end(), r) == keys.end() &&
                    std::find(all.begin() + static_cast<long>(nb), all.end(), r) == all.end())
                    keys.push_back(r);
    const std::vector<std::string> custom(all.begin() + static_cast<long>(nb), all.end());
    const float                    wrap = ui::Dp(300.0f);
    ui::MenuCaption(("Roles \xC2\xB7 " + n.name + (own ? " (this sound)" : " (group)")).c_str());
    const std::string lib_uid = lib.uid, uid = n.uid;
    for (size_t k = 0; k < d.divide_values.size(); ++k) {
        ImGui::PushID(static_cast<int>(k));
        if (k > 0) ui::MenuSeparator();
        const std::string v = d.divide_values[k], vname = ValueName(store, d.divide_group, v);
        PartDot(PartColor(static_cast<int>(k)));
        FitText(vname, ui::kText);
        const float x0 = ImGui::GetCursorScreenPos().x;
        auto        chips = [&](const std::vector<std::string>& list) {
            bool first = true;
            for (const std::string& key : list) {
                int owner = -1;
                for (size_t j = 0; j < by.size(); ++j)
                    if (std::find(by[j].begin(), by[j].end(), key) != by[j].end()) owner = static_cast<int>(j);
                const bool        on = owner == static_cast<int>(k);
                const std::string other = owner >= 0 && !on ? ValueName(store, d.divide_group, d.divide_values[static_cast<size_t>(owner)]) : "";
                const std::string label = RoleKeyLabel(key) + (other.empty() ? "" : " \xE2\x86\x92 " + other) + "##" + key;
                const float       cw = ui::TextWidth(label.c_str()) + ui::Dp(14.0f);
                if (!first && ImGui::GetItemRectMax().x + ImGui::GetStyle().ItemSpacing.x + cw <= x0 + wrap) ImGui::SameLine();
                first = false;
                if (ui::ChipButton(label.c_str(), ImVec2(0.0f, 0.0f), on)) {
                    const std::string toast = "This sound: " + RoleKeyLabel(key) + " \xE2\x86\x92 " + (on ? std::string("whole") : vname);
                    Gesture("Roles", [lib_uid, uid, v, key](SoundStore& s) {
                        SoundLibrary* l = FindLibrary(s, lib_uid);
                        SoundNode*    x = l ? FindNode(*l, uid) : nullptr;
                        return x && ToggleSoundRole(s, x->sound, v, key);
                    }, toast);
                }
                if (on) ui::ItemTipText("Click to remove");
                else if (!other.empty()) ui::ItemTip("Used by %s: click to move it to %s", other.c_str(), vname.c_str());
                else ui::ItemTip("Click: events with this role get the %s part", vname.c_str());
            }
        };
        chips(keys);
        if (!custom.empty()) {
            ui::ToneText("Custom", ui::kFaint);
            chips(custom);
        }
        ImGui::PopID();
    }
    ui::MenuSeparator();
    if (ui::MenuEntry(P.roles_more ? "Fewer roles" : "More roles\xE2\x80\xA6 (knee, hip, spine, head\xE2\x80\xA6)")) P.roles_more = !P.roles_more;
    ui::MenuSeparator();
    if (ui::MenuEntry("Use the group's roles", own ? nullptr : "already", false, own))
        EditNode("Roles", lib_uid, uid, [](SoundNode& x) {
            if (x.sound.role_map.empty()) return false;
            x.sound.role_map.clear();
            return true;
        }, "This sound uses the group's roles again");
    ui::ToneText("An event gets the part its role maps to; no role (a combined FS step) or an unmapped role = the whole "
                 "sound. A change here applies to this sound only; the group's roles (Library \xE2\x80\xBA Switch Groups) serve "
                 "every other sound.", ui::kFaint, true, wrap);
    ui::EndPanelPopup();
}

// Edit's DETECTION rows of 13-20 / 13-23 / 13-25: Sync point (Attack / Peak / Slice start), its Offset,
// the hand-moved count, Divide by (a Switch Group), the Roles line + Roles..., the part list. Disabled
// with a tip in Takes mode.
void PartsControls(const SoundLibrary& lib, const SoundNode& n, const SoundMedia& m, float label_col)
{
    const SoundStore& store = GetSoundStore(nullptr);
    const SoundData&  d = Shown(n);
    const std::string path = m.path, lib_uid = lib.uid, uid = n.uid;
    // 13-62h / 13-62i: the Sync point and its Offset are the Settings of rows (SoundZoneRows); every Sound (Takes mode
    // too: its takes are zones) can be divided.
    // Divide by: None (combined) or a Switch Group "Name (V1, V2...)".
    const SwitchGroup* cg = IsDivided(d) ? FindGroup(store, d.divide_group) : nullptr;
    {
        auto glabel = [](const SwitchGroup& g) {
            std::string vals;
            for (const SwitchValue& v : g.values) vals += (vals.empty() ? "" : ", ") + v.name;
            return g.name + " (" + vals + ")";
        };
        const float       w = ui::LabelRow("Divide by", ui::Dp(140.0f), label_col);
        const std::string preview = !IsDivided(d) ? std::string("None (combined)") : cg ? glabel(*cg) : std::string("?");
        const bool        open = ui::BeginComboStyled("##divide", preview.c_str(), w);
        ui::ItemTipText("Divide each variation into parts, one per value of a Switch Group");
        if (open) {
            bool        picked = false;
            std::string gid, gname;
            std::vector<std::string> vals, names;
            if (ImGui::Selectable("None (combined)", !IsDivided(d)) && IsDivided(d)) picked = true;
            for (const SwitchGroup& g : store.groups) {
                if (g.values.empty()) continue;
                if (ImGui::Selectable((glabel(g) + "##" + g.uid).c_str(), IsDivided(d) && d.divide_group == g.uid)) {
                    picked = true;
                    gid = g.uid;
                    gname = g.name;
                    for (const SwitchValue& v : g.values) {
                        vals.push_back(v.uid);
                        names.push_back(v.name);
                    }
                }
            }
            if (store.groups.empty()) ImGui::TextDisabled("No Switch Group yet");
            ui::EndComboStyled();
            if (picked) {
                std::string toast = "Combined: each event gets the whole variation (the zones stay where they are)";
                if (!gid.empty()) {
                    std::string vs;
                    for (const std::string& x : names) vs += (vs.empty() ? "" : " / ") + x;
                    toast = "Divided by " + gname + ": " + vs + " \xC2\xB7 zones proposed";
                }
                P.part_sel.clear();  // 13-62: the parts are other values now
                P.part_mute.clear();
                P.part_solo.clear();
                P.zv.sel.clear();  // 13-62h: other zones now
                P.listen_request = 0;
                EditNode("Divide by", lib_uid, uid, [gid, vals, path](SoundNode& x) {
                    if (!SetDivide(x.sound, gid, vals)) return false;
                    RefreshWith(x.sound, path);
                    return true;
                }, toast);
                return;
            }
        }
    }
    if (DivideStale(store, d))
        RichText({{cg ? cg->name + " changed: choose it again to re-divide" : std::string("Its Switch Group is gone: choose one again to re-divide"),
                   ui::kWarn}});
    if (IsDivided(d)) {
        bool                                        own = false;
        const std::vector<std::vector<std::string>> by = RolesByValue(store, d, &own);
        std::vector<TextRun>                        runs = {{"Roles: ", ui::kMuted}};
        for (const TextRun& r : RolesRuns(store, d, by)) runs.push_back(r);
        runs.push_back({own ? " (this sound)" : " (group)", ui::kFaint});
        RichText(runs);
        if (ui::SolidButton("Roles\xE2\x80\xA6")) ImGui::OpenPopup("##rolespop");
        ui::ItemTipText("Set which role gets which part, for this sound");
        RolesPopup(store, lib, n);
        if (StoreReplaced()) return;
    } else {
        ui::SubText("Combined: every event gets the whole variation. Divide by a Switch Group to give its values their own part.");
    }
}

// The Detection controls (panel Section and Edit's side strip; `label_col`: the .row > .k 80 px /
// Edit's .edrow 70 px). 13-7b (audit C5.6): "Split by" segmented, the Silence parameters as a
// fader with its number field, the saved-with note, Reset to detected + "edited by hand".
void DetectionControls(const SoundLibrary& lib, const SoundNode& n, const SoundMedia& m, float label_col = ui::kLabelCol,
                       bool edit = false)
{
    const SoundData& d = Shown(n);
    const bool       ready = RequestPeaks(m.path, nullptr) == PeakState::Ready;
    std::vector<DetectMode>  modes;
    std::vector<const char*> labels;
    // Whole file is always offered: a Sound in another mode can go back to one variation.
    modes.push_back(DetectMode::Whole);
    labels.push_back("Whole file");
    if (m.facts.takes.size() >= 2) {
        modes.push_back(DetectMode::Takes);
        labels.push_back("Takes");
    }
    if (m.facts.slices.size() >= 2) {
        modes.push_back(DetectMode::TakeMarkers);
        labels.push_back("Take markers");
    }
    if (ready) {
        modes.push_back(DetectMode::Silence);
        labels.push_back("Silence");
    }
    if (!ready && d.mode == DetectMode::Silence && RequestPeaks(m.path, nullptr) == PeakState::Building) {
        // Not "Whole file" while the peaks Silence needs are read.
        ui::LabelRow("Split by", ui::Dp(60.0f), label_col);
        FitText("Reading\xE2\x80\xA6", ui::kFaint);
    } else if (modes.size() == 1 && d.mode == DetectMode::Whole) {
        ui::LabelRow("Split by", ui::Dp(60.0f), label_col);
        FitText("Whole file", ui::kMuted);
    } else {
        int sel = -1;
        for (size_t i = 0; i < modes.size(); ++i)
            if (modes[i] == d.mode) sel = static_cast<int>(i);
        ui::LabelRow("Split by", ui::Dp(160.0f), label_col);
        if (ui::Segmented("##detmode", labels.data(), static_cast<int>(labels.size()), &sel) &&
            sel >= 0) {
            const DetectMode mode = modes[static_cast<size_t>(sel)];
            // A new mode starts from its own detection (hand frames belong to the old one).
            // 13-67b: the mode left is parked with its zones; back to it, they come back (SwitchDetectMode).
            const std::vector<SoundFrame> frames = Detected(m, mode, d.silence);
            const std::string             what = mode == DetectMode::Whole ? std::string("Whole file")
                                                                            : std::string("Split by ") + labels[static_cast<size_t>(sel)];
            std::string toast;
            if (mode != d.mode && d.parked_mode == mode && !d.parked.empty() && d.parked_edited)
                toast = what + ": its zones edited by hand are back";
            else if (mode != d.mode && d.edited)
                toast = what + ": the hand edits of the mode left are kept, switch back to get them";
            // Review: only the last mode left is kept: a parked set edited by hand (another mode) goes now.
            if (mode != d.mode && d.parked_mode != mode && !d.parked.empty() && d.parked_edited) {
                std::string gone = "an earlier mode";
                for (size_t i = 0; i < modes.size(); ++i)
                    if (modes[i] == d.parked_mode) gone = labels[i];
                toast += (toast.empty() ? what + ": the" : ". The") + std::string(" hand edits of ") + gone +
                         " are dropped (only the mode just left is kept)";
            }
            const std::string path = m.path;
            EditNode("Detect", lib.uid, n.uid, [mode, frames, path](SoundNode& x) {
                if (!SwitchDetectMode(x.sound, mode, frames)) return false;  // no empty undo point
                RefreshWith(x.sound, path);
                return true;
            }, toast);
            if (StoreReplaced()) return;
        }
    }
    if (d.mode == DetectMode::Silence) {
        struct F {
            NumRow                  row;
            double SilenceParams::*field;
        };
        static const F kFields[] = {
            // 13-67c: the fader -80 to 0 dB (a -60 dB default no longer sits at its end); the field takes -90.
            {{"##thr", "Threshold", "dB", 0.1, -90.0, 0.0, -80.0f, 0.0f, nullptr}, &SilenceParams::threshold_db},
            {{"##gap", "Min gap", "ms", 1.0, 0.0, 5000.0, 0.0f, 400.0f, nullptr}, &SilenceParams::min_gap_ms},
            {{"##pre", "Pre-roll", "ms", 0.5, 0.0, 1000.0, 0.0f, 50.0f, nullptr}, &SilenceParams::preroll_ms},
            {{"##tail", "Tail", "ms", 1.0, 0.0, 5000.0, 0.0f, 300.0f, nullptr}, &SilenceParams::tail_ms},
        };
        for (const F& f : kFields) {
            // 13-62h: a zoned Sound's Pre-roll / Tail are its Settings of rows (the zones' edges, no re-detection).
            if (UsesZones(d) && (f.field == &SilenceParams::preroll_ms || f.field == &SilenceParams::tail_ms)) continue;
            // 13-62h: the shared row (SettingsNumberRow); its fader or field re-detects live, entered once.
            NumRowLook look;
            look.label_w = label_col;
            look.label_col = ui::kMuted;
            const NumRowOut o = SettingsNumberRow(n.uid + f.row.id, f.row, Shown(n).silence.*(f.field), look);
            if (o.e == ui::DragNumberEvent::Live || o.e == ui::DragNumberEvent::Commit) {
                if (o.fader) {  // its row gone mid-gesture: entered on its own node (SettleLostFaders)
                    P.detf_lib = lib.uid;
                    P.detf_uid = n.uid;
                    P.detf_field = f.field;
                }
                // The draft restarts from the store each frame (Live): this field set again, re-detected.
                SoundData& dr = Live(n);
                dr.silence.*(f.field) = o.v;
                Redetect(dr, m, P.snap.frames);
                if (o.e == ui::DragNumberEvent::Commit) {
                    P.detf_field = nullptr;
                    if (dr.silence.*(f.field) == n.sound.silence.*(f.field)) CancelLive();  // no empty undo point
                    else CommitLive("Detection", lib.uid, n.uid);
                    if (StoreReplaced()) return;
                }
            } else if (o.e == ui::DragNumberEvent::Cancel) {
                P.detf_field = nullptr;
                CancelLive();  // Esc: nothing re-detected or written (no undo point)
            }
        }
        // 13-67b ("Detection has no effect"): a re-detection keeps the variations edited by hand; say so.
        const std::string frozen = HandFramesText(Shown(n).frames);
        if (!frozen.empty()) ui::ToneText(frozen.c_str(), ui::kWarn, true);
    }
    // 13-73: in the Edit window, the Detection card goes on with the zone edges and its foot (EditDetectionCard); Divide
    // by is the Parts card's.
    if (edit) return;
    if (UsesZones(Shown(n))) {  // 13-62h review: where they went
        ui::SubText("Pre-roll, Tail, Snap offset: in the Edit window (Settings of)");
    }
    ui::SubText(VariationCountText(DetectedCount(n.uid, m, Shown(n)), Shown(n).frames).c_str());
    ui::SubText("Saved with the sound in the library (.ravlib).");
    if (ui::SolidButton("Reset to detected")) {
        const std::vector<SoundFrame> det = Detected(m, d.mode, d.silence);
        const std::string             path = m.path;
        const std::string             toast = "Back to detected: zones proposed again";
        EditNode("Reset to detected", lib.uid, n.uid, [det, path](SoundNode& x) {
            ResetToDetected(x.sound, det);
            RefreshWith(x.sound, path);
            return true;
        }, toast);
        if (StoreReplaced()) return;
    }
    ui::ItemTipText("Variations and their zones back to RAV's proposal (edits by hand dropped).");
    if (Shown(n).edited) {
        SameLineIfFits(ui::TextWidth("edited by hand"));
        FitText("edited by hand", ui::kMuted);
    }
}

// ---- 13-28: LEVEL -- normalize modes, scope, the user's presets ---------------------------------------

void LoadNormalizePrefs()
{
    if (P.norm_loaded) return;
    P.norm_loaded = true;
    P.norm_db = std::clamp(static_cast<double>(LoadPrefFloat("lib_normalize_db", -1.0f)), kNormTargetMin, kNormTargetMax);
    P.norm_scope = NormScopeFromIndex(static_cast<int>(std::lround(LoadPrefFloat("lib_normalize_scope", 0.0f))));
    P.norm_mode = NormModeFromIndex(static_cast<int>(std::lround(LoadPrefFloat("lib_normalize_mode", 0.0f))));
}

void SaveNormalizePrefs()
{
    SavePrefFloat("lib_normalize_db", static_cast<float>(P.norm_db));
    SavePrefFloat("lib_normalize_scope", static_cast<float>(static_cast<int>(P.norm_scope)));
    SavePrefFloat("lib_normalize_mode", static_cast<float>(static_cast<int>(P.norm_mode)));
}

std::string ResourceRoot()
{
    const char* p = GetResourcePath();
    return p ? std::string(p) : std::string();
}

void CancelNormJob()
{
    for (auto& kv : P.norm_job.sources) delete kv.second;
    P.norm_job = NormJob{};
}

// 13-62h (Lot E2): a zoned Sound's Normalize written -- each group's gain into its zones, the touched faders back
// to 0 unless one zone was normalized (one undo point, the mockup's toast); "Nothing to normalize" (no undo point)
// when no gain changes.
void FinishZoneNormalize(const SoundLibrary& lib, const SoundNode& n, const std::vector<std::vector<ZoneRef>>& groups,
                         bool reset_faders, const std::vector<double>& adj)
{
    SoundData probe = n.sound;
    ApplyZoneNormalize(probe, groups, adj, reset_faders);
    if (SameZoneEdit(probe, n.sound)) {
        bool silent = false;
        for (double a : adj) silent = silent || !std::isfinite(a) || a > 60.0;
        ShowLibraryToast(silent ? "Nothing to normalize: silent" : "Nothing to normalize", false);
        return;
    }
    int nz = 0, silent = 0;
    for (size_t i = 0; i < groups.size(); ++i) {
        if (i >= adj.size() || !std::isfinite(adj[i]) || adj[i] > 60.0) {
            ++silent;
            continue;
        }
        nz += static_cast<int>(groups[i].size());
    }
    // 13-62h review: only the faders the write really put back to 0 are named (a part with a silent group keeps its).
    int faders = 0;
    for (const std::string& v : n.sound.divide_values)
        if (std::fabs(PartFaderDb(n.sound, v)) >= 0.05 && std::fabs(PartFaderDb(probe, v)) < 0.05) ++faders;
    std::string toast = nz > 0 ? "Normalized " + std::to_string(nz) + (nz == 1 ? " zone" : " zones") + ": each zone's gain written"
                               : std::string("Normalize");
    if (silent > 0) toast += " \xC2\xB7 " + std::to_string(silent) + " silent, skipped";
    if (reset_faders && faders > 0) toast += faders > 1 ? "; the faders are back to 0 dB" : "; the fader is back to 0 dB";
    ZoneSelAfter(n.sound, probe);
    EditNode("Normalize", lib.uid, n.uid, [groups, adj, reset_faders](SoundNode& x) {
        const SoundData before = x.sound;
        ApplyZoneNormalize(x.sound, groups, adj, reset_faders);
        return !SameZoneEdit(before, x.sound);
    }, toast);
}

// The zones' edges and parts alike (a Normalize measuring them is still right).
bool SameZoneSpans(const SoundData& x, const SoundData& y)
{
    if (x.frames.size() != y.frames.size()) return false;
    for (size_t i = 0; i < x.frames.size(); ++i) {
        const std::vector<EditZone>& a = x.frames[i].zones;
        const std::vector<EditZone>& b = y.frames[i].zones;
        if (a.size() != b.size() || x.frames[i].on != y.frames[i].on) return false;  // a variation turned off meanwhile
        for (size_t k = 0; k < a.size(); ++k)
            if (a[k].a != b[k].a || a[k].b != b[k].b || a[k].part != b[k].part) return false;
    }
    return true;
}

// Measures and writes `regions` (each with its mode and target, 13-62): Peak regions at once on the cached
// peaks, the others in the time-sliced job (REAPER's CalculateNormalization). 13-62h: `zone_groups` (non-null) =
// a zoned Sound's Normalize, one region per group (NormGroupRegions), written by FinishZoneNormalize.
void StartNormalizeRegions(const SoundLibrary& lib, const SoundNode& n, const SoundMedia& m, NormScope scope,
                           std::vector<NormRegion> regions, const std::string& label,
                           const std::vector<std::vector<ZoneRef>>* zone_groups = nullptr, bool reset_faders = false)
{
    CancelNormJob();
    const SoundData& d = n.sound;
    if (regions.empty()) {
        ShowLibraryToast("Nothing to normalize", false);
        return;
    }
    std::vector<double>             peak_adj(regions.size(), std::numeric_limits<double>::quiet_NaN());
    std::vector<char>               by_peaks(regions.size(), 0);
    std::map<std::string, PeakData> peaks;
    bool                            measure = false;
    for (size_t i = 0; i < regions.size(); ++i) {
        NormRegion& r = regions[i];
        r.path = VariationPath(m, d, r.frame);
        if (r.mode != NormMode::Peak) {
            measure = true;
            continue;
        }
        auto it = peaks.find(r.path);
        if (it == peaks.end()) {
            PeakData pk;
            if (RequestPeaks(r.path, &pk) != PeakState::Ready) pk = PeakData{};
            it = peaks.emplace(r.path, std::move(pk)).first;
        }
        peak_adj[i] = RegionPeakAdjDb(it->second, r, r.target);
        by_peaks[i] = 1;
    }
    if (!measure) {
        if (zone_groups) FinishZoneNormalize(lib, n, *zone_groups, reset_faders, peak_adj);
        return;
    }
    NormJob& j = P.norm_job;
    j.active = true;
    j.lib = lib.uid;
    j.uid = n.uid;
    j.label = label;
    j.scope = scope;
    j.regions = std::move(regions);
    j.peak_adj = std::move(peak_adj);
    j.by_peaks = std::move(by_peaks);
    j.data0 = d;
    j.zoned = zone_groups != nullptr;
    if (zone_groups) j.groups = *zone_groups;
    j.reset_faders = reset_faders;
    j.span_adj.resize(j.regions.size());
    for (size_t i = 0; i < j.regions.size(); ++i) {
        j.span_adj[i].assign(j.regions[i].spans.size(), std::numeric_limits<double>::quiet_NaN());
        if (!j.by_peaks[i]) j.total_spans += j.regions[i].spans.size();
    }
}

// One frame of the job: spans measured until ~8 ms are spent (at least one), then the write once all are.
void PumpNormJob(const SoundLibrary& lib, const SoundNode& n)
{
    NormJob& j = P.norm_job;
    if (!j.active || j.pump_frame == ImGui::GetFrameCount()) return;
    j.pump_frame = ImGui::GetFrameCount();
    if (j.lib != lib.uid || j.uid != n.uid) {
        CancelNormJob();
        return;
    }
    if (!SameNormMeasured(j.data0, n.sound) || (j.zoned && !SameZoneSpans(j.data0, n.sound))) {
        CancelNormJob();
        ShowLibraryToast("The variations changed while measuring: Normalize again", false);
        return;
    }
    if (P.phase.Shows(n.uid)) return;  // a drag in progress: its commit would write over the result
    using Clock = std::chrono::steady_clock;
    const Clock::time_point t0 = Clock::now();
    bool                    first = true;
    while (j.region < j.regions.size()) {
        const NormRegion& r = j.regions[j.region];
        if (j.by_peaks[j.region] || j.span >= r.spans.size()) {
            ++j.region;
            j.span = 0;
            continue;
        }
        if (!first && Clock::now() - t0 > std::chrono::milliseconds(8)) return;
        first = false;
        auto it = j.sources.find(r.path);
        if (it == j.sources.end()) it = j.sources.emplace(r.path, r.path.empty() ? nullptr : PCM_Source_CreateFromFile(r.path.c_str())).first;
        const PartSpan& sp = r.spans[j.span];
        double          adj = std::numeric_limits<double>::quiet_NaN();
        if (it->second && sp.length > 0.0)
            adj = NormAdjustmentDb(CalculateNormalization(it->second, NormModeReaperIndex(r.mode), r.target, sp.start,
                                                          sp.start + sp.length));
        j.span_adj[j.region][j.span] = adj;
        ++j.span;
        ++j.done_spans;
    }
    std::vector<double> adj(j.regions.size());
    for (size_t i = 0; i < j.regions.size(); ++i) {
        if (j.by_peaks[i]) {
            adj[i] = j.peak_adj[i];
            continue;
        }
        std::vector<double> dur;
        for (const PartSpan& sp : j.regions[i].spans) dur.push_back(sp.length);
        adj[i] = CombineSpanAdjustments(j.regions[i].mode, j.regions[i].target, j.span_adj[i], dur);
    }
    const NormScope                         scope = j.scope;
    const std::string                       label = j.label;
    const std::vector<NormRegion>           regions = j.regions;
    const bool                              zoned = j.zoned, reset = j.reset_faders;
    const std::vector<std::vector<ZoneRef>> groups = j.groups;
    CancelNormJob();  // the sources deleted
    if (zoned) FinishZoneNormalize(lib, n, groups, reset, adj);
}

// Peak reads RAV's cached peaks: true when the Sound's file(s) are read (the waveform first).
bool NormPeaksReady(const SoundData& d, const SoundMedia& m)
{
    if (d.mode != DetectMode::Takes) return RequestPeaks(m.path, nullptr) == PeakState::Ready;
    bool ready = true;
    for (const std::string& p : m.take_paths) ready = ready && RequestPeaks(p, nullptr) == PeakState::Ready;
    return ready;
}

// 13-62h (Lot E2): a zoned Sound's Normalize button: `groups` (NormGroups: zones of one part glued by Gaps measured
// together, one gain each) each to its part's target (its own Normalize mode / target, else the Level settings);
// `reset_faders` = the parts' faders back to 0 (the part / Sound buttons, not one zone). Disabled while it measures
// ("Measuring n / m...") or while Peak waits for the waveform. Nothing to normalize: a toast, no undo point.
// What a zone Normalize button reads: its label, "Measuring n / m…" while the job measures (its width is that).
std::string ZoneNormalizeText(const std::string& label)
{
    if (!P.norm_job.active) return label;
    char buf[64];
    std::snprintf(buf, sizeof(buf), "Measuring %d / %d\xE2\x80\xA6", static_cast<int>(P.norm_job.done_spans),
                  static_cast<int>(P.norm_job.total_spans));
    return buf;
}

void ZoneNormalizeButton(const char* id, const std::string& label, const char* tip, const SoundLibrary& lib, const SoundNode& n,
                         const SoundMedia& m, const std::vector<std::vector<ZoneRef>>& groups, bool reset_faders, bool stretch)
{
    LoadNormalizePrefs();
    const SoundData& d = n.sound;
    const bool       busy = P.norm_job.active;
    bool             need_peaks = false;
    for (const std::vector<ZoneRef>& g : groups)
        if (const EditZone* z = g.empty() ? nullptr : ZoneOf(d, g.front()))
            need_peaks = need_peaks || PartNormMode(d, z->part, P.norm_mode) == NormMode::Peak;
    need_peaks = need_peaks && m.ok && !NormPeaksReady(d, m);
    std::string text = ZoneNormalizeText(label) + std::string("##") + id;
    ImGui::BeginDisabled(busy || need_peaks || !m.ok);
    const bool go = ui::SolidButton(text.c_str(), ImVec2(stretch ? std::max(1.0f, ImGui::GetContentRegionAvail().x) : 0.0f, 0.0f));
    ImGui::EndDisabled();
    if (need_peaks) ui::ItemTipText("Waiting for the waveform");
    else if (!busy && tip) ui::ItemTipText(tip);
    if (!go || StoreReplaced()) return;
    ZoneNormalizeGo(lib, n, m, groups, label, reset_faders);
}

// The Normalize of `groups` itself (the buttons above, the waveform's item menu): a toast instead while it measures,
// waits for the waveform (Peak) or has no file; nothing to normalize: a toast, no undo point.
void ZoneNormalizeGo(const SoundLibrary& lib, const SoundNode& n, const SoundMedia& m, const std::vector<std::vector<ZoneRef>>& groups,
                     const std::string& label, bool reset_faders)
{
    LoadNormalizePrefs();
    const SoundData& d = n.sound;
    bool             need_peaks = false;
    for (const std::vector<ZoneRef>& g : groups)
        if (const EditZone* z = g.empty() ? nullptr : ZoneOf(d, g.front()))
            need_peaks = need_peaks || PartNormMode(d, z->part, P.norm_mode) == NormMode::Peak;
    if (P.norm_job.active) {
        ShowLibraryToast("Normalize is measuring: wait for it", false);
        return;
    }
    if (!m.ok) {
        ShowLibraryToast("Cannot normalize: the sound file is not read", false);
        return;
    }
    if (need_peaks && !NormPeaksReady(d, m)) {
        ShowLibraryToast("Waiting for the waveform", false);
        return;
    }
    if (groups.empty()) {
        ShowLibraryToast("No zone to normalize", false);
        return;
    }
    StartNormalizeRegions(lib, n, m, NormScope::Part, NormGroupRegions(d, groups, P.norm_mode, P.norm_db), label, &groups,
                          reset_faders);
}

// 13-62 (C1) / 13-62h: the Normalize button of a zoned Sound's Settings of. value "" = "Normalize (every zone to its
// part's target)", a value = "Normalize <Part>" (its zones only); each writes the zones' gains and puts the faders
// touched back to 0 (one undo point). `lib` = the library's index in the store (else Edit's library). Nothing (no
// button) when `node_uid` is not the zoned Sound shown in Edit (the measuring job is Edit's).
void PartNormalizeNow(int lib_index, const std::string& node_uid, const std::string& value)
{
    if (!P.edit_open || node_uid != P.edit_uid) return;
    const SoundStore&   store = GetSoundStore(nullptr);
    const SoundLibrary* lib = lib_index >= 0 && static_cast<size_t>(lib_index) < store.libraries.size()
                                  ? &store.libraries[static_cast<size_t>(lib_index)]
                                  : nullptr;
    if (!lib || !FindNode(*lib, node_uid)) lib = FindLibrary(store, P.edit_lib);
    if (!lib || lib->uid != P.edit_lib) return;
    const SoundNode* n = FindNode(*lib, node_uid);
    if (!n || n->type != SoundNodeType::Sound || !UsesZones(n->sound)) return;
    LoadNormalizePrefs();
    PumpNormJob(*lib, *n);  // once per frame
    if (StoreReplaced()) return;
    const SoundMedia& m = MediaOf(*n);
    const std::string name = value.empty() ? std::string() : ValueName(store, n->sound.divide_group, value);
    const std::string label = value.empty() ? std::string(IsDivided(n->sound) ? "Normalize (every zone to its part's target)"
                                                                               : "Normalize (every zone to the target)")
                                            : "Normalize " + name;
    const std::string tip = value.empty() ? std::string("Writes each zone's gain so it reaches its part's target; the faders back to 0 dB")
                                          : std::string("Writes each zone's gain so it reaches its part's target; the fader back to 0 dB");
    ZoneNormalizeButton(("pnorm" + value).c_str(), label, tip.c_str(), *lib, *n, m, NormGroups(n->sound, value), true, true);
    if (StoreReplaced()) return;
    if (P.norm_job.active && P.norm_job.total_spans > 0)  // 13-59: the busy card's bar
        DrawBusyBar(static_cast<float>(P.norm_job.done_spans) / static_cast<float>(P.norm_job.total_spans));
}

// ---- 13-20 / 13-23 / 13-25: Edit's legend and the fade at cuts ------------------------------------------

enum class LegendGlyph { Swatch, Hatch, Flag, Cut, Pin };

// A legend chip (.lg): its glyph (a colour swatch, a hatch, the sync flag, the cut) and its name; `on` =
// shown alone (accent-soft). True when clicked (a clickable chip only).
bool LegendChip(const char* id, const std::string& label, ImU32 col, LegendGlyph glyph, bool on, bool clickable, const char* tip)
{
    const float  gw = ui::Dp(10.0f), pad = ui::Dp(7.0f), gap = ui::Dp(5.0f), h = ui::ControlH();
    const float  w = pad + gw + gap + ImGui::CalcTextSize(label.c_str()).x + pad;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton(id, ImVec2(w, h));
    const bool   hov = clickable && ImGui::IsItemHovered();
    const bool   clicked = clickable && ImGui::IsItemClicked();
    ImDrawList*  dl = ImGui::GetWindowDrawList();
    const ImVec2 b(p.x + w, p.y + h);
    if (on) {
        dl->AddRectFilled(p, b, ui::kAccentSoft, h * 0.5f);
        dl->AddRect(p, b, ui::kAccentLine, h * 0.5f);
    } else if (hov) {
        dl->AddRectFilled(p, b, ui::kHover, h * 0.5f);
    }
    const float  cy = p.y + h * 0.5f, gx = p.x + pad;
    const ImVec2 g0(gx, cy - gw * 0.5f), g1(gx + gw, cy + gw * 0.5f);
    switch (glyph) {
    case LegendGlyph::Swatch: dl->AddRectFilled(g0, g1, col, 2.0f); break;
    case LegendGlyph::Hatch:
        dl->AddRectFilled(g0, g1, WithAlpha(ui::kMuted, 0x30), 2.0f);
        Hatch(dl, g0, g1, ui::kText);
        break;
    case LegendGlyph::Flag:
        dl->AddLine(ImVec2(gx + 1.0f, g0.y), ImVec2(gx + 1.0f, g1.y + 2.0f), col);
        dl->AddRectFilled(ImVec2(gx + 1.0f, g0.y), ImVec2(gx + gw, g0.y + gw * 0.55f), col, 1.0f);
        break;
    case LegendGlyph::Pin:  // 13-62h: an edge dragged by hand (the zones' amber triangle)
        dl->AddTriangleFilled(ImVec2(gx, g0.y + 1.0f), ImVec2(gx + gw, g0.y + 1.0f), ImVec2(gx + gw * 0.5f, g1.y), ui::kWarn);
        break;
    case LegendGlyph::Cut:
        for (float y = g0.y; y < g1.y - 3.0f; y += 3.0f) dl->AddLine(ImVec2(gx + gw * 0.5f, y), ImVec2(gx + gw * 0.5f, y + 1.5f), ui::kText);
        dl->AddRectFilled(ImVec2(gx + gw * 0.5f - 2.5f, g1.y - 4.0f), ImVec2(gx + gw * 0.5f + 2.5f, g1.y + 1.0f), ui::kText, 1.0f);
        break;
    }
    dl->AddText(ImVec2(gx + gw + gap, std::floor(cy - ImGui::GetTextLineHeight() * 0.5f)), hov || on ? ui::kText : ui::kMuted,
                label.c_str());
    if (tip) ui::ItemTipText(tip);
    return clicked;
}

// The legend row above Edit's waveform: once divided, "Lanes [Combined | Lane]" (remembered per Sound,
// one undo point), the values' chips (13-62: click = select that part, as its lane or its chip in the right
// column; no undo point) and "Excluded"; then the
// keys of the two handles, "Snap offset" and "Cut (V1 | V2)".
void EditLegend(const SoundStore& store, const SoundLibrary& lib, const SoundNode& n, const SoundMedia& m)
{
    const SoundData& d = Shown(n);
    bool             any = false;
    auto             next = [&](float w) {
        if (any) SameLineIfFits(w);
        any = true;
    };
    auto chip_w = [](const std::string& label) { return ui::Dp(29.0f) + ImGui::CalcTextSize(label.c_str()).x; };
    // 13-62i: Lane mode and the part chips only where there are parts (an undivided Sound has its one implicit part).
    if (IsDivided(d)) {
        std::vector<std::string> names;
        for (const std::string& v : d.divide_values) names.push_back(ValueName(store, d.divide_group, v));
        static const char* const kLanes[2] = {"Combined", "Lane"};
        FitText("Lanes", ui::kFaint);
        ImGui::SameLine();
        int sel = d.lanes_split ? 1 : 0;
        if (ui::Segmented("##lanes", kLanes, 2, &sel) && (sel == 1) != d.lanes_split) {
            const bool split = sel == 1;
            EditNode("Lanes", lib.uid, n.uid, [split](SoundNode& x) {
                if (x.sound.lanes_split == split) return false;
                x.sound.lanes_split = split;
                return true;
            }, split ? "Lanes: Lane \xC2\xB7 one waveform per value (remembered for this sound)"
                     : "Lanes: Combined \xC2\xB7 one coloured waveform (remembered for this sound)");
            return;
        }
        ui::ItemTipText("Lane mode, remembered for this sound. Combined: one waveform coloured by part (what is placed). Lane: one "
                        "lane per value, as REAPER's fixed lanes with comping.");
        any = true;
        for (size_t k = 0; k < names.size(); ++k) {
            next(chip_w(names[k]));
            const std::string id = "##lg" + std::to_string(k);
            const std::string ztip = names[k] + ": click to pick it; double-click: select all its zones";
            if (LegendChip(id.c_str(), names[k], PartColor(static_cast<int>(k)), LegendGlyph::Swatch, P.part_sel == d.divide_values[k], true,
                           ztip.c_str()))
                P.part_sel = d.divide_values[k];
            // 13-62h: double-click a part's chip (Combined; Lane: its lane header): every zone of it selected.
            if (m.ok && ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                ZoneSelectPart(ZoneHost(lib, n, m), d.divide_values[k]);
        }
    }
    // 13-62h: the zones' legend, then the zoom on a variation (All, V1, V2...; 13-62i: not in Takes mode, each take
    // has its own time).
    next(chip_w("Gap"));
    LegendChip("##lggap", "Gap", 0, LegendGlyph::Hatch, false, false, "A Gap: playback jumps over it and glues its two neighbours");
    next(chip_w("By hand"));
    LegendChip("##lghand", "By hand", 0, LegendGlyph::Pin, false, false, "An edge dragged by hand (double-click: automatic again)");
    if (d.sides) {  // 13-73: L R Side · L set by hand (the letters as the waveform draws them)
        ImFont*      fm = ui::FontMonoTiny();
        const float  lw = ui::TextSizeIn(fm, "L").x + ui::Dp(4.0f), gap = ui::Dp(4.0f);
        const char*  t1 = "Side \xC2\xB7";
        const char*  t2 = "set by hand";
        const float  w = ui::Dp(12.0f) + 3.0f * lw + 4.0f * gap + ImGui::CalcTextSize(t1).x + ImGui::CalcTextSize(t2).x;
        const float  bh = ui::Dp(22.0f);
        next(w);
        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        ImGui::Dummy(ImVec2(w, bh));
        ui::ItemTipText("Each zone's side (Parts > Left / Right). Yellow and underlined: set by hand (right-click > Follow rule: "
                        "back to the Pattern). Dropped when the zone is too narrow: then a colour mark in the variations band.");
        ImDrawList* dl = ImGui::GetWindowDrawList();
        float       x = p0.x + ui::Dp(6.0f);
        const float ty = p0.y + (bh - ImGui::GetFontSize()) * 0.5f, ly = p0.y + (bh - fm->FontSize) * 0.5f;
        auto letter = [&](const char* c, ImU32 col, bool under) {
            dl->AddRectFilled(ImVec2(x, ly - ui::Dp(1.0f)), ImVec2(x + lw, ly + fm->FontSize + ui::Dp(1.0f)), ui::kBg);
            ui::AddTextIn(dl, fm, ImVec2(x + ui::Dp(2.0f), ly), col, c);
            if (under) dl->AddLine(ImVec2(x + ui::Dp(2.0f), ly + fm->FontSize + ui::Dp(0.5f)), ImVec2(x + lw - ui::Dp(2.0f), ly + fm->FontSize + ui::Dp(0.5f)), col);
            x += lw;
        };
        letter("L", kZoneSideLeftInk, false);
        letter("R", ui::kUserInk, false);
        x += gap;
        dl->AddText(ImVec2(x, ty), ui::kMuted, t1);
        x += ImGui::CalcTextSize(t1).x + gap;
        letter("L", ui::kWarn, true);
        x += gap;
        dl->AddText(ImVec2(x, ty), ui::kMuted, t2);
    }
    const double len = m.ok ? m.source_len : 0.0;
    if (len > 0.0 && d.mode != DetectMode::Takes) {
        const float bh = ui::Dp(22.0f);
        next(ui::ButtonWidth("All"));
        const std::vector<ZoneVarBlock> vblocks = ZoneVarBlocks(d);
        // Sound Edit evening (A): the chips show the V filter (Ctrl+A's), not the zoom: a variation gone, All again.
        bool has_focus = false;
        for (const ZoneVarBlock& b : vblocks) has_focus = has_focus || b.frame == P.zv.var_focus;
        if (!has_focus) P.zv.var_focus = -1;
        const bool all = P.zv.var_focus < 0;
        if (ui::ToggleButton("All##zvall", all, ImVec2(0.0f, bh))) {
            P.view_start = 0.0;
            P.view_len = 0.0;
            P.zv.var_focus = -1;  // Sound Edit evening (A): no V filter (Ctrl+A: every zone)
            if (!vblocks.empty()) P.edit_cursor = vblocks.front().a;  // 13-67a: the cursor at the first item's start
        }
        int vi = 0;
        for (const ZoneVarBlock& b : vblocks) {
            ++vi;
            const std::string lbl = "V" + std::to_string(vi) + "##zv" + std::to_string(vi);
            const double      a0 = std::max(0.0, b.a - 0.03), b0 = std::min(len, b.b + 0.03);
            const bool        on = P.zv.var_focus == b.frame;
            next(ui::ButtonWidth(("V" + std::to_string(vi)).c_str()));
            if (ui::ToggleButton(lbl.c_str(), on, ImVec2(0.0f, bh))) {
                P.view_start = a0;
                P.view_len = b0 - a0 >= len ? 0.0 : b0 - a0;
                P.edit_cursor = b.a;  // 13-67a (Antho 2026-10-17): play starts on it right away (view state)
                P.zv.var_focus = b.frame;  // Sound Edit evening (A): the V filter (Ctrl+A: its zones only)
            }
            ui::ItemTip("Zoom on variation %d, the edit cursor at its start (Select all: its zones only)", vi);
        }
    }
    ImGui::Dummy(ImVec2(1.0f, ui::Dp(2.0f)));
}

// ---- 13-29 / 13-41: Edit's Play and Mono ----------------------------------------------------------------

// 13-41: Mono -- Off (as the file) / Left / Right / Sum: heard on Play and on audition, placed as the
// take's channel mode (I_CHANMODE), one lane on the waveform. One undo point per pick.
void MonoControl(const SoundLibrary& lib, const SoundNode& n)
{
    static const char* const kLabels[4] = {"Off", "Left", "Right", "Sum"};
    // 13-73 review (mockup MONO_TIPS): each segment says the REAPER action whose key it follows.
    static const char* const kTips[4] = {"As the file: a stereo file stays stereo. REAPER's \"Set take channel mode to normal\" (40176): its key works here too",
                                         "The left channel alone, on both sides. REAPER's \"Set take channel mode to mono (left)\" (40179): its key works here too",
                                         "The right channel alone, on both sides. REAPER's \"Set take channel mode to mono (right)\" (40180): its key works here too",
                                         "Left + right mixed into one channel. REAPER's \"Set take channel mode to mono (downmix)\" (40178): its key works here too"};
    static const char* const kWords[4] = {"off (as the file)", "left", "right", "sum L+R"};
    ui::LabelRow("Mono", ui::Dp(160.0f), ui::Dp(70.0f));
    int        sel = static_cast<int>(n.sound.mono);
    const bool picked = ui::Segmented("##mono", kLabels, 4, &sel);
    const int  hov = ui::SegmentedHovered();
    if (hov >= 0 && hov < 4) ui::ShowTip(kTips[hov]);
    if (picked && sel >= 0 && sel < 4 && sel != static_cast<int>(n.sound.mono)) {
        const MonoMode mode = static_cast<MonoMode>(sel);
        EditNode("Mono", lib.uid, n.uid, [mode](SoundNode& x) {
            if (x.sound.mono == mode) return false;
            x.sound.mono = mode;
            return true;
        }, std::string("Mono: ") + kWords[sel]);
    }
}

// ---- 13-62h (Lot E2): a zoned Sound's panels -- Settings of, the Selection panel, the header's options ------------
// The mockup maquette-zones-13-62h-v3.html (settingsHTML, selHTML, edgeRow, fadeRow, gainRow, snapRow, heardBlock,
// drawHeard, optsHTML) in Dear ImGui. Every number edit = one undo point (a drag: Live + one CommitLive, none when
// nothing changed); every button = one write probed first (nothing changed: no undo point). View state (the
// selection, Settings of, Mute / Solo, the options) is never saved.

int ZoneMs(double t) { return static_cast<int>(std::lround(t * 1000.0)); }

// Columns side by side (`min_w` each at least, as many as fit, else stacked): each column's content narrowed to it
// (the window's work / content rects, as a card's), so its rows wrap and ellipsize inside it. `cols` returns true
// when the drawing must stop (the store replaced): the layout still closes. Returns that.
bool ZoneColumns(const std::vector<std::function<bool()>>& cols, float min_w)
{
    if (cols.empty()) return false;
    const float  gap = ui::Dp(14.0f), avail = std::max(1.0f, ImGui::GetContentRegionAvail().x);
    const int    per = std::max(1, std::min(static_cast<int>(cols.size()), static_cast<int>((avail + gap) / (min_w + gap))));
    const float  w = std::max(1.0f, (avail - gap * static_cast<float>(per - 1)) / static_cast<float>(per));
    const ImVec2 o = ImGui::GetCursorScreenPos();
    ImGuiWindow* win = ImGui::GetCurrentWindow();
    float        row_y = o.y, bottom = o.y;
    bool         stop = false;
    for (size_t i = 0; i < cols.size() && !stop; ++i) {
        const int col = static_cast<int>(i) % per;
        if (col == 0 && i > 0) row_y = bottom + ImGui::GetStyle().ItemSpacing.y + ui::Dp(4.0f);
        // +8 px: room for the own-row bars left of the labels (PartRowBar).
        const float x = o.x + static_cast<float>(col) * (w + gap) + ui::Dp(8.0f);
        ImGui::SetCursorScreenPos(ImVec2(x, row_y));
        ImGui::BeginGroup();
        const float work = win->WorkRect.Max.x, content = win->ContentRegionRect.Max.x;
        const float right = std::max(x + 1.0f, o.x + static_cast<float>(col) * (w + gap) + w);
        win->WorkRect.Max.x = std::min(work, right);
        win->ContentRegionRect.Max.x = std::min(content, right);
        stop = cols[i]();
        win->WorkRect.Max.x = work;
        win->ContentRegionRect.Max.x = content;
        ImGui::EndGroup();
        bottom = std::max(bottom, ImGui::GetItemRectMax().y);
    }
    ImGui::SetCursorScreenPos(ImVec2(o.x, bottom));
    ImGui::Dummy(ImVec2(1.0f, 1.0f));
    return stop;
}

// A tag pill (.tag) at p, the row `h` high: `fill` with `ink` text. Returns its width.
float ZoneTagPill(ImDrawList* dl, ImVec2 p, float h, const std::string& text, ImU32 fill, ImU32 ink, ImU32 edge = 0)
{
    ImFont*      f = ui::FontSemibold();
    const ImVec2 ts = ui::TextSizeIn(f, text.c_str());
    const float  ph = ts.y + ui::Dp(4.0f), w = ts.x + ui::Dp(14.0f);
    const ImVec2 a(p.x, std::floor(p.y + (h - ph) * 0.5f)), b(p.x + w, a.y + ph);
    dl->AddRectFilled(a, b, fill, ph * 0.5f);
    if (edge) dl->AddRect(a, b, edge, ph * 0.5f);
    ui::AddTextIn(dl, f, ImVec2(a.x + ui::Dp(7.0f), a.y + ui::Dp(2.0f)), ink, text.c_str());
    return w;
}

// A solid button with a part's dot before its label ("● → Heel").
bool DotButton(const char* id, const std::string& label, ImU32 col)
{
    const float  r = ui::Dp(4.0f), pad = ui::Dp(8.0f);
    const float  w = std::min(pad + r * 2.0f + ui::Dp(5.0f) + ImGui::CalcTextSize(label.c_str()).x + pad,
                              std::max(ui::Dp(40.0f), ImGui::GetContentRegionAvail().x));
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const bool   clicked = ui::SolidButton(id, ImVec2(w, 0.0f));
    ImDrawList*  dl = ImGui::GetWindowDrawList();
    const float  h = ImGui::GetItemRectSize().y, cy = p.y + h * 0.5f;
    dl->AddCircleFilled(ImVec2(p.x + pad + r, cy), r, col);
    ui::EllipsisText(dl, ImVec2(p.x + pad + r * 2.0f + ui::Dp(5.0f), std::floor(cy - ImGui::GetTextLineHeight() * 0.5f)),
                     p.x + w - ui::Dp(4.0f), ImGui::IsItemHovered() || !ui::ItemDisabled() ? ui::kText : ui::kFaint, label);
    return clicked;
}

// One write on the stored Sound from the panels: `f(data, ctx, sel)` probed on a copy first (nothing changed: false,
// no undo point), then picked as one undo point; the selection follows the gesture's.
using ZonePanelFn = std::function<bool(SoundData&, const ZoneContext&, ZoneSel*)>;
bool ZoneWriteNow(const SoundLibrary& lib, const SoundNode& n, const SoundMedia& m, const std::string& name, const ZonePanelFn& f,
                  const std::string& toast)
{
    SoundData probe = n.sound;
    ZoneSel   out = P.zv.sel;
    if (!f(probe, ZoneCtxOf(m), &out)) return false;
    P.zv.sel = SelValid(probe, out);
    MarkZoneOwn();
    const std::string path = m.path;
    const double      len = m.source_len;
    // Fix 2026-10-19 (Sound Edit writes slow): the store unchanged since the probe (its generation): the probe is written
    // as is (no second run of the gesture, no copy of the peaks).
    const uint64_t                   gen0 = SoundStoreGeneration();
    const std::shared_ptr<SoundData> done = std::make_shared<SoundData>(std::move(probe));
    EditNode(name, lib.uid, n.uid, [f, path, len, gen0, done](SoundNode& x) {
        if (SoundStoreGeneration() == gen0) {
            x.sound = std::move(*done);
            return true;
        }
        PeakData   pk;
        const bool has = !path.empty() && RequestPeaks(path, &pk) == PeakState::Ready;
        ZoneSel    s;
        return f(x.sound, ZoneContext{has ? &pk : nullptr, len}, &s);
    }, toast);
    return true;
}

// A number row of the Selection panel (the mockup's .prow: the bar, the label, the field and its unit, the badge,
// ↺). What happened to it.
struct ZoneRowLook {
    const char* id = "";
    const char* label = "";
    const char* unit = "ms";
    double      value = 0.0, step = 0.1;
    bool        sign = false;
    ImU32       bar = 0;          // its own value: the bar in that colour
    bool        muted = false;    // a default shown (the mockup's dashed field)
    bool        disabled = false;
    std::string pill;             // the badge after it ("= Sound", "▼ by hand"...)
    ImU32       pill_ink = ui::kFaint;
    std::string pill_tip;
    bool        reset = false;    // ↺ shown
    const char* reset_tip = nullptr;
    const char* tip = nullptr;
};
struct ZoneRowOut {
    ui::DragNumberEvent e = ui::DragNumberEvent::None;
    double              v = 0.0;
    bool                reset = false;
};

ZoneRowOut ZoneNumRow(const ZoneRowLook& k)
{
    ZoneRowOut  o;
    const float fw = ui::Dp(56.0f), nw = ui::NumberFieldWidth(fw, k.unit);
    o.v = k.value;
    ImGui::PushID(k.id);
    PartRowBar(k.bar != 0, k.bar);
    ui::LabelRow(k.label, nw, ui::kLabelCol, ui::kText);
    ImGui::BeginDisabled(k.disabled);
    o.e = ui::DragNumber("##num", &o.v, k.step, 1, k.unit, fw, /*trim=*/true, k.muted ? ui::kMuted : 0, k.sign);
    ImGui::EndDisabled();
    if (k.tip) ui::ItemTipText(k.tip);
    if (!k.pill.empty()) RowPill(k.pill, k.pill_ink, k.pill_tip.empty() ? nullptr : k.pill_tip.c_str());
    o.reset = PartResetButton("##r", k.reset, k.reset_tip);
    ImGui::PopID();
    return o;
}

// A dragged / typed number of the Selection panel on zone `r` (`f` writes it on the data): live while dragged (the
// zone and the selection of the gesture's start), one undo point at its end, none when nothing changed. True when the
// drawing must stop.
using ZoneNumFn = std::function<bool(SoundData&, const ZoneRef&, double, const ZoneContext&, ZoneSel*)>;
bool ZoneNumberEdit(const ZoneRowOut& o, const SoundLibrary& lib, const SoundNode& n, const SoundMedia& m, const ZoneRef& r,
                    const ZoneNumFn& f, const char* name, const std::string& toast)
{
    if (o.e == ui::DragNumberEvent::Live || o.e == ui::DragNumberEvent::Commit) {
        if (!P.zedit_on) {
            P.zedit_on = true;
            P.zedit_ref = r;
            P.zedit_sel = P.zv.sel;
        }
        SoundData& dr = Live(n);
        ZoneSel    s = P.zedit_sel;
        f(dr, P.zedit_ref, o.v, ZoneCtxOf(m), &s);
        P.zv.sel = SelValid(dr, s);
        if (o.e == ui::DragNumberEvent::Commit) {
            P.zedit_on = false;
            if (SameZoneEdit(dr, n.sound)) {
                CancelLive();
            } else {
                MarkZoneOwn();
                CommitLive(name, lib.uid, n.uid, toast);
            }
            return StoreReplaced();
        }
    } else if (o.e == ui::DragNumberEvent::Cancel) {
        if (P.zedit_on) P.zv.sel = P.zedit_sel;
        P.zedit_on = false;
        CancelLive();
    }
    return false;
}

// The part names of a Sound by divide value index (the zone helpers' `names`).
std::vector<std::string> ZonePartNames(const SoundData& d)
{
    std::vector<std::string> out;
    for (const std::string& v : PartKeys(d)) out.push_back(PartName(d, v));  // 13-62i: "Sound" when undivided
    return out;
}

// A zone's variation number among the Variations strip's blocks (1-based; 0 = none).
int ZoneVariationNumber(const SoundData& d, const ZoneRef& r)
{
    int i = 0;
    for (const ZoneVarBlock& b : ZoneVarBlocks(d)) {
        ++i;
        if (b.frame == r.frame) return i;
    }
    return 0;
}

// The mockup's heardBlock + drawHeard: the chain of `r` glued -- Gaps removed, crossfades overlapped -- coloured by
// part through its fades and gains, a dashed line at each join, the playheads while it plays, "n ms heard".
void ZoneHeardBlock(const SoundData& d, const ZoneRef& r, const ZoneWaveHost& zh, const SoundMedia& m)
{
    const std::vector<ZoneRef> c = ChainOf(d, r);
    if (c.empty()) return;
    const EditZone* z0 = ZoneOf(d, c.front());
    const EditZone* z1 = ZoneOf(d, c.back());
    if (!z0 || !z1) return;
    const int                     tl = ZoneTimeline(d, r.frame);  // 13-62i: its take's timeline and file
    const std::string             path = d.mode == DetectMode::Takes ? VariationPath(m, d, r.frame) : m.path;
    const std::vector<HeardPiece> pcs = HeardPieces(d, z0->a, z1->b, tl);
    struct Placed {
        HeardPiece q;
        double     at = 0.0;
    };
    std::vector<Placed> placed;
    double              o = 0.0, heard = 0.0;
    for (const HeardPiece& q : pcs) {
        placed.push_back({q, o});
        o += (q.b - q.a) - q.xo;
        heard += (q.b - q.a) - q.xo;
    }
    double total = 0.0;
    for (const Placed& p : placed) total = std::max(total, p.at + (p.q.b - p.q.a));
    const std::vector<std::string> names = ZonePartNames(d);
    std::string                    cap = "Heard \xC2\xB7 ";
    if (c.size() > 1) {
        std::string glued;
        for (const ZoneRef& q : c) glued += (glued.empty() ? "" : " + ") + ZoneName(d, q, names);
        cap += glued + " glued: " + std::to_string(ZoneMs(heard)) + " ms instead of " + std::to_string(ZoneMs(z1->b - z0->a));
    } else {
        cap += std::to_string(ZoneMs(heard)) + " ms";
    }
    ui::GroupCaption(cap.c_str(), ui::kMuted);
    // 13-62h (Antho): as tall as the Selection panel leaves it (its own child), at least 48 px.
    const float  W = std::max(ui::Dp(40.0f), ImGui::GetContentRegionAvail().x);
    const float  H = std::max(ui::Dp(48.0f), ImGui::GetContentRegionAvail().y - ui::kCardPad - ui::Dp(2.0f));
    const ImVec2 p = ImGui::GetCursorScreenPos();
    // 13-67a (Antho 2026-10-17): a click places the edit cursor where it lands in the file, like the main timeline,
    // for fine control without zooming in (view state). A crossfade's overlap: the earlier piece's time.
    // Antho 2026-10-19: held and dragged, the cursor follows the mouse (the main timeline's scrub: cursor only).
    ImGui::InvisibleButton("##zheard", ImVec2(W, H));
    const bool scrub = ImGui::IsItemActive() && ImGui::IsMouseDown(ImGuiMouseButton_Left);
    if ((ImGui::IsItemClicked(ImGuiMouseButton_Left) || scrub) && total > 0.0) {
        const double at = std::clamp(static_cast<double>(ImGui::GetIO().MousePos.x - p.x) / W, 0.0, 1.0) * total;
        for (const Placed& q : placed) {
            if (at < q.at - 1e-9 || at > q.at + (q.q.b - q.q.a) + 1e-9) continue;
            P.edit_cursor = q.q.a + (at - q.at);
            if (d.mode == DetectMode::Takes) P.zv.cur_tl = tl;  // 13-62i: a Takes-mode Sound's cursor belongs to that take
            break;
        }
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + W, p.y + H), ui::kBg, ui::Dp(ui::kRadiusSm));
    dl->AddRect(p, ImVec2(p.x + W, p.y + H), ui::kStroke, ui::Dp(ui::kRadiusSm));
    if (!(total > 0.0)) return;
    // 13-62h (Antho): each piece drawn as the waveform above draws the file (ReadWaveColumns: one column per pixel
    // from the detail tiles, the samples as a line when zoomed past them), its zone's gain and fades / crossfade on.
    // 13-71: a lane per channel (DrawWaveLanes); their zero lines under the pieces, across the whole strip.
    dl->PushClipRect(p, ImVec2(p.x + W, p.y + H), true);
    dl->ChannelsSplit(2);
    dl->ChannelsSetCurrent(1);
    int lanes = 1;
    for (const Placed& q : placed) {
        const EditZone* z = ZoneOf(d, q.q.z);
        const double    len = q.q.b - q.q.a;
        if (!z || !(len > 0.0)) continue;  // the silence of empty space
        const float xa = p.x + static_cast<float>(q.at / total) * W;
        const float xb = p.x + static_cast<float>((q.at + len) / total) * W;
        const int   cols = std::max(1, static_cast<int>(std::floor(xb - xa)));
        WaveColumns wc;
        if (!ReadWaveColumns(path, q.q.a, len, cols, &wc)) continue;
        const double g = std::pow(10.0, z->db / 20.0);
        const ImU32  col = WithAlpha(PartColor(PartIndex(d, z->part)), 0xCC);
        WaveDrawStyle st;
        st.pad = ui::Dp(3.0f);
        st.split = ui::kStrokeStrong;
        lanes = DrawWaveLanes(dl, wc, d.mono, xa, p.y, p.y + H, st, [&](double pos, double, bool, WaveInk* out) {
            out[0].col = col;
            out[0].g = g * ZoneEnv(d, q.q.z, q.q.a + pos / static_cast<double>(cols) * len);
            return 1;
        });
    }
    dl->ChannelsSetCurrent(0);
    for (int l = 0; l < lanes; ++l) {
        const float y = std::floor(p.y + H * (static_cast<float>(l) + 0.5f) / static_cast<float>(lanes)) + 0.5f;
        dl->AddLine(ImVec2(p.x, y), ImVec2(p.x + W, y), ui::kStroke);
    }
    dl->ChannelsMerge();
    dl->PopClipRect();
    for (size_t k = 1; k < placed.size(); ++k) {  // the joins
        const float x = std::floor(p.x + static_cast<float>(placed[k].at / total) * W) + 0.5f;
        for (float y = p.y; y < p.y + H; y += 4.0f) dl->AddLine(ImVec2(x, y), ImVec2(x, std::min(p.y + H, y + 2.0f)), WithAlpha(ui::kText, 0x80));
    }
    // Antho 2026-10-19: the edit cursor, as the main timeline draws it (accent, 2 px while scrubbed), where it falls in
    // the heard chain (the first piece holding it); a Takes-mode Sound's only on its own take.
    if (P.edit_cursor >= 0.0 && (d.mode != DetectMode::Takes || P.zv.cur_tl == tl))
        for (const Placed& q : placed) {
            if (P.edit_cursor < q.q.a - 1e-9 || P.edit_cursor > q.q.b + 1e-9) continue;
            const float x = std::round(p.x + static_cast<float>((q.at + (P.edit_cursor - q.q.a)) / total) * W) + 0.5f;
            dl->AddLine(ImVec2(x, p.y), ImVec2(x, p.y + H), ui::kAccent, scrub ? 2.0f : 1.0f);
            break;
        }
    if (ZonePlaying(zh))
        for (const HeardHead& h : HeardHeads(P.zv.map, AuditionPosition())) {
            for (const Placed& q : placed) {
                if (h.file_t < q.q.a || h.file_t > q.q.b || !(q.q.z == h.z)) continue;
                const float x = p.x + static_cast<float>((q.at + (h.file_t - q.q.a)) / total) * W;
                dl->AddLine(ImVec2(x, p.y), ImVec2(x, p.y + H), h.incoming ? ui::kWarn : ui::kWhite);
                break;
            }
        }
    const std::string lbl = std::to_string(ZoneMs(total)) + " ms heard";
    ui::AddTextIn(dl, ui::FontMonoTiny(), ImVec2(p.x + ui::Dp(4.0f), p.y + H - ui::FontMonoTiny()->FontSize - ui::Dp(3.0f)), ui::kMuted,
                  lbl.c_str());
}

// 13-62h review: the Selection panel's rows keyed by their zone (its variation, index and stored start), so a value
// typed or dragged never lands on another zone picked meanwhile. The caller pops it.
void ZoneRowsId(const SoundNode& n, const ZoneRef& r, const EditZone* shown)
{
    const EditZone* st = ZoneOf(n.sound, r);  // the stored start: a live drag of the Start row keeps its id
    const double    a = st ? st->a : (shown ? shown->a : 0.0);
    char            id[64];
    std::snprintf(id, sizeof(id), "zrow%d:%d:%lld", r.frame, r.index, static_cast<long long>(std::llround(a * 1e6)));
    ImGui::PushID(id);
}

// The Selection panel UNDER the waveform (the mockup's selHTML): nothing selected (the hint), one zone (its part,
// gain, edges, snap offset, fades and what is heard), a Gap (its crossfade and what is heard), several (Play, Heal,
// Delete, Give them to). 13-62h (Antho): always shown, in its own child at the height the user dragged.
void ZoneSelectionPanel(const SoundLibrary& lib, const SoundNode& n, const SoundMedia& m, const ZoneWaveHost& zh)
{
    const SoundData&               d = Shown(n);
    const ZoneSel                  L = SelSorted(d, SelValid(d, P.zv.sel));
    const std::vector<std::string> names = ZonePartNames(d);
    auto                           nm = [&](const ZoneRef& r) { return ZoneName(d, r, names); };
    // The header: its tag and its line.
    std::string tag = "Selection", line;
    ImU32       fill = ui::kRaised, ink = ui::kMuted, edge = ui::kStrokeStrong;
    const EditZone* z = L.size() == 1 ? ZoneOf(d, L.front()) : nullptr;
    // 13-62h (Antho 2026-10-15, v5): exactly one whole item (zones joined by Gaps) selected: shown as one zone.
    const ZoneSel item = SelItem(d, L);
    double        ia = 0.0, ib = 0.0;
    if (!item.empty() && ItemEdges(d, item, &ia, &ib)) {
        tag = "Zone";
        fill = PartColor(PartIndex(d, ZoneOf(d, item.front())->part));
        ink = IM_COL32(0x10, 0x13, 0x1A, 0xFF);
        edge = 0;
        line = ItemNames(d, item, names) + " " + ItemGlue(d, item) + " \xC2\xB7 variation " +
               std::to_string(ZoneVariationNumber(d, item.front())) + " \xC2\xB7 " + ZoneRangeMs(ia, ib) + " \xC2\xB7 " +
               std::to_string(ZoneMs(ItemHeard(d, item))) + " ms heard";
    } else if (L.size() > 1) {
        tag = std::to_string(L.size()) + " selected";
        fill = ui::kAccentSoft;
        ink = ui::kAccentText;
        edge = ui::kAccentLine;
        for (const ZoneRef& r : L) line += (line.empty() ? "" : " + ") + nm(r);
        line += " \xC2\xB7 " + ZoneRangeMs(ZoneOf(d, L.front())->a, ZoneOf(d, L.back())->b);
    } else if (z && z->IsGap()) {
        const ZoneRef gl = GapLeft(d, L.front()), gr = GapRight(d, L.front());
        tag = "Gap";
        fill = WithAlpha(ui::kMuted, 0x40);
        ink = ui::kText;
        edge = 0;
        line = (ZoneOf(d, gl) ? nm(gl) : std::string("?")) + " \xE2\x86\x92 " + (ZoneOf(d, gr) ? nm(gr) : std::string("?")) + " \xC2\xB7 " +
               ZoneRangeMs(z->a, z->b) + " \xC2\xB7 jumps " + std::to_string(ZoneMs(z->b - z->a)) + " ms";
    } else if (z) {
        tag = "Zone";
        fill = PartColor(PartIndex(d, z->part));
        ink = IM_COL32(0x10, 0x13, 0x1A, 0xFF);
        edge = 0;
        line = nm(L.front()) + " \xC2\xB7 variation " + std::to_string(ZoneVariationNumber(d, L.front())) + " \xC2\xB7 " + ZoneRangeMs(z->a, z->b);
    }
    if (d.sides && !L.empty() && !(z && z->IsGap())) {  // 13-73: the side of what is selected
        const std::vector<int>  fr = SelFrames(L);
        const std::vector<char> sides = FrameSides(d), rule = FrameRuleSides(d);
        int                     nl = 0, nr = 0, hand = 0;
        for (int f : fr) {
            if (f < 0 || static_cast<size_t>(f) >= sides.size()) continue;
            (sides[static_cast<size_t>(f)] == 'L' ? nl : nr)++;
            if (FrameSideByHand(d, rule, f)) ++hand;
        }
        if (nl + nr == 1) line += std::string(" \xC2\xB7 ") + (nl ? "Left" : "Right") + (hand ? " (by hand)" : "");
        else if (nl + nr > 1)
            line += " \xC2\xB7 " + std::to_string(nl) + " L \xC2\xB7 " + std::to_string(nr) + " R" + (hand ? " \xC2\xB7 " + std::to_string(hand) + " by hand" : "");
    }
    if (L.empty()) {
        // 13-62h review: nothing selected = the mockup's .sect.sel.empty -- a dashed card, no fill, its grey tag and
        // the hint on the header line.
        ImDrawList*  dl = ImGui::GetWindowDrawList();
        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        const float  w = std::max(1.0f, ImGui::GetContentRegionAvail().x), pad = ui::kCardPad, hh = ui::SectionHeaderH();
        const ImVec2 p1(p0.x + w, p0.y + hh + pad * 2.0f);
        const float  tw = ZoneTagPill(dl, ImVec2(p0.x + pad, p0.y + pad), hh, tag, ui::kStrokeStrong, ui::kMuted);
        ImGui::Dummy(ImVec2(w, p1.y - p0.y));
        // 13-62h (Antho): just a short line; the gestures in its tooltip.
        ui::EllipsisTextTip(dl, ImVec2(p0.x + pad + tw + ui::Dp(8.0f), std::floor(p0.y + pad + (hh - ImGui::GetTextLineHeight()) * 0.5f)),
                            p1.x - pad, ui::kMuted, "Select a zone");
        ui::DashedRect(dl, p0, p1, ui::kStrokeStrong, ui::kRadiusLg);
        if (ImGui::IsMouseHoveringRect(p0, p1))
            ui::ShowTip("Click a zone or a Gap (Ctrl+click: several). Shift+drag: a new zone \xC2\xB7 Alt+drag: a Gap.");
        ImGui::Dummy(ImVec2(w, std::max(0.0f, ui::kCardGap - ImGui::GetStyle().ItemSpacing.y)));
        return;
    }
    ui::Section       s;
    ui::SectionOpts   so;
    so.foldable = false;
    ui::BeginSection(s, "##zselection", so);
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float hh = s.head_max.y - s.head_min.y;
        const float tw = ZoneTagPill(dl, s.head_min, hh, tag, fill, ink, edge);
        if (!line.empty())
            ui::EllipsisTextTip(dl, ImVec2(s.head_min.x + tw + ui::Dp(8.0f), std::floor(s.head_min.y + (hh - ImGui::GetTextLineHeight()) * 0.5f)),
                                s.head_max.x, ui::kText, line);
    }
    const ZoneContext ctx = ZoneCtxOf(m);
    const std::string zname = z ? nm(L.front()) : std::string();
    auto play_btn = [&](const char* label) {
        if (ui::SolidButton(label)) ZonePlaySelection(zh);
    };
    if (!item.empty()) {
        // 13-62h (v5, the mockup's chainHTML): ▶ Play, Normalize (one gain: NormGroups), Heal into one zone, Delete;
        // "Made of" (one row per member, not clickable: its handles are dragged on the waveform), "Give it to", Heard.
        const std::string key = ShortcutKeyLabel(kShortcutEditHeal)[0] ? ShortcutKeyLabel(kShortcutEditHeal) : "H";
        const std::string heal = "Heal into one zone (" + key + ")##zheal";
        const std::string glue = ItemGlue(d, item);
        play_btn("\xE2\x96\xB6 Play##zsplay");
        SameLineIfFits(ui::ButtonWidth(ZoneNormalizeText("Normalize").c_str()));
        ZoneNormalizeButton("znormi", "Normalize", "Measured as one sound: one gain for its zones of each part", lib, n, m,
                            NormGroups(n.sound, L), false, false);
        if (!StoreReplaced()) {
            SameLineIfFits(ui::ButtonWidth(heal.c_str()));
            if (ui::SolidButton(heal.c_str())) ZoneHeal(zh);
            ui::ItemTipText(("Your REAPER key for 40548 (H if none): one zone over it all, its Gap" +
                             std::string(glue == "glued by a Gap" ? "" : "s") + " absorbed (it keeps the first zone's part)")
                                .c_str());
        }
        if (!StoreReplaced()) {
            SameLineIfFits(ui::ButtonWidth("Delete"));
            if (ui::ToneButton("Delete##zsdel", ui::TagTone::Bad)) ZoneDelete(zh);
            ui::ItemTipText("Delete: its zones and Gaps removed, empty space remains");
        }
        if (!StoreReplaced())
            ZoneColumns({[&]() {
                             ui::GroupCaption("Made of", ui::kMuted);
                             // 13-62h (Antho 2026-10-15, v6 rule A): each member's own numbers -- a zone its gain, a Gap
                             // its two crossfade sides and Fill (the zone on its left takes it) -- each live while dragged,
                             // one undo point. A member's line: its glyph (part dot / Gap hatch), its name and range.
                             ImDrawList* dl = ImGui::GetWindowDrawList();
                             const float lh = ImGui::GetTextLineHeight(), rh = std::max(lh + ui::Dp(4.0f), ui::ControlH());
                             for (const ZoneRef& q : item) {
                                 const EditZone* e = ZoneOf(d, q);
                                 if (!e) continue;
                                 ZoneRowsId(n, q, e);
                                 const float  w = std::max(1.0f, ImGui::GetContentRegionAvail().x);
                                 const float  bw = e->IsGap() ? ui::ButtonWidth("Fill") : 0.0f;
                                 const bool   beside = e->IsGap() && w >= bw + ui::Dp(96.0f);  // Fill on its line, else under it
                                 const float  lw = beside ? w - bw - ImGui::GetStyle().ItemSpacing.x : w;
                                 const ImVec2 p = ImGui::GetCursorScreenPos();
                                 ImGui::Dummy(ImVec2(lw, rh));
                                 const float cy = p.y + rh * 0.5f, ty = std::floor(cy - lh * 0.5f);
                                 const float gw = ui::Dp(12.0f), gx = p.x;
                                 if (e->IsGap()) {
                                     const ImVec2 g0(gx, std::floor(cy - ui::Dp(4.5f))), g1(gx + gw, g0.y + ui::Dp(9.0f));
                                     dl->AddRectFilled(g0, g1, WithAlpha(ui::kMuted, 0x30), 2.0f);
                                     Hatch(dl, g0, g1, ui::kText);
                                 } else {
                                     dl->AddCircleFilled(ImVec2(gx + ui::Dp(4.0f), cy), ui::Dp(4.0f), PartColor(PartIndex(d, e->part)));
                                 }
                                 std::string what = nm(q) + " \xC2\xB7 " + ZoneRangeMs(e->a, e->b);
                                 if (e->IsGap()) what += " \xC2\xB7 jumps " + std::to_string(ZoneMs(e->b - e->a)) + " ms";
                                 ui::EllipsisTextTip(dl, ImVec2(gx + gw + ui::Dp(6.0f), ty), p.x + lw, ui::kText, what);
                                 if (e->IsGap()) {
                                     if (beside) ImGui::SameLine();
                                     if (ui::SolidButton("Fill##zifill")) {
                                         const ZoneRef     gl = GapLeft(d, q);
                                         const std::string ln = ZoneOf(d, gl) ? nm(gl) : std::string("the left zone");
                                         const ZoneRef     gq = q;
                                         ZoneWriteNow(lib, n, m, "Fill Gap",
                                                      [gq](SoundData& x, const ZoneContext& c, ZoneSel* sl) { return FillGap(x, gq, c, sl); },
                                                      "Hole filled: " + ln + " takes the Gap");
                                         ImGui::PopID();
                                         return true;
                                     }
                                     ui::ItemTipText("Fill the hole: the zone on its left takes it");
                                     const ZoneRef     gl = GapLeft(d, q), gr = GapRight(d, q);
                                     const std::string lo = (ZoneOf(d, gl) ? nm(gl) : std::string("Left")) + " out";
                                     const std::string ri = (ZoneOf(d, gr) ? nm(gr) : std::string("Right")) + " in";
                                     for (int side = -1; side <= 1; side += 2) {
                                         ZoneRowLook k;
                                         k.id = side < 0 ? "##zixl" : "##zixr";
                                         k.label = side < 0 ? lo.c_str() : ri.c_str();
                                         k.value = (side < 0 ? e->xl : e->xr) * 1000.0;
                                         k.tip = side < 0 ? "The left zone's side of the crossfade (ms)" : "The right zone's side of the crossfade (ms)";
                                         const ZoneRowOut o = ZoneNumRow(k);
                                         if (ZoneNumberEdit(o, lib, n, m, q,
                                                            [side](SoundData& x, const ZoneRef& g, double v, const ZoneContext& c, ZoneSel* sl) {
                                                                const double sv = std::max(0.0, v) / 1000.0;
                                                                return SetGapXfade(x, g, side < 0 ? sv : -1.0, side < 0 ? -1.0 : sv, c, sl);
                                                            },
                                                            "Gap crossfade", "Gap updated")) {
                                             ImGui::PopID();
                                             return true;
                                         }
                                     }
                                 } else {
                                     const double db = e->db;
                                     const bool   on = std::fabs(db) >= 0.05;
                                     ZoneRowLook  k;
                                     k.id = "##zidb";
                                     k.label = "Gain";
                                     k.unit = "dB";
                                     k.value = std::round(db * 10.0) / 10.0;
                                     k.sign = true;
                                     k.bar = on ? PartColor(PartIndex(d, e->part)) : 0;
                                     k.reset = on;
                                     k.reset_tip = "0 dB again";
                                     k.tip = "This zone's own gain (dragging the item up / down moves all of them)";
                                     const ZoneRowOut  o = ZoneNumRow(k);
                                     const std::string zn = nm(q);
                                     const ZoneRef     zq = q;
                                     if (ZoneNumberEdit(o, lib, n, m, q,
                                                        [](SoundData& x, const ZoneRef& r2, double v, const ZoneContext& c, ZoneSel* sl) {
                                                            return SetZoneDb(x, r2, v, c, sl);
                                                        },
                                                        "Zone gain", zn + " updated")) {
                                         ImGui::PopID();
                                         return true;
                                     }
                                     if (o.reset &&
                                         ZoneWriteNow(lib, n, m, "Zone gain",
                                                      [zq](SoundData& x, const ZoneContext& c, ZoneSel* sl) { return SetZoneDb(x, zq, 0.0, c, sl); },
                                                      zn + " gain: 0 dB") &&
                                         StoreReplaced()) {
                                         ImGui::PopID();
                                         return true;
                                     }
                                 }
                                 ImGui::PopID();
                             }
                             ui::GroupCaption("Give it to", ui::kMuted);
                             for (size_t k = 0; k < d.divide_values.size(); ++k) {
                                 const std::string lbl = "\xE2\x86\x92 " + names[k];
                                 if (k > 0) SameLineIfFits(ui::ButtonWidth(lbl.c_str()) + ui::Dp(17.0f));
                                 ImGui::PushID(static_cast<int>(k));
                                 const bool go = DotButton("##zigive", lbl, PartColor(static_cast<int>(k)));
                                 ImGui::PopID();
                                 if (go) {
                                     ZoneGiveTo(zh, d.divide_values[k]);
                                     return true;
                                 }
                             }
                             return false;
                         },
                         [&]() {
                             ZoneHeardBlock(d, item.front(), zh, m);
                             return false;
                         }},
                        ui::Dp(230.0f));
    } else if (L.size() > 1) {
        int nh = 0;
        for (size_t i = 0; i + 1 < L.size(); ++i) nh += Butted(d, L[i], L[i + 1]) ? 1 : 0;
        const std::string key = ShortcutKeyLabel(kShortcutEditHeal)[0] ? ShortcutKeyLabel(kShortcutEditHeal) : "H";
        const std::string heal = "Heal" + (nh ? " " + std::to_string(nh) + (nh > 1 ? " cuts" : " cut") : std::string()) + " (" + key + ")##zheal";
        play_btn("\xE2\x96\xB6 Play##zsplay");
        SameLineIfFits(ui::ButtonWidth(heal.c_str()));
        ImGui::BeginDisabled(nh == 0);
        if (ui::SolidButton(heal.c_str())) ZoneHeal(zh);
        ImGui::EndDisabled();
        ui::ItemTipText("Your REAPER key for 40548 Item: Heal splits in items (H if none)");
        SameLineIfFits(ui::ButtonWidth("Delete"));
        if (ui::ToneButton("Delete##zsdel", ui::TagTone::Bad)) ZoneDelete(zh);
        ui::ItemTipText("Delete: the zones removed (empty space remains), a Gap given to the zone on its left");
        if (!StoreReplaced())
            ZoneColumns({[&]() {
                             ui::GroupCaption("Give them to", ui::kMuted);
                             for (size_t k = 0; k < d.divide_values.size(); ++k) {
                                 const std::string lbl = "\xE2\x86\x92 " + names[k];
                                 if (k > 0) SameLineIfFits(ui::ButtonWidth(lbl.c_str()) + ui::Dp(17.0f));
                                 ImGui::PushID(static_cast<int>(k));
                                 const bool go = DotButton("##zgive", lbl, PartColor(static_cast<int>(k)));
                                 ImGui::PopID();
                                 if (go) {
                                     ZoneGiveTo(zh, d.divide_values[k]);
                                     return true;
                                 }
                             }
                             return false;
                         },
                         [&]() {
                             ui::SubText(nh ? "Heal: the ones edge to edge become one zone (it keeps the first one's part, like REAPER)."
                                            : "None of them touch: Heal needs zones edge to edge.");
                             return false;
                         }},
                        ui::Dp(220.0f));
    } else if (z && z->IsGap()) {
        const ZoneRef     r = L.front();
        const ZoneRef     gl = GapLeft(d, r), gr = GapRight(d, r);
        const std::string ln = ZoneOf(d, gl) ? nm(gl) : std::string(), rn = ZoneOf(d, gr) ? nm(gr) : std::string();
        play_btn("\xE2\x96\xB6 Hear the jump##zsplay");
        const std::string give = "Give it to " + (ln.empty() ? std::string("the left") : ln) + " (Delete)##zsdel";
        SameLineIfFits(ui::ButtonWidth(give.c_str()));
        if (ui::ToneButton(give.c_str(), ui::TagTone::Bad)) ZoneDelete(zh);
        ui::ItemTipText("Delete");
        const double xl = z->xl, xr = z->xr;
        ZoneRowsId(n, r, z);
        if (!StoreReplaced())
            ZoneColumns({[&]() {
                             ui::GroupCaption("Crossfade", ui::kMuted);
                             const std::string lo = (ln.empty() ? std::string("Left") : ln) + " out", ri = (rn.empty() ? std::string("Right") : rn) + " in";
                             ZoneRowLook k;
                             k.id = "##zxl";
                             k.label = lo.c_str();
                             k.value = xl * 1000.0;
                             k.tip = "The left zone's side of the crossfade (ms)";
                             ZoneRowOut out = ZoneNumRow(k);
                             if (ZoneNumberEdit(out, lib, n, m, r,
                                                [](SoundData& x, const ZoneRef& g, double v, const ZoneContext& c, ZoneSel* sl) {
                                                    return SetGapXfade(x, g, std::max(0.0, v) / 1000.0, -1.0, c, sl);
                                                },
                                                "Gap crossfade", "Gap updated"))
                                 return true;
                             k.id = "##zxr";
                             k.label = ri.c_str();
                             k.value = xr * 1000.0;
                             k.tip = "The right zone's side of the crossfade (ms)";
                             out = ZoneNumRow(k);
                             if (ZoneNumberEdit(out, lib, n, m, r,
                                                [](SoundData& x, const ZoneRef& g, double v, const ZoneContext& c, ZoneSel* sl) {
                                                    return SetGapXfade(x, g, -1.0, std::max(0.0, v) / 1000.0, c, sl);
                                                },
                                                "Gap crossfade", "Gap updated"))
                                 return true;
                             char tot[96];
                             std::snprintf(tot, sizeof(tot), "Total %.1f ms: the two zones overlap that much at Populate.", (xl + xr) * 1000.0);
                             ui::SubText(tot);
                             return false;
                         },
                         [&]() {
                             ZoneHeardBlock(d, r, zh, m);
                             return false;
                         }},
                        ui::Dp(230.0f));
        ImGui::PopID();
    } else if (z) {
        const ZoneRef     r = L.front();
        const std::string part = z->part, pname = zname;
        const ImU32       col = PartColor(PartIndex(d, part));
        play_btn("\xE2\x96\xB6 Play##zsplay");
        SameLineIfFits(ui::ButtonWidth("Split at cursor"));
        if (ui::SolidButton("Split at cursor##zssplit")) ZoneSplit(zh);
        {
            const std::string key = ShortcutKeyLabel(kShortcutEditSplit)[0] ? ShortcutKeyLabel(kShortcutEditSplit) : "B";
            const std::string tip = std::string("Split at the edit cursor (your REAPER key for 40757") +
                                    (ShortcutKeyLabel(kShortcutEditSplit)[0] ? ": " + key : std::string("; B while it has none")) + ")";
            ui::ItemTipText(tip.c_str());
        }
        SameLineIfFits(ui::ButtonWidth(ZoneNormalizeText("Normalize").c_str()));
        {
            const std::string tip = "Writes this zone's gain so it reaches " + pname + "'s target";
            ZoneNormalizeButton("znormz", "Normalize", tip.c_str(), lib, n, m, NormGroups(n.sound, ZoneSel{r}), false, false);
        }
        if (!StoreReplaced()) {
            SameLineIfFits(ui::ButtonWidth("Delete"));
            if (ui::ToneButton("Delete##zsdel", ui::TagTone::Bad)) ZoneDelete(zh);
            ui::ItemTipText("Remove it (Delete): empty space remains");
        }
        ZoneRowsId(n, r, z);
        if (!StoreReplaced())
            ZoneColumns(
                {[&]() {
                     ui::GroupCaption("Zone", ui::kMuted);
                     {  // Part: the zone given to another part (one undo point)
                         const float w = ui::LabelRow("Part", ui::Dp(90.0f), ui::kLabelCol, ui::kText);
                         if (ui::BeginComboStyled("##zpart", pname.c_str(), w)) {
                             for (size_t k = 0; k < d.divide_values.size(); ++k) {
                                 const std::string& v = d.divide_values[k];
                                 if (!ImGui::Selectable((names[k] + "##zp" + std::to_string(k)).c_str(), v == part)) continue;
                                 if (v == part) {
                                     ShowLibraryToast("Already " + names[k], false);
                                     continue;
                                 }
                                 P.part_sel = v;
                                 ZoneWriteNow(lib, n, m, "Zone to part",
                                              [r, v](SoundData& x, const ZoneContext& c, ZoneSel* sl) {
                                                  ZoneSel in{r};
                                                  return GiveZonesPart(x, in, v, c, sl);
                                              },
                                              ZoneRangeMs(z->a, z->b) + ": " + pname + " \xE2\x86\x92 " + names[k]);
                             }
                             ui::EndComboStyled();
                         }
                         if (StoreReplaced()) return true;
                     }
                     {  // Gain
                         const double    db = z->db;
                         const bool      on = std::fabs(db) >= 0.05;
                         const std::string tip = "This zone's gain (drag the zone up / down). The " + pname + " fader moves it with the others";
                         ZoneRowLook     k;
                         k.id = "##zdb";
                         k.label = "Gain";
                         k.unit = "dB";
                         k.value = std::round(db * 10.0) / 10.0;
                         k.sign = true;
                         k.bar = on ? col : 0;
                         k.reset = on;
                         k.reset_tip = "0 dB again";
                         k.tip = tip.c_str();
                         const ZoneRowOut o = ZoneNumRow(k);
                         if (ZoneNumberEdit(o, lib, n, m, r,
                                            [](SoundData& x, const ZoneRef& q, double v, const ZoneContext& c, ZoneSel* sl) {
                                                return SetZoneDb(x, q, v, c, sl);
                                            },
                                            "Zone gain", pname + " updated"))
                             return true;
                         if (o.reset &&
                             ZoneWriteNow(lib, n, m, "Zone gain",
                                          [r](SoundData& x, const ZoneContext& c, ZoneSel* sl) { return SetZoneDb(x, r, 0.0, c, sl); },
                                          pname + " gain: 0 dB") &&
                             StoreReplaced())
                             return true;
                     }
                     for (int side = -1; side <= 1; side += 2) {  // Start / End
                         const bool        hand = side < 0 ? z->a_hand : z->b_hand;
                         const bool        gap = ZoneOf(d, side < 0 ? GapBefore(d, r) : GapAfter(d, r)) != nullptr;
                         const bool        cut = side < 0 ? Butted(d, PrevZone(d, r), r) : Butted(d, r, NextZone(d, r));
                         const char*       what = side < 0 ? "Pre-roll" : "Tail";
                         ZoneRowLook       k;
                         k.id = side < 0 ? "##za" : "##zb";
                         k.label = side < 0 ? "Start" : "End";
                         k.value = (side < 0 ? z->a : z->b) * 1000.0;
                         k.bar = hand && !gap ? ui::kWarn : 0;
                         k.reset = hand && !gap;
                         k.reset_tip = "Automatic again (= double-click the edge)";
                         if (gap) {
                             k.pill = "at the Gap";
                             k.pill_tip = "It touches a Gap: the Gap holds it";
                         } else if (hand) {
                             k.pill = "\xE2\x96\xBC by hand";
                             k.pill_ink = ui::kWarn;
                             k.pill_tip = std::string("Dragged by hand: ") +
                                          (ZoneTailWins() ? std::string("changing ") + what + " will move it again (\xE2\x9A\x99 Options)"
                                                          : std::string("stays when ") + what + " changes (\xE2\x9A\x99 Options)");
                         } else {
                             k.pill = cut ? std::string("at the cut") : std::string("= ") + what;
                             k.pill_tip = "Automatic: follows " + pname + "'s " + what + ", never past a cut";
                         }
                         const ZoneRowOut o = ZoneNumRow(k);
                         // 13-62h review: an edge held by a Gap is not set by number (SetZoneEdge refuses it): say why.
                         if (gap && o.e == ui::DragNumberEvent::Commit) ShowLibraryToast("It touches a Gap: drag the Gap's edge", false);
                         if (ZoneNumberEdit(o, lib, n, m, r,
                                            [side](SoundData& x, const ZoneRef& q, double v, const ZoneContext& c, ZoneSel* sl) {
                                                return SetZoneEdge(x, q, side, v / 1000.0, c, sl);
                                            },
                                            "Zone edge", pname + " updated"))
                             return true;
                         if (o.reset &&
                             ZoneWriteNow(lib, n, m, "Zone edge",
                                          [r, side](SoundData& x, const ZoneContext& c, ZoneSel* sl) { return ResetZoneEdge(x, r, side, c, sl); },
                                          pname + ": " + (side < 0 ? "start" : "end") + " automatic again") &&
                             StoreReplaced())
                             return true;
                     }
                     {  // Snap offset
                         const ZoneSnapInfo sn = ZoneSnap(d, r, ctx.peaks);
                         static const char* const kModes[3] = {"Attack", "Peak", "Start"};
                         const int          mi = std::clamp(static_cast<int>(PartSyncMode(d, part)), 0, 2);
                         ZoneRowLook        k;
                         k.id = "##zsnap";
                         k.label = "Snap offset";
                         k.value = sn.t * 1000.0;
                         k.bar = sn.own ? ui::kWarn : 0;
                         k.reset = sn.own;
                         k.reset_tip = "Automatic again (= double-click its tag)";
                         k.tip = "Where the event lands (REAPER: the item's snap offset)";
                         if (sn.own) {
                             k.pill = "by hand";
                             k.pill_ink = ui::kWarn;
                         } else {
                             k.pill = std::string("= ") + kModes[mi] + " " + RoundedNumberText(PartSyncOffsetMs(d, part), true) + " ms";
                             k.pill_tip = pname + "'s Snap offset + Offset";
                         }
                         const ZoneRowOut o = ZoneNumRow(k);
                         if (ZoneNumberEdit(o, lib, n, m, r,
                                            [](SoundData& x, const ZoneRef& q, double v, const ZoneContext& c, ZoneSel* sl) {
                                                return SetZoneSnap(x, q, v / 1000.0, c, sl);
                                            },
                                            "Zone snap offset", pname + " updated"))
                             return true;
                         if (o.reset &&
                             ZoneWriteNow(lib, n, m, "Zone snap offset",
                                          [r](SoundData& x, const ZoneContext& c, ZoneSel* sl) { return ResetZoneSnap(x, r, c, sl); },
                                          pname + " snap offset: automatic again (its part's Snap offset)") &&
                             StoreReplaced())
                             return true;
                     }
                     return false;
                 },
                 [&]() {
                     ui::GroupCaption("Fades", ui::kMuted);
                     for (int side = -1; side <= 1; side += 2) {
                         const ZoneFadeInfo f = ZoneFade(d, r, side);
                         const bool         own = f.src == FadeSrc::Own;
                         ZoneRowLook        k;
                         k.id = side < 0 ? "##zfin" : "##zfout";
                         k.label = side < 0 ? "Fade in" : "Fade out";
                         k.value = std::round(f.s * 10000.0) / 10.0;
                         k.bar = own ? col : 0;
                         k.muted = !own;
                         k.disabled = f.src == FadeSrc::Gap;
                         k.reset = own;
                         k.reset_tip = "Default again (= double-click the corner)";
                         switch (f.src) {
                         case FadeSrc::Cut: k.pill = "= At cuts"; break;
                         case FadeSrc::Part: k.pill = "= " + pname; break;
                         case FadeSrc::Sound: k.pill = "= Sound"; break;
                         case FadeSrc::Gap: k.pill = "= the Gap's crossfade"; break;
                         case FadeSrc::Own: break;
                         }
                         const ZoneRowOut o = ZoneNumRow(k);
                         if (ZoneNumberEdit(o, lib, n, m, r,
                                            [side](SoundData& x, const ZoneRef& q, double v, const ZoneContext& c, ZoneSel* sl) {
                                                return SetZoneFade(x, q, side, std::max(0.0, v) / 1000.0, c, sl);
                                            },
                                            "Zone fade", pname + " updated"))
                             return true;
                         if (o.reset &&
                             ZoneWriteNow(lib, n, m, "Zone fade",
                                          [r, side](SoundData& x, const ZoneContext& c, ZoneSel* sl) { return ResetZoneFade(x, r, side, c, sl); },
                                          pname + " fade " + (side < 0 ? "in" : "out") + ": its default again") &&
                             StoreReplaced())
                             return true;
                     }
                     return false;
                 },
                 [&]() {
                     ZoneHeardBlock(d, r, zh, m);
                     return false;
                 }},
                ui::Dp(230.0f));
        ImGui::PopID();
    }
    ui::EndSection(s);
}

// ---- Settings of (the right column of a zoned Sound) ----

// A number row of the Sound (Settings of: Sound; the shared row, SettingsNumberRow): live while its fader or field
// is dragged (`apply` on the draft; it returns the hand edges a Pre-roll / Tail change met, else 0), one undo point
// at its end, none when `get` reads the same as stored. True when the drawing must stop.
bool SoundZoneNumberRow(const NumRow& f, const SoundLibrary& lib, const SoundNode& n,
                        const std::function<double(const SoundData&)>& get, const std::function<int(SoundData&, double)>& apply,
                        double mark = -1.0)
{
    NumRowLook look;
    look.label_w = ui::kLabelCol;
    look.mark = mark;  // 13-74
    if (mark >= 0.0) look.mark_tip = TailMarkTip(mark);
    const NumRowOut           o = SettingsNumberRow(n.uid + f.id, f, get(Shown(n)), look);
    const ui::DragNumberEvent e = o.e;
    const double              v = o.v;
    if (e == ui::DragNumberEvent::Live || e == ui::DragNumberEvent::Commit) {
        if (!P.zedit_on) {
            P.zedit_on = true;
            P.zedit_sel = P.zv.sel;
        }
        SoundData& dr = Live(n);
        const int  hand = apply(dr, v);
        if (UsesZones(dr)) P.zv.sel = SelFollow(n.sound, P.zedit_sel, dr);  // 13-62h review: carried over by time and part
        if (e == ui::DragNumberEvent::Commit) {
            P.zedit_on = false;
            if (get(dr) == get(n.sound)) {
                CancelLive();
            } else {
                if (UsesZones(dr)) MarkZoneOwn();
                const std::string toast = std::string(f.label) + ": " + RoundedNumberText(get(dr), false) + " " + f.unit +
                                          PreTailToastExtra(hand, ZoneTailWins());
                CommitLive(f.label, lib.uid, n.uid, toast);
            }
            return StoreReplaced();
        }
    } else if (e == ui::DragNumberEvent::Cancel) {
        if (P.zedit_on) P.zv.sel = P.zedit_sel;
        P.zedit_on = false;
        CancelLive();
    }
    return false;
}

// Settings of: Sound (the mockup's FIELDS, Sound side): Detection (Pre-roll, Tail, Sync point, Offset), Level
// (Normalize mode and Target -- the user's Level settings, C1 -- and "Normalize (every zone to its part's target)"),
// Fades (Fade in, Fade out, At cuts, At Gaps). True when the drawing must stop.
// 13-73: the Sound's zone edges (the Detection card's second half): Pre-roll, Tail, Snap offset (this sound's: overrides
// Options > New sounds), Offset. True when the drawing must stop (the store replaced).
bool SoundEdgeRows(const SoundLibrary& lib, const SoundNode& n, const SoundMedia& m)
{
    const std::string path = m.path;
    const double      len = m.source_len;
    // 13-67b / 13-74: the zones the Tail does not reach (the line under its row) and where the first one starts to be
    // stopped (the tick on its fader).
    const TailCaps caps = TailCappedZones(Shown(n), ZoneContext{EditPeaks(path), len});
    for (int k = 0; k < 2; ++k) {  // Pre-roll / Tail: the zones' automatic edges, then decision 7 (the hand edges)
        const bool      pre = k == 0;
        const PartField f = pre ? PartField::Preroll : PartField::Tail;
        static const NumRow kRows[2] = {
            {"##spre", "Pre-roll", "ms", 0.5, 0.0, 1000.0, 0.0f, 50.0f,
             "Automatic zone starts: this much before the detected onset (never before a cut)"},
            {"##stail", "Tail", "ms", 1.0, 0.0, 5000.0, 0.0f, 300.0f,
             "Automatic zone ends: this much after the sound dies (never past a cut)"}};
        if (SoundZoneNumberRow(kRows[k], lib, n, [pre](const SoundData& x) { return pre ? x.silence.preroll_ms : x.silence.tail_ms; },
                               [pre, f, path, len](SoundData& x, double v) {
                                   (pre ? x.silence.preroll_ms : x.silence.tail_ms) = v;
                                   RefreshWith(x, path);
                                   return ApplyPreTailChange(x, std::string(), ZoneTailWins(), f, ZoneContext{EditPeaks(path), len});
                               },
                               pre ? -1.0 : caps.first_ms))
            return true;
        if (!pre) {  // 13-67b: why a longer Tail stops lengthening some zones
            const std::string capped = TailCapsText(caps);
            if (!capped.empty()) ui::ToneText(capped.c_str(), ui::kMuted, true);
        }
    }
    {
        static const char* const kModes[3] = {"Attack", "Peak", "Start"};
        static const char* const kTips[3] = {"Where the first transient crosses the threshold", "The loudest transient's peak",
                                             "The zone's start, pre-roll included"};
        const SoundData& d = Shown(n);
        ui::LabelRow("Snap offset", ui::Dp(120.0f), ui::kLabelCol, ui::kText);
        int        sel = static_cast<int>(d.sync_mode);
        const bool picked = ui::Segmented("##ssync", kModes, 3, &sel);
        const int  hov = ui::SegmentedHovered();
        if (hov >= 0 && hov < 3) ui::ShowTip(kTips[hov]);
        else ui::ItemTipText("Where each zone's snap offset sits by default: the event lands there");
        if (picked && sel >= 0 && sel < 3 && sel != static_cast<int>(d.sync_mode)) {
            const SyncMode mode = static_cast<SyncMode>(sel);
            const double   off = n.sound.sync_offset_ms;
            {  // 13-62h review: probed on a copy, the selection carried over
                SoundData probe = n.sound;
                if (SetSyncSettings(probe, mode, off)) RefreshWith(probe, path);
                ZoneSelAfter(n.sound, probe);
            }
            EditNode("Snap offset", lib.uid, n.uid, [mode, off, path](SoundNode& x) {
                if (!SetSyncSettings(x.sound, mode, off)) return false;
                RefreshWith(x.sound, path);
                return true;
            }, std::string("Snap offset at ") + kModes[sel]);
            return StoreReplaced();  // the write is picked at the frame's end: the rest is still drawn
        }
    }
    static const NumRow kOff = {"##soff", "Offset", "ms", 0.1, -50.0, 50.0, -50.0f, 50.0f, "Moves the snap offset from where its mode puts it (ms)"};
    if (SoundZoneNumberRow(kOff, lib, n,
                           [](const SoundData& x) { return x.sync_offset_ms; },
                           [path](SoundData& x, double v) {
                               SetSyncSettings(x, x.sync_mode, v);
                               RefreshWith(x, path);
                               return 0;
                           }))
        return true;
    return false;
}

// 13-73: the Level card: Normalize, Target, the implicit part's Gain.
bool SoundLevelRows(const SoundLibrary& lib, const SoundNode& n)
{
    LoadNormalizePrefs();
    {  // C1: the Sound's Normalize mode and target = the user's Level settings (every project; no undo point)
        const float w = ui::LabelRow("Normalize", ui::Dp(110.0f), ui::kLabelCol, ui::kText);
        if (ui::BeginComboStyled("##snmode", NormModeLabel(P.norm_mode), w)) {
            for (int k = 0; k < kNormModeCount; ++k) {
                const NormMode md = static_cast<NormMode>(k);
                if (ImGui::Selectable(NormModeLabel(md), md == P.norm_mode) && md != P.norm_mode) {
                    P.norm_mode = md;
                    SaveNormalizePrefs();
                }
            }
            ui::EndComboStyled();
        }
        ui::ItemTipText("Normalize mode (REAPER's): every part follows it unless it has its own (your setting, every project)");
        // 13-62h: the shared row (its fader shows the value live; the setting is entered once, at the gesture's end).
        const NumRow kTgt = {"##sntgt", "Target", NormModeUnit(P.norm_mode), 0.1, kNormTargetMin, kNormTargetMax, -60.0f, 0.0f,
                             "Normalize target: every part follows it unless it has its own (your setting, every project)"};
        NumRowLook look;
        look.label_w = ui::kLabelCol;
        const NumRowOut o = SettingsNumberRow(n.uid + kTgt.id, kTgt, P.norm_db, look);
        if (o.e == ui::DragNumberEvent::Commit) {
            const double now = std::clamp(std::round(o.v * 10.0) / 10.0, kNormTargetMin, kNormTargetMax);
            if (now != P.norm_db) {
                P.norm_db = now;
                SaveNormalizePrefs();
            }
        }
    }
    if (!IsDivided(Shown(n))) {  // 13-62i: the implicit part's fader (what it has applied to every zone of the Sound)
        static const NumRow kGain = {"##sgain", "Gain", "dB", 0.1, -60.0, 24.0, -24.0f, 12.0f,
                                     "The fader: moves the gain of every zone by the same amount (they keep their differences); "
                                     "shows what it has applied"};
        NumRowLook look;
        look.label_w = ui::kLabelCol;
        look.sign = true;
        const NumRowOut o = SettingsNumberRow(n.uid + kGain.id, kGain, PartFaderDb(Shown(n), kWholePart), look);
        if (PartFaderEdit(o.e, o.v, lib, n, kWholePart, "Sound")) return true;
    }
    PartNormalizeNow(EditLibIndex(), n.uid, std::string());
    return StoreReplaced();
}

// 13-73: the Fades card: Fade in, Fade out, At cuts, At Gaps (none for a folder-sourced Sound: nothing is faded).
bool SoundFadeRows(const SoundLibrary& lib, const SoundNode& n)
{
    if (!FolderSourced(Shown(n))) {
        static const NumRow kIn = {"##sfin", "Fade in", "ms", 0.1, 0.0, 10000.0, 0.0f, 50.0f, "Default fade in of its zones"};
        static const NumRow kOut = {"##sfout", "Fade out", "ms", 0.1, 0.0, 10000.0, 0.0f, 50.0f, "Default fade out of its zones"};
        static const NumRow kCut = {"##scut", "At cuts", "ms", 0.1, 0.0, 50.0, 0.0f, 20.0f,
                                    "Default fade of a zone edge that touches a cut"};
        static const NumRow kGap = {"##sgap", "At Gaps", "ms", 0.1, 0.0, 200.0, 0.0f, 50.0f,
                                    "Crossfade of a new Gap (half on each side)"};
        if (SoundZoneNumberRow(kIn, lib, n,
                               [](const SoundData& x) { return x.fade_in * 1000.0; },
                               [](SoundData& x, double v) {
                                   SetDefaultFades(x, v / 1000.0, x.fade_out);
                                   return 0;
                               }) ||
            SoundZoneNumberRow(kOut, lib, n,
                               [](const SoundData& x) { return x.fade_out * 1000.0; },
                               [](SoundData& x, double v) {
                                   SetDefaultFades(x, x.fade_in, v / 1000.0);
                                   return 0;
                               }) ||
            SoundZoneNumberRow(kCut, lib, n,
                               [](const SoundData& x) { return x.cut_fade * 1000.0; },
                               [](SoundData& x, double v) {
                                   x.cut_fade = v / 1000.0;
                                   return 0;
                               }) ||
            SoundZoneNumberRow(kGap, lib, n,
                               [](const SoundData& x) { return x.gap_xfade * 1000.0; },
                               [](SoundData& x, double v) {
                                   x.gap_xfade = v / 1000.0;
                                   return 0;
                               }))
            return true;
    }
    return false;
}

// ---- 13-73: the Edit window's Sound cards (maquette-sound-edit-reglages-13-67-v2.html) ----

// The Detection card's foot: "N variations · edited by hand" and Reset to detected (one undo point).
void DetectionFoot(const SoundLibrary& lib, const SoundNode& n, const SoundMedia& m)
{
    const SoundData& d = Shown(n);
    {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float  w = ImGui::GetContentRegionAvail().x;
        ImGui::GetWindowDrawList()->AddLine(ImVec2(p.x, p.y + 0.5f), ImVec2(p.x + w, p.y + 0.5f), ui::kStroke);
        ImGui::Dummy(ImVec2(1.0f, ui::Dp(3.0f)));
    }
    const std::string count = std::to_string(d.frames.size()) + (d.frames.size() == 1 ? " variation" : " variations") +
                              (d.edited ? " \xC2\xB7 edited by hand" : "");
    FitText(count.c_str(), ui::kMuted);
    ui::ItemTipText(VariationCountText(DetectedCount(n.uid, m, d), d.frames).c_str());
    SameLineIfFits(ui::ButtonWidth("Reset to detected"));
    if (ui::SolidButton("Reset to detected##edreset")) {
        const std::vector<SoundFrame> det = Detected(m, d.mode, d.silence);
        const std::string             path = m.path;
        EditNode("Reset to detected", lib.uid, n.uid, [det, path](SoundNode& x) {
            ResetToDetected(x.sound, det);
            RefreshWith(x.sound, path);
            return true;
        }, "Back to detected: zones proposed again");
    }
    ui::ItemTipText("Variations and their zones back to RAV's proposal (edits by hand dropped; their sides are kept).");
}

// The Parts card's Left / Right (the rule only; each variation's side is read and changed on the waveform): Off / On,
// Pattern, the count, Swap L/R, Reset hand sides, a warning when a side has no variation. One undo point per gesture.
void SidesControls(const SoundLibrary& lib, const SoundNode& n, float label_col)
{
    const SoundData& d = Shown(n);
    {
        static const char* const kOnOff[2] = {"Off", "On"};
        static const char* const kTips[2] = {"Every event plays any variation",
                                             "Each variation is Left or Right (the Pattern; each side shows on the waveform)"};
        ui::LabelRow("Left / Right", ui::Dp(110.0f), label_col);
        int        sel = d.sides ? 1 : 0;
        const bool picked = ui::Segmented("##sides", kOnOff, 2, &sel);
        const int  hov = ui::SegmentedHovered();
        if (hov >= 0 && hov < 2) ui::ShowTip(kTips[hov]);
        else ui::ItemTipText("Left / Right: a left foot or hand event plays a Left variation, a right one a Right variation");
        if (picked && (sel == 1) != d.sides) {
            const bool        on = sel == 1;
            const std::string pat = SidePatternLabel(d.side_pattern);
            EditNode("Left / Right", lib.uid, n.uid, [on](SoundNode& x) { return SetSidesOn(x.sound, on); },
                     on ? "Left / Right: on \xC2\xB7 " + pat + " \xC2\xB7 each zone shows its side on the waveform"
                        : std::string("Left / Right: off \xC2\xB7 every event plays any variation (the sides are kept)"));
            return;
        }
    }
    if (!d.sides) return;
    {
        static const SidePattern kPats[4] = {SidePattern::AltLR, SidePattern::AltRL, SidePattern::AllLeft, SidePattern::AllRight};
        const float w = ui::LabelRow("Pattern", ui::Dp(140.0f), label_col);
        const bool  open = ui::BeginComboStyled("##sidepat", SidePatternLabel(d.side_pattern), w);
        ui::ItemTipText("How the variations get their side, in time order. Sides set by hand on the waveform (select zones, "
                        "right-click) stay.");
        if (open) {
            int picked = -1;
            for (int k = 0; k < 4; ++k)
                if (ImGui::Selectable(SidePatternLabel(kPats[k]), d.side_pattern == kPats[k])) picked = k;
            ui::EndComboStyled();
            if (picked >= 0) {
                const SidePattern p = kPats[picked];
                SoundData         probe = n.sound;
                SetSidePattern(probe, p);
                const int         hand = CountSides(probe).hand;
                EditNode("Left / Right Pattern", lib.uid, n.uid, [p](SoundNode& x) { return SetSidePattern(x.sound, p); },
                         std::string("Pattern: ") + SidePatternLabel(p) + (hand ? " \xC2\xB7 " + std::to_string(hand) + " set by hand kept" : ""));
                return;
            }
        }
    }
    const SideCounts c = CountSides(d);
    {
        std::vector<TextRun> runs = {{std::to_string(c.left), ui::kText}, {" L \xC2\xB7 ", ui::kMuted}, {std::to_string(c.right), ui::kText},
                                     {" R", ui::kMuted}};
        if (c.hand) runs.push_back({" \xC2\xB7 " + std::to_string(c.hand) + " set by hand", ui::kWarn});
        RichText(runs);
        ui::ItemTipText("Set a side by hand: select zones on the waveform (click, Ctrl+click, right-drag), then right-click > Set "
                        "Left / Set Right / Follow rule");
    }
    if (ui::SolidButton("\xE2\x87\x84 Swap L/R##sideswap")) {
        const bool hand = c.hand > 0;
        SoundData  probe = n.sound;
        SwapSides(probe);
        EditNode("Swap L/R", lib.uid, n.uid, [](SoundNode& x) { return SwapSides(x.sound); },
                 std::string("Left / Right swapped: ") + SidePatternLabel(probe.side_pattern) + (hand ? ", the sides set by hand too" : ""));
        return;
    }
    ui::ItemTipText("Every variation to the other side: the Pattern and the sides set by hand (one undo point)");
    if (c.hand) {
        SameLineIfFits(ui::ButtonWidth("Reset hand sides"));
        if (ui::SolidButton("Reset hand sides##sidereset")) {
            EditNode("Reset hand sides", lib.uid, n.uid, [](SoundNode& x) { return ResetHandSides(x.sound); },
                     std::string("Every variation follows the Pattern again (") + SidePatternLabel(d.side_pattern) + ")");
            return;
        }
        ui::ItemTip("The %d side%s set by hand follow the Pattern again (one undo point)", c.hand, c.hand > 1 ? "s" : "");
    }
    if (!d.frames.empty() && (c.left == 0 || c.right == 0))
        ui::ToneText(c.left ? "No Right variation: right events play every variation" : "No Left variation: left events play every variation",
                     ui::kWarn, true);
}

// The Sound's cards (nothing picked in Settings of): Detection (how zones are found, then their edges), Parts (Divide by,
// Left / Right), Level, Fades, Channels; no tutorial line (Antho 2026-10-19). Divided: the Settings of chips above.
void SoundSettingsCards(const SoundLibrary& lib, const SoundNode& n, const SoundMedia& m, const ZoneWaveHost& zh)
{
    const SoundStore& store = GetSoundStore(nullptr);
    const float       lc = ui::Dp(70.0f);
    {
        const SoundData& d = Shown(n);
        if (IsDivided(d)) {
            std::string pick, select_all;
            FitText("Settings of", ui::kMuted);
            ImGui::SameLine();
            PartChip("##pchip_sound", "Sound", ui::kAccent, PartChipLook{}, true, "The whole Sound: every part follows it unless it has its own");
            for (size_t k = 0; k < d.divide_values.size(); ++k) {
                const std::string& v = d.divide_values[k];
                const std::string  name = PartName(d, v);
                PartChipLook       look;
                look.dot = true;
                look.solo = P.part_solo.count(v) > 0;
                look.mute = P.part_mute.count(v) > 0;
                look.count = OwnSettingCount(d, v);
                ImGui::PushID(static_cast<int>(k));
                ui::SameLineIfFits(PartChipWidth(name, look));
                const std::string tip = name + ": click to pick it; double-click: select all its zones";
                if (PartChip("##pchip", name, PartColor(static_cast<int>(k)), look, false, tip.c_str())) pick = v;
                if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) select_all = v;
                ImGui::PopID();
            }
            ImGui::Dummy(ImVec2(1.0f, ui::Dp(2.0f)));
            if (!pick.empty()) P.part_sel = pick;
            if (!select_all.empty()) ZoneSelectPart(zh, select_all);
        }
    }
    {
        ui::Section     ds;
        ui::SectionOpts o;
        o.suffix = "\xC2\xB7 this sound";
        if (ui::BeginSection(ds, "Detection", o)) {
            DetectionControls(lib, n, m, lc, /*edit=*/true);
            bool stop = StoreReplaced();
            if (!stop) {
                const ImVec2 p = ImGui::GetCursorScreenPos();
                ImGui::GetWindowDrawList()->AddLine(ImVec2(p.x, p.y + ui::Dp(2.0f)), ImVec2(p.x + ImGui::GetContentRegionAvail().x, p.y + ui::Dp(2.0f)),
                                                    ui::kStroke);
                ImGui::Dummy(ImVec2(1.0f, ui::Dp(5.0f)));
                stop = SoundEdgeRows(lib, n, m);
            }
            if (!stop) DetectionFoot(lib, n, m);
        }
        ui::EndSection(ds);
        if (StoreReplaced()) return;
    }
    {
        const SoundData&   d = Shown(n);
        const SwitchGroup* g = IsDivided(d) ? FindGroup(store, d.divide_group) : nullptr;
        std::string        suf = "\xC2\xB7 combined";
        if (g) {
            suf = "\xC2\xB7 ";
            for (size_t k = 0; k < d.divide_values.size(); ++k) suf += (k ? " \xC2\xB7 " : "") + PartName(d, d.divide_values[k]);
        }
        ui::Section     ps;
        ui::SectionOpts o;
        o.suffix = suf.c_str();
        if (ui::BeginSection(ps, "Parts", o)) {
            PartsControls(lib, n, m, lc);
            if (!StoreReplaced()) SidesControls(lib, n, lc);
        }
        ui::EndSection(ps);
        if (StoreReplaced()) return;
    }
    {
        SeedSection("Level", false);
        ui::Section ls;
        if (ui::BeginSection(ls, "Level")) {
            if (SoundLevelRows(lib, n)) {
                ui::EndSection(ls);
                return;
            }
        }
        ui::EndSection(ls);
    }
    if (!FolderSourced(Shown(n))) {
        SeedSection("Fades", false);
        ui::Section fs;
        if (ui::BeginSection(fs, "Fades")) {
            if (SoundFadeRows(lib, n)) {
                ui::EndSection(fs);
                return;
            }
        }
        ui::EndSection(fs);
    }
    {
        ui::Section cs;
        if (ui::BeginSection(cs, "Channels")) {
            MonoControl(lib, n);  // 13-41
        }
        ui::EndSection(cs);
        if (StoreReplaced()) return;
    }
    ui::SubText("Every change is saved at once (Ctrl+Z to undo), with the sound.");
}

// The right column of a zoned Sound (the mockup's settingsHTML): the "Settings of" card -- its chips (the Sound, a
// part each, with its own-settings count; double-click a part's chip: all its zones selected), Mute / Solo of the
// part picked, its rows (PartSettingsRows, or the Sound's) and the foot line -- then, for the Sound, its detection
// (Split by, Threshold, Min gap, Divide by, Roles) and Mono in a card of their own.
void ZoneSettingsColumn(const SoundLibrary& lib, const SoundNode& n, const SoundMedia& m, const ZoneWaveHost& zh)
{
    LoadNormalizePrefs();
    PumpNormJob(lib, n);  // a Normalize of the zones keeps measuring
    if (StoreReplaced()) return;
    {
        const SoundData& d = Shown(n);
        // not one of its values now; 13-62i: an undivided Sound's one part is the Sound itself (never picked)
        if (!P.part_sel.empty() && (PartIndex(d, P.part_sel) < 0 || !IsDivided(d))) P.part_sel.clear();
    }
    if (P.part_sel.empty()) {  // 13-73: the Sound's cards
        SoundSettingsCards(lib, n, m, zh);
        return;
    }
    const std::string part = P.part_sel;
    const SoundData&  d = Shown(n);
    const int         own = part.empty() ? 0 : OwnSettingCount(d, part);
    const float       sp = ImGui::GetStyle().ItemSpacing.x;
    const float       mw = ui::ButtonWidth("Mute"), sw = ui::ButtonWidth("Solo"), aw = ui::ButtonWidth("\xE2\x86\xBA All");
    ui::Section       s;
    ui::SectionOpts   so;
    so.foldable = false;
    so.right_w = part.empty() ? 0.0f : mw + sp + sw + (own > 0 ? sp + aw : 0.0f);
    ui::BeginSection(s, "Settings of", so);
    bool mute = false, solo = false, all = false;
    if (!part.empty()) {
        ui::BeginSectionHeaderRight(s);
        if (own > 0) {
            all = ui::SolidButton("\xE2\x86\xBA All##pall");
            ui::ItemTipText("Every setting same as the Sound again (one undo point)");
            ImGui::SameLine();
        }
        mute = ListenButton("##pmute", "Mute", false, P.part_mute.count(part) > 0, ImVec2(mw, 0.0f));
        ImGui::SameLine();
        solo = ListenButton("##psolo", "Solo", true, P.part_solo.count(part) > 0, ImVec2(sw, 0.0f));
        ui::EndSectionHeaderRight(s);
    }
    std::string pick, select_all;
    bool        pick_sound = false;
    // 13-62i: an undivided Sound shows no part picker (its one part is the Sound: its rows below).
    if (IsDivided(d) && PartChip("##pchip_sound", "Sound", ui::kAccent, PartChipLook{}, part.empty(),
                                 "The whole Sound: every part follows it unless it has its own"))
        pick_sound = true;
    for (size_t k = 0; k < d.divide_values.size(); ++k) {
        const std::string& v = d.divide_values[k];
        const std::string  name = PartName(d, v);
        PartChipLook       look;
        look.dot = true;
        look.solo = P.part_solo.count(v) > 0;
        look.mute = P.part_mute.count(v) > 0;
        look.count = OwnSettingCount(d, v);
        ImGui::PushID(static_cast<int>(k));
        ui::SameLineIfFits(PartChipWidth(name, look));
        const std::string tip = name + ": click to pick it; double-click: select all its zones";
        if (PartChip("##pchip", name, PartColor(static_cast<int>(k)), look, part == v, tip.c_str())) pick = v;
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) select_all = v;
        ImGui::PopID();
    }
    if (IsDivided(d)) ImGui::Dummy(ImVec2(1.0f, ui::Dp(2.0f)));
    bool stop = false;
    if (!part.empty()) {
        ImGui::Indent(ui::Dp(8.0f));  // room for the own bars left of the labels
        PartSettingsRows("sidepart", 0, n.uid, part);
        ImGui::Unindent(ui::Dp(8.0f));
        stop = StoreReplaced();
    } else {
        stop = SoundEdgeRows(lib, n, m) || SoundLevelRows(lib, n) || SoundFadeRows(lib, n);  // (a part is always picked here)
    }
    if (!stop) {
        ImGui::Dummy(ImVec2(1.0f, ui::Dp(2.0f)));
        ui::SubText(!IsDivided(d)    ? "A zone's own fade wins over the Sound's."
                    : part.empty() ? "Every part follows the Sound unless it has its own. A zone's own fade wins over both."
                                   : "At cuts and At Gaps: for the whole Sound. A zone's own fade wins over both.");
    }
    ui::EndSection(s);
    if (stop || StoreReplaced()) return;
    if (pick_sound) P.part_sel.clear();
    if (!pick.empty()) P.part_sel = pick;
    if (!select_all.empty()) ZoneSelectPart(zh, select_all);
    if (mute) ToggleListen(false, d, part);
    if (solo) ToggleListen(true, d, part);
    if (all) {
        // 13-62h review: the settings back to the Sound's (the fader stays); a Pre-roll / Tail the part owned moves its
        // hand edges again per the option (decision 7), in the same write.
        const std::string   path = m.path, name = PartName(d, part), value = part;
        const double        len = m.source_len;
        const bool          wins = ZoneTailWins();
        const PartSettings* ps = PartSettingsOf(n.sound, value);
        const bool          pre = ps && ps->preroll_ms.has_value(), tail = ps && ps->tail_ms.has_value();
        auto                write = [value, path, pre, tail, wins](SoundData& x, const ZoneContext& c) {
            ResetPartAll(x, value);
            RefreshWith(x, path);
            int hand = 0;
            if (pre) hand += ApplyPreTailChange(x, value, wins, PartField::Preroll, c);
            if (tail) hand += ApplyPreTailChange(x, value, wins, PartField::Tail, c);
            return hand;
        };
        SoundData probe = n.sound;
        const int hand = write(probe, ZoneCtxOf(m));
        ZoneSelAfter(n.sound, probe);
        EditNode("Part settings", lib.uid, n.uid, [value, path, len, write](SoundNode& x) {
            if (OwnSettingCount(x.sound, value) == 0) return false;
            PeakData   pk;
            const bool has = !path.empty() && RequestPeaks(path, &pk) == PeakState::Ready;
            write(x.sound, ZoneContext{has ? &pk : nullptr, len});
            return true;
        }, name + ": every setting same as the Sound again" + PreTailToastExtra(hand, wins));
    }
    ui::SubText("Every change is saved at once (Ctrl+Z to undo), with the sound.");
}

// ---- the header of a zoned Sound's Edit window ----

constexpr const char kZoneHelp[] =
    "Top half of a zone: drag moves it. Bottom half: drag moves the cursor. Shift+drag: a new zone. Alt+drag: a Gap. Right-drag on "
    "empty space: select the zones it touches (Ctrl adds). Every change is one undo point (Ctrl+Z).";

// How many Sound Edit options are off their default (the ⚙ badge).
int ZoneOptionsChanged()
{
    return (ZoneClickPlays() ? 0 : 1) + (ZoneTailWins() ? 0 : 1) + (ZoneNewSnapMode() == SyncMode::Peak ? 0 : 1) +
           (ZoneNewSides() ? 1 : 0);  // 13-73
}

// The header's "Click plays" toggle (the speaker icon, on = accent): a pref of the user, no undo point.
float ZoneClickPlaysWidth() { return ui::ButtonWidth("Click plays") + ui::IconRoom(); }
void  ZoneClickPlaysButton(float h)
{
    const bool   on = ZoneClickPlays();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float  w = ZoneClickPlaysWidth();
    if (ui::ToggleButton("##zclickplays", on, ImVec2(w, h))) {
        SetZoneClickPlays(!on);
        ShowLibraryToast(!on ? "Click plays: on" : "Click plays: off. A click only selects", false);
    }
    ui::ItemTipText("On: a click on a zone or a Gap plays it (the time selection goes). Off: a click only selects.");
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float is = ui::IconPx(), x = p.x + ImGui::GetStyle().FramePadding.x;
    const ImU32 ink = on ? ui::kAccentText : ui::kText;
    DrawLibraryGlyph(dl, ImVec2(x, std::floor(p.y + (h - is) * 0.5f)), is, kGlyphSpeaker, ink);
    dl->AddText(ImVec2(x + ui::IconRoom(), std::floor(p.y + (h - ImGui::GetTextLineHeight()) * 0.5f)), ink, "Click plays");
}

// The Sound Edit options themselves: group Editing (Click plays, Pre-roll / Tail overwrite edges dragged by hand), group
// New sounds (13-67b: the snap offset mode, 13-73: Left / Right). The one list both the Edit window's ⚙ Options popup
// and the main menu (Library tab > Sound Edit) draw: an option added here shows in both. User prefs, no undo point.
void SoundEditOptionItems()
{
    struct Opt {
        const char* id;
        const char* t;
        const char* d;
        bool        on, live;
    };
    const Opt opts[] = {
        {"##oclick", "Click plays", "A click on a zone or a Gap plays it. Also in the header.", ZoneClickPlays(), true},
        {"##otail", "Pre-roll / Tail overwrite edges dragged by hand",
         "On: changing Pre-roll or Tail moves every edge of that part, even ones you dragged (\xE2\x96\xBC). Off: dragged edges stay; "
         "double-click one to follow again.",
         ZoneTailWins(), true},
    };
    for (int i = 0; i < 2; ++i) {  // 13-67b: the "Later" examples are gone (a later story makes them real)
        const Opt& o = opts[i];
        if (i == 0) ui::GroupCaption("Editing", ui::kMuted);
        ImGui::PushID(o.id);
        ImGui::BeginDisabled(!o.live);
        ImGui::BeginGroup();
        const bool flip = ui::Switch("##sw", o.on);
        const bool on_switch = ImGui::IsItemHovered();
        ImGui::SameLine();
        ImGui::BeginGroup();
        const float tw = std::max(ui::Dp(60.0f), ImGui::GetContentRegionAvail().x);
        ui::ToneText(o.t, o.live ? ui::kText : ui::kFaint, true, tw);
        ui::ToneText(o.d, o.live ? ui::kMuted : ui::kFaint, true, tw);
        ImGui::EndGroup();
        ImGui::EndGroup();
        const bool row_click = o.live && !on_switch && ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left);
        ImGui::EndDisabled();
        ImGui::PopID();
        if (o.live && (flip || row_click)) {
            const bool now = !o.on;
            if (i == 0) SetZoneClickPlays(now);
            else SetZoneTailWins(now);
            ShowLibraryToast(std::string(o.t) + ": " + (now ? "on" : "off") + " (yours, every project)", false);
        }
        ImGui::Dummy(ImVec2(1.0f, ui::Dp(2.0f)));
    }
    {  // 13-67b: where a new Sound's snap offset sits (Antho 2026-10-17: default Peak); saved Sounds keep theirs
        ImGui::Dummy(ImVec2(1.0f, ui::Dp(4.0f)));
        ui::GroupCaption("New sounds", ui::kMuted);
        static const char* const kModes[3] = {"Attack", "Peak", "Start"};
        static const char* const kTips[3] = {"Where the first transient crosses the threshold", "The loudest transient's peak",
                                             "The zone's start, pre-roll included"};
        ui::LabelRow("Snap offset", ui::Dp(150.0f), ui::Dp(84.0f), ui::kText);
        int        sel = static_cast<int>(ZoneNewSnapMode());
        const bool picked = ui::Segmented("##osnapmode", kModes, 3, &sel);
        const int  hov = ui::SegmentedHovered();
        if (hov >= 0 && hov < 3) ui::ShowTip(kTips[hov]);
        else ui::ItemTipText("Where the snap offset of a new sound's zones sits (the event lands there). A sound already "
                             "made keeps its own: Settings of > Snap offset.");
        if (picked && sel >= 0 && sel < 3 && sel != static_cast<int>(ZoneNewSnapMode())) {
            SetZoneNewSnapMode(static_cast<SyncMode>(sel));
            ShowLibraryToast(std::string("New sounds: snap offset at ") + kModes[sel] + " (yours, every project)", false);
        }
        {  // 13-73: whether a new Sound starts with Left / Right on (default Off, Antho 2026-10-19)
            static const char* const kOnOff[2] = {"Off", "On"};
            ui::LabelRow("Left / Right", ui::Dp(150.0f), ui::Dp(84.0f), ui::kText);
            int        ls = ZoneNewSides() ? 1 : 0;
            const bool lp = ui::Segmented("##onewsides", kOnOff, 2, &ls);
            ui::ItemTipText("Whether a new sound starts with Left / Right on (Pattern: Alternate L R). A sound already made "
                            "keeps its own: its Parts card.");
            if (lp && (ls == 1) != ZoneNewSides()) {
                SetZoneNewSides(ls == 1);
                ShowLibraryToast(std::string("New sounds: Left / Right ") + (ls == 1 ? "on (Alternate L R)" : "off") + " (yours, every project)",
                                 false);
            }
        }
        ui::ToneText("For the sounds you make from now on.", ui::kMuted, true);
    }
}

// "⚙ Options" (the gear, its badge: the options off their default) and its popup (the mockup's optsHTML): group
// Editing (Click plays, Pre-roll / Tail overwrite edges dragged by hand), group New sounds (13-67b: the snap offset mode),
// the foot.
float ZoneOptionsWidth()
{
    const int n = ZoneOptionsChanged();
    return ui::ButtonWidth("Options") + ui::IconRoom() + (n > 0 ? ui::Dp(18.0f) : 0.0f);
}

void ZoneOptionsButton(float h)
{
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float  w = ZoneOptionsWidth();
    const bool   open = ui::SolidButton("##zopts", ImVec2(w, h));
    ui::ItemTipText("Options: how editing behaves (yours, every project)");
    const ImVec2 bmax = ImGui::GetItemRectMax();
    ImDrawList*  dl = ImGui::GetWindowDrawList();
    const float  is = ui::IconPx(), x = p.x + ImGui::GetStyle().FramePadding.x;
    DrawToolGlyph(dl, ImVec2(x, std::floor(p.y + (h - is) * 0.5f)), is, kToolGear, ui::kText);
    const float tx = x + ui::IconRoom();
    dl->AddText(ImVec2(tx, std::floor(p.y + (h - ImGui::GetTextLineHeight()) * 0.5f)), ui::kText, "Options");
    if (const int nb = ZoneOptionsChanged(); nb > 0) {
        ImFont*           cf = ui::FontMonoTiny();
        const std::string cs = std::to_string(nb);
        const float       cw = ui::TextSizeIn(cf, cs.c_str()).x + ui::Dp(8.0f), ch = cf->FontSize + ui::Dp(4.0f);
        const float       bx = tx + ImGui::CalcTextSize("Options").x + ui::Dp(5.0f), cy = p.y + h * 0.5f;
        dl->AddRectFilled(ImVec2(bx, cy - ch * 0.5f), ImVec2(bx + cw, cy + ch * 0.5f), ui::kWarn, ch * 0.5f);
        ui::AddTextIn(dl, cf, ImVec2(bx + ui::Dp(4.0f), std::floor(cy - cf->FontSize * 0.5f)), IM_COL32(0x10, 0x13, 0x1A, 0xFF), cs.c_str());
    }
    if (open) {
        ImGui::OpenPopup("##zoptspop");
        ImGui::SetNextWindowPos(ImVec2(bmax.x, bmax.y + ui::Dp(4.0f)), ImGuiCond_Always, ImVec2(1.0f, 0.0f));
    }
    const float pw = std::min(ui::Dp(340.0f), std::max(ui::Dp(200.0f), ImGui::GetIO().DisplaySize.x - ui::Dp(32.0f)));
    ImGui::SetNextWindowSizeConstraints(ImVec2(pw, 0.0f), ImVec2(pw, std::numeric_limits<float>::max()));
    if (!ui::BeginPanelPopup("##zoptspop")) return;
    {
        const float side = ui::ControlH();
        ui::FontScope fs(ui::FontSemibold());
        FitText("\xE2\x9A\x99 Options \xC2\xB7 Sound Edit", ui::kText, std::max(1.0f, ImGui::GetContentRegionAvail().x - side - ImGui::GetStyle().ItemSpacing.x));
    }
    ImGui::SameLine();
    ImGui::SetCursorScreenPos(ImVec2(std::max(ImGui::GetCursorScreenPos().x, ui::LineRight() - ui::ControlH()), ImGui::GetCursorScreenPos().y));
    if (ui::IconOnlyButton("##zoptsclose", "\xE2\x9C\x95", "Close", ui::ControlH())) ImGui::CloseCurrentPopup();
    SoundEditOptionItems();
    ImGui::Dummy(ImVec2(1.0f, ui::Dp(2.0f)));
    ui::ToneText("Your preferences, for every project. Not part of the sound: no undo point.", ui::kFaint, true);
    ui::EndPanelPopup();
}

// ---- the Edit overlay -------------------------------------------------------------------------------

void DrawEditOverlay()
{
    if (!P.edit_open) {
        if (P.norm_job.active) CancelNormJob();  // 13-28: Edit closed while measuring: nothing written
        return;
    }
    const SoundStore&   store = GetSoundStore(nullptr);
    const SoundLibrary* lib = FindLibrary(store, P.edit_lib);
    const SoundNode*    n = lib ? FindNode(*lib, P.edit_uid) : nullptr;
    if (!n || n->type != SoundNodeType::Sound) {
        P.edit_open = false;
        ClearLaneGain();
        CancelNormJob();
        return;
    }
    P.edit_frame = ImGui::GetFrameCount();
    const SoundMedia& m = MediaOf(*n);
    // 13-62h (Lot E1): the Sound's zones -- waveform, keys and play (zone_wave_ui.h; 13-62i: every Sound); the media
    // must be read (E1 review: Delete is Edit's only then, when the zone waveform can act on it).
    const bool         zoned = m.ok;
    P.edit_divided = zoned;
    const ZoneWaveHost zh = ZoneHost(*lib, *n, m);
    if (zoned) ZonePumpPlay(zh);  // Loop restarts (C4)
    const ImVec2      ds = ImGui::GetIO().DisplaySize;
    if (P.edit_focus) {
        ImGui::SetNextWindowFocus();
        P.edit_focus = false;
    }
    bool close = false;
    P.edit_has_focus = false;
    // .edbox: --bg, stroke-strong edge, r10, its shadow (ui::BeginOverlayPanel).
    if (ui::BeginOverlayPanel("##soundedit", ImVec2(ui::Dp(24.0f), ui::Dp(52.0f)),
                              ImVec2(std::max(ui::Dp(200.0f), ds.x - ui::Dp(48.0f)), std::max(ui::Dp(160.0f), ds.y - ui::Dp(84.0f))),
                              ImGuiWindowFlags_NoScrollWithMouse)) {
        const bool focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
        P.edit_has_focus = focused;
        const bool keys = focused && !ImGui::GetIO().WantTextInput && !ui::DragNumberActive() &&
                          P.edit_opened != ImGui::GetFrameCount();  // the E that opened it
        const bool whole_playing = zoned && ZonePlaying(zh);
        auto toggle_play = [&] {
            if (zoned) ZoneTransport(zh);  // 13-62h: from the edit cursor (time selection, Loop), Gaps jumped
            else ShowLibraryToast("Cannot play: the sound file is not read", false);
        };
        // Header (13-7b, the mockup's .edhead): "▶ Play" / "■ Stop", the name (15 px semibold) and the
        // count (muted), then the tools at the top right: − + Fit, ?, Done (primary), ✕.
        // 13-53 (Antho 2026-10-14): the header sits 10 px in from the box's sides and 8 px under its top
        // (.edhead padding 8 10); every button of it is one height (bh, the Play button's) with its label
        // centred, the tools ItemSpacing apart, the groups (Play | name | tools) 10 px apart.
        const float bh = ui::LargeH();
        const float hpad_x = ui::Dp(10.0f), hpad_y = ui::Dp(8.0f), group_gap = ui::Dp(10.0f);
        ImGui::Indent(hpad_x);
        ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x, ImGui::GetCursorScreenPos().y + hpad_y));
        if (ui::ToggleButton(whole_playing ? "\xE2\x96\xA0 Stop##editplay" : "\xE2\x96\xB6 Play##editplay", whole_playing,
                             ImVec2(0.0f, bh)))
            toggle_play();
        ui::ItemTipText(Shown(*n).mode == DetectMode::Takes ? "Play the active take from the edit cursor (Space; again: stop)"
                                                            : "Play from the edit cursor (Space; again: stop)");
        if (zoned) {  // 13-62h (Lot E2): ⟳ Loop -- the time selection repeats, else the whole file (view state)
            ImGui::SameLine();
            if (ui::ToggleButton("\xE2\x86\xBB Loop##zloop", P.zv.loop, ImVec2(0.0f, bh))) ZoneToggleLoop(zh);
            const char*       k = ShortcutKeyLabel(kShortcutEditLoop);
            const std::string tip = std::string("Loop (your REAPER key for 1068 Transport: Toggle repeat; ") + (k && k[0] ? k : "R") +
                                    " here): the time selection repeats, else the whole file";
            ui::ItemTipText(tip.c_str());
        }
        ImGui::SameLine(0.0f, group_gap);
        std::string count;
        if (zoned) {  // 13-62h (the mockup's header): "n variations · n zones · n Gaps"
            const SoundData& sd = Shown(*n);
            const int        nv = static_cast<int>(ZoneVarBlocks(sd).size()), nz = PartZoneCount(sd), ng = GapCount(sd);
            count = std::to_string(nv) + (nv == 1 ? " variation" : " variations") + " \xC2\xB7 " + std::to_string(nz) +
                    (nz == 1 ? " zone" : " zones");
            if (ng > 0) count += " \xC2\xB7 " + std::to_string(ng) + (ng == 1 ? " Gap" : " Gaps");
        } else {  // the media not read
            count = VariationCountText(DetectedCount(n->uid, m, Shown(*n)), Shown(*n).frames);
        }
        // The tools sit at the top right; the name and count take what they leave (shortened),
        // and go above them only when the panel is too narrow for both (Antho, 13-7b).
        const float sp = ImGui::GetStyle().ItemSpacing.x;
        const float side_btn = bh;  // − + ✕: squares of the header's height
        const float zone_tools = zoned ? ZoneClickPlaysWidth() + ZoneOptionsWidth() + sp * 2.0f : 0.0f;  // 13-62h
        const float tools_w = zone_tools + side_btn * 2.0f + ui::ButtonWidth("Fit") + ui::Dp(18.0f) + ui::ButtonWidth("Done") + side_btn +
                              sp * 5.0f;
        bool        tools_beside = true;
        const float head_w = std::max(1.0f, ImGui::GetContentRegionAvail().x - hpad_x);  // the right inset too
        const float room = flow::StretchBeside(head_w, ui::Dp(80.0f), tools_w, group_gap, &tools_beside);
        {
            const ImVec2 p = ImGui::GetCursorScreenPos();
            const float  h = bh;
            ImDrawList*  dl = ImGui::GetWindowDrawList();
            ImFont*      tf = ui::FontPaneTitle();
            const float  name_w = std::min(ui::TextSizeIn(tf, n->name.c_str()).x + 1.0f, room * 0.6f);
            {
                ui::FontScope fs(tf);
                ui::EllipsisTextTip(dl, ImVec2(p.x, p.y + std::floor((h - tf->FontSize) * 0.5f) - 1.0f), p.x + name_w, ui::kText,
                                    n->name);
            }
            ui::EllipsisTextTip(dl, ImVec2(p.x + name_w + ui::Dp(10.0f), p.y + std::floor((h - ImGui::GetTextLineHeight()) * 0.5f)),
                                p.x + room, ui::kMuted, count);
            ImGui::Dummy(ImVec2(room, h));
        }
        if (tools_beside) ImGui::SameLine(0.0f, group_gap);
        else ImGui::SetCursorScreenPos(ImVec2(std::max(ImGui::GetCursorScreenPos().x, ui::LineRight() - hpad_x - tools_w),
                                              ImGui::GetCursorScreenPos().y));
        const float tools_right = ui::LineRight() - hpad_x;  // the tools wrap inside the header inset
        if (zoned) {  // 13-62h (Lot E2): Click plays (the speaker) and ⚙ Options, the user's prefs
            ZoneClickPlaysButton(bh);
            SameLineIfFits(ZoneOptionsWidth(), tools_right);
            ZoneOptionsButton(bh);
            SameLineIfFits(side_btn, tools_right);
        }
        if (ui::SolidButton("\xE2\x88\x92##zoomout", ImVec2(side_btn, bh)))
            ZoomView(1.5, P.view_start + (P.view_len > 0.0 ? P.view_len : m.source_len) * 0.5, m.source_len);
        ui::ItemTipText(("Zoom out (" + ViewNavRowText(kNavZoomOut) + ")").c_str());
        SameLineIfFits(side_btn, tools_right);
        if (ui::SolidButton("+##zoomin", ImVec2(side_btn, bh)))
            ZoomView(1.0 / 1.5, P.view_start + (P.view_len > 0.0 ? P.view_len : m.source_len) * 0.5, m.source_len);
        ui::ItemTipText(("Zoom in (" + ViewNavRowText(kNavZoomIn) + ")").c_str());
        SameLineIfFits(ui::ButtonWidth("Fit"), tools_right);
        if (ui::SolidButton("Fit", ImVec2(0.0f, bh))) {
            P.view_start = 0.0;
            P.view_len = 0.0;
            P.vzoom = 1.0;  // 13-45
        }
        SameLineIfFits(ui::Dp(18.0f), tools_right);
        ui::HelpMark("##edithelp", kZoneHelp, bh);
        SameLineIfFits(ui::ButtonWidth("Done"), tools_right);
        if (ui::PrimaryButton("Done", ImVec2(0.0f, bh))) close = true;
        SameLineIfFits(side_btn, tools_right);
        if (ui::IconOnlyButton("##editclose", "\xE2\x9C\x95", "Close (Esc)", side_btn)) close = true;
        ImGui::Unindent(hpad_x);
        {
            // The header's bottom line (.edhead border-bottom), across the whole box.
            const float  y = ImGui::GetCursorScreenPos().y + ui::Dp(1.0f);
            const ImVec2 wp = ImGui::GetWindowPos();
            ImGui::GetWindowDrawList()->AddLine(ImVec2(wp.x, y), ImVec2(wp.x + ImGui::GetWindowSize().x, y), ui::kStroke);
            ImGui::Dummy(ImVec2(1.0f, ui::Dp(6.0f)));
        }

        // Waveform | side strip. 13-62h (Antho): the right column's width is the user's (its left edge dragged).
        const float scale = std::max(0.01f, ui::Dp(1.0f));
        const float gap = ui::Dp(8.0f);
        const float avail_x = ImGui::GetContentRegionAvail().x;
        const float side_hi = (avail_x - ui::Dp(100.0f) - gap) / scale;
        const float side_w = ui::Dp(std::clamp(g_side_w.Get(), g_side_w.lo, std::max(g_side_w.lo, side_hi)));
        const float wave_w = std::max(ui::Dp(100.0f), avail_x - side_w - gap);
        const float col_h = std::max(ui::Dp(80.0f), ImGui::GetContentRegionAvail().y - ui::Dp(4.0f));
        // 13-62h (Antho): a zoned Sound's Selection panel is always under the waveform, outside its scroll, at the
        // height the user dragged (its top edge), down to a waveform of 140 px.
        const float sp_y = ImGui::GetStyle().ItemSpacing.y;
        const bool  zsel = zoned && m.ok;
        const float bar_h = zsel ? ui::Dp(8.0f) : 0.0f;
        const float sel_hi = (col_h - bar_h - 2.0f * sp_y - ui::Dp(140.0f)) / scale;
        const float sel_h = zsel ? std::min(ui::Dp(std::clamp(g_zsel_h.Get(), g_zsel_h.lo, std::max(g_zsel_h.lo, sel_hi))),
                                             std::max(0.0f, col_h - bar_h - 2.0f * sp_y - ui::Dp(60.0f)))
                                  : 0.0f;
        const float wave_h = zsel ? std::max(ui::Dp(60.0f), col_h - sel_h - bar_h - 2.0f * sp_y) : col_h;
        ImGui::BeginGroup();
        ImGui::BeginChild("##editwave", ImVec2(wave_w, wave_h), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollWithMouse);
        // 13-45: the view actions (your REAPER wheel and zoom keys, or your own: Shortcuts), the wheel over
        // the waveform, the keys while Edit has the focus.
        // 13-74: the gain rows' wheel (Ctrl+Shift by default) over it moves the selected zones' gain instead.
        const bool wave_hot = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows);
        bool       gain_wheel = false;
        const int  gain_notches = m.ok && zoned ? EditGainWheel(wave_hot, ImGui::GetID("##editgain"), &gain_wheel) : 0;
        if (m.ok) EditNavigate(ViewNavInput(wave_hot && !gain_wheel, keys, ImGui::GetID("##editnav")));
        if (gain_notches != 0 && !StoreReplaced()) ZoneGainNudge(zh, gain_notches);
        if (!m.ok) {
            FitText("Source not found", ui::kMissText);
        } else {
            const float y0 = ImGui::GetCursorScreenPos().y;
            EditLegend(store, *lib, *n, m);  // 13-62h / 13-62i: Lane mode and parts (divided), the zones' legend
            const float used = ImGui::GetCursorScreenPos().y - y0;
            if (!StoreReplaced())
                DrawSoundWaves("editwave", *lib, *n, m, std::max(ui::Dp(60.0f), wave_h - 24.0f - used) * static_cast<float>(P.vzoom), true,
                               nullptr);
        }
        ImGui::EndChild();
        if (zsel && !StoreReplaced()) {
            PanelSizeBar("##zselh", ImVec2(wave_w, bar_h), false, -1.0f, g_zsel_h, sel_hi,
                         "Drag: the Selection panel's height. Double-click: back to the default");
            ImGui::BeginChild("##zselpanel", ImVec2(wave_w, sel_h), ImGuiChildFlags_None, ImGuiWindowFlags_None);
            ZoneSelectionPanel(*lib, *n, m, zh);
            ImGui::EndChild();
        }
        ImGui::EndGroup();
        if (!StoreReplaced()) {
            // The side strip (.edside): surface, a stroke line on its left, the captions
            // "DETECTION · THIS SOUND ONLY" and "LEVEL" over their rows (.edrow: 70 px labels).
            ImGui::SameLine(0.0f, 0.0f);
            PanelSizeBar("##editsidew", ImVec2(gap, col_h), true, -1.0f, g_side_w, side_hi,
                         "Drag: the right column's width. Double-click: back to the default");
            ImGui::SameLine(0.0f, 0.0f);
            ui::PaneOpts side;  // 13-14: the theme's pane, 10 px inside, square, its edge on the left only
            side.edge = ui::PaneEdge::Left;
            side.pad = 10.0f;
            side.radius = 0.0f;
            ui::BeginPane("##editside", ImVec2(side_w, col_h), side);
            // 13-62h (Lot E2): the right column is its Settings of (the Sound or a part) and its detection (13-62i:
            // every Sound); the media not read: its detection only.
            if (zoned) {
                ZoneSettingsColumn(*lib, *n, m, zh);
            } else {
                ui::GroupCaption("Detection \xC2\xB7 this sound only", ui::kMuted);
                if (P.norm_job.active) CancelNormJob();  // 13-28: the media stopped reading
            }
            ui::EndPane();
        }
        // 13-61: a trim row's key (Shortcuts, KEYS: SOUND EDIT), on Edit's selection. 13-63: the split's too.
        // Review: taken only when it can run (a key while a drag is live waits for its commit; the media not
        // read, a toast says so), never dropped silently.
        if (P.trim_request >= 0 && !StoreReplaced() && !P.phase.Live() && !m.ok) {
            const bool split = std::exchange(P.trim_request, -1) == kShortcutEditSplit;
            ShowLibraryToast(split ? "Cannot split: the sound file is not read" : "Cannot trim: the sound file is not read", false);
        }
        if (P.trim_request >= 0 && m.ok && !StoreReplaced() && !P.phase.Live())  // 13-62h: on the zones (split / trims / 40508)
            ZoneEditRequest(zh, std::exchange(P.trim_request, -1));
        // 13-62 (D): the Solo / Mute row's key toggles the selected part's listening (view state: no undo
        // point, nothing written, so no media / live gating). No part selected, or not divided: nothing.
        if (P.listen_request != 0 && !StoreReplaced()) {
            const int listen = std::exchange(P.listen_request, 0);
            if (IsDivided(n->sound) && PartIndex(n->sound, P.part_sel) >= 0 && listen == 3) {
                // 13-67a: Exclusive Solo -- the selected part alone soloed; on the only soloed part: unsolo.
                const bool alone = P.part_solo.size() == 1 && P.part_solo.count(P.part_sel) == 1;
                P.part_solo.clear();
                if (!alone) P.part_solo.insert(P.part_sel);
            } else if (IsDivided(n->sound) && PartIndex(n->sound, P.part_sel) >= 0) {
                std::set<std::string>& on = listen == 1 ? P.part_solo : P.part_mute;
                if (!on.erase(P.part_sel)) on.insert(P.part_sel);
            } else if (IsDivided(n->sound)) {  // review: the mockup's toast (an undivided Sound: nothing)
                ShowLibraryToast("Select a part first: click its lane (Lane view) or its chip (Combined)", false);
            }
        }
        // 13-62F (F5): Next / Previous variation (REAPER's 40417 / 40416 keys): select it, the edit cursor at its
        // start, the view centred on it at the same zoom. View state: no undo point, nothing written. At the
        // last / first one nothing moves. Takes mode: the selection only (each take has its own time).
        if (P.nav_request != 0 && zoned && !StoreReplaced()) {  // 13-62h: its first zone, the cursor at its start
            ZoneStepVariation(zh, std::exchange(P.nav_request, 0));
        }
        // 13-62h (Lot D keys): Heal (one undo point, or a toast) and Loop (view state), once no drag is live.
        if (P.zone_request >= 0 && !StoreReplaced() && !P.phase.Live()) {
            const int zr = std::exchange(P.zone_request, -1);
            if (zoned) ZoneEditRequest(zh, zr);
            else  // E1 review: never dropped silently (the media not read)
                ShowLibraryToast(zr == kShortcutEditHeal ? "Cannot heal: the sound file is not read"
                                 : zr == kShortcutEditLoop ? "Cannot loop: the sound file is not read"
                                                           : "Nothing to do: the sound file is not read",
                                 false);
        }
        if (P.nav_request != 0 && !StoreReplaced() && !zoned) P.nav_request = 0;  // the media not read: nothing to step
        // 13-74: the gain rows' keys (one notch each; in a row: one undo point), on the selected zones.
        if (P.gain_request != 0 && !StoreReplaced()) {
            const int g = std::exchange(P.gain_request, 0);
            if (zoned && m.ok) ZoneGainNudge(zh, g);
            else ShowLibraryToast("Cannot change the gain: the sound file is not read", false);
        }
        if (keys && !StoreReplaced()) {
            // 13-62h: a zoned Sound's Delete, Space (from the edit cursor; again = stop) and Esc (the selection,
            // then the time selection, before Stop / close).
            bool esc_used = false;
            if (zoned) ZoneEditKeys(zh, &esc_used);
            else if (ImGui::IsKeyPressed(ImGuiKey_Space, false)) {
                if (AuditionPlaying()) StopAudition();
                else toggle_play();
            }
            if (!esc_used && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
                if (AuditionPlaying()) {
                    StopAudition();
                    P.zv.playing = false;  // 13-67a: a stop, not an end (a Loop never restarts it)
                } else {
                    close = true;
                }
            }
            if (ImGui::IsKeyPressed(ImGuiKey_E, false)) close = true;
        }
    }
    ui::EndOverlayPanel();
    if (close) {
        P.edit_open = false;
        ClearLaneGain();
        P.trim_request = -1;  // review (13-61): a key held for a drag never fires on the next Edit
        CancelLive();
        CancelNormJob();  // 13-28: nothing written, no undo point
        P.zv.drag = ZoneDrag{};  // 13-62h: a zone drag too
        P.zedit_on = false;      // 13-62h (Lot E2): a panel number's gesture too
    }
}

// ---- node panel sections --------------------------------------------------------------------------

// 13-7b (audit C4.1 / C5.1): the mockup's .pane-h on one line -- "▶ Play" (26 px, "■ Stop" in the
// on style while playing), the title (15 px semibold; a Sound's is its name field), then the
// parents' breadcrumb in 12 px muted, ellipsized, at most 45 % of the row; the breadcrumb goes
// under the title only when the column is too narrow for both. "played: X" under it.
void DrawHeader(const SoundStore& store, const SoundLibrary& lib, const SoundNode& n)
{
    const bool playing = NodePlaying(n.uid) || (AuditionPlaying() && AuditionTag() == "frame:" + n.uid);
    if (ui::ToggleButton(playing ? "\xE2\x96\xA0 Stop##play" : "\xE2\x96\xB6 Play##play", playing, ImVec2(0.0f, ui::LargeH()))) {
        if (playing) StopPlay();
        else PlayPanel(lib, n);  // 13-40: the Mix row selected, else the node
    }
    {
        const std::string child = MixSelected(lib, n);
        const SoundNode*  k = child.empty() ? nullptr : FindNode(lib, child);
        const std::string tip = playing ? std::string("Stop (Esc)")
                                : k     ? "Play the child selected in Mix, " + NodeName(store, *k) +
                                          ", as it plays on its own (Space); click its row again for the whole node"
                                        : std::string("Play what Populate would place for one event (Space)");
        ui::ItemTipText(tip.c_str());
    }
    const bool        sound = n.type == SoundNodeType::Sound;
    const float       min_title = ui::Dp(sound ? 140.0f : 60.0f);
    const float       sp = ui::Dp(8.0f);  // .pane-h gap
    SameLineIfFits(min_title);
    if (ImGui::GetCursorScreenPos().x > ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMin().x + 1.0f)
        ImGui::SameLine(0.0f, sp);
    const std::string crumbs = Crumbs(store, lib, n);
    const float       avail = std::max(1.0f, ImGui::GetContentRegionAvail().x);
    const float       crumbs_nat = ImGui::CalcTextSize(crumbs.c_str()).x + 1.0f;
    const float       crumbs_w = std::min(crumbs_nat, avail * 0.45f);
    const bool        crumbs_beside = avail - crumbs_w - sp >= min_title;
    const float       row_h = ui::LargeH();
    ImDrawList*       dl = ImGui::GetWindowDrawList();
    const ImVec2      p0 = ImGui::GetCursorScreenPos();
    float             title_end = p0.x;
    if (sound) {
        if (P.name_uid != n.uid) {
            P.name_uid = n.uid;
            std::snprintf(P.name_buf, sizeof(P.name_buf), "%s", n.name.c_str());
        }
        ui::TitleField("##name", P.name_buf, sizeof(P.name_buf), crumbs_beside ? avail - crumbs_w - sp : avail);
        const bool done = ImGui::IsItemDeactivatedAfterEdit();
        ui::ItemTipText("Click to rename");
        title_end = ImGui::GetItemRectMax().x;
        if (!done && !ImGui::IsItemActive() && std::strcmp(P.name_buf, n.name.c_str()) != 0)
            std::snprintf(P.name_buf, sizeof(P.name_buf), "%s", n.name.c_str());  // renamed elsewhere / undone
        if (done && P.name_buf[0] && n.name != P.name_buf) {
            const std::string name = P.name_buf, lib_uid = lib.uid;
            const LibraryRow  row = RowOfNode(lib, n.uid);
            Gesture("Rename", [lib_uid, row, name](SoundStore& s) { return RenameRow(s, lib_uid, row, name); });
            if (StoreReplaced()) return;
        }
    } else {
        // The container's title (the mockup's slotLabel): no icon here, the icon is in "Plays".
        ImFont*           tf = ui::FontPaneTitle();
        const std::string title = NodeTitle(store, lib, n);
        const float       room = crumbs_beside ? avail - crumbs_w - sp : avail;
        const float       tw = std::min(room, ui::TextSizeIn(tf, title.c_str()).x + 1.0f);
        {
            ui::FontScope fs(tf);
            ui::EllipsisTextTip(dl, ImVec2(p0.x, p0.y + std::floor((row_h - tf->FontSize) * 0.5f) - 1.0f), p0.x + tw, ui::kText,
                                title);
        }
        ImGui::Dummy(ImVec2(std::max(1.0f, tw), row_h));
        title_end = p0.x + tw;
    }
    if (crumbs_beside) {
        const float x = title_end + sp;
        ui::EllipsisTextTip(dl, ImVec2(x, p0.y + std::floor((row_h - ImGui::GetTextLineHeight()) * 0.5f)), x + crumbs_w,
                            ui::kMuted, crumbs);
    } else {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ui::EllipsisTextTip(dl, p, p.x + std::min(crumbs_nat, ImGui::GetContentRegionAvail().x), ui::kMuted, crumbs);
        ImGui::Dummy(ImVec2(1.0f, ImGui::GetTextLineHeight()));
    }
    if (P.plan.root == n.uid && P.plan_lib == lib.uid && !P.plan.played.empty())
        RichText({{"played: ", ui::kMuted}, {P.plan.played, ui::kText}});
}

// 13-7b (audit C4.3): Type as a plain row -- [Random | Blend | Switch on <group> ▾], one segmented
// control; its third option opens the group menu. Then, for a Switch (C4.4), "Default value" /
// "Preview value" selects and the hint.
void DrawTypeRows(const SoundStore& store, const SoundLibrary& lib, const SoundNode& n)
{
    static const char* const kTypes[3] = {"Random", "Blend", "Switch"};
    int sel = n.type == SoundNodeType::Random ? 0 : n.type == SoundNodeType::Blend ? 1 : 2;
    const LibraryRow  row = RowOfNode(lib, n.uid);
    const std::string lib_uid = lib.uid;
    const SwitchGroup* cur_g = n.type == SoundNodeType::Switch ? FindGroup(store, n.group_uid) : nullptr;
    const std::string sw_label = n.type == SoundNodeType::Switch
                                     ? "Switch on " + (cur_g ? cur_g->name : std::string("?")) + " \xE2\x96\xBE"
                                     : std::string("Switch on \xE2\x96\xBE");
    const char* const labels[3] = {"Random", "Blend", sw_label.c_str()};
    ui::LabelRow("Type", ui::Dp(150.0f));
    const ui::Icon kTypeIcons[3] = {TypeIcon(SoundNodeType::Random), TypeIcon(SoundNodeType::Blend),
                                    TypeIcon(SoundNodeType::Switch)};  // the mock-up's .tseg: an icon per type
    const int before = sel;
    const bool changed = ui::Segmented("##type", labels, 3, &sel, 0.0f, -1, kTypeIcons);
    // The Switch option opens the group menu, also when it is already the type (to change the group).
    const bool sw_clicked = ui::SegmentedHovered() == 2 && ImGui::IsMouseReleased(ImGuiMouseButton_Left) &&
                            !ImGui::IsMouseDragPastThreshold(ImGuiMouseButton_Left);
    if ((changed && sel == 2) || (sw_clicked && before == 2)) {
        ImGui::OpenPopup("##switchon");
    } else if (changed && sel != before) {
        const SoundNodeType t = sel == 0 ? SoundNodeType::Random : SoundNodeType::Blend;
        Gesture(std::string("Make ") + kTypes[sel], [lib_uid, row, t](SoundStore& s) { return SetRowsType(s, lib_uid, {row}, t, {}); });
        if (StoreReplaced()) return;
    }
    if (ui::BeginMenuPopup("##switchon")) {
        ui::MenuCaption("Switch on");
        if (store.groups.empty()) ImGui::TextDisabled("No Switch Group yet");
        for (const SwitchGroup& g : store.groups) {
            if (ImGui::MenuItem(g.name.c_str(), nullptr, n.type == SoundNodeType::Switch && n.group_uid == g.uid)) {
                const std::string gid = g.uid;
                ImGui::CloseCurrentPopup();
                ui::EndMenuPopup();
                Gesture("Switch on " + g.name,
                        [lib_uid, row, gid](SoundStore& s) { return SetRowsType(s, lib_uid, {row}, SoundNodeType::Switch, gid); });
                return;
            }
        }
        ui::EndMenuPopup();
    }
    if (n.type != SoundNodeType::Switch) return;
    const SwitchGroup* g = cur_g;
    if (!g) return;
    auto value_name = [&](const std::string& v) {
        for (const SwitchValue& x : g->values)
            if (x.uid == v) return x.name;
        return std::string("none");
    };
    const float combo_min = ui::Dp(150.0f);
    // Default value.
    const std::string def = DefaultValue(store, n);
    {
        const float w = ui::LabelRow("Default value", combo_min);
        // 13-18 D4: a node with no valid default of its own follows its group's ("Group default (X)").
        const bool        own = GroupHasValue(*g, n.default_value);
        const std::string glabel = "Group default (" + value_name(GroupDefaultValue(*g)) + ")";
        const bool        open = ui::BeginComboStyled("##default", own ? value_name(def).c_str() : glabel.c_str(),
                                                      std::min(w, std::max(combo_min, ui::Dp(180.0f))));
        ui::ItemTipText("Plays when the event's value is not set, or when a value has no sound of its own");
        if (open) {
            // First, follow the group's default (clears the node's own, a stale one too).
            if (ImGui::Selectable(glabel.c_str(), !own) && !n.default_value.empty()) {
                const std::string sw_uid = n.uid;
                ui::EndComboStyled();
                Gesture("Follow group default", [lib_uid, sw_uid](SoundStore& s) { return FollowGroupDefault(s, lib_uid, sw_uid); },
                        "Follows the group's default");
                return;
            }
            ui::ItemTipText("Plays the Switch Group's default (set by right-clicking a value in Switch Groups)");
            ImGui::Separator();
            for (const SwitchValue& v : g->values) {
                if (ImGui::Selectable(v.name.c_str(), own && v.uid == def) && !(own && v.uid == def)) {
                    const LibraryRow  vr{n.uid, v.uid};
                    const std::string name = v.name;
                    ui::EndComboStyled();
                    Gesture("Make default", [lib_uid, vr](SoundStore& s) { return MakeDefault(s, lib_uid, {vr}); }, name + " is the default");
                    return;
                }
            }
            ui::EndComboStyled();
        }
    }
    // Preview value.
    bool prev_known = false;  // a deleted value plays the default (SwitchPreviewChild)
    for (const SwitchValue& v : g->values) prev_known = prev_known || v.uid == n.preview_value;
    const std::string prev_label = !prev_known ? "(default: " + value_name(def) + ")" : value_name(n.preview_value);
    {
        const float w = ui::LabelRow("Preview value", combo_min);
        const bool  open = ui::BeginComboStyled("##preview", prev_label.c_str(), std::min(w, std::max(combo_min, ui::Dp(180.0f))));
        ui::ItemTipText("The value \xE2\x96\xB6 plays (nested Switches use their own preview value)");
        if (open) {
            std::string pick;
            bool        picked = false;
            if (ImGui::Selectable("(the default)", !prev_known) && !n.preview_value.empty()) picked = true;
            for (const SwitchValue& v : g->values)
                if (ImGui::Selectable(v.name.c_str(), v.uid == n.preview_value) && v.uid != n.preview_value) {
                    pick = v.uid;
                    picked = true;
                }
            ui::EndComboStyled();
            if (picked) {
                const std::string uid = n.uid;
                EditNode("Preview value", lib_uid, uid, [pick](SoundNode& x) {
                    x.preview_value = pick;
                    return true;
                });
                return;
            }
        }
    }
    // The hint (.hint): "Switches on Material · 6 values. A value with no sound of its own plays Concrete."
    ImGui::Dummy(ImVec2(1.0f, ui::Dp(2.0f)));
    std::vector<TextRun> hint = {{"Switches on ", ui::kMuted}, {g->name, ui::kText},
                                 {" \xC2\xB7 " + std::to_string(g->values.size()) + (g->values.size() == 1 ? " value" : " values"), ui::kMuted}};
    if (!n.unassigned.empty()) {
        hint.push_back({" \xC2\xB7 ", ui::kMuted});
        hint.push_back({std::to_string(n.unassigned.size()) + " unassigned", ui::kWarn});
    }
    hint.push_back({". A value with no sound of its own plays ", ui::kMuted});
    hint.push_back({value_name(def), ui::kText});
    hint.push_back({".", ui::kMuted});
    RichText(hint);
    bool def_has_sound = false;
    for (const SwitchCase& c : n.cases) def_has_sound = def_has_sound || (c.value_uid == def && !c.child_uid.empty());
    if (!def.empty() && !def_has_sound)
        RichText({{value_name(def) + " (the default value) has no sound yet: an event on a value with no sound of its own gets nothing until the default value has a sound.", ui::kWarn}});
    ImGui::Dummy(ImVec2(1.0f, ui::Dp(2.0f)));
}

// 13-7b (audit C4.5 / C5.2): Volume as a plain row (no card): the muted label, the fader, its dB.
void DrawVolume(const SoundLibrary& lib, const SoundNode& n)
{
    ui::LabelRow("Volume", ui::Dp(120.0f));
    double v = 0.0;
    if (VolumeControl("##vol", lib.uid, n.uid, n.volume_db, &v, 1e9f))
        EditNode("Volume", lib.uid, n.uid, [v](SoundNode& x) {
            x.volume_db = v;
            return true;
        });
}

// The Mix label of a child: under a Switch, the value(s) it plays for, then its own name.
std::string MixLabel(const SoundStore& store, const SoundNode& sw, const SoundNode& k)
{
    if (sw.type != SoundNodeType::Switch) return k.name;
    const SwitchGroup* g = FindGroup(store, sw.group_uid);
    std::string vals;
    for (const SwitchCase& c : sw.cases) {
        if (c.child_uid != k.uid || !g) continue;
        for (const SwitchValue& v : g->values)
            if (v.uid == c.value_uid) vals += (vals.empty() ? "" : ", ") + v.name;
    }
    if (vals.empty()) return k.name;
    return k.name.empty() ? vals : vals + " \xC2\xB7 " + k.name;
}

// 13-7b (audit C4.6): the "Mix" section, "· N children" after its title; one .mixrow per child --
// its type icon (muted), its name in a column 80-150 px wide, its fader and dB.
void DrawMix(const SoundStore& store, const SoundLibrary& lib, const SoundNode& n)
{
    SeedSection("Mix", n.type != SoundNodeType::Switch);
    const std::vector<const SoundNode*> kids = ChildNodes(lib, n);
    const std::string suffix = "\xC2\xB7 " + std::to_string(kids.size()) + (kids.size() == 1 ? " child" : " children");
    ui::SectionOpts opts;
    opts.suffix = suffix.c_str();
    ui::Section s;
    if (ui::BeginSection(s, "Mix", opts)) {
        if (kids.empty()) FitText("No children", ui::kFaint);
        const float icon_col = ui::Dp(18.0f) + ImGui::GetStyle().ItemSpacing.x;
        const std::string selected = MixSelected(lib, n);  // 13-40
        for (const SoundNode* k : kids) {
            if (StoreReplaced()) break;
            ImGui::PushID(k->uid.c_str());
            const ImVec2 ip = ImGui::GetCursorScreenPos();
            ImDrawList*  dl = ImGui::GetWindowDrawList();
            // 13-40: the selected row's fill under the row (drawn first: channel 0).
            dl->ChannelsSplit(2);
            dl->ChannelsSetCurrent(1);
            ui::PaintIconAt(dl, TypeIcon(k->type), ip.x, ip.y, ImGui::GetFrameHeight());
            ImGui::SetCursorScreenPos(ImVec2(ip.x + icon_col, ip.y));
            const float avail = ImGui::GetContentRegionAvail().x;
            const float name_col = std::clamp(avail * 0.4f, ui::Dp(80.0f), ui::Dp(150.0f));
            const bool  is_sel = selected == k->uid;
            ui::LabelRowText(MixLabel(store, n, *k), ui::Dp(120.0f), name_col,
                             IsLit(k->uid) ? ui::kAccent : is_sel ? ui::kAccentText : ui::kText);
            // 13-40: a click on its icon or name selects the row (again: none); Play / Space then play it.
            const ImVec2 lmax = ImGui::GetItemRectMax();
            const bool   over = ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(ip, lmax) && !ImGui::IsAnyItemActive();
            if (over) {
                ui::ShowTip(is_sel ? "Selected: Play / Space plays this child \xC2\xB7 click again to unselect"
                                   : "Click to select: Play / Space then play this child");
                if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                    P.mix_parent = n.uid;
                    P.mix_child = is_sel ? std::string() : k->uid;
                }
            }
            double v = 0.0;
            const std::string kid = k->uid;
            if (VolumeControl("##mixvol", lib.uid, kid, k->volume_db, &v, 1e9f))
                EditNode("Volume", lib.uid, kid, [v](SoundNode& x) {
                    x.volume_db = v;
                    return true;
                });
            dl->ChannelsSetCurrent(0);
            if (is_sel || over) {
                const float y1 = std::max(ImGui::GetItemRectMax().y, lmax.y);
                dl->AddRectFilled(ImVec2(ip.x - ui::Dp(4.0f), ip.y - ui::Dp(1.0f)), ImVec2(ui::LineRight() + ui::Dp(4.0f), y1 + ui::Dp(1.0f)),
                                  is_sel ? ui::kAccentSoft : ui::kRowAlt, ui::kRadiusSm);
            }
            dl->ChannelsMerge();
            ImGui::PopID();
        }
    }
    ui::EndSection(s);
}

void DrawRandomAvoid(const SoundLibrary& lib, const SoundNode& n)
{
    const int v = AvoidControl("##avoid", n.avoid_repeat);  // "Avoid repeating [the last 2 v]"
    if (v >= 0)
        EditNode("Avoid repeating", lib.uid, n.uid, [v](SoundNode& x) {
            x.avoid_repeat = v;
            return true;
        });
}

// A lane's volume as the mockup's .lnum dB field (46 px), live while dragged / typed, one undo
// point when entered. True when a value is to be written (*out).
bool VolumeNumber(const char* id, const std::string& key, double stored, double* out, float width)
{
    bool   commit = false;
    double d = P.vol_key == key ? P.vol_live : stored;
    switch (ui::DragNumber(id, &d, 0.1, 1, "dB", width, /*trim=*/true)) {
    case ui::DragNumberEvent::Live:
        P.vol_key = key;
        P.vol_live = std::clamp(d, kMinDb, kMaxDb);
        P.vol_gid = 0;
        break;
    case ui::DragNumberEvent::Commit:
        if (P.vol_key == key) P.vol_key.clear();
        *out = std::clamp(d, kMinDb, kMaxDb);
        commit = std::fabs(*out - stored) > 1e-6;
        break;
    case ui::DragNumberEvent::Cancel:
        if (P.vol_key == key) P.vol_key.clear();
        break;
    default: break;
    }
    return commit;
}

// 13-31: a Blend layer's roles (the "plays on" column, any layer type): a chip whose preview is the role
// labels joined ("toe, toe end"), or a muted "Combined only" while it has none; its list = checkable
// role keys (the common ones, "More roles", the custom ones), then "Keep offset on separate events"
// (greyed while no role). The list stays open; one undo point per click.
void BlendLayerRoles(const SoundLibrary& lib, const SoundNode& k, float w)
{
    // 13-31 (Q1): a Blend where no layer has a role plays every layer on every event.
    const SoundNode* parent = k.parent.empty() ? nullptr : FindNode(lib, k.parent);
    const bool       uses_roles = parent && BlendUsesRoles(lib, *parent);
    std::string joined;
    for (const std::string& r : k.layer_roles) joined += (joined.empty() ? "" : ", ") + RoleKeyLabel(r);
    ImDrawList*  dl = ImGui::GetWindowDrawList();
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const float  right = p0.x + w;
    const bool   open = ui::BeginChipCombo("##roles", joined.c_str(), w, false, 0, right);
    if (joined.empty()) {  // the muted preview (the chip draws its own in kText)
        const float h = ImGui::GetFrameHeight();
        ui::EllipsisText(dl, ImVec2(p0.x + ImGui::GetStyle().FramePadding.x, p0.y + (h - ImGui::GetTextLineHeight()) * 0.5f),
                         right - ui::Dp(18.0f), ui::kMuted, uses_roles ? "Combined only" : "Every event");
    }
    if (open) {
        const std::vector<std::string>& all = LibraryRoleKeys();
        const size_t                    nb = std::min(LibraryBuiltinRoleCount(), all.size());
        static const char* const        kCommon[] = {"heel", "toe", "toe_end", "hand", "knee"};
        std::vector<std::string>        keys;
        if (P.layer_roles_more) keys.assign(all.begin(), all.begin() + static_cast<long>(nb));
        else keys.assign(std::begin(kCommon), std::end(kCommon));
        const std::vector<std::string> custom(all.begin() + static_cast<long>(nb), all.end());
        for (const std::string& r : k.layer_roles)  // a role held outside the list stays visible (and removable)
            if (std::find(keys.begin(), keys.end(), r) == keys.end() && std::find(custom.begin(), custom.end(), r) == custom.end())
                keys.push_back(r);
        std::string toggle;
        bool        koff = false;
        ImGui::PushItemFlag(ImGuiItemFlags_AutoClosePopups, false);
        ui::MenuCaption("Plays on");
        auto rows = [&](const std::vector<std::string>& list) {
            for (const std::string& key : list) {
                const bool on = std::find(k.layer_roles.begin(), k.layer_roles.end(), key) != k.layer_roles.end();
                if (ui::MenuEntry((RoleKeyLabel(key) + "##" + key).c_str(), nullptr, on)) toggle = key;
            }
        };
        rows(keys);
        if (!custom.empty()) {
            ui::MenuSeparator();
            ui::MenuCaption("Custom");
            rows(custom);
        }
        if (ui::MenuEntry(P.layer_roles_more ? "Fewer roles" : "More roles\xE2\x80\xA6")) P.layer_roles_more = !P.layer_roles_more;
        ui::MenuSeparator();
        if (ui::MenuEntry("Keep offset on separate events", nullptr, k.layer_keep_offset, !k.layer_roles.empty())) koff = true;
        if (k.layer_roles.empty()) ui::ItemTipText("Give this layer a role first");
        else ui::ItemTipText("On an event with a role this layer keeps its offset (off: offset 0)");
        ImGui::PopItemFlag();
        ui::EndChipCombo();
        if (!toggle.empty()) {
            std::vector<std::string> roles = k.layer_roles;
            const auto               it = std::find(roles.begin(), roles.end(), toggle);
            if (it != roles.end()) roles.erase(it);
            else roles.push_back(toggle);
            std::string list;
            for (const std::string& r : roles) list += (list.empty() ? "" : ", ") + RoleKeyLabel(r);
            bool sibling_roles = false;  // another layer of this Blend still has a role
            if (parent)
                for (const SoundNode* c : ChildrenOf(lib, parent->uid))
                    if (c->uid != k.uid && !c->layer_roles.empty()) sibling_roles = true;
            const std::string toast = !roles.empty()   ? "Layer plays on: " + list + " (and combined events)"
                                      : sibling_roles ? std::string("Layer plays on combined events only")
                                                      : std::string("No layer has a role now: every layer plays on every event");
            EditNode("Layer roles", lib.uid, k.uid, [roles](SoundNode& x) {
                if (x.layer_roles == roles) return false;
                x.layer_roles = roles;
                return true;
            }, toast);
        } else if (koff) {
            const bool v = !k.layer_keep_offset;
            EditNode("Layer keep offset", lib.uid, k.uid, [v](SoundNode& x) {
                if (x.layer_keep_offset == v) return false;
                x.layer_keep_offset = v;
                return true;
            }, v ? "Layer keeps its offset on separate events" : "Layer offset applies on combined events only");
        }
    }
    if (open) return;
    if (k.layer_roles.empty() && !uses_roles)
        ui::ItemTipText("No layer of this Blend has a role: every layer plays on every event, offsets kept. Give layers "
                        "a role to split it by event");
    else if (k.layer_roles.empty()) ui::ItemTipText("Combined only: silent on separate events (heel, toe...) until given a role");
    else ui::ItemTipText(("Plays on combined events and on: " + joined).c_str());
}

// 13-7b (audit C4.8): the Blend layer view (.blendview) -- a dark box (r7, pad 6); a header row
// with the "ms" caption, the ruler (mono 10 faint, labels centred on their time) and the
// "offset" / "volume" captions; then one row per layer: its icon and name, its track (surface,
// stroke edge, the zero line) with the bar (solid accent, hatched where the length varies; drag
// it to offset the layer, its "+12 ms" chip on it), its offset (ms) and its volume (dB) beside
// the track. A Blend has no Mix section: the lane's dB is its child's only volume (Antho
// 2026-10-12, one control per value). A narrow column puts the two fields under the track.
// 13-45: the lanes zoom and scroll like every timeline (view_nav.h: the wheel over the box, the keys
// while it is under the mouse, a middle-drag pans); a double-click on the ruler shows the whole Blend.
void DrawBlendView(const SoundStore& store, const SoundLibrary& lib, const SoundNode& n)
{
    const std::vector<const SoundNode*> parts = BlendParts(lib, n);
    if (parts.empty()) return;
    double end_ms = 100.0;
    for (const SoundNode* k : parts) {
        const double off = P.off_uid == k->uid ? P.off_live : k->offset_ms;
        end_ms = std::max(end_ms, off + NodeLengthRange(store, lib, k->uid).max_s * 1000.0);
    }
    const double step = end_ms > 1500.0 ? 500.0 : end_ms > 600.0 ? 200.0 : end_ms > 250.0 ? 100.0 : 50.0;
    const double total_ms = std::ceil((end_ms + step * 0.3) / step) * step;
    // 13-45: the view window [bv0, bv0 + bspan] ms (the whole Blend unless zoomed).
    if (P.blend_uid != n.uid) {
        P.blend_uid = n.uid;
        P.blend_v0 = 0.0;
        P.blend_span = 0.0;
        P.blend_pan = false;
        P.blend_focus = false;
    }
    double bspan = P.blend_span > 0.0 ? std::min(P.blend_span, total_ms) : total_ms;
    double bv0 = std::clamp(P.blend_v0, 0.0, std::max(0.0, total_ms - bspan));

    ImDrawList*  dl = ImGui::GetWindowDrawList();
    const ImGuiIO& io = ImGui::GetIO();
    const float  pad = ui::Dp(6.0f), gap = ui::Dp(6.0f);
    const ImVec2 box0 = ImGui::GetCursorScreenPos();
    const float  box_x1 = std::max(box0.x + ui::Dp(60.0f), ui::LineRight());
    const float  x0 = box0.x + pad;
    const float  fh = ImGui::GetFrameHeight();
    const float  ms_w = ui::Dp(52.0f), db_w = ui::Dp(46.0f), part_w = ui::Dp(92.0f);  // 13-31: the "plays on" column
    const float  ms_full = ui::NumberFieldWidth(ms_w, "ms"), db_full = ui::NumberFieldWidth(db_w, "dB");
    const float  fields_w = ms_full + gap + db_full + gap + part_w;
    const float  head_h = ui::Dp(16.0f);
    const float  rgap = ui::Dp(2.0f);
    // 13-14 (C4.8): the lanes take at most 196 px (.lanes { max-height:196px; overflow-y:auto }) and
    // scroll beyond; a scrolling list keeps the scrollbar's room at the right of its rows.
    const float lanes_max = ui::Dp(196.0f);
    float       x1 = box_x1 - pad, inner = 0.0f, name_w = 0.0f, row_h = 0.0f;
    bool        stacked = false;
    auto        layout = [&] {
        inner = x1 - x0;
        name_w = std::clamp(inner * 0.3f, ui::Dp(84.0f), ui::Dp(120.0f));
        stacked = inner < name_w + gap + ui::Dp(80.0f) + gap + fields_w;
        row_h = stacked ? std::max(ui::Dp(30.0f), fh) + fh + ui::Dp(2.0f) : std::max(ui::Dp(30.0f), fh);
    };
    layout();
    const bool scrolls = (row_h + rgap) * static_cast<float>(parts.size()) > lanes_max;
    if (scrolls) {
        x1 -= ImGui::GetStyle().ScrollbarSize;
        layout();
    }
    const float lanes_h = std::min(lanes_max, (row_h + rgap) * static_cast<float>(parts.size()));
    const float lane_x0 = x0 + name_w + gap;
    const float lane_w = std::max(ui::Dp(24.0f), (stacked ? x1 : x1 - fields_w - gap) - lane_x0);
    const float box_h = pad * 2.0f + head_h + lanes_h;
    // 13-45: the view actions over the box (not while a bar or a field is dragged), and a middle-drag.
    ViewNavStep nav;
    {
        const ImVec2 box1(box_x1, box0.y + box_h);
        const bool   hot = P.off_uid.empty() && ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) &&
                         ImGui::IsMouseHoveringRect(box0, box1, false);
        // 13-45 review: the box sits in the scrolling Sound panel: until it is zoomed in or clicked, the
        // plain / Shift wheel scrolls the panel (as before), Ctrl / Alt wheels still run their action.
        const bool in_box = ImGui::IsMouseHoveringRect(box0, box1, false) && ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows);
        for (int b = 0; b < 3; ++b)
            if (ImGui::IsMouseClicked(b)) P.blend_focus = in_box;
        // 13-60b review: Scroll vertically only when the lanes scroll (Zoom vertically never): a wheel bound
        // to a move the lanes do not have stays the Sound panel's (or REAPER's).
        nav = ViewNavInput(hot, hot, ImGui::GetID("##blendnav"), !(P.blend_focus || P.blend_span > 0.0),
                           scrolls ? kNavMovesScrollV : 0);
        if (nav.zoom_h != 0.0) {
            const double f = std::clamp(static_cast<double>((io.MousePos.x - lane_x0) / lane_w), 0.0, 1.0);
            const double at = bv0 + f * bspan;
            const double ns = std::clamp(bspan * std::pow(0.8, nav.zoom_h), std::min(total_ms, 10.0), total_ms);
            bv0 = at - (at - bv0) * ns / bspan;
            bspan = ns;
        }
        bv0 += (nav.scroll_h * 0.1 + nav.scroll_h_pages) * bspan;
        if (hot && ImGui::IsMouseClicked(ImGuiMouseButton_Middle)) P.blend_pan = true;
        if (P.blend_pan) {
            if (!ImGui::IsMouseDown(ImGuiMouseButton_Middle)) P.blend_pan = false;
            else bv0 -= static_cast<double>(io.MouseDelta.x / lane_w) * bspan;
        }
        bv0 = std::clamp(bv0, 0.0, std::max(0.0, total_ms - bspan));
        P.blend_v0 = bv0;
        P.blend_span = bspan >= total_ms - 1e-9 ? 0.0 : bspan;
    }
    const bool zoomed = P.blend_span > 0.0;
    // The box behind (its height is known beforehand: one row per layer, up to the cap).
    dl->AddRectFilled(box0, ImVec2(box_x1, box0.y + box_h), ui::kBg, ui::kRadiusMd);
    dl->AddRect(box0, ImVec2(box_x1, box0.y + box_h), ui::kStroke, ui::kRadiusMd);
    auto X = [&](double ms) { return lane_x0 + static_cast<float>((ms - bv0) / bspan) * lane_w; };

    // Header: "ms" | the ruler | "offset" | "volume".
    {
        const float  y = box0.y + pad;
        ImFont*      cf = ui::FontTiny();
        ImFont*      rf = ui::FontMonoTiny();
        const float  cy = y + std::floor((head_h - cf->FontSize) * 0.5f);
        ui::AddTextIn(dl, cf, ImVec2(x0, cy), ui::kFaint, "ms");
        if (!stacked) {
            ui::AddTextIn(dl, cf, ImVec2(x1 - fields_w, cy), ui::kFaint, "offset");
            ui::AddTextIn(dl, cf, ImVec2(x1 - fields_w + ms_full + gap, cy), ui::kFaint, "volume");
            ui::AddTextIn(dl, cf, ImVec2(x1 - part_w, cy), ui::kFaint, "plays on");
        }
        dl->PushClipRect(ImVec2(lane_x0 - ui::Dp(20.0f), y), ImVec2(lane_x0 + lane_w + ui::Dp(20.0f), y + head_h), true);
        // 13-45: zoomed, finer steps over the view window (unzoomed: as before).
        const double rstep = !zoomed            ? step
                             : bspan > 1500.0   ? 500.0
                             : bspan > 600.0    ? 200.0
                             : bspan > 250.0    ? 100.0
                             : bspan > 100.0    ? 50.0
                             : bspan > 40.0     ? 20.0
                             : bspan > 20.0     ? 10.0
                                                : 5.0;
        const double t0 = std::ceil(bv0 / rstep - 1e-9) * rstep;
        const int    nt = dash::TickCount(t0, bv0 + bspan, rstep, 1e-6);  // freeze audit #8: capped, never endless
        for (int i = 0; i < nt; ++i) {
            const double t = t0 + i * rstep;
            if ((t - bv0) / bspan > 0.94) break;  // the last label would run past the track
            char lab[32];
            std::snprintf(lab, sizeof(lab), "%.0f", t);
            const ImVec2 ts = ui::TextSizeIn(rf, lab);
            ui::AddTextIn(dl, rf, ImVec2(std::floor(X(t) - ts.x * 0.5f), y + std::floor((head_h - ts.y) * 0.5f)), ui::kFaint, lab);
        }
        dl->PopClipRect();
        // 13-45: a double-click on the ruler shows the whole Blend again.
        ImGui::SetCursorScreenPos(ImVec2(lane_x0, y));
        ImGui::InvisibleButton("##blendruler", ImVec2(std::max(1.0f, lane_w), head_h));
        if (zoomed) {
            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                P.blend_v0 = 0.0;
                P.blend_span = 0.0;
            }
            ui::ItemTipText("Zoomed in: double-click to see the whole Blend");
        }
    }
    // The lanes in a child of their own (scrolled past the cap); drawn at the same screen x.
    ImGui::SetCursorScreenPos(ImVec2(box0.x + 1.0f, box0.y + pad + head_h));
    ImGui::BeginChild("##blendlanes", ImVec2(std::max(1.0f, box_x1 - box0.x - 2.0f), lanes_h), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoBackground);
    dl = ImGui::GetWindowDrawList();
    // 13-45: Scroll vertically scrolls the lanes (Zoom vertically: nothing, the rows keep their height).
    if (const double dy = nav.scroll_v * 0.15 + nav.scroll_v_pages; dy != 0.0)
        ImGui::SetScrollY(ImGui::GetScrollY() + static_cast<float>(dy) * lanes_h);
    const float lanes_top = ImGui::GetCursorScreenPos().y;
    float       y = lanes_top;
    for (const SoundNode* k : parts) {
        if (StoreReplaced()) break;
        ImGui::PushID(k->uid.c_str());
        const std::string kid = k->uid;
        const float       th = ui::Dp(22.0f);  // .ltrack
        const float       line_h = std::max(ui::Dp(30.0f), fh);
        const float       ty = y + std::floor((line_h - th) * 0.5f);
        // .ln2: the icon (13 px, muted) and the name.
        ui::PaintIconAt(dl, TypeIcon(k->type), x0, y, line_h, ui::Dp(13.0f));
        const float nx = x0 + ui::IconRoom(ui::Dp(13.0f), ui::Dp(5.0f));
        ui::EllipsisTextTip(dl, ImVec2(nx, y + std::floor((line_h - ImGui::GetTextLineHeight()) * 0.5f)), x0 + name_w,
                            IsLit(kid) ? ui::kAccent : ui::kText, k->name.empty() ? std::string(TypeName(k->type)) : k->name);
        // The track and its zero line.
        dl->AddRectFilled(ImVec2(lane_x0, ty), ImVec2(lane_x0 + lane_w, ty + th), ui::kSurface, ui::kRadiusSm);
        dl->AddRect(ImVec2(lane_x0, ty), ImVec2(lane_x0 + lane_w, ty + th), ui::kStroke, ui::kRadiusSm);
        if (bv0 <= 1e-9) dl->AddLine(ImVec2(std::floor(X(0.0)) + 0.5f, ty), ImVec2(std::floor(X(0.0)) + 0.5f, ty + th), ui::kStrokeStrong);
        // The bar.
        const LengthRange r = NodeLengthRange(store, lib, kid);
        const double      off = P.off_uid == kid ? P.off_live : k->offset_ms;
        // 13-45: the bar within the view window (a bar outside it is not drawn, nor dragged).
        const float       bx0 = X(off);
        const float       bx1 = bx0 + std::max(ui::Dp(3.0f), static_cast<float>(r.max_s * 1000.0 / bspan) * lane_w);
        // (One being dragged stays, at the lane's edge if needs be: its drag must not lose its button.)
        const float       vx0 = std::min(std::max(bx0, lane_x0), lane_x0 + lane_w - 3.0f), vx1 = std::min(bx1, lane_x0 + lane_w);
        const bool        bar_seen = vx1 - vx0 >= 1.0f || P.off_uid == kid;
        const ImVec2      ba(vx0, ty + 3.0f), bb(std::max(vx0 + 3.0f, vx1), ty + th - 3.0f);
        if (bar_seen) {
            dl->AddRectFilled(ba, bb, ui::kAccent, 3.0f);
            if (r.varies()) {
                const float bmin = X(off + r.min_s * 1000.0);
                if (bmin < bb.x) Hatch(dl, ImVec2(std::max(ba.x, bmin), ba.y), bb, ui::kHatchLight);
            }
            ImGui::SetCursorScreenPos(ImVec2(vx0, ty));
            ImGui::InvisibleButton("##bar", ImVec2(std::max(ui::Dp(6.0f), bb.x - vx0), th));
            const bool bar_hot = ImGui::IsItemHovered() || ImGui::IsItemActive();
            if (bar_hot) {
                ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
                dl->AddRect(ImVec2(ba.x - 2.0f, ba.y - 2.0f), ImVec2(bb.x + 2.0f, bb.y + 2.0f), ui::kRing, 5.0f, 0, 2.0f);
            }
            if (ImGui::IsItemHovered() && !ImGui::IsItemActive()) ui::ItemTipText("Drag to set the offset \xC2\xB7 Shift = free");
            if (ImGui::IsItemActivated()) {
                P.off_uid = kid;
                P.off0 = k->offset_ms;
                P.off_live = k->offset_ms;
                P.off_scale = bspan;  // 13-45: the ms the lane shows
                P.off_dx = 0.0f;
            }
            if (ImGui::IsItemActive() && P.off_uid == kid) {
                P.off_dx += io.MouseDelta.x;
                double v = P.off0 + static_cast<double>(P.off_dx) / lane_w * P.off_scale;
                if (!io.KeyShift) v = std::round(v);
                P.off_live = std::max(0.0, v);
                // The .barlbl chip on the bar (mono 11, raised, stroke-strong edge, r4).
                char lab[32];
                std::snprintf(lab, sizeof(lab), "%s%.0f ms", P.off_live >= 0.0 ? "+" : "", P.off_live);
                ImFont*      cf = ui::FontMonoSmall();
                const ImVec2 ts = ui::TextSizeIn(cf, lab);
                const ImVec2 c0(std::min(vx0 + 4.0f, lane_x0 + lane_w - ts.x - 8.0f), ty + std::floor((th - ts.y - 2.0f) * 0.5f));
                const ImVec2 c1(c0.x + ts.x + 8.0f, c0.y + ts.y + 2.0f);
                dl->AddRectFilled(c0, c1, ui::kRaised, 4.0f);
                dl->AddRect(c0, c1, ui::kStrokeStrong, 4.0f);
                ui::AddTextIn(dl, cf, ImVec2(c0.x + 4.0f, c0.y + 1.0f), ui::kText, lab);
            }
            if (ImGui::IsItemDeactivated() && P.off_uid == kid) {
                const double v = P.off_live;
                P.off_uid.clear();
                if (std::fabs(v - k->offset_ms) > 1e-6)
                    EditNode("Offset", lib.uid, kid, [v](SoundNode& x) {
                        x.offset_ms = v;
                        return true;
                    });
            }
        }
        if (StoreReplaced()) {
            ImGui::PopID();
            break;
        }
        // The offset (ms) then the volume (dB): beside the track, or under it in a narrow column.
        const float fy = stacked ? y + line_h + ui::Dp(2.0f) : y + std::floor((line_h - fh) * 0.5f);
        ImGui::SetCursorScreenPos(ImVec2(stacked ? lane_x0 : x1 - fields_w, fy));
        double ms = off;
        switch (ui::DragNumber("##ms", &ms, 1.0, 0, "ms", ms_w)) {
        case ui::DragNumberEvent::Live:
            P.off_uid = kid;
            P.off_live = std::max(0.0, ms);
            break;
        case ui::DragNumberEvent::Commit: {
            const double v = std::max(0.0, ms);
            if (P.off_uid == kid) P.off_uid.clear();
            if (std::fabs(v - k->offset_ms) > 1e-6)
                EditNode("Offset", lib.uid, kid, [v](SoundNode& x) {
                    x.offset_ms = v;
                    return true;
                });
            break;
        }
        case ui::DragNumberEvent::Cancel:
            if (P.off_uid == kid) P.off_uid.clear();
            break;
        default: break;
        }
        if (!k->layer_roles.empty() && !k->layer_keep_offset)  // 13-31 (a)
            ui::ItemTipText("Offset \xC2\xB7 applies on combined events only (0 on events with a role)");
        if (StoreReplaced()) {
            ImGui::PopID();
            break;
        }
        ImGui::SetCursorScreenPos(ImVec2((stacked ? lane_x0 : x1 - fields_w) + ms_full + gap, fy));
        double v = 0.0;
        if (VolumeNumber("##vol", kid, k->volume_db, &v, db_w))
            EditNode("Volume", lib.uid, kid, [v](SoundNode& x) {
                x.volume_db = v;
                return true;
            });
        if (StoreReplaced()) {
            ImGui::PopID();
            break;
        }
        ImGui::SetCursorScreenPos(ImVec2((stacked ? lane_x0 : x1 - fields_w) + ms_full + gap + db_full + gap, fy));
        BlendLayerRoles(lib, *k, part_w);
        if (StoreReplaced()) {
            ImGui::PopID();
            break;
        }
        ImGui::PopID();
        y += row_h + rgap;
    }
    if (!StoreReplaced() && NodePlaying(n.uid)) {
        const float x = std::floor(X(AuditionPosition() * 1000.0)) + 0.5f;
        if (x >= lane_x0 && x <= lane_x0 + lane_w) dl->AddLine(ImVec2(x, lanes_top), ImVec2(x, y), ui::kText, 1.0f);
    }
    ImGui::SetCursorScreenPos(ImVec2(box0.x, y));
    ImGui::Dummy(ImVec2(0.0f, 0.0f));  // the child's content ends under the last lane
    ImGui::EndChild();
    ImGui::SetCursorScreenPos(box0);
    ImGui::Dummy(ImVec2(box_x1 - box0.x, box_h));
}

// 13-7b (audit C4.7): the fixed "Plays" section of a Random / Blend (also the panel's drop zone):
// the type icon (17 px muted), the type in 15 px semibold and "N children" (" · M missing" in
// --bad); what the type does; Random: Avoid repeating; Blend: the layer view; an empty container's
// warning; "Drop files here to add them as children."
void DrawPlays(const SoundStore& store, const SoundLibrary& lib, const SoundNode& n)
{
    ui::SectionOpts opts;
    opts.foldable = false;
    ui::Section s;
    if (ui::BeginSection(s, "Plays", opts)) {
        const size_t nk = ChildNodes(lib, n).size();
        const int    miss = MissingChildren(lib, n);
        {
            ImDrawList*  dl = ImGui::GetWindowDrawList();
            const ImVec2 p = ImGui::GetCursorScreenPos();
            const float  h = ui::LargeH();
            const float  ip = ui::Dp(17.0f);
            ui::PaintIconAt(dl, TypeIcon(n.type), p.x, p.y, h, ip);
            float        x = p.x + ip + ui::Dp(8.0f);
            ImFont*      tf = ui::FontPaneTitle();
            const char*  tn = TypeName(n.type);
            ui::AddTextIn(dl, tf, ImVec2(x, p.y + std::floor((h - tf->FontSize) * 0.5f) - 1.0f), ui::kText, tn);
            x += ui::TextSizeIn(tf, tn).x + ui::Dp(8.0f);
            const float       ty = p.y + std::floor((h - ImGui::GetTextLineHeight()) * 0.5f);
            const float       right = std::max(x + 1.0f, ui::LineRight());
            const std::string kids = std::to_string(nk) + (nk == 1 ? " child" : " children");
            ui::EllipsisTextTip(dl, ImVec2(x, ty), right, ui::kMuted, kids);
            x += ImGui::CalcTextSize(kids.c_str()).x;
            if (miss > 0 && x < right) {
                const std::string ms = " \xC2\xB7 " + std::to_string(miss) + " missing";
                ui::EllipsisTextTip(dl, ImVec2(x, ty), right, ui::kBad, ms);
            }
            ImGui::Dummy(ImVec2(1.0f, h));
        }
        if (n.type == SoundNodeType::Random) {
            ui::SubText("At each event, plays one of its children, picked at random.");
            DrawRandomAvoid(lib, n);
        } else {
            ui::SubText("At each event, plays all its children together, layered. Drag a bar to offset a layer.");
            DrawBlendView(store, lib, n);
        }
        if (!StoreReplaced()) {
            if (nk == 0) RichText({{"Empty: add sounds, or the library cannot be assigned.", ui::kWarn}});
            ui::SubText("Drop files here to add them as children.");
        }
    }
    ui::EndSection(s);
}

// 13-7b (audit C5.3): the source's state under its waveform -- an item: [Show in REAPER] at the
// right; a file: its name (text) and [Show in Media Explorer] at the right. Missing: the red line
// (#ffc4c4) with [Set from selected item] (an item) or [Locate file...] [Locate in a folder...]
// (a file, DrawLocateButtons, 13-50).
void DrawSourceLine(const SoundLibrary& lib, const SoundNode& n, const SoundMedia& m)
{
    const SoundSource& src = n.sound.source;
    const std::string  lib_uid = lib.uid, uid = n.uid;
    const float        sp = ui::Dp(8.0f);
    if (!m.ok) {
        const bool        item = src.kind == SoundSourceKind::Item;
        const std::string msg = item ? std::string("Its item is no longer on the tracks.")
                                     : (src.asset.last_path.empty() ? std::string("Its file") : src.asset.last_path) + " not found on this PC.";
        if (!item) {  // 13-50: the User Library's Locate pair (name, size and length re-read; the others too)
            FitText(msg, ui::kMissName, ImGui::GetContentRegionAvail().x);
            DrawLocateButtons(lib_uid, uid);
            return;
        }
        const char* btn = "Set from selected item";
        const float bw = ui::ButtonWidth(btn);
        bool        beside = true;
        const float room = flow::StretchBeside(ImGui::GetContentRegionAvail().x, ui::Dp(60.0f), bw, sp, &beside);
        FitText(msg, ui::kMissName, room);
        if (beside) ImGui::SameLine(0.0f, sp);
        if (ui::SolidButton(btn)) ReplaceFromSelected(lib_uid, uid, "Set from selected item");
        return;
    }
    if (src.kind == SoundSourceKind::Item) {
        const float bw = ui::ButtonWidth("Show in REAPER");
        ImGui::SetCursorScreenPos(ImVec2(std::max(ImGui::GetCursorScreenPos().x, ui::LineRight() - bw), ImGui::GetCursorScreenPos().y));
        if (ui::SolidButton("Show in REAPER")) ShowInReaper(src);
        return;
    }
    const char* btn = "Show in Media Explorer";
    const float bw = ui::ButtonWidth(btn);
    bool        beside = true;
    const float room = flow::StretchBeside(ImGui::GetContentRegionAvail().x, ui::Dp(60.0f), bw, sp, &beside);
    FitText(src.asset.name.empty() ? FileName(src.asset.last_path) : src.asset.name, ui::kText, room);
    if (beside) ImGui::SameLine(0.0f, std::max(sp, room - ImGui::GetItemRectSize().x + sp));
    const bool can = static_cast<bool>(P.hooks.show_in_media);
    ImGui::BeginDisabled(!can);
    if (ui::SolidButton(btn) && can) P.hooks.show_in_media(src);
    ImGui::EndDisabled();
    if (!can) ui::ItemTipText(kNoMediaTip);
}

void DrawSoundPanel(const SoundLibrary& lib, const SoundNode& n)
{
    RAV_QUIET_SCOPE("Sound panel", n.name);  // freeze audit
    P.sound_frame = ImGui::GetFrameCount();
    const SoundMedia& m = MediaOf(n);
    const SoundData&  d = Shown(n);
    const std::string lib_uid = lib.uid, uid = n.uid;

    // A new Sound (no frames yet) detects once, when its waveform is read (decision 7).
    if (n.sound.frames.empty() && m.ok && !P.auto_tried.count(uid)) {
        const PeakState st = RequestPeaks(m.path, nullptr);
        if (st != PeakState::Building) {
            P.auto_tried.insert(uid);
            PeakData   pk;
            const bool ready = RequestPeaks(m.path, &pk) == PeakState::Ready;
            const DetectMode mode = DefaultDetectMode(m.facts, ready);
            const std::vector<SoundFrame> frames = DetectFrames(m.facts, mode, n.sound.silence, ready ? &pk : nullptr);
            if (!frames.empty()) {
                const std::string path = m.path;
                EditNode("Detect", lib_uid, uid, [mode, frames, path](SoundNode& x) {
                    x.sound.mode = mode;
                    x.sound.frames = frames;
                    x.sound.edited = false;
                    RefreshWith(x.sound, path);  // 13-25: the sync points measured at once
                    return true;
                });
                return;
            }
        }
    }

    // Source: [Edit] (and "Takes side by side") in the header's right slot.
    const bool  takes = d.mode == DetectMode::Takes && m.take_paths.size() >= 2;
    const char* kTakesNote = "Takes side by side";
    ui::SectionOpts src_opts;
    src_opts.foldable = false;
    src_opts.right_w = (m.ok ? ui::ButtonWidth("Edit") : 0.0f) +
                       (takes ? ui::TextSizeIn(ui::FontSmall(), kTakesNote).x + ImGui::GetStyle().ItemSpacing.x : 0.0f);
    ui::Section src_sec;
    const bool  src_open = ui::BeginSection(src_sec, "Source", src_opts);
    if (src_opts.right_w > 0.0f) {
        ui::BeginSectionHeaderRight(src_sec);
        if (takes) {
            ui::FontScope fs(ui::FontSmall());
            FitText(kTakesNote, ui::kMuted);
            if (m.ok) ImGui::SameLine();
        }
        if (m.ok) {
            if (ui::SolidButton("Edit")) OpenEdit(lib_uid, uid);
            ui::ItemTipText("Edit the variations large (E)");
        }
        ui::EndSectionHeaderRight(src_sec);
    }
    if (src_open) {
        const float wave_h = ui::Dp(96.0f);  // .swave
        if (!m.ok) {
            // .swave.missing: a dashed red box, "Source not found" in its middle.
            const ImVec2 a = ImGui::GetCursorScreenPos();
            const ImVec2 b(std::max(a.x + 8.0f, ui::LineRight()), a.y + wave_h);
            ImDrawList*  dl = ImGui::GetWindowDrawList();
            dl->AddRectFilled(a, b, ui::kBadSoft, ui::kRadiusSm);
            ui::DashedRect(dl, a, b, ui::kBadLine, ui::kRadiusSm);
            const char*  t = "Source not found";
            const ImVec2 ts = ImGui::CalcTextSize(t);
            dl->AddText(ImVec2(std::max(a.x + 4.0f, (a.x + b.x - ts.x) * 0.5f), (a.y + b.y - ts.y) * 0.5f), ui::kMissText, t);
            ImGui::Dummy(ImVec2(b.x - a.x, wave_h));
            // 13-50 review: its audio can still be replaced there (a renamed or re-rendered file, or
            // another sound): a Media Explorer drop and the right-click "Replace with selected items".
            const bool rclick = ImGui::IsItemClicked(ImGuiMouseButton_Right);
            if (MediaDropTarget(ImGui::GetID("##wavedrop"), a, b, lib, n, "Replace", MediaDropKind::Replace)) {
                ui::EndSection(src_sec);
                return;
            }
            if (rclick) ImGui::OpenPopup("##wavemenu");
        } else {
            WaveformEvents ev;
            DrawSoundWaves("panelwave", lib, n, m, wave_h, false, &ev);
            if (StoreReplaced()) {
                ui::EndSection(src_sec);
                return;
            }
            // A Media Explorer drag over the waveform (decision 6: on a Source waveform, it
            // replaces the audio): its own target, so the drop lands on this Sound.
            if (MediaDropTarget(ImGui::GetID("##wavedrop"), ev.min, ev.max, lib, n, "Replace", MediaDropKind::Replace)) {
                ui::EndSection(src_sec);
                return;
            }
            if (ev.right_clicked) ImGui::OpenPopup("##wavemenu");
        }
        if (ui::BeginMenuPopup("##wavemenu")) {
            const bool go = ImGui::MenuItem("Replace with selected items");
            ui::EndMenuPopup();
            if (go) {
                ReplaceFromSelected(lib_uid, uid, "Replace audio");
                ui::EndSection(src_sec);
                return;
            }
        }
        DrawSourceLine(lib, n, m);
        // 13-49 review: what a placed copy leaves out of its source item (read with the media, once per
        // project change): it plays its file plain, as RAV hears it. A folder pile is copied as it is.
        if (m.ok && d.source.kind == SoundSourceKind::Item && !FolderSourced(d)) {
            const std::string drops = PlacementDropsText(m.drops_take_fx, m.drops_pitch, m.drops_rate, m.drops_pan);
            if (!drops.empty()) {
                FitText(drops, ui::kMuted);
                ui::ItemTipText("RAV hears the file itself, so a placed copy plays it plain: the source item's take FX, "
                                "pitch, playrate and pan are not on it (its length follows the rate, so the attack still "
                                "lands on the event). Mono is set in Edit.");
            }
        }
    }
    ui::EndSection(src_sec);
    if (StoreReplaced()) return;

    // Variations (C5.5): "6 variations" in text, "(2 off) — one at random at each event" muted.
    ui::SectionOpts var_opts;
    var_opts.foldable = false;
    ui::Section var_sec;
    if (ui::BeginSection(var_sec, "Variations", var_opts)) {
        const int on = static_cast<int>(ActiveVariations(d).size());
        const int off = static_cast<int>(d.frames.size()) - on;
        std::vector<TextRun> line = {{std::to_string(on) + (on == 1 ? " variation" : " variations"), ui::kText}};
        std::string rest;
        if (off > 0) rest += " (" + std::to_string(off) + " off)";
        if (on > 1) rest += " \xE2\x80\x94 one at random at each event";
        if (!rest.empty()) line.push_back({rest, ui::kMuted});
        RichText(line);
        // 13-46 (indicative only): a divided Sound says so, read-only (the Edit window divides it).
        const std::string divided = DividedByText(GetSoundStore(nullptr), d);
        if (!divided.empty()) {
            FitText(divided, ui::kMuted);
            ui::ItemTipText("Set in the Edit window (E): each event plays the part its role maps to");
        }
        bool linked = n.sound.linked_copies;
        // 13-7 (audit B9): the mockup's .chk -- the box, then its label wrapped within the column;
        // box and label both toggle.
        if (ui::Checkbox("Linked copies replay the same variation##linked", &linked)) {
            EditNode("Linked copies", lib_uid, uid, [linked](SoundNode& x) {
                x.sound.linked_copies = linked;
                return true;
            });
        }
        ui::ItemTipText("Copies of an animation item (linked items) replay the same variation as their source");
        if (!StoreReplaced()) DrawRandomAvoid(lib, n);
        if (!StoreReplaced()) DefaultFadeControls(lib, n);
    }
    ui::EndSection(var_sec);
    if (StoreReplaced()) return;

    SeedSection("Detection", false);
    ui::SectionOpts det_opts;
    det_opts.suffix = "\xC2\xB7 this sound only";
    ui::Section det_sec;
    if (ui::BeginSection(det_sec, "Detection", det_opts) && m.ok) DetectionControls(lib, n, m);
    ui::EndSection(det_sec);
}

void OpenEditKey(const SoundLibrary& lib, const SoundNode& n)
{
    const ImGuiIO& io = ImGui::GetIO();
    if (P.edit_open || !LibraryViewFocused() || io.WantTextInput || io.KeyCtrl || io.KeyAlt || io.KeyShift) return;
    if (ImGui::IsKeyPressed(ImGuiKey_E, false) && MediaOf(n).ok) OpenEdit(lib.uid, n.uid);
}

// A fader typed into whose node is no longer drawn: a click on another tree node took the mouse
// (the tree is drawn before this panel) and the panel moved to that node, or to the batch panel, in
// the same frame, so the fader never saw its end. Its value is entered on its own node, as a click
// elsewhere would (one undo point), and its live state cleared (never shown on another node).
// Only a fader drawn last frame (or this one) counts: one left behind longer (the view left, the
// panel not drawn) is dropped unwritten, never entered minutes later over newer edits.
void SettleLostFaders()
{
    const ImGuiID active = ImGui::GetCurrentContext()->ActiveId;
    const int     frame = ImGui::GetFrameCount();
    // 13-62h review: a number of the zone panels dragged whose row went away mid-gesture (Delete / Heal / a key /
    // Ctrl+Z / another zone selected): its zone and selection are forgotten and its draft dropped (never entered on
    // zones that changed under it); the next gesture starts afresh.
    // 13-62h: a settings row's fader (SettingsNumberRow) whose row went away mid-gesture: a Detection row's moved
    // value drawn last frame is entered on its own node (one undo point), a Settings of row's dropped (below).
    const bool numf_held = P.numf_gid != 0 && active == P.numf_gid;
    if (P.numf_gid != 0 && !numf_held) {
        const bool fresh = P.numf_seen >= frame - 1 && P.numf_moved;
        double SilenceParams::*field = P.detf_field;
        P.numf_gid = 0;
        P.numf_key.clear();
        P.detf_field = nullptr;
        if (field && P.phase.Shows(P.detf_uid)) {
            const SoundLibrary* l = fresh ? FindLibrary(GetSoundStore(nullptr), P.detf_lib) : nullptr;
            const SoundNode*    nd = l ? FindNode(*l, P.detf_uid) : nullptr;
            if (nd && P.draft.silence.*field != nd->sound.silence.*field) CommitLive("Detection", P.detf_lib, P.detf_uid);
            else CancelLive();
            if (StoreReplaced()) return;
        }
    }
    // 13-62h review: a number of the zone panels dragged whose row went away mid-gesture (Delete / Heal / a key /
    // Ctrl+Z / another zone selected): its zone and selection are forgotten and its draft dropped (never entered on
    // zones that changed under it); the next gesture starts afresh. (A settings row's fader held keeps it.)
    if (P.zedit_on && !ui::DragNumberActive() && !numf_held) {
        P.zedit_on = false;
        P.zedit_ref = ZoneRef{};
        P.zedit_sel.clear();
        if (P.phase.Live()) CancelLive();
    }
    if (P.vol_gid != 0 && active != P.vol_gid) {
        const std::string lib_uid = P.vol_lib, uid = P.vol_key;
        const double      v = P.vol_live;
        const bool        fresh = P.vol_seen >= frame - 1;
        P.vol_gid = 0;
        P.vol_key.clear();
        const SoundLibrary* l = (!fresh || uid.empty()) ? nullptr : FindLibrary(GetSoundStore(nullptr), lib_uid);
        const SoundNode*    nd = l ? FindNode(*l, uid) : nullptr;
        if (nd && std::fabs(v - nd->volume_db) > 1e-4)
            EditNode("Volume", lib_uid, uid, [v](SoundNode& x) {
                x.volume_db = v;
                return true;
            });
    }
    // 13-62: the lane header's fader (13-62h: the part's fader) whose header went away mid-gesture (Split -> Combined, Edit
    // closed): a moved value drawn last frame is entered (one undo point), else dropped; never kept for a
    // later click on that lane.
    if (g_lane_gain.gid != 0 && active != g_lane_gain.gid) {
        const LaneGainDrag g = g_lane_gain;
        ClearLaneGain();
        if (P.phase.Shows(g.uid)) {
            const SoundLibrary* l = g.moved && g.seen >= frame - 1 ? FindLibrary(GetSoundStore(nullptr), g.lib) : nullptr;
            const SoundNode*    nd = l ? FindNode(*l, g.uid) : nullptr;
            if (nd && !SameZoneEdit(P.draft, nd->sound)) {  // 13-62h: the part's fader
                const double now = PartFaderDb(P.draft, g.value);
                CommitLive("Part fader", g.lib, g.uid, FaderToast(g.name, now, now - PartFaderDb(nd->sound, g.value)));
            } else {
                CancelLive();
            }
            if (StoreReplaced()) return;
        }
    }
}

void NodePanelWidgets()
{
    P.gen = SoundStoreGeneration();
    SettleLostFaders();
    if (StoreReplaced()) return;
    // A live edit only lasts while its drag does (an unfinished one never shadows the store).
    // (13-14: a fader typed into holds no mouse button either. 13-53: kept on the release frame, whose
    // "done" commits it as the waveform draws.)
    // 13-74: a run of gain notches is entered once the wheel rests (0.5 s), at once when anything else happens: a click,
    // another Edit key, another Sound, the Edit window closed. Until then its draft is held.
    {
        bool other = !P.edit_open || P.trim_request >= 0 || P.zone_request >= 0 || P.listen_request != 0 || P.nav_request != 0 ||
                     ZoneGainUid() != P.edit_uid || ZoneGainUid() != LibraryRowNode();
        for (int b = 0; b < ImGuiMouseButton_COUNT && !other; ++b) other = ImGui::IsMouseClicked(b);
        ZoneGainSettle(other);
    }
    if (P.phase.EndsAtFrameStart(ImGui::IsMouseDown(ImGuiMouseButton_Left), ImGui::IsMouseReleased(ImGuiMouseButton_Left),
                                 ui::DragNumberActive(), ui::FaderTyping() || ZoneGainHolds()))
        CancelLive();
    MergeTreeHooks();
    AuditionPlaying();  // frees an audition that has played to its end
    const SoundStore&   store = GetSoundStore(nullptr);
    const SoundLibrary* lib = FindLibrary(store, SelectedLibraryUid());
    const std::string   uid = lib ? LibraryRowNode() : std::string();
    const SoundNode*    n = lib && !uid.empty() ? FindNode(*lib, uid) : nullptr;
    if (P.phase.Live() && (!n || P.phase.Uid() != uid)) CancelLive();
    if (!n) {
        ImGui::Dummy(ImVec2(0.0f, ui::Dp(4.0f)));
        ImGui::Indent(ui::Dp(6.0f));
        ui::ToneText(lib ? "Select a node of the tree" : "No library", ui::kFaint);
        ImGui::Unindent(ui::Dp(6.0f));
        DrawEditOverlay();
        return;
    }
    // Esc stops / Space plays while column 3 has the focus (the tree has its own).
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows) && !ImGui::GetIO().WantTextInput && !ui::DragNumberActive()) {
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) && AuditionPlaying()) StopPlay();
        else if (ImGui::IsKeyPressed(ImGuiKey_Space, false)) {
            if (NodePlaying(uid)) StopPlay();
            else PlayPanel(*lib, *n);  // 13-40
        }
    }
    ImGui::PushID(uid.c_str());
    ImGui::Dummy(ImVec2(0.0f, ui::Dp(2.0f)));
    DrawHeader(store, *lib, *n);
    // 13-7b (audit C4.2 / C5.2): Type / Volume are plain rows under the header (no "Node" / "Level"
    // card); then Mix (Random, Switch), then Plays (Random, Blend: a Blend's lanes carry its
    // children's volumes, it has no Mix).
    // 13-33: Expand / Collapse all over the panel's sections (Mix, Plays; Source, Variations, Detection),
    // right-aligned above the first one when two or more show. One scope per node type (each type shows
    // its own sections), so selecting another node never blinks the bar.
    const char* fold_scope = n->type == SoundNodeType::Sound    ? "libnode.sound"
                             : n->type == SoundNodeType::Random ? "libnode.random"
                             : n->type == SoundNodeType::Blend  ? "libnode.blend"
                                                                : "libnode.switch";
    ui::BeginFoldScope(fold_scope);
    if (!StoreReplaced() && n->type != SoundNodeType::Sound) {
        DrawTypeRows(store, *lib, *n);
        if (!StoreReplaced()) DrawVolume(*lib, *n);
        if (!StoreReplaced()) ui::FoldScopeBar(fold_scope);
        if (!StoreReplaced() && n->type != SoundNodeType::Blend) DrawMix(store, *lib, *n);
        if (!StoreReplaced() && n->type != SoundNodeType::Switch) DrawPlays(store, *lib, *n);
    } else if (!StoreReplaced()) {
        DrawVolume(*lib, *n);
        if (!StoreReplaced()) {
            OpenEditKey(*lib, *n);
            ui::FoldScopeBar(fold_scope);
            DrawSoundPanel(*lib, *n);
        }
    }
    ui::EndFoldScope(!StoreReplaced());
    // The whole node panel takes a Media Explorer drop too (after the waveform's own target, so
    // the waveform keeps it when the mouse is over it).
    if (!StoreReplaced()) {
        const ImVec2 a = ImGui::GetWindowPos();
        const ImVec2 b(a.x + ImGui::GetWindowSize().x, a.y + ImGui::GetWindowSize().y);
        // The caption says what UseMediaAt does on this node type.
        MediaDropKind kind = MediaDropKind::Use;
        std::string   cap;
        if (n->type == SoundNodeType::Sound) {
            cap = "Drop: becomes a Random of this sound + the new ones";
        } else if (n->type == SoundNodeType::Switch) {
            kind = MediaDropKind::Refused;
            cap = "Pick a value row of this Switch first";  // UseMediaAt's refusal
        } else {
            cap = "Drop: add into " + n->name;
        }
        MediaDropTarget(ImGui::GetID("##paneldrop"), ImVec2(a.x + 2.0f, a.y + 2.0f), ImVec2(b.x - 2.0f, b.y - 2.0f), *lib, *n,
                        cap.c_str(), kind);
    }
    ImGui::PopID();
    if (!StoreReplaced()) DrawEditOverlay();
}

}  // namespace

// The main menu's Library tab: the Sound Edit options (the same rows as the Edit window's ⚙ Options popup).
void DrawSoundEditMenuSection()
{
    static ui::Section sec;
    if (ui::BeginSection(sec, "Sound Edit")) {
        SoundEditOptionItems();
        ImGui::Dummy(ImVec2(1.0f, ui::Dp(2.0f)));
        ui::ToneText("Your preferences, for every project. Not part of the sound: no undo point.", ui::kFaint, true);
    }
    ui::EndSection(sec);
}

void DrawNodePanel()
{
    // 13-62i: a waveform press released while that waveform was not drawn is forgotten (a later draw never takes it
    // for a click).
    if (P.wave_press_id != 0 && !ImGui::IsMouseDown(ImGuiMouseButton_Left) && !ImGui::IsMouseReleased(ImGuiMouseButton_Left))
        P.wave_press_id = 0;
    NodePanelWidgets();
    g_picks.Run();  // the gestures picked above, the panel and the Edit overlay drawn
}

void SettleNodePanelFaders()
{
    P.gen = SoundStoreGeneration();
    SettleLostFaders();
    g_picks.Run();
}

}  // namespace rav

#endif  // _WIN32
