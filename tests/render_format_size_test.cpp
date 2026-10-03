// SPDX-License-Identifier: MIT
//
// Host test of the render format size patch (src/render_format_size.h, spec 11-fb-13):
// base64 round trip, the PMFF (FFmpeg video) and GIF layouts, odd -> even, refused tags
// and short blobs, the already-equal case. No REAPER, no Windows.
//   c++ -std=c++17 -I src tests/render_format_size_test.cpp -o t && ./t

#include "render_format_size.h"

#include <cstdio>
#include <string>

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

// MP4 (PMFF, format 3, H.264 / AAC), 1920 x 1080, 30 fps, keep aspect, no options: the
// RENDER_FORMAT layout REAPER writes for FFmpeg video (46 bytes).
const char* const kMp4_1080 = "UE1GRgMAAAAAAAAAAAgAAAIAAACAAAAAgAcAADgEAAAAAPBBAQAAAAEAAAAAAA==";
// GIF, 640 x 360, 30 fps max, keep aspect (19 bytes).
const char* const kGif_360 = "IEZJR4ACAABoAQAAAADwQQEAAA==";
// WAV 24 bit, REAPER's default ("evaw").
const char* const kWav = "ZXZhdxgAAA==";

int I32(const std::string& b, size_t at)
{
    return static_cast<int>(static_cast<unsigned>(static_cast<unsigned char>(b[at])) |
                            (static_cast<unsigned>(static_cast<unsigned char>(b[at + 1])) << 8) |
                            (static_cast<unsigned>(static_cast<unsigned char>(b[at + 2])) << 16) |
                            (static_cast<unsigned>(static_cast<unsigned char>(b[at + 3])) << 24));
}

}  // namespace

