// SPDX-License-Identifier: MIT
//
// See zone_wave_ui.h. The mockup's drawLane / drawRuler / drawRib / drawVars / drawOverlays and its pointer,
// menu and key handlers, in Dear ImGui (ImDrawList), DM-XYZ-Pad theme (ui_theme.h).

#include "zone_wave_ui.h"

#ifdef _WIN32

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "library_view_ui.h"  // ShowLibraryToast
#include "shortcuts.h"        // LoadPrefBool, ShortcutKeyLabel, kShortcutEdit*
#include "sound_audition.h"
#include "sound_entries.h"
#include "sound_peaks.h"
#include "sound_project.h"  // SoundStoreGeneration
#include "ui_flow.h"
#include "ui_theme.h"
#include "wave_draw_ui.h"  // 13-71: the one waveform drawer
#include "wave_tiles_plan.h"

namespace rav {
namespace {

constexpr const char kClickPlaysPref[] = "lib_edit_click_plays";
constexpr const char kTailWinsPref[] = "lib_edit_tail_wins";
int g_click_plays = -1, g_tail_wins = -1;  // < 0 = not read yet
constexpr const char kSnapModePref[] = "lib_edit_snap_mode";  // 13-67b: the snap offset mode of new Sounds
int g_snap_mode = -1;                                       // < 0 = not read yet

constexpr ImU32 kGapCol = IM_COL32(0x8B, 0x91, 0x9C, 0xFF);  // the mockup's GAPC (--muted)

// ---- small helpers ------------------------------------------------------------------------------------------

ImU32 Alpha(ImU32 c, float a)
{
    return ui::WithAlpha(c, static_cast<int>(std::lround(std::clamp(a, 0.0f, 1.0f) * 255.0f)));
}

double Clamp(double v, double lo, double hi) { return std::max(lo, std::min(hi, v)); }

int Ms(double t) { return static_cast<int>(std::lround(t * 1000.0)); }

std::string MsText(double t) { return std::to_string(Ms(t)) + " ms"; }

std::string Signed(double v, const char* fmt = "%.1f")
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), fmt, v);
    return (v > 0.0 ? "+" : "") + std::string(buf);
}

void DashedV(ImDrawList* dl, float x, float y0, float y1, ImU32 col, float dash, float gap, float th = 1.0f)
{
    for (float y = y0; y < y1; y += dash + gap) dl->AddLine(ImVec2(x, y), ImVec2(x, std::min(y + dash, y1)), col, th);
}

void DashedH(ImDrawList* dl, float x0, float x1, float y, ImU32 col, float dash, float gap, float th = 1.0f)
{
    for (float x = x0; x < x1; x += dash + gap) dl->AddLine(ImVec2(x, y), ImVec2(std::min(x + dash, x1), y), col, th);
}

void DashedRect(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 col, float dash, float gap, float th = 1.0f)
{
    DashedH(dl, a.x, b.x, a.y, col, dash, gap, th);
    DashedH(dl, a.x, b.x, b.y, col, dash, gap, th);
    DashedV(dl, a.x, a.y, b.y, col, dash, gap, th);
    DashedV(dl, b.x, a.y, b.y, col, dash, gap, th);
}

void HatchRect(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 col)
{
    if (b.x <= a.x || b.y <= a.y) return;
    dl->PushClipRect(a, b, true);
    const float H = b.y - a.y;
    for (float x = a.x - H; x < b.x; x += 6.0f) dl->AddLine(ImVec2(x, b.y), ImVec2(x + H, a.y), col);
    dl->PopClipRect();
}

// Text whose baseline is at y (the mockup's fillText).
void TextAt(ImDrawList* dl, ImFont* f, float x, float baseline, ImU32 col, const std::string& s)
{
    const float fs = f ? f->FontSize : ImGui::GetFontSize();
    ui::AddTextIn(dl, f, ImVec2(x, std::floor(baseline - fs * 0.85f)), col, s.c_str());
}

float TextW(ImFont* f, const std::string& s) { return ui::TextSizeIn(f, s.c_str()).x; }

// The file's peaks: copied once when they become Ready (or the file they were read from changed), then kept; each
// frame only asks whether they still are. `rev` moves with each copy (the looks' cache key). 13-62i: one per file (a
// Takes-mode Sound draws a host per take, each on its own file).
struct PeakCache {
    int         frame = -1;
    std::string path;
    bool        ok = false;
    FileVersion file;
    PeakData    pk;
    uint64_t    rev = 0;
};
std::map<std::string, PeakCache> g_pks;

PeakCache& PeakCacheOf(const std::string& path)
{
    // Up to 64 files kept; past that, the one read longest ago goes (never one asked this frame: a pointer to its
    // peaks may be held for this frame's drawing).
    if (g_pks.size() >= 64 && g_pks.find(path) == g_pks.end()) {
        const int f   = ImGui::GetFrameCount();
        auto      old = g_pks.end();
        for (auto it = g_pks.begin(); it != g_pks.end(); ++it)
            if (it->second.frame != f && (old == g_pks.end() || it->second.frame < old->second.frame)) old = it;
        if (old != g_pks.end()) g_pks.erase(old);
    }
    return g_pks[path];
}

const PeakView* PeaksOf(const std::string& path)
{
    PeakCache& g_pk = PeakCacheOf(path);
    const int  f = ImGui::GetFrameCount();
    if (g_pk.frame != f || g_pk.path != path) {
        g_pk.frame = f;
        const bool ready = !path.empty() && RequestPeaks(path, nullptr) == PeakState::Ready;
        if (!ready) {
            if (g_pk.ok || g_pk.path != path) ++g_pk.rev;
            g_pk.ok = false;
            g_pk.path = path;
        } else {
            const FileVersion fv = PeaksFileVersion(path);
            if (!g_pk.ok || g_pk.path != path || fv != g_pk.file) {
                g_pk.ok = RequestPeaks(path, &g_pk.pk) == PeakState::Ready;
                g_pk.file = fv;
                g_pk.path = path;
                ++g_pk.rev;
            }
        }
    }
    return g_pk.ok ? &g_pk.pk : nullptr;
}

ZoneContext Ctx(const ZoneWaveHost& h) { return ZoneContext{PeaksOf(h.path), h.file_len, h.tl}; }

// 13-62i: the host is the one the edit cursor, the time selection and the keys belong to (a Takes-mode Sound's
// active take; any other Sound has one host).
bool ActiveHost(const ZoneWaveHost& h) { return h.tl < 0 || h.view->cur_tl == h.tl; }

// 13-62i: a press in a take's host makes it the active one (the time selection of another take goes).
void Activate(const ZoneWaveHost& h)
{
    ZoneEditView& v = *h.view;
    if (h.tl < 0 || v.cur_tl == h.tl) return;
    v.cur_tl = h.tl;
    v.has_tsel = false;
}

const SoundData& Shown(const ZoneWaveHost& h) { return h.shown(); }
const SoundData& Stored(const ZoneWaveHost& h) { return h.node->sound; }

std::string PartNameOf(const ZoneWaveHost& h, const std::string& part)
{
    const int k = PartIndex(Stored(h), part);
    return k >= 0 && static_cast<size_t>(k) < h.names.size() ? h.names[static_cast<size_t>(k)] : std::string("?");
}

ImU32 PartColOf(const ZoneWaveHost& h, const SoundData& d, const std::string& part)
{
    const int k = PartIndex(d, part);
    return k >= 0 && static_cast<size_t>(k) < h.colors.size() ? h.colors[static_cast<size_t>(k)] : ui::kAccent;
}

std::string NameOfZone(const ZoneWaveHost& h, const SoundData& d, const ZoneRef& r) { return ZoneName(d, r, h.names); }

bool PartHeardNow(const ZoneWaveHost& h, const std::string& part)
{
    const bool solo_on = h.solo && !h.solo->empty();
    return !(h.mute && h.mute->count(part)) && (!solo_on || h.solo->count(part));
}

std::string KeyHint(int id, const char* fallback)
{
    const char* k = ShortcutKeyLabel(id);
    return k && k[0] ? std::string(k) : std::string(fallback);
}

// ---- writes ---------------------------------------------------------------------------------------------------

// One of our writes is on its way: the store generation it moves is not an outside change (FollowOutside).
void MarkOwn(ZoneEditView& v)
{
    v.own_pending = true;
    v.own_frame = ImGui::GetFrameCount();
}

void Commit(const ZoneWaveHost& h, const std::string& name, const std::string& toast)
{
    MarkOwn(*h.view);
    h.commit(name, toast);
}

// One zone gesture: f(data, ctx, sel_out) -> changed. It runs first on a copy of the stored Sound (nothing changed:
// false, no undo point; the caller toasts why), then once as the write (one undo point). The selection follows: the
// gesture's own (`own_sel`) or the zones carried over (SelFollow).
using ZoneFn = std::function<bool(SoundData&, const ZoneContext&, ZoneSel*)>;
using ToastFn = std::function<std::string(const SoundData& after, const ZoneSel& out)>;

bool Write(const ZoneWaveHost& h, const std::string& name, const ZoneFn& f, const ToastFn& toast, bool own_sel)
{
    const SoundData& st = Stored(h);
    SoundData        probe = st;
    ZoneEditView&    v = *h.view;
    ZoneSel          out = v.sel;  // in / out: the gestures re-resolve it (or put their new zone there)
    if (!f(probe, Ctx(h), &out)) return false;
    v.sel = own_sel ? SelValid(probe, out) : SelFollow(st, v.sel, probe);
    const std::string msg = toast ? toast(probe, out) : name;
    const std::string path = h.path;
    const double      len = h.file_len;
    const int         tl = h.tl;
    MarkOwn(v);
    h.edit(name, [f, path, len, tl](SoundData& x) {
        PeakData   pk;
        const bool has = !path.empty() && RequestPeaks(path, &pk) == PeakState::Ready;
        ZoneSel    s;
        return f(x, ZoneContext{has ? &pk : nullptr, len, tl}, &s);
    }, msg);
    return true;
}

bool Write(const ZoneWaveHost& h, const std::string& name, const ZoneFn& f, const std::string& toast, bool own_sel = false)
{
    return Write(h, name, f, [toast](const SoundData&, const ZoneSel&) { return toast; }, own_sel);
}

// ---- playback -------------------------------------------------------------------------------------------------

std::string PlayTag(const ZoneWaveHost& h) { return "zone:" + h.node->uid; }

// Plays [from, to] as heard (EditPlayFrom: Gaps jumped, empty space silent, Mute / Solo); `tp` = its transport.
bool StartPlay(const ZoneWaveHost& h, const EditTransport& tp, double from, double to, bool quiet = false)
{
    ZoneEditView& v = *h.view;
    SoundNode     shown = *h.node;
    shown.sound = Shown(h);
    const PartListen listen{h.mute ? *h.mute : std::set<std::string>{}, h.solo ? *h.solo : std::set<std::string>{}};
    const EditPlay   ep = EditPlayFrom(*h.lib, shown, from, to, &listen, h.tl);
    std::vector<PlayVoice> pv;
    for (const AuditionVoice& a : ep.voices) {
        if (!(a.length_s > 0.0)) continue;
        PlayVoice p;
        p.path = h.path;
        p.start_s = a.start_s;
        p.length_s = a.length_s;
        p.delay_s = a.delay_ms / 1000.0;
        p.gain = std::pow(10.0, a.gain_db / 20.0);
        p.fade_in_s = a.fade_in_s;
        p.fade_out_s = a.fade_out_s;
        p.fade_in_shape = a.fade_in_shape;
        p.fade_out_shape = a.fade_out_shape;
        p.fade_from_s = a.fade_from_s;
        p.fade_span_s = a.fade_span_s;
        p.mono = a.mono;
        pv.push_back(p);
    }
    if (pv.empty()) {
        v.playing = false;
        if (!quiet)
            ShowLibraryToast(listen.Active() && !EditPlayFrom(*h.lib, shown, from, to, nullptr, h.tl).voices.empty()
                                 ? "Nothing to play: every part here is muted / not soloed"
                                 : "Nothing placed here: nothing to play",
                             false);
        return false;
    }
    if (!PlayVoices(pv, PlayTag(h))) {
        v.playing = false;
        if (!quiet) ShowLibraryToast("Cannot play: the sound file is not read", false);
        return false;
    }
    v.playing = true;
    v.last_pos = 0.0;
    v.last_seen_t = ImGui::GetTime();  // 13-67a
    v.play_uid = h.node->uid;
    v.play_tl = h.tl;  // 13-62i: its take's host draws the playheads
    v.tp = tp;
    v.map = ep.map;
    return true;
}

// The mockup's playFrom: the cursor goes to `from`; a time selection keeps the playhead inside it; Loop repeats.
void PlayRange(const ZoneWaveHost& h, const EditRange& r)
{
    if (!r.ok) return;
    const ZoneEditView& v = *h.view;
    const EditTransport tp = PlanEditPlay(r.from, r.to, v.has_tsel, v.ts_a, v.ts_b, v.loop, h.file_len);
    *h.cursor = tp.cursor;
    StartPlay(h, tp, tp.from, tp.to);
}

// ---- view helpers -----------------------------------------------------------------------------------------------

void SetTsel(const ZoneWaveHost& h, double a, double b, const std::string& toast)
{
    ZoneEditView& v = *h.view;
    if (b < a) std::swap(a, b);
    if (b - a <= 0.002) return;  // too short: the time selection there stays as it was
    v.has_tsel = true;
    v.ts_a = a;
    v.ts_b = b;
    *h.cursor = a;  // Antho 2026-10-14: the cursor goes to its start
    if (!toast.empty()) ShowLibraryToast(toast, false);
}

// A variation's zones selected (Ctrl / Shift adds them).
void SelectVariation(const ZoneWaveHost& h, int frame, bool add)
{
    ZoneEditView& v = *h.view;
    const ZoneSel all = ZonesOfFrame(Shown(h), frame);
    if (!add) v.sel.clear();
    for (const ZoneRef& r : all)
        if (!SelHas(v.sel, r)) v.sel.push_back(r);
    const ZoneRef   first = FirstZoneOf(Shown(h), frame);
    const EditZone* fz = ZoneOf(Shown(h), first);
    if (fz && !fz->IsGap()) *h.part_sel = fz->part;
}

