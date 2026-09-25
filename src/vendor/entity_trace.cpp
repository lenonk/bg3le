// Ext.Entity's tracing -- SetupTracing, EnableTracing, GetTrace, ClearTrace --
// as upstream's ECSChangeTracer (GameDefinitions/EntitySystem.cpp) logs it:
// command-buffer, immediate-cache, modification and replication changes from
// a pre-hook on
// the engine's EntityWorld::FlushECBs (image+0x2486dd0, which the world update
// calls when it has an Executor, as upstream's mapping finds it, and two
// callers reach by tail call). Upstream logs replication after the update;
// here it is the update's own second flush, since by bg3le's tick the pools
// have been sent. One tracer per world, since the server and
// the client each trace their own.
//
// The logging loops, ECSChangeLog and ECSChangeTracerOptions are bg3se's (by
// Norbyte and the bg3se contributors); the hook and the per-world state are
// ours.

#include <stdafx.h>

#include <GameDefinitions/EntitySystem.h>
#include <GameDefinitions/EntitySystemHelpers.h>

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>

#include "../ecs_types.h"
#include "../hook.h"
#include "../log.h"

namespace {

using namespace bg3se;
using namespace bg3se::ecs;

struct Tracer {
    EntityWorld* World{nullptr};
    bool Tracing{false};
    ECSChangeTracerOptions Options;
    ECSChangeLog Log;
    ECSChangeLog Snapshot;
};

std::mutex& trace_lock() {
    static std::mutex m;
    return m;
}

Tracer g_tracers[2];

// The world's tracer; with create, the first free one if it has none.
Tracer* tracer_for(EntityWorld* world, bool create) {
    for (auto& t : g_tracers) {
        if (t.World == world) return &t;
    }
    if (!create) return nullptr;
    for (auto& t : g_tracers) {
        if (t.World == nullptr) {
            t.World = world;
            return &t;
        }
    }
    return nullptr;
}

void log_ecb_changes(Tracer& t) {
    auto* world = t.World;
    for (auto& ecb : world->CommandBuffers) {
        for (unsigned i = 0; i < ecb.Data.EntityChanges.size(); i++) {
            auto entityHandle = ecb.Data.EntityChanges.key_at(i);
            auto const& entityChanges = ecb.Data.EntityChanges.Values[i];
            t.Log.AddEntityChange(entityHandle, entityChanges.Flags);
            for (unsigned j = 0; j < entityChanges.Store.size(); j++) {
                auto const& upd = entityChanges.Store[j];
                t.Log.AddComponentChange(world, entityHandle, upd.ComponentTypeId,
                    upd.Index ? ComponentChangeFlags::Create : ComponentChangeFlags::Destroy);
            }
        }
    }
}

void log_immediate_changes(Tracer& t) {
    auto* world = t.World;
    if (world->Cache == nullptr) return;
    auto const& changes = world->Cache->WriteChanges;
    for (unsigned i = 0; i < changes.AvailableComponentTypes.NumBits; i++) {
        if (!changes.AvailableComponentTypes[i]) continue;
        auto const& changeSet = changes.ComponentsByType[i];
        for (unsigned j = 0; j < changeSet.Components.size(); j++) {
            auto entityHandle = changeSet.Components.key_at(j);
            auto const& change = changeSet.Components.Values[j];
            t.Log.AddComponentChange(world, entityHandle, ComponentTypeIndex{(uint16_t)i},
                change.Ptr ? ComponentChangeFlags::Create : ComponentChangeFlags::Destroy);
        }
    }
}

void log_modifications(Tracer& t) {
    auto* world = t.World;
    auto* storages = world->Storage;
    for (auto index = storages->UsedFrameDataStorages.FindFirst(); index;
         index = storages->UsedFrameDataStorages.FindNext(*index)) {
        auto* storage = storages->Storages[*index];
        for (auto slot = storage->ModifiedComponents.FindFirst(); slot;
             slot = storage->ModifiedComponents.FindNext(*slot)) {
            auto componentType = storage->ComponentDtors[*slot].ComponentTypeId;
            if (t.Options.ExcludeModificationTypes[(unsigned)componentType]) continue;
            for (unsigned pageIdx = 0; pageIdx < storage->Components.size(); pageIdx++) {
                auto& page = storage->Components[pageIdx]->Components[*slot];
                auto handles = storage->Handles[pageIdx];
                auto modifications = page.ModifiedEntities.load();
                for (auto idx = BitSetScan(&modifications, &modifications + 1); idx;
                     idx = BitSetScan(&modifications, &modifications + 1, *idx)) {
                    if (handles->Pool[*idx]) {
                        t.Log.AddComponentChange(world, handles->Pool[*idx], componentType,
                                                 ComponentChangeFlags::Modify);
                    }
                }
            }
        }
    }
}

// A replicated type's component index, by name, as upstream's helpers map it.
std::optional<ComponentTypeIndex> component_of_replicated(std::uint16_t replicationIndex) {
    static std::vector<std::int32_t> cache;
    if (replicationIndex >= cache.size()) cache.resize(replicationIndex + 1, -2);
    if (cache[replicationIndex] == -2) {
        auto name = bg3le::ecs::name_of(bg3le::ecs::Context::Replication, replicationIndex);
        auto index = name ? bg3le::ecs::index_of(bg3le::ecs::Context::Component, *name)
                          : std::optional<std::int32_t>{};
        cache[replicationIndex] = index ? *index : -1;
    }
    if (cache[replicationIndex] < 0) return {};
    return ComponentTypeIndex{(uint16_t)cache[replicationIndex]};
}

void log_replication(Tracer& t) {
    auto* buffers = t.World->Replication;
    if (buffers == nullptr) return;
    for (unsigned i = 0; i < buffers->ComponentPools.size(); i++) {
        auto const& pool = buffers->ComponentPools[i];
        if (pool.size() == 0) continue;
        auto type = component_of_replicated((std::uint16_t)i);
        if (!type) continue;
        for (auto const& entity : pool) {
            t.Log.AddComponentChange(t.World, entity.Key(), *type, ComponentChangeFlags::Replicate);
        }
    }
}

using FlushProc = void (*)(EntityWorld*);
FlushProc g_flush = nullptr;

void flush_ecbs_hook(EntityWorld* world) {
    {
        const std::lock_guard<std::mutex> held(trace_lock());
        auto* t = tracer_for(world, false);
        if (t != nullptr && t->Tracing) {
            if (t->Options.TrackECB) log_ecb_changes(*t);
            if (t->Options.TrackImmediateWorldCache) log_immediate_changes(*t);
            if (t->Options.TrackModifications) log_modifications(*t);
            if (t->Options.TrackReplication) log_replication(*t);
        }
    }
    g_flush(world);
}

constexpr std::uintptr_t kFlushECBs = 0x2486dd0;
constexpr unsigned char kFlushECBsPrologue[] = {
    0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54,
    0x53, 0x48, 0x83, 0xe4, 0xc0, 0x48, 0x81, 0xec, 0x40, 0x0b, 0x00, 0x00};

}  // namespace

