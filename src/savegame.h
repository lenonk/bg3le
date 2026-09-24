#pragma once

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

}  // namespace bg3le
