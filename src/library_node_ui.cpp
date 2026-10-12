// SPDX-License-Identifier: MIT
//
// See library_node_ui.h. Every gesture is one ModifySoundStore undo point ("RAV: <gesture>")
// followed by the Library toast with Undo; live drags (faders, offsets, trims, dB, detection
// fields, the threshold line) preview a local draft and write once on release. A gesture is only
// picked while the panel draws (the 13-8 rule, picked_gestures.h: it holds copies of what it
// writes) and runs once the panel is drawn, so no SoundLibrary / SoundNode reference the panel
// holds is read after the store it points into was replaced; the panel stops drawing for the
// frame a gesture was picked in (P.changed). The audition / pre-edit logic is
// sound_audition_plan.h (pure).

#include "library_node_ui.h"

#ifdef _WIN32

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <set>

#include <imgui_internal.h>  // BeginDragDropTargetCustom, ImRect

#include "library_tree_edit.h"
#include "library_icons.h"
#include "library_tree_ui.h"
#include "library_view_ui.h"
#include "picked_gestures.h"
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
#include "ui_flow.h"
#include "ui_flow_math.h"
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
    // A volume fader's gesture (0 = a dB field's): its id and its library, to enter its value on its
    // own node when the node it belongs to is no longer drawn (SettleLostFaders).
    ImGuiID               vol_gid = 0;
    std::string           vol_lib;
    int                   vol_seen = -1;  // the last frame its fader was drawn active
    std::string           off_uid;
    double                off0 = 0.0, off_live = 0.0;
    double                off_scale = 0.0;  // the lanes' ms at the drag's start (fixed while dragging)
    float                 off_dx = 0.0f;
    // The Edit overlay.
    bool                  edit_open = false;
    bool                  edit_focus = false;
    bool                  edit_has_focus = false;  // the Edit overlay window is focused (its zoom keys)
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
    // A Detection fader being dragged (its field's key) and its live value.
    std::string           detf_key;
    double                detf_live = 0.0;
    // Its fader's id, library, node and field (SettleLostFaders).
    ImGuiID               detf_gid = 0;
    std::string           detf_lib, detf_uid;
    double SilenceParams::*detf_field = nullptr;
    int                   detf_seen = -1;  // the last frame its fader was drawn active
    bool                  norm_loaded = false;
    double                norm_db = -1.0;
    bool                  norm_whole = false;
    // The user's REAPER view bindings (reaper-kb.ini), read when Edit opens.
    ViewKeymap            keys = DefaultViewKeymap();
    // Edit's waveform: its x span on screen and the source length (key zoom around the mouse).
    float                 wave_x0 = 0.0f, wave_x1 = 0.0f;
    double                wave_len = 0.0;
    ImGuiID               pan_id = 0;  // the waveform a middle-drag pans
    bool                  keys_stale = true;  // re-read reaper-kb.ini (each time Edit opens)
};
PanelState P;

// 13-7a / 13-7b: the layout helpers are the shared ones (ui_flow.h): labelled rows, text
// ellipsized with a tooltip, items wrapping when they do not fit beside the last one.
using ui::FitText;
using ui::SameLineIfFits;

double DbToGain(double db) { return std::pow(10.0, db / 20.0); }

using ui::WithAlpha;

void OpenEdit(const std::string& lib_uid, const std::string& uid)
{
    P.edit_open = true;
    P.edit_focus = true;
    P.keys_stale = true;
    P.wave_len = 0.0;  // set by a Sound's waveform; Takes mode leaves it 0 (no key zoom)
    P.wave_x0 = P.wave_x1 = 0.0f;
    P.edit_opened = ImGui::GetFrameCount();
    P.edit_lib = lib_uid;
    P.edit_uid = uid;
    P.view_start = 0.0;
    P.view_len = 0.0;
    P.sel.clear();
}

// ---- gestures -------------------------------------------------------------------------------------

// The gestures picked while the panel draws, run by DrawNodePanel once it is drawn.
PickedGestures g_picks;

// The write itself: one undo point and the toast. False when nothing changed.
bool WriteNow(const std::string& name, const std::function<bool(SoundStore&)>& edit, const std::string& toast)
{
    const std::string desc = "RAV: " + name;
    if (!ModifySoundStore(nullptr, desc.c_str(), edit)) return false;
    ShowLibraryToast(toast.empty() ? name : toast, true, desc);
    return true;
}

