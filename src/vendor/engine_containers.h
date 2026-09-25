// Growing engine containers without freeing what the engine allocated.
//
// An insert is a rebuild into fresh buffers from the engine's own heap, the
// way the hash-set writes in component_meta.cpp are, and the old buffers are
// left alone: bg3le did not allocate them. The hash algorithm is bg3se's
// InsertToHashMap (by Norbyte and the bg3se contributors); only the stores
// are ours.
#pragma once

#include <cstdint>
#include <cstring>
#include <vector>

#include "../mem.h"

extern "C" bool bg3le_fixed_string_hash(std::uint32_t id, std::uint32_t* out);
extern "C" bool bg3le_game_allocator_ready();

namespace bg3se {
void* GameAllocRaw(std::size_t size);
unsigned int GetNearestSmallMultiHashMapPrime(unsigned int num);
}

namespace bg3le {

// bg3se's Array<T>: buffer, capacity, size.
struct RawArray {
    void* Buffer;
    std::uint32_t Capacity;
    std::uint32_t Size;
};
static_assert(sizeof(RawArray) == 16);

// Appends one element, in place while there is room, else into a buffer
// twice the size.
template <class T>
bool array_append(void* header, T const& value) {
    RawArray a{};
    if (!safe_read(header, &a, sizeof(a)) || a.Size > (1u << 24)
        || !bg3le_game_allocator_ready()) {
        return false;
    }
    if (a.Size < a.Capacity && a.Buffer != nullptr) {
        std::memcpy((char*)a.Buffer + (std::size_t)a.Size * sizeof(T), &value, sizeof(T));
        const std::uint32_t size = a.Size + 1;
        std::memcpy((char*)header + 12, &size, sizeof(size));
        return true;
    }
    const std::uint32_t capacity = a.Capacity == 0 ? 4 : a.Capacity * 2;
    void* fresh = bg3se::GameAllocRaw((std::size_t)capacity * sizeof(T));
    if (fresh == nullptr) return false;
    if (a.Size > 0 && !safe_read(a.Buffer, fresh, (std::size_t)a.Size * sizeof(T))) return false;
    std::memcpy((char*)fresh + (std::size_t)a.Size * sizeof(T), &value, sizeof(T));
    RawArray grown{fresh, capacity, a.Size + 1};
    std::memcpy(header, &grown, sizeof(grown));
    return true;
}

// bg3se's HashMap<FixedString, V>: HashKeys, NextIds, Keys, then Values.
struct RawMap {
    std::int32_t* HashKeys;
    std::uint32_t Buckets;
    std::uint32_t HashKeysSize;
    std::int32_t* NextIds;
    std::uint32_t NextCapacity;
    std::uint32_t NextSize;
    std::uint32_t* Keys;
    std::uint32_t KeysCapacity;
    std::uint32_t KeysSize;
    void* Values;
    std::uint32_t ValuesCapacity;
    std::uint32_t ValuesSize;
};
static_assert(sizeof(RawMap) == 0x40);

// Inserts key -> value, or overwrites it, and hands back where the value now
// is. In place while the arrays have room, as bg3se's HashMap::insert does,
// so nothing already in the map moves; rebuilt into fresh buffers otherwise.
// Refuses a map whose buckets do not find a sample of its own keys under the
// string table's hash, since a key bucketed by the wrong hash is silently
// unfindable rather than a crash.
template <class V>
bool fs_map_insert(void* at, std::uint32_t key, V const& value, void** slotOut = nullptr) {
    RawMap m{};
    if (!safe_read(at, &m, sizeof(m)) || !bg3le_game_allocator_ready()) return false;
    const std::uint32_t n = m.KeysSize;
    // The engine keeps a map's Values size at 0 and counts by its keys.
    if (n > (1u << 22) || m.NextSize != n || (m.ValuesSize != n && m.ValuesSize != 0)
        || (n > 0 && m.Buckets == 0)) {
        return false;
    }

    std::vector<std::uint32_t> keys(n);
    std::vector<std::int32_t> next(n), heads(m.Buckets);
    if ((n > 0 && (!safe_read(m.Keys, keys.data(), n * 4)
                   || !safe_read(m.NextIds, next.data(), n * 4)))
        || (m.Buckets > 0 && !safe_read(m.HashKeys, heads.data(), m.Buckets * 4))) {
        return false;
    }

    auto chain_reaches = [&](std::uint32_t hash, std::uint32_t index) {
        std::int32_t cur = heads[hash % m.Buckets];
        for (std::uint32_t step = 0; cur >= 0 && step <= n; ++step) {
            if ((std::uint32_t)cur == index) return true;
            cur = next[(std::uint32_t)cur];
        }
        return false;
    };
    const std::uint32_t stride = n > 64 ? n / 64 : 1;
    for (std::uint32_t i = 0; i < n; i += stride) {
        std::uint32_t hash = 0;
        if (!bg3le_fixed_string_hash(keys[i], &hash) || !chain_reaches(hash, i)) return false;
    }

    for (std::uint32_t i = 0; i < n; ++i) {
        if (keys[i] != key) continue;
        void* slot = (char*)m.Values + (std::size_t)i * sizeof(V);
        std::memcpy(slot, &value, sizeof(V));
        if (slotOut != nullptr) *slotOut = slot;
        return true;
    }

    std::uint32_t hash = 0;
    if (!bg3le_fixed_string_hash(key, &hash)) return false;

    if (n > 0 && n < m.KeysCapacity && n < m.NextCapacity && n < m.ValuesCapacity) {
        const std::uint32_t bucket = hash % m.Buckets;
        std::int32_t prev = heads[bucket];
        if (prev < 0) prev = -2 - (std::int32_t)bucket;
        void* slot = (char*)m.Values + (std::size_t)n * sizeof(V);
        std::memcpy(slot, &value, sizeof(V));
        m.Keys[n] = key;
        m.NextIds[n] = prev;
        m.HashKeys[bucket] = (std::int32_t)n;
        const std::uint32_t total = n + 1;
        std::memcpy((char*)at + offsetof(RawMap, NextSize), &total, 4);
        if (m.ValuesSize != 0) std::memcpy((char*)at + offsetof(RawMap, ValuesSize), &total, 4);
        std::memcpy((char*)at + offsetof(RawMap, KeysSize), &total, 4);
        if (slotOut != nullptr) *slotOut = slot;
        return true;
    }

    std::vector<unsigned char> values((std::size_t)n * sizeof(V));
    if (n > 0 && !safe_read(m.Values, values.data(), values.size())) return false;
    std::vector<std::uint32_t> hashes(n + 1);
    for (std::uint32_t i = 0; i < n; ++i) {
        if (!bg3le_fixed_string_hash(keys[i], &hashes[i])) return false;
    }
    hashes[n] = hash;

    const std::uint32_t total = n + 1;
    const std::uint32_t buckets = bg3se::GetNearestSmallMultiHashMapPrime(total + 2);
    auto* keysBuf = (std::uint32_t*)bg3se::GameAllocRaw(4 * total);
    auto* nextBuf = (std::int32_t*)bg3se::GameAllocRaw(4 * total);
    auto* hashBuf = (std::int32_t*)bg3se::GameAllocRaw(4 * buckets);
    auto* valueBuf = (unsigned char*)bg3se::GameAllocRaw(sizeof(V) * total);
    if (!keysBuf || !nextBuf || !hashBuf || !valueBuf) return false;

    std::memcpy(keysBuf, keys.data(), 4 * (std::size_t)n);
    keysBuf[n] = key;
    std::memcpy(valueBuf, values.data(), values.size());
    std::memcpy(valueBuf + values.size(), &value, sizeof(V));
    for (std::uint32_t b = 0; b < buckets; ++b) hashBuf[b] = -1;
    for (std::uint32_t i = 0; i < total; ++i) {
        const std::uint32_t bucket = hashes[i] % buckets;
        std::int32_t prev = hashBuf[bucket];
        if (prev < 0) prev = -2 - (std::int32_t)bucket;
        nextBuf[i] = prev;
        hashBuf[bucket] = (std::int32_t)i;
    }

    // Empty first, then filled, as the set writes do.
    const std::uint32_t zero = 0;
    std::memcpy((char*)at + offsetof(RawMap, KeysSize), &zero, 4);
    RawMap grown{hashBuf, buckets, m.HashKeysSize, nextBuf, total, total,
                 keysBuf, total, total, valueBuf, total, m.ValuesSize == 0 ? 0 : total};
    std::memcpy(at, &grown, sizeof(grown));
    if (slotOut != nullptr) *slotOut = valueBuf + values.size();
    return true;
}

}  // namespace bg3le
