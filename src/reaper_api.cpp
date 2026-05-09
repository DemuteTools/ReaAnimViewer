// SPDX-License-Identifier: MIT
//
// Single translation unit that materializes the Reaper API function
// pointers declared as `extern` everywhere else. All other source files
// include `reaper_api.h` only.

#define REAPERAPI_IMPLEMENT
#include "reaper_api.h"
