// SPDX-License-Identifier: MIT
//
// See ravlib.h. Spec 13-11 (decisions 5, 6, 7, 10, 11, 12).

#include "ravlib.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>
#include <set>

#include "library_tree_edit.h"
#include "media_db.h"
#include "rule_record.h"
#include "sound_record.h"
#include "sound_switch.h"

namespace rav {

namespace {

std::string LowerAscii(std::string s)
{
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

const std::string& Mapped(const std::map<std::string, std::string>& m, const std::string& k)
{
    const auto it = m.find(k);
    return it == m.end() ? k : it->second;
}

}  // namespace

bool ParseRavlib(const std::string& text, SoundStore* out)
{
    if (!out) return false;
    size_t pos = 0;
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF && static_cast<unsigned char>(text[1]) == 0xBB &&
        static_cast<unsigned char>(text[2]) == 0xBF)
        pos = 3;
    // The first non-blank line must be "RAVLIB <n>".
    while (pos < text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos) end = text.size();
        const std::string line = text.substr(pos, end - pos);
        if (line.find_first_not_of(" \t\r") != std::string::npos) break;
        pos = end + 1;
    }
    if (pos >= text.size()) return false;
    const size_t lead = text.find_first_not_of(" \t", pos);
    const size_t mlen = sizeof(kRavlibMagic) - 1;
    if (lead == std::string::npos || text.compare(lead, mlen, kRavlibMagic) != 0) return false;
    const size_t after = lead + mlen;
    if (after >= text.size() || (text[after] != ' ' && text[after] != '\t')) return false;
    const std::string as_store = std::string(kSoundStoreMagic) + " " + text.substr(after + 1);  // a tab too
    SoundStore s;
    if (!ParseSoundStore(as_store, &s)) return false;  // also checks the version (>= 1)
    if (s.libraries.size() != 1) return false;
    *out = s;
    return true;
}

std::string SerializeRavlib(const SoundStore& one)
{
    const std::string body = SerializeSoundStore(one);
    const size_t      nl = body.find('\n');
    std::string       out = std::string(kRavlibMagic) + " 1\n";
    if (nl != std::string::npos) out += body.substr(nl + 1);
    return out;
}

SoundStore RavlibOf(const SoundStore& store, const std::string& lib_uid, int* item_sounds)
{
    SoundStore out;
    if (item_sounds) *item_sounds = 0;
    const SoundLibrary* lib = FindLibrary(store, lib_uid);
    if (!lib) return out;
    SoundLibrary copy = *lib;
    copy.ravlib_path.clear();
    copy.kit_offered = false;  // 13-15a: "offered already" is this project's, not the library's
    std::set<std::string> used_groups;
    for (SoundNode& n : copy.nodes) {
        if (n.type == SoundNodeType::Switch && !n.group_uid.empty()) used_groups.insert(n.group_uid);
        if (n.type != SoundNodeType::Sound) continue;
        if (n.sound.source.kind == SoundSourceKind::File) n.sound.source.item_guid.clear();
        else if (item_sounds) ++*item_sounds;
    }
    for (const SwitchGroup& g : store.groups)
        if (used_groups.count(g.uid)) out.groups.push_back(g);
    out.libraries.push_back(copy);
    return out;
}

