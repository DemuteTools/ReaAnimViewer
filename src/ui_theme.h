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
//
// 13-14 (one theme for every tab: Main, Tagging, Video, Library, the ☰ menu). Every view draws
// with the widgets above and below; no raw ImGui widget or hand-made style outside ui_theme /
// ui_flow (menu rows -- MenuItem / Selectable / BeginMenu / TextDisabled / Separator -- are fine
// inside a themed popup or combo; tooltips are fine, ItemTip styles them).
//
//  Metrics
//    kCardPad 8, kCardGap 8 (here), kLabelCol 80 (ui_flow.h) are DpPx: they read as floats already
//                scaled by the DPI (N3), so `x + ui::kCardPad` and `label_col = ui::kLabelCol` hold.
//
//  Colours (G-11)  kOverlayBg (kBg .72, frameless buttons over the 3D view), kKnob / kKnobLine
//                (--knob), kPlayhead, kOutsideDim (kBg .65), kAccentFaint (accent .10), kHatch (.35) /
//                kHatchLight (.20), kRing (white .35), kRaisedVeil (kRaised .95), kAxisX/Y/Z (+ Dark),
//                kNavEdge, kHighlight (#ffd54f); WithAlpha(c, a)
//
//  Disabled (A5 / B2 / C3.2: .btn[disabled] = raised + faint text; .tbb.dis = transparent + faint;
//    .pop .mi[disabled] = faint). Inside ImGui::BeginDisabled the theme's buttons (Solid / Primary /
//    Toggle / ghost FittedButton / MenuButton / IconButton / IconOnlyButton / AddButton / ChipButton /
//    ConfirmButton / ToneButton) draw that skin themselves at full opacity; IconMenuItem / MenuEntry
//    rows with enabled = false are faint. style.DisabledAlpha stays ImGui's 0.6 for the stock
//    widgets left (a disabled MenuItem would otherwise look enabled).
//    BeginDisabledLook / EndDisabledLook: BeginDisabled with no fade, text faint (stock content).
//    ItemDisabled(): true inside a disabled region.
//
//  Text        ToneText (a toned line or wrapped paragraph: kWarn notice, kBad error, kFaint note)
//  Buttons     ConfirmButton (.btn.confirm: kConfirmAct fill, #1a1a1a text, 22 px), ToneButton (a solid
//              button with a tone edge + text and a trailing icon: "Roles · 2 missing"), ChipButton
//              (a clickable .tag pill: angles, Cut / Move), AddButton(+ key cap, pill),
//              IconOnlyButton(texture; framed = the .colbtn over the 3D view: kOverlayBg + stroke,
//              28 x 26 burger), LinkButton(label, col)
//  Switch      the .sw toggle: 26 x 16 track (accent on / stroke-strong off), kKnob thumb r 6
//  Bands       BeginConfirmBand / EndConfirmBand: the amber in-place confirm (.confirmbox: kWarn .45
//              edge, .08 fill, r7, inset 6) or the warn band (kWarn .08, r5, inset 8 3)
//  Combos      BeginChipCombo / EndChipCombo (select.chip: ControlH, sized to its preview, small muted
//              chevron, list as a menu), BeginComboStyled(..., TagTone) (Bad = kBadInk + kBadLine edge),
//              SelectField (a select-looking button that opens the caller's popup: "Preset: x  edited ▾")
//  Popups      BeginPanelPopup (content popup: kRaised, stroke-strong, r7, pad 12, no row spacing),
//              BeginModalCard (centred modal, pad 16 14), BeginFloatingCard (the same, not modal),
//              popups / combos / toast / modal cards cast the mockup's shadow (0 18 50 rgba 0 .6)
//  Menus       MenuEntry (a MenuItem with the 11 px faint hint .pop .mi .hint and a faint disabled
//              row), BeginSubMenu (BeginMenu, min 220 wide), MenuSeparator (.pop .sep)
//  Overlays    BeginOverlay (frameless auto-size window over the 3D view), BeginOverlayPanel (the
//              Edit overlay: kBg, stroke-strong, r10)
//  Panes       BeginPane (child: kSurface, stroke, r10, pad 8), BeginListPane (kBg, stroke, r5, pad 1),
//              BeginWell (.tb well: kBg, stroke, r7, pad 4), BeginToolbar (a well with .tbb metrics:
//              24 px buttons, pad 0 6, 3 px apart, disabled = .tbb.dis)
//  Fader       Fader(id, v, min, max, width, FaderOpts): + log range, + value drawn in mono 11 (.fader)
//  DragNumber  + text_col (a toned value: kWarn "raised by Detect")
//  Fields      TextField's focus ring is 2 px accent-line (outline-offset 1); TitleField(..., font);
//              RenameField(..., hint)
//  Colour      ColorSwatchButton (a swatch that opens a themed picker), SwatchPalette (8 dots, r7,
//              kText ring on the current one)
//  Tooltips    ItemTipText (literal text) / ItemTip (printf format) / BeginItemTip / EndItemTip (B14: pad 8 6,
//              wrap 24 em, kText / kMuted text even inside a disabled region); the theme's own tooltips use
//              the same look
//  Labels      LabelRow / LabelRowText wrap a long label onto two lines in its column (ui_flow.h)
//
// 13-14 (finishing: the views' last local helpers, moved here)
//  Colour      ColorSwatchButton(..., ColorSwatchOpts): `inputs` adds the RGB / HSV / Hex fields under the
//              picker, `original` the Current / Original preview beside it (click Original = revert)
//  Fader       Ctrl+click types a value in place of the track (Enter or a click elsewhere enters it, Esc
//              restores it); the gesture stays one active item (IsItemActive / IsItemDeactivated hold);
//              FaderTyping() is true meanwhile
//  Panels      PushOverlayPanelStyle (a window over the 3D view: kOverlayBg + kStroke edge, the closed ☰)
//  Rows        kRowHoverStrong (hover kHover: a row on a raised popup), kRowCard (a filled, edged card
//              row: kRaised, kHover under the mouse, accent-soft + accent-line selected) and RowBackground's
//              `stripe` (a 3 px colour stripe on its left, under the edge); SelectRow(..., width) fixes the
//              row's width (an auto-sizing submenu sizes itself to it)
//  Buttons     AddButtonWidth(label, key): AddButton's natural width
//  Panes       BeginPane(id, size, PaneOpts): padding, radius (0 = square), edge all round / left only / none
//  Section     SectionOpts.fill_h: the card fills the column's height left, its body a scrolling child
//  Text        VerticalText (a line turned a quarter), EllipsizeToWidth (whole UTF-8 characters + "...")
//  Icons       the small tool glyphs (cross, duplicate, undo, plus, lock, gear, check, fold, open arrow) are
//              library_icons.h's ToolIcon / DrawToolGlyph (ui::IconFn)
//  Labels      LabelRow in a window that sizes itself (a popup) keeps the label beside its field (ui_flow.h)
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

