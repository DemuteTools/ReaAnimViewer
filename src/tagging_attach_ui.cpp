// SPDX-License-Identifier: MIT
//
// See tagging_attach_ui.h (13-10): the Attach button and its menu (Attach to nearest event,
// Attach Selected to Markers > rule, Spread to > rule, Promote to Library > library (> value)),
// and the result note with Undo under the footer row.

#include "tagging_attach_ui.h"

#ifdef _WIN32

#include <functional>
#include <string>
#include <vector>

#include "imgui.h"
#include "rule_record.h"
#include "sound_attach.h"
#include "sound_project.h"
#include "sound_record.h"
#include "tagging_session.h"
#include "ui_theme.h"

namespace rav {
namespace {

// Strings outside ASCII are spelled as UTF-8 escapes (the build has no /utf-8).
#define RAV_ATTACH_LABEL "Attach \xE2\x96\xBE"

constexpr double kNoteSeconds = 6.0;

std::function<void()> g_pending;    // the gesture picked this frame, run on the next one
std::string           g_note;
bool                  g_note_undo = false;
double                g_note_until = 0.0;
int                   g_note_state = -1;    // the project state right after the gesture

// The selection, read again when the project or the selection changes.
AttachSelectionInfo g_sel;
int                 g_sel_state = -1;
int                 g_sel_count = -1;
void*               g_sel_first = nullptr;

const AttachSelectionInfo& Selection()
{
    const int   state = GetProjectStateChangeCount(nullptr);
    const int   count = CountSelectedMediaItems(nullptr);
    void* const first = count > 0 ? static_cast<void*>(GetSelectedMediaItem(nullptr, 0)) : nullptr;
    if (state != g_sel_state || count != g_sel_count || first != g_sel_first) {
        g_sel = ReadAttachSelection();
        g_sel_state = state;
        g_sel_count = count;
        g_sel_first = first;
    }
    return g_sel;
}

// Shows a gesture's result. Its Undo stays only while the gesture is REAPER's last change.
void ShowResult(const AttachResult& r)
{
    g_note = r.note;
    g_note_undo = r.wrote;
    g_note_until = ImGui::GetTime() + kNoteSeconds;
    g_note_state = -1;  // read on the next draw, once the gesture's undo point is closed
    g_sel_state = -1;
}

void Run(AttachResult (*fn)(MediaItem*, const std::vector<AttachEvent>&, int), MediaItem* item,
         std::vector<AttachEvent> events, int block)
{
    g_pending = [fn, item, events, block]() { ShowResult(fn(item, events, block)); };
}

AttachResult Nearest(MediaItem* item, const std::vector<AttachEvent>& events, int)
{
    return AttachToNearest(item, events);
}

std::string RuleLabel(const ItemRules& rules, int b)
{
    const Block& blk = rules.blocks[static_cast<size_t>(b)];
    std::string  name = blk.marker.empty() ? "Rule " + std::to_string(b + 1) : blk.marker;
    return name + "##attrule" + std::to_string(b);
}

// A tooltip on the item drawn last, also when it is disabled.
void Why(const char* text)
{
    if (text && *text && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled | ImGuiHoveredFlags_ForTooltip))
        ImGui::SetTooltip("%s", text);
}

// The rule submenu body: the blocks that have at least one event.
void RuleItems(const TaggingSoundContext& ctx, const std::vector<AttachEvent>& events,
               AttachResult (*fn)(MediaItem*, const std::vector<AttachEvent>&, int))
{
    const ItemRules& rules = *ctx.rules;
    bool             any = false;
    for (int b = 0; b < static_cast<int>(rules.blocks.size()); ++b) {
        bool has = false;
        for (const AttachEvent& e : events) has = has || e.block == b;
        if (!has) continue;
        any = true;
        if (ImGui::MenuItem(RuleLabel(rules, b).c_str())) Run(fn, ctx.item, events, b);
    }
    if (!any) ImGui::TextDisabled("No rule has events");
}

void PromoteItems()
{
    const SoundStore& store = GetSoundStore(nullptr);
    for (const SoundLibrary& lib : store.libraries) {
        const std::string label = (lib.name.empty() ? std::string("Library") : lib.name) + "##attlib" + lib.uid;
        const SoundNode*  root = FindNode(lib, lib.root);
        const std::string lib_uid = lib.uid;
        if (root && root->type == SoundNodeType::Switch) {
            const SwitchGroup* g = FindGroup(store, root->group_uid);
            const bool         values = g && !g->values.empty();
            if (!values) {
                ImGui::MenuItem(label.c_str(), nullptr, false, false);
                Why("Its Switch has no values: add one in the Library view.");
                continue;
            }
            if (ImGui::BeginMenu(label.c_str())) {
                for (const SwitchValue& v : g->values) {
                    const std::string vl = (v.name.empty() ? std::string("Value") : v.name) + "##attval" + v.uid;
                    if (ImGui::MenuItem(vl.c_str())) {
                        const std::string value_uid = v.uid;
                        g_pending = [lib_uid, value_uid]() { ShowResult(PromoteToLibrary(lib_uid, value_uid)); };
                    }
                }
                ImGui::EndMenu();
            }
            continue;
        }
        if (ImGui::MenuItem(label.c_str())) {
            g_pending = [lib_uid]() { ShowResult(PromoteToLibrary(lib_uid, std::string())); };
        }
    }
}

}  // namespace

