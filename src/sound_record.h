// SPDX-License-Identifier: MIT
//
// Epic 13 (D20, story 13-2) -- the text of the sound model (sound_model.h). Pure, no REAPER.
// FROZEN after 13-2: wave stories call it, never edit it.
//
// Three grammars, each `KIND key=value ...` per line like rule_record.h: string values are
// percent-encoded (space, '%', '=', ',' and control bytes), numbers in C locale, an absent field
// = its default (optional fields are written only when they differ from it). Unknown fields are
// kept and written back at the end of their line (a known field whose value does not read is
// kept the same way and stands for that field); unknown lines are kept with the object read
// last (the header, a group, a library or a node; an item record's rule or event) and written
// back at the end of that object's lines. A higher version reads best-effort and is kept on
// write.
//
// The project store (the .rpp's <RAVSOUNDS chunk, one line per chunk line):
//   RAVSOUNDS 1
//   auto key=<s> [lib=<id>]                          -- an auto marker type's sound (Antho 2026-10-12);
//   avalue group=<id> value=<id>                        read for migration only (13-15a): each becomes a
//                                                       Project `assign` and is never written again
//   assign key=<part>|<part.type> [lib=<id>|none] [kit=1]   -- 13-15a: a Project assignment (no lib = inherit)
//   asvalue group=<id> value=<id>                    -- a Switch value of the assign above
//   character uid=<id> name=<s> skel=<16 hex> [model=<16 hex>] [files=<s>,<s>]   -- a rig (model = a split's model signature)
//   cassign key=<k> [lib=<id>|none] [kit=1]          -- an assignment of the character above
//   casvalue group=<id> value=<id>                   -- a Switch value of the cassign above
//   group uid=<id> name=<s>
//   value uid=<id> name=<s> [roles=<key>,<key>]      -- a value of the group above (13-23: the side-free
//                                                       role keys whose events get its part)
//   library uid=<id> name=<s> root=<id> [color=<hex>] [user=1] [ravlib=<s>]
//   kit parts=<id>,<id> [offered=1]                  -- 13-15a: the body parts the library suggests itself for
//                                                       (right after its library line; travels in the .ravlib)
//   node uid=<id> type=sound|random|blend|switch [parent=<id>] [name=<s>] [vol=<dB>]
//        [offset_ms=<ms>] [avoid=<n>] [group=<id>] [default=<id>] [preview=<id>] [lands=<s>]
//        [roles=<k>,<k>] [koff=1]                    -- 13-31: a Blend layer's role keys, keep offset on separate events
//   case value=<id> child=<id>                       -- a Switch case of the node above
//   unassigned child=<id>                            -- a child on no value of the Switch above
//   sound kind=item|file [guid=<s>] [mode=whole|takes|tmarkers|silence] [thr=] [gap=] [pre=]
//         [tail=] [edited=1] [linked=0] [fin=<s>] [fout=<s>] [aname=<s>] [asize=<n>] [alen=<s>] [apath=<s>]
//         [arel=<s>] [amtime=<n>]                    -- the Sound data of the node above
//         [div=<group id>] [divv=<id>,<id>] [rmap=<role>:<id>,...] [lanes=split] [sync=peak|start]
//         [soff=<ms>] [cfade=<s>]                    -- 13-23/25: divided by a group (its values at divide
//                                                       time), its own role map, lane mode, sync point, cut fade
//   part v=<id> [pre=<ms>] [tail=<ms>] [sync=attack|peak|start] [soff=<ms>] [gain=<dB>] [nmode=<0-5>]
//        [ntgt=<dB>] [mono=off|left|right|sum] [fin=<s>] [fout=<s>]
//                                                    -- 13-62: a part's own settings (one line per part
//                                                       owning one, right after the sound line; only the
//                                                       fields it owns: an absent one = the Sound's)
//   frame start=<s> len=<s> [on=0] [db=<dB>] [hand=1] [fin=<s>] [fout=<s>] [fish=<n>] [fosh=<n>]
//         [cuts=<s>,<s>] [cutsh=1] [cutq=1] [sync=<s>,<s>] [hsync=<s>,<s>] [ptrim=<s>,<s>,...] [ptrimk=<key>]
//         [own=<id>,<id>,...]                        -- 13-62: who owns each piece between the cuts (cuts + 1
//                                                       of them; none = piece k is the k-th divide value)
//                                                    -- a variation of that Sound (no fin/fout = the Sound's fades;
//                                                       13-23/25: its cuts, hand / unsure flags, one sync per
//                                                       part and the hand sync points, absolute source s;
//                                                       13-54: each part's span after its pre-roll / tail and
//                                                       the key they were measured with. A RAV older than
//                                                       13-54 keeps ptrim / ptrimk as unknown fields and
//                                                       writes them back as read, even after it moved a cut
//                                                       or changed the margins: their key then no longer
//                                                       matches and this version measures them again)
//   zone start=<s> len=<s> [to=<id>]                 -- 13-20: a zone of that Sound (no to = excluded; later win)
// The auto / assign / character lines come first (a version without them keeps them as the header's
// unknown lines), then the groups, then the libraries, each library's nodes in order.
//
// The per-animation-item record (the take's P_EXT:RAV_SOUNDS):
//   RAVITEMSND 1 [seed=<n>]
//   ivalue group=<id> value=<id>                     -- an item-level Switch value
//   rule rid=<rid> [lib=<id>] [none=1]               -- none: place nothing on purpose (no lib, no none = inherit)
//   rvalue group=<id> value=<id>                     -- a value of the rule above
//   event rid=<rid> t=<clip s>
//   evalue group=<id> value=<id>                     -- an override of the event above
//   draw node=<id> pick=<n> [hand=1] [part=<id>]     -- 13-23: the part the draw is for (none = whole)
//   place item=<guid> node=<id> own=populated|attached|spread|deleted [off=<s>] [var=<n>] [track=<guid>]
//
// The tag on a placed item (item P_EXT:RAV_SND), one line:
//   RAVSND 1 owner=<guid> rid=<rid> t=<clip s> node=<id> own=populated|attached|spread [ao=<s> sy=<s>] [tk=<s>]
//        [pt=<id>]                                   -- 13-23: the part it plays (none = the whole variation)