// 13-14 (G-11): the views' remaining literal colours, named once.
constexpr ImU32 kOverlayBg   = IM_COL32(0x12, 0x13, 0x17, 0xB8);  // a frameless button over the 3D view (kBg .72)
constexpr ImU32 kKnob        = IM_COL32(0xC9, 0xCE, 0xD8, 0xFF);  // --knob: a switch's thumb, icons over the view
constexpr ImU32 kKnobLine    = IM_COL32(0xC9, 0xCE, 0xD8, 0x8C);  // its lines (.55): the skeleton's bones
constexpr ImU32 kPlayhead    = IM_COL32(0xDF, 0xE3, 0xEA, 0xFF);  // the strips' playhead
constexpr ImU32 kOutsideDim  = IM_COL32(0x12, 0x13, 0x17, 0xA6);  // kBg .65 over a lane outside its item
constexpr ImU32 kAccentFaint = IM_COL32(0x5B, 0x8C, 0xFF, 0x1A);  // accent .10: a lane's lit row
constexpr ImU32 kHatch       = IM_COL32(0x00, 0x00, 0x00, 0x59);  // an off frame's hatch (.35)
constexpr ImU32 kHatchLight  = IM_COL32(0x00, 0x00, 0x00, 0x33);  // a Blend layer's hatch (.20)
constexpr ImU32 kRing        = IM_COL32(0xFF, 0xFF, 0xFF, 0x59);  // the white ring of a selected bar (.35)
constexpr ImU32 kRaisedVeil  = IM_COL32(0x20, 0x23, 0x2A, 0xF2);  // kRaised .95: a label over the 3D view
constexpr ImU32 kAxisX       = IM_COL32(0xD6, 0x54, 0x4E, 0xFF);  // the nav cube: +X red
constexpr ImU32 kAxisXDark   = IM_COL32(0x96, 0x34, 0x30, 0xFF);  //   -X
constexpr ImU32 kAxisY       = IM_COL32(0x78, 0xBE, 0x5A, 0xFF);  //   +Y green
constexpr ImU32 kAxisYDark   = IM_COL32(0x50, 0x82, 0x3E, 0xFF);  //   -Y
constexpr ImU32 kAxisZ       = IM_COL32(0x54, 0x8C, 0xDC, 0xFF);  //   +Z blue
constexpr ImU32 kAxisZDark   = IM_COL32(0x38, 0x60, 0x9C, 0xFF);  //   -Z
constexpr ImU32 kNavEdge     = IM_COL32(0x14, 0x16, 0x1C, 0xDC);  //   its faces' edges
constexpr ImU32 kHighlight   = IM_COL32(0xFF, 0xD5, 0x4F, 0xFF);  //   the amber hover highlight