double ViewLen(const ZoneWaveHost& h) { return *h.view_len > 0.0 ? *h.view_len : h.file_len; }

// ---- clicks ---------------------------------------------------------------------------------------------------

void GiveOne(const ZoneWaveHost& h, const ZoneRef& r, const std::string& part, const std::string& why)
{
    const SoundData& d = Shown(h);
    const EditZone*  z = ZoneOf(d, r);
    if (!z || z->IsGap()) return;
    if (z->part == part) {
        ShowLibraryToast("Already " + PartNameOf(h, part), false);
        return;
    }
    const std::string from = PartNameOf(h, z->part), to = PartNameOf(h, part);
    const std::string toast = ZoneRangeMs(z->a, z->b) + ": " + from + " \xE2\x86\x92 " + to + (why.empty() ? "" : " " + why);
    const ZoneSel     one = {r};
    h.view->sel = one;
    *h.part_sel = part;
    Write(h, "Zone to part", [one, part](SoundData& x, const ZoneContext& c, ZoneSel* s) { return GiveZonesPart(x, one, part, c, s); }, toast, true);
}

// The mockup's clickHit.
void Click(const ZoneWaveHost& h, const ZoneHit& hit, bool add)
{
    ZoneEditView&    v = *h.view;
    const SoundData& d = Shown(h);
    if (hit.row == ZoneRow::Ruler) {  // the cursor (the time selection stays; double-click removes it)
        *h.cursor = hit.t;
        return;
    }
    if (hit.row == ZoneRow::Vars) {
        if (hit.kind == ZoneHitKind::Var) {
            SelectVariation(h, hit.var, add);
            const size_t n = ZonesOfFrame(d, hit.var).size();
            ShowLibraryToast("Variation " + std::to_string(hit.var + 1) + ": its " + std::to_string(n) +
                                 " zones selected (H heals them, Delete removes them)",
                             false);
        } else if (!add) {
            v.sel.clear();
        }
        return;
    }
    *h.cursor = hit.t;
    const EditZone* z = ZoneOf(d, hit.z);
    if (!z) {
        if (!add) v.sel.clear();  // empty space: nothing selected (the time selection stays)
        return;
    }
    // 13-62h (Antho 2026-10-15, v5): zones joined by Gaps are one item: a click on any of its zones or its Gap
    // selects it whole (Ctrl / Shift: adds or removes it whole). A greyed zone of a lane still goes alone to the lane.
    const ZoneSel item = ItemOf(d, hit.z);
    if (add) {  // REAPER: Ctrl+click adds the zone to the selection, or takes it out
        if (!item.empty()) SelToggleItem(v.sel, item, hit.z);
        else SelToggle(v.sel, hit.z);
        return;
    }
    if (hit.kind == ZoneHitKind::Grey && hit.lane >= 0 && static_cast<size_t>(hit.lane) < d.divide_values.size()) {
        GiveOne(h, hit.z, d.divide_values[static_cast<size_t>(hit.lane)],
                "(its copy on " + PartNameOf(h, z->part) + "'s lane is now greyed)");
        return;
    }
    if (!item.empty()) {
        v.sel = item;
        *h.part_sel = ZoneOf(d, item.front())->part;
    } else {
        v.sel = {hit.z};
        if (!z->IsGap()) *h.part_sel = z->part;
    }
    if (hit.row == ZoneRow::Placed) return;  // the Placed strip: click selects
    if (ZoneClickPlays()) {
        // 13-67a (Antho 2026-10-17): the click's intent is "hear this": the time selection goes (like REAPER), so the
        // clicked item plays instead of the time selection.
        v.has_tsel = false;
        PlayRange(h, ClickPlayRange(d, hit.z, hit.t, h.file_len));
    }
}

// The mockup's onDouble.
void Double(const ZoneWaveHost& h, const ZoneHit& hit)
{
    ZoneEditView&    v = *h.view;
    const SoundData& d = Shown(h);
    const EditZone*  z = ZoneOf(d, hit.z);
    const std::string nm = NameOfZone(h, d, hit.z);
    switch (DoubleActionOf(d, hit)) {
    case ZoneDoubleAction::RemoveTsel:
        if (v.has_tsel) {
            v.has_tsel = false;
            ShowLibraryToast("Time selection removed", false);
        }
        return;
    case ZoneDoubleAction::TselVariation: {
        SelectVariation(h, hit.var, false);
        double a = 0.0, b = 0.0;
        if (VariationEdges(d, hit.var, &a, &b))
            SetTsel(h, a, b, "Variation " + std::to_string(hit.var + 1) + ": time selection " + ZoneRangeMs(a, b) +
                                 ", the cursor at its start");
        return;
    }
    case ZoneDoubleAction::TselZone:
        if (!z) return;
        v.sel = {hit.z};
        if (!z->IsGap()) *h.part_sel = z->part;
        SetTsel(h, z->a, z->b, nm + ": time selection " + ZoneRangeMs(z->a, z->b) + ", the cursor at its start");
        return;
    case ZoneDoubleAction::TselItem: {  // 13-62h (v5): the item whole, a time selection over it, the cursor at its start
        const ZoneSel item = ItemOf(d, hit.z);
        double        a = 0.0, b = 0.0;
        if (!ItemEdges(d, item, &a, &b)) return;
        v.sel = item;
        *h.part_sel = ZoneOf(d, item.front())->part;
        SetTsel(h, a, b, ItemNames(d, item, h.names) + " (glued): time selection " + ZoneRangeMs(a, b) + ", the cursor at its start");
        return;
    }
    case ZoneDoubleAction::ResetEdge: {
        if (!z) return;
        const ZoneRef r = hit.z;
        const int     side = hit.side;
        if (!Write(h, "Zone edge automatic", [r, side](SoundData& x, const ZoneContext& c, ZoneSel* s) { return ResetZoneEdge(x, r, side, c, s); },
                   nm + ": " + (side < 0 ? "start follows Pre-roll" : "end follows Tail") + " again", true))
            ShowLibraryToast("Already automatic", false);
        return;
    }
    case ZoneDoubleAction::ResetSnap: {
        const ZoneRef r = hit.z;
        if (!Write(h, "Snap offset automatic", [r](SoundData& x, const ZoneContext& c, ZoneSel* s) { return ResetZoneSnap(x, r, c, s); },
                   nm + " snap offset: automatic again (its part's Snap offset)", true))
            ShowLibraryToast("Already automatic", false);
        return;
    }
    case ZoneDoubleAction::ResetFade: {
        const ZoneRef r = hit.z;
        const int     side = hit.side;
        if (!Write(h, "Zone fade default", [r, side](SoundData& x, const ZoneContext& c, ZoneSel* s) { return ResetZoneFade(x, r, side, c, s); },
                   nm + " fade " + (side < 0 ? "in" : "out") + ": its default again", true))
            ShowLibraryToast("Already its default", false);
        return;
    }
    case ZoneDoubleAction::None: return;
    }
}

// ---- the rows ---------------------------------------------------------------------------------------------------

struct Row {
    ZoneRow row = ZoneRow::Lane;
    int     lane = -1;
    float   y0 = 0.0f, y1 = 0.0f;
};

double EnvOf(const ZoneLook& z, double t)
{
    if (z.gap || t < z.lo || t >= z.hi) return 0.0;
    double g = 1.0;
    if (z.fin > 0.0 && t < z.lo + z.fin) g *= (t - z.lo) / z.fin;
    if (z.fout > 0.0 && t > z.hi - z.fout) g *= (z.hi - t) / z.fout;
    return Clamp(g, 0.0, 1.0);
}

// The looks of the Sound shown and its Variations strip, computed once per change: the store's generation, the
// peaks copied, the Sound; while a live draft shows (a drag) at each call.
struct LookCache {
    bool                      valid = false, live = false;
    std::string               uid;
    uint64_t                  gen = 0, peaks_rev = 0;
    std::vector<ZoneLook>     looks;
    std::vector<ZoneVarBlock> blocks;
    std::vector<ZoneSel>      by_frame;  // each frame's zones (time order)
};
std::map<int, LookCache> g_lcs;  // 13-62i: per timeline (a Takes-mode Sound's take)

const LookCache& LooksNow(const ZoneWaveHost& h)
{
    const PeakView*  pk = PeaksOf(h.path);
    const uint64_t   rev = PeakCacheOf(h.path).rev;
    const bool       live = h.live_now();
    const uint64_t   gen = SoundStoreGeneration();
    LookCache&       g_lc = g_lcs[h.tl];
    if (g_lc.valid && !live && !g_lc.live && g_lc.uid == h.node->uid && g_lc.gen == gen && g_lc.peaks_rev == rev)
        return g_lc;
    const SoundData& d = Shown(h);
    const int        tl = h.tl < 0 ? 0 : h.tl;  // 13-62i: the host's timeline (0 but a take's)
    g_lc.looks = ZoneLooks(d, pk, tl);
    g_lc.blocks = ZoneVarBlocks(d, tl);
    g_lc.by_frame.assign(d.frames.size(), ZoneSel{});
    for (const ZoneLook& L : g_lc.looks)
        if (L.ref.frame >= 0 && static_cast<size_t>(L.ref.frame) < g_lc.by_frame.size()) g_lc.by_frame[static_cast<size_t>(L.ref.frame)].push_back(L.ref);
    g_lc.valid = true;
    g_lc.live = live;
    g_lc.uid = h.node->uid;
    g_lc.gen = gen;
    g_lc.peaks_rev = rev;
    return g_lc;
}

// Everything one frame of drawing needs.
struct Frame {
    const ZoneWaveHost*   h = nullptr;
    const SoundData*      d = nullptr;
    ImDrawList*           dl = nullptr;
    float                 cx0 = 0.0f, cw = 1.0f;  // the canvas' left edge and width
    double                v0 = 0.0, vl = 1.0;
    const LookCache*      lc = nullptr;
    std::vector<HeardHead> heads;
    float                 dp = 1.0f;
    const PeakView*       peaks = nullptr;
    WaveColumns           wc;
    bool                  have_cols = false;
    const std::vector<ZoneLook>& Looks() const { return lc->looks; }
    float X(double t) const { return cx0 + static_cast<float>((t - v0) / vl * cw); }
    double T(float x) const { return v0 + static_cast<double>(x - cx0) / cw * vl; }
};

// The time selection, the edit cursor and the playheads over a row (drawOverlays). `thin`: a strip.
void DrawOverlays(const Frame& F, float y0, float y1, bool thin)
{
    const ZoneWaveHost& h = *F.h;
    const ZoneEditView& v = *h.view;
    ImDrawList*         dl = F.dl;
    const bool          act = ActiveHost(h);  // 13-62i: another take's host draws no cursor / time selection
    bool                has = v.has_tsel && act;
    double              a = v.ts_a, b = v.ts_b;
    if (v.drag.kind == ZoneDragKind::Tsel) {
        has = true;
        a = std::min(v.drag.t0, v.drag.t1);
        b = std::max(v.drag.t0, v.drag.t1);
    }
    if (has) {
        dl->AddRectFilled(ImVec2(F.X(a), y0), ImVec2(F.X(b), y1), IM_COL32(231, 233, 238, 26));
        dl->AddLine(ImVec2(std::round(F.X(a)) + 0.5f, y0), ImVec2(std::round(F.X(a)) + 0.5f, y1), IM_COL32(231, 233, 238, 115));
        dl->AddLine(ImVec2(std::round(F.X(b)) - 0.5f, y0), ImVec2(std::round(F.X(b)) - 0.5f, y1), IM_COL32(231, 233, 238, 115));
    }
    if (*h.cursor >= 0.0 && act) {
        const float x = std::round(F.X(*h.cursor)) + 0.5f;
        dl->AddLine(ImVec2(x, y0), ImVec2(x, y1), ui::kAccent, v.drag.kind == ZoneDragKind::Scrub ? 2.0f : 1.0f);
    }
    // Two playheads during a crossfade: the outgoing one solid and fading, the incoming one dashed and rising.
    const size_t n = F.heads.size();
    for (const HeardHead& hd : F.heads) {
        const float x = std::round(F.X(hd.file_t)) + 0.5f;
        const float al = n > 1 ? 0.35f + 0.65f * static_cast<float>(hd.gain) : 1.0f;
        const ImU32 col = Alpha(hd.incoming ? ui::kWarn : ui::kWhite, al);
        const float th = thin ? 1.0f : 2.0f;
        if (hd.incoming) DashedV(dl, x, y0, y1, col, 4.0f, 3.0f, th);
        else dl->AddLine(ImVec2(x, y0), ImVec2(x, y1), col, th);
        if (!thin && n > 1) {
            char lbl[32];
            std::snprintf(lbl, sizeof(lbl), "%s %d%%", hd.incoming ? "in" : "out", static_cast<int>(std::lround(hd.gain * 100.0)));
            ImFont*     f = ui::FontCaptionBold();
            const float w = TextW(f, lbl) + 6.0f * F.dp;
            const float ly = hd.incoming ? y1 - 34.0f * F.dp : y0 + 22.0f * F.dp;
            const float lx = hd.incoming ? x + 3.0f * F.dp : x - w - 3.0f * F.dp;
            dl->AddRectFilled(ImVec2(lx, ly), ImVec2(lx + w, ly + 14.0f * F.dp), IM_COL32(18, 19, 23, 230));
            ui::AddTextIn(dl, f, ImVec2(lx + 3.0f * F.dp, ly + 1.0f), hd.incoming ? ui::kWarn : ui::kWhite, lbl);
        }
    }
}

