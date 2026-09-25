// SetRawAttribute on a functor attribute: upstream's ClearStatsFunctors, then
// what stats::Object::SetPropertyString does with StatsFunctors text. This
// build has SetPropertyString inlined into the stats loader (image+0x2fcdf90),
// so its functor branch is followed step by step through the engine's own
// helpers: split the text into [TextKey] groups (image+0x2fddd40), make each
// group's Functors set, named <stat>_<attribute>_<key> and registered in
// RPGStats::StatsFunctors (image+0x2fd0700), parse each ';'-separated functor
// (image+0x2b87470), insert it through the set's own vtable, then store the
// groups under the attribute in Object::Functors. Each helper is checked by its
// opening before any is called.
//
// Object, RPGStats and FunctorGroup are bg3se's (by Norbyte and the bg3se
// contributors), as is ClearStatsFunctors, ported here.

#include <stdafx.h>

#include <GameDefinitions/Stats/Stats.h>

#include <cctype>
#include <cstdint>
#include <cstring>
#include <vector>
#include <new>
#include <string>
#include <string_view>

#include "engine_containers.h"
#include "../hook.h"
#include "../log.h"
#include "../mem.h"

extern "C" void* bg3le_rpgstats();
extern "C" bool bg3le_fixed_string_index_of(char const* wanted, std::uint32_t* out);

namespace {

using namespace bg3se;
using namespace bg3se::stats;

constexpr std::uintptr_t kSplitGroups = 0x2fddd40;
constexpr std::uintptr_t kMakeSet = 0x2fd0700;
constexpr std::uintptr_t kParseFunctor = 0x2b87470;
constexpr std::uintptr_t kParserContext = 0x7ce43e8;
constexpr int kFunctorsInsertSlot = 3;

constexpr unsigned char kSplitGroupsHead[] = {
    0x41, 0x57, 0x41, 0x56, 0x53, 0x48, 0x81, 0xec, 0x80, 0x00, 0x00, 0x00, 0x48, 0x8d,
    0x5c, 0x24, 0x48, 0x0f, 0x57, 0xc0};
constexpr unsigned char kMakeSetHead[] = {
    0x55, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x83, 0xec, 0x28,
    0x48, 0x89, 0xfb, 0x8b, 0x7e, 0x20, 0x49, 0x89, 0xce, 0x49, 0x89, 0xd7};
constexpr unsigned char kParseFunctorHead[] = {
    0x55, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x81, 0xec, 0xf8,
    0x03, 0x00, 0x00, 0x0f, 0x57, 0xc0, 0x48, 0xc7, 0x84, 0x24, 0xe0, 0x00};

using SplitProc = void (*)(HashMap<FixedString, STDString>* out, STDString const* text, void* ctx);
using MakeSetProc = Functors* (*)(void* statsFunctors, Object* object, FixedString const* attribute,
                                  FixedString const* textKey);
using ParseProc = Functor* (*)(STDString const* text, std::uint32_t index);
using InsertProc = int (*)(Functors*, Functor*);

template <std::size_t N>
bool code_is(std::uintptr_t at, unsigned char const (&head)[N]) {
    unsigned char held[N] = {};
    return bg3le::safe_read((void const*)(bg3le::load_bias() + at), held, N)
           && std::memcmp(held, head, N) == 0;
}

// The engine's LegacyMap node, and its unlinking without freeing: an entry
// removed by name, as upstream's erase does, with the node left where it is.
struct SetNode {
    SetNode* Next;
    std::uint32_t Key;
    Functors* Value;
};

void unlink_set(RPGStats* stats, std::string const& name) {
    std::uint32_t index = 0;
    if (!bg3le_fixed_string_index_of(name.c_str(), &index)) return;
    auto* raw = reinterpret_cast<unsigned char*>(&stats->StatsFunctors);
    std::uint32_t hashSize = 0;
    SetNode** table = nullptr;
    std::memcpy(&hashSize, raw, 4);
    std::memcpy(&table, raw + 8, 8);
    if (hashSize == 0 || table == nullptr) return;
    SetNode** link = &table[index % hashSize];
    while (*link != nullptr) {
        if ((*link)->Key == index) {
            *link = (*link)->Next;
            std::uint32_t count = 0;
            std::memcpy(&count, raw + 16, 4);
            if (count > 0) --count;
            std::memcpy(raw + 16, &count, 4);
            return;
        }
        link = &(*link)->Next;
    }
}

// Upstream's ClearStatsFunctors: the sets a new value would replace, taken
// out of RPGStats::StatsFunctors first, so that replacing them does not free
// a set an inheriting stat still points at.
void clear_sets(RPGStats* stats, char const* statName, char const* key, char const* value) {
    const std::string prefix = std::string(statName) + '_' + key + '_';
    unlink_set(stats, prefix + "Default");
    const std::string functors(value);
    std::string::size_type pos = 0;
    for (;;) {
        const auto nextKey = functors.find_first_of('[', pos);
        if (nextKey == std::string::npos) break;
        pos = nextKey + 1;
        auto start = nextKey;
        while (start > 0 && std::isalnum((unsigned char)functors[start - 1])) start--;
        unlink_set(stats, prefix + functors.substr(start, nextKey - start));
    }
}

std::string_view trimmed(std::string_view s) {
    while (!s.empty() && std::isspace((unsigned char)s.front())) s.remove_prefix(1);
    while (!s.empty() && std::isspace((unsigned char)s.back())) s.remove_suffix(1);
    return s;
}

}  // namespace

