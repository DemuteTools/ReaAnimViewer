// SPDX-License-Identifier: MIT
//
// The project's single user-feedback channel (AR16): printf-style helpers that
// funnel through Reaper's ShowConsoleMsg, emitting one line per event in the D7
// format `[RAV] <level>: <message>`. Levels are info | warn | error only.

#pragma once

namespace rav {

void LogInfo(const char* fmt, ...);
void LogWarn(const char* fmt, ...);
void LogError(const char* fmt, ...);

}  // namespace rav
