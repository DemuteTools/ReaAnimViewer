// SPDX-License-Identifier: MIT
//
// rav_video_fx.clap -- the RAV video FX (Epic 11, Stories 11-1 and 11-3). A thin CLAP shim:
//   - audio: stereo passthrough;
//   - video: gets REAPER's IREAPERVideoProcessor in init, and for each frame REAPER
//     asks for, calls the frame-render API that reaper_animviewer.dll registers
//     (src/video_fx_api.h). No renderer, no loader, no assimp in this binary;
//   - camera (11-3): six automatable parameters (src/video_camera_params.h) that REAPER
//     passes back in each frame's parmlist, and a CLAP state (src/video_fx_state.h:
//     values, output override, shot names, saved angles) saved with the project and in
//     REAPER's FX presets (a preset saved with CLAP's preset context holds the camera
//     values only). The extension changes the state through REAPER's "clap_chunk".
//
// It is inert -- audio passthrough, no picture, no crash -- when REAPER gives no
// FxDsp context (startup scan process, another host), when the extension is not
// loaded, or when its API version differs; the last two write ONE line per REAPER
// session to the console, the only output of this file.
//
// REAPER access (reaper_plugin.h): host->get_extension("cockos.reaper_extension")
// -> reaper_plugin_info_t*; GetFunc("clap_get_reaper_context")(host, sel) with
// sel 1 = parent track, 3 = project, 4 = FxDsp, 6 = index in chain.

#include <windows.h>

#include <clap/clap.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <new>
#include <string>

#include "rav_version.h"     // generated: RAV_VERSION_STRING
#include "reaper_plugin.h"   // reaper_plugin_info_t
#include "video_camera_params.h"
#include "video_fx_api.h"
#include "video_fx_state.h"

#ifndef WDL_FIXALIGN
#define WDL_FIXALIGN  // WDL's macro is empty for MSVC; doubles are naturally aligned on x64
#endif
#include "video_processor.h"  // IREAPERVideoProcessor, IVideoFrame (reaper-sdk 490ded5)

namespace {

// Embedded so the extension's self-update can read this file's version without
// loading it (same marker as the extension, src/self_update.cpp). The descriptor's
// version points into it, which also keeps it in the binary.
const char kVersionMarker[] = "RAV_VERSION_MARKER:" RAV_VERSION_STRING;
constexpr size_t kMarkerPrefix = sizeof("RAV_VERSION_MARKER:") - 1;

constexpr int kFourccRgba = 'RGBA';
constexpr int kMinSize    = 16;
constexpr int kMaxSize    = 8192;
constexpr size_t kMaxStateBytes = 4u << 20;  // a RAV state is a few kB; refuse absurd streams

using rav::vcam::kParamCount;

const char* const kFeatures[] = {CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, CLAP_PLUGIN_FEATURE_UTILITY, nullptr};

const clap_plugin_descriptor_t kDescriptor = {
    CLAP_VERSION_INIT,
    RAV_VIDEO_FX_CLAP_ID,
    RAV_VIDEO_FX_NAME,
    RAV_VIDEO_FX_VENDOR,
    "https://github.com/DemuteTools/ReaAnimViewer",
    "",
    "",
    kVersionMarker + kMarkerPrefix,
    "Supplies ReaAnimViewer's picture of this track's animation to REAPER's video engine",
    kFeatures,
};

typedef IREAPERVideoProcessor* (*CreateVideoProcessorFn)(void* fxctx, int version);
typedef void* (*GetReaperContextFn)(const clap_host_t* host, int sel);
typedef void (*ShowConsoleMsgFn)(const char* msg);

struct Instance {
    clap_plugin_t plugin;
    const clap_host_t* host = nullptr;
    const reaper_plugin_info_t* rec = nullptr;
    GetReaperContextFn get_ctx = nullptr;
    IREAPERVideoProcessor* vproc = nullptr;
    // Resolved on the main thread (init / activate), read on the video thread.
    std::atomic<const RavVideoFxApi*> api{nullptr};
    // The extension's module, pinned (one reference) while `api` points into it, so
    // REAPER's shutdown order can never unmap code this instance may still call.
    HMODULE api_module = nullptr;
    bool api_checked = false;

