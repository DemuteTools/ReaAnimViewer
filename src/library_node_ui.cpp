// SPDX-License-Identifier: MIT
//
// See library_node_ui.h. Every gesture is one ModifySoundStore undo point ("RAV: <gesture>")
// followed by the Library toast with Undo; live drags (faders, offsets, trims, dB, detection
// fields, the threshold line) preview a local draft and write once on release. After a write
// every SoundLibrary / SoundNode reference taken before it is stale: the panel stops drawing
// for this frame (P.changed). The audition / pre-edit logic is sound_audition_plan.h (pure).

#include "library_node_ui.h"

#ifdef _WIN32

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <limits>
#include <set>

#include "library_tree_edit.h"
#include "library_tree_ui.h"
#include "library_view_ui.h"
#include "reaper_api.h"
#include "shortcuts.h"
#include "sound_audition.h"
#include "sound_audition_plan.h"
#include "sound_draw.h"
#include "sound_entries.h"
#include "sound_peaks.h"
#include "sound_place.h"
#include "sound_populate.h"
#include "sound_project.h"
#include "sound_record.h"
#include "sound_switch.h"
#include "ui_theme.h"

namespace rav {
namespace {

constexpr double kMinDb = -60.0, kMaxDb = 12.0;
constexpr double kNewFrameSeconds = 0.25;  // a frame added by double-click in Edit
constexpr const char kNoMediaTip[] = "Arrives with the Media Explorer";
constexpr const char kTakesTip[] = "Not in Takes mode";

struct PanelState {
    AuditionHistory       hist;
    AuditionPlan          plan;
    std::string           plan_lib;
    NodePanelHooks        hooks;
    bool                  changed = false;
    // The shown Sound's media, re-read on a project change.
    std::string           media_uid;
    int                   media_scc = -1;
    SoundSource           media_src;
    SoundMedia            media;
    // A live sound edit: `draft` is shown instead of the store until it is written.
    bool                  live = false;
    std::string           draft_uid;
    SoundData             snap, draft;
    // A live volume (fader / dB field) or Blend offset.
    std::string           vol_key;
    double                vol_live = 0.0;
    std::string           off_uid;
    double                off0 = 0.0, off_live = 0.0;
    double                off_scale = 0.0;  // the lanes' ms at the drag's start (fixed while dragging)
    float                 off_dx = 0.0f;
    // The Edit overlay.
    bool                  edit_open = false;
    bool                  edit_focus = false;
    int                   edit_opened = -1;  // the frame Edit opened (its E is not read again then)
    std::string           edit_lib, edit_uid;
    double                view_start = 0.0, view_len = 0.0;
    std::vector<int>      sel;
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
    bool                  norm_loaded = false;
    double                norm_db = -1.0;
    bool                  norm_whole = false;
};
PanelState P;

double DbToGain(double db) { return std::pow(10.0, db / 20.0); }

ImU32 WithAlpha(ImU32 c, int a) { return (c & ~IM_COL32_A_MASK) | (static_cast<ImU32>(a) << IM_COL32_A_SHIFT); }

void Tip(const char* text)
{
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", text);
}

void OpenEdit(const std::string& lib_uid, const std::string& uid)
{
    P.edit_open = true;
    P.edit_focus = true;
    P.edit_opened = ImGui::GetFrameCount();
    P.edit_lib = lib_uid;
    P.edit_uid = uid;
    P.view_start = 0.0;
    P.view_len = 0.0;
    P.sel.clear();
}

// ---- gestures -------------------------------------------------------------------------------------

bool Gesture(const std::string& name, const std::function<bool(SoundStore&)>& edit, const std::string& toast = {})
{
    const std::string desc = "RAV: " + name;
    if (!ModifySoundStore(nullptr, desc.c_str(), edit)) return false;
    P.changed = true;
    ShowLibraryToast(toast.empty() ? name : toast, true, desc);
    return true;
}

bool EditNode(const std::string& name, const std::string& lib_uid, const std::string& uid,
              const std::function<bool(SoundNode&)>& f, const std::string& toast = {})
{
    return Gesture(
        name,
        [&](SoundStore& s) {
            SoundLibrary* l = FindLibrary(s, lib_uid);
            SoundNode*    n = l ? FindNode(*l, uid) : nullptr;
            return n && f(*n);
        },
        toast);
}

const SoundData& Shown(const SoundNode& n) { return (P.live && P.draft_uid == n.uid) ? P.draft : n.sound; }

// Starts (or continues) a live edit of the Sound: the draft restarts from the snapshot each call.
SoundData& Live(const SoundNode& n)
{
    if (!P.live || P.draft_uid != n.uid) {
        P.live = true;
        P.draft_uid = n.uid;
        P.snap = n.sound;
    }
    P.draft = P.snap;
    return P.draft;
}

void CommitLive(const std::string& name, const std::string& lib_uid, const std::string& uid)
{
    if (!P.live || P.draft_uid != uid) return;
    const SoundData d = P.draft;
    P.live = false;
    EditNode(name, lib_uid, uid, [&d](SoundNode& n) {
        n.sound = d;
        return true;
    });
}

void CancelLive() { P.live = false; }

// ---- media and detection --------------------------------------------------------------------------

bool SameSource(const SoundSource& a, const SoundSource& b)
{
    return a.kind == b.kind && a.item_guid == b.item_guid && a.asset.last_path == b.asset.last_path;
}

const SoundMedia& MediaOf(const SoundNode& n)
{
    const int scc = GetProjectStateChangeCount(nullptr);
    if (P.media_uid != n.uid || P.media_scc != scc || !SameSource(P.media_src, n.sound.source)) {
        P.media_uid = n.uid;
        P.media_scc = scc;
        P.media_src = n.sound.source;
        ReadSoundMedia(n.sound.source, &P.media);
    }
    return P.media;
}

std::vector<SoundFrame> Detected(const SoundMedia& m, DetectMode mode, const SilenceParams& sp)
{
    PeakData   pk;
    const bool has = mode == DetectMode::Silence && RequestPeaks(m.path, &pk) == PeakState::Ready;
    return DetectFrames(m.facts, mode, sp, has ? &pk : nullptr);
}

// A re-detection that keeps the hand frames (decision 7).
void Redetect(SoundData& d, const SoundMedia& m, const std::vector<SoundFrame>& previous)
{
    d.frames = MergeHandFrames(Detected(m, d.mode, d.silence), previous, d.mode == DetectMode::Takes);
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

void PlayNode(const std::string& lib_uid, const std::string& uid)
{
    const SoundStore&   store = GetSoundStore(nullptr);
    const SoundLibrary* lib = FindLibrary(store, lib_uid);
    if (!lib) return;
    AuditionPlan           plan = PlanAudition(store, *lib, uid, P.hist.Picker());
    std::vector<PlayVoice> pv;
    std::vector<AuditionVoice> played;
    for (const AuditionVoice& v : plan.voices) {
        const SoundNode* s = FindNode(*lib, v.node);
        SoundMedia       m;
        if (!s || !ReadSoundMedia(s->sound.source, &m)) continue;
        PlayVoice p;
        p.path = VariationPath(m, s->sound, v.variation);
        p.start_s = v.start_s;
        p.length_s = v.length_s;
        p.delay_s = v.delay_ms / 1000.0;
        p.gain = DbToGain(v.gain_db);
        pv.push_back(p);
        played.push_back(v);
    }
    std::string why = plan.voices.empty() ? std::string("nothing to play") : std::string();
    if (!pv.empty() && PlayVoices(pv, uid, &why)) {
        plan.voices = played;
        plan.played = PlayedText(*lib, played);  // only the voices that play
        P.plan = plan;
        P.plan_lib = lib_uid;
        return;
    }
    if (!plan.voices.empty() && pv.empty()) why = "the sound files are not found";
    // Fallback (decision 2): the selected Sound's one variation.
    const SoundNode* n = FindNode(*lib, uid);
    if (n && n->type == SoundNodeType::Sound) {
        const std::vector<int> act = ActiveVariations(n->sound);
        SoundMedia             m;
        if (!act.empty() && ReadSoundMedia(n->sound.source, &m)) {
            const SoundFrame& f = n->sound.frames[static_cast<size_t>(act.front())];
            if (AuditionFile(VariationPath(m, n->sound, act.front()), f.start, f.length, uid,
                             DbToGain(GainFromRoot(*lib, *n) + f.db))) {
                AuditionPlan one;
                one.root = uid;
                AuditionVoice v;
                v.node = uid;
                v.variation = act.front();
                v.length_s = f.length;
                one.voices = {v};
                one.played = n->name;
                P.plan = one;
                P.plan_lib = lib_uid;
                return;
            }
        }
    }
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

void PlayFrame(const SoundLibrary& lib, const SoundNode& n, const SoundMedia& m, int i)
{
    const SoundData& d = Shown(n);
    if (i < 0 || static_cast<size_t>(i) >= d.frames.size()) return;
    const SoundFrame& f = d.frames[static_cast<size_t>(i)];
    if (!AuditionFile(VariationPath(m, d, i), f.start, f.length, "frame:" + n.uid, DbToGain(GainFromRoot(lib, n) + f.db)))
        ShowLibraryToast("Cannot play: the sound file is not read", false);
    else P.frame_start = f.start;
}

// The playhead in a Sound's source seconds (or < 0): its frame, or its whole file.
double SoundPlayhead(const SoundNode& n)
{
    if (!AuditionPlaying()) return -1.0;
    const std::string& tag = AuditionTag();
    if (tag == "whole:" + n.uid) return AuditionPosition();
    if (tag == "frame:" + n.uid) return P.frame_start + AuditionPosition();
    return -1.0;
}

// ---- small controls -------------------------------------------------------------------------------

std::string NodePathText(const SoundLibrary& lib, const SoundNode& n)
{
    std::vector<std::string> names;
    const SoundNode*         p = &n;
    for (int guard = 0; p && guard < 64; ++guard) {
        names.push_back(p->name);
        if (p->parent.empty()) break;
        p = FindNode(lib, p->parent);
    }
    std::string s = lib.name;
    for (auto it = names.rbegin(); it != names.rend(); ++it) s += " \xE2\x80\xBA " + *it;
    return s;
}

// A volume fader -60..+12 dB with its dB field; true when a value is to be written (*out).
bool VolumeControl(const char* id, const std::string& key, double stored, double* out, float fader_w)
{
    ImGui::PushID(id);
    const bool live = P.vol_key == key;
    float      v = static_cast<float>(live ? P.vol_live : stored);
    bool       commit = false;
    ImGui::SetNextItemWidth(std::max(40.0f, fader_w));
    ImGui::SliderFloat("##fader", &v, static_cast<float>(kMinDb), static_cast<float>(kMaxDb), "");
    if (ImGui::IsItemActive()) {
        P.vol_key = key;
        P.vol_live = v;
    }
    if (ImGui::IsItemDeactivated() && P.vol_key == key) {
        P.vol_key.clear();
        if (std::fabs(v - stored) > 1e-4) {
            *out = v;
            commit = true;
        }
    }
    ImGui::SameLine();
    double d = P.vol_key == key ? P.vol_live : stored;
    switch (ui::DragNumber("##db", &d, 0.1, 1, "dB", 48.0f)) {
    case ui::DragNumberEvent::Live:
        P.vol_key = key;
        P.vol_live = std::clamp(d, kMinDb, kMaxDb);
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

int AvoidControl(const char* id, int stored)
{
    double v = stored;
    if (ui::DragNumber(id, &v, 0.05, 0, "", 40.0f) == ui::DragNumberEvent::Commit) {
        const int n = std::clamp(static_cast<int>(std::lround(v)), 0, 32);
        if (n != stored) return n;
    }
    return -1;
}

void Hatch(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 col)
{
    dl->PushClipRect(a, b, true);
    for (float x = a.x - (b.y - a.y); x < b.x; x += 6.0f) dl->AddLine(ImVec2(x, b.y), ImVec2(x + (b.y - a.y), a.y), col);
    dl->PopClipRect();
}

// ---- waveform -------------------------------------------------------------------------------------

struct WaveDrag {
    ImGuiID id = 0;
    enum Kind { None, Pending, Trim, Db, Thr } kind = None;
    int     frame = -1;
    int     edge = 0;  // -1 left, +1 right
    double  s0 = 0.0, l0 = 0.0, db0 = 0.0;
    ImVec2  m0;
    double  last_s = 0.0, last_l = 0.0, last_db = 0.0, last_thr = 0.0;
    bool    moved = false;
};
WaveDrag g_wd;

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
    WaveformEvents ev;
    ImGui::PushID(id);
    const ImGuiID gid = ImGui::GetID("##wave");
    const ImGuiIO& io = ImGui::GetIO();
    const float  w = std::max(8.0f, o.size.x > 0.0f ? o.size.x : ImGui::GetContentRegionAvail().x);
    const float  h = std::max(16.0f, o.size.y);
    const float  rh = o.ruler ? 16.0f : 0.0f;
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##wave", ImVec2(w, h + rh), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    const bool hovered = ImGui::IsItemHovered();
    const ImVec2 a(p0.x, p0.y + rh), b(p0.x + w, p0.y + rh + h);
    ev.min = a;
    ev.max = b;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(a, b, ui::kBg, ui::kRadiusSm);
    const double vs = o.view_start;
    const double vl = o.view_len > 0.0 ? o.view_len : o.source_len;
    if (!(vl > 0.0)) {
        dl->AddText(ImVec2(a.x + 8.0f, a.y + h * 0.5f - 7.0f), ui::kFaint, "No audio");
        ImGui::PopID();
        return ev;
    }
    auto X = [&](double t) { return static_cast<float>(a.x + (t - vs) / vl * w); };
    auto T = [&](float x) { return vs + static_cast<double>(x - a.x) / w * vl; };
    const float mid = (a.y + b.y) * 0.5f, hh = h * 0.5f - 2.0f;
    dl->PushClipRect(a, b, true);

    // Frames: accent ~9 % (on), ~18 % (selected), off hatched on the stroke grey.
    for (size_t i = 0; i < frames.size(); ++i) {
        const SoundFrame& f = frames[i];
        const ImVec2 fa(X(f.start), a.y), fb(X(f.start + f.length), b.y);
        if (fb.x < a.x || fa.x > b.x) continue;
        const bool sel = std::find(selection.begin(), selection.end(), static_cast<int>(i)) != selection.end();
        if (!f.on) {
            dl->AddRectFilled(fa, fb, WithAlpha(ui::kStroke, 0xFF));
            Hatch(dl, fa, fb, ui::kFaint);
        } else {
            dl->AddRectFilled(fa, fb, WithAlpha(ui::kAccent, sel ? 0x2E : 0x17));
        }
    }
    // Peaks, each column scaled by its frame's dB; the part past full scale in the clip colour.
    PeakData pk;
    if (ReadPeaks(path, vs, vl, w / vl, &pk)) {
        for (size_t i = 0; i < pk.max_abs.size(); ++i) {
            const double t = vs + (static_cast<double>(i) + 0.5) / pk.rate;
            const int    fi = FrameAt(frames, t);
            const double g = fi >= 0 ? DbToGain(frames[static_cast<size_t>(fi)].db) : 1.0;
            const double amp = pk.max_abs[i] * g;
            const float  x = a.x + static_cast<float>(i) * w / static_cast<float>(pk.max_abs.size());
            const float  y = static_cast<float>(std::min(amp, 1.0)) * hh;
            const ImU32  col = amp > 1.0 ? kLibMissing
                               : (fi >= 0 && frames[static_cast<size_t>(fi)].on) ? ui::kText
                                                                                   : ui::kFaint;
            dl->AddLine(ImVec2(x, mid - y), ImVec2(x, mid + y + 1.0f), col);
        }
    } else {
        ev.reading = RequestPeaks(path, nullptr) == PeakState::Building;
        dl->AddText(ImVec2(a.x + 8.0f, mid - 7.0f), ui::kFaint, ev.reading ? "Reading waveform\xE2\x80\xA6" : "Waveform unavailable");
    }
    // Frame edges: solid when clamped, else the accent line.
    for (size_t i = 0; i < frames.size(); ++i) {
        const bool solid = i < o.solid_edges.size() && o.solid_edges[i];
        const ImU32 col = solid ? ui::kAccent : ui::kAccentLine;
        dl->AddLine(ImVec2(X(frames[i].start), a.y), ImVec2(X(frames[i].start), b.y), col);
        dl->AddLine(ImVec2(X(frames[i].start + frames[i].length), a.y), ImVec2(X(frames[i].start + frames[i].length), b.y), col);
    }
    // The threshold line (Silence).
    const bool thr = std::isfinite(o.threshold_db);
    float      thr_y = 0.0f;
    if (thr) {
        thr_y = static_cast<float>(std::min(1.0, DbToGain(o.threshold_db))) * hh;
        dl->AddLine(ImVec2(a.x, mid - thr_y), ImVec2(b.x, mid - thr_y), ui::kWarn);
        dl->AddLine(ImVec2(a.x, mid + thr_y), ImVec2(b.x, mid + thr_y), WithAlpha(ui::kWarn, 0x60));
    }
    if (playhead_s >= vs && playhead_s <= vs + vl) dl->AddLine(ImVec2(X(playhead_s), a.y), ImVec2(X(playhead_s), b.y), ui::kWhite, 1.5f);
    dl->PopClipRect();
    dl->AddRect(a, b, ui::kStroke, ui::kRadiusSm);

    // The ruler: ms below 2 s shown, else s.
    if (o.ruler) {
        const double steps[] = {0.001, 0.005, 0.01, 0.05, 0.1, 0.25, 0.5, 1, 2, 5, 10, 30, 60};
        double       st = steps[0];
        for (double s : steps) {
            st = s;
            if (s / vl * w >= 60.0) break;
        }
        for (double t = std::ceil(vs / st) * st; t <= vs + vl; t += st) {
            const float x = X(t);
            dl->AddLine(ImVec2(x, p0.y + rh - 5.0f), ImVec2(x, p0.y + rh), ui::kFaint);
            char lab[32];
            if (st < 1.0) std::snprintf(lab, sizeof(lab), "%.0f ms", t * 1000.0);
            else std::snprintf(lab, sizeof(lab), "%.0f s", t);
            dl->AddText(ImVec2(x + 2.0f, p0.y), ui::kFaint, lab);
        }
    }

    // ---- interaction ----
    const ImVec2 m = io.MousePos;
    const int    under = hovered ? FrameAt(frames, T(m.x)) : -1;
    int          edge_frame = -1, edge_side = 0;
    if (hovered && o.gestures) {
        for (size_t i = 0; i < frames.size(); ++i) {
            if (std::fabs(m.x - X(frames[i].start)) <= 4.0f) {
                edge_frame = static_cast<int>(i);
                edge_side = -1;
            } else if (std::fabs(m.x - X(frames[i].start + frames[i].length)) <= 4.0f) {
                edge_frame = static_cast<int>(i);
                edge_side = 1;
            }
        }
    }
    const bool near_thr = hovered && thr && (std::fabs(m.y - (mid - thr_y)) <= 4.0f || std::fabs(m.y - (mid + thr_y)) <= 4.0f);
    if (near_thr) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
    else if (edge_frame >= 0) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);

    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        g_wd = WaveDrag{};
        g_wd.id = gid;
        g_wd.m0 = m;
        if (near_thr) {
            g_wd.kind = WaveDrag::Thr;
        } else if (edge_frame >= 0) {
            g_wd.kind = WaveDrag::Trim;
            g_wd.frame = edge_frame;
            g_wd.edge = edge_side;
        } else {
            g_wd.kind = WaveDrag::Pending;
            g_wd.frame = under;
        }
        if (g_wd.frame >= 0) {
            g_wd.s0 = frames[static_cast<size_t>(g_wd.frame)].start;
            g_wd.l0 = frames[static_cast<size_t>(g_wd.frame)].length;
            g_wd.db0 = frames[static_cast<size_t>(g_wd.frame)].db;
        }
    }
    if (g_wd.id == gid && g_wd.kind != WaveDrag::None) {
        const bool down = ImGui::IsMouseDown(ImGuiMouseButton_Left);
        if (g_wd.kind == WaveDrag::Pending && o.gestures && g_wd.frame >= 0 && std::fabs(m.y - g_wd.m0.y) > 3.0f)
            g_wd.kind = WaveDrag::Db;
        const bool moved = std::fabs(m.x - g_wd.m0.x) > 1.0f || std::fabs(m.y - g_wd.m0.y) > 1.0f;
        if (moved && (g_wd.kind == WaveDrag::Trim || g_wd.kind == WaveDrag::Thr)) g_wd.moved = true;
        if (down) {
            if (g_wd.kind == WaveDrag::Trim && g_wd.moved) {
                const double t = std::clamp(T(m.x), 0.0, o.source_len);
                if (g_wd.edge < 0) {
                    g_wd.last_s = std::min(t, g_wd.s0 + g_wd.l0);
                    g_wd.last_l = g_wd.s0 + g_wd.l0 - g_wd.last_s;
                } else {
                    g_wd.last_s = g_wd.s0;
                    g_wd.last_l = std::max(0.0, t - g_wd.s0);
                }
                ev.trim = g_wd.frame;
                ev.trim_start = g_wd.last_s;
                ev.trim_len = g_wd.last_l;
            } else if (g_wd.kind == WaveDrag::Db) {
                g_wd.last_db = std::clamp(g_wd.db0 - (m.y - g_wd.m0.y) * 0.1, -60.0, 24.0);
                ev.db_frame = g_wd.frame;
                ev.db = g_wd.last_db;
                ImGui::SetTooltip("%+.1f dB", g_wd.last_db);
            } else if (g_wd.kind == WaveDrag::Thr && g_wd.moved) {
                const double amp = std::max(1e-4, static_cast<double>(std::fabs(mid - m.y) / std::max(1.0f, hh)));
                g_wd.last_thr = std::clamp(20.0 * std::log10(amp), -90.0, 0.0);
                ev.threshold_live = true;
                ev.threshold_db = g_wd.last_thr;
                ImGui::SetTooltip("%.1f dB", g_wd.last_thr);
            }
        } else {
            switch (g_wd.kind) {
            case WaveDrag::Trim:
                if (!g_wd.moved) break;
                ev.trim = g_wd.frame;
                ev.trim_start = g_wd.last_s;
                ev.trim_len = g_wd.last_l;
                ev.trim_done = true;
                break;
            case WaveDrag::Db:
                ev.db_frame = g_wd.frame;
                ev.db = g_wd.last_db;
                ev.db_done = true;
                break;
            case WaveDrag::Thr:
                if (!g_wd.moved) break;
                ev.threshold_db = g_wd.last_thr;
                ev.threshold_done = true;
                break;
            case WaveDrag::Pending:
                if (io.KeyAlt && o.gestures && g_wd.frame >= 0) ev.toggled = g_wd.frame;
                else if (!io.KeyAlt) {
                    ev.clicked = g_wd.frame;
                    ev.clicked_empty = g_wd.frame < 0;
                    ev.ctrl = io.KeyCtrl;
                }
                break;
            default: break;
            }
            g_wd = WaveDrag{};
        }
    }
    if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        ev.double_clicked = under >= 0 ? under : -2;
        ev.double_at = T(m.x);
    }
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) ev.right_clicked = true;
    if (hovered && o.gestures && under >= 0 && !io.WantTextInput && !io.KeyCtrl && !io.KeyAlt && !io.KeyShift &&
        ImGui::IsKeyPressed(ImGuiKey_S, false)) {
        ev.split = under;
        ev.split_at = T(m.x);
    }
    if (hovered && o.wheel && io.MouseWheel != 0.0f) {
        if (io.KeyCtrl) {
            ev.zoom = std::pow(0.8, static_cast<double>(io.MouseWheel));
            ev.zoom_at = T(m.x);
        } else {
            ev.scroll_s = -static_cast<double>(io.MouseWheel) * vl * 0.1;
        }
    }
    ImGui::PopID();
    return ev;
}

void SetNodePanelHooks(const NodePanelHooks& hooks) { P.hooks = hooks; }

bool LibraryNodeTakesKey(unsigned vk, bool ctrl, bool shift, bool alt)
{
    if (ctrl || shift || alt) return false;
    const int  f = ImGui::GetCurrentContext() ? ImGui::GetFrameCount() : -100;
    const bool edit = P.edit_open && P.edit_frame >= f - 1;
    if (edit) return vk == 0x20 || vk == 0x1B || vk == 'E' || vk == 'S';
    const bool sound = P.sound_frame >= f - 1;
    return sound && LibraryViewFocused() && (vk == 'E' || vk == 'S');
}

namespace {

// ---- Sound panel pieces ---------------------------------------------------------------------------

struct SoundCtx {
    std::string lib_uid, uid;
};

// Applies a waveform's events to the Sound (live draft or one write). `fi` maps the waveform's
// frame index to the Sound's (Takes lanes show one frame each); `take_len` > 0 = a Takes lane.
void HandleWave(const WaveformEvents& ev, const SoundLibrary& lib, const SoundNode& n, const SoundMedia& m,
                const std::vector<int>& fi, double take_len, bool in_edit)
{
    const SoundCtx c{lib.uid, n.uid};
    auto           map = [&](int i) { return i >= 0 && static_cast<size_t>(i) < fi.size() ? fi[static_cast<size_t>(i)] : -1; };
    const DetectMode mode = n.sound.mode;
    if (ev.trim >= 0) {
        const int i = map(ev.trim);
        SoundData& d = Live(n);
        if (take_len > 0.0) TrimTakeFrame(d, i, ev.trim_start, ev.trim_len, take_len);
        else TrimFrame(d, i, ev.trim_start, ev.trim_len, m.source_len);
        if (ev.trim_done) CommitLive("Trim variation", c.lib_uid, c.uid);
        return;
    }
    if (ev.db_frame >= 0) {
        const int i = map(ev.db_frame);
        SetFrameDb(Live(n), i, ev.db);
        if (ev.db_done) CommitLive("Variation level", c.lib_uid, c.uid);
        return;
    }
    if (ev.toggled >= 0) {
        const int i = map(ev.toggled);
        EditNode("Variation on / off", c.lib_uid, c.uid, [i](SoundNode& x) {
            if (i < 0 || static_cast<size_t>(i) >= x.sound.frames.size()) return false;
            SetFrameOn(x.sound, i, !x.sound.frames[static_cast<size_t>(i)].on);
            return true;
        });
        return;
    }
    if (ev.split >= 0) {
        if (!FrameEditAllowed(mode, FrameEdit::Split)) {
            ShowLibraryToast(kTakesTip, false);
            return;
        }
        const int    i = map(ev.split);
        const double at = ev.split_at;
        EditNode("Split variation", c.lib_uid, c.uid, [i, at](SoundNode& x) { return SplitFrame(x.sound, i, at); });
        return;
    }
    if (ev.double_clicked != -1) {
        if (!in_edit) {
            OpenEdit(c.lib_uid, c.uid);
        } else if (ev.double_clicked == -2) {
            if (!FrameEditAllowed(mode, FrameEdit::Add)) {
                ShowLibraryToast(kTakesTip, false);
                return;
            }
            if (take_len > 0.0) return;
            const double at = ev.double_at, len = m.source_len;
            EditNode("Add variation", c.lib_uid, c.uid,
                     [at, len](SoundNode& x) { return AddFrame(x.sound, at, kNewFrameSeconds, len) >= 0; });
        }
        return;
    }
    if (ev.clicked >= 0) {
        const int i = map(ev.clicked);
        if (in_edit) {
            if (ev.ctrl) {
                auto it = std::find(P.sel.begin(), P.sel.end(), i);
                if (it != P.sel.end()) P.sel.erase(it);
                else P.sel.push_back(i);
            } else {
                P.sel = {i};
            }
        }
        PlayFrame(lib, n, m, i);
    } else if (ev.clicked_empty && in_edit && !ev.ctrl) {
        P.sel.clear();
    }
}

// The Sound's waveform(s): one lane, or one lane per take in Takes mode.
void DrawSoundWaves(const char* id, const SoundLibrary& lib, const SoundNode& n, const SoundMedia& m, float height,
                    bool in_edit, WaveformEvents* first_ev)
{
    const SoundData& d = Shown(n);
    const double     ph = SoundPlayhead(n);
    if (d.mode == DetectMode::Takes && m.take_paths.size() >= 2) {
        const float lane_h = std::max(24.0f, height / static_cast<float>(std::max<size_t>(1, d.frames.size())));
        for (size_t i = 0; i < d.frames.size() && !P.changed; ++i) {
            const std::string path = i < m.take_paths.size() ? m.take_paths[i] : m.path;
            const double      len = i < m.take_lens.size() ? m.take_lens[i] : m.source_len;
            WaveformOptions   o;
            o.size = ImVec2(0.0f, lane_h - 2.0f);
            o.source_len = len;
            o.gestures = true;
            o.select = in_edit;
            o.ruler = in_edit && i == 0;
            std::vector<int> sel;
            if (std::find(P.sel.begin(), P.sel.end(), static_cast<int>(i)) != P.sel.end()) sel.push_back(0);
            char lid[64];
            std::snprintf(lid, sizeof(lid), "%s_t%d", id, static_cast<int>(i));
            const WaveformEvents ev = DrawWaveform(lid, path, {d.frames[i]}, sel, -1.0, o);
            if (i == 0 && first_ev) *first_ev = ev;
            if (ev.right_clicked && first_ev) first_ev->right_clicked = true;
            HandleWave(ev, lib, n, m, {static_cast<int>(i)}, len, in_edit);
        }
        return;
    }
    WaveformOptions o;
    o.size = ImVec2(0.0f, height);
    o.source_len = m.source_len;
    o.gestures = true;
    o.select = in_edit;
    o.ruler = in_edit;
    o.wheel = in_edit;
    if (in_edit) {
        o.view_start = P.view_start;
        o.view_len = P.view_len;
        if (d.mode == DetectMode::Silence) o.threshold_db = d.silence.threshold_db;
    }
    std::vector<int> all;
    for (size_t i = 0; i < d.frames.size(); ++i) all.push_back(static_cast<int>(i));
    const WaveformEvents ev = DrawWaveform(id, m.path, d.frames, in_edit ? P.sel : std::vector<int>{}, ph, o);
    if (first_ev) *first_ev = ev;
    if (in_edit) {
        const double vl = P.view_len > 0.0 ? P.view_len : m.source_len;
        if (ev.zoom != 1.0 && vl > 0.0) {
            const double nl = std::clamp(vl * ev.zoom, 0.01, std::max(0.01, m.source_len));
            P.view_start = std::clamp(ev.zoom_at - (ev.zoom_at - P.view_start) * nl / vl, 0.0, std::max(0.0, m.source_len - nl));
            P.view_len = nl;
        }
        if (ev.scroll_s != 0.0) P.view_start = std::clamp(P.view_start + ev.scroll_s, 0.0, std::max(0.0, m.source_len - vl));
        if (ev.threshold_live || ev.threshold_done) {
            SoundData& dr = Live(n);
            dr.silence.threshold_db = ev.threshold_db;
            Redetect(dr, m, P.snap.frames);
            if (ev.threshold_done) CommitLive("Detection", lib.uid, n.uid);
            return;
        }
    }
    // Takes mode on one lane (fewer than 2 takes read): a trim still stays inside its take (decision 3).
    HandleWave(ev, lib, n, m, all, d.mode == DetectMode::Takes ? m.source_len : 0.0, in_edit);
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
    EditNode(
        gesture, lib_uid, uid,
        [&](SoundNode& x) {
            x.sound.source = SoundSource{};
            x.sound.source.kind = SoundSourceKind::Item;
            x.sound.source.item_guid = guid;
            x.sound.mode = mode;
            x.sound.frames = frames;
            x.sound.edited = false;
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
    if (frames.empty() && ReadSoundMedia(src, &m)) {
        PeakData   pk;
        const bool ready = RequestPeaks(m.path, &pk) == PeakState::Ready;
        mode = DefaultDetectMode(m.facts, ready);
        frames = DetectFrames(m.facts, mode, SilenceParams{}, ready ? &pk : nullptr);
    }
    EditNode(
        "Replace audio", lib_uid, uid,
        [&](SoundNode& x) {
            x.sound.source = src;
            x.sound.mode = mode;
            x.sound.frames = frames;
            x.sound.edited = !dropped.empty();
            return true;
        },
        "Replaced audio");
}

std::string FileName(const std::string& path)
{
    const size_t k = path.find_last_of("/\\");
    return k == std::string::npos ? path : path.substr(k + 1);
}

void Locate(const std::string& lib_uid, const std::string& uid, const SoundSource& src)
{
    std::string path = src.asset.last_path;
    if (!ChooseSoundFile("Locate " + src.asset.name, &path)) return;
    const double len = SourceFileLength(path);
    std::error_code ec;  // the size too, so the file is found again by name, size and length (13-11)
    const auto fsize = std::filesystem::file_size(std::filesystem::u8path(path), ec);
    const int64_t size = ec ? int64_t(-1) : int64_t(fsize);
    EditNode(
        "Locate sound file", lib_uid, uid,
        [&](SoundNode& x) {
            x.sound.source.kind = SoundSourceKind::File;
            x.sound.source.asset.last_path = path;
            x.sound.source.asset.name = FileName(path);
            if (len > 0.0) x.sound.source.asset.length = len;
            x.sound.source.asset.size = size;
            return true;
        },
        "Located " + FileName(path));
}

void ShowInReaper(const SoundSource& src)
{
    MediaItem* it = src.item_guid.empty() ? nullptr : FindItemByGuid(src.item_guid);
    if (!it) return;
    SelectOnlyItem(it);
    SetEditCurPos(GetMediaItemInfo_Value(it, "D_POSITION"), true, false);
    UpdateArrange();
}

// The Detection controls (panel Section and Edit's side strip).
void DetectionControls(const SoundLibrary& lib, const SoundNode& n, const SoundMedia& m)
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
        ImGui::TextDisabled("Reading\xE2\x80\xA6");
    } else if (modes.size() == 1 && d.mode == DetectMode::Whole) {
        ImGui::TextDisabled("Whole file");
    } else {
        int sel = -1;
        for (size_t i = 0; i < modes.size(); ++i)
            if (modes[i] == d.mode) sel = static_cast<int>(i);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Split by");
        ImGui::SameLine();
        if (ui::Segmented("##detmode", labels.data(), static_cast<int>(labels.size()), &sel) && sel >= 0) {
            const DetectMode mode = modes[static_cast<size_t>(sel)];
            // A new mode starts from its own detection (hand frames belong to the old one).
            const std::vector<SoundFrame> frames = Detected(m, mode, d.silence);
            const std::string toast = d.edited && mode != d.mode
                                          ? (mode == DetectMode::Whole ? std::string("Whole file")
                                                                       : std::string("Split by ") + labels[static_cast<size_t>(sel)]) +
                                                ": the hand edits of the previous mode were dropped"
                                          : std::string();
            EditNode("Detect", lib.uid, n.uid, [&](SoundNode& x) {
                if (x.sound.mode == mode) return false;  // no empty undo point
                x.sound.mode = mode;
                x.sound.frames = frames;
                x.sound.edited = false;
                return true;
            }, toast);
            if (P.changed) return;
        }
    }
    if (d.mode == DetectMode::Silence) {
        struct F {
            const char* id;
            const char* label;
            const char* unit;
            double SilenceParams::*field;
            double step;
            double lo, hi;
        };
        static const F kFields[] = {
            {"##thr", "Threshold", "dB", &SilenceParams::threshold_db, 0.1, -90.0, 0.0},
            {"##gap", "Min gap", "ms", &SilenceParams::min_gap_ms, 1.0, 0.0, 5000.0},
            {"##pre", "Pre-roll", "ms", &SilenceParams::preroll_ms, 0.5, 0.0, 1000.0},
            {"##tail", "Tail", "ms", &SilenceParams::tail_ms, 1.0, 0.0, 5000.0},
        };
        for (const F& f : kFields) {
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(f.label);
            ImGui::SameLine(90.0f);
            double v = d.silence.*(f.field);
            const ui::DragNumberEvent e = ui::DragNumber(f.id, &v, f.step, 1, f.unit, 60.0f);
            if (e == ui::DragNumberEvent::Live || e == ui::DragNumberEvent::Commit) {
                SoundData& dr = Live(n);
                dr.silence.*(f.field) = std::clamp(v, f.lo, f.hi);
                Redetect(dr, m, P.snap.frames);
                if (e == ui::DragNumberEvent::Commit) {
                    CommitLive("Detection", lib.uid, n.uid);
                    if (P.changed) return;
                }
            } else if (e == ui::DragNumberEvent::Cancel) {
                CancelLive();
            }
        }
    }
    ImGui::TextDisabled("%s", VariationCountText(DetectedCount(n.uid, m, d), d.frames).c_str());
    if (ui::SolidButton("Reset to detected")) {
        const std::vector<SoundFrame> det = Detected(m, d.mode, d.silence);
        EditNode("Reset to detected", lib.uid, n.uid, [&](SoundNode& x) {
            ResetToDetected(x.sound, det);
            return true;
        });
    }
}

void LoadNormalizePrefs()
{
    if (P.norm_loaded) return;
    P.norm_loaded = true;
    P.norm_db = LoadPrefFloat("lib_normalize_db", -1.0f);
    P.norm_whole = LoadPrefBool("lib_normalize_whole", false);
}

void NormalizeControls(const SoundLibrary& lib, const SoundNode& n, const SoundMedia& m)
{
    LoadNormalizePrefs();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Normalize peaks to");
    ImGui::SameLine();
    double v = P.norm_db;
    if (ui::DragNumber("##normdb", &v, 0.1, 1, "dBFS", 50.0f) == ui::DragNumberEvent::Commit) {
        P.norm_db = std::clamp(v, -60.0, 0.0);
        SavePrefFloat("lib_normalize_db", static_cast<float>(P.norm_db));
    }
    static const char* const kModes[2] = {"Per variation", "Whole sound"};
    int mode = P.norm_whole ? 1 : 0;
    if (ui::Segmented("##normmode", kModes, 2, &mode)) {
        P.norm_whole = mode == 1;
        SavePrefBool("lib_normalize_whole", P.norm_whole);
    }
    const SoundData& d = n.sound;
    bool ready = true;
    if (d.mode == DetectMode::Takes) {
        for (const std::string& p : m.take_paths) ready = ready && RequestPeaks(p, nullptr) == PeakState::Ready;
    } else {
        ready = RequestPeaks(m.path, nullptr) == PeakState::Ready;
    }
    std::string label = "Normalize";
    if (!P.sel.empty()) label += " (" + std::to_string(P.sel.size()) + " selected)";
    ImGui::BeginDisabled(!ready);
    if (ui::SolidButton(label.c_str())) {
        const std::vector<int> sel = NormalizeSelection(d.frames, P.sel);
        std::vector<SoundFrame> frames = d.frames;
        if (d.mode == DetectMode::Takes) {
            // Each take has its own file: its peak is read there (not read = left as is).
            std::vector<double> peak(frames.size(), -std::numeric_limits<double>::infinity());
            for (size_t i = 0; i < frames.size() && i < m.take_paths.size(); ++i) {
                PeakData pk;
                if (RequestPeaks(m.take_paths[i], &pk) == PeakState::Ready) peak[i] = FramePeakDb(pk, frames[i]);
            }
            NormalizeTakeFrames(frames, peak, P.norm_db, P.norm_whole, sel);
        } else {
            PeakData pk;
            if (RequestPeaks(m.path, &pk) == PeakState::Ready) NormalizeFrames(frames, pk, P.norm_db, P.norm_whole, sel);
        }
        bool same = true;
        for (size_t i = 0; i < frames.size(); ++i)
            same = same && frames[i].db == d.frames[i].db && frames[i].hand == d.frames[i].hand;
        if (same) {
            ShowLibraryToast("Nothing to normalize", false);  // no empty undo point
            ImGui::EndDisabled();
            return;
        }
        EditNode("Normalize", lib.uid, n.uid, [&](SoundNode& x) {
            x.sound.frames = frames;
            x.sound.edited = true;
            return true;
        });
    }
    ImGui::EndDisabled();
    if (!ready) Tip("Waiting for the waveform");
}

// ---- the Edit overlay -------------------------------------------------------------------------------

void DrawEditOverlay()
{
    if (!P.edit_open) return;
    const SoundStore&   store = GetSoundStore(nullptr);
    const SoundLibrary* lib = FindLibrary(store, P.edit_lib);
    const SoundNode*    n = lib ? FindNode(*lib, P.edit_uid) : nullptr;
    if (!n || n->type != SoundNodeType::Sound) {
        P.edit_open = false;
        return;
    }
    P.edit_frame = ImGui::GetFrameCount();
    const SoundMedia& m = MediaOf(*n);
    const ImVec2      ds = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos(ImVec2(24.0f, 52.0f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(std::max(200.0f, ds.x - 48.0f), std::max(160.0f, ds.y - 84.0f)), ImGuiCond_Always);
    if (P.edit_focus) {
        ImGui::SetNextWindowFocus();
        P.edit_focus = false;
    }
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ui::Col(ui::kSurface));
    ImGui::PushStyleColor(ImGuiCol_Border, ui::Col(ui::kStrokeStrong));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, ui::kRadiusLg);
    constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings |
                                        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollWithMouse;
    bool close = false;
    if (ImGui::Begin("##soundedit", nullptr, kFlags)) {
        const bool focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
        const bool keys = focused && !ImGui::GetIO().WantTextInput && !ui::DragNumberActive() &&
                          P.edit_opened != ImGui::GetFrameCount();  // the E that opened it
        const bool whole_playing = AuditionPlaying() && AuditionTag() == "whole:" + n->uid;
        auto       toggle_play = [&] {
            if (AuditionPlaying()) StopAudition();
            else if (!AuditionFile(m.path, 0.0, 0.0, "whole:" + n->uid, DbToGain(GainFromRoot(*lib, *n))))
                ShowLibraryToast("Cannot play: the sound file is not read", false);
        };
        // Header.
        if (ui::SolidButton(whole_playing ? "\xE2\x96\xA0##editplay" : "\xE2\x96\xB6##editplay")) toggle_play();
        Tip("Play the whole file (Space)");
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(n->name.c_str());
        ImGui::SameLine();
        ImGui::TextDisabled("%s", VariationCountText(DetectedCount(n->uid, m, Shown(*n)), Shown(*n).frames).c_str());
        const float right = ImGui::GetWindowContentRegionMax().x;
        ImGui::SameLine(std::max(ImGui::GetCursorPosX() + 8.0f, right - 230.0f));
        const double vl = P.view_len > 0.0 ? P.view_len : m.source_len;
        if (ui::SolidButton("\xE2\x88\x92##zoomout") && m.source_len > 0.0) {
            const double nl = std::min(m.source_len, vl * 1.5);
            P.view_start = std::clamp(P.view_start - (nl - vl) * 0.5, 0.0, std::max(0.0, m.source_len - nl));
            P.view_len = nl;
        }
        ImGui::SameLine();
        if (ui::SolidButton("+##zoomin") && m.source_len > 0.0) {
            const double nl = std::max(0.01, vl / 1.5);
            P.view_start = std::clamp(P.view_start + (vl - nl) * 0.5, 0.0, std::max(0.0, m.source_len - nl));
            P.view_len = nl;
        }
        ImGui::SameLine();
        if (ui::SolidButton("Fit")) {
            P.view_start = 0.0;
            P.view_len = 0.0;
        }
        ImGui::SameLine();
        ImGui::TextDisabled("?");
        Tip(Shown(*n).mode == DetectMode::Takes
                ? "Click a variation: play and select (Ctrl-click adds)\nDrag an edge: trim (inside its take)\n"
                  "Drag up / down inside: its dB\nAlt-click: off / on\nSplit and add: not in Takes mode\n"
                  "Space: play / stop, Esc: stop, then close"
                : "Click a variation: play and select (Ctrl-click adds)\nDrag an edge: trim\nDrag up / down inside: its dB\n"
                  "Alt-click: off / on\nS: split under the mouse\nDouble-click empty space: add a variation\n"
                  "Ctrl+wheel: zoom, wheel: scroll\nDrag the threshold line (Silence)\nSpace: play / stop, Esc: stop, then close");
        ImGui::SameLine();
        if (ui::SolidButton("Done")) close = true;
        ImGui::SameLine();
        if (ImGui::Button("\xE2\x9C\x95##editclose")) close = true;
        ImGui::Separator();

        // Waveform | side strip.
        const float side_w = 260.0f;
        const float wave_w = std::max(100.0f, ImGui::GetContentRegionAvail().x - side_w - 8.0f);
        const float wave_h = std::max(80.0f, ImGui::GetContentRegionAvail().y - 4.0f);
        ImGui::BeginChild("##editwave", ImVec2(wave_w, wave_h), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollWithMouse);
        if (!m.ok) {
            ImGui::TextColored(ui::Col(kLibMissing), "Source not found");
        } else {
            DrawSoundWaves("editwave", *lib, *n, m, wave_h - 24.0f, true, nullptr);
        }
        ImGui::EndChild();
        if (!P.changed) {
            ImGui::SameLine();
            ImGui::BeginChild("##editside", ImVec2(side_w, wave_h));
            ui::Section s1;
            if (ui::BeginSection(s1, "Detection", ImTextureID(), false) && m.ok) DetectionControls(*lib, *n, m);
            ui::EndSection(s1);
            if (!P.changed) {
                ui::Section s2;
                if (ui::BeginSection(s2, "Level", ImTextureID(), false) && m.ok) NormalizeControls(*lib, *n, m);
                ui::EndSection(s2);
            }
            ImGui::EndChild();
        }
        if (keys && !P.changed) {
            if (ImGui::IsKeyPressed(ImGuiKey_Space, false)) toggle_play();
            if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
                if (AuditionPlaying()) StopAudition();
                else close = true;
            }
            if (ImGui::IsKeyPressed(ImGuiKey_E, false)) close = true;
        }
    }
    ImGui::End();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(2);
    if (close) {
        P.edit_open = false;
        P.sel.clear();
        CancelLive();
        g_wd = WaveDrag{};  // a drag cut by the close never fires on the next Edit
    }
}

