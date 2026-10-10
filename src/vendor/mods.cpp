// Finds the engine's module list, which is what Ext.Mod reads.
//
// The mod manager hangs off esv::EoCServer upstream, reached through a
// symbol bg3se recovers by pattern-scanning a Windows image. The Linux build
// names neither: no EoCServer, no ModManager, nothing for LoadOrderedModules.
//
// It does not need one. Every install has the base module "Shared", its UUID
// is the constant ed539163-bb70-431b-96a7-f5b2eda5376b, and it is first in
// load order -- so sixteen known bytes locate Module[0] directly. That is an
// exact-value scan rather than a structural guess, which is the distinction
// that decided every other search in this project: a structural test
// ("an array of named things") matches the wrong array, a known value does
// not.
//
// ModuleInfo's layout is the Windows struct with one substitution. Larian's
// string is sixteen bytes here, not std::string's thirty-two, which is why
// sizeof(Module) upstream (264+) does not match the stride found in memory
// (240). Every field was then checked against a module whose values are
// known from reference/mod-shape.txt: "Shared" inline at +32 with its length
// in the sixteenth byte, four empty FixedStrings where the level names are,
// PhotoBoothLevelName resolving to SYS_PortraitGeneration_A, and the packed
// version at +72 reading back as exactly 1.0.233.3395918.

#include <stdafx.h>

#include <GameDefinitions/Module.h>

#include <atomic>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "ls_string.h"
#include "mods.h"

#include "../hook.h"
#include "../log.h"
#include "../mem.h"
#include "../targets.h"

extern "C" bool bg3le_scannable_region(char const* line,
                                       unsigned long long* from,
                                       unsigned long long* to);

