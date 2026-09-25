// Ext.Level's functions that go through a level manager, as upstream's
// Lua/Libs/Level.inl writes them: the level data, persistent level templates,
// the physics scene's queries, the AI grid's tiles and paths, and surface
// actions.
//
// The level, physics and hit types are bg3se's (by Norbyte and the bg3se
// contributors); the server's manager is found in templates.cpp.

#include <stdafx.h>

#include <cstdint>
#include <cstring>

#include <GameDefinitions/Level.h>
#include <GameDefinitions/RootTemplates.h>
#include <GameDefinitions/Physics.h>
#include <GameDefinitions/Ai.h>
#include <GameDefinitions/Surface.h>

#include <cmath>

#include "engine_containers.h"
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

// ---- the AI grid ----
//
// Upstream's AiGrid::ToWorldPos, GetSubgridsAt, ToTilePos and GetHeightsAt
// (GameDefinitions/Ai.inl), over the engine's grid.

namespace {

bg3se::AiGrid* ai_grid(bool client) {
    auto* mgr = static_cast<bg3se::LevelManager*>(client ? client_level_manager()
                                                         : bg3le_server_level_manager());
    bg3se::EoCLevel* level = nullptr;
    bg3se::AiGrid* grid = nullptr;
    if (mgr == nullptr || !bg3le::safe_read(&mgr->CurrentLevel, &level, sizeof(level))
        || level == nullptr || !bg3le::safe_read(&level->AiGrid, &grid, sizeof(grid))
        || !has_vtable(grid)) {
        return nullptr;
    }
    return grid;
}

bg3se::AiWorldPos to_world_pos(glm::vec3 pos) {
    if (pos.x > 9999999.0f || pos.x < -9999999.0f || pos.z > 9999999.0f || pos.z < -9999999.0f) {
        return bg3se::AiWorldPos{.Xglobal = 0x7fff, .Zglobal = 0x7fff, .Xlocal = .0f,
                                 .Zlocal = .0f, .Y = 3.40282347e+38f};
    }
    const float xG = floorf(pos.x + pos.x), zG = floorf(pos.z + pos.z);
    return bg3se::AiWorldPos{.Xglobal = (int)xG, .Zglobal = (int)zG,
                             .Xlocal = pos.x - (xG * 0.5f), .Zlocal = pos.z - (zG * 0.5f),
                             .Y = pos.y};
}

bool world_to_tile(bg3se::AiSubgrid const& sg, bg3se::AiWorldPos const& pos, glm::ivec2& local) {
    int gX, gZ;
    if (fabs(pos.Xlocal) < 0.00000011920929f && fabs(pos.Zlocal) < 0.00000011920929f) {
        gX = pos.Xglobal - sg.WorldPos.Xglobal;
        gZ = pos.Zglobal - sg.WorldPos.Zglobal;
    } else {
        auto x = (int)::floor(((pos.Xglobal * sg.CellSize + pos.Xlocal) - sg.WorldPos.Xlocal) / sg.CellSize);
        auto z = (int)::floor(((pos.Zglobal * sg.CellSize + pos.Zlocal) - sg.WorldPos.Zlocal) / sg.CellSize);
        gX = x - sg.WorldPos.Xglobal;
        gZ = z - sg.WorldPos.Zglobal;
    }
    if (gX < 0 || gZ < 0 || gX >= sg.SizeX || gZ >= sg.SizeY) return false;
    local = glm::ivec2(gX, gZ);
    return true;
}

bg3se::AiGridTile const* tile_at(bg3se::AiSubgrid const& sg, glm::ivec2 local) {
    auto const* data = sg.TileGrid;
    if (data == nullptr || local.x >= data->Width || local.y >= data->Height) return nullptr;
    return &data->Tiles[local.x + local.y * data->Width];
}

// Each subgrid overlapping the position, with the tile under it.
template <class F>
void each_tile(bg3se::AiGrid const& grid, bg3se::AiWorldPos const& pos, F&& f) {
    const std::uint64_t key =
        (std::uint32_t)(std::int32_t)::floor(pos.Xglobal / bg3se::AiGrid::PatchSize)
        | ((std::uint64_t)(std::uint32_t)(std::int32_t)::floor(pos.Zglobal / bg3se::AiGrid::PatchSize) << 32);
    auto patch = grid.SubgridsAtPatch.find(key);
    if (patch == grid.SubgridsAtPatch.end()) return;
    for (auto id : patch.Value()) {
        auto* sg = grid.Subgrids.get_or_default(id);
        glm::ivec2 local;
        if (sg == nullptr || !world_to_tile(*sg, pos, local)) continue;
        auto const* tile = tile_at(*sg, local);
        if (tile != nullptr && !tile->Flags.IsBlocker()) f(id, *sg, local, *tile);
    }
}

// Upstream's ToTilePos: the tile whose floor is nearest the position's height.
bool to_tile_pos(bg3se::AiGrid const& grid, bg3se::AiWorldPos const& pos,
                 bg3se::AiSubgrid const*& subgrid, bg3se::AiTilePos& tilePos,
                 bg3se::AiGridTile const*& tileInfo) {
    float ydiff = 3.40282347e+38f;
    each_tile(grid, pos, [&](bg3se::AiSubgridId id, bg3se::AiSubgrid const& sg, glm::ivec2 local,
                             bg3se::AiGridTile const& tile) {
        const float minY = sg.Translate.y + tile.GetLocalMinHeight();
        const float diff = fabs(pos.Y - minY);
        if (diff < ydiff) {
            ydiff = diff;
            tilePos.SubgridId = id;
            tilePos.X = (decltype(tilePos.X))local.x;
            tilePos.Y = (decltype(tilePos.Y))local.y;
            tileInfo = &tile;
            subgrid = &sg;
        }
    });
    return ydiff < 3.40282347e+38f;
}

bg3se::AiMetaData const* meta_of(bg3se::AiGrid const& grid, bg3se::AiGridTile const& tile) {
    if (tile.MetaDataIndex == bg3se::AiNullMetaData || tile.MetaDataIndex >= grid.MetaData.size()) {
        return nullptr;
    }
    return grid.MetaData[tile.MetaDataIndex];
}

bg3se::AiGridLuaTile g_test_tile;

}  // namespace

