// SPDX-License-Identifier: MIT
//
// See video_shots.h.

#include "video_shots.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "console_log.h"
#include "video_fx_track.h"
#include "video_shot_timing.h"
#include "video_timeline.h"

namespace rav {
namespace {

constexpr int kChunkBufferSmall = 64 << 10;  // the FX state is a few kB (base64 in REAPER's chunk)
constexpr int kChunkBufferLarge = 4 << 20;   // retry size when the small buffer came back full
constexpr int kListMaxShots    = 40;       // lines in the "show shots" message box

// ---- base64 (REAPER's clap_chunk) -----------------------------------------------------
const char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string Base64Encode(const std::string& in)
{
    std::string out;
    out.reserve((in.size() + 2) / 3 * 4);
    size_t i = 0;
    for (; i + 2 < in.size(); i += 3) {
        const unsigned v = (static_cast<unsigned char>(in[i]) << 16) | (static_cast<unsigned char>(in[i + 1]) << 8) |
                           static_cast<unsigned char>(in[i + 2]);
        out += kB64[(v >> 18) & 63];
        out += kB64[(v >> 12) & 63];
        out += kB64[(v >> 6) & 63];
        out += kB64[v & 63];
    }
    const size_t rest = in.size() - i;
    if (rest > 0) {
        unsigned v = static_cast<unsigned char>(in[i]) << 16;
        if (rest > 1) v |= static_cast<unsigned char>(in[i + 1]) << 8;
        out += kB64[(v >> 18) & 63];
        out += kB64[(v >> 12) & 63];
        out += rest > 1 ? kB64[(v >> 6) & 63] : '=';
        out += '=';
    }
    return out;
}

int B64Value(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+' || c == '-') return 62;
    if (c == '/' || c == '_') return 63;
    return -1;
}

// Skips anything outside the alphabet (line breaks, spaces); stops at '='.
std::string Base64Decode(const char* in)
{
    std::string out;
    unsigned acc = 0;
    int bits = 0;
    for (const char* s = in; s && *s && *s != '='; ++s) {
        const int v = B64Value(*s);
        if (v < 0) continue;
        acc = (acc << 6) | static_cast<unsigned>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out += static_cast<char>((acc >> bits) & 0xFF);
        }
    }
    return out;
}

// ---- envelopes ----------------------------------------------------------------------------
struct EnvPoint {
    double time;
    double value;  // normalized parameter value
    int shape;
};

struct ParamEnv {
    TrackEnvelope* env = nullptr;
    int mode = 0;  // REAPER scaling mode
    std::vector<EnvPoint> points;
};

double CurrentValue(MediaTrack* track, int fx, int p)
{
    return vcam::Sanitize(p, TrackFX_GetParamNormalized(track, fx, p));
}

ParamEnv LoadEnv(MediaTrack* track, int fx, int p, bool create)
{
    ParamEnv pe;
    pe.env = GetFXEnvelope(track, fx, p, create);
    if (!pe.env) return pe;
    pe.mode = GetEnvelopeScalingMode(pe.env);
    const int n = CountEnvelopePoints(pe.env);
    pe.points.reserve(n > 0 ? static_cast<size_t>(n) : 0);
    for (int i = 0; i < n; ++i) {
        double time = 0.0;
        double raw = 0.0;
        int shape = 0;
        double tension = 0.0;
        bool selected = false;
        if (!GetEnvelopePoint(pe.env, i, &time, &raw, &shape, &tension, &selected)) continue;
        if (!std::isfinite(time)) continue;
        pe.points.push_back({time, vcam::Sanitize(p, ScaleFromEnvelopeMode(pe.mode, raw)), shape});
    }
    std::sort(pe.points.begin(), pe.points.end(), [](const EnvPoint& a, const EnvPoint& b) { return a.time < b.time; });
    return pe;
}

void LoadAllEnvs(MediaTrack* track, int fx, ParamEnv out[vcam::kParamCount])
{
    for (int p = 0; p < vcam::kParamCount; ++p) out[p] = LoadEnv(track, fx, p, false);
}

const EnvPoint* PointNear(const ParamEnv& pe, double t)
{
    for (const EnvPoint& pt : pe.points) {
        if (std::fabs(pt.time - t) <= kVideoShotTimeTolerance) return &pt;
    }
    return nullptr;
}

double EnvValueAt(MediaTrack* track, int fx, int p, const ParamEnv& pe, double t)
{
    if (!pe.env || pe.points.empty()) return CurrentValue(track, fx, p);
    double raw = 0.0;
    double d1 = 0.0;
    double d2 = 0.0;
    double d3 = 0.0;
    Envelope_Evaluate(pe.env, t, 48000.0, 1, &raw, &d1, &d2, &d3);
    return vcam::Sanitize(p, ScaleFromEnvelopeMode(pe.mode, raw));
}

// Shot start times: envelope point times grouped within the tolerance.
std::vector<double> ShotTimes(const ParamEnv envs[vcam::kParamCount])
{
    std::vector<double> all;
    for (int p = 0; p < vcam::kParamCount; ++p) {
        for (const EnvPoint& pt : envs[p].points) all.push_back(pt.time);
    }
    std::sort(all.begin(), all.end());
    std::vector<double> times;
    for (double t : all) {
        if (times.empty() || t - times.back() > kVideoShotTimeTolerance) times.push_back(t);
    }
    return times;
}

bool NearAny(const std::vector<double>& times, double t)
{
    for (double x : times) {
        if (std::fabs(x - t) <= kVideoShotTimeTolerance) return true;
    }
    return false;
}

void RemoveNamesNear(VideoFxState* st, double t)
{
    st->shot_names.erase(std::remove_if(st->shot_names.begin(), st->shot_names.end(),
                                        [t](const VideoFxShotName& n) {
                                            return std::fabs(n.time - t) <= kVideoShotTimeTolerance;
                                        }),
                         st->shot_names.end());
}

// Shot start times as they are now in the envelopes; the implicit shot (0 s) without points.
std::vector<double> CurrentShotTimes(MediaTrack* track, int fx)
{
    ParamEnv envs[vcam::kParamCount];
    LoadAllEnvs(track, fx, envs);
    std::vector<double> times = ShotTimes(envs);
    if (times.empty()) times.push_back(0.0);
    return times;
}

// Keeps only the names that still sit on a shot (points moved or deleted by hand
// leave stale names behind).
void PruneNames(const std::vector<double>& times, VideoFxState* st)
{
    st->shot_names.erase(std::remove_if(st->shot_names.begin(), st->shot_names.end(),
                                        [&times](const VideoFxShotName& n) { return !NearAny(times, n.time); }),
                         st->shot_names.end());
}

// Order does not matter: re-storing an unchanged name moves it to the end of the list,
// and that must not resend the state on every camera gesture.
bool SameNames(std::vector<VideoFxShotName> a, std::vector<VideoFxShotName> b)
{
    if (a.size() != b.size()) return false;
    const auto by_time = [](const VideoFxShotName& x, const VideoFxShotName& y) {
        return x.time < y.time || (x.time == y.time && x.name < y.name);
    };
    std::sort(a.begin(), a.end(), by_time);
    std::sort(b.begin(), b.end(), by_time);
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i].time != b[i].time || a[i].name != b[i].name) return false;
    }
    return true;
}

