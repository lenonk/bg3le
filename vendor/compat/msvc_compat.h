#pragma once
//
// Compatibility glue for building the BG3 Script Extender
// (https://github.com/Norbyte/bg3se) with clang on Linux instead of MSVC on
// Windows. The code it supports is by Norbyte and the bg3se contributors,
// MIT + Commons Clause; only this shim is ours. With thanks to them.
//
//
// Force-included (clang -include) so upstream headers compile unmodified.
// MSVC defines SAL annotations and the SRWLOCK types implicitly; clang does
// not, and the headers that use them do not include anything that would.
//

// SAL annotations carry no codegen meaning; discard them.
#define _Post_ptr_invalid_
#define _Pre_valid_
#define _Post_writable_byte_size_(size)
#define _Post_writable_size_(size)
#define _Pre_writable_size_(size)
#define _Pre_readable_size_(size)
#define _In_
#define _In_opt_
#define _Out_
#define _Inout_

// ---- basic Win32 types ----
//
// Declared up front: later shims in this header use them, and the whole file
// is force-included ahead of everything else.
//
// DWORD and LONG are 32-bit on Windows, so they are unsigned int and int --
// not long, which is 64-bit on LP64.
typedef int BOOL;
typedef unsigned int DWORD;
typedef unsigned short WORD;
typedef unsigned char BYTE;
typedef int LONG;
typedef long long LONG64;
typedef unsigned long long ULONG64;
typedef unsigned long long ULONGLONG;
typedef void* HANDLE;
typedef void* HMODULE;
typedef void* LPVOID;
typedef void* LPSECURITY_ATTRIBUTES;
typedef wchar_t* LPWSTR;
typedef const wchar_t* LPCWSTR;
typedef void (*FARPROC)();

// Where the Windows engine has an SRWLOCK the Linux one has a glibc
// pthread_rwlock_t -- a resource Bank's BankTypeId sits 56 bytes past its
// lock -- so that is what this is, and zero-initialised it is ready, as an
// SRWLOCK is.
#include <pthread.h>
extern "C" {
typedef struct _RTL_SRWLOCK { pthread_rwlock_t Lock; } SRWLOCK, *PSRWLOCK;
}

typedef void* HANDLE;

// A recursive mutex, since that is what a Win32 critical section is. 48
// bytes, as the engine's are: ResourceManager's next member starts 48 past
// its lock, which holds a mutex and then a spin count. Never enter one of
// the engine's, though -- Initialized overlaps that spin count.
#include <pthread.h>
typedef struct _RTL_CRITICAL_SECTION {
    pthread_mutex_t Mutex;
    bool Initialized;
} CRITICAL_SECTION, *PCRITICAL_SECTION;

inline void InitializeCriticalSection(PCRITICAL_SECTION cs) {
    pthread_mutexattr_t attr;
    ::pthread_mutexattr_init(&attr);
    ::pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    ::pthread_mutex_init(&cs->Mutex, &attr);
    ::pthread_mutexattr_destroy(&attr);
    cs->Initialized = true;
}

inline void DeleteCriticalSection(PCRITICAL_SECTION cs) {
    if (cs->Initialized) {
        ::pthread_mutex_destroy(&cs->Mutex);
        cs->Initialized = false;
    }
}

// Upstream sometimes relies on a zero-initialised section, which Win32 does
// not allow either; initialise on first use rather than deadlocking.
inline void EnterCriticalSection(PCRITICAL_SECTION cs) {
    if (!cs->Initialized) InitializeCriticalSection(cs);
    ::pthread_mutex_lock(&cs->Mutex);
}

inline void LeaveCriticalSection(PCRITICAL_SECTION cs) {
    ::pthread_mutex_unlock(&cs->Mutex);
}

inline BOOL TryEnterCriticalSection(PCRITICAL_SECTION cs) {
    if (!cs->Initialized) InitializeCriticalSection(cs);
    return ::pthread_mutex_trylock(&cs->Mutex) == 0;
}

// Declared, not defined. These guard state belonging to bg3se rather than to
// the engine, so a futex-backed one-pointer implementation can stand in; that
// is only needed to link, not to compile the definitions.
extern "C" {
void InitializeSRWLock(PSRWLOCK);
void AcquireSRWLockExclusive(PSRWLOCK);
void ReleaseSRWLockExclusive(PSRWLOCK);
void AcquireSRWLockShared(PSRWLOCK);
void ReleaseSRWLockShared(PSRWLOCK);
BOOL TryAcquireSRWLockExclusive(PSRWLOCK);
}

