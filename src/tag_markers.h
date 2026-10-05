// SPDX-License-Identifier: MIT
//
// Apply (Epic 10, story 10-4): writes RAV's markers on every selected item that has rules,
// from each item's event list (event_list.h), in ONE undo point ("RAV: Apply auto-tagging",
// UNDO_STATE_ALL), and records what it wrote in each item's record.
//
//   - Per item: the markers RAV recorded as its own are deleted first (project markers by
//     GUID, take markers by the time and name recorded at Apply; a recorded GUID that no
//     longer exists is ignored), then the take and / or project markers are written as the
//     global option says (project markers in the rule's colour, take markers too).
//   - A marker RAV does not own with the same name at the same time (+-1 ms) on that take
//     (take marker) or in the project (project marker) is left as it is: RAV writes no
//     duplicate and takes no ownership; the result counts it ("already present").
//   - Items without rules, and items whose roles have no bone (or whose file does not load),
//     are skipped and counted.
//   - The marker functions are resolved through GetFunc (REAPER 5.981+ take markers, REAPER 7
//     marker GUIDs): when one the option needs is missing, Apply writes nothing and says so.
//
// The option (Take / Project / Both) is a viewer setting kept in REAPER ExtState
// (shortcuts.h LoadPrefFloat / SavePrefFloat), default Both.
//
// Main thread only. No-throw. Nothing goes to the REAPER console.

#pragma once

#ifdef _WIN32

#include <string>

#include "rule_record.h"  // MarkerMode

namespace rav {

// Resolves the marker functions (plugin load, with rec->GetFunc).
void InitTagMarkers(void* (*get_func)(const char* name));

MarkerMode GetTaggingMarkerMode();
void SetTaggingMarkerMode(MarkerMode mode);  // kept across sessions
// "Writes take markers" / "Writes project markers" / "Writes take + project markers".
const char* MarkerModeLine(MarkerMode mode);

struct ApplyResult {
    bool        ran = false;       // something was written (one undo point)
    int         items = 0;         // items whose markers were written
    int         markers = 0;       // markers written
    int         without_rules = 0; // selected items without rules (or not RAV items)
    int         roles_skipped = 0; // skipped: a role has no bone, or the file did not load
    int         failed = 0;        // skipped: the item could not be read or written
    int         already_present = 0;  // slots left to a marker RAV does not own
    std::string error;             // non-empty: nothing was written, why
};

// Apply on the selected items. The result is also kept for the footer.
ApplyResult ApplyTaggingMarkers();
const ApplyResult& LastApplyResult();
void ClearLastApplyResult();

}  // namespace rav

#endif  // _WIN32
