// Finds the engine's GUID resource manager, which is what Ext.StaticData
// reads.
//
// A resource is looked up by type and GUID: the manager maps a static data
// type index to a bank, and the bank maps a GUID to the resource. Half of that
// bg3le already has -- the type indices are named in the symbol table, under
// ls::ImmutableDataHeadmaster, 121 of them. The manager itself is an anonymous
// global with no symbol, like the string table.
//
// Unlike the string table, though, the fingerprint here does not have to be
// invented. The manager is one HashMap<StaticDataTypeIndex,
// GuidResourceBankBase*>, and the keys are indices bg3le already knows: a
// table whose keys are all drawn from those 121 values, with pointers whose
// first word is a vtable, is that manager rather than a coincidence. So the
// search is checked against something independently known rather than against
// a shape that merely looks right.
//
// The layouts are bg3se's, from GameDefinitions/GuidResources.h; only the
// search is ours.

#include <stdafx.h>

#include <GameDefinitions/GuidResources.h>
#include <GameDefinitions/Components/All.h>

#include <cstdio>
#include <cstring>
#include <vector>
#include <unordered_map>
#include <string>

#include "../ecs_types.h"
#include "../log.h"
#include "cache_lock.h"
#include "../mem.h"

extern "C" bool bg3le_scannable_region(char const* line,
                                       unsigned long long* from,
                                       unsigned long long* to);