#define _In_z_
#define _In_reads_(n)
#define _Out_writes_(n)
#define _Printf_format_string_

typedef unsigned int DWORD;   // 32-bit on Windows; unsigned long is 64-bit on LP64
typedef long long LONG64;
typedef unsigned long long ULONG64;
typedef void* HMODULE;

// MSVC declares these in the global namespace as well as in std.
#include <exception>
using std::terminate;

typedef int LONG;             // 32-bit on Windows
typedef struct _EXCEPTION_POINTERS {
    void* ExceptionRecord;
    void* ContextRecord;
} EXCEPTION_POINTERS, *PEXCEPTION_POINTERS;

// MSVC pulls these in transitively; libc++ does not.
#include <variant>
#include <optional>
#include <string>

#define WINBASEAPI
#define WINAPI
#define CALLBACK

// Win32 file-enumeration types, needed only so the hook typedefs in
// GameHooks/EngineHooksFwdDecl.h parse. The Linux build does not hook these.
typedef int BOOL;
typedef wchar_t* LPWSTR;
typedef const wchar_t* LPCWSTR;
typedef struct _WIN32_FIND_DATAW { unsigned char opaque[592]; }
    WIN32_FIND_DATAW, *LPWIN32_FIND_DATAW;

typedef unsigned char BYTE;   // one byte: it appears in engine layouts
typedef unsigned short WORD;

// Upstream uses the Win32 pointer probe to reject garbage pointers before
// dereferencing them during validation. bg3le implements it over
// process_vm_readv; see src/win_compat.cpp.
extern "C" BOOL IsBadReadPtr(void const* p, unsigned long long size);

// MSVC bit-scan intrinsics. Their out-parameter is unsigned long, which is
// 32-bit on Windows and 64-bit here, so clang builtins of the same name do
// not accept the upstream call sites. Macros take precedence over builtins.
namespace bg3le_compat {
template <class TIndex>
inline unsigned char bsf64(TIndex* index, unsigned long long mask) {
    if (mask == 0) return 0;
    *index = static_cast<TIndex>(__builtin_ctzll(mask));
    return 1;
}
template <class TIndex>
inline unsigned char bsf32(TIndex* index, unsigned int mask) {
    if (mask == 0) return 0;
    *index = static_cast<TIndex>(__builtin_ctz(mask));
    return 1;
}
template <class TIndex>
inline unsigned char bsr64(TIndex* index, unsigned long long mask) {
    if (mask == 0) return 0;
    *index = static_cast<TIndex>(63 - __builtin_clzll(mask));
    return 1;
}
}  // namespace bg3le_compat

#define _BitScanForward64(Index, Mask) ::bg3le_compat::bsf64((Index), (Mask))
#define _BitScanForward(Index, Mask)   ::bg3le_compat::bsf32((Index), (Mask))
#define _BitScanReverse64(Index, Mask) ::bg3le_compat::bsr64((Index), (Mask))

typedef void* LPSECURITY_ATTRIBUTES;

// Win32 spellings the upstream sources use directly.
#include <strings.h>
#include <sys/syscall.h>
#include <unistd.h>
inline int _stricmp(const char* a, const char* b) { return ::strcasecmp(a, b); }
inline unsigned long GetCurrentThreadId() {
    return static_cast<unsigned long>(::syscall(SYS_gettid));
}

// Win32 generic function pointer, used by the Osiris DLL wrappers.
typedef void (*FARPROC)();

// Win32 memory protection, used by the PE symbol mapper. bg3le does not use
// that mapper -- it reads the ELF symbol table instead -- but the header still
// has to compile.
#include <sys/mman.h>
#include <sys/stat.h>
typedef void* LPVOID;
#define PAGE_NOACCESS          0x01
#define PAGE_READONLY          0x02
#define PAGE_READWRITE         0x04
#define PAGE_EXECUTE           0x10
#define PAGE_EXECUTE_READ      0x20
#define PAGE_EXECUTE_READWRITE 0x40

