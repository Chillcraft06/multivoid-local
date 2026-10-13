// ue_wrap/world/game_rules.h -- the world rules of the running world (engine substrate).
//
// The per-world settings are one struct, Fstruct_gameRules: 36 members, fall damage, difficulty,
// seasons, food spoilage, the minigame and decay toggles. The game keeps it twice. The SAVED copy is
// saveSlot.localGameRules, and a save file names only the rules that differ from the defaults. The
// PER-PROCESS copy is mainGameInstance.gameRules, never saved, and it is the one nearly every rule
// is read from (three are read off the save object: blood loss, experimental WIP, cold swap). One
// blueprint writes the process copy, the game's slot menu on its way into a world, so a load that
// does not pass that menu puts the rules in force itself: ApplySavedToProcess, called by the two
// boots in ue_wrap/engine/engine_save.cpp, on the host and on a joiner alike. A joiner loads the
// host's captured save, whose saved copy is the host's, so every peer ends under the host's rules.
// ReadLocal reads BOTH copies of the local peer, so a rule whose copies differ stays visible.
// Members are enumerated by reflection, so the list adapts if a patch adds or removes a rule.
// Principle 7: the engine read and write only -- ui/ owns the render. Game thread.
#pragma once

#include <string>
#include <utility>
#include <vector>

namespace ue_wrap::game_rules {

enum class Kind { Bool, Enum, Float };

// One rule, resolved + read. `key` is the member name with its blueprint tail cut ("fallDamage"),
// the name a rule is known by; `label` is the same, prettified ("Fall damage"); `kind` picks which
// value member is meaningful.
struct RuleField {
    std::string key;
    std::string label;
    Kind        kind = Kind::Bool;
    bool        bval = false;   // Kind::Bool
    int         ival = 0;       // Kind::Enum: the ordinal
    std::string valueName;      // Kind::Enum: the game's own name for it ("Normal"); empty if the
                                //             enum object did not resolve
    float       fval = 0.f;     // Kind::Float
};

struct Snapshot {
    bool                   valid = false;
    int                    gamemode = -1;   // mainGameInstance.GameMode ordinal
    std::string            gamemodeName;    // "Story"/"Sandbox"/... or "#N"
    std::vector<RuleField> fields;          // gameRules members, declaration order
    bool                   savedValid = false;  // the save object was up and `saved` was read
    std::vector<RuleField> saved;           // the same members off the save object's localGameRules
};

// Snapshot the local peer's world rules into `out`. Returns false (out.valid
// stays false) if the GameInstance / gameRules struct isn't resolvable yet
// (still booting). Game-thread only.
bool ReadLocal(Snapshot& out);

// The `n`-th rule of `kind` in declaration order, or null. The game's rules pane addresses a rule
// this way: a checkbox row carries the index of its bool among the bools, the slider row of its
// float among the floats, a combo row of its enum among the enums.
const RuleField* NthOfKind(const std::vector<RuleField>& rules, Kind kind, int n);

// Put the save's rules in force: copy `save.localGameRules` over `gameInstance.gameRules`. The
// game does this in its slot menu on the way into a world (ui_saveSlots, the one blueprint that
// writes the process copy), so a load that does not pass that menu has to do it itself, and BEFORE
// the travel: difficulty, fall damage, water fall damage, nightmares and the grass rule are latched
// on a BeginPlay. Returns how many rules the copy changed, 0 when the two already agreed, -1 when
// either struct could not be resolved (nothing written). Game thread.
int ApplySavedToProcess(void* gameInstance, void* save);

// Write one rule by key to BOTH copies the game keeps: the process copy
// (mainGameInstance.gameRules -- takes effect now) and the saved copy
// (saveSlot.localGameRules -- persists into the save). The write walks the struct's members the
// same way ReadRulesAt does, so the field mapping mirrors ApplySavedToProcess. Fail-closed: the
// key must name a rule, value.kind must be the rule's own kind, enum ordinals must be one of the
// game's named values for that rule (see EnumOptions), floats must be finite and are clamped to
// the edit bound (see ClampFloatEdit). Returns false and writes nothing otherwise. Game thread.
bool WriteRule(const std::string& key, const RuleField& value);

// The game's named values for an enum rule: (ordinal, display name) for every ordinal whose
// enumerator the game's own library names, in ordinal order. An edit UI offers exactly these, so
// an out-of-range ordinal cannot be authored. Cached per process (the enum sets are fixed per
// game build). Empty when the key names no enum rule, or nothing resolved. Game thread.
std::vector<std::pair<int, std::string>> EnumOptions(const std::string& key);

// The sane bound for a float-rule edit. The game's own min/max is not discoverable in the
// reflected data (the pane row carries only the decimal count), so the fallback is 10x of the
// rule's current value with a floor of 1.0, symmetric about zero: a rule at 1.0 allows -10..10,
// a rule at 0 allows -10..10. NaN and inf never pass, whatever the bound. Pure math, but part
// of the game-thread write path conceptually.
bool ClampFloatEdit(float current, float desired, float& out);

}  // namespace ue_wrap::game_rules