// Spec 11-fb-15 -- the span flags, like the names: keyed by the shot time.
void RemoveSpansNear(VideoFxState* st, double t)
{
    st->shot_spans.erase(std::remove_if(st->shot_spans.begin(), st->shot_spans.end(),
                                        [t](double x) { return std::fabs(x - t) <= kVideoShotTimeTolerance; }),
                         st->shot_spans.end());
}

void PruneSpans(const std::vector<double>& times, VideoFxState* st)
{
    st->shot_spans.erase(std::remove_if(st->shot_spans.begin(), st->shot_spans.end(),
                                        [&times](double x) { return !NearAny(times, x); }),
                         st->shot_spans.end());
}

bool SameSpans(std::vector<double> a, std::vector<double> b)
{
    std::sort(a.begin(), a.end());
    std::sort(b.begin(), b.end());
    return a == b;
}

// Stores `name` for the shot at t (an empty name or the shot's automatic name `auto_name`
// stores nothing, so the automatic name stays positional), drops stale names and span
// flags, and sends the state only when it changed (camera gestures rewrite shots many
// times per second).
void StoreShotName(MediaTrack* track, int fx, double t, const std::string& name, const std::string& auto_name)
{
    VideoFxState st;
    if (!ReadVideoFxState(track, fx, &st)) return;  // no readable state: the shot stays unnamed
    const std::vector<VideoFxShotName> before = st.shot_names;
    const std::vector<double> spans_before = st.shot_spans;
    const std::vector<double> times = CurrentShotTimes(track, fx);
    RemoveNamesNear(&st, t);
    const std::string clean = CleanVideoFxName(name);
    if (!clean.empty() && clean != CleanVideoFxName(auto_name) && st.shot_names.size() < kVideoFxMaxNames) {
        st.shot_names.push_back({t, clean});
    }
    PruneNames(times, &st);
    PruneSpans(times, &st);
    if (!SameNames(before, st.shot_names) || !SameSpans(spans_before, st.shot_spans)) WriteVideoFxMeta(track, fx, st);
}

bool ValidFx(MediaTrack* track, int fx)
{
    return track && fx >= 0 && fx < TrackFX_GetCount(track);
}