inline int bg3le_page_prot(unsigned long win) {
    switch (win) {
        case PAGE_READONLY:          return PROT_READ;
        case PAGE_READWRITE:         return PROT_READ | PROT_WRITE;
        case PAGE_EXECUTE:           return PROT_EXEC;
        case PAGE_EXECUTE_READ:      return PROT_READ | PROT_EXEC;
        case PAGE_EXECUTE_READWRITE: return PROT_READ | PROT_WRITE | PROT_EXEC;
        default:                     return PROT_NONE;
    }
}

inline BOOL VirtualProtect(LPVOID addr, std::size_t size,
                           DWORD newProtect, DWORD* oldProtect) {
    const long page = 4096;
    auto start = reinterpret_cast<unsigned long long>(addr) & ~(unsigned long long)(page - 1);
    const unsigned long long span =
        (reinterpret_cast<unsigned long long>(addr) + size) - start;
    if (oldProtect != nullptr) *oldProtect = PAGE_EXECUTE_READ;  // not queryable
    return ::mprotect(reinterpret_cast<void*>(start), span,
                      bg3le_page_prot(newProtect)) == 0;
}

// MSVC secure-CRT string formatting. Upstream uses the array-reference
// overloads, where the bound is deduced. _snprintf_s takes a maximum
// character count excluding the terminator; snprintf takes a buffer size
// including it.
#include <cstdio>
#include <cstdarg>
#include <cstddef>

template <std::size_t N, class... Args>
int _snprintf_s(char (&buf)[N], std::size_t count, const char* fmt, Args... args) {
    const std::size_t size = (count + 1 < N) ? count + 1 : N;
    return std::snprintf(buf, size, fmt, args...);
}

template <std::size_t N, class... Args>
int sprintf_s(char (&buf)[N], const char* fmt, Args... args) {
    return std::snprintf(buf, N, fmt, args...);
}

template <std::size_t N>
int strcpy_s(char (&buf)[N], const char* src) {
    std::snprintf(buf, N, "%s", src);
    return 0;
}

typedef unsigned long long ULONGLONG;

// Win32 high-resolution timing, used by Ext.Timer. CLOCK_MONOTONIC with a
// fixed 1 GHz frequency gives the nanosecond resolution the callers expect.
#include <ctime>
#include <sched.h>

typedef union _LARGE_INTEGER {
    struct { DWORD LowPart; LONG HighPart; };
    long long QuadPart;
} LARGE_INTEGER;

inline BOOL QueryPerformanceCounter(LARGE_INTEGER* count) {
    timespec ts{};
    ::clock_gettime(CLOCK_MONOTONIC, &ts);
    count->QuadPart = (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
    return 1;
}

inline BOOL QueryPerformanceFrequency(LARGE_INTEGER* freq) {
    freq->QuadPart = 1000000000LL;
    return 1;
}

inline ULONGLONG GetTickCount64() {
    timespec ts{};
    ::clock_gettime(CLOCK_MONOTONIC, &ts);
    return (ULONGLONG)ts.tv_sec * 1000ULL + (ULONGLONG)(ts.tv_nsec / 1000000);
}

#include <cstdlib>
#include <cwchar>
#include <string>

inline char* _strdup(const char* s) { return ::strdup(s); }

// Win32 returns the raw command line as one wide string. /proc/self/cmdline
// holds the arguments NUL-separated, so rejoin them. Cached, because the
// caller expects a pointer that stays valid.
inline const wchar_t* GetCommandLineW() {
    static const std::wstring cmdline = [] {
        std::string joined;
        if (std::FILE* f = std::fopen("/proc/self/cmdline", "rb")) {
            char buf[4096];
            std::size_t n;
            while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
                joined.append(buf, n);
            }
            std::fclose(f);
        }
        for (char& c : joined) {
            if (c == '\0') c = ' ';
        }
        while (!joined.empty() && joined.back() == ' ') joined.pop_back();

        std::wstring wide(joined.size() + 1, L'\0');
        const std::size_t written =
            std::mbstowcs(wide.data(), joined.c_str(), wide.size());
        wide.resize(written == static_cast<std::size_t>(-1) ? 0 : written);
        return wide;
    }();
    return cmdline.c_str();
}

// Win32 Sleep takes milliseconds; Sleep(0) yields.
inline void Sleep(unsigned long ms) {
    if (ms == 0) {
        ::sched_yield();
        return;
    }
    timespec ts{(long)(ms / 1000), (long)((ms % 1000) * 1000000L)};
    ::nanosleep(&ts, nullptr);
}

