// Ext.Level's functions that need only the server's level manager, as
// upstream's Lua/Libs/Level.inl writes them.
//
// esv::LevelManager, LevelDataManager and ActivePersistentLevelTemplate are
// bg3se's (by Norbyte and the bg3se contributors); the manager is found in
// templates.cpp.

#include <stdafx.h>

#include <cstdint>
#include <cstring>

#include <GameDefinitions/Level.h>
#include <GameDefinitions/RootTemplates.h>

#include "../log.h"
#include "../mem.h"

extern "C" void* bg3le_server_level_manager();
extern "C" bool bg3le_game_allocator_ready();
extern "C" bool bg3le_fixed_string_index_of(char const* wanted, std::uint32_t* out);

namespace {

using LevelManager = bg3se::esv::LevelManager;

// A readable object whose first word points into the executable.
bool has_vtable(void const* obj) {
    std::uint64_t vmt = 0;
    return obj != nullptr && bg3le::safe_read(obj, &vmt, sizeof(vmt)) && vmt > 0x10000;
}

}  // namespace

// The LevelDataManager behind the level manager's local templates, or null.
extern "C" void* bg3le_level_data_manager() {
    auto* mgr = static_cast<LevelManager*>(bg3le_server_level_manager());
    if (mgr == nullptr) return nullptr;
    bg3se::LocalTemplateManager* local = nullptr;
    if (!bg3le::safe_read(&mgr->LocalTemplateManager, &local, sizeof(local))
        || local == nullptr) {
        return nullptr;
    }
    bg3se::LevelDataManager* data = nullptr;
    if (!bg3le::safe_read(&local->LevelDataManager, &data, sizeof(data)) || !has_vtable(data)) {
        return nullptr;
    }
    return data;
}

// Upstream's AddActivePersistentLevelTemplate: the new count, or 0 with why.
// A full set is rebuilt into a fresh buffer and the engine's left alone.
extern "C" std::uint32_t bg3le_level_add_persistent_template(char const* parent,
                                                              char const* subLevel,
                                                              char const* instance,
                                                              char const** why) {
    auto* mgr = static_cast<LevelManager*>(bg3le_server_level_manager());
    if (mgr == nullptr) {
        *why = "the server level manager is not up";
        return 0;
    }
    std::uint32_t key = 0;
    bg3se::EoCLevel* level = nullptr;
    if (parent != nullptr && bg3le_fixed_string_index_of(parent, &key)) {
        for (auto it = mgr->Levels.begin(); it != mgr->Levels.end(); ++it) {
            if (it.Key().Index == key) {
                level = it.Value();
                break;
            }
        }
    }
    if (level == nullptr) {
        *why = "no parent level by that name";
        return 0;
    }

    auto& set = level->ActiveLevelTemplates;
    const bg3se::ActivePersistentLevelTemplate entry{
        .SubLevelName = bg3se::FixedString(subLevel),
        .LevelInstanceID = bg3se::FixedString(instance),
    };
    if (set.Size < set.Capacity) {
        set.Buf[set.Size] = entry;
        set.Size++;
        return set.Size;
    }
    if (!bg3le_game_allocator_ready()) {
        *why = "the engine allocator is not up";
        return 0;
    }
    using Set = std::remove_reference_t<decltype(set)>;
    alignas(Set) unsigned char raw[sizeof(Set)];
    auto* fresh = new (raw) Set();
    const auto capacity = set.CapacityIncrement();
    fresh->Buf = fresh->RawReallocate(capacity);
    for (std::uint32_t i = 0; i < capacity; ++i) {
        new (fresh->Buf + i) bg3se::ActivePersistentLevelTemplate(
            i < set.Size ? set.Buf[i] : i == set.Size ? entry : bg3se::ActivePersistentLevelTemplate{});
    }
    fresh->Size = set.Size + 1;
    std::memcpy((void*)&set, raw, sizeof(Set));
    return fresh->Size;
}
