// SPDX-License-Identifier: MIT
//
// See sound_audition.h. The mixing source (AuditionMix) is read on REAPER's audio thread: its
// voices are fixed once it plays and freed only after StopPreview has returned.

#include "sound_audition.h"
#include "watchdog.h"  // freeze audit: breadcrumbs and scopes

#ifdef _WIN32

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <memory>
#include <set>
#include <string>

#include "console_log.h"
#include "library_media_ui.h"
#include "reaper_api.h"
#include "sound_audition_plan.h"

namespace rav {
namespace {

using CreateFromFileFn = PCM_source* (*)(const char*);
using PreviewFn = int (*)(preview_register_t*);
using PreviewExFn = int (*)(preview_register_t*, int, double);

CreateFromFileFn f_create = nullptr;
PreviewFn        f_play = nullptr;
PreviewExFn      f_play_ex = nullptr;
PreviewFn        f_stop = nullptr;

struct Voice {
    std::unique_ptr<PCM_source> src;
    std::string                 path;  // for the freeze report only
    double                      start = 0.0, length = 0.0, delay = 0.0, gain = 1.0;
    double                      fade_in = 0.0, fade_out = 0.0;  // s (FadeGainAt)
    int                         in_shape = 0, out_shape = 0;
    double                      in_dir = NAN, out_dir = NAN;        // 13-75
    double                      fade_from = 0.0, fade_span = 0.0;  // 13-29: a window of a longer stretch
    MonoMode                    mono = MonoMode::Off;               // 13-41
};

class AuditionMix : public PCM_source {
public:
    // Fix 2026-10-19 (Loop not regular): `loop` / `loop_s` > 0: after the head (`head_s` long) the loop pass repeats,
    // wrapped here at the sample (LoopSegAt), not restarted from the UI.
    explicit AuditionMix(std::vector<Voice> v, std::vector<Voice> loop = {}, double head_s = 0.0, double loop_s = 0.0)
        : voices_(std::move(v)), loop_(std::move(loop))
    {
        for (const Voice& x : voices_) len_ = std::max(len_, x.delay + x.length);
        for (const std::vector<Voice>* set : {&voices_, &loop_})
            for (const Voice& x : *set) {
                nch_ = std::max(nch_, std::min(2, x.src->GetNumChannels()));
                if (sr_ < 1.0) sr_ = x.src->GetSampleRate();
            }
        if (nch_ < 1) nch_ = 1;
        if (!loop_.empty() && loop_s > 0.0 && std::isfinite(loop_s) && std::isfinite(head_s)) {  // freeze audit #6
            head_s_ = std::max(head_s, 0.0);
            loop_s_ = loop_s;
        }
        scratch_.resize(16384 * 2);  // a usual block: no allocation on the audio thread
    }
    // Freeze fix (audit #1): REAPER buffers a preview on its own thread and asks for a copy of the source: each voice's
    // file source is duplicated (nullptr when one cannot be: the preview then plays unbuffered, as before).
    PCM_source* Duplicate() override
    {
        std::vector<Voice> a, b;
        if (!CloneVoices(voices_, &a) || !CloneVoices(loop_, &b)) return nullptr;
        return new AuditionMix(std::move(a), std::move(b), head_s_, loop_s_);
    }
    bool        IsAvailable() override { return true; }
    const char* GetType() override { return "RAV_AUDITION"; }
    bool        SetFileName(const char*) override { return false; }
    int         GetNumChannels() override { return nch_; }
    double      GetSampleRate() override { return sr_ > 1.0 ? sr_ : 44100.0; }
    double      GetLength() override { return Loops() ? kLoopLen : len_; }  // looping: plays until stopped
    int         PropertiesWindow(HWND) override { return 0; }
    void        GetPeakInfo(PCM_source_peaktransfer_t*) override {}
    void        SaveState(ProjectStateContext*) override {}
    int         LoadState(const char*, ProjectStateContext*) override { return -1; }
    void        Peaks_Clear(bool) override {}
    int         PeaksBuild_Begin() override { return 0; }
    int         PeaksBuild_Run() override { return 0; }
    void        PeaksBuild_Finish() override {}

