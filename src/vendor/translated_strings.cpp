// ls::TranslatedStringRepository, read and written as bg3se does
// (Lua/Libs/Localization.inl, by Norbyte and the bg3se contributors), and
// the engine's FixedString::CreateFromString.
//
// The repository has no symbol. Its global was found by content in a live
// game -- the pool whose Texts map holds the copyright line's handle, the
// repository whose TranslatedStrings[0] points at that pool, and the one
// .bss pointer to the repository -- and is checked on use: the copyright
// handle has to resolve through bg3le's hash rule before anything is read.
//
// Upstream's layout with Linux widths: TextPool is Array<STDString*> then
// HashMap<RuntimeStringHandle, LSStringView>, and the SRWSpinLock's owner id
// is eight bytes. Writes never reallocate: an existing handle's value is
// overwritten, a new one goes in only while the map has room, so no engine
// buffer is ever freed.

#include <stdafx.h>

#include <GameDefinitions/GlobalFixedStrings.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <utility>
#include <vector>
#include <cstdint>
#include <cstring>
#include <string>
#include <thread>

#include "../hook.h"
#include "../log.h"
#include "../mem.h"

extern "C" bool bg3le_game_allocator_ready();

extern "C" bool bg3le_fixed_string_create(char const* text, std::uint32_t* out);
extern "C" char const* bg3le_fixed_string(std::uint32_t index,
                                          std::uint32_t* length);

namespace bg3le {
char const* client_state_name();
bool translated_string_get(char const* handle, std::string* out);
}

