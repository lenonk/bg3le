// Ext.Level's functions that go through a level manager, as upstream's
// Lua/Libs/Level.inl writes them: the level data, persistent level templates,
// and the physics scene's queries.
//
// The level, physics and hit types are bg3se's (by Norbyte and the bg3se
// contributors); the server's manager is found in templates.cpp.

#include <stdafx.h>

#include <cstdint>
#include <cstring>

#include <GameDefinitions/Level.h>
#include <GameDefinitions/RootTemplates.h>
#include <GameDefinitions/Physics.h>

#include "../hook.h"
#include "../log.h"
#include "../mem.h"

extern "C" void* bg3le_server_level_manager();
extern "C" bool bg3le_game_allocator_ready();
extern "C" bool bg3le_fixed_string_index_of(char const* wanted, std::uint32_t* out);

namespace {

using LevelManager = bg3se::esv::LevelManager;

// A readable object whose first word points into the executable.
bool has_vtable(void const* obj) {
    std::uint64_t vmt = 0;
    return obj != nullptr && bg3le::safe_read(obj, &vmt, sizeof(vmt)) && vmt > 0x10000;
}

}  // namespace

// The LevelDataManager behind the level manager's local templates, or null.
extern "C" void* bg3le_level_data_manager() {
    auto* mgr = static_cast<LevelManager*>(bg3le_server_level_manager());
    if (mgr == nullptr) return nullptr;
    bg3se::LocalTemplateManager* local = nullptr;
    if (!bg3le::safe_read(&mgr->LocalTemplateManager, &local, sizeof(local))
        || local == nullptr) {
        return nullptr;
    }
    bg3se::LevelDataManager* data = nullptr;
    if (!bg3le::safe_read(&local->LevelDataManager, &data, sizeof(data)) || !has_vtable(data)) {
        return nullptr;
    }
    return data;
}

// Upstream's AddActivePersistentLevelTemplate: the new count, or 0 with why.
// A full set is rebuilt into a fresh buffer and the engine's left alone.
extern "C" std::uint32_t bg3le_level_add_persistent_template(char const* parent,
                                                              char const* subLevel,
                                                              char const* instance,
                                                              char const** why) {
    auto* mgr = static_cast<LevelManager*>(bg3le_server_level_manager());
    if (mgr == nullptr) {
        *why = "the server level manager is not up";
        return 0;
    }
    std::uint32_t key = 0;
    bg3se::EoCLevel* level = nullptr;
    if (parent != nullptr && bg3le_fixed_string_index_of(parent, &key)) {
        for (auto it = mgr->Levels.begin(); it != mgr->Levels.end(); ++it) {
            if (it.Key().Index == key) {
                level = it.Value();
                break;
            }
        }
    }
    if (level == nullptr) {
        *why = "no parent level by that name";
        return 0;
    }

    auto& set = level->ActiveLevelTemplates;
    const bg3se::ActivePersistentLevelTemplate entry{
        .SubLevelName = bg3se::FixedString(subLevel),
        .LevelInstanceID = bg3se::FixedString(instance),
    };
    if (set.Size < set.Capacity) {
        set.Buf[set.Size] = entry;
        set.Size++;
        return set.Size;
    }
    if (!bg3le_game_allocator_ready()) {
        *why = "the engine allocator is not up";
        return 0;
    }
    using Set = std::remove_reference_t<decltype(set)>;
    alignas(Set) unsigned char raw[sizeof(Set)];
    auto* fresh = new (raw) Set();
    const auto capacity = set.CapacityIncrement();
    fresh->Buf = fresh->RawReallocate(capacity);
    for (std::uint32_t i = 0; i < capacity; ++i) {
        new (fresh->Buf + i) bg3se::ActivePersistentLevelTemplate(
            i < set.Size ? set.Buf[i] : i == set.Size ? entry : bg3se::ActivePersistentLevelTemplate{});
    }
    fresh->Size = set.Size + 1;
    std::memcpy((void*)&set, raw, sizeof(Set));
    return fresh->Size;
}