// `c` with alpha `a` (0..255, clamped).
constexpr ImU32 WithAlpha(ImU32 c, int a)
{
    return (c & ~IM_COL32_A_MASK) | (static_cast<ImU32>(a < 0 ? 0 : (a > 255 ? 255 : a)) << IM_COL32_A_SHIFT);
}

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
// 13-14 (N3): a layout constant in mockup px that reads as a float at the screen's DPI, so it keeps
// the syntax of the constexpr float it replaces (`x + kCardPad`, `float col = kLabelCol`).
struct DpPx {
    float px;
    operator float() const { return Dp(px); }
};
// A DpPx is already scaled: Dp(kCardPad) would scale it twice, so it does not compile (pass it as is).
float Dp(DpPx) = delete;
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
// edge, text kAccentText. Play <-> Stop, the speaker toggle. Disabled, on or off, it takes the disabled
// button's skin (raised, stroke edge, faint text: no accent).
bool ToggleButton(const char* label, bool on, const ImVec2& size = ImVec2(0.0f, 0.0f));
// A square ghost button holding one glyph (.btn.sm.ico, .colbtn: "«", "»", "⋯", "×") or an icon:
// muted, kText and kHover under the mouse. `side` 0 = ControlH(). `tip`: its tooltip.
bool IconOnlyButton(const char* id, const char* glyph, const char* tip = nullptr, float side = 0.0f, bool on = false);
bool IconOnlyButton(const char* id, const Icon& icon, const char* tip = nullptr, float side = 0.0f, bool on = false);
// 13-14 (G-9): a texture icon (the ☰ burger, the keyboard), tinted kMuted (kText under the mouse,
// accent when on); its icon fills the button less 4 px.
bool IconOnlyButton(const char* id, ImTextureID tex, const char* tip = nullptr, float side = 0.0f, bool on = false);
// The same with its size and look spelled out. `framed` = the mockup's .colbtn over the 3D view:
// kOverlayBg fill + a stroke edge (accent-soft + accent-line when on, kHover under the mouse).
struct IconOnlyOpts {
    float w = 0.0f, h = 0.0f;  // 0 = ControlH() (h 0 = w)
    bool  on = false;
    bool  framed = false;
    float icon_px = 0.0f;      // 0 = the button's height less 4 px (texture) / IconPx() (Icon)
    ImU32 tint = 0;            // the icon's ink at rest; 0 = kMuted (texture) / the Icon's own col
};
bool IconOnlyButton(const char* id, ImTextureID tex, const char* tip, const IconOnlyOpts& o);
bool IconOnlyButton(const char* id, const Icon& icon, const char* tip, const IconOnlyOpts& o);
// A link (.linkbtn): kAccentInk underlined text, kAccentText under the mouse. The toast's Undo.
// 13-14 (G-21): `col` (non-zero) draws a quiet link in that colour (a muted report line), kText
// and underlined under the mouse only.
bool LinkButton(const char* label, ImU32 col = 0);
// The dashed "add" button (.addbtn: "+ value", "+ Library"): transparent, dashed stroke-strong
// edge, faint text; solid edge and kText under the mouse. ControlH() high, shrinks + ellipsizes.
// 13-14 (G-15): `key` draws its shortcut as a key cap 8 px after the label ("+ Event  E"); `pill`
// rounds it fully (the Video view's "+ Save" chip).
bool AddButton(const char* label, const ImVec2& size = ImVec2(0.0f, 0.0f), const char* key = nullptr,
               bool pill = false);
// 13-14 (finishing): AddButton's natural width: the visible label, 10 px each side, and with `key` 8 px
// and its key cap.
float AddButtonWidth(const char* label, const char* key = nullptr);
// 13-14 (G-1): the amber action of an in-place confirm (.btn.confirm: kConfirmAct fill, kConfirmActHover
// under the mouse, kConfirmActText). ControlH() high; shrinks and ellipsizes like SolidButton.
bool ConfirmButton(const char* label, const ImVec2& size = ImVec2(0.0f, 0.0f));
// 13-14 (G-16): a clickable pill (a .tag that acts): r = h / 2, 1 px stroke edge, kBg (kHover +
// stroke-strong under the mouse), text muted (kText under the mouse); `on` = accent-soft, accent-line,
// kAccentText. Natural width = label + 7 px each side; ControlH() high; shrinks and ellipsizes.
bool ChipButton(const char* label, const ImVec2& size = ImVec2(0.0f, 0.0f), bool on = false);
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

// 13-14 (G-24): a solid button toned like a Tag (Bad: kBadInk text, kBadLine edge; Ok / Warn / ...),
// raised fill (kHover under the mouse), r5, ControlH() high, an optional `icon` drawn after the
// label (its own colour: the ✓ of "Roles"). Shrinks and ellipsizes its label; the icon stays.
float ToneButtonWidth(const char* label, const Icon* icon = nullptr);
bool  ToneButton(const char* label, TagTone tone, const Icon* icon = nullptr, const ImVec2& size = ImVec2(0.0f, 0.0f));

