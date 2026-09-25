// Three of the objects upstream's Ext.UI getters return. None has a symbol here:
// - ecl::gCursorControl: the global pointing at an object whose vtable is the
//   one with the "CursorControl" name function (image+0x79ec2a0), whose
//   destructor frees CursorName (+0x1c) and CursorOverrides (+0x30).
// - ls::gDragDropManager: the global pointing at an object holding the client
//   world just after its PlayerData map.
// - the picking helpers: ecl::PickingHelperManager's PlayerHelpers.
// Layouts are bg3se's (by Norbyte and the bg3se contributors) -- thank you.

#include <stdafx.h>

#include <Lua/Shared/Proxies/PropertyMapDependencies.h>

#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

#include "../log.h"
#include "../mem.h"

namespace bg3le {
std::uintptr_t load_bias();
}
extern "C" void* bg3le_entity_world(void* container);

namespace {

constexpr std::uintptr_t kCursorControlVtable = 0x79ec2a0;

// The executable's writable mappings: .data through the anonymous .bss after it.
std::vector<std::pair<std::uintptr_t, std::uintptr_t>> writable_image() {
    std::vector<std::pair<std::uintptr_t, std::uintptr_t>> out;
    std::FILE* maps = std::fopen("/proc/self/maps", "r");
    if (maps == nullptr) return out;
    char line[512];
    bool inImage = false;
    std::uintptr_t lastEnd = 0;
    while (std::fgets(line, sizeof(line), maps) != nullptr) {
        unsigned long long from = 0, to = 0;
        char perms[8] = {};
        int pathAt = 0;
        // The path may hold spaces (Baldurs Gate 3), so it is tested whole.
        if (std::sscanf(line, "%llx-%llx %7s %*s %*s %*s %n", &from, &to, perms, &pathAt) < 3) continue;
        const char* path = line + pathAt;
        const bool isImage = std::strstr(path, "/bin/bg3") != nullptr;
        const bool anonymousAfter = inImage && from == lastEnd && (*path == '\n' || *path == '\0');
        if ((isImage || anonymousAfter) && perms[1] == 'w') out.emplace_back(from, to);
        inImage = isImage || anonymousAfter;
        lastEnd = to;
    }
    std::fclose(maps);
    return out;
}

// The first global whose object passes the test; the global's address.
template <class Test>
void** scan_globals(Test test) {
    for (auto const& range : writable_image()) {
        std::vector<std::uintptr_t> words((range.second - range.first) / 8);
        const std::size_t got = bg3le::safe_read_some((void const*)range.first, words.data(), words.size() * 8) / 8;
        for (std::size_t i = 0; i < got; ++i) {
            if (words[i] < 0x10000 || (words[i] & 7) != 0) continue;
            if (test((char const*)words[i])) return (void**)(range.first + i * 8);
        }
    }
    return nullptr;
}

template <class T>
T* cached_global(void**& slot, bool& searched, char const* what, bool (*test)(char const*)) {
    static std::mutex lock;
    std::lock_guard<std::mutex> held(lock);
    if (!searched) {
        searched = true;
        slot = scan_globals(test);
        if (slot != nullptr) {
            bg3le::logf("ui: %s global at image+%#lx", what,
                        (unsigned long)((std::uintptr_t)slot - bg3le::load_bias()));
        } else {
            bg3le::logf("ui: no %s global found", what);
        }
    }
    void* object = nullptr;
    if (slot == nullptr || !bg3le::safe_read(slot, &object, sizeof(object))) return nullptr;
    return static_cast<T*>(object);
}

void* g_client_world = nullptr;

bool small_refmap(char const* at) {
    struct { std::uint32_t ItemCount, HashSize; void* Table; } m{};
    return bg3le::safe_read(at, &m, sizeof(m)) && m.ItemCount <= 16 && m.HashSize > 0
           && m.HashSize <= 1024 && m.Table != nullptr;
}

bool is_cursor_control(char const* object) {
    std::uintptr_t vt = 0;
    return bg3le::safe_read(object, &vt, sizeof(vt)) && vt == bg3le::load_bias() + kCursorControlVtable;
}

bool is_drag_drop(char const* object) {
    using M = bg3se::ecl::DragDropManager;
    void* world = nullptr;
    return g_client_world != nullptr
           && bg3le::safe_read(object + offsetof(M, EntityWorld), &world, sizeof(world))
           && world == g_client_world && small_refmap(object + offsetof(M, PlayerData));
}

}  // namespace

extern "C" void* bg3le_cursor_control() {
    static void** slot = nullptr;
    static bool searched = false;
    return cached_global<void>(slot, searched, "ecl::CursorControl", &is_cursor_control);
}

// Upstream's GetDragDrop: the player's entry in ls::gDragDropManager's map.
extern "C" void* bg3le_drag_drop(void* container, std::uint16_t playerId) {
    static void** slot = nullptr;
    static bool searched = false;
    if (!searched) g_client_world = bg3le_entity_world(container);
    auto* manager = cached_global<bg3se::ecl::DragDropManager>(slot, searched, "ls::DragDropManager", &is_drag_drop);
    if (manager == nullptr || !small_refmap((char const*)&manager->PlayerData)) return nullptr;
    return manager->PlayerData.try_get(playerId);
}

// Upstream's GetPickingHelper: PickingHelperManager's entry for the player.
extern "C" void* bg3le_picking_helper(void* system, std::uint16_t playerIndex) {
    if (system == nullptr) return nullptr;
    auto* manager = static_cast<bg3se::ecl::PickingHelperManager*>(system);
    if (!small_refmap((char const*)&manager->PlayerHelpers)) return nullptr;
    auto* found = manager->PlayerHelpers.try_get(playerIndex);
    return found != nullptr ? *found : nullptr;
}
