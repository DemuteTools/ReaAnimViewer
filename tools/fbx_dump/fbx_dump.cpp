// SPDX-License-Identifier: MIT
//
// fbx_dump <in.fbx|glb> <out.csv>
//
// Offline twin of the plugin's detection dump (DumpTracks in src/detection_measure.cpp):
// loads the file through the plugin's own LoadCpuAsset and samples every bone with its
// own SampleBoneTracks at 240 Hz, then writes the same CSV (no REF markers: "# ref=" is
// empty; "# visible=" is the whole clip, as for an item covering it). Lets detection be
// tuned (tests/detection_eval) on clips never dumped in REAPER.

#include "win32_prelude.h"

#include "asset_loader.h"
#include "bone_sampling.h"

namespace {

constexpr double kRateHz = 240.0;  // kDetectRateHz (footstep_measure.h)

std::string Format(const char* fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return buf;
}

}  // namespace

int main(int argc, char** argv)
{
    using namespace rav;
    if (argc != 3) {
        std::fprintf(stderr, "usage: fbx_dump <in.fbx> <out.csv>\n");
        return 2;
    }
    const std::filesystem::path in = std::filesystem::u8path(argv[1]);
    const CpuLoadResult loaded = LoadCpuAsset(in.u8string());
    if (!loaded.asset) {
        std::fprintf(stderr, "could not load %s: %s\n", argv[1],
                     (loaded.hint.empty() ? loaded.detail : loaded.hint).c_str());
        return 1;
    }
    const CpuAsset& asset = *loaded.asset;
    if (asset.animations.empty() || asset.no_animation) {
        std::fprintf(stderr, "%s: the file has no animation\n", argv[1]);
        return 1;
    }

    std::vector<int> all;
    for (size_t b = 0; b < asset.skeleton.bones.size(); ++b) all.push_back(static_cast<int>(b));
    const std::vector<BoneTrack> tr = SampleBoneTracks(asset, all, kRateHz);
    if (tr.empty()) {
        std::fprintf(stderr, "%s: no bone track\n", argv[1]);
        return 1;
    }

    std::ofstream out(std::filesystem::u8path(argv[2]), std::ios::binary);
    if (!out) {
        std::fprintf(stderr, "cannot write %s\n", argv[2]);
        return 1;
    }
    const double lo = 0.0;
    const double hi = asset.animations[0].duration;
    out << "# item=" << in.filename().u8string() << "\n# rate_hz=" << kRateHz << "\n# visible="
        << Format("%.6f,%.6f", lo, hi) << "\n# ref=\n# parent=";
    for (size_t b = 0; b < asset.skeleton.bones.size(); ++b) out << (b ? "," : "") << asset.skeleton.bones[b].parentIdx;
    out << "\nt";
    for (const SceneBone& b : asset.skeleton.bones)
        out << "," << b.name << ".x," << b.name << ".y," << b.name << ".z," << b.name << ".qw," << b.name << ".qx,"
            << b.name << ".qy," << b.name << ".qz";
    out << "\n";
    const size_t n = tr[0].pos.size();
    for (size_t i = 0; i < n; ++i) {
        out << Format("%.6f", static_cast<double>(i) / kRateHz);
        for (const BoneTrack& t : tr) {
            out << Format(",%.6f,%.6f,%.6f", t.pos[i].x, t.pos[i].y, t.pos[i].z);
            const Quatd q = i < t.rot_world.size() ? t.rot_world[i] : Quatd{};
            out << Format(",%.6f,%.6f,%.6f,%.6f", q.w, q.x, q.y, q.z);
        }
        out << "\n";
    }
    if (!out) {
        std::fprintf(stderr, "write failed: %s\n", argv[2]);
        return 1;
    }
    std::fprintf(stderr, "%s: %zu bones, %zu rows, %.3f s\n", argv[2], asset.skeleton.bones.size(), n, hi);
    return 0;
}