// 13-14 (G-3): the on / off switch (.sw): a 26 x 16 track (accent on, stroke-strong off), a kKnob
// thumb (r 6) at its end, centred on the frame line (an item 26 px x ControlH()). True when toggled:
// a click released over it, or the keyboard / gamepad activating it. Under the mouse its track lightens;
// disabled (ImGui::BeginDisabled) it is drawn faint at full opacity, its thumb still telling on / off.
bool Switch(const char* id, bool on);

// A small muted caption (the mock-up's .k label).
void Caption(const char* text);
// An UPPERCASE caption (.capt / .lgrp, 11 px, letter-spaced): "PROJECT", "USER". `col` 0 = kFaint.
void GroupCaption(const char* text, ImU32 col = 0);

// Muted, wrapped body text (the mock-up's .sub).
void SubText(const char* text);

// 13-14 (G-4): text in a token colour: a notice (kWarn), an error (kBad), a note (kFaint), a muted
// line. `wrap`: wrapped at `wrap_w` from the cursor (0 = at the room left), else one line.
void ToneText(const char* text, ImU32 col = kMuted, bool wrap = false, float wrap_w = 0.0f);
void ToneText(const std::string& text, ImU32 col = kMuted, bool wrap = false, float wrap_w = 0.0f);

// 13-14 (G-14, B14) tooltips. The mockup's tooltip look: kRaised, stroke-strong edge, pad 8 6, the
// 12 px body font, kText (kMuted for TextDisabled) even over a disabled item. ItemTipText: the tooltip
// of the last item (hovered for a tooltip: still for 0.5 s, disabled items too), its text shown as is --
// use it for any text that is not a format of your own (a name, a path, a translated string: a '%' in
// it is shown, never read as a format). ItemTip: the same with a printf format and its arguments
// (ItemTip("%d events", n)); never pass it a variable as the format. BeginItemTip / EndItemTip: free
// content, wrapped at `wrap_em` font sizes; call EndItemTip only when BeginItemTip returned true.
void ItemTipText(const char* text);
void ItemTip(const char* fmt, ...) IM_FMTARGS(1);
bool BeginItemTip(float wrap_em = 24.0f, int hovered_flags = 0);
void EndItemTip();
// The same look for a tooltip shown now (not tied to an item: a draw-list row under the mouse).
void ShowTip(const char* text);

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
// 13-14 (G-20): `text_col` (non-zero) tones the number (kWarn: a value Detect raised).
DragNumberEvent DragNumber(const char* id, double* value, double step, int decimals, const char* unit,
                           float width = 0.0f, bool trim = false, ImU32 text_col = 0);
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
// 13-14 (G-13): `log` = a logarithmic range (needs 0 < v_min < v_max); `format` (printf, e.g. "%.2f")
// draws the value right of the track (.fader: 8 px gap) in mono 11, right-aligned in `value_w`
// (0 = wide enough for v_min and v_max). `width` covers track and value. The last item is still the
// track: IsItemActive / IsItemDeactivated after it hold (never IsItemDeactivatedAfterEdit: an
// InvisibleButton).
struct FaderOpts {
    bool        log = false;
    const char* format = nullptr;
    float       value_w = 0.0f;
};
bool Fader(const char* id, float* v, float v_min, float v_max, float width, const FaderOpts& o);
// 13-14 (finishing): Ctrl+click types a value (as ImGui's slider): a raised mono field (accent-line edge)
// takes the track's place, the value applied as typed (within v_min..v_max); Enter or a click elsewhere
// ends it, Esc puts *v back to the float it held when typing began (FaderCancelled). The field keeps
// the fader's id, so the whole gesture -- click, typing, end -- is one active item: IsItemActive while
// typing, IsItemDeactivated once at its end (one undo point). `id` must be an id only ("##v"): it is the field's label. Its text is the value in
// `format` without its decorations ("45" for "%.0f°"), "%.2f" with no format.
// FaderTyping: true while a fader is typed into (no mouse button held: a live edit must not end).
bool FaderTyping();
// FaderCancelled: right after a Fader, true on the frame Esc ended its typing. *v is then back to the
// float it held when typing began (not the field's rounded text) and the gesture deactivates
// (IsItemDeactivated): a caller must then skip its write (drop its draft, no undo point).
bool FaderCancelled();

// 13-7 (B7) text fields. All take the room left when `width` is 0 (else min(width, room left)).
// TextField: input.txt (FieldH() high, bg kBg, 1 px stroke edge, accent-line edge while typing,
// r5, faint hint). Returns ImGui::InputText's result (`flags` passed on).
// 13-14 (B7): while typed into, the mockup's focus ring: 2 px accent-line, 1 px outside the edge.
bool TextField(const char* id, const char* hint, char* buf, size_t buf_size, float width = 0.0f,
               int flags = 0);
