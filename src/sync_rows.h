// SPDX-License-Identifier: MIT
//
// 13-77c (readability pass): how the "Sync with global" window turns the engine's changes into ROWS, and the readable
// words it shows (pure: no REAPER, no _WIN32). The engine keeps one change per fact (a Sound, a Switch value, a case, a
// variation ...); the window groups them: an ADDED branch (a node with its case(s), the Switch value only it uses and
// everything under it that the same side added) is ONE row, a REMOVED branch with its cases too, and the variation /
// part changes of one Sound are ONE row "<sound> · N variations edited". Ticking a grouped row ticks all its keys.
// The grouping is built once per diff. Names are the user's: a Switch is named by its group, a Switch's child by the
// value that plays it, settings are spelled out ("Snap offset", "Gain") and their values formatted ("−3 dB").

#pragma once

#include <string>
#include <utility>
#include <vector>

#include "library_sync.h"

namespace rav {

enum class SyncRowKind { Single, Added, Removed, Edited };

struct SyncRowGroup {
    SyncRowKind              kind = SyncRowKind::Single;
    std::vector<std::string> keys;       // the lead first, then the rest in diff order
    SyncSide                 side = SyncSide::Both;  // the side every key shares, else Both
    int                      sounds = 0;       // Added / Removed: the Sounds in the branch
    std::string              value;            // Added / Removed: the Switch value that plays the branch ("" none)
    int                      edited = 0, added = 0, removed = 0, parts = 0, parked = 0;  // Edited: what changed
    // Edited, from the field-level details (13-77): variations by what moved in them, the part settings changed, the
    // edits each side made (the cells' "13 edits"), and whether the Sound's hand-edited flag is the ONLY difference.
    int                      moved = 0, gains = 0, fades = 0, snaps = 0, mutes = 0, others = 0, part_fields = 0;
    int                      here_edits = 0, file_edits = 0;
    bool                     hand_only = false;
};

// One line of an Edited row's detail strip: the CHANGE column text, and each side's value in its own column ("" = none).
struct SyncDetailLine {
    int         indent = 0;
    std::string name, global, project;
};

// Every change in exactly one group; groups in the order of their lead in the diff.
std::vector<SyncRowGroup> SyncGroupChanges(const SyncDiff& d);

// "Switch value Gravel" -> "Gravel" (a case's label); other labels unchanged.
std::string SyncCaseValueName(const std::string& label);
// A node the project does not have: its name; an unnamed one by the value that plays it, else its type word ("Random").
// "" when the diff knows nothing of it.
std::string SyncNodeDisplayName(const SyncDiff& d, const std::string& id);
// "3 variations edited" / "1 variation edited, 2 added" / "parts edited" (an Edited group).
std::string SyncEditSummary(const SyncRowGroup& g);
// The cell of one side for an Edited row: "13 edits" / "1 edit" / "unchanged" (`global`: the GLOBAL cell, else THIS PROJECT's).
std::string SyncSideSummary(const SyncRowGroup& g, bool global);
// An Edited group's detail strip: per variation / parts change its field differences, a side's value in its column.
std::vector<SyncDetailLine> SyncEditDetailLines(const SyncDiff& d, const SyncRowGroup& g);
// One field difference as one side shows it: "0.0 -> -3.0 dB" (arrow and true minus sign) on the side that moved, "0.0 dB" on
// the other, "changed" / "unchanged" for an opaque value, a dash for a variation that side lacks.
std::string SyncDetailCell(const SyncFieldDiff& f, bool global);
// One line of an Edited group's detail: "Variation 3: edited here" / "Parts: edited in the global" / "Variation 2: added here".
std::string SyncEditLine(const SyncChange& c);
// The value cell of an Added (the adder's) / Removed (the remover's) group: "+ Gravel · plays Run_Gravel (3 sounds)",
// "− Mud · plays Walk_Mud"; `name` = the branch top's name.
std::string SyncBranchCell(const SyncRowGroup& g, const std::string& name);
// A setting's name in clear: "Offset" -> "Snap offset", "edited changed" -> "Edited", "fade_in" -> "Fade in".
std::string SyncSettingName(const std::string& label);
// A setting's value in clear: "-3 dB" -> "−3 dB", "100/200 ms" -> "in 100 ms · out 200 ms", "" -> "not set", "1" (an
// unknown flag) -> "yes".
std::string SyncSettingValue(const std::string& label, const std::string& text);
// The branch's content for the detail strip: (indent level, text), the top first.
std::vector<std::pair<int, std::string>> SyncBranchLines(const SyncDiff& d, const SyncRowGroup& g);

}  // namespace rav