// The first selected track that holds the RAV video FX.
bool FindSelectedVideoFx(MediaTrack** out_track, int* out_fx)
{
    const int n = CountSelectedTracks(nullptr);
    for (int i = 0; i < n; ++i) {
        MediaTrack* track = GetSelectedTrack(nullptr, i);
        const int fx = FindVideoFxOnTrack(track);
        if (fx >= 0) {
            *out_track = track;
            *out_fx = fx;
            return true;
        }
    }
    return false;
}

void FormatValues(const double v[vcam::kParamCount], char* out, size_t cap)
{
    char t[vcam::kParamCount][32];
    for (int p = 0; p < vcam::kParamCount; ++p) vcam::FormatValue(p, v[p], t[p], sizeof(t[p]));
    std::snprintf(out, cap, "yaw %s, pitch %s, distance %s, target %s / %s / %s", t[0], t[1], t[2], t[3], t[4], t[5]);
}

}  // namespace

// ---- FX state ------------------------------------------------------------------------------
bool ReadVideoFxState(MediaTrack* track, int fx, VideoFxState* out)
{
    if (!ValidFx(track, fx) || !out) return false;
    std::vector<char> buf(kChunkBufferSmall, '\0');
    if (!TrackFX_GetNamedConfigParm(track, fx, "clap_chunk", buf.data(), kChunkBufferSmall)) return false;
    buf.back() = '\0';
    if (std::strlen(buf.data()) >= static_cast<size_t>(kChunkBufferSmall) - 2) {  // may be cut: ask again, bigger
        buf.assign(kChunkBufferLarge, '\0');
        if (!TrackFX_GetNamedConfigParm(track, fx, "clap_chunk", buf.data(), kChunkBufferLarge)) return false;
        buf.back() = '\0';
    }
    const std::string bytes = Base64Decode(buf.data());
    return ParseVideoFxState(bytes.data(), bytes.size(), out);
}

bool WriteVideoFxMeta(MediaTrack* track, int fx, const VideoFxState& meta)
{
    if (!ValidFx(track, fx)) return false;
    VideoFxState st = meta;
    st.kind = VideoFxStateKind::Meta;
    const std::string b64 = Base64Encode(SerializeVideoFxState(st));
    const bool ok = TrackFX_SetNamedConfigParm(track, fx, "clap_chunk", b64.c_str());
    if (!ok) LogWarn("video FX: REAPER refused the FX state (clap_chunk), shot names / angles not saved");
    return ok;
}

// ---- Shots ---------------------------------------------------------------------------------
std::vector<VideoShot> ReadVideoShots(MediaTrack* track, int fx)
{
    std::vector<VideoShot> shots;
    if (!ValidFx(track, fx)) return shots;

    ParamEnv envs[vcam::kParamCount];
    LoadAllEnvs(track, fx, envs);
    VideoFxState st;
    const bool have_state = ReadVideoFxState(track, fx, &st);

    const std::vector<double> times = ShotTimes(envs);
    if (times.empty()) {
        VideoShot s;
        s.implicit = true;
        for (int p = 0; p < vcam::kParamCount; ++p) s.values[p] = CurrentValue(track, fx, p);
        shots.push_back(s);
    } else {
        for (double t : times) {
            VideoShot s;
            s.time = t;
            bool all_square = true;
            for (int p = 0; p < vcam::kParamCount; ++p) {
                const EnvPoint* pt = PointNear(envs[p], t);
                if (pt) {
                    s.values[p] = pt->value;
                    if (pt->shape != kVideoShapeCut) all_square = false;
                } else {
                    s.values[p] = EnvValueAt(track, fx, p, envs[p], t);
                }
            }
            s.move_to_next = !all_square;
            shots.push_back(s);
        }
    }

    for (size_t i = 0; i < shots.size(); ++i) {
        if (have_state) {
            for (const VideoFxShotName& n : st.shot_names) {
                if (std::fabs(n.time - shots[i].time) <= kVideoShotTimeTolerance) {
                    shots[i].name = n.name;
                    shots[i].stored_name = !n.name.empty();
                    break;
                }
            }
            if (!shots[i].implicit) shots[i].span = NearAny(st.shot_spans, shots[i].time);
        }
        if (shots[i].name.empty()) {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "Shot %d", static_cast<int>(i) + 1);
            shots[i].name = buf;
        }
    }
    return shots;
}

void ReadVideoCameraAt(MediaTrack* track, int fx, double t, double out[vcam::kParamCount])
{
    for (int p = 0; p < vcam::kParamCount; ++p) out[p] = vcam::DefaultNorm(p);
    if (!ValidFx(track, fx)) return;
    for (int p = 0; p < vcam::kParamCount; ++p) {
        const ParamEnv pe = LoadEnv(track, fx, p, false);
        out[p] = EnvValueAt(track, fx, p, pe, t);
    }
}