// MSVC pulls <list> in transitively; libc++ does not, and OsiList is a
// std::list alias used throughout the Osiris definitions.
#include <list>

#ifndef TRUE
#define TRUE 1
#define FALSE 0
#endif

#define _TRUNCATE ((std::size_t)-1)

inline int strncpy_s(char* buf, std::size_t bufSize, const char* src,
                     std::size_t count) {
    const std::size_t limit =
        (count == (std::size_t)-1 || count >= bufSize) ? bufSize - 1 : count;
    std::snprintf(buf, limit + 1, "%s", src);
    return 0;
}

template <std::size_t N>
int strncpy_s(char (&buf)[N], const char* src, std::size_t count) {
    const std::size_t limit = (count == (std::size_t)-1 || count >= N) ? N - 1 : count;
    std::snprintf(buf, limit + 1, "%s", src);
    return 0;
}

inline int freopen_s(std::FILE** out, const char* path, const char* mode,
                     std::FILE* stream) {
    std::FILE* f = std::freopen(path, mode, stream);
    if (out != nullptr) *out = f;
    return f == nullptr ? 1 : 0;
}

// Interlocked intrinsics, over the compiler atomics.
// Templated on the integer type: callers pass int64_t, which is long on LP64
// and long long on Windows.
template <class T, class V>
inline T InterlockedExchangeAdd64(T volatile* addend, V value) {
    return __atomic_fetch_add(addend, static_cast<T>(value), __ATOMIC_SEQ_CST);
}

template <class T, class V>
inline T InterlockedOr64(T volatile* dest, V value) {
    return __atomic_fetch_or(dest, static_cast<T>(value), __ATOMIC_SEQ_CST);
}

inline void* GetCurrentThread() { return nullptr; }
inline void* GetCurrentProcess() { return nullptr; }

inline void DebugBreak() { __builtin_trap(); }

inline void OutputDebugStringA(const char* text) {
    std::fputs(text, stderr);
}

// Code page and text conversion. Only CP_UTF8 is ever requested, and the
// process locale is UTF-8, so the standard multibyte functions do the job.
#define CP_UTF8 65001
#define CP_ACP 0

inline int MultiByteToWideChar(unsigned int /*codePage*/, unsigned long /*flags*/,
                               const char* in, int inLen, wchar_t* out,
                               int outLen) {
    std::string src = (inLen < 0) ? std::string(in) : std::string(in, (std::size_t)inLen);
    const std::size_t needed = std::mbstowcs(nullptr, src.c_str(), 0);
    if (needed == (std::size_t)-1) return 0;
    if (outLen == 0 || out == nullptr) return (int)needed;
    const std::size_t written = std::mbstowcs(out, src.c_str(), (std::size_t)outLen);
    return written == (std::size_t)-1 ? 0 : (int)written;
}

inline int WideCharToMultiByte(unsigned int /*codePage*/, unsigned long /*flags*/,
                               const wchar_t* in, int inLen, char* out,
                               int outLen, const char* /*defaultChar*/,
                               BOOL* /*usedDefault*/) {
    std::wstring src = (inLen < 0) ? std::wstring(in) : std::wstring(in, (std::size_t)inLen);
    const std::size_t needed = std::wcstombs(nullptr, src.c_str(), 0);
    if (needed == (std::size_t)-1) return 0;
    if (outLen == 0 || out == nullptr) return (int)needed;
    const std::size_t written = std::wcstombs(out, src.c_str(), (std::size_t)outLen);
    return written == (std::size_t)-1 ? 0 : (int)written;
}

// Console attributes. There is no Win32 console here, so the colour calls are
// accepted and ignored rather than translated to ANSI.
#define STD_INPUT_HANDLE  ((DWORD)-10)
#define STD_OUTPUT_HANDLE ((DWORD)-11)
#define STD_ERROR_HANDLE  ((DWORD)-12)

#define FOREGROUND_BLUE      0x0001
#define FOREGROUND_GREEN     0x0002
#define FOREGROUND_RED       0x0004
#define FOREGROUND_INTENSITY 0x0008

#define ENABLE_PROCESSED_OUTPUT            0x0001
#define ENABLE_WRAP_AT_EOL_OUTPUT          0x0002
#define ENABLE_VIRTUAL_TERMINAL_PROCESSING 0x0004

