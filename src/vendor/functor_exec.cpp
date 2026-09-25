// Ext.Stats.ExecuteFunctors, ExecuteFunctor and PrepareFunctorParams, as
// upstream's Lua/Libs/StatFunctors.inl writes them, through the engine's own
// per-context executors (esv::ExecuteStatsFunctor_*Context). Those were found
// by what they read: each takes (hit, functors, context), except Interrupt,
// which takes the world second, and each reads its context's own fields --
// its Hit and Attack, or its first entity refs -- at bg3se's offsets. Each is
// checked by its opening before it is called.
//
// The context and functor types are bg3se's (by Norbyte and the bg3se
// contributors).

#include <stdafx.h>

#include <cstdint>
#include <cstring>
#include <new>

#include <GameDefinitions/Stats/Functors.h>
#include <GameDefinitions/Hit.h>

#include "../hook.h"
#include "../log.h"
#include "../mem.h"

namespace {

using namespace bg3se::stats;

struct Executor {
    std::uintptr_t At;
    unsigned char Head[24];
};

// By FunctorContextType, 1 to 9.
constexpr Executor kExecutors[] = {
    {0x3c7dee0, {0x55, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x81,
                 0xec, 0xc8, 0x04, 0x00, 0x00, 0x49, 0x89, 0xf4, 0x48, 0x8d, 0xb2, 0x40}},
    {0x3c7ed20, {0x55, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x81,
                 0xec, 0xc8, 0x04, 0x00, 0x00, 0x49, 0x89, 0xf4, 0x48, 0x8d, 0xb2, 0x10}},
    {0x70a2250, {0x55, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x81,
                 0xec, 0x38, 0x0c, 0x00, 0x00, 0x49, 0x89, 0xd6, 0x49, 0x89, 0xf4, 0x48}},
    {0x70a34b0, {0x55, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x81,
                 0xec, 0x18, 0x08, 0x00, 0x00, 0x49, 0x89, 0xd7, 0x49, 0x89, 0xfe, 0x48}},
    {0x70c6f20, {0x55, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x81,
                 0xec, 0x78, 0x09, 0x00, 0x00, 0x48, 0x89, 0xf5, 0x48, 0x8d, 0xb2, 0x50}},
    {0x70ce490, {0x55, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x81,
                 0xec, 0x28, 0x09, 0x00, 0x00, 0x49, 0x89, 0xd5, 0x48, 0x89, 0xfb, 0x48}},
    {0x70cf4d0, {0x55, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x81,
                 0xec, 0x58, 0x0a, 0x00, 0x00, 0x49, 0x89, 0xd5, 0x48, 0x89, 0xfb, 0x48}},
    {0x70d65e0, {0x55, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x81,
                 0xec, 0x98, 0x09, 0x00, 0x00, 0x48, 0x89, 0xd3, 0x49, 0x89, 0xff, 0x48}},
    {0x3c768c0, {0x55, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x81,
                 0xec, 0x08, 0x08, 0x00, 0x00, 0x49, 0x89, 0xcc, 0x49, 0x89, 0xd6, 0x48}},
};

// The engine's stats::Functors vtable, read off a live Functors container.
constexpr std::uintptr_t kFunctorsVtable = 0x7a87058;
constexpr int kFunctorCloneSlot = 3;
constexpr int kFunctorDeleteSlot = 1;

using ExecuteProc = void (*)(bg3se::HitResult*, Functors*, ContextData*);
using ExecuteInterruptProc = void (*)(bg3se::HitResult*, void* world, Functors*, ContextData*);

std::uintptr_t checked(int type) {
    static signed char usable[10] = {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1};
    if (type < 1 || type > 9) return 0;
    auto const& e = kExecutors[type - 1];
    if (usable[type] < 0) {
        unsigned char held[sizeof(e.Head)] = {};
        usable[type] = bg3le::safe_read((void const*)(bg3le::load_bias() + e.At), held, sizeof(held))
                       && std::memcmp(held, e.Head, sizeof(held)) == 0;
        if (!usable[type]) bg3le::logf("functors: the executor for context %d is not where this build has it", type);
    }
    return usable[type] ? bg3le::load_bias() + e.At : 0;
}

// Upstream's DefaultInitFunctorParams: one context per type, reconstructed
// on every call without destroying the last, as upstream's static is.
template <class T>
ContextData* prepare(void* classDescriptions) {
    alignas(T) static unsigned char storage[sizeof(T)];
    auto* ctx = new (storage) T();
    ctx->Type = T::ContextType;
    ctx->PropertyContext = PropertyContext::TARGET | PropertyContext::AOE;
    ctx->ClassResources = static_cast<bg3se::resource::GuidResourceBankBase*>(classDescriptions);
    return ctx;
}

// A fresh result per call; what the engine allocates into it is left, not
// freed by bg3le.
bg3se::HitResult* fresh_hit() {
    alignas(bg3se::HitResult) static unsigned char storage[sizeof(bg3se::HitResult)];
    return new (storage) bg3se::HitResult();
}

}  // namespace