bool WriteVideoShot(MediaTrack* track, int fx, const VideoShot& shot, const std::string& auto_name)
{
    if (!ValidFx(track, fx) || !std::isfinite(shot.time)) return false;
    const double t = shot.time;

    // An envelope without points plays the parameter's current value everywhere. Before
    // giving it its first point at t, pin that value at 0 s, so the time before t keeps
    // its framing (REAPER holds the first point's value before it).
    double current[vcam::kParamCount];
    bool empty[vcam::kParamCount];
    for (int p = 0; p < vcam::kParamCount; ++p) {
        current[p] = CurrentValue(track, fx, p);
        TrackEnvelope* env = GetFXEnvelope(track, fx, p, false);
        empty[p] = !env || CountEnvelopePoints(env) <= 0;
    }

    TrackEnvelope* envs[vcam::kParamCount];
    for (int p = 0; p < vcam::kParamCount; ++p) {
        envs[p] = GetFXEnvelope(track, fx, p, true);
        if (!envs[p]) {
            LogWarn("video FX: could not create the envelope of parameter %d", p);
            return false;
        }
    }

    const int shape = shot.move_to_next ? kVideoShapeMove : kVideoShapeCut;
    for (int p = 0; p < vcam::kParamCount; ++p) {
        TrackEnvelope* env = envs[p];
        const int mode = GetEnvelopeScalingMode(env);
        bool no_sort = true;
        if (empty[p] && t > kVideoShotTimeTolerance) {
            // A fresh envelope may hold a point REAPER seeded: the pinned value replaces it.
            DeleteEnvelopePointRange(env, -kVideoShotTimeTolerance, kVideoShotTimeTolerance);
            InsertEnvelopePoint(env, 0.0, ScaleToEnvelopeMode(mode, current[p]), kVideoShapeCut, 0.0, false, &no_sort);
        }
        DeleteEnvelopePointRange(env, t - kVideoShotTimeTolerance, t + kVideoShotTimeTolerance);
        InsertEnvelopePoint(env, t, ScaleToEnvelopeMode(mode, vcam::Sanitize(p, shot.values[p])), shape, 0.0, false,
                            &no_sort);
        Envelope_SortPoints(env);
    }

    StoreShotName(track, fx, t, shot.name, auto_name);
    UpdateArrange();
    return true;
}

bool DeleteVideoShot(MediaTrack* track, int fx, double time)
{
    if (!ValidFx(track, fx)) return false;
    bool any = false;
    for (int p = 0; p < vcam::kParamCount; ++p) {
        TrackEnvelope* env = GetFXEnvelope(track, fx, p, false);
        if (!env) continue;
        if (DeleteEnvelopePointRange(env, time - kVideoShotTimeTolerance, time + kVideoShotTimeTolerance)) any = true;
        Envelope_SortPoints(env);
    }
    // Its name and span flag go with it (pruned: no point is left at `time`).
    StoreShotName(track, fx, time, std::string(), std::string());
    UpdateArrange();
    return any;
}

bool MoveVideoShot(MediaTrack* track, int fx, double old_time, double new_time)
{
    if (!ValidFx(track, fx) || !std::isfinite(old_time) || !std::isfinite(new_time)) return false;

    // The shot's stored name (none = automatic) and span flag, read before the points move.
    std::string name;
    bool span = false;
    VideoFxState st;
    if (ReadVideoFxState(track, fx, &st)) {
        for (const VideoFxShotName& n : st.shot_names) {
            if (std::fabs(n.time - old_time) <= kVideoShotTimeTolerance) {
                name = n.name;
                break;
            }
        }
        span = NearAny(st.shot_spans, old_time);
    }

    bool any = false;
    for (int p = 0; p < vcam::kParamCount; ++p) {
        TrackEnvelope* env = GetFXEnvelope(track, fx, p, false);
        if (!env) continue;
        const int n = CountEnvelopePoints(env);
        bool moved = false;
        for (int i = 0; i < n; ++i) {
            double t = 0.0;
            double value = 0.0;
            int shape = 0;
            double tension = 0.0;
            bool selected = false;
            if (!GetEnvelopePoint(env, i, &t, &value, &shape, &tension, &selected)) continue;
            if (std::fabs(t - old_time) > kVideoShotTimeTolerance) continue;
            double time = new_time;
            bool no_sort = true;
            if (SetEnvelopePoint(env, i, &time, nullptr, nullptr, nullptr, nullptr, &no_sort)) moved = true;
        }
        if (moved) {
            Envelope_SortPoints(env);
            any = true;
        }
    }
    if (!any) return false;

    // Re-key the name on the new time; the old time is no longer a shot, so its entry is pruned.
    // The stored name is kept as it is (an automatic name is never stored).
    StoreShotName(track, fx, new_time, name, std::string());
    if (span) SetVideoShotSpan(track, fx, new_time, true);
    UpdateArrange();
    return true;
}

