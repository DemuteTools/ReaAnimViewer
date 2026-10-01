// SPDX-License-Identifier: MIT
//
// Silent-by-default diagnostic helpers (AR16, amended Epic 6.5). printf-style
// log helpers that all funnel through a single Emit() into Reaper's
// ShowConsoleMsg. Emit()'s body is compiled to a NO-OP unless
// RAV_ENABLE_CONSOLE_LOG is defined, so a normal build produces no console
// output — the diagnostic capability is gated, never deleted (re-enable for a
// debug build via build_debuglog.bat). User-facing signals are surfaced
// on-canvas (FR53 / Story 6.5.5), not the console. When logging IS compiled in,
// the D7 format `[RAV] <level>: <message>` still applies; levels are
// info | warn | error only.
//
// Copyable diagnostics: the last warn/error lines are ALWAYS kept in a small
// in-memory ring, even in a silent build, so a user can copy them from the
// viewer (Copy error log) and paste them into a bug report. The console stays
// silent; info lines are not kept (the 1-Hz FPS line would flush the ring).
// A line already in the ring is moved to the end with a repeat count instead of
// being added again (scrubbing back onto a broken item re-logs its failure).

#pragma once

#include <string>

namespace rav {

void LogInfo(const char* fmt, ...);
void LogWarn(const char* fmt, ...);
void LogError(const char* fmt, ...);

// Names what the following kept lines are about (e.g. the file being loaded), so
// warnings that don't carry a file name can be told apart in a report: each kept
// line is tagged "  [<context>]" until it is cleared with "". Only the kept copy is
// tagged, not the console line. Thread-safe.
void SetLogContext(const std::string& context);

// The kept warn/error lines, oldest first, one per line ("" if none). Thread-safe.
std::string RecentLogText();
bool        HasRecentLog();

}  // namespace rav