#pragma once

#include <string>
#include <vector>

#include "rule_record.h"  // NewSoundUid, IsSoundUid
#include "sound_model.h"

namespace rav {

constexpr const char kSoundStoreMagic[] = "RAVSOUNDS";
constexpr const char kItemSoundsMagic[] = "RAVITEMSND";
constexpr const char kPlacedTagMagic[] = "RAVSND";
// The event match window (an event = rule id + clip time).
constexpr double kEventMatchSeconds = 0.030;

// False (and `out` untouched) when the text does not start with "<magic> <n>" (leading blank
// lines / BOM allowed). Never throws.
bool ParseSoundStore(const std::string& text, SoundStore* out);
// "" for an empty store (no group, no library, nothing kept): an old project saves no chunk.
std::string SerializeSoundStore(const SoundStore& store);
bool SoundStoreEmpty(const SoundStore& store);

bool ParseItemSounds(const std::string& text, ItemSounds* out);
std::string SerializeItemSounds(const ItemSounds& item);
// True when the record holds nothing (writing it would only write the header).
bool ItemSoundsEmpty(const ItemSounds& item);

bool ParsePlacedTag(const std::string& text, PlacedTag* out);
std::string SerializePlacedTag(const PlacedTag& tag);

const char* SoundNodeTypeWord(SoundNodeType t);  // "sound", "random", "blend", "switch"
bool SoundNodeTypeFromWord(const std::string& s, SoundNodeType* out);
const char* OwnershipWord(Ownership o);          // "populated", "attached", "spread", "deleted"
bool OwnershipFromWord(const std::string& s, Ownership* out);

// ---- Lookups (nullptr / empty when absent) --------------------------------------------------
const SoundLibrary* FindLibrary(const SoundStore& store, const std::string& uid);
SoundLibrary* FindLibrary(SoundStore& store, const std::string& uid);
const SoundNode* FindNode(const SoundLibrary& lib, const std::string& uid);
SoundNode* FindNode(SoundLibrary& lib, const std::string& uid);
// The node's children, in line order (uid "" = the root's parent: the root itself).
std::vector<const SoundNode*> ChildrenOf(const SoundLibrary& lib, const std::string& uid);
const SwitchGroup* FindGroup(const SoundStore& store, const std::string& uid);
SwitchGroup* FindGroup(SoundStore& store, const std::string& uid);
// The event of rule `rid` nearest to clip time `t` within `window` s, nullptr when none.
const EventSounds* FindEventSounds(const ItemSounds& item, const std::string& rid, double t,
                                   double window = kEventMatchSeconds);
EventSounds* FindEventSounds(ItemSounds& item, const std::string& rid, double t, double window = kEventMatchSeconds);
const RuleSounds* FindRuleSounds(const ItemSounds& item, const std::string& rid);
RuleSounds* FindRuleSounds(ItemSounds& item, const std::string& rid);

}  // namespace rav