// TitleField: an inline title (.pane-h input.ttl / .sname): transparent until hovered or typed
// into (then kBg with a stroke-strong edge), FontHeading, 28 px high.
// 13-14 (G-22): `font` (non-null) = an inline field in that font, as high as the font plus 6 px and
// never under ControlH() (the Video view's shot name in the body font: 22 px).
bool TitleField(const char* id, char* buf, size_t buf_size, float width = 0.0f, int flags = 0, ImFont* font = nullptr);
// RenameField: an inline rename in a row (.redit / .gedit): ControlH() high, kBg, accent-line edge,
// 13 px text. 13-14 (G-23): `hint` shown faint while empty ("Role name").
bool RenameField(const char* id, char* buf, size_t buf_size, float width = 0.0f, int flags = 0,
                 const char* hint = nullptr);
// AddField: the dashed "+ value" field (.trow.add input): transparent with a dashed stroke-strong
// edge and a faint hint; solid edge on kBg while typed into.
bool AddField(const char* id, const char* hint, char* buf, size_t buf_size, float width = 0.0f, int flags = 0);

// 13-7 (B8): a dropdown (select): FieldH() high, kStrokeStrong fill and edge, r5, `preview`
// ellipsized before a drawn muted chevron; its list is styled as a menu popup. `width` 0 =
// natural (min 150) within the room left. Call EndComboStyled only when it returned true.
// 13-14 (G-6): `tone` Bad = a value that is missing: preview kBadInk, edge kBadLine.
bool BeginComboStyled(const char* id, const char* preview, float width = 0.0f, int flags = 0,
                      TagTone tone = TagTone::Neutral);
void EndComboStyled();

// 13-14 (G-5): the small "chip" select of the Tagging rules (select.chip): ControlH() high, r7,
// stroke-strong fill (kFrameHover under the mouse), sized to its preview (+ 30 px for the pad and the
// chevron, never under `min_w`; 0 = 40 px) within the room left -- or within `right` (a Sentence's
// right edge) when given -- the preview ellipsized before a small muted chevron. `disabled` = really
// disabled (ImGui::BeginDisabled around it: never hovered, never opens, returns false), drawn at full
// opacity with a faint preview and chevron. Its list is a menu popup (BeginMenuPopup's look). `flags`:
// ImGuiComboFlags (the height). Call EndChipCombo only when it returned true.
float ChipComboWidth(const char* preview, float min_w = 0.0f);
bool  BeginChipCombo(const char* id, const char* preview, float min_w = 0.0f, bool disabled = false, int flags = 0,
                     float right = 0.0f);
void  EndChipCombo();

// 13-14 (G-17): a select-looking button (the Tagging panel's preset field): FieldH() high, kRaised
// (kHover under the mouse), r5, its edge kStroke (accent-line while `open`), then: `label` muted,
// the icon, `value` (ellipsized, kText or value_col), `tag` right-aligned (faint, or toned) and a
// muted chevron. Narrow, the label and the value are ellipsized (both in the tooltip; the tag goes
// first). True when clicked: the caller opens its popup under it (GetItemRectMin / Max).
struct SelectFieldOpts {
    const char* tag = nullptr;
    TagTone     tag_tone = TagTone::Neutral;  // Warn: the tag in kWarn ("Legacy")
    bool        open = false;
    float       width = 0.0f;                 // 0 = the room left
    const Icon* icon = nullptr;               // drawn 12 px before the value (the factory padlock)
    ImU32       value_col = 0;                // 0 = kText (kMuted: a preset that is gone)
};
bool SelectField(const char* id, const char* label, const char* value, const SelectFieldOpts& o = SelectFieldOpts());

