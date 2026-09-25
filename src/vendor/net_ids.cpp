// entity:GetNetId, as upstream's EntityToNetId: the server's
// EntityReplicationAuthority (inside esv::GameServer) or the client's
// EntityReplicationPeer (inside net::GameClient). Neither has a symbol, so
// each is found by the world it points at. Layouts are bg3se's (by Norbyte
// and the bg3se contributors) -- thank you.

#include <stdafx.h>

#include <GameDefinitions/EntitySystem.h>

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "../log.h"
#include "../mem.h"

namespace bg3le {
std::uintptr_t load_bias();
}
extern "C" void* bg3le_entity_world(void* container);

namespace {

using NetMap = bg3se::HashMap<bg3se::EntityHandle, bg3se::NetId>;
using EntityMap = bg3se::HashMap<bg3se::NetId, bg3se::EntityHandle>;

constexpr std::uintptr_t kEoCClient = 0x7b75b50;

std::atomic<void*> g_game_server{nullptr};

// A HashMap whose bookkeeping agrees with itself; its key count, or -1.
long map_count(void const* at) {
    struct {
        void* Hash; std::uint32_t Buckets; std::uint32_t Pad;
        void* Next; std::uint32_t NextCap; std::uint32_t NextSize;
        void* Keys; std::uint32_t KeysCap; std::uint32_t KeysSize;
        void* Values; std::uint32_t ValuesCap; std::uint32_t ValuesSize;
    } m{};
    static_assert(sizeof(m) == sizeof(NetMap));
    if (!bg3le::safe_read(at, &m, sizeof(m))) return -1;
    const std::uint32_t n = m.KeysSize;
    if (n > (1u << 22) || m.NextSize != n || n > m.KeysCap
        || (m.ValuesSize != 0 && m.ValuesSize != n) || (n > 0 && (m.Buckets == 0 || m.Keys == nullptr))) {
        return -1;
    }
    return (long)n;
}

// Both directions present, the same size, and not empty.
template <class Owner>
bool maps_agree(char const* owner) {
    const long a = map_count(owner + offsetof(Owner, EntityToNetId));
    const long b = map_count(owner + offsetof(Owner, NetIdToEntity));
    return a > 0 && a == b;
}

NetMap const* server_map(bg3se::ecs::EntityWorld* world) {
    static NetMap const* cached = nullptr;
    static void* cachedWorld = nullptr;
    if (cached != nullptr && cachedWorld == world) return cached;
    auto* gs = static_cast<char*>(g_game_server.load());
    if (gs == nullptr || world == nullptr) return nullptr;
    using A = bg3se::ecs::EntityReplicationAuthority;
    for (std::size_t o = 0; o < 0x1000; o += 8) {
        void* w = nullptr;
        if (!bg3le::safe_read(gs + o + offsetof(A, World), &w, sizeof(w)) || w != world) continue;
        if (!maps_agree<A>(gs + o)) continue;
        cached = reinterpret_cast<NetMap const*>(gs + o + offsetof(A, EntityToNetId));
        cachedWorld = world;
        bg3le::logf("net ids: server replication authority at GameServer+%#zx", o);
        return cached;
    }
    return nullptr;
}

NetMap const* client_map(bg3se::ecs::EntityWorld* world) {
    static NetMap const* cached = nullptr;
    static void* cachedWorld = nullptr;
    if (cached != nullptr && cachedWorld == world) return cached;
    char* eoc = nullptr;
    if (world == nullptr || !bg3le::safe_read((void const*)(bg3le::load_bias() + kEoCClient), &eoc, sizeof(eoc))
        || eoc == nullptr) {
        return nullptr;
    }
    using P = bg3se::ecs::EntityReplicationPeer;
    // The peer names its GameClient, which is what holds it.
    for (std::size_t o = 0; o < 0x400; o += 8) {
        char* client = nullptr;
        if (!bg3le::safe_read(eoc + o, &client, sizeof(client)) || client == nullptr) continue;
        for (std::size_t q = 0; q < 0x800; q += 8) {
            void* w = nullptr;
            void* back = nullptr;
            if (!bg3le::safe_read(client + q + offsetof(P, World), &w, sizeof(w)) || w != world
                || !bg3le::safe_read(client + q + offsetof(P, Client), &back, sizeof(back)) || back != client
                || !maps_agree<P>(client + q)) {
                continue;
            }
            cached = reinterpret_cast<NetMap const*>(client + q + offsetof(P, EntityToNetId));
            cachedWorld = world;
            bg3le::logf("net ids: client replication peer at GameClient+%#zx", q);
            return cached;
        }
    }
    return nullptr;
}

}  // namespace

// The server tick's esv::GameServer, which holds the replication authority.
extern "C" void bg3le_note_game_server(void* server) {
    g_game_server.store(server, std::memory_order_relaxed);
}

// Upstream's EntityToNetId for the context's world; false if it has none.
extern "C" bool bg3le_entity_net_id(void* container, std::uint64_t handle, bool server,
                                    std::uint64_t* out) {
    auto* world = static_cast<bg3se::ecs::EntityWorld*>(bg3le_entity_world(container));
    NetMap const* map = server ? server_map(world) : client_map(world);
    if (map == nullptr) return false;
    auto const* id = map->try_get(bg3se::EntityHandle{handle});
    if (id == nullptr) return false;
    *out = id->Id;
    return true;
}