    // Camera parameter values (Story 11-3). Written on the main thread (state load) and
    // the audio thread (parameter events), read on every thread.
    std::atomic<double> values[kParamCount];
    // The refresh counter (rav::vcam::kRefreshParam), set by the extension. Not saved.
    std::atomic<double> refresh{0.0};
    // Output size override from the state, read on the video thread.
    std::atomic<int> override_w{0};
    std::atomic<int> override_h{0};
    // Shot names and saved angles: main thread only (state save / load).
    rav::VideoFxState meta;
    const clap_host_params_t* host_params = nullptr;

    Instance()
    {
        std::memset(&plugin, 0, sizeof(plugin));  // a user constructor no longer zero-initializes it
        for (int p = 0; p < kParamCount; ++p) values[p].store(rav::vcam::DefaultNorm(p));
    }
};

Instance* Self(const clap_plugin_t* p) { return static_cast<Instance*>(p->plugin_data); }

// ---- Diagnostic: one console line per REAPER session ------------------------------
std::atomic<bool> g_diag_written{false};

void DiagnoseOnce(const Instance* s, const char* text)
{
    if (!s->rec || !s->rec->GetFunc || g_diag_written.exchange(true)) return;
    auto show = reinterpret_cast<ShowConsoleMsgFn>(s->rec->GetFunc("ShowConsoleMsg"));
    if (show) show(text);
}

// ---- Frame-render API --------------------------------------------------------------
// Main thread only. Returns the table when present and of our exact version.
void ResolveApi(Instance* s)
{
    if (s->api.load() || !s->rec || !s->rec->GetFunc) return;
    auto get_api = reinterpret_cast<RavGetVideoFxApiFn>(s->rec->GetFunc(RAV_VIDEO_FX_API_NAME));
    const RavVideoFxApi* api = get_api ? get_api(RAV_VIDEO_FX_API_VERSION) : nullptr;
    if (api && (api->version != RAV_VIDEO_FX_API_VERSION ||
                api->struct_size < static_cast<int>(sizeof(RavVideoFxApi)) || !api->frame_size ||
                !api->render_frame)) {
        api = nullptr;
    }
    if (api) {
        // Takes a reference on the module holding the table (released in destroy).
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(api->render_frame),
                                &s->api_module)) {
            s->api_module = nullptr;
        }
        s->api.store(api);
        return;
    }
    if (s->api_checked) return;  // already diagnosed for this instance
    s->api_checked = true;
    if (!get_api) {
        DiagnoseOnce(s, "[RAV video FX] The ReaAnimViewer extension is not loaded, so the video FX shows no "
                        "picture. Install or update ReaAnimViewer (ReaPack, or Run on its Demute Reaper "
                        "Toolkit card), then restart REAPER.\n");
    } else {
        DiagnoseOnce(s, "[RAV video FX] rav_video_fx.clap and the ReaAnimViewer extension do not match "
                        "(frame API version), so the video FX shows no picture. Restart REAPER after an "
                        "update; if it stays, reinstall ReaAnimViewer (from ReaPack, or from the Demute "
                        "Reaper Toolkit).\n");
    }
}

// Bytes per row of an 'RGBA' frame. Measured in bytes (spike 11-0); a value below
// w * 4 can only be pixels (LICE convention), so it is converted.
int FrameRowBytes(IVideoFrame* vf)
{
    const int w = vf->get_w();
    const int span = vf->get_rowspan();
    return span >= w * 4 ? span : span * 4;
}

void* Ctx(const Instance* s, int sel)
{
    return s->get_ctx ? s->get_ctx(s->host, sel) : nullptr;
}