bool AddRavlibLibrary(SoundStore& store, const SoundStore& file, const std::string& file_name, std::string* lib_uid,
                      std::string* why)
{
    if (file.libraries.size() != 1) {
        if (why) *why = "Not a User Library file";
        return false;
    }
    const SoundLibrary& src = file.libraries.front();
    if (FindLibrary(store, src.uid)) {
        if (why) *why = "Already in this project";
        if (lib_uid) *lib_uid = src.uid;
        return false;
    }
    SoundStore s = store;
    std::set<std::string> used;
    for (const SwitchGroup& g : s.groups) {
        used.insert(g.uid);
        for (const SwitchValue& v : g.values) used.insert(v.uid);
    }
    for (const SoundLibrary& l : s.libraries) {
        used.insert(l.uid);
        for (const SoundNode& n : l.nodes) used.insert(n.uid);
    }
    auto fresh = [&](const std::string& uid) {
        std::string u = uid;
        while (u.empty() || used.count(u)) u = NewSoundUid();
        used.insert(u);
        return u;
    };

    std::map<std::string, std::string> gmap, vmap, nmap;
    // 13-18: a merged group whose project default differs from the file's: the file's (mapped), so a
    // Switch that followed it in the file keeps playing it.
    std::map<std::string, std::string> merged_def;
    for (const SwitchGroup& fg : file.groups) {
        SwitchGroup* target = nullptr;
        for (SwitchGroup& pg : s.groups)
            if (SwitchNameKey(pg.name) == SwitchNameKey(fg.name)) {
                target = &pg;
                break;
            }
        if (target) {
            gmap[fg.uid] = target->uid;
            for (const SwitchValue& fv : fg.values) {
                const SwitchValue* match = nullptr;
                for (const SwitchValue& pv : target->values)
                    if (SwitchNameKey(pv.name) == SwitchNameKey(fv.name)) {
                        match = &pv;
                        break;
                    }
                if (match) {
                    vmap[fv.uid] = match->uid;
                } else {
                    SwitchValue nv = fv;
                    nv.uid = fresh(fv.uid);
                    vmap[fv.uid] = nv.uid;
                    target->values.push_back(nv);
                }
            }
            const std::string fdef = GroupDefaultValue(fg);
            const std::string mdef = fdef.empty() ? std::string() : Mapped(vmap, fdef);
            if (!mdef.empty() && mdef != GroupDefaultValue(*target)) merged_def[target->uid] = mdef;
        } else {
            SwitchGroup ng = fg;
            ng.uid = fresh(fg.uid);
            gmap[fg.uid] = ng.uid;
            for (SwitchValue& v : ng.values) {
                const std::string old = v.uid;
                v.uid = fresh(old);
                vmap[old] = v.uid;
            }
            // 13-18 D6: a new group's default follows its values (a merged group keeps the project's).
            if (!ng.default_value.empty()) ng.default_value = vmap.count(ng.default_value) ? vmap[ng.default_value] : std::string();
            std::vector<std::string> names;
            for (const SwitchGroup& g : s.groups) names.push_back(g.name);
            ng.name = UniqueName(ng.name, names);
            s.groups.push_back(ng);
        }
    }

    SoundLibrary lib = src;
    lib.kit_offered = false;  // 13-15a: a new project offers the kit once
    lib.uid = fresh(src.uid);
    for (const SoundNode& n : src.nodes) nmap[n.uid] = fresh(n.uid);
    lib.root = Mapped(nmap, src.root);
    for (SoundNode& n : lib.nodes) {
        n.uid = Mapped(nmap, n.uid);
        if (!n.parent.empty()) n.parent = Mapped(nmap, n.parent);
        if (!n.group_uid.empty()) n.group_uid = Mapped(gmap, n.group_uid);
        for (SwitchCase& c : n.cases) {
            c.value_uid = Mapped(vmap, c.value_uid);
            c.child_uid = Mapped(nmap, c.child_uid);
        }
        const bool followed = n.type == SoundNodeType::Switch && (n.default_value.empty() || !vmap.count(n.default_value));
        if (!n.default_value.empty()) n.default_value = Mapped(vmap, n.default_value);
        if (followed) {  // 13-18: on a merged group, what it played in the file keeps playing
            const auto md = merged_def.find(n.group_uid);
            if (md != merged_def.end()) n.default_value = md->second;
        }
        if (!n.preview_value.empty()) n.preview_value = Mapped(vmap, n.preview_value);
        for (std::string& u : n.unassigned) u = Mapped(nmap, u);
        if (n.type == SoundNodeType::Sound && n.sound.source.kind == SoundSourceKind::File) n.sound.source.item_guid.clear();
    }
    std::vector<std::string> names;
    for (const SoundLibrary& l : s.libraries) names.push_back(l.name);
    lib.name = UniqueName(lib.name.empty() ? std::string("User Library") : lib.name, names);
    lib.user = true;
    lib.ravlib_path = file_name;
    s.libraries.push_back(lib);
    store = s;
    if (lib_uid) *lib_uid = lib.uid;
    return true;
}

std::string EffectiveLandsIn(const SoundLibrary& lib, const std::string& node_uid)
{
    if (!lib.user) return kDefaultLandsIn;
    const SoundNode* n = FindNode(lib, node_uid);
    for (size_t guard = 0; n && guard <= lib.nodes.size(); ++guard) {
        if (!n->lands_in.empty()) return n->lands_in;
        if (n->parent.empty()) break;
        n = FindNode(lib, n->parent);
    }
    return kDefaultLandsIn;
}