// ---- the physics scene ----

namespace {

// ecl::LevelManager's global, found beside the GlobalTemplateManager in the
// client's template lookups, and checked by its level pointing back at it.
constexpr std::uintptr_t kRecordedClientLevelManager = 0x7bfb820;

void* client_level_manager() {
    void* mgr = nullptr;
    auto const* slot = (void const*)(bg3le::load_bias() + kRecordedClientLevelManager);
    if (!bg3le::safe_read(slot, &mgr, sizeof(mgr)) || !has_vtable(mgr)) return nullptr;
    auto* m = static_cast<bg3se::LevelManager*>(mgr);
    bg3se::EoCLevel* level = nullptr;
    bg3se::LevelManager* owner = nullptr;
    if (!bg3le::safe_read(&m->CurrentLevel, &level, sizeof(level)) || level == nullptr
        || !bg3le::safe_read(&level->LevelManager, &owner, sizeof(owner)) || owner != m) {
        return nullptr;
    }
    return mgr;
}

// The virtuals are called as bg3se declares them, which under this ABI puts
// both cylinder sweeps at slots 14 and 18: `xor eax, eax; ret` on this build.
// Anything else is not the class that declaration describes.
bool scene_checks_out(void const* scene) {
    std::uint64_t vmt = 0, slots[19] = {};
    if (!bg3le::safe_read(scene, &vmt, sizeof(vmt))
        || !bg3le::safe_read((void const*)vmt, slots, sizeof(slots))) {
        return false;
    }
    for (int slot : {14, 18}) {
        unsigned char code[3] = {};
        if (!bg3le::safe_read((void const*)slots[slot], code, sizeof(code))
            || code[0] != 0x31 || code[1] != 0xc0 || code[2] != 0xc3) {
            return false;
        }
    }
    return true;
}

bg3se::phx::PhysicsSceneBase* physics_scene(bool client) {
    auto* mgr = static_cast<bg3se::LevelManager*>(client ? client_level_manager()
                                                         : bg3le_server_level_manager());
    if (mgr == nullptr) return nullptr;
    bg3se::EoCLevel* level = nullptr;
    bg3se::phx::PhysicsSceneBase* scene = nullptr;
    if (!bg3le::safe_read(&mgr->CurrentLevel, &level, sizeof(level)) || level == nullptr
        || !bg3le::safe_read(&level->PhysicsScene, &scene, sizeof(scene)) || scene == nullptr) {
        return nullptr;
    }
    static bool warned = false;
    if (!scene_checks_out(scene)) {
        if (!warned) bg3le::logf("physics: the scene's vtable is not the one bg3se declares");
        warned = true;
        return nullptr;
    }
    return scene;
}

// Upstream's thread_local results. Raw storage, so nothing frees what the
// engine allocated into them; the arrays are cleared, which keeps them.
alignas(bg3se::phx::PhysicsHit) thread_local unsigned char g_hit[sizeof(bg3se::phx::PhysicsHit)];
alignas(bg3se::phx::PhysicsHitAll) thread_local unsigned char g_hits[sizeof(bg3se::phx::PhysicsHitAll)];
thread_local bool g_hits_made = false;

bg3se::phx::PhysicsHit& fresh_hit() {
    return *new (g_hit) bg3se::phx::PhysicsHit{};
}

bg3se::phx::PhysicsHitAll& fresh_hits() {
    auto* hits = reinterpret_cast<bg3se::phx::PhysicsHitAll*>(g_hits);
    if (!g_hits_made) {
        new (g_hits) bg3se::phx::PhysicsHitAll{};
        g_hits_made = true;
    }
    hits->Normals.clear();
    hits->Positions.clear();
    hits->Distances.clear();
    hits->PhysicsGroup.clear();
    hits->PhysicsExtraFlags.clear();
    hits->Shapes.clear();
    return *hits;
}

// bg3se's TestBoxFunc/TestBox (and the Sphere and Shape pairs) are overloads
// of one name, which MSVC lays out in reverse and this ABI in order: here the
// hit-list overloads are the earlier slots, so those are called by slot.
using TestBoxProc = bool (*)(void const*, glm::vec3 const&, glm::vec3 const&,
                             bg3se::phx::PhysicsHitAll&, bg3se::PhysicsType,
                             bg3se::PhysicsGroupFlags, bg3se::PhysicsGroupFlags);
using TestSphereProc = bool (*)(void const*, glm::vec3 const&, float,
                                bg3se::phx::PhysicsHitAll&, bg3se::PhysicsType,
                                bg3se::PhysicsGroupFlags, bg3se::PhysicsGroupFlags);
constexpr int kTestBoxSlot = 20;
constexpr int kTestSphereSlot = 24;

template <class Proc>
Proc slot_of(void const* scene, int slot) {
    return (*reinterpret_cast<Proc const* const*>(scene))[slot];
}

}  // namespace