// 13-7 (B9): a checkbox (.chk): a 14 px box, r3 (off: kBg + stroke-strong edge; on: accent
// fill + white tick), the label 6 px after it, wrapped within the room left; box and label
// both toggle. Returns true when *v changed.
bool Checkbox(const char* label, bool* v);
// 13-15b: a Checkbox's box with no label that can also be half-ticked (state 2: a dash), for a parent row
// whose children are partly selected. state 0 off, 1 on, 2 mixed. True when clicked (the caller decides what
// the next state is: a mixed box usually ticks everything).
bool TriCheckbox(const char* id, int state);
// A library's colour (Block::color form: 0x1000000 | 0xRRGGBB; 0 = the accent): its dot's ink (13-15b: moved
// here from the Library view, the assignment window draws it too).
ImU32 LibDot(unsigned int color);

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
    // 13-14 (finishing): hovered draws kHover instead of kRaised -- a row on a raised popup (a menu, the
    // Shortcuts sheet), where kRaised would not show.
    kRowHoverStrong = 1u << 6,
    // A card row (the Tagging panel's rule cards): always filled kRaised (kHover under the mouse when not
    // selected) with a kStroke edge; selected: accent-soft over the fill and a kAccentLine edge.
    kRowCard = 1u << 7,
};
// `stripe` (non-zero): a 3 px stripe in that colour on the row's left (RuleStripe), between its fill
// and its edge (a rule's colour).
void RowBackground(ImDrawList* dl, ImVec2 a, ImVec2 b, unsigned bits, float radius = kRadiusSm, ImU32 stripe = 0);
// A Selectable drawn as a row: ImGui's own highlight off, RowBackground behind, the visible part
// of `label` (if any) at 6 px, ellipsized. `height` 0 = RowH(); full width. Hover counts the whole
// row (its overlaid buttons included). `bits` adds kRowLit / kRowCurrent / kRowEven /
// kRowBarSelect. `sflags`: ImGuiSelectableFlags (AllowDoubleClick, AllowOverlap...).
// Returns true when clicked.
// 13-14 (finishing): `width` > 0 fixes the row's width (rows of a submenu that sizes itself to them).
bool SelectRow(const char* label, bool selected, unsigned bits = 0, float height = 0.0f, int sflags = 0,
               float radius = kRadiusSm, float width = 0.0f);
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
// 13-14 (G-18): a nested menu with the menu's 220 px minimum (ImGui::BeginMenu otherwise); call
// ImGui::EndMenu only when it returned true. MenuSeparator: the .pop .sep line (kStroke).
bool BeginSubMenu(const char* label, bool enabled = true);
void MenuSeparator();

// 13-14 (G-8): a popup that holds content rather than a list of actions (a search field and its
// cascades, a name field and its buttons, the Shortcuts sheet): the menu's look (kRaised, 1 px
// stroke-strong edge, r7, the shadow) with `pad` inside (mockup px; default 12 12), the theme's own
// item spacing and frame padding (6 7 / 8 3, whatever a toolbar or menu around it pushed) and no
// minimum width; a button inside it is never a toolbar's. Call EndPanelPopup only when it returned true.
bool BeginPanelPopup(const char* id, int window_flags = 0, ImVec2 pad = ImVec2(12.0f, 12.0f));
void EndPanelPopup();

// 13-14 (G-7): a modal card, centred in the viewport: kRaised, 1 px stroke-strong edge, r7, pad 16 14,
// the shadow; sized to its content (`width` 0) or `width` wide (at most the viewport less 32 px) with
// its height capped to the viewport less 32 px in both cases (the content scrolls); the theme's item
// spacing and frame padding inside, as BeginPanelPopup. `window_flags` are added to
// NoTitleBar | NoMove | NoSavedSettings. Open it with ImGui::OpenPopup(id); ImGui::CloseCurrentPopup
// inside closes it. Call EndModalCard only when it returned true.
bool BeginModalCard(const char* id, int window_flags = 0, float width = 0.0f);
void EndModalCard();
// 13-14 (G-7b): the same card as a plain window (not modal, not focused when it appears) at `pos`
// (pivot as SetNextWindowPos), `width` wide (0 = sized to its content), the theme's item spacing and
// frame padding inside. Call EndFloatingCard whatever it returns.
bool BeginFloatingCard(const char* id, ImVec2 pos, ImVec2 pivot = ImVec2(0.0f, 0.0f), float width = 0.0f);
void EndFloatingCard();
// 13-15b: the floating card's look (kRaised, stroke-strong edge, r7, shadow, the theme's metrics) for a window the
// user moves and resizes (the caller places it and sets its size constraints; `window_flags` adds to
// NoTitleBar | NoCollapse | NoSavedSettings | NoScrollbar; pass NoMove to drag it by its own title row).
// Always pair with EndMovableCard.
bool BeginMovableCard(const char* id, int window_flags = 0);
void EndMovableCard();
// 13-15: the window filling a tool window of its own (FloatUiWindow): its whole client area, square, the raised
// card's fill and edge, no move / resize / bring to front (the OS window does that). Pair with EndToolWindowFill.
bool BeginToolWindowFill(const char* id);
void EndToolWindowFill();

// 13-14 (G-10): a frameless window over the 3D view holding a few widgets (the view toggle, the
// panel button, the status icon, the nav cube): pad 0, no background, no border, sized to its
// content, never focused / scrolled / saved. Call EndOverlay whatever it returns.
bool BeginOverlay(const char* id, ImVec2 pos, ImVec2 pivot = ImVec2(0.0f, 0.0f), int extra_flags = 0);
void EndOverlay();
// 13-14 (G-19): a panel over the view at `pos`, `size` (the Library's Edit overlay, .edbox): kBg, 1 px
// stroke-strong edge, r10, the shadow; no decoration, never moved or saved. Call EndOverlayPanel
// whatever it returns.
bool BeginOverlayPanel(const char* id, ImVec2 pos, ImVec2 size, int extra_flags = 0);
void EndOverlayPanel();