typedef struct _COORD { short X; short Y; } COORD;
typedef struct _SMALL_RECT { short Left, Top, Right, Bottom; } SMALL_RECT;
typedef struct _CONSOLE_SCREEN_BUFFER_INFOEX {
    unsigned long cbSize;
    COORD dwSize;
    COORD dwCursorPosition;
    unsigned short wAttributes;
    SMALL_RECT srWindow;
    COORD dwMaximumWindowSize;
    unsigned short wPopupAttributes;
    BOOL bFullscreenSupported;
    unsigned long ColorTable[16];
} CONSOLE_SCREEN_BUFFER_INFOEX, *PCONSOLE_SCREEN_BUFFER_INFOEX;

inline HANDLE GetStdHandle(DWORD) { return nullptr; }
inline BOOL SetConsoleTextAttribute(HANDLE, unsigned short) { return 1; }
inline BOOL SetConsoleOutputCP(unsigned int) { return 1; }
inline BOOL SetConsoleTitleW(const wchar_t*) { return 1; }
inline BOOL AllocConsole() { return 1; }
inline BOOL FreeConsole() { return 1; }
inline BOOL GetConsoleScreenBufferInfoEx(HANDLE, PCONSOLE_SCREEN_BUFFER_INFOEX) { return 0; }
inline BOOL SetConsoleScreenBufferInfoEx(HANDLE, PCONSOLE_SCREEN_BUFFER_INFOEX) { return 0; }
inline BOOL GetConsoleMode(HANDLE, DWORD*) { return 0; }
inline BOOL SetConsoleMode(HANDLE, DWORD) { return 0; }

// Filesystem entry points, over POSIX.
#define GENERIC_WRITE 0x40000000
#define MB_OK 0
#define MB_ICONERROR 0x10
#define MAKEINTRESOURCE(x) ((const char*)(unsigned long long)(x))

inline BOOL CreateDirectoryW(const wchar_t* path, void*) {
    const std::size_t needed = std::wcstombs(nullptr, path, 0);
    if (needed == (std::size_t)-1) return 0;
    std::string narrow(needed + 1, '\0');
    std::wcstombs(narrow.data(), path, narrow.size());
    narrow.resize(needed);
    return ::mkdir(narrow.c_str(), 0755) == 0 ? 1 : 0;
}

inline BOOL DeleteFileW(const wchar_t* path) {
    const std::size_t needed = std::wcstombs(nullptr, path, 0);
    if (needed == (std::size_t)-1) return 0;
    std::string narrow(needed + 1, '\0');
    std::wcstombs(narrow.data(), path, narrow.size());
    narrow.resize(needed);
    return ::unlink(narrow.c_str()) == 0 ? 1 : 0;
}

inline BOOL ReadConsoleW(HANDLE, void*, DWORD, DWORD* read, void*) {
    if (read != nullptr) *read = 0;
    return 0;
}

// ---- module and process queries ----
//
// GetProcAddress maps onto dlsym. GetModuleHandleW is only ever used to test
// whether a module is loaded or to pass to GetProcAddress, so a dlopen handle
// with RTLD_NOLOAD answers both.
#include <dlfcn.h>
#include <limits.h>
#include <cerrno>
#include <cwchar>

inline FARPROC GetProcAddress(HMODULE module, const char* name) {
    void* sym = ::dlsym(module != nullptr ? module : RTLD_DEFAULT, name);
    return reinterpret_cast<FARPROC>(sym);
}

inline HMODULE GetModuleHandleW(const wchar_t* name) {
    if (name == nullptr) return ::dlopen(nullptr, RTLD_LAZY | RTLD_NOLOAD);
    const std::size_t needed = std::wcstombs(nullptr, name, 0);
    if (needed == (std::size_t)-1) return nullptr;
    std::string narrow(needed + 1, '\0');
    std::wcstombs(narrow.data(), name, narrow.size());
    narrow.resize(needed);
    return ::dlopen(narrow.c_str(), RTLD_LAZY | RTLD_NOLOAD);
}

inline HMODULE GetModuleHandleA(const char* name) {
    return ::dlopen(name, RTLD_LAZY | RTLD_NOLOAD);
}

inline DWORD GetModuleFileNameW(HMODULE, wchar_t* out, DWORD size) {
    char path[PATH_MAX];
    const ssize_t n = ::readlink("/proc/self/exe", path, sizeof(path) - 1);
    if (n <= 0) return 0;
    path[n] = '\0';
    const std::size_t written = std::mbstowcs(out, path, size);
    return written == (std::size_t)-1 ? 0 : (DWORD)written;
}

