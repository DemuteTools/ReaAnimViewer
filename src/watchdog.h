// SPDX-License-Identifier: MIT
//
// The freeze watchdog (freeze audit, 2026-10-18: "sometimes the tool freezes and never recovers, we have to kill
// REAPER; no repro"). RAV leaves breadcrumbs as it runs (what it starts, with a short argument: a Sound, a file, a
// command id) and keeps the main thread's stack of RAV scopes; the main thread beats a heartbeat (a REAPER timer and
// each viewer frame), the audio thread marks its entry / exit in RAV's sources, the workers their state. A watchdog
// thread looks every 500 ms: no main heartbeat for 10 s = one report <resource>\RAV\freeze-YYYYMMDD-HHMMSS.txt (the
// stall, the scope stack, the last 256 crumbs, the audio thread and the workers, the main thread's native stack as
// module+offset; a second stack sample 20 s later tells an endless loop from a deadlock; "recovered after X s" when
// the heartbeat returns). The next load points to it (a toast) and Copy error log prepends it.
//
// Cost when nothing hangs: a crumb is a few stores into a fixed ring (no allocation, no lock), a scope one more push.
// The watchdog never calls the REAPER API (the resource path is read at load). Host builds (RAV_HOST_TEST) compile
// the macros to nothing, except the watchdog's own host test (RAV_WATCHDOG_HOST).

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace rav {
namespace wd {

constexpr int kCrumbs = 256;
constexpr int kArgLen = 56;
constexpr int kScopeDepth = 16;
constexpr int kWorkers = 8;

// ---- what the code leaves (any thread unless said) ----

// A breadcrumb: `what` must be a string that lives for ever (a literal); `arg` is copied (cut to 55 bytes).
void Crumb(const char* what, const char* arg = nullptr);
inline void Crumb(const char* what, const std::string& arg) { Crumb(what, arg.c_str()); }
void CrumbNum(const char* what, long long n);
// The end of a file path that fits an argument (its file name, the folders before it as room allows): the
// pointer into `path` itself, no copy. Freeze fix #1: the crumbs of the file loads name the file.
inline const char* PathTail(const std::string& path)
{
    const size_t keep = static_cast<size_t>(kArgLen) - 1;
    return path.size() > keep ? path.c_str() + (path.size() - keep) : path.c_str();
}
inline void Crumb(const char* what, long long n) { CrumbNum(what, n); }
inline void Crumb(const char* what, int n) { CrumbNum(what, n); }
inline void Crumb(const char* what, size_t n) { CrumbNum(what, static_cast<long long>(n)); }

// A crumb on entry; on the main thread also on its scope stack (16 levels kept) until it ends. A Quiet one leaves
// no crumb (what runs every frame or every tick: a panel's draw, a timer; the ring keeps the events).
struct Quiet {};
class Scope {
public:
    Scope(Quiet, const char* what, const char* arg) { Enter(what, arg, false); }
    Scope(Quiet, const char* what, const std::string& arg) { Enter(what, arg.c_str(), false); }
    Scope(Quiet, const char* what, long long n) { EnterNum(what, n, false); }
    Scope(Quiet, const char* what, int n) { EnterNum(what, n, false); }
    Scope(Quiet, const char* what, size_t n) { EnterNum(what, static_cast<long long>(n), false); }
    explicit Scope(const char* what, const char* arg = nullptr);
    Scope(const char* what, const std::string& arg) : Scope(what, arg.c_str()) {}
    Scope(const char* what, long long n);
    Scope(const char* what, int n) : Scope(what, static_cast<long long>(n)) {}
    Scope(const char* what, size_t n) : Scope(what, static_cast<long long>(n)) {}
    ~Scope();
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

private:
    void Enter(const char* what, const char* arg, bool crumb = true);
    void EnterNum(const char* what, long long n, bool crumb);
    bool pushed_ = false;
};

// The audio thread inside one of RAV's sources (`what`: a literal), and out of it.
void AudioEnter(const char* what);
void AudioExit();
// The audio thread inside one of RAV's sources for the lifetime of this (every return path).
struct AudioScope {
    explicit AudioScope(const char* what) { AudioEnter(what); }
    ~AudioScope() { AudioExit(); }
    AudioScope(const AudioScope&) = delete;
    AudioScope& operator=(const AudioScope&) = delete;
};

// The main thread is alive (a REAPER timer tick, a viewer frame).
void MainBeat();
// A worker thread's slot (`name` a literal, `index` for several of one kind); -1 when all are taken.
int  WorkerSlot(const char* name, int index = 0);
// Its state: busy with `what` (a literal) on `arg` (copied), or idle.
void WorkerBusy(int slot, const char* what, const char* arg = nullptr);
inline void WorkerBusy(int slot, const char* what, const std::string& arg) { WorkerBusy(slot, what, arg.c_str()); }
void WorkerIdle(int slot);

// ---- the watchdog (plugin load / unload, main thread) ----

// Starts the watchdog thread (Windows; nothing elsewhere). `resource_path`: REAPER's resource folder (UTF-8), the
// reports go to <it>\RAV. Also reads the report the last session left (FreezeReportFromLastSession).
void StartWatchdog(const char* resource_path, const char* version);
// Stops it (first thing at unload): the thread joined.
void StopWatchdog();
// The newest report written since the one last shown ("" when none): its file name + text. Main thread.
const std::string& FreezeReportFromLastSession();
// That report's full path (UTF-8), "" when none. Main thread.
const std::string& FreezeReportPath();
// True once, when that report never recovered (REAPER was killed): the load's toast. Main thread.
bool TakeFreezeToast();

// ---- platform-neutral parts (also for the host test) ----

int64_t NowMs();
uint32_t CurrentThreadId();
// The calling thread is the main one (StartWatchdog does it; the host test too).
void SetMainThread();

struct CrumbView {
    uint64_t    seq = 0;
    const char* what = nullptr;
    char        arg[kArgLen] = {};
    int64_t     ms = 0;
    uint32_t    tid = 0;
};
// The crumbs kept, oldest first (at most kCrumbs); returns how many were copied into `out`.
int SnapshotCrumbs(CrumbView* out, int cap);

struct ScopeView {
    const char* what = nullptr;
    char        arg[kArgLen] = {};
    int64_t     ms = 0;
};
// The main thread's scopes, outermost first; `*depth` = the real depth (may pass kScopeDepth).
int SnapshotScopes(ScopeView* out, int cap, int* depth);

struct ReportHead {
    const char* version = "";
    const char* when = "";     // local time, "2026-10-18 14:03:22"
    int64_t     now_ms = 0;    // NowMs() at the report
    int64_t     stall_ms = 0;  // since the last main heartbeat
};
// The report's text up to the native stacks (header, scope stack, audio thread, workers, crumbs with their ages),
// into `buf` (no allocation); returns its length (cut at cap - 1).
size_t FormatReport(char* buf, size_t cap, const ReportHead& h);

// Test hook: everything back to empty.
void ResetForTest();

}  // namespace wd
}  // namespace rav

