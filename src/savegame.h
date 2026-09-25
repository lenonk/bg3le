#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace bg3le {

// Hooks the savegame visit so PersistentVars are written into and read from
// saves. Early enough to see the save a launch loads.
void install_savegame_hook();

// (mod UUID, JSON) pairs from the last save read.
std::vector<std::pair<std::string, std::string>> saved_persistent_vars();

// The same, once per read: false if they have already reached the mods.
bool take_saved_persistent_vars(
    std::vector<std::pair<std::string, std::string>>* out);

// A user or mod variable as a save holds it: upstream's UserVariableType
// (1 Int64, 2 Double, 3 String, 4 Composite JSON, 5 Boolean, 6 binary) and
// the value in the member that type uses. Owner is the entity or module
// UUID.
struct SavedVariable {
    std::string Owner;
    std::string Name;
    std::uint8_t Type{0};
    bool Bool{false};
    std::int64_t Int{0};
    double Num{0.0};
    std::string Str;
};

// A persistent timer, frozen: seconds left, repeat interval, and the handler
// name and JSON arguments it fires with.
struct SavedTimer {
    float Frozen{0.0f};
    float Repeat{0.0f};
    bool Paused{false};
    std::string Handler;
    std::string Args;
};

// Ext.Vars and persistent timers, the rest of upstream's save region.
struct SaveExtras {
    std::vector<SavedVariable> User;
    std::vector<SavedVariable> Mod;
    std::vector<SavedTimer> Timers;
};

// What the last save read held, once per read.
bool take_saved_extras(SaveExtras* out);

}  // namespace bg3le