// Upstream's GetEntitiesOnTile: up to `cap` handles into `out`, and how many
// there are; 0 with no grid, as upstream's empty array.
extern "C" std::size_t bg3le_ai_entities_on_tile(bool client, float const* v,
                                                 std::uint64_t* out, std::size_t cap) {
    auto* grid = ai_grid(client);
    if (grid == nullptr) return 0;
    bg3se::AiSubgrid const* sg = nullptr;
    bg3se::AiTilePos pos{};
    bg3se::AiGridTile const* tile = nullptr;
    if (!to_tile_pos(*grid, to_world_pos({v[0], v[1], v[2]}), sg, pos, tile)) return 0;
    auto const* meta = meta_of(*grid, *tile);
    if (meta == nullptr) return 0;
    const std::size_t n = meta->Entities.size();
    for (std::size_t i = 0; i < n && i < cap; ++i) std::memcpy(&out[i], &meta->Entities[i], 8);
    return n;
}

// Upstream's GetTileDebugInfo: its one shared tile, filled in when the
// position is on a tile, or null with no grid.
extern "C" void* bg3le_ai_tile_info(bool client, float const* v) {
    auto* grid = ai_grid(client);
    if (grid == nullptr) return nullptr;
    bg3se::AiSubgrid const* sg = nullptr;
    bg3se::AiTilePos pos{};
    bg3se::AiGridTile const* tile = nullptr;
    auto& out = g_test_tile;
    if (to_tile_pos(*grid, to_world_pos({v[0], v[1], v[2]}), sg, pos, tile)) {
        out.Flags = tile->Flags.GetFlags();
        out.GroundSurface = tile->Flags.GetGroundSurface();
        out.CloudSurface = tile->Flags.GetCloudSurface();
        out.Material = tile->Flags.GetMaterial();
        out.UnmappedFlags = 0;
        out.ExtraFlags = tile->Flags.GetExtraFlags();
        out.SubgridId = pos.SubgridId;
        out.TileX = pos.X;
        out.TileY = pos.Y;
        out.MinHeight = sg->Translate.y + tile->GetLocalMinHeight();
        out.MaxHeight = sg->Translate.y + tile->GetLocalMaxHeight();
        out.MetaDataIndex = tile->MetaDataIndex;
        out.SurfaceMetaDataIndex = tile->SurfaceMetaDataIndex;
        if (auto const* meta = meta_of(*grid, *tile)) out.Entities = meta->Entities;
    }
    return &out;
}

