// SPDX-License-Identifier: MIT
//
// The picture size inside REAPER's render format (spec 11-fb-13). Pure, header-only, no REAPER.
//
// REAPER keeps the render settings of a project as a binary "sink configuration", read and
// written base64-encoded through GetSetProjectInfo_String(proj, "RENDER_FORMAT" / "RENDER_FORMAT2").
// Its first 4 bytes are the format's tag. Only two layouts are known here and patched:
//   - "PMFF", FFmpeg-family video (AVI, MPEG-1/2, QT/MOV/MP4, MKV, FLV, WebM): int32 LE
//     width at offset 24, height at 28 (then fps, aspect flag, MJPEG quality, options).
//   - " FIG", GIF: int32 LE width at offset 4, height at 8 (then max fps, flags).
// Any other tag (WAV, MP3, FLAC...), or a blob too short for its layout, is left alone.

#pragma once

#include <climits>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

namespace rav {

// Standard base64 (RFC 4648 alphabet, '=' padding). Whitespace is skipped; padding is
// optional. Returns false on any other character or a truncated last group.
inline bool Base64Decode(const std::string& in, std::string* out)
{
    auto value = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    std::string bytes;
    bytes.reserve(in.size() * 3 / 4);
    std::uint32_t acc = 0;
    int bits = 0;
    int count = 0;  // significant characters in the current group
    bool padded = false;
    for (char c : in) {
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') continue;
        if (c == '=') {
            padded = true;
            continue;
        }
        if (padded) return false;  // data after padding
        const int v = value(c);
        if (v < 0) return false;
        acc = (acc << 6) | static_cast<std::uint32_t>(v);
        bits += 6;
        count = (count + 1) % 4;
        if (bits >= 8) {
            bits -= 8;
            bytes.push_back(static_cast<char>((acc >> bits) & 0xFFu));
        }
    }
    if (count == 1) return false;  // one character cannot hold a byte
    *out = std::move(bytes);
    return true;
}

inline std::string Base64Encode(const std::string& in)
{
    static const char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((in.size() + 2) / 3 * 4);
    std::size_t i = 0;
    for (; i + 2 < in.size(); i += 3) {
        const std::uint32_t n = (static_cast<std::uint8_t>(in[i]) << 16) |
                                (static_cast<std::uint8_t>(in[i + 1]) << 8) | static_cast<std::uint8_t>(in[i + 2]);
        out.push_back(kAlphabet[(n >> 18) & 63]);
        out.push_back(kAlphabet[(n >> 12) & 63]);
        out.push_back(kAlphabet[(n >> 6) & 63]);
        out.push_back(kAlphabet[n & 63]);
    }
    const std::size_t rest = in.size() - i;
    if (rest == 1) {
        const std::uint32_t n = static_cast<std::uint8_t>(in[i]) << 16;
        out.push_back(kAlphabet[(n >> 18) & 63]);
        out.push_back(kAlphabet[(n >> 12) & 63]);
        out += "==";
    } else if (rest == 2) {
        const std::uint32_t n = (static_cast<std::uint8_t>(in[i]) << 16) | (static_cast<std::uint8_t>(in[i + 1]) << 8);
        out.push_back(kAlphabet[(n >> 18) & 63]);
        out.push_back(kAlphabet[(n >> 12) & 63]);
        out.push_back(kAlphabet[(n >> 6) & 63]);
        out.push_back('=');
    }
    return out;
}

// The format's 4-byte tag, printable for a log ("PMFF", " FIG", "evaw"...); non-printable
// bytes show as '?'. Empty when the blob is shorter than 4 bytes.
inline std::string RenderFormatTag(const std::string& bytes)
{
    if (bytes.size() < 4) return std::string();
    std::string tag = bytes.substr(0, 4);
    for (char& c : tag) {
        if (c < 0x20 || c > 0x7E) c = '?';
    }
    return tag;
}

enum class RenderSizePatch { Patched, AlreadyThere, Unknown };

namespace render_format_detail {

// The fixed header of each known layout: a blob shorter than this is not trusted.
constexpr std::size_t kFfmpegMinSize = 44;  // tag..MJPEG quality; the option strings follow
constexpr std::size_t kGifMinSize = 18;     // tag, width, height, max fps, two flag bytes

inline std::int32_t ReadI32(const std::string& b, std::size_t at)
{
    const std::uint32_t v = static_cast<std::uint32_t>(static_cast<std::uint8_t>(b[at])) |
                            (static_cast<std::uint32_t>(static_cast<std::uint8_t>(b[at + 1])) << 8) |
                            (static_cast<std::uint32_t>(static_cast<std::uint8_t>(b[at + 2])) << 16) |
                            (static_cast<std::uint32_t>(static_cast<std::uint8_t>(b[at + 3])) << 24);
    return static_cast<std::int32_t>(v);
}

inline void WriteI32(std::string& b, std::size_t at, std::int32_t value)
{
    const std::uint32_t v = static_cast<std::uint32_t>(value);
    for (int i = 0; i < 4; ++i) b[at + static_cast<std::size_t>(i)] = static_cast<char>((v >> (8 * i)) & 0xFFu);
}

}  // namespace render_format_detail

// An odd size goes up to the next even one, as REAPER's encoders need (INT_MAX: down).
inline int EvenRenderSize(int v)
{
    if (v % 2 == 0) return v;
    return v < INT_MAX ? v + 1 : v - 1;
}

// Writes w x h (each rounded up to even) into a decoded render format blob of a known
// layout. Unknown: the tag is not a known video format, the blob is too short for its
// layout, or w / h is not positive -- the blob is untouched. AlreadyThere: it already
// holds that size, untouched too.
inline RenderSizePatch PatchRenderFormatSize(std::string& bytes, int w, int h)
{
    namespace d = render_format_detail;
    if (w <= 0 || h <= 0 || bytes.size() < 4) return RenderSizePatch::Unknown;
    std::size_t at_w = 0;
    if (bytes.compare(0, 4, "PMFF") == 0) {
        if (bytes.size() < d::kFfmpegMinSize) return RenderSizePatch::Unknown;
        at_w = 24;
    } else if (bytes.compare(0, 4, " FIG") == 0) {
        if (bytes.size() < d::kGifMinSize) return RenderSizePatch::Unknown;
        at_w = 4;
    } else {
        return RenderSizePatch::Unknown;
    }
    const std::int32_t ew = EvenRenderSize(w);
    const std::int32_t eh = EvenRenderSize(h);
    if (d::ReadI32(bytes, at_w) == ew && d::ReadI32(bytes, at_w + 4) == eh) return RenderSizePatch::AlreadyThere;
    d::WriteI32(bytes, at_w, ew);
    d::WriteI32(bytes, at_w + 4, eh);
    return RenderSizePatch::Patched;
}

// What to do with the project's two render formats (RENDER_FORMAT, RENDER_FORMAT2) for an
// Output size of w x h. Each input is the raw base64 string REAPER returned ("" = none).
struct RenderSizePlan {
    bool        write[2] = {false, false};  // this key gets text[i]
    std::string text[2];                    // the patched base64 to write
    bool        known = false;  // at least one key is a known video format (else: the notice)
    int         w = 0;          // the size written (even)
    int         h = 0;
    std::string tags;           // the refused keys and their tags, for the log
};

inline RenderSizePlan PlanRenderFormatSize(const std::string& format, const std::string& format2, int w, int h)
{
    static const char* const kKeys[2] = {"RENDER_FORMAT", "RENDER_FORMAT2"};
    const std::string* in[2] = {&format, &format2};
    RenderSizePlan plan;
    plan.w = w > 0 ? EvenRenderSize(w) : 0;
    plan.h = h > 0 ? EvenRenderSize(h) : 0;
    for (int i = 0; i < 2; ++i) {
        if (in[i]->empty()) continue;  // no (secondary) format
        std::string bytes;
        if (!Base64Decode(*in[i], &bytes)) {
            if (!plan.tags.empty()) plan.tags += ", ";
            plan.tags += std::string(kKeys[i]) + " not base64";
            continue;
        }
        const RenderSizePatch r = PatchRenderFormatSize(bytes, w, h);
        if (r == RenderSizePatch::Unknown) {
            if (!plan.tags.empty()) plan.tags += ", ";
            plan.tags += std::string(kKeys[i]) + " \"" + RenderFormatTag(bytes) + "\"";
            continue;
        }
        plan.known = true;
        if (r == RenderSizePatch::Patched) {
            plan.write[i] = true;
            plan.text[i] = Base64Encode(bytes);
        }
    }
    return plan;
}

}  // namespace rav
