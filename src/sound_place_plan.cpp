// SPDX-License-Identifier: MIT
//
// See sound_place_plan.h (13-6).

#include "sound_place_plan.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace rav {
namespace {

// Splits on '\n'; each line keeps its '\r' (if any) so a rebuild is byte-identical.
std::vector<std::string> SplitLines(const std::string& s)
{
    std::vector<std::string> out;
    size_t start = 0;
    for (;;) {
        const size_t nl = s.find('\n', start);
        if (nl == std::string::npos) {
            out.push_back(s.substr(start));
            break;
        }
        out.push_back(s.substr(start, nl - start));
        start = nl + 1;
    }
    return out;
}

size_t IndentOf(const std::string& line)
{
    size_t i = 0;
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
    return i;
}

bool StartsWithWord(const std::string& line, size_t at, const char* word)
{
    size_t n = 0;
    while (word[n]) {
        if (at + n >= line.size() || line[at + n] != word[n]) return false;
        ++n;
    }
    return at + n == line.size() || line[at + n] == ' ' || line[at + n] == '\t' || line[at + n] == '\r';
}

std::string StripCr(std::string s)
{
    if (!s.empty() && s.back() == '\r') s.pop_back();
    return s;
}

}  // namespace

bool IsFolderItemRule(const FolderItemFacts& facts)
{
    if (!facts.on_folder_track || !facts.empty_source) return false;
    return facts.children_inside || facts.nvk_item_type == "folder";
}

std::string NvkItemTypeFromChunk(const std::string& chunk)
{
    const std::vector<std::string> lines = SplitLines(chunk);
    int depth = 0;        // block depth (the <ITEM block = 1)
    int exti_depth = -1;  // depth of the item's <EXTI block while inside it
    for (const std::string& raw : lines) {
        const std::string line = StripCr(raw);
        const size_t      i = IndentOf(line);
        if (i >= line.size()) continue;
        if (line[i] == '<') {
            ++depth;
            if (exti_depth < 0 && depth == 2 && StartsWithWord(line, i + 1, "EXTI")) exti_depth = depth;
            continue;
        }
        if (line[i] == '>') {
            if (depth == exti_depth) exti_depth = -1;
            --depth;
            continue;
        }
        if (exti_depth > 0 && depth == exti_depth && StartsWithWord(line, i, "nvk_item_type")) {
            size_t v = i + 13;
            while (v < line.size() && (line[v] == ' ' || line[v] == '\t')) ++v;
            std::string value = line.substr(v);
            while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) value.pop_back();
            if (value.size() >= 2 && (value.front() == '"' || value.front() == '\'' || value.front() == '`')
                && value.back() == value.front())
                value = value.substr(1, value.size() - 2);
            return value;
        }
    }
    return {};
}

std::string PrepareCopyChunk(const std::string& chunk, const std::string& item_guid,
                             const std::function<std::string()>& new_guid)
{
    const std::vector<std::string> lines = SplitLines(chunk);
    std::string                    out;
    out.reserve(chunk.size() + 64);
    for (size_t n = 0; n < lines.size(); ++n) {
        std::string  line = lines[n];
        const bool   cr = !line.empty() && line.back() == '\r';
        const size_t i = IndentOf(line);
        const std::string indent = line.substr(0, i);
        const char*       eol = cr ? "\r" : "";
        if (StartsWithWord(line, i, "IGUID"))
            line = indent + "IGUID " + item_guid + eol;
        else if (new_guid && StartsWithWord(line, i, "GUID"))
            line = indent + "GUID " + new_guid() + eol;
        else if (new_guid && StartsWithWord(line, i, "FXID"))
            line = indent + "FXID " + new_guid() + eol;
        else if (StartsWithWord(line, i, "SEL"))
            line = indent + "SEL 0" + eol;
        out += line;
        if (n + 1 < lines.size()) out += '\n';
    }
    return out;
}

bool SpanInside(double outer_pos, double outer_len, double inner_pos, double inner_len, double tol)
{
    return inner_pos >= outer_pos - tol && inner_pos + inner_len <= outer_pos + outer_len + tol;
}

double PileMemberPosition(double anchor, double src_anchor_pos, double member_src_pos)
{
    return anchor + (member_src_pos - src_anchor_pos);
}

SliceApply SliceApplyFor(const SoundFrame& frame)
{
    SliceApply a;
    if (!std::isfinite(frame.start) || !std::isfinite(frame.length) || !(frame.length > 0.0)) return a;
    a.set = true;
    a.start_offs = frame.start;
    a.length = frame.length;
    a.snap_offset = 0.0;
    return a;
}

double PlacementPosition(double event_t, double sync_offset, double blend_ms, double hand_offset_s)
{
    const double p = event_t - sync_offset + blend_ms / 1000.0 + hand_offset_s;
    return (std::isfinite(p) && p > 0.0) ? p : 0.0;
}

double ItemGainFor(double gain_db)
{
    if (std::isnan(gain_db)) return 1.0;
    return std::pow(10.0, gain_db / 20.0);
}

std::string PlacedItemRefusal(bool found, bool has_tag, bool folder, Ownership own, bool edit)
{
    if (!found) return "item missing";
    if (!has_tag) return "not a placed item (no RAV_SND tag)";
    if (folder) return "folder item";
    if (edit && own == Ownership::Attached) return "attached by the user";
    return {};
}

bool PlacedCopyMuted(bool src_muted, bool src_is_library_source)
{
    return src_muted && !src_is_library_source;
}

double LibrarySourcePosition(double project_end, bool track_has_library_source, double last_library_source_end)
{
    // Never inside the content: after this track's last library source only when that is past it.
    return std::max(project_end, track_has_library_source ? last_library_source_end : project_end) + 1.0;
}

}  // namespace rav