// One of upstream's scene queries, by op (see the Lua side for the order).
// -1 when there is no scene; else whether it hit, with *out the hit or hits.
extern "C" int bg3le_physics_query(int op, bool client, float const* v, std::uint32_t type,
                                   std::uint32_t include, std::uint32_t exclude, int context,
                                   void** out) {
    auto* scene = physics_scene(client);
    if (scene == nullptr) return -1;
    using namespace bg3se;
    const glm::vec3 a{v[0], v[1], v[2]}, b{v[3], v[4], v[5]}, extents{v[6], v[7], v[8]};
    const float radius = v[9], halfHeight = v[10];
    const auto t = (PhysicsType)type;
    const auto in = (PhysicsGroupFlags)include, ex = (PhysicsGroupFlags)exclude;
    // An empty Function: the engine skips the filter, as upstream's accepts all.
    alignas(16) unsigned char noFilter[64] = {};
    auto* filter = reinterpret_cast<Function<bool(phx::PhysicsShape const*)>*>(noFilter);
    bool hit = false;
    switch (op) {
    case 0: hit = scene->RaycastClosest(a, b, fresh_hit(), t, in, ex, context, -1, -1, filter); break;
    case 1: scene->RaycastAll(a, b, fresh_hits(), t, in, ex, context, -1, -1, {}); hit = true; break;
    case 2: hit = scene->RaycastAny(a, b, t, in, ex, context, -1, -1, {}); break;
    case 3: hit = scene->SweepSphereClosest(radius, a, b, fresh_hit(), t, in, ex, context, -1, -1); break;
    case 4: hit = scene->SweepCapsuleClosest(radius, halfHeight, a, b, fresh_hit(), t, in, ex, context, -1, -1); break;
    case 5: hit = scene->SweepBoxClosest(extents, a, b, fresh_hit(), t, in, ex, context, -1, -1); break;
    case 6: hit = scene->SweepSphereAll(radius, a, b, fresh_hits(), t, in, ex, context, -1, -1); break;
    case 7: hit = scene->SweepCapsuleAll(radius, halfHeight, a, b, fresh_hits(), t, in, ex, context, -1, -1); break;
    case 8: hit = scene->SweepBoxAll(extents, a, b, fresh_hits(), t, in, ex, context, -1, -1); break;
    // Position first, as the engine reads it; upstream swaps the two, which
    // hands PhysX a box with the position's (often negative) half-extents.
    case 9: hit = slot_of<TestBoxProc>(scene, kTestBoxSlot)(scene, a, extents, fresh_hits(), t, in, ex); break;
    case 10: hit = slot_of<TestSphereProc>(scene, kTestSphereSlot)(scene, a, radius, fresh_hits(), t, in, ex); break;
    default: return -1;
    }
    const bool many = op == 1 || op >= 6;
    *out = many ? (void*)g_hits : (void*)g_hit;
    return hit ? 1 : 0;
}
