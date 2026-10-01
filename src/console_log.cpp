// SPDX-License-Identifier: MIT

#include "console_log.h"

#include <cstdarg>
#include <cstdio>
#include <deque>
#include <mutex>

#include "reaper_api.h"  // ShowConsoleMsg

namespace rav {
namespace {

// Ring of the last warn/error lines for "Copy error log" (see console_log.h). Logging
// can come from Reaper-driven PCM_source callbacks as well as the render tick, so the
// ring is mutex-guarded. Small cap: a bug report needs the recent failures, not history.
constexpr size_t kMaxKeptLines = 64;

struct KeptLine {
    std::string text;   // one formatted line, with its trailing '\n'
    int         count;  // how many times it was logged (shown as "(xN)" when > 1)
};

std::mutex           g_kept_mutex;
std::deque<KeptLine> g_kept_lines;
std::string          g_context;  // see SetLogContext; "" = none

// Appends `text`, or — if the same line is already kept — moves it to the end and bumps
// its count, so a failure re-logged on every scrub can't flush the older lines out.
// Caller holds g_kept_mutex.
void AppendLocked(const std::string& text)
{
    for (auto it = g_kept_lines.begin(); it != g_kept_lines.end(); ++it) {
        if (it->text == text) {
            KeptLine moved{std::move(it->text), it->count + 1};
            g_kept_lines.erase(it);
            g_kept_lines.push_back(std::move(moved));
            return;
        }
    }
    if (g_kept_lines.size() >= kMaxKeptLines) g_kept_lines.pop_front();
    g_kept_lines.push_back({text, 1});
}

void Keep(const char* line)
{
    // A logger must never throw into its caller (several call-sites sit inside the
    // AR18 no-throw boundary, some in catch blocks) — drop the line on bad_alloc.
    try {
        std::lock_guard<std::mutex> lock(g_kept_mutex);
        if (g_context.empty()) {
            AppendLocked(line);
        } else {
            // Tag the line itself (not a separate header line) so the same warning from two
            // files stays two lines when repeats are merged. `line` ends with '\n'.
            std::string tagged(line);
            tagged.insert(tagged.size() - 1, "  [" + g_context + "]");
            AppendLocked(tagged);
        }
    } catch (...) {
    }
}

// Formats the caller's message, prefixes `[RAV] <level>: `, and emits one line.
//
// Silent by default (AR16, amended Epic 6.5): the console output is compiled in ONLY
// when RAV_ENABLE_CONSOLE_LOG is #defined, so a normal build produces no console
// output — every Log{Info,Warn,Error} call-site goes quiet here, at the single
// funnel, without deleting a single call-site. User-facing signals (FPS,
// load-failure) are surfaced on-canvas (FR53 / Story 6.5.5), not the console.
// `keep` lines (warn/error) are always formatted and kept for "Copy error log".
// Flip logging back on for a debug build with:
//   -DCMAKE_CXX_FLAGS="/D RAV_ENABLE_CONSOLE_LOG"   (MSVC; see build_debuglog.bat)
//   -DCMAKE_CXX_FLAGS="-D RAV_ENABLE_CONSOLE_LOG"    (GCC/Clang on Linux)
// This mirrors the project's existing RAV_FORCE_INIT_FAILURE test switch
// (gl_loader.cpp) — default OFF, costs nothing, never #defined in a normal build.
void Emit(const char* level, bool keep, const char* fmt, va_list args)
{
#ifndef RAV_ENABLE_CONSOLE_LOG
    // Default build: info lines cost nothing (no format, no output).
    if (!keep) return;
#endif
    char msg[512];
    std::vsnprintf(msg, sizeof(msg), fmt, args);

    char line[576];
    std::snprintf(line, sizeof(line), "[RAV] %s: %s\n", level, msg);
    if (keep) Keep(line);
#ifdef RAV_ENABLE_CONSOLE_LOG
    ShowConsoleMsg(line);
#endif
}

}  // namespace

void LogInfo(const char* fmt, ...)
{
    va_list args; va_start(args, fmt);
    Emit("info", /*keep=*/false, fmt, args);
    va_end(args);
}

void LogWarn(const char* fmt, ...)
{
    va_list args; va_start(args, fmt);
    Emit("warn", /*keep=*/true, fmt, args);
    va_end(args);
}

void LogError(const char* fmt, ...)
{
    va_list args; va_start(args, fmt);
    Emit("error", /*keep=*/true, fmt, args);
    va_end(args);
}

void SetLogContext(const std::string& context)
{
    try {
        std::lock_guard<std::mutex> lock(g_kept_mutex);
        g_context = context;
    } catch (...) {
    }
}

std::string RecentLogText()
{
    std::lock_guard<std::mutex> lock(g_kept_mutex);
    std::string text;
    for (const KeptLine& l : g_kept_lines) {
        if (l.count > 1) {
            // Insert the count before the line's trailing newline.
            text.append(l.text, 0, l.text.size() - 1);
            text += " (x" + std::to_string(l.count) + ")\n";
        } else {
            text += l.text;
        }
    }
    return text;
}

bool HasRecentLog()
{
    std::lock_guard<std::mutex> lock(g_kept_mutex);
    return !g_kept_lines.empty();
}

}  // namespace rav
