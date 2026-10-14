// SPDX-License-Identifier: MIT
//
// Host test of src/action_link.h (lot 13-60): how a typed REAPER command id is written, compared and
// stored, and the action list's search.

#include "action_link.h"

#include <cstdio>

using namespace rav;

static int g_fail = 0;
#define CHECK(c)                                                              \
    do {                                                                      \
        if (!(c)) {                                                           \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);          \
            ++g_fail;                                                         \
        }                                                                     \
    } while (0)

static void TestNormalize()
{
    CHECK(NormalizeCommandId("990") == "990");
    CHECK(NormalizeCommandId("  0990 ") == "990");
    CHECK(NormalizeCommandId("\"_WOL_ADJENVTRHMOUSE\"") == "_WOL_ADJENVTRHMOUSE");
    CHECK(NormalizeCommandId("WOL_ADJENVTRHMOUSE") == "_WOL_ADJENVTRHMOUSE");  // ReverseNamedCommandLookup's form
    CHECK(NormalizeCommandId("_S&M_CYCLACTION_1") == "_S&M_CYCLACTION_1");     // SWS ids hold punctuation
    CHECK(NormalizeCommandId("_RS7d3c_0f1a2b") == "_RS7d3c_0f1a2b");
    CHECK(NormalizeCommandId("").empty());
    CHECK(NormalizeCommandId("   ").empty());
    CHECK(NormalizeCommandId("0").empty());     // REAPER's "no action"
    CHECK(NormalizeCommandId("000").empty());
    CHECK(NormalizeCommandId("_").empty());
    CHECK(NormalizeCommandId("zoom in").empty());  // a blank inside: words, not an id
    CHECK(NormalizeCommandId("1234567890").empty());  // too long for a command number
    CHECK(NormalizeCommandId(std::string(250, 'A')).empty());
}

static void TestLooksLikeId()
{
    CHECK(LooksLikeCommandId("990"));
    CHECK(LooksLikeCommandId("_WOL_ADJENVTRHMOUSE"));
    CHECK(LooksLikeCommandId("WOL_ADJENVTRHMOUSE"));
    CHECK(LooksLikeCommandId(" _SWS_ABOUT "));
    CHECK(LooksLikeCommandId("_abc"));
    CHECK(!LooksLikeCommandId("zoom"));
    CHECK(!LooksLikeCommandId("zoom vertically"));
    CHECK(!LooksLikeCommandId(""));
}

static void TestSame()
{
    CHECK(SameCommandId("_WOL_ADJENVTRHMOUSE", "_wol_adjenvtrhmouse"));
    CHECK(SameCommandId("WOL_ADJENVTRHMOUSE", "_WOL_ADJENVTRHMOUSE"));
    CHECK(SameCommandId("0990", "990"));
    CHECK(!SameCommandId("990", "9900"));
    CHECK(!SameCommandId("", ""));
    CHECK(!SameCommandId("0", "0"));
}

static void TestSearch()
{
    const std::string name = "View: Zoom vertically (MIDI CC relative/mousewheel)";
    CHECK(ActionSearchMatch(name, "1000", ""));
    CHECK(ActionSearchMatch(name, "1000", "zoom VERT"));
    CHECK(ActionSearchMatch(name, "1000", "  mousewheel   zoom "));
    CHECK(ActionSearchMatch(name, "1000", "100"));  // the id
    CHECK(!ActionSearchMatch(name, "1000", "zoom horizontally"));
    CHECK(ActionSearchMatch("SWS/wol: Adjust envelope or track height under mouse", "_WOL_ADJENVTRHMOUSE", "wol adj"));
}

static void TestStorage()
{
    CHECK(EncodeActionLink("WOL_ADJENVTRHMOUSE") == "link:_WOL_ADJENVTRHMOUSE");
    std::string id = "keep";
    CHECK(DecodeActionLink("link:990", &id) && id == "990");
    CHECK(DecodeActionLink("link:_WOL_ADJENVTRHMOUSE", &id) && id == "_WOL_ADJENVTRHMOUSE");
    id = "keep";
    CHECK(!DecodeActionLink("link:", &id) && id == "keep");
    CHECK(!DecodeActionLink("Ctrl+67", &id) && id == "keep");  // a key of its own: not a link
    CHECK(!DecodeActionLink("own:Wheel", &id));
    CHECK(!DecodeActionLink(nullptr, &id));
}

int main()
{
    TestNormalize();
    TestLooksLikeId();
    TestSame();
    TestSearch();
    TestStorage();
    if (g_fail) {
        std::printf("%d failure(s)\n", g_fail);
        return 1;
    }
    std::printf("action_link: all passed\n");
    return 0;
}