void DrawRuler(const Frame& F, float y0, float y1)
{
    ImDrawList* dl = F.dl;
    dl->AddRectFilled(ImVec2(F.cx0, y0), ImVec2(F.cx0 + F.cw, y1), ui::kSurface);
    static const double kSteps[] = {0.005, 0.01, 0.02, 0.05, 0.1, 0.2, 0.5, 1, 2, 5, 10, 20, 30, 60, 120, 300, 600};
    double              st = kSteps[sizeof(kSteps) / sizeof(kSteps[0]) - 1];
    for (double s : kSteps)
        if (s / F.vl * F.cw >= 64.0f * F.dp) {
            st = s;
            break;
        }
    ImFont*      f = ui::FontMonoTiny();
    const double v1 = F.v0 + F.vl;
    for (double t = std::ceil(F.v0 / st) * st; t <= v1 + 1e-9; t += st) {
        const float x = std::round(F.X(t)) + 0.5f;
        dl->AddLine(ImVec2(x, y1 - 6.0f * F.dp), ImVec2(x, y1), ui::kStrokeStrong);
        char lbl[32];
        if (st < 1.0) std::snprintf(lbl, sizeof(lbl), "%d ms", Ms(t));
        else std::snprintf(lbl, sizeof(lbl), "%g s", std::round(t * 100.0) / 100.0);
        TextAt(dl, f, x + 3.0f * F.dp, y1 - 7.0f * F.dp, ui::kFaint, lbl);
    }
    const double cur = *F.h->cursor;
    if (cur >= 0.0 && ActiveHost(*F.h)) {
        const float x = F.X(cur), s = 5.0f * F.dp;
        dl->AddTriangleFilled(ImVec2(x - s, y0), ImVec2(x + s, y0), ImVec2(x, y0 + 7.0f * F.dp), ui::kAccent);
    }
    DrawOverlays(F, y0, y1, true);
}

void DrawVars(const Frame& F, float y0, float y1)
{
    ImDrawList*         dl = F.dl;
    const ZoneEditView& v = *F.h->view;
    dl->AddRectFilled(ImVec2(F.cx0, y0), ImVec2(F.cx0 + F.cw, y1), ui::kSurface);
    dl->AddLine(ImVec2(F.cx0, y0 + 0.5f), ImVec2(F.cx0 + F.cw, y0 + 0.5f), ui::kStroke);
    ImFont* f = ui::FontCaptionBold();
    int     i = 0;
    for (const ZoneVarBlock& b : F.lc->blocks) {
        ++i;
        const float xa = F.X(b.a), xb = F.X(b.b);
        if (xb < F.cx0 || xa > F.cx0 + F.cw) continue;
        static const ZoneSel kNone;
        const ZoneSel& zs = b.frame >= 0 && static_cast<size_t>(b.frame) < F.lc->by_frame.size() ? F.lc->by_frame[static_cast<size_t>(b.frame)] : kNone;
        bool          all = !zs.empty();
        for (const ZoneRef& r : zs) all = all && SelHas(v.sel, r);
        const bool hot = v.hover_on && v.hover.row == ZoneRow::Vars && v.hover.kind == ZoneHitKind::Var && v.hover.var == b.frame;
        dl->AddRectFilled(ImVec2(xa + 0.5f, y0 + 2.0f * F.dp), ImVec2(xb - 0.5f, y1 - 2.0f * F.dp),
                          all ? Alpha(ui::kAccent, 0.32f) : hot ? IM_COL32(255, 255, 255, 31) : IM_COL32(255, 255, 255, 15));
        dl->AddRect(ImVec2(std::round(xa) + 0.5f, y0 + 2.5f * F.dp), ImVec2(std::round(xb) - 0.5f, y1 - 2.5f * F.dp),
                    all ? ui::kAccent : ui::kStrokeStrong);
        const std::string full = ZoneVarLabel(b.frame + 1, b.a, b.b), num = std::to_string(b.frame + 1);
        const std::string lbl = TextW(f, full) + 10.0f * F.dp < xb - xa ? full : num;
        if (TextW(f, lbl) + 6.0f * F.dp < xb - xa)
            TextAt(dl, f, std::max(xa, F.cx0) + 5.0f * F.dp, y1 - 6.0f * F.dp, all ? ui::kText : ui::kMuted, lbl);
    }
    (void)i;
    DrawOverlays(F, y0, y1, true);
}

void DrawPlaced(const Frame& F, float y0, float y1)
{
    ImDrawList*         dl = F.dl;
    const ZoneWaveHost& h = *F.h;
    dl->AddRectFilled(ImVec2(F.cx0, y0), ImVec2(F.cx0 + F.cw, y1), ui::kSurface);
    for (const ZoneLook& z : F.Looks()) {
        const float xa = F.X(z.a), xb = F.X(z.b);
        if (xb < F.cx0 || xa > F.cx0 + F.cw) continue;
        if (z.gap) {
            dl->AddRectFilled(ImVec2(xa, y0 + 1.0f), ImVec2(xb, y1 - 1.0f), ui::kSurface);
            HatchRect(dl, ImVec2(xa, y0 + 1.0f), ImVec2(xb, y1 - 1.0f), Alpha(kGapCol, 0.9f));
        } else {
            const std::string& part = ZoneOf(*F.d, z.ref)->part;
            const bool         lit = h.part_sel->empty() || *h.part_sel == part;
            dl->AddRectFilled(ImVec2(xa + 0.5f, y0 + 1.0f), ImVec2(std::max(xa + 1.5f, xb - 0.5f), y1 - 1.0f),
                              Alpha(PartColOf(h, *F.d, part), lit ? 1.0f : 0.4f));
        }
        if (SelHas(h.view->sel, z.ref)) dl->AddRect(ImVec2(xa + 1.0f, y0 + 1.0f), ImVec2(xb - 1.0f, y1 - 1.0f), ui::kWhite);
    }
    DrawOverlays(F, y0, y1, true);
}

// A Gap's two neighbours' looks (null when missing).
std::array<const ZoneLook*, 2> GapSides(const Frame& F, const ZoneLook& g)
{
    const std::vector<ZoneLook>& L = F.Looks();
    auto at = [&](int i) { return i >= 0 && static_cast<size_t>(i) < L.size() ? &L[static_cast<size_t>(i)] : nullptr; };
    return {at(g.li), at(g.ri)};
}

bool Mine(const ZoneLook& z, int lane)
{
    return lane < 0 || z.gap || z.k == lane;
}

