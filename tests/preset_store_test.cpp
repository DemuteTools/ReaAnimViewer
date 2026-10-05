// SPDX-License-Identifier: MIT
//
// Host test of the preset store (src/preset_store.h, story 10-2): the embedded Footsteps
// presets are Footsteps Heel and Footsteps Toe, factory presets are read-only, user
// presets on disk (save bumps the version, save as, rename, delete, import, export), and
// the item states Legacy / Edited / UpToDate / Unknown. Works in a temp folder.

#include "bone_presets.h"
#include "preset_store.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "factory_presets.h"

using namespace rav;
namespace fs = std::filesystem;

namespace {

int g_fails = 0;

#define CHECK(c)                                                    \
    do {                                                            \
        if (!(c)) {                                                 \
            std::printf("FAIL line %d: %s\n", __LINE__, #c);        \
            ++g_fails;                                              \
        }                                                           \
    } while (0)

// FNV-1a 64 of a preset's text, CR dropped (a CRLF checkout pins the same).
unsigned long long TextHash(const std::string& text)
{
    unsigned long long h = 1469598103934665603ull;
    for (char c : text) {
        if (c == '\r') continue;
        h ^= static_cast<unsigned char>(c);
        h *= 1099511628211ull;
    }
    return h;
}

// Each factory preset's version and text, pinned: a text change without a version bump
// would never show as Legacy on the items that copied it.
struct FactoryPin {
    const char*        stem;
    int                version;
    unsigned long long hash;
};
const FactoryPin kFactoryPins[] = {
    {"footsteps", 2, 0x18d5e6abc3bc225cull},
    {"footsteps_toe", 1, 0xa26f7eff3fa43fe8ull},
};

std::string ReadAll(const fs::path& p)
{
    std::ifstream      in(p, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

}  // namespace

int main()
{
    std::random_device rd;
    const fs::path root = fs::temp_directory_path() / ("rav_preset_test_" + std::to_string(rd()));
    fs::create_directories(root);
    const std::string R = root.u8string();
    const fs::path    user_dir = fs::u8path(UserPresetDir(R));

    // The factory footstep presets (10-4 follow-up, Antho 2026-10-05): Footsteps Heel (v2,
    // replaces Footsteps v1, same id) and Footsteps Toe. One rule per foot, one condition (the
    // bone's height above the floor goes below the threshold), the marker at the start of the
    // match, strength = the bone's downward vertical speed.
    {
        struct Want {
            const char* id;
            int         version;
            const char* name;
            const char* left;
            const char* right;
        };
        const Want wants[] = {{"factory/footsteps", 2, "Footsteps Heel", "role:left_heel", "role:right_heel"},
                              {"factory/footsteps_toe", 1, "Footsteps Toe", "role:left_toe", "role:right_toe"}};
        for (const Want& w : wants) {
            PresetData  d;
            std::string err;
            CHECK(LoadPreset(R, w.id, &d, &err));
            CHECK(d.id == w.id && d.version == w.version && d.name == w.name);
            CHECK(d.blocks.size() == 2);
            if (d.blocks.size() != 2) continue;
            const char* markers[2] = {"Footstep L", "Footstep R"};
            const char* bones[2] = {w.left, w.right};
            for (int k = 0; k < 2; ++k) {
                const Block& blk = d.blocks[static_cast<size_t>(k)];
                CHECK(blk.marker == markers[k] && blk.enabled && blk.landing == Landing::Crossing);
                CHECK(blk.conditions.size() == 1);
                if (blk.conditions.size() != 1) continue;
                const SignalSpec& sg = blk.conditions[0].signal;
                CHECK(sg.quantity == Quantity::Point && sg.measure == Measure::Position && sg.axis == Axis::Vertical &&
                      sg.reference == Reference::Floor && blk.conditions[0].dir == Direction::Below);
                CHECK(sg.bones.size() == 1 && sg.bones[0] == BoneRefId(bones[k]));
                CHECK(blk.strength_signal.bones.size() == 1 && blk.strength_signal.bones[0] == BoneRefId(bones[k]) &&
                      blk.strength_signal.measure == Measure::Speed && blk.strength_sign == -1.0);
            }
            CHECK(!HasKeptText(d));
        }
    }

    {

        // Every shipped file reads fully (a typo would otherwise ship as a kept unknown
        // field), names itself after its file, is listed, and is in the canonical form.
        size_t n = 0;
        const FactoryPresetSource* src = FactoryPresetSources(&n);
        CHECK(n >= 1);
        const std::vector<PresetInfo> list = ListPresets(R);
        for (size_t i = 0; i < n; ++i) {
            PresetData p;
            const bool parsed = ParsePreset(src[i].text, &p);
            CHECK(parsed);
            if (!parsed) std::printf("  factory preset \"%s\" does not parse\n", src[i].stem);
            CHECK(!HasKeptText(p));
            if (HasKeptText(p)) std::printf("  factory preset \"%s\" has text this version does not read\n", src[i].stem);
            CHECK(p.id == std::string("factory/") + src[i].stem);
            CHECK(SerializePreset(p) == src[i].text);
            const FactoryPin* pin = nullptr;
            for (const FactoryPin& fp : kFactoryPins)
                if (std::string(fp.stem) == src[i].stem) pin = &fp;
            CHECK(pin != nullptr);
            if (!pin) std::printf("  factory preset %s has no pin: add it to kFactoryPins\n", src[i].stem);
            if (pin && (pin->version != p.version || pin->hash != TextHash(src[i].text))) {
                if (pin->version == p.version)
                    std::printf("factory preset %s changed: bump its version and update the pin (hash 0x%016llxull)\n",
                                src[i].stem, TextHash(src[i].text));
                else
                    std::printf("factory preset %s is now version %d: update the pin to {\"%s\", %d, 0x%016llxull}\n",
                                src[i].stem, p.version, src[i].stem, p.version, TextHash(src[i].text));
                CHECK(pin->version == p.version && pin->hash == TextHash(src[i].text));
            }
            bool listed = false;
            for (const PresetInfo& info : list) listed = listed || info.id == std::string("factory/") + src[i].stem;
            CHECK(listed);
        }
    }

    // List: factory first, built-in.
    {
        const std::vector<PresetInfo> list = ListPresets(R);
        CHECK(!list.empty() && list[0].id == "factory/footsteps" && list[0].factory && list[0].name == "Footsteps Heel" &&
              list[0].path.empty());
    }

    // Factory presets are read-only: refused with a reason, nothing written.
    {
        PresetData  d;
        std::string err;
        CHECK(LoadPreset(R, "factory/footsteps", &d, &err));
        int v = 0;
        err.clear();
        CHECK(!SavePreset(R, "factory/footsteps", d, &v, &err) && !err.empty());
        err.clear();
        CHECK(!DeletePreset(R, "factory/footsteps", &err) && !err.empty());
        err.clear();
        CHECK(!RenamePreset(R, "factory/footsteps", "Mine", &err) && !err.empty());
        CHECK(!fs::exists(user_dir));
        PresetInfo info;
        CHECK(FindPreset(R, "factory/footsteps", &info) && info.version == 2);
    }

    // User presets: save as, save (version + 1), rename, list, export, import, delete.
    std::string id;
    {
        PresetData fs_;
        std::string err;
        CHECK(LoadPreset(R, "factory/footsteps", &fs_, &err));
        fs_.blocks[0].cooldown_ms = 300;
        CHECK(!SavePresetAs(R, "  ", fs_, &id, &err) && !err.empty());
        CHECK(!SavePresetAs(R, "footsteps heel", fs_, &id, &err) && !err.empty());  // the name is taken (any case)
        CHECK(SavePresetAs(R, "My Steps!", fs_, &id, &err));
        CHECK(id == "user/my-steps");
        CHECK(fs::exists(user_dir / "my-steps.ravpreset"));
        PresetData back;
        CHECK(LoadPreset(R, id, &back, &err));
        CHECK(back.version == 1 && back.name == "My Steps!" && BlocksEqual(back.blocks, fs_.blocks));

        int v = 0;
        fs_.blocks[0].cooldown_ms = 320;
        CHECK(SavePreset(R, id, fs_, &v, &err) && v == 2);
        CHECK(LoadPreset(R, id, &back, &err) && back.version == 2 && back.blocks[0].cooldown_ms == 320);

        CHECK(RenamePreset(R, id, "Soft steps", &err));
        PresetInfo info;
        CHECK(FindPreset(R, id, &info) && info.name == "Soft steps" && info.version == 2 && !info.factory);
        CHECK(!RenamePreset(R, id, "FOOTSTEPS HEEL", &err) && !err.empty());

        const std::vector<PresetInfo> list = ListPresets(R);
        CHECK(list.size() == 3 && list[2].id == id);

        const fs::path out = root / "export" / "soft.ravpreset";
        CHECK(ExportPreset(R, id, out.u8string(), &err));
        CHECK(ReadAll(out) == ReadAll(user_dir / "my-steps.ravpreset"));
        std::string id2;
        CHECK(ImportPreset(R, out.u8string(), &id2, &err));
        CHECK(id2 != id);
        CHECK(FindPreset(R, id2, &info) && info.name == "Soft steps (2)");
        CHECK(LoadPreset(R, id2, &back, &err) && BlocksEqual(back.blocks, fs_.blocks));

        const fs::path fout = root / "export" / "footsteps.ravpreset";
        CHECK(ExportPreset(R, "factory/footsteps", fout.u8string(), &err));
        std::string id3;
        CHECK(ImportPreset(R, fout.u8string(), &id3, &err));
        CHECK(id3.compare(0, 5, "user/") == 0 && FindPreset(R, id3, &info) && info.name == "Footsteps Heel (2)");

        const fs::path junk = root / "junk.ravpreset";
        std::ofstream(junk) << "not a preset";
        CHECK(!ImportPreset(R, junk.u8string(), &id2, &err) && !err.empty());

        CHECK(DeletePreset(R, id3, &err));
        CHECK(!FindPreset(R, id3, &info));
        CHECK(!DeletePreset(R, "user/../../etc", &err));
        CHECK(!FindPreset(R, "user/", &info) && !FindPreset(R, "user/aux", &info));
    }

    // Two names with the same slug: the second gets its own file, the first keeps its blocks.
    {
        PresetData  a, b, back;
        std::string err, ida, idb;
        CHECK(LoadPreset(R, "factory/footsteps", &a, &err));
        b = a;
        a.blocks[0].cooldown_ms = 401;
        b.blocks[0].cooldown_ms = 402;
        CHECK(SavePresetAs(R, "Our Steps!", a, &ida, &err) && ida == "user/our-steps");
        CHECK(SavePresetAs(R, "Our steps?", b, &idb, &err) && idb == "user/our-steps-2");
        CHECK(LoadPreset(R, ida, &back, &err) && back.name == "Our Steps!" && back.blocks[0].cooldown_ms == 401);
        CHECK(LoadPreset(R, idb, &back, &err) && back.name == "Our steps?" && back.blocks[0].cooldown_ms == 402);
        CHECK(DeletePreset(R, ida, &err) && DeletePreset(R, idb, &err));
    }

    // A user file from a newer RAV: Save keeps its name, its unknown line and field, in place.
    {
        fs::create_directories(user_dir);
        const std::string text = "RAVPRESET 2 flags=x\n"
                                 "future header line\n"
                                 "preset id=user/odd version=3 owner=me name=Odd one\n"
                                 "options sensitivity=0 edge_ms=0 smooth_ms=8\n"
                                 "analyse floor_pct=2 pos_frac=0.25 speed_pct=30 margin_ratio=0.5 onset_frac=0.1 "
                                 "per_bone_floor=0\n"
                                 "block color=none hold_ms=0 cooldown_ms=0 offset_ms=0 land=cross marker=A\n";
        std::ofstream(user_dir / "odd.ravpreset", std::ios::binary) << text;
        PresetData  d;
        std::string err;
        CHECK(LoadPreset(R, "user/odd", &d, &err) && d.name == "Odd one" && d.version == 3);
        d.blocks[0].cooldown_ms = 50;
        int v = 0;
        CHECK(SavePreset(R, "user/odd", d, &v, &err) && v == 4);
        const std::string out = ReadAll(user_dir / "odd.ravpreset");
        CHECK(out.rfind("RAVPRESET 2 flags=x\nfuture header line\n", 0) == 0);
        CHECK(out.find("\npreset id=user/odd version=4 owner=me name=Odd one\n") != std::string::npos);
        CHECK(out.find("cooldown_ms=50") != std::string::npos);
        CHECK(DeletePreset(R, "user/odd", &err));
    }

    // A file dropped into the folder by hand, any name, any extension case: listed and loaded.
    {
        fs::create_directories(user_dir);
        PresetData  d;
        std::string err;
        CHECK(LoadPreset(R, "factory/footsteps", &d, &err));
        d.name = "Hand made";
        std::ofstream(user_dir / "My Steps.RAVPRESET", std::ios::binary) << SerializePreset(d);
        bool listed = false;
        for (const PresetInfo& info : ListPresets(R)) listed = listed || (info.id == "user/My Steps" && info.name == "Hand made");
        CHECK(listed);
        PresetData back;
        CHECK(LoadPreset(R, "user/My Steps", &back, &err) && BlocksEqual(back.blocks, d.blocks));
        PresetInfo info;
        CHECK(!FindPreset(R, "user/a:b", &info) && !FindPreset(R, "user/x.", &info) && !FindPreset(R, "user/Aux.v2", &info) &&
              !FindPreset(R, "user/\xC3", &info));
        CHECK(DeletePreset(R, "user/My Steps", &err) && !fs::exists(user_dir / "My Steps.RAVPRESET"));
    }

    // File names: [a-z0-9-], cut before the trailing '-' is stripped, never a Windows device name.
    {
        PresetData  content;
        std::string err, nid;
        CHECK(SavePresetAs(R, "Aux", content, &nid, &err) && nid == "user/aux-preset");
        CHECK(fs::exists(user_dir / "aux-preset.ravpreset"));
        CHECK(SavePresetAs(R, "COM1", content, &nid, &err) && nid == "user/com1-preset");
        CHECK(SavePresetAs(R, "com10", content, &nid, &err) && nid == "user/com10");
        CHECK(SavePresetAs(R, "!!!", content, &nid, &err) && nid == "user/preset");
        // 47 letters, then " x": the cut lands on the '-'.
        const std::string longname = std::string(47, 'a') + " xyz";
        CHECK(SavePresetAs(R, longname, content, &nid, &err) && nid == "user/" + std::string(47, 'a'));
        CHECK(SavePresetAs(R, "D\xC3\xA9j\xC3\xA0 vu", content, &nid, &err) && nid == "user/d-j-vu");
        PresetInfo info;
        CHECK(FindPreset(R, nid, &info) && info.name == "D\xC3\xA9j\xC3\xA0 vu");
        for (const char* id : {"user/aux-preset", "user/com1-preset", "user/com10", "user/preset", "user/d-j-vu"})
            CHECK(DeletePreset(R, id, &err));
        CHECK(DeletePreset(R, "user/" + std::string(47, 'a'), &err));
    }

    // Item states.
    {
        PresetData  fact;
        std::string err;
        CHECK(LoadPreset(R, "factory/footsteps", &fact, &err));

        ItemRules none;
        CHECK(GetPresetState(R, none) == PresetState::Unknown);

        ItemRules item;
        ApplyPreset(item, fact);
        CHECK(item.has_preset && item.preset_copy.id == "factory/footsteps" && item.preset_copy.version == 2);
        CHECK(BlocksEqual(item.blocks, fact.blocks));
        CHECK(GetPresetState(R, item) == PresetState::UpToDate);

        // Edited: a project tweak; reloading the preset clears it.
        item.blocks[0].conditions[0].threshold = 0.07;
        CHECK(GetPresetState(R, item) == PresetState::Edited);
        CHECK(UpdateFromPreset(R, item, &err));
        CHECK(GetPresetState(R, item) == PresetState::UpToDate);
        // An options or analyse tweak is a project tweak too.
        CHECK(OptionsEqual(item.preset_copy.options, fact.options) && AnalyseEqual(item.preset_copy.analyse, fact.analyse));
        item.options.sensitivity = 0.3;
        CHECK(GetPresetState(R, item) == PresetState::Edited);
        CHECK(UpdateFromPreset(R, item, &err) && GetPresetState(R, item) == PresetState::UpToDate);
        item.analyse.per_bone_floor = !item.analyse.per_bone_floor;
        CHECK(GetPresetState(R, item) == PresetState::Edited);
        CHECK(UpdateFromPreset(R, item, &err) && GetPresetState(R, item) == PresetState::UpToDate);
        // Kept text alone is not an edit.
        item.blocks[0].kept.lines.push_back("future line");
        CHECK(GetPresetState(R, item) == PresetState::UpToDate);
        item.blocks[0].kept.lines.clear();

        // Legacy: the item's copy is footsteps v2, the factory is now v3. Rules unchanged until Update.
        PresetInfo v2;
        v2.id = "factory/footsteps";
        v2.version = 3;
        v2.factory = true;
        const std::vector<Block> before = item.blocks;
        CHECK(PresetStateOf(item, &v2) == PresetState::Legacy);
        CHECK(BlocksEqual(item.blocks, before));
        CHECK(PresetStateOf(item, nullptr) == PresetState::Unknown);
        item.preset_copy.kept_version = 3;  // Keep current dismissed v3
        CHECK(PresetStateOf(item, &v2) == PresetState::UpToDate);
        v2.version = 4;
        CHECK(PresetStateOf(item, &v2) == PresetState::Legacy);
        // An older version (a downgrade, or the id re-created) is not the copied one either.
        item.preset_copy.kept_version = 0;
        item.preset_copy.version = 5;
        CHECK(PresetStateOf(item, &v2) == PresetState::Legacy);
        item.preset_copy.version = 2;
        item.preset_copy.kept_version = 3;

        // The same on disk, with a user preset.
        ItemRules mine;
        PresetData u;
        CHECK(LoadPreset(R, id, &u, &err));
        ApplyPreset(mine, u);
        CHECK(GetPresetState(R, mine) == PresetState::UpToDate);
        PresetData content = u;
        content.blocks[1].offset_ms = 5;
        int v = 0;
        CHECK(SavePreset(R, id, content, &v, &err) && v == u.version + 1);
        CHECK(GetPresetState(R, mine) == PresetState::Legacy);
        CHECK(mine.blocks[1].offset_ms == 0);  // never rewritten without an explicit Update
        CHECK(KeepCurrent(R, mine, &err));
        CHECK(mine.preset_copy.kept_version == v);
        CHECK(GetPresetState(R, mine) == PresetState::UpToDate);
        CHECK(SavePreset(R, id, content, &v, &err));
        CHECK(GetPresetState(R, mine) == PresetState::Legacy);
        CHECK(UpdateFromPreset(R, mine, &err));
        CHECK(GetPresetState(R, mine) == PresetState::UpToDate && mine.blocks[1].offset_ms == 5 &&
              mine.preset_copy.version == v && mine.preset_copy.kept_version == 0);

        // Edited, then saved over its preset: no longer edited.
        mine.blocks[0].cooldown_ms = 111;
        CHECK(GetPresetState(R, mine) == PresetState::Edited);
        mine.options.edge_margin_ms = 20;
        PresetData saved, content2;
        content2.options = mine.options;
        content2.analyse = mine.analyse;
        content2.blocks = mine.blocks;
        CHECK(SavePreset(R, id, content2, &v, &err));
        CHECK(LoadPreset(R, id, &saved, &err));
        AdoptSavedPreset(mine, saved);
        CHECK(GetPresetState(R, mine) == PresetState::UpToDate);
        CHECK(saved.options.edge_margin_ms == 20 && mine.preset_copy.options.edge_margin_ms == 20);

        // The preset is gone: Unknown, Update refused.
        const int copied = mine.preset_copy.version;
        CHECK(copied > 1);
        CHECK(DeletePreset(R, id, &err));
        CHECK(GetPresetState(R, mine) == PresetState::Unknown);
        CHECK(!UpdateFromPreset(R, mine, &err) && !err.empty());

        // Re-created under the same name (same id, version 1): Legacy, not UpToDate.
        std::string again;
        CHECK(SavePresetAs(R, "My steps", content2, &again, &err) && again == id);
        CHECK(GetPresetState(R, mine) == PresetState::Legacy);
        CHECK(DeletePreset(R, id, &err));

        // The item record round-trips with its preset copy.
        ItemRules p;
        CHECK(ParseItemRules(SerializeItemRules(item), &p));
        CHECK(SerializeItemRules(p) == SerializeItemRules(item));
        CHECK(BlocksEqual(p.preset_copy.blocks, item.preset_copy.blocks) && p.preset_copy.kept_version == 3);
    }

    std::error_code ec;
    fs::remove_all(root, ec);
    if (g_fails) {
        std::printf("preset_store_test: %d failure(s)\n", g_fails);
        return 1;
    }
    std::printf("preset_store_test: all passed\n");
    return 0;
}