// Picks the write (`edit` holds copies: it runs after the panel, the caller's locals gone). The
// panel stops drawing for this frame, as it did after a write.
void Gesture(const std::string& name, std::function<bool(SoundStore&)> edit, const std::string& toast = {})
{
    if (!edit) return;
    P.changed = true;
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
    EditNode(name, lib_uid, uid, [d](SoundNode& n) {
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

// A Sound whose source is an NVK folder item: its pile is placed as it is, without fades.
bool FolderSourced(const SoundData& d)
{
    if (d.source.kind != SoundSourceKind::Item || d.source.item_guid.empty()) return false;
    MediaItem* it = FindItemByGuid(d.source.item_guid);
    return it && IsFolderItem(it);
}

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
        p.fade_in_s = v.fade_in_s;
        p.fade_out_s = v.fade_out_s;
        p.fade_in_shape = v.fade_in_shape;
        p.fade_out_shape = v.fade_out_shape;
        if (FolderSourced(s->sound)) p.fade_in_s = p.fade_out_s = 0.0;  // placed without fades
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
            double            fin = 0.0, fout = 0.0;
            int               shin = 0, shout = 0;
            AuditionFades(n->sound, act.front(), &fin, &fout, &shin, &shout, FolderSourced(n->sound));
            if (AuditionFile(VariationPath(m, n->sound, act.front()), f.start, f.length, uid,
                             DbToGain(GainFromRoot(*lib, *n) + f.db), fin, fout, shin, shout)) {
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
    double            fin = 0.0, fout = 0.0;
    int               shin = 0, shout = 0;
    AuditionFades(d, i, &fin, &fout, &shin, &shout, FolderSourced(d));
    if (!AuditionFile(VariationPath(m, d, i), f.start, f.length, "frame:" + n.uid, DbToGain(GainFromRoot(lib, n) + f.db),
                      fin, fout, shin, shout))
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
    int     lane = 0;  // Thr: the lane whose line is dragged (a stereo source has two)
};
WaveDrag g_wd;

int FrameAt(const std::vector<SoundFrame>& frames, double t)
{
    for (size_t i = 0; i < frames.size(); ++i)
        if (t >= frames[i].start && t < frames[i].start + frames[i].length) return static_cast<int>(i);
    return -1;
}

// ---- fades on the waveform (Antho 2026-10-12) -------------------------------------------------------

struct FadeDrag {
    ImGuiID id = 0;
    int     frame = -1;
    int     side = 0;  // -1 fade in, +1 fade out
    double  in = -1.0, out = -1.0;  // the frame's own fades as dragged (s, < 0 = the Sound's default)
    bool    moved = false;
    float   x0 = 0.0f;
};
FadeDrag g_fd;

constexpr float kFadeGrab = 5.0f;   // a handle's grab half-width (px)
constexpr float kFadeBand = 10.0f;  // the top band a handle is grabbed in (px)

// Draws each frame's fades as REAPER draws an item's (a shaded corner over the fade curve) with a
// handle at the frame's top corners, and drags them: `ev->fade_*` while dragging / on release,
// Alt-click `ev->fade_reset`. True while the mouse is on a handle or drags one (the waveform's own
// click is then not started).
bool WaveFades(ImDrawList* dl, ImGuiID gid, bool hovered, const std::vector<SoundFrame>& frames,
               const WaveformOptions& o, ImVec2 a, ImVec2 b, double vs, double vl, WaveformEvents* ev)
{
    if (o.fades.empty() || !(vl > 0.0)) return false;
    const float w = b.x - a.x, h = b.y - a.y;
    auto        X = [&](double t) { return static_cast<float>(a.x + (t - vs) / vl * w); };
    auto        T = [&](float x) { return vs + static_cast<double>(x - a.x) / w * vl; };
    const ImU32 shade = WithAlpha(ui::kBg, 0x90), line = WithAlpha(ui::kText, 0xB0);
    const ImVec2 m = ImGui::GetIO().MousePos;
    int   hot_frame = -1, hot_side = 0;
    float hot_d = kFadeGrab + 1.0f;
    dl->PushClipRect(a, b, true);
    for (size_t i = 0; i < frames.size() && i < o.fades.size(); ++i) {
        const SoundFrame& f = frames[i];
        const FrameFades& fd = o.fades[i];
        const double      end = f.start + f.length;
        if (X(end) < a.x || X(f.start) > b.x) continue;
        constexpr int kSteps = 12;
        for (int side = -1; side <= 1; side += 2) {
            const double len = side < 0 ? fd.in : fd.out;
            if (len > 0.0) {
                const double t0 = side < 0 ? f.start : end - len;
                ImVec2       prev;
                for (int k = 0; k <= kSteps; ++k) {
                    const double t = t0 + len * k / kSteps;
                    const double g = FadeGainAt(t - f.start, f.length, side < 0 ? fd.in : 0.0, side > 0 ? fd.out : 0.0,
                                                fd.in_shape, fd.out_shape);
                    const ImVec2 p(X(t), b.y - static_cast<float>(g) * h);
                    if (k > 0) {
                        dl->AddQuadFilled(ImVec2(prev.x, a.y), ImVec2(p.x, a.y), p, prev, shade);
                        dl->AddLine(prev, p, line);
                    }
                    prev = p;
                }
            }
            if (!o.gestures) continue;
            const float  hx = X(side < 0 ? f.start + fd.in : end - fd.out);
            const bool   own = side < 0 ? fd.own_in : fd.own_out;
            const ImVec2 c(hx, a.y + 4.0f);
            if (own) dl->AddCircleFilled(c, 3.5f, ui::kAccent);
            else dl->AddCircle(c, 3.5f, ui::kFaint);
            const float d = std::fabs(m.x - hx);
            if (hovered && m.y <= a.y + kFadeBand && d < hot_d) {
                hot_d = d;
                hot_frame = static_cast<int>(i);
                hot_side = side;
            }
        }
    }
    dl->PopClipRect();
    if (!o.gestures) return false;
    const ImGuiIO& io = ImGui::GetIO();
    if (hot_frame >= 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        if (io.KeyAlt) {
            ev->fade_reset = hot_frame;
            ev->fade_reset_side = hot_side;
        } else {
            g_fd = FadeDrag{};
            g_fd.id = gid;
            g_fd.frame = hot_frame;
            g_fd.side = hot_side;
            g_fd.in = frames[static_cast<size_t>(hot_frame)].fade_in;
            g_fd.out = frames[static_cast<size_t>(hot_frame)].fade_out;
            g_fd.x0 = m.x;
        }
    }
    const bool dragging = g_fd.id == gid && g_fd.frame >= 0 && static_cast<size_t>(g_fd.frame) < frames.size();
    if (dragging) {
        const SoundFrame& f = frames[static_cast<size_t>(g_fd.frame)];
        if (std::fabs(m.x - g_fd.x0) > 1.0f) g_fd.moved = true;
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            if (g_fd.moved) {
                const double t = T(m.x);
                if (g_fd.side < 0) g_fd.in = std::clamp(t - f.start, 0.0, std::max(0.0, f.length));
                else g_fd.out = std::clamp(f.start + f.length - t, 0.0, std::max(0.0, f.length));
                ev->fade_frame = g_fd.frame;
                ev->fade_in = g_fd.in;
                ev->fade_out = g_fd.out;
                char tip[64];
                std::snprintf(tip, sizeof(tip), "Fade %s %.1f ms", g_fd.side < 0 ? "in" : "out",
                              (g_fd.side < 0 ? g_fd.in : g_fd.out) * 1000.0);
                ui::ShowTip(tip);
            }
        } else {
            if (g_fd.moved) {
                ev->fade_frame = g_fd.frame;
                ev->fade_in = g_fd.in;
                ev->fade_out = g_fd.out;
                ev->fade_done = true;
            }
            g_fd = FadeDrag{};
        }
    } else if (g_fd.id == gid) {
        g_fd = FadeDrag{};  // its frame went away
    }
    const bool hot = hot_frame >= 0 || dragging;
    if (hot) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    if (hot_frame >= 0 && !dragging)
        ui::ShowTip(hot_side < 0 ? "Fade in: drag to set, Alt-click for the Sound's default"
                                 : "Fade out: drag to set, Alt-click for the Sound's default");
    return hot;
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
    const float  rh = o.ruler ? ui::Dp(18.0f + 4.0f) : 0.0f;  // .edruler: 18 px + 4 px under it
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##wave", ImVec2(w, h + rh), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    const bool hovered = ImGui::IsItemHovered();
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
    auto X = [&](double t) { return static_cast<float>(a.x + (t - vs) / vl * w); };
    auto T = [&](float x) { return vs + static_cast<double>(x - a.x) / w * vl; };
    // 13-14 (C5.4): a stereo source draws two lanes (L above R), each half the height with its own
    // zero line (the mockup's paintWave(..., 2)); frames, trim handles, fades and the playhead span
    // both. hh: a lane's half height; LaneMid(l): its zero line; LaneAt(y): the lane at y.
    PeakData   pk;
    const bool have_peaks = ReadPeaks(path, vs, vl, w / vl, &pk);
    const int  lanes = have_peaks ? PeakChannels(pk) : 1;
    const float lane_h = h / static_cast<float>(lanes), hh = lane_h * 0.5f - 3.0f;
    auto LaneMid = [&](int l) { return a.y + lane_h * (static_cast<float>(l) + 0.5f); };
    auto LaneAt = [&](float y) { return std::clamp(static_cast<int>((y - a.y) / std::max(1.0f, lane_h)), 0, lanes - 1); };
    const ImVec2 m = io.MousePos;
    const int    under = hovered ? FrameAt(frames, T(m.x)) : -1;
    const int    playing_frame = playhead_s >= 0.0 ? FrameAt(frames, playhead_s) : -1;
    const bool   trimming = g_wd.id == gid && g_wd.kind == WaveDrag::Trim && g_wd.moved;
    dl->PushClipRect(a, b, true);

    // Frames (.sbox): a box inset 2 px, accent 9 % (13 % under the mouse, 18 % selected / playing /
    // trimmed, then with an accent edge); an off frame hatched with a dashed edge.
    const float by0 = a.y + 2.0f, by1 = b.y - 2.0f;
    for (size_t i = 0; i < frames.size(); ++i) {
        const SoundFrame& f = frames[i];
        const ImVec2 fa(X(f.start), by0), fb(std::max(X(f.start + f.length), X(f.start) + 1.0f), by1);
        if (fb.x < a.x || fa.x > b.x) continue;
        const bool sel = std::find(selection.begin(), selection.end(), static_cast<int>(i)) != selection.end();
        const bool hot = static_cast<int>(i) == playing_frame || (trimming && g_wd.frame == static_cast<int>(i));
        if (!f.on) {
            dl->AddRectFilled(fa, fb, WithAlpha(ui::kMuted, 0x14), 3.0f);
            Hatch(dl, fa, fb, ui::kHatch);
        } else {
            const int alpha = (sel || hot) ? 0x2E : (static_cast<int>(i) == under ? 0x21 : 0x17);
            dl->AddRectFilled(fa, fb, WithAlpha(ui::kAccent, alpha), 3.0f);
        }
    }
    // Peaks, each column scaled by its frame's dB: accent inside an active frame, muted outside;
    // the part past full scale in --bad.
    if (have_peaks) {
        for (size_t i = 0; i < pk.max_abs.size(); ++i) {
            const double t = vs + (static_cast<double>(i) + 0.5) / pk.rate;
            const int    fi = FrameAt(frames, t);
            const double g = fi >= 0 ? DbToGain(frames[static_cast<size_t>(fi)].db) : 1.0;
            const float  x = a.x + static_cast<float>(i) * w / static_cast<float>(pk.max_abs.size());
            const bool   in = fi >= 0 && frames[static_cast<size_t>(fi)].on;
            for (int l = 0; l < lanes; ++l) {
                const float  mid = LaneMid(l);
                const double amp = PeakLane(pk, l)[i] * g;
                const float  y = static_cast<float>(std::min(amp, 1.0)) * hh;
                dl->AddLine(ImVec2(x, mid - y), ImVec2(x, mid + y + 1.0f), in ? ui::kAccent : ui::kMuted);
                if (amp > 1.0) {  // clipped: the top and bottom of the column
                    const float over = std::min(hh, static_cast<float>(amp - 1.0) * hh);
                    dl->AddLine(ImVec2(x, mid - hh), ImVec2(x, mid - hh + over), ui::kBad);
                    dl->AddLine(ImVec2(x, mid + hh - over), ImVec2(x, mid + hh + 1.0f), ui::kBad);
                }
            }
        }
        // The line between the lanes (1 px stroke-strong).
        for (int l = 1; l < lanes; ++l) {
            const float y = std::floor(a.y + lane_h * static_cast<float>(l));
            dl->AddRectFilled(ImVec2(a.x, y), ImVec2(b.x, y + 1.0f), ui::kStrokeStrong);
        }
    } else {
        ev.reading = RequestPeaks(path, nullptr) == PeakState::Building;
        dl->AddText(ImVec2(a.x + ui::Dp(8.0f), text_y), ui::kMuted, ev.reading ? "Reading waveform\xE2\x80\xA6" : "Waveform unavailable");
    }
    // The frames' edges and numbers over the peaks: 1 px stroke-strong r3 (accent when selected,
    // playing or trimmed; dashed when off); a clamped edge (it ends where the next one starts) is a
    // 2 px text-colour right edge; the number in mono at (3, 1), struck through when off.
    ImFont* nf = o.big_numbers ? ui::FontMonoSmall() : ui::FontMonoMicro();
    for (size_t i = 0; i < frames.size(); ++i) {
        const SoundFrame& f = frames[i];
        const ImVec2 fa(X(f.start), by0), fb(std::max(X(f.start + f.length), X(f.start) + 1.0f), by1);
        if (fb.x < a.x || fa.x > b.x) continue;
        const bool sel = std::find(selection.begin(), selection.end(), static_cast<int>(i)) != selection.end();
        const bool hot = static_cast<int>(i) == playing_frame || (trimming && g_wd.frame == static_cast<int>(i));
        if (!f.on) ui::DashedRect(dl, fa, fb, ui::kStrokeStrong, 3.0f, 3.0f, 2.0f);
        else dl->AddRect(fa, fb, (sel || hot) ? ui::kAccent : ui::kStrokeStrong, 3.0f);
        if (i < o.solid_edges.size() && o.solid_edges[i])
            dl->AddRectFilled(ImVec2(fb.x - 2.0f, fa.y), fb, ui::kText);
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
    // The threshold line (Silence): dashed text colour at 45 % (accent under the mouse), its dB in a
    // chip at the right.
    const bool thr = std::isfinite(o.threshold_db);
    float      thr_y = 0.0f;
    if (thr) thr_y = static_cast<float>(std::min(1.0, DbToGain(o.threshold_db))) * hh;
    // The mouse within 4 px of a threshold line of the lane it is over (two per lane), so a stereo
    // wave has no more grab zones under the mouse than a mono one.
    auto       on_thr = [&](float y) {
        const int l = LaneAt(y);
        return std::fabs(y - (LaneMid(l) - thr_y)) <= ui::Dp(4.0f) || std::fabs(y - (LaneMid(l) + thr_y)) <= ui::Dp(4.0f);
    };
    const bool near_thr = hovered && thr && on_thr(m.y);
    if (thr) {
        const bool  hot = near_thr || (g_wd.id == gid && g_wd.kind == WaveDrag::Thr);
        const ImU32 col = hot ? ui::kAccent : WithAlpha(ui::kText, 0x73);
        const float dash = ui::Dp(3.0f), step = ui::Dp(6.0f);
        for (int l = 0; l < lanes; ++l) {
            const float mid = LaneMid(l);
            for (float x = a.x; x < b.x; x += step) {
                dl->AddLine(ImVec2(x, std::floor(mid - thr_y) + 0.5f), ImVec2(std::min(x + dash, b.x), std::floor(mid - thr_y) + 0.5f), col);
                dl->AddLine(ImVec2(x, std::floor(mid + thr_y) + 0.5f), ImVec2(std::min(x + dash, b.x), std::floor(mid + thr_y) + 0.5f), col);
            }
        }
        const float mid = LaneMid(0);  // the chip by the first lane's upper line
        char lab[32];
        std::snprintf(lab, sizeof(lab), "%.0f dB", o.threshold_db);
        ImFont*      cf = ui::FontMonoTiny();
        const ImVec2 cs = ui::TextSizeIn(cf, lab);
        const ImVec2 c1(b.x - 4.0f, std::max(a.y + cs.y + 2.0f, mid - thr_y + cs.y * 0.5f + 1.0f));
        const ImVec2 c0(c1.x - cs.x - 8.0f, c1.y - cs.y - 2.0f);
        dl->AddRectFilled(c0, c1, ui::kRaised, 3.0f);
        dl->AddRect(c0, c1, ui::kStrokeStrong, 3.0f);
        ui::AddTextIn(dl, cf, ImVec2(c0.x + 4.0f, c0.y + 1.0f), ui::kText, lab);
    }
    if (playhead_s >= vs && playhead_s <= vs + vl) dl->AddLine(ImVec2(X(playhead_s), a.y), ImVec2(X(playhead_s), b.y), ui::kText, 1.0f);
    dl->PopClipRect();
    const bool fade_hot = WaveFades(dl, gid, hovered, frames, o, a, b, vs, vl, &ev);  // Antho 2026-10-12
    dl->AddRect(a, b, ui::kStroke, ui::kRadiusSm);

    // The ruler (.edruler): ms below 2 s shown, else s; mono 10 faint labels 2 px after a 6 px
    // stroke-strong tick, a stroke line under it.
    if (o.ruler) {
        const double steps[] = {0.001, 0.005, 0.01, 0.02, 0.05, 0.1, 0.2, 0.25, 0.5, 1, 2, 5, 10, 30, 60};
        double       st = steps[0];
        for (double s : steps) {
            st = s;
            if (s / vl * w >= 70.0) break;
        }
        ImFont*     rf = ui::FontMonoTiny();
        const float base = p0.y + rh - 4.0f;
        dl->AddLine(ImVec2(p0.x, base - 0.5f), ImVec2(p0.x + w, base - 0.5f), ui::kStroke);
        for (double t = std::ceil(vs / st) * st; t <= vs + vl; t += st) {
            const float x = std::floor(X(t)) + 0.5f;
            dl->AddLine(ImVec2(x, base - 7.0f), ImVec2(x, base - 1.0f), ui::kStrokeStrong);
            char lab[32];
            if (st < 1.0) std::snprintf(lab, sizeof(lab), "%.0f ms", t * 1000.0);
            else std::snprintf(lab, sizeof(lab), "%g s", t);
            ui::AddTextIn(dl, rf, ImVec2(x + 2.0f, p0.y + 2.0f), ui::kFaint, lab);
        }
    }

    // ---- interaction ----
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
    // A trim handle (.sh) lit under the mouse or while trimming: an accent-line band on the edge.
    {
        int   hf = edge_frame, hs = edge_side;
        if (trimming) {
            hf = g_wd.frame;
            hs = g_wd.edge;
        }
        if (hf >= 0 && static_cast<size_t>(hf) < frames.size() && !fade_hot) {
            const SoundFrame& f = frames[static_cast<size_t>(hf)];
            const float       ex = X(hs < 0 ? f.start : f.start + f.length);
            const float       hw = o.big_numbers ? 4.0f : 3.0f;
            dl->AddRectFilled(ImVec2(std::max(a.x, ex - hw), by0), ImVec2(std::min(b.x, ex + hw), by1), ui::kAccentLine);
        }
    }
    if (near_thr) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
    else if (edge_frame >= 0) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);

    if (hovered && !fade_hot && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        g_wd = WaveDrag{};
        g_wd.id = gid;
        g_wd.m0 = m;
        if (near_thr) {
            g_wd.kind = WaveDrag::Thr;
            g_wd.lane = LaneAt(m.y);
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
                char tip[32];
                std::snprintf(tip, sizeof(tip), "%+.1f dB", g_wd.last_db);
                ui::ShowTip(tip);
            } else if (g_wd.kind == WaveDrag::Thr && g_wd.moved) {
                const int    l = std::min(g_wd.lane, lanes - 1);
                const double amp = std::max(1e-4, static_cast<double>(std::fabs(LaneMid(l) - m.y) / std::max(1.0f, hh)));
                g_wd.last_thr = std::clamp(20.0 * std::log10(amp), -90.0, 0.0);
                ev.threshold_live = true;
                ev.threshold_db = g_wd.last_thr;
                char tip[32];
                std::snprintf(tip, sizeof(tip), "%.1f dB", g_wd.last_thr);
                ui::ShowTip(tip);
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
    if (hovered && o.wheel && (io.MouseWheel != 0.0f || io.MouseWheelH != 0.0f)) {
        // The user's REAPER bindings: the action of this wheel with these modifiers.
        const int mods = (io.KeyCtrl ? kViewModCtrl : 0) | (io.KeyAlt ? kViewModAlt : 0) | (io.KeyShift ? kViewModShift : 0);
        auto apply = [&](ViewWheelAction a, float wheel) {
            if (wheel == 0.0f) return;
            if (a == ViewWheelAction::ZoomH) {
                ev.zoom *= std::pow(0.8, static_cast<double>(wheel));
                ev.zoom_at = T(m.x);
            } else if (a == ViewWheelAction::ScrollH) {
                ev.scroll_s += -static_cast<double>(wheel) * vl * 0.1;
            }
        };
        if (o.view_keys) {
            apply(o.view_keys->wheel[mods], io.MouseWheel);
            apply(o.view_keys->hwheel[mods], io.MouseWheelH);  // ImGui: > 0 = left, so a tilt right scrolls right
        } else {
            apply(io.KeyCtrl ? ViewWheelAction::ZoomH : ViewWheelAction::ScrollH, io.MouseWheel);
        }
    }
    // Middle-drag pans (REAPER's default middle-drag in the arrange view: hand scroll).
    if (o.wheel) {
        if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Middle)) P.pan_id = gid;
        if (P.pan_id == gid) {
            if (!ImGui::IsMouseDown(ImGuiMouseButton_Middle)) P.pan_id = 0;
            else if (io.MouseDelta.x != 0.0f) ev.scroll_s += -static_cast<double>(io.MouseDelta.x) / w * vl;
        }
    }
    ImGui::PopID();
    return ev;
}

void SetNodePanelHooks(const NodePanelHooks& hooks) { P.hooks = hooks; }

bool LibraryNodeTakesKey(unsigned vk, bool ctrl, bool shift, bool alt)
{
    const int  f = ImGui::GetCurrentContext() ? ImGui::GetFrameCount() : -100;
    const bool edit = P.edit_open && P.edit_frame >= f - 1;
    if (edit && P.edit_has_focus) {  // the user's zoom keys (Up / Down too) zoom Edit, not the arrange view
        const int mods = (ctrl ? kViewModCtrl : 0) | (alt ? kViewModAlt : 0) | (shift ? kViewModShift : 0);
        for (const auto* list : {&P.keys.zoom_in, &P.keys.zoom_out})
            for (const ViewKeyBinding& b : *list)
                if (!b.is_char && b.mods == mods && b.code == static_cast<int>(vk)) return true;
    }
    if (ctrl || shift || alt) return false;
    if (edit) return vk == 0x20 || vk == 0x1B || vk == 'E' || vk == 'S';
    const bool sound = P.sound_frame >= f - 1;
    return sound && LibraryViewFocused() && (vk == 'E' || vk == 'S');
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
        AuditionFades(d, static_cast<int>(i), &f.in, &f.out, &f.in_shape, &f.out_shape);
        out.push_back(f);
    }
    return out;
}

// A fade handle's events (drag: live then one write on release; Alt-click: that side back to the
// Sound's default, one write). True when the event was a fade one.
bool HandleWaveFades(const WaveformEvents& ev, const SoundLibrary& lib, const SoundNode& n, const std::vector<int>& fi)
{
    auto map = [&](int i) { return i >= 0 && static_cast<size_t>(i) < fi.size() ? fi[static_cast<size_t>(i)] : -1; };
    if (ev.fade_frame >= 0) {
        const int i = map(ev.fade_frame);
        SetFrameFades(Live(n), i, ev.fade_in, ev.fade_out);
        if (ev.fade_done) CommitLive("Variation fade", lib.uid, n.uid);
        return true;
    }
    if (ev.fade_reset >= 0) {
        const int i = map(ev.fade_reset), side = ev.fade_reset_side;
        EditNode("Reset variation fade", lib.uid, n.uid, [i, side](SoundNode& x) {
            if (i < 0 || static_cast<size_t>(i) >= x.sound.frames.size()) return false;
            const SoundFrame& f = x.sound.frames[static_cast<size_t>(i)];
            if ((side < 0 ? f.fade_in : f.fade_out) < 0.0) return false;  // already the default: no empty undo point
            SetFrameFades(x.sound, i, side < 0 ? -1.0 : f.fade_in, side < 0 ? f.fade_out : -1.0);
            return true;
        });
        return true;
    }
    return false;
}

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
                if (P.changed) return;
            }
        } else if (e == ui::DragNumberEvent::Cancel) {
            CancelLive();
        }
    }
}

