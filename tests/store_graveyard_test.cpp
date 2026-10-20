// SPDX-License-Identifier: MIT
// Freeze audit #3: src/store_graveyard.h.

#include <cstdio>
#include <memory>
#include <vector>

#include "store_graveyard.h"

static int g_fail = 0;
#define CHECK(c)                                                         \
    do {                                                                 \
        if (!(c)) {                                                      \
            std::printf("FAIL line %d: %s\n", __LINE__, #c);             \
            ++g_fail;                                                    \
        }                                                                \
    } while (0)

namespace {
int g_alive = 0;
struct Probe {
    std::vector<int> v{1, 2, 3};
    Probe() { ++g_alive; }
    ~Probe() { --g_alive; }
};
}  // namespace

int main()
{
    rav::Graveyard<Probe> gy;
    auto a = std::make_unique<Probe>();
    const std::vector<int>* held = &a->v;  // a reference into the object a frame still holds
    gy.Retire(std::move(a));
    gy.Retire(nullptr);                      // nothing to keep
    CHECK(gy.Size() == 1 && g_alive == 1);   // retired, not freed
    CHECK(held->size() == 3);                // the held reference is still valid memory
    CHECK(gy.Sweep(false) == 0 && g_alive == 1);  // nested (a modal, a gesture up the stack): nothing freed
    gy.Retire(std::make_unique<Probe>());
    CHECK(g_alive == 2);
    CHECK(gy.Sweep(true) == 2 && g_alive == 0 && gy.Size() == 0);  // top level: all freed
    CHECK(gy.Sweep(true) == 0);
    if (g_fail) {
        std::printf("%d failure(s)\n", g_fail);
        return 1;
    }
    std::printf("store_graveyard: all passed\n");
    return 0;
}