IVideoFrame* ProcessFrame(IREAPERVideoProcessor* vproc, const double* parmlist, int nparms, double project_time,
                          double frate, int /*force_format: REAPER converts our 'RGBA'*/)
{
    Instance* s = vproc ? static_cast<Instance*>(vproc->userdata) : nullptr;
    if (!s) return nullptr;
    const RavVideoFxApi* api = s->api.load();
    if (!api) return nullptr;

    try {
        RavVideoFrameRequest req;
        std::memset(&req, 0, sizeof(req));
        req.struct_size = static_cast<int>(sizeof(req));
        req.project_time = project_time;
        req.frame_rate = frate;
        req.project = static_cast<ReaProject*>(Ctx(s, 3));
        req.track = static_cast<MediaTrack*>(Ctx(s, 1));
        req.fx_index = req.track ? static_cast<int>(reinterpret_cast<INT_PTR>(Ctx(s, 6))) : -1;
        req.fx_instance = s;
        req.parms = (parmlist && nparms > 0) ? parmlist : nullptr;
        req.nparms = req.parms ? nparms : 0;
        req.override_width = s->override_w.load();   // the FX's saved output size, 0 = none
        req.override_height = s->override_h.load();

        int w = 0;
        int h = 0;
        api->frame_size(&req, &w, &h);
        if (w <= 0 || h <= 0) return nullptr;  // no picture at this time
        if (w < kMinSize) w = kMinSize;
        if (h < kMinSize) h = kMinSize;
        if (w > kMaxSize) w = kMaxSize;
        if (h > kMaxSize) h = kMaxSize;

        IVideoFrame* vf = vproc->newVideoFrame(w, h, kFourccRgba);
        if (!vf) return nullptr;
        if (vf->get_fmt() != kFourccRgba || !vf->get_bits() || vf->get_w() <= 0 || vf->get_h() <= 0) {
            vf->Release();  // never write a layout we do not know
            return nullptr;
        }
        req.width = vf->get_w();
        req.height = vf->get_h();
        req.pixels = reinterpret_cast<unsigned char*>(vf->get_bits());
        req.row_bytes = FrameRowBytes(vf);
        if (api->render_frame(&req) == 0) {
            vf->Release();
            return nullptr;
        }
        return vf;
    } catch (...) {
        return nullptr;
    }
}

// REAPER's video thread asks for the current value of a plug-in parameter (idx 0 = the
// first plug-in parameter, not wet: spike 11-0 log).
bool GetParameterValue(IREAPERVideoProcessor* vproc, int idx, double* v)
{
    Instance* s = vproc ? static_cast<Instance*>(vproc->userdata) : nullptr;
    if (!s || !v || idx < 0 || idx >= rav::vcam::kFxParamCount) return false;
    *v = (idx == rav::vcam::kRefreshParam) ? s->refresh.load() : s->values[idx].load();
    return true;
}

// Main thread. Creates the video processor once, when REAPER gives the context.
void TryAttach(Instance* s)
{
    if (s->vproc || !s->rec || !s->rec->GetFunc) return;
    auto create = reinterpret_cast<CreateVideoProcessorFn>(s->rec->GetFunc("video_CreateVideoProcessor"));
    s->get_ctx = reinterpret_cast<GetReaperContextFn>(s->rec->GetFunc("clap_get_reaper_context"));
    void* fxdsp = Ctx(s, 4);
    if (!create || !fxdsp) return;  // scan process or not REAPER: stay inert, silently

    ResolveApi(s);
    IREAPERVideoProcessor* vp = create(fxdsp, IREAPERVideoProcessor::REAPER_VIDEO_PROCESSOR_VERSION);
    if (!vp) return;
    vp->userdata = s;
    vp->get_parameter_value = &GetParameterValue;
    vp->process_frame = &ProcessFrame;
    s->vproc = vp;
}

// ---- Parameters (Story 11-3) ----------------------------------------------------------
// CLAP id == index == REAPER's parameter index; all declared 0..1 so a parameter value,
// an envelope value and a parmlist value are the same number.
void SetValue(Instance* s, clap_id id, double v)
{
    if (id == static_cast<clap_id>(rav::vcam::kRefreshParam)) {
        s->refresh.store(std::isfinite(v) ? std::clamp(v, 0.0, 1.0) : 0.0);
        return;
    }
    if (id >= static_cast<clap_id>(kParamCount)) return;
    s->values[id].store(rav::vcam::Sanitize(static_cast<int>(id), v));
}

void ApplyParamEvents(Instance* s, const clap_input_events_t* in)
{
    if (!in || !in->size || !in->get) return;
    const uint32_t n = in->size(in);
    for (uint32_t i = 0; i < n; ++i) {
        const clap_event_header_t* h = in->get(in, i);
        if (!h || h->space_id != CLAP_CORE_EVENT_SPACE_ID || h->type != CLAP_EVENT_PARAM_VALUE) continue;
        if (h->size < sizeof(clap_event_param_value_t)) continue;
        const auto* ev = reinterpret_cast<const clap_event_param_value_t*>(h);
        SetValue(s, ev->param_id, ev->value);
    }
}

