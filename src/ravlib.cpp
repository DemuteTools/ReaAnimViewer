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
#include "sound_zones.h"  // 13-62i: SanitizeZones (FreezeFrames)

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
    return SerializeRavlib(one, std::string(), 0);
}

namespace {
std::string PctEnc(const std::string& s)
{
    static const char kHex[] = "0123456789ABCDEF";
    std::string       out;
    for (char ch : s) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (c <= 0x20 || c == 0x7F || c == ',' || c == '=' || c == '%') {
            out += '%';
            out += kHex[c >> 4];
            out += kHex[c & 15];
        } else {
            out += ch;
        }
    }
    return out;
}
bool PctDec(const std::string& s, std::string* out)
{
    auto hex = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; };
    std::string r;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '%') {
            r += s[i];
            continue;
        }
        if (i + 2 >= s.size()) return false;
        const int h = hex(s[i + 1]), l = hex(s[i + 2]);
        if (h < 0 || l < 0) return false;
        r += static_cast<char>(h * 16 + l);
        i += 2;
    }
    *out = r;
    return true;
}
}  // namespace

std::string SerializeRavlib(const SoundStore& one, const std::string& by, int64_t at)
{
    const std::string body = SerializeSoundStore(one);
    const size_t      nl = body.find('\n');
    std::string       out = std::string(kRavlibMagic) + " " + std::to_string(kRavlibVersion);
    if (!by.empty()) out += " by=" + PctEnc(by);
    if (at != 0) out += " at=" + std::to_string(at);
    out += "\n";
    if (nl != std::string::npos) out += body.substr(nl + 1);
    return out;
}

void RavlibWriter(const SoundStore& parsed, std::string* by, int64_t* at)
{
    if (by) by->clear();
    if (at) *at = 0;
    for (const std::string& tok : parsed.kept) {
        if (tok.compare(0, 3, "by=") == 0 && by) {
            std::string v;
            if (PctDec(tok.substr(3), &v)) *by = v;
        } else if (tok.compare(0, 3, "at=") == 0 && at) {
            try {
                *at = std::stoll(tok.substr(3));
            } catch (...) {
            }
        }
    }
}

bool SameRavlibContent(const std::string& a, const std::string& b)
{
    SoundStore sa, sb;
    if (ParseRavlib(a, &sa) && ParseRavlib(b, &sb)) return SerializeRavlib(sa) == SerializeRavlib(sb);
    auto body = [](const std::string& t) {
        const size_t nl = t.find('\n');
        std::string  r = nl == std::string::npos ? std::string() : t.substr(nl + 1);
        r.erase(std::remove(r.begin(), r.end(), '\r'), r.end());
        return r;
    };
    return body(a) == body(b);
}

SoundStore RavlibOf(const SoundStore& store, const std::string& lib_uid, int* item_sounds)
{
    SoundStore out;
    if (item_sounds) *item_sounds = 0;
    const SoundLibrary* lib = FindLibrary(store, lib_uid);
    if (!lib) return out;
    SoundLibrary copy = *lib;
    copy.ravlib_path.clear();
    copy.user = false;  // 13-77a: legacy flag, never written
    copy.kit_offered = false;  // 13-15a: "offered already" is this project's, not the library's
    std::set<std::string> used_groups;
    for (SoundNode& n : copy.nodes) {
        if (n.type == SoundNodeType::Switch && !n.group_uid.empty()) used_groups.insert(n.group_uid);
        if (n.type != SoundNodeType::Sound) continue;
        if (!n.sound.divide_group.empty()) used_groups.insert(n.sound.divide_group);  // 13-23
        if (n.sound.source.kind == SoundSourceKind::File) n.sound.source.item_guid.clear();
        else if (item_sounds) ++*item_sounds;
    }
    for (const SwitchGroup& g : store.groups)
        if (used_groups.count(g.uid)) out.groups.push_back(g);
    out.libraries.push_back(copy);
    return out;
}

