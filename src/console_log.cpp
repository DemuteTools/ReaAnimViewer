// SPDX-License-Identifier: MIT

#include "console_log.h"

#include <cstdarg>
#include <cstdio>

#include "reaper_api.h"  // ShowConsoleMsg

namespace rav {
namespace {

// Formats the caller's message, prefixes `[RAV] <level>: `, and emits one line.
void Emit(const char* level, const char* fmt, va_list args)
{
    char msg[512];
    std::vsnprintf(msg, sizeof(msg), fmt, args);

    char line[576];
    std::snprintf(line, sizeof(line), "[RAV] %s: %s\n", level, msg);
    ShowConsoleMsg(line);
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