namespace bg3le {
namespace {

// ls::gTranslatedStringRepository.
constexpr std::uintptr_t kRepositoryGlobal = 0x7d9d258;

// ls::FixedString::CreateFromString(LSStringView const&), found through
// upstream's anchor string; checked by its prologue and its hash seed.
constexpr std::uintptr_t kFixedStringCreate = 0x226db50;
constexpr unsigned char kFixedStringCreatePrologue[] = {
    0x55, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53,
    0x48, 0x81, 0xec, 0xc8, 0x00, 0x00, 0x00};
constexpr std::uintptr_t kFixedStringCreateSeedAt = 0x226dbbc;
constexpr unsigned char kFixedStringCreateSeed[] = {0x41, 0xba, 0xed, 0x5e,
                                                    0xad, 0xde};

// The menu's copyright line, which the check resolves.
constexpr char const* kCheckHandle = "h5b6e4138g2cf0g4d67gb825gee416cf8c54f";

// TranslatedStringRepository.
constexpr std::size_t kTranslatedStrings = 0x08;  // TextPool*[9]
constexpr std::size_t kFallbackPool = 0x50;
constexpr std::size_t kVersionedFallbackPool = 0x58;
constexpr std::size_t kLock = 0xf0;               // SRWSpinLock::FastLock

// TextPool.
constexpr std::size_t kStrings = 0x00;    // Array<STDString*>
constexpr std::size_t kHashKeys = 0x10;   // StaticArray<int32_t>
constexpr std::size_t kNextIds = 0x20;    // Array<int32_t>
constexpr std::size_t kKeys = 0x30;       // Array<RuntimeStringHandle>
constexpr std::size_t kValues = 0x40;     // UninitializedStaticArray<LSStringView>

// LSStringView: the size is 32 bits here, and the rest is padding.
struct View {
    char const* data;
    std::uint32_t size;
    std::uint32_t pad;
};
static_assert(sizeof(View) == 16);

struct Handle {
    std::uint32_t id;
    std::uint16_t version;
    std::uint16_t pad;
};
static_assert(sizeof(Handle) == 8);

struct StaticArrayView {
    void* buf;
    std::uint32_t size;
};
struct ArrayView {
    void* buf;
    std::uint32_t capacity;
    std::uint32_t size;
};

struct CreateView {
    char const* data;
    std::uint32_t size;
};
using CreateProc = std::uint32_t (*)(CreateView const*);
CreateProc g_create = nullptr;

// HashMapHash(FixedString) is the string's own hash, which the engine's
// CreateFromString computes as MurmurHash3_x86_32 seeded with 0xdead5eed.
std::uint32_t string_hash(char const* text) {
    const auto* data = reinterpret_cast<unsigned char const*>(text);
    const std::size_t len = std::strlen(text);
    constexpr std::uint32_t c1 = 0xcc9e2d51, c2 = 0x1b873593;
    auto rotl = [](std::uint32_t x, int r) { return (x << r) | (x >> (32 - r)); };
    std::uint32_t h = 0xdead5eed;
    const std::size_t blocks = len / 4;
    for (std::size_t i = 0; i < blocks; ++i) {
        std::uint32_t k;
        std::memcpy(&k, data + 4 * i, 4);
        h ^= rotl(k * c1, 15) * c2;
        h = rotl(h, 13) * 5 + 0xe6546b64;
    }
    std::uint32_t k = 0;
    switch (len & 3) {
        case 3: k ^= std::uint32_t(data[4 * blocks + 2]) << 16; [[fallthrough]];
        case 2: k ^= std::uint32_t(data[4 * blocks + 1]) << 8; [[fallthrough]];
        case 1: k ^= data[4 * blocks]; h ^= rotl(k * c1, 15) * c2;
    }
    h ^= static_cast<std::uint32_t>(len);
    h ^= h >> 16;
    h *= 0x85ebca6b;
    h ^= h >> 13;
    h *= 0xc2b2ae35;
    h ^= h >> 16;
    return h;
}

// A handle's FixedString, interned through the engine as upstream's
// FixedString(handle) conversion does.
bool handle_key(char const* handle, std::uint32_t* id, std::uint32_t* hash) {
    if (!bg3le_fixed_string_create(handle, id)) return false;
    *hash = string_hash(handle);
    return true;
}

template <class T>
T& field(void* base, std::size_t offset) {
    return *reinterpret_cast<T*>(static_cast<char*>(base) + offset);
}

// SRWSpinLock, as upstream's Lock.inl. The owner id is left alone: it only
// makes the engine's own thread re-entrant, and bg3le never calls the
// engine while holding the lock.
std::atomic<std::uint32_t>& lock_word(void* repo) {
    return *reinterpret_cast<std::atomic<std::uint32_t>*>(
        static_cast<char*>(repo) + kLock);
}

// Bounded: bg3le runs on the game's own threads, and the repository is
// write-locked while the localisation loads -- by a thread that may be
// waiting on the one asking. So a busy lock is a miss, never a wait, except
// on bg3le's own thread (spins < 0).
bool read_lock(void* repo, int spins = 2000) {
    auto& fast = lock_word(repo);
    for (int i = 0; spins < 0 || i < spins; ++i) {
        if ((fast.load() & 0xfff00000u) == 0) {
            if ((fast.fetch_add(1) & 0xfff00000u) == 0) return true;
            fast.fetch_sub(1);
        }
        std::this_thread::yield();
    }
    return false;
}

void read_unlock(void* repo) { lock_word(repo).fetch_sub(1); }

bool write_lock(void* repo, int spins = 2000) {
    auto& fast = lock_word(repo);
    for (int i = 0;; ++i) {
        if (spins >= 0 && i >= spins) return false;
        if ((fast.load() & 0xfff00000u) == 0) {
            if ((fast.fetch_add(0x100000u) & 0xfff00000u) == 0) break;
            fast.fetch_sub(0x100000u);
        }
        std::this_thread::yield();
    }
    // Readers hold it briefly; wait them out.
    while ((fast.load() & 0x000fffffu) != 0) std::this_thread::yield();
    return true;
}

void write_unlock(void* repo) { lock_word(repo).fetch_sub(0x100000u); }

// HashSet::find_index.
int find_index(void* pool, std::uint32_t id, std::uint32_t hash) {
    auto const& hashKeys = field<StaticArrayView>(pool, kHashKeys);
    auto const& nextIds = field<ArrayView>(pool, kNextIds);
    auto const& keys = field<ArrayView>(pool, kKeys);
    if (hashKeys.size == 0 || hashKeys.buf == nullptr) return -1;

    int index = static_cast<std::int32_t*>(hashKeys.buf)[hash % hashKeys.size];
    for (std::uint32_t steps = 0; index >= 0 && steps <= keys.size; ++steps) {
        if (static_cast<std::uint32_t>(index) >= keys.size) return -1;
        if (static_cast<Handle*>(keys.buf)[index].id == id) return index;
        index = static_cast<std::int32_t*>(nextIds.buf)[index];
    }
    return -1;
}

bool lookup(void* repo, std::uint32_t id, std::uint32_t hash, View* out) {
    void* pools[] = {field<void*>(repo, kTranslatedStrings),
                     field<void*>(repo, kVersionedFallbackPool),
                     field<void*>(repo, kFallbackPool)};
    for (void* pool : pools) {
        if (pool == nullptr) continue;
        const int index = find_index(pool, id, hash);
        if (index < 0) continue;
        *out = static_cast<View*>(field<StaticArrayView>(pool, kValues).buf)[index];
        return true;
    }
    return false;
}

// The repository, once the copyright handle resolves in it. Before the
// module's localisation is loaded it does not, and this says so quietly.
void* repository() {
    static std::atomic<void*> verified{nullptr};
    if (void* repo = verified.load()) return repo;

    void* repo = nullptr;
    auto* global = reinterpret_cast<void* const*>(load_bias() + kRepositoryGlobal);
    if (!safe_read(global, &repo, sizeof(repo)) || repo == nullptr) return nullptr;

    void* pool = nullptr;
    if (!safe_read(static_cast<char*>(repo) + kTranslatedStrings, &pool,
                   sizeof(pool)) || pool == nullptr) {
        return nullptr;
    }
    std::uint32_t id = 0;
    std::uint32_t hash = 0;
    if (!handle_key(kCheckHandle, &id, &hash)) return nullptr;

    View view{};
    if (!read_lock(repo)) return nullptr;
    const bool found = lookup(repo, id, hash, &view);
    read_unlock(repo);
    if (!found || view.data == nullptr || view.size > 4096) return nullptr;

    std::string text(view.size, '\0');
    if (!safe_read(view.data, text.data(), view.size)
        || text.find("Larian Studios") == std::string::npos) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            logf("loca: the repository at image+%#lx does not resolve the "
                 "copyright line; Ext.Loca stays on bg3le's own index",
                 (unsigned long)kRepositoryGlobal);
        }
        return nullptr;
    }
    logf("loca: TranslatedStringRepository at %p", repo);
    verified.store(repo);
    return repo;
}

}  // namespace

