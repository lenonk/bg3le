// Ext.Stats.Create, as upstream's RPGStats::CreateObject (bg3se's Stats.cpp,
// by Norbyte and the bg3se contributors): a new stats::Object in the modifier
// list, every attribute unset -- -1 for conditions, 0 otherwise -- inserted
// into RPGStats::Objects by name.
//
// The object is built the way the engine's own parser builds one (see its
// inline construction at image+0x2fce120): 0xf0 bytes from the engine heap,
// Functors with five empty buckets, Using and ModifierListIndex -1, Level 1.
// The insert goes through src/vendor/engine_containers.h, so nothing the
// engine allocated is freed.

#include <stdafx.h>

#include <GameDefinitions/Stats/Stats.h>

#include <cstdint>
#include <cstring>
#include <string>

#include "../log.h"
#include "../mem.h"
#include "engine_containers.h"

extern "C" void* bg3le_rpgstats();
extern "C" void* bg3le_stats_find(char const* name);
extern "C" int bg3le_stats_list_handle(char const* listName);
extern "C" std::size_t bg3le_stats_list_attr_count(char const* listName);
extern "C" bool bg3le_stats_list_attr_at(char const* listName, std::size_t index,
                                         char const** nameOut, char const** typeOut);
extern "C" void bg3le_stats_objects_changed();
extern "C" bool bg3le_fixed_string_index_of(char const* wanted, std::uint32_t* out);
extern "C" bool bg3le_fixed_string_intern(char const* text, std::uint32_t* out);

namespace bg3le {
namespace {

using bg3se::stats::Object;
using bg3se::stats::RPGStats;
using Manager = bg3se::stats::CNamedElementManager<Object>;

static_assert(sizeof(Object) == 0xf0);
static_assert(offsetof(Object, IndexedProperties) == 0x08);
static_assert(offsetof(Object, Name) == 0x20);
static_assert(offsetof(Object, Functors) == 0x28);
static_assert(offsetof(Object, AIFlags) == 0xa8);
static_assert(offsetof(Object, Using) == 0xe0);
static_assert(offsetof(Object, ModifierListIndex) == 0xe4);
static_assert(offsetof(Object, Level) == 0xe8);
static_assert(offsetof(RPGStats, Objects) == 0xc0);
static_assert(offsetof(Manager, Values) == 0x08);
static_assert(offsetof(Manager, NameToHandle) == 0x18);
static_assert(offsetof(Manager, NextHandle) == 0x58);

template <class T>
void put(void* base, std::size_t offset, T value) {
    std::memcpy((char*)base + offset, &value, sizeof(T));
}

bool is_condition_type(char const* type) {
    return type != nullptr
           && (std::strcmp(type, "Conditions") == 0 || std::strcmp(type, "TargetConditions") == 0
               || std::strcmp(type, "UseConditions") == 0
               || std::strcmp(type, "RollConditions") == 0);
}

}  // namespace
}  // namespace bg3le

// Returns the new object, or nullptr and why (upstream's messages).
extern "C" void* bg3le_stats_create(char const* name, char const* listName, char const** err) {
    using namespace bg3le;
    static std::string why;
    auto fail = [&](std::string const& message) -> void* {
        why = message;
        if (err != nullptr) *err = why.c_str();
        return nullptr;
    };

    auto* rpg = (char*)bg3le_rpgstats();
    if (rpg == nullptr) return fail("the engine's stats manager is not where this build has it");
    const int list = bg3le_stats_list_handle(listName);
    if (list < 0) return fail(std::string("Unknown modifier list type: ") + listName);
    if (bg3le_stats_find(name) != nullptr) {
        return fail(std::string("A stats object already exists with this name: ") + name);
    }

    char* manager = rpg + offsetof(RPGStats, Objects);
    RawArray values{};
    std::int32_t nextHandle = 0;
    if (!safe_read(manager + offsetof(Manager, Values), &values, sizeof(values))
        || !safe_read(manager + offsetof(Manager, NextHandle), &nextHandle, sizeof(nextHandle))
        || values.Size == 0 || (std::int32_t)values.Size != nextHandle) {
        return fail("No stats object found to copy VMT from!");
    }
    void* first = nullptr;
    void* vmt = nullptr;
    if (!safe_read(values.Buffer, &first, sizeof(first)) || first == nullptr
        || !safe_read(first, &vmt, sizeof(vmt))) {
        return fail("No stats object found to copy VMT from!");
    }

    std::uint32_t nameId = 0;
    if (!bg3le_fixed_string_index_of(name, &nameId) && !bg3le_fixed_string_intern(name, &nameId)) {
        return fail("the stat's name could not be interned");
    }

    const std::size_t count = bg3le_stats_list_attr_count(listName);
    auto* props = (std::int32_t*)bg3se::GameAllocRaw(sizeof(std::int32_t) * (count > 0 ? count : 1));
    auto* buckets = (std::int32_t*)bg3se::GameAllocRaw(sizeof(std::int32_t) * 5);
    auto* object = (char*)bg3se::GameAllocRaw(sizeof(Object));
    if (props == nullptr || buckets == nullptr || object == nullptr) return fail("out of memory");

    for (std::size_t i = 0; i < count; ++i) {
        char const* attr = nullptr;
        char const* type = nullptr;
        bg3le_stats_list_attr_at(listName, i, &attr, &type);
        props[i] = is_condition_type(type) ? -1 : 0;
    }
    std::memset(buckets, 0xff, sizeof(std::int32_t) * 5);

    std::memset(object, 0, sizeof(Object));
    put(object, 0x00, vmt);
    put(object, 0x08, props);                  // IndexedProperties: begin,
    put(object, 0x10, props + count);          // end,
    put(object, 0x18, props + count);          // capacity
    put(object, 0x20, nameId);
    put(object, 0x28, buckets);                // Functors: five empty buckets
    put<std::uint32_t>(object, 0x30, 5);
    put<std::uint32_t>(object, 0xa8, 0xffffffffu);  // AIFlags
    put<std::int32_t>(object, 0xe0, -1);       // Using
    put<std::uint32_t>(object, 0xe4, (std::uint32_t)list);
    put<std::uint32_t>(object, 0xe8, 1);       // Level

    // Upstream's Insert: the value first, then the name, then the count, so
    // a reader that finds the name finds the object.
    if (!array_append<void*>(manager + offsetof(Manager, Values), object)) {
        return fail("could not grow RPGStats::Objects");
    }
    if (!fs_map_insert<std::int32_t>(manager + offsetof(Manager, NameToHandle), nameId, nextHandle)) {
        // Put the array back as it was; the new buffer, if any, is abandoned.
        std::memcpy(manager + offsetof(Manager, Values), &values, sizeof(values));
        return fail("RPGStats::Objects' name map does not hash the way bg3le reads it");
    }
    put<std::int32_t>(manager, offsetof(Manager, NextHandle), nextHandle + 1);
    bg3le_stats_objects_changed();
    logf("stats: created %s (%s), handle %d", name, listName, nextHandle);
    return object;
}