namespace bg3le {
namespace {

using bg3se::resource::GuidResourceBankBase;
using bg3se::resource::GuidResourceManager;

// HashMap's layout, which the string table search already relies on:
// HashKeys (StaticArray, 16), NextIds (Array, 16), Keys (Array, 16), then
// Values (UninitializedStaticArray, 16).
constexpr std::size_t kKeysOffset = 32;
constexpr std::size_t kValuesOffset = 48;

// Within an Array: buffer, capacity, size.
constexpr std::size_t kArrayBuffer = 0;
constexpr std::size_t kArraySize = 12;

static_assert(sizeof(GuidResourceManager) == 64,
              "the manager is expected to be one HashMap");

template <class T>
bool read_as(void const* addr, T* out) {
    return safe_read(addr, out, sizeof(T));
}

// The executable's mapped range, so a vtable pointer can be told from a heap
// pointer. Taken from the main mapping rather than assumed.
struct ImageRange {
    unsigned long long From{0};
    unsigned long long To{0};
};

ImageRange image_range() {
    static ImageRange range = [] {
        ImageRange r;
        std::FILE* maps = std::fopen("/proc/self/maps", "r");
        if (maps == nullptr) return r;

        char line[512];
        while (std::fgets(line, sizeof(line), maps) != nullptr) {
            // The executable's own mappings: the first r-xp region backed by a
            // path ending in /bg3.
            if (std::strstr(line, "/bin/bg3") == nullptr) continue;
            unsigned long long from = 0;
            unsigned long long to = 0;
            if (std::sscanf(line, "%llx-%llx", &from, &to) != 2) continue;
            if (r.From == 0 || from < r.From) r.From = from;
            if (to > r.To) r.To = to;
        }
        std::fclose(maps);
        return r;
    }();
    return range;
}

bool looks_like_vtable(unsigned long long p) {
    const auto range = image_range();
    if (range.To == 0) return p > 0x1000;  // cannot tell; accept anything sane
    return p >= range.From && p < range.To;
}

// Whether a candidate address holds the resource manager's Definitions map.
//
// Every check here is against something known independently: the key set comes
// from the symbol table, and the values have to be objects whose first word
// points into the executable. A HashMap of the right shape holding arbitrary
// integers will not pass.
bool looks_like_manager(void const* candidate) {
    void* keyBuf = nullptr;
    std::uint32_t keyCount = 0;
    void* valueBuf = nullptr;

    auto const* base = (char const*)candidate;
    if (!read_as(base + kKeysOffset + kArrayBuffer, &keyBuf)) return false;
    if (!read_as(base + kKeysOffset + kArraySize, &keyCount)) return false;
    if (!read_as(base + kValuesOffset + kArrayBuffer, &valueBuf)) return false;

    if (keyBuf == nullptr || valueBuf == nullptr) return false;
    // The engine registers on the order of a hundred resource types; a handful
    // or thousands means this is something else.
    if (keyCount < 8 || keyCount > 400) return false;

    for (std::uint32_t i = 0; i < keyCount; ++i) {
        std::int32_t key = 0;
        if (!read_as((char const*)keyBuf + i * sizeof(std::int32_t), &key)) {
            return false;
        }
        if (!ecs::has_index(ecs::Context::ImmutableData, key)) return false;

        unsigned long long bank = 0;
        if (!read_as((char const*)valueBuf + i * sizeof(void*), &bank)) {
            return false;
        }
        if (bank == 0) return false;

        unsigned long long vtable = 0;
        if (!read_as((void const*)bank, &vtable)) return false;
        if (!looks_like_vtable(vtable)) return false;
    }
    return true;
}

void* g_manager = nullptr;
bool g_searched = false;

// Where the manager has sat inside its region in every run of this build:
// the region moves, the offset does not. Tried in each region first, and
// checked like any other candidate.
constexpr unsigned long long kRecordedRegionOffset = 0x15a940;

void* search_for_manager() {
    std::FILE* maps = std::fopen("/proc/self/maps", "r");
    if (maps == nullptr) return nullptr;

    char line[512];
    while (std::fgets(line, sizeof(line), maps) != nullptr) {
        unsigned long long from = 0;
        unsigned long long to = 0;
        if (!bg3le_scannable_region(line, &from, &to)) continue;
        const unsigned long long addr = from + kRecordedRegionOffset;
        if (addr + sizeof(GuidResourceManager) > to) continue;
        if (!looks_like_manager((void const*)addr)) continue;
        std::fclose(maps);
        logf("static data: resource manager at %#llx, at its recorded offset", addr);
        return (void*)addr;
    }
    std::rewind(maps);

    std::size_t regions = 0;
    std::size_t scanned = 0;
    while (std::fgets(line, sizeof(line), maps) != nullptr) {
        unsigned long long from = 0;
        unsigned long long to = 0;
        if (!bg3le_scannable_region(line, &from, &to)) continue;

        ++regions;
        if ((std::size_t)(to - from) < sizeof(GuidResourceManager)) continue;
        scanned += (std::size_t)(to - from);

        for (unsigned long long addr = from;
             addr + sizeof(GuidResourceManager) <= to; addr += 8) {
            if (!looks_like_manager((void const*)addr)) continue;

            std::fclose(maps);
            logf("static data: resource manager at %#llx (scanned %zu bytes "
                 "over %zu regions)", addr, scanned, regions);
            return (void*)addr;
        }
    }

    std::fclose(maps);
    logf("static data: resource manager not found (scanned %zu bytes over %zu "
         "regions); Ext.StaticData stays unavailable", scanned, regions);
    return nullptr;
}

// Where a bank's resource map starts, past the base class.
//
// Computed from a concrete instantiation rather than added up by hand, so the
// compiler supplies it. bg3se declares GetObjectByKey as a virtual, and
// calling it would be shorter -- but that means calling a vtable slot by
// number, which is a guess about the engine's vtable order, and a wrong guess
// there calls an unrelated function. The map's layout can be checked; a vtable
// index cannot.
// The element type is irrelevant: Resources sits immediately after the base,
// so any instantiation gives the same answer, and a local stand-in says that
// more plainly than naming a real resource would.
struct OffsetProbe : bg3se::resource::GuidResource {};

constexpr std::size_t kResourcesOffset =
    offsetof(bg3se::resource::GuidResourceBank<OffsetProbe>, Resources);

// Looks a GUID up in a bank, given the size of one resource.
//
// Not hashed: the engine's hash for a Guid key is its own, and a wrong
// hash would miss silently where a scan either finds the key or does not.
// But not a read per key either, which is what this was -- the comment
// said "banks hold hundreds of entries and this runs once per lookup",
// and both halves turned out to be wrong. A mod that edits spell lists
// calls Ext.StaticData.Get in a loop, and 5eSpells' stats pass spent two
// hundred and twenty million fault-tolerant reads here, at about a
// million a second, without finishing.
//
// So the key array is read once per bank and indexed. The keys do not
// change while the game runs -- a bank is built from the game's data and
// then read -- and the index is rebuilt if the count moves, which is the
// only way they could.
struct Bank {
    std::unordered_map<std::string, std::uint32_t> ByGuid;
    std::uint32_t Count{0};
};

std::string guid_key(void const* guid) {
    return std::string((char const*)guid, sizeof(bg3se::Guid));
}

void* find_resource(void* bank, void const* guid, std::size_t resourceSize) {
    if (bank == nullptr || guid == nullptr || resourceSize == 0) return nullptr;

    auto const* map = (char const*)bank + kResourcesOffset;

    void* keyBuf = nullptr;
    std::uint32_t keyCount = 0;
    void* valueBuf = nullptr;
    if (!read_as(map + kKeysOffset + kArrayBuffer, &keyBuf)) return nullptr;
    if (!read_as(map + kKeysOffset + kArraySize, &keyCount)) return nullptr;
    if (!read_as(map + kValuesOffset + kArrayBuffer, &valueBuf)) return nullptr;
    if (keyBuf == nullptr || valueBuf == nullptr) return nullptr;
    if (keyCount > (1u << 22)) return nullptr;  // implausible; refuse

    static std::unordered_map<void*, Bank> banks;
    Bank& known = banks[bank];
    if (known.Count != keyCount || known.ByGuid.empty()) {
        std::vector<bg3se::Guid> keys(keyCount);
        const std::size_t got =
            safe_read_some(keyBuf, keys.data(),
                           (std::size_t)keyCount * sizeof(bg3se::Guid))
            / sizeof(bg3se::Guid);

        known.ByGuid.clear();
        known.ByGuid.reserve(got);
        for (std::size_t i = 0; i < got; ++i) {
            known.ByGuid.emplace(guid_key(&keys[i]), (std::uint32_t)i);
        }
        known.Count = keyCount;
    }

    auto found = known.ByGuid.find(guid_key(guid));
    if (found == known.ByGuid.end()) return nullptr;
    return (char*)valueBuf + (std::size_t)found->second * resourceSize;
}

}  // namespace

// Found on first use and cached, for the same reason as the string table: the
// engine builds it during startup.
extern "C" void* bg3le_resource_manager() {
    const CacheLock lock(resource_cache_lock());
    if (!g_searched) {
        g_searched = true;
        g_manager = search_for_manager();
    }
    return g_manager;
}

// The bank for a static data type index, or null.
extern "C" void* bg3le_resource_bank(std::int32_t typeIndex) {
    const CacheLock lock(resource_cache_lock());
    void* manager = bg3le_resource_manager();
    if (manager == nullptr || typeIndex < 0) return nullptr;

    auto* map = reinterpret_cast<GuidResourceManager*>(manager);
    auto* bank = map->Definitions.try_get(
        (bg3se::resource::StaticDataTypeIndex)typeIndex);
    return bank != nullptr ? *bank : nullptr;
}

// How many banks the manager holds, and the index of the i'th, so the set can
// be listed and checked against the registry from script.
extern "C" std::size_t bg3le_resource_bank_count() {
    const CacheLock lock(resource_cache_lock());
    void* manager = bg3le_resource_manager();
    if (manager == nullptr) return 0;
    return reinterpret_cast<GuidResourceManager*>(manager)
        ->Definitions.keys().size();
}

// The bank's ResourceGuidsByMod, a HashMap<Guid, Array<Guid>>: each mod and
// the resources it defines, handed to `each` one mod at a time. Returns the
// number of mods, or -1 without a bank.
extern "C" long bg3le_resource_sources(std::int32_t typeIndex,
                                       void (*each)(void* context, void const* mod,
                                                    void const* guids, std::uint32_t count),
                                       void* context) {
    const CacheLock lock(resource_cache_lock());
    void* bank = bg3le_resource_bank(typeIndex);
    if (bank == nullptr) return -1;

    constexpr std::size_t kSources = offsetof(
        bg3se::resource::GuidResourceBank<OffsetProbe>, ResourceGuidsByMod);
    auto const* map = (char const*)bank + kSources;
    void* keyBuf = nullptr;
    void* valueBuf = nullptr;
    std::uint32_t keyCount = 0;
    if (!read_as(map + kKeysOffset + kArrayBuffer, &keyBuf)
        || !read_as(map + kKeysOffset + kArraySize, &keyCount)
        || !read_as(map + kValuesOffset + kArrayBuffer, &valueBuf)) {
        return -1;
    }
    if (keyCount == 0) return 0;
    if (keyBuf == nullptr || valueBuf == nullptr || keyCount > 4096) return -1;

    struct GuidArray {
        void* Buffer;
        std::uint32_t Capacity;
        std::uint32_t Size;
    };
    std::vector<bg3se::Guid> mods(keyCount);
    std::vector<GuidArray> lists(keyCount);
    if (!safe_read(keyBuf, mods.data(), keyCount * sizeof(bg3se::Guid))
        || !safe_read(valueBuf, lists.data(), keyCount * sizeof(GuidArray))) {
        return -1;
    }
    std::vector<bg3se::Guid> guids;
    for (std::uint32_t i = 0; i < keyCount; ++i) {
        const std::uint32_t n = lists[i].Size <= (1u << 20) ? lists[i].Size : 0;
        guids.resize(n);
        if (n > 0 && !safe_read(lists[i].Buffer, guids.data(), n * sizeof(bg3se::Guid))) {
            continue;
        }
        each(context, &mods[i], guids.data(), n);
    }
    return (long)keyCount;
}

// A resource by type index and GUID, or null. resourceSize is the size of one
// resource of that type, which the caller takes from the field metadata.
extern "C" void* bg3le_resource_get(std::int32_t typeIndex, void const* guid,
                                    std::size_t resourceSize) {
    const CacheLock lock(resource_cache_lock());
    return find_resource(bg3le_resource_bank(typeIndex), guid, resourceSize);
}

// How many resources a bank holds, so a caller can tell an empty bank from a
// missing GUID.
extern "C" std::size_t bg3le_resource_count(std::int32_t typeIndex) {
    const CacheLock lock(resource_cache_lock());
    void* bank = bg3le_resource_bank(typeIndex);
    if (bank == nullptr) return 0;

    std::uint32_t keyCount = 0;
    if (!read_as((char const*)bank + kResourcesOffset + kKeysOffset + kArraySize,
                 &keyCount)) {
        return 0;
    }
    return keyCount > (1u << 22) ? 0 : keyCount;
}

// The GUID of the i'th resource in a bank, for listing one.
extern "C" bool bg3le_resource_guid_at(std::int32_t typeIndex, std::size_t i,
                                       void* guidOut) {
    const CacheLock lock(resource_cache_lock());
    void* bank = bg3le_resource_bank(typeIndex);
    if (bank == nullptr || guidOut == nullptr) return false;

    auto const* map = (char const*)bank + kResourcesOffset;
    void* keyBuf = nullptr;
    std::uint32_t keyCount = 0;
    if (!read_as(map + kKeysOffset + kArrayBuffer, &keyBuf)) return false;
    if (!read_as(map + kKeysOffset + kArraySize, &keyCount)) return false;
    if (keyBuf == nullptr || i >= keyCount) return false;

    return safe_read((char const*)keyBuf + i * sizeof(bg3se::Guid), guidOut,
                     sizeof(bg3se::Guid));
}

extern "C" bool bg3le_resource_bank_at(std::size_t i, std::int32_t* typeIndex,
                                       void** bank) {
    const CacheLock lock(resource_cache_lock());
    void* manager = bg3le_resource_manager();
    if (manager == nullptr) return false;

    auto* map = reinterpret_cast<GuidResourceManager*>(manager);
    auto const& keys = map->Definitions.keys();
    if (i >= keys.size()) return false;

    *typeIndex = (std::int32_t)keys[(std::uint32_t)i];
    *bank = map->Definitions.values()[i];
    return true;
}

}  // namespace bg3le