extern "C" bool bg3le_stats_set_functors(void* object, char const* attribute, char const* value,
                                         char const** why) {
    static int usable = -1;
    if (usable < 0) {
        usable = code_is(kSplitGroups, kSplitGroupsHead) && code_is(kMakeSet, kMakeSetHead)
                 && code_is(kParseFunctor, kParseFunctorHead);
        if (!usable) bg3le::logf("stats: the engine's functor parsing is not where this build has it");
    }
    auto* stats = static_cast<RPGStats*>(bg3le_rpgstats());
    auto* obj = static_cast<Object*>(object);
    void* ctx = nullptr;
    if (!usable || stats == nullptr || obj == nullptr
        || !bg3le::safe_read((void const*)(bg3le::load_bias() + kParserContext), &ctx, sizeof(ctx))
        || ctx == nullptr) {
        *why = "the engine's functor parsing is not available";
        return false;
    }
    if (!bg3le_game_allocator_ready()) {
        *why = "the engine allocator is not up";
        return false;
    }

    clear_sets(stats, obj->Name.GetString(), attribute, value);

    const FixedString attr(attribute);
    const STDString text(value);
    // The engine allocates into these; they are left, not freed.
    alignas(HashMap<FixedString, STDString>) unsigned char groupsRaw[sizeof(HashMap<FixedString, STDString>)] = {};
    auto* groups = new (groupsRaw) HashMap<FixedString, STDString>();
    reinterpret_cast<SplitProc>(bg3le::load_bias() + kSplitGroups)(groups, &text, ctx);

    auto makeSet = reinterpret_cast<MakeSetProc>(bg3le::load_bias() + kMakeSet);
    auto parse = reinterpret_cast<ParseProc>(bg3le::load_bias() + kParseFunctor);
    alignas(Array<FunctorGroup>) unsigned char resultRaw[sizeof(Array<FunctorGroup>)] = {};
    auto* result = new (resultRaw) Array<FunctorGroup>();
    std::uint32_t index = 0;
    for (auto it = groups->begin(); it != groups->end(); ++it) {
        FixedString const& textKey = it.Key();
        auto* set = makeSet(&stats->StatsFunctors, obj, &attr, &textKey);
        if (set == nullptr) continue;
        std::uint64_t vmt = 0, insert = 0;
        if (!bg3le::safe_read(set, &vmt, sizeof(vmt))
            || !bg3le::safe_read((void const*)(vmt + kFunctorsInsertSlot * 8), &insert, sizeof(insert))) {
            continue;
        }
        std::string_view rest = it.Value();
        while (!rest.empty()) {
            const auto semi = rest.find(';');
            const auto piece = trimmed(rest.substr(0, semi));
            rest = semi == std::string_view::npos ? std::string_view{} : rest.substr(semi + 1);
            if (piece.empty()) continue;
            const STDString one(piece.data(), piece.size());
            if (auto* functor = parse(&one, index)) {
                reinterpret_cast<InsertProc>(insert)(set, functor);
            }
            ++index;
        }
        result->push_back(FunctorGroup{textKey, set});
    }

    struct { void* Buffer; std::uint32_t Capacity; std::uint32_t Size; } header{};
    std::memcpy(&header, resultRaw, sizeof(header));
    if (!bg3le::fs_map_insert(&obj->Functors, attr.Index, header)) {
        *why = "the stat's functor map could not take the new value";
        return false;
    }
    return true;
}

