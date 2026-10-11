// SPDX-License-Identifier: MIT
//
// See tagging_populate_ui.h. Stub bodies (13-2): each returns a neutral value and is replaced by 13-8.

#include "tagging_populate_ui.h"

#ifdef _WIN32


namespace rav {

void DrawRuleSoundSection(const TaggingSoundContext&)
{
    // STUB (13-8)
}

void DrawItemSoundValuesSection(const TaggingSoundContext&)
{
    // STUB (13-8)
}

void DrawEventSoundSection(const TaggingSoundContext&, const ShownEvent&)
{
    // STUB (13-8)
}

bool SoundFooterVisible(const TaggingSoundContext&)
{
    return false;  // STUB (13-8)
}

float SoundFooterHeight(const TaggingSoundContext&)
{
    return 0.0f;  // STUB (13-8)
}

void DrawPopulateRow(const TaggingSoundContext&, float)
{
    // STUB (13-8)
}

void DrawSoundReportLine(const TaggingSoundContext&)
{
    // STUB (13-8)
}

void DrawSoundStripGlyph(ImDrawList*, const TaggingModel&, const ShownEvent&, float, float)
{
    // STUB (13-8)
}

}  // namespace rav

#endif  // _WIN32
