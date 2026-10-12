// SPDX-License-Identifier: MIT
//
// Epic 13 (13-15a, pure, no REAPER) -- layered sound assignment. Which library a rule's events
// play, and which Switch values they play it with, is read through layers, the most precise
// first:   Kit > Project > Character > Item (a rule or an auto marker type) > Event.
// - "inherit" (nothing said at a level) is not "None" (place nothing, on purpose): a None stops
//   the search; an inherit passes it on to the level below.
// - Project and Character assign by body part and type ("feet", "feet.step"; sides L / R stay at
//   Item level). A target with a type wins over the same part with none.
// - A Character is a rig recognised by its skeleton signature (SkeletonKey, role_map_store.h); a
//   character with a model file is a split of the rig (one per file), the whole-rig one has "".
// - A Kit is a suggestion stored in a library (SoundLibrary::kit, it travels in the .ravlib):
//   adopting it copies the library into an assignment (kit flag set). Resolution never reads it.
// The text of the model is sound_record.h's; the types are sound_model.h's.

#pragma once

#include <string>
#include <utility>
#include <vector>

#include "auto_detect.h"
#include "bone_roles.h"
#include "sound_model.h"
#include "sound_switch.h"

namespace rav {

// ---- Body parts and types ---------------------------------------------------------------------

constexpr const char kTypeRules[] = "rules";  // the type word of a part's bone rules
// A gesture's library argument meaning None (place nothing); "" there means inherit.
constexpr const char kLibNoneWord[] = "none";

// One type of a body part: its word ("step", "rules"...), its label, and the AutoType it is.
struct PartType {
    std::string word;
    std::string label;
    bool        is_auto = false;
    AutoType    auto_type = AutoType::Step;
};

struct BodyPart {
    std::string           id;     // "feet", "hands", "body", "other"
    std::string           label;  // "Feet"
    RoleGroup             group = RoleGroup::Legs;      // the roles it holds
    bool                  has_category = false;         // feet / hands: an auto marker category
    AutoCategory          category = AutoCategory::Feet;
    std::vector<PartType> types;  // in order; "rules" last
};

// The table: feet, hands, body, other.
const std::vector<BodyPart>& BodyParts();
const BodyPart*              FindBodyPart(const std::string& id);
// "Feet"; the id itself for an unknown part.
std::string PartLabel(const std::string& part);
// "Step", "Bone rules"; the word itself for an unknown type.
std::string TypeLabel(const std::string& part, const std::string& type);

// The stable lower-case word of an auto type: "step", "lift", "slide", "pivot", "grab", "release",
// "hand_pivot".
const char* AutoTypeWord(AutoType t);
bool        AutoTypeFromWord(const std::string& w, AutoType* out);

// "feet" / "feet.step" (type "" = the part alone). SplitAssignKey is its inverse.
std::string AssignKey(const std::string& part, const std::string& type);
void        SplitAssignKey(const std::string& key, std::string* part, std::string* type);
// "Feet", "Feet \xC2\xB7 Step".
std::string AssignKeyLabel(const std::string& key);

// The part and type a block's sounds are assigned under. A bone rule: the part of the first
// built-in role of its first condition (record blocks: bone-reference ids below kBoneRefExtra are
// Role values), else "other"; type "rules". An auto block: its category's part and its AutoType's
// word. False (part "other", type "rules") for an auto block of a kind this version does not know.
bool PartOfBlock(const Block& b, std::string* part, std::string* type);

// ---- Resolution -------------------------------------------------------------------------------

// Where the library came from.
enum class AssignLevel { Unset, Rule, Character, Project };

struct EffectiveAssign {
    std::string library;               // the library uid placed ("" = nothing)
    bool        none = false;          // nothing placed on purpose (a None was met)
    AssignLevel level = AssignLevel::Unset;
    bool        type_specific = false; // Character / Project: found on "part.type" (else on "part")
    bool        kit = false;           // the assignment that gave the library was adopted from a kit
    std::string from;                  // the library's source text: "this rule", "Hero \xC2\xB7 Feet \xC2\xB7 Step",
                                       // "Project \xC2\xB7 Feet", "nothing set"
    ValueLayers layers;                // item (merged over the inherited ones) and rule; event left empty
    // The source text of each Switch Group that some layer sets: "event", "this rule" / "this type",
    // "item", "<Character> \xC2\xB7 Feet [\xC2\xB7 Step]", "Project \xC2\xB7 Feet [\xC2\xB7 Step]". A group in
    // no layer reads "default" (ValueFrom).
    std::vector<std::pair<std::string, std::string>> value_from;
    std::string ValueFromText(const std::string& group_uid) const;
    // The same for the item layer alone (what a rule's value inherits from): each group the item
    // layer holds, with where it comes from ("item", "Project \xC2\xB7 Feet"...).
    std::vector<std::pair<std::string, std::string>> item_from;
};

// The effective assignment of rule `rid` (its part and type from PartOfBlock), with `ch` its
// character (null = the Project's). `event_values` (optional, the event's overrides) fills the
// event layer.
EffectiveAssign ResolveAssignAt(const SoundStore& store, const ItemSounds& item, const SoundCharacter* ch,
                                const std::string& rid, const std::string& part, const std::string& type,
                                const std::vector<ValueSet>* event_values);
EffectiveAssign ResolveAssign(const SoundStore& store, const ItemSounds& item, const SoundCharacter* ch,
                              const std::string& rid, const std::string& part, const std::string& type);
// The same for event (rid, t) of the item: its overrides go in the event layer.
EffectiveAssign ResolveAssignFor(const SoundStore& store, const ItemSounds& item, const SoundCharacter* ch,
                                 const std::string& rid, const std::string& part, const std::string& type, double t);
// The same for a block (its part and type read from it).
EffectiveAssign ResolveBlock(const SoundStore& store, const ItemSounds& item, const SoundCharacter* ch, const Block& b);

// The character of a rig: the split of that model file (same skeleton, file equal ignoring case),
// else the rig's whole-rig character (model_file ""), else null (the Project's settings apply).
const SoundCharacter* CharacterFor(const SoundStore& store, const std::string& skeleton, const std::string& model_file);
// The record of exactly that skeleton and model file (no fallback); null when none.
const SoundCharacter* FindCharacterExact(const SoundStore& store, const std::string& skeleton,
                                         const std::string& model_file);
const SoundCharacter* FindCharacterByUid(const SoundStore& store, const std::string& uid);
SoundCharacter*       FindCharacterByUid(SoundStore& store, const std::string& uid);

// The rids of `blocks` whose effective library or values differ between the two stores (the
// item's character looked up in each). Block order, each once; a block with no rid is skipped.
std::vector<std::string> ChangedRids(const SoundStore& before, const SoundStore& after, const ItemSounds& item,
                                     const std::vector<Block>& blocks, const std::string& skeleton,
                                     const std::string& model_file);

// ---- Pure edits -------------------------------------------------------------------------------

const SoundAssign* FindAssign(const std::vector<SoundAssign>& list, const std::string& key);
SoundAssign*       FindAssign(std::vector<SoundAssign>& list, const std::string& key);

// Sets the library state of each key (inherit clears it; an entry left with nothing is removed).
// `kit`: the state came from a kit. True when the list changed.
bool SetAssignLibraryIn(std::vector<SoundAssign>& list, const std::vector<std::string>& keys, LibState state,
                        const std::string& library_uid, bool kit = false);
// Sets one Switch Group's value on each key ("" = inherit; the entry is dropped when it sets
// nothing). True when the list changed.
bool SetAssignValueIn(std::vector<SoundAssign>& list, const std::vector<std::string>& keys,
                      const std::string& group_uid, const std::string& value_uid);
// True when the part (its key alone or any "part.type") has a library state other than inherit.
bool PartIsSet(const std::vector<SoundAssign>& list, const std::string& part);
// Adopts a library's kit for `part`: its key gets the library (kit flag). Nothing happens when the
// part is already set in the list, unless `replace` (the part's own and its types' library states
// are cleared first). True when the list changed.
bool AdoptKitIn(std::vector<SoundAssign>& list, const std::string& part, const std::string& library_uid,
                bool replace = false);
// Sets a library's kit: the known part ids, deduplicated, in table order. True when it changed.
bool SetKitParts(SoundLibrary& lib, const std::vector<std::string>& parts);
bool KitServes(const SoundLibrary& lib, const std::string& part);

// A character's default name from a model file name: Hero_Body.fbx -> "Hero", Hero.glb -> "Hero".
std::string DefaultCharacterName(const std::string& model_file);

// A rig to find or make a character for (a virtual character has no record until its first edit).
struct CharacterRef {
    std::string uid;         // a stored character, or ""
    std::string skeleton;    // else the rig...
    std::string model_file;  // ...and its model file ("" = the whole rig)
    std::string name;        // the name a new record gets ("" = the default from the file)
    std::vector<std::string> files;  // the rig's model files seen (a hint a new record keeps)
};
// The stored character of the ref (by uid, else exactly by skeleton + model file), made and added
// when absent. Null when the ref names nothing (no uid match and no skeleton).
SoundCharacter* EnsureCharacter(SoundStore& store, const CharacterRef& ref);
bool            RenameCharacterIn(SoundStore& store, const std::string& uid, const std::string& name);
// Splits a character into one per model file of `files` (the rig's files): each new character is
// a copy of the rig's settings, named from its file, with model_file set; a file that already has a
// character of the rig is skipped. The original stays (it serves files not split). Returns how many
// were made.
int SplitCharacterIn(SoundStore& store, const std::string& uid, const std::vector<std::string>& files);

}  // namespace rav