// A lane (lane -1 = Combined): the mockup's drawLane.
void DrawLane(const Frame& F, const Row& row)
{
    const ZoneWaveHost& h = *F.h;
    const ZoneEditView& v = *h.view;
    const SoundData&    d = *F.d;
    ImDrawList*         dl = F.dl;
    const float         dp = F.dp, H = row.y1 - row.y0;
    const float         y0 = row.y0 + 3.0f * dp, y1 = row.y1 - 3.0f * dp, mid = row.y0 + H * 0.5f;
    const int           lane = row.lane;
    const float         left = F.cx0, right = F.cx0 + F.cw;
    dl->PushClipRect(ImVec2(left, row.y0), ImVec2(right, row.y1), true);
    dl->AddRectFilled(ImVec2(left, row.y0), ImVec2(right, row.y1), ui::kBg);
    if (lane >= 0 && static_cast<size_t>(lane) < d.divide_values.size() && *h.part_sel == d.divide_values[static_cast<size_t>(lane)])
        dl->AddRectFilled(ImVec2(left, row.y0), ImVec2(right, row.y1), Alpha(ui::kAccent, 0.05f));
    if (!F.have_cols) dl->AddLine(ImVec2(left, mid + 0.5f), ImVec2(right, mid + 0.5f), ui::kStroke);  // else each channel's
    const double v1 = F.v0 + F.vl;
    // Bodies.
    for (const ZoneLook& z : F.Looks()) {
        if (z.gap || z.b < F.v0 || z.a > v1) continue;
        const float xa = F.X(z.a), xb = F.X(z.b);
        if (Mine(z, lane)) {
            dl->AddRectFilled(ImVec2(xa, y0), ImVec2(xb, y1), Alpha(PartColOf(h, d, ZoneOf(d, z.ref)->part), 0.1f));
        } else {
            DashedRect(dl, ImVec2(xa + 0.5f, y0 + 0.5f), ImVec2(xb - 0.5f, y1 - 0.5f), Alpha(kGapCol, 0.35f), 3.0f * dp, 3.0f * dp);
        }
    }
    // Peaks: a zone's audio in its part's colour (fades, Gap crossfades and its gain applied); elsewhere the file, grey.
    // 13-71: a lane per channel of the file (the Sound's mono: one), drawn by DrawWaveLanes.
    if (F.have_cols) {
        size_t zi = 0;
        auto   zone_at = [&](double t) -> const ZoneLook* {
            while (zi < F.Looks().size() && F.Looks()[zi].b <= t) ++zi;
            for (size_t k = zi; k < F.Looks().size() && F.Looks()[k].a <= t; ++k)
                if (t >= F.Looks()[k].a && t < F.Looks()[k].b) return &F.Looks()[k];
            return nullptr;
        };
        WaveDrawStyle st;
        st.pad = 6.0f * dp;
        st.zero = ui::kStroke;
        st.split = ui::kStrokeStrong;
        // A column: the file grey where no zone of this lane plays; a zone's audio in its part's colour (its gain,
        // its fades: the grey under a fade); a Gap's crossfading neighbours of this lane over its grey. A pixel shorter
        // than a sample (`line`): the samples joined by a line, each segment coloured as its column.
        DrawWaveLanes(dl, F.wc, F.d->mono, F.cx0, row.y0, row.y1, st, [&](double pos, double, bool line, WaveInk* out) {
            const double    t = F.T(F.cx0 + static_cast<float>(pos));  // grows with pos: zone_at walks on
            const ZoneLook* z = zone_at(t);
            int             n = 0;
            auto put = [&](ImU32 col, float al, double g, double e) {
                if (n < kWaveMaxInks) out[n++] = WaveInk{Alpha(col, al), g, e, 0};
            };
            auto heard = [&](const ZoneLook& L) {
                const std::string& part = ZoneOf(d, L.ref)->part;
                return std::make_pair(PartColOf(h, d, part), PartHeardNow(h, part) ? 0.92f : 0.28f);
            };
            if (z && z->gap) {
                put(kGapCol, line ? 0.5f : 0.2f, 1.0, 1.0);
                for (const ZoneLook* L : GapSides(F, *z)) {
                    if (!L || !Mine(*L, lane)) continue;
                    const double e = EnvOf(*L, t);
                    if (e > 0.001) {
                        const auto c = heard(*L);
                        put(c.first, c.second, L->gain, e);
                    }
                }
                return n;
            }
            if (z && Mine(*z, lane)) {
                const double e = EnvOf(*z, t);
                const auto   c = heard(*z);
                if (!line && e < 0.999) put(kGapCol, 0.14f, z->gain, 1.0);
                put(c.first, c.second, z->gain, e);
                return n;
            }
            put(kGapCol, line ? 0.5f : z ? 0.26f : 0.16f, 1.0, 1.0);
            return n;
        });
    }
    ImFont* fb = ui::FontCaptionBold();
    // Gaps: hatched on every lane, their crossfade lines and two square handles.
    for (const ZoneLook& z : F.Looks()) {
        if (!z.gap || z.b < F.v0 || z.a > v1) continue;
        const EditZone& g = *ZoneOf(d, z.ref);
        const float     ha = F.X(z.a), hb = F.X(z.b);
        dl->AddRectFilled(ImVec2(ha, y0), ImVec2(hb, y1), IM_COL32(12, 13, 16, 115));
        HatchRect(dl, ImVec2(ha, y0), ImVec2(hb, y1), Alpha(kGapCol, 0.45f));
        dl->AddLine(ImVec2(std::round(ha) + 0.5f, y0), ImVec2(std::round(ha) + 0.5f, y1), Alpha(kGapCol, 0.9f));
        dl->AddLine(ImVec2(std::round(hb) - 0.5f, y0), ImVec2(std::round(hb) - 0.5f, y1), Alpha(kGapCol, 0.9f));
        const ImU32 xc = IM_COL32(231, 233, 238, 217);
        dl->AddLine(ImVec2(F.X(g.a - g.xr), y0 + 6.0f * dp), ImVec2(F.X(g.a + g.xl), y1), xc, 1.2f);
        dl->AddLine(ImVec2(F.X(g.b - g.xr), y1), ImVec2(F.X(g.b + g.xl), y0 + 6.0f * dp), xc, 1.2f);
        const bool hv = v.hover_on && v.hover.kind == ZoneHitKind::Xf && v.hover.z == z.ref && v.hover.lane == lane;
        auto       handle = [&](float hx, bool hot) {
            const ImVec2 a(hx - 4.0f * dp, y0 + 2.0f * dp), b(hx + 4.0f * dp, y0 + 10.0f * dp);
            dl->AddRectFilled(a, b, hot ? ui::kWhite : ui::kText);
            dl->AddRect(a, b, ui::kBg);
        };
        handle(F.X(g.a + g.xl), hv && v.hover.side < 0);
        handle(F.X(g.b - g.xr), hv && v.hover.side > 0);
        if (hb - ha > 50.0f * dp) {
            dl->AddRectFilled(ImVec2(ha + 2.0f * dp, y0 + 12.0f * dp), ImVec2(ha + 2.0f * dp + TextW(fb, "Gap") + 6.0f * dp, y0 + 25.0f * dp),
                              Alpha(kGapCol, 0.95f));
            TextAt(dl, fb, ha + 5.0f * dp, y0 + 22.0f * dp, IM_COL32(0x10, 0x13, 0x1A, 0xFF), "Gap");
        }
        if (hb - ha > 84.0f * dp) {
            ImFont*           fm = ui::FontMonoTiny();
            const std::string s = "jumps " + MsText(g.b - g.a);
            TextAt(dl, fm, (ha + hb) * 0.5f - TextW(fm, s) * 0.5f, y1 - 6.0f * dp, ui::kText, s);
        }
        if (SelHas(v.sel, z.ref)) dl->AddRect(ImVec2(ha + 1.0f, y0 + 1.0f), ImVec2(hb - 1.0f, y1 - 1.0f), ui::kWhite);
    }
    // Zones: borders, fades, labels, hand marks, snap offsets.
    const bool gain_drag = v.drag.kind == ZoneDragKind::Gain;
    for (const ZoneLook& z : F.Looks()) {
        if (z.gap || z.b < F.v0 || z.a > v1 || !Mine(z, lane)) continue;
        const EditZone&   ez = *ZoneOf(d, z.ref);
        const ImU32       pc = PartColOf(h, d, ez.part);
        const std::string pname = PartNameOf(h, ez.part);
        const float       xa = F.X(z.a), xb = F.X(z.b);
        const bool        sel = SelHas(v.sel, z.ref);
        const bool        hvz = v.hover_on && v.hover.z == z.ref && v.hover.kind == ZoneHitKind::Zone && v.hover.top && v.hover.lane == lane;
        dl->AddRect(ImVec2(std::round(xa) + 0.5f, y0 + 0.5f), ImVec2(std::round(xb) - 0.5f, y1 - 0.5f), sel ? ui::kWhite : Alpha(pc, hvz ? 1.0f : 0.65f),
                    0.0f, 0, 1.0f);  // Antho 2026-10-15: a thin outline when selected
        if (v.hover_on && v.hover.kind == ZoneHitKind::Edge && v.hover.z == z.ref && v.hover.lane == lane) {
            const float ex = v.hover.side < 0 ? xa : xb;
            dl->AddRectFilled(ImVec2(ex - 1.5f * dp, y0), ImVec2(ex + 1.5f * dp, y1), ui::kWhite);
        }
        const ImU32 fl = IM_COL32(231, 233, 238, 191);
        if (!z.lg) dl->AddLine(ImVec2(xa, y1), ImVec2(F.X(z.a + z.fi_s), y0 + 1.0f), fl);
        if (!z.rg) dl->AddLine(ImVec2(F.X(z.b - z.fo_s), y0 + 1.0f), ImVec2(xb, y1), fl);
        const bool hf = v.hover_on && v.hover.kind == ZoneHitKind::Fade && v.hover.z == z.ref && v.hover.lane == lane;
        auto       fh = [&](float fx, bool hot) {
            dl->AddCircleFilled(ImVec2(fx, y0 + 4.0f * dp), (hot ? 4.0f : 3.0f) * dp, hot ? ui::kWhite : IM_COL32(0xC7, 0xCB, 0xD3, 0xFF));
        };
        if (xb - xa > 14.0f * dp) {
            if (!z.lg) fh(F.X(z.a + z.fi_s), hf && v.hover.side < 0);
            if (!z.rg) fh(F.X(z.b - z.fo_s), hf && v.hover.side > 0);
        }
        if (xb - xa > 36.0f * dp && IsDivided(d)) {  // 13-62i: no part chip on an undivided Sound's zones
            const float w = std::min(TextW(fb, pname) + 6.0f * dp, xb - xa - 4.0f * dp);
            dl->AddRectFilled(ImVec2(xa + 2.0f * dp, y0 + 10.0f * dp), ImVec2(xa + 2.0f * dp + w, y0 + 23.0f * dp), Alpha(pc, 0.85f));
            dl->PushClipRect(ImVec2(xa + 2.0f * dp, y0 + 10.0f * dp), ImVec2(xa + 2.0f * dp + w, y0 + 23.0f * dp), true);
            TextAt(dl, fb, xa + 5.0f * dp, y0 + 20.0f * dp, IM_COL32(0x10, 0x13, 0x1A, 0xFF), pname);
            dl->PopClipRect();
        }
        if (std::fabs(ez.db) >= 0.05 || (gain_drag && SelHas(v.drag.sel, z.ref))) {
            const std::string txt = ZoneDbText(ez.db);
            const float       w = TextW(fb, txt);
            if (xb - xa > w + 50.0f * dp) {
                bool clip = false;  // its loudest peak at its gain over full scale
                if (F.peaks && F.peaks->rate > 0.0) {
                    const long long i0 = static_cast<long long>(std::floor(z.a * F.peaks->rate));
                    const long long i1 = static_cast<long long>(std::ceil(z.b * F.peaks->rate));
                    float           pk = 0.0f;
                    for (long long i = std::max(0LL, i0); i < i1 && i < static_cast<long long>(F.peaks->max_abs.size()); ++i)
                        pk = std::max(pk, F.peaks->max_abs[static_cast<size_t>(i)]);
                    clip = pk * z.gain > 1.0;
                }
                const float gx = xb - 8.0f * dp - w;
                dl->AddRectFilled(ImVec2(gx - 3.0f * dp, y0 + 9.0f * dp), ImVec2(gx + w + 3.0f * dp, y0 + 23.0f * dp), ui::kRaised);
                dl->AddRect(ImVec2(gx - 2.5f * dp, y0 + 9.5f * dp), ImVec2(gx + w + 2.5f * dp, y0 + 22.5f * dp),
                            clip ? ui::kBadLine : ui::kStrokeStrong);
                TextAt(dl, fb, gx, y0 + 20.0f * dp, clip ? ui::kBadInk : ui::kText, txt);
            }
        }
        auto pin = [&](float ex, float dir) {
            dl->AddTriangleFilled(ImVec2(ex, y0), ImVec2(ex + dir * 7.0f * dp, y0), ImVec2(ex, y0 + 8.0f * dp), ui::kWarn);
        };
        if (ez.a_hand && !z.lg) pin(xa + 1.0f, 1.0f);
        if (ez.b_hand && !z.rg) pin(xb - 1.0f, -1.0f);
        {
            const float sx = std::round(F.X(z.snap)) + 0.5f;
            const bool  hot = v.hover_on && v.hover.kind == ZoneHitKind::Snap && v.hover.z == z.ref && v.hover.lane == lane;
            DashedV(dl, sx, y0 + 26.0f * dp, y1, Alpha(pc, hot ? 1.0f : 0.75f), 2.0f * dp, 3.0f * dp);
            dl->AddRectFilled(ImVec2(sx - 5.0f * dp, y0 + 26.0f * dp), ImVec2(sx + 6.0f * dp, y0 + 37.0f * dp), hot ? ui::kWhite : pc);
            const std::string ini = pname.empty() ? std::string("?") : pname.substr(0, 1);
            TextAt(dl, ui::FontBadge(), sx - 3.0f * dp, y0 + 35.0f * dp, IM_COL32(0x10, 0x13, 0x1A, 0xFF), ini);
            if (z.snap_own) dl->AddRectFilled(ImVec2(sx + 6.0f * dp, y0 + 26.0f * dp), ImVec2(sx + 9.0f * dp, y0 + 37.0f * dp), ui::kWarn);
        }
        // The top / bottom halves, faint (top: move, bottom: cursor).
        if (v.hover_on && v.hover.z == z.ref && v.hover.kind == ZoneHitKind::Zone && v.hover.lane == lane) {
            if (v.hover.top) dl->AddRectFilled(ImVec2(xa, y0), ImVec2(xb, mid), IM_COL32(255, 255, 255, 10));
            else dl->AddRectFilled(ImVec2(xa, mid), ImVec2(xb, y1), IM_COL32(255, 255, 255, 10));
        }
    }
    // Cuts between two zones edge to edge: a grab at the bottom.
    for (size_t i = 0; i + 1 < F.Looks().size(); ++i) {
        const ZoneLook &x = F.Looks()[i], &y = F.Looks()[i + 1];
        if (x.gap || y.gap || std::fabs(x.b - y.a) >= kZoneTouch || !(Mine(x, lane) || Mine(y, lane))) continue;
        const float cx = std::round(F.X(x.b)) + 0.5f;
        const bool  hot = (v.hover_on && v.hover.kind == ZoneHitKind::Cut && v.hover.z == x.ref && v.hover.lane == lane) ||
                         (v.drag.kind == ZoneDragKind::Cut && v.drag.hit.z == x.ref);
        DashedV(dl, cx, y0, y1 - 10.0f * dp, hot ? ui::kWhite : IM_COL32(231, 233, 238, 153), 3.0f * dp, 3.0f * dp);
        dl->AddRectFilled(ImVec2(cx - 2.5f * dp, y1 - 11.0f * dp), ImVec2(cx + 2.5f * dp, y1 - 1.0f * dp), hot ? ui::kWhite : IM_COL32(0xC7, 0xCB, 0xD3, 0xFF));
    }
    // Lane: another part's zone shows what a click does.
    if (lane >= 0 && v.hover_on && v.hover.lane == lane && v.hover.kind == ZoneHitKind::Grey && v.drag.kind == ZoneDragKind::None &&
        static_cast<size_t>(lane) < d.divide_values.size()) {
        if (const EditZone* z = ZoneOf(d, v.hover.z)) {
            const ImU32       lc = static_cast<size_t>(lane) < h.colors.size() ? h.colors[static_cast<size_t>(lane)] : ui::kAccent;
            const float       xa = F.X(z->a), xb = F.X(z->b);
            DashedRect(dl, ImVec2(xa + 1.0f, y0 + 1.0f), ImVec2(xb - 1.0f, y1 - 1.0f), lc, 4.0f * dp, 3.0f * dp, 1.5f);
            const std::string label = "click \xE2\x86\x92 " + PartNameOf(h, d.divide_values[static_cast<size_t>(lane)]) + " \xC2\xB7 drag: swipe";
            const float       lw = TextW(fb, label);
            if (xb - xa > lw + 10.0f * dp) {
                dl->AddRectFilled(ImVec2(xa + 3.0f * dp, y0 + 3.0f * dp), ImVec2(xa + 11.0f * dp + lw, y0 + 19.0f * dp), IM_COL32(18, 19, 23, 217));
                TextAt(dl, fb, xa + 7.0f * dp, y0 + 15.0f * dp, lc, label);
            }
        }
    }
    // A new zone / a new Gap / a swipe being drawn.
    const ZoneDrag& dg = v.drag;
    if (dg.tl == h.tl && (dg.kind == ZoneDragKind::MkGap || dg.kind == ZoneDragKind::MkZone || dg.kind == ZoneDragKind::Swipe)) {
        const float a = F.X(std::min(dg.t0, dg.t1)), b = F.X(std::max(dg.t0, dg.t1));
        if (dg.kind == ZoneDragKind::MkGap) {
            dl->AddRectFilled(ImVec2(a, row.y0), ImVec2(b, row.y1), IM_COL32(12, 13, 16, 128));
            HatchRect(dl, ImVec2(a, row.y0), ImVec2(b, row.y1), IM_COL32(231, 233, 238, 153));
        } else if (dg.hit.lane == lane) {
            const ImU32 pc = PartColOf(h, d, dg.part);
            dl->AddRectFilled(ImVec2(a, row.y0), ImVec2(b, row.y1), Alpha(pc, 0.22f));
            dl->AddRect(ImVec2(a + 0.5f, row.y0 + 0.5f), ImVec2(b - 0.5f, row.y1 - 0.5f), pc);
        }
    }
    DrawOverlays(F, row.y0, row.y1, false);
    dl->PopClipRect();
}

// ---- the drag ---------------------------------------------------------------------------------------------------

bool IsHandle(ZoneDragKind k)
{
    return k == ZoneDragKind::Edge || k == ZoneDragKind::Cut || k == ZoneDragKind::GapEdge || k == ZoneDragKind::Fade ||
           k == ZoneDragKind::Snap || k == ZoneDragKind::Xf;
}

// One frame of a drag at source time t (the mouse moved dx, dy px from the press).
void DragMove(const ZoneWaveHost& h, double t, float dx, float dy, double px_per_s)
{
    ZoneEditView& v = *h.view;
    ZoneDrag&     g = v.drag;
    const float   dp = ui::Dp(1.0f);
    if (g.kind == ZoneDragKind::Pending) {
        if (ZoneReservedOnMove(g.hit, g.ctrl, g.alt)) {  // reserved: Ctrl+drag, Alt+drag of a handle (unmoved: the click)
            if (std::fabs(dx) > 4.0f * dp || std::fabs(dy) > 4.0f * dp) g.kind = ZoneDragKind::Reserved;
            return;
        }
        if (PendingBecomesGain(Stored(h), g.hit, dx, dy, g.shift, g.alt, dp)) {
            g.kind = ZoneDragKind::Gain;
            g.sel = GainDragZones(Stored(h), v.sel, g.hit.z);  // the selection; unselected: its whole item (v5)
            if (g.sel.empty()) g.sel = {g.hit.z};
        } else if (std::fabs(dx) > 4.0f * dp) {
            g.kind = StartZoneDrag(Stored(h), g.hit, g.shift, g.alt);
            if (g.kind == ZoneDragKind::MkZone) g.part = NewZonePart(Stored(h), g.hit, *h.part_sel, h.tl);
            if (g.kind == ZoneDragKind::Swipe && g.hit.lane >= 0 && static_cast<size_t>(g.hit.lane) < Stored(h).divide_values.size())
                g.part = Stored(h).divide_values[static_cast<size_t>(g.hit.lane)];
        } else {
            return;
        }
    }
    if (IsHandle(g.kind) && !g.moved) {
        if (std::fabs(dx) <= 2.0f * dp && std::fabs(dy) <= 2.0f * dp) return;
        g.moved = true;
    }
    const ZoneContext c = Ctx(h);
    switch (g.kind) {
    case ZoneDragKind::Gain: {
        g.moved = true;
        g.value = GainDragDb(dy / dp);  // 0.1 dB per logical px
        SoundData& dr = h.live();
        g.changed = ShiftZonesDb(dr, g.sel, g.value, {}, c, &(g.after = v.sel));
        return;
    }
    case ZoneDragKind::Move: {
        g.moved = true;
        SoundData& dr = h.live();
        ZoneSel    out;
        g.value = MoveRange(Stored(h), g.hit.z, t - g.hit.t, h.file_len);
        g.changed = MoveZone(dr, g.hit.z, t - g.hit.t, c, &out);
        if (g.changed) g.sel = out;
        return;
    }
    case ZoneDragKind::MoveChain: {  // 13-62h (v5): the whole item, its members by the same delta
        g.moved = true;
        SoundData& dr = h.live();
        ZoneSel    out;
        g.value = MoveChainRange(Stored(h), g.hit.z, t - g.hit.t, h.file_len);
        g.changed = MoveChain(dr, g.hit.z, t - g.hit.t, c, &out);
        if (g.changed) g.sel = out;
        return;
    }
    case ZoneDragKind::Edge: g.changed = DragZoneEdge(h.live(), g.hit.z, g.hit.side, t, c, &(g.after = {g.hit.z})); return;
    case ZoneDragKind::Cut: g.changed = DragCut(h.live(), g.hit.z, t, c, &(g.after = {g.hit.z})); return;
    case ZoneDragKind::GapEdge: g.changed = DragGapEdge(h.live(), g.hit.z, g.hit.side, t, c, &(g.after = {g.hit.z})); return;
    case ZoneDragKind::Fade: {
        const EditZone* z = ZoneOf(Stored(h), g.hit.z);
        if (!z) return;
        const double len = z->b - z->a;
        const double s = Clamp(g.hit.side < 0 ? t - z->a : z->b - t, 0.0, len / 2.0);
        g.value = s;
        g.changed = SetZoneFade(h.live(), g.hit.z, g.hit.side, std::round(s * 2000.0) / 2000.0, c, &(g.after = {g.hit.z}));
        return;
    }
    case ZoneDragKind::Snap: g.changed = SetZoneSnap(h.live(), g.hit.z, t, c, &(g.after = {g.hit.z})); return;
    case ZoneDragKind::Xf:
        g.changed = DragGapXfade(h.live(), g.hit.z, g.hit.side, t, !g.alt, c, &(g.after = {g.hit.z}));
        return;
    case ZoneDragKind::Scrub:
        g.moved = true;
        *h.cursor = t;
        return;
    case ZoneDragKind::Tsel:
    case ZoneDragKind::MkGap:
    case ZoneDragKind::MkZone:
    case ZoneDragKind::Swipe: g.t1 = t; return;
    case ZoneDragKind::TselEdge: {
        const double s = SnapZoneTime(Shown(h), t, px_per_s, *h.cursor, 6.0f * dp, h.tl);
        if (g.hit.side < 0) v.ts_a = std::min(s, v.ts_b - 0.002);
        else v.ts_b = std::max(s, v.ts_a + 0.002);
        g.moved = true;
        return;
    }
    default: return;
    }
}

