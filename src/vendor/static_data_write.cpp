// Ext.StaticData.Create, ClearResourceBank and SyncResourceBank, as upstream's
// Lua/Libs/StaticData.inl writes them.
//
// Create adds a key to the bank's HashMap<Guid, T> and default-constructs the
// resource, with the VMT of the bank's first. A full bank grows as upstream's
// Grow does, the old values copied into a fresh buffer -- but into fresh key
// and link buffers too, where upstream's reserve frees the engine's; nothing
// of the engine's is freed. The bank calls are its ClearInternal and PostLoad
// virtuals, at the slots bg3se's declaration gives them, checked first by
// VisitorSortKey's shape two slots before.
//
// The resource types and bank layout are bg3se's (by Norbyte and the bg3se
// contributors).

#include <stdafx.h>

#include <GameDefinitions/GuidResources.h>
#include <GameDefinitions/Components/All.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <new>

#include "engine_containers.h"
#include "../log.h"
#include "../mem.h"

extern "C" void* bg3le_resource_bank(std::int32_t typeIndex);

namespace {

using bg3se::Guid;

// bg3se's HashMap<Guid, T>: HashKeys, NextIds, Keys, then the values buffer
// and its capacity.
struct GuidMap {
    std::int32_t* HashKeys;
    std::uint32_t Buckets;
    std::uint32_t Pad0;
    std::int32_t* NextIds;
    std::uint32_t NextCapacity;
    std::uint32_t NextSize;
    Guid* Keys;
    std::uint32_t KeysCapacity;
    std::uint32_t KeysSize;
    void* Values;
    std::uint32_t ValuesCapacity;
    std::uint32_t Pad1;
};
static_assert(sizeof(GuidMap) == 0x40);

std::uint32_t bucket_of(Guid const& g, std::uint32_t buckets) {
    return (std::uint32_t)((g.Val[0] ^ g.Val[1]) % buckets);
}

// Whether a sample of the map's own keys is found under bg3se's Guid hash.
bool hash_agrees(GuidMap const& m) {
    const std::uint32_t n = m.KeysSize;
    const std::uint32_t stride = n > 64 ? n / 64 : 1;
    for (std::uint32_t i = 0; i < n; i += stride) {
        std::int32_t cur = m.HashKeys[bucket_of(m.Keys[i], m.Buckets)];
        bool found = false;
        for (std::uint32_t step = 0; cur >= 0 && step <= n; ++step) {
            if ((std::uint32_t)cur == i) {
                found = true;
                break;
            }
            cur = m.NextIds[cur];
        }
        if (!found) return false;
    }
    return true;
}

template <class T>
T* create(void* bank, Guid const& guid, char const** why) {
    auto* b = static_cast<bg3se::resource::GuidResourceBank<T>*>(bank);
    auto* m = reinterpret_cast<GuidMap*>(&b->Resources);
    const std::uint32_t n = m->KeysSize;
    if (n == 0 || m->Buckets == 0) {
        *why = "Unable to create resource - resource bank is empty";
        return nullptr;
    }
    if (n > m->KeysCapacity || n != m->NextSize || !hash_agrees(*m)) {
        *why = "the resource bank's map is not laid out as bg3se's";
        return nullptr;
    }
    for (std::uint32_t i = 0; i < n; ++i) {
        if (m->Keys[i] == guid) {
            *why = "Unable to create resource - a resource with the same GUID already exists";
            return nullptr;
        }
    }
    auto* values = static_cast<T*>(m->Values);
    if (n >= m->KeysCapacity || n >= m->NextCapacity || n >= m->ValuesCapacity) {
        if (!bg3le_game_allocator_ready()) {
            *why = "the engine allocator is not up";
            return nullptr;
        }
        bg3le::logf("static data: growing a resource bank of %s; this may break resource references",
                    T::EngineClass);
        const std::uint32_t cap = std::max(n, 200u) * 2;
        auto* freshValues = static_cast<T*>(bg3se::GameAllocRaw((std::size_t)cap * sizeof(T)));
        auto* freshKeys = static_cast<Guid*>(bg3se::GameAllocRaw((std::size_t)cap * sizeof(Guid)));
        auto* freshNext = static_cast<std::int32_t*>(bg3se::GameAllocRaw((std::size_t)cap * 4));
        if (freshValues == nullptr || freshKeys == nullptr || freshNext == nullptr) {
            *why = "the engine allocator refused";
            return nullptr;
        }
        for (std::uint32_t i = 0; i < n; ++i) new (freshValues + i) T(values[i]);
        std::memcpy(freshKeys, m->Keys, (std::size_t)n * sizeof(Guid));
        std::memcpy(freshNext, m->NextIds, (std::size_t)n * 4);
        m->Values = values = freshValues;
        m->ValuesCapacity = cap;
        m->Keys = freshKeys;
        m->KeysCapacity = cap;
        m->NextIds = freshNext;
        m->NextCapacity = cap;
    }
    const std::uint32_t bucket = bucket_of(guid, m->Buckets);
    std::int32_t prev = m->HashKeys[bucket];
    if (prev < 0) prev = -2 - (std::int32_t)bucket;
    auto* resource = new (values + n) T();
    resource->VMT = values[0].VMT;
    resource->ResourceUUID = guid;
    m->Keys[n] = guid;
    m->NextIds[n] = prev;
    m->HashKeys[bucket] = (std::int32_t)n;
    m->NextSize = n + 1;
    m->KeysSize = n + 1;
    return resource;
}

// VisitorSortKey, slot 9, hands back a static FixedString: lea rax, [rip+x]; ret.
bool bank_vtable_checks_out(void* bank) {
    std::uint64_t vmt = 0, slot9 = 0;
    unsigned char code[8] = {};
    return bg3le::safe_read(bank, &vmt, sizeof(vmt))
           && bg3le::safe_read((void const*)(vmt + 9 * 8), &slot9, sizeof(slot9))
           && bg3le::safe_read((void const*)slot9, code, sizeof(code))
           && code[0] == 0x48 && code[1] == 0x8d && code[2] == 0x05 && code[7] == 0xc3;
}

}  // namespace

// Upstream's CreateGuidResource: the new resource, or null with why.
extern "C" void* bg3le_static_data_create(char const* engineClass, std::int32_t typeIndex,
                                          void const* guid16, char const** why) {
    void* bank = bg3le_resource_bank(typeIndex);
    if (bank == nullptr || engineClass == nullptr) {
        *why = "Resource manager not available";
        return nullptr;
    }
    Guid guid;
    std::memcpy(&guid, guid16, sizeof(guid));
    using namespace bg3se::resource;
#define FOR_RESOURCE_TYPE(ty) \
    if (std::strcmp(engineClass, ty::EngineClass) == 0) return create<ty>(bank, guid, why);
    FOR_EACH_GUID_RESOURCE_TYPE()
#undef FOR_RESOURCE_TYPE
    *why = "not a resource type bg3se describes";
    return nullptr;
}

// ClearResourceBank (clear) or SyncResourceBank: the bank's ClearInternal or
// PostLoad.
extern "C" bool bg3le_static_data_bank_call(std::int32_t typeIndex, bool clear, char const** why) {
    auto* bank = static_cast<bg3se::resource::GuidResourceBankBase*>(bg3le_resource_bank(typeIndex));
    if (bank == nullptr) {
        *why = "Resource manager not available";
        return false;
    }
    if (!bank_vtable_checks_out(bank)) {
        *why = "the resource bank's vtable is not the one bg3se declares";
        return false;
    }
    if (clear) {
        bank->ClearInternal();
    } else {
        bank->PostLoad();
    }
    return true;
}