extern "C" bool bg3le_engine_strings_install() {
    if (!bytes_match(kFixedStringCreate, kFixedStringCreatePrologue,
                     sizeof(kFixedStringCreatePrologue))
        || !bytes_match(kFixedStringCreateSeedAt, kFixedStringCreateSeed,
                        sizeof(kFixedStringCreateSeed))) {
        logf("strings: FixedString::CreateFromString not at %#lx",
             (unsigned long)kFixedStringCreate);
        return false;
    }
    g_create = reinterpret_cast<CreateProc>(load_bias() + kFixedStringCreate);
    return true;
}

// Interns through the engine, so the id is the one its own lookups find.
// The reference is never released.
extern "C" bool bg3le_fixed_string_create(char const* text, std::uint32_t* out) {
    if (g_create == nullptr || text == nullptr) return false;
    const CreateView view{text, static_cast<std::uint32_t>(std::strlen(text))};
    *out = g_create(&view);
    return *out != 0xffffffffu;
}

extern "C" bool bg3le_repository_ready() { return repository() != nullptr; }

namespace {

// The vendored FixedString's two platform hooks, which upstream points at the
// engine: bg3le's string-table reader and the engine's CreateFromString.
bg3se::LSStringView* corelib_get_string(bg3se::FixedStringBase const* fs,
                                        bg3se::LSStringView& out) {
    std::uint32_t length = 0;
    char const* text = bg3le_fixed_string(fs->Index, &length);
    out = text != nullptr ? bg3se::LSStringView(text, length)
                          : bg3se::LSStringView();
    return &out;
}

std::uint32_t corelib_create(bg3se::LSStringView const& view) {
    const std::string text(view.data(), view.size());
    std::uint32_t id = bg3se::FixedStringBase::NullIndex;
    return bg3le_fixed_string_create(text.c_str(), &id)
        ? id : bg3se::FixedStringBase::NullIndex;
}

}  // namespace

// Upstream fills these at startup and then builds GFS; without them every
// FixedString in the vendored code read as null.
extern "C" void bg3le_corelib_strings_install() {
    bg3se::gCoreLibPlatformInterface.ls__FixedString__GetString = &corelib_get_string;
    if (g_create != nullptr) {
        bg3se::gCoreLibPlatformInterface.ls__FixedString__CreateFromString =
            &corelib_create;
    }
    bg3se::GFS.Initialize();
    logf("strings: vendored FixedString hooks installed; GFS built");
}

// TranslatedStringRepository::GetTranslatedString.
bool translated_string_get(char const* handle, std::string* out) {
    void* repo = repository();
    if (repo == nullptr || handle == nullptr) return false;

    std::uint32_t id = 0;
    std::uint32_t hash = 0;
    if (!handle_key(handle, &id, &hash)) return false;

    View view{};
    if (!read_lock(repo)) return false;
    bool found = lookup(repo, id, hash, &view);
    if (found) {
        out->assign(view.size, '\0');
        found = view.data != nullptr && safe_read(view.data, out->data(), view.size);
    }
    read_unlock(repo);
    return found;
}

