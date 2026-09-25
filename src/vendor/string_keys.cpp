// ls::TranslatedStringKeyManager, which Ext.Loca's *TranslatedStringKey
// functions read and write: Keys maps a string key to its TranslatedString.
// Upstream's Localization.inl, by Norbyte and the bg3se contributors, adapted.
//
// The manager global has no symbol. It was found by upstream's own anchor --
// the "OnMap" string next to `cmp [reg+..], 2; mov reg, [manager]` -- through
// tools/relocs-xref.py, and is checked before use by that instruction's
// displacement. The same code buckets a key by its string index modulo the
// table size, which is the hash an insert has to use.

#include <stdafx.h>

#include <GameDefinitions/TranslatedString.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "../log.h"
#include "../mem.h"

namespace bg3le {
std::uintptr_t load_bias();
}

extern "C" char const* bg3le_fixed_string(std::uint32_t index, std::uint32_t* length);
extern "C" bool bg3le_fixed_string_index_of(char const* wanted, std::uint32_t* out);
extern "C" bool bg3le_fixed_string_intern(char const* text, std::uint32_t* out);
extern "C" bool bg3le_game_allocator_ready();

namespace bg3le {
namespace {

using Manager = bg3se::TranslatedStringKeyManager;
using EngineNode = bg3se::MapNode<bg3se::FixedString, bg3se::TranslatedString>;

// A node as bytes: copying or building the real one would reference-count
// its strings through engine calls bg3le does not have.
struct Node {
    Node* Next;
    std::uint32_t Key;
    unsigned char Value[16];
};
static_assert(offsetof(EngineNode, Key) == offsetof(Node, Key));
static_assert(offsetof(EngineNode, Value) == offsetof(Node, Value));
static_assert(sizeof(EngineNode) == sizeof(Node));

constexpr std::uintptr_t kManagerGlobal = 0x7d9d238;
// `mov rsi, [rip+disp]` loading it, in the "OnMap" function.
constexpr std::uintptr_t kManagerLoad = 0x2f9baf2;
constexpr unsigned char kManagerLoadBytes[] = {0x48, 0x8b, 0x35};

constexpr std::uint32_t kNull = 0xffffffffu;
constexpr std::uint32_t kMaxBuckets = 1u << 24;

struct KeysMap {
    std::uint32_t HashSize;
    Node** HashTable;
    std::uint32_t ItemCount;
};
static_assert(sizeof(bg3se::TranslatedString) == 16);
static_assert(offsetof(Manager, Keys) == 0x18);

KeysMap* keys() {
    const std::uintptr_t bias = load_bias();
    unsigned char code[7] = {};
    if (!safe_read((void const*)(bias + kManagerLoad), code, sizeof(code))
        || std::memcmp(code, kManagerLoadBytes, sizeof(kManagerLoadBytes)) != 0) {
        return nullptr;
    }
    std::int32_t disp = 0;
    std::memcpy(&disp, code + 3, sizeof(disp));
    if (kManagerLoad + sizeof(code) + disp != kManagerGlobal) return nullptr;

    Manager* manager = nullptr;
    if (!safe_read((void const*)(bias + kManagerGlobal), &manager, sizeof(manager))
        || manager == nullptr) {
        return nullptr;
    }
    auto* map = reinterpret_cast<KeysMap*>((char*)manager + offsetof(Manager, Keys));
    KeysMap probe{};
    if (!safe_read(map, &probe, sizeof(probe)) || probe.HashSize == 0
        || probe.HashSize > kMaxBuckets || probe.HashTable == nullptr) {
        return nullptr;
    }
    return map;
}

// Every node, walked with checked reads.
template <class F>
void each_node(KeysMap* map, F&& fn) {
    std::vector<Node*> buckets(map->HashSize);
    if (!safe_read(map->HashTable, buckets.data(), buckets.size() * sizeof(Node*))) return;
    for (Node* node : buckets) {
        for (std::uint32_t guard = 0; node != nullptr && guard < (1u << 20); ++guard) {
            Node copy;
            if (!safe_read(node, &copy, sizeof(copy))) break;
            if (!fn(node, copy)) return;
            node = copy.Next;
        }
    }
}

Node* find(KeysMap* map, std::uint32_t key) {
    Node* head = nullptr;
    if (!safe_read(map->HashTable + key % map->HashSize, &head, sizeof(head))) return nullptr;
    for (std::uint32_t guard = 0; head != nullptr && guard < (1u << 20); ++guard) {
        Node copy;
        if (!safe_read(head, &copy, sizeof(copy))) return nullptr;
        if (copy.Key == key) return head;
        head = copy.Next;
    }
    return nullptr;
}

}  // namespace
}  // namespace bg3le

