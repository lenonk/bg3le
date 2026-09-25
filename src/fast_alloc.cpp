#include "fast_alloc.h"

#include <sys/mman.h>

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <new>

#include "debug_server.h"
#include "hook.h"
#include "log.h"

namespace bg3le {
namespace {

// physx::shdfnd::TempAllocator::allocate(unsigned long, char const*, int)
constexpr std::uintptr_t kTempAlloc = 0x5666b30;
// physx::shdfnd::TempAllocator::deallocate(void*)
constexpr std::uintptr_t kTempFree = 0x5666c90;

using AllocProc = void* (*)(void*, unsigned long, const char*, int);
using FreeProc = void (*)(void*, void*);

AllocProc g_real_alloc = nullptr;
FreeProc g_real_free = nullptr;

// 16-byte header keeps the payload 16-byte aligned, which PhysX assumes.
constexpr std::uint64_t kMagic = 0x42473354454D5031ULL;  // "BG3TEMP1"
constexpr std::size_t kHeaderSize = 16;
constexpr std::size_t kClasses = 14;          // 16B .. 128KB
constexpr std::size_t kSmallest = 16;
constexpr std::size_t kArenaChunk = 4u << 20;  // 4MB per refill

struct Header {
    std::uint64_t magic;
    std::uint64_t size_class;
};

struct FreeNode {
    FreeNode* next;
};

struct ThreadPool {
    FreeNode* free_list[kClasses] = {nullptr};
    char* arena = nullptr;
    std::size_t arena_left = 0;
};

thread_local ThreadPool t_pool;

std::atomic<unsigned long> g_served{0};
std::atomic<unsigned long> g_recycled{0};
std::atomic<unsigned long> g_arena_bytes{0};
std::atomic<unsigned long> g_passthrough{0};

std::size_t class_of(std::size_t size) {
    std::size_t need = size + kHeaderSize;
    std::size_t cls = 0;
    std::size_t cap = kSmallest;
    while (cap < need && cls + 1 < kClasses) {
        cap <<= 1;
        ++cls;
    }
    return cap >= need ? cls : kClasses;  // kClasses = too big for us
}

std::size_t bytes_of(std::size_t cls) { return kSmallest << cls; }

void* carve(std::size_t want) {
    ThreadPool& pool = t_pool;
    if (pool.arena_left < want) {
        const std::size_t chunk = want > kArenaChunk ? want : kArenaChunk;
        void* mem = ::mmap(nullptr, chunk, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (mem == MAP_FAILED) return nullptr;
        pool.arena = static_cast<char*>(mem);
        pool.arena_left = chunk;
        g_arena_bytes.fetch_add(chunk, std::memory_order_relaxed);
    }
    char* p = pool.arena;
    pool.arena += want;
    pool.arena_left -= want;
    return p;
}

void* alloc_hook(void* self, unsigned long size, const char* file, int line) {
    const std::size_t cls = class_of(size);
    if (cls >= kClasses) {
        // Rare large request: let PhysX handle it, lock and all.
        g_passthrough.fetch_add(1, std::memory_order_relaxed);
        return g_real_alloc != nullptr ? g_real_alloc(self, size, file, line)
                                       : nullptr;
    }

    ThreadPool& pool = t_pool;
    void* block = nullptr;
    if (pool.free_list[cls] != nullptr) {
        FreeNode* node = pool.free_list[cls];
        pool.free_list[cls] = node->next;
        block = node;
        g_recycled.fetch_add(1, std::memory_order_relaxed);
    } else {
        block = carve(bytes_of(cls));
        if (block == nullptr) {
            return g_real_alloc != nullptr ? g_real_alloc(self, size, file, line)
                                           : nullptr;
        }
    }

    auto* head = static_cast<Header*>(block);
    head->magic = kMagic;
    head->size_class = cls;
    g_served.fetch_add(1, std::memory_order_relaxed);
    return static_cast<char*>(block) + kHeaderSize;
}

void free_hook(void* self, void* ptr) {
    if (ptr == nullptr) return;

    auto* head = reinterpret_cast<Header*>(static_cast<char*>(ptr) - kHeaderSize);
    // Anything not ours -- allocated before the hook, or through a call site
    // we did not patch -- must go back to PhysX.
    if (head->magic != kMagic) {
        if (g_real_free != nullptr) g_real_free(self, ptr);
        return;
    }

    const std::size_t cls = head->size_class;
    if (cls >= kClasses) {
        if (g_real_free != nullptr) g_real_free(self, ptr);
        return;
    }

    head->magic = 0;  // so a double free is not silently recycled
    ThreadPool& pool = t_pool;
    auto* node = reinterpret_cast<FreeNode*>(head);
    node->next = pool.free_list[cls];
    pool.free_list[cls] = node;
}

}  // namespace

void fast_alloc_install() {
    // On by default: it takes level loads from 65-98s to ~1.2s, which is
    // worth more than the caution once physics has been played on it.
    // BG3LE_FAST_ALLOC=0 disables it.
    const char* opt = std::getenv("BG3LE_FAST_ALLOC");
    if (opt != nullptr && opt[0] == '0') {
        statusf("fast alloc: disabled by BG3LE_FAST_ALLOC=0");
        return;
    }

    // Free first: its hook passes foreign blocks on, so it is safe alone. An
    // allocate hook without it hands the engine blocks it cannot free.
    void* original = nullptr;
    const std::size_t f = hook_call_sites(kTempFree,
                                          reinterpret_cast<void*>(&free_hook),
                                          &original);
    if (f > 0) g_real_free = reinterpret_cast<FreeProc>(original);
    if (f == 0) {
        statusf("fast alloc: NOT active (no free call sites patched)");
        return;
    }

    const std::size_t a = hook_call_sites(kTempAlloc,
                                          reinterpret_cast<void*>(&alloc_hook),
                                          &original);
    if (a > 0) g_real_alloc = reinterpret_cast<AllocProc>(original);
    if (a == 0) {
        statusf("fast alloc: NOT active (no allocate call sites patched)");
        return;
    }
    // Two counts, not a ratio: the old wording read "77/80 sites" as though
    // three had been missed, when they are the allocate and free call sites
    // and both were patched in full.
    statusf("fast alloc: active, replacing PhysX TempAllocator at %zu "
            "allocate and %zu free call sites", a, f);
}

void fast_alloc_report(const char* when) {
    statusf("fast alloc (%s): %lu served (%lu recycled), %lu passed through, "
            "%.1f MB arena",
            when, g_served.load(), g_recycled.load(), g_passthrough.load(),
            (double)g_arena_bytes.load() / (1024.0 * 1024.0));
}

}  // namespace bg3le