namespace bg3le {

void install_entity_trace_hook() {
    void* original = nullptr;
    if (bytes_match(kFlushECBs, kFlushECBsPrologue, sizeof(kFlushECBsPrologue))
        && hook_call_sites(kFlushECBs, reinterpret_cast<void*>(&flush_ecbs_hook), &original, true) > 0) {
        g_flush = reinterpret_cast<FlushProc>(original);
    } else {
        logf("entity trace: EntityWorld::FlushECBs is not at %#lx; Ext.Entity tracing is off",
             (unsigned long)kFlushECBs);
    }
}

}  // namespace bg3le

extern "C" void* bg3le_entity_world(void* container);

// SetupTracing: the four switches and the excluded component types.
extern "C" bool bg3le_trace_setup(void* container, bool ecb, bool immediate, bool replication,
                                  bool modifications, std::uint16_t const* exclude,
                                  std::size_t excludeCount) {
    auto* world = static_cast<EntityWorld*>(bg3le_entity_world(container));
    const std::lock_guard<std::mutex> held(trace_lock());
    auto* t = world != nullptr ? tracer_for(world, true) : nullptr;
    if (t == nullptr) return false;
    t->Options.TrackECB = ecb;
    t->Options.TrackImmediateWorldCache = immediate;
    t->Options.TrackReplication = replication;
    t->Options.TrackModifications = modifications;
    t->Options.ExcludeModificationTypes.Clear();
    for (std::size_t i = 0; i < excludeCount; ++i) {
        t->Options.ExcludeModificationTypes.Set((unsigned)exclude[i]);
    }
    return true;
}

// EnableTracing; false when there is no flush hook to trace from.
extern "C" bool bg3le_trace_enable(void* container, bool enable) {
    auto* world = static_cast<EntityWorld*>(bg3le_entity_world(container));
    const std::lock_guard<std::mutex> held(trace_lock());
    auto* t = world != nullptr ? tracer_for(world, true) : nullptr;
    if (t == nullptr || g_flush == nullptr) return false;
    t->Tracing = enable;
    return true;
}

// GetTrace: a copy of the log taken under the lock, for Lua to read at leisure.
extern "C" void* bg3le_trace_get(void* container) {
    auto* world = static_cast<EntityWorld*>(bg3le_entity_world(container));
    const std::lock_guard<std::mutex> held(trace_lock());
    auto* t = world != nullptr ? tracer_for(world, true) : nullptr;
    if (t == nullptr) return nullptr;
    t->Snapshot.Entities = t->Log.Entities;
    return &t->Snapshot;
}

extern "C" void bg3le_trace_clear(void* container) {
    auto* world = static_cast<EntityWorld*>(bg3le_entity_world(container));
    const std::lock_guard<std::mutex> held(trace_lock());
    auto* t = world != nullptr ? tracer_for(world, false) : nullptr;
    if (t != nullptr) t->Log.Clear();
}
