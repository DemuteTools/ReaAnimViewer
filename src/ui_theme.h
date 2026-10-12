// SPDX-License-Identifier: MIT
//
// The house theme of Demute's tools (DM-XYZ-Pad lib/ui/theme.lua, validated mock-up
// maquette-refonte-ui-v2), laid over Dear ImGui (Story 11-4): neutral greys from #121317
// to #282c34, text #e7e9ee / #8b919c, one accent #5b8cff, radii 10 / 7 / 5, ghost
// buttons (transparent until hovered), segmented controls, thin discreet scrollbar.
//
// ApplyRavTheme sets the live ImGui style once (the whole viewer UI uses it). The
// helpers draw the theme's components that ImGui has no widget for. ImGui only; call
// between NewFrame and Render (except ApplyRavTheme).
//
// =====================================================================================
// 13-7 (visual parity with maquette-sound-library-v11.html, audit sections A and B):
// THE API FOR PANELS. Use these instead of stock ImGui widgets wherever the mockup has
// its own look; every one is width-aware (ui_flow.h rules: shrink / ellipsize / wrap,
// never clipped by its column) and scales with the screen's DPI (Dp).
//
//  Metrics (CSS px of the mockup, times the DPI scale)
//    Dp(px)            a mockup pixel size at the screen's DPI (UiScale())
//    ControlH()  22    small buttons (.btn.sm), segment options, number fields = the default
//                      frame height (FramePadding 8,3 with the 12 px body font)
//    FieldH()    24    text fields, combos (input.txt, select), toolbar buttons (.tbb)
//    LargeH()    26    full-size buttons (.btn, the Play button): SolidButton(l, {0, LargeH()})
//    RowH()      28    tree / list rows (.trow, .lrow)
//    SectionHeaderH() 26, MenuRowH() 26, kCardGap 8, kLabelCol 80 (the .row > .k column)
//    ItemSpacing (6, 7) is the theme's default (.row gap 6, .secx-b gap 7).
//
//  Fonts (Segoe UI; semibold = Segoe UI Semibold; mono = Cascadia Mono / Consolas)
//    FontBody 12 (the default: controls, labels)   FontTitle 13 (rows, section titles, toast)
//    FontSmall 11 (captions, tags, counts)         FontTiny 10 (units, the "default" tag)
//    FontBadge 9 (the "new" badge)                 FontBig 15
//    FontSemibold 12/600 (bold list name)          FontCaptionBold 11/600 (missing count)
//    FontHeading 14/600 (library name, Sound name) FontPaneTitle 15/600 (node title)
//    FontMono 12, FontMonoSmall 11 (number fields), FontMonoTiny 10, FontMonoMicro 9
//    Font(FontRole), FontScope (RAII PushFont/PopFont), TextSizeIn(font, text).
//    A role whose file is missing answers the nearest one that loaded: always safe.
//
//  Colours: the mockup's tokens below (kBad, kBadSoft, kAccentInk, kAccentText, kMissText,
//    kMissName, kRowAlt, kUnassignedName, kUnassignedSoft, kUserInk...).
//
//  Widgets
//    Section     BeginSection(s, title, SectionOpts) / EndSection: card r10, 26 px header,
//                fold triangle on the LEFT (foldable) or none (fixed), `suffix` in faint
//                11 px after the title ("· 2 children"), `right_w` room for header widgets
//                (BeginSectionHeaderRight / EndSectionHeaderRight), nested = kBg card.
//    Buttons     SolidButton, PrimaryButton (ghost = ImGui::Button), ToggleButton (.btn.on),
//                IconOnlyButton (22 px ghost glyph / icon, .btn.sm.ico / .colbtn),
//                LinkButton (.linkbtn), AddButton (dashed .addbtn), HelpMark ("?" circle),
//                MenuButton / IconButton (ui_flow.h)
//    Segmented   22 px options, 13 px icons, per-option badge ("new")
//    Fader       thin track, accent fill, round thumb (B5); IsItemActive/Deactivated hold
//    DragNumber  22 px raised field, mono 11 right-aligned, unit 10 px faint (B6);
//                `trim` shows "0" / "-14.5" instead of "0.0"
//    TextField   dark field + stroke edge, accent-line edge while typing (B7);
//                TitleField (transparent until hovered, 14/600), RenameField (accent-line
//                edge, 22 px), AddField (dashed "+ value")
//    Combo       BeginComboStyled / EndComboStyled (24 px, r5, drawn chevron, menu popup)
//    Checkbox    14 px box, accent fill + white tick when on, label clickable (B9)
//    Rows        SelectRow (a Selectable drawn as the mockup's row), RowBackground (any
//                custom row: hover raised, selected accent-soft + accent-line edge, lit /
//                current / even-row variants)
//    Chevron     an 18 px fold button (tree / groups), DrawFoldTriangle
//    Tags        Tag (.tag pill, tones), SmallTag ("default", .dtag), CountChip ("→ 3")
//    Menus       BeginMenuPopup / BeginMenuPopupContextItem / EndMenuPopup (r7, stroke-
//                strong edge, pad 4, 26 px rows, min 220 wide), MenuCaption ("SWITCH ON")
//    Toast       BeginToast / EndToast (pad 14 8, 13 px), ToastNote (11 px faint)
//    Splitter    SplitterBar (8 px hit, accent-line on hover, accent while dragged)
//    DashedRect  a dashed rounded outline (drop hints, the empty-library box)
// =====================================================================================