// The engine's [TextKey] split of a stats value, one call per group.
extern "C" bool bg3le_stats_split_groups(char const* value,
                                         void (*each)(void* user, char const* key,
                                                      char const* text, std::size_t size),
                                         void* user) {
    static const bool usable = code_is(kSplitGroups, kSplitGroupsHead);
    void* ctx = nullptr;
    if (!usable || !bg3le_game_allocator_ready()
        || !bg3le::safe_read((void const*)(bg3le::load_bias() + kParserContext), &ctx, sizeof(ctx))
        || ctx == nullptr) {
        return false;
    }
    const STDString text(value);
    // The engine allocates into this; it is left, not freed.
    alignas(HashMap<FixedString, STDString>) unsigned char groupsRaw[sizeof(HashMap<FixedString, STDString>)] = {};
    auto* groups = new (groupsRaw) HashMap<FixedString, STDString>();
    reinterpret_cast<SplitProc>(bg3le::load_bias() + kSplitGroups)(groups, &text, ctx);
    for (auto it = groups->begin(); it != groups->end(); ++it) {
        STDString const& group = it.Value();
        each(user, it.Key().GetString(), group.data(), group.size());
    }
    return true;
}

// The rest of upstream's Object::CopyFrom, past the indexed properties and
// AIFlags: the Functors and RollConditions maps entry by entry (reusing the
// compiled sets, as upstream does), Requirements and the two combo sets. Each
// copy is bg3se's own copy constructor into fresh buffers; what the
// destination held is left, not freed.
namespace {

template <class T>
void assign_container(T* dest, T const& source) {
    alignas(T) unsigned char raw[sizeof(T)];
    new (raw) T(source);
    std::memcpy((void*)dest, raw, sizeof(T));
}

template <class V>
bool copy_map(HashMap<FixedString, V>& dest, HashMap<FixedString, V> const& source) {
    for (auto it = source.begin(); it != source.end(); ++it) {
        alignas(V) unsigned char raw[sizeof(V)];
        new (raw) V(it.Value());
        struct { void* Buffer; std::uint32_t Capacity; std::uint32_t Size; } header{};
        static_assert(sizeof(V) == sizeof(header));
        std::memcpy(&header, raw, sizeof(header));
        if (!bg3le::fs_map_insert(&dest, it.Key().Index, header)) return false;
    }
    return true;
}

}  // namespace

extern "C" bool bg3le_stats_copy_rest(void* dest, void const* source) {
    auto* to = static_cast<Object*>(dest);
    auto const* from = static_cast<Object const*>(source);
    if (to == nullptr || from == nullptr || !bg3le_game_allocator_ready()) return false;
    const bool maps = copy_map(to->Functors, from->Functors) && copy_map(to->RollConditions, from->RollConditions);
    assign_container(&to->Requirements, from->Requirements);
    assign_container(&to->ComboProperties, from->ComboProperties);
    assign_container(&to->ComboCategories, from->ComboCategories);
    return maps;
}

// Object::ComboProperties (which 0) and ComboCategories (which 1), which
// upstream exposes as plain properties.
namespace {
TrackedCompactSet<FixedString>* combo_set(void const* object, int which) {
    auto* obj = static_cast<Object*>(const_cast<void*>(object));
    if (obj == nullptr) return nullptr;
    return which == 0 ? &obj->ComboProperties : &obj->ComboCategories;
}
}  // namespace

