# Drop the generated ReaImGui binding header here

The spike's `src/spike_main.cpp` does:

```cpp
#define REAIMGUIAPI_IMPLEMENT
#include "reaper_imgui_functions.h"
```

This header is **generated** by the ReaImGui build (it is not in the ReaImGui
source tree), so it is not vendored in this repo. Obtain it one of two ways:

1. **From a release** — download `reaper_imgui_functions.h` from
   <https://github.com/cfillion/reaimgui/releases> (use the **v0.10.0.5** asset to
   match the pin), and place it next to this file as
   `extern/reaimgui/include/reaper_imgui_functions.h`.

2. **From REAPER** — with ReaImGui installed, run the REAPER action
   *"[developer] Write C++ API functions header"* and copy the produced
   `reaper_imgui_functions.h` here.

The header provides the `ImGui::` C++ namespace wrappers (`ImGui::init`,
`ImGui::CreateContext`, `ImGui::Begin`, `ImGui::Image`, `ImGui::CreateImageFromSize`,
`ImGui::Image_SetPixels_Array`, …) and the `reaper_array` type the spike uses.

(Production Phase 0.5 will vendor a pinned copy under `extern/reaimgui/include/`
per `extern/VENDORED.md` once the header source/version is settled.)