#pragma once

#ifdef _WIN32

#include <imgui.h>

#include <cstddef>
#include <string>

namespace rav {
namespace ui {

// ---- palette (theme.lua PALETTE + the mock-up's state colours) -----------------------
constexpr ImU32 kBg           = IM_COL32(0x12, 0x13, 0x17, 0xFF);
constexpr ImU32 kSurface      = IM_COL32(0x19, 0x1B, 0x20, 0xFF);
constexpr ImU32 kRaised       = IM_COL32(0x20, 0x23, 0x2A, 0xFF);
constexpr ImU32 kHover        = IM_COL32(0x28, 0x2C, 0x34, 0xFF);
constexpr ImU32 kStroke       = IM_COL32(0x2B, 0x2F, 0x37, 0xFF);
constexpr ImU32 kStrokeStrong = IM_COL32(0x3A, 0x3F, 0x49, 0xFF);
constexpr ImU32 kFrameHover   = IM_COL32(0x47, 0x4D, 0x59, 0xFF);  // a control under the mouse
constexpr ImU32 kText         = IM_COL32(0xE7, 0xE9, 0xEE, 0xFF);
constexpr ImU32 kMuted        = IM_COL32(0x8B, 0x91, 0x9C, 0xFF);
constexpr ImU32 kFaint        = IM_COL32(0x5D, 0x63, 0x6E, 0xFF);
constexpr ImU32 kAccent       = IM_COL32(0x5B, 0x8C, 0xFF, 0xFF);
constexpr ImU32 kAccentSoft   = IM_COL32(0x5B, 0x8C, 0xFF, 0x29);  // --accent-soft (.16)
constexpr ImU32 kAccentLine   = IM_COL32(0x5B, 0x8C, 0xFF, 0x8C);  // --accent-line (.55)
constexpr ImU32 kPrimaryHover = IM_COL32(0x6D, 0x99, 0xFF, 0xFF);
constexpr ImU32 kOk           = IM_COL32(0x5F, 0xDD, 0x9E, 0xFF);
constexpr ImU32 kWarn         = IM_COL32(0xFF, 0xD1, 0x66, 0xFF);
// Story 10-3b -- the amber in-place confirm (overwrite / delete) and the Legacy band: its
// outline (kWarn at .45), its action button and that button's text.
constexpr ImU32 kConfirmLine      = (kWarn & ~IM_COL32_A_MASK) | (0x73u << IM_COL32_A_SHIFT);
constexpr ImU32 kConfirmAct       = IM_COL32(0xD9, 0xA1, 0x2B, 0xFF);
constexpr ImU32 kConfirmActHover  = IM_COL32(0xE6, 0xB0, 0x3E, 0xFF);
constexpr ImU32 kConfirmActText   = IM_COL32(0x1A, 0x1A, 0x1A, 0xFF);
constexpr ImU32 kWhite        = IM_COL32(0xFF, 0xFF, 0xFF, 0xFF);
constexpr ImU32 kFrameStroke  = IM_COL32(0xE7, 0xE9, 0xEE, 0x47);  // the Video view frame's edge (text at .28)

// 13-7 (audit A5): the Sound Library mockup's other tokens.
constexpr ImU32 kNote           = kWarn;                                // --note: the "new" badge, notes
constexpr ImU32 kBad            = IM_COL32(0xF0, 0x6C, 0x6C, 0xFF);     // --bad: missing counts, errors
constexpr ImU32 kBadSoft        = IM_COL32(0xF0, 0x6C, 0x6C, 0x1F);     // --bad-soft (.12): banners, missing cells
constexpr ImU32 kBadLine        = IM_COL32(0xF0, 0x6C, 0x6C, 0x99);     // .tag.bad edge (.6)
constexpr ImU32 kBadInk         = IM_COL32(0xFF, 0xB3, 0xB3, 0xFF);     // .tag.bad text
constexpr ImU32 kAlert          = IM_COL32(0xF0, 0x3C, 0x3C, 0xFF);     // --alert
constexpr ImU32 kAccentText     = IM_COL32(0xDF, 0xE8, 0xFF, 0xFF);     // text on accent-soft (.btn.on, chips, "new")
constexpr ImU32 kAccentInk      = IM_COL32(0xA8, 0xC2, 0xFF, 0xFF);     // container icons, .tag.proj, links, .snd
constexpr ImU32 kMissText       = IM_COL32(0xFF, 0x9A, 0x9A, 0xFF);     // "missing" (.rt.miss), .cell .meta
constexpr ImU32 kMissName       = IM_COL32(0xFF, 0xC4, 0xC4, 0xFF);     // a missing sound's name (.nm.missn)
constexpr ImU32 kRowAlt         = IM_COL32(0xFF, 0xFF, 0xFF, 0x06);     // every other Media Explorer row (.025)
constexpr ImU32 kUnassignedName = IM_COL32(0xF5, 0xD3, 0x8A, 0xFF);     // an Unassigned child's name
constexpr ImU32 kUnassignedSoft = IM_COL32(0xFF, 0xD1, 0x66, 0x0D);     // the Unassigned rows' fill (.05)
constexpr ImU32 kUserInk        = IM_COL32(0xC9, 0xB8, 0xFF, 0xFF);     // .tag.user text
constexpr ImU32 kUserLine       = IM_COL32(0x9B, 0x7B, 0xFF, 0x80);     // .tag.user edge (.5)
constexpr ImU32 kWarnLine       = IM_COL32(0xFF, 0xD1, 0x66, 0x80);     // .tag.warn edge (.5)
constexpr ImU32 kOkLine         = IM_COL32(0x5F, 0xDD, 0x9E, 0x73);     // .tag.ok edge (.45)

// The curve palette (theme.lua CURVE_PALETTE, the mock-up's shot colours): same
// saturation and lightness, only the hue changes. Shot i takes kCurvePalette[i % 8].
constexpr ImU32 kCurvePalette[8] = {
    IM_COL32(0xDD, 0x5F, 0x5F, 0xFF), IM_COL32(0x5F, 0xDD, 0xDD, 0xFF), IM_COL32(0xDD, 0xDD, 0x5F, 0xFF),
    IM_COL32(0x5F, 0x5F, 0xDD, 0xFF), IM_COL32(0x5F, 0xDD, 0x5F, 0xFF), IM_COL32(0xDD, 0x5F, 0xDD, 0xFF),
    IM_COL32(0xDD, 0x9E, 0x5F, 0xFF), IM_COL32(0x5F, 0x9E, 0xDD, 0xFF),
};

// ---- radii (theme.lua RADIUS_LG / MD / SM) ----------------------------------------------
constexpr float kRadiusLg = 10.0f;
constexpr float kRadiusMd = 7.0f;
constexpr float kRadiusSm = 5.0f;

ImVec4 Col(ImU32 c);

// Lays the theme over the live style (call once after ImGui::CreateContext).
void ApplyRavTheme();

// ---- scale and metrics (13-7, audit A2) -------------------------------------------------
// The screen's DPI over 96 (the viewer sets it once, with the style's ScaleAllSizes).
void  SetUiScale(float scale);
float UiScale();
// A size in the mockup's CSS px at the screen's DPI.
inline float Dp(float px) { return px * UiScale(); }
float ControlH();        // 22: .btn.sm, .seg .btn, .lnum (the default frame height)
float FieldH();          // 24: input.txt, select, .tbb
float LargeH();          // 26: .btn, .playbig
float RowH();            // 28: .trow, .lrow
float SectionHeaderH();  // 26: .secx-h
float MenuRowH();        // 26: .pop .mi

// ---- fonts (13-7: the mock-up's --font Segoe UI and --mono Cascadia Mono / Consolas) ----
// The viewer builds the atlas and registers its fonts here once; a role whose font could
// not be loaded answers the nearest one that did (semibold -> regular -> the default), so
// ImGui::PushFont(ui::FontMono()) is always safe. Sizes are the mock-up's CSS pixels.
struct FontSet {
    ImFont* body  = nullptr;  // 12 (the default)
    ImFont* small = nullptr;  // 11
    ImFont* title = nullptr;  // 13
    ImFont* big   = nullptr;  // 15
    ImFont* mono  = nullptr;  // 12
    // 13-7 (audit A1): the rest of the mockup's type scale.
    ImFont* tiny          = nullptr;  // 10
    ImFont* badge         = nullptr;  // 9
    ImFont* semibold      = nullptr;  // 12 / 600
    ImFont* caption_bold  = nullptr;  // 11 / 600
    ImFont* heading       = nullptr;  // 14 / 600
    ImFont* pane_title    = nullptr;  // 15 / 600
    ImFont* mono_small    = nullptr;  // 11
    ImFont* mono_tiny     = nullptr;  // 10
    ImFont* mono_micro    = nullptr;  // 9
};
void SetFonts(const FontSet& fonts);
ImFont* FontBody();
ImFont* FontSmall();
ImFont* FontTitle();
ImFont* FontBig();
ImFont* FontMono();
ImFont* FontTiny();
ImFont* FontBadge();
ImFont* FontSemibold();
ImFont* FontCaptionBold();
ImFont* FontHeading();
ImFont* FontPaneTitle();
ImFont* FontMonoSmall();
ImFont* FontMonoTiny();
ImFont* FontMonoMicro();

enum class FontRole {
    Body, Small, Title, Big, Mono, Tiny, Badge, Semibold, CaptionBold, Heading, PaneTitle, MonoSmall, MonoTiny, MonoMicro,
};
ImFont* Font(FontRole role);

// PushFont for a scope: { ui::FontScope f(ui::FontTitle()); ImGui::TextUnformatted(...); }
struct FontScope {
    explicit FontScope(ImFont* f);
    explicit FontScope(FontRole r) : FontScope(Font(r)) {}
    ~FontScope();
    FontScope(const FontScope&) = delete;
    FontScope& operator=(const FontScope&) = delete;
};

// The size of `text` (up to "##") drawn in `font`.
ImVec2 TextSizeIn(ImFont* font, const char* text, const char* text_end = nullptr);
// Draws `text` in `font` at p (its own size).
void AddTextIn(ImDrawList* dl, ImFont* font, ImVec2 p, ImU32 col, const char* text, const char* text_end = nullptr);

// A segmented control (the mock-up's .seg): `count` options side by side in a dark
// well, the selected one raised. `item_width` 0 = sized to the labels. An option at
// `disabled_index` cannot be picked. Returns true when *selected changed.
// Width-aware (ui_flow.h): narrower than its options, they share the room, then wrap onto
// rows that fill the width; a label that still does not fit is ellipsized (tooltip).
// 13-7: an icon drawn before a widget's label (the mock-up's .ico in .btn / .mi / .seg). `fn`
// draws glyph `which` in the square at `p` of side `s` (the Library's are library_icons.h);
// `col` is its ink (the mock-up's icons are --muted), faded with the widget when disabled.
using IconFn = void (*)(ImDrawList* dl, ImVec2 p, float s, int which, ImU32 col);
struct Icon {
    IconFn fn = nullptr;
    int    which = 0;
    ImU32  col = kMuted;
};
// The icon's side in buttons and rows (the mockup's .tbb .ico: 14 px).
float IconPx();
// The room an icon takes before its label (icon + gap): IconPx() + 5 by default, or an icon
// `icon_px` wide followed by `gap`, both screen px (the segment's Dp(13) + Dp(5), a menu row's
// Dp(15) + Dp(8)).
float IconRoom();
float IconRoom(float icon_px, float gap);
// Draws `icon` (nothing when it has no fn) in its room starting at x, centred on a line of
// height `line_h` from y; `icon_px` 0 = IconPx().
void PaintIconAt(ImDrawList* dl, const Icon& icon, float x, float y, float line_h, float icon_px = 0.0f);

// 13-7 (B4): options 22 px high, 10 px each side, icons 13 px; `badges` (one per option, nullptr
// entries = none) puts a small superscript after the label (the "new" of the Library view tab),
// that option's label then in kAccentText.
bool Segmented(const char* id, const char* const* labels, int count, int* selected,
               float item_width = 0.0f, int disabled_index = -1,
               const Icon* icons = nullptr,             // icons: one per option, drawn before its label
               const char* const* badges = nullptr);

// Buttons: primary = accent fill, solid = raised fill with a stroke edge. Ghost (the
// default ImGui::Button under this theme) needs no helper. Both shrink to the room left
// and ellipsize their label (ui::FittedButton). Height: ControlH() by default; pass
// ImVec2(0, LargeH()) for the full-size .btn (the Play button).
bool PrimaryButton(const char* label, const ImVec2& size = ImVec2(0.0f, 0.0f));
bool SolidButton(const char* label, const ImVec2& size = ImVec2(0.0f, 0.0f));
// A solid button that shows an on state (.btn.on / .playbig.on): accent-soft fill, accent-line
// edge, text kAccentText. Play <-> Stop, the speaker toggle.
bool ToggleButton(const char* label, bool on, const ImVec2& size = ImVec2(0.0f, 0.0f));
// A square ghost button holding one glyph (.btn.sm.ico, .colbtn: "«", "»", "⋯", "×") or an icon:
// muted, kText and kHover under the mouse. `side` 0 = ControlH(). `tip`: its tooltip.
bool IconOnlyButton(const char* id, const char* glyph, const char* tip = nullptr, float side = 0.0f, bool on = false);
bool IconOnlyButton(const char* id, const Icon& icon, const char* tip = nullptr, float side = 0.0f, bool on = false);
// A link (.linkbtn): kAccentInk underlined text, kAccentText under the mouse. The toast's Undo.
bool LinkButton(const char* label);
// The dashed "add" button (.addbtn: "+ value", "+ Library"): transparent, dashed stroke-strong
// edge, faint text; solid edge and kText under the mouse. ControlH() high, shrinks + ellipsizes.
bool AddButton(const char* label, const ImVec2& size = ImVec2(0.0f, 0.0f));
// The help mark (.qmark): an 18 px circle with "?"; `tip` shown on hover. True when clicked.
bool HelpMark(const char* id, const char* tip = nullptr);

// A pill with a coloured dot and a label (the mock-up's .tag with .dot). 11 px.
void StateTag(ImU32 dot, const char* text);

// 13-7 (B3) tags. Tag: the .tag pill (11 px, bg kBg, 1 px edge, pad 1 8), toned like the
// mockup's .tag.proj / .user / .bad / .warn / .ok, an optional dot before the text.
enum class TagTone { Neutral, Project, User, Bad, Warn, Ok };
// Tag / SmallTag / CountChip are items ControlH() high (the pill centred on the frame line), so
// SameLine / SameLineIfFits place them beside fields and buttons.
float TagWidth(const char* text, bool dot = false);
float TagHeight();
void  Tag(const char* text, TagTone tone = TagTone::Neutral, ImU32 dot = 0);
// The same drawn with its top-left at p on a draw list (a custom row: centre it with TagHeight());
// returns its width.
float DrawTag(ImDrawList* dl, ImVec2 p, const char* text, TagTone tone = TagTone::Neutral, ImU32 dot = 0);
// SmallTag: the tree's "default" (.dtag: 10 px muted, stroke-strong edge, r4, pad 0 5, 17 px).
float SmallTagWidth(const char* text);
float SmallTagHeight();
void  SmallTag(const char* text);
float DrawSmallTag(ImDrawList* dl, ImVec2 p, const char* text);  // top-left at p; returns its width
// CountChip: "→ 3" / "3 rows selected" (.tbto / .selcount: 11 px kAccentText on accent-soft,
// accent-line edge, pill).
float CountChipWidth(const char* text);
void  CountChip(const char* text);

// A small muted caption (the mock-up's .k label).
void Caption(const char* text);
// An UPPERCASE caption (.capt / .lgrp, 11 px, letter-spaced): "PROJECT", "USER". `col` 0 = kFaint.
void GroupCaption(const char* text, ImU32 col = 0);

// Muted, wrapped body text (the mock-up's .sub).
void SubText(const char* text);

// Story 10-3 -- a number field (the mock-up's input.nin), never a slider. Dragging it
// sideways changes the value by `step` per pixel (Shift held: a tenth of it); a click
// without a drag types a value (Enter or a click elsewhere enters it, Esc cancels).
// `*value` is in the field's own (display) units, rounded to `decimals`; `unit` is drawn
// after the number ("" = none). `width` 0 = sized to a typical value. The field shrinks to
// the room left so that its unit always shows (never under 24 px).
// 13-7 (B6): the mockup's .lnum: ControlH() high, raised, 1 px stroke edge (stroke-strong under
// the mouse), r5, the number in mono 11 right-aligned, the unit in 10 px faint 6 px after it.
// `trim` drops trailing zeros ("0", "-14.5": the volume fields).
enum class DragNumberEvent {
    None,
    Live,    // dragging: *value changed this frame (preview it, write nothing)
    Commit,  // released after a drag, or a typed value entered: write *value (one undo point)
    Cancel,  // Esc: *value is back to its value before the gesture
};
DragNumberEvent DragNumber(const char* id, double* value, double step, int decimals, const char* unit,
                           float width = 0.0f, bool trim = false);
// True while a DragNumber is dragged or typed into (the viewer routes Esc to it).
bool DragNumberActive();
// Forgets a running drag / typed value (its view went away; the caller ends the gesture).
void DragNumberReset();

// 13-7 (B5): a fader (the mockup's .fader range input): a 6 px track (stroke-strong) filled in
// accent up to a 16 px round accent thumb. Click or drag anywhere on it to set the value.
// `width` 0 = the next item width (SetNextItemWidth) else the room left; never past the room left.
// ControlH() high. Returns true when *v changed this frame; ImGui::IsItemActive() /
// IsItemDeactivated() after it tell the gesture's state (as with ImGui::SliderFloat).
bool Fader(const char* id, float* v, float v_min, float v_max, float width = 0.0f);

// 13-7 (B7) text fields. All take the room left when `width` is 0 (else min(width, room left)).
// TextField: input.txt (FieldH() high, bg kBg, 1 px stroke edge, accent-line edge while typing,
// r5, faint hint). Returns ImGui::InputText's result (`flags` passed on).
bool TextField(const char* id, const char* hint, char* buf, size_t buf_size, float width = 0.0f,
               int flags = 0);
// TitleField: an inline title (.pane-h input.ttl / .sname): transparent until hovered or typed
// into (then kBg with a stroke-strong edge), FontHeading, 28 px high.
bool TitleField(const char* id, char* buf, size_t buf_size, float width = 0.0f, int flags = 0);
// RenameField: an inline rename in a row (.redit / .gedit): ControlH() high, kBg, accent-line edge,
// 13 px text.
bool RenameField(const char* id, char* buf, size_t buf_size, float width = 0.0f, int flags = 0);
// AddField: the dashed "+ value" field (.trow.add input): transparent with a dashed stroke-strong
// edge and a faint hint; solid edge on kBg while typed into.
bool AddField(const char* id, const char* hint, char* buf, size_t buf_size, float width = 0.0f, int flags = 0);

// 13-7 (B8): a dropdown (select): FieldH() high, kStrokeStrong fill and edge, r5, `preview`
// ellipsized before a drawn muted chevron; its list is styled as a menu popup. `width` 0 =
// natural (min 150) within the room left. Call EndComboStyled only when it returned true.
bool BeginComboStyled(const char* id, const char* preview, float width = 0.0f, int flags = 0);
void EndComboStyled();

// 13-7 (B9): a checkbox (.chk): a 14 px box, r3 (off: kBg + stroke-strong edge; on: accent
// fill + white tick), the label 6 px after it, wrapped within the room left; box and label
// both toggle. Returns true when *v changed.
bool Checkbox(const char* label, bool* v);

// 13-7 (B10) rows. RowBackground draws a row's look behind custom content (any list):
//   hovered: kRaised; selected: kAccentSoft + a 1 px kAccentLine edge; lit (playing):
//   kAccentSoft + a 2 px accent bar on its left; current (Media Explorer, not selected): a
//   2 px accent-line bar; even: kRowAlt; bar_select: selected shows the 2 px accent bar
//   instead of the edge (the Media Explorer's .frow.res.sel).
enum RowBits : unsigned {
    kRowSelected  = 1u << 0,
    kRowHovered   = 1u << 1,
    kRowLit       = 1u << 2,
    kRowCurrent   = 1u << 3,
    kRowEven      = 1u << 4,
    kRowBarSelect = 1u << 5,
};
void RowBackground(ImDrawList* dl, ImVec2 a, ImVec2 b, unsigned bits, float radius = kRadiusSm);
// A Selectable drawn as a row: ImGui's own highlight off, RowBackground behind, the visible part
// of `label` (if any) at 6 px, ellipsized. `height` 0 = RowH(); full width. Hover counts the whole
// row (its overlaid buttons included). `bits` adds kRowLit / kRowCurrent / kRowEven /
// kRowBarSelect. `sflags`: ImGuiSelectableFlags (AllowDoubleClick, AllowOverlap...).
// Returns true when clicked.
bool SelectRow(const char* label, bool selected, unsigned bits = 0, float height = 0.0f, int sflags = 0,
               float radius = kRadiusSm);
// True when the mouse is over the last SelectRow (its overlaid buttons included).
bool SelectRowHovered();

// 13-7 (B11): a fold chevron: an 18 px button, hover kHover r5, the triangle (8x5, muted ->
// kText) pointing down when open, right when folded. Returns true when clicked.
bool Chevron(const char* id, bool open, float side = 0.0f);
// The fold triangle alone, centred on c (the section's and the chevron's).
void DrawFoldTriangle(ImDrawList* dl, ImVec2 c, bool open, ImU32 col);

// 13-7 (B18): a dashed rounded rectangle outline (dash and gap in px along the edge).
void DashedRect(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 col, float radius = kRadiusSm, float dash = 4.0f,
                float gap = 3.0f, float thickness = 1.0f);

// 13-7 (B16): a menu popup (.pop): kRaised, 1 px stroke-strong edge, r7, pad 4, rows 26 px
// (MenuItem / Selectable / IconMenuItem), min 220 wide, hints and disabled rows faint. Call
// EndMenuPopup only when Begin* returned true. Nested BeginMenu windows take the same look.
bool BeginMenuPopup(const char* id, int window_flags = 0);
bool BeginMenuPopupContextItem(const char* id = nullptr, int popup_flags = 1 /* right button */);
void EndMenuPopup();
// A caption inside a menu (.pop .mh: 10 px faint UPPERCASE): "SWITCH ON", "LIBRARIES".
void MenuCaption(const char* text);

// 13-7 (B15): the toast (.toast): kRaised, 1 px stroke-strong edge, r7, pad 14 8, 13 px text.
// A window placed at `pos` (pivot as SetNextWindowPos). Call EndToast whatever it returns.
bool BeginToast(const char* id, ImVec2 pos, ImVec2 pivot = ImVec2(0.5f, 1.0f));
void EndToast();
// Its secondary text (.toast .u: 11 px faint), on the same line.
void ToastNote(const char* text);

// 13-7 (B13): a column splitter (.split): an invisible hit area `size` (8 px wide), a 2 px bar
// in its middle -- none idle, kAccentLine under the mouse, kAccent while dragged -- and the
// resize cursor. `vertical` = a bar between side-by-side columns.
struct SplitterState {
    bool  hovered = false;
    bool  active = false;
    bool  double_clicked = false;
    bool  released = false;  // the drag ended this frame
    float delta = 0.0f;      // the mouse's move along the drag axis this frame (while active)
};
SplitterState SplitterBar(const char* id, ImVec2 size, bool vertical = true);

// A key cap ("C") drawn at p (top-left), the mock-up's <kbd>. Returns its width.
// `text` 0 = the theme's text colour.
float KeyCap(ImDrawList* dl, ImVec2 p, const char* key, ImU32 text = 0);

// ---- compartments and aligned sentences (spec 10-3-fb-2, made systemic by the
// viewer-options-menu-tabs spec) ------------------------------------------------------------

// Text from p, shortened with "..." (whole glyphs) so it ends before max_x. True when shortened.
bool EllipsisText(ImDrawList* dl, ImVec2 p, float max_x, ImU32 col, const std::string& text);

// A boxed track: its header cell (x0..lx, kRaised) and its lane (lx..x1, lane_fill), filled.
void TrackFill(ImDrawList* dl, float x0, float lx, float x1, float y0, float y1, ImU32 lane_fill);
// A boxed track's edges: the divider between header and lane, one edge around both.
void TrackEdge(ImDrawList* dl, float x0, float lx, float x1, float y0, float y1, ImU32 edge);

// A 3 px colour stripe on the left of a rounded box (p0..p1): the box's own rounded rect
// clipped to the stripe, so it follows the box's corners.
void RuleStripe(ImDrawList* dl, ImVec2 p0, ImVec2 p1, ImU32 col, float radius);

// A card: its fill and edge are drawn behind its content once its height is known.
// BeginCard / EndCard around the content; the cursor ends under it, a kCardGap below.
// 13-7 (A3/A4): r 10 (kRadiusLg), 8 px between cards.
constexpr float kCardPad = 8.0f;
constexpr float kCardGap = 8.0f;
// The card's content ends at its inner edge (kCardPad inside its right side): the window's work
// and content rects are narrowed while it is open, so GetContentRegionAvail, wrapping, spanning
// rows and the ui_flow helpers all stop there.
struct Card {
    ImDrawListSplitter split;
    ImVec2             p0;
    float              w = 0.0f;
    float              saved_work_max = 0.0f;  // the window's rects before the card narrowed them
    float              saved_content_max = 0.0f;
};
// `top_pad` < 0 = kCardPad (a Section passes the mockup's 4).
void BeginCard(Card& k, float top_pad = -1.0f);
void EndCard(Card& k, ImU32 fill, ImU32 edge);

// THE PANEL: the look of every panel and strip docked in the viewer, and of the open
// options menu -- the panel grey (kSurface) with a kStroke edge. Pushes 2 colours: call
// before Begin, PopStyleColor(2) after End. A window that is a panel uses this, never its
// own colours, so every panel matches.
void PushPanelStyle();

// THE SECTION: how every group of settings is shown, in any panel or menu -- a card
// (kSurface with a kStroke edge, as the Tagging panel's condition cards) headed by its icon and title, a divider under
// the header. Its header folds it (a triangle on the left; the state is kept for the
// session). Always pair with EndSection, open or not:
//     ui::Section s;
//     if (ui::BeginSection(s, "Light", icon)) { ...settings... }
//     ui::EndSection(s);
// A new setting goes inside a section: it then looks like the others with no styling of its own.
// 13-7 (A4): the mockup's .secx: padding 4 8 8, a 26 px header with the title in 13 px, the fold
// triangle on the LEFT of a foldable section's title (hover kHover), none on a fixed one; the
// divider 5 px under the header, the settings 8 px under it; a section inside a section is kBg.
// The open state lives in ImGui storage at PushID(title) / "##open" (SeedSection relies on it).
struct SectionOpts {
    ImTextureID icon = ImTextureID();
    bool        foldable = true;
    const char* suffix = nullptr;  // faint 11 px after the title: "· 2 children", "· this sound only"
    float       right_w = 0.0f;    // room kept at the header's right for widgets (BeginSectionHeaderRight)
};
struct Section {
    Card   card;
    bool   open = true;
    bool   nested = false;
    ImVec2 head_min, head_max;     // the header row (inside the card's padding)
    float  right_w = 0.0f;
    ImVec2 resume;                 // where the content goes once the header's widgets are drawn
    bool   in_header = false;
};
bool BeginSection(Section& s, const char* title, ImTextureID icon = ImTextureID(), bool foldable = true);
bool BeginSection(Section& s, const char* title, const SectionOpts& opts);
void EndSection(Section& s);
// The header's right slot (opts.right_w wide, right-aligned, items ControlH() high centred in
// the header): call right after BeginSection, open or folded, draw the widgets (SameLine between
// them), then EndSectionHeaderRight. Those widgets take the mouse before the fold.
void BeginSectionHeaderRight(Section& s);
void EndSectionHeaderRight(Section& s);

// The "aligned sentence": one phrase per row, its connector word right-aligned in a fixed
// column, every row's chips from one shared edge. A chip that does not fit beside the
// previous one wraps under the row's first chip (a row wraps as a whole, never a lone word).
struct Sentence {
    float x0 = 0.0f;       // the label column's left
    float chips_x = 0.0f;  // the chips' shared left edge
    float right = 0.0f;    // the right edge chips stay within
    bool  first = true;    // no chip on this row yet
};
// The label column's width: as wide as the widest of `labels`, and of `badge` drawn as a
// badge (its pad included; nullptr = none). Each caller passes its own labels.
float SentenceColumnWidth(const char* const* labels, int count, const char* badge = nullptr);
// A sentence whose label column starts at x0, `column_w` wide (SentenceColumnWidth), its
// chips within `right`.
Sentence MakeSentence(float x0, float right, float column_w);
// Starts a row on the cursor's line: its label right-aligned in the column (an item, so it
// can be hovered). `badge`: drawn as a tag (the IF / AND badge).
void SentenceRow(Sentence& s, const char* label, bool badge = false);
// Puts the cursor where the row's next chip (w wide) goes: beside the previous one when it
// fits (`spacing` < 0 = the item spacing), else under the row's first chip.
void SentenceChip(Sentence& s, float w, float spacing = -1.0f);

// The width a DragNumber field_w wide takes with its unit.
float NumberFieldWidth(float field_w, const char* unit);

// Vertical tabs (REAPER's preferences column): `count` labels stacked, `width` wide (0 =
// sized to the labels). The selected row: kAccentSoft fill and an accent bar on its left,
// text kText; the others muted, kHover under the mouse. Returns true when *selected changed.
bool VerticalTabs(const char* id, const char* const* labels, int count, int* selected, float width = 0.0f);

}  // namespace ui
}  // namespace rav

#endif  // _WIN32
