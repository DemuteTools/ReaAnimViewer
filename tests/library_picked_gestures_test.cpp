// SPDX-License-Identifier: MIT
//
// Host test of the Library view's picked gestures (the 13-8 rule, src/picked_gestures.h, applied
// to library_tree_ui.cpp / library_node_ui.cpp): the Library view's lists, menus, rename fields
// and drops iterate the project's sound store by reference; a gesture replaces that store whole
// (sound_project.cpp's ModifySoundStore). Each case draws over a real store, picks the gesture
// the Library view picks (its copies), finishes the loop over the old store, then runs the pick
// and checks the store it wrote. Built with -fsanitize=address it also shows that nothing reads
// freed memory.
//   c++ -std=c++17 -I src tests/library_picked_gestures_test.cpp src/library_tree_edit.cpp \
//       src/sound_switch.cpp src/sound_record.cpp src/rule_record.cpp -o t && ./t

#include "library_tree_edit.h"
#include "picked_gestures.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "sound_record.h"

using namespace rav;

namespace {

int g_fails = 0;

#define CHECK(c)                                                    \
    do {                                                            \
        if (!(c)) {                                                 \
            std::printf("FAIL line %d: %s\n", __LINE__, #c);        \
            ++g_fails;                                              \
        }                                                           \
    } while (0)

// sound_project.cpp's store and ModifySoundStore, without REAPER: the edit runs on a copy that
// then replaces the stored one (its vectors freed); one undo point per write that changed it.
std::map<int, SoundStore> g_stores;
int                       g_undo_points = 0;

const SoundStore& Store() { return g_stores[0]; }

bool Modify(const std::function<bool(SoundStore&)>& edit)
{
    SoundStore        copy = Store();
    const std::string before = SerializeSoundStore(copy);
    if (!edit(copy)) return false;
    if (SerializeSoundStore(copy) == before) return false;
    g_stores[0] = std::move(copy);
    ++g_undo_points;
    return true;
}

SoundNode Node(const std::string& uid, const std::string& parent, SoundNodeType t, const std::string& name)
{
    SoundNode n;
    n.uid = uid;
    n.parent = parent;
    n.type = t;
    n.name = name;
    return n;
}

// Group Material {Wood, Concrete, Grass}; library "Steps": root Random holding the Sounds a, b.
void Reset()
{
    SoundStore s;
    SwitchGroup g;
    g.uid = "gmat";
    g.name = "Material";
    g.values = {{"vwood", "Wood", {}}, {"vconc", "Concrete", {}}, {"vgrass", "Grass", {}}};
    s.groups.push_back(g);
    SoundLibrary l;
    l.uid = "L";
    l.name = "Steps";
    l.root = "r";
    l.nodes = {Node("r", "", SoundNodeType::Random, "Steps"), Node("a", "r", SoundNodeType::Sound, "A"),
               Node("b", "r", SoundNodeType::Sound, "B")};
    s.libraries.push_back(l);
    g_stores.clear();
    g_stores[0] = s;
    g_undo_points = 0;
}

}  // namespace

int main()
{
    // Switch Groups: a value renamed in its row's field, inside the loop over the groups and their
    // values (the rename used to run there). The loop reads every value of the old store to its
    // end; the rename runs after it, once, with the uids and the name it copied.
    {
        Reset();
        PickedGestures picks;
        std::vector<std::string> seen;
        for (const SwitchGroup& gr : Store().groups) {
            for (const SwitchValue& v : gr.values) {
                seen.push_back(v.name);
                if (v.uid == "vconc") {
                    std::string name = "Stone";  // the field's text, gone with this scope
                    picks.Pick([gid = gr.uid, vid = v.uid, name] {
                        Modify([&](SoundStore& s) { return RenameValue(s, gid, vid, name); });
                    });
                }
            }
            seen.push_back(std::to_string(gr.values.size()));  // the row's value count, after the pick
        }
        CHECK(g_undo_points == 0);  // nothing written while the list draws
        CHECK(seen.size() == 4 && seen[1] == "Concrete" && seen[2] == "Grass" && seen[3] == "3");
        CHECK(picks.Run() == 1);
        CHECK(g_undo_points == 1);
        CHECK(Store().groups.front().values[1].name == "Stone");
        CHECK(picks.Run() == 0);  // once
    }

    // The row menu: "Add child > Sound" picked, then the menu's next items still ask CanAddAt
    // about the library they hold by reference (they used to read it after the add had replaced
    // the store). The add runs after the menu with the rows it copied: one undo point.
    {
        Reset();
        PickedGestures      picks;
        const SoundLibrary& lib = *FindLibrary(Store(), "L");
        const std::vector<LibraryRow> rows = {{"", "r"}};
        int asked = 0;
        for (SoundNodeType t : {SoundNodeType::Sound, SoundNodeType::Random, SoundNodeType::Blend}) {
            std::string why;
            if (CanAddAt(Store(), lib, &rows.front(), t, &why)) ++asked;
            if (t == SoundNodeType::Sound)
                picks.Pick([rows] {
                    NewNodeSpec sp;
                    sp.type = SoundNodeType::Sound;
                    sp.name = "New Sound";
                    std::vector<std::string> made;
                    Modify([&](SoundStore& s) { return AddNodes(s, "L", &rows.front(), {sp}, &made, nullptr); });
                });
        }
        CHECK(asked == 3);
        CHECK(lib.nodes.size() == 3);  // the store the menu drew is the one it reads to its end
        CHECK(picks.Run() == 1);
        CHECK(g_undo_points == 1);
        CHECK(FindLibrary(Store(), "L")->nodes.size() == 4);
    }

    // A library renamed in its list row: the row still draws its dot from the library after the
    // field closes (it used to read the replaced library's colour).
    {
        Reset();
        PickedGestures picks;
        uint32_t       dot = 0;
        for (const SoundLibrary& l : Store().libraries) {
            picks.Pick([uid = l.uid, name = std::string("Feet")] {
                Modify([&](SoundStore& s) { return RenameLibrary(s, uid, name); });
            });
            dot = l.color | 0x1000000u;  // read after the pick
        }
        CHECK(dot == 0x1000000u);
        CHECK(picks.Run() == 1);
        CHECK(FindLibrary(Store(), "L")->name == "Feet");
    }

    // A Media Explorer drop: the payload is ImGui's for this frame only, so the pick copies it.
    {
        Reset();
        PickedGestures picks;
        std::string    got;
        {
            auto payload = std::make_unique<char[]>(4);
            std::memcpy(payload.get(), "drop", 4);
            const char*       d = payload.get();
            std::vector<char> data(d, d + 4);
            picks.Pick([data, &got] { got.assign(data.begin(), data.end()); });
            std::memset(payload.get(), 0, 4);  // ImGui reuses its payload buffer
        }
        CHECK(picks.Run() == 1);
        CHECK(got == "drop");
    }

    // The node panel: an edit picked from a section whose locals (the dragged value, the node it
    // edits) are gone when it runs; it captured them by value. A pick made while the picks run
    // (a picked Locate writing through the picked path) waits for the next Run, so a gesture that
    // already runs after the panel writes at once.
    {
        Reset();
        PickedGestures picks;
        {
            const std::string lib_uid = "L", uid = "a";
            const double      v = -6.0;
            picks.Pick([lib_uid, uid, v] {
                Modify([&](SoundStore& s) {
                    SoundLibrary* l = FindLibrary(s, lib_uid);
                    SoundNode*    n = l ? FindNode(*l, uid) : nullptr;
                    if (!n) return false;
                    n->volume_db = v;
                    return true;
                });
            });
        }
        bool nested_ran = false;
        picks.Pick([&picks, &nested_ran] { picks.Pick([&nested_ran] { nested_ran = true; }); });
        CHECK(picks.Run() == 2);
        CHECK(FindNode(*FindLibrary(Store(), "L"), "a")->volume_db == -6.0);
        CHECK(!nested_ran);  // waits for the next Run: hence Locate's write is not picked again
        CHECK(picks.Run() == 1);
        CHECK(nested_ran);
    }

    // A gesture that changes nothing (the same name): no undo point, as before.
    {
        Reset();
        PickedGestures picks;
        picks.Pick([] { Modify([](SoundStore& s) { return RenameValue(s, "gmat", "vwood", "Wood"); }); });
        picks.Run();
        CHECK(g_undo_points == 0);
    }

    if (g_fails == 0) std::printf("library_picked_gestures: all passed\n");
    return g_fails == 0 ? 0 : 1;
}
