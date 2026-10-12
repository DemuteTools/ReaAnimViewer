// SPDX-License-Identifier: MIT
//
// Host test of the gesture Undo guard (src/undo_guard.h, 13-7a / 13-10): Undo is offered only
// while REAPER's last undo point is the gesture's own and the project has not changed since.
//   c++ -std=c++17 -I src tests/undo_guard_test.cpp -o t && ./t

#include "undo_guard.h"

#include <cstdio>

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

}  // namespace

int main()
{
    // The gesture is the last point, nothing changed since.
    CHECK(GestureUndoAvailable("RAV: Add sounds", "RAV: Add sounds", 12, 12));
    // State not read yet: the description alone decides.
    CHECK(GestureUndoAvailable("RAV: Add sounds", "RAV: Add sounds", 12, -1));
    // Another REAPER edit since: a different last point.
    CHECK(!GestureUndoAvailable("Move items", "RAV: Add sounds", 13, 12));
    // Same description but the project changed since (e.g. the same gesture made elsewhere, an undo).
    CHECK(!GestureUndoAvailable("RAV: Add sounds", "RAV: Add sounds", 13, 12));
    // Nothing to undo, or the gesture's name unknown.
    CHECK(!GestureUndoAvailable(nullptr, "RAV: Add sounds", 12, 12));
    CHECK(!GestureUndoAvailable("RAV: Add sounds", "", 12, 12));
    // A prefix is not a match.
    CHECK(!GestureUndoAvailable("RAV: Add sounds 2", "RAV: Add sounds", 12, 12));

    if (g_fails) {
        std::printf("%d failure(s)\n", g_fails);
        return 1;
    }
    std::printf("undo_guard: all passed\n");
    return 0;
}
