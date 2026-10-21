// SPDX-License-Identifier: MIT
//
// Story 10-8o: the all-trigger bench. Runs Detect with every auto type ticked (the product path,
// DetectAutoEvents: sensitivity, signatures, Land / Jump replacement) on every clip of the
// fixtures folder and scores it against each clip's reference file (tests/ref_file.h).
//
//   trigger_bench [--fixtures=<dir>] [--fbx-dump=<exe>] [--out=<dir>]
//   trigger_bench --old-refs <file.csv>
//
// Clips: every '*.csv' dump of the folder (top level), and every '.fbx' / '.glb' with no
// '<name>.csv' beside it, dumped by tools/fbx_dump into <out>/dumps/<name>.csv (redumped when
// the source or the fbx_dump binary is newer; a failure is cached the same way and listed
// "dump failed: <reason>"). A clip's id is its source file name (a dump's name without .csv);
// its references are <fixtures>/<id>.ref.tsv (the bench creates none: clips without are listed).
//
// Two configurations, one matrix each: default (every AutoType at the default sensitivity in its
// default modes) and parts (steps heel / toe separate, slides and pivots Each part, story
// 10-9a-3: lift-off, jump and land Separate). Detect's
// raise of silent blocks (ApplyAutoSensSearch) runs on a copy of each configuration's blocks:
// the raised blocks and their events are listed in their own section and never count in a
// matrix (story 10-9a-4: one on a clip whose reference has no line of its kind is marked "no
// reference" and counted: a raise the reference says is false). Matching (ref_file.h ScoreRow): per clip x kind x side, one-to-one, smallest |err|
// first, +-kMatchWindowS, inside '# visible' only; confirmed / draft counted apart.
//
// The run is saved to <out>/last.tsv (the previous one read first), and every row that changed
// is listed. <out>/review.json holds what the review page shows (per clip: joints at 30 Hz, the
// parents, foot / hand / hips heights at 60 Hz, the found events of both configurations, the
// references).
//
// --old-refs: prints the csv dump's old REF markers as reference lines (ref_file.h OldRefLines),
// then '# skipped <label>' lines; tools/ref_draft merges them.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "auto_detect.h"
#include "auto_types.h"
#include "bone_roles.h"
#include "dump_clip.h"
#include "event_list.h"  // story 10-10a: IsDoubtful
#include "footstep_measure.h"
#include "motion_physics.h"
#include "ref_file.h"

using namespace rav;
using namespace rav::bench;
namespace fs = std::filesystem;

