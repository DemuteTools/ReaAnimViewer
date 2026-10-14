// SPDX-License-Identifier: MIT
//
// Lot 13-60 -- a Shortcuts row linked to a REAPER action the user picks (any action of REAPER's Main
// section: a native one by its number, "990", or a named one by its string id, "_WOL_ADJENVTRHMOUSE",
// "_SWS_ABOUT", "_RS<hash>" for a script): RAV runs the row on that action's gestures, read live from
// reaper-kb.ini (reaper_view_keys.h KbGesturesOf). The pure text rules: how a typed id is written,
// compared and stored, and the action list's search. Header only (no REAPER, no Windows): host-tested
// by tests/action_link_test.cpp.

#pragma once

#include <cctype>
#include <cstring>
#include <string>
#include <vector>

namespace rav {

// The id as reaper-kb.ini writes it: a native action as its decimal number ("990", leading zeros and
// blanks dropped), a named one with its leading underscore ("_WOL_ADJENVTRHMOUSE": one typed without
// it, as ReverseNamedCommandLookup gives it, gets it back). Quotes and blanks around it go. "" when the
// text cannot be an id (empty, "0" -- REAPER's "no action" --, a blank inside, too long).
inline std::string NormalizeCommandId(const std::string& text)
{
    size_t a = 0, b = text.size();
    auto trim = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '"' || c == '\''; };
    while (a < b && trim(text[a])) ++a;
    while (b > a && trim(text[b - 1])) --b;
    if (a == b || b - a > 200) return std::string();
    const std::string s = text.substr(a, b - a);
    bool digits = true;
    for (char c : s) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u <= 32 || u == 127) return std::string();  // a blank or a control character inside
        if (c < '0' || c > '9') digits = false;
    }
    if (digits) {
        size_t z = 0;
        while (z + 1 < s.size() && s[z] == '0') ++z;
        const std::string n = s.substr(z);
        if (n == "0" || n.size() > 9) return std::string();
        return n;
    }
    if (s[0] == '_') return s.size() > 1 ? s : std::string();
    return "_" + s;
}

// Whether a typed text reads as an id rather than words of an action's name: digits only, or one word
// that starts with "_" or holds one ("WOL_ADJENVTRHMOUSE").
inline bool LooksLikeCommandId(const std::string& text)
{
    const std::string n = NormalizeCommandId(text);
    if (n.empty()) return false;
    if (n[0] != '_') return true;  // a number
    // "_" + the text: a typed word with no underscore of its own ("zoom") is a search, not an id.
    size_t a = 0;
    while (a < text.size() && (text[a] == ' ' || text[a] == '\t' || text[a] == '"' || text[a] == '\'')) ++a;
    return (a < text.size() && text[a] == '_') || n.find('_', 1) != std::string::npos;
}

// Two ids name the same action (both normalized; a named id's case does not matter, as REAPER's).
inline bool SameCommandId(const std::string& a, const std::string& b)
{
    const std::string x = NormalizeCommandId(a), y = NormalizeCommandId(b);
    if (x.empty() || x.size() != y.size()) return false;
    for (size_t i = 0; i < x.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(x[i])) != std::tolower(static_cast<unsigned char>(y[i]))) return false;
    return true;
}

// The action list's search: every word of `query` (blank-separated, any case) is in the action's name
// or its id. An empty query matches everything.
inline bool ActionSearchMatch(const std::string& name, const std::string& id, const std::string& query)
{
    auto lower = [](std::string s) {
        for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    };
    const std::string n = lower(name), i = lower(id), q = lower(query);
    size_t p = 0;
    while (p < q.size()) {
        while (p < q.size() && (q[p] == ' ' || q[p] == '\t')) ++p;
        size_t e = p;
        while (e < q.size() && q[e] != ' ' && q[e] != '\t') ++e;
        if (e > p) {
            const std::string w = q.substr(p, e - p);
            if (n.find(w) == std::string::npos && i.find(w) == std::string::npos) return false;
        }
        p = e;
    }
    return true;
}

// ExtState: a row linked to an action is stored as "link:" + its id (the rows' other values never
// start so). False when `text` is not one (then *id is untouched).
inline std::string EncodeActionLink(const std::string& id)
{
    return "link:" + NormalizeCommandId(id);
}

inline bool DecodeActionLink(const char* text, std::string* id)
{
    if (!text || std::strncmp(text, "link:", 5) != 0) return false;
    const std::string n = NormalizeCommandId(text + 5);
    if (n.empty()) return false;
    if (id) *id = n;
    return true;
}

}  // namespace rav