// The hooks. Host builds of RAV's shared sources compile them to nothing (their tests link no watchdog).
#if defined(RAV_HOST_TEST) && !defined(RAV_WATCHDOG_HOST)
#define RAV_CRUMB(what, arg) ((void)0)
#define RAV_SCOPE(what, arg) ((void)0)
#define RAV_QUIET_SCOPE(what, arg) ((void)0)
#define RAV_MAIN_BEAT() ((void)0)
#define RAV_AUDIO_SCOPE(what) ((void)0)
#define RAV_WORKER_SLOT(name, index) (-1)
#define RAV_WORKER_BUSY(slot, what, arg) ((void)0)
#define RAV_WORKER_IDLE(slot) ((void)0)
#else
#define RAV_WD_CAT2(a, b) a##b
#define RAV_WD_CAT(a, b) RAV_WD_CAT2(a, b)
#define RAV_CRUMB(what, arg) ::rav::wd::Crumb(what, arg)
#define RAV_SCOPE(what, arg) const ::rav::wd::Scope RAV_WD_CAT(rav_wd_scope_, __COUNTER__)(what, arg)
#define RAV_QUIET_SCOPE(what, arg) const ::rav::wd::Scope RAV_WD_CAT(rav_wd_scope_, __COUNTER__)(::rav::wd::Quiet{}, what, arg)
#define RAV_MAIN_BEAT() ::rav::wd::MainBeat()
#define RAV_AUDIO_SCOPE(what) const ::rav::wd::AudioScope RAV_WD_CAT(rav_wd_audio_, __COUNTER__)(what)
#define RAV_WORKER_SLOT(name, index) ::rav::wd::WorkerSlot(name, index)
#define RAV_WORKER_BUSY(slot, what, arg) ::rav::wd::WorkerBusy(slot, what, arg)
#define RAV_WORKER_IDLE(slot) ::rav::wd::WorkerIdle(slot)
#endif