// Upstream's GetHeightsAt: each walkable tile's top at (x, z).
extern "C" std::size_t bg3le_ai_heights_at(bool client, float x, float z, float* out,
                                           std::size_t cap) {
    auto* grid = ai_grid(client);
    if (grid == nullptr) return 0;
    std::size_t n = 0;
    each_tile(*grid, to_world_pos({x, 0.0f, z}),
              [&](bg3se::AiSubgridId, bg3se::AiSubgrid const& sg, glm::ivec2,
                  bg3se::AiGridTile const& tile) {
                  if (n < cap) out[n] = sg.Translate.y + tile.GetLocalMaxHeight();
                  ++n;
              });
    return n;
}

// ---- paths ----
//
// Upstream's AiGrid::CreatePath, GetPathId and FreePath and AiPath::Reset
// (Ai.inl). The engine runs the search itself for a path in the grid's Paths
// list, which is what an asynchronous request needs.

namespace {

// A Function whose implementation pointer is set, which Reset would have to
// destroy through Larian's own function object; such a path is not taken.
bool has_function(void const* fn) {
    void* impl = nullptr;
    return bg3le::safe_read(fn, &impl, sizeof(impl)) && impl != nullptr;
}

// AiPath::Reset, but for its two Function members: a released path has
// them empty already, and only such a path is taken.
void reset_path(bg3se::AiPath& p) {
    p.SearchStarted = false;
    p.SearchComplete = false;
    p.GoalFound = false;
    p.DestinationReached = false;
    p.CanUseLadders = false;
    p.CanUsePortals = false;
    p.CanUseCombatPortals = false;
    p.UseSmoothing = true;
    p.AddBoundsToMargin = true;
    p.AddSourceBoundsToMargin = true;
    p.StepHeight = 0;
    p.WorldClimbingHeight = 0;
    p.Nodes.clear();
    p.Checkpoints.clear();
    p.Source = bg3se::EntityHandle{};
    p.Target = bg3se::EntityHandle{};
    p.IgnoreEntities.clear();
    p.MovedEntities.clear();
    p.PathType = 3;
    p.CoverFlags = 0;
    p.InteractionRange = .0f;
    p.SearchHorizon = 32000;
    p.WorldClimbType = 0;
    p.WorldDropType = 0;
    p.DangerousAuras.Auras.clear();
    p.DangerousAuras.Avoidance = 0;
    p.CollisionMask.Flags = 0x40000000440094;
    p.CollisionMaskMove.Flags = 0x40000000000084;
    p.CollisionMaskStand.Flags = 0x10;
    p.CloseEnoughMin = .0f;
    p.CloseEnoughMax = .0f;
    p.CloseEnoughFloor = .0f;
    p.CloseEnoughPreference = 0;
    p.PreciseItemInteraction = false;
    p.UseSplines = true;
    p.UseTurning = true;
    p.IsPlayer = false;
    p.Portal = bg3se::EntityHandle{};
    p.Climbing = false;
    p.field_154 = 0;
    p.ClosestFullTileIndex = -1;
    p.ClosestCollidingCount = 0x7fffffff;
    p.ClosestCost = 1.0e30f;
}

std::optional<bg3se::AiPathId> path_id(bg3se::AiGrid& grid, bg3se::AiPath* path) {
    for (auto it = grid.PathMap.begin(); it != grid.PathMap.end(); ++it) {
        if (it.Value() == path) return it.Key();
    }
    return {};
}

}  // namespace

// Upstream's CreatePath, and the request's push onto the grid's Paths.
extern "C" void* bg3le_ai_path_create(bool client, char const** why) {
    auto* grid = ai_grid(client);
    if (grid == nullptr) {
        *why = "no level loaded";
        return nullptr;
    }
    if (!bg3le_game_allocator_ready()) {
        *why = "the engine allocator is not up";
        return nullptr;
    }
    bg3se::AiPath* path = nullptr;
    for (auto* p : grid->PathPool) {
        if (!p->InUse && !has_function(&p->DestinationFunc) && !has_function(&p->WeightFunc)) {
            path = p;
            break;
        }
    }
    if (path == nullptr) {
        *why = "No free AiPath available; make sure you released paths that are no longer in use";
        return nullptr;
    }
    if (!bg3le::array_append<bg3se::AiPath*>(&grid->Paths, path)) {
        *why = "the grid's path list could not grow";
        return nullptr;
    }
    const auto handle = grid->NextPathHandle++;
    reset_path(*path);
    path->InUse = true;
    grid->PathMap.insert(handle, path);
    return path;
}

