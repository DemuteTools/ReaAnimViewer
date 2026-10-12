// SPDX-License-Identifier: MIT
//
// 13-7 -- every symbol a UI string can draw has a glyph (src/ui_glyph_ranges.h).
// Scans each string literal of src/ (raw UTF-8 and \x / \u escapes alike), decodes it,
// lists every code point above U+00FF with where it is, and fails on one outside the
// ranges the viewer merges into the ImGui font atlas: ImGui would draw it as "?".

#include "ui_glyph_ranges.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

#ifndef RAV_SRC_DIR
#error "RAV_SRC_DIR must name the src/ folder"
#endif

namespace {

int g_fail = 0;
void Check(bool ok, const std::string& what) {
    if (!ok) { std::printf("FAIL: %s\n", what.c_str()); ++g_fail; }
}

// Code points that are in strings but never drawn (a file's UTF-8 byte-order mark).
bool NotDrawn(unsigned long cp) { return cp == 0xFEFF; }

bool IsHex(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
int HexVal(char c) { return c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10; }

void AppendUtf8(std::string& out, unsigned long cp) {
    if (cp < 0x80) out += char(cp);
    else if (cp < 0x800) { out += char(0xC0 | (cp >> 6)); out += char(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) { out += char(0xE0 | (cp >> 12)); out += char(0x80 | ((cp >> 6) & 0x3F)); out += char(0x80 | (cp & 0x3F)); }
    else { out += char(0xF0 | (cp >> 18)); out += char(0x80 | ((cp >> 12) & 0x3F)); out += char(0x80 | ((cp >> 6) & 0x3F)); out += char(0x80 | (cp & 0x3F)); }
}

// One string literal's bytes, escapes resolved (body = between the quotes).
std::string Unescape(const std::string& body) {
    std::string out;
    for (size_t i = 0; i < body.size(); ++i) {
        if (body[i] != '\\' || i + 1 >= body.size()) { out += body[i]; continue; }
        const char e = body[++i];
        if (e == 'x') {
            unsigned long v = 0;
            while (i + 1 < body.size() && IsHex(body[i + 1])) v = v * 16 + HexVal(body[++i]);
            out += char(v & 0xFF);
        } else if (e == 'u' || e == 'U') {
            const int n = e == 'u' ? 4 : 8;
            unsigned long v = 0;
            for (int k = 0; k < n && i + 1 < body.size() && IsHex(body[i + 1]); ++k) v = v * 16 + HexVal(body[++i]);
            AppendUtf8(out, v);
        } else if (e >= '0' && e <= '7') {
            unsigned long v = e - '0';
            for (int k = 0; k < 2 && i + 1 < body.size() && body[i + 1] >= '0' && body[i + 1] <= '7'; ++k) v = v * 8 + (body[++i] - '0');
            out += char(v & 0xFF);
        } else {
            out += e;  // \n \t \" \\ ...: all ASCII, irrelevant here
        }
    }
    return out;
}

// Code points of a UTF-8 byte string (a malformed byte yields U+FFFD).
std::vector<unsigned long> Decode(const std::string& s) {
    std::vector<unsigned long> cps;
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = (unsigned char)s[i];
        const int n = c < 0x80 ? 0 : (c >> 5) == 6 ? 1 : (c >> 4) == 14 ? 2 : (c >> 3) == 30 ? 3 : -1;
        if (n < 0) { cps.push_back(0xFFFD); ++i; continue; }
        unsigned long cp = n == 0 ? c : (c & (0x3F >> n));
        bool ok = true;
        for (int k = 1; k <= n; ++k) {
            if (i + k >= s.size() || ((unsigned char)s[i + k] >> 6) != 2) { ok = false; break; }
            cp = (cp << 6) | ((unsigned char)s[i + k] & 0x3F);
        }
        if (!ok) { cps.push_back(0xFFFD); ++i; continue; }
        cps.push_back(cp);
        i += size_t(n) + 1;
    }
    return cps;
}

struct Literal { int line; std::string bytes; };

// The string literals of a C++ source (comments and character literals skipped,
// raw strings R"d(...)d" kept verbatim).
std::vector<Literal> Literals(const std::string& src) {
    std::vector<Literal> out;
    int line = 1;
    for (size_t i = 0; i < src.size(); ++i) {
        const char c = src[i];
        if (c == '\n') { ++line; continue; }
        if (c == '/' && i + 1 < src.size() && src[i + 1] == '/') {
            while (i < src.size() && src[i] != '\n') ++i;
            --i;
            continue;
        }
        if (c == '/' && i + 1 < src.size() && src[i + 1] == '*') {
            i += 2;
            while (i + 1 < src.size() && !(src[i] == '*' && src[i + 1] == '/')) { if (src[i] == '\n') ++line; ++i; }
            ++i;
            continue;
        }
        if (c == '\'') {
            // A digit separator (1'000) is not a character literal.
            if (i > 0 && IsHex(src[i - 1]) && i + 1 < src.size() && IsHex(src[i + 1])) continue;
            ++i;
            while (i < src.size() && src[i] != '\'' && src[i] != '\n') { if (src[i] == '\\') ++i; ++i; }
            continue;
        }
        if (c == '"') {
            const int start = line;
            const auto ident = [&](size_t k) { const char p = src[k]; return std::isalnum((unsigned char)p) || p == '_'; };
            const bool raw = i > 0 && src[i - 1] == 'R' &&
                             (i < 2 || !ident(i - 2) || src[i - 2] == 'L' || src[i - 2] == 'u' || src[i - 2] == 'U' || src[i - 2] == '8');
            if (raw) {
                const size_t paren = src.find('(', i);
                const std::string delim = src.substr(i + 1, paren - i - 1);
                const std::string close = ")" + delim + "\"";
                const size_t end = src.find(close, paren);
                const std::string body = src.substr(paren + 1, end - paren - 1);
                line += int(std::count(body.begin(), body.end(), '\n'));
                // R"GLSL(...)GLSL" is shader source handed to OpenGL, never drawn as text.
                if (delim != "GLSL") out.push_back({start, body});
                i = end + close.size() - 1;
                continue;
            }
            std::string body;
            ++i;
            while (i < src.size() && src[i] != '"' && src[i] != '\n') {
                if (src[i] == '\\' && i + 1 < src.size()) body += src[i++];
                body += src[i++];
            }
            out.push_back({start, Unescape(body)});
        }
    }
    return out;
}

void SelfTests() {
    Check(rav::kUiGlyphRanges[sizeof(rav::kUiGlyphRanges) / sizeof(rav::kUiGlyphRanges[0]) - 1] == 0, "ranges end with 0");
    for (int i = 0; rav::kUiGlyphRanges[i] != 0; i += 2) {
        Check(rav::kUiGlyphRanges[i + 1] != 0 && rav::kUiGlyphRanges[i] <= rav::kUiGlyphRanges[i + 1], "a range is [first, last]");
        if (i > 0) Check(rav::kUiGlyphRanges[i] > rav::kUiGlyphRanges[i - 1], "ranges sorted, no overlap");
    }
    const auto lits = Literals("// \"\xE2\x98\x85 in a comment\"\n"
                               "/* \"\xE2\x98\x86\" */ char q = '\"';\n"
                               "f(\"\\xE2\\x96\\xB6##play\", u8\"\\u25A0\", R\"x(\xE2\x80\xA6)x\", \"\xF0\x9F\x98\x80\");\n");
    std::vector<unsigned long> got;
    for (const Literal& l : lits)
        for (unsigned long cp : Decode(l.bytes))
            if (cp > 0xFF) got.push_back(cp);
    Check(got == std::vector<unsigned long>({0x25B6, 0x25A0, 0x2026, 0x1F600}), "scanner reads escapes, u8, raw strings; skips comments");
    Check(lits.size() == 4 && lits[0].line == 3, "literal lines");
    Check(rav::UiGlyphCovered(0x25B6) && rav::UiGlyphCovered(0x2026) && rav::UiGlyphCovered(0x2605), "play, ellipsis, star covered");
    Check(!rav::UiGlyphCovered(0x1F600) && !rav::UiGlyphCovered(0x0400), "emoji / Cyrillic not covered");
}

}  // namespace

int main() {
    SelfTests();

    std::map<unsigned long, std::vector<std::string>> found;  // code point -> file:line
    int files = 0;
    for (const auto& e : fs::recursive_directory_iterator(RAV_SRC_DIR)) {
        if (!e.is_regular_file()) continue;
        const std::string ext = e.path().extension().string();
        if (ext != ".cpp" && ext != ".h") continue;
        std::ifstream in(e.path(), std::ios::binary);
        std::stringstream ss;
        ss << in.rdbuf();
        ++files;
        const std::string name = fs::relative(e.path(), RAV_SRC_DIR).generic_string();
        for (const Literal& l : Literals(ss.str()))
            for (unsigned long cp : Decode(l.bytes))
                if (cp > 0xFF && !NotDrawn(cp)) {
                    auto& at = found[cp];
                    const std::string where = name + ":" + std::to_string(l.line);
                    if (at.empty() || at.back() != where) at.push_back(where);
                }
    }
    Check(files > 50, "src/ scanned (" + std::to_string(files) + " files)");

    std::printf("Symbols above U+00FF in src/ string literals (%d files):\n", files);
    for (const auto& [cp, at] : found) {
        std::string utf8;
        AppendUtf8(utf8, cp);
        const bool ok = rav::UiGlyphCovered(cp);
        std::printf("  U+%04lX %s  %s  %zu use(s):", cp, utf8.c_str(), ok ? "ok" : "NO GLYPH", at.size());
        for (const std::string& w : at) std::printf(" %s", w.c_str());
        std::printf("\n");
        Check(ok, "U+" + [&] { char b[16]; std::snprintf(b, sizeof b, "%04lX", cp); return std::string(b); }() +
                      " has no glyph in kUiGlyphRanges (add its block there, or draw it as a vector)");
    }

    if (g_fail) { std::printf("%d failure(s)\n", g_fail); return 1; }
    std::printf("ui_glyphs: all symbols covered\n");
    return 0;
}
