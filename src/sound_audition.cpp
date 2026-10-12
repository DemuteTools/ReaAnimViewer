// SPDX-License-Identifier: MIT
//
// See sound_audition.h. The mixing source (AuditionMix) is read on REAPER's audio thread: its
// voices are fixed once it plays and freed only after StopPreview has returned.

#include "sound_audition.h"

#ifdef _WIN32

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <memory>

#include "library_media_ui.h"
#include "reaper_api.h"
#include "sound_audition_plan.h"

namespace rav {
namespace {

using CreateFromFileFn = PCM_source* (*)(const char*);
using PreviewFn = int (*)(preview_register_t*);

CreateFromFileFn f_create = nullptr;
PreviewFn        f_play = nullptr;
PreviewFn        f_stop = nullptr;

struct Voice {
    std::unique_ptr<PCM_source> src;
    double                      start = 0.0, length = 0.0, delay = 0.0, gain = 1.0;
    double                      fade_in = 0.0, fade_out = 0.0;  // s (FadeGainAt)
    int                         in_shape = 0, out_shape = 0;
};

class AuditionMix : public PCM_source {
public:
    explicit AuditionMix(std::vector<Voice> v) : voices_(std::move(v))
    {
        for (const Voice& x : voices_) {
            len_ = std::max(len_, x.delay + x.length);
            nch_ = std::max(nch_, std::min(2, x.src->GetNumChannels()));
            if (sr_ < 1.0) sr_ = x.src->GetSampleRate();
        }
        if (nch_ < 1) nch_ = 1;
        scratch_.resize(16384 * 2);  // a usual block: no allocation on the audio thread
    }
    PCM_source* Duplicate() override { return nullptr; }
    bool        IsAvailable() override { return true; }
    const char* GetType() override { return "RAV_AUDITION"; }
    bool        SetFileName(const char*) override { return false; }
    int         GetNumChannels() override { return nch_; }
    double      GetSampleRate() override { return sr_ > 1.0 ? sr_ : 44100.0; }
    double      GetLength() override { return len_; }
    int         PropertiesWindow(HWND) override { return 0; }
    void        GetPeakInfo(PCM_source_peaktransfer_t*) override {}
    void        SaveState(ProjectStateContext*) override {}
    int         LoadState(const char*, ProjectStateContext*) override { return -1; }
    void        Peaks_Clear(bool) override {}
    int         PeaksBuild_Begin() override { return 0; }
    int         PeaksBuild_Run() override { return 0; }
    void        PeaksBuild_Finish() override {}

    void GetSamples(PCM_source_transfer_t* block) override
    {
        if (!block || !block->samples || block->length <= 0 || block->nch <= 0 || !(block->samplerate > 0.0)) {
            if (block) block->samples_out = 0;
            return;
        }
        const int    n = block->length, nch = block->nch;
        const double sr = block->samplerate, t0 = block->time_s, t1 = t0 + n / sr;
        std::fill(block->samples, block->samples + static_cast<size_t>(n) * static_cast<size_t>(nch), 0.0);
        if (scratch_.size() < static_cast<size_t>(n) * static_cast<size_t>(nch))
            scratch_.resize(static_cast<size_t>(n) * static_cast<size_t>(nch));
        for (Voice& v : voices_) {
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
            v.src->GetSamples(&tr);
            const int    got = std::min(cnt, std::max(0, tr.samples_out));
            const double t_voice = a - v.delay;  // the first sample's time into the voice
            const bool   fades = v.fade_in > 0.0 || v.fade_out > 0.0;
            for (int k = 0; k < got; ++k) {
                const double g = fades ? v.gain * FadeGainAt(t_voice + k / sr, v.length, v.fade_in, v.fade_out,
                                                             v.in_shape, v.out_shape)
                                       : v.gain;
                for (int c = 0; c < nch; ++c)
                    block->samples[static_cast<size_t>((off + k) * nch + c)] +=
                        scratch_[static_cast<size_t>(k * nch + c)] * g;
            }
        }
        block->samples_out = n;
    }

private:
    std::vector<Voice>     voices_;
    std::vector<ReaSample> scratch_;
    double                 len_ = 0.0, sr_ = 0.0;
    int                    nch_ = 0;
};

struct Playing {
    bool                         active = false;
    preview_register_t           reg = {};
    std::unique_ptr<AuditionMix> mix;
    std::string                  tag;
    double                       last_pos = -1.0;  // AuditionPlaying's end watch
    double                       moved_s = 0.0;
};
Playing     g_play;
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
    f_stop = reinterpret_cast<PreviewFn>(get_func("StopPreview"));
}

void StopAudition()
{
    if (!g_play.active) return;
    if (f_stop) f_stop(&g_play.reg);
    DeleteCriticalSection(&g_play.reg.cs);
    g_play.reg = preview_register_t{};
    g_play.mix.reset();  // after StopPreview: the audio thread no longer reads it
    g_play.tag.clear();
    g_play.active = false;
}

bool PlayVoices(const std::vector<PlayVoice>& voices, const std::string& tag, std::string* why)
{
    StopAudition();
    StopMediaPreview();  // a tree audition and a Media Explorer preview never sound together (13-7c)
    if (!f_play || !f_stop || !f_create) {
        if (why) *why = "REAPER's preview is not available";
        return false;
    }
    std::vector<Voice> vs;
    for (const PlayVoice& p : voices) {
        if (p.path.empty()) continue;
        PCM_source* s = f_create(p.path.c_str());
        if (!s) continue;
        if (s->GetSampleRate() < 1.0 || s->GetNumChannels() < 1) {
            delete s;
            continue;
        }
        Voice v;
        v.src.reset(s);
        v.start = std::max(0.0, p.start_s);
        const double rest = std::max(0.0, s->GetLength() - v.start);
        v.length = p.length_s > 0.0 ? std::min(p.length_s, rest) : rest;
        v.delay = std::max(0.0, p.delay_s);
        v.gain = p.gain;
        v.fade_in = std::max(0.0, p.fade_in_s);
        v.fade_out = std::max(0.0, p.fade_out_s);
        v.in_shape = p.fade_in_shape;
        v.out_shape = p.fade_out_shape;
        if (v.length > 0.0) vs.push_back(std::move(v));
    }
    if (vs.empty()) {
        if (why) *why = "no sound file could be read";
        return false;
    }
    g_play.mix = std::make_unique<AuditionMix>(std::move(vs));
    InitReg(g_play.reg, g_play.mix.get());
    g_play.active = true;
    g_play.tag = tag;
    g_play.last_pos = -1.0;
    g_play.moved_s = NowS();
    if (f_play(&g_play.reg) == 0) {
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
    EnterCriticalSection(&g_play.reg.cs);
    const double p = g_play.reg.curpos;
    LeaveCriticalSection(&g_play.reg.cs);
    return p;
}

bool AuditionPlaying()
{
    if (!g_play.active) return false;
    const double    pos = AuditionPosition();
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

const std::string& AuditionTag() { return g_play.active ? g_play.tag : g_empty; }

}  // namespace rav

#endif  // _WIN32