uint32_t ParamsCount(const clap_plugin_t*) { return rav::vcam::kFxParamCount; }

bool ParamsGetInfo(const clap_plugin_t*, uint32_t index, clap_param_info_t* info)
{
    if (index >= static_cast<uint32_t>(rav::vcam::kFxParamCount) || !info) return false;
    std::memset(info, 0, sizeof(*info));
    if (index == static_cast<uint32_t>(rav::vcam::kRefreshParam)) {
        // Declared like the camera (automatable, 0..1) so REAPER surely passes it in the
        // parmlist and re-asks for its frames when it moves. Named so nobody automates it.
        info->id = index;
        info->flags = CLAP_PARAM_IS_AUTOMATABLE;
        std::snprintf(info->name, sizeof(info->name), "Refresh (RAV internal)");
        std::snprintf(info->module, sizeof(info->module), "Internal");
        info->min_value = 0.0;
        info->max_value = 1.0;
        info->default_value = 0.0;
        return true;
    }
    info->id = index;
    info->flags = CLAP_PARAM_IS_AUTOMATABLE;
    info->cookie = nullptr;
    std::snprintf(info->name, sizeof(info->name), "%s", rav::vcam::ParamName(static_cast<int>(index)));
    std::snprintf(info->module, sizeof(info->module), "Camera");
    info->min_value = 0.0;
    info->max_value = 1.0;
    info->default_value = rav::vcam::DefaultNorm(static_cast<int>(index));
    return true;
}

bool ParamsGetValue(const clap_plugin_t* p, clap_id id, double* out)
{
    if (id >= static_cast<clap_id>(rav::vcam::kFxParamCount) || !out) return false;
    *out = (id == static_cast<clap_id>(rav::vcam::kRefreshParam)) ? Self(p)->refresh.load()
                                                                 : Self(p)->values[id].load();
    return true;
}

bool ParamsValueToText(const clap_plugin_t*, clap_id id, double v, char* out, uint32_t cap)
{
    if (id >= static_cast<clap_id>(rav::vcam::kFxParamCount) || !out || cap == 0) return false;
    if (id == static_cast<clap_id>(rav::vcam::kRefreshParam)) {
        std::snprintf(out, cap, "%.3f", v);
        return true;
    }
    rav::vcam::FormatValue(static_cast<int>(id), v, out, cap);
    return true;
}

bool ParamsTextToValue(const clap_plugin_t*, clap_id id, const char* text, double* out)
{
    if (id == static_cast<clap_id>(rav::vcam::kRefreshParam) && text && out) {
        *out = std::clamp(std::atof(text), 0.0, 1.0);
        return true;
    }
    if (id >= static_cast<clap_id>(kParamCount)) return false;
    return rav::vcam::ParseValue(static_cast<int>(id), text, out);
}

void ParamsFlush(const clap_plugin_t* p, const clap_input_events_t* in, const clap_output_events_t*)
{
    ApplyParamEvents(Self(p), in);
}

const clap_plugin_params_t kParamsExt = {ParamsCount, ParamsGetInfo, ParamsGetValue, ParamsValueToText,
                                         ParamsTextToValue, ParamsFlush};

// ---- State (Story 11-3) ------------------------------------------------------------------
// Main thread. The camera values come from the atomics, the rest from `meta`.
bool SaveState(Instance* s, const clap_ostream_t* stream, bool preset)
{
    if (!stream || !stream->write) return false;
    rav::VideoFxState st = s->meta;
    st.kind = preset ? rav::VideoFxStateKind::Preset : rav::VideoFxStateKind::Full;
    st.has_values = true;
    for (int p = 0; p < kParamCount; ++p) st.values[p] = s->values[p].load();
    st.override_width = s->override_w.load();
    st.override_height = s->override_h.load();
    const std::string text = rav::SerializeVideoFxState(st);
    size_t done = 0;
    while (done < text.size()) {
        const int64_t n = stream->write(stream, text.data() + done, text.size() - done);
        if (n <= 0) return false;
        done += static_cast<size_t>(n);
    }
    return true;
}