namespace {

const char* kMissingMotions = "stairs down, skid to a halt";

std::string Lower(std::string s)
{
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool EndsWith(const std::string& s, const std::string& tail)
{
    return s.size() >= tail.size() && s.compare(s.size() - tail.size(), tail.size(), tail) == 0;
}

std::string Fmt(const char* f, double v)
{
    char b[64];
    std::snprintf(b, sizeof(b), f, v);
    return b;
}

std::string JsonStr(const std::string& s)
{
    std::string o = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') o += '\\', o += c;
        else if (static_cast<unsigned char>(c) < 0x20) o += ' ';
        else o += c;
    }
    return o + "\"";
}

// ---- Clips -------------------------------------------------------------------------------------

struct ClipSrc {
    std::string id;        // the source file name
    std::string csv;       // the csv read ("" = none: `failure`)
    std::string failure;   // "dump failed: <reason>"
};

std::string LastLine(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    std::string   line, last;
    while (std::getline(in, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (!line.empty()) last = line;
    }
    return last.empty() ? "no output" : last;
}

bool Newer(const fs::path& a, const fs::path& b)  // a newer than b (b missing: true)
{
    std::error_code ec;
    if (!fs::exists(b, ec)) return true;
    return fs::last_write_time(a, ec) > fs::last_write_time(b, ec);
}

// Dumps `src` to <dumps>/<name>.csv when needed. Fills c.csv or c.failure.
void EnsureDump(const fs::path& src, const fs::path& dumps, const std::string& fbx_dump, ClipSrc& c)
{
    const fs::path csv = dumps / (src.filename().u8string() + ".csv");
    const fs::path fail = dumps / (src.filename().u8string() + ".fail");
    const fs::path exe = fbx_dump.empty() ? fs::path() : fs::u8path(fbx_dump);
    std::error_code ec;
    const bool exe_ok = !fbx_dump.empty() && fs::exists(exe, ec);
    const auto fresh = [&](const fs::path& f) {
        return fs::exists(f, ec) && !Newer(src, f) && (!exe_ok || !Newer(exe, f));
    };
    if (fresh(csv)) {
        c.csv = csv.u8string();
        return;
    }
    if (fresh(fail)) {
        c.failure = "dump failed: " + LastLine(fail.u8string());
        return;
    }
    if (!exe_ok) {
        if (fs::exists(csv, ec)) {  // stale, but nothing to redump with
            c.csv = csv.u8string();
            return;
        }
        c.failure = "dump failed: no fbx_dump (--fbx-dump=<exe>)";
        return;
    }
    const fs::path tmp = dumps / (src.filename().u8string() + ".csv.tmp");
    const fs::path log = dumps / (src.filename().u8string() + ".log");
    // Run from the source's folder with its bare name: the loader reads some files (external
    // texture paths such as "C:\...") only that way on Linux (already so before story 10-8o).
    const fs::path abs_tmp = fs::absolute(tmp, ec), abs_log = fs::absolute(log, ec);
    const fs::path abs_exe = fs::absolute(exe, ec), src_dir = fs::absolute(src, ec).parent_path();
    const std::string cmd = "cd \"" + src_dir.u8string() + "\" && \"" + abs_exe.u8string() + "\" \"" +
                            src.filename().u8string() + "\" \"" + abs_tmp.u8string() + "\" > \"" + abs_log.u8string() +
                            "\" 2>&1";
    const int rc = std::system(cmd.c_str());
    if (rc == 0 && fs::exists(tmp, ec)) {
        std::error_code rename_ec, remove_ec;
        fs::rename(tmp, csv, rename_ec);
        if (!rename_ec) {
            fs::remove(fail, remove_ec);
            c.csv = csv.u8string();
            return;
        }
        fs::remove(tmp, ec);
        c.failure = "dump failed: cannot save " + csv.u8string() + ": " + rename_ec.message();
        return;
    }
    fs::remove(tmp, ec);
    {  // copied through streams: fs::copy_file leaves an empty file on the Windows mount (WSL)
        std::ifstream in(log, std::ios::binary);
        std::ofstream out(fail, std::ios::binary | std::ios::trunc);
        out << in.rdbuf();
    }
    c.failure = "dump failed: " + LastLine(log.u8string());
}

std::vector<ClipSrc> ListClips(const fs::path& dir, const fs::path& dumps, const std::string& fbx_dump)
{
    std::vector<std::string> names;
    std::error_code          ec;
    for (const auto& e : fs::directory_iterator(dir, ec))
        if (e.is_regular_file(ec)) names.push_back(e.path().filename().u8string());
    std::sort(names.begin(), names.end());
    const std::set<std::string> have(names.begin(), names.end());
    std::vector<ClipSrc>        out;
    for (const std::string& n : names) {
        const std::string l = Lower(n);
        ClipSrc           c;
        if (EndsWith(l, ".csv")) {
            c.id = n.substr(0, n.size() - 4);
            c.csv = (dir / fs::u8path(n)).u8string();
        } else if (EndsWith(l, ".fbx") || EndsWith(l, ".glb")) {
            if (have.count(n + ".csv")) continue;  // the REAPER dump covers it
            c.id = n;
            EnsureDump(dir / fs::u8path(n), dumps, fbx_dump, c);
        } else {
            continue;
        }
        out.push_back(c);
    }
    return out;
}

// ---- Detection ---------------------------------------------------------------------------------

AutoSettings BenchSettings(BenchConfig c)
{
    AutoSettings s;
    for (int t = 0; t < kAutoTypeCount; ++t) {
        s.type[t].on = true;
        s.type[t].sens = kAutoDefaultSens;
    }
    if (c == BenchConfig::Parts) {
        s.separate_steps = true;
        s.slide_parts = ScuffParts::EachPart;
        s.pivot_parts = ScuffParts::EachPart;
        // Story 10-9a-3: lift-off, jump and land Separate.
        for (int t = 0; t < kAutoTypeCount; ++t) s.separate_parts[t] = AutoTypeRow(static_cast<AutoType>(t)).part_split;
    }
    return s;
}

struct Found {
    std::string target;
    char        side = 'L';
    double      t = 0.0;
    double      strength = 0.0;  // story 10-10b: the event's strength (its type's unit)
    AutoType    type = AutoType::Step;
    double      conf = 1.0;  // story 10-10a: Event::confidence
};

struct Raised {
    std::string text;    // "grab L 50→72: 3 events"
    std::string target;  // its row kind word (FoundTarget)
    int         events = 0;
};

struct ConfigRun {
    std::vector<Found>  found;
    std::vector<Raised> raised;
};

ConfigRun RunConfig(BenchConfig cfg, const std::vector<BoneTrack>& role_tracks, const std::vector<int>& role_bone)
{
    ConfigRun                 r;
    const std::vector<Block>  blocks = AutoBlocks(BenchSettings(cfg));
    std::vector<AutoAnalysis> cache, probes;
    // Story 10-10a: the product path rates the events (their confidence).
    for (const Event& e : DetectRatedAutoEvents(blocks, role_tracks, &cache, &probes, false)) {
        if (e.block < 0 || static_cast<size_t>(e.block) >= blocks.size()) continue;
        const Block& b = blocks[static_cast<size_t>(e.block)];
        AutoKind     k = AutoKind::Step;
        AutoBlockKind(b, &k, nullptr);
        r.found.push_back({FoundTarget(b.auto_type), b.auto_side, e.time_s, e.strength, AutoTypeOfKind(k), e.confidence});
    }
    // Detect's raise, on a copy.
    std::vector<Block>          copy = blocks;
    std::vector<AutoSensResult> res;
    std::vector<AutoAnalysis>   cache2 = cache;
    if (ApplyAutoSensSearch(copy, role_tracks, role_bone, &res, &cache2, false)) {
        const std::vector<Event> ev = DetectAutoEvents(copy, role_tracks, &cache2, false);
        for (size_t i = 0; i < copy.size() && i < res.size(); ++i) {
            if (!res[i].raised) continue;
            int n = 0;
            for (const Event& e : ev) n += e.block == static_cast<int>(i);
            char buf[160];
            std::snprintf(buf, sizeof(buf), "%s %c %g\xE2\x86\x92%g: %d event%s", blocks[i].auto_type.c_str(),
                          blocks[i].auto_side, blocks[i].sens, res[i].sens, n, n == 1 ? "" : "s");
            r.raised.push_back({buf, FoundTarget(blocks[i].auto_type), n});
        }
    }
    return r;
}

// Story 10-9j: the scuffs (the types whose markers sit at the start by default, AutoStartDefault)
// switched to Peak, in that configuration: their events only (Detect, rated as the matrices').
std::vector<Found> RunScuffPeak(BenchConfig cfg, const std::vector<BoneTrack>& role_tracks)
{
    AutoSettings s = BenchSettings(cfg);
    for (int t = 0; t < kAutoTypeCount; ++t)
        if (AutoStartDefault(static_cast<AutoType>(t))) s.at_start[t] = false;
    const std::vector<Block>  blocks = AutoBlocks(s);
    std::vector<AutoAnalysis> cache, probes;
    std::vector<Found>        out;
    for (const Event& e : DetectRatedAutoEvents(blocks, role_tracks, &cache, &probes, false)) {
        if (e.block < 0 || static_cast<size_t>(e.block) >= blocks.size()) continue;
        const Block& b = blocks[static_cast<size_t>(e.block)];
        AutoKind     k = AutoKind::Step;
        AutoBlockKind(b, &k, nullptr);
        if (!AutoStartDefault(AutoTypeOfKind(k))) continue;
        out.push_back({FoundTarget(b.auto_type), b.auto_side, e.time_s, e.strength, AutoTypeOfKind(k), e.confidence});
    }
    return out;
}

// ---- Rows --------------------------------------------------------------------------------------

struct RowKey {
    std::string target;
    char        side;
    bool operator<(const RowKey& o) const { return target != o.target ? target < o.target : side < o.side; }
};

struct RowData {
    std::vector<RowRef> refs;
    std::vector<double> det;
};

// The order rows print in: the kinds of the auto types, in table order.
int TargetOrder(const std::string& w)
{
    static std::vector<std::string> order;
    if (order.empty()) {
        for (int t = 0; t < kAutoTypeCount; ++t)
            for (AutoKind k : AutoKindsOf(static_cast<AutoType>(t))) {
                const std::string word = FoundTarget(AutoKindWord(k));
                if (std::find(order.begin(), order.end(), word) == order.end()) order.push_back(word);
            }
    }
    const auto it = std::find(order.begin(), order.end(), w);
    return it == order.end() ? 999 : static_cast<int>(it - order.begin());
}

std::string RowName(const std::string& target, char side)
{
    AutoKind k;
    if (AutoKindFromWord(target, &k)) return AutoMarkerName(k, side);
    return target + (side == 'B' ? "" : std::string(" ") + side);
}

struct ClipResult {
    ClipSrc               src;
    bool                  read = false;
    std::string           err;
    dump::Clip            clip;
    std::vector<int>      role_bone;
    ConfigRun             run[2];
    std::vector<Found>    scuff_peak[2];  // story 10-9j: the scuffs at Peak, per configuration
    bool                  has_ref = false;
    std::vector<RefLine>  refs;
    std::vector<std::string> ref_errors;
};

std::map<RowKey, RowData> Rows(const ClipResult& c, BenchConfig cfg)
{
    std::map<RowKey, RowData> rows;
    for (const Found& f : c.run[static_cast<int>(cfg)].found) rows[{f.target, f.side}].det.push_back(f.t);
    for (const RefLine& r : c.refs) {
        if (r.status == "deleted") continue;
        const std::string tg = RefTarget(r, cfg);
        if (tg.empty()) continue;
        rows[{tg, RowSide(tg, r.side)}].refs.push_back({r.time_s, r.status == "confirmed"});
    }
    return rows;
}

std::vector<std::pair<RowKey, RowData>> Ordered(const std::map<RowKey, RowData>& m)
{
    std::vector<std::pair<RowKey, RowData>> v(m.begin(), m.end());
    std::stable_sort(v.begin(), v.end(), [](const auto& a, const auto& b) {
        const int oa = TargetOrder(a.first.target), ob = TargetOrder(b.first.target);
        return oa != ob ? oa < ob : a.first.side < b.first.side;
    });
    return v;
}

std::string TsvValue(const RowScore& s)
{
    const ErrStats st = StatsOf(s.err_s);
    char           b[256];
    std::snprintf(b, sizeof(b), "%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%s\t%s\t%s", s.exp_c, s.exp_d, s.found, s.matched_c,
                  s.matched_d, s.missed_c, s.missed_d, s.extra, st.n ? Fmt("%+.1f", st.mean_ms).c_str() : "-",
                  st.n ? Fmt("%.1f", st.sd_ms).c_str() : "-", st.n ? Fmt("%.1f", st.med_abs_ms).c_str() : "-");
    return b;
}

std::map<std::string, std::string> ReadLast(const std::string& path, bool* had)
{
    std::map<std::string, std::string> m;
    std::ifstream                      in(path, std::ios::binary);
    *had = static_cast<bool>(in);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        size_t p = 0;
        for (int k = 0; k < 4 && p != std::string::npos; ++k) p = line.find('\t', p + (k ? 1 : 0));
        if (p == std::string::npos) continue;
        m[line.substr(0, p)] = line.substr(p + 1);
    }
    return m;
}

// ---- Review data -------------------------------------------------------------------------------

bool FingerBone(const std::string& name)
{
    const std::string l = Lower(name);
    for (const char* w : {"thumb", "index", "middle", "ring", "pinky", "finger", "twist", "ik_", "_ik", "weapon",
                          "metacarpal", "attach", "socket"})
        if (l.find(w) != std::string::npos) return true;
    return false;
}

std::string ReviewJson(const std::vector<ClipResult>& clips)
{
    std::ostringstream o;
    o << "{\"version\":1,\"configs\":[\"default\",\"parts\"],\"clips\":[";
    bool first_clip = true;
    for (const ClipResult& c : clips) {
        if (!c.read) continue;
        const dump::Clip& d = c.clip;
        o << (first_clip ? "" : ",") << "\n{\"id\":" << JsonStr(c.src.id) << ",\"label\":" << JsonStr(d.label)
          << ",\"lo\":" << Fmt("%.4f", d.lo) << ",\"hi\":" << Fmt("%.4f", d.hi) << ",\"has_ref\":"
          << (c.has_ref ? "true" : "false");
        first_clip = false;
        // Joints: the non-finger bones, parents to the nearest kept ancestor.
        std::vector<int> keep, idx(d.names.size(), -1);
        for (size_t b = 0; b < d.names.size(); ++b)
            if (!FingerBone(d.names[b])) {
                idx[b] = static_cast<int>(keep.size());
                keep.push_back(static_cast<int>(b));
            }
        o << ",\"bones\":[";
        for (size_t k = 0; k < keep.size(); ++k) o << (k ? "," : "") << JsonStr(d.names[static_cast<size_t>(keep[k])]);
        o << "],\"parent\":[";
        for (size_t k = 0; k < keep.size(); ++k) {
            int p = static_cast<size_t>(keep[k]) < d.parent.size() ? d.parent[static_cast<size_t>(keep[k])] : -1;
            while (p >= 0 && static_cast<size_t>(p) < idx.size() && idx[static_cast<size_t>(p)] < 0)
                p = static_cast<size_t>(p) < d.parent.size() ? d.parent[static_cast<size_t>(p)] : -1;
            o << (k ? "," : "") << (p >= 0 && static_cast<size_t>(p) < idx.size() ? idx[static_cast<size_t>(p)] : -1);
        }
        const size_t n = d.tracks.empty() ? 0 : d.tracks[0].pos.size();
        const auto   sample_at = [&](double hz) {
            std::vector<size_t> s;
            for (double t = 0.0; n && t <= (n - 1) / d.rate + 1e-9; t += 1.0 / hz)
                s.push_back(std::min(n - 1, static_cast<size_t>(std::lround(t * d.rate))));
            return s;
        };
        o << "],\"fps\":30,\"frames\":[";
        const std::vector<size_t> f30 = sample_at(30.0);
        for (size_t i = 0; i < f30.size(); ++i) {
            o << (i ? "," : "") << "[";
            for (size_t k = 0; k < keep.size(); ++k) {
                const Vec3d& p = d.tracks[static_cast<size_t>(keep[k])].pos[f30[i]];
                o << (k ? "," : "") << Fmt("%.3f", p.x) << "," << Fmt("%.3f", p.y) << "," << Fmt("%.3f", p.z);
            }
            o << "]";
        }
        o << "],\"curve_hz\":60,\"curves\":{";
        const std::vector<size_t> f60 = sample_at(60.0);
        const std::pair<const char*, Role> curves[] = {
            {"heel_L", Role::LeftHeel},   {"toe_L", Role::LeftToe},   {"tip_L", Role::LeftToeEnd},
            {"heel_R", Role::RightHeel},  {"toe_R", Role::RightToe},  {"tip_R", Role::RightToeEnd},
            {"hand_L", Role::LeftHand},   {"hand_R", Role::RightHand}, {"hips", Role::Hips}};
        bool fc = true;
        for (const auto& cv : curves) {
            const size_t r = static_cast<size_t>(cv.second);
            if (r >= c.role_bone.size() || c.role_bone[r] < 0) continue;
            const BoneTrack& t = d.tracks[static_cast<size_t>(c.role_bone[r])];
            o << (fc ? "" : ",") << "\"" << cv.first << "\":[";
            fc = false;
            for (size_t i = 0; i < f60.size(); ++i) o << (i ? "," : "") << Fmt("%.3f", t.pos[f60[i]].y);
            o << "]";
        }
        o << "}";
        for (int cfg = 0; cfg < 2; ++cfg) {
            o << ",\"found_" << BenchConfigName(static_cast<BenchConfig>(cfg)) << "\":[";
            bool fe = true;
            for (const Found& f : c.run[cfg].found) {
                o << (fe ? "" : ",") << "{\"k\":" << JsonStr(f.target) << ",\"s\":\"" << f.side << "\",\"t\":"
                  << Fmt("%.4f", f.t) << "}";
                fe = false;
            }
            o << "]";
        }
        o << ",\"refs\":[";
        for (size_t i = 0; i < c.refs.size(); ++i) {
            const RefLine& r = c.refs[i];
            o << (i ? "," : "") << "{\"id\":" << JsonStr(r.kind + "|" + r.side + "|" + r.part + "|" + Fmt("%.4f", r.time_s))
              << ",\"k\":" << JsonStr(r.kind) << ",\"s\":\"" << r.side << "\",\"p\":" << JsonStr(r.part)
              << ",\"t\":" << Fmt("%.4f", r.time_s) << ",\"st\":" << JsonStr(r.status) << ",\"src\":" << JsonStr(r.source)
              << "}";
        }
        o << "]}";
    }
    o << "\n]}\n";
    return o.str();
}

int OldRefsMode(const std::string& path)
{
    dump::Clip  c;
    std::string err;
    if (!dump::ReadClip(path, c, &err)) {
        std::fprintf(stderr, "%s: %s\n", path.c_str(), err.c_str());
        return 1;
    }
    std::vector<std::string> skipped;
    for (const RefLine& r : OldRefLines(c.refs, c.names, GuessRoleMapping(c.names), &skipped))
        std::printf("%s\n", FormatRefLine(r).c_str());
    for (const std::string& s : skipped) std::printf("# skipped %s\n", s.c_str());
    return 0;
}

}  // namespace

