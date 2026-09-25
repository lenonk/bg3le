// Ext.Resource: the engine's ls::ResourceManager, read as upstream's
// GetCurrentResourceBank and ResourceBank::GetResource read it (bg3se, by
// Norbyte and the bg3se contributors). The layouts are this build's, and
// everything found by address is checked against its own contents first.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "log.h"
#include "mem.h"

namespace bg3le {
namespace {

// Where ls::gGlobalResourceManager sat in this build, relative to the
// executable's first mapping. Tried first, and searched for if it disagrees.
constexpr std::uintptr_t kRecordedGlobal = 0x7d473a8;

// ResourceManager: 0x30 bytes, PreviewResources, then ResourceBanks[2].
constexpr std::uintptr_t kResourceBanks = 0x50;
// ResourceBank: VMT, Container { VMT, Banks[34] }, Packages (LegacyMap).
constexpr std::uintptr_t kContainerBanks = 0x10;
constexpr std::uint32_t kBankCount = 34;
constexpr std::uintptr_t kPackagesCount = 0x130;
// Bank: VMT, Resources (LegacyMap), an rwlock, BankTypeId.
constexpr std::uintptr_t kResourcesHashSize = 0x08;
constexpr std::uintptr_t kResourcesTable = 0x10;
constexpr std::uintptr_t kResourcesCount = 0x18;
constexpr std::uintptr_t kBankTypeId = 0x58;
// LegacyMap node: Next, FixedString key, value.
constexpr std::uintptr_t kNodeKey = 0x08;
constexpr std::uintptr_t kNodeValue = 0x10;

template <class T>
bool peek(std::uintptr_t addr, T* out) {
    return addr >= 0x10000 && safe_read(reinterpret_cast<void const*>(addr), out, sizeof(T));
}

struct Image {
    std::uintptr_t From = 0;
    std::uintptr_t To = 0;
    std::vector<std::pair<std::uintptr_t, std::uintptr_t>> Writable;
};

// The executable's mappings, and the writable ones from its data up to the
// end of the anonymous .bss after it.
Image const& image() {
    static Image found = [] {
        Image r;
        std::FILE* maps = std::fopen("/proc/self/maps", "r");
        if (maps == nullptr) return r;
        char line[512];
        std::uintptr_t lastEnd = 0;
        while (std::fgets(line, sizeof(line), maps) != nullptr) {
            unsigned long long from = 0, to = 0;
            char perms[8] = {};
            if (std::sscanf(line, "%llx-%llx %7s", &from, &to, perms) != 3) continue;
            const bool ours = std::strstr(line, "/bin/bg3\n") != nullptr;
            const bool anonymous = std::strchr(line, '/') == nullptr && std::strchr(line, '[') == nullptr;
            if (ours) {
                if (r.From == 0 || from < r.From) r.From = from;
                if (to > r.To) r.To = to;
            }
            if (perms[1] == 'w' && (ours || (anonymous && from == lastEnd && lastEnd != 0))) {
                r.Writable.emplace_back(from, to);
                lastEnd = to;
            } else if (ours) {
                lastEnd = to;
            }
        }
        std::fclose(maps);
        return r;
    }();
    return found;
}

bool in_image(std::uintptr_t p) { return p >= image().From && p < image().To; }

// Whether this is a ResourceBank: two vtables, then banks that each hold
// their own slot index as BankTypeId.
bool looks_like_resource_bank(std::uintptr_t rb) {
    std::uintptr_t vmt = 0, containerVmt = 0;
    if (!peek(rb, &vmt) || !peek(rb + 8, &containerVmt)) return false;
    if (!in_image(vmt) || !in_image(containerVmt)) return false;
    std::uint32_t agreed = 0;
    for (std::uint32_t i = 0; i < kBankCount; ++i) {
        std::uintptr_t bank = 0;
        if (!peek(rb + kContainerBanks + i * 8, &bank)) return false;
        if (bank == 0) continue;
        std::uint32_t type = 0;
        if (!peek(bank + kBankTypeId, &type) || type != i) return false;
        ++agreed;
    }
    return agreed >= kBankCount / 2;
}

bool looks_like_manager(std::uintptr_t mgr) {
    std::uintptr_t first = 0;
    return peek(mgr + kResourceBanks, &first) && looks_like_resource_bank(first);
}

// The global that holds the manager: the recorded one if it agrees, else
// whatever qword in the executable's data points at something that does.
std::uintptr_t find_global() {
    const std::uintptr_t recorded = image().From + kRecordedGlobal;
    std::uintptr_t mgr = 0;
    if (image().From != 0 && peek(recorded, &mgr) && looks_like_manager(mgr)) return recorded;

    // Read a chunk at a time: one safe_read per qword would be millions.
    constexpr std::size_t kChunk = 1 << 16;
    std::vector<std::uintptr_t> words(kChunk / 8);
    for (auto const& [from, to] : image().Writable) {
        for (std::uintptr_t base = from; base < to; base += kChunk) {
            const std::size_t want = to - base < kChunk ? to - base : kChunk;
            const std::size_t got = safe_read_some(reinterpret_cast<void const*>(base),
                                                   words.data(), want) / 8;
            for (std::size_t i = 0; i < got; ++i) {
                const std::uintptr_t p = words[i];
                if (p < 0x10000 || in_image(p) || (p & 7) != 0) continue;
                if (looks_like_manager(p)) {
                    const std::uintptr_t at = base + i * 8;
                    logf("resources: ResourceManager global at image+%#lx (recorded +%#lx)",
                         (unsigned long)(at - image().From), (unsigned long)kRecordedGlobal);
                    return at;
                }
            }
        }
    }
    return 0;
}

std::mutex& lock() {
    static std::mutex m;
    return m;
}

std::uintptr_t g_global = 0;
bool g_searched = false;

// Upstream's GetCurrentResourceBank: the first bank once it has packages.
std::uintptr_t current_bank() {
    if (!g_searched) {
        g_searched = true;
        g_global = find_global();
        if (g_global == 0) logf("resources: no ResourceManager found");
    }
    std::uintptr_t mgr = 0, first = 0, second = 0;
    if (g_global == 0 || !peek(g_global, &mgr) || !peek(mgr + kResourceBanks, &first)) return 0;
    std::uint32_t packages = 0;
    if (first != 0 && peek(first + kPackagesCount, &packages) && packages > 0) return first;
    return peek(mgr + kResourceBanks + 8, &second) ? second : 0;
}

// A bank's resources by FixedString index, rebuilt when its count moves.
struct Index {
    std::uint32_t Count = 0;
    std::unordered_map<std::uint32_t, std::uintptr_t> ByKey;
    std::vector<std::uint32_t> Keys;
};

Index const* bank_index(std::uint32_t type) {
    if (type >= kBankCount) return nullptr;
    const std::uintptr_t rb = current_bank();
    std::uintptr_t bank = 0;
    if (rb == 0 || !peek(rb + kContainerBanks + type * 8, &bank) || bank == 0) return nullptr;

    std::uint32_t hashSize = 0, count = 0;
    std::uintptr_t table = 0;
    if (!peek(bank + kResourcesHashSize, &hashSize) || !peek(bank + kResourcesTable, &table)
        || !peek(bank + kResourcesCount, &count) || hashSize > (1u << 24)) {
        return nullptr;
    }

    static std::unordered_map<std::uintptr_t, Index> indices;
    Index& index = indices[bank];
    if (index.Count == count && (count == 0 || !index.Keys.empty())) return &index;

    index = Index{};
    index.Count = count;
    std::vector<std::uintptr_t> buckets(hashSize);
    if (hashSize > 0 && table != 0
        && safe_read_some(reinterpret_cast<void const*>(table), buckets.data(),
                          hashSize * sizeof(std::uintptr_t)) != hashSize * sizeof(std::uintptr_t)) {
        return nullptr;
    }
    for (std::uintptr_t node : buckets) {
        for (std::uint32_t guard = 0; node != 0 && guard < (1u << 20); ++guard) {
            std::uintptr_t raw[3] = {};  // Next, Key, Value
            if (!safe_read(reinterpret_cast<void const*>(node), raw, sizeof(raw))) break;
            const auto key = (std::uint32_t)raw[kNodeKey / 8];
            if (index.ByKey.emplace(key, raw[kNodeValue / 8]).second) index.Keys.push_back(key);
            node = raw[0];
        }
    }
    return &index;
}

}  // namespace
}  // namespace bg3le

extern "C" void* bg3le_resource_bank_get(std::uint32_t type, std::uint32_t key) {
    const std::lock_guard<std::mutex> held(bg3le::lock());
    auto const* index = bg3le::bank_index(type);
    if (index == nullptr) return nullptr;
    auto found = index->ByKey.find(key);
    return found == index->ByKey.end() ? nullptr : reinterpret_cast<void*>(found->second);
}

// Up to max keys of a bank; returns how many it holds, or -1 without one.
extern "C" long bg3le_resource_bank_keys(std::uint32_t type, std::uint32_t* out, std::size_t max) {
    const std::lock_guard<std::mutex> held(bg3le::lock());
    auto const* index = bg3le::bank_index(type);
    if (index == nullptr) return -1;
    const std::size_t n = index->Keys.size() < max ? index->Keys.size() : max;
    if (out != nullptr) std::memcpy(out, index->Keys.data(), n * sizeof(std::uint32_t));
    return (long)index->Keys.size();
}