namespace bg3le {

extern "C" char const* bg3le_fixed_string(std::uint32_t index,
                                          std::uint32_t* length);
extern "C" bool bg3le_meta_parse_guid(char const* text, void* out);
extern "C" void* bg3le_static_get(char const* key, std::size_t which);
extern "C" std::size_t bg3le_static_count(char const* key);
extern "C" void bg3le_static_confirm(char const* key, std::size_t which);
extern "C" bool bg3le_static_record_path(char const* key, void const* target,
                                         std::uint64_t first_window);

namespace {

// The base module, present in every install.
constexpr char const* kSharedUuid = "ed539163-bb70-431b-96a7-f5b2eda5376b";

// Within Module: the vtable, then ModuleInfo, whose UUID follows a
// FixedString and its padding.
constexpr std::size_t kUuidInModule = 16;

// Within ModuleInfo: the UUID string index precedes the UUID itself.
constexpr std::size_t kUuidStringBeforeUuid = 8;

// ModuleInfo, relative to the Module. Established from known values; see the
// note at the top of the file.
constexpr std::size_t kInfoName = 32;
constexpr std::size_t kInfoStartLevel = 48;
constexpr std::size_t kInfoMenuLevel = 52;
constexpr std::size_t kInfoLobbyLevel = 56;
constexpr std::size_t kInfoCharCreationLevel = 60;
constexpr std::size_t kInfoPhotoBoothLevel = 64;
constexpr std::size_t kInfoModVersion = 72;
constexpr std::size_t kInfoPublishVersion = 80;
constexpr std::size_t kInfoHash = 88;
constexpr std::size_t kInfoDirectory = 104;
constexpr std::size_t kInfoAuthor = 136;
constexpr std::size_t kInfoDescription = 152;
constexpr std::size_t kInfoNumPlayers = 168;
constexpr std::size_t kInfoFileSize = 176;
constexpr std::size_t kInfoPublishHandle = 184;

// Module's three ModuleShortDesc arrays, which follow ModuleInfo.
constexpr std::size_t kModuleLists[3] = {192, 208, 224};

// ModuleShortDesc, same substitution applied to the upstream struct.
constexpr std::size_t kDescUuidString = 0;
constexpr std::size_t kDescName = 24;
constexpr std::size_t kDescModVersion = 40;
constexpr std::size_t kDescPublishVersion = 48;
constexpr std::size_t kDescHash = 56;
constexpr std::size_t kDescFolder = 72;
constexpr std::size_t kDescPublishHandle = 88;
constexpr std::size_t kDescStride = 96;

// An Array<T> header: buffer, capacity, size.
constexpr std::size_t kArrayHeader = 16;

// Around LoadOrderedModules inside ModManager. BaseModule is the member just
// ahead of it -- one module plus the two flag bytes' padding -- and
// AvailableMods the array just after. Both were confirmed against the
// running game: header-248 names the campaign module, and header+16 is a
// sixteen-entry module array where the load order holds thirteen.
constexpr std::size_t kBaseModuleBeforeLoadOrder = 248;
constexpr std::size_t kAvailableAfterLoadOrder = kArrayHeader;

// ModManager::Settings.Mods, from the same header.
//
// Derived rather than searched for. bg3se declares the members in order:
// LoadOrderedModules, AvailableMods, a HashMap<uint64_t, void*>, a uint64,
// then ModuleSettings, which is a vtable and an Array<ModuleShortDesc>. That
// is 16 for AvailableMods, 64 for the hash map -- a HashSet's three members
// are 48 and the value array is 16 -- 8 for the spare word and 8 for the
// vtable: 112.
//
// Confirmed against the running game before it was trusted: the array at
// +112 holds 29 descriptors whose names read as GustavX, 5eSpells, PHB
// Additional Spells and so on, and 29 authored mods plus the 14 base modules
// is the 43 the load order holds.
constexpr std::size_t kSettingsModsAfterLoadOrder = 112;

template <class T>
bool read_as(void const* addr, T* out) {
    return safe_read(addr, out, sizeof(T));
}

struct Modules {
    void const* Buffer{nullptr};
    std::size_t Count{0};
    std::size_t Stride{0};
};

struct Manager {
    Modules LoadOrder;
    Modules Available;
    void const* BaseModule{nullptr};
    // ModManager::LoadOrderedModules' array header. The arrays above are a
    // snapshot; the header is where the engine keeps the live one.
    unsigned long long Header{0};
};

Manager& state() {
    static Manager m;
    return m;
}

// A module's UUID as text, from its own UUIDString index rather than by
// formatting the bytes, so a mismatch between the two shows up as a failure
// instead of being papered over.
char const* uuid_string_at(void const* module) {
    std::uint32_t index = 0;
    auto const* at = (char const*)module + kUuidInModule
                     - kUuidStringBeforeUuid;
    if (!read_as(at, &index)) return nullptr;
    return bg3le_fixed_string(index, nullptr);
}

// Whether every entry at this stride carries a UUID string that resolves.
// Every entry, not a sample: a 698-entry run of garbage passed a six-entry
// probe because its first sixteen slots were a real module array.
bool array_holds(void const* buffer, std::size_t count, std::size_t stride) {
    for (std::size_t i = 0; i < count; ++i) {
        char const* text = uuid_string_at((char const*)buffer + i * stride);
        if (text == nullptr) return false;
        // A UUID string is 36 characters; anything else is a coincidence.
        if (std::strlen(text) != 36) return false;
    }
    return true;
}

// The stride that makes every entry a module. sizeof(Module) as this build
// computes it is not the engine's, as the stats work showed repeatedly, so it
// is derived rather than assumed.
std::size_t derive_stride(void const* buffer, std::size_t count) {
    for (std::size_t stride = 64; stride <= 2048; stride += 8) {
        if (array_holds(buffer, count, stride)) return stride;
    }
    return 0;
}

// Walks every readable region once, handing each chunk to `visit`.
template <class Visit>
void scan_memory(Visit visit) {
    std::FILE* maps = std::fopen("/proc/self/maps", "r");
    if (maps == nullptr) return;

    constexpr std::size_t kChunk = 1u << 20;
    constexpr std::size_t kOverlap = 16;
    static std::vector<unsigned char> block;
    block.resize(kChunk + kOverlap);

    char line[512];
    while (std::fgets(line, sizeof(line), maps) != nullptr) {
        unsigned long long from = 0;
        unsigned long long to = 0;
        if (!bg3le_scannable_region(line, &from, &to)) continue;

        for (unsigned long long base = from; base < to; base += kChunk) {
            std::size_t want = (std::size_t)(to - base);
            if (want > kChunk + kOverlap) want = kChunk + kOverlap;
            const std::size_t got =
                safe_read_some((void const*)base, block.data(), want);
            scan_yield();
            if (got < kOverlap) continue;
            visit(base, block.data(), got);
        }
    }
    std::fclose(maps);
}

struct Header {
    unsigned long long At{0};
    Modules Array;
};

// Under BG3LE_DUMP_MODULE: one module's bytes annotated with anything
// recognisable in them -- resolvable FixedString indices and pointers to
// text. This is how the field offsets above were read off rather than
// predicted.
void dump_module(void const* module, std::size_t stride) {
    std::vector<unsigned char> bytes(stride);
    const std::size_t got = safe_read_some(module, bytes.data(), bytes.size());
    logf("moddump: %zu bytes at %p", got, module);
    for (std::size_t off = 0; off + 8 <= got; off += 8) {
        std::uint64_t word = 0;
        std::memcpy(&word, bytes.data() + off, sizeof(word));
        auto const lo = (std::uint32_t)(word & 0xffffffffu);

        char note[192];
        note[0] = '\0';
        char const* fs = lo != 0 ? bg3le_fixed_string(lo, nullptr) : nullptr;
        std::size_t used = 0;
        if (fs != nullptr) {
            used = (std::size_t)std::snprintf(note, sizeof(note),
                                              " fs=%.48s", fs);
        }
        if (word > 0x1000 && word < 0x7fffffffffffull) {
            char text[41] = {};
            if (safe_read_some((void const*)(std::uintptr_t)word, text, 40)
                > 0) {
                bool printable = text[0] >= 0x20 && text[0] < 0x7f;
                for (std::size_t i = 0; i < 40 && printable; ++i) {
                    if (text[i] == '\0') break;
                    if (text[i] < 0x20 || text[i] >= 0x7f) printable = false;
                }
                if (printable) {
                    std::snprintf(note + used, sizeof(note) - used,
                                  " ->\"%s\"", text);
                }
            }
        }
        logf("moddump: +%3zu %016llx%s", off, (unsigned long long)word, note);
    }
}

// ModManager::BaseModule, the member just ahead of LoadOrderedModules. A
// wrong offset gives an unresolvable uuid, so it is checked before it is
// kept.
void const* base_module_before(unsigned long long header) {
    if (header < kBaseModuleBeforeLoadOrder) return nullptr;
    auto const* at = (char const*)(std::uintptr_t)(
        header - kBaseModuleBeforeLoadOrder);
    char const* uuid = uuid_string_at(at);
    if (uuid == nullptr || std::strlen(uuid) != 36) return nullptr;
    return at;
}

// ModManager::AvailableMods, the array just after LoadOrderedModules.
Modules available_after(unsigned long long header, std::size_t stride) {
    std::uint64_t buffer = 0;
    std::uint32_t capacity = 0;
    std::uint32_t size = 0;
    auto const* at = (char const*)(std::uintptr_t)(
        header + kAvailableAfterLoadOrder);
    if (!read_as(at, &buffer) || !read_as(at + 8, &capacity)
        || !read_as(at + 12, &size)) {
        return Modules{};
    }
    if (size == 0 || size > 4096 || size > capacity) return Modules{};

    auto const* mods = (void const*)(std::uintptr_t)buffer;
    if (!array_holds(mods, size, stride)) return Modules{};
    return Modules{mods, size, stride};
}

// The keys the paths from a static to each manager's array header are
// recorded under. There are two managers, client and server, and which of
// them holds the session's mods depends on when you look, so both are
// recorded and the fuller one wins.
constexpr char const* kStaticKeys[] = {"mods.loadorder", "mods.loadorder.2"};

// Builds the manager from an address claimed to be ModManager's
// LoadOrderedModules array header. Shared between the scan and the
// recorded static so neither can adopt something the other would reject.
bool adopt(unsigned long long header) {
    std::uint64_t buffer = 0;
    std::uint32_t capacity = 0;
    std::uint32_t size = 0;
    auto const* at = (char const*)(std::uintptr_t)header;
    if (!read_as(at, &buffer) || !read_as(at + 8, &capacity)
        || !read_as(at + 12, &size)) {
        return false;
    }
    if (size == 0 || size > 4096 || size > capacity) return false;

    auto const* mods = (void const*)(std::uintptr_t)buffer;
    const std::size_t stride = derive_stride(mods, size);
    if (stride == 0) return false;

    Manager m{};
    m.LoadOrder = Modules{mods, size, stride};
    m.BaseModule = base_module_before(header);
    m.Available = available_after(header, stride);
    if (m.BaseModule == nullptr || m.Available.Buffer == nullptr) return false;
    m.Header = header;

    state() = m;
    return true;
}

// Re-reads the arrays from the header the search found.
//
// The load order is not fixed for the run: the engine builds it when a
// level or savegame loads, and reallocates the array as it goes. Holding
// the buffer from startup meant Ext.Mod reported the menu's thirteen base
// modules for the whole session, and every mod the save brought in was
// invisible -- to Ext.Mod, and to anything keyed off it, which includes
// which mod scripts bg3le loads.
void refresh() {
    Manager& m = state();
    if (m.Header == 0) return;

    std::uint64_t buffer = 0;
    std::uint32_t capacity = 0;
    std::uint32_t size = 0;
    auto const* at = (char const*)(std::uintptr_t)m.Header;
    if (!read_as(at, &buffer) || !read_as(at + 8, &capacity)
        || !read_as(at + 12, &size)) {
        return;
    }
    if (size == 0 || size > 4096 || size > capacity) return;

    auto const* mods = (void const*)(std::uintptr_t)buffer;
    if (mods == m.LoadOrder.Buffer && size == m.LoadOrder.Count) return;

    // The stride does not change within a build, so only the ends are
    // checked -- a full validation here would run on every Ext.Mod call.
    if (!array_holds(mods, 1, m.LoadOrder.Stride)) return;
    if (!array_holds((char const*)mods + (size - 1) * m.LoadOrder.Stride, 1,
                     m.LoadOrder.Stride)) {
        return;
    }

    m.LoadOrder = Modules{mods, size, m.LoadOrder.Stride};
    m.Available = available_after(m.Header, m.LoadOrder.Stride);
    m.BaseModule = base_module_before(m.Header);
    logf("mods: load order now holds %zu modules", m.LoadOrder.Count);
}

// The manager from the pointer chain a previous run recorded, so nothing
// is scanned. Each candidate is validated exactly as a scanned one is.
bool search_from_statics() {
    unsigned long long best = 0;
    std::size_t bestCount = 0;
    std::size_t recorded = 0;

    for (char const* key : kStaticKeys) {
        const std::size_t count = bg3le_static_count(key);
        recorded += count;
        for (std::size_t i = 0; i < count; ++i) {
            void* header = bg3le_static_get(key, i);
            if (header == nullptr) continue;
            const auto at = (unsigned long long)(std::uintptr_t)header;
            if (!adopt(at)) continue;
            bg3le_static_confirm(key, i);
            if (state().LoadOrder.Count > bestCount) {
                bestCount = state().LoadOrder.Count;
                best = at;
            }
            break;
        }
    }

    if (best == 0) {
        if (recorded != 0) {
            logf("mods: none of the %zu recorded statics reaches a load "
                 "order; scanning", recorded);
        }
        return false;
    }

    // adopt() left the state at whichever manager was tried last.
    if (!adopt(best)) return false;
    logf("mods: %zu modules at %p without scanning, from a recorded static",
         state().LoadOrder.Count, state().LoadOrder.Buffer);
    return true;
}

// Upstream's GetModManagerClient: ecl::EoCClient::ModManager. This build's
// EoCClient drifts from the vendored one, so the member, and where the load
// order sits in the manager, are found once by adopting what they lead to.
bool search_from_client() {
    static std::size_t member = 0;
    static std::size_t delta = 0;
    static bool logged = false;
    if (target::EoCClient() == 0) return false;
    void* client = nullptr;
    if (!read_as((void const*)(load_bias() + target::EoCClient()), &client) || client == nullptr) {
        return false;
    }
    auto manager_at = [&](std::size_t at) -> std::uint64_t {
        std::uint64_t manager = 0;
        return read_as((char const*)client + at, &manager) && manager > 0x10000 ? manager : 0;
    };
    if (member != 0) {
        const std::uint64_t manager = manager_at(member);
        return manager != 0 && adopt(manager + delta);
    }
    static std::chrono::steady_clock::time_point last{};
    const auto now = std::chrono::steady_clock::now();
    if (now - last < std::chrono::milliseconds(250)) return false;
    last = now;
    // ModManager sits ahead of EntityWorld (+0x1a0 here); the load order is
    // BaseModule's 248 bytes past the manager's vtable and two signals.
    for (std::size_t at = 0x100; at < 0x1a0; at += 8) {
        const std::uint64_t manager = manager_at(at);
        if (manager == 0) continue;
        for (std::size_t d = kBaseModuleBeforeLoadOrder + 8; d < kBaseModuleBeforeLoadOrder + 0x100; d += 8) {
            if (!adopt(manager + d)) continue;
            member = at;
            delta = d;
            if (!logged) {
                logged = true;
                logf("mods: ecl::EoCClient keeps its ModManager at +%#zx, the load order at +%#zx in it",
                     at, d);
            }
            return true;
        }
    }
    return false;
}

bool search() {
    std::uint8_t needle[16] = {};
    if (!bg3le_meta_parse_guid(kSharedUuid, needle)) {
        logf("mods: could not parse the base module's uuid");
        return false;
    }

    // Pass one: every place the base module's guid appears. Each is a
    // candidate Module, since the guid sits at a known offset within one.
    std::vector<std::uint64_t> candidates;
    scan_memory([&](unsigned long long base, unsigned char const* block,
                    std::size_t got) {
        const std::size_t last = got - sizeof(needle);
        for (std::size_t off = 0; off <= last; off += 4) {
            if (std::memcmp(block + off, needle, sizeof(needle)) != 0) continue;
            const unsigned long long guidAddr = base + off;
            if (guidAddr < kUuidInModule) continue;
            candidates.push_back(guidAddr - kUuidInModule);
        }
    });

    if (candidates.empty()) {
        logf("mods: the base module's uuid is nowhere in memory yet");
        return false;
    }
    std::sort(candidates.begin(), candidates.end());
    candidates.erase(std::unique(candidates.begin(), candidates.end()),
                     candidates.end());

    // Pass two: array headers whose buffer is one of them. One scan for all
    // candidates rather than one scan each -- the latter took over a minute,
    // which put it ahead of the module list being built.
    std::vector<Header> headers;
    scan_memory([&](unsigned long long base, unsigned char const* block,
                    std::size_t got) {
        for (std::size_t off = 0; off + kArrayHeader <= got; off += 8) {
            std::uint64_t word = 0;
            std::memcpy(&word, block + off, sizeof(word));
            if (!std::binary_search(candidates.begin(), candidates.end(),
                                    word)) {
                continue;
            }

            std::uint32_t capacity = 0;
            std::uint32_t size = 0;
            std::memcpy(&capacity, block + off + 8, sizeof(capacity));
            std::memcpy(&size, block + off + 12, sizeof(size));
            if (size == 0 || size > 4096 || size > capacity) continue;

            auto const* buffer = (void const*)(std::uintptr_t)word;
            const std::size_t stride = derive_stride(buffer, size);
            if (stride == 0) continue;

            headers.push_back(Header{base + off, Modules{buffer, size,
                                                         stride}});
        }
    });

    if (headers.empty()) {
        logf("mods: the base module's uuid appeared at %zu places but none of "
             "them sat in a module array yet", candidates.size());
        return false;
    }

    // A header is the load order if the rest of ModManager reads correctly
    // around it: BaseModule a module back, AvailableMods an array on. That
    // is what tells LoadOrderedModules apart from AvailableMods itself --
    // both are Array<Module> in the same object, GetLoadOrder means the
    // loaded ones, and picking whichever the scan reached first would
    // silently return the wrong list.
    // Every header that validates, not the first.
    //
    // There are two mod managers, client and server, and which of them is
    // populated depends on when the search runs: at the main menu only one
    // exists, and the server's list is built when a level loads. Taking
    // the first meant that after a save came up Ext.Mod still reported the
    // menu's base modules, and every mod the game had loaded was missing.
    Header const* chosen = nullptr;
    std::size_t best = 0;
    for (Header const& h : headers) {
        if (!adopt(h.At)) continue;
        const std::size_t count = state().LoadOrder.Count;
        logf("mods: header %#llx validates with %zu modules", h.At, count);
        if (count > best) {
            best = count;
            chosen = &h;
        }
    }
    // adopt() left the state at whichever header was tried last.
    if (chosen != nullptr && !adopt(chosen->At)) chosen = nullptr;

    // Nothing validated as a whole manager, so nothing is adopted. An
    // earlier version fell back to the first header on the grounds that
    // GetLoadOrder works without the rest; it accepted a four-entry
    // coincidence at stride 336, and because a non-null buffer stops the
    // retry, that wrong answer stuck for the rest of the run and took
    // Ext.Stats' ModId down with it. Waiting is better than guessing.
    if (chosen == nullptr) {
        logf("mods: %zu candidate headers, none with a mod manager around "
             "it yet", headers.size());
        return false;
    }

    Manager const found = state();
    char const* baseUuid = found.BaseModule != nullptr
                               ? uuid_string_at(found.BaseModule)
                               : nullptr;

    // Recorded so the next run reads the engine's own pointer. The window
    // covers ModManager, since a static points at the object rather than
    // at the array partway into it.
    constexpr std::uint64_t kManagerWindow = 1u << 16;
    std::size_t recorded = 0;
    for (Header const& h : headers) {
        if (recorded >= std::size(kStaticKeys)) break;
        if (!adopt(h.At)) continue;
        bg3le_static_record_path(kStaticKeys[recorded],
                                 (void const*)(std::uintptr_t)h.At,
                                 kManagerWindow);
        ++recorded;
    }
    adopt(chosen->At);
    logf("mods: %zu modules at %p, stride %zu (header at %#llx, %zu candidate "
         "headers); base module %s, %zu available", chosen->Array.Count,
         chosen->Array.Buffer, chosen->Array.Stride, chosen->At,
         headers.size(), baseUuid != nullptr ? baseUuid : "(not found)",
         found.Available.Count);

    if (std::getenv("BG3LE_DUMP_MODULE") != nullptr) {
        for (Header const& h : headers) {
            logf("mods: header %#llx -> %p, %zu entries, stride %zu%s", h.At,
                 h.Array.Buffer, h.Array.Count, h.Array.Stride,
                 &h == chosen ? " (chosen)" : "");
        }
        for (std::size_t i = 0; i < chosen->Array.Count; ++i) {
            char const* text = uuid_string_at(
                (char const*)chosen->Array.Buffer + i * chosen->Array.Stride);
            logf("mods:   %2zu %s", i, text != nullptr ? text : "(unresolved)");
        }
        // ModManager::Settings.Mods -- the load order as parsed from
        // modsettings.lsx, before the engine decided what to load. The
        // offset is not assumed: the array is found by looking for one
        // whose entries read as module descriptors. Comparing its length
        // with the loaded count tells a rejected mod from one the engine
        // never read.
        for (std::size_t at = kArrayHeader; at <= 512; at += 8) {
            std::uint64_t buffer = 0;
            std::uint32_t capacity = 0;
            std::uint32_t size = 0;
            auto const* head = (char const*)(std::uintptr_t)(chosen->At + at);
            if (!read_as(head, &buffer) || !read_as(head + 8, &capacity)
                || !read_as(head + 12, &size)) {
                continue;
            }
            if (buffer == 0 || size == 0 || size > 4096 || size > capacity) {
                continue;
            }

            auto const uuid_of = [&](std::size_t i) -> char const* {
                std::uint32_t index = 0;
                auto const* desc =
                    (char const*)(std::uintptr_t)buffer + i * kDescStride;
                if (!read_as(desc + kDescUuidString, &index)) return nullptr;
                return bg3le_fixed_string(index, nullptr);
            };

            std::size_t named = 0;
            for (std::size_t i = 0; i < size; ++i) {
                char const* uuid = uuid_of(i);
                if (uuid == nullptr || std::strlen(uuid) != 36) break;
                ++named;
            }
            if (named != size) continue;

            logf("mods: descriptor array at +%zu holds %u entries (cap %u)",
                 at, size, capacity);
            for (std::size_t i = 0; i < size && i < 4; ++i) {
                logf("mods:   +%zu[%zu] %s", at, i, uuid_of(i));
            }
        }

        // And the mods the engine found but did not load, which is what
        // tells a load order the engine rejected from one it never read.
        for (std::size_t i = 0; i < found.Available.Count; ++i) {
            auto const* module = (char const*)found.Available.Buffer
                                 + i * found.Available.Stride;
            char const* text = uuid_string_at(module);
            ModInfo info{};
            const bool ok = bg3le_mod_info(module, &info);
            logf("mods: available %2zu %s %s", i,
                 text != nullptr ? text : "(unresolved)",
                 ok && info.Name != nullptr ? info.Name : "");
        }
        dump_module(chosen->Array.Buffer, chosen->Array.Stride);
    }
    return true;
}

// Retried rather than latched: an early failure only means the engine has not
// built the load order yet. The warm thread calls this on a timer, so a cap
// keeps a genuinely absent module list from rescanning memory forever.
// Set when the game has loaded a level: the manager the menu had is not
// necessarily the one that now holds the mods.
std::atomic<bool> g_rescan{false};

// Every manager bg3le has a pointer to, and what each holds right now.
//
// There are two -- client and server -- and the load order is not fixed for
// the run: the engine builds it as a level loads and rebuilds it. One reached
// 69 modules during a load and 43 by the end of it, which is the whole reason
// Ext.Mod.GetLoadOrder merges modsettings.lsx over the top. This says which
// manager is which and what each one holds, so the question can be settled
// from data rather than from the one bg3le happened to adopt.
extern "C" std::size_t bg3le_mods_manager_dump(
    void (*report)(void* ctx, char const* key, unsigned long long header,
                   std::size_t count, void const* buffer, bool chosen),
    void* ctx) {
    if (report == nullptr) return 0;

    std::size_t seen = 0;
    for (char const* key : kStaticKeys) {
        const std::size_t count = bg3le_static_count(key);
        for (std::size_t i = 0; i < count; ++i) {
            void* header = bg3le_static_get(key, i);
            if (header == nullptr) continue;
            const auto at = (unsigned long long)(std::uintptr_t)header;

            std::uint64_t buffer = 0;
            std::uint32_t capacity = 0;
            std::uint32_t size = 0;
            auto const* head = (char const*)(std::uintptr_t)at;
            if (!read_as(head, &buffer) || !read_as(head + 8, &capacity)
                || !read_as(head + 12, &size)) {
                continue;
            }
            if (size > 4096 || size > capacity) continue;

            ++seen;
            report(ctx, key, at, size,
                   (void const*)(std::uintptr_t)buffer,
                   at == state().Header);
        }
    }
    return seen;
}

// One module's uuid out of a manager named by its header, so the two can be
// compared entry by entry without adopting either.
extern "C" char const* bg3le_mods_manager_uuid_at(unsigned long long header,
                                                  std::size_t index) {
    std::uint64_t buffer = 0;
    std::uint32_t capacity = 0;
    std::uint32_t size = 0;
    auto const* head = (char const*)(std::uintptr_t)header;
    if (!read_as(head, &buffer) || !read_as(head + 8, &capacity)
        || !read_as(head + 12, &size)) {
        return nullptr;
    }
    if (index >= size || size > capacity) return nullptr;

    const std::size_t stride = state().LoadOrder.Stride;
    if (stride == 0) return nullptr;
    return uuid_string_at((char const*)(std::uintptr_t)buffer
                          + index * stride);
}

bool ready() {
    static int attempts = 0;

    // A rescan re-resolves from the recorded pointers and keeps whichever
    // manager holds more modules. It deliberately does not fall back to a
    // scan: this runs on the story thread during level load, and a scan
    // there is fifteen seconds the player waits through.
    if (g_rescan.exchange(false)) {
        Manager const previous = state();
        if (!search_from_statics()
            || state().LoadOrder.Count < previous.LoadOrder.Count) {
            state() = previous;
        }
        refresh();
        if (state().LoadOrder.Buffer != nullptr) return true;
    }
    if (state().LoadOrder.Buffer != nullptr) {
        refresh();
        return true;
    }

    // Neither reading EoCClient nor resolving a recorded pointer is a scan,
    // so any thread may do them.
    if (search_from_client()) return true;
    if (search_from_statics()) return true;

    // Only the warming thread scans; see mem.h.
    if (!scan_allowed()) return false;
    if (attempts >= 40) return false;
    ++attempts;
    return search();
}

void const* module_at(Modules const& m, std::size_t index) {
    if (index >= m.Count) return nullptr;
    return (char const*)m.Buffer + index * m.Stride;
}

void const* module_at(std::size_t index) {
    if (!ready()) return nullptr;
    return module_at(state().LoadOrder, index);
}

// Storage for the strings a ModInfo points at, one slot per field so a
// filled struct stays wholly readable until the next call.
char const* hold(int slot, std::string const& text) {
    static thread_local std::string slots[12];
    slots[slot] = text;
    return slots[slot].c_str();
}

char const* read_string_field(void const* module, std::size_t offset,
                              int slot) {
    std::string text;
    if (!read_ls_string((char const*)module + offset, &text)) return "";
    return hold(slot, text);
}

char const* read_fixed_field(void const* module, std::size_t offset) {
    std::uint32_t index = 0;
    if (!read_as((char const*)module + offset, &index)) return "";
    char const* text = bg3le_fixed_string(index, nullptr);
    return text != nullptr ? text : "";
}

// Version is one packed uint64 upstream; unpacked here into the four numbers
// the public API reports.
void read_version(void const* module, std::size_t offset,
                  std::uint32_t out[4]) {
    std::uint64_t packed = 0;
    read_as((char const*)module + offset, &packed);
    out[0] = (std::uint32_t)(packed >> 55);
    out[1] = (std::uint32_t)((packed >> 47) & 0xff);
    out[2] = (std::uint32_t)((packed >> 31) & 0xffff);
    out[3] = (std::uint32_t)(packed & 0x7fffffffull);
}

}  // namespace

// Called when a level or savegame has loaded. The next query re-finds the
// manager rather than trusting the one the main menu had.
extern "C" void bg3le_mods_rescan() {
    g_rescan.store(true);
}

extern "C" std::size_t bg3le_mods_count() {
    return ready() ? state().LoadOrder.Count : 0;
}

extern "C" std::size_t bg3le_mods_available_count() {
    return ready() ? state().Available.Count : 0;
}

extern "C" void* bg3le_mods_available_at(std::size_t index) {
    if (!ready()) return nullptr;
    return (void*)module_at(state().Available, index);
}

extern "C" char const* bg3le_mods_uuid_at(std::size_t index) {
    void const* module = module_at(index);
    return module != nullptr ? uuid_string_at(module) : nullptr;
}

extern "C" void* bg3le_mods_at(std::size_t index) {
    return (void*)module_at(index);
}

// The module whose uuid matches, or null.
extern "C" void* bg3le_mods_find(char const* uuid) {
    if (uuid == nullptr || !ready()) return nullptr;
    const std::size_t n = state().LoadOrder.Count;
    for (std::size_t i = 0; i < n; ++i) {
        char const* text = bg3le_mods_uuid_at(i);
        if (text != nullptr && std::strcmp(text, uuid) == 0) {
            return (void*)module_at(i);
        }
    }
    return nullptr;
}

// ModManager::BaseModule, which is the campaign rather than the first module
// in load order -- upstream's GetBaseMod returns that member, so this does.
extern "C" void* bg3le_mods_base() {
    if (!ready()) return nullptr;
    return (void*)state().BaseModule;
}

extern "C" bool bg3le_mod_info(void const* module, ModInfo* out) {
    if (module == nullptr || out == nullptr) return false;

    char const* uuid = uuid_string_at(module);
    if (uuid == nullptr) return false;
    out->ModuleUUIDString = uuid;

    out->Name = read_string_field(module, kInfoName, 0);
    out->Directory = read_string_field(module, kInfoDirectory, 1);
    out->Hash = read_string_field(module, kInfoHash, 2);
    out->Author = read_string_field(module, kInfoAuthor, 3);
    out->Description = read_string_field(module, kInfoDescription, 4);

    out->StartLevelName = read_fixed_field(module, kInfoStartLevel);
    out->MenuLevelName = read_fixed_field(module, kInfoMenuLevel);
    out->LobbyLevelName = read_fixed_field(module, kInfoLobbyLevel);
    out->CharacterCreationLevelName =
        read_fixed_field(module, kInfoCharCreationLevel);
    out->PhotoBoothLevelName = read_fixed_field(module, kInfoPhotoBoothLevel);

    read_version(module, kInfoModVersion, out->ModVersion);
    read_version(module, kInfoPublishVersion, out->PublishVersion);

    out->NumPlayers = 0;
    read_as((char const*)module + kInfoNumPlayers, &out->NumPlayers);
    out->FileSize = 0;
    read_as((char const*)module + kInfoFileSize, &out->FileSize);
    out->PublishHandle = 0;
    read_as((char const*)module + kInfoPublishHandle, &out->PublishHandle);
    return true;
}

extern "C" std::size_t bg3le_mod_list_count(void const* module, int list) {
    if (module == nullptr || list < 0 || list > 2) return 0;
    std::uint32_t size = 0;
    if (!read_as((char const*)module + kModuleLists[list] + 12, &size)) {
        return 0;
    }
    return size > 4096 ? 0 : size;
}

// One ModuleShortDesc, wherever it sits. Shared by a module's own dependency
// lists and by ModManager::Settings.Mods, so neither can decode it its own
// way.
bool read_short_desc(char const* desc, ModShortDesc* out) {
    char const* uuid = read_fixed_field(desc, kDescUuidString);
    // A short desc that does not name a module means the stride is wrong;
    // reporting nothing beats reporting the bytes that follow it.
    if (std::strlen(uuid) != 36) return false;
    out->ModuleUUIDString = uuid;

    out->Name = read_string_field(desc, kDescName, 5);
    out->Folder = read_string_field(desc, kDescFolder, 6);
    out->Hash = read_string_field(desc, kDescHash, 7);
    read_version(desc, kDescModVersion, out->ModVersion);
    read_version(desc, kDescPublishVersion, out->PublishVersion);
    out->PublishHandle = 0;
    read_as(desc + kDescPublishHandle, &out->PublishHandle);
    return true;
}

extern "C" bool bg3le_mod_list_at(void const* module, int list,
                                  std::size_t index, ModShortDesc* out) {
    if (out == nullptr || index >= bg3le_mod_list_count(module, list)) {
        return false;
    }

    std::uint64_t buffer = 0;
    if (!read_as((char const*)module + kModuleLists[list], &buffer)) {
        return false;
    }
    if (buffer == 0) return false;
    return read_short_desc((char const*)(std::uintptr_t)buffer
                               + index * kDescStride,
                           out);
}

// ModManager::Settings.Mods -- the mod list as the session actually has it,
// which for a loaded save is the list the save recorded. Distinct from the
// load order, which adds the base modules, and from modsettings.lsx, which is
// what was enabled the last time the menu wrote it.
extern "C" std::size_t bg3le_mods_settings_count() {
    if (!ready() || state().Header == 0) return 0;

    auto const* head = (char const*)(std::uintptr_t)(
        state().Header + kSettingsModsAfterLoadOrder);
    std::uint32_t capacity = 0;
    std::uint32_t size = 0;
    if (!read_as(head + 8, &capacity) || !read_as(head + 12, &size)) return 0;
    if (size > 4096 || size > capacity) return 0;
    return size;
}

extern "C" bool bg3le_mods_settings_at(std::size_t index, ModShortDesc* out) {
    if (out == nullptr || index >= bg3le_mods_settings_count()) return false;

    auto const* head = (char const*)(std::uintptr_t)(
        state().Header + kSettingsModsAfterLoadOrder);
    std::uint64_t buffer = 0;
    if (!read_as(head, &buffer) || buffer == 0) return false;

    return read_short_desc((char const*)(std::uintptr_t)buffer
                               + index * kDescStride,
                           out);
}

}  // namespace bg3le