float AttachButtonWidth()
{
    return ImGui::CalcTextSize(RAV_ATTACH_LABEL).x + ImGui::GetStyle().FramePadding.x * 2.0f + 8.0f;
}

void DrawAttachButton(const TaggingSoundContext& ctx)
{
    if (g_pending) {
        std::function<void()> f;
        f.swap(g_pending);
        f();
    }
    const TaggingModel* m = ctx.model;
    // The events (D9): kind Detected or User, at every project time.
    std::vector<AttachEvent> events;
    if (m && m->detected && !m->previewing) events = AttachEventsOf(m->events, m->map);

    const AttachSelectionInfo& sel = Selection();
    const char*                why = nullptr;
    if (!ctx.item || !m || !ctx.rules) why = "No animation item shown.";
    else if (m->previewing) why = "Finish the edit first.";
    else if (sel.eligible == 0)
        why = sel.skipped > 0 ? "The selected items cannot be attached (animation, folder or placed by RAV)."
                              : "Select the sound items to attach in REAPER.";
    else if (events.empty()) why = "This item has no events: detect or add some first.";

    ImGui::SameLine();
    const bool disabled = why != nullptr;
    if (disabled) ImGui::BeginDisabled();
    if (ui::SolidButton(RAV_ATTACH_LABEL "##tagattach", ImVec2(AttachButtonWidth(), 0.0f))) ImGui::OpenPopup("##tagattachmenu");
    if (disabled) ImGui::EndDisabled();
    if (disabled) Why(why);
    else Why("Attach the items selected in REAPER to this animation's events.");

    if (ImGui::BeginPopup("##tagattachmenu")) {
        if (ImGui::MenuItem("Attach to nearest event")) Run(&Nearest, ctx.item, events, -1);
        Why("Locks the selected items, as one pile, to the nearest event.");
        if (ImGui::BeginMenu("Attach Selected to Markers")) {
            RuleItems(ctx, events, &AttachToMarkers);
            ImGui::EndMenu();
        }
        Why("Each group of overlapping items goes to its nearest event of the rule.");
        if (ImGui::BeginMenu("Spread to")) {
            RuleItems(ctx, events, &SpreadTo);
            ImGui::EndMenu();
        }
        Why("Copies the selected pile onto every other event of the rule.");
        const bool        libs = !GetSoundStore(nullptr).libraries.empty();
        const char*       pwhy = sel.attached == 0 ? "Select an attached sound first." : !libs ? "No library yet: create one in the Library view." : nullptr;
        if (pwhy) {
            ImGui::MenuItem("Promote to Library", nullptr, false, false);
            Why(pwhy);
        } else if (ImGui::BeginMenu("Promote to Library")) {
            PromoteItems();
            ImGui::EndMenu();
        }
        if (!sel.skip_note.empty()) ImGui::TextDisabled("%s", sel.skip_note.c_str());
        ImGui::EndPopup();
    }

    // The result note (6 s) with Undo.
    if (!g_note.empty() && ImGui::GetTime() < g_note_until) {
        ImGui::AlignTextToFramePadding();
        ImGui::PushStyleColor(ImGuiCol_Text, ui::Col(ui::kMuted));
        ImGui::TextUnformatted(g_note.c_str());
        ImGui::PopStyleColor();
        // Any later change (another edit, an undo) retires the Undo: it would undo that instead.
        const int state = GetProjectStateChangeCount(nullptr);
        if (g_note_undo && g_note_state < 0) g_note_state = state;
        else if (g_note_undo && state != g_note_state) g_note_undo = false;
        if (g_note_undo) {
            ImGui::SameLine();
            if (ImGui::SmallButton("Undo##tagattachundo")) {
                Main_OnCommand(40029, 0);
                g_note.clear();
                g_sel_state = -1;
            }
        }
    }
}

}  // namespace rav

#endif  // _WIN32
