// SPDX-License-Identifier: MIT
//
// "RAV: Measure detection against reference markers" (Epic 10, spike 10-0): the
// measurement harness behind the epic's GO/NO-GO (NFR-AT1). Dev-facing; the detection it
// runs (bone_events / bone_roles / bone_presets) is production code.
//
// On the selected items: a knobs dialog first (session-remembered; Cancel = nothing
// done), then for each RAV animation item with `REF` take markers (else `REF` project
// markers over the item): Footsteps preset + Analyse + Detect on the item's file, compared
// in source time with its REF markers inside the visible part of the item. The report (per
// item, total, GO/NO-GO) goes to REAPER's console. The detections are written as `RAV?` take
// markers on each measured item, replacing only earlier `RAV?` markers, in one undo point.
// Spike 10-7a: a marker may be `REF <label>`, a role key or a bone name (ref_labels.h). Every
// REF counts in the Footsteps match; the bone dump (RAV_detection_dump/*.csv) also lists the
// times per label with the label's bone (roles.txt read as the Tagging view maps roles), and
// the report adds a "REF by bone" line.
// Spike 10-8a: every selected RAV item is loaded and dumped, REF or not ("no REF: physics only"
// skips the Footsteps measure alone), and gets the physics foot events (motion_physics.h, the
// user's role mapping) as `PHY <L|R> <heel|toe|tip|step|lift|slide|pivot> ...` take markers,
// replacing only earlier `PHY ` markers, in the same undo point as the RAV? markers. The report
// adds one physics line per item, then its events.
//
// The take-marker functions are resolved optionally (GetFunc), so an older REAPER still
// loads the extension (this action then says it needs REAPER 5.981 or newer).
// Main thread only. No-throw.

#pragma once

#ifdef _WIN32

namespace rav {

// Called once at load with REAPER's GetFunc (resolves the take-marker functions).
void InitDetectionMeasure(void* (*get_func)(const char* name));

// The action body.
void MeasureDetectionOnSelectedItems();

}  // namespace rav

#endif  // _WIN32