void DragRelease(const ZoneWaveHost& h, double px_per_s)
{
    ZoneEditView&  v = *h.view;
    const ZoneDrag g = v.drag;
    v.drag = ZoneDrag{};
    const ImVec2 m = ImGui::GetIO().MousePos;
    const float  dp = ui::Dp(1.0f);
    auto         is_double = [&] { return ZoneDoubleTap(v.tap, ZoneTapKey(g.hit), ImGui::GetTime(), m.x, m.y, 6.0f * dp); };
    if (IsHandle(g.kind)) {
        if (!g.moved) {
            if (h.live_now() && g.changed) h.cancel();
            if (is_double()) Double(h, g.hit);
            else Click(h, g.hit, false);
            return;
        }
        if (!g.changed) {
            h.cancel();
            return;
        }
        const SoundData&  dr = Shown(h);
        const std::string nm = NameOfZone(h, Stored(h), g.hit.z);
        std::string       name, toast;
        const EditZone*   z = ZoneOf(dr, g.hit.z);
        switch (g.kind) {
        case ZoneDragKind::Edge:
            name = "Zone edge";
            toast = nm + " " + (g.hit.side < 0 ? "start" : "end") + ": " + (z ? MsText(g.hit.side < 0 ? z->a : z->b) : std::string()) + " (by hand)";
            break;
        case ZoneDragKind::Cut:
            name = "Move cut";
            toast = "Cut moved to " + (z ? MsText(z->b) : std::string()) + ": both zones follow";
            break;
        case ZoneDragKind::GapEdge:
            name = "Gap edge";
            toast = "Gap " + (z ? ZoneRangeMs(z->a, z->b) : std::string()) + ": the zone beside it follows";
            break;
        case ZoneDragKind::Fade:
            name = "Zone fade";
            toast = nm + " fade " + (g.hit.side < 0 ? "in" : "out") + ": " + std::to_string(Ms(g.value)) + " ms (this zone's own)";
            break;
        case ZoneDragKind::Snap:
            name = "Snap offset";
            toast = nm + " snap offset: " + (z && z->snap ? MsText(*z->snap) : std::string()) + " (this zone's own)";
            break;
        default: {
            char buf[96];
            std::snprintf(buf, sizeof(buf), "Gap crossfade: %.1f + %.1f ms", z ? z->xl * 1000.0 : 0.0, z ? z->xr * 1000.0 : 0.0);
            name = "Gap crossfade";
            toast = buf;
            break;
        }
        }
        v.sel = SelValid(dr, g.after);  // the zone dragged, re-resolved by its gesture
        Commit(h, name, toast);
        return;
    }
    switch (g.kind) {
    case ZoneDragKind::Pending:
        if (!g.add && is_double()) Double(h, g.hit);
        else Click(h, g.hit, g.add);
        return;
    case ZoneDragKind::TselEdge:
        *h.cursor = v.ts_a;
        ShowLibraryToast("Time selection " + ZoneRangeMs(v.ts_a, v.ts_b), false);
        return;
    case ZoneDragKind::Tsel: {
        const double a = SnapZoneTime(Shown(h), std::min(g.t0, g.t1), px_per_s, *h.cursor, 6.0f * dp, h.tl);
        const double b = SnapZoneTime(Shown(h), std::max(g.t0, g.t1), px_per_s, *h.cursor, 6.0f * dp, h.tl);
        if (b - a <= 0.002) {  // as good as a click: the cursor there, the time selection kept
            *h.cursor = g.hit.t;
            return;
        }
        SetTsel(h, a, b, "Time selection " + ZoneRangeMs(a, b) + ": playback stays inside it" + (v.loop ? " (Loop: repeats)" : ""));
        return;
    }
    case ZoneDragKind::Gain: {
        if (!g.changed) {
            h.cancel();
            return;
        }
        std::string toast;
        if (g.sel.size() > 1) {
            toast = std::to_string(g.sel.size()) + " zones: " + Signed(g.value) + " dB each (their own gain)";
        } else {
            const EditZone* z = ZoneOf(Shown(h), g.sel.front());
            toast = z ? ZoneGainToast(NameOfZone(h, Shown(h), g.sel.front()), PartNameOf(h, z->part), z->db,
                                      PartFaderDb(Shown(h), z->part))  // 13-62i: the implicit part's fader too
                      : std::string("Zone gain");
        }
        v.sel = SelValid(Shown(h), g.after);
        Commit(h, "Zone gain", toast);
        return;
    }
    case ZoneDragKind::Move: {
        if (!g.changed) {
            h.cancel();
            return;
        }
        const ZoneRef  r = g.sel.empty() ? g.hit.z : g.sel.front();
        const EditZone* z0 = ZoneOf(Stored(h), g.hit.z);
        const ZoneRef  pv = PrevZone(Stored(h), g.hit.z), nx = NextZone(Stored(h), g.hit.z);
        const bool     followed = (z0 && z0->IsGap()) || Butted(Stored(h), pv, g.hit.z) || Butted(Stored(h), g.hit.z, nx);
        // 13-62h (v6 rule A): a Gap slid inside its item leaves the whole item selected.
        const ZoneSel     item = ItemOf(Shown(h), r);
        const bool        slid = z0 && z0->IsGap() && !item.empty();
        const std::string by = std::string(g.value > 0 ? "+" : "") + std::to_string(Ms(g.value)) + " ms";
        const std::string toast = slid ? "Gap slid " + by + " inside " + ItemNames(Shown(h), item, h.names) + " (the zones beside it follow)"
                                       : NameOfZone(h, Shown(h), r) + " moved " + by + " (its two edges" +
                                             (followed ? "; what touches it followed)" : ")");
        if (slid) v.sel = item;
        else v.sel = {r};
        Commit(h, slid ? "Slide Gap" : "Move zone", toast);
        return;
    }
    case ZoneDragKind::MoveChain: {
        if (!g.changed) {
            h.cancel();
            return;
        }
        const SoundData& st = Stored(h);
        const ZoneSel    item = ItemOf(st, g.hit.z);
        const ZoneRef    pv = item.empty() ? ZoneRef{} : PrevZone(st, item.front());
        const ZoneRef    nx = item.empty() ? ZoneRef{} : NextZone(st, item.back());
        const bool       followed = !item.empty() && (Butted(st, pv, item.front()) || Butted(st, item.back(), nx));
        const std::string toast = ItemNames(st, item, h.names) + " moved " + (g.value > 0 ? "+" : "") + std::to_string(Ms(g.value)) +
                                  " ms (its zones and Gaps together" + (followed ? "; what touches it followed)" : ")");
        v.sel = SelValid(Shown(h), g.sel);
        Commit(h, "Move item", toast);
        return;
    }
    case ZoneDragKind::MkGap:
    case ZoneDragKind::MkZone:
    case ZoneDragKind::Swipe: {
        const double a = SnapZoneTime(Shown(h), std::min(g.t0, g.t1), px_per_s, *h.cursor, 6.0f * dp, h.tl);
        const double b = SnapZoneTime(Shown(h), std::max(g.t0, g.t1), px_per_s, *h.cursor, 6.0f * dp, h.tl);
        if (b - a < kMinZone) return;
        if (g.kind == ZoneDragKind::MkGap) {
            const bool wrote = Write(
                h, "New Gap",
                [a, b](SoundData& x, const ZoneContext& c, ZoneSel* s) { return MakeGap(x, a, b, c, nullptr, s); },
                [a, b](const SoundData& after, const ZoneSel& out) {
                    const EditZone* gz = out.empty() ? nullptr : ZoneOf(after, out.front());
                    if (gz && gz->IsGap()) {
                        char buf[48];
                        std::snprintf(buf, sizeof(buf), "%.1f", (gz->xl + gz->xr) * 1000.0);
                        return "Gap " + ZoneRangeMs(a, b) + ": playback jumps over it (crossfade " + buf + " ms)";
                    }
                    return ZoneRangeMs(a, b) + " cleared: it touched no zone on one side, so it is empty space (the zone is shortened)";
                },
                true);
            if (!wrote) ShowLibraryToast("Nothing there to make a Gap of", false);
            return;
        }
        const std::string part = g.part;
        const std::string toast = std::string(g.kind == ZoneDragKind::Swipe ? "Swipe" : "New zone") + ": " + ZoneRangeMs(a, b) + " is " +
                                  PartNameOf(h, part);
        if (Write(h, g.kind == ZoneDragKind::Swipe ? "Swipe to part" : "New zone",
                  [part, a, b](SoundData& x, const ZoneContext& c, ZoneSel* s) { return NewZone(x, part, a, b, c, s); }, toast, true))
            *h.part_sel = part;
        return;
    }
    default: return;
    }
}

// ---- outside writes ---------------------------------------------------------------------------------------------

// The stored Sound changed and none of our writes did it (Ctrl+Z, another panel): the selection is carried over by
// time and part (SelFollow from what we saw last), the menu's target and the hover dropped.
void FollowOutside(const ZoneWaveHost& h)
{
    ZoneEditView&  v = *h.view;
    const uint64_t gen = SoundStoreGeneration();
    if (v.seen_uid != h.node->uid) {
        v.seen_uid = h.node->uid;
        v.seen = Stored(h);
        v.seen_gen = gen;
        v.own_pending = false;
        return;
    }
    if (gen == v.seen_gen) {
        if (v.own_pending && ImGui::GetFrameCount() - v.own_frame > 30) v.own_pending = false;  // it changed nothing
        return;
    }
    if (!v.own_pending && v.drag.kind == ZoneDragKind::None && !h.live_now()) {
        v.sel = SelFollow(v.seen, v.sel, Stored(h));
        v.menu_z = ZoneRef{};
        v.menu_kind = 0;
        v.menu_close = true;
        v.hover_on = false;
    }
    v.own_pending = false;
    v.seen = Stored(h);
    v.seen_gen = gen;
}

// ---- the right-click menu ---------------------------------------------------------------------------------------

// Esc (the mockup's keydown): the menu closes, and on the same press the selection goes, else the time selection.
// Once per press, whether the menu or the window's keys see it first.
bool EscStep(const ZoneWaveHost& h)
{
    ZoneEditView& v = *h.view;
    const int     frame = ImGui::GetFrameCount();
    if (v.esc_frame == frame) return true;
    v.esc_frame = frame;
    const bool menu = v.menu_frame >= frame - 1;
    if (menu) v.menu_close = true;
    if (v.rpress.on) {  // 13-67a: a marquee (or a right press) is dropped, nothing selected
        v.rpress = ZoneRightPress{};
        return true;
    }
    if (v.drag.kind != ZoneDragKind::None) {
        if (h.live_now()) h.cancel();
        v.drag = ZoneDrag{};
        return true;
    }
    if (!v.sel.empty()) {
        v.sel.clear();
        return true;
    }
    if (v.has_tsel) {
        v.has_tsel = false;
        return true;
    }
    return menu;
}