// Main thread. A blob that is not ours (or garbled) is refused and changes nothing.
bool LoadState(Instance* s, const clap_istream_t* stream, bool preset)
{
    if (!stream || !stream->read) return false;
    std::string bytes;
    char buf[4096];
    for (;;) {
        const int64_t n = stream->read(stream, buf, sizeof(buf));
        if (n < 0) return false;
        if (n == 0) break;
        bytes.append(buf, static_cast<size_t>(n));
        if (bytes.size() > kMaxStateBytes) return false;
    }
    rav::VideoFxState in;
    if (!rav::ParseVideoFxState(bytes.data(), bytes.size(), &in)) return false;

    rav::VideoFxState cur = s->meta;
    cur.has_values = true;
    for (int p = 0; p < kParamCount; ++p) cur.values[p] = s->values[p].load();
    cur.override_width = s->override_w.load();
    cur.override_height = s->override_h.load();
    const bool values_changed = rav::MergeVideoFxState(in, preset, &cur);

    // Only when the blob changed them: storing back the values read above could undo an
    // automation event the audio thread applied meanwhile (a meta write during playback).
    if (values_changed) {
        for (int p = 0; p < kParamCount; ++p) s->values[p].store(cur.values[p]);
    }
    const bool ov = rav::VideoFxOverrideValid(cur.override_width, cur.override_height);
    s->override_w.store(ov ? cur.override_width : 0);
    s->override_h.store(ov ? cur.override_height : 0);
    s->meta = cur;
    // Tell REAPER the values changed (it then re-reads them for its UI and automation).
    if (values_changed && s->host_params && s->host_params->rescan) {
        s->host_params->rescan(s->host, CLAP_PARAM_RESCAN_VALUES);
    }
    return true;
}

bool StateSave(const clap_plugin_t* p, const clap_ostream_t* stream) { return SaveState(Self(p), stream, false); }
bool StateLoad(const clap_plugin_t* p, const clap_istream_t* stream) { return LoadState(Self(p), stream, false); }

bool StateContextSave(const clap_plugin_t* p, const clap_ostream_t* stream, uint32_t context)
{
    return SaveState(Self(p), stream, context == CLAP_STATE_CONTEXT_FOR_PRESET);
}

bool StateContextLoad(const clap_plugin_t* p, const clap_istream_t* stream, uint32_t context)
{
    return LoadState(Self(p), stream, context == CLAP_STATE_CONTEXT_FOR_PRESET);
}

const clap_plugin_state_t kStateExt = {StateSave, StateLoad};
const clap_plugin_state_context_t kStateContextExt = {StateContextSave, StateContextLoad};

// ---- Audio ports --------------------------------------------------------------------
uint32_t PortsCount(const clap_plugin_t*, bool) { return 1; }

bool PortsGet(const clap_plugin_t*, uint32_t index, bool is_input, clap_audio_port_info_t* info)
{
    if (index != 0 || !info) return false;
    std::memset(info, 0, sizeof(*info));
    info->id = 0;
    std::snprintf(info->name, sizeof(info->name), "%s", is_input ? "Input" : "Output");
    info->flags = CLAP_AUDIO_PORT_IS_MAIN;
    info->channel_count = 2;
    info->port_type = CLAP_PORT_STEREO;
    info->in_place_pair = 0;
    return true;
}

const clap_plugin_audio_ports_t kPortsExt = {PortsCount, PortsGet};

// ---- Plug-in ------------------------------------------------------------------------
bool PluginInit(const clap_plugin_t* p)
{
    Instance* s = Self(p);
    if (s->host->get_extension) {
        s->rec = static_cast<const reaper_plugin_info_t*>(s->host->get_extension(s->host, "cockos.reaper_extension"));
        s->host_params = static_cast<const clap_host_params_t*>(s->host->get_extension(s->host, CLAP_EXT_PARAMS));
    }
    TryAttach(s);
    return true;
}

void PluginDestroy(const clap_plugin_t* p)
{
    Instance* s = Self(p);
    if (s->vproc) {
        // REAPER's own deleting destructor (virtual): stops the frame callbacks first.
        delete s->vproc;
        s->vproc = nullptr;
    }
    const RavVideoFxApi* api = s->api.load();
    if (api && api->release_instance) api->release_instance(s);
    if (s->api_module) FreeLibrary(s->api_module);
    delete s;
}

bool PluginActivate(const clap_plugin_t* p, double, uint32_t, uint32_t)
{
    Instance* s = Self(p);
    TryAttach(s);
    if (s->vproc) ResolveApi(s);
    return true;
}