// Upstream's FreePath.
extern "C" void bg3le_ai_path_free(bool client, void* at) {
    auto* grid = ai_grid(client);
    auto* path = static_cast<bg3se::AiPath*>(at);
    if (grid == nullptr || path == nullptr || !path->InUse) return;
    auto id = path_id(*grid, path);
    if (!id) {
        bg3le::logf("paths: trying to free a path that has no ID");
        return;
    }
    for (std::uint32_t i = 0; i < grid->Paths.size(); i++) {
        if (grid->Paths[i] == path) {
            grid->Paths.ordered_remove_at(i);
            break;
        }
    }
    grid->PathMap.erase(grid->PathMap.find(*id));
    path->InUse = false;
}

// Upstream's GetPathById, or null.
extern "C" void* bg3le_ai_path_by_id(bool client, std::uint32_t id) {
    auto* grid = ai_grid(client);
    return grid != nullptr ? grid->PathMap.get_or_default(id) : nullptr;
}

// The pool's paths in use, as upstream's GetActivePathfindingRequests.
extern "C" std::size_t bg3le_ai_paths_active(bool client, void** out, std::size_t cap) {
    auto* grid = ai_grid(client);
    if (grid == nullptr) return 0;
    std::size_t n = 0;
    for (auto* p : grid->PathPool) {
        if (p->InUse) {
            if (n < cap) out[n] = p;
            ++n;
        }
    }
    return n;
}

// The engine's search of one path, image+0x2646b30 (grid, path): what the
// grid's own update runs on the head of its Paths list, between marking and
// unmarking the path's ignored and moved entities. Checked by its opening,
// which copies TargetAdjusted into TargetPosition.
namespace {
constexpr std::uintptr_t kPathSearch = 0x2646b30;
constexpr unsigned char kPathSearchHead[] = {
    0x8b, 0x86, 0x84, 0x00, 0x00, 0x00, 0x89, 0x86, 0x50, 0x01, 0x00, 0x00,
    0x48, 0x8b, 0x46, 0x7c, 0x48, 0x89, 0x86, 0x48, 0x01, 0x00, 0x00};
constexpr std::size_t kPathSearchHeadAt = 0x47;
using PathSearchProc = bool (*)(bg3se::AiGrid*, bg3se::AiPath*);
}  // namespace

// Upstream's FindPathImmediate: 1 when the goal was found, 0 when not, and
// -1 with why when the search cannot be run here.
extern "C" int bg3le_ai_path_search(bool client, void* at, char const** why) {
    static int usable = -1;
    if (usable < 0) {
        unsigned char held[sizeof(kPathSearchHead)] = {};
        usable = bg3le::safe_read((void const*)(bg3le::load_bias() + kPathSearch + kPathSearchHeadAt),
                                  held, sizeof(held))
                 && std::memcmp(held, kPathSearchHead, sizeof(held)) == 0;
        if (!usable) bg3le::logf("paths: the engine's path search is not where this build has it");
    }
    auto* grid = ai_grid(client);
    auto* path = static_cast<bg3se::AiPath*>(at);
    if (!usable) {
        *why = "the engine's path search is not where this build has it";
        return -1;
    }
    if (grid == nullptr || path == nullptr) {
        *why = "no level loaded";
        return -1;
    }
    // The grid's update marks these on the grid around the search; that
    // part is not called, so a path that needs it is not searched.
    if (path->IgnoreEntities.size() != 0 || path->MovedEntities.size() != 0) {
        *why = "a path with IgnoreEntities or MovedEntities needs the grid's entity marking";
        return -1;
    }
    reinterpret_cast<PathSearchProc>(bg3le::load_bias() + kPathSearch)(grid, path);
    return path->GoalFound ? 1 : 0;
}

// ---- surface actions ----
//
// Upstream's SurfaceManager::CreateAction and AddAction (Surface.inl), through
// the engine's own: the factory's create, found where the CreateSurface
// Osiris calls use it, and the manager's AddAction, which sets the level,
// enters the action and appends it. Each is checked by its opening.