bool AddRavlibLibrary(SoundStore& store, const SoundStore& file, const std::string& file_name, std::string* lib_uid,
                      std::string* why, std::vector<std::pair<std::string, std::string>>* uid_map_out, LinkBase* base_out)
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
                    // 13-23: the project's map stands; only the keys no value of the group holds come along.
                    std::vector<std::string> roles;
                    for (const std::string& r : nv.roles) {
                        bool held = false;
                        for (const SwitchValue& pv : target->values)
                            held = held || std::find(pv.roles.begin(), pv.roles.end(), r) != pv.roles.end();
                        if (!held) roles.push_back(r);
                    }
                    nv.roles = roles;
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
        // 13-23: the parts follow the merged groups and values (13-31: layer roles are keys, unmapped).
        SoundData& d = n.sound;
        if (!d.divide_group.empty()) d.divide_group = Mapped(gmap, d.divide_group);
        for (std::string& v : d.divide_values) v = Mapped(vmap, v);
        for (RoleValue& r : d.role_map) r.value = Mapped(vmap, r.value);
        for (SoundZone& z : d.zones)
            if (!z.to.empty()) z.to = Mapped(vmap, z.to);
        for (SoundFrame& f : d.frames) {  // 13-28: the per-part gains follow their values
            std::map<std::string, double> pdb;
            for (const auto& kv : f.part_db) pdb[Mapped(vmap, kv.first)] = kv.second;
            f.part_db = pdb;
            for (std::string& o : f.owners) o = Mapped(vmap, o);  // 13-62: who owns each piece
            for (EditZone& z : f.zones)                            // 13-62h: each zone's part
                if (!z.part.empty() && z.part != kWholePart) z.part = Mapped(vmap, z.part);  // 13-62i: the implicit part stays
        }
        for (PartSettings& ps : d.parts) ps.value = Mapped(vmap, ps.value);  // 13-62: the parts' own settings
    }
    std::vector<std::string> names;
    for (const SoundLibrary& l : s.libraries) names.push_back(l.name);
    lib.name = UniqueName(lib.name.empty() ? std::string("User Library") : lib.name, names);
    lib.user = false;  // 13-77a: linked = ravlib_path
    lib.ravlib_path = file_name;
    s.libraries.push_back(lib);
    store = s;
    if (lib_uid) *lib_uid = lib.uid;
    if (uid_map_out || base_out) {
        std::vector<std::pair<std::string, std::string>> changed;
        auto note = [&](const std::string& from, const std::string& to) {
            if (from != to) changed.push_back({from, to});
        };
        note(src.uid, lib.uid);
        for (const auto& kv : nmap) note(kv.first, kv.second);
        for (const auto& kv : gmap) note(kv.first, kv.second);
        for (const auto& kv : vmap) note(kv.first, kv.second);
        if (uid_map_out) *uid_map_out = changed;
        if (base_out) {
            LinkBase b;
            b.lib_uid = lib.uid;
            b.text = SerializeRavlib(RavlibOf(store, lib.uid, nullptr));
            b.uid_map = changed;
            RavlibWriter(file, &b.file_by, &b.file_at);
            *base_out = b;
        }
    }
    return true;
}

std::string FolderFileOfUid(const std::vector<FolderRavlib>& folder, const std::string& uid)
{
    if (uid.empty()) return {};
    for (const FolderRavlib& f : folder)
        if (f.uid == uid) return f.file;
    return {};
}

LinkedFileState LinkedFileStateOf(const SoundLibrary& lib, const std::vector<FolderRavlib>& folder)
{
    if (lib.ravlib_path.empty() || lib.attached) return LinkedFileState::Unlinked;
    for (const FolderRavlib& f : folder) {
        if (!SameFileName(f.file, lib.ravlib_path)) continue;
        if (f.uid.empty()) return LinkedFileState::Unreadable;
        return f.uid == lib.uid ? LinkedFileState::Ok : LinkedFileState::OtherLibrary;
    }
    return LinkedFileState::Missing;
}

namespace {
FileNameFoldFn g_fold = nullptr;

// One code point of two-byte UTF-8 (U+0080..U+07FF), folded to lower case (Latin-1, Latin Extended-A, Greek, Cyrillic).
unsigned FoldCp(unsigned cp)
{
    if (cp >= 0xC0 && cp <= 0xDE && cp != 0xD7) return cp + 0x20;
    if (cp >= 0x100 && cp <= 0x137) return (cp & 1) ? cp : cp + 1;
    if (cp >= 0x139 && cp <= 0x148) return (cp & 1) ? cp + 1 : cp;
    if (cp >= 0x14A && cp <= 0x177) return (cp & 1) ? cp : cp + 1;
    if (cp >= 0x391 && cp <= 0x3A9 && cp != 0x3A2) return cp + 0x20;
    if (cp >= 0x410 && cp <= 0x42F) return cp + 0x20;
    if (cp >= 0x400 && cp <= 0x40F) return cp + 0x50;
    return cp;
}
}  // namespace