#define ERROR_ALREADY_EXISTS 183
inline DWORD GetLastError() { return (DWORD)errno; }
inline DWORD GetCurrentProcessId() { return (DWORD)::getpid(); }

// MSVC byte-swap intrinsics.
inline unsigned short _byteswap_ushort(unsigned short v) { return __builtin_bswap16(v); }
inline unsigned int _byteswap_ulong(unsigned int v) { return __builtin_bswap32(v); }
inline unsigned long long _byteswap_uint64(unsigned long long v) { return __builtin_bswap64(v); }

// Wide and secure CRT stragglers.
inline int _wcsicmp(const wchar_t* a, const wchar_t* b) { return ::wcscasecmp(a, b); }

template <std::size_t N>
int wcscpy_s(wchar_t (&buf)[N], const wchar_t* src) {
    std::wcsncpy(buf, src, N - 1);
    buf[N - 1] = L'\0';
    return 0;
}

inline int gmtime_s(std::tm* out, const std::time_t* time) {
    return ::gmtime_r(time, out) == nullptr ? 1 : 0;
}

inline BOOL CopyFileW(const wchar_t* from, const wchar_t* to, BOOL failIfExists) {
    auto narrow = [](const wchar_t* w) {
        const std::size_t needed = std::wcstombs(nullptr, w, 0);
        if (needed == (std::size_t)-1) return std::string();
        std::string out(needed + 1, '\0');
        std::wcstombs(out.data(), w, out.size());
        out.resize(needed);
        return out;
    };
    const std::string src = narrow(from);
    const std::string dst = narrow(to);
    if (src.empty() || dst.empty()) return 0;
    if (failIfExists && ::access(dst.c_str(), F_OK) == 0) return 0;

    std::FILE* in = std::fopen(src.c_str(), "rb");
    if (in == nullptr) return 0;
    std::FILE* out = std::fopen(dst.c_str(), "wb");
    if (out == nullptr) {
        std::fclose(in);
        return 0;
    }
    char buf[65536];
    std::size_t n;
    bool ok = true;
    while ((n = std::fread(buf, 1, sizeof(buf), in)) > 0) {
        if (std::fwrite(buf, 1, n, out) != n) { ok = false; break; }
    }
    std::fclose(in);
    std::fclose(out);
    return ok ? 1 : 0;
}

inline void TerminateProcess(HANDLE, unsigned int code) { ::_exit((int)code); }
inline int MessageBoxA(HANDLE, const char* text, const char* caption, unsigned int) {
    std::fprintf(stderr, "%s: %s\n", caption != nullptr ? caption : "bg3se",
                 text != nullptr ? text : "");
    return 0;
}
// There are no PE resources in an ELF image. GetExeResource is used for the
// embedded Lua bundle, which will have to come from a file instead.
template <class TName, class TType>
inline void* FindResource(HMODULE, TName, TType) { return nullptr; }
inline void* LoadResource(HMODULE, void*) { return nullptr; }
inline void* LockResource(void*) { return nullptr; }
inline DWORD SizeofResource(HMODULE, void*) { return 0; }

// (buffer, buffer size, max characters excluding the terminator, format, args)
inline int _vsnprintf_s(char* buf, std::size_t bufSize, std::size_t count,
                        const char* fmt, va_list args) {
    const std::size_t size =
        (count == (std::size_t)-1 || count + 1 > bufSize) ? bufSize : count + 1;
    return std::vsnprintf(buf, size, fmt, args);
}

#define MAKEWORD(a, b) ((WORD)(((BYTE)(a)) | (((WORD)((BYTE)(b))) << 8)))
#define WSAECONNRESET ECONNRESET
#define WSAEWOULDBLOCK EWOULDBLOCK
#define WSAEINPROGRESS EINPROGRESS

// TracerPid is non-zero while a debugger is attached.
inline BOOL IsDebuggerPresent() {
    std::FILE* f = std::fopen("/proc/self/status", "r");
    if (f == nullptr) return 0;
    char line[256];
    BOOL traced = 0;
    while (std::fgets(line, sizeof(line), f) != nullptr) {
        int pid = 0;
        if (std::sscanf(line, "TracerPid: %d", &pid) == 1) {
            traced = pid != 0;
            break;
        }
    }
    std::fclose(f);
    return traced;
}
