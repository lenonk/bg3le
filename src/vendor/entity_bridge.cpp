// Bridges bg3le's captured ECS pointers to bg3se's ECS implementation.
//
// bg3le supplies what only it can: the component type indices, read from the
// named TypeId statics in the symbol table, and the EntityStorageContainer,
// captured from a hooked GetEntityStorage call. bg3se supplies what it already
// reverse engineered: the layout of the container, of EntityStorageData, and
// the page-map walk that turns an EntityHandle plus a component index into a
// pointer.
//
// Its layout has been checked against this build twice over -- the component
// mask is 0x110 bytes, matching ComponentMapSize = 0x880, and the container
// begins with Array<EntityStorageData*>, which is where the disassembled
// lookup reads its buffer pointer from.
//
// The bg3se code this calls is by Norbyte and the bg3se contributors
// (https://github.com/Norbyte/bg3se); only the bridge is ours.

// Utils.h first: upstream relies on its own translation units pulling the
// logging macros in before the Lua headers.
#include <Extender/Shared/Utils.h>

#include <GameDefinitions/EntitySystem.h>
#include <GameDefinitions/Components/All.h>

namespace bg3le {

// Returns the component of the given type for an entity, or nullptr if the
// entity has no such component. componentSize is what bg3se uses to stride the
// component page, so it has to match the engine's real component size.
extern "C" void* bg3le_entity_component(void* container, std::uint64_t handle,
                                        std::uint16_t componentIndex,
                                        std::size_t componentSize) {
    if (container == nullptr) return nullptr;

    auto* storages = reinterpret_cast<bg3se::ecs::EntityStorageContainer*>(container);
    const auto entity = bg3se::EntityHandle(handle);

    const auto storageIndex = storages->GetEntityStorageIndex(entity);
    if (!storageIndex.has_value()) return nullptr;

    auto* storage = storages->GetEntityStorage(*storageIndex);
    if (storage == nullptr) return nullptr;

    return storage->GetComponent(entity,
                                 bg3se::ecs::ComponentTypeIndex(componentIndex),
                                 componentSize);
}

// End-to-end proof: reads Health off an entity. Keeps every bg3se type inside
// this translation unit, so the rest of bg3le needs none of its headers.
//
// The component size matters: bg3se strides the component page with it, so it
// has to match the engine's. Taking it from bg3se's own struct is exactly the
// earlier finding that its component definitions lay out correctly here,
// because they are plain structs the compiler arranges rather than hardcoded
// offsets.
extern "C" bool bg3le_entity_health(void* container, std::uint64_t handle,
                                    std::uint16_t componentIndex,
                                    std::int32_t* hp, std::int32_t* maxHp) {
    auto* component = static_cast<bg3se::HealthComponent*>(bg3le_entity_component(
        container, handle, componentIndex, sizeof(bg3se::HealthComponent)));
    if (component == nullptr) return false;
    *hp = component->Hp;
    *maxHp = component->MaxHp;
    return true;
}

// Reports each step of the walk separately, because a single null cannot
// distinguish "the entity genuinely has no such component" from "the storage
// lookup failed". Returns the storage index (-1 if none), the storage pointer,
// and the component pointer.
extern "C" void bg3le_entity_probe(void* container, std::uint64_t handle,
                                   std::uint16_t componentIndex,
                                   std::int32_t* storageIndex,
                                   void** storage, void** component) {
    *storageIndex = -1;
    *storage = nullptr;
    *component = nullptr;
    if (container == nullptr) return;

    auto* storages = reinterpret_cast<bg3se::ecs::EntityStorageContainer*>(container);
    const auto entity = bg3se::EntityHandle(handle);

    const auto index = storages->GetEntityStorageIndex(entity);
    if (!index.has_value()) return;
    *storageIndex = *index;

    auto* data = storages->GetEntityStorage(*index);
    if (data == nullptr) return;
    *storage = data;

    *component = data->GetComponent(entity,
                                    bg3se::ecs::ComponentTypeIndex(componentIndex),
                                    sizeof(bg3se::HealthComponent));
}

// Walks the container's storages looking for one whose component set includes
// the given type, and returns an entity out of it. Needed because the handle
// the thunk captures is whatever the engine touched last, which is usually
// something with no Health at all.
extern "C" std::uint64_t bg3le_find_entity_with(void* container,
                                                std::uint16_t componentIndex,
                                                std::uint32_t* storagesSeen) {
    *storagesSeen = 0;
    if (container == nullptr) return 0;

    auto* storages = reinterpret_cast<bg3se::ecs::EntityStorageContainer*>(container);
    const auto type = bg3se::ecs::ComponentTypeIndex(componentIndex);

    for (auto* storage : storages->Storages) {
        if (storage == nullptr) continue;
        ++*storagesSeen;
        if (!storage->ComponentTypeToIndex.try_get(type)) continue;

        // First live entity in this storage. HashMap keeps its keys in an
        // array, so they can be read without walking buckets.
        for (auto const& key : storage->InstanceToPageMap.keys()) {
            if (key.Handle != bg3se::EntityHandle::NullHandle) return key.Handle;
        }
    }
    return 0;
}

// Every live entity, or every entity carrying one component.
//
// The walk is the one bg3le already uses to find a single entity, run to
// completion: each storage's InstanceToPageMap holds its entities as keys
// in an array, so they come out without touching buckets or page masks.
// Filtering is per storage rather than per entity, because a storage is an
// archetype -- every entity in it has exactly the same component set, so
// one lookup decides for all of them.
//
// Returns how many exist, which may exceed `max`; the caller sizes its
// buffer from a first call and reads on a second, and a count that grew in
// between is truncated rather than overrunning.
extern "C" std::size_t bg3le_entities_collect(void* container,
                                              int componentIndex,
                                              std::uint64_t* out,
                                              std::size_t max) {
    if (container == nullptr) return 0;

    auto* storages =
        reinterpret_cast<bg3se::ecs::EntityStorageContainer*>(container);
    const bool filtered = componentIndex >= 0;
    const auto type =
        bg3se::ecs::ComponentTypeIndex((std::uint16_t)componentIndex);

    std::size_t found = 0;
    for (auto* storage : storages->Storages) {
        if (storage == nullptr) continue;
        // ComponentsInClass, which is what bg3se's own HasComponent
        // tests. ComponentTypeToIndex is a slot map and matched only 19
        // entities for Health, none of which read one back.
        if (filtered && !storage->HasComponent(type)) continue;

        // InstanceToPageMap's keys, which are the handles the rest of
        // bg3le already resolves -- the same ones find_entity_with hands
        // to the component readers. The handle pages looked like the
        // authoritative list and are not: their entries came back with a
        // different high half and GetEntityStorageIndex rejected every
        // one.
        for (auto const& key : storage->InstanceToPageMap.keys()) {
            if (key.Handle == bg3se::EntityHandle::NullHandle) continue;
            if (out != nullptr && found < max) out[found] = key.Handle;
            ++found;
        }
    }
    return found;
}

// Finds the component of a given type on whichever entity carries it, by
// scanning the container's storages. Used for the singleton components, which
// bg3se normally reaches through EntityWorld -- and the world is the one thing
// with no symbol and no capture point, so this route avoids needing it.
static void* find_any_component(void* container, std::uint16_t componentIndex,
                                std::size_t componentSize) {
    if (container == nullptr) return nullptr;

    auto* storages = reinterpret_cast<bg3se::ecs::EntityStorageContainer*>(container);
    const auto type = bg3se::ecs::ComponentTypeIndex(componentIndex);

    for (auto* storage : storages->Storages) {
        if (storage == nullptr) continue;
        if (!storage->ComponentTypeToIndex.try_get(type)) continue;

        for (auto const& key : storage->InstanceToPageMap.keys()) {
            if (key.Handle == bg3se::EntityHandle::NullHandle) continue;
            auto* component = storage->GetComponent(key, type, componentSize);
            if (component != nullptr) return component;
        }
    }
    return nullptr;
}

// UUID string -> EntityHandle, through ls::uuid::ToHandleMappingComponent.
// Returns 0 if the mapping component cannot be found or the UUID is unknown.
extern "C" std::uint64_t bg3le_uuid_to_handle(void* container,
                                              std::uint16_t mappingIndex,
                                              char const* uuid) {
    auto* mapping = static_cast<bg3se::UuidToHandleMappingComponent*>(
        find_any_component(container, mappingIndex,
                           sizeof(bg3se::UuidToHandleMappingComponent)));
    if (mapping == nullptr || uuid == nullptr) return 0;

    const auto guid = bg3se::Guid::ParseGuidString(uuid);
    if (!guid.has_value()) return 0;

    auto* handle = mapping->Mappings.try_get(*guid);
    return handle != nullptr ? handle->Handle : 0;
}

// Marks a component dirty so the change is picked up and replicated.
//
// bg3se routes this through EntityWorld::MarkComponentAsChanged, but that
// function only touches the storage and the container's UsedFrameDataStorages
// bitset -- and EntityWorld::Storage *is* the container we captured. So it
// needs no world either.
//
// This is MarkComponentAsChanged on its own, which makes the server act on
// the new value. Getting it to the client as well takes the replication flags
// too -- see bg3le_replicate_component below.
extern "C" bool bg3le_mark_component_changed(void* container,
                                             std::uint64_t handle,
                                             std::uint16_t componentIndex) {
    if (container == nullptr) return false;

    auto* storages = reinterpret_cast<bg3se::ecs::EntityStorageContainer*>(container);
    const auto entity = bg3se::EntityHandle(handle);
    const auto type = bg3se::ecs::ComponentTypeIndex(componentIndex);

    const auto index = storages->GetEntityStorageIndex(entity);
    if (!index.has_value()) return false;

    auto* storage = storages->GetEntityStorage(*index);
    if (storage == nullptr) return false;

    if (!storage->MarkComponentAsChanged(entity, type)) return false;

    if (!storages->UsedFrameDataStorages[storage->StorageIndex]) {
        storages->UsedFrameDataStorages.Set(storage->StorageIndex);
    }
    return true;
}

// Health is read and written through typed accessors rather than raw offsets,
// so the field layout comes from bg3se's struct rather than being restated.
extern "C" bool bg3le_set_health(void* container, std::uint64_t handle,
                                 std::uint16_t componentIndex,
                                 std::int32_t hp, bool setMax) {
    auto* component = static_cast<bg3se::HealthComponent*>(bg3le_entity_component(
        container, handle, componentIndex, sizeof(bg3se::HealthComponent)));
    if (component == nullptr) return false;
    component->Hp = hp;
    if (setMax) component->MaxHp = hp;
    return true;
}


// Recovers the EntityWorld from the captured container.
//
// EntityWorld has no symbol and no capture point, but it turns out not to need
// one: EntityStorageContainer::ComponentRegistry points at EntityWorld's
// ComponentRegistry_, which is a by-value member sitting immediately after
// Replication. So the world is that registry pointer less the member's offset.
//
// Two independent pointers confirm it: world->Storage has to come back out as
// the container we started from, and &world->Queries has to equal the
// container's own Queries pointer. bg3le_world_probe reports both, so a layout
// drift shows up as a failed check rather than as a wild pointer.
namespace {

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Winvalid-offsetof"
constexpr std::size_t kComponentRegistryOffset =
    offsetof(bg3se::ecs::EntityWorld, ComponentRegistry_);
#pragma clang diagnostic pop

// Replication is the first member, so the registry is one pointer in.
static_assert(kComponentRegistryOffset == sizeof(void*),
              "EntityWorld::Replication is expected to precede ComponentRegistry_");

bg3se::ecs::EntityWorld* world_from_container(void* container) {
    if (container == nullptr) return nullptr;

    auto* storages = reinterpret_cast<bg3se::ecs::EntityStorageContainer*>(container);
    auto* registry = storages->ComponentRegistry;
    if (registry == nullptr) return nullptr;

    return reinterpret_cast<bg3se::ecs::EntityWorld*>(
        reinterpret_cast<char*>(registry) - kComponentRegistryOffset);
}

}  // namespace

// The EntityWorld a captured container belongs to, or null. What an EntityRef
// written from Lua is paired with when it has no world of its own.
extern "C" void* bg3le_entity_world(void* container) {
    return world_from_container(container);
}

// Reports everything needed to judge whether the recovered world is real: the
// pointer itself, its Replication buffers, the size of the replication pool
// array -- which should be close to the number of ReplicatedTypeContext
// indices in the symbol table -- and whether Storage and Queries round-trip.
extern "C" void bg3le_world_probe(void* container, void** world,
                                  void** replication, std::int32_t* poolCount,
                                  bool* storageMatches, bool* queriesMatch) {
    *world = nullptr;
    *replication = nullptr;
    *poolCount = -1;
    *storageMatches = false;
    *queriesMatch = false;

    auto* w = world_from_container(container);
    if (w == nullptr) return;
    *world = w;

    auto* storages = reinterpret_cast<bg3se::ecs::EntityStorageContainer*>(container);
    *storageMatches = (w->Storage == storages);
    *queriesMatch = (&w->Queries == storages->Queries);

    if (w->Replication == nullptr) return;
    *replication = w->Replication;
    *poolCount = (std::int32_t)w->Replication->ComponentPools.Size();
}

// A one-frame component, which does not live in the entity page at all.
//
// These are the transient event components -- a request or a notification that
// exists for a single tick. The engine keeps them in a per-storage pool keyed
// by entity rather than in the page, so reading one through the page path
// returns whatever happens to be at that offset. That is what the 17 size
// mismatches SizeAudit reported were: not a wrong struct, a wrong storage
// mechanism. Same failure shape as the proxy components, silent in the same
// way.
extern "C" void* bg3le_entity_one_frame_component(void* container,
                                                  std::uint64_t handle,
                                                  std::uint16_t componentIndex) {
    if (container == nullptr) return nullptr;

    auto* storages = reinterpret_cast<bg3se::ecs::EntityStorageContainer*>(container);
    const auto entity = bg3se::EntityHandle(handle);

    auto* storage = storages->GetEntityStorage(entity);
    if (storage == nullptr) return nullptr;

    return storage->GetOneFrameComponent(
        entity, bg3se::ecs::ComponentTypeIndex(componentIndex));
}

// The engine's own recorded size for a component.
//
// Worth having because the size is not cosmetic: GetComponent returns
// buf + componentSize * entryIndex, so if bg3se's struct is the wrong size
// every read of that component is misaligned -- for every entity except
// whichever one happens to sit at index 0, which is exactly the sort of bug
// that looks like correct code failing intermittently.
//
// The engine keeps the sizes per storage, indexed by the component's slot
// within that storage, so this searches the storages for one that carries the
// component. Returns -1 if none does, which only means no live entity has it.
extern "C" int bg3le_component_engine_size(void* container,
                                           std::uint16_t componentIndex) {
    if (container == nullptr) return -1;

    auto* storages = reinterpret_cast<bg3se::ecs::EntityStorageContainer*>(container);
    const auto type = bg3se::ecs::ComponentTypeIndex(componentIndex);

    for (auto* storage : storages->Storages) {
        if (storage == nullptr || storage->ComponentSizes == nullptr) continue;
        auto const* slot = storage->ComponentTypeToIndex.try_get(type);
        if (slot == nullptr) continue;
        return (int)storage->ComponentSizes[*slot];
    }
    return -1;
}

// Whether this container belongs to the server world.
//
// The client and server each have an EntityWorld, and replication buffers are
// only allocated on the server's -- which is how bg3se's ReplicateComponent
// decides it is being called from the wrong side. So the same test identifies
// which of the two captured containers is which, without depending on the
// order the engine happened to touch them in.
extern "C" bool bg3le_container_is_server(void* container) {
    auto* world = world_from_container(container);
    return world != nullptr && world->Replication != nullptr;
}

// bg3se's ReplicateComponent, reached through the recovered world.
//
// Marking a component changed makes the server act on the new value;
// replication is what sends it to the client, and without it anything reading
// the replicated copy -- the UI included -- never sees it. Whole-component
// replication is qword 0 with every flag set, as EntityProxyMetatable::
// Replicate passes.
//
// Status: 0 ok, 1 no container, 2 no world, 3 no replication buffers (a
// client-side world; only the server replicates), 4 replication type index out
// of range, 5 could not add the entity to the pool, 6 the engine allocator is
// not installed and this call would have had to allocate.
extern "C" bool bg3le_game_allocator_ready();

extern "C" std::int32_t bg3le_replicate_component(void* container,
                                                  std::uint64_t handle,
                                                  std::uint16_t replicationTypeIndex,
                                                  std::uint32_t qword,
                                                  std::uint64_t flags) {
    if (container == nullptr) return 1;

    auto* world = world_from_container(container);
    if (world == nullptr) return 2;
    if (world->Replication == nullptr) return 3;

    auto& pools = world->Replication->ComponentPools;
    if (replicationTypeIndex >= pools.Size()) return 4;

    const auto entity = bg3se::EntityHandle(handle);
    auto& pool = pools[replicationTypeIndex];

    // add_key and EnsureSize both grow engine-owned memory, so they need the
    // engine's allocator to be installed. Refuse rather than call a null
    // Alloc, and refuse rather than mix a foreign heap into an engine
    // container.
    auto* syncFlags = pool.try_get(entity);
    if (syncFlags == nullptr) {
        if (!bg3le_game_allocator_ready()) return 6;
        syncFlags = pool.add_key(entity);
    }
    if (syncFlags == nullptr) return 5;

    if (syncFlags->NumQwords() <= qword && !bg3le_game_allocator_ready()) {
        return 6;
    }
    syncFlags->EnsureSize((qword + 1) * 64);
    const bool changed = (syncFlags->GetBuf()[qword] & flags) != flags;
    syncFlags->GetBuf()[qword] |= flags;
    if (changed) world->Replication->Dirty = true;

    return 0;
}

}  // namespace bg3le
