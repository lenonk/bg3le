// The templates Ext.Template reads, from the engine's own managers.
//
// Root templates come from the GlobalTemplateManager's bank, walked once.
// The server's others -- esv::CacheTemplateManager, and the current level's
// LocalTemplateManager and CacheTemplateManager -- are read live on every
// call, under the lock the engine takes, as upstream's ServerTemplate.inl
// reads them. Each global is recorded for this build and checked on read.
//
// The manager layouts are bg3se's (by Norbyte and the bg3se contributors);
// the globals and the reading are ours.

#include <stdafx.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <pthread.h>
#include <unistd.h>

#include <GameDefinitions/Level.h>
#include <GameDefinitions/RootTemplates.h>

#include "engine_containers.h"

#include "../log.h"
#include "../mem.h"

extern "C" bool bg3le_fixed_string_hash(std::uint32_t id, std::uint32_t* out);
extern "C" bool bg3le_fixed_string_index_of(char const* wanted, std::uint32_t* out);

namespace bg3le {

extern "C" char const* bg3le_fixed_string(std::uint32_t index,
                                          std::uint32_t* length);

namespace {

constexpr std::uint32_t kNullFixedString = 0xffffffffu;
constexpr std::size_t kGuidLength = 36;

struct Found {
    std::uint64_t Address{0};
    std::string Type;       // "character", "item", ... or empty
};

struct Templates {
    bool Built{false};
    std::unordered_map<std::string, Found> ById;
    std::vector<std::string> Order;
};

// Root templates, from the manager.
Templates& state() {
    static Templates t;
    return t;
}

// Guards the root set.
std::mutex& templates_lock() {
    static std::mutex m;
    return m;
}

// Where ls::GlobalTemplateManager sat in this build, relative to the
// executable's first mapping; and within it, Banks[2] at +0x20. A bank is
// { VMT, LegacyMap<FixedString, GameObjectTemplate*> Templates, ... }.
constexpr std::uintptr_t kRecordedManagerGlobal = 0x7d203f8;
constexpr std::uintptr_t kManagerBanks = 0x20;
constexpr std::uintptr_t kBankHashSize = 0x08;
constexpr std::uintptr_t kBankTable = 0x10;
constexpr std::uintptr_t kBankCount = 0x18;

template <class T>
bool read_as(void const* addr, T* out) {
    return safe_read(addr, out, sizeof(T));
}

// A template's type name, read rather than called.
//
// bg3se asks GetType(), a virtual. Calling one blind is how you run a
// destructor by accident, so the slot is decoded instead: on this build
// every template's slot four is
//
//     48 8d 05 <disp32>    lea rax, [rip+disp]
//     c3                   ret
//
// which hands back the address of a per-class static FixedString. The
// displacement is right there in the code, so the string can simply be
// read. The pattern is checked exactly, and anything else yields nothing
// rather than a guess.
constexpr std::size_t kTypeGetterSlot = 4;

char const* type_name_of(std::uint64_t vtable) {
    std::uint64_t fn = 0;
    if (!read_as((char const*)(std::uintptr_t)vtable
                     + kTypeGetterSlot * 8, &fn)) {
        return nullptr;
    }

    unsigned char code[8] = {};
    if (!safe_read((void const*)(std::uintptr_t)fn, code, sizeof(code))) {
        return nullptr;
    }
    if (code[0] != 0x48 || code[1] != 0x8d || code[2] != 0x05
        || code[7] != 0xc3) {
        return nullptr;
    }

    std::int32_t displacement = 0;
    std::memcpy(&displacement, code + 3, sizeof(displacement));
    const auto at = (std::uint64_t)((std::int64_t)fn + 7 + displacement);

    std::uint32_t index = 0;
    if (!read_as((void const*)(std::uintptr_t)at, &index)) return nullptr;
    if (index == 0 || index == kNullFixedString) return nullptr;
    return bg3le_fixed_string(index, nullptr);
}

// The executable's own mapping, so a candidate's vtable pointer can be
// tested without a syscall. A code pointer into the image is rare in data,
// which is what makes this cheap enough to apply per eight bytes.
bool image_range(unsigned long long* from, unsigned long long* to) {
    static unsigned long long low = 0;
    static unsigned long long high = 0;
    if (high != 0) {
        *from = low;
        *to = high;
        return true;
    }

    char exe[4096];
    const ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n <= 0) return false;
    exe[n] = '\0';

