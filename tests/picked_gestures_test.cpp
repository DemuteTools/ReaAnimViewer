// SPDX-License-Identifier: MIT
//
// Host test of src/picked_gestures.h (13-8 fix): a menu picks, the gesture runs after the widgets,
// with its own copies, once, never re-entered.
//   c++ -std=c++17 -I src tests/picked_gestures_test.cpp -o t && ./t

#include "picked_gestures.h"

#include <cstdio>
#include <map>
#include <string>
#include <vector>

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

// The shape of the bug: a project's store, replaced whole by a gesture (ModifySoundStore).
struct Value {
    std::string uid, name;
};
struct Store {
    std::vector<Value> values;
};

}  // namespace

int main()
{
    // A menu over the store picks its second value; the gesture replaces the store and writes the
    // pick into the rule. The menu reads the whole list as it was, the rule gets the picked value.
    {
        std::map<int, Store> stores;
        stores[0].values = {{"u-wood-0000000000000000000000000001", "Wood"},
                            {"u-concrete-00000000000000000000000002", "Concrete"},
                            {"u-grass-000000000000000000000000003", "Grass"}};
        std::string     rule_value;
        PickedGestures  picks;
        const Store&    shown = stores[0];
        std::vector<std::string> seen;
        for (const Value& v : shown.values) {  // the menu
            seen.push_back(v.name);
            if (v.name == "Concrete")
                picks.Pick([&stores, &rule_value, uid = v.uid]() {
                    Store copy = stores[0];
                    copy.values.push_back({"u-new-0000000000000000000000000004", "New"});
                    stores[0] = std::move(copy);  // the old values are freed here
                    rule_value = uid;             // the closure's own copy
                });
        }
        CHECK(rule_value.empty());  // nothing ran inside the menu
        CHECK(seen.size() == 3 && seen[1] == "Concrete" && seen[2] == "Grass");
        CHECK(picks.Pending());
        CHECK(picks.Run() == 1);
        CHECK(rule_value == "u-concrete-00000000000000000000000002");
        CHECK(stores[0].values.size() == 4);
        CHECK(!picks.Pending());
        CHECK(picks.Run() == 0);  // once
        CHECK(rule_value == "u-concrete-00000000000000000000000002");
    }

    // Several picks in one frame: each once, in pick order.
    {
        PickedGestures picks;
        std::string    order;
        picks.Pick([&order]() { order += "a"; });
        picks.Pick([&order]() { order += "b"; });
        picks.Pick(nullptr);  // nothing picked
        CHECK(picks.Run() == 2);
        CHECK(order == "ab");
    }

    // A pick made while a gesture runs waits for the next Run; a Run from inside a gesture does
    // nothing (no gesture runs inside another).
    {
        PickedGestures picks;
        int            outer = 0, inner = 0;
        size_t         nested = 99;
        picks.Pick([&]() {
            ++outer;
            picks.Pick([&inner]() { ++inner; });
            nested = picks.Run();
        });
        CHECK(picks.Run() == 1);
        CHECK(outer == 1 && inner == 0 && nested == 0);
        CHECK(picks.Pending());
        CHECK(picks.Run() == 1);
        CHECK(outer == 1 && inner == 1);
        CHECK(!picks.Pending());
    }

    // A gesture that throws leaves the queue usable (the flag is cleared).
    {
        PickedGestures picks;
        int            ran = 0;
        picks.Pick([]() { throw 1; });
        bool threw = false;
        try {
            picks.Run();
        } catch (...) {
            threw = true;
        }
        CHECK(threw);
        picks.Pick([&ran]() { ++ran; });
        CHECK(picks.Run() == 1);
        CHECK(ran == 1);
    }

    if (g_fails) {
        std::printf("%d failure(s)\n", g_fails);
        return 1;
    }
    std::printf("picked_gestures: all passed\n");
    return 0;
}
