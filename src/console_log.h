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

#pragma once

namespace rav {

void LogInfo(const char* fmt, ...);
void LogWarn(const char* fmt, ...);
void LogError(const char* fmt, ...);

}  // namespace rav