void ZoneMenu(const ZoneWaveHost& h)
{
    if (!ui::BeginMenuPopup("##zonewavemenu")) return;
    ZoneEditView& v = *h.view;
    v.menu_frame = ImGui::GetFrameCount();
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) EscStep(h);
    if (v.menu_close) {
        v.menu_close = false;
        v.menu_frame = -1;
        ImGui::CloseCurrentPopup();
        ui::EndMenuPopup();
        return;
    }
    const SoundData& d = Shown(h);
    const EditZone*  z = ZoneOf(d, v.menu_z);
    if (v.menu_kind == 0 || !z) {
        ui::MenuCaption("Nothing placed here");
        ui::MenuEntry("Shift+drag: a new zone", nullptr, false, false);
        ui::MenuEntry("Alt+drag: a Gap", nullptr, false, false);
    } else if (v.menu_kind == 3) {  // 13-62h (v5): an item; every entry acts on the selection (the item, or more)
        const ZoneSel item = ItemOf(d, v.menu_z);
        double        a = z->a, b = z->b;
        ItemEdges(d, item, &a, &b);
        ui::MenuCaption((ItemNames(d, item, h.names) + " (glued) \xC2\xB7 " + ZoneRangeMs(a, b)).c_str());
        if (ui::MenuEntry("\xE2\x96\xB6 Play it")) ZonePlaySelection(h);
        if (ui::MenuEntry("Normalize (one gain)##znorm", nullptr, false, static_cast<bool>(h.normalize))) h.normalize(SelValid(d, v.sel));
        ui::MenuSeparator();
        for (size_t k = 0; k < d.divide_values.size(); ++k) {
            const std::string label = "\xE2\x86\x92 " + PartNameOf(h, d.divide_values[k]) + "##zcto" + std::to_string(k);
            if (ui::MenuEntry(label.c_str())) ZoneGiveTo(h, d.divide_values[k]);
        }
        ui::MenuSeparator();
        if (ui::MenuEntry("Heal into one zone", KeyHint(kShortcutEditHeal, "H").c_str())) ZoneHeal(h);
        if (ui::MenuEntry("Delete it", "Delete")) ZoneDelete(h);
    } else if (z->IsGap()) {
        const ZoneRef     l = GapLeft(d, v.menu_z);
        const std::string ln = ZoneOf(d, l) ? NameOfZone(h, d, l) : std::string("the left");
        ui::MenuCaption(("Gap " + ZoneRangeMs(z->a, z->b)).c_str());
        // 13-62h (v6 rule A): fills this Gap only (the zone on its left takes it), whatever else is selected; Delete
        // does it only while the Gap is selected alone (with its item selected, Delete removes the item).
        const ZoneRef gz = v.menu_z;
        const bool    alone = v.sel.size() == 1 && v.sel.front() == gz;
        if (ui::MenuEntry(("Give it to " + ln + " (fill the hole)##zgive").c_str(), alone ? "Delete" : nullptr)) {
            if (!Write(h, "Fill Gap", [gz](SoundData& x, const ZoneContext& c, ZoneSel* s) { return FillGap(x, gz, c, s); },
                       "Hole filled: " + ln + " takes the Gap"))
                ShowLibraryToast("Nothing to fill", false);
        }
        if (ui::MenuEntry("\xE2\x96\xB6 Hear the jump")) ZonePlayZone(h, gz);
        if (v.sel.size() > 1) {
            ui::MenuSeparator();
            if (ui::MenuEntry("Heal selected", KeyHint(kShortcutEditHeal, "H").c_str())) ZoneHeal(h);
        }
    } else {
        ui::MenuCaption((NameOfZone(h, d, v.menu_z) + " zone " + ZoneRangeMs(z->a, z->b)).c_str());
        const ZoneRef r = v.menu_z;
        for (size_t k = 0; k < d.divide_values.size(); ++k) {
            const std::string& part = d.divide_values[k];
            const bool         now = z->part == part;
            const std::string  label = "\xE2\x86\x92 " + PartNameOf(h, part) + "##zto" + std::to_string(k);
            if (ui::MenuEntry(label.c_str(), now ? "\xE2\x9C\x93 now" : nullptr, false, !now)) GiveOne(h, r, part, "");
        }
        ui::MenuSeparator();
        if (ui::MenuEntry("Split at cursor", KeyHint(kShortcutEditSplit, "B").c_str())) ZoneSplit(h);
        if (v.sel.size() > 1 && ui::MenuEntry("Heal selected", KeyHint(kShortcutEditHeal, "H").c_str())) ZoneHeal(h);
        if (ui::MenuEntry(v.sel.size() > 1 ? "Delete selected" : "Delete zone", "Delete")) ZoneDelete(h);
        const bool hand = z->a_hand || z->b_hand;
        if (ui::MenuEntry("Edges: automatic again", "double-click", false, hand)) {
            const std::string nm = NameOfZone(h, d, r);
            Write(h, "Zone edges automatic", [r](SoundData& x, const ZoneContext& c, ZoneSel* s) {
                ZoneSel    one = {r};
                const bool a = ResetZoneEdge(x, r, -1, c, &one);  // the zone itself re-resolved between the two
                const ZoneRef r2 = one.empty() ? r : one.front();
                const bool b = ResetZoneEdge(x, r2, 1, c, s);
                return a || b;
            }, nm + ": both edges automatic again", true);
        }
        ui::MenuSeparator();
        if (ui::MenuEntry("\xE2\x96\xB6 Play zone")) ZonePlayZone(h, r);
    }
    ui::EndMenuPopup();
}

}  // namespace

// ---- prefs ------------------------------------------------------------------------------------------------------

bool ZoneClickPlays()
{
    if (g_click_plays < 0) g_click_plays = LoadPrefBool(kClickPlaysPref, true) ? 1 : 0;
    return g_click_plays == 1;
}

void SetZoneClickPlays(bool on)
{
    g_click_plays = on ? 1 : 0;
    SavePrefBool(kClickPlaysPref, on);
}

bool ZoneTailWins()
{
    if (g_tail_wins < 0) g_tail_wins = LoadPrefBool(kTailWinsPref, true) ? 1 : 0;
    return g_tail_wins == 1;
}

void SetZoneTailWins(bool on)
{
    g_tail_wins = on ? 1 : 0;
    SavePrefBool(kTailWinsPref, on);
}

SyncMode ZoneNewSnapMode()
{
    if (g_snap_mode < 0) {
        const long v = std::lround(LoadPrefFloat(kSnapModePref, static_cast<float>(SyncMode::Peak)));
        g_snap_mode = v >= 0 && v <= 2 ? static_cast<int>(v) : static_cast<int>(SyncMode::Peak);
        SetNewSoundSyncMode(static_cast<SyncMode>(g_snap_mode));
    }
    return static_cast<SyncMode>(g_snap_mode);
}

void SetZoneNewSnapMode(SyncMode m)
{
    g_snap_mode = static_cast<int>(m);
    SetNewSoundSyncMode(m);
    SavePrefFloat(kSnapModePref, static_cast<float>(g_snap_mode));
}

void LoadZoneNewSnapMode()
{
    (void)ZoneNewSnapMode();
}

void ZoneEditReset(ZoneEditView& v)
{
    v = ZoneEditView{};
}

// ---- the waveform -----------------------------------------------------------------------------------------------

// 13-62h (Antho): the detection threshold line's drag (Silence): the lane row it was grabbed on, whether it moved.
struct ThrDrag {
    bool   on = false, moved = false;
    int    row = -1;
    float  m0y = 0.0f;
    double db = 0.0;
};
ThrDrag g_thr;