void SetFileNameFold(FileNameFoldFn fn)
{
    g_fold = fn;
}

std::string BuiltinFoldFileName(const std::string& s)
{
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < 0x80) {
            out += static_cast<char>(std::tolower(c));
        } else if (c >= 0xC2 && c <= 0xDF && i + 1 < s.size() && (static_cast<unsigned char>(s[i + 1]) & 0xC0) == 0x80) {
            const unsigned cp = FoldCp(((c & 0x1Fu) << 6) | (static_cast<unsigned char>(s[i + 1]) & 0x3Fu));
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
            ++i;
        } else {
            out += static_cast<char>(c);
        }
    }
    return out;
}

std::string FoldFileName(const std::string& s)
{
    return g_fold ? g_fold(s) : BuiltinFoldFileName(s);
}

bool SameFileName(const std::string& a, const std::string& b)
{
    return FoldFileName(a) == FoldFileName(b);
}

LinkedFileWords LinkedFileWordsOf(LinkedFileState st, const std::string& file)
{
    LinkedFileWords w;
    const std::string intact = " This project's copy is intact.";
    const std::string again_new = "Write this project's copy as a new file (the file now there is not touched)";
    if (st == LinkedFileState::Missing) {
        w.row_tip = file + " not found in the User Library folder: click the library, then Locate...";
        w.title = file + " is not in the User Library folder";
        w.detail = "Moved, renamed or deleted." + intact;
        w.save_again_tip = "Write this project's copy as the file again";
    } else if (st == LinkedFileState::OtherLibrary) {
        w.row_tip = file + " now holds another library: click the library, then Locate... or Save it again";
        w.title = file + " holds another library";
        w.detail = "The file was replaced by another library." + intact;
        w.save_again_tip = again_new;
    } else if (st == LinkedFileState::Unreadable) {
        w.row_tip = file + " cannot be read: click the library, then Locate... or Save it again";
        w.title = file + " cannot be read";
        w.detail = "It is damaged or is not a RAV library file." + intact;
        w.save_again_tip = again_new;
    }
    return w;
}

SaveFileDecision DecideSaveToFile(const SoundLibrary& lib, const std::vector<FolderRavlib>& folder)
{
    SaveFileDecision d;
    if (lib.attached || !lib.ravlib_path.empty()) return d;  // Refuse
    d.file = FolderFileOfUid(folder, lib.uid);
    d.action = d.file.empty() ? SaveFileAction::WriteNew : SaveFileAction::LinkExisting;
    return d;
}

std::string LinkedExistingToastText(const std::string& file)
{
    return file + " already holds this library: linked to it, nothing written. Sync with file will show the differences.";
}

RewriteDecision DecideSaveAgain(const SoundLibrary& lib, const std::vector<FolderRavlib>& folder, bool target_exists_on_disk)
{
    RewriteDecision d;  // Nothing
    if (lib.attached || lib.ravlib_path.empty()) return d;
    const LinkedFileState st = LinkedFileStateOf(lib, folder);
    if (st == LinkedFileState::Ok || st == LinkedFileState::Unlinked) return d;
    d.file = FolderFileOfUid(folder, lib.uid);
    if (!d.file.empty()) {
        d.action = RewriteAction::LinkExisting;
        return d;
    }
    d.file.clear();
    d.action = st == LinkedFileState::Missing && !target_exists_on_disk ? RewriteAction::WriteSame : RewriteAction::WriteNew;
    return d;
}

RelinkDecision DecideRelink(const std::vector<FolderRavlib>& folder, const std::string& lib_uid, bool picked_in_folder,
                            const std::string& picked_name)
{
    RelinkDecision d;
    if (picked_in_folder) {
        d.action = RelinkAction::LinkPicked;
        d.file = picked_name;
        return d;
    }
    d.file = FolderFileOfUid(folder, lib_uid);
    d.action = d.file.empty() ? RelinkAction::CopyIn : RelinkAction::LinkExisting;
    return d;
}