// ---- node panel sections --------------------------------------------------------------------------

void DrawHeader(const SoundLibrary& lib, const SoundNode& n)
{
    const bool playing = NodePlaying(n.uid) || (AuditionPlaying() && AuditionTag() == "frame:" + n.uid);
    if (ui::SolidButton(playing ? "\xE2\x96\xA0##play" : "\xE2\x96\xB6##play")) {
        if (playing) StopPlay();
        else PlayNode(lib.uid, n.uid);
    }
    Tip(playing ? "Stop (Esc)" : "Play (Space)");
    ImGui::SameLine();
    if (n.type == SoundNodeType::Sound) {
        if (P.name_uid != n.uid) {
            P.name_uid = n.uid;
            std::snprintf(P.name_buf, sizeof(P.name_buf), "%s", n.name.c_str());
        }
        ImGui::SetNextItemWidth(std::max(80.0f, ImGui::GetContentRegionAvail().x));
        ImGui::InputText("##name", P.name_buf, sizeof(P.name_buf));
        const bool done = ImGui::IsItemDeactivatedAfterEdit();
        if (!done && !ImGui::IsItemActive() && std::strcmp(P.name_buf, n.name.c_str()) != 0)
            std::snprintf(P.name_buf, sizeof(P.name_buf), "%s", n.name.c_str());  // renamed elsewhere / undone
        if (done && P.name_buf[0] && n.name != P.name_buf) {
            const std::string name = P.name_buf, lib_uid = lib.uid;
            const LibraryRow  row = RowOfNode(lib, n.uid);
            Gesture("Rename", [&](SoundStore& s) { return RenameRow(s, lib_uid, row, name); });
            if (P.changed) return;
        }
    } else {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(n.name.c_str());
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float maxx = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
    ImVec2      p = ImGui::GetCursorScreenPos();
    ui::EllipsisText(dl, p, maxx, ui::kFaint, NodePathText(lib, n));
    ImGui::Dummy(ImVec2(1.0f, ImGui::GetTextLineHeight()));
    if (P.plan.root == n.uid && P.plan_lib == lib.uid) {
        p = ImGui::GetCursorScreenPos();
        ui::EllipsisText(dl, p, maxx, ui::kMuted, "played: " + P.plan.played);
        ImGui::Dummy(ImVec2(1.0f, ImGui::GetTextLineHeight()));
    }
}

void DrawTypeRows(const SoundStore& store, const SoundLibrary& lib, const SoundNode& n)
{
    static const char* const kTypes[3] = {"Random", "Blend", "Switch"};
    int sel = n.type == SoundNodeType::Random ? 0 : n.type == SoundNodeType::Blend ? 1 : 2;
    const LibraryRow  row = RowOfNode(lib, n.uid);
    const std::string lib_uid = lib.uid;
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Type");
    ImGui::SameLine(90.0f);
    if (ui::Segmented("##type", kTypes, 3, &sel)) {
        if (sel == 2) {
            ImGui::OpenPopup("##switchon");
        } else {
            const SoundNodeType t = sel == 0 ? SoundNodeType::Random : SoundNodeType::Blend;
            Gesture(std::string("Make ") + kTypes[sel],
                    [&](SoundStore& s) { return SetRowsType(s, lib_uid, {row}, t, {}); });
            if (P.changed) return;
        }
    }
    if (n.type == SoundNodeType::Switch) {
        const SwitchGroup* g = FindGroup(store, n.group_uid);
        const std::string  lab = std::string("Switch on: ") + (g ? g->name : std::string("?")) + " \xE2\x96\xBE";
        ImGui::SameLine();
        if (ui::SolidButton(lab.c_str())) ImGui::OpenPopup("##switchon");
    }
    if (ImGui::BeginPopup("##switchon")) {
        if (store.groups.empty()) ImGui::TextDisabled("No Switch Group yet");
        for (const SwitchGroup& g : store.groups) {
            if (ImGui::MenuItem(g.name.c_str(), nullptr, n.type == SoundNodeType::Switch && n.group_uid == g.uid)) {
                const std::string gid = g.uid;
                ImGui::CloseCurrentPopup();
                ImGui::EndPopup();
                Gesture("Switch on " + g.name, [&](SoundStore& s) { return SetRowsType(s, lib_uid, {row}, SoundNodeType::Switch, gid); });
                return;
            }
        }
        ImGui::EndPopup();
    }
    if (n.type != SoundNodeType::Switch) return;
    const SwitchGroup* g = FindGroup(store, n.group_uid);
    if (!g) return;
    auto value_name = [&](const std::string& v) {
        for (const SwitchValue& x : g->values)
            if (x.uid == v) return x.name;
        return std::string("none");
    };
    // Default value.
    const std::string def = DefaultValue(store, n);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Default");
    ImGui::SameLine(90.0f);
    ImGui::SetNextItemWidth(160.0f);
    if (ImGui::BeginCombo("##default", value_name(def).c_str())) {
        for (const SwitchValue& v : g->values) {
            if (ImGui::Selectable(v.name.c_str(), v.uid == def) && v.uid != def) {
                const LibraryRow vr{n.uid, v.uid};
                const std::string name = v.name;
                ImGui::EndCombo();
                Gesture("Make default", [&](SoundStore& s) { return MakeDefault(s, lib_uid, {vr}); }, name + " is the default");
                return;
            }
        }
        ImGui::EndCombo();
    }
    // Preview value.
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Preview");
    ImGui::SameLine(90.0f);
    ImGui::SetNextItemWidth(160.0f);
    bool prev_known = false;  // a deleted value plays the default (SwitchPreviewChild)
    for (const SwitchValue& v : g->values) prev_known = prev_known || v.uid == n.preview_value;
    const std::string prev_label = !prev_known ? "(default: " + value_name(def) + ")" : value_name(n.preview_value);
    if (ImGui::BeginCombo("##preview", prev_label.c_str())) {
        std::string pick;
        bool        picked = false;
        if (ImGui::Selectable("(the default)", !prev_known) && !n.preview_value.empty()) picked = true;
        for (const SwitchValue& v : g->values)
            if (ImGui::Selectable(v.name.c_str(), v.uid == n.preview_value) && v.uid != n.preview_value) {
                pick = v.uid;
                picked = true;
            }
        ImGui::EndCombo();
        if (picked) {
            const std::string uid = n.uid;
            EditNode("Preview value", lib_uid, uid, [&](SoundNode& x) {
                x.preview_value = pick;
                return true;
            });
            return;
        }
    }
}

void DrawVolume(const SoundLibrary& lib, const SoundNode& n)
{
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Volume");
    ImGui::SameLine(90.0f);
    double v = 0.0;
    if (VolumeControl("##vol", n.uid, n.volume_db, &v, ImGui::GetContentRegionAvail().x - 110.0f))
        EditNode("Volume", lib.uid, n.uid, [v](SoundNode& x) {
            x.volume_db = v;
            return true;
        });
}

void DrawMix(const SoundLibrary& lib, const SoundNode& n)
{
    SeedSection("Mix", n.type != SoundNodeType::Switch);
    ui::Section s;
    if (ui::BeginSection(s, "Mix")) {
        const std::vector<const SoundNode*> kids = ChildNodes(lib, n);
        if (kids.empty()) ImGui::TextDisabled("No children");
        for (const SoundNode* k : kids) {
            if (P.changed) break;
            ImGui::PushID(k->uid.c_str());
            ImGui::AlignTextToFramePadding();
            const ImVec2 p = ImGui::GetCursorScreenPos();
            ui::EllipsisText(ImGui::GetWindowDrawList(), p, p.x + 80.0f, IsLit(k->uid) ? ui::kAccent : ui::kText, k->name);
            ImGui::Dummy(ImVec2(84.0f, ImGui::GetFrameHeight()));
            ImGui::SameLine();
            double v = 0.0;
            const std::string kid = k->uid;
            if (VolumeControl("##mixvol", kid, k->volume_db, &v, ImGui::GetContentRegionAvail().x - 110.0f))
                EditNode("Volume", lib.uid, kid, [v](SoundNode& x) {
                    x.volume_db = v;
                    return true;
                });
            ImGui::PopID();
        }
    }
    ui::EndSection(s);
}

void DrawRandomAvoid(const SoundLibrary& lib, const SoundNode& n)
{
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Avoid repeating the last");
    ImGui::SameLine();
    const int v = AvoidControl("##avoid", n.avoid_repeat);
    if (v >= 0)
        EditNode("Avoid repeating", lib.uid, n.uid, [v](SoundNode& x) {
            x.avoid_repeat = v;
            return true;
        });
}

// The Blend layer view: one lane per child, a ms ruler, drag a bar to offset it.
void DrawBlendLanes(const SoundStore& store, const SoundLibrary& lib, const SoundNode& n)
{
    ui::Section s;
    if (!ui::BeginSection(s, "Layers")) {
        ui::EndSection(s);
        return;
    }
    const std::vector<const SoundNode*> parts = BlendParts(lib, n);
    double total_ms = 100.0;
    for (const SoundNode* k : parts) {
        const double off = P.off_uid == k->uid ? P.off_live : k->offset_ms;
        total_ms = std::max(total_ms, off + NodeLengthRange(store, lib, k->uid).max_s * 1000.0);
    }
    total_ms *= 1.15;
    const float name_w = 84.0f, fields_w = 128.0f;
    const float lane_x0 = ImGui::GetCursorScreenPos().x + name_w;
    const float lane_w = std::max(40.0f, ImGui::GetContentRegionAvail().x - name_w - fields_w);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    // Ruler.
    {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const double steps[] = {10, 25, 50, 100, 250, 500, 1000, 2000, 5000, 10000};
        double       st = steps[0];
        for (double x : steps) {
            st = x;
            if (x / total_ms * lane_w >= 50.0) break;
        }
        for (double t = 0.0; t <= total_ms; t += st) {
            const float x = lane_x0 + static_cast<float>(t / total_ms) * lane_w;
            dl->AddLine(ImVec2(x, p.y + 10.0f), ImVec2(x, p.y + 14.0f), ui::kFaint);
            char lab[32];
            std::snprintf(lab, sizeof(lab), "%.0f", t);
            dl->AddText(ImVec2(x + 2.0f, p.y), ui::kFaint, lab);
        }
        ImGui::Dummy(ImVec2(1.0f, 15.0f));
    }
    const float lanes_top = ImGui::GetCursorScreenPos().y;
    const ImGuiIO& io = ImGui::GetIO();
    for (const SoundNode* k : parts) {
        if (P.changed) break;
        ImGui::PushID(k->uid.c_str());
        const std::string kid = k->uid;
        const ImVec2      p = ImGui::GetCursorScreenPos();
        const float       fh = ImGui::GetFrameHeight();
        ui::EllipsisText(dl, ImVec2(p.x, p.y + 3.0f), p.x + name_w - 4.0f, IsLit(kid) ? ui::kAccent : ui::kText, k->name);
        const LengthRange r = NodeLengthRange(store, lib, kid);
        const double      off = P.off_uid == kid ? P.off_live : k->offset_ms;
        const float       bx0 = lane_x0 + static_cast<float>(off / total_ms) * lane_w;
        const float       bx1 = bx0 + std::max(3.0f, static_cast<float>(r.max_s * 1000.0 / total_ms) * lane_w);
        const float       bmin = bx0 + static_cast<float>(r.min_s * 1000.0 / total_ms) * lane_w;
        dl->AddRectFilled(ImVec2(lane_x0, p.y + 1.0f), ImVec2(lane_x0 + lane_w, p.y + fh - 1.0f), ui::kBg, ui::kRadiusSm);
        dl->AddRectFilled(ImVec2(bx0, p.y + 3.0f), ImVec2(std::max(bx0 + 3.0f, bmin), p.y + fh - 3.0f), WithAlpha(ui::kAccent, 0x8C), ui::kRadiusSm);
        if (r.varies()) {
            dl->AddRectFilled(ImVec2(bmin, p.y + 3.0f), ImVec2(bx1, p.y + fh - 3.0f), WithAlpha(ui::kAccent, 0x2E));
            Hatch(dl, ImVec2(bmin, p.y + 3.0f), ImVec2(bx1, p.y + fh - 3.0f), WithAlpha(ui::kAccent, 0x8C));
        }
        ImGui::SetCursorScreenPos(ImVec2(bx0, p.y));
        ImGui::InvisibleButton("##bar", ImVec2(std::max(6.0f, bx1 - bx0), fh));
        if (ImGui::IsItemHovered() || ImGui::IsItemActive()) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        if (ImGui::IsItemActivated()) {
            P.off_uid = kid;
            P.off0 = k->offset_ms;
            P.off_live = k->offset_ms;
            P.off_scale = total_ms;
            P.off_dx = 0.0f;
        }
        if (ImGui::IsItemActive() && P.off_uid == kid) {
            P.off_dx += io.MouseDelta.x;
            double v = P.off0 + static_cast<double>(P.off_dx) / lane_w * P.off_scale;
            if (!io.KeyShift) v = std::round(v);
            P.off_live = std::max(0.0, v);
            const double delta = P.off_live - P.off0;
            ImGui::SetTooltip("%s%.0f ms", delta >= 0.0 ? "+" : "", delta);
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
        if (P.changed) {
            ImGui::PopID();
            break;
        }
        // Fields: ms and dB.
        ImGui::SetCursorScreenPos(ImVec2(lane_x0 + lane_w + 6.0f, p.y));
        double ms = off;
        if (ui::DragNumber("##ms", &ms, 1.0, 0, "ms", 40.0f) == ui::DragNumberEvent::Commit) {
            const double v = std::max(0.0, ms);
            EditNode("Offset", lib.uid, kid, [v](SoundNode& x) {
                x.offset_ms = v;
                return true;
            });
        }
        if (!P.changed) {
            ImGui::SameLine();
            double db = k->volume_db;
            if (ui::DragNumber("##db", &db, 0.1, 1, "dB", 36.0f) == ui::DragNumberEvent::Commit) {
                const double v = std::clamp(db, kMinDb, kMaxDb);
                EditNode("Volume", lib.uid, kid, [v](SoundNode& x) {
                    x.volume_db = v;
                    return true;
                });
            }
        }
        ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + fh + 2.0f));
        ImGui::Dummy(ImVec2(1.0f, 0.0f));
        ImGui::PopID();
    }
    if (!P.changed && NodePlaying(n.uid)) {
        const float x = lane_x0 + static_cast<float>(AuditionPosition() * 1000.0 / total_ms) * lane_w;
        dl->AddLine(ImVec2(x, lanes_top), ImVec2(x, ImGui::GetCursorScreenPos().y), ui::kWhite, 1.5f);
    }
    ui::EndSection(s);
}