// Antho 2026-10-13: the Edit window's "Selected (N) in [ms] out [ms]": the selected variations' own
// fades (the first one's value shown, "mixed" when they differ), a drag sets that side on all of
// them (one undo point on release); "Default" puts them back on the Sound's default fades.
void SelectedFadeControls(const SoundLibrary& lib, const SoundNode& n)
{
    const SoundData& d = Shown(n);
    if (FolderSourced(d)) return;
    std::vector<int> sel;
    for (int i : P.sel)
        if (i >= 0 && static_cast<size_t>(i) < d.frames.size()) sel.push_back(i);
    if (sel.empty()) {
        ui::SubText("Select variations to give them their own fades.");
        return;
    }
    const float       fw = ui::Dp(52.0f);
    const std::string label = "Selected (" + std::to_string(sel.size()) + ")";
    ui::LabelRow(label.c_str(), ui::TextWidth("out") + ImGui::GetStyle().ItemSpacing.x + ui::NumberFieldWidth(fw, "ms"));
    ui::ItemTipText("The selected variations' own fades (their fade handles in the waveform)");
    bool own = false;
    for (int side = -1; side <= 1; side += 2) {
        double first = 0.0;
        bool   mixed = false;
        for (size_t k = 0; k < sel.size(); ++k) {
            const SoundFrame& f = d.frames[static_cast<size_t>(sel[k])];
            const FrameFades  ef = EffectiveFades(d, f);
            const double      v = side < 0 ? ef.in : ef.out;
            own = own || (side < 0 ? f.fade_in : f.fade_out) >= 0.0;
            if (k == 0) first = v;
            else if (std::fabs(v - first) > 1e-9) mixed = true;
        }
        if (side > 0) SameLineIfFits(ui::TextWidth("out") + ImGui::GetStyle().ItemSpacing.x + ui::NumberFieldWidth(fw, "ms"));
        FitText(side < 0 ? "in" : "out", ui::kMuted);
        ImGui::SameLine();
        double v = first * 1000.0;
        const ui::DragNumberEvent e = ui::DragNumber(side < 0 ? "##selfadein" : "##selfadeout", &v, 0.1, 1, "ms", fw, /*trim=*/true);
        if (mixed) ui::ItemTipText("mixed: the selected variations differ; a drag sets them all");
        if (e == ui::DragNumberEvent::Live || e == ui::DragNumberEvent::Commit) {
            SoundData&   dr = Live(n);
            const double sec = std::clamp(v, 0.0, 10000.0) / 1000.0;
            for (int i : sel) {
                const SoundFrame& f = dr.frames[static_cast<size_t>(i)];
                SetFrameFades(dr, i, side < 0 ? sec : f.fade_in, side < 0 ? f.fade_out : sec);
            }
            if (e == ui::DragNumberEvent::Commit) {
                CommitLive("Variation fades", lib.uid, n.uid);
                if (P.changed) return;
            }
        } else if (e == ui::DragNumberEvent::Cancel) {
            CancelLive();
        }
    }
    if (own) {
        if (ui::LinkButton("Default##selfadedef")) {
            const std::vector<int> picked = sel;
            EditNode("Variation fades", lib.uid, n.uid, [picked](SoundNode& x) {
                for (int i : picked)
                    if (static_cast<size_t>(i) < x.sound.frames.size()) SetFrameFades(x.sound, i, -1.0, -1.0);
                return true;
            });
        }
        ui::ItemTipText("Back to the Sound's default fades");
    }
}

