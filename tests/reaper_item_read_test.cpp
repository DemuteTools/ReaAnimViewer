// SPDX-License-Identifier: MIT
//
// Host test of src/reaper_item_read.h (13-51): the shared GUID compare and the growing text
// reader's policy (grow on a false answer AND on a full buffer, stop at the cap, no zero-fill).

#include "reaper_item_read.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace rav;

static int g_fail = 0;
#define CHECK(c)                                                         \
    do {                                                                 \
        if (!(c)) {                                                      \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);     \
            ++g_fail;                                                    \
        }                                                                \
    } while (0)

// A fake REAPER getter holding `text`. mode 0: cuts the text to the buffer (lstrcpyn, true);
// mode 1: answers false when the text does not fit; mode 2: always false.
struct Fake {
    std::string      text;
    int              mode = 0;
    std::vector<int> calls;
    bool             garbage_seen = false;
    bool operator()(char* buf, int size)
    {
        calls.push_back(size);
        // The reader must clear the first byte (and never rely on a zero-filled buffer).
        if (buf[0] != '\0') garbage_seen = true;
        if (mode == 2) return false;
        if (text.size() + 1 > static_cast<size_t>(size)) {
            if (mode == 1) return false;
            std::memcpy(buf, text.data(), static_cast<size_t>(size) - 1);
            buf[size - 1] = '\0';
            return true;
        }
        std::memcpy(buf, text.c_str(), text.size() + 1);
        return true;
    }
};

static bool Read(Fake& f, const std::vector<int>& sizes, std::string* out)
{
    return ReadGrowingText([&](char* b, int s) { return f(b, s); }, sizes, out);
}

int main()
{
    // ---- SameGuid ----
    CHECK(SameGuid("{AB-cd}", "{ab-CD}"));
    CHECK(SameGuid("{AB-CD}", "{AB-CD}"));
    CHECK(!SameGuid("", ""));
    CHECK(!SameGuid("{AB}", "{AB-}"));
    CHECK(!SameGuid("{AB}", "{AC}"));
    CHECK(!SameGuid("{AB}", ""));

    // ---- DoublingSizes ----
    {
        const std::vector<int> s = DoublingSizes(kItemChunkFirst, kItemChunkCap);
        CHECK(s.size() == 11);  // 64 KB .. 64 MB
        CHECK(s.front() == 64 * 1024 && s.back() == 64 * 1024 * 1024);
        CHECK(DoublingSizes(1, 100).empty());
        CHECK(DoublingSizes(100, 50).empty());
        CHECK(DoublingSizes(16, 40) == (std::vector<int>{16, 32}));
    }

    const std::vector<int> sizes = {16, 32, 64};
    // ---- fits the first buffer: one call ----
    {
        Fake        f;
        std::string out;
        f.text = "short";
        CHECK(Read(f, sizes, &out) && out == "short");
        CHECK(f.calls.size() == 1);
    }
    // ---- a cutting getter: a full buffer is asked again bigger ----
    {
        Fake        f;
        std::string out;
        f.text = std::string(40, 'x');
        CHECK(Read(f, sizes, &out) && out == f.text);
        CHECK(f.calls == (std::vector<int>{16, 32, 64}));
    }
    // ---- a refusing getter (false when too small): grows too (13-10 B8) ----
    {
        Fake        f;
        std::string out;
        f.mode = 1;
        f.text = std::string(20, 'y');
        CHECK(Read(f, sizes, &out) && out == f.text);
        CHECK(f.calls == (std::vector<int>{16, 32}));
    }
    // ---- a text at size - 2 may be cut: asked again; at size - 3 it fits ----
    {
        Fake        f, g;
        std::string out;
        f.text = std::string(14, 'z');  // 16 - 2
        CHECK(Read(f, sizes, &out) && out == f.text && f.calls.size() == 2);
        g.text = std::string(13, 'z');
        CHECK(Read(g, sizes, &out) && out == g.text && g.calls.size() == 1);
    }
    // ---- over the cap: fails, "" out, every size tried once ----
    {
        Fake        f, r;
        std::string out = "stale";
        f.text = std::string(100, 'q');
        CHECK(!Read(f, sizes, &out) && out.empty());
        CHECK(f.calls.size() == 3);
        r.mode = 1;
        r.text = f.text;
        CHECK(!Read(r, sizes, &out) && out.empty() && r.calls.size() == 3);
    }
    // ---- a real failure: tried at each size, then false; buffers start cleared ----
    {
        Fake        f;
        std::string out;
        f.mode = 2;
        CHECK(!Read(f, sizes, &out) && out.empty() && f.calls.size() == 3);
        CHECK(!f.garbage_seen);
    }
    // ---- empty text, a two-try policy (video FX state), no getter, no sizes ----
    {
        Fake        f, v;
        std::string out = "x";
        CHECK(Read(f, sizes, &out) && out.empty());
        v.mode = 1;
        v.text = std::string(20, 'b');
        CHECK(Read(v, {16, 1024}, &out) && out == v.text && v.calls == (std::vector<int>{16, 1024}));
        CHECK(!ReadGrowingText(TextGetter(), sizes, &out));
        CHECK(!Read(f, {}, &out));
    }
    // ---- a getter that leaves no terminator in a full buffer: never read past it ----
    {
        std::string out;
        int         n = 0;
        const bool  ok = ReadGrowingText(
            [&](char* b, int s) {
                ++n;
                std::memset(b, 'w', static_cast<size_t>(s));
                return true;
            },
            sizes, &out);
        CHECK(!ok && n == 3 && out.empty());
    }

    if (g_fail) {
        std::printf("%d check(s) failed\n", g_fail);
        return 1;
    }
    std::printf("reaper_item_read: all checks passed\n");
    return 0;
}
