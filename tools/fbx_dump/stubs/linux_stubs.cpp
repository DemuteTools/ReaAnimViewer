// SPDX-License-Identifier: MIT
//
// Link-time stand-ins for what the load path pulls from the plugin's Windows-only
// units: the REAPER console log (here: stderr, warnings only), the modern-GL function
// table (null: never called without a GL context), and stb's Win32 UTF-8 helpers.
#include "win32_prelude.h"

#include "console_log.h"
#include "gl_loader.h"

namespace rav {

namespace {
void Print(const char* tag, const char* fmt, va_list ap)
{
    std::fprintf(stderr, "%s", tag);
    std::vfprintf(stderr, fmt, ap);
    std::fprintf(stderr, "\n");
}
}  // namespace

void LogInfo(const char*, ...) {}
void LogWarn(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    Print("warn: ", fmt, ap);
    va_end(ap);
}
void LogError(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    Print("error: ", fmt, ap);
    va_end(ap);
}
void SetLogContext(const std::string&) {}
std::string RecentLogText() { return {}; }
bool HasRecentLog() { return false; }

}  // namespace rav

#define RAV_GL_DEF(ret, name, args) PFN_##name rav_##name = nullptr;
RAV_GL_FUNCS(RAV_GL_DEF)
#undef RAV_GL_DEF

extern "C" int MultiByteToWideChar(unsigned int, unsigned long, const char* str, int, wchar_t* out, int cap)
{
    int i = 0;
    for (; i < cap; ++i) {
        out[i] = static_cast<unsigned char>(str[i]);
        if (!str[i]) return i + 1;
    }
    return 0;
}

extern "C" int WideCharToMultiByte(unsigned int, unsigned long, const wchar_t* in, int, char* out, int cap,
                                   const char*, int*)
{
    int i = 0;
    for (; i < cap; ++i) {
        out[i] = static_cast<char>(in[i]);
        if (!in[i]) return i + 1;
    }
    return 0;
}

extern "C" FILE* _wfopen(const wchar_t* name, const wchar_t* mode)
{
    std::string n, m;
    for (; *name; ++name) n.push_back(static_cast<char>(*name));
    for (; *mode; ++mode) m.push_back(static_cast<char>(*mode));
    return std::fopen(n.c_str(), m.c_str());
}