bool BlendLayerPath(const SoundLibrary& lib, const std::string& node_uid, std::string* blend_uid,
                    std::string* child_uid)
{
    if (blend_uid) blend_uid->clear();
    if (child_uid) child_uid->clear();
    const SoundNode* n = FindNode(lib, node_uid);
    for (size_t guard = 0; n && !n->parent.empty() && guard <= lib.nodes.size(); ++guard) {
        const SoundNode* p = FindNode(lib, n->parent);
        if (!p) return false;
        if (p->type == SoundNodeType::Blend) {
            if (blend_uid) *blend_uid = p->uid;
            if (child_uid) *child_uid = n->uid;
            return true;
        }
        n = p;
    }
    return false;
}

std::string ResolveAssetPath(const AssetIdentity& id, const std::function<int64_t(const std::string&)>& file_size,
                             const MediaDbIndex* idx)
{
    if (!id.last_path.empty() && file_size) {
        const int64_t sz = file_size(id.last_path);
        if (sz >= 0 && (id.size < 0 || sz == id.size)) return id.last_path;
    }
    if (idx) {
        const LookupResult r = idx->Find(id);
        if (r.status == LookupStatus::Found) return r.path;
    }
    return {};
}

bool SameAssetIdentity(const AssetIdentity& a, const AssetIdentity& b)
{
    if (a.name.empty() || LowerAscii(a.name) != LowerAscii(b.name)) return false;
    if (a.size >= 0 && b.size >= 0 && a.size != b.size) return false;
    if (a.length >= 0.0 && b.length >= 0.0 && std::fabs(a.length - b.length) > 0.0015) return false;
    return true;
}

int ApplyLocated(SoundStore& store, const std::vector<std::pair<AssetIdentity, std::string>>& found,
                 const AssetIdentity* picked)
{
    int changed = 0;
    for (SoundLibrary& lib : store.libraries)
        for (SoundNode& n : lib.nodes) {
            if (n.type != SoundNodeType::Sound || n.sound.source.kind != SoundSourceKind::File) continue;
            AssetIdentity& a = n.sound.source.asset;
            for (const auto& f : found) {
                if (f.second.empty() || !SameAssetIdentity(a, f.first)) continue;
                bool any = false;
                if (a.last_path != f.second) {
                    a.last_path = f.second;
                    any = true;
                }
                if (a.size < 0 && f.first.size >= 0) {
                    a.size = f.first.size;
                    any = true;
                }
                if (a.length < 0.0 && f.first.length >= 0.0) {
                    a.length = f.first.length;
                    any = true;
                }
                if (picked && !picked->last_path.empty() && f.second == picked->last_path) {
                    if (picked->size >= 0 && a.size != picked->size) {
                        a.size = picked->size;
                        any = true;
                    }
                    if (picked->length >= 0.0 && std::fabs(a.length - picked->length) > 1e-9) {
                        a.length = picked->length;
                        any = true;
                    }
                }
                if (any) ++changed;
                break;
            }
        }
    return changed;
}

int FolderChildren(const std::vector<int>& depths, int fi, std::vector<int>* kids, int* last)
{
    if (kids) kids->clear();
    if (last) *last = fi;
    const int n = static_cast<int>(depths.size());
    if (fi < 0 || fi >= n) return 0;
    int level = 1;
    for (int j = fi + 1; j < n && level > 0; ++j) {
        if (level == 1 && kids) kids->push_back(j);
        level += depths[static_cast<size_t>(j)];
        if (last) *last = j;
    }
    return level < 0 ? -level : 0;
}

std::string RavlibFileName(const std::string& lib_name, const std::vector<std::string>& taken)
{
    std::string base;
    for (char c : lib_name) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u < 32 || std::string("\\/:*?\"<>|").find(c) != std::string::npos) base += '_';
        else base += c;
    }
    const size_t b = base.find_first_not_of(" .");
    const size_t e = base.find_last_not_of(" .");
    base = b == std::string::npos ? std::string() : base.substr(b, e - b + 1);
    if (base.empty()) base = "Library";
    auto used = [&](const std::string& n) {
        for (const std::string& t : taken)
            if (LowerAscii(t) == LowerAscii(n)) return true;
        return false;
    };
    std::string name = base + kRavlibExt;
    for (int i = 2; used(name); ++i) name = base + " " + std::to_string(i) + kRavlibExt;
    return name;
}

}  // namespace rav
