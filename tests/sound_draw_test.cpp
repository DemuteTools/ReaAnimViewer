// SPDX-License-Identifier: MIT
//
// Host test of src/sound_draw.h (Epic 13 story 13-3): deterministic seeded draw, avoid-repeat,
// keep-or-draw, reshuffle, Blend parts and child order, event salt. Pure: any C++17 compiler.

#include "sound_draw.h"

#include <cstdio>
#include <set>
#include <string>
#include <vector>

using namespace rav;

namespace {

int g_fails = 0;

#define CHECK(c)                                             \
    do {                                                     \
        if (!(c)) {                                          \
            std::printf("FAIL line %d: %s\n", __LINE__, #c); \
            ++g_fails;                                       \
        }                                                    \
    } while (0)

void TestDraw()
{
    const std::vector<int> act = {0, 1, 2, 3};
    CHECK(DrawPick(act, 0, {}, 42, 7) == DrawPick(act, 0, {}, 42, 7));
    std::set<int> seen;
    for (uint64_t s = 0; s < 1000; ++s) seen.insert(DrawPick(act, 0, {}, 42, s));
    CHECK(seen.size() == 4);
    for (uint64_t s = 0; s < 200; ++s) CHECK(DrawPick({0, 1, 2}, 2, {2, 2, 0, 1}, 9, s) == 2);
    for (uint64_t s = 0; s < 200; ++s) {
        const int p = DrawPick({0, 1, 2}, 99, {0, 1}, 9, s);  // N clamped to 2
        CHECK(p == 2);
    }
    CHECK(DrawPick({5}, 3, {5}, 1, 1) == 5);  // N clamped to 0
    CHECK(DrawPick({}, 2, {}, 1, 2) == -1);
    CHECK(DrawPick({0, 1}, -4, {}, 1, 2) >= 0);
    // Golden values: the fixed splitmix64 mix gives these on every platform and build.
    CHECK(DrawPick(act, 0, {}, 42, 7) == 0);
    CHECK(DrawPick({0, 1, 2, 3, 4, 5, 6}, 0, {}, 1, 2) == 1);
    CHECK(DrawPick({0, 1, 2, 3, 4, 5, 6}, 0, {}, 123456789, 987654321) == 3);
}

void TestActiveKeep()
{
    SoundData d;
    d.frames.resize(3);
    d.frames[1].on = false;
    std::vector<int> a = ActiveVariations(d);
    CHECK(a.size() == 2 && a[0] == 0 && a[1] == 2);
    SoundData off;
    off.frames.resize(1);
    off.frames[0].on = false;
    CHECK(ActiveVariations(off).empty());

    SoundDraw prev;
    prev.pick = 1;
    prev.hand = true;
    CHECK(KeepOrDraw(&prev, {0, 1, 2}, 2, {1}, 3, 4) == 1);
    CHECK(KeepOrDraw(&prev, a, 0, {}, 3, 4) == DrawPick(a, 0, {}, 3, 4));  // pick 1 is off -> redraw
    CHECK(KeepOrDraw(nullptr, a, 0, {}, 3, 4) == DrawPick(a, 0, {}, 3, 4));
    // 13-62i (D, P3): a stored pick (not by hand) is kept only while it is not one of the last n of the history.
    SoundDraw stored;
    stored.pick = 1;
    CHECK(KeepOrDraw(&stored, {0, 1, 2, 3}, 2, {3, 0}, 3, 4) == 1);     // not in the last 2: kept
    CHECK(KeepOrDraw(&stored, {0, 1, 2, 3}, 2, {1, 0, 3}, 3, 4) == 1);  // repeated 3 events ago: kept
    for (uint64_t s = 0; s < 50; ++s) {
        const int p = KeepOrDraw(&stored, {0, 1, 2, 3}, 2, {0, 1}, 3, s);  // the last one: drawn again
        CHECK(p == 2 || p == 3);
    }
    CHECK(KeepOrDraw(&stored, {0, 1, 2, 3}, 0, {1}, 3, 4) == 1);  // avoid 0: nothing to avoid
    CHECK(KeepOrDraw(&stored, {1}, 2, {1}, 3, 4) == 1);           // n clamped to 0 (one variation)
    CHECK(KeepOrDraw(&prev, {0, 1, 2}, 2, {0, 1}, 3, 4) == 1);    // by hand: always kept
    // 13-62i (D): `ahead` (the picks kept after it, a filtered Populate) is avoided too, while it can be.
    for (uint64_t s = 0; s < 50; ++s) {
        CHECK(KeepOrDraw(&stored, {0, 1, 2, 3}, 2, {0}, 3, s, {1, 3}) == 2);
        CHECK(DrawPick({0, 1, 2, 3}, 2, {0, 2}, 3, s, {1, 3}) == 3);  // not both: the nearest one ahead (1) avoided
        CHECK(KeepOrDraw(&stored, {0, 1, 2, 3}, 2, {0, 2}, 3, s, {1, 3}) == 3);
    }
    CHECK(KeepOrDraw(&stored, {0, 1, 2, 3}, 2, {0}, 3, 4, {3}) == 1);  // not ahead: kept
    CHECK(KeepOrDraw(&prev, {0, 1, 2}, 2, {}, 3, 4, {1}) == 1);         // by hand: kept

    ItemSounds item;
    EventSounds ev;
    SoundDraw h, n;
    h.node = "a";
    h.pick = 1;
    h.hand = true;
    n.node = "b";
    n.pick = 0;
    ev.draws = {h, n};
    item.events = {ev};
    Reshuffle(item, 77);
    CHECK(item.seed == 77);
    CHECK(item.events[0].draws.size() == 1 && item.events[0].draws[0].node == "a");
    CHECK(KeepOrDraw(&item.events[0].draws[0], {0, 1, 2}, 2, {}, 77, 4) == 1);
}

void TestTree()
{
    SoundLibrary lib;
    SoundNode root, b, c1, c2, other;
    root.uid = "r";
    b.uid = "b";
    b.parent = "r";
    b.type = SoundNodeType::Blend;
    c1.uid = "c1";
    c1.parent = "b";
    c1.offset_ms = 0;
    c2.uid = "c2";
    c2.parent = "b";
    c2.offset_ms = 120;
    c2.volume_db = -3;
    other.uid = "o";
    other.parent = "r";
    lib.nodes = {root, c2, b, other, c1};
    const SoundNode& blend = lib.nodes[2];
    std::vector<const SoundNode*> parts = BlendParts(lib, blend);
    CHECK(parts.size() == 2 && parts[0]->uid == "c2" && parts[1]->uid == "c1");
    if (parts.size() == 2) CHECK(parts[0]->offset_ms == 120 && parts[0]->volume_db == -3);
    CHECK(BlendParts(lib, lib.nodes[0]).empty());
    std::vector<const SoundNode*> kids = ChildNodes(lib, lib.nodes[0]);
    CHECK(kids.size() == 2 && kids[0]->uid == "b" && kids[1]->uid == "o");
}

void TestSalt()
{
    CHECK(EventSalt("abc", 1.21) == EventSalt("abc", 1.19));
    CHECK(EventSalt("abc", 1.2) != EventSalt("abd", 1.2));
    CHECK(EventSalt("abc", 1.2) != EventSalt("abc", 2.2));
    CHECK(EventSalt("abc", 1.2) == 9190078331769735277ULL);  // golden: FNV-1a 64 + splitmix64
    CHECK(EventSalt("", 0.0) == 6566800829925814604ULL);
}

// 13-62h (Lot B): a Sound edited as zones: a variation plays a part when it has a zone of it (and is on); a whole
// event: any part zone. A Gap is no part zone.
void TestZonedActive()
{
    SoundData d;
    d.mode = DetectMode::Silence;
    d.divide_group = "g";
    d.divide_values = {"heel", "toe"};
    auto zone = [](double a, double b, const std::string& part) {
        EditZone z;
        z.a = z.slot_a = a;
        z.b = z.slot_b = b;
        z.part = part;
        return z;
    };
    for (int i = 0; i < 4; ++i) d.frames.push_back(SoundFrame{});
    d.frames[0].zones = {zone(0.0, 0.2, "heel"), zone(0.2, 0.4, "toe")};
    d.frames[1].zones = {zone(1.0, 1.2, "heel"), zone(1.2, 1.25, ""), zone(1.25, 1.4, "heel")};  // no toe
    d.frames[2].zones = {zone(2.0, 2.2, "toe")};
    d.frames[3].zones = {zone(3.0, 3.2, "toe")};
    d.frames[3].on = false;
    CHECK((ActiveVariationsFor(d, "heel") == std::vector<int>{0, 1}));
    CHECK((ActiveVariationsFor(d, "toe") == std::vector<int>{0, 2}));
    CHECK((ActiveVariationsFor(d, "") == std::vector<int>{0, 1, 2}));
    d.frames[2].zones.clear();  // a variation with no zone at all
    CHECK((ActiveVariationsFor(d, "") == std::vector<int>{0, 1}));
    CHECK((ActiveVariationsFor(d, "toe") == std::vector<int>{0}));
}

}  // namespace

int main()
{
    TestDraw();
    TestActiveKeep();
    TestTree();
    TestSalt();
    TestZonedActive();
    if (g_fails) {
        std::printf("sound_draw_test: %d failure(s)\n", g_fails);
        return 1;
    }
    std::printf("sound_draw_test: all passed\n");
    return 0;
}