bool SetVideoShotTransition(MediaTrack* track, int fx, double time, bool move_to_next)
{
    if (!ValidFx(track, fx)) return false;
    int shape = move_to_next ? kVideoShapeMove : kVideoShapeCut;
    bool any = false;
    for (int p = 0; p < vcam::kParamCount; ++p) {
        TrackEnvelope* env = GetFXEnvelope(track, fx, p, false);
        if (!env) continue;
        const int n = CountEnvelopePoints(env);
        for (int i = 0; i < n; ++i) {
            double t = 0.0;
            double value = 0.0;
            int old_shape = 0;
            double tension = 0.0;
            bool selected = false;
            if (!GetEnvelopePoint(env, i, &t, &value, &old_shape, &tension, &selected)) continue;
            if (std::fabs(t - time) > kVideoShotTimeTolerance) continue;
            bool no_sort = true;
            if (SetEnvelopePoint(env, i, nullptr, nullptr, &shape, nullptr, nullptr, &no_sort)) any = true;
        }
        Envelope_SortPoints(env);
    }
    UpdateArrange();
    return any;
}

bool RenameVideoShot(MediaTrack* track, int fx, double time, const std::string& name, const std::string& auto_name)
{
    VideoFxState probe;
    if (!ReadVideoFxState(track, fx, &probe)) return false;
    StoreShotName(track, fx, time, name, auto_name);
    return true;
}

bool SetVideoShotSpan(MediaTrack* track, int fx, double time, bool on)
{
    if (!ValidFx(track, fx) || !std::isfinite(time)) return false;
    VideoFxState st;
    if (!ReadVideoFxState(track, fx, &st)) return false;
    const std::vector<double> before = st.shot_spans;
    RemoveSpansNear(&st, time);
    if (on && st.shot_spans.size() < kVideoFxMaxNames) st.shot_spans.push_back(time);
    PruneSpans(CurrentShotTimes(track, fx), &st);
    if (SameSpans(before, st.shot_spans)) return true;
    return WriteVideoFxMeta(track, fx, st);
}

// ---- Shots per clip (spec 11-fb-15) -------------------------------------------------------------
VideoTrackClipShots ReadVideoClipShots(MediaTrack* track, int fx, double fps)
{
    VideoTrackClipShots out;
    if (!ValidFx(track, fx)) return out;
    out.raw = ReadVideoShots(track, fx);
    std::vector<VideoClipIn> clips;
    const std::shared_ptr<const VideoTimelineSnapshot> snap = CurrentVideoTimeline();
    if (const VideoTrackTimeline* tl = snap ? snap->Find(track) : nullptr) {
        clips.reserve(tl->items.size());
        for (const VideoItemSpan& it : tl->items) {
            VideoClipIn c;
            c.start = it.start;
            c.end = it.end;
            c.anim_name = VideoAnimNameFromPath(it.path);
            clips.push_back(std::move(c));
        }
    }
    std::vector<VideoRawShotIn> raws;
    raws.reserve(out.raw.size());
    for (const VideoShot& s : out.raw) {
        if (s.implicit) continue;  // no point at all: every clip opens with an implicit shot
        VideoRawShotIn r;
        r.time = s.time;
        r.stored_name = s.stored_name;
        r.span = s.span;
        raws.push_back(r);
    }
    out.built = BuildVideoClipShots(clips, raws, fps);
    // `raw` indices of the built shots point into out.raw: the implicit raw entry (no point)
    // was skipped, and it is the only entry then.
    if (raws.empty()) out.raw.clear();
    return out;
}

std::string VideoClipShotAutoName(const VideoClipShots& built, size_t i)
{
    if (i >= built.shots.size()) return std::string();
    const VideoClipShot& s = built.shots[i];
    if (s.clip < 0 || static_cast<size_t>(s.clip) >= built.clips.size()) return std::string();
    return VideoAutoShotName(built.clips[static_cast<size_t>(s.clip)].anim_name, s.number);
}

bool EnsureVideoShotPoint(MediaTrack* track, int fx, double time, double fps, const double* values)
{
    if (!ValidFx(track, fx) || !std::isfinite(time)) return false;
    ParamEnv envs[vcam::kParamCount];
    LoadAllEnvs(track, fx, envs);
    if (NearAny(ShotTimes(envs), time)) return true;  // already a real shot
    VideoShot s;
    s.time = time;
    s.move_to_next = false;
    // The camera of its first frame: what the envelopes render there (the inherited camera).
    if (values) {
        for (int p = 0; p < vcam::kParamCount; ++p) s.values[p] = values[p];
    } else {
        ReadVideoCameraAt(track, fx, VideoShotFirstFrameTime(time, fps), s.values);
    }
    return WriteVideoShot(track, fx, s);
}