    static constexpr double kLoopLen = 86400.0 * 365.0;
    bool   Loops() const { return loop_s_ > 0.0; }
    double HeadS() const { return head_s_; }
    double LoopS() const { return loop_s_; }

    void GetSamples(PCM_source_transfer_t* block) override
    {
        RAV_AUDIO_SCOPE("AuditionMix");  // freeze audit
        if (!block || !block->samples || block->length <= 0 || block->nch <= 0 || !(block->samplerate > 0.0)) {
            if (block) block->samples_out = 0;
            return;
        }
        const int    n = block->length, nch = block->nch;
        const double sr = block->samplerate;
        std::fill(block->samples, block->samples + static_cast<size_t>(n) * static_cast<size_t>(nch), 0.0);
        if (scratch_.size() < static_cast<size_t>(n) * static_cast<size_t>(nch))
            scratch_.resize(static_cast<size_t>(n) * static_cast<size_t>(nch));
        if (!Loops()) {
            Mix(voices_, block->time_s, n, sr, nch, block->samples);
        } else {
            for (int done = 0; done < n;) {  // the head, then the loop pass from its start each time it ends
                const LoopSeg g = LoopSegAt(block->time_s + done / sr, n - done, sr, head_s_, loop_s_);
                Mix(g.body ? loop_ : voices_, g.t, g.n, sr, nch, block->samples + static_cast<size_t>(done) * static_cast<size_t>(nch));
                if (g.n <= 0) break;  // freeze audit #6: no progress would hold the audio thread (and the preview's lock) for ever
                done += g.n;
            }
        }
        block->samples_out = n;
    }

private:
    static bool CloneVoices(const std::vector<Voice>& from, std::vector<Voice>* to)
    {
        for (const Voice& v : from) {
            PCM_source* d = v.src ? v.src->Duplicate() : nullptr;
            if (!d) return false;
            Voice c;
            c.src.reset(d);
            c.path = v.path;
            c.start = v.start; c.length = v.length; c.delay = v.delay; c.gain = v.gain;
            c.fade_in = v.fade_in; c.fade_out = v.fade_out;
            c.in_shape = v.in_shape; c.out_shape = v.out_shape;
            c.in_dir = v.in_dir; c.out_dir = v.out_dir;
            c.fade_from = v.fade_from; c.fade_span = v.fade_span;
            c.mono = v.mono;
            to->push_back(std::move(c));
        }
        return true;
    }

    // `n` samples of the voices from mix time t0, added into `outp` (nch interleaved).
    void Mix(std::vector<Voice>& set, double t0, int n, double sr, int nch, ReaSample* outp)
    {
        const double t1 = t0 + n / sr;
        for (Voice& v : set) {
            const double a = std::max(t0, v.delay), b = std::min(t1, v.delay + v.length);
            if (b <= a) continue;
            const int off = static_cast<int>(std::floor((a - t0) * sr + 0.5));
            const int cnt = std::min(n - off, static_cast<int>(std::floor((b - a) * sr + 0.5)));
            if (off < 0 || cnt <= 0) continue;
            PCM_source_transfer_t tr = {};
            tr.time_s = v.start + (a - v.delay);
            tr.samplerate = sr;
            tr.nch = nch;
            tr.length = cnt;
            tr.samples = scratch_.data();
            RAV_AUDIO_ARG(v.path.c_str());  // freeze audit: the report names the file a stalled read is in
            v.src->GetSamples(&tr);
            const int    got = std::min(cnt, std::max(0, tr.samples_out));
            const double t_voice = a - v.delay + v.fade_from;  // the first sample's time into the faded stretch
            const double span = v.fade_span > 0.0 ? v.fade_span : v.length;
            const bool   fades = v.fade_in > 0.0 || v.fade_out > 0.0;
            const bool   mono = v.mono != MonoMode::Off && nch >= 2;  // 13-41: one channel or L + R, on both
            for (int k = 0; k < got; ++k) {
                const double g = fades ? v.gain * FadeGainAt(t_voice + k / sr, span, v.fade_in, v.fade_out,
                                                             v.in_shape, v.out_shape, v.in_dir, v.out_dir)
                                       : v.gain;
                const ReaSample* in = &scratch_[static_cast<size_t>(k * nch)];
                ReaSample*       out = &outp[static_cast<size_t>((off + k) * nch)];
                if (mono) {
                    const double m = MonoOf(in[0], in[1], v.mono) * g;
                    for (int c = 0; c < nch; ++c) out[c] += m;
                } else {
                    for (int c = 0; c < nch; ++c) out[c] += in[c] * g;
                }
            }
        }
    }