// 13-14: the soft shadow the mockup casts under popups, the toast and the overlay cards (.pop:
// 0 18px 50px rgba(0,0,0,.6)), drawn around the current window, outside it only. The Begin* above
// call it; a custom popup calls it right after its Begin.
void WindowShadow(float alpha = 0.6f);

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

// 13-14 (finishing): `text` shortened to whole characters plus "..." so it is at most max_w wide in
// `font` (UTF-8 safe: a character's continuation bytes go with its lead byte); "" when even "..." does
// not fit, `text` itself when it fits.
std::string EllipsizeToWidth(ImFont* font, const std::string& text, float max_w);
// `text` drawn turned a quarter (writing-mode: vertical-rl), centred on cx from `top` down: reading top
// to bottom (`down`), or bottom to top (rotate(180deg)). Clipped to clip_min..clip_max (the strip).
void VerticalText(ImDrawList* dl, ImFont* font, float cx, float top, ImU32 col, const std::string& text, bool down,
                  ImVec2 clip_min, ImVec2 clip_max);

// A boxed track: its header cell (x0..lx, kRaised) and its lane (lx..x1, lane_fill), filled.
void TrackFill(ImDrawList* dl, float x0, float lx, float x1, float y0, float y1, ImU32 lane_fill);
// A boxed track's edges: the divider between header and lane, one edge around both.
void TrackEdge(ImDrawList* dl, float x0, float lx, float x1, float y0, float y1, ImU32 edge);

// A 3 px colour stripe on the left of a rounded box (p0..p1): the box's own rounded rect
// clipped to the stripe, so it follows the box's corners.
void RuleStripe(ImDrawList* dl, ImVec2 p0, ImVec2 p1, ImU32 col, float radius);

// A card: its fill and edge are drawn behind its content once its height is known.
// BeginCard / EndCard around the content; the cursor ends under it, a kCardGap below.
// 13-7 (A3/A4): r 10 (kRadiusLg), 8 px between cards. 13-14 (N3): at the screen's DPI (DpPx).
inline constexpr DpPx kCardPad{8.0f};
inline constexpr DpPx kCardGap{8.0f};
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

// 13-14 (G-19) panes. BeginPane: a child window (the Library's columns, a side strip): `fill`, a 1 px
// kStroke edge (none when `edge` 0), r10, 8 px inside. BeginListPane: a list's dark box (the Media
// Explorer's results: kBg, stroke edge, r5, rows 1 px inside the edge). Both return ImGui::BeginChild's
// result; call EndPane / EndListPane whatever they return (as EndChild).
bool BeginPane(const char* id, ImVec2 size, ImU32 fill = kSurface, int child_flags = 0, int window_flags = 0,
               ImU32 edge = kStroke);
// 13-14 (finishing): the same with its padding, radius and edge spelled out (the Edit overlay's side
// strip: 10 px inside, square, a stroke line on its left only). Call EndPane whatever it returns.
enum class PaneEdge { All, Left, None };
struct PaneOpts {
    ImU32    fill = kSurface;
    ImU32    edge_col = kStroke;
    PaneEdge edge = PaneEdge::All;
    float    pad = 8.0f;          // mockup px, each side
    float    radius = kRadiusLg;  // mockup px; 0 = square corners
    int      child_flags = 0;
    int      window_flags = 0;
};
bool BeginPane(const char* id, ImVec2 size, const PaneOpts& o);
void EndPane();
bool BeginListPane(const char* id, ImVec2 size, int window_flags = 0);
void EndListPane();

// A well (.tb, the Library toolbar's dark box): kBg, 1 px kStroke edge, r7, `pad` inside (< 0 = 4 px
// plus the edge), the room left wide. Its content ends at its inner edge (the window's rects are
// narrowed while it is open, as a Card's), so rows wrap within it; the cursor ends under it, 8 px
// below. BeginToolbar / EndToolbar: a well holding .tbb buttons -- FieldH() high (pad 0 6), 3 px
// apart -- and a disabled button inside it takes .tbb.dis (transparent, faint text and icon). The
// toolbar belongs to the window it was begun in, for this frame only: a popup, a card or a child
// window opened inside it never takes .tbb.dis, and a toolbar left open is forgotten next frame.
struct Well {
    ImDrawListSplitter split;
    ImVec2             p0;
    float              w = 0.0f;
    float              pad = 0.0f;
    float              saved_work_max = 0.0f;
    float              saved_content_max = 0.0f;
    bool               toolbar = false;
};
void BeginWell(Well& k, float pad = -1.0f);
void EndWell(Well& k);
void BeginToolbar(Well& k);
void EndToolbar(Well& k);