void MoveSpanToCut(MediaTrack* track, int fx, const VideoTrackClipShots& cs, int clip, double cut_time,
                   double fps)
{
    const int i = VideoClipShotAt(cs.built.shots, VideoShotFirstFrameTime(cut_time, fps), fps);
    if (i < 0) return;
    const VideoClipShot& s = cs.built.shots[static_cast<size_t>(i)];
    if (s.clip != clip || s.raw < 0) return;  // in the clip it spans into: it still covers that start
    const VideoShot& r = cs.raw[static_cast<size_t>(s.raw)];
    if (!r.span || std::fabs(r.time - cut_time) <= kVideoShotTimeTolerance) return;
    SetVideoShotSpan(track, fx, r.time, false);
    SetVideoShotSpan(track, fx, cut_time, true);
}

bool MaterializeClipFirstShotForCut(MediaTrack* track, int fx, const VideoClipShots& built, int clip,
                                    double cut_time, double fps)
{
    if (clip < 0) return true;
    for (const VideoClipShot& s : built.shots) {
        if (s.clip != clip || !s.clip_first || s.raw >= 0) continue;
        const double t = VideoClipImplicitTime(built.clips[static_cast<size_t>(clip)].start, fps);
        if (std::fabs(t - cut_time) <= kVideoShotTimeTolerance) return true;  // the cut is that shot
        return EnsureVideoShotPoint(track, fx, t, fps);
    }
    return true;  // its first shot has a point, or a shot from the previous clip spans it
}

// ---- Envelope visibility (spec 11-fb-11) ------------------------------------------------------
void VideoFxEnvelopesState(MediaTrack* track, int fx, bool* any, bool* visible)
{
    if (any) *any = false;
    if (visible) *visible = false;
    if (!ValidFx(track, fx)) return;
    const int n = TrackFX_GetNumParams(track, fx);
    for (int p = 0; p < n; ++p) {
        TrackEnvelope* env = GetFXEnvelope(track, fx, p, false);
        if (!env) continue;
        if (any) *any = true;
        char buf[16] = {};
        if (GetSetEnvelopeInfo_String(env, "VISIBLE", buf, false) && buf[0] == '1') {
            if (visible) *visible = true;
            return;  // both known
        }
    }
}

bool SetVideoFxEnvelopesVisible(MediaTrack* track, int fx, bool visible)
{
    if (!ValidFx(track, fx)) return false;
    bool any = false;
    const int n = TrackFX_GetNumParams(track, fx);
    for (int p = 0; p < n; ++p) {
        TrackEnvelope* env = GetFXEnvelope(track, fx, p, false);
        if (!env) continue;
        any = true;
        char buf[4] = {visible ? '1' : '0', '\0'};
        GetSetEnvelopeInfo_String(env, "VISIBLE", buf, true);
    }
    if (!any) return false;
    TrackList_AdjustWindows(false);  // envelope lanes change the track heights
    UpdateArrange();
    return true;
}

// ---- Saved angles and output -----------------------------------------------------------------
bool SaveVideoAngle(MediaTrack* track, int fx, const std::string& name, const double values[vcam::kParamCount])
{
    const std::string clean = CleanVideoFxName(name);
    if (clean.empty() || !values) return false;
    VideoFxState st;
    if (!ReadVideoFxState(track, fx, &st)) return false;
    VideoFxAngle a;
    a.name = clean;
    for (int p = 0; p < vcam::kParamCount; ++p) a.values[p] = vcam::Sanitize(p, values[p]);
    auto it = std::find_if(st.angles.begin(), st.angles.end(), [&clean](const VideoFxAngle& x) { return x.name == clean; });
    if (it != st.angles.end()) {
        *it = a;
    } else {
        if (st.angles.size() >= kVideoFxMaxAngles) return false;
        st.angles.push_back(a);
    }
    return WriteVideoFxMeta(track, fx, st);
}

bool DeleteVideoAngle(MediaTrack* track, int fx, const std::string& name)
{
    VideoFxState st;
    if (!ReadVideoFxState(track, fx, &st)) return false;
    const std::string clean = CleanVideoFxName(name);
    const size_t before = st.angles.size();
    st.angles.erase(std::remove_if(st.angles.begin(), st.angles.end(),
                                   [&clean](const VideoFxAngle& x) { return x.name == clean; }),
                    st.angles.end());
    if (st.angles.size() == before) return false;
    return WriteVideoFxMeta(track, fx, st);
}

bool SetVideoOutputOverride(MediaTrack* track, int fx, int width, int height)
{
    VideoFxState st;
    if (!ReadVideoFxState(track, fx, &st)) return false;
    if (VideoFxOverrideValid(width, height)) {
        st.override_width = width;
        st.override_height = height;
    } else {
        st.override_width = 0;
        st.override_height = 0;
    }
    return WriteVideoFxMeta(track, fx, st);
}

