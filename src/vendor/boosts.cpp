// The boost prototype manager, which is what Ext.Stats.GetCachedBoost reads:
// upstream's (*eoc__BoostPrototypeManager)->Boosts.try_get(guid) (bg3se, by
// Norbyte and the bg3se contributors).
//
// It has no symbol. It was found from a live boost -- its BoostInfo's
// Prototype GUID sits in exactly one HashMap's keys -- and followed back to
// the static that holds it, whose offset is recorded for this build. Every
// read checks what it finds: a BoostPrototype's TypeName names its own
// BoostType, so a sample whose names and types mostly disagree means this is
// not the map.

#include <cstdint>
#include <cstring>
#include <link.h>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "../log.h"
#include "../mem.h"

extern "C" char const* bg3le_fixed_string(std::uint32_t index, std::uint32_t* length);
extern "C" bool bg3le_meta_enum_label_value(char const* enumName, char const* label,
                                            std::uint64_t* value);
extern "C" bool bg3le_meta_parse_guid(char const* text, void* out);
extern "C" std::uintptr_t bg3le_image_find_static(bool (*accept)(std::uintptr_t));

namespace bg3le {
namespace {

// Where the manager's static sat in this build, from the executable's base.
constexpr std::uintptr_t kRecordedManagerGlobal = 0x7bc1aa0;

// BoostPrototypeManager: IsLoading, ConditionsManager, then Boosts, a
// HashMap<Guid, BoostPrototype*> whose Keys and Values sit at +32 and +48.
constexpr std::uintptr_t kBoosts = 0x10;
constexpr std::uintptr_t kKeysBuffer = 32;
constexpr std::uintptr_t kKeysCapacity = 40;
constexpr std::uintptr_t kKeysSize = 44;
constexpr std::uintptr_t kValuesBuffer = 48;
// BoostPrototype: Description (16), TypeName, BoostData, Boost (a byte).
constexpr std::uintptr_t kTypeName = 16;
constexpr std::uintptr_t kBoostType = 32;
constexpr std::size_t kGuidSize = 16;

template <class T>
bool peek(std::uintptr_t addr, T* out) {
    return addr >= 0x10000 && safe_read(reinterpret_cast<void const*>(addr), out, sizeof(T));
}

std::uintptr_t image_base() {
    static std::uintptr_t base = [] {
        std::uintptr_t found = 0;
        ::dl_iterate_phdr([](struct dl_phdr_info* info, std::size_t, void* data) {
            *static_cast<std::uintptr_t*>(data) = info->dlpi_addr;
            return 1;  // the first entry is the executable
        }, &found);
        return found;
    }();
    return base;
}

// Whether a prototype's TypeName is the label of its own Boost type. Most
// do; a Disadvantage boost is the Advantage type under its own name.
bool names_its_type(std::uintptr_t prototype) {
    std::uint32_t typeName = 0;
    std::uint8_t type = 0;
    if (!peek(prototype + kTypeName, &typeName) || !peek(prototype + kBoostType, &type)) {
        return false;
    }
    char const* text = bg3le_fixed_string(typeName, nullptr);
    std::uint64_t value = 0;
    return text != nullptr && bg3le_meta_enum_label_value("BoostType", text, &value)
           && value == type;
}

// A manager whose Boosts map is well formed and names its own types.
bool looks_like_manager(std::uintptr_t mgr) {
    const std::uintptr_t map = mgr + kBoosts;
    std::uint32_t size = 0, capacity = 0;
    std::uintptr_t values = 0;
    if (!peek(map + kKeysSize, &size) || !peek(map + kKeysCapacity, &capacity)
        || !peek(map + kValuesBuffer, &values) || size < 100 || size > capacity
        || size > (1u << 20)) {
        return false;
    }
    std::uintptr_t sample[8] = {};
    if (!safe_read(reinterpret_cast<void const*>(values), sample, sizeof(sample))) return false;
    int agreed = 0;
    for (std::uintptr_t prototype : sample) agreed += names_its_type(prototype) ? 1 : 0;
    return agreed >= 6;
}

// The static holding the manager: the recorded one, else found again.
std::uintptr_t manager_static() {
    static std::uintptr_t found = 0;
    static bool searched = false;
    std::uintptr_t mgr = 0;
    if (found != 0 && peek(found, &mgr) && looks_like_manager(mgr)) return found;
    const std::uintptr_t recorded = image_base() + kRecordedManagerGlobal;
    if (image_base() != 0 && peek(recorded, &mgr) && looks_like_manager(mgr)) {
        return found = recorded;
    }
    if (searched) return 0;
    searched = true;
    found = bg3le_image_find_static(&looks_like_manager);
    if (found != 0) {
        logf("boosts: BoostPrototypeManager static at image+%#lx (recorded +%#lx)",
             (unsigned long)(found - image_base()), (unsigned long)kRecordedManagerGlobal);
    }
    return found;
}

struct Index {
    std::uintptr_t Map = 0;
    std::uint32_t Size = 0;
    std::unordered_map<std::string, std::uintptr_t> ByGuid;
};

std::mutex& lock() {
    static std::mutex m;
    return m;
}

// The map, checked, and indexed by GUID; rebuilt when its size moves.
Index const* index() {
    static Index built;
    std::uintptr_t mgr = 0;
    const std::uintptr_t at = manager_static();
    if (at == 0 || !peek(at, &mgr)) return nullptr;
    const std::uintptr_t map = mgr + kBoosts;
    std::uint32_t size = 0, capacity = 0;
    std::uintptr_t keys = 0, values = 0;
    if (!peek(map + kKeysSize, &size) || !peek(map + kKeysCapacity, &capacity)
        || !peek(map + kKeysBuffer, &keys) || !peek(map + kValuesBuffer, &values)
        || size == 0 || size > capacity || size > (1u << 20)) {
        return nullptr;
    }
    if (built.Map == map && built.Size == size) return &built;

    std::vector<std::uintptr_t> prototypes(size);
    std::string guids(size * kGuidSize, '\0');
    if (safe_read_some(reinterpret_cast<void const*>(values), prototypes.data(),
                       size * sizeof(std::uintptr_t)) != size * sizeof(std::uintptr_t)
        || safe_read_some(reinterpret_cast<void const*>(keys), guids.data(), guids.size())
               != guids.size()) {
        return nullptr;
    }

    built = Index{map, size, {}};
    built.ByGuid.reserve(size);
    for (std::uint32_t i = 0; i < size; ++i) {
        built.ByGuid.emplace(guids.substr(i * kGuidSize, kGuidSize), prototypes[i]);
    }
    logf("boosts: %u boost prototypes from the BoostPrototypeManager", size);
    return &built;
}

}  // namespace
}  // namespace bg3le

// The prototype for a boost GUID, or null.
extern "C" void* bg3le_boost_prototype(char const* guid) {
    unsigned char raw[16] = {};
    if (guid == nullptr || !bg3le_meta_parse_guid(guid, raw)) return nullptr;
    const std::lock_guard<std::mutex> held(bg3le::lock());
    auto const* index = bg3le::index();
    if (index == nullptr) return nullptr;
    auto found = index->ByGuid.find(std::string((char const*)raw, sizeof(raw)));
    return found == index->ByGuid.end() ? nullptr : reinterpret_cast<void*>(found->second);
}