extern "C" int bg3le_stats_combo_get(void const* object, int which,
                                     void (*each)(void* user, char const* name), void* user) {
    auto* set = combo_set(object, which);
    if (set == nullptr) return -1;
    for (auto const& name : *set) each(user, name.GetString());
    return (int)set->size();
}

// A fresh set replaces the old one, which is left, not freed.
extern "C" bool bg3le_stats_combo_set(void* object, int which, char const* const* names, int count) {
    auto* set = combo_set(object, which);
    if (set == nullptr || !bg3le_game_allocator_ready()) return false;
    alignas(TrackedCompactSet<FixedString>) unsigned char raw[sizeof(TrackedCompactSet<FixedString>)];
    auto* fresh = new (raw) TrackedCompactSet<FixedString>();
    for (int i = 0; i < count; ++i) fresh->push_back(FixedString(names[i]));
    std::memcpy((void*)set, raw, sizeof(raw));
    return true;
}

// RPGStats::ExtraData, which upstream exposes as a live map.
namespace {
// Whether a map's keys hold both wanted strings.
bool map_has_keys(void const* at, std::uint32_t a, std::uint32_t b) {
    bg3le::RawMap m{};
    if (!bg3le::safe_read(at, &m, sizeof(m)) || m.Keys == nullptr
        || m.KeysSize < 32 || m.KeysSize > 8192 || m.KeysSize > m.KeysCapacity) {
        return false;
    }
    std::vector<std::uint32_t> keys(m.KeysSize);
    if (!bg3le::safe_read(m.Keys, keys.data(), keys.size() * sizeof(std::uint32_t))) return false;
    bool hasA = false, hasB = false;
    for (auto k : keys) {
        hasA = hasA || k == a;
        hasB = hasB || k == b;
    }
    return hasA && hasB;
}

// The vendored RPGStats drifts from the engine's before ExtraData, so the
// pointer is found by what its map holds.
HashMap<FixedString, float>* extra_data() {
    static std::ptrdiff_t offset = -1;
    auto* rpg = static_cast<char*>(bg3le_rpgstats());
    if (rpg == nullptr) return nullptr;
    if (offset < 0) {
        const FixedString a("DefaultDC"), b("LethalHP");
        for (std::ptrdiff_t off = offsetof(RPGStats, StatsFunctors); off < 0x2000 && offset < 0; off += 8) {
            void* candidate = nullptr;
            if (bg3le::safe_read(rpg + off, &candidate, sizeof(candidate)) && candidate != nullptr
                && map_has_keys(candidate, a.Index, b.Index)) {
                offset = off;
                bg3le::logf("stats: RPGStats::ExtraData at +%#tx", off);
            }
        }
        if (offset < 0) return nullptr;
    }
    void* map = nullptr;
    if (!bg3le::safe_read(rpg + offset, &map, sizeof(map))) return nullptr;
    return static_cast<HashMap<FixedString, float>*>(map);
}
}  // namespace

extern "C" bool bg3le_stats_extra_get(char const* name, float* out) {
    auto* map = extra_data();
    if (map == nullptr || name == nullptr) return false;
    auto* value = map->try_get(FixedString(name));
    if (value == nullptr) return false;
    *out = *value;
    return true;
}

// An existing key is written in place; a new one goes in through fresh buffers.
extern "C" bool bg3le_stats_extra_set(char const* name, float value) {
    auto* map = extra_data();
    if (map == nullptr || name == nullptr) return false;
    const FixedString key(name);
    if (auto* slot = map->try_get(key)) {
        *slot = value;
        return true;
    }
    return bg3le::fs_map_insert<float>(map, key.Index, value);
}

extern "C" void bg3le_stats_extra_each(void (*each)(void* user, char const* name, float value), void* user) {
    auto* map = extra_data();
    if (map == nullptr) return;
    for (auto it = map->begin(); it != map->end(); ++it) {
        each(user, it.Key().GetString(), it.Value());
    }
}