    std::FILE* maps = std::fopen("/proc/self/maps", "r");
    if (maps == nullptr) return false;

    char line[1024];
    while (std::fgets(line, sizeof(line), maps) != nullptr) {
        if (std::strstr(line, exe) == nullptr) continue;
        unsigned long long a = 0;
        unsigned long long b = 0;
        if (std::sscanf(line, "%llx-%llx", &a, &b) != 2) continue;
        if (low == 0 || a < low) low = a;
        if (b > high) high = b;
    }
    std::fclose(maps);

    *from = low;
    *to = high;
    return high != 0;
}

char const* type_name_of(std::uint64_t vtable);
bool image_range(unsigned long long* from, unsigned long long* to);

}  // namespace
extern "C" std::uintptr_t bg3le_image_find_static(bool (*accept)(std::uintptr_t));
namespace {

// A GlobalTemplateManager: a vtable, then a bank that holds templates keyed
// by their own Ids. Checked on the first bucket entry only, for speed.
bool looks_like_manager(std::uintptr_t mgr) {
    unsigned long long imageFrom = 0, imageTo = 0;
    if (!image_range(&imageFrom, &imageTo)) return false;
    auto in_image = [&](std::uint64_t v) { return v >= imageFrom && v < imageTo; };
    std::uint64_t vmt = 0;
    if (!read_as((void const*)mgr, &vmt) || !in_image(vmt)) return false;
    for (int slot = 0; slot < 2; ++slot) {
        std::uint64_t bank = 0, bvmt = 0, table = 0;
        std::uint32_t count = 0, hashSize = 0;
        if (!read_as((void const*)(mgr + kManagerBanks + slot * 8), &bank)
            || !read_as((void const*)bank, &bvmt) || !in_image(bvmt)
            || !read_as((void const*)(bank + kBankCount), &count) || count < 100
            || !read_as((void const*)(bank + kBankHashSize), &hashSize)
            || !read_as((void const*)(bank + kBankTable), &table)
            || hashSize == 0 || hashSize > (1u << 22)) {
            continue;
        }
        for (std::uint32_t b = 0; b < hashSize && b < 64; ++b) {
            std::uint64_t node = 0;
            if (!read_as((void const*)(table + b * 8), &node)) break;
            if (node == 0) continue;
            std::uint64_t raw[3] = {};
            std::uint64_t head[3] = {};
            return safe_read((void const*)node, raw, sizeof(raw))
                   && safe_read((void const*)raw[2], head, sizeof(head))
                   && in_image(head[0]) && (std::uint32_t)head[2] == (std::uint32_t)raw[1];
        }
    }
    return false;
}

// The static holding the manager: the recorded one, else found again.
std::uint64_t manager_static(unsigned long long imageFrom) {
    static std::uint64_t found = 0;
    static bool searched = false;
    std::uint64_t mgr = 0;
    const std::uint64_t recorded = imageFrom + kRecordedManagerGlobal;
    if (found == 0 && read_as((void const*)recorded, &mgr) && looks_like_manager(mgr)) {
        found = recorded;
    }
    if (found != 0 || searched) return found;
    searched = true;
    found = bg3le_image_find_static(&looks_like_manager);
    if (found != 0) {
        logf("templates: GlobalTemplateManager static at image+%#lx (recorded +%#lx)",
             (unsigned long)(found - imageFrom), (unsigned long)kRecordedManagerGlobal);
    }
    return found;
}

// The populated bank of the GlobalTemplateManager, walked into `out`.
bool build_from_manager(Templates* out) {
    unsigned long long imageFrom = 0, imageTo = 0;
    if (!image_range(&imageFrom, &imageTo)) return false;
    auto in_image = [&](std::uint64_t v) { return v >= imageFrom && v < imageTo; };

    std::uint64_t mgr = 0, vmt = 0;
    const std::uint64_t at = manager_static(imageFrom);
    if (at == 0 || !read_as((void const*)(std::uintptr_t)at, &mgr)
        || !read_as((void const*)(std::uintptr_t)mgr, &vmt) || !in_image(vmt)) {
        return false;
    }

    // The bank with templates in it; the other is empty on this build.
    std::uint64_t bank = 0;
    std::uint32_t best = 0;
    for (int slot = 0; slot < 2; ++slot) {
        std::uint64_t b = 0, bvmt = 0;
        std::uint32_t count = 0;
        if (!read_as((void const*)(std::uintptr_t)(mgr + kManagerBanks + slot * 8), &b)
            || !read_as((void const*)(std::uintptr_t)b, &bvmt) || !in_image(bvmt)
            || !read_as((void const*)(std::uintptr_t)(b + kBankCount), &count)) {
            continue;
        }
        if (count > best) {
            best = count;
            bank = b;
        }
    }
    std::uint32_t hashSize = 0;
    std::uint64_t table = 0;
    if (bank == 0 || best < 100
        || !read_as((void const*)(std::uintptr_t)(bank + kBankHashSize), &hashSize)
        || !read_as((void const*)(std::uintptr_t)(bank + kBankTable), &table)
        || hashSize == 0 || hashSize > (1u << 22)) {
        return false;
    }

    std::vector<std::uint64_t> buckets(hashSize);
    if (safe_read_some((void const*)(std::uintptr_t)table, buckets.data(),
                       hashSize * sizeof(std::uint64_t)) != hashSize * sizeof(std::uint64_t)) {
        return false;
    }

    // Each node is { Next, Key, Value }, and the key is the template's own
    // Id: a node that disagrees means this is not the bank.
    std::size_t disagreed = 0;
    for (std::uint64_t node : buckets) {
        for (std::uint32_t guard = 0; node != 0 && guard < (1u << 16); ++guard) {
            std::uint64_t raw[3] = {};
            if (!safe_read((void const*)(std::uintptr_t)node, raw, sizeof(raw))) break;
            node = raw[0];
            const auto key = (std::uint32_t)raw[1];
            std::uint64_t head[3] = {};  // VMT, tags, Id and TemplateName
            if (!safe_read((void const*)(std::uintptr_t)raw[2], head, sizeof(head))
                || !in_image(head[0]) || (std::uint32_t)head[2] != key) {
                ++disagreed;
                continue;
            }
            char const* id = bg3le_fixed_string(key, nullptr);
            if (id == nullptr || std::strlen(id) != kGuidLength) continue;
            char const* type = type_name_of(head[0]);
            if (out->ById.emplace(id, Found{raw[2], type != nullptr ? type : ""}).second) {
                out->Order.push_back(id);
            }
        }
    }
    if (out->ById.size() < 100 || disagreed > out->ById.size() / 100) {
        logf("templates: the manager at image+%#lx did not check out (%zu read, %zu disagreed)",
             (unsigned long)kRecordedManagerGlobal, out->ById.size(), disagreed);
        *out = Templates{};
        return false;
    }
    out->Built = true;
    return true;
}

// The root templates, from the manager: cheap, so any thread may ask, and
// a failure is retried a few seconds later rather than on every call.
// Called with templates_lock held.
bool root_ready() {
    if (state().Built) return true;
    static std::time_t lastAttempt = 0;
    const std::time_t now = std::time(nullptr);
    if (lastAttempt != 0 && now - lastAttempt < 3) return false;
    lastAttempt = now;

    Templates found{};
    if (!build_from_manager(&found)) return false;
    state() = std::move(found);
    logf("templates: %zu root templates from the GlobalTemplateManager",
         state().ById.size());
    return true;
}

Found const* lookup(char const* id) {
    auto it = state().ById.find(id);
    return it != state().ById.end() ? &it->second : nullptr;
}

// ---- the server's local and cache managers ----

constexpr std::uintptr_t kRecordedCacheGlobal = 0x7c8d978;
constexpr std::uintptr_t kRecordedLevelManagerGlobal = 0x7c9f7b0;

// TemplateManagerType, which the engine's resolver switches on.
constexpr std::uint8_t kCacheType = 3;
constexpr std::uint8_t kLevelCacheType = 5;

using CacheManager = bg3se::LevelCacheTemplateManager;
constexpr std::size_t kCacheTemplates = offsetof(CacheManager, Templates);
constexpr std::size_t kCacheLock = offsetof(CacheManager, Lock);
// The writing thread's pthread_self, or -1: the rwlock's own size past it.
constexpr std::size_t kCacheWriter = kCacheLock + sizeof(pthread_rwlock_t);
constexpr std::size_t kLocalTemplates = offsetof(bg3se::LocalTemplateManager, Templates);
constexpr std::size_t kCurrentLevel = offsetof(bg3se::esv::LevelManager, CurrentLevel);
constexpr std::size_t kLevelOwner = offsetof(bg3se::esv::Level, LevelManager);
constexpr std::size_t kLevelLocal = offsetof(bg3se::esv::Level, LocalTemplateManager);
constexpr std::size_t kLevelCache = offsetof(bg3se::esv::Level, CacheTemplateManager);
static_assert(kCacheLock == 0xd0 && kLocalTemplates == 0x38 && kCurrentLevel == 0x90
              && kLevelCache == 0xe8, "the offsets the engine's resolver reads");

bool in_image(std::uint64_t v) {
    unsigned long long from = 0, to = 0;
    return image_range(&from, &to) && v >= from && v < to;
}

bool looks_like_cache(std::uintptr_t mgr, std::uint8_t type) {
    std::uint64_t vmt = 0, writer = 0;
    std::uint8_t kind = 0;
    bg3le::RawMap map{};
    return read_as((void const*)mgr, &vmt) && in_image(vmt)
           && read_as((void const*)(mgr + 8), &kind) && kind == type
           && read_as((void const*)(mgr + kCacheWriter), &writer)
           && safe_read((void const*)(mgr + kCacheTemplates), &map, sizeof(map))
           && map.Buckets != 0 && map.Buckets < (1u << 22) && map.KeysSize <= map.KeysCapacity;
}

bool looks_like_cache_global(std::uintptr_t mgr) { return looks_like_cache(mgr, kCacheType); }

// A level manager whose current level points back at it.
bool looks_like_level_manager(std::uintptr_t mgr) {
    std::uint64_t vmt = 0, level = 0, owner = 0;
    return read_as((void const*)mgr, &vmt) && in_image(vmt)
           && read_as((void const*)(mgr + kCurrentLevel), &level) && level != 0
           && read_as((void const*)(level + kLevelOwner), &owner) && owner == mgr;
}

// The object a recorded global holds, found again by content if a patch
// moved the global. An unset one is not a moved one, so it is not searched.
std::uintptr_t held_by(std::uintptr_t recorded, bool (*accept)(std::uintptr_t),
                       std::uintptr_t* cached, char const* what) {
    unsigned long long imageFrom = 0, imageTo = 0;
    if (!image_range(&imageFrom, &imageTo)) return 0;
    std::uint64_t at = *cached != 0 ? *cached : imageFrom + recorded;
    std::uint64_t obj = 0;
    if (read_as((void const*)at, &obj) && obj != 0 && accept(obj)) {
        *cached = at;
        return obj;
    }
    static bool searched[2] = {};
    bool& done = searched[recorded == kRecordedCacheGlobal ? 0 : 1];
    if (obj != 0 || done || *cached != 0) return 0;
    done = true;
    at = bg3le_image_find_static(accept);
    if (at == 0) return 0;
    logf("templates: %s static at image+%#lx (recorded +%#lx)", what,
         (unsigned long)(at - imageFrom), (unsigned long)recorded);
    *cached = at;
    return read_as((void const*)at, &obj) ? obj : 0;
}

std::uintptr_t cache_manager() {
    static std::uintptr_t at = 0;
    return held_by(kRecordedCacheGlobal, &looks_like_cache_global, &at, "CacheTemplateManager");
}

std::uintptr_t current_level() {
    static std::uintptr_t at = 0;
    const std::uintptr_t mgr =
        held_by(kRecordedLevelManagerGlobal, &looks_like_level_manager, &at, "LevelManager");
    std::uint64_t level = 0;
    return mgr != 0 && read_as((void const*)(mgr + kCurrentLevel), &level) ? level : 0;
}

// Taken as the engine takes it: tried for a while before blocking, and not at
// all by a thread that already holds it for writing.
struct ReadLock {
    pthread_rwlock_t* Lock{nullptr};
    ReadLock(std::uintptr_t lock, std::uintptr_t writer) {
        std::uint64_t owner = ~0ull;
        if (writer != 0 && read_as((void const*)writer, &owner)
            && owner == (std::uint64_t)pthread_self()) {
            return;
        }
        Lock = (pthread_rwlock_t*)lock;
        for (int i = 0; i < 4001; ++i) {
            if (pthread_rwlock_tryrdlock(Lock) == 0) return;
        }
        pthread_rwlock_rdlock(Lock);
    }
    ~ReadLock() {
        if (Lock != nullptr) pthread_rwlock_unlock(Lock);
    }
};

struct Entry {
    std::uint32_t Key;
    std::uint64_t Address;
};

// A HashMap<FixedString, GameObjectTemplate*>'s entries, or one of them.
bool read_hash_map(std::uintptr_t at, std::uint32_t only, bool one, std::vector<Entry>* out) {
    bg3le::RawMap m{};
    if (!safe_read((void const*)at, &m, sizeof(m)) || m.KeysSize > (1u << 22)) return false;
    const std::uint32_t n = m.KeysSize;
    std::vector<std::uint32_t> keys(n);
    std::vector<std::uint64_t> values(n);
    if (n > 0 && (!safe_read(m.Keys, keys.data(), n * 4)
                  || !safe_read(m.Values, values.data(), n * 8))) {
        return false;
    }
    if (one) {
        std::uint32_t hash = 0;
        if (n == 0 || m.Buckets == 0 || !bg3le_fixed_string_hash(only, &hash)) return true;
        std::int32_t cur = -1;
        if (!read_as(m.HashKeys + hash % m.Buckets, &cur)) return false;
        for (std::uint32_t step = 0; cur >= 0 && (std::uint32_t)cur < n && step <= n; ++step) {
            if (keys[(std::uint32_t)cur] == only) {
                out->push_back({only, values[(std::uint32_t)cur]});
                return true;
            }
            if (!read_as(m.NextIds + cur, &cur)) return false;
        }
        return true;
    }
    for (std::uint32_t i = 0; i < n; ++i) out->push_back({keys[i], values[i]});
    return true;
}

// A LegacyRefMap<FixedString, GameObjectTemplate*>'s, bucketed by the id itself.
bool read_ref_map(std::uintptr_t at, std::uint32_t only, bool one, std::vector<Entry>* out) {
    struct {
        std::uint32_t ItemCount, HashSize;
        std::uint64_t Table;
    } m{};
    if (!safe_read((void const*)at, &m, sizeof(m)) || m.HashSize > (1u << 22)) return false;
    if (m.HashSize == 0) return true;
    std::vector<std::uint64_t> buckets;
    if (one) {
        buckets.resize(1);
        if (!read_as((void const*)(m.Table + (only % m.HashSize) * 8), &buckets[0])) return false;
    } else {
        buckets.resize(m.HashSize);
        if (!safe_read((void const*)m.Table, buckets.data(), m.HashSize * 8)) return false;
    }
    for (std::uint64_t node : buckets) {
        for (std::uint32_t guard = 0; node != 0 && guard <= m.ItemCount; ++guard) {
            std::uint64_t raw[3] = {};  // Next, Key, Value
            if (!safe_read((void const*)node, raw, sizeof(raw))) return false;
            const auto key = (std::uint32_t)raw[1];
            if (!one || key == only) out->push_back({key, raw[2]});
            if (one && key == only) return true;
            node = raw[0];
        }
    }
    return true;
}

// Which manager: 1 the level's local templates, 2 the server's cache, 3 the
// level's cache. False when it is not there, as upstream's nil.
bool read_source(int source, std::uint32_t only, bool one, std::vector<Entry>* out) {
    if (source == 2) {
        const std::uintptr_t mgr = cache_manager();
        if (mgr == 0) return false;
        const ReadLock held(mgr + kCacheLock, mgr + kCacheWriter);
        return read_hash_map(mgr + kCacheTemplates, only, one, out);
    }
    const std::uintptr_t level = current_level();
    if (level == 0 || (source != 1 && source != 3)) return false;
    std::uint64_t mgr = 0;
    if (!read_as((void const*)(level + (source == 1 ? kLevelLocal : kLevelCache)), &mgr)
        || mgr == 0) {
        return false;
    }
    if (source == 1) {
        const ReadLock held(mgr, 0);
        return read_ref_map(mgr + kLocalTemplates, only, one, out);
    }
    if (!looks_like_cache(mgr, kLevelCacheType)) return false;
    const ReadLock held(mgr + kCacheLock, mgr + kCacheWriter);
    return read_hash_map(mgr + kCacheTemplates, only, one, out);
}

char const* type_of(std::uint64_t tmpl) {
    std::uint64_t vmt = 0;
    return tmpl != 0 && read_as((void const*)tmpl, &vmt) && in_image(vmt) ? type_name_of(vmt)
                                                                          : nullptr;
}

}  // namespace