// How many keys there are, and each key's text and value address in turn.
extern "C" std::size_t bg3le_string_keys(void (*each)(void* context, char const* key,
                                                      void* value),
                                         void* context) {
    auto* map = bg3le::keys();
    if (map == nullptr) return 0;
    std::size_t n = 0;
    bg3le::each_node(map, [&](bg3le::Node* node, bg3le::Node const& copy) {
        char const* text = bg3le_fixed_string(copy.Key, nullptr);
        if (text != nullptr) {
            ++n;
            if (each != nullptr) each(context, text, node->Value);
        }
        return true;
    });
    return n;
}

// The TranslatedString a key maps to, or nullptr.
extern "C" void* bg3le_string_key_find(char const* key) {
    auto* map = bg3le::keys();
    std::uint32_t id = 0;
    // Interned if new, as upstream's FixedString(key) is.
    if (map == nullptr || key == nullptr
        || (!bg3le_fixed_string_index_of(key, &id) && !bg3le_fixed_string_intern(key, &id))) {
        return nullptr;
    }
    auto* node = bg3le::find(map, id);
    return node != nullptr ? node->Value : nullptr;
}

// Upstream's UpdateTranslatedStringKey: Keys[key] = {Handle = handle}, with
// the default ArgumentString. A new key gets a node from the engine's heap.
extern "C" bool bg3le_string_key_set(char const* key, char const* handle) {
    auto* map = bg3le::keys();
    std::uint32_t keyId = 0, handleId = 0, unknownId = 0;
    if (map == nullptr || key == nullptr || handle == nullptr
        || !bg3le_game_allocator_ready()
        || (!bg3le_fixed_string_index_of(key, &keyId) && !bg3le_fixed_string_intern(key, &keyId))
        || (!bg3le_fixed_string_index_of(handle, &handleId)
            && !bg3le_fixed_string_intern(handle, &handleId))
        || !bg3le_fixed_string_intern("ls::TranslatedStringRepository::s_HandleUnknown",
                                      &unknownId)) {
        return false;
    }

    // Written raw: FixedString's own assignment would reference-count
    // through engine calls bg3le does not have.
    struct Raw {
        std::uint32_t Handle;
        std::uint16_t Version;
        std::uint16_t Pad;
        std::uint32_t ArgHandle;
        std::uint16_t ArgVersion;
        std::uint16_t ArgPad;
    } value{handleId, 0, 0, unknownId, 0, 0};
    static_assert(sizeof(Raw) == sizeof(bg3se::TranslatedString));

    if (auto* node = bg3le::find(map, keyId)) {
        std::memcpy(node->Value, &value, sizeof(value));
        return true;
    }

    auto* node = (bg3le::Node*)bg3se::GameAllocRaw(sizeof(bg3le::Node));
    if (node == nullptr) return false;
    std::memset(node, 0, sizeof(*node));
    const std::uint32_t slot = keyId % map->HashSize;
    node->Next = map->HashTable[slot];
    node->Key = keyId;
    std::memcpy(node->Value, &value, sizeof(value));
    map->HashTable[slot] = node;
    ++map->ItemCount;
    return true;
}