ZoneWaveEvents DrawZoneWave(const char* id, const ZoneWaveHost& h, float height)
{
    ZoneWaveEvents ev;
    ZoneEditView&  v = *h.view;
    ImGui::PushID(id);
    const float  dp = ui::Dp(1.0f);
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const float  W = std::max(40.0f, ImGui::GetContentRegionAvail().x);
    const SoundData& d0 = Shown(h);
    const bool   lane_view = d0.lanes_split && IsDivided(d0);  // 13-62i: no Lane view without parts
    const int    nparts = std::max(1, PartCount(d0));
    const float  head_lo = 104.0f * dp;
    const float  head_w = lane_view ? std::min(std::clamp(h.head_px * dp, head_lo, std::max(head_lo, W * 0.5f)), std::max(0.0f, W - 40.0f)) : 0.0f;
    const float  cx0 = p0.x + head_w, cw = std::max(8.0f, W - head_w);
    // Rows: the ruler, the Variations strip, (Lane) the Placed strip, the zones.
    std::vector<Row> rows;
    float            y = p0.y;
    rows.push_back({ZoneRow::Ruler, -1, y, y + 20.0f * dp});
    y += 20.0f * dp;
    rows.push_back({ZoneRow::Vars, -1, y, y + 20.0f * dp});
    y += 20.0f * dp;
    if (lane_view) {
        rows.push_back({ZoneRow::Placed, -1, y, y + 14.0f * dp});
        y += 14.0f * dp;
    }
    const float rest = std::max(40.0f * dp, height - (y - p0.y));
    const int   nl = lane_view ? nparts : 1;
    const float lh = std::floor(rest / static_cast<float>(nl));
    for (int k = 0; k < nl; ++k) {
        rows.push_back({ZoneRow::Lane, lane_view ? k : -1, y, y + lh - (k + 1 < nl ? 1.0f : 0.0f)});
        y += lh;
    }
    const float bottom = y;
    ev.head_x = p0.x;
    ev.head_w = head_w;
    ev.head_narrow = lane_view && head_w < 116.0f * dp;
    ev.lanes_y = rows.back().y0 - (lane_view ? lh * static_cast<float>(nl - 1) : 0.0f);
    ev.lane_h = lh;
    ev.lanes = lane_view ? nl : 0;
    ev.min = ImVec2(cx0, p0.y);
    ev.max = ImVec2(cx0 + cw, bottom);

    ImGui::SetCursorScreenPos(ImVec2(cx0, p0.y));
    ImGui::InvisibleButton("##zwave", ImVec2(cw, bottom - p0.y),
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
    const bool    hovered = ImGui::IsItemHovered();
    const bool    active = ImGui::IsItemActive();
    const ImGuiIO& io = ImGui::GetIO();
    const ImVec2  m = io.MousePos;
    const double  vs = *h.view_start, vl = ViewLen(h) > 0.0 ? ViewLen(h) : 1.0;
    const double  px_per_s = cw / vl;
    auto          time_at = [&](float x) { return Clamp(vs + static_cast<double>(x - cx0) / cw * vl, 0.0, h.file_len > 0.0 ? h.file_len : 1e300); };
    auto          row_at = [&](float yy) -> int {
        for (size_t i = 0; i < rows.size(); ++i)
            if (yy >= rows[i].y0 && yy < rows[i].y1 + 1.0f) return static_cast<int>(i);
        return -1;
    };
    auto hit_at = [&](int ri, ImVec2 p) {
        const Row&  r = rows[static_cast<size_t>(ri)];
        ZoneHitView hv;
        hv.v0 = vs;
        hv.vl = vl;
        hv.w = cw;
        hv.h = r.y1 - r.y0;
        hv.row = r.row;
        hv.lane = r.lane;
        hv.has_tsel = v.has_tsel;
        hv.ts_a = v.ts_a;
        hv.ts_b = v.ts_b;
        hv.file_len = h.file_len;
        hv.peaks = PeaksOf(h.path);
        hv.dp = dp;
        hv.tl = h.tl;
        const LookCache& lc = LooksNow(h);
        hv.looks = &lc.looks;
        hv.blocks = &lc.blocks;
        return HitZones(Shown(h), hv, p.x - cx0, p.y - r.y0);
    };
    // An undo or a write elsewhere: the selection follows by time and part; then keep it valid.
    FollowOutside(h);
    v.sel = SelValid(Shown(h), v.sel);

    // 13-62h (Antho): the detection threshold (Silence), a dashed line above and below the middle of every zone row
    // (Combined and Lane alike), at the peaks' scale (DrawLane: half the row less 6 px).
    const bool thr_on = h.threshold && d0.mode == DetectMode::Silence && std::isfinite(d0.silence.threshold_db);
    auto       row_mid = [&](const Row& r) { return r.y0 + (r.y1 - r.y0) * 0.5f; };
    auto       row_hh = [&](const Row& r) { return std::max(1.0f, (r.y1 - r.y0) * 0.5f - 6.0f * dp); };
    auto       thr_y = [&](const Row& r, double db) { return static_cast<float>(std::min(1.0, std::pow(10.0, db / 20.0))) * row_hh(r); };

    // ---- input
    v.hover_on = false;
    if (v.drag.kind == ZoneDragKind::None && hovered) {
        const int ri = row_at(m.y);
        if (ri >= 0) {
            v.hover = hit_at(ri, m);
            v.hover_on = true;
        }
    }
    // The threshold line is grabbed within 4 px, where nothing else is (a zone's body, a greyed zone, empty space).
    bool thr_hot = false;
    if (thr_on && hovered && v.hover_on && !g_thr.on && v.drag.kind == ZoneDragKind::None) {
        const int ri = row_at(m.y);
        if (ri >= 0 && rows[static_cast<size_t>(ri)].row == ZoneRow::Lane &&
            (v.hover.kind == ZoneHitKind::Zone || v.hover.kind == ZoneHitKind::Grey || v.hover.kind == ZoneHitKind::None)) {
            const Row&  r = rows[static_cast<size_t>(ri)];
            const float ty = thr_y(r, d0.silence.threshold_db);
            thr_hot = std::fabs(m.y - (row_mid(r) - ty)) <= 4.0f * dp || std::fabs(m.y - (row_mid(r) + ty)) <= 4.0f * dp;
        }
    }
    if (thr_hot) {
        v.hover_on = false;  // the line, not the zone under it
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
    }
    if (thr_hot && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !io.KeyCtrl && !io.KeyShift && !io.KeyAlt) {
        g_thr = ThrDrag{};
        g_thr.on = true;
        g_thr.row = row_at(m.y);
        g_thr.m0y = m.y;
        g_thr.db = d0.silence.threshold_db;
    }
    if (g_thr.on) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
            if (std::fabs(m.y - g_thr.m0y) > 2.0f * dp) g_thr.moved = true;
            if (g_thr.moved && g_thr.row >= 0 && static_cast<size_t>(g_thr.row) < rows.size()) {
                const Row&   r = rows[static_cast<size_t>(g_thr.row)];
                const double amp = std::max(1e-4, static_cast<double>(std::fabs(row_mid(r) - m.y) / row_hh(r)));
                g_thr.db = Clamp(20.0 * std::log10(amp), -90.0, 0.0);
                h.threshold(g_thr.db, false);
                char tip[32];
                std::snprintf(tip, sizeof(tip), "%.1f dB", g_thr.db);
                ui::ShowTip(tip);
            }
        } else {
            if (g_thr.moved) h.threshold(g_thr.db, true);
            g_thr = ThrDrag{};
        }
        if (h.store_replaced()) {
            ImGui::PopID();
            return ev;
        }
    }
    if (hovered && (ImGui::IsMouseClicked(ImGuiMouseButton_Left) || ImGui::IsMouseClicked(ImGuiMouseButton_Right))) Activate(h);
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && v.drag.kind == ZoneDragKind::None && !thr_hot && !g_thr.on) {
        const int ri = row_at(m.y);
        if (ri >= 0) {
            ZoneDrag g;
            g.hit = hit_at(ri, m);
            g.row_index = ri;
            g.m0 = m;
            g.row_x = cx0;
            g.shift = io.KeyShift;
            g.alt = io.KeyAlt;
            g.ctrl = io.KeyCtrl;
            g.add = io.KeyCtrl || io.KeyShift;
            g.t0 = g.t1 = g.hit.t;
            g.kind = PressDragKind(g.hit, g.ctrl, g.alt);
            g.tl = h.tl;
            v.drag = g;
        }
    }
    if (v.drag.kind != ZoneDragKind::None && v.drag.tl == h.tl) {  // 13-62i: only the host it was pressed in
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            const ImVec2 dm(m.x - v.drag.m0.x, m.y - v.drag.m0.y);
            DragMove(h, time_at(m.x), dm.x, dm.y, px_per_s);
        }
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) DragRelease(h, px_per_s);
    }
    if (h.store_replaced()) {
        ImGui::PopID();
        return ev;
    }
    // 13-67a (Antho 2026-10-18): the right button. A press records where; dragged past 4 px from empty space it is a
    // marquee (like REAPER's arrange view); released without that, the right-click menu opens for the press point.
    ZoneRightPress& rp = v.rpress;
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right) && v.drag.kind == ZoneDragKind::None) {
        const int ri = row_at(m.y);
        if (ri >= 0) {
            rp = ZoneRightPress{};
            rp.on = true;
            rp.m0 = m;
            rp.add = io.KeyCtrl;
            rp.hit = hit_at(ri, m);
            rp.tl = h.tl;
            rp.empty = rows[static_cast<size_t>(ri)].row == ZoneRow::Lane && rp.hit.kind != ZoneHitKind::Grey &&
                       !ZoneOf(Shown(h), rp.hit.z);
        }
    }
    bool marquee_now = false;
    if (rp.on && rp.tl == h.tl && ImGui::IsMouseDown(ImGuiMouseButton_Right)) {
        if (!rp.marquee && rp.empty && (std::fabs(m.x - rp.m0.x) > 4.0f * dp || std::fabs(m.y - rp.m0.y) > 4.0f * dp))
            rp.marquee = true;
        marquee_now = rp.marquee;
    } else if (rp.on && rp.tl == h.tl) {  // released (a release this host did not see: dropped)
        const ZoneRightPress press = rp;
        rp = ZoneRightPress{};
        if (!ImGui::IsMouseReleased(ImGuiMouseButton_Right)) {
        } else if (press.marquee) {
            const float y0 = std::min(press.m0.y, m.y), y1 = std::max(press.m0.y, m.y);
            const SoundData&         d = Shown(h);
            std::vector<std::string> parts;
            bool                     every = !lane_view;
            for (const Row& r : rows) {
                if (r.row != ZoneRow::Lane || r.y1 < y0 || r.y0 > y1) continue;
                if (r.lane < 0) every = true;
                else if (static_cast<size_t>(r.lane) < d.divide_values.size()) parts.push_back(d.divide_values[static_cast<size_t>(r.lane)]);
            }
            const ZoneSel hits = ZonesInSpan(d, h.tl, time_at(press.m0.x), time_at(m.x), every ? nullptr : &parts);
            SelMarquee(v.sel, hits, press.add);
            if (!hits.empty() && !press.add) {
                const EditZone* fz = nullptr;
                for (const ZoneRef& r : hits)
                    if (const EditZone* z = ZoneOf(d, r); z && !z->IsGap()) {
                        fz = z;
                        break;
                    }
                if (fz) *h.part_sel = fz->part;
            }
            ShowLibraryToast(hits.empty() ? std::string(press.add ? "Marquee: nothing added" : "Marquee: nothing selected")
                                          : std::to_string(v.sel.size()) + (v.sel.size() == 1 ? " zone selected" : " zones selected"),
                             false);
        } else if (std::fabs(m.x - press.m0.x) <= 4.0f * dp && std::fabs(m.y - press.m0.y) <= 4.0f * dp) {
            const ZoneHit   hit = press.hit;
            const EditZone* z = ZoneOf(Shown(h), hit.z);
            // 13-62h (v5): on an item (but its greyed copy on another lane): it selected whole (unless it already
            // is) and its item menu.
            const ZoneSel item = hit.kind == ZoneHitKind::Grey ? ZoneSel{} : ItemOf(Shown(h), hit.z);
            v.menu_z = item.empty() || (z && z->IsGap()) ? hit.z : item.front();
            v.menu_kind = !z ? 0 : z->IsGap() ? 2 : !item.empty() ? 3 : 1;  // v6 rule A: an item's Gap: the Gap menu
            v.menu_close = false;
            if (z && !item.empty() && !z->IsGap()) {
                if (!SelHas(v.sel, hit.z)) v.sel = item;
                *h.part_sel = ZoneOf(Shown(h), item.front())->part;
                *h.cursor = hit.t;
            } else if (z) {
                if (!SelHas(v.sel, hit.z)) v.sel = {hit.z};
                if (!z->IsGap()) *h.part_sel = z->part;
                *h.cursor = hit.t;
            }
            ImGui::OpenPopup("##zonewavemenu");
        }
    }
    if (active && ImGui::IsMouseDown(ImGuiMouseButton_Middle) && io.MouseDelta.x != 0.0f)  // middle-drag pans (REAPER's hand scroll)
        ev.scroll_s = -static_cast<double>(io.MouseDelta.x) / cw * vl;

    // ---- drawing
    Frame F;
    F.h = &h;
    F.d = &Shown(h);
    F.dl = ImGui::GetWindowDrawList();
    F.cx0 = cx0;
    F.cw = cw;
    F.v0 = vs;
    F.vl = vl;
    F.dp = dp;
    F.peaks = PeaksOf(h.path);
    F.lc = &LooksNow(h);
    if (v.playing && v.play_tl == h.tl && AuditionPlaying() && AuditionTag() == PlayTag(h)) F.heads = HeardHeads(v.map, AuditionPosition());
    const int ncols = std::max(1, static_cast<int>(std::floor(cw)));
    F.have_cols = ReadWaveColumns(h.path, vs, vl, ncols, &F.wc);
    ImDrawList* dl = F.dl;
    dl->PushClipRect(ImVec2(p0.x, p0.y), ImVec2(p0.x + W, bottom), true);
    for (const Row& r : rows) {
        switch (r.row) {
        case ZoneRow::Ruler: DrawRuler(F, r.y0, r.y1); break;
        case ZoneRow::Vars: DrawVars(F, r.y0, r.y1); break;
        case ZoneRow::Placed: DrawPlaced(F, r.y0, r.y1); break;
        case ZoneRow::Lane: DrawLane(F, r); break;
        }
        if (r.row == ZoneRow::Lane && r.lane > 0) dl->AddLine(ImVec2(cx0, r.y0 - 0.5f), ImVec2(cx0 + cw, r.y0 - 0.5f), ui::kStrokeStrong);
    }
    // The threshold line (as the plain waveform's): dashed, the text colour at 45 % (accent grabbed or under the
    // mouse), its dB in a chip at the right of the first zone row.
    if (thr_on) {
        const double thr = Shown(h).silence.threshold_db;  // a live drag shows its draft
        const ImU32  col = (thr_hot || g_thr.on) ? ui::kAccent : Alpha(ui::kText, 0.45f);
        const float  dash = 3.0f * dp, step = 6.0f * dp;
        bool         chip = false;
        for (const Row& r : rows) {
            if (r.row != ZoneRow::Lane) continue;
            const float mid = row_mid(r), ty = thr_y(r, thr);
            for (float x = cx0; x < cx0 + cw; x += step)
                for (const float yy : {std::floor(mid - ty) + 0.5f, std::floor(mid + ty) + 0.5f})
                    dl->AddLine(ImVec2(x, yy), ImVec2(std::min(x + dash, cx0 + cw), yy), col);
            if (chip) continue;
            chip = true;
            char lab[32];
            std::snprintf(lab, sizeof(lab), "%.0f dB", thr);
            ImFont*      cf = ui::FontMonoTiny();
            const ImVec2 cs = ui::TextSizeIn(cf, lab);
            const ImVec2 c1(cx0 + cw - 4.0f, std::max(r.y0 + cs.y + 2.0f, mid - ty + cs.y * 0.5f + 1.0f));
            const ImVec2 c0(c1.x - cs.x - 8.0f, c1.y - cs.y - 2.0f);
            dl->AddRectFilled(c0, c1, ui::kRaised, 3.0f);
            dl->AddRect(c0, c1, ui::kStrokeStrong, 3.0f);
            ui::AddTextIn(dl, cf, ImVec2(c0.x + 4.0f, c0.y + 1.0f), ui::kText, lab);
        }
    }
    // Lane: the header column's captions (the lane headers are the caller's, drawn after).
    if (lane_view) {
        ImFont*                  f = ui::FontTiny();
        static const char* const kCaps[3] = {"TIME", "VARIATIONS", "PLACED"};
        for (int i = 0; i < 3; ++i) {
            const Row& r = rows[static_cast<size_t>(i)];
            dl->AddRectFilled(ImVec2(p0.x, r.y0), ImVec2(p0.x + head_w - 1.0f, r.y1), ui::kSurface);
            dl->AddLine(ImVec2(p0.x + head_w - 0.5f, r.y0), ImVec2(p0.x + head_w - 0.5f, r.y1), ui::kStrokeStrong);
            ui::FontScope fs(f);
            if (ui::EllipsisTextTip(dl, ImVec2(p0.x + 9.0f * dp, std::floor((r.y0 + r.y1 - f->FontSize) * 0.5f)), p0.x + head_w - 6.0f * dp,
                                    ui::kFaint, kCaps[i]))
                (void)0;
        }
    }
    if (marquee_now) {  // 13-67a: the marquee (accent, translucent), inside the zone canvas
        const ImVec2 a(std::clamp(std::min(rp.m0.x, m.x), cx0, cx0 + cw), std::clamp(std::min(rp.m0.y, m.y), p0.y, bottom));
        const ImVec2 b(std::clamp(std::max(rp.m0.x, m.x), cx0, cx0 + cw), std::clamp(std::max(rp.m0.y, m.y), p0.y, bottom));
        dl->AddRectFilled(a, b, Alpha(ui::kAccent, 0.15f));
        dl->AddRect(a, b, ui::kAccent);
    }
    dl->AddRect(ImVec2(cx0, p0.y), ImVec2(cx0 + cw, bottom), ui::kStroke);
    dl->PopClipRect();

    // ---- hover: cursor and tooltip
    if (v.drag.kind == ZoneDragKind::None && v.hover_on) {
        switch (ZoneCursorOf(v.hover)) {
        case ZoneCursor::ResizeEW: ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW); break;
        case ZoneCursor::Hand: ImGui::SetMouseCursor(ImGuiMouseCursor_Hand); break;
        case ZoneCursor::Text: ImGui::SetMouseCursor(ImGuiMouseCursor_TextInput); break;
        case ZoneCursor::Grab: ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll); break;
        case ZoneCursor::Arrow: break;
        }
        // Antho 2026-10-15: no tooltip over the zones and the strips (too much); ZoneTip waits for a tool-wide tips
        // system (deferred-work.md).
    } else if (IsHandle(v.drag.kind) || v.drag.kind == ZoneDragKind::TselEdge) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    } else if (v.drag.kind == ZoneDragKind::Gain) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
    }
    ZoneMenu(h);
    ImGui::SetCursorScreenPos(ImVec2(p0.x, bottom));
    ImGui::Dummy(ImVec2(W, 1.0f));
    ImGui::PopID();
    return ev;
}

// ---- keys and requests ------------------------------------------------------------------------------------------

void ZoneEditKeys(const ZoneWaveHost& h, bool* esc_used)
{
    if (esc_used) *esc_used = false;
    if (ImGui::IsKeyPressed(ImGuiKey_Delete, false)) ZoneDelete(h);
    if (ImGui::IsKeyPressed(ImGuiKey_Space, false)) ZoneTransport(h);
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) && EscStep(h) && esc_used) *esc_used = true;
}

void ZoneEditRequest(const ZoneWaveHost& h, int id)
{
    switch (id) {
    case kShortcutEditSplit: ZoneSplit(h); return;
    case kShortcutEditTrimLeft: ZoneTrimAt(h, -1); return;
    case kShortcutEditTrimRight: ZoneTrimAt(h, 1); return;
    case kShortcutEditTrimZone: ZoneTrimToTsel(h); return;
    case kShortcutEditHeal: ZoneHeal(h); return;
    case kShortcutEditLoop: ZoneToggleLoop(h); return;
    case kShortcutEditNextVar: ZoneStepVariation(h, 1); return;
    case kShortcutEditPrevVar: ZoneStepVariation(h, -1); return;
    case kShortcutEditSnapOffset: ZoneSnapToCursor(h); return;  // 13-67a
    case kShortcutEditPrevStop: ZonePrevStop(h); return;        // 13-67a
    default: return;
    }
}

void ZonePumpPlay(const ZoneWaveHost& h)
{
    ZoneEditView& v = *h.view;
    if (!v.playing) return;
    if (v.play_uid != h.node->uid) {
        v.playing = false;
        return;
    }
    const double now = ImGui::GetTime();
    // Not ours any more: it ended, was stopped (Esc, another button) or another audition took over. 13-67a (Loop
    // sometimes stops): a looping play that reached its end between two frames is gone before EditLoopDue could see
    // it within a frame of its end (frames are uneven; AuditionPlaying ends it at its end): restarted too, unless
    // another audition took over.
    if (!(AuditionPlaying() && AuditionTag() == PlayTag(h))) {
        v.playing = false;
        if (!AuditionPlaying() && EditLoopEndedAtEnd(v.tp, v.last_pos, now - v.last_seen_t, v.map.total)) {
            const EditTransport tp = v.tp;
            StartPlay(h, tp, tp.loop_a, tp.loop_b, true);
            v.last_seen_t = now;
        }
        return;
    }
    v.last_pos = AuditionPosition();
    v.last_seen_t = now;
    // C4: REAPER's preview has no loop: ours, looping and at its heard end (within this frame), the looped range
    // again from its start. Without Loop it plays to its real end.
    if (!EditLoopDue(v.tp, v.last_pos, v.map.total, ImGui::GetIO().DeltaTime)) return;
    const EditTransport tp = v.tp;
    StartPlay(h, tp, tp.loop_a, tp.loop_b, true);
}

bool ZonePlaying(const ZoneWaveHost& h)
{
    return h.view->playing && AuditionPlaying() && AuditionTag() == PlayTag(h);
}

void ZonePlayZone(const ZoneWaveHost& h, const ZoneRef& z)
{
    PlayRange(h, ZonePlayRange(Shown(h), z));
}