void DrawSoundPanel(const SoundLibrary& lib, const SoundNode& n)
{
    P.sound_frame = ImGui::GetFrameCount();
    const SoundMedia& m = MediaOf(n);
    const SoundData&  d = Shown(n);
    const std::string lib_uid = lib.uid, uid = n.uid;
    const SoundSource src = n.sound.source;

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
                EditNode("Detect", lib_uid, uid, [&](SoundNode& x) {
                    x.sound.mode = mode;
                    x.sound.frames = frames;
                    x.sound.edited = false;
                    return true;
                });
                return;
            }
        }
    }

    ui::Section src_sec;
    if (ui::BeginSection(src_sec, "Source", ImTextureID(), false)) {
        if (!m.ok) {
            ImGui::TextColored(ui::Col(kLibMissing), "Source not found");
            if (src.kind == SoundSourceKind::Item) {
                if (ui::SolidButton("Set from selected item")) ReplaceFromSelected(lib_uid, uid, "Set from selected item");
            } else {
                if (ui::SolidButton("Locate\xE2\x80\xA6")) Locate(lib_uid, uid, src);
            }
        } else {
            WaveformEvents ev;
            DrawSoundWaves("panelwave", lib, n, m, 90.0f, false, &ev);
            if (P.changed) {
                ui::EndSection(src_sec);
                return;
            }
            // Drop a Media Explorer file / variations (13-7c) to replace the audio.
            const ImGuiPayload* drag = ImGui::GetDragDropPayload();
            if (!P.hooks.media_payload_type.empty() && P.hooks.decode_media && drag && drag->IsDataType(P.hooks.media_payload_type.c_str())) {
                ImDrawList* dl = ImGui::GetWindowDrawList();
                dl->AddRect(ev.min, ev.max, ui::kAccent, ui::kRadiusSm, 0, 2.0f);
                dl->AddText(ImVec2(ev.min.x + 6.0f, ev.min.y + 4.0f), ui::kAccent, "Replace");
                if (ImGui::BeginDragDropTarget()) {
                    if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload(P.hooks.media_payload_type.c_str())) {
                        SoundSource             ns;
                        std::vector<SoundFrame> fr;
                        if (P.hooks.decode_media(pl->Data, pl->DataSize, &ns, &fr)) ReplaceFromDrop(lib_uid, uid, ns, fr);
                    }
                    ImGui::EndDragDropTarget();
                }
                if (P.changed) {
                    ui::EndSection(src_sec);
                    return;
                }
            }
            if (ev.right_clicked) ImGui::OpenPopup("##wavemenu");
            if (ImGui::BeginPopup("##wavemenu")) {
                const bool go = ImGui::MenuItem("Replace with selected items");
                ImGui::EndPopup();
                if (go) {
                    ReplaceFromSelected(lib_uid, uid, "Replace audio");
                    ui::EndSection(src_sec);
                    return;
                }
            }
        }
        if (src.kind == SoundSourceKind::Item) {
            ImGui::BeginDisabled(!m.ok);
            if (ui::SolidButton("Show in REAPER")) ShowInReaper(src);
            ImGui::EndDisabled();
        } else {
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(src.asset.name.empty() ? FileName(src.asset.last_path).c_str() : src.asset.name.c_str());
            ImGui::SameLine();
            const bool can = static_cast<bool>(P.hooks.show_in_media);
            ImGui::BeginDisabled(!can);
            if (ui::SolidButton("Show in Media Explorer") && can) P.hooks.show_in_media(src);
            ImGui::EndDisabled();
            if (!can) Tip(kNoMediaTip);
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(!m.ok);
        if (ui::SolidButton("Edit")) OpenEdit(lib_uid, uid);
        ImGui::EndDisabled();
        Tip("Edit the variations (E)");
    }
    ui::EndSection(src_sec);
    if (P.changed) return;

    ui::Section var_sec;
    if (ui::BeginSection(var_sec, "Variations", ImTextureID(), false)) {
        const int on = static_cast<int>(ActiveVariations(d).size());
        if (on == 1) ImGui::TextUnformatted("1 variation");
        else ImGui::Text("%d variations \xE2\x80\x94 one at random at each event", on);
        bool linked = n.sound.linked_copies;
        if (ImGui::Checkbox("Linked copies replay the same variation", &linked)) {
            EditNode("Linked copies", lib_uid, uid, [linked](SoundNode& x) {
                x.sound.linked_copies = linked;
                return true;
            });
        }
        if (!P.changed) DrawRandomAvoid(lib, n);
    }
    ui::EndSection(var_sec);
    if (P.changed) return;

    SeedSection("Detection", false);
    ui::Section det_sec;
    if (ui::BeginSection(det_sec, "Detection") && m.ok) DetectionControls(lib, n, m);
    ui::EndSection(det_sec);
}