namespace {

// TranslatedStringRepository::UpdateTranslatedString, into a repository
// that is up. spins < 0 waits for the lock; bg3le's own thread only.
bool store(void* repo, char const* handle, char const* text, int spins) {
    std::uint32_t id = 0;
    std::uint32_t hash = 0;
    if (!handle_key(handle, &id, &hash)) return false;
    if (!write_lock(repo, spins)) return false;

    auto* value = bg3se::GameAlloc<bg3se::STDString>(text);
    const View view{value->data(), static_cast<std::uint32_t>(value->size()), 0};

    bool stored = false;
    void* pool = field<void*>(repo, kTranslatedStrings);
    auto& strings = field<ArrayView>(pool, kStrings);
    if (strings.size < strings.capacity) {
        static_cast<bg3se::STDString**>(strings.buf)[strings.size++] = value;
    }

    auto& values = field<StaticArrayView>(pool, kValues);
    const int index = find_index(pool, id, hash);
    if (index >= 0) {
        static_cast<View*>(values.buf)[index] = view;
        stored = true;
    } else {
        // HashSet::insertUnchecked without its growth paths.
        auto& hashKeys = field<StaticArrayView>(pool, kHashKeys);
        auto& nextIds = field<ArrayView>(pool, kNextIds);
        auto& keys = field<ArrayView>(pool, kKeys);
        const std::uint32_t n = keys.size;
        const std::uint32_t desired = (n + 1) + ((n + 1) >> 1);
        if (n < keys.capacity && nextIds.size == n && n < nextIds.capacity
            && n < values.size && hashKeys.size >= desired) {
            const std::uint32_t bucket = hash % hashKeys.size;
            auto* heads = static_cast<std::int32_t*>(hashKeys.buf);
            std::int32_t previous = heads[bucket];
            if (previous < 0) previous = -2 - static_cast<std::int32_t>(bucket);

            static_cast<Handle*>(keys.buf)[n] = Handle{id, 0, 0};
            static_cast<std::int32_t*>(nextIds.buf)[n] = previous;
            static_cast<View*>(values.buf)[n] = view;
            keys.size = n + 1;
            nextIds.size = n + 1;
            heads[bucket] = static_cast<std::int32_t>(n);
            stored = true;
        }
    }
    write_unlock(repo);

    if (!stored) {
        logf("loca: no room to add %s to the repository without "
             "reallocating it", handle);
    }
    return stored;
}

// Writes made before the localisation is loaded, applied the moment it is:
// upstream's run after it, which is the order the engine expects.
std::mutex g_pending_mutex;
std::vector<std::pair<std::string, std::string>> g_pending;
std::string g_version_suffix;
std::atomic<bool> g_monitor_started{false};

void apply_pending(void* repo) {
    std::vector<std::pair<std::string, std::string>> pending;
    std::string suffix;
    {
        const std::lock_guard<std::mutex> lock(g_pending_mutex);
        pending.swap(g_pending);
        suffix.swap(g_version_suffix);
    }
    if (!suffix.empty()) {
        // ecl::ScriptExtender::ShowVersionNumber.
        std::string text;
        if (translated_string_get(kCheckHandle, &text)
            && text.find("Script Extender") == std::string::npos
            && store(repo, kCheckHandle, (text + suffix).c_str(), -1)) {
            logf("loca: version line updated");
        }
    }
    for (auto const& [handle, text] : pending) {
        store(repo, handle.c_str(), text.c_str(), -1);
    }
    if (!pending.empty()) {
        logf("loca: applied %zu string(s) set before the repository loaded",
             pending.size());
    }
}

void start_monitor() {
    if (g_monitor_started.exchange(true)) return;
    std::thread([] {
        for (int i = 0; i < 36000; ++i) {
            if (void* repo = repository()) {
                logf("loca: repository populated after %d ms of waiting "
                     "(client state %s)", i * 5, client_state_name());
                apply_pending(repo);
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        logf("loca: the repository never loaded; queued strings dropped");
    }).detach();
}

}  // namespace

bool translated_string_set(char const* handle, char const* text) {
    if (handle == nullptr || text == nullptr || !bg3le_game_allocator_ready()) {
        return false;
    }
    if (void* repo = repository()) {
        {
            const std::lock_guard<std::mutex> lock(g_pending_mutex);
            if (g_pending.empty() && g_version_suffix.empty()
                && store(repo, handle, text, 2000)) {
                return true;
            }
        }
    }
    {
        const std::lock_guard<std::mutex> lock(g_pending_mutex);
        g_pending.emplace_back(handle, text);
    }
    start_monitor();
    return true;
}

// The menu line, appended once the copyright string is loaded.
void translated_string_show_version(char const* suffix) {
    {
        const std::lock_guard<std::mutex> lock(g_pending_mutex);
        g_version_suffix = suffix;
    }
    start_monitor();
}

}  // namespace bg3le
