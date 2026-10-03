// SPDX-License-Identifier: MIT
//
// The viewer's display settings, as the video FX must reproduce them (Story 11-2): the
// picture REAPER renders uses the same light, floor, grid, shadow, normal maps, MSAA and
// background as RAV's viewer. The viewer publishes them every frame it draws; the video
// thread reads the last published copy, so a closed viewer keeps its last settings. Before
// the viewer was ever opened, the renderer defaults (a fresh viewer's look) apply.
// Plain data, glm only; publish/read are thread-safe.

#pragma once

#ifdef _WIN32

#include <glm/glm.hpp>

namespace rav {

struct DisplaySettings {
    glm::vec3 light_color{1.0f};
    glm::vec3 light_dir{0.0f, 1.0f, 0.0f};
    float     ambient         = 0.5f;
    float     spec_strength   = 1.5f;
    float     normal_strength = 1.5f;
    bool      floor_visible   = true;
    float     grid_step_m     = 1.0f;
    int       shadow_quality  = 2;     // ShadowQuality: 0 Off, 1 Low, 2 Mid, 3 High
    bool      normal_maps     = true;
    int       msaa_samples    = 4;     // 0 = off
    glm::vec3 background{0.10f, 0.10f, 0.12f};
};

// Viewer (UI thread): the settings it just drew with. Returns true when they differ from
// the last published ones (the video FX pictures then need a refresh).
bool PublishDisplaySettings(const DisplaySettings& s);

// Any thread: the last published settings, or the renderer defaults if none yet.
DisplaySettings LastDisplaySettings();

}  // namespace rav

#endif  // _WIN32
