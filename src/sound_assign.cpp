// SPDX-License-Identifier: MIT
//
// See sound_assign.h.

#include "sound_assign.h"

#include <algorithm>
#include <map>

#include "auto_types.h"    // story 10-9-0: the types' rows
#include "rule_record.h"   // kBoneRefExtra, NewSoundUid
#include "sound_record.h"  // FindRuleSounds, FindEventSounds, FindLibrary

namespace rav {
namespace {

const char kDotText[] = " \xC2\xB7 ";  // " · "

std::string Lower(std::string s)
{
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
    return s;
}

PartType MakeAutoType(AutoType t)
{
    PartType p;
    p.word = AutoTypeWord(t);
    p.label = AutoTypeLabel(t);
    p.is_auto = true;
    p.auto_type = t;
    return p;
}

PartType RulesType()
{
    PartType p;
    p.word = kTypeRules;
    p.label = "Bone rules";
    return p;
}

std::vector<BodyPart> BuildParts()
{
    std::vector<BodyPart> v;
    BodyPart              legs;
    legs.id = AutoCategoryRow(AutoCategory::Legs).sound_part;
    legs.label = AutoCategoryRow(AutoCategory::Legs).label;
    legs.group = RoleGroup::Legs;
    legs.has_category = true;
    legs.category = AutoCategory::Legs;
    for (AutoType t : AutoTypesOf(AutoCategory::Legs)) legs.types.push_back(MakeAutoType(t));
    legs.types.push_back(RulesType());
    legs.roles = "heels, toes, knees, legs";
    legs.role_word = "foot";
    v.push_back(legs);
    BodyPart arms;
    arms.id = AutoCategoryRow(AutoCategory::Arms).sound_part;
    arms.label = AutoCategoryRow(AutoCategory::Arms).label;
    arms.group = RoleGroup::Arms;
    arms.has_category = true;
    arms.category = AutoCategory::Arms;
    for (AutoType t : AutoTypesOf(AutoCategory::Arms)) arms.types.push_back(MakeAutoType(t));
    arms.types.push_back(RulesType());
    arms.roles = "hands, elbows, shoulders";
    arms.role_word = "hand";
    v.push_back(arms);
    // Story 10-9-1: Body (trunk, head; was "Hips & spine") is a category with no auto type yet.
    BodyPart body;
    body.id = AutoCategoryRow(AutoCategory::Body).sound_part;
    body.label = AutoCategoryRow(AutoCategory::Body).label;
    body.group = RoleGroup::Body;
    body.has_category = true;
    body.category = AutoCategory::Body;
    for (AutoType t : AutoTypesOf(AutoCategory::Body)) body.types.push_back(MakeAutoType(t));
    body.types.push_back(RulesType());
    body.roles = "hips, spine, chest, neck, head";
    body.role_word = "hips and spine";
    v.push_back(body);
    BodyPart other;
    other.id = "other";
    other.label = "Other bones";
    other.group = RoleGroup::Body;  // custom roles and raw bones: never found by group
    other.types.push_back(RulesType());
    other.roles = "other bones";
    other.role_word = "other bone";
    v.push_back(other);
    return v;
}

// Which Switch values each source sets, lowest to highest (a later source wins).
struct ValueMerge {
    std::vector<std::string>                         order;
    std::map<std::string, std::pair<std::string, std::string>> by_group;  // group -> (value, source)
    void Put(const std::vector<ValueSet>& values, const std::string& source)
    {
        for (const ValueSet& v : values) {
            if (v.group_uid.empty() || v.value_uid.empty()) continue;
            if (!by_group.count(v.group_uid)) order.push_back(v.group_uid);
            by_group[v.group_uid] = {v.value_uid, source};
        }
    }
    void Layer(std::vector<ValueSet>* out) const
    {
        for (const std::string& g : order) {
            ValueSet v;
            v.group_uid = g;
            v.value_uid = by_group.at(g).first;
            out->push_back(v);
        }
    }
};

std::string CharName(const SoundCharacter& c)
{
    return c.name.empty() ? std::string("Character") : c.name;
}

std::string SourceText(const std::string& owner, const std::string& part, const std::string& type, bool by_type)
{
    return owner + kDotText + PartLabel(part) + (by_type ? kDotText + TypeLabel(part, type) : std::string());
}

bool SameValues(const std::vector<ValueSet>& a, const std::vector<ValueSet>& b)
{
    std::map<std::string, std::string> x, y;
    for (const ValueSet& v : a)
        if (!v.value_uid.empty()) x[v.group_uid] = v.value_uid;
    for (const ValueSet& v : b)
        if (!v.value_uid.empty()) y[v.group_uid] = v.value_uid;
    return x == y;
}

bool SameEffective(const EffectiveAssign& a, const EffectiveAssign& b)
{
    return a.library == b.library && a.none == b.none && SameValues(a.layers.item, b.layers.item) &&
           SameValues(a.layers.rule, b.layers.rule);
}

bool AssignEmpty(const SoundAssign& a)
{
    return a.state == LibState::Inherit && a.values.empty() && a.kept.empty() && a.kept_lines.empty();
}

bool SetValue(std::vector<ValueSet>& values, const std::string& group, const std::string& value)
{
    for (size_t i = 0; i < values.size(); ++i) {
        if (values[i].group_uid != group) continue;
        if (value.empty()) {
            values.erase(values.begin() + static_cast<long>(i));
            return true;
        }
        if (values[i].value_uid == value) return false;
        values[i].value_uid = value;
        return true;
    }
    if (value.empty()) return false;
    ValueSet v;
    v.group_uid = group;
    v.value_uid = value;
    values.push_back(v);
    return true;
}

SoundAssign& EntryFor(std::vector<SoundAssign>& list, const std::string& key)
{
    if (SoundAssign* a = FindAssign(list, key)) return *a;
    SoundAssign fresh;
    fresh.key = key;
    list.push_back(fresh);
    return list.back();
}

void DropEmptyAssigns(std::vector<SoundAssign>& list)
{
    list.erase(std::remove_if(list.begin(), list.end(), AssignEmpty), list.end());
}

}  // namespace

// ---- Body parts -------------------------------------------------------------------------------

const std::vector<BodyPart>& BodyParts()
{
    static const std::vector<BodyPart> kParts = BuildParts();
    return kParts;
}

const BodyPart* FindBodyPart(const std::string& id)
{
    for (const BodyPart& p : BodyParts())
        if (p.id == id) return &p;
    return nullptr;
}

std::string PartLabel(const std::string& part)
{
    const BodyPart* p = FindBodyPart(part);
    return p ? p->label : part;
}

std::string TypeLabel(const std::string& part, const std::string& type)
{
    if (const BodyPart* p = FindBodyPart(part))
        for (const PartType& t : p->types)
            if (t.word == type) return t.label;
    if (type == kTypeRules) return "Bone rules";
    return type;
}

const char* AutoTypeWord(AutoType t)
{
    return AutoTypeRow(t).sound_word;  // an unknown type reads the first row's ("step")
}

bool AutoTypeFromWord(const std::string& w, AutoType* out)
{
    for (int i = 0; i < kAutoTypeCount; ++i)
        if (w == AutoTypeWord(static_cast<AutoType>(i))) {
            if (out) *out = static_cast<AutoType>(i);
            return true;
        }
    return false;
}

std::string AssignKey(const std::string& part, const std::string& type)
{
    return type.empty() ? part : part + "." + type;
}

void SplitAssignKey(const std::string& key, std::string* part, std::string* type)
{
    const size_t dot = key.find('.');
    if (part) *part = key.substr(0, dot);
    if (type) *type = dot == std::string::npos ? std::string() : key.substr(dot + 1);
}

std::string AssignKeyLabel(const std::string& key)
{
    std::string part, type;
    SplitAssignKey(key, &part, &type);
    return PartLabel(part) + (type.empty() ? std::string() : kDotText + TypeLabel(part, type));
}

bool PartOfBlock(const Block& b, std::string* part, std::string* type)
{
    std::string p = "other", t = kTypeRules;
    bool        ok = true;
    if (IsAutoBlock(b)) {
        AutoKind kind;
        char     side;
        if (AutoBlockKind(b, &kind, &side)) {
            const AutoType at = AutoTypeOfKind(kind);
            const AutoCategory cat = AutoCategoryOf(at);
            for (const BodyPart& bp : BodyParts())
                if (bp.has_category && bp.category == cat) p = bp.id;
            t = AutoTypeWord(at);
        } else {
            ok = false;
        }
    } else if (!b.conditions.empty()) {
        const SignalSpec& sp = b.conditions.front().signal;
        int               role = -1;
        for (const std::vector<int>* ids : {&sp.bones, &sp.ref_bones}) {
            for (int id : *ids)
                if (id >= 0 && id < static_cast<int>(Role::Count) && id < kBoneRefExtra) {
                    role = id;
                    break;
                }
            if (role >= 0) break;
        }
        if (role >= 0) {
            const RoleGroup g = RoleGroupOf(static_cast<Role>(role));
            for (const BodyPart& bp : BodyParts())
                if (bp.id != "other" && bp.group == g) {
                    p = bp.id;
                    break;
                }
        }
    }
    if (part) *part = p;
    if (type) *type = t;
    return ok;
}

std::string RoleFamilyKey(Role r)
{
    std::string k = RolePart(r);
    for (char& c : k)
        if (c == ' ') c = '_';
    return k == "?" ? std::string() : k;
}

std::vector<std::string> RoleMenuKeys(const std::vector<std::string>& custom_role_keys)
{
    std::vector<std::string> out = {"heel", "toe",   "toe_end", "knee",  "hip",  "hips", "clavicle",
                                    "shoulder", "elbow", "hand", "knuckle", "fingertip",  // story 10-9l: the fingers
                                    "spine", "chest", "neck", "head"};
    for (std::string key : custom_role_keys) {
        Role r;
        if (RoleFromKey(key, &r)) continue;  // a custom key that became built-in reads as the built-in
        for (const char* side : {"left_", "right_"}) {
            const std::string pre = side;
            if (key.size() > pre.size() && key.compare(0, pre.size(), pre) == 0) {
                key = key.substr(pre.size());
                break;
            }
        }
        if (key.empty() || std::find(out.begin(), out.end(), key) != out.end()) continue;
        out.push_back(key);
    }
    return out;
}

std::string RoleKeyLabel(const std::string& key)
{
    std::string s = key;
    for (char& c : s)
        if (c == '_') c = ' ';
    return s;
}

std::string EventRoleKey(const Block& b)
{
    if (IsAutoBlock(b)) {
        AutoKind kind;
        char     side;
        if (!AutoBlockKind(b, &kind, &side)) return std::string();
        return AutoKindRow(kind).role_key;
    }
    if (b.conditions.empty()) return std::string();
    const SignalSpec& sp = b.conditions.front().signal;
    for (const std::vector<int>* ids : {&sp.bones, &sp.ref_bones})
        for (int id : *ids) {
            if (id < 0) continue;
            if (id < static_cast<int>(Role::Count) && id < kBoneRefExtra) return RoleFamilyKey(static_cast<Role>(id));
            const std::string ref = BoneRefKey(id);
            static const char kRole[] = "role:";
            if (ref.compare(0, sizeof(kRole) - 1, kRole) != 0) continue;  // a plain bone: not a role
            std::string key = ref.substr(sizeof(kRole) - 1);
            Role        builtin;
            if (RoleFromKey(key, &builtin)) return RoleFamilyKey(builtin);
            for (const char* side : {"left_", "right_"}) {
                const std::string pre = side;
                if (key.size() > pre.size() && key.compare(0, pre.size(), pre) == 0) {
                    key = key.substr(pre.size());
                    break;
                }
            }
            if (!key.empty()) return key;
        }
    return std::string();
}

char EventSide(const Block& b)
{
    if (IsAutoBlock(b)) {
        AutoKind kind;
        char     side = 0;
        if (!AutoBlockKind(b, &kind, &side)) return 0;
        return side == 'L' || side == 'R' ? side : 0;
    }
    if (b.conditions.empty()) return 0;
    const SignalSpec& sp = b.conditions.front().signal;
    for (const std::vector<int>* ids : {&sp.bones, &sp.ref_bones})
        for (int id : *ids) {
            if (id < 0) continue;
            if (id < static_cast<int>(Role::Count) && id < kBoneRefExtra) {
                const char s = RoleSide(static_cast<Role>(id));
                if (s) return s;
                continue;
            }
            const std::string ref = BoneRefKey(id);
            static const char kRole[] = "role:";
            if (ref.compare(0, sizeof(kRole) - 1, kRole) != 0) continue;  // a plain bone: no side
            const std::string key = ref.substr(sizeof(kRole) - 1);
            Role              builtin;
            if (RoleFromKey(key, &builtin)) {
                if (const char s = RoleSide(builtin)) return s;
                continue;
            }
            if (key.compare(0, 5, "left_") == 0) return 'L';
            if (key.compare(0, 6, "right_") == 0) return 'R';
        }
    return 0;
}

// ---- Resolution -------------------------------------------------------------------------------

std::string EffectiveAssign::ValueFromText(const std::string& group_uid) const
{
    for (const auto& f : value_from)
        if (f.first == group_uid) return f.second;
    return "default";
}

EffectiveAssign ResolveAssignAt(const SoundStore& store, const ItemSounds& item, const SoundCharacter* ch,
                                const std::string& rid, const std::string& part, const std::string& type,
                                const std::vector<ValueSet>* event_values)
{
    EffectiveAssign r;
    r.from = "nothing set";
    const bool        by_type = !type.empty();
    const std::string type_key = AssignKey(part, type);
    const std::string rule_text = type == kTypeRules || type.empty() ? "this rule" : "this type";
    const RuleSounds* rs = rid.empty() ? nullptr : FindRuleSounds(item, rid);

    // The layers a target is read through, most precise first.
    struct Source {
        const std::vector<SoundAssign>* list;
        std::string                     key;
        AssignLevel                     level;
        bool                            typed;
        std::string                     owner;
    };
    std::vector<Source> chain;
    const std::string   cname = ch ? CharName(*ch) : std::string();
    if (ch && by_type) chain.push_back({&ch->assigns, type_key, AssignLevel::Character, true, cname});
    if (ch) chain.push_back({&ch->assigns, part, AssignLevel::Character, false, cname});
    if (by_type) chain.push_back({&store.project, type_key, AssignLevel::Project, true, "Project"});
    chain.push_back({&store.project, part, AssignLevel::Project, false, "Project"});

    // The library.
    if (rs && rs->none) {
        r.none = true;
        r.level = AssignLevel::Rule;
        r.from = rule_text;
    } else if (rs && !rs->library.empty() && FindLibrary(store, rs->library)) {
        r.library = rs->library;
        r.level = AssignLevel::Rule;
        r.from = rule_text;
    } else {
        // A rule pointing to a deleted library reads as inherit (the data stays; "Reset" clears it).
        if (rs && !rs->library.empty()) {
            r.stale_library = rs->library;
            r.stale_from = rule_text;
        }
        for (const Source& s : chain) {
            const SoundAssign* a = FindAssign(*s.list, s.key);
            if (!a || a->state == LibState::Inherit) continue;
            if (a->state == LibState::Library && !FindLibrary(store, a->library)) {  // same fall-back one level up
                if (r.stale_library.empty()) {
                    r.stale_library = a->library;
                    r.stale_from = SourceText(s.owner, part, type, s.typed);
                }
                continue;
            }
            r.level = s.level;
            r.type_specific = s.typed;
            r.kit = a->kit;
            r.from = SourceText(s.owner, part, type, s.typed);
            if (a->state == LibState::None) r.none = true;
            else r.library = a->library;
            break;
        }
    }

    // The values, lowest to highest: Project part, type, Character part, type, item, rule, event.
    ValueMerge m;
    for (size_t i = chain.size(); i-- > 0;) {
        const SoundAssign* a = FindAssign(*chain[i].list, chain[i].key);
        if (a) m.Put(a->values, SourceText(chain[i].owner, part, type, chain[i].typed));
    }
    m.Put(item.item_values, "item");
    ValueMerge top = m;  // the item layer = everything below the rule
    top.Layer(&r.layers.item);
    for (const std::string& g : top.order) r.item_from.push_back({g, top.by_group.at(g).second});
    if (rs) {
        ValueMerge rv;
        rv.Put(rs->values, rule_text);
        rv.Layer(&r.layers.rule);
        m.Put(rs->values, rule_text);
    }
    if (event_values) {
        m.Put(*event_values, "event");
        for (const ValueSet& v : *event_values)
            if (!v.group_uid.empty() && !v.value_uid.empty()) r.layers.event.push_back(v);
    }
    for (const std::string& g : m.order) r.value_from.push_back({g, m.by_group.at(g).second});
    return r;
}

EffectiveAssign ResolveAssign(const SoundStore& store, const ItemSounds& item, const SoundCharacter* ch,
                              const std::string& rid, const std::string& part, const std::string& type)
{
    return ResolveAssignAt(store, item, ch, rid, part, type, nullptr);
}

EffectiveAssign ResolveAssignFor(const SoundStore& store, const ItemSounds& item, const SoundCharacter* ch,
                                 const std::string& rid, const std::string& part, const std::string& type, double t)
{
    const EventSounds* ev = FindEventSounds(item, rid, t);
    return ResolveAssignAt(store, item, ch, rid, part, type, ev ? &ev->overrides : nullptr);
}

EffectiveAssign ResolveBlock(const SoundStore& store, const ItemSounds& item, const SoundCharacter* ch, const Block& b)
{
    std::string part, type;
    PartOfBlock(b, &part, &type);
    return ResolveAssign(store, item, ch, b.rid, part, type);
}

const SoundCharacter* FindCharacterExact(const SoundStore& store, const std::string& skeleton,
                                         const std::string& model)
{
    for (const SoundCharacter& c : store.characters)
        if (c.skeleton == skeleton && Lower(c.model) == Lower(model)) return &c;
    return nullptr;
}

const SoundCharacter* CharacterFor(const SoundStore& store, const std::string& skeleton, const std::string& model)
{
    if (skeleton.empty()) return nullptr;
    if (!model.empty())
        if (const SoundCharacter* c = FindCharacterExact(store, skeleton, model)) return c;
    return FindCharacterExact(store, skeleton, std::string());
}

const SoundCharacter* FindCharacterByUid(const SoundStore& store, const std::string& uid)
{
    if (uid.empty()) return nullptr;
    for (const SoundCharacter& c : store.characters)
        if (c.uid == uid) return &c;
    return nullptr;
}

SoundCharacter* FindCharacterByUid(SoundStore& store, const std::string& uid)
{
    return const_cast<SoundCharacter*>(FindCharacterByUid(static_cast<const SoundStore&>(store), uid));
}

std::vector<std::string> ChangedRids(const SoundStore& before, const SoundStore& after, const ItemSounds& item,
                                     const std::vector<Block>& blocks, const std::string& skeleton,
                                     const std::string& model)
{
    std::vector<std::string> out;
    const SoundCharacter*    cb = CharacterFor(before, skeleton, model);
    const SoundCharacter*    ca = CharacterFor(after, skeleton, model);
    for (const Block& b : blocks) {
        if (b.rid.empty() || std::find(out.begin(), out.end(), b.rid) != out.end()) continue;
        if (!SameEffective(ResolveBlock(before, item, cb, b), ResolveBlock(after, item, ca, b))) out.push_back(b.rid);
    }
    return out;
}

// ---- Edits ------------------------------------------------------------------------------------

const SoundAssign* FindAssign(const std::vector<SoundAssign>& list, const std::string& key)
{
    for (const SoundAssign& a : list)
        if (a.key == key) return &a;
    return nullptr;
}

SoundAssign* FindAssign(std::vector<SoundAssign>& list, const std::string& key)
{
    return const_cast<SoundAssign*>(FindAssign(static_cast<const std::vector<SoundAssign>&>(list), key));
}

bool SetAssignLibraryIn(std::vector<SoundAssign>& list, const std::vector<std::string>& keys, LibState state,
                        const std::string& library_uid, bool kit)
{
    if (state == LibState::Library && library_uid.empty()) state = LibState::Inherit;
    bool changed = false;
    for (const std::string& key : keys) {
        if (key.empty()) continue;
        if (state == LibState::Inherit && !FindAssign(list, key)) continue;
        SoundAssign& a = EntryFor(list, key);
        const std::string lib = state == LibState::Library ? library_uid : std::string();
        const bool        k = state == LibState::Inherit ? false : kit;
        if (a.state != state || a.library != lib || a.kit != k) changed = true;
        a.state = state;
        a.library = lib;
        a.kit = k;
    }
    DropEmptyAssigns(list);
    return changed;
}

bool SetAssignValueIn(std::vector<SoundAssign>& list, const std::vector<std::string>& keys,
                      const std::string& group_uid, const std::string& value_uid)
{
    if (group_uid.empty()) return false;
    bool changed = false;
    for (const std::string& key : keys) {
        if (key.empty()) continue;
        if (value_uid.empty() && !FindAssign(list, key)) continue;
        SoundAssign& a = EntryFor(list, key);
        if (SetValue(a.values, group_uid, value_uid)) changed = true;
    }
    DropEmptyAssigns(list);
    return changed;
}

bool PartIsSet(const std::vector<SoundAssign>& list, const std::string& part)
{
    for (const SoundAssign& a : list) {
        std::string p;
        SplitAssignKey(a.key, &p, nullptr);
        if (p == part && a.state != LibState::Inherit) return true;
    }
    return false;
}

bool PartKeySet(const std::vector<SoundAssign>& list, const std::string& part)
{
    const SoundAssign* a = FindAssign(list, part);
    return a && a->state != LibState::Inherit;
}

bool AdoptKitIn(std::vector<SoundAssign>& list, const std::string& part, const std::string& library_uid, bool replace)
{
    if (part.empty() || library_uid.empty()) return false;
    // Review 13-15c (the mockup's adopt): only the part's own key; its types ("legs.slide") are left as set.
    if (PartKeySet(list, part) && !replace) return false;
    return SetAssignLibraryIn(list, {part}, LibState::Library, library_uid, true);
}

bool KitServes(const SoundLibrary& lib, const std::string& part)
{
    return std::find(lib.kit.begin(), lib.kit.end(), part) != lib.kit.end();
}

bool SetKitParts(SoundLibrary& lib, const std::vector<std::string>& parts)
{
    std::vector<std::string> kit;
    for (const BodyPart& bp : BodyParts())
        if (std::find(parts.begin(), parts.end(), bp.id) != parts.end()) kit.push_back(bp.id);
    if (kit == lib.kit) return false;
    lib.kit = kit;
    return true;
}

namespace {

std::string FileStem(const std::string& file)
{
    std::string s = file;
    const size_t slash = s.find_last_of("/\\");
    if (slash != std::string::npos) s = s.substr(slash + 1);
    const size_t dot = s.find_last_of('.');
    if (dot != std::string::npos && dot > 0) s = s.substr(0, dot);
    return s;
}

}  // namespace

std::string DefaultCharacterName(const std::string& file)
{
    std::string s = FileStem(file);
    const size_t us = s.find('_');
    if (us != std::string::npos && us > 0) s = s.substr(0, us);
    return s;
}

std::string SplitCharacterName(const std::string& file)
{
    return FileStem(file);
}

std::string ModelSignature(const std::vector<MeshShape>& meshes)
{
    if (meshes.empty()) return {};
    std::vector<std::string> parts;
    for (const MeshShape& m : meshes)
        parts.push_back(std::to_string(m.verts) + "/" + std::to_string(m.indices) + "/" + std::to_string(m.material) +
                        (m.skinned ? "s" : "r"));
    std::sort(parts.begin(), parts.end());
    uint64_t h = 1469598103934665603ull;  // FNV-1a 64
    for (const std::string& p : parts)
        for (unsigned char c : p + ";") {
            h ^= c;
            h *= 1099511628211ull;
        }
    static const char kHex[] = "0123456789abcdef";
    std::string out(16, '0');
    for (int i = 15; i >= 0; --i, h >>= 4) out[i] = kHex[h & 15];
    return out;
}

SoundCharacter* EnsureCharacter(SoundStore& store, const CharacterRef& ref)
{
    if (SoundCharacter* c = FindCharacterByUid(store, ref.uid)) return c;
    if (ref.skeleton.empty()) return nullptr;
    if (const SoundCharacter* c = FindCharacterExact(store, ref.skeleton, ref.model))
        return const_cast<SoundCharacter*>(c);
    SoundCharacter n;
    n.uid = NewSoundUid();
    n.skeleton = ref.skeleton;
    n.model = ref.model;
    n.name = ref.name;
    n.files = ref.files;
    if (!ref.model.empty())
        if (const SoundCharacter* rig = FindCharacterExact(store, ref.skeleton, std::string())) {
            n.assigns = rig->assigns;  // a split reads the rig's settings until it changes them
            if (n.name.empty()) n.name = rig->name;
        }
    if (n.name.empty()) n.name = ref.files.empty() ? std::string() : DefaultCharacterName(ref.files.front());
    if (n.name.empty()) n.name = "Character";
    store.characters.push_back(n);
    return &store.characters.back();
}

bool RenameCharacterIn(SoundStore& store, const std::string& uid, const std::string& name)
{
    SoundCharacter* c = FindCharacterByUid(store, uid);
    if (!c || name.empty() || c->name == name) return false;
    c->name = name;
    return true;
}

int SplitCharacterIn(SoundStore& store, const std::string& uid, const std::vector<ModelRef>& models)
{
    const SoundCharacter* src = FindCharacterByUid(store, uid);
    if (!src) return 0;
    const SoundCharacter base = *src;  // the vector grows below
    int                  made = 0;
    for (const ModelRef& m : models) {
        if (m.model.empty() || FindCharacterExact(store, base.skeleton, m.model)) continue;
        SoundCharacter n;
        n.uid = NewSoundUid();
        n.name = m.name.empty() ? base.name : m.name;
        n.skeleton = base.skeleton;
        n.model = m.model;
        n.assigns = base.assigns;
        store.characters.push_back(n);
        ++made;
    }
    return made;
}

int ResetStaleRuleLibraries(ItemSounds& item, const SoundStore& store)
{
    int n = 0;
    for (RuleSounds& r : item.rules)
        if (!r.library.empty() && !FindLibrary(store, r.library)) {
            r.library.clear();
            ++n;
        }
    return n;
}

int ResetStaleAssignLibraries(SoundStore& store)
{
    int  n = 0;
    auto fix = [&](std::vector<SoundAssign>& list) {
        for (SoundAssign& a : list)
            if (a.state == LibState::Library && !FindLibrary(store, a.library)) {
                a.state = LibState::Inherit;
                a.library.clear();
                a.kit = false;
                ++n;
            }
    };
    fix(store.project);
    for (SoundCharacter& c : store.characters) fix(c.assigns);
    return n;
}

}  // namespace rav
