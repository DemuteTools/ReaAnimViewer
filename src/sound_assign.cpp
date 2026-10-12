// SPDX-License-Identifier: MIT
//
// See sound_assign.h.

#include "sound_assign.h"

#include <algorithm>
#include <map>

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
    BodyPart              feet;
    feet.id = "feet";
    feet.label = "Feet";
    feet.group = RoleGroup::Legs;
    feet.has_category = true;
    feet.category = AutoCategory::Feet;
    for (AutoType t : {AutoType::Step, AutoType::LiftOff, AutoType::Slide, AutoType::Pivot})
        feet.types.push_back(MakeAutoType(t));
    feet.types.push_back(RulesType());
    v.push_back(feet);
    BodyPart hands;
    hands.id = "hands";
    hands.label = "Hands";
    hands.group = RoleGroup::Arms;
    hands.has_category = true;
    hands.category = AutoCategory::Hands;
    for (AutoType t : {AutoType::Grab, AutoType::Release, AutoType::HandPivot}) hands.types.push_back(MakeAutoType(t));
    hands.types.push_back(RulesType());
    v.push_back(hands);
    BodyPart body;
    body.id = "body";
    body.label = "Hips & spine";
    body.group = RoleGroup::Body;
    body.types.push_back(RulesType());
    v.push_back(body);
    BodyPart other;
    other.id = "other";
    other.label = "Other bones";
    other.group = RoleGroup::Body;  // custom roles and raw bones: never found by group
    other.types.push_back(RulesType());
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
    switch (t) {
    case AutoType::Step: return "step";
    case AutoType::LiftOff: return "lift";
    case AutoType::Slide: return "slide";
    case AutoType::Pivot: return "pivot";
    case AutoType::Grab: return "grab";
    case AutoType::Release: return "release";
    case AutoType::HandPivot: return "hand_pivot";
    }
    return "step";
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
    } else if (rs && !rs->library.empty()) {
        r.library = rs->library;
        r.level = AssignLevel::Rule;
        r.from = rule_text;
    } else {
        for (const Source& s : chain) {
            const SoundAssign* a = FindAssign(*s.list, s.key);
            if (!a || a->state == LibState::Inherit) continue;
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
                                         const std::string& model_file)
{
    for (const SoundCharacter& c : store.characters)
        if (c.skeleton == skeleton && Lower(c.model_file) == Lower(model_file)) return &c;
    return nullptr;
}

const SoundCharacter* CharacterFor(const SoundStore& store, const std::string& skeleton, const std::string& model_file)
{
    if (skeleton.empty()) return nullptr;
    if (!model_file.empty())
        if (const SoundCharacter* c = FindCharacterExact(store, skeleton, model_file)) return c;
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
                                     const std::string& model_file)
{
    std::vector<std::string> out;
    const SoundCharacter*    cb = CharacterFor(before, skeleton, model_file);
    const SoundCharacter*    ca = CharacterFor(after, skeleton, model_file);
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

bool AdoptKitIn(std::vector<SoundAssign>& list, const std::string& part, const std::string& library_uid, bool replace)
{
    if (part.empty() || library_uid.empty()) return false;
    if (PartIsSet(list, part)) {
        if (!replace) return false;
        std::vector<std::string> keys;
        for (const SoundAssign& a : list) {
            std::string p;
            SplitAssignKey(a.key, &p, nullptr);
            if (p == part && a.key != part && a.state != LibState::Inherit) keys.push_back(a.key);
        }
        SetAssignLibraryIn(list, keys, LibState::Inherit, std::string());
    }
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

std::string DefaultCharacterName(const std::string& model_file)
{
    std::string s = model_file;
    const size_t slash = s.find_last_of("/\\");
    if (slash != std::string::npos) s = s.substr(slash + 1);
    const size_t dot = s.find_last_of('.');
    if (dot != std::string::npos && dot > 0) s = s.substr(0, dot);
    const size_t us = s.find('_');
    if (us != std::string::npos && us > 0) s = s.substr(0, us);
    return s;
}

SoundCharacter* EnsureCharacter(SoundStore& store, const CharacterRef& ref)
{
    if (SoundCharacter* c = FindCharacterByUid(store, ref.uid)) return c;
    if (ref.skeleton.empty()) return nullptr;
    if (const SoundCharacter* c = FindCharacterExact(store, ref.skeleton, ref.model_file))
        return const_cast<SoundCharacter*>(c);
    SoundCharacter n;
    n.uid = NewSoundUid();
    n.skeleton = ref.skeleton;
    n.model_file = ref.model_file;
    n.name = ref.name.empty() ? DefaultCharacterName(ref.model_file) : ref.name;
    if (n.name.empty()) n.name = "Character";
    n.files = ref.files;
    if (!ref.model_file.empty() && std::find(n.files.begin(), n.files.end(), ref.model_file) == n.files.end())
        n.files.push_back(ref.model_file);
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

int SplitCharacterIn(SoundStore& store, const std::string& uid, const std::vector<std::string>& files)
{
    const SoundCharacter* src = FindCharacterByUid(store, uid);
    if (!src) return 0;
    const SoundCharacter base = *src;  // the vector grows below
    int                  made = 0;
    for (const std::string& f : files) {
        if (f.empty() || FindCharacterExact(store, base.skeleton, f)) continue;
        SoundCharacter n;
        n.uid = NewSoundUid();
        n.name = DefaultCharacterName(f);
        if (n.name.empty()) n.name = base.name;
        n.skeleton = base.skeleton;
        n.model_file = f;
        n.files.push_back(f);
        n.assigns = base.assigns;
        store.characters.push_back(n);
        ++made;
    }
    return made;
}

}  // namespace rav