    std::vector<Voice>     voices_;
    std::vector<Voice>     loop_;  // fix 2026-10-19: the loop pass (empty = no loop)
    std::vector<ReaSample> scratch_;
    double                 len_ = 0.0, sr_ = 0.0;
    double                 head_s_ = 0.0, loop_s_ = 0.0;
    int                    nch_ = 0;
};

struct Playing {
    bool                         active = false;
    preview_register_t           reg = {};
    std::unique_ptr<AuditionMix> mix;
    std::string                  tag;
    double                       last_pos = -1.0;  // AuditionPlaying's end watch
    double                       moved_s = 0.0;
    double                       last_raw = 0.0;  // RawPosition's last read (kept while the preview's lock is busy)
};
Playing     g_play;

// Freeze fix (audit #1): a play whose files are not known reachable yet. The probe worker asks (a stalled share or an
// offline cloud drive never blocks the UI); the play starts from AuditionPlaying once they all answered (the callers
// poll it every frame), or is dropped after kPendGiveUpS.
struct Pending {
    bool                  active = false;
    bool                  looped = false;
    std::vector<PlayVoice> head, loop;
    double                head_s = 0.0, loop_s = 0.0;
    std::string           tag;
    double                t0 = 0.0;
};
Pending           g_pend;
constexpr double  kPendGiveUpS = 6.0;
std::string g_empty;

double NowS()
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

void InitReg(preview_register_t& r, PCM_source* src)
{
    InitializeCriticalSection(&r.cs);
    r.src = src;
    r.m_out_chan = 0;
    r.curpos = 0.0;
    r.loop = false;
    r.volume = 1.0;
    r.peakvol[0] = r.peakvol[1] = 0.0;
    r.preview_track = nullptr;
}

}  // namespace

void InitSoundAudition(void* (*get_func)(const char* name))
{
    if (!get_func) return;
    f_create = reinterpret_cast<CreateFromFileFn>(get_func("PCM_Source_CreateFromFile"));
    f_play = reinterpret_cast<PreviewFn>(get_func("PlayPreview"));
    f_play_ex = reinterpret_cast<PreviewExFn>(get_func("PlayPreviewEx"));
    f_stop = reinterpret_cast<PreviewFn>(get_func("StopPreview"));
}

void StopAudition()
{
    g_pend.active = false;  // a play still waiting for its files is cancelled too
    if (!g_play.active) return;
    RAV_CRUMB("StopAudition", nullptr);  // freeze audit
    if (f_stop) f_stop(&g_play.reg);
    DeleteCriticalSection(&g_play.reg.cs);
    g_play.reg = preview_register_t{};
    g_play.mix.reset();  // after StopPreview: the audio thread no longer reads it
    g_play.tag.clear();
    g_play.active = false;
}

namespace {

// The voices read (a file that cannot be opened, or with nothing left past its start, is dropped; `used` = the indices
// kept).
std::vector<Voice> OpenVoices(const std::vector<PlayVoice>& voices, std::vector<int>* used)
{
    std::vector<Voice> vs;
    for (size_t pi = 0; pi < voices.size(); ++pi) {
        const PlayVoice& p = voices[pi];
        if (p.path.empty()) continue;
        if (MediaReachGet(p.path) == 2) continue;  // asked and not reachable: never opened on this thread
        RAV_CRUMB("OpenVoice", wd::PathTail(p.path));  // freeze audit: a stalled open names its file
        PCM_source* s = f_create(p.path.c_str());
        if (!s) continue;
        if (s->GetSampleRate() < 1.0 || s->GetNumChannels() < 1) {
            delete s;
            continue;
        }
        Voice v;
        v.src.reset(s);
        v.path = p.path;
        v.start = std::max(0.0, p.start_s);
        const double rest = std::max(0.0, s->GetLength() - v.start);
        v.length = p.length_s > 0.0 ? std::min(p.length_s, rest) : rest;
        v.delay = std::max(0.0, p.delay_s);
        v.gain = p.gain;
        v.fade_in = std::max(0.0, p.fade_in_s);
        v.fade_out = std::max(0.0, p.fade_out_s);
        v.in_shape = p.fade_in_shape;
        v.out_shape = p.fade_out_shape;
        v.in_dir = p.fade_in_dir;
        v.out_dir = p.fade_out_dir;
        v.fade_from = std::max(0.0, p.fade_from_s);
        v.fade_span = std::max(0.0, p.fade_span_s);
        v.mono = p.mono;
        if (v.length > 0.0) {
            vs.push_back(std::move(v));
            if (used) used->push_back(static_cast<int>(pi));
        }
    }
    return vs;
}

bool StartMix(std::unique_ptr<AuditionMix> mix, const std::string& tag, std::string* why)
{
    g_play.mix = std::move(mix);
    InitReg(g_play.reg, g_play.mix.get());
    g_play.active = true;
    g_play.tag = tag;
    g_play.last_pos = -1.0;
    g_play.last_raw = 0.0;
    g_play.moved_s = NowS();
    // Buffered (REAPER reads the files on its own thread, never the audio thread); unbuffered if it cannot.
    const bool started = (f_play_ex && f_play_ex(&g_play.reg, 1, -1.0) != 0) || f_play(&g_play.reg) != 0;
    if (!started) {
        // Not started: nothing reads the source, free it at once.
        DeleteCriticalSection(&g_play.reg.cs);
        g_play.reg = preview_register_t{};
        g_play.mix.reset();
        g_play.active = false;
        g_play.tag.clear();
        if (why) *why = "REAPER refused the preview";
        return false;
    }
    return true;
}

double RawPosition()
{
    if (!g_play.active) return 0.0;
    // Never wait on the preview's lock (freeze audit #1): a busy one keeps the last position read.
    if (TryEnterCriticalSection(&g_play.reg.cs)) {
        g_play.last_raw = g_play.reg.curpos;
        LeaveCriticalSection(&g_play.reg.cs);
    }
    return g_play.last_raw;
}

}  // namespace

namespace {

bool PlayVoicesNow(const std::vector<PlayVoice>& voices, const std::string& tag, std::string* why, std::vector<int>* kept)
{
    std::vector<int>   used;
    std::vector<Voice> vs = OpenVoices(voices, &used);
    if (vs.empty()) {
        if (why) *why = "no sound file could be read";
        return false;
    }
    if (!StartMix(std::make_unique<AuditionMix>(std::move(vs)), tag, why)) return false;
    if (kept) *kept = std::move(used);
    return true;
}

bool PlayLoopedNow(const std::vector<PlayVoice>& head, double head_s, const std::vector<PlayVoice>& loop, double loop_s,
                   const std::string& tag)
{
    std::vector<Voice> hv = OpenVoices(head, nullptr);
    std::vector<Voice> lv = OpenVoices(loop, nullptr);
    if (lv.empty() || !(loop_s > 0.0)) return false;
    return StartMix(std::make_unique<AuditionMix>(std::move(hv), std::move(lv), head_s, loop_s), tag, nullptr);
}

// True when every file of the sets has been answered (the unanswered ones are asked, once per call).
bool AllReachKnown(const std::vector<PlayVoice>& a, const std::vector<PlayVoice>& b, bool ask)
{
    bool known = true;
    std::set<std::string> seen;
    for (const std::vector<PlayVoice>* set : {&a, &b})
        for (const PlayVoice& v : *set) {
            if (v.path.empty() || !seen.insert(v.path).second) continue;
            if (MediaReachGet(v.path) != 0) continue;
            known = false;
            if (ask) MediaReachAsk(v.path);
        }
    return known;
}

void PumpPending()
{
    if (!g_pend.active) return;
    MediaReachPoll();
    if (!AllReachKnown(g_pend.head, g_pend.loop, false)) {
        if (NowS() - g_pend.t0 > kPendGiveUpS) {
            LogWarn("audition: the sound files did not answer in %.0f s; not played", kPendGiveUpS);
            g_pend.active = false;
        }
        return;
    }
    Pending p = std::move(g_pend);  // moved out first: starting the play cancels a pending one
    g_pend = Pending{};
    if (p.looped) PlayLoopedNow(p.head, p.head_s, p.loop, p.loop_s, p.tag);
    else PlayVoicesNow(p.head, p.tag, nullptr, nullptr);
}

}  // namespace

bool PlayVoices(const std::vector<PlayVoice>& voices, const std::string& tag, std::string* why, std::vector<int>* kept)
{
    RAV_SCOPE("PlayVoices", tag);  // freeze audit
    if (kept) kept->clear();
    StopAudition();
    StopMediaPreview();  // a tree audition and a Media Explorer preview never sound together (13-7c)
    if (!f_play || !f_stop || !f_create) {
        if (why) *why = "REAPER's preview is not available";
        return false;
    }
    if (!AllReachKnown(voices, {}, true)) {  // the files are being asked: the play starts when they answer
        g_pend = Pending{};
        g_pend.active = true;
        g_pend.head = voices;
        g_pend.tag = tag;
        g_pend.t0 = NowS();
        if (kept)
            for (size_t i = 0; i < voices.size(); ++i)
                if (!voices[i].path.empty()) kept->push_back(static_cast<int>(i));
        return true;
    }
    return PlayVoicesNow(voices, tag, why, kept);
}

bool PlayVoicesLooped(const std::vector<PlayVoice>& head, double head_s, const std::vector<PlayVoice>& loop, double loop_s,
                      const std::string& tag)
{
    RAV_SCOPE("PlayVoicesLooped", tag);  // freeze audit
    StopAudition();
    StopMediaPreview();
    if (!f_play || !f_stop || !f_create) return false;
    if (loop.empty() || !(loop_s > 0.0)) return false;
    if (!AllReachKnown(head, loop, true)) {
        g_pend = Pending{};
        g_pend.active = true;
        g_pend.looped = true;
        g_pend.head = head;
        g_pend.loop = loop;
        g_pend.head_s = head_s;
        g_pend.loop_s = loop_s;
        g_pend.tag = tag;
        g_pend.t0 = NowS();
        return true;
    }
    return PlayLoopedNow(head, head_s, loop, loop_s, tag);
}

int AuditionLoopPass()
{
    if (!g_play.active || !g_play.mix->Loops()) return 0;
    return LoopedPass(RawPosition(), g_play.mix->HeadS(), g_play.mix->LoopS());
}

bool AuditionFile(const std::string& path, double start_s, double length_s, const std::string& tag, double gain,
                  double fade_in_s, double fade_out_s, int fade_in_shape, int fade_out_shape)
{
    PlayVoice v;
    v.path = path;
    v.start_s = start_s;
    v.length_s = length_s;
    v.gain = gain;
    v.fade_in_s = fade_in_s;
    v.fade_out_s = fade_out_s;
    v.fade_in_shape = fade_in_shape;
    v.fade_out_shape = fade_out_shape;
    return PlayVoices({v}, tag, nullptr);
}

double AuditionPosition()
{
    if (!g_play.active) return 0.0;
    const double p = RawPosition();
    return g_play.mix->Loops() ? LoopedPosition(p, g_play.mix->HeadS(), g_play.mix->LoopS()) : p;  // in the pass
}

bool AuditionPlaying()
{
    PumpPending();
    if (g_pend.active) return true;  // waiting for its files: it is "playing" (not ended) for its callers
    if (!g_play.active) return false;
    const double    pos = RawPosition();  // the mix's own time (a self-looping one keeps moving)
    const double    now = NowS();
    if (pos != g_play.last_pos) {
        g_play.last_pos = pos;
        g_play.moved_s = now;
    }
    if (AuditionEnded(pos, g_play.mix->GetLength(), now - g_play.moved_s)) {
        StopAudition();
        return false;
    }
    return true;
}

const std::string& AuditionTag() { return g_play.active ? g_play.tag : g_pend.active ? g_pend.tag : g_empty; }

}  // namespace rav

#endif  // _WIN32
