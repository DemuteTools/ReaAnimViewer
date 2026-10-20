// SPDX-License-Identifier: MIT
//
// See watchdog.h. The breadcrumb ring, the scope stack, the heartbeats and the report's text are platform-neutral
// (the host test runs them); the watchdog thread, the report file and the native stack are Windows only.

#include "watchdog.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <functional>
#include <thread>

#if defined(_WIN32) && !defined(RAV_HOST_TEST)
#define RAV_WD_WIN 1
#include <windows.h>
#endif

namespace rav {
namespace wd {
namespace {

// ---- the ring of crumbs (a seqlock per slot: seq 0 while written, then its number + 1) ----

struct Slot {
    std::atomic<uint64_t>    seq{0};
    std::atomic<const char*> what{nullptr};
    std::atomic<int64_t>     ms{0};
    std::atomic<uint32_t>    tid{0};
    char                     arg[kArgLen] = {};
};
Slot                  g_ring[kCrumbs];
std::atomic<uint64_t> g_head{0};

void CopyArg(char* dst, const char* src)
{
    size_t i = 0;
    if (src)
        for (; i + 1 < static_cast<size_t>(kArgLen) && src[i]; ++i) dst[i] = src[i];
    dst[i] = '\0';
}

// A number as text into buf (24 bytes), by hand: the hot paths call no CRT formatting (its locale lock).
void NumText(char* buf, long long v)
{
    char               tmp[24];
    int                k = 0;
    unsigned long long u = v < 0 ? 0ull - static_cast<unsigned long long>(v) : static_cast<unsigned long long>(v);
    do {
        tmp[k++] = static_cast<char>('0' + u % 10);
        u /= 10;
    } while (u && k < 21);
    int i = 0;
    if (v < 0) buf[i++] = '-';
    while (k) buf[i++] = tmp[--k];
    buf[i] = '\0';
}

// ---- the main thread's scopes ----

struct ScopeSlot {
    std::atomic<const char*> what{nullptr};
    std::atomic<int64_t>     ms{0};
    char                     arg[kArgLen] = {};
};
ScopeSlot             g_scopes[kScopeDepth];
std::atomic<int>      g_depth{0};
std::atomic<uint32_t> g_main_tid{0};

// ---- heartbeats and states ----

std::atomic<int64_t>     g_main_beat{0};
std::atomic<bool>        g_armed{false};  // a heartbeat came since the start (REAPER's own load is not a stall)
std::atomic<int>         g_audio_depth{0};
std::atomic<const char*> g_audio_what{nullptr};
std::atomic<int64_t>     g_audio_in{0}, g_audio_out{0};
std::atomic<uint32_t>    g_audio_tid{0};
char                     g_audio_arg[kArgLen] = {};  // the file the audio thread reads (racy copy, read for a report only)
std::atomic<int>         g_video_depth{0};
std::atomic<const char*> g_video_what{nullptr};
std::atomic<int64_t>     g_video_in{0}, g_video_out{0};
std::atomic<uint32_t>    g_video_tid{0};
char                     g_video_arg[kArgLen] = {};

struct WorkerSlotData {
    std::atomic<bool>        taken{false};
    std::atomic<const char*> name{nullptr};  // published once its index is set
    std::atomic<int>         index{0};
    std::atomic<const char*> what{nullptr};
    std::atomic<int64_t>     since{0};
    char                     arg[kArgLen] = {};
};
WorkerSlotData g_workers[kWorkers];

// Appends printf-style text to a fixed buffer, always 0-terminated, never past cap.
struct Out {
    char*  buf;
    size_t cap, len = 0;
    void   f(const char* fmt, ...)
    {
        if (!buf || cap == 0 || len + 1 >= cap) return;
        va_list ap;
        va_start(ap, fmt);
        const int n = std::vsnprintf(buf + len, cap - len, fmt, ap);
        va_end(ap);
        if (n > 0) len = std::min(cap - 1, len + static_cast<size_t>(n));
    }
};

double Secs(int64_t ms) { return static_cast<double>(ms) / 1000.0; }

}  // namespace

int64_t NowMs()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

uint32_t CurrentThreadId()
{
#ifdef RAV_WD_WIN
    return static_cast<uint32_t>(::GetCurrentThreadId());
#else
    return static_cast<uint32_t>(std::hash<std::thread::id>()(std::this_thread::get_id()) | 1u);
#endif
}

void SetMainThread() { g_main_tid.store(CurrentThreadId(), std::memory_order_relaxed); }

void Crumb(const char* what, const char* arg)
{
    const uint64_t n = g_head.fetch_add(1, std::memory_order_relaxed);
    Slot&          s = g_ring[n % kCrumbs];
    s.seq.store(0, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    s.what.store(what, std::memory_order_relaxed);
    s.ms.store(NowMs(), std::memory_order_relaxed);
    s.tid.store(CurrentThreadId(), std::memory_order_relaxed);
    CopyArg(s.arg, arg);
    s.seq.store(n + 1, std::memory_order_release);
}

void CrumbNum(const char* what, long long n)
{
    char buf[24];
    NumText(buf, n);
    Crumb(what, buf);
}

Scope::Scope(const char* what, const char* arg) { Enter(what, arg); }

Scope::Scope(const char* what, long long n) { EnterNum(what, n, true); }

void Scope::EnterNum(const char* what, long long n, bool crumb)
{
    char buf[24];
    NumText(buf, n);
    Enter(what, buf, crumb);
}

void Scope::Enter(const char* what, const char* arg, bool crumb)
{
    if (crumb) Crumb(what, arg);
    if (CurrentThreadId() != g_main_tid.load(std::memory_order_relaxed)) return;
    const int d = g_depth.load(std::memory_order_relaxed);
    if (d >= 0 && d < kScopeDepth) {
        ScopeSlot& s = g_scopes[d];
        s.what.store(what, std::memory_order_relaxed);
        s.ms.store(NowMs(), std::memory_order_relaxed);
        CopyArg(s.arg, arg);
    }
    g_depth.store(d + 1, std::memory_order_release);
    pushed_ = true;
}

Scope::~Scope()
{
    if (pushed_) g_depth.store(std::max(0, g_depth.load(std::memory_order_relaxed) - 1), std::memory_order_release);
}

void MainBeat()
{
    g_main_beat.store(NowMs(), std::memory_order_relaxed);
    g_armed.store(true, std::memory_order_relaxed);
}

void AudioEnter(const char* what)
{
    g_audio_what.store(what, std::memory_order_relaxed);
    g_audio_tid.store(CurrentThreadId(), std::memory_order_relaxed);
    g_audio_in.store(NowMs(), std::memory_order_relaxed);
    g_audio_depth.fetch_add(1, std::memory_order_relaxed);
}

void AudioExit()
{
    g_audio_out.store(NowMs(), std::memory_order_relaxed);
    g_audio_depth.fetch_sub(1, std::memory_order_relaxed);
}

void AudioArg(const char* path)
{
    if (!path) return;
    const size_t n = std::min(std::strlen(path), static_cast<size_t>(kArgLen - 1));
    std::memcpy(g_audio_arg, path + (std::strlen(path) - n), n);  // the end of the path: its file name
    g_audio_arg[n] = '\0';
}

void VideoEnter(const char* what, const char* arg)
{
    if (arg) {
        const size_t n = std::min(std::strlen(arg), static_cast<size_t>(kArgLen - 1));
        std::memcpy(g_video_arg, arg + (std::strlen(arg) - n), n);
        g_video_arg[n] = '\0';
    } else {
        g_video_arg[0] = '\0';
    }
    g_video_what.store(what, std::memory_order_relaxed);
    g_video_tid.store(CurrentThreadId(), std::memory_order_relaxed);
    g_video_in.store(NowMs(), std::memory_order_relaxed);
    g_video_depth.fetch_add(1, std::memory_order_relaxed);
}

void VideoExit()
{
    g_video_out.store(NowMs(), std::memory_order_relaxed);
    g_video_depth.fetch_sub(1, std::memory_order_relaxed);
}

int WorkerSlot(const char* name, int index)
{
    if (!name) return -1;
    for (int i = 0; i < kWorkers; ++i) {
        const char* n = g_workers[i].name.load(std::memory_order_acquire);
        if (n && std::strcmp(n, name) == 0 && g_workers[i].index.load(std::memory_order_relaxed) == index) return i;
    }
    for (int i = 0; i < kWorkers; ++i)
        if (!g_workers[i].taken.exchange(true, std::memory_order_acq_rel)) {
            g_workers[i].index.store(index, std::memory_order_relaxed);
            g_workers[i].name.store(name, std::memory_order_release);
            return i;
        }
    return -1;
}

void WorkerBusy(int slot, const char* what, const char* arg)
{
    if (slot < 0 || slot >= kWorkers) return;
    WorkerSlotData& w = g_workers[slot];
    w.what.store(nullptr, std::memory_order_relaxed);
    CopyArg(w.arg, arg);
    w.since.store(NowMs(), std::memory_order_relaxed);
    w.what.store(what, std::memory_order_release);
}

void WorkerIdle(int slot)
{
    if (slot < 0 || slot >= kWorkers) return;
    g_workers[slot].what.store(nullptr, std::memory_order_relaxed);
    g_workers[slot].since.store(NowMs(), std::memory_order_relaxed);
}

int SnapshotCrumbs(CrumbView* out, int cap)
{
    if (!out || cap <= 0) return 0;
    const uint64_t head = g_head.load(std::memory_order_acquire);
    const uint64_t first = head > static_cast<uint64_t>(kCrumbs) ? head - kCrumbs : 0;
    int            k = 0;
    for (uint64_t n = first; n < head && k < cap; ++n) {
        const Slot&    s = g_ring[n % kCrumbs];
        const uint64_t q1 = s.seq.load(std::memory_order_acquire);
        if (q1 != n + 1) continue;  // being written, or already reused
        CrumbView v;
        v.seq = n;
        v.what = s.what.load(std::memory_order_relaxed);
        v.ms = s.ms.load(std::memory_order_relaxed);
        v.tid = s.tid.load(std::memory_order_relaxed);
        std::memcpy(v.arg, s.arg, sizeof(v.arg));
        v.arg[kArgLen - 1] = '\0';
        std::atomic_thread_fence(std::memory_order_acquire);
        if (s.seq.load(std::memory_order_relaxed) != q1) continue;
        out[k++] = v;
    }
    return k;
}

int SnapshotScopes(ScopeView* out, int cap, int* depth)
{
    const int d = g_depth.load(std::memory_order_acquire);
    if (depth) *depth = d;
    int k = 0;
    for (int i = 0; i < d && i < kScopeDepth && out && k < cap; ++i) {
        ScopeView v;
        v.what = g_scopes[i].what.load(std::memory_order_relaxed);
        v.ms = g_scopes[i].ms.load(std::memory_order_relaxed);
        std::memcpy(v.arg, g_scopes[i].arg, sizeof(v.arg));
        v.arg[kArgLen - 1] = '\0';
        out[k++] = v;
    }
    return k;
}

size_t FormatReport(char* buf, size_t cap, const ReportHead& h)
{
    Out o{buf, cap};
    if (buf && cap) buf[0] = '\0';
    o.f("RAV freeze report\r\n");
    o.f("Version: %s\r\n", h.version ? h.version : "");
    o.f("Written: %s (local time)\r\n", h.when ? h.when : "");
    o.f("REAPER's main thread gave no heartbeat for %.1f s.\r\n\r\n", Secs(h.stall_ms));

    ScopeView sc[kScopeDepth];
    int       depth = 0;
    const int ns = SnapshotScopes(sc, kScopeDepth, &depth);
    if (depth <= 0) {
        o.f("Main thread: outside RAV (no RAV scope open: REAPER, another extension or a dialog held it).\r\n");
    } else {
        o.f("Main thread inside RAV, outermost first (%d open):\r\n", depth);
        for (int i = 0; i < ns; ++i)
            o.f("  %2d. %s%s%s%s  (entered %.1f s ago)\r\n", i + 1, sc[i].what ? sc[i].what : "?", sc[i].arg[0] ? "  \"" : "",
                sc[i].arg, sc[i].arg[0] ? "\"" : "", Secs(h.now_ms - sc[i].ms));
        if (depth > ns) o.f("  ... %d deeper not kept\r\n", depth - ns);
    }

    const int         ad = g_audio_depth.load(std::memory_order_relaxed);
    const char*       aw = g_audio_what.load(std::memory_order_relaxed);
    const int64_t     ain = g_audio_in.load(std::memory_order_relaxed), aout = g_audio_out.load(std::memory_order_relaxed);
    if (!ain) o.f("Audio thread: never ran RAV's sources.\r\n");
    else if (ad > 0)
        o.f("Audio thread: INSIDE %s for %.1f s (thread %u).\r\n", aw ? aw : "?", Secs(h.now_ms - ain),
            static_cast<unsigned>(g_audio_tid.load(std::memory_order_relaxed)));
    else o.f("Audio thread: idle, last left %s %.2f s ago.\r\n", aw ? aw : "?", Secs(h.now_ms - aout));
    if (ain && ad > 0 && g_audio_arg[0]) {
        char file[kArgLen];
        std::memcpy(file, g_audio_arg, kArgLen);
        file[kArgLen - 1] = '\0';
        o.f("  reading \"%s\" (the last file it asked for)\r\n", file);
    }
    const int     vd = g_video_depth.load(std::memory_order_relaxed);
    const int64_t vin = g_video_in.load(std::memory_order_relaxed), vout = g_video_out.load(std::memory_order_relaxed);
    if (!vin) {
        o.f("Video thread: never ran RAV's video frame.\r\n");
    } else if (vd > 0) {
        char file[kArgLen];
        std::memcpy(file, g_video_arg, kArgLen);
        file[kArgLen - 1] = '\0';
        const char* vw = g_video_what.load(std::memory_order_relaxed);
        o.f("Video thread: INSIDE %s%s%s%s for %.1f s (thread %u).\r\n", vw ? vw : "?", file[0] ? " \"" : "", file,
            file[0] ? "\"" : "", Secs(h.now_ms - vin), static_cast<unsigned>(g_video_tid.load(std::memory_order_relaxed)));
    } else {
        const char* vw = g_video_what.load(std::memory_order_relaxed);
        o.f("Video thread: idle, last left %s %.2f s ago.\r\n", vw ? vw : "?", Secs(h.now_ms - vout));
    }

    o.f("Workers:");
    bool any = false;
    for (int i = 0; i < kWorkers; ++i) {
        const char* n = g_workers[i].name.load(std::memory_order_acquire);
        if (!n) continue;
        any = true;
        const char* w = g_workers[i].what.load(std::memory_order_acquire);
        char        arg[kArgLen];
        std::memcpy(arg, g_workers[i].arg, sizeof(arg));
        arg[kArgLen - 1] = '\0';
        const double for_s = Secs(h.now_ms - g_workers[i].since.load(std::memory_order_relaxed));
        if (w) o.f("\r\n  %s %d: %s%s%s%s for %.1f s", n, g_workers[i].index.load(), w, arg[0] ? "  \"" : "", arg, arg[0] ? "\"" : "", for_s);
        else o.f("\r\n  %s %d: idle", n, g_workers[i].index.load());
    }
    o.f(any ? "\r\n\r\n" : " none started.\r\n\r\n");

    static CrumbView cr[kCrumbs];  // the watchdog thread only (the host test: one thread)
    const int        nc = SnapshotCrumbs(cr, kCrumbs);
    const uint32_t   main = g_main_tid.load(std::memory_order_relaxed);
    o.f("Last %d crumbs, oldest first (seconds before this report, thread):\r\n", nc);
    for (int i = 0; i < nc; ++i) {
        char who[16];
        if (cr[i].tid == main) std::snprintf(who, sizeof(who), "main");
        else std::snprintf(who, sizeof(who), "%u", static_cast<unsigned>(cr[i].tid));
        o.f("  %9.3f  %-6s %s%s%s%s\r\n", Secs(h.now_ms - cr[i].ms), who, cr[i].what ? cr[i].what : "?", cr[i].arg[0] ? "  \"" : "",
            cr[i].arg, cr[i].arg[0] ? "\"" : "");
    }
    return o.len;
}

void ResetForTest()
{
    g_head.store(0);
    for (Slot& s : g_ring) {
        s.seq.store(0);
        s.what.store(nullptr);
        s.arg[0] = '\0';
    }
    g_depth.store(0);
    g_audio_depth.store(0);
    g_audio_what.store(nullptr);
    g_audio_in.store(0);
    g_audio_out.store(0);
    g_audio_arg[0] = '\0';
    g_video_depth.store(0);
    g_video_what.store(nullptr);
    g_video_in.store(0);
    g_video_out.store(0);
    g_video_arg[0] = '\0';
    for (WorkerSlotData& w : g_workers) {
        w.taken.store(false);
        w.name.store(nullptr);
        w.what.store(nullptr);
        w.arg[0] = '\0';
    }
    g_main_beat.store(0);
    g_armed.store(false);
}

// ---- the watchdog thread (Windows) ----------------------------------------------------------------------------

#ifdef RAV_WD_WIN
namespace {

constexpr int64_t kStallMs = 10000;         // no heartbeat this long = a report
constexpr int64_t kSecondSampleMs = 20000;  // the second stack sample, after the first
constexpr int64_t kWakeMs = 500;
constexpr int     kKeepReports = 20;
constexpr int     kMaxFrames = 64;

HANDLE       g_thread = nullptr;
HANDLE       g_stop = nullptr;
HANDLE       g_main = nullptr;  // the main thread (suspend, context, times)
wchar_t      g_dir[MAX_PATH] = {};  // <resource>\RAV
char         g_version[64] = {};
std::string& g_pending = *new std::string();  // the last session's report (main thread)
std::string& g_pending_path = *new std::string();
bool         g_toast = false;

using MappedFileNameFn = DWORD(WINAPI*)(HANDLE, LPVOID, LPWSTR, DWORD);
MappedFileNameFn g_mapped_name = nullptr;  // resolved at load: no loader call while the main thread is suspended

char g_text[128 * 1024];  // the watchdog thread's report buffer (no allocation once it runs)

void LocalStamp(char* when, size_t wn, wchar_t* file, size_t fn)
{
    SYSTEMTIME t;
    GetLocalTime(&t);
    if (when)
        std::snprintf(when, wn, "%04u-%02u-%02u %02u:%02u:%02u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    if (file)
        _snwprintf(file, fn, L"freeze-%04u%02u%02u-%02u%02u%02u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
}

HANDLE OpenReport(const wchar_t* stem)
{
    CreateDirectoryW(g_dir, nullptr);
    wchar_t path[MAX_PATH + 64];
    for (int k = 0; k < 10; ++k) {
        if (k == 0) _snwprintf(path, MAX_PATH + 64, L"%ls\\%ls.txt", g_dir, stem);
        else _snwprintf(path, MAX_PATH + 64, L"%ls\\%ls-%d.txt", g_dir, stem, k);
        path[MAX_PATH + 63] = L'\0';
        HANDLE f = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f != INVALID_HANDLE_VALUE) return f;
        if (GetLastError() != ERROR_FILE_EXISTS) return INVALID_HANDLE_VALUE;
    }
    return INVALID_HANDLE_VALUE;
}

void WriteText(HANDLE f, const char* s, size_t n)
{
    if (f == INVALID_HANDLE_VALUE || !s) return;
    while (n > 0) {
        DWORD put = 0;
        if (!WriteFile(f, s, static_cast<DWORD>(std::min<size_t>(n, 1 << 20)), &put, nullptr) || put == 0) return;
        s += put;
        n -= put;
    }
    // No FlushFileBuffers: the text is in the system's cache once written, a killed REAPER does not lose it, and a
    // flush per line would cost a disk sync each.
}

void WriteLine(HANDLE f, const char* fmt, ...)
{
    char    line[512];
    va_list ap;
    va_start(ap, fmt);
    const int n = std::vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (n > 0) WriteText(f, line, std::min(static_cast<size_t>(n), sizeof(line) - 1));
}

uint64_t ThreadCpu100ns(HANDLE th)
{
    FILETIME c, e, k, u;
    if (!th || !GetThreadTimes(th, &c, &e, &k, &u)) return 0;
    auto v = [](const FILETIME& t) { return (static_cast<uint64_t>(t.dwHighDateTime) << 32) | t.dwLowDateTime; };
    return v(k) + v(u);
}

// The main thread's stack, read while it is suspended: its registers and a copy of its stack's top (a kernel call
// each: no lock of the loader or the heap is taken while it is suspended, it may hold them). The return addresses
// are picked from the copy once it runs again (StackReturns).
constexpr size_t kStackCopy = 64 * 1024;
uint8_t          g_stack_copy[kStackCopy];

struct RawStack {
    uint64_t pc = 0;
    size_t   got = 0;  // bytes copied into g_stack_copy (from the stack pointer up)
};

RawStack SuspendAndCopy(HANDLE target)
{
    RawStack r;
#if defined(_M_X64) || defined(__x86_64__)
    if (!target) return r;
    // Time-critical while the main thread is suspended, so the window stays a few microseconds even on a busy PC
    // (the main thread may hold a lock the audio thread waits for).
    const HANDLE self = GetCurrentThread();
    const int    prio = GetThreadPriority(self);
    SetThreadPriority(self, THREAD_PRIORITY_TIME_CRITICAL);
    if (SuspendThread(target) == static_cast<DWORD>(-1)) {
        SetThreadPriority(self, prio == THREAD_PRIORITY_ERROR_RETURN ? THREAD_PRIORITY_NORMAL : prio);
        return r;
    }
    CONTEXT ctx;
    std::memset(&ctx, 0, sizeof(ctx));
    ctx.ContextFlags = CONTEXT_CONTROL;
    if (GetThreadContext(target, &ctx)) {
        r.pc = ctx.Rip;
        MEMORY_BASIC_INFORMATION mb;
        if (VirtualQuery(reinterpret_cast<LPCVOID>(ctx.Rsp), &mb, sizeof(mb)) == sizeof(mb) && mb.State == MEM_COMMIT) {
            const uint64_t end = reinterpret_cast<uint64_t>(mb.BaseAddress) + mb.RegionSize;
            const size_t   want = static_cast<size_t>(std::min<uint64_t>(kStackCopy, end > ctx.Rsp ? end - ctx.Rsp : 0));
            SIZE_T         got = 0;
            if (want && ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(ctx.Rsp), g_stack_copy, want, &got))
                r.got = static_cast<size_t>(got);
        }
    }
    ResumeThread(target);
    SetThreadPriority(self, prio == THREAD_PRIORITY_ERROR_RETURN ? THREAD_PRIORITY_NORMAL : prio);
#endif
    return r;
}

bool Executable(DWORD protect)
{
    return (protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
}

// True for an address in a loaded module's code right after a call instruction (a return address).
bool ReturnAddress(uint64_t a)
{
    if (a < 0x10000) return false;
    MEMORY_BASIC_INFORMATION mb;
    if (VirtualQuery(reinterpret_cast<LPCVOID>(a), &mb, sizeof(mb)) != sizeof(mb) || mb.Type != MEM_IMAGE || mb.State != MEM_COMMIT ||
        !Executable(mb.Protect) || a - reinterpret_cast<uint64_t>(mb.BaseAddress) < 7)
        return false;
    uint8_t c[7] = {};
    SIZE_T  got = 0;
    if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(a - 7), c, sizeof(c), &got) || got != sizeof(c)) return false;
    auto ff2 = [](uint8_t op, uint8_t modrm) { return op == 0xFF && (modrm & 0x38) == 0x10; };  // FF /2: call r/m
    return c[2] == 0xE8                                 // call rel32
           || ff2(c[1], c[2])                           // call [rip+disp32] / [reg+disp32]
           || ff2(c[0], c[1])                           // the same with a SIB byte
           || ff2(c[4], c[5])                           // call [reg+disp8]
           || (ff2(c[5], c[6]) && (c[6] & 0xC0) == 0xC0)  // call reg
           || ff2(c[3], c[4]);                          // call [reg+sib+disp8]
}

// The frames: the instruction pointer, then each return address found on the copied stack, innermost first.
int StackReturns(const RawStack& r, uint64_t* pcs, int cap)
{
    int n = 0;
    if (r.pc && n < cap) pcs[n++] = r.pc;
    for (size_t off = 0; off + 8 <= r.got && n < cap; off += 8) {
        uint64_t w = 0;
        std::memcpy(&w, g_stack_copy + off, 8);
        if (ReturnAddress(w)) pcs[n++] = w;
    }
    return n;
}

int SampleMainStack(uint64_t* pcs, int cap) { return StackReturns(SuspendAndCopy(g_main), pcs, cap); }

// A named thread's stack (the audio or the video thread that is the one stuck): opened by id for the sample only.
int SampleThreadStack(uint32_t tid, uint64_t* pcs, int cap)
{
    if (!tid) return 0;
    HANDLE th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, tid);
    if (!th) return 0;
    const int n = StackReturns(SuspendAndCopy(th), pcs, cap);
    CloseHandle(th);
    return n;
}

// "module.dll+0x1a2b" for an address (its image's base from the memory map: no loader lock taken).
void ModuleOffset(uint64_t pc, char* out, size_t cap)
{
    MEMORY_BASIC_INFORMATION mb;
    if (VirtualQuery(reinterpret_cast<LPCVOID>(pc), &mb, sizeof(mb)) == sizeof(mb) && mb.Type == MEM_IMAGE && mb.AllocationBase) {
        wchar_t path[MAX_PATH] = {};
        const char* name = "?";
        char        nm[MAX_PATH];
        if (g_mapped_name && g_mapped_name(GetCurrentProcess(), mb.AllocationBase, path, MAX_PATH)) {
            const wchar_t* b = wcsrchr(path, L'\\');
            WideCharToMultiByte(CP_UTF8, 0, b ? b + 1 : path, -1, nm, sizeof(nm), nullptr, nullptr);
            name = nm;
        }
        std::snprintf(out, cap, "%s+0x%llx", name,
                      static_cast<unsigned long long>(pc - reinterpret_cast<uint64_t>(mb.AllocationBase)));
        return;
    }
    std::snprintf(out, cap, "0x%llx", static_cast<unsigned long long>(pc));
}

void WriteStack(HANDLE f, const char* title, const uint64_t* pcs, int n)
{
    WriteLine(f, "\r\n%s: the instruction pointer, then the return addresses found on it, innermost first (%d):\r\n", title, n);
    if (n == 0) WriteLine(f, "  (could not be read)\r\n");
    for (int i = 0; i < n; ++i) {
        char where[MAX_PATH + 32];
        ModuleOffset(pcs[i], where, sizeof(where));
        WriteLine(f, "  %2d  %s\r\n", i, where);
    }
}

// Keeps the newest kKeepReports freeze files.
void PruneReports()
{
    wchar_t pat[MAX_PATH + 32];
    _snwprintf(pat, MAX_PATH + 32, L"%ls\\freeze-2*.txt", g_dir);
    pat[MAX_PATH + 31] = L'\0';
    std::wstring    names[64];
    int             n = 0;
    WIN32_FIND_DATAW fd;
    HANDLE           h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (n < 64) names[n++] = fd.cFileName;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    if (n <= kKeepReports) return;
    std::sort(names, names + n);
    for (int i = 0; i < n - kKeepReports; ++i) DeleteFileW((std::wstring(g_dir) + L"\\" + names[i]).c_str());
}

// The newest report and whether it is newer than the one last shown (freeze-seen.txt holds its name).
void ReadLastSession()
{
    wchar_t pat[MAX_PATH + 32];
    _snwprintf(pat, MAX_PATH + 32, L"%ls\\freeze-2*.txt", g_dir);
    pat[MAX_PATH + 31] = L'\0';
    WIN32_FIND_DATAW fd;
    HANDLE           h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    std::wstring newest;
    do {
        if (newest.empty() || std::wstring(fd.cFileName) > newest) newest = fd.cFileName;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    if (newest.empty()) return;
    const std::wstring seen_path = std::wstring(g_dir) + L"\\freeze-seen.txt";
    char               seen[MAX_PATH * 3] = {};
    HANDLE             s = CreateFileW(seen_path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (s != INVALID_HANDLE_VALUE) {
        DWORD got = 0;
        ReadFile(s, seen, sizeof(seen) - 1, &got, nullptr);
        seen[got] = '\0';
        CloseHandle(s);
    }
    char name[MAX_PATH * 3] = {};
    WideCharToMultiByte(CP_UTF8, 0, newest.c_str(), -1, name, sizeof(name), nullptr, nullptr);
    if (std::strcmp(seen, name) >= 0) return;  // shown already (names sort by time)
    HANDLE f = CreateFileW((std::wstring(g_dir) + L"\\" + newest).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    std::string text(256 * 1024, '\0');
    DWORD       got = 0;
    ReadFile(f, &text[0], static_cast<DWORD>(text.size()), &got, nullptr);
    CloseHandle(f);
    text.resize(got);
    g_pending = std::string("=== RAV freeze report from the last session: ") + name + " ===\r\n" + text +
                "\r\n=== end of the freeze report ===\r\n\r\n";
    char dir[MAX_PATH * 3] = {};
    WideCharToMultiByte(CP_UTF8, 0, g_dir, -1, dir, sizeof(dir), nullptr, nullptr);
    g_pending_path = std::string(dir) + "\\" + name;
    g_toast = text.find("Recovered after") == std::string::npos && text.find("RAV unloaded") == std::string::npos;
    s = CreateFileW(seen_path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (s != INVALID_HANDLE_VALUE) {
        DWORD put = 0;
        WriteFile(s, name, static_cast<DWORD>(std::strlen(name)), &put, nullptr);
        CloseHandle(s);
    }
}

DWORD WINAPI WatchLoop(LPVOID)
{
    HANDLE   report = INVALID_HANDLE_VALUE;
    int64_t  stalled_beat = 0, first_ms = 0, last_wake = NowMs(), grace_until = 0;
    bool     second_done = false;
    uint64_t pcs1[kMaxFrames], pcs2[kMaxFrames];
    int      n1 = 0, written = 0;
    uint64_t cpu1 = 0;
    while (WaitForSingleObject(g_stop, static_cast<DWORD>(kWakeMs)) == WAIT_TIMEOUT) {
        const int64_t now = NowMs();
        if (now - last_wake > 5000) grace_until = now + kStallMs;  // the PC slept: not a stall
        last_wake = now;
        if (!g_armed.load(std::memory_order_relaxed)) continue;
        const int64_t beat = g_main_beat.load(std::memory_order_relaxed);
        if (report != INVALID_HANDLE_VALUE) {
            if (beat != stalled_beat) {
                WriteLine(report, "\r\nRecovered after %.1f s (the main thread beat again).\r\n", Secs(now - stalled_beat));
                CloseHandle(report);
                report = INVALID_HANDLE_VALUE;
                continue;
            }
            if (!second_done && now - first_ms >= kSecondSampleMs) {
                second_done = true;
                const uint64_t cpu2 = ThreadCpu100ns(g_main);
                const int      n2 = SampleMainStack(pcs2, kMaxFrames);
                WriteStack(report, "Main thread's stack, second sample", pcs2, n2);
                // The return addresses (the instruction pointer aside: a loop moves it).
                const bool same = n1 == n2 && n1 > 1 && std::memcmp(pcs1 + 1, pcs2 + 1, sizeof(uint64_t) * static_cast<size_t>(n1 - 1)) == 0;
                const double cpu_s = static_cast<double>(cpu2 - cpu1) / 1e7, wall_s = Secs(now - first_ms);
                WriteLine(report, "\r\nBetween the samples (%.1f s): the main thread used %.1f s of CPU; its stack %s.\r\n", wall_s, cpu_s,
                          same ? "is the same" : "changed");
                if (cpu_s >= 0.5 * wall_s)
                    WriteLine(report, "Verdict: it kept running (an endless loop or a very long job)%s.\r\n",
                              same ? ", in one place" : "");
                else
                    WriteLine(report, "Verdict: it was waiting (a deadlock, or a blocking call: disk, network, another thread).\r\n");
            }
            continue;
        }
        if (now < grace_until || now - beat < kStallMs) continue;
        if (written >= kKeepReports) continue;  // a session of long REAPER jobs: at most kKeepReports reports
        ++written;
        // A stall: the report's text first (so a kill right after still leaves it), then the stack.
        char    when[32];
        wchar_t stem[64];
        LocalStamp(when, sizeof(when), stem, 64);
        report = OpenReport(stem);
        if (report == INVALID_HANDLE_VALUE) {  // no folder: try again at the next stall
            grace_until = now + kStallMs;
            continue;
        }
        stalled_beat = beat;
        first_ms = now;
        second_done = false;
        ReportHead h;
        h.version = g_version;
        h.when = when;
        h.now_ms = now;
        h.stall_ms = now - beat;
        WriteText(report, g_text, FormatReport(g_text, sizeof(g_text), h));
        cpu1 = ThreadCpu100ns(g_main);
        n1 = SampleMainStack(pcs1, kMaxFrames);
        WriteStack(report, "Main thread's stack, first sample", pcs1, n1);
        // The audio / video thread when it is the one stuck inside RAV (a blocked file read, or a loop in our mixer).
        const int64_t t_now = NowMs();
        if (g_audio_depth.load(std::memory_order_relaxed) > 0 && t_now - g_audio_in.load(std::memory_order_relaxed) > 2000) {
            uint64_t pa[kMaxFrames];
            const int na = SampleThreadStack(g_audio_tid.load(std::memory_order_relaxed), pa, kMaxFrames);
            WriteStack(report, "Audio thread's stack (it has been inside RAV's source for over 2 s)", pa, na);
        }
        if (g_video_depth.load(std::memory_order_relaxed) > 0 && t_now - g_video_in.load(std::memory_order_relaxed) > 2000) {
            uint64_t pv[kMaxFrames];
            const int nv = SampleThreadStack(g_video_tid.load(std::memory_order_relaxed), pv, kMaxFrames);
            WriteStack(report, "Video thread's stack (it has been inside RAV's video frame for over 2 s)", pv, nv);
        }
        WriteLine(report, "\r\n(a second sample follows in %d s, if REAPER is still running)\r\n", static_cast<int>(kSecondSampleMs / 1000));
    }
    if (report != INVALID_HANDLE_VALUE) {
        WriteLine(report, "\r\nRAV unloaded while it was stalled.\r\n");
        CloseHandle(report);
    }
    return 0;
}

}  // namespace

void StartWatchdog(const char* resource_path, const char* version)
{
    if (g_thread) return;
    SetMainThread();
    std::snprintf(g_version, sizeof(g_version), "%s", version ? version : "");
    wchar_t res[MAX_PATH] = {};
    if (!resource_path || !MultiByteToWideChar(CP_UTF8, 0, resource_path, -1, res, MAX_PATH)) return;
    _snwprintf(g_dir, MAX_PATH, L"%ls\\RAV", res);
    g_dir[MAX_PATH - 1] = L'\0';
    if (HMODULE k = GetModuleHandleW(L"kernel32.dll"))
        g_mapped_name = reinterpret_cast<MappedFileNameFn>(reinterpret_cast<void*>(GetProcAddress(k, "K32GetMappedFileNameW")));
    ReadLastSession();
    PruneReports();
    g_main = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, GetCurrentThreadId());
    g_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!g_stop) return;
    g_main_beat.store(NowMs());
    g_thread = CreateThread(nullptr, 0, &WatchLoop, nullptr, 0, nullptr);
}

void StopWatchdog()
{
    if (g_thread) {
        SetEvent(g_stop);
        if (WaitForSingleObject(g_thread, 5000) != WAIT_OBJECT_0) {
            // Stuck (a report on a dead network folder): it must never run in an unmapped DLL nor on closed
            // handles, so the DLL stays loaded for the rest of the process and its handles stay open.
            HMODULE self = nullptr;
            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                               reinterpret_cast<LPCWSTR>(&StopWatchdog), &self);
            g_thread = nullptr;
            g_stop = nullptr;
            g_main = nullptr;
            return;
        }
        CloseHandle(g_thread);
        g_thread = nullptr;
    }
    if (g_stop) CloseHandle(g_stop);
    g_stop = nullptr;
    if (g_main) CloseHandle(g_main);
    g_main = nullptr;
}

const std::string& FreezeReportFromLastSession() { return g_pending; }
const std::string& FreezeReportPath() { return g_pending_path; }

bool TakeFreezeToast()
{
    const bool t = g_toast;
    g_toast = false;
    return t;
}

#else  // not Windows (host builds)

void StartWatchdog(const char*, const char*) { SetMainThread(); }
void StopWatchdog() {}
const std::string& FreezeReportFromLastSession()
{
    static const std::string none;
    return none;
}
const std::string& FreezeReportPath() { return FreezeReportFromLastSession(); }
bool TakeFreezeToast() { return false; }

#endif

}  // namespace wd
}  // namespace rav
