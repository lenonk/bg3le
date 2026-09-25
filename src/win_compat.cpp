// Linux stand-ins for the few Win32 entry points the vendored bg3se code
// calls. The callers are by Norbyte and the bg3se contributors
// (https://github.com/Norbyte/bg3se); these implementations are ours.

#include <sys/uio.h>
#include <unistd.h>


namespace {

// Probing every page of a large range would cost more than the validation it
// guards, so check the first and last byte. That catches the null and
// wild-pointer cases the callers actually care about.
bool readable(void const* p) {
    unsigned char sink;
    iovec local{&sink, 1};
    iovec remote{const_cast<void*>(p), 1};
    return ::process_vm_readv(::getpid(), &local, 1, &remote, 1, 0) == 1;
}

}  // namespace

// Win32 returns nonzero when the range is *not* readable.
extern "C" int IsBadReadPtr(void const* p, unsigned long long size) {
    if (p == nullptr || size == 0) return 1;
    if (!readable(p)) return 1;
    auto const last = static_cast<unsigned char const*>(p) + (size - 1);
    return readable(last) ? 0 : 1;
}

// DetourAttachEx and DetourDetach used to refuse here. They record instead
// now -- see src/detour_interpose.cpp, which explains why recording is enough
// on a platform where a symbol can be interposed.

// ---- Windows RPC UUID functions ----
//
// Declared by vendor/compat/combaseapi.h. A Windows GUID keeps Data1/Data2/
// Data3 in native byte order, so the textual form is not a plain hex dump of
// the struct; these follow that layout because Guid values round-trip through
// Osiris and the ECS.

#include <cstdio>
#include <cstdint>
#include <sys/random.h>

struct Bg3leGuid {
    std::uint32_t Data1;
    std::uint16_t Data2;
    std::uint16_t Data3;
    std::uint8_t Data4[8];
};

extern "C" long UuidFromStringA(unsigned char* stringUuid, void* out) {
    if (stringUuid == nullptr || out == nullptr) return 1700;

    unsigned int d1 = 0, d2 = 0, d3 = 0;
    unsigned int b[8] = {0};
    const int matched = std::sscanf(reinterpret_cast<const char*>(stringUuid),
                                    "%8x-%4x-%4x-%2x%2x-%2x%2x%2x%2x%2x%2x",
                                    &d1, &d2, &d3, &b[0], &b[1], &b[2], &b[3],
                                    &b[4], &b[5], &b[6], &b[7]);
    if (matched != 11) return 1700;  // RPC_S_INVALID_STRING_UUID

    auto* guid = static_cast<Bg3leGuid*>(out);
    guid->Data1 = d1;
    guid->Data2 = static_cast<std::uint16_t>(d2);
    guid->Data3 = static_cast<std::uint16_t>(d3);
    for (int i = 0; i < 8; ++i) guid->Data4[i] = static_cast<std::uint8_t>(b[i]);
    return 0;
}

extern "C" long UuidCreate(void* out) {
    if (out == nullptr) return 1700;
    auto* guid = static_cast<Bg3leGuid*>(out);
    if (::getrandom(guid, sizeof(Bg3leGuid), 0) != (ssize_t)sizeof(Bg3leGuid)) {
        return 1700;
    }
    // RFC 4122 version 4, variant 1, matching what UuidCreate produces.
    guid->Data3 = static_cast<std::uint16_t>((guid->Data3 & 0x0FFF) | 0x4000);
    guid->Data4[0] = static_cast<std::uint8_t>((guid->Data4[0] & 0x3F) | 0x80);
    return 0;
}

extern "C" long DetourTransactionBegin() { return 50; }
extern "C" long DetourTransactionCommit() { return 50; }
extern "C" long DetourUpdateThread(void*) { return 50; }

// ---- slim reader/writer locks ----
//
// Declared in vendor/compat/msvc_compat.h, where SRWLOCK is the engine's own
// pthread_rwlock_t, so these work on the engine's locks as well as bg3se's.
// A zero-filled one is PTHREAD_RWLOCK_INITIALIZER, which is what upstream
// relies on of a Win32 SRWLOCK.
//
// extern "C" means the declared parameter type does not affect linkage, so
// taking void* here matches the PSRWLOCK declarations.

#include <pthread.h>

namespace {
pthread_rwlock_t* srw(void* lock) { return static_cast<pthread_rwlock_t*>(lock); }
}  // namespace

extern "C" void InitializeSRWLock(void* lock) {
    ::pthread_rwlock_init(srw(lock), nullptr);
}

extern "C" void AcquireSRWLockExclusive(void* lock) { ::pthread_rwlock_wrlock(srw(lock)); }
extern "C" void ReleaseSRWLockExclusive(void* lock) { ::pthread_rwlock_unlock(srw(lock)); }
extern "C" void AcquireSRWLockShared(void* lock) { ::pthread_rwlock_rdlock(srw(lock)); }
extern "C" void ReleaseSRWLockShared(void* lock) { ::pthread_rwlock_unlock(srw(lock)); }

extern "C" int TryAcquireSRWLockExclusive(void* lock) {
    return ::pthread_rwlock_trywrlock(srw(lock)) == 0;
}

// ---- guarded regions ----
//
// vendor/bg3se/BG3Extender/GameDefinitions/Base/Base.h routes BEGIN_GUARDED /
// END_GUARDED through a C++ try/catch on Linux, because SEH does not exist
// here and the upstream SEH handler lives in CrashReporter.cpp, which is a
// Windows component. Called from inside a catch handler, so it can rethrow to
// inspect the exception.

#include <exception>

#include "log.h"

namespace bg3se {

void HandleGuardedCppException() {
    try {
        throw;
    } catch (const std::exception& e) {
        bg3le::logf("guarded: exception escaped into engine code: %s", e.what());
    } catch (...) {
        bg3le::logf("guarded: unknown exception escaped into engine code");
    }
}

}  // namespace bg3se
