// SPDX-License-Identifier: MIT
//
// See library_tree_nav.h.

#include "library_tree_nav.h"

#include <algorithm>

#include "sound_record.h"

namespace rav {
namespace {

bool IsContainer(const SoundNode& n) { return n.type != SoundNodeType::Sound; }

bool StorableUid(const std::string& uid)
{
    if (uid.empty()) return false;
    for (const char c : uid)
        if (c == ',' || static_cast<unsigned char>(c) < 0x20) return false;
    return true;
}

// A Switch's child on `value` ("" = none), as sound_switch.cpp's CaseChild reads it.
const SoundNode* CaseChildOf(const SoundLibrary& lib, const SoundNode& sw, const std::string& value)
{
    if (value.empty()) return nullptr;
    for (const SwitchCase& c : sw.cases) {
        if (c.value_uid != value) continue;
        if (c.child_uid.empty() || std::find(sw.unassigned.begin(), sw.unassigned.end(), c.child_uid) != sw.unassigned.end())
            return nullptr;
        const SoundNode* n = FindNode(lib, c.child_uid);
        if (!n || n->parent != sw.uid || n->uid == sw.uid) return nullptr;
        return n;
    }
    return nullptr;
}

}  // namespace

std::string EncodeTreeFolds(const SoundLibrary& lib, const std::set<std::string>& closed)
{
    std::set<std::string> mine;  // sorted
    for (const SoundNode& n : lib.nodes)
        if (IsContainer(n) && closed.count(n.uid) && StorableUid(n.uid)) mine.insert(n.uid);
    std::string out;
    for (const std::string& uid : mine) {
        if (!out.empty()) out += ',';
        out += uid;
    }
    return out;
}

std::set<std::string> DecodeTreeFolds(const std::string& text)
{
    std::set<std::string> out;
    size_t                at = 0;
    while (at <= text.size()) {
        size_t end = text.find(',', at);
        if (end == std::string::npos) end = text.size();
        std::string piece = text.substr(at, end - at);
        while (!piece.empty() && (piece.back() == ' ' || piece.back() == '\r' || piece.back() == '\n')) piece.pop_back();
        while (!piece.empty() && piece.front() == ' ') piece.erase(piece.begin());
        if (!piece.empty()) out.insert(piece);
        at = end + 1;
    }
    return out;
}

std::set<std::string> CollapsedTreeFolds(const SoundLibrary& lib)
{
    std::set<std::string> out;
    for (const SoundNode& n : lib.nodes)
        if (IsContainer(n) && n.uid != lib.root) out.insert(n.uid);
    return out;
}

bool TreeCanExpandAll(const SoundLibrary& lib, const std::set<std::string>& closed)
{
    for (const SoundNode& n : lib.nodes)
        if (IsContainer(n) && closed.count(n.uid)) return true;
    return false;
}

bool TreeCanCollapseAll(const SoundLibrary& lib, const std::set<std::string>& closed)
{
    for (const SoundNode& n : lib.nodes)
        if (IsContainer(n) && (n.uid == lib.root ? closed.count(n.uid) != 0 : !closed.count(n.uid))) return true;
    return false;
}

void SetTreeFoldsAll(const SoundLibrary& lib, bool collapse, std::set<std::string>* closed)
{
    if (!closed) return;
    for (const SoundNode& n : lib.nodes)
        if (IsContainer(n)) closed->erase(n.uid);
    if (collapse)
        for (const std::string& uid : CollapsedTreeFolds(lib)) closed->insert(uid);
}

FoldScopeButtons FoldScopeButtonsFor(int foldable, int open)
{
    FoldScopeButtons b;
    b.shown = foldable >= 2;
    if (!b.shown) return b;
    b.can_expand = open < foldable;
    b.can_collapse = open > 0;
    return b;
}

LibraryRow RevealRowFor(const SoundStore& store, const SoundLibrary& lib, const ValueLayers& layers)
{
    const SoundNode* n = FindNode(lib, lib.root);
    if (!n) return {"", lib.root};
    for (size_t guard = 0; guard <= lib.nodes.size(); ++guard) {
        if (n->type != SoundNodeType::Switch) return RowOfNode(lib, n->uid);
        const SwitchGroup* g = FindGroup(store, n->group_uid);
        if (!g) return RowOfNode(lib, n->uid);  // no value rows to show: stop on the Switch
        ValueFrom         from = ValueFrom::Default;
        const std::string v = EffectiveValue(*n, layers, &from);
        const std::string def = DefaultValue(store, *n);
        const std::string picked = from == ValueFrom::Default ? def : v;
        const SoundNode*  c = CaseChildOf(lib, *n, picked);
        if (!c && from != ValueFrom::Default) c = CaseChildOf(lib, *n, def);  // ResolveEvent's fallback
        if (!c) {  // a value no longer in the group has no row: stop on the Switch
            if (picked.empty() || !GroupHasValue(*g, picked)) return RowOfNode(lib, n->uid);
            return LibraryRow{n->uid, picked};
        }
        n = c;
    }
    return RowOfNode(lib, n->uid);  // a cycle: stop where the walk is
}

}  // namespace rav