void ZonePlaySelection(const ZoneWaveHost& h)
{
    if (h.view->sel.empty()) {
        ShowLibraryToast("Nothing selected", false);
        return;
    }
    PlayRange(h, SelectionPlayRange(Shown(h), SelSorted(Shown(h), h.view->sel)));
}

void ZoneTransport(const ZoneWaveHost& h)
{
    ZoneEditView& v = *h.view;
    if (AuditionPlaying()) {
        StopAudition();
        v.playing = false;
        return;
    }
    const EditTransport tp = PlanEditTransport(*h.cursor, v.has_tsel, v.ts_a, v.ts_b, v.loop, h.file_len);
    *h.cursor = tp.cursor;
    StartPlay(h, tp, tp.from, tp.to);
}

void ZoneToggleLoop(const ZoneWaveHost& h)
{
    ZoneEditView& v = *h.view;
    v.loop = !v.loop;
    // While it plays: what plays loops from now on (as if Play had been pressed with Loop), or stops looping.
    if (!v.loop) {
        v.tp.loops = false;
    } else if (ZonePlaying(h)) {
        const EditTransport re = PlanEditPlay(v.tp.from, v.tp.to, v.has_tsel, v.ts_a, v.ts_b, true, h.file_len);
        v.tp.loops = re.loops;
        v.tp.loop_a = re.loop_a;
        v.tp.loop_b = re.loop_b;
    }
    ShowLibraryToast(v.loop ? std::string("Loop on: ") + (v.has_tsel ? "the time selection" : "the whole file") + " repeats" : "Loop off",
                     false);
}

void ZoneDelete(const ZoneWaveHost& h)
{
    ZoneEditView& v = *h.view;
    const SoundData& d = Shown(h);
    const ZoneSel    sel = SelValid(d, v.sel);
    if (sel.empty()) {
        ShowLibraryToast("Nothing selected", false);
        return;
    }
    // 13-62h (v5): a Gap whose left zone goes too (a whole item deleted) goes with it, it is given to nobody.
    int nz = 0, ng = 0, gone = 0;
    for (const ZoneRef& r : sel) {
        if (!ZoneOf(d, r)->IsGap()) ++nz;
        else if (SelHas(sel, GapLeft(d, r))) ++gone;
        else ++ng;
    }
    std::string toast;
    const ZoneSel item = SelItem(d, sel);
    if (!item.empty()) toast = ItemNames(d, item, h.names) + " removed with its Gap" + (gone > 1 ? "s" : "") + " (empty space remains)";
    else if (nz) toast = std::to_string(nz) + (nz > 1 ? " zones" : " zone") + (gone ? " and " + std::to_string(gone) + (gone > 1 ? " Gaps" : " Gap") : std::string()) +
                         " removed (empty space remains)";
    if (ng) toast += (toast.empty() ? "" : " \xC2\xB7 ") + std::to_string(ng) + (ng > 1 ? " Gaps" : " Gap") + " given back to the zone on the left";
    Write(h, "Delete zones", [sel](SoundData& x, const ZoneContext& c, ZoneSel* s) {
        ZoneSel io = sel;
        const bool ch = DeleteZones(x, io, c);
        if (s) *s = io;
        return ch;
    }, toast, true);
    v.sel.clear();
}

void ZoneHeal(const ZoneWaveHost& h)
{
    ZoneEditView& v = *h.view;
    const ZoneSel sel = SelValid(Shown(h), v.sel);
    if (sel.size() < 2) {
        ShowLibraryToast("Heal: select the zones to heal first (Ctrl+click adds one)", false);
        return;
    }
    const int  cuts = std::max(1, HealCutCount(Stored(h), sel));
    const auto mixed = std::make_shared<bool>(false);
    const bool wrote = Write(
        h, "Heal zones",
        [sel, mixed](SoundData& x, const ZoneContext& c, ZoneSel* s) {
            ZoneSel io = sel;
            bool    mx = false;
            const bool ch = HealZones(x, io, c, &mx);
            *mixed = mx;
            if (s) *s = io;
            return ch;
        },
        [&h, cuts, mixed](const SoundData& after, const ZoneSel& out) {
            const EditZone* z = out.empty() ? nullptr : ZoneOf(after, out.front());
            std::string     t = "Healed " + std::to_string(cuts) + (cuts > 1 ? " cuts" : " cut");
            if (z) t += ": one " + PartNameOf(h, z->part) + " zone, " + ZoneRangeMs(z->a, z->b);
            if (*mixed) t += " (it keeps the first zone's part)";
            return t;
        },
        true);
    if (!wrote) ShowLibraryToast("The selected zones are not edge to edge: nothing to heal", false);
}

void ZoneSplit(const ZoneWaveHost& h)
{
    const double t = *h.cursor;
    if (t < 0.0) {
        ShowLibraryToast("Click where to split first (the edit cursor)", false);
        return;
    }
    const SoundData& d = Shown(h);
    const EditZone*  z = ZoneOf(d, ZoneAt(d, t, h.tl));
    if (!z) {
        ShowLibraryToast("No zone under the cursor: nothing to split", false);
        return;
    }
    if (z->IsGap()) {
        ShowLibraryToast("The cursor is in a Gap: split a zone", false);
        return;
    }
    const std::string pn = PartNameOf(h, z->part);
    if (!Write(h, "Split zone", [t](SoundData& x, const ZoneContext& c, ZoneSel* s) { return SplitZoneAt(x, t, c, s); },
               "Split at " + MsText(t) + ": two " + pn + " zones, edge to edge (fades: At cuts)", true))
        ShowLibraryToast("Too close to the zone's edge", false);
}

// 13-67a: REAPER's 40541 "Item: Set snap offset to cursor": the selected part zones holding the edit cursor (none
// selected: the part zone under it) get their own snap offset there (a hand snap offset), one undo point.
void ZoneSnapToCursor(const ZoneWaveHost& h)
{
    const double t = *h.cursor;
    if (t < 0.0) {
        ShowLibraryToast("Click where the snap offset goes first (the edit cursor)", false);
        return;
    }
    const SoundData& d = Shown(h);
    ZoneSel          on;
    const ZoneSel    sel = SelValid(d, h.view->sel);
    for (const ZoneRef& r : sel) {
        const EditZone* z = ZoneOf(d, r);
        if (z && !z->IsGap() && t >= z->a && t <= z->b && (h.tl < 0 || ZoneTimeline(d, r.frame) == NormTl(d, h.tl))) on.push_back(r);
    }
    if (sel.empty()) {
        const ZoneRef   r = ZoneAt(d, t, h.tl);
        const EditZone* z = ZoneOf(d, r);
        if (z && !z->IsGap()) on.push_back(r);
    }
    if (on.empty()) {
        // 13-67b (13-67a review): only Gaps selected: a Gap has no snap offset.
        bool only_gaps = !sel.empty();
        for (const ZoneRef& r : sel)
            if (const EditZone* z = ZoneOf(d, r); z && !z->IsGap()) only_gaps = false;
        if (sel.empty()) {  // review: nothing selected, the cursor on a Gap: same answer
            const EditZone* g = ZoneOf(d, ZoneAt(d, t, h.tl));
            only_gaps = g && g->IsGap();
        }
        ShowLibraryToast(sel.empty() && !only_gaps ? "No zone under the cursor: no snap offset to set"
                         : only_gaps ? "A Gap has no snap offset: select a zone"
                                     : "The edit cursor is not inside the selected zone: no snap offset to set",
                         false);
        return;
    }
    const std::string what = on.size() == 1 ? NameOfZone(h, d, on.front()) : std::to_string(on.size()) + " zones";
    if (!Write(h, "Snap offset", [on, t](SoundData& x, const ZoneContext& c, ZoneSel*) {
                   bool any = false;
                   for (const ZoneRef& r : on) any = SetZoneSnap(x, r, t, c) || any;
                   return any;
               },
               what + ": snap offset at " + MsText(t) + " (by hand; double-click it: automatic again)"))
        ShowLibraryToast("The snap offset is already there", false);
}

// 13-67a: REAPER's 40172 "Go to previous marker / project start": the edit cursor back to the nearest earlier stop
// (the time selection's start, a zone's start), the file start last. View state (no undo point).
void ZonePrevStop(const ZoneWaveHost& h)
{
    const ZoneEditView& v = *h.view;
    const double        cur = *h.cursor;
    if (!(cur > 0.0)) {
        *h.cursor = 0.0;
        if (cur == 0.0) ShowLibraryToast("Already at the file start", false);  // review: no cursor yet: placed, no toast
        return;
    }
    *h.cursor = PrevEditStop(Shown(h), h.tl, cur, v.has_tsel, v.ts_a);
    // Keep the cursor in view (same zoom), as Next / Previous variation do.
    const double vl = *h.view_len;
    if (vl > 0.0 && (*h.cursor < *h.view_start || *h.cursor > *h.view_start + vl))
        *h.view_start = std::max(0.0, std::min(*h.cursor - vl * 0.1, std::max(0.0, h.file_len - vl)));
}

void ZoneTrimAt(const ZoneWaveHost& h, int side)
{
    const double t = *h.cursor;
    if (t < 0.0) {
        ShowLibraryToast("Click where to trim first (the edit cursor)", false);
        return;
    }
    const ZoneSel     sel = SelValid(Shown(h), h.view->sel);
    const std::string what = side < 0 ? "left" : "right";
    if (!Write(h, side < 0 ? "Trim left of cursor" : "Trim right of cursor",
               [sel, t, side](SoundData& x, const ZoneContext& c, ZoneSel* s) { return TrimZonesAt(x, sel, t, side, c, s); },
               "Trim " + what + " of the cursor (" + MsText(t) + ")", true))
        ShowLibraryToast("No zone under the cursor: nothing to trim", false);
}

void ZoneTrimToTsel(const ZoneWaveHost& h)
{
    const ZoneEditView& v = *h.view;
    if (!v.has_tsel) {
        ShowLibraryToast("Make a time selection on the ruler first", false);
        return;
    }
    const ZoneSel sel = SelValid(Shown(h), v.sel);
    const double  a = v.ts_a, b = v.ts_b;
    if (!Write(h, "Trim to time selection", [sel, a, b](SoundData& x, const ZoneContext& c, ZoneSel* s) { return TrimZonesTo(x, sel, a, b, c, s); },
               "Trimmed to the time selection " + ZoneRangeMs(a, b), true))
        ShowLibraryToast("Nothing to trim: no zone crosses the time selection", false);
}

void ZoneGiveTo(const ZoneWaveHost& h, const std::string& part)
{
    const SoundData& d = Shown(h);
    ZoneSel          sel;
    for (const ZoneRef& r : SelValid(d, h.view->sel))
        if (!ZoneOf(d, r)->IsGap() && ZoneOf(d, r)->part != part) sel.push_back(r);
    if (sel.empty()) {
        ShowLibraryToast("Already " + PartNameOf(h, part), false);
        return;
    }
    *h.part_sel = part;
    Write(h, "Zones to part", [sel, part](SoundData& x, const ZoneContext& c, ZoneSel*) { return GiveZonesPart(x, sel, part, c); },
          std::to_string(sel.size()) + (sel.size() > 1 ? " zones" : " zone") + " \xE2\x86\x92 " + PartNameOf(h, part));
}

void ZoneSelectPart(const ZoneWaveHost& h, const std::string& part)
{
    h.view->sel = ZonesOfPart(Shown(h), part);
    *h.part_sel = part;
    ShowLibraryToast(PartNameOf(h, part) + ": its " + std::to_string(h.view->sel.size()) + " zones selected", false);
}

void ZoneStepVariation(const ZoneWaveHost& h, int dir)
{
    // The mockup's stepVariation over the Variations strip's blocks (on variations with zones; an off one is out of
    // the zone model): its first zone selected, the cursor at its start, the view centred on it, same zoom.
    const SoundData&                d = Shown(h);
    if (d.mode == DetectMode::Takes) {  // 13-62i: each take is a variation on its own timeline: the next / previous take
        const int n = static_cast<int>(d.frames.size());
        int       k = h.view->cur_tl < 0 ? (dir > 0 ? -1 : n) : h.view->cur_tl;
        for (k += dir; k >= 0 && k < n; k += dir)
            if (d.frames[static_cast<size_t>(k)].on && !d.frames[static_cast<size_t>(k)].zones.empty()) break;
        if (k < 0 || k >= n) {
            ShowLibraryToast(dir > 0 ? "Already the last take" : "Already the first take", false);
            return;
        }
        h.view->cur_tl = k;
        h.view->has_tsel = false;
        const ZoneRef first = FirstZoneOf(d, k);
        h.view->sel.clear();
        if (const EditZone* fz = ZoneOf(d, first)) {
            h.view->sel = {first};
            if (!fz->IsGap()) *h.part_sel = fz->part;
            *h.cursor = fz->a;
        }
        ShowLibraryToast("Take " + std::to_string(k + 1) + " of " + std::to_string(n), false);
        return;
    }
    const std::vector<ZoneVarBlock> blocks = ZoneVarBlocks(d);
    if (blocks.empty()) return;
    const int i = ZoneStepBlock(blocks, ZoneStepRef(d, SelValid(d, h.view->sel), *h.cursor), dir);
    if (i < 0) {
        ShowLibraryToast(dir > 0 ? "Already the last variation" : "Already the first variation", false);
        return;
    }
    const ZoneVarBlock& b = blocks[static_cast<size_t>(i)];
    const ZoneRef       first = FirstZoneOf(d, b.frame);
    h.view->sel.clear();
    if (const EditZone* fz = ZoneOf(d, first)) {
        h.view->sel = {first};
        if (!fz->IsGap()) *h.part_sel = fz->part;
    }
    *h.cursor = b.a;  // its first zone's start
    const double w = *h.view_len;
    if (w > 0.0 && h.file_len > 0.0) *h.view_start = Clamp((b.a + b.b) / 2.0 - w / 2.0, 0.0, std::max(0.0, h.file_len - w));
    // Numbered as the strip numbers it (the variation's own number), of the Sound's variations.
    ShowLibraryToast("Variation " + std::to_string(b.frame + 1) + " of " + std::to_string(d.frames.size()) + ": centred (same zoom)", false);
}

}  // namespace rav

#endif  // _WIN32
