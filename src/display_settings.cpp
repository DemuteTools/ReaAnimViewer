// SPDX-License-Identifier: MIT
//
// See display_settings.h.

#include "display_settings.h"

#ifdef _WIN32

#include <mutex>

#include "renderer.h"

namespace rav {
namespace {

std::mutex      g_mutex;
bool            g_published = false;  // guarded by g_mutex
DisplaySettings g_last;               // guarded by g_mutex

// The renderer's own defaults, read off a default-constructed instance (no GL object is
// created or freed by constructing/destroying an un-initialized Renderer), so they can
// never drift from what a fresh viewer shows.
DisplaySettings RendererDefaults()
{
    Renderer fresh;
    return fresh.CurrentDisplaySettings();
}

}  // namespace

bool PublishDisplaySettings(const DisplaySettings& s)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    const DisplaySettings& o = g_last;
    const bool changed = !g_published || s.light_color != o.light_color || s.light_dir != o.light_dir ||
                         s.ambient != o.ambient || s.spec_strength != o.spec_strength ||
                         s.normal_strength != o.normal_strength || s.floor_visible != o.floor_visible ||
                         s.grid_step_m != o.grid_step_m || s.shadow_quality != o.shadow_quality ||
                         s.normal_maps != o.normal_maps || s.msaa_samples != o.msaa_samples ||
                         s.background != o.background;
    g_last = s;
    g_published = true;
    return changed;
}

DisplaySettings LastDisplaySettings()
{
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_published) return g_last;
    }
    static const DisplaySettings defaults = RendererDefaults();  // thread-safe init (C++11)
    return defaults;
}

}  // namespace rav

#endif  // _WIN32