extern "C" void* bg3le_functor_params(int type, void* classDescriptions) {
    switch (type) {
    case 1: return prepare<AttackTargetContextData>(classDescriptions);
    case 2: return prepare<AttackPositionContextData>(classDescriptions);
    case 3: return prepare<MoveContextData>(classDescriptions);
    case 4: return prepare<TargetContextData>(classDescriptions);
    case 5: return prepare<NearbyAttackedContextData>(classDescriptions);
    case 6: return prepare<NearbyAttackingContextData>(classDescriptions);
    case 7: return prepare<EquipContextData>(classDescriptions);
    case 8: return prepare<SourceContextData>(classDescriptions);
    case 9: return prepare<InterruptContextData>(classDescriptions);
    default: return nullptr;
    }
}

// Upstream's ExecuteFunctors. False with why when it cannot run.
extern "C" bool bg3le_functors_execute(void* functors, void* context, void* world, char const** why) {
    auto* ctx = static_cast<ContextData*>(context);
    auto* list = static_cast<Functors*>(functors);
    if (ctx == nullptr || list == nullptr) {
        *why = "no functors or context";
        return false;
    }
    const int type = (int)ctx->Type;
    const std::uintptr_t proc = checked(type);
    if (proc == 0) {
        *why = type >= 1 && type <= 9 ? "the engine's executor for that context is not where this build has it"
                                      : "Don't know how to execute functors in that context";
        return false;
    }
    auto* hit = fresh_hit();
    if (type == 9) {
        if (world == nullptr) {
            *why = "the entity world is not available";
            return false;
        }
        reinterpret_cast<ExecuteInterruptProc>(proc)(hit, world, list, ctx);
    } else {
        reinterpret_cast<ExecuteProc>(proc)(hit, list, ctx);
    }
    return true;
}

// Upstream's ExecuteFunctor: the one functor, cloned into a container of its
// own. The container is bg3se's, given the engine's vtable, since the
// executors call through it.
extern "C" bool bg3le_functor_execute(void* functor, void* context, void* world, char const** why) {
    auto* f = static_cast<Functor*>(functor);
    std::uint64_t fvmt = 0, clone = 0;
    if (f == nullptr || !bg3le::safe_read(f, &fvmt, sizeof(fvmt))
        || !bg3le::safe_read((void const*)(fvmt + kFunctorCloneSlot * 8), &clone, sizeof(clone))) {
        *why = "not a functor";
        return false;
    }
    std::uint64_t vmt[4] = {};
    const auto vtable = bg3le::load_bias() + kFunctorsVtable;
    if (!bg3le::safe_read((void const*)vtable, vmt, sizeof(vmt)) || vmt[0] == 0 || vmt[0] == vmt[1]) {
        *why = "the engine's Functors vtable is not where this build has it";
        return false;
    }
    auto* copy = reinterpret_cast<Functor* (*)(Functor const*)>(clone)(f);
    if (copy == nullptr) {
        *why = "the functor would not clone";
        return false;
    }
    alignas(Functors) unsigned char storage[sizeof(Functors)];
    auto* list = new (storage) Functors();
    std::memcpy(storage, &vtable, sizeof(vtable));
    list->Values.push_back(copy);
    list->NextHandle = 1;
    const bool ok = bg3le_functors_execute(list, context, world, why);
    // Nothing of the engine's is freed: the clone goes through its own
    // deleting destructor, and the list's buffer is bg3le's.
    std::uint64_t cvmt = 0, del = 0;
    if (bg3le::safe_read(copy, &cvmt, sizeof(cvmt))
        && bg3le::safe_read((void const*)(cvmt + kFunctorDeleteSlot * 8), &del, sizeof(del))) {
        reinterpret_cast<void (*)(Functor*)>(del)(copy);
    }
    list->Values.clear();
    return ok;
}
