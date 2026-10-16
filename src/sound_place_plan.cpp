// SPDX-License-Identifier: MIT
//
// See sound_place_plan.h (13-6).

#include "sound_place_plan.h"

#include "sound_entries.h"
#include "sound_zones.h"  // 13-62h: a divided Sound placed from its zones

#include <algorithm>
#include <cmath>
#include <cstdio>
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

double PileAnchorOf(double member_pos, double src_anchor_pos, double member_src_pos)
{
    return member_pos - (member_src_pos - src_anchor_pos);
}

void PlacedAnchorOf(const PlacedTag& tag, double item_pos, double item_snap, double* anchor_pos, double* sync)
{
    if (anchor_pos) *anchor_pos = tag.has_anchor ? item_pos - tag.ao : item_pos;
    if (sync) *sync = tag.has_anchor ? tag.sy : item_snap;
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

FadeApply FadeApplyFor(const SoundData& data, int variation, bool folder_source)
{
    FadeApply fa;
    if (folder_source || variation < 0 || variation >= static_cast<int>(data.frames.size())) return fa;
    const FrameFades f = EffectiveFades(data, data.frames[static_cast<size_t>(variation)]);
    const bool whole = data.mode == DetectMode::Whole;
    fa.set_in = !whole || f.own_in;
    fa.set_out = !whole || f.own_out;
    if (fa.set_in) {
        fa.in = f.in;
        fa.in_shape = f.in_shape;
        fa.shape_in = !whole || f.in_shape != 0;
    }
    if (fa.set_out) {
        fa.out = f.out;
        fa.out_shape = f.out_shape;
        fa.shape_out = !whole || f.out_shape != 0;
    }
    return fa;
}

void FadesOnItem(const FadeApply& fa, double playrate, double item_len, double cur_in, double cur_out,
                 double* in, double* out)
{
    const double rate = (std::isfinite(playrate) && playrate > 0.0) ? playrate : 1.0;
    double a = fa.set_in ? fa.in / rate : cur_in;
    double b = fa.set_out ? fa.out / rate : cur_out;
    ClampFadePair(item_len, &a, &b);
    if (in) *in = a;
    if (out) *out = b;
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

int ChanModeFor(MonoMode mono)
{
    switch (mono) {
    case MonoMode::Sum: return 2;
    case MonoMode::Left: return 3;
    case MonoMode::Right: return 4;
    case MonoMode::Off: break;
    }
    return 0;
}

MonoMode MonoForChanMode(int chan_mode)
{
    switch (chan_mode) {
    case 2: return MonoMode::Sum;
    case 3: return MonoMode::Left;
    case 4: return MonoMode::Right;
    default: break;
    }
    return MonoMode::Off;
}

PlayrateFold FoldPlayrate(double rate, double item_len, double snap)
{
    PlayrateFold f;
    f.rate = (std::isfinite(rate) && rate > 0.0) ? rate : 1.0;
    f.length = item_len * f.rate;
    f.snap = snap * f.rate;
    return f;
}

std::string StripTakeFxAndStretch(const std::string& chunk)
{
    const std::vector<std::string> lines = SplitLines(chunk);
    std::string                    out;
    out.reserve(chunk.size());
    int  depth = 0;       // block depth (the <ITEM block = 1)
    int  skip_depth = -1; // depth of the <TAKEFX block being dropped
    bool first = true;
    for (const std::string& raw : lines) {
        const std::string line = StripCr(raw);
        const size_t      i = IndentOf(line);
        bool              keep = skip_depth < 0;
        if (i < line.size() && line[i] == '<') {
            ++depth;
            if (skip_depth < 0 && depth == 2 && StartsWithWord(line, i + 1, "TAKEFX")) {
                skip_depth = depth;
                keep = false;
            }
        } else if (i < line.size() && line[i] == '>') {
            if (depth == skip_depth) {
                skip_depth = -1;
                keep = false;
            }
            --depth;
        } else if (keep && depth == 1 && (StartsWithWord(line, i, "TAKE_FX_HAVE") || StartsWithWord(line, i, "SM"))) {
            keep = false;
        }
        if (!keep) continue;
        if (!first) out += '\n';
        out += raw;
        first = false;
    }
    return out;
}

std::string PlacementDropsText(bool take_fx, bool pitch, bool rate, bool pan)
{
    std::vector<std::string> what;
    if (take_fx) what.push_back("take FX");
    if (pitch) what.push_back("pitch");
    if (rate) what.push_back("rate");
    if (pan) what.push_back("pan");
    if (what.empty()) return {};
    std::string list;
    for (size_t k = 0; k < what.size(); ++k) {
        if (k > 0) list += k + 1 == what.size() ? " and " : ", ";
        list += what[k];
    }
    return "Placed without the source item's " + list + " (RAV plays the file itself)";
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

PlacedVolumes PlacedCopyVolumes(bool folder, double src_item_vol, double take_vol, double gain)
{
    PlacedVolumes v;
    if (folder) {
        v.item = src_item_vol * gain;
        v.take = take_vol;
        return v;
    }
    v.item = gain;
    v.take = take_vol < 0.0 ? -1.0 : 1.0;
    v.set_take = true;
    return v;
}

KeptVolume KeptItemVolume(const std::vector<double>& piece_db, bool pile, int items, bool has_g, double g, int pi,
                          double cur_vol)
{
    KeptVolume k;
    if (piece_db.empty()) return k;
    if (has_g) {
        if (pi < 0 || pi >= static_cast<int>(piece_db.size()) || !std::isfinite(g)) return k;
        const double want = piece_db[static_cast<size_t>(pi)];
        if (!std::isfinite(want) || std::fabs(want - g) <= 1e-9 || !std::isfinite(cur_vol)) return k;
        k.set = true;
        k.vol = cur_vol * std::pow(10.0, (want - g) / 20.0);
        k.g = want;
        k.pi = pi;
        return k;
    }
    // An item placed before its tag recorded its gain: only a lone plain item can be set as placed now.
    if (pile || items != 1 || piece_db.size() != 1 || !std::isfinite(piece_db[0])) return k;
    k.set = true;
    k.vol = ItemGainFor(piece_db[0]);
    k.g = piece_db[0];
    k.pi = 0;
    return k;
}

SliceApply FileSliceFor(const SoundData& data, int variation)
{
    if (variation < 0 || variation >= static_cast<int>(data.frames.size())) return SliceApply{};
    return SliceApplyFor(data.frames[static_cast<size_t>(variation)]);
}

FadeApply FileFadeApplyFor(const SoundData& data, int variation)
{
    FrameFades f;
    if (variation >= 0 && variation < static_cast<int>(data.frames.size())) {
        f = EffectiveFades(data, data.frames[static_cast<size_t>(variation)]);
    } else {
        f.in = std::max(0.0, data.fade_in);
        f.out = std::max(0.0, data.fade_out);
    }
    FadeApply fa;
    fa.set_in = fa.set_out = true;
    fa.shape_in = fa.shape_out = true;
    fa.in = f.in;
    fa.out = f.out;
    fa.in_shape = f.in_shape;
    fa.out_shape = f.out_shape;
    return fa;
}

FileItemFields FileItemFieldsFor(double file_len, const SliceApply& slice, const FadeApply& fades, double gain_db,
                                 double project_t, double blend_ms, double offset_s)
{
    FileItemFields o;
    const bool known = std::isfinite(file_len) && file_len > 0.0;
    double     start = slice.set ? slice.start_offs : 0.0;
    double     len = slice.set ? slice.length : (known ? file_len : 0.0);
    if (!std::isfinite(start) || start < 0.0) start = 0.0;
    if (known) {
        if (start >= file_len) return o;
        len = std::min(len, file_len - start);
    }
    if (!std::isfinite(len) || !(len > 0.0)) return o;
    double snap = slice.set && std::isfinite(slice.snap_offset) ? slice.snap_offset : 0.0;
    snap = std::min(std::max(snap, 0.0), len);
    o.ok = true;
    o.start_offs = start;
    o.length = len;
    o.snap_offset = snap;
    o.position = PlacementPosition(project_t, snap, blend_ms, offset_s);
    o.volume = ItemGainFor(gain_db);
    FadesOnItem(fades, 1.0, len, 0.0, 0.0, &o.fade_in, &o.fade_out);
    return o;
}

namespace {

// 13-62h: the part zones of variation `variation` placed for `part_value` ("" = every part zone), in time order.
std::vector<ZoneRef> PlacedZones(const SoundData& data, int variation, const std::string& part_value)
{
    std::vector<ZoneRef> out;
    const SoundFrame&    f = data.frames[static_cast<size_t>(variation)];
    if (!f.on) return out;
    for (size_t k = 0; k < f.zones.size(); ++k) {
        const EditZone& z = f.zones[k];
        if (!z.IsGap() && (part_value.empty() || z.part == part_value)) out.push_back({variation, static_cast<int>(k)});
    }
    return out;
}

// 13-62h: the zone an event lands by: the first placed zone with a snap offset of its own (a hand sync point, also
// one migrated from the old model), else the first placed zone.
ZoneRef AnchorZone(const SoundData& data, const std::vector<ZoneRef>& zones)
{
    for (const ZoneRef& r : zones)
        if (ZoneOf(data, r)->snap) return r;
    return zones.front();
}

// 13-62h: the pieces of a zoned placement (PieceApplyFor).
std::vector<PieceApply> ZonePieces(const SoundData& data, int variation, const std::string& part_value,
                                   const PeakData* peaks)
{
    std::vector<PieceApply>    out;
    const std::vector<ZoneRef> zones = PlacedZones(data, variation, part_value);
    if (zones.empty()) return out;
    const SoundFrame& f = data.frames[static_cast<size_t>(variation)];
    const ZoneRef     anchor = AnchorZone(data, zones);
    std::vector<double> heard;  // each piece's start in heard time
    auto placed = [&](const ZoneRef& q) { return std::find(zones.begin(), zones.end(), q) != zones.end(); };
    for (const ZoneRef& r : zones) {
        const EditZone& z = *ZoneOf(data, r);
        const ZoneRef   lgr = GapBefore(data, r), rgr = GapAfter(data, r);
        const EditZone* lg = ZoneOf(data, lgr);
        const EditZone* rg = ZoneOf(data, rgr);
        // 13-62h review (R3): a side whose Gap leads to a zone this event does not place (another part) is cut at
        // the Gap's edge with the At cuts fade: it would crossfade against nothing and play the jumped audio.
        const bool   cut_in = lg && !placed(GapLeft(data, lgr));
        const bool   cut_out = rg && !placed(GapRight(data, rgr));
        const double a = lg && !cut_in ? lg->b - lg->xr : z.a;  // as HeardPieces cuts it
        const double b = rg && !cut_out ? rg->a + rg->xl : z.b;
        if (!(b > a)) continue;
        double jumped = 0.0;  // the Gaps before it, jumped at playback
        for (const EditZone& g : f.zones)
            if (g.IsGap() && g.a < a - 1e-9) jumped += g.b - g.a;
        PieceApply pa;
        pa.slice.set = true;
        pa.slice.start_offs = a;
        pa.slice.length = b - a;
        pa.fades.set_in = pa.fades.set_out = true;
        pa.fades.shape_in = pa.fades.shape_out = true;
        const ZoneFadeInfo fi = cut_in ? ZoneCutFade(data, r, -1) : ZoneFade(data, r, -1);
        const ZoneFadeInfo fo = cut_out ? ZoneCutFade(data, r, 1) : ZoneFade(data, r, 1);
        pa.fades.in = fi.s;
        pa.fades.out = fo.s;
        // 13-62i: a zone's own fade keeps its shape (a frame's shaped fade migrated); any other is linear.
        pa.fades.in_shape = fi.src == FadeSrc::Own ? z.fade_in_shape : 0;
        pa.fades.out_shape = fo.src == FadeSrc::Own ? z.fade_out_shape : 0;
        pa.db = std::isfinite(z.db) ? z.db : 0.0;
        if (r == anchor) {  // the anchor, at its snap offset
            pa.anchor = true;
            const double t = ZoneSnap(data, r, peaks).t;
            pa.slice.snap_offset = std::isfinite(t) ? std::min(std::max(t - a, 0.0), b - a) : 0.0;
        }
        heard.push_back(a - jumped);
        out.push_back(pa);
    }
    double heard0 = heard.empty() ? 0.0 : heard.front();
    for (size_t k = 0; k < out.size(); ++k)
        if (out[k].anchor) heard0 = heard[k];
    if (!out.empty() && std::none_of(out.begin(), out.end(), [](const PieceApply& q) { return q.anchor; })) {
        out.front().anchor = true;  // the anchor zone gave no piece: the first one, at its start
        out.front().slice.snap_offset = 0.0;
    }
    for (size_t k = 0; k < out.size(); ++k) out[k].from_anchor = heard[k] - heard0;
    return out;
}

bool ZonedPlacement(const SoundData& data, int variation)
{
    return PlacedFromZones(data) && variation >= 0 && variation < static_cast<int>(data.frames.size());
}

}  // namespace

bool PieceSnapNeedsPeaks(const SoundData& data0, int variation, const std::string& part_value)
{
    if (!ZonedPlacement(data0, variation)) return false;
    SoundData                  tmp;
    const SoundData&           data = ZonesLaidOut(data0, &tmp);  // 13-62i
    const std::vector<ZoneRef> zones = PlacedZones(data, variation, part_value);
    if (zones.empty()) return false;
    const ZoneRef   anchor = AnchorZone(data, zones);
    const EditZone& z = *ZoneOf(data, anchor);
    // 13-62h review (R2): a snap measured and stored by the Edit window needs no peaks.
    return !z.snap && PartSyncMode(data, z.part) != SyncMode::SliceStart && !ZoneSnapStored(data, anchor);
}

std::vector<PieceApply> PieceApplyFor(const SoundData& data, int variation, const std::string& part_value, bool,
                                      const PeakData* peaks)
{
    // 13-62i: every Sound is placed from its zones (Takes mode too: its take's zones; a Sound not laid out yet is laid
    // out on a copy first, as it would be read).
    if (variation < 0 || variation >= static_cast<int>(data.frames.size())) return {};
    SoundData        tmp;
    const SoundData& d = ZonesLaidOut(data, &tmp);
    return ZonePieces(d, variation, part_value, peaks);
}

double PileGainDb(const std::vector<PieceApply>& pieces, double gain_db)
{
    for (const PieceApply& p : pieces)
        if (p.anchor) return gain_db + p.db;
    return pieces.empty() ? gain_db : gain_db + pieces.front().db;
}

double PiecePosition(double anchor_pos, double from_anchor, double playrate)
{
    const double rate = (std::isfinite(playrate) && playrate > 0.0) ? playrate : 1.0;
    const double p = anchor_pos + from_anchor / rate;
    return (std::isfinite(p) && p > 0.0) ? p : 0.0;
}

std::string MediaCopyName(const std::string& file_name, int n)
{
    if (n <= 0) return file_name;
    const size_t dot = file_name.rfind('.');
    const bool   has_ext = dot != std::string::npos && dot > 0;
    const std::string stem = has_ext ? file_name.substr(0, dot) : file_name;
    const std::string ext = has_ext ? file_name.substr(dot) : std::string();
    char num[16];
    std::snprintf(num, sizeof(num), "-%03d", n);
    return stem + num + ext;
}

}  // namespace rav