// ---- Test hooks ------------------------------------------------------------------------------
void ShowVideoShotsOfSelectedTrack()
{
    MediaTrack* track = nullptr;
    int fx = -1;
    if (!FindSelectedVideoFx(&track, &fx)) {
        ShowMessageBox("Select a track that has the RAV video FX (action \"RAV: Add video FX to selected track\").",
                       "RAV: Video FX shots", 0);
        return;
    }
    const std::vector<VideoShot> shots = ReadVideoShots(track, fx);
    VideoFxState st;
    const bool have_state = ReadVideoFxState(track, fx, &st);

    std::string text;
    char line[512];
    std::snprintf(line, sizeof(line), "Track %d, FX %d: %d shot(s)\n\n",
                  static_cast<int>(GetMediaTrackInfo_Value(track, "IP_TRACKNUMBER")), fx + 1,
                  static_cast<int>(shots.size()));
    text += line;
    for (size_t i = 0; i < shots.size() && i < static_cast<size_t>(kListMaxShots); ++i) {
        const VideoShot& s = shots[i];
        char values[256];
        FormatValues(s.values, values, sizeof(values));
        std::snprintf(line, sizeof(line), "%d. %.3f s  \"%s\"  %s%s\n     %s\n", static_cast<int>(i) + 1, s.time,
                      s.name.c_str(), s.move_to_next ? "Move to next" : "Cut to next",
                      s.implicit ? "  (no envelope point: current values)" : "", values);
        text += line;
    }
    if (shots.size() > static_cast<size_t>(kListMaxShots)) {
        std::snprintf(line, sizeof(line), "... and %d more\n",
                      static_cast<int>(shots.size()) - kListMaxShots);
        text += line;
    }
    text += "\n";
    if (have_state) {
        if (VideoFxOverrideValid(st.override_width, st.override_height)) {
            std::snprintf(line, sizeof(line), "Output: override %d x %d\n", st.override_width, st.override_height);
        } else {
            std::snprintf(line, sizeof(line), "Output: project video size\n");
        }
        text += line;
        std::snprintf(line, sizeof(line), "Saved angles: %d\n", static_cast<int>(st.angles.size()));
        text += line;
        for (const VideoFxAngle& a : st.angles) {
            char values[256];
            FormatValues(a.values, values, sizeof(values));
            text += "  \"" + a.name + "\"  " + values + "\n";
        }
    } else {
        text += "FX state not readable (shot names, output override and saved angles unavailable).\n";
    }
    ShowMessageBox(text.c_str(), "RAV: Video FX shots", 0);
}

namespace {
// The project frame rate, 0 when unknown (the same source as Video view's model fps).
double ProjectFps()
{
    bool drop = false;
    const double fps = TimeMap_curFrameRate(nullptr, &drop);
    return (std::isfinite(fps) && fps > 0.0) ? fps : 0.0;
}
}  // namespace

void AddVideoShotAtEditCursor()
{
    MediaTrack* track = nullptr;
    int fx = -1;
    if (!FindSelectedVideoFx(&track, &fx)) {
        ShowMessageBox("Select a track that has the RAV video FX (action \"RAV: Add video FX to selected track\").",
                       "RAV: Add video shot", 0);
        return;
    }
    const double t = std::max(0.0, GetCursorPositionEx(nullptr));
    const double fps = ProjectFps();

    // Spec 11-fb-15: shots belong to clips. The clip holding the cursor's frame (none: refused).
    const VideoTrackClipShots cs = ReadVideoClipShots(track, fx, fps);
    const int clip = VideoClipIndexAtTime(cs.built.clips, t, fps);
    if (clip < 0) {
        ShowMessageBox("The edit cursor is between clips: there is no shot there. Put it on an animation item of "
                       "the track.",
                       "RAV: Add video shot", 0);
        return;
    }
    const double item_start = cs.built.clips[static_cast<size_t>(clip)].start;

    // Like a cut in Video view (spec 11-fb-10): the shot's points go half a frame before
    // the cursor's frame, and it holds the camera of that frame (fps unknown: the cursor).
    // Spec 11-fb-12: never on a frame that begins before the clip under the cursor.
    VideoShot shot;
    shot.time = VideoCutTimeInItem(t, fps, item_start);
    shot.move_to_next = false;
    // The camera at the shot's own first frame (the item rule may move it past the cursor's).
    const long long cut_frame = VideoCutFrameInItem(t, fps, item_start);
    const double camera_time = (fps > 0.0 && cut_frame > VideoFrameIndexAt(t, fps))
                                   ? static_cast<double>(cut_frame) / fps
                                   : VideoPlayheadFrameTime(t, fps);
    ReadVideoCameraAt(track, fx, camera_time, shot.values);

    // Suggested name: the shot's own name when one already starts on that frame, else the
    // automatic "<anim name> N" it will get (N: its place in the clip).
    const double cut_first = VideoShotFirstFrameTime(shot.time, fps);
    std::string auto_name;
    std::string existing;
    int number = 1;
    for (size_t i = 0; i < cs.built.shots.size(); ++i) {
        const VideoClipShot& s = cs.built.shots[i];
        if (s.clip != clip) continue;
        const bool same = fps > 0.0 ? VideoShotFirstFrame(s.start, fps) == VideoShotFirstFrame(cut_first, fps)
                                    : std::fabs(s.start - cut_first) <= kVideoShotTimeTolerance;
        if (same) {
            auto_name = VideoClipShotAutoName(cs.built, i);
            if (s.raw >= 0) {
                const VideoShot& r = cs.raw[static_cast<size_t>(s.raw)];
                if (r.stored_name) existing = r.name;
                shot.move_to_next = r.move_to_next;  // re-adding (renaming) a shot keeps its transition
                shot.time = r.time;                  // and its points' time
            }
            break;
        }
        if (s.start < cut_first) number = s.number + 1;
    }
    if (auto_name.empty()) auto_name = VideoAutoShotName(cs.built.clips[static_cast<size_t>(clip)].anim_name, number);
    char buf[512];
    std::snprintf(buf, sizeof(buf), "%s", existing.empty() ? auto_name.c_str() : existing.c_str());
    if (!GetUserInputs("RAV: Add video shot", 1, "Shot name:,extrawidth=160", buf, sizeof(buf))) return;
    shot.name = buf;  // the automatic name is not stored (StoreShotName)

    // The clip's implicit first shot is written with it (one undo point).
    Undo_BeginBlock2(nullptr);
    bool ok = MaterializeClipFirstShotForCut(track, fx, cs.built, clip, shot.time, fps);
    ok = WriteVideoShot(track, fx, shot, auto_name) && ok;
    if (ok) MoveSpanToCut(track, fx, cs, clip, shot.time, fps);
    Undo_EndBlock2(nullptr, "RAV: Add video shot", UNDO_STATE_FX);
    if (!ok) {
        ShowMessageBox("RAV could not write the shot (the FX envelopes could not be created).", "RAV: Add video shot", 0);
    }
}

