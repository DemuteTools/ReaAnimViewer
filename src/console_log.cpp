// SPDX-License-Identifier: MIT

#include "console_log.h"

#include <cstdarg>
#include <cstdio>

#include "reaper_api.h"  // ShowConsoleMsg

namespace rav {
namespace {

// Formats the caller's message, prefixes `[RAV] <level>: `, and emits one line.
//
// Silent by default (AR16, amended Epic 6.5): the body is compiled in ONLY when
// RAV_ENABLE_CONSOLE_LOG is #defined, so a normal build produces no console
// output — every Log{Info,Warn,Error} call-site goes quiet here, at the single
// funnel, without deleting a single call-site. User-facing signals (FPS,
// load-failure) are surfaced on-canvas (FR53 / Story 6.5.5), not the console.
// Flip logging back on for a debug build with:
//   -DCMAKE_CXX_FLAGS="/D RAV_ENABLE_CONSOLE_LOG"   (MSVC; see build_debuglog.bat)
//   -DCMAKE_CXX_FLAGS="-D RAV_ENABLE_CONSOLE_LOG"    (GCC/Clang on Linux)
// This mirrors the project's existing RAV_FORCE_INIT_FAILURE test switch
// (gl_loader.cpp) — default OFF, costs nothing, never #defined in a normal build.
void Emit(const char* level, const char* fmt, va_list args)
{
#ifdef RAV_ENABLE_CONSOLE_LOG
    char msg[512];
    std::vsnprintf(msg, sizeof(msg), fmt, args);

    char line[576];
    std::snprintf(line, sizeof(line), "[RAV] %s: %s\n", level, msg);
    ShowConsoleMsg(line);
#else
    // Default build: no-op. Reference the params so a future /W4 (or -Wunused-
    // parameter) bump stays clean. Keep the params — the debug branch needs them.
    (void)level;
    (void)fmt;
    (void)args;
#endif
}

}  // namespace

void LogInfo(const char* fmt, ...)
{
    va_list args; va_start(args, fmt);
    Emit("info", fmt, args);
    va_end(args);
}

void LogWarn(const char* fmt, ...)
{
    va_list args; va_start(args, fmt);
    Emit("warn", fmt, args);
    va_end(args);
}

void LogError(const char* fmt, ...)
{
    va_list args; va_start(args, fmt);
    Emit("error", fmt, args);
    va_end(args);
}

}  // namespace rav