// 13-14 (G-2): an in-place confirm band around its content (BeginConfirmBand, the content, then
// EndConfirmBand): `warn` false = the amber confirm (.confirmbox: kConfirmLine edge, its .08 fill,
// r7, content 6 px inside); `warn` true = the warn band (Legacy: kWarn .08 fill, kConfirmLine edge,
// r5, content 8 / 3 px inside). `width` 0 = the room left. The content ends at the band's inner
// edge; the cursor ends under the band.
struct ConfirmBand {
    ImDrawListSplitter split;
    ImVec2             p0;
    float              w = 0.0f;
    bool               warn = false;
    float              pad_x = 0.0f, pad_y = 0.0f;
    float              saved_work_max = 0.0f;
    float              saved_content_max = 0.0f;
};
void BeginConfirmBand(ConfirmBand& b, float width = 0.0f, bool warn = false);
void EndConfirmBand(ConfirmBand& b);

// 13-14 (G-12): a colour swatch (ControlH() square, r5, the colour, stroke-strong edge, kFrameHover
// under the mouse) that opens a themed colour picker (a panel popup) on click. True while `rgb`
// changed this frame. `tip`: its tooltip. The picker is a BeginPanelPopup (the theme's metrics inside).
bool ColorSwatchButton(const char* id, float rgb[3], const char* tip = nullptr);
// 13-14 (finishing): the picker's extras. `inputs`: the RGB / HSV / Hex fields under the picker (type a
// value); `original`: the Current / Original preview beside it, Original the colour when the picker
// opened (a click on it puts it back).
struct ColorSwatchOpts {
    bool inputs = false;
    bool original = false;
};
bool ColorSwatchButton(const char* id, float rgb[3], const char* tip, const ColorSwatchOpts& o);
// A row of colour dots to pick from (the rule colours): `count` dots (r7 in a 20 px cell, 4 px apart),
// a 1.5 px kText ring (r9) around `current`, kStrokeStrong around the one under the mouse. Width-aware
// (ui_flow.h): a dot that does not fit beside the previous one wraps onto the next row. True when
// one was clicked: *picked = its index.
bool SwatchPalette(const char* id, const ImU32* colors, int count, ImU32 current, int* picked);

// 13-14 (A5 / B2): the disabled look. ItemDisabled: true inside ImGui::BeginDisabled (the next item
// is disabled). BeginDisabledLook / EndDisabledLook: BeginDisabled(disabled) without ImGui's fade,
// the text faint instead (the mockup's [disabled]) -- for stock content (Text, Selectable rows); the
// theme's buttons need no wrapper. Pair them always (EndDisabledLook ends what Begin started).
bool ItemDisabled();
void BeginDisabledLook(bool disabled = true);
void EndDisabledLook();
// The skin a themed button takes inside a disabled region, pushed just before ImGui::Button (the
// base of FittedButton / MenuButton / IconButton): full opacity, faint text, the fill kRaised
// (.btn[disabled]) or transparent for a ghost button and inside a toolbar (.tbb.dis). Does nothing
// (returns false) when the item is enabled. Pass its result to PopDisabledButtonSkin.
bool PushDisabledButtonSkin();
void PopDisabledButtonSkin(bool pushed);

// THE PANEL: the look of every panel and strip docked in the viewer, and of the open
// options menu -- the panel grey (kSurface) with a kStroke edge. Pushes 2 colours: call
// before Begin, PopStyleColor(2) after End. A window that is a panel uses this, never its
// own colours, so every panel matches.
void PushPanelStyle();
// 13-14 (finishing): a window over the 3D view with no panel grey of its own (the closed ☰ menu: the bare
// burger): kOverlayBg (kBg .72) with a kStroke edge. Pushes 2 colours as PushPanelStyle: call before
// Begin, PopStyleColor(2) after End.
void PushOverlayPanelStyle();

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
    // 13-14 (finishing): open, the card fills the window's height left (.secx.sg { flex: 1 }): its body is
    // a child (no background) as high as that room less the card's bottom pad and gap -- never under
    // `fill_min_h` mockup px -- and scrolls. The body begins after the header: in BeginSection, or in
    // EndSectionHeaderRight when `right_w` keeps a header slot; EndSection ends it.
    bool        fill_h = false;
    float       fill_min_h = 96.0f;
};
struct Section {
    Card   card;
    bool   open = true;
    bool   nested = false;
    ImVec2 head_min, head_max;     // the header row (inside the card's padding)
    float  right_w = 0.0f;
    ImVec2 resume;                 // where the content goes once the header's widgets are drawn
    bool   in_header = false;
    bool   fill_h = false;         // SectionOpts.fill_h: the body child is begun (body) or still to begin
    float  fill_min_h = 0.0f;
    bool   body = false;
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