int main()
{
    // Base64: known vectors, round trip of every byte, padding optional, junk refused.
    {
        std::string out;
        CHECK(Base64Encode("") == "");
        CHECK(Base64Encode("f") == "Zg==");
        CHECK(Base64Encode("fo") == "Zm8=");
        CHECK(Base64Encode("foo") == "Zm9v");
        CHECK(Base64Encode("foobar") == "Zm9vYmFy");
        CHECK(Base64Decode("Zm9vYg==", &out) && out == "foob");
        CHECK(Base64Decode("Zm9vYg", &out) && out == "foob");          // no padding
        CHECK(Base64Decode("Zm9v\r\nYmFy", &out) && out == "foobar");  // line breaks skipped
        CHECK(!Base64Decode("Zm9v!", &out));
        CHECK(!Base64Decode("Zm9vY", &out));       // one dangling character
        CHECK(!Base64Decode("Zg==Zg==", &out));    // data after padding
        std::string all;
        for (int i = 0; i < 256; ++i) all.push_back(static_cast<char>(i));
        for (size_t n = 0; n <= all.size(); n += 37) {
            const std::string part = all.substr(0, n);
            CHECK(Base64Decode(Base64Encode(part), &out) && out == part);
        }
        CHECK(Base64Decode(kMp4_1080, &out) && Base64Encode(out) == kMp4_1080);
        CHECK(Base64Decode(kGif_360, &out) && Base64Encode(out) == kGif_360);
    }

    // PMFF: width at 24, height at 28; nothing else changes.
    {
        std::string b;
        CHECK(Base64Decode(kMp4_1080, &b));
        CHECK(b.size() == 46);
        CHECK(RenderFormatTag(b) == "PMFF");
        CHECK(I32(b, 24) == 1920 && I32(b, 28) == 1080);
        const std::string before = b;
        CHECK(PatchRenderFormatSize(b, 1280, 720) == RenderSizePatch::Patched);
        CHECK(I32(b, 24) == 1280 && I32(b, 28) == 720);
        CHECK(b.size() == before.size());
        CHECK(b.substr(0, 24) == before.substr(0, 24));
        CHECK(b.substr(32) == before.substr(32));  // fps, aspect, quality, options kept
        // Already there: untouched.
        const std::string patched = b;
        CHECK(PatchRenderFormatSize(b, 1280, 720) == RenderSizePatch::AlreadyThere);
        CHECK(b == patched);
        CHECK(PatchRenderFormatSize(b, 1279, 719) == RenderSizePatch::AlreadyThere);  // odd -> even
        // Odd -> even.
        CHECK(PatchRenderFormatSize(b, 1081, 1921) == RenderSizePatch::Patched);
        CHECK(I32(b, 24) == 1082 && I32(b, 28) == 1922);
        // Back through base64 as REAPER would get it.
        std::string again;
        CHECK(Base64Decode(Base64Encode(b), &again) && again == b);
    }

    // GIF: width at 4, height at 8.
    {
        std::string b;
        CHECK(Base64Decode(kGif_360, &b));
        CHECK(RenderFormatTag(b) == " FIG");
        CHECK(I32(b, 4) == 640 && I32(b, 8) == 360);
        const std::string before = b;
        CHECK(PatchRenderFormatSize(b, 1920, 1080) == RenderSizePatch::Patched);
        CHECK(I32(b, 4) == 1920 && I32(b, 8) == 1080);
        CHECK(b.substr(0, 4) == before.substr(0, 4));
        CHECK(b.substr(12) == before.substr(12));
        CHECK(PatchRenderFormatSize(b, 1920, 1080) == RenderSizePatch::AlreadyThere);
        CHECK(PatchRenderFormatSize(b, 333, 201) == RenderSizePatch::Patched);
        CHECK(I32(b, 4) == 334 && I32(b, 8) == 202);
    }

    // Refused: audio formats, unknown tags, short blobs, bad sizes -- blob untouched.
    {
        std::string b;
        CHECK(Base64Decode(kWav, &b));
        CHECK(RenderFormatTag(b) == "evaw");
        std::string before = b;
        CHECK(PatchRenderFormatSize(b, 1280, 720) == RenderSizePatch::Unknown);
        CHECK(b == before);

        std::string mp3 = "l3pm" + std::string(40, '\0');
        before = mp3;
        CHECK(PatchRenderFormatSize(mp3, 1280, 720) == RenderSizePatch::Unknown);
        CHECK(mp3 == before);

        std::string mp4;
        CHECK(Base64Decode(kMp4_1080, &mp4));
        std::string short_mp4 = mp4.substr(0, 43);  // fixed header incomplete
        before = short_mp4;
        CHECK(PatchRenderFormatSize(short_mp4, 1280, 720) == RenderSizePatch::Unknown);
        CHECK(short_mp4 == before);
        std::string short_mp4b = mp4.substr(0, 30);
        CHECK(PatchRenderFormatSize(short_mp4b, 1280, 720) == RenderSizePatch::Unknown);

        std::string gif;
        CHECK(Base64Decode(kGif_360, &gif));
        std::string short_gif = gif.substr(0, 17);
        before = short_gif;
        CHECK(PatchRenderFormatSize(short_gif, 1280, 720) == RenderSizePatch::Unknown);
        CHECK(short_gif == before);

        std::string tiny = "PM";
        CHECK(PatchRenderFormatSize(tiny, 1280, 720) == RenderSizePatch::Unknown);
        CHECK(RenderFormatTag(tiny).empty());
        std::string empty;
        CHECK(PatchRenderFormatSize(empty, 1280, 720) == RenderSizePatch::Unknown);

        before = mp4;
        CHECK(PatchRenderFormatSize(mp4, 0, 720) == RenderSizePatch::Unknown);
        CHECK(PatchRenderFormatSize(mp4, 1280, -2) == RenderSizePatch::Unknown);
        CHECK(mp4 == before);

        std::string odd_tag = std::string("\x01\x02" "ab", 4);
        CHECK(RenderFormatTag(odd_tag) == "??ab");
    }

    // The plan for both keys (RENDER_FORMAT, RENDER_FORMAT2).
    {
        std::string mp4_720;
        CHECK(Base64Decode(kMp4_1080, &mp4_720));
        CHECK(PatchRenderFormatSize(mp4_720, 1280, 720) == RenderSizePatch::Patched);
        const std::string mp4_720_b64 = Base64Encode(mp4_720);

        // Primary WAV + secondary MP4: the secondary only, no notice.
        RenderSizePlan p = PlanRenderFormatSize(kWav, kMp4_1080, 1280, 720);
        CHECK(p.known);
        CHECK(!p.write[0] && p.write[1]);
        CHECK(p.text[1] == mp4_720_b64);
        CHECK(p.w == 1280 && p.h == 720);

        // Primary MP4, no secondary: the primary only.
        p = PlanRenderFormatSize(kMp4_1080, "", 1280, 720);
        CHECK(p.known && p.write[0] && !p.write[1] && p.text[0] == mp4_720_b64);

        // Both already at size: nothing to write, no notice.
        p = PlanRenderFormatSize(mp4_720_b64, mp4_720_b64, 1280, 720);
        CHECK(p.known && !p.write[0] && !p.write[1]);
        p = PlanRenderFormatSize(mp4_720_b64, "", 1279, 719);  // odd -> even, same size
        CHECK(p.known && !p.write[0] && p.w == 1280 && p.h == 720);

        // Both unknown / empty: the notice, nothing written.
        p = PlanRenderFormatSize(kWav, "", 1280, 720);
        CHECK(!p.known && !p.write[0] && !p.write[1]);
        CHECK(p.tags.find("evaw") != std::string::npos);
        p = PlanRenderFormatSize("", "", 1280, 720);
        CHECK(!p.known && !p.write[0] && !p.write[1]);
        p = PlanRenderFormatSize(kWav, kWav, 1280, 720);
        CHECK(!p.known && !p.write[0] && !p.write[1]);

        // A non-base64 key is skipped without blocking the other.
        p = PlanRenderFormatSize("not*base64!", kMp4_1080, 1280, 720);
        CHECK(p.known && !p.write[0] && p.write[1] && p.text[1] == mp4_720_b64);
        CHECK(p.tags.find("not base64") != std::string::npos);
        p = PlanRenderFormatSize(kGif_360, "%%%", 1920, 1080);
        CHECK(p.known && p.write[0] && !p.write[1]);
    }

    // Even rounding.
    CHECK(EvenRenderSize(720) == 720);
    CHECK(EvenRenderSize(721) == 722);
    CHECK(EvenRenderSize(1) == 2);

    if (g_fails == 0) std::printf("render_format_size: all passed\n");
    return g_fails == 0 ? 0 : 1;
}
