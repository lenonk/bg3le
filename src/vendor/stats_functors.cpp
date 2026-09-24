// Functors on a stat: SpellProperties, SpellSuccess, OnApplyFunctors and the
// rest.
//
// A functor attribute is not a value in a pool. Object::Functors is a
// HashMap from the attribute name to an Array<FunctorGroup>, each group a
// text key ("Default", "CastOffhand") and a Functors container holding
// polymorphic Functor pointers. Upstream hands those to Lua through its
// property maps, and bg3le already compiles the same property maps in for
// components -- all 63 functor types are in there -- so this file only has
// to reach the objects and name their classes. The fields come out through
// the reflective reader the prelude already uses for resources.
//
// Offsets come from the property maps rather than from arithmetic here.
// That is worth trusting for these types: Object's members predict
// ModifierListIndex at +228, which is exactly where the independent
// derivation from value counts puts it.

#include <stdafx.h>
#include <variant>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <GameDefinitions/Stats/Expression.h>

#include "ls_string.h"

#include "../log.h"
#include "../mem.h"

namespace bg3le {

extern "C" char const* bg3le_fixed_string(std::uint32_t index,
                                          std::uint32_t* length);
extern "C" void const* bg3le_meta_class(char const* className);
extern "C" bool bg3le_meta_field(void const* handle, char const* name,
                                 std::uint32_t* offset, std::uint16_t* size,
                                 std::uint8_t* kind, std::uint8_t* elemKind,
                                 std::uint16_t* elemCount);
extern "C" bool bg3le_meta_enum_label(void const* handle, char const* path,
                                      std::size_t index, char const** label,
                                      std::uint64_t* value, bool* isBitmask);
extern "C" std::size_t bg3le_stats_list_index_offset();

namespace {

template <class T>
bool read_as(void const* addr, T* out) {
    return safe_read(addr, out, sizeof(T));
}

// A field's offset, from the compiled property map.
bool field_offset(char const* className, char const* field,
                  std::size_t* out) {
    void const* meta = bg3le_meta_class(className);
    if (meta == nullptr) return false;

    std::uint32_t offset = 0;
    std::uint16_t size = 0;
    std::uint8_t kind = 0;
    std::uint8_t elemKind = 0;
    std::uint16_t elemCount = 0;
    if (!bg3le_meta_field(meta, field, &offset, &size, &kind, &elemKind,
                          &elemCount)) {
        return false;
    }
    *out = offset;
    return true;
}

// Object::Functors and FunctorGroup::Functors are not in the property maps
// -- upstream reaches them through free getters, and the generator only
// emits plain fields -- so those two come from the member walk in
// stats.cpp. Functor::TypeId is a plain field and comes from the maps.
//
// The walk is not taken on trust. stats::Object does expose
// ModifierListIndex, and the stats search derives that same offset
// independently from the value counts; ready() checks they agree before
// using any of it. AIFlags and RollConditions, found by the same walk,
// already match the captured reference exactly.
constexpr std::size_t kObjectFunctors = 40;
constexpr std::size_t kGroupTextKey = 0;
constexpr std::size_t kGroupFunctors = 8;
constexpr std::size_t kGroupStride = 16;

struct Layout {
    bool Ready{false};
    std::size_t FunctorTypeId{0};     // Functor::TypeId, from the maps
};

Layout& layout() {
    static Layout l;
    return l;
}

bool ready() {
    Layout& l = layout();
    if (l.Ready) return true;

    static bool tried = false;
    if (tried) return false;
    tried = true;

    if (!field_offset("stats::Functor", "TypeId", &l.FunctorTypeId)) {
        logf("functors: the property maps do not describe Functor::TypeId; "
             "functor attributes stay unavailable");
        return false;
    }

    std::size_t metaListIndex = 0;
    if (!field_offset("stats::Object", "ModifierListIndex", &metaListIndex)) {
        logf("functors: cannot cross-check the Object walk; functor "
             "attributes stay unavailable");
        return false;
    }

    const std::size_t derived = bg3le_stats_list_index_offset();
    if (derived == 0 || derived != metaListIndex) {
        logf("functors: Object walk disagrees -- ModifierListIndex is +%zu "
             "by the property maps and +%zu by the stats search; functor "
             "attributes stay unavailable", metaListIndex, derived);
        return false;
    }

    l.Ready = true;
    logf("functors: Object walk agrees on ModifierListIndex at +%zu; "
         "Functors at +%zu, Functor::TypeId at +%zu", derived,
         kObjectFunctors, l.FunctorTypeId);
    return true;
}

// The FunctorId label for a type id, from bg3se's generated enumeration.
char const* functor_label(std::uint8_t typeId) {
    void const* meta = bg3le_meta_class("stats::Functor");
    if (meta == nullptr) return nullptr;

    for (std::size_t i = 0;; ++i) {
        char const* label = nullptr;
        std::uint64_t value = 0;
        bool isBitmask = false;
        if (!bg3le_meta_enum_label(meta, "TypeId", i, &label, &value,
                                   &isBitmask)) {
            return nullptr;
        }
        if (label != nullptr && value == typeId) return label;
    }
}

// A bg3se HashMap keyed by FixedString: Keys and Values are parallel, so a
// linear walk finds the slot without reimplementing the bucket walk.
int map_slot(void const* map, char const* name) {
    constexpr std::size_t kKeysBuffer = 32;
    constexpr std::size_t kKeysSize = 44;

    void const* keys = nullptr;
    std::uint32_t count = 0;
    if (name == nullptr || !read_as((char const*)map + kKeysBuffer, &keys)
        || !read_as((char const*)map + kKeysSize, &count)) {
        return -1;
    }
    if (count == 0 || count > 4096 || keys == nullptr) return -1;

    for (std::uint32_t i = 0; i < count; ++i) {
        std::uint32_t key = 0;
        if (!read_as((char const*)keys + i * sizeof(std::uint32_t), &key)) {
            return -1;
        }
        char const* text = bg3le_fixed_string(key, nullptr);
        if (text != nullptr && std::strcmp(text, name) == 0) return (int)i;
    }
    return -1;
}

// The Array<FunctorGroup> an attribute maps to.
bool groups_for(void const* object, char const* attribute,
                void const** bufferOut, std::uint32_t* countOut) {
    if (object == nullptr || !ready()) return false;

    auto const* map = (char const*)object + kObjectFunctors;
    const int slot = map_slot(map, attribute);
    if (slot < 0) return false;

    constexpr std::size_t kValuesBuffer = 48;
    void const* values = nullptr;
    if (!read_as(map + kValuesBuffer, &values) || values == nullptr) {
        return false;
    }

    // Values[slot] is an Array<FunctorGroup>: buffer, capacity, size.
    auto const* entry = (char const*)values + (std::size_t)slot * 16;
    void const* buffer = nullptr;
    std::uint32_t size = 0;
    if (!read_as(entry, &buffer) || !read_as(entry + 12, &size)) return false;
    if (size == 0 || size > 4096 || buffer == nullptr) return false;

    *bufferOut = buffer;
    *countOut = size;
    return true;
}

}  // namespace

// How many functor groups an attribute carries; -1 when it carries none,
// which is not the same as zero.
extern "C" int bg3le_stats_functor_groups(void const* object,
                                          char const* attribute) {
    void const* buffer = nullptr;
    std::uint32_t count = 0;
    if (!groups_for(object, attribute, &buffer, &count)) return -1;
    return (int)count;
}

// One group's text key and its Functors container.
extern "C" bool bg3le_stats_functor_group_at(void const* object,
                                             char const* attribute,
                                             int index,
                                             char const** textKeyOut,
                                             void** functorsOut) {
    void const* buffer = nullptr;
    std::uint32_t count = 0;
    if (!groups_for(object, attribute, &buffer, &count)) return false;
    if (index < 0 || (std::uint32_t)index >= count) return false;

    auto const* group = (char const*)buffer + (std::size_t)index * kGroupStride;

    std::uint32_t key = 0;
    void* functors = nullptr;
    if (!read_as(group + kGroupTextKey, &key)
        || !read_as(group + kGroupFunctors, &functors)) {
        return false;
    }

    if (textKeyOut != nullptr) {
        char const* text = bg3le_fixed_string(key, nullptr);
        *textKeyOut = text != nullptr ? text : "";
    }
    if (functorsOut != nullptr) *functorsOut = functors;
    return true;
}

// The Array<Functor*> inside a Functors container.
//
// bg3se declares Functors as CNamedElementManager<Functor>, whose first
// member is Array<Functor*> Values, so the list should be at the front.
// It is not: the engine's class has a vtable there, and reading the array
// from offset zero picked up the vtable pointer as the buffer and the top
// half of the real pointer as a count of 809, which produced 809 garbage
// functors with type ids like 122 and 204.
//
// So the offset is derived rather than taken from either side, and the test
// is bg3se's own FunctorId list: at the right offset the first entry is a
// functor whose TypeId is one of the 63 labels. Nothing else lines up that
// way by accident.
std::size_t values_offset(void const* functors);

bool functor_values(void const* functors, void const** bufferOut,
                    std::uint32_t* countOut) {
    if (functors == nullptr || !ready()) return false;
    const std::size_t at = values_offset(functors);
    if (at == (std::size_t)-1) return false;

    auto const* array = (char const*)functors + at;
    void const* buffer = nullptr;
    std::uint32_t capacity = 0;
    std::uint32_t size = 0;
    if (!read_as(array, &buffer) || !read_as(array + 8, &capacity)
        || !read_as(array + 12, &size)) {
        return false;
    }
    if (buffer == nullptr || size == 0 || size > capacity
        || capacity > 4096) {
        return false;
    }

    *bufferOut = buffer;
    *countOut = size;
    return true;
}

// Whether the TypeId of the entry at this array head names a functor.
bool head_is_functor(void const* functors, std::size_t at) {
    auto const* array = (char const*)functors + at;
    void const* buffer = nullptr;
    std::uint32_t capacity = 0;
    std::uint32_t size = 0;
    if (!read_as(array, &buffer) || !read_as(array + 8, &capacity)
        || !read_as(array + 12, &size)) {
        return false;
    }
    if (buffer == nullptr || size == 0 || size > capacity
        || capacity > 4096) {
        return false;
    }

    void const* first = nullptr;
    if (!read_as(buffer, &first) || first == nullptr) return false;

    std::uint8_t typeId = 0;
    if (!read_as((char const*)first + layout().FunctorTypeId, &typeId)) {
        return false;
    }
    return functor_label(typeId) != nullptr;
}

std::size_t values_offset(void const* functors) {
    static std::size_t cached = (std::size_t)-1;
    if (cached != (std::size_t)-1) return cached;

    // Zero is what the vendored struct says; eight is the same thing behind
    // a vtable. Both are tried, and only a resolvable functor decides it.
    for (std::size_t at : {(std::size_t)8, (std::size_t)0}) {
        if (!head_is_functor(functors, at)) continue;
        cached = at;
        logf("functors: Functors::Values at +%zu", at);
        return at;
    }
    logf("functors: no readable functor list in the container; functor "
         "attributes stay unavailable");
    return (std::size_t)-1;
}

// The raw bytes of an expression's Params buffer, so the engine's element
// stride and where it keeps the variant index can be read off rather than
// inferred from what this build happens to compile std::variant to.
extern "C" void bg3le_stats_expression_dump(void const* pooled) {
    std::size_t params = 0;
    if (pooled == nullptr
        || !field_offset("StatsExpressionPooled", "Params", &params)) {
        return;
    }

    void const* buffer = nullptr;
    std::uint32_t capacity = 0;
    std::uint32_t size = 0;
    if (!read_as((char const*)pooled + params, &buffer)
        || !read_as((char const*)pooled + params + 8, &capacity)
        || !read_as((char const*)pooled + params + 12, &size)
        || buffer == nullptr) {
        return;
    }

    logf("expr: Params buffer %p, capacity %u, size %u", buffer, capacity,
         size);

    // What this build compiles the element to. std::variant keeps its
    // discriminant after the union, so a union of a different size puts the
    // index somewhere else -- which is why Params[0] reads as alternative 256
    // of nine. The index offset is found rather than assumed: set the variant
    // to two known alternatives and look for the byte that follows.
    using Param = bg3se::StatsExpressionInternal::Param;
    logf("expr: this build's Param is %zu bytes, aligned %zu, %zu "
         "alternatives", sizeof(Param), alignof(Param),
         std::variant_size_v<Param>);

    Param probe;
    probe.emplace<0>();
    unsigned char first[sizeof(Param)];
    std::memcpy(first, &probe, sizeof(Param));
    probe.emplace<7>();  // int32_t
    unsigned char second[sizeof(Param)];
    std::memcpy(second, &probe, sizeof(Param));

    for (std::size_t at = 0; at < sizeof(Param); ++at) {
        if (first[at] == 0 && second[at] == 7) {
            logf("expr: this build keeps the index at +%zu", at);
            break;
        }
    }
    for (std::size_t off = 0; off < 128; off += 8) {
        std::uint64_t word = 0;
        if (!read_as((char const*)buffer + off, &word)) break;
        auto const* b = (unsigned char const*)&word;
        logf("expr:   +%3zu %016llx  bytes %3u %3u %3u %3u %3u %3u %3u %3u",
             off, (unsigned long long)word, b[0], b[1], b[2], b[3], b[4],
             b[5], b[6], b[7]);
    }
}

extern "C" int bg3le_stats_functor_count(void const* functors) {
    void const* buffer = nullptr;
    std::uint32_t count = 0;
    if (!functor_values(functors, &buffer, &count)) return -1;
    return (int)count;
}

extern "C" void* bg3le_stats_functor_at(void const* functors, int index) {
    void const* buffer = nullptr;
    std::uint32_t count = 0;
    if (!functor_values(functors, &buffer, &count)) return nullptr;
    if (index < 0 || (std::uint32_t)index >= count) return nullptr;

    void* functor = nullptr;
    if (!read_as((char const*)buffer + (std::size_t)index * sizeof(void*),
                 &functor)) {
        return nullptr;
    }
    return functor;
}

// The concrete class of a functor, for the reflective reader to walk.
//
// Functor::TypeId is a FunctorId, and every one of the 63 labels names a
// class as "stats::" + label + "Functor" -- checked against the generated
// property map names, where all 63 are present.
extern "C" char const* bg3le_stats_functor_class(void const* functor) {
    if (functor == nullptr || !ready()) return nullptr;

    std::uint8_t typeId = 0;
    if (!read_as((char const*)functor + layout().FunctorTypeId, &typeId)) {
        return nullptr;
    }

    char const* label = functor_label(typeId);
    if (label == nullptr) {
        logf("functors: no FunctorId label for type %u", (unsigned)typeId);
        return nullptr;
    }

    static thread_local std::string name;
    name = "stats::";
    name += label;
    name += "Functor";
    // Only if the property maps describe it; otherwise the caller would
    // walk fields that do not exist.
    return bg3le_meta_class(name.c_str()) != nullptr ? name.c_str()
                                                     : nullptr;
}


// StatsExpressionRef, which the property maps mark unsupported: a pointer to
// a pooled expression whose Code and RefCount are reachable once Params'
// offset is known -- Params is a plain field, so the maps do give that one.

// The pooled expression a StatsExpressionRef field points at.
extern "C" void* bg3le_stats_object_expression(void const* object,
                                               char const* className,
                                               char const* field) {
    std::size_t at = 0;
    if (object == nullptr || !field_offset(className, field, &at)) {
        return nullptr;
    }
    void* pooled = nullptr;
    if (!read_as((char const*)object + at, &pooled)) return nullptr;
    return pooled;
}

// StatsExpressionPooled is { Array<Param> Params; STDString Code; } with an
// atomic refcount after them, so Code follows Params and RefCount follows
// Code. Params' offset comes from the maps; the other two are its
// neighbours.
extern "C" char const* bg3le_stats_expression_code(void const* pooled) {
    std::size_t params = 0;
    if (pooled == nullptr
        || !field_offset("StatsExpressionPooled", "Params", &params)) {
        return nullptr;
    }

    static thread_local std::string code;
    if (!read_ls_string((char const*)pooled + params + 16, &code)) {
        return nullptr;
    }
    return code.c_str();
}

extern "C" bool bg3le_stats_expression_refcount(void const* pooled,
                                                int* out) {
    std::size_t params = 0;
    if (pooled == nullptr
        || !field_offset("StatsExpressionPooled", "Params", &params)) {
        return false;
    }
    std::int32_t count = 0;
    if (!read_as((char const*)pooled + params + 32, &count)) return false;
    if (out != nullptr) *out = count;
    return true;
}

}  // namespace bg3le