namespace {

constexpr std::uintptr_t kSurfaceActionFactory = 0x7ca5eb0;
constexpr std::uintptr_t kCreateAction = 0x38a5fd0;
constexpr std::uintptr_t kAddAction = 0x2cc5b20;
constexpr std::uintptr_t kTransformInit = 0x2682850;

// The create takes a story action id and the ClassDescription bank, which
// it stores at +0x10 and +0x50 (upstream sets the latter afterwards), then
// upstream's action handle, null for a new action.
constexpr unsigned char kCreateActionHead[] = {0x48, 0x89, 0xcb, 0x89, 0xd5, 0x49, 0x89, 0xfe};
constexpr unsigned char kAddActionHead[] = {0x48, 0x8b, 0x87, 0x70, 0x01, 0x00, 0x00, 0x48, 0x89,
                                            0xfb, 0x48, 0x89, 0xf7, 0x49, 0x89, 0xf6, 0x48, 0x89,
                                            0x46, 0x08, 0x48, 0x8b, 0x06, 0xff, 0x50, 0x60};
constexpr unsigned char kTransformInitHead[] = {0x41, 0x89, 0xf6, 0x48, 0x89, 0xfb, 0x44, 0x88, 0xb7,
                                                0x80, 0x00, 0x00, 0x00, 0x88, 0x8f, 0x81, 0x00, 0x00,
                                                0x00, 0x88, 0x97, 0x82, 0x00, 0x00, 0x00};

using CreateActionProc = bg3se::esv::SurfaceAction* (*)(void* factory, int type, int storyActionId,
                                                       void* classDescriptions,
                                                       std::uint64_t actionHandle);
constexpr std::uint64_t kNullHandle = 0xffc0000000000000ull;
using AddActionProc = void (*)(bg3se::esv::SurfaceManager*, bg3se::esv::SurfaceAction*);
using TransformInitProc = void (*)(bg3se::esv::TransformSurfaceAction*, int transform, int layer,
                                   int origin);

template <std::size_t N>
bool code_is(std::uintptr_t at, unsigned char const (&head)[N]) {
    unsigned char held[N] = {};
    return bg3le::safe_read((void const*)(bg3le::load_bias() + at), held, N)
           && std::memcmp(held, head, N) == 0;
}

bool surface_code_checks_out() {
    static int usable = -1;
    if (usable < 0) {
        usable = code_is(kCreateAction + 0xb, kCreateActionHead)
                 && code_is(kAddAction + 4, kAddActionHead)
                 && code_is(kTransformInit + 5, kTransformInitHead);
        if (!usable) bg3le::logf("surfaces: the engine's surface action code is not where this build has it");
    }
    return usable != 0;
}

bg3se::esv::Level* server_level() {
    auto* mgr = static_cast<bg3se::LevelManager*>(bg3le_server_level_manager());
    bg3se::EoCLevel* level = nullptr;
    if (mgr == nullptr || !bg3le::safe_read(&mgr->CurrentLevel, &level, sizeof(level))) return nullptr;
    return static_cast<bg3se::esv::Level*>(level);
}

}  // namespace

// Upstream's CreateSurfaceAction: a new action of the type, or null.
extern "C" void* bg3le_surface_action_create(int type, void* classDescriptions, char const** why) {
    if (!surface_code_checks_out()) {
        *why = "the engine's surface action code is not where this build has it";
        return nullptr;
    }
    if (server_level() == nullptr) return nullptr;
    void* factory = nullptr;
    auto const* slot = (void const*)(bg3le::load_bias() + kSurfaceActionFactory);
    if (!bg3le::safe_read(slot, &factory, sizeof(factory)) || !has_vtable(factory)) {
        *why = "the surface action factory is not up";
        return nullptr;
    }
    auto create = reinterpret_cast<CreateActionProc>(bg3le::load_bias() + kCreateAction);
    return create(factory, type, 0, classDescriptions, kNullHandle);
}

// Upstream's ExecuteSurfaceAction, through the manager's own AddAction.
extern "C" bool bg3le_surface_action_execute(void* at, char const** why) {
    auto* action = static_cast<bg3se::esv::SurfaceAction*>(at);
    auto* level = server_level();
    if (!surface_code_checks_out()) {
        *why = "the engine's surface action code is not where this build has it";
        return false;
    }
    if (level == nullptr || action == nullptr || !has_vtable(action)) return true;
    if (action->Level != nullptr) {
        *why = "Surface action is already activated!";
        return false;
    }
    if (action->GetTypeId() == bg3se::SurfaceActionType::TransformSurface) {
        auto* t = static_cast<bg3se::esv::TransformSurfaceAction*>(action);
        reinterpret_cast<TransformInitProc>(bg3le::load_bias() + kTransformInit)(
            t, (int)t->SurfaceTransformAction, (int)t->SurfaceLayer, (int)t->OriginSurface);
    }
    reinterpret_cast<AddActionProc>(bg3le::load_bias() + kAddAction)(level->SurfaceManager, action);
    return true;
}