void OpenEditKey(const SoundLibrary& lib, const SoundNode& n)
{
    const ImGuiIO& io = ImGui::GetIO();
    if (P.edit_open || !LibraryViewFocused() || io.WantTextInput || io.KeyCtrl || io.KeyAlt || io.KeyShift) return;
    if (ImGui::IsKeyPressed(ImGuiKey_E, false) && MediaOf(n).ok) OpenEdit(lib.uid, n.uid);
}

}  // namespace

void DrawNodePanel()
{
    P.changed = false;
    // A live edit only lasts while its drag does (an unfinished one never shadows the store).
    if (P.live && !ImGui::IsMouseDown(ImGuiMouseButton_Left) && !ui::DragNumberActive()) CancelLive();
    MergeTreeHooks();
    AuditionPlaying();  // frees an audition that has played to its end
    const SoundStore&   store = GetSoundStore(nullptr);
    const SoundLibrary* lib = FindLibrary(store, SelectedLibraryUid());
    const std::string   uid = lib ? LibraryRowNode() : std::string();
    const SoundNode*    n = lib && !uid.empty() ? FindNode(*lib, uid) : nullptr;
    if (P.live && (!n || P.draft_uid != uid)) CancelLive();
    if (!n) {
        ImGui::Dummy(ImVec2(0.0f, 4.0f));
        ImGui::Indent(6.0f);
        ImGui::TextDisabled(lib ? "Select a node of the tree" : "No library");
        ImGui::Unindent(6.0f);
        DrawEditOverlay();
        return;
    }
    // Esc stops / Space plays while column 3 has the focus (the tree has its own).
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows) && !ImGui::GetIO().WantTextInput && !ui::DragNumberActive()) {
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) && AuditionPlaying()) StopPlay();
        else if (ImGui::IsKeyPressed(ImGuiKey_Space, false)) {
            if (NodePlaying(uid)) StopPlay();
            else PlayNode(lib->uid, uid);
        }
    }
    ImGui::PushID(uid.c_str());
    ImGui::Dummy(ImVec2(0.0f, 2.0f));
    DrawHeader(*lib, *n);
    if (!P.changed && n->type != SoundNodeType::Sound) {
        ui::Section s;
        if (ui::BeginSection(s, "Node", ImTextureID(), false)) {
            DrawTypeRows(store, *lib, *n);
            if (!P.changed) DrawVolume(*lib, *n);
            if (!P.changed && n->type == SoundNodeType::Random) DrawRandomAvoid(*lib, *n);
        }
        ui::EndSection(s);
        if (!P.changed) DrawMix(*lib, *n);
        if (!P.changed && n->type == SoundNodeType::Blend) DrawBlendLanes(store, *lib, *n);
    } else if (!P.changed) {
        ui::Section s;
        if (ui::BeginSection(s, "Level", ImTextureID(), false)) DrawVolume(*lib, *n);
        ui::EndSection(s);
        if (!P.changed) {
            OpenEditKey(*lib, *n);
            DrawSoundPanel(*lib, *n);
        }
    }
    ImGui::PopID();
    if (!P.changed) DrawEditOverlay();
}

}  // namespace rav

#endif  // _WIN32