void SetVideoOutputAndAngleOfSelectedTrack()
{
    const char* const kTitle = "RAV: Video FX output size and angle";
    MediaTrack* track = nullptr;
    int fx = -1;
    if (!FindSelectedVideoFx(&track, &fx)) {
        ShowMessageBox("Select a track that has the RAV video FX (action \"RAV: Add video FX to selected track\").",
                       kTitle, 0);
        return;
    }
    VideoFxState st;
    if (!ReadVideoFxState(track, fx, &st)) {
        ShowMessageBox("The FX state is not readable: the output size and the saved angles cannot be set.", kTitle, 0);
        return;
    }

    // Two fields, returned on separate lines (a name may hold commas).
    char buf[512];
    if (VideoFxOverrideValid(st.override_width, st.override_height)) {
        std::snprintf(buf, sizeof(buf), "%d x %d\n", st.override_width, st.override_height);
    } else {
        std::snprintf(buf, sizeof(buf), "\n");
    }
    if (!GetUserInputs(kTitle, 2,
                       "Output size (W x H; empty = project size):,"
                       "Save the camera at the edit cursor as angle (name; empty = none):,"
                       "extrawidth=160,separator=\n",
                       buf, sizeof(buf))) {
        return;
    }
    const std::string all(buf);
    const size_t nl = all.find('\n');
    const std::string size_text = all.substr(0, nl);
    const std::string angle_name = nl == std::string::npos ? std::string() : CleanVideoFxName(all.substr(nl + 1));

    int width = 0;
    int height = 0;
    if (!CleanVideoFxName(size_text).empty()) {
        const char* s = size_text.c_str();
        char* end = nullptr;
        const long w = std::strtol(s, &end, 10);
        while (*end == ' ' || *end == 'x' || *end == 'X' || *end == '*') ++end;
        const char* hs = end;
        const long h = std::strtol(hs, &end, 10);
        if (end == hs || w <= 0 || h <= 0 || w > kVideoFxMaxSize || h > kVideoFxMaxSize) {
            ShowMessageBox("Output size: type W x H (1 to 8192 each), or leave it empty for the project size.",
                           kTitle, 0);
            return;
        }
        width = static_cast<int>(w);
        height = static_cast<int>(h);
    }

    Undo_BeginBlock2(nullptr);
    bool ok = SetVideoOutputOverride(track, fx, width, height);
    if (ok && !angle_name.empty()) {
        double values[vcam::kParamCount];
        // The camera of the cursor's frame, as Video view shows it (spec 11-fb-10).
        ReadVideoCameraAt(track, fx, VideoPlayheadFrameTime(GetCursorPositionEx(nullptr), ProjectFps()), values);
        ok = SaveVideoAngle(track, fx, angle_name, values);
    }
    Undo_EndBlock2(nullptr, "RAV: Set video FX output size / save angle", UNDO_STATE_FX);
    if (!ok) ShowMessageBox("RAV could not write the FX state (see the RAV log).", kTitle, 0);
}

}  // namespace rav