ImportDecision DecideImport(const SoundStore& store, const std::vector<FolderRavlib>& folder, const std::string& file_uid)
{
    ImportDecision d;
    if (FindLibrary(store, file_uid)) {
        d.action = ImportAction::AlreadyInProject;
        return d;
    }
    d.file = FolderFileOfUid(folder, file_uid);
    d.action = d.file.empty() ? ImportAction::Copy : ImportAction::Refuse;
    return d;
}

std::string ImportRefusedToastText(const std::string& file)
{
    return file + " in the User Library folder is that library: Add it from the list";
}

std::vector<std::pair<std::string, std::string>> FirstSightTargets(const SoundStore& store,
                                                                    const std::vector<FolderRavlibText>& files,
                                                                    const std::function<bool(const std::string&)>& first_time)
{
    std::vector<std::pair<std::string, std::string>> out;
    for (const SoundLibrary& lib : store.libraries) {
        if (!LibraryLinked(lib) || FindLinkBase(store, lib.uid)) continue;
        if (first_time && !first_time(lib.uid)) continue;
        for (const FolderRavlibText& f : files)
            if (!f.uid.empty() && f.uid == lib.uid && SameFileName(f.file, lib.ravlib_path)) {
                out.push_back({lib.uid, f.text});
                break;
            }
    }
    return out;
}

LinkBase MakeLinkBase(const SoundStore& store, const std::string& lib_uid, const std::string& by, int64_t at,
                      int64_t synced_at)
{
    LinkBase b;
    b.lib_uid = lib_uid;
    if (FindLibrary(store, lib_uid)) b.text = SerializeRavlib(RavlibOf(store, lib_uid, nullptr));
    b.file_by = by;
    b.file_at = at;
    b.synced_at = synced_at;
    return b;
}

bool AdoptFileAsBase(SoundStore& store, const std::string& lib_uid, const std::string& file_text, int64_t synced_at)
{
    const SoundLibrary* lib = FindLibrary(store, lib_uid);
    if (!lib || !LibraryLinked(*lib) || FindLinkBase(store, lib_uid)) return false;
    const std::string mine = SerializeRavlib(RavlibOf(store, lib_uid, nullptr));
    if (!SameRavlibContent(mine, file_text)) return false;
    SoundStore parsed;
    std::string by;
    int64_t     at = 0;
    if (ParseRavlib(file_text, &parsed)) RavlibWriter(parsed, &by, &at);
    store.link_bases.push_back(MakeLinkBase(store, lib_uid, by, at, synced_at));
    return true;
}