int main(int argc, char** argv)
{
    std::string fixtures = "fixtures", fbx_dump, out_dir;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--old-refs" && i + 1 < argc) return OldRefsMode(argv[i + 1]);
        if (a.rfind("--fixtures=", 0) == 0) fixtures = a.substr(11);
        else if (a.rfind("--fbx-dump=", 0) == 0) fbx_dump = a.substr(11);
        else if (a.rfind("--out=", 0) == 0) out_dir = a.substr(6);
        else {
            std::fprintf(stderr,
                         "usage: trigger_bench [--fixtures=<dir>] [--fbx-dump=<exe>] [--out=<dir>]\n"
                         "       trigger_bench --old-refs <file.csv>\n");
            return 2;
        }
    }
    const fs::path dir = fs::u8path(fixtures);
    const fs::path out = out_dir.empty() ? dir / "_bench" : fs::u8path(out_dir);
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) {
        std::fprintf(stderr, "no fixtures folder: %s\n", fixtures.c_str());
        return 1;
    }
    fs::create_directories(out / "dumps", ec);

    std::vector<ClipResult> clips;
    for (const ClipSrc& s : ListClips(dir, out / "dumps", fbx_dump)) {
        ClipResult c;
        c.src = s;
        if (!s.csv.empty()) {
            c.read = dump::ReadClip(s.csv, c.clip, &c.err);
            if (!c.read) c.err = "read failed: " + c.err;
        } else {
            c.err = s.failure;
        }
        if (c.read) {
            c.role_bone = GuessRoleMapping(c.clip.names);
            const dump::Clip& d = c.clip;
            const std::vector<BoneTrack> rt = PhysicsRoleTracks(c.role_bone, [&](const std::vector<int>& idx) {
                std::vector<BoneTrack> t;
                for (int i : idx) t.push_back(d.tracks.at(static_cast<size_t>(i)));
                return t;
            });
            for (int cfg = 0; cfg < 2; ++cfg) {
                c.run[cfg] = RunConfig(static_cast<BenchConfig>(cfg), rt, c.role_bone);
                c.scuff_peak[cfg] = RunScuffPeak(static_cast<BenchConfig>(cfg), rt);
            }
            c.has_ref = ReadRefFile((dir / fs::u8path(s.id + ".ref.tsv")).u8string(), &c.refs, &c.ref_errors);
        }
        clips.push_back(std::move(c));
    }

    std::printf("=== trigger_bench: every auto type ticked, Detect path (DetectAutoEvents) ===\n");
    std::printf("%zu clips in %s; match +-%g ms inside # visible; exp / matched / missed = confirmed/draft\n",
                clips.size(), fixtures.c_str(), kMatchWindowS * 1000.0);
    std::vector<std::string> tsv;
    std::map<std::string, std::string> now;
    for (int cfg = 0; cfg < 2; ++cfg) {
        const BenchConfig bc = static_cast<BenchConfig>(cfg);
        std::printf("\n--- %s matrix (%s) ---\n", BenchConfigName(bc),
                    bc == BenchConfig::Default ? "default sensitivity and modes"
                                               : "steps heel / toe separate, slides and pivots Each part, lift-off / jump / land Separate");
        std::map<std::string, RowScore> sum;
        for (const ClipResult& c : clips) {
            if (!c.read) continue;
            const auto rows = Ordered(Rows(c, bc));
            std::printf("%s%s\n", c.src.id.c_str(), c.has_ref ? "" : "  (no reference)");
            if (rows.empty()) std::printf("  (nothing expected, nothing found)\n");
            for (const auto& kv : rows) {
                const RowScore s = ScoreRow(kv.second.refs, kv.second.det, kMatchWindowS, c.clip.lo, c.clip.hi);
                if (s.exp_c + s.exp_d + s.found == 0) continue;
                std::printf("  %-16s %s\n", RowName(kv.first.target, kv.first.side).c_str(), RowText(s).c_str());
                const std::string key = std::string(BenchConfigName(bc)) + "\t" + c.src.id + "\t" + kv.first.target +
                                        "\t" + std::string(1, kv.first.side);
                now[key] = TsvValue(s);
                tsv.push_back(key + "\t" + now[key]);
                if (!c.has_ref) continue;  // the summary sums clips with a reference only
                RowScore& t = sum[kv.first.target];
                t.exp_c += s.exp_c, t.exp_d += s.exp_d, t.found += s.found, t.matched_c += s.matched_c;
                t.matched_d += s.matched_d, t.missed_c += s.missed_c, t.missed_d += s.missed_d, t.extra += s.extra;
                t.err_s.insert(t.err_s.end(), s.err_s.begin(), s.err_s.end());
            }
        }
        std::printf("Summary (%s, every clip with a reference, both sides):\n", BenchConfigName(bc));
        std::vector<std::string> kinds;
        for (const auto& kv : sum) kinds.push_back(kv.first);
        std::stable_sort(kinds.begin(), kinds.end(),
                         [](const std::string& a, const std::string& b) { return TargetOrder(a) < TargetOrder(b); });
        for (const std::string& k : kinds) std::printf("  %-12s %s\n", k.c_str(), RowText(sum[k]).c_str());
    }

    // Story 10-9a-4: a raised block on a clip whose reference holds no line of its kind (in that
    // configuration, either side) found what the reference says is not there: "no reference", and
    // counted per kind below (a clip with a full reference and no jump, such as Treadmill Running,
    // is a negative reference for the raise too).
    std::printf("\n--- Raise of silent blocks (Detect's search, on a copy; never in a matrix) ---\n");
    bool                       any_raise = false;
    std::map<std::string, int> false_events, false_blocks;
    for (const ClipResult& c : clips) {
        if (!c.read) continue;
        for (int cfg = 0; cfg < 2; ++cfg)
            for (const Raised& r : c.run[cfg].raised) {
                bool none = c.has_ref;
                for (const RefLine& l : c.refs)
                    if (l.status != "deleted" && RefTarget(l, static_cast<BenchConfig>(cfg)) == r.target) none = false;
                std::printf("  %s [%s]  %s%s\n", c.src.id.c_str(), BenchConfigName(static_cast<BenchConfig>(cfg)),
                            r.text.c_str(), none ? "  (no reference)" : "");
                if (none) false_events[r.target] += r.events, ++false_blocks[r.target];
                any_raise = true;
            }
    }
    if (!any_raise) std::printf("  (no block raised)\n");
    std::printf("Raised against a reference with none of the kind (events / blocks):");
    for (const char* k : {"jump", "land"}) std::printf(" %s %d / %d,", k, false_events[k], false_blocks[k]);
    std::printf(" every kind %d / %d\n", [&] { int n = 0; for (const auto& kv : false_events) n += kv.second; return n; }(),
                [&] { int n = 0; for (const auto& kv : false_blocks) n += kv.second; return n; }());

    // Story 10-10b: the strength of every event (default matrix), per type over every clip (its
    // distribution, and where the level cuts of the type table fall), then per clip.
    {
        std::printf("\n--- STRENGTH (default matrix, every clip; levels Soft < medium <= Medium < hard <= Hard) ---\n");
        std::map<int, std::vector<double>> all;
        // Each clip weighs the same in the clip-weighted cuts (wp33 / wp67): a long treadmill run
        // does not set a type's levels alone.
        std::map<int, std::vector<std::pair<double, double>>> weighted;
        for (const ClipResult& c : clips) {
            if (!c.read) continue;
            std::map<int, int> n;
            for (const Found& f : c.run[0].found) ++n[static_cast<int>(f.type)];
            for (const Found& f : c.run[0].found) {
                all[static_cast<int>(f.type)].push_back(f.strength);
                weighted[static_cast<int>(f.type)].push_back({f.strength, 1.0 / n[static_cast<int>(f.type)]});
            }
        }
        const auto wpct = [](std::vector<std::pair<double, double>> v, double q) {
            if (v.empty()) return 0.0;
            std::sort(v.begin(), v.end());
            double total = 0.0, acc = 0.0;
            for (const auto& x : v) total += x.second;
            for (const auto& x : v) {
                acc += x.second;
                if (acc >= q * total - 1e-12) return x.first;
            }
            return v.back().first;
        };
        const auto pct = [](std::vector<double> v, double q) {
            if (v.empty()) return 0.0;
            std::sort(v.begin(), v.end());
            const double x = q * static_cast<double>(v.size() - 1);
            const size_t i = static_cast<size_t>(x);
            const double f = x - static_cast<double>(i);
            return i + 1 < v.size() ? v[i] * (1.0 - f) + v[i + 1] * f : v[i];
        };
        std::printf("  %-12s %4s %8s %8s %8s %8s %8s %8s %8s %8s %8s | %8s %8s | %5s %5s %5s\n", "type", "n", "min", "p10",
                    "p33", "p50", "p67", "p90", "max", "wp33", "wp67", "medium", "hard", "soft", "med", "hard");
        for (int t = 0; t < kAutoTypeCount; ++t) {
            const auto it = all.find(t);
            const AutoStrengthCuts& cut = AutoStrengthRow(static_cast<AutoType>(t));
            const char* word = AutoTypeRow(static_cast<AutoType>(t)).sound_word;
            if (it == all.end()) {
                std::printf("  %-12s %4d %80s | %8.3g %8.3g |\n", word, 0, "(no event on the fixtures)", cut.medium, cut.hard);
                continue;
            }
            const std::vector<double>& v = it->second;
            int lv[4] = {};
            for (double x : v) ++lv[AutoStrengthLevel(static_cast<AutoType>(t), x)];
            const auto& w = weighted[t];
            std::printf("  %-12s %4zu %8.3g %8.3g %8.3g %8.3g %8.3g %8.3g %8.3g %8.3g %8.3g | %8.3g %8.3g | %5d %5d %5d%s\n",
                        word, v.size(), pct(v, 0.0), pct(v, 0.10), pct(v, 1.0 / 3.0), pct(v, 0.5), pct(v, 2.0 / 3.0),
                        pct(v, 0.90), pct(v, 1.0), wpct(w, 1.0 / 3.0), wpct(w, 2.0 / 3.0), cut.medium, cut.hard, lv[1], lv[2],
                        lv[3], lv[0] ? "  (some with no strength)" : "");
        }
        std::printf("Per clip (type: n, min / median / max, Soft / Medium / Hard):\n");
        for (const ClipResult& c : clips) {
            if (!c.read) continue;
            std::map<int, std::vector<double>> by;
            for (const Found& f : c.run[0].found) by[static_cast<int>(f.type)].push_back(f.strength);
            if (by.empty()) continue;
            std::printf("  %s\n", c.src.id.c_str());
            for (const auto& kv : by) {
                int lv[4] = {};
                for (double x : kv.second) ++lv[AutoStrengthLevel(static_cast<AutoType>(kv.first), x)];
                std::printf("    %-12s %3zu  %7.3g / %7.3g / %7.3g   %d / %d / %d\n",
                            AutoTypeRow(static_cast<AutoType>(kv.first)).sound_word, kv.second.size(), pct(kv.second, 0.0),
                            pct(kv.second, 0.5), pct(kv.second, 1.0), lv[1], lv[2], lv[3]);
            }
        }
    }

    // Story 10-10a: each found event's confidence (RateAutoEvents), by what the reference says of
    // it: matched to a confirmed line, to a draft, extra (a clip with a reference, no line near),
    // or no reference. Per clip, the events under full confidence; then the counts per level.
    std::printf("\n--- Confidence (story 10-10a; default matrix; doubtful below %.2f) ---\n", kDefaultDoubtLimit);
    {
        const int                 nl = kAutoConfidenceSteps + 1;
        std::map<std::string, std::vector<int>> hist;  // class -> count per level
        const char*               classes[4] = {"confirmed", "draft", "extra", "no ref"};
        for (const char* k : classes) hist[k].assign(static_cast<size_t>(nl), 0);
        for (const ClipResult& c : clips) {
            if (!c.read) continue;
            std::string lines;
            int         full = 0;  // confirmed at full confidence (not listed)
            for (const auto& kv : Ordered(Rows(c, BenchConfig::Default))) {
                std::vector<RowRef> conf_refs;
                for (const RowRef& r : kv.second.refs)
                    if (r.confirmed) conf_refs.push_back(r);
                const RowScore all = ScoreRow(kv.second.refs, kv.second.det, kMatchWindowS, c.clip.lo, c.clip.hi);
                const RowScore only_c = ScoreRow(conf_refs, kv.second.det, kMatchWindowS, c.clip.lo, c.clip.hi);
                auto in = [](const std::vector<double>& v, double t) {
                    return std::find(v.begin(), v.end(), t) != v.end();
                };
                for (const Found& f : c.run[0].found) {
                    if (f.target != kv.first.target || f.side != kv.first.side) continue;
                    if (f.t < c.clip.lo || f.t > c.clip.hi) continue;
                    const char* cls = !c.has_ref                ? "no ref"
                                      : in(all.extra_t, f.t)    ? "extra"
                                      : !in(only_c.extra_t, f.t) ? "confirmed"
                                                                 : "draft";
                    const int lvl = std::clamp(static_cast<int>(std::lround(f.conf * kAutoConfidenceSteps)), 0, nl - 1);
                    ++hist[cls][static_cast<size_t>(lvl)];
                    if (f.conf >= 1.0 && std::string(cls) == "confirmed") {
                        ++full;
                        continue;
                    }
                    char buf[200];
                    std::snprintf(buf, sizeof(buf), "  %-16s %7.3f s  conf %.1f  %s%s\n",
                                  RowName(kv.first.target, kv.first.side).c_str(), f.t, f.conf, cls,
                                  IsDoubtful(f.conf) ? "  DOUBTFUL" : "");
                    lines += buf;
                }
            }
            if (!lines.empty() || full) std::printf("%s  (%d confirmed at 1.0)\n%s", c.src.id.c_str(), full, lines.c_str());
        }
        std::printf("Events per confidence level (0.0 .. 1.0, step %.1f):\n", 1.0 / kAutoConfidenceSteps);
        for (const char* k : classes) {
            std::printf("  %-10s", k);
            for (int v : hist[k]) std::printf(" %5d", v);
            std::printf("\n");
        }
    }

    // Story 10-9j: the scuffs (slide, pivot, hand pivot) at Peak against Start (the default, the
    // matrices'): per clip and configuration, each event's Start time and Peak time (paired in
    // order per row and side: the same scuffs, only their time moves), then Hip Hop Dancing's
    // seven scuffs by ear (Antho, 2026-10-21) against the nearest pivot of that foot.
    std::printf("\n--- Scuff Peak (story 10-9j; Start = the matrices', Peak = the rub's speed peak) ---\n");
    {
        std::map<std::string, std::vector<double>> deltas;  // per row kind: Peak - Start (ms)
        int                                        unpaired = 0, conf_moved = 0;
        for (const ClipResult& c : clips) {
            if (!c.read) continue;
            for (int cfg = 0; cfg < 2; ++cfg) {
                std::map<RowKey, std::vector<double>>                    st, pk;
                std::map<RowKey, std::vector<std::pair<double, double>>> stc, pkc;  // (time, confidence)
                for (const Found& f : c.run[cfg].found)
                    if (AutoStartDefault(f.type)) {
                        st[{f.target, f.side}].push_back(f.t);
                        stc[{f.target, f.side}].push_back({f.t, f.conf});
                    }
                for (const Found& f : c.scuff_peak[cfg]) {
                    pk[{f.target, f.side}].push_back(f.t);
                    pkc[{f.target, f.side}].push_back({f.t, f.conf});
                }
                for (const auto& kv : pk)
                    if (!st.count(kv.first)) st[kv.first];
                std::string lines;
                for (const auto& kv : st) {
                    std::vector<double> a = kv.second, b = pk[kv.first];
                    std::sort(a.begin(), a.end());
                    std::sort(b.begin(), b.end());
                    if (a.size() != b.size()) {
                        ++unpaired;
                        lines += "    " + RowName(kv.first.target, kv.first.side) + "  UNPAIRED (" +
                                 std::to_string(a.size()) + " start, " + std::to_string(b.size()) + " peak)\n";
                        continue;
                    }
                    std::vector<std::pair<double, double>> ac = stc[kv.first], bc = pkc[kv.first];
                    std::sort(ac.begin(), ac.end());
                    std::sort(bc.begin(), bc.end());
                    for (size_t i = 0; i < a.size(); ++i) {
                        const double d = (b[i] - a[i]) * 1000.0;
                        const bool conf = i < ac.size() && i < bc.size() && ac[i].second != bc[i].second;
                        conf_moved += conf ? 1 : 0;
                        deltas[kv.first.target].push_back(d);
                        char buf[200];
                        std::snprintf(buf, sizeof(buf), "    %-16s start %7.3f  peak %7.3f  %+6.0f ms",
                                      RowName(kv.first.target, kv.first.side).c_str(), a[i], b[i], d);
                        lines += buf;
                        if (conf) {
                            std::snprintf(buf, sizeof(buf), "  conf %.1f -> %.1f", ac[i].second, bc[i].second);
                            lines += buf;
                        }
                        lines += "\n";
                    }
                }
                if (!lines.empty())
                    std::printf("  %s [%s]\n%s", c.src.id.c_str(), BenchConfigName(static_cast<BenchConfig>(cfg)), lines.c_str());
            }
        }
        std::printf("Peak - Start per row kind (ms): n, min / median / max\n");
        for (auto& kv : deltas) {
            std::vector<double>& v = kv.second;
            std::sort(v.begin(), v.end());
            std::printf("  %-20s %3zu  %6.0f / %6.0f / %6.0f\n", kv.first.c_str(), v.size(), v.front(), v[v.size() / 2],
                        v.back());
        }
        std::printf("Unpaired rows: %d; events whose confidence differs at Peak: %d\n", unpaired, conf_moved);
        // Antho's seven Hip Hop Dancing scuffs by ear (story 10-9j), against the nearest pivot of
        // that foot (default matrix, combined): its Start and its Peak.
        const struct {
            char   side;
            double t;
        } kHipHopScuffs[] = {{'L', 1.40}, {'L', 3.38}, {'L', 6.81}, {'R', 1.56}, {'R', 3.25}, {'R', 5.13}, {'R', 6.75}};
        for (const ClipResult& c : clips) {
            if (!c.read || c.src.id != "Hip Hop Dancing.fbx") continue;
            std::printf("Hip Hop Dancing, Antho's scuffs by ear vs the nearest pivot of that foot (default matrix):\n");
            double sum_s = 0.0, sum_p = 0.0;
            int    n = 0;
            for (const auto& h : kHipHopScuffs) {
                const auto nearest = [&](const std::vector<Found>& v, bool peak) {
                    double best = 1e9;
                    for (const Found& f : v)
                        if (f.target == "pivot" && f.side == h.side && (peak || AutoStartDefault(f.type)) &&
                            std::fabs(f.t - h.t) < std::fabs(best - h.t))
                            best = f.t;
                    return best;
                };
                const double st = nearest(c.run[0].found, false), pk = nearest(c.scuff_peak[0], true);
                std::printf("  %c %5.2f  start %7.3f (%+5.0f ms)  peak %7.3f (%+5.0f ms)\n", h.side, h.t, st,
                            (st - h.t) * 1000.0, pk, (pk - h.t) * 1000.0);
                sum_s += std::fabs(st - h.t), sum_p += std::fabs(pk - h.t), ++n;
            }
            std::printf("  mean |error|: start %.0f ms, peak %.0f ms\n", sum_s / n * 1000.0, sum_p / n * 1000.0);
        }
    }

    std::printf("\n--- Not benched ---\n");
    bool any_fail = false;
    for (const ClipResult& c : clips)
        if (!c.read) {
            std::printf("  %s: %s\n", c.src.id.c_str(), c.err.c_str());
            any_fail = true;
        }
    if (!any_fail) std::printf("  (none)\n");
    std::printf("No reference:");
    bool any_noref = false;
    for (const ClipResult& c : clips)
        if (c.read && !c.has_ref) {
            std::printf("%s %s", any_noref ? "," : "", c.src.id.c_str());
            any_noref = true;
        }
    std::printf("%s\n", any_noref ? "" : " (none)");
    for (const ClipResult& c : clips)
        for (const std::string& e : c.ref_errors) std::printf("  %s.ref.tsv %s (skipped)\n", c.src.id.c_str(), e.c_str());
    std::printf("Missing motions (still to add): %s\n", kMissingMotions);

    // The run, and its changes against the previous one.
    const std::string last = (out / "last.tsv").u8string();
    bool              had = false;
    const auto        prev = ReadLast(last, &had);
    std::printf("\n--- Changes against the previous run ---\n");
    if (!had) {
        std::printf("  (no previous run)\n");
    } else {
        int n = 0;
        for (const auto& kv : now) {
            const auto it = prev.find(kv.first);
            if (it == prev.end()) std::printf("  new      %s\t%s\n", kv.first.c_str(), kv.second.c_str()), ++n;
            else if (it->second != kv.second)
                std::printf("  changed  %s\t%s  ->  %s\n", kv.first.c_str(), it->second.c_str(), kv.second.c_str()), ++n;
        }
        for (const auto& kv : prev)
            if (!now.count(kv.first)) std::printf("  gone     %s\t%s\n", kv.first.c_str(), kv.second.c_str()), ++n;
        if (!n) std::printf("  (none)\n");
    }
    {
        std::ofstream f(last, std::ios::binary);
        f << "# config\tclip\tkind\tside\texp_c\texp_d\tfound\tmatched_c\tmatched_d\tmissed_c\tmissed_d\textra\tmean_ms\tsd_"
             "ms\tmed_abs_ms\n";
        for (const std::string& l : tsv) f << l << "\n";
    }
    {
        std::ofstream f((out / "review.json").u8string(), std::ios::binary);
        f << ReviewJson(clips);
    }
    std::printf("Saved %s and %s\n", last.c_str(), (out / "review.json").u8string().c_str());
    return 0;
}