void PluginDeactivate(const clap_plugin_t*) {}
bool PluginStartProcessing(const clap_plugin_t*) { return true; }
void PluginStopProcessing(const clap_plugin_t*) {}
void PluginReset(const clap_plugin_t*) {}

clap_process_status PluginProcess(const clap_plugin_t* p, const clap_process_t* proc)
{
    if (!proc) return CLAP_PROCESS_CONTINUE;
    // Automation reaches the parameters as value events (CLAP_EVENT_PARAM_MOD is not
    // handled); the picture itself uses the parmlist REAPER passes per frame, these only
    // keep get_value current.
    ApplyParamEvents(Self(p), proc->in_events);
    if (proc->audio_outputs_count < 1 || !proc->audio_outputs) return CLAP_PROCESS_CONTINUE;
    const clap_audio_buffer_t& out = proc->audio_outputs[0];
    const clap_audio_buffer_t* in =
        (proc->audio_inputs_count > 0 && proc->audio_inputs) ? &proc->audio_inputs[0] : nullptr;
    const size_t bytes = sizeof(float) * proc->frames_count;
    for (uint32_t c = 0; c < out.channel_count; ++c) {
        float* dst = out.data32 ? out.data32[c] : nullptr;
        if (!dst) continue;
        const float* src = (in && c < in->channel_count && in->data32) ? in->data32[c] : nullptr;
        if (src) {
            if (src != dst) std::memcpy(dst, src, bytes);
        } else {
            std::memset(dst, 0, bytes);
        }
    }
    return CLAP_PROCESS_CONTINUE;
}

const void* PluginGetExtension(const clap_plugin_t*, const char* id)
{
    if (!id) return nullptr;
    if (!std::strcmp(id, CLAP_EXT_AUDIO_PORTS)) return &kPortsExt;
    if (!std::strcmp(id, CLAP_EXT_PARAMS)) return &kParamsExt;
    if (!std::strcmp(id, CLAP_EXT_STATE)) return &kStateExt;
    if (!std::strcmp(id, CLAP_EXT_STATE_CONTEXT)) return &kStateContextExt;
    return nullptr;
}

void PluginOnMainThread(const clap_plugin_t*) {}

// ---- Factory + entry ----------------------------------------------------------------
uint32_t FactoryCount(const clap_plugin_factory_t*) { return 1; }

const clap_plugin_descriptor_t* FactoryDescriptor(const clap_plugin_factory_t*, uint32_t index)
{
    return index == 0 ? &kDescriptor : nullptr;
}

const clap_plugin_t* FactoryCreate(const clap_plugin_factory_t*, const clap_host_t* host, const char* plugin_id)
{
    if (!host || !plugin_id || std::strcmp(plugin_id, RAV_VIDEO_FX_CLAP_ID) != 0) return nullptr;
    if (!clap_version_is_compatible(host->clap_version)) return nullptr;
    Instance* s = new (std::nothrow) Instance();
    if (!s) return nullptr;
    s->host = host;
    s->plugin.desc = &kDescriptor;
    s->plugin.plugin_data = s;
    s->plugin.init = PluginInit;
    s->plugin.destroy = PluginDestroy;
    s->plugin.activate = PluginActivate;
    s->plugin.deactivate = PluginDeactivate;
    s->plugin.start_processing = PluginStartProcessing;
    s->plugin.stop_processing = PluginStopProcessing;
    s->plugin.reset = PluginReset;
    s->plugin.process = PluginProcess;
    s->plugin.get_extension = PluginGetExtension;
    s->plugin.on_main_thread = PluginOnMainThread;
    return &s->plugin;
}

const clap_plugin_factory_t kFactory = {FactoryCount, FactoryDescriptor, FactoryCreate};

bool EntryInit(const char*) { return true; }  // nothing to do: the scan process stays inert
void EntryDeinit() {}

const void* EntryGetFactory(const char* factory_id)
{
    return (factory_id && !std::strcmp(factory_id, CLAP_PLUGIN_FACTORY_ID)) ? &kFactory : nullptr;
}

}  // namespace

extern "C" CLAP_EXPORT const clap_plugin_entry_t clap_entry = {CLAP_VERSION_INIT, EntryInit, EntryDeinit,
                                                               EntryGetFactory};