std::string EffectiveLandsIn(const SoundLibrary& lib, const std::string& node_uid)
{
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
        // A database lists where a file was when it was scanned: the file must still be there.
        const LookupResult r = idx->Find(id);
        if (r.status == LookupStatus::Found && (!file_size || file_size(r.path) >= 0)) return r.path;
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

namespace {
// A Sound Locate may relink: a File Sound, or an item Sound `item_ok` lets through.
bool Relocatable(const SoundNode& n, const std::function<bool(const SoundNode&)>& item_ok)
{
    if (n.type != SoundNodeType::Sound) return false;
    if (n.sound.source.kind == SoundSourceKind::File) return true;
    return item_ok && item_ok(n);
}
}  // namespace

int ApplyLocated(SoundStore& store, const std::vector<std::pair<AssetIdentity, std::string>>& found,
                 const AssetIdentity* picked, const std::function<bool(const SoundNode&)>& item_ok)
{
    int changed = 0;
    for (SoundLibrary& lib : store.libraries)
        for (SoundNode& n : lib.nodes) {
            if (!Relocatable(n, item_ok)) continue;
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

int ApplyRelinked(SoundStore& store, const std::string& lib_uid, const std::string& node_uid,
                  const AssetIdentity& old_id, const AssetIdentity& now, const std::function<bool(const SoundNode&)>& item_ok)
{
    if (now.last_path.empty()) return 0;
    int changed = 0;
    for (SoundLibrary& lib : store.libraries)
        for (SoundNode& n : lib.nodes) {
            if (!Relocatable(n, item_ok)) continue;
            AssetIdentity& a = n.sound.source.asset;
            const bool self = lib.uid == lib_uid && n.uid == node_uid;
            const bool shares = !old_id.last_path.empty() && LowerAscii(a.last_path) == LowerAscii(old_id.last_path) &&
                                SameAssetIdentity(a, old_id);
            if (!self && !shares) continue;
            if (a.name == now.name && a.last_path == now.last_path && a.size == now.size && a.length == now.length)
                continue;
            a.name = now.name;
            a.last_path = now.last_path;
            a.size = now.size;
            a.length = now.length;
            ++changed;
        }
    return changed;
}

int ApplyFoundByName(SoundStore& store, const std::vector<FoundByName>& found)
{
    int changed = 0;
    for (const FoundByName& f : found) {
        if (f.found.last_path.empty()) continue;
        SoundLibrary* lib = FindLibrary(store, f.lib_uid);
        SoundNode*    n = lib ? FindNode(*lib, f.node_uid) : nullptr;
        if (!n || n->type != SoundNodeType::Sound) continue;
        AssetIdentity& a = n->sound.source.asset;
        if (!a.name.empty() || !a.last_path.empty()) continue;
        a = f.found;
        if (a.name.empty()) {
            const size_t cut = a.last_path.find_last_of("/\\");
            a.name = cut == std::string::npos ? a.last_path : a.last_path.substr(cut + 1);
        }
        ++changed;
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
            if (SameFileName(t, n)) return true;
        return false;
    };
    std::string name = base + kRavlibExt;
    for (int i = 2; used(name); ++i) name = base + " " + std::to_string(i) + kRavlibExt;
    return name;
}

// ---- 13-24 ----------------------------------------------------------------------------------------

namespace {

// Windows paths: one file whatever the case and the slash.
std::string PathKey(const std::string& p)
{
    std::string k = LowerAscii(p);
    for (char& c : k)
        if (c == '\\') c = '/';
    return k;
}

std::string UpperAscii(std::string s)
{
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

std::string BaseName(const std::string& path)
{
    const size_t p = path.find_last_of("/\\");
    return p == std::string::npos ? path : path.substr(p + 1);
}

// Why one take cannot be a file ("" = it can).
std::string TakeRefusal(const ItemTakeFile& t)
{
    const std::string type = UpperAscii(t.type);
    if (type == "MIDI" || type == "MIDIPOOL") return "MIDI";
    if (type == "RPP_PROJECT") return "a subproject";
    if (type == "SECTION") return "a reversed or section take";
    if (type == "EMPTY" || t.path.empty()) return "no media file";
    return {};
}

// 13-24 review: the frames a converted Sound keeps, read in file time once it is a File Sound.
void FreezeFrames(SoundData& d, const EntryFacts& entry)
{
    if (d.frames.empty()) {
        // Never detected (added with several items, panel not opened): the item's own detection,
        // not the file's (no peaks: Takes, TakeMarkers or the item's portion).
        const DetectMode              mode = DefaultDetectMode(entry, false);
        const std::vector<SoundFrame> frames = DetectFrames(entry, mode, d.silence, nullptr);
        if (frames.empty()) return;
        d.mode = mode;
        d.frames = frames;
        d.edited = false;
        SanitizeZones(d);  // 13-62i: laid out as zones
        return;
    }
    if (d.edited || (d.mode != DetectMode::Whole && d.mode != DetectMode::Takes)) return;
    // An item copy ignored these lengths; a file slice reads them: the item's length in source time.
    const std::vector<SoundFrame> det = DetectFrames(entry, d.mode, d.silence, nullptr);
    if (det.size() != d.frames.size()) return;
    for (size_t i = 0; i < det.size(); ++i) {
        if (d.frames[i].start == det[i].start && d.frames[i].length == det[i].length) continue;
        // 13-62i: a variation whose extent changes gets its zones again (never edited by hand: proposed anew, its
        // gain carried).
        if (!d.frames[i].zones.empty()) {
            for (const EditZone& z : d.frames[i].zones)
                if (!z.IsGap()) {
                    d.frames[i].db = z.db;
                    break;
                }
            d.frames[i].zones.clear();
        }
        d.frames[i].start = det[i].start;
        d.frames[i].length = det[i].length;
    }
    SanitizeZones(d);
}

}  // namespace

bool FileSourceFromItem(const SoundData& data, const ItemFileFacts& facts, SoundSource* out, std::string* why)
{
    auto refuse = [&](const std::string& w) {
        if (why) *why = w;
        return false;
    };
    if (data.source.kind != SoundSourceKind::Item) return refuse("not an item sound");
    if (!facts.found || data.source.item_guid.empty()) return refuse("its item is not in the project");
    if (facts.pile) return refuse("an item pile");
    if (facts.takes.empty()) return refuse("no media file");
    const ItemTakeFile* file = nullptr;
    if (data.mode == DetectMode::Takes && facts.takes.size() >= 2) {
        for (const ItemTakeFile& t : facts.takes) {
            const std::string w = TakeRefusal(t);
            if (!w.empty()) return refuse(w);
            if (file && PathKey(file->path) != PathKey(t.path)) return refuse("takes of different files");
            if (!file) file = &t;
        }
    } else {
        const int a = facts.active >= 0 && facts.active < static_cast<int>(facts.takes.size()) ? facts.active : 0;
        file = &facts.takes[static_cast<size_t>(a)];
        const std::string w = TakeRefusal(*file);
        if (!w.empty()) return refuse(w);
    }
    if (!out) return true;
    SoundSource s;
    s.kind = SoundSourceKind::File;
    s.item_guid = data.source.item_guid;  // the item is this project's copy of the file (C3)
    s.item_db = facts.entry_ok ? facts.entry.item_db : data.source.item_db;  // its gain, for later detections
    s.asset.name = BaseName(file->path);
    s.asset.size = file->size >= 0 ? file->size : -1;
    s.asset.length = file->length > 0.0 ? file->length : -1.0;
    s.asset.last_path = file->path;
    *out = s;
    return true;
}

bool ConvertLibraryToUser(SoundStore& store, const std::string& lib_uid, const std::string& file_name,
                          const std::function<ItemFileFacts(const std::string& item_guid)>& facts,
                          ConvertReport* report)
{
    ConvertReport r;
    SoundLibrary* found = FindLibrary(store, lib_uid);
    if (!found || LibraryLinked(*found)) return false;
    SoundLibrary lib = *found;
    lib.ravlib_path = file_name;
    for (SoundNode& n : lib.nodes) {
        if (n.type != SoundNodeType::Sound) continue;
        ++r.sounds;
        if (n.sound.source.kind != SoundSourceKind::Item) continue;
        const ItemFileFacts f =
            (facts && !n.sound.source.item_guid.empty()) ? facts(n.sound.source.item_guid) : ItemFileFacts{};
        SoundSource src;
        std::string why;
        if (FileSourceFromItem(n.sound, f, &src, &why)) {
            if (f.entry_ok) FreezeFrames(n.sound, f.entry);
            n.sound.source = src;
            ++r.converted;
        } else {
            r.project_only.push_back({n.uid, n.name, why});
        }
    }
    *found = lib;
    if (report) *report = r;
    return true;
}

std::string SaveToFileToastText(const std::string& lib_name, const std::string& file, const ConvertReport& r)
{
    std::string t = lib_name + " saved to " + file + " in the User Library folder: other projects can Add it";
    const int   n = static_cast<int>(r.project_only.size());
    if (n == 0) return t;
    t += ". " + std::to_string(n) + " of " + std::to_string(r.sounds) +
         (r.sounds == 1 ? " sound is" : " sounds are") + " project-only and will be missing in other projects (";
    for (int i = 0; i < n && i < 3; ++i) {
        const ProjectOnlySound& p = r.project_only[static_cast<size_t>(i)];
        t += (i ? ", " : "") + (p.name.empty() ? std::string("Sound") : p.name) + ": " + p.why;
    }
    if (n > 3) t += ", +" + std::to_string(n - 3) + " more";
    return t + ")";
}

std::string AbsoluteMediaPath(const std::string& path, const std::string& dir)
{
    if (path.empty() || dir.empty()) return path;
    const bool drive = path.size() >= 2 && std::isalpha(static_cast<unsigned char>(path[0])) && path[1] == ':';
    if (drive || path[0] == '/' || path[0] == '\\') return path;
    const char last = dir.back();
    return (last == '/' || last == '\\') ? dir + path : dir + "/" + path;
}

}  // namespace rav