// Applies a waveform's events to the Sound (live draft or one write). `fi` maps the waveform's
// frame index to the Sound's (Takes lanes show one frame each); `take_len` > 0 = a Takes lane.
void HandleWave(const WaveformEvents& ev, const SoundLibrary& lib, const SoundNode& n, const SoundMedia& m,
                const std::vector<int>& fi, double take_len, bool in_edit)
{
    const SoundCtx c{lib.uid, n.uid};
    auto           map = [&](int i) { return i >= 0 && static_cast<size_t>(i) < fi.size() ? fi[static_cast<size_t>(i)] : -1; };
    const DetectMode mode = n.sound.mode;
    if (HandleWaveFades(ev, lib, n, fi)) return;
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

// Reads the user's REAPER view bindings (REAPER's defaults when the file is absent).
void LoadViewKeys()
{
    std::string text;
    if (const char* rp = GetResourcePath()) {
        std::ifstream f(std::filesystem::u8path(std::string(rp) + "/reaper-kb.ini"), std::ios::binary);
        if (f) text.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    }
    P.keys = ParseViewKeymap(text);
}

ImGuiKey KeyOfVk(int vk)
{
    if (vk >= 'A' && vk <= 'Z') return static_cast<ImGuiKey>(ImGuiKey_A + (vk - 'A'));
    if (vk >= '0' && vk <= '9') return static_cast<ImGuiKey>(ImGuiKey_0 + (vk - '0'));
    if (vk >= 0x60 && vk <= 0x69) return static_cast<ImGuiKey>(ImGuiKey_Keypad0 + (vk - 0x60));
    if (vk >= 0x70 && vk <= 0x7B) return static_cast<ImGuiKey>(ImGuiKey_F1 + (vk - 0x70));
    switch (vk) {
    case 0x6B: return ImGuiKey_KeypadAdd;
    case 0x6D: return ImGuiKey_KeypadSubtract;
    case 0x6A: return ImGuiKey_KeypadMultiply;
    case 0x6F: return ImGuiKey_KeypadDivide;
    case 0xBB: return ImGuiKey_Equal;
    case 0xBD: return ImGuiKey_Minus;
    case 0xBC: return ImGuiKey_Comma;
    case 0xBE: return ImGuiKey_Period;
    case 0xBA: return ImGuiKey_Semicolon;
    case 0xBF: return ImGuiKey_Slash;
    case 0xDB: return ImGuiKey_LeftBracket;
    case 0xDD: return ImGuiKey_RightBracket;
    case 0x21: return ImGuiKey_PageUp;
    case 0x22: return ImGuiKey_PageDown;
    case 0x24: return ImGuiKey_Home;
    case 0x23: return ImGuiKey_End;
    case 0x25: return ImGuiKey_LeftArrow;
    case 0x26: return ImGuiKey_UpArrow;
    case 0x27: return ImGuiKey_RightArrow;
    case 0x28: return ImGuiKey_DownArrow;
    default: return ImGuiKey_None;
    }
}

// True when one of `keys` was pressed this frame (exact modifiers).
bool ViewKeyPressed(const std::vector<ViewKeyBinding>& keys)
{
    const ImGuiIO& io = ImGui::GetIO();
    const int mods = (io.KeyCtrl ? kViewModCtrl : 0) | (io.KeyAlt ? kViewModAlt : 0) | (io.KeyShift ? kViewModShift : 0);
    for (const ViewKeyBinding& b : keys) {
        if (b.is_char) {
            for (int i = 0; i < io.InputQueueCharacters.Size; ++i)
                if (static_cast<int>(io.InputQueueCharacters[i]) == b.code) return true;
        } else if (b.mods == mods) {
            const ImGuiKey k = KeyOfVk(b.code);
            if (k != ImGuiKey_None && ImGui::IsKeyPressed(k, true)) return true;
        }
    }
    return false;
}

// A key zoom: around the mouse when it is over the waveform, else the view's middle.
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

// The Sound's waveform(s): one lane, or one lane per take in Takes mode.
void DrawSoundWaves(const char* id, const SoundLibrary& lib, const SoundNode& n, const SoundMedia& m, float height,
                    bool in_edit, WaveformEvents* first_ev)
{
    const SoundData& d = Shown(n);
    const double     ph = SoundPlayhead(n);
    const std::vector<FrameFades> fades = ShownFades(d);
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
            o.big_numbers = in_edit;
            if (i < fades.size()) o.fades = {fades[i]};
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
    o.big_numbers = in_edit;
    o.fades = fades;
    if (in_edit) {
        o.view_start = P.view_start;
        o.view_len = P.view_len;
        if (d.mode == DetectMode::Silence) o.threshold_db = d.silence.threshold_db;
    }
    if (in_edit) {
        o.view_keys = &P.keys;
        o.size.y = std::max(16.0f, height - kScrollbarH - 4.0f);
    }
    std::vector<int> all;
    for (size_t i = 0; i < d.frames.size(); ++i) all.push_back(static_cast<int>(i));
    const WaveformEvents ev = DrawWaveform(id, m.path, d.frames, in_edit ? P.sel : std::vector<int>{}, ph, o);
    if (first_ev) *first_ev = ev;
    if (in_edit) {
        P.wave_x0 = ev.min.x;
        P.wave_x1 = ev.max.x;
        P.wave_len = m.source_len;
        if (ev.zoom != 1.0) ZoomView(ev.zoom, ev.zoom_at, m.source_len);
        if (ev.scroll_s != 0.0) ScrollView(ev.scroll_s, m.source_len);
        ViewScrollbar(m.source_len);
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
        [guid, mode, frames](SoundNode& x) {
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
        [src, mode, frames, edited = !dropped.empty()](SoundNode& x) {
            x.sound.source = src;
            x.sound.mode = mode;
            x.sound.frames = frames;
            x.sound.edited = edited;
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
// row, or decoded and replacing the Sound's audio. True when a drop was taken (the store may have
// changed: the panel stops drawing for this frame).
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
            P.changed = true;
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

// Run as a picked gesture (after the panel): the file dialog lets REAPER's timers run, and a
// timer may write the store (the User Library's autosave, a Locate result), so no panel reference
// may be held across it. Writes at once (it already runs after the panel).
void Locate(const std::string& lib_uid, const std::string& uid, const SoundSource& src)
{
    std::string path = src.asset.last_path;
    if (!ChooseSoundFile("Locate " + src.asset.name, &path)) return;
    const double len = SourceFileLength(path);
    std::error_code ec;  // the size too, so the file is found again by name, size and length (13-11)
    const auto fsize = std::filesystem::file_size(std::filesystem::u8path(path), ec);
    const int64_t size = ec ? int64_t(-1) : int64_t(fsize);
    WriteNow("Locate sound file",
             NodeEdit(lib_uid, uid,
                      [path, len, size](SoundNode& x) {
                          x.sound.source.kind = SoundSourceKind::File;
                          x.sound.source.asset.last_path = path;
                          x.sound.source.asset.name = FileName(path);
                          if (len > 0.0) x.sound.source.asset.length = len;
                          x.sound.source.asset.size = size;
                          return true;
                      }),
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

// The Detection controls (panel Section and Edit's side strip; `label_col`: the .row > .k 80 px /
// Edit's .edrow 70 px). 13-7b (audit C5.6): "Split by" segmented, the Silence parameters as a
// fader with its number field, the saved-with note, Reset to detected + "edited by hand".
void DetectionControls(const SoundLibrary& lib, const SoundNode& n, const SoundMedia& m, float label_col = ui::kLabelCol)
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
            const std::vector<SoundFrame> frames = Detected(m, mode, d.silence);
            const std::string toast = d.edited && mode != d.mode
                                          ? (mode == DetectMode::Whole ? std::string("Whole file")
                                                                       : std::string("Split by ") + labels[static_cast<size_t>(sel)]) +
                                                ": the hand edits of the previous mode were dropped"
                                          : std::string();
            EditNode("Detect", lib.uid, n.uid, [mode, frames](SoundNode& x) {
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
            double lo, hi;      // what the field takes
            float  flo, fhi;    // the fader's span (the mockup's ranges)
        };
        static const F kFields[] = {
            {"##thr", "Threshold", "dB", &SilenceParams::threshold_db, 0.1, -90.0, 0.0, -60.0f, 0.0f},
            {"##gap", "Min gap", "ms", &SilenceParams::min_gap_ms, 1.0, 0.0, 5000.0, 0.0f, 400.0f},
            {"##pre", "Pre-roll", "ms", &SilenceParams::preroll_ms, 0.5, 0.0, 1000.0, 0.0f, 50.0f},
            {"##tail", "Tail", "ms", &SilenceParams::tail_ms, 1.0, 0.0, 5000.0, 0.0f, 300.0f},
        };
        const float field_w = ui::Dp(56.0f);
        for (const F& f : kFields) {
            ImGui::PushID(f.id);
            const float num_w = ui::NumberFieldWidth(field_w, f.unit);
            ui::LabelRow(f.label, num_w + ui::Dp(40.0f), label_col);
            // The fader (a live draft, one write on release), then the typed / dragged field.
            const std::string key = n.uid + f.id;
            const bool        fader_live = P.detf_key == key;
            float             fv = static_cast<float>(fader_live ? P.detf_live : d.silence.*(f.field));
            bool              beside = true;
            const float room = flow::StretchBeside(ImGui::GetContentRegionAvail().x, ui::Dp(40.0f), num_w,
                                                   ImGui::GetStyle().ItemSpacing.x, &beside);
            ImGui::SetNextItemWidth(std::max(1.0f, room));
            const bool moved = ui::Fader("##fader", &fv, f.flo, f.fhi);
            const bool active = ImGui::IsItemActive(), released = ImGui::IsItemDeactivated();
            const bool cancelled = released && ui::FaderCancelled();  // Esc after typing
            if (cancelled && fader_live) {
                // Esc: the draft is dropped, nothing re-detected or written (no undo point).
                P.detf_key.clear();
                P.detf_gid = 0;
                CancelLive();
            } else if (active || (released && fader_live)) {
                // Not moved yet: the stored value itself (not its float), so a click alone changes nothing.
                if (moved) P.detf_live = std::clamp(static_cast<double>(fv), f.lo, f.hi);
                else if (!fader_live) P.detf_live = d.silence.*(f.field);
                P.detf_key = key;
                P.detf_gid = ImGui::GetItemID();
                P.detf_lib = lib.uid;
                P.detf_uid = n.uid;
                P.detf_field = f.field;
                P.detf_seen = ImGui::GetFrameCount();
                // The draft restarts from the store each frame (Live): this field set again, re-detected.
                SoundData& dr = Live(n);
                dr.silence.*(f.field) = P.detf_live;
                Redetect(dr, m, P.snap.frames);
                if (released) {
                    P.detf_key.clear();
                    P.detf_gid = 0;
                    if (dr.silence.*(f.field) == n.sound.silence.*(f.field)) CancelLive();  // no empty undo point
                    else CommitLive("Detection", lib.uid, n.uid);
                    if (P.changed) {
                        ImGui::PopID();
                        return;
                    }
                }
            }
            if (beside) ImGui::SameLine();
            double v = P.detf_key == key ? P.detf_live : Shown(n).silence.*(f.field);
            const ui::DragNumberEvent e = ui::DragNumber("##num", &v, f.step, 1, f.unit, field_w, /*trim=*/true);
            ImGui::PopID();
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
    ui::SubText(VariationCountText(DetectedCount(n.uid, m, Shown(n)), Shown(n).frames).c_str());
    ui::SubText("Saved with the sound in the library (.ravlib).");
    if (ui::SolidButton("Reset to detected")) {
        const std::vector<SoundFrame> det = Detected(m, d.mode, d.silence);
        EditNode("Reset to detected", lib.uid, n.uid, [det](SoundNode& x) {
            ResetToDetected(x.sound, det);
            return true;
        });
        if (P.changed) return;
    }
    if (Shown(n).edited) {
        SameLineIfFits(ui::TextWidth("edited by hand"));
        FitText("edited by hand", ui::kMuted);
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
    FitText("Normalize peaks to", ui::kText);
    SameLineIfFits(ui::NumberFieldWidth(50.0f, "dBFS"));
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
        EditNode("Normalize", lib.uid, n.uid, [frames](SoundNode& x) {
            x.sound.frames = frames;
            x.sound.edited = true;
            return true;
        });
    }
    ImGui::EndDisabled();
    if (!ready) ui::ItemTipText("Waiting for the waveform");
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
    if (P.keys_stale) {
        P.keys_stale = false;
        LoadViewKeys();
    }
    const SoundMedia& m = MediaOf(*n);
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
        const bool whole_playing = AuditionPlaying() && AuditionTag() == "whole:" + n->uid;
        auto       toggle_play = [&] {
            if (AuditionPlaying()) {
                StopAudition();
                return;
            }
            // Antho 2026-10-13: each variation at its own dB (a Normalize is heard at once, not after
            // Done); Takes mode plays the active take's file as it is.
            const SoundData& d = Shown(*n);
            const double     root_db = GainFromRoot(*lib, *n);
            bool             ok = false;
            if (d.mode != DetectMode::Takes) {
                std::vector<PlayVoice> pv;
                for (const WholeFileStretch& st : WholeFileStretches(d.frames, m.source_len)) {
                    PlayVoice v;
                    v.path = m.path;
                    v.start_s = st.start;
                    v.length_s = st.length;
                    v.delay_s = st.start;  // the audition's clock = the file's time (the playhead)
                    v.gain = DbToGain(root_db + st.db);
                    pv.push_back(v);
                }
                ok = PlayVoices(pv, "whole:" + n->uid);
            } else {
                ok = AuditionFile(m.path, 0.0, 0.0, "whole:" + n->uid, DbToGain(root_db));
            }
            if (!ok) ShowLibraryToast("Cannot play: the sound file is not read", false);
        };
        // Header (13-7b, the mockup's .edhead): "▶ Play" / "■ Stop", the name (15 px semibold) and the
        // count (muted), then the tools at the top right: − + Fit, ?, Done (primary), ✕.
        if (ui::ToggleButton(whole_playing ? "\xE2\x96\xA0 Stop##editplay" : "\xE2\x96\xB6 Play##editplay", whole_playing,
                             ImVec2(0.0f, ui::LargeH())))
            toggle_play();
        ui::ItemTipText("Play the whole file (Space) \xC2\xB7 Esc stops");
        ImGui::SameLine(0.0f, ui::Dp(10.0f));
        const std::string count = VariationCountText(DetectedCount(n->uid, m, Shown(*n)), Shown(*n).frames);
        static const char* const kHelpPlain =
            "Click a variation: play and select (Ctrl-click adds)\nDrag an edge: trim\nDrag up / down inside: its dB\n"
            "Alt-click: off / on\nS: split under the mouse\nDouble-click empty space: add a variation\n"
            "Zoom and scroll: your REAPER mouse wheel and zoom keys, middle-drag, the scrollbar\n"
            "Drag the threshold line (Silence)\nSpace: play / stop, Esc: stop, then close";
        static const char* const kHelpTakes =
            "Click a variation: play and select (Ctrl-click adds)\nDrag an edge: trim (inside its take)\n"
            "Drag up / down inside: its dB\nAlt-click: off / on\nSplit and add: not in Takes mode\n"
            "Space: play / stop, Esc: stop, then close";
        // The tools sit at the top right; the name and count take what they leave (shortened),
        // and go above them only when the panel is too narrow for both (Antho, 13-7b).
        const float sp = ImGui::GetStyle().ItemSpacing.x;
        const float side_btn = ui::ControlH();
        const float tools_w = side_btn * 2.0f + ui::ButtonWidth("Fit") + ui::Dp(18.0f) + ui::ButtonWidth("Done") + side_btn +
                              sp * 5.0f + ui::Dp(10.0f);
        bool        tools_beside = true;
        const float room = flow::StretchBeside(ImGui::GetContentRegionAvail().x, ui::Dp(80.0f), tools_w, sp, &tools_beside);
        {
            const ImVec2 p = ImGui::GetCursorScreenPos();
            const float  h = ui::LargeH();
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
        if (tools_beside) ImGui::SameLine(0.0f, sp);
        else ImGui::SetCursorScreenPos(ImVec2(std::max(ImGui::GetCursorScreenPos().x, ui::LineRight() - tools_w),
                                              ImGui::GetCursorScreenPos().y));
        {
            // The tools' 22 px buttons centred on the 26 px header line.
            const float dy = std::floor((ui::LargeH() - ui::ControlH()) * 0.5f);
            ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x, ImGui::GetCursorScreenPos().y + (tools_beside ? dy : 0.0f)));
        }
        if (ui::SolidButton("\xE2\x88\x92##zoomout", ImVec2(side_btn, 0.0f)))
            ZoomView(1.5, P.view_start + (P.view_len > 0.0 ? P.view_len : m.source_len) * 0.5, m.source_len);
        ui::ItemTipText("Zoom out (your REAPER 'View: Zoom out horizontal' keys)");
        SameLineIfFits(side_btn);
        if (ui::SolidButton("+##zoomin", ImVec2(side_btn, 0.0f)))
            ZoomView(1.0 / 1.5, P.view_start + (P.view_len > 0.0 ? P.view_len : m.source_len) * 0.5, m.source_len);
        ui::ItemTipText("Zoom in (your REAPER 'View: Zoom in horizontal' keys)");
        SameLineIfFits(ui::ButtonWidth("Fit"));
        if (ui::SolidButton("Fit")) {
            P.view_start = 0.0;
            P.view_len = 0.0;
        }
        SameLineIfFits(ui::Dp(18.0f));
        ui::HelpMark("##edithelp", Shown(*n).mode == DetectMode::Takes ? kHelpTakes : kHelpPlain);
        SameLineIfFits(ui::ButtonWidth("Done"));
        if (ui::PrimaryButton("Done")) close = true;
        SameLineIfFits(side_btn);
        if (ui::IconOnlyButton("##editclose", "\xE2\x9C\x95", "Close (Esc)")) close = true;
        {
            // The header's bottom line (.edhead border-bottom), across the whole box.
            const float  y = ImGui::GetCursorScreenPos().y + ui::Dp(1.0f);
            const ImVec2 wp = ImGui::GetWindowPos();
            ImGui::GetWindowDrawList()->AddLine(ImVec2(wp.x, y), ImVec2(wp.x + ImGui::GetWindowSize().x, y), ui::kStroke);
            ImGui::Dummy(ImVec2(1.0f, ui::Dp(6.0f)));
        }

        // Waveform | side strip.
        const float side_w = ui::Dp(270.0f);  // .edmain: 1fr 270px
        const float wave_w = std::max(ui::Dp(100.0f), ImGui::GetContentRegionAvail().x - side_w - ui::Dp(8.0f));
        const float wave_h = std::max(ui::Dp(80.0f), ImGui::GetContentRegionAvail().y - ui::Dp(4.0f));
        ImGui::BeginChild("##editwave", ImVec2(wave_w, wave_h), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollWithMouse);
        if (!m.ok) {
            FitText("Source not found", ui::kMissText);
        } else {
            DrawSoundWaves("editwave", *lib, *n, m, wave_h - 24.0f, true, nullptr);
        }
        ImGui::EndChild();
        if (!P.changed) {
            // The side strip (.edside): surface, a stroke line on its left, the captions
            // "DETECTION · THIS SOUND ONLY" and "LEVEL" over their rows (.edrow: 70 px labels).
            ImGui::SameLine();
            ui::PaneOpts side;  // 13-14: the theme's pane, 10 px inside, square, its edge on the left only
            side.edge = ui::PaneEdge::Left;
            side.pad = 10.0f;
            side.radius = 0.0f;
            ui::BeginPane("##editside", ImVec2(side_w, wave_h), side);
            ui::GroupCaption("Detection \xC2\xB7 this sound only", ui::kMuted);
            if (m.ok) DetectionControls(*lib, *n, m, ui::Dp(70.0f));
            if (!P.changed) {
                ImGui::Dummy(ImVec2(1.0f, ui::Dp(2.0f)));
                const float  y = ImGui::GetCursorScreenPos().y;
                const ImVec2 wp = ImGui::GetWindowPos();
                ImGui::GetWindowDrawList()->AddLine(ImVec2(wp.x + ui::Dp(10.0f), y), ImVec2(wp.x + ImGui::GetWindowSize().x - ui::Dp(10.0f), y),
                                                    ui::kStroke);
                ImGui::Dummy(ImVec2(1.0f, ui::Dp(6.0f)));
                ui::GroupCaption("Level", ui::kMuted);
                if (m.ok) NormalizeControls(*lib, *n, m);
                if (!P.changed) {  // Antho 2026-10-13: the fades here too
                    ImGui::Dummy(ImVec2(1.0f, ui::Dp(6.0f)));
                    ui::GroupCaption("Fades", ui::kMuted);
                    DefaultFadeControls(*lib, *n);
                    if (!P.changed) SelectedFadeControls(*lib, *n);
                }
                ui::SubText("Every change is saved at once (Ctrl+Z to undo), with the sound in the library (.ravlib).");
            }
            ui::EndPane();
        }
        if (keys && !P.changed) {
            if (ImGui::IsKeyPressed(ImGuiKey_Space, false)) toggle_play();
            if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
                if (AuditionPlaying()) StopAudition();
                else close = true;
            }
            if (ImGui::IsKeyPressed(ImGuiKey_E, false)) close = true;
            if (ViewKeyPressed(P.keys.zoom_in)) KeyZoom(1.0 / 1.5);
            else if (ViewKeyPressed(P.keys.zoom_out)) KeyZoom(1.5);
        }
    }
    ui::EndOverlayPanel();
    if (close) {
        P.edit_open = false;
        P.sel.clear();
        CancelLive();
        g_wd = WaveDrag{};  // a drag cut by the close never fires on the next Edit
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
        else PlayNode(lib.uid, n.uid);
    }
    ui::ItemTipText(playing ? "Stop (Esc)" : "Play what Populate would place for one event (Space)");
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
            if (P.changed) return;
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
        if (P.changed) return;
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
        RichText({{value_name(def) + " (the default value) has no sound yet: the library cannot be used until it has one.", ui::kWarn}});
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
        for (const SoundNode* k : kids) {
            if (P.changed) break;
            ImGui::PushID(k->uid.c_str());
            const ImVec2 ip = ImGui::GetCursorScreenPos();
            ui::PaintIconAt(ImGui::GetWindowDrawList(), TypeIcon(k->type), ip.x, ip.y, ImGui::GetFrameHeight());
            ImGui::SetCursorScreenPos(ImVec2(ip.x + icon_col, ip.y));
            const float avail = ImGui::GetContentRegionAvail().x;
            const float name_col = std::clamp(avail * 0.4f, ui::Dp(80.0f), ui::Dp(150.0f));
            ui::LabelRowText(MixLabel(store, n, *k), ui::Dp(120.0f), name_col, IsLit(k->uid) ? ui::kAccent : ui::kText);
            double v = 0.0;
            const std::string kid = k->uid;
            if (VolumeControl("##mixvol", lib.uid, kid, k->volume_db, &v, 1e9f))
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

// 13-7b (audit C4.8): the Blend layer view (.blendview) -- a dark box (r7, pad 6); a header row
// with the "ms" caption, the ruler (mono 10 faint, labels centred on their time) and the
// "offset" / "volume" captions; then one row per layer: its icon and name, its track (surface,
// stroke edge, the zero line) with the bar (solid accent, hatched where the length varies; drag
// it to offset the layer, its "+12 ms" chip on it), its offset (ms) and its volume (dB) beside
// the track. A Blend has no Mix section: the lane's dB is its child's only volume (Antho
// 2026-10-12, one control per value). A narrow column puts the two fields under the track.
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

    ImDrawList*  dl = ImGui::GetWindowDrawList();
    const ImGuiIO& io = ImGui::GetIO();
    const float  pad = ui::Dp(6.0f), gap = ui::Dp(6.0f);
    const ImVec2 box0 = ImGui::GetCursorScreenPos();
    const float  box_x1 = std::max(box0.x + ui::Dp(60.0f), ui::LineRight());
    const float  x0 = box0.x + pad;
    const float  fh = ImGui::GetFrameHeight();
    const float  ms_w = ui::Dp(52.0f), db_w = ui::Dp(46.0f);
    const float  ms_full = ui::NumberFieldWidth(ms_w, "ms"), db_full = ui::NumberFieldWidth(db_w, "dB");
    const float  fields_w = ms_full + gap + db_full;
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
    // The box behind (its height is known beforehand: one row per layer, up to the cap).
    dl->AddRectFilled(box0, ImVec2(box_x1, box0.y + box_h), ui::kBg, ui::kRadiusMd);
    dl->AddRect(box0, ImVec2(box_x1, box0.y + box_h), ui::kStroke, ui::kRadiusMd);
    auto X = [&](double ms) { return lane_x0 + static_cast<float>(ms / total_ms) * lane_w; };

    // Header: "ms" | the ruler | "offset" | "volume".
    {
        const float  y = box0.y + pad;
        ImFont*      cf = ui::FontTiny();
        ImFont*      rf = ui::FontMonoTiny();
        const float  cy = y + std::floor((head_h - cf->FontSize) * 0.5f);
        ui::AddTextIn(dl, cf, ImVec2(x0, cy), ui::kFaint, "ms");
        if (!stacked) {
            ui::AddTextIn(dl, cf, ImVec2(x1 - fields_w, cy), ui::kFaint, "offset");
            ui::AddTextIn(dl, cf, ImVec2(x1 - db_full, cy), ui::kFaint, "volume");
        }
        dl->PushClipRect(ImVec2(lane_x0 - ui::Dp(20.0f), y), ImVec2(lane_x0 + lane_w + ui::Dp(20.0f), y + head_h), true);
        for (double t = 0.0; t <= total_ms + 1e-6; t += step) {
            if (t / total_ms > 0.94) break;  // the last label would run past the track
            char lab[32];
            std::snprintf(lab, sizeof(lab), "%.0f", t);
            const ImVec2 ts = ui::TextSizeIn(rf, lab);
            ui::AddTextIn(dl, rf, ImVec2(std::floor(X(t) - ts.x * 0.5f), y + std::floor((head_h - ts.y) * 0.5f)), ui::kFaint, lab);
        }
        dl->PopClipRect();
    }
    // The lanes in a child of their own (scrolled past the cap); drawn at the same screen x.
    ImGui::SetCursorScreenPos(ImVec2(box0.x + 1.0f, box0.y + pad + head_h));
    ImGui::BeginChild("##blendlanes", ImVec2(std::max(1.0f, box_x1 - box0.x - 2.0f), lanes_h), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoBackground);
    dl = ImGui::GetWindowDrawList();
    const float lanes_top = ImGui::GetCursorScreenPos().y;
    float       y = lanes_top;
    for (const SoundNode* k : parts) {
        if (P.changed) break;
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
        dl->AddLine(ImVec2(std::floor(X(0.0)) + 0.5f, ty), ImVec2(std::floor(X(0.0)) + 0.5f, ty + th), ui::kStrokeStrong);
        // The bar.
        const LengthRange r = NodeLengthRange(store, lib, kid);
        const double      off = P.off_uid == kid ? P.off_live : k->offset_ms;
        const float       bx0 = X(off);
        const float       bx1 = std::min(lane_x0 + lane_w, bx0 + std::max(ui::Dp(3.0f), static_cast<float>(r.max_s * 1000.0 / total_ms) * lane_w));
        const ImVec2      ba(bx0, ty + 3.0f), bb(std::max(bx0 + 3.0f, bx1), ty + th - 3.0f);
        dl->AddRectFilled(ba, bb, ui::kAccent, 3.0f);
        if (r.varies()) {
            const float bmin = X(off + r.min_s * 1000.0);
            Hatch(dl, ImVec2(std::max(ba.x, bmin), ba.y), bb, ui::kHatchLight);
        }
        ImGui::SetCursorScreenPos(ImVec2(bx0, ty));
        ImGui::InvisibleButton("##bar", ImVec2(std::max(ui::Dp(6.0f), bb.x - bx0), th));
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
            P.off_scale = total_ms;
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
            const ImVec2 c0(std::min(bx0 + 4.0f, lane_x0 + lane_w - ts.x - 8.0f), ty + std::floor((th - ts.y - 2.0f) * 0.5f));
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
        if (P.changed) {
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
        if (P.changed) {
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
        ImGui::PopID();
        y += row_h + rgap;
    }
    if (!P.changed && NodePlaying(n.uid)) {
        const float x = std::floor(X(AuditionPosition() * 1000.0)) + 0.5f;
        if (x <= lane_x0 + lane_w) dl->AddLine(ImVec2(x, lanes_top), ImVec2(x, y), ui::kText, 1.0f);
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
        if (!P.changed) {
            if (nk == 0) RichText({{"Empty: add sounds, or the library cannot be assigned.", ui::kWarn}});
            ui::SubText("Drop files here to add them as children.");
        }
    }
    ui::EndSection(s);
}

// 13-7b (audit C5.3): the source's state under its waveform -- an item: [Show in REAPER] at the
// right; a file: its name (text) and [Show in Media Explorer] at the right. Missing: the red line
// (#ffc4c4) with [Set from selected item] / [Locate…].
void DrawSourceLine(const SoundLibrary& lib, const SoundNode& n, const SoundMedia& m)
{
    const SoundSource& src = n.sound.source;
    const std::string  lib_uid = lib.uid, uid = n.uid;
    const float        sp = ui::Dp(8.0f);
    if (!m.ok) {
        const bool        item = src.kind == SoundSourceKind::Item;
        const char*       btn = item ? "Set from selected item" : "Locate\xE2\x80\xA6";
        const std::string msg = item ? std::string("Its item is no longer on the tracks.")
                                     : (src.asset.last_path.empty() ? std::string("Its file") : src.asset.last_path) + " not found on this PC.";
        const float bw = ui::ButtonWidth(btn);
        bool        beside = true;
        const float room = flow::StretchBeside(ImGui::GetContentRegionAvail().x, ui::Dp(60.0f), bw, sp, &beside);
        FitText(msg, ui::kMissName, room);
        if (beside) ImGui::SameLine(0.0f, sp);
        if (ui::SolidButton(btn)) {
            if (item) ReplaceFromSelected(lib_uid, uid, "Set from selected item");
            else {
                P.changed = true;  // a gesture is picked: the panel stops drawing for this frame
                g_picks.Pick([lib_uid, uid, src]() { Locate(lib_uid, uid, src); });
            }
        }
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
                EditNode("Detect", lib_uid, uid, [mode, frames](SoundNode& x) {
                    x.sound.mode = mode;
                    x.sound.frames = frames;
                    x.sound.edited = false;
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
        } else {
            WaveformEvents ev;
            DrawSoundWaves("panelwave", lib, n, m, wave_h, false, &ev);
            if (P.changed) {
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
            if (ui::BeginMenuPopup("##wavemenu")) {
                const bool go = ImGui::MenuItem("Replace with selected items");
                ui::EndMenuPopup();
                if (go) {
                    ReplaceFromSelected(lib_uid, uid, "Replace audio");
                    ui::EndSection(src_sec);
                    return;
                }
            }
        }
        DrawSourceLine(lib, n, m);
    }
    ui::EndSection(src_sec);
    if (P.changed) return;

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
        if (!P.changed) DrawRandomAvoid(lib, n);
        if (!P.changed) DefaultFadeControls(lib, n);
    }
    ui::EndSection(var_sec);
    if (P.changed) return;

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
    if (P.detf_gid != 0 && active != P.detf_gid) {
        const bool fresh = P.detf_seen >= frame - 1;
        P.detf_gid = 0;
        P.detf_key.clear();
        if (!P.live || P.draft_uid != P.detf_uid || !P.detf_field) return;
        const SoundLibrary* l = fresh ? FindLibrary(GetSoundStore(nullptr), P.detf_lib) : nullptr;
        const SoundNode*    nd = l ? FindNode(*l, P.detf_uid) : nullptr;
        if (nd && P.draft.silence.*(P.detf_field) != nd->sound.silence.*(P.detf_field))
            CommitLive("Detection", P.detf_lib, P.detf_uid);
        else
            CancelLive();
    }
}

void NodePanelWidgets()
{
    P.changed = false;
    SettleLostFaders();
    if (P.changed) return;  // the panel draws again once that value is written
    // A live edit only lasts while its drag does (an unfinished one never shadows the store).
    // (13-14: a fader typed into holds no mouse button either.)
    if (P.live && !ImGui::IsMouseDown(ImGuiMouseButton_Left) && !ui::DragNumberActive() && !ui::FaderTyping()) CancelLive();
    MergeTreeHooks();
    AuditionPlaying();  // frees an audition that has played to its end
    const SoundStore&   store = GetSoundStore(nullptr);
    const SoundLibrary* lib = FindLibrary(store, SelectedLibraryUid());
    const std::string   uid = lib ? LibraryRowNode() : std::string();
    const SoundNode*    n = lib && !uid.empty() ? FindNode(*lib, uid) : nullptr;
    if (P.live && (!n || P.draft_uid != uid)) CancelLive();
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
            else PlayNode(lib->uid, uid);
        }
    }
    ImGui::PushID(uid.c_str());
    ImGui::Dummy(ImVec2(0.0f, ui::Dp(2.0f)));
    DrawHeader(store, *lib, *n);
    // 13-7b (audit C4.2 / C5.2): Type / Volume are plain rows under the header (no "Node" / "Level"
    // card); then Mix (Random, Switch), then Plays (Random, Blend: a Blend's lanes carry its
    // children's volumes, it has no Mix).
    if (!P.changed && n->type != SoundNodeType::Sound) {
        DrawTypeRows(store, *lib, *n);
        if (!P.changed) DrawVolume(*lib, *n);
        if (!P.changed && n->type != SoundNodeType::Blend) DrawMix(store, *lib, *n);
        if (!P.changed && n->type != SoundNodeType::Switch) DrawPlays(store, *lib, *n);
    } else if (!P.changed) {
        DrawVolume(*lib, *n);
        if (!P.changed) {
            OpenEditKey(*lib, *n);
            DrawSoundPanel(*lib, *n);
        }
    }
    // The whole node panel takes a Media Explorer drop too (after the waveform's own target, so
    // the waveform keeps it when the mouse is over it).
    if (!P.changed) {
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
    if (!P.changed) DrawEditOverlay();
}

}  // namespace

void DrawNodePanel()
{
    NodePanelWidgets();
    g_picks.Run();  // the gestures picked above, the panel and the Edit overlay drawn
}

void SettleNodePanelFaders()
{
    P.changed = false;
    SettleLostFaders();
    g_picks.Run();
}

}  // namespace rav

#endif  // _WIN32