extern "C" bool bg3le_templates_ready() {
    const std::lock_guard<std::mutex> held(templates_lock());
    return root_ready();
}

extern "C" std::size_t bg3le_templates_count() {
    const std::lock_guard<std::mutex> held(templates_lock());
    root_ready();
    return state().Order.size();
}

extern "C" char const* bg3le_templates_id_at(std::size_t index) {
    const std::lock_guard<std::mutex> held(templates_lock());
    root_ready();
    if (index >= state().Order.size()) return nullptr;
    return state().Order[index].c_str();
}

extern "C" void* bg3le_templates_find(char const* id) {
    if (id == nullptr) return nullptr;
    const std::lock_guard<std::mutex> held(templates_lock());
    root_ready();
    Found const* found = lookup(id);
    return found != nullptr ? (void*)(std::uintptr_t)found->Address : nullptr;
}

// The engine's own name for a template's type: "character", "item" and so
// on, from the class's static FixedString.
extern "C" char const* bg3le_templates_type(char const* id) {
    if (id == nullptr) return nullptr;
    const std::lock_guard<std::mutex> held(templates_lock());
    root_ready();
    Found const* found = lookup(id);
    if (found == nullptr || found->Type.empty()) return nullptr;
    return found->Type.c_str();
}

// One template from a manager other than the root one (see read_source),
// with its engine type name, or null.
extern "C" void* bg3le_templates_in(int source, char const* id, char const** type) {
    std::uint32_t key = 0;
    std::vector<Entry> found;
    if (id == nullptr || !bg3le_fixed_string_index_of(id, &key)
        || !read_source(source, key, true, &found) || found.empty()) {
        return nullptr;
    }
    if (type != nullptr) *type = type_of(found[0].Address);
    return (void*)(std::uintptr_t)found[0].Address;
}

// Every template in one; false when the manager is not there.
extern "C" bool bg3le_templates_list(int source,
                                     void (*each)(void* ctx, char const* id, void* at,
                                                  char const* type),
                                     void* ctx) {
    std::vector<Entry> found;
    if (!read_source(source, 0, false, &found)) return false;
    for (Entry const& e : found) {
        char const* id = bg3le_fixed_string(e.Key, nullptr);
        if (id != nullptr && e.Address != 0) each(ctx, id, (void*)(std::uintptr_t)e.Address, type_of(e.Address));
    }
    return true;
}

}  // namespace bg3le
