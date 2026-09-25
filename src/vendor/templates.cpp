// Root templates, which is what Ext.Template reads.
//
// The root templates come from the engine's GlobalTemplateManager, whose
// global was found by content and is recorded for this build, checked on
// every read: its bank's Templates map is walked once. A scan for template
// objects remains for the rest -- the level's local templates -- and runs
// on the warming thread only, since it takes seconds.
//
// The scan works because That is viable because a
// GameObjectTemplate is unusually self-identifying: past its vtable it
// carries its own Id, TemplateName and ParentTemplateId as FixedStrings
// and its Name as a Larian string, and an Id is always a 36-character
// GUID. Four independent checks on the same object is not something other
// data satisfies by accident.
//
// Deliberately not a fingerprint over a container, which is how the first
// attempt at the translated strings went wrong: here every candidate is
// validated against its own contents before it is kept.

#include <stdafx.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <unistd.h>

#include "ls_string.h"

#include "../log.h"
#include "../mem.h"

extern "C" bool bg3le_scannable_region(char const* line,
                                       unsigned long long* from,
                                       unsigned long long* to);

namespace bg3le {

extern "C" char const* bg3le_fixed_string(std::uint32_t index,
                                          std::uint32_t* length);

namespace {

// Within GameObjectTemplate, past the vtable and the tag container.
constexpr std::size_t kId = 16;
constexpr std::size_t kTemplateName = 20;
constexpr std::size_t kParentTemplateId = 24;
constexpr std::size_t kName = 32;          // a Larian string

constexpr std::uint32_t kNullFixedString = 0xffffffffu;
constexpr std::size_t kGuidLength = 36;

struct Found {
    std::uint64_t Address{0};
    std::string Type;       // "character", "item", ... or empty
};

struct Templates {
    bool Built{false};
    std::unordered_map<std::string, Found> ById;
    std::vector<std::string> Order;
    std::unordered_map<std::uint64_t, std::size_t> ByVtable;
};

// Root templates from the manager, and what the scan found besides.
Templates& state() {
    static Templates t;
    return t;
}

Templates& scanned() {
    static Templates t;
    return t;
}

// Guards both sets. Held to publish or read, never across a scan.
std::mutex& templates_lock() {
    static std::mutex m;
    return m;
}

// Where ls::GlobalTemplateManager sat in this build, relative to the
// executable's first mapping; and within it, Banks[2] at +0x20. A bank is
// { VMT, LegacyMap<FixedString, GameObjectTemplate*> Templates, ... }.
constexpr std::uintptr_t kRecordedManagerGlobal = 0x7d203f8;
constexpr std::uintptr_t kManagerBanks = 0x20;
constexpr std::uintptr_t kBankHashSize = 0x08;
constexpr std::uintptr_t kBankTable = 0x10;
constexpr std::uintptr_t kBankCount = 0x18;

template <class T>
bool read_as(void const* addr, T* out) {
    return safe_read(addr, out, sizeof(T));
}

bool resolves(std::uint32_t index, std::size_t* lengthOut) {
    if (index == 0 || index == kNullFixedString) return false;
    char const* text = bg3le_fixed_string(index, nullptr);
    if (text == nullptr) return false;
    *lengthOut = std::strlen(text);
    return true;
}

// Whether the object at this address reads as a GameObjectTemplate.
bool is_template(void const* at, std::string* idOut, std::uint64_t* vtable) {
    std::uint64_t vmt = 0;
    if (!read_as(at, &vmt) || vmt < 0x1000) return false;

    std::uint32_t id = 0;
    std::uint32_t templateName = 0;
    std::uint32_t parent = 0;
    if (!read_as((char const*)at + kId, &id)
        || !read_as((char const*)at + kTemplateName, &templateName)
        || !read_as((char const*)at + kParentTemplateId, &parent)) {
        return false;
    }

    // The Id is a GUID.
    std::size_t length = 0;
    if (!resolves(id, &length) || length != kGuidLength) return false;

    // TemplateName is a name, not a GUID, and never empty.
    if (!resolves(templateName, &length) || length == 0) return false;

    // ParentTemplateId is either a GUID or unset.
    if (parent != 0 && parent != kNullFixedString) {
        if (!resolves(parent, &length) || length != kGuidLength) return false;
    }

    // And Name reads as a string.
    std::string name;
    if (!read_ls_string((char const*)at + kName, &name)) return false;

    *idOut = bg3le_fixed_string(id, nullptr);
    *vtable = vmt;
    return true;
}

// A template's type name, read rather than called.
//
// bg3se asks GetType(), a virtual. Calling one blind is how you run a
// destructor by accident, so the slot is decoded instead: on this build
// every template's slot four is
//
//     48 8d 05 <disp32>    lea rax, [rip+disp]
//     c3                   ret
//
// which hands back the address of a per-class static FixedString. The
// displacement is right there in the code, so the string can simply be
// read. The pattern is checked exactly, and anything else yields nothing
// rather than a guess.
constexpr std::size_t kTypeGetterSlot = 4;

char const* type_name_of(std::uint64_t vtable) {
    std::uint64_t fn = 0;
    if (!read_as((char const*)(std::uintptr_t)vtable
                     + kTypeGetterSlot * 8, &fn)) {
        return nullptr;
    }

    unsigned char code[8] = {};
    if (!safe_read((void const*)(std::uintptr_t)fn, code, sizeof(code))) {
        return nullptr;
    }
    if (code[0] != 0x48 || code[1] != 0x8d || code[2] != 0x05
        || code[7] != 0xc3) {
        return nullptr;
    }

    std::int32_t displacement = 0;
    std::memcpy(&displacement, code + 3, sizeof(displacement));
    const auto at = (std::uint64_t)((std::int64_t)fn + 7 + displacement);

    std::uint32_t index = 0;
    if (!read_as((void const*)(std::uintptr_t)at, &index)) return nullptr;
    if (index == 0 || index == kNullFixedString) return nullptr;
    return bg3le_fixed_string(index, nullptr);
}

// Under BG3LE_DUMP_TEMPLATES: the first bytes of each vtable slot.
//
// bg3se asks a template its type through GetType(), a virtual. Calling one
// blind is how you run a destructor by accident, so the slots are read as
// data instead: a getter that hands back the address of a member compiles
// to a two-instruction body, and its displacement says which member --
// which can then simply be read.
void dump_vtable(std::uint64_t vtable) {
    logf("tmpldump: vtable %#llx", (unsigned long long)vtable);
    for (int slot = 0; slot < 10; ++slot) {
        std::uint64_t fn = 0;
        if (!read_as((char const*)(std::uintptr_t)vtable
                         + (std::size_t)slot * 8, &fn)) {
            break;
        }
        unsigned char code[12] = {};
        if (safe_read_some((void const*)(std::uintptr_t)fn, code,
                           sizeof(code)) == 0) {
            continue;
        }
        logf("tmpldump:   slot %2d -> %#llx  %02x %02x %02x %02x %02x %02x "
             "%02x %02x", slot, (unsigned long long)fn, code[0], code[1],
             code[2], code[3], code[4], code[5], code[6], code[7]);
    }
}

// The executable's own mapping, so a candidate's vtable pointer can be
// tested without a syscall. A code pointer into the image is rare in data,
// which is what makes this cheap enough to apply per eight bytes.
bool image_range(unsigned long long* from, unsigned long long* to) {
    static unsigned long long low = 0;
    static unsigned long long high = 0;
    if (high != 0) {
        *from = low;
        *to = high;
        return true;
    }

    char exe[4096];
    const ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n <= 0) return false;
    exe[n] = '\0';

    std::FILE* maps = std::fopen("/proc/self/maps", "r");
    if (maps == nullptr) return false;

    char line[1024];
    while (std::fgets(line, sizeof(line), maps) != nullptr) {
        if (std::strstr(line, exe) == nullptr) continue;
        unsigned long long a = 0;
        unsigned long long b = 0;
        if (std::sscanf(line, "%llx-%llx", &a, &b) != 2) continue;
        if (low == 0 || a < low) low = a;
        if (b > high) high = b;
    }
    std::fclose(maps);

    *from = low;
    *to = high;
    return high != 0;
}

char const* type_name_of(std::uint64_t vtable);
bool image_range(unsigned long long* from, unsigned long long* to);

}  // namespace
extern "C" std::uintptr_t bg3le_image_find_static(bool (*accept)(std::uintptr_t));
namespace {

// A GlobalTemplateManager: a vtable, then a bank that holds templates keyed
// by their own Ids. Checked on the first bucket entry only, for speed.
bool looks_like_manager(std::uintptr_t mgr) {
    unsigned long long imageFrom = 0, imageTo = 0;
    if (!image_range(&imageFrom, &imageTo)) return false;
    auto in_image = [&](std::uint64_t v) { return v >= imageFrom && v < imageTo; };
    std::uint64_t vmt = 0;
    if (!read_as((void const*)mgr, &vmt) || !in_image(vmt)) return false;
    for (int slot = 0; slot < 2; ++slot) {
        std::uint64_t bank = 0, bvmt = 0, table = 0;
        std::uint32_t count = 0, hashSize = 0;
        if (!read_as((void const*)(mgr + kManagerBanks + slot * 8), &bank)
            || !read_as((void const*)bank, &bvmt) || !in_image(bvmt)
            || !read_as((void const*)(bank + kBankCount), &count) || count < 100
            || !read_as((void const*)(bank + kBankHashSize), &hashSize)
            || !read_as((void const*)(bank + kBankTable), &table)
            || hashSize == 0 || hashSize > (1u << 22)) {
            continue;
        }
        for (std::uint32_t b = 0; b < hashSize && b < 64; ++b) {
            std::uint64_t node = 0;
            if (!read_as((void const*)(table + b * 8), &node)) break;
            if (node == 0) continue;
            std::uint64_t raw[3] = {};
            std::uint64_t head[3] = {};
            return safe_read((void const*)node, raw, sizeof(raw))
                   && safe_read((void const*)raw[2], head, sizeof(head))
                   && in_image(head[0]) && (std::uint32_t)head[2] == (std::uint32_t)raw[1];
        }
    }
    return false;
}

// The static holding the manager: the recorded one, else found again.
std::uint64_t manager_static(unsigned long long imageFrom) {
    static std::uint64_t found = 0;
    static bool searched = false;
    std::uint64_t mgr = 0;
    const std::uint64_t recorded = imageFrom + kRecordedManagerGlobal;
    if (found == 0 && read_as((void const*)recorded, &mgr) && looks_like_manager(mgr)) {
        found = recorded;
    }
    if (found != 0 || searched) return found;
    searched = true;
    found = bg3le_image_find_static(&looks_like_manager);
    if (found != 0) {
        logf("templates: GlobalTemplateManager static at image+%#lx (recorded +%#lx)",
             (unsigned long)(found - imageFrom), (unsigned long)kRecordedManagerGlobal);
    }
    return found;
}

// The populated bank of the GlobalTemplateManager, walked into `out`.
bool build_from_manager(Templates* out) {
    unsigned long long imageFrom = 0, imageTo = 0;
    if (!image_range(&imageFrom, &imageTo)) return false;
    auto in_image = [&](std::uint64_t v) { return v >= imageFrom && v < imageTo; };

    std::uint64_t mgr = 0, vmt = 0;
    const std::uint64_t at = manager_static(imageFrom);
    if (at == 0 || !read_as((void const*)(std::uintptr_t)at, &mgr)
        || !read_as((void const*)(std::uintptr_t)mgr, &vmt) || !in_image(vmt)) {
        return false;
    }

    // The bank with templates in it; the other is empty on this build.
    std::uint64_t bank = 0;
    std::uint32_t best = 0;
    for (int slot = 0; slot < 2; ++slot) {
        std::uint64_t b = 0, bvmt = 0;
        std::uint32_t count = 0;
        if (!read_as((void const*)(std::uintptr_t)(mgr + kManagerBanks + slot * 8), &b)
            || !read_as((void const*)(std::uintptr_t)b, &bvmt) || !in_image(bvmt)
            || !read_as((void const*)(std::uintptr_t)(b + kBankCount), &count)) {
            continue;
        }
        if (count > best) {
            best = count;
            bank = b;
        }
    }
    std::uint32_t hashSize = 0;
    std::uint64_t table = 0;
    if (bank == 0 || best < 100
        || !read_as((void const*)(std::uintptr_t)(bank + kBankHashSize), &hashSize)
        || !read_as((void const*)(std::uintptr_t)(bank + kBankTable), &table)
        || hashSize == 0 || hashSize > (1u << 22)) {
        return false;
    }

    std::vector<std::uint64_t> buckets(hashSize);
    if (safe_read_some((void const*)(std::uintptr_t)table, buckets.data(),
                       hashSize * sizeof(std::uint64_t)) != hashSize * sizeof(std::uint64_t)) {
        return false;
    }

    // Each node is { Next, Key, Value }, and the key is the template's own
    // Id: a node that disagrees means this is not the bank.
    std::size_t disagreed = 0;
    for (std::uint64_t node : buckets) {
        for (std::uint32_t guard = 0; node != 0 && guard < (1u << 16); ++guard) {
            std::uint64_t raw[3] = {};
            if (!safe_read((void const*)(std::uintptr_t)node, raw, sizeof(raw))) break;
            node = raw[0];
            const auto key = (std::uint32_t)raw[1];
            std::uint64_t head[3] = {};  // VMT, tags, Id and TemplateName
            if (!safe_read((void const*)(std::uintptr_t)raw[2], head, sizeof(head))
                || !in_image(head[0]) || (std::uint32_t)head[2] != key) {
                ++disagreed;
                continue;
            }
            char const* id = bg3le_fixed_string(key, nullptr);
            if (id == nullptr || std::strlen(id) != kGuidLength) continue;
            char const* type = type_name_of(head[0]);
            if (out->ById.emplace(id, Found{raw[2], type != nullptr ? type : ""}).second) {
                ++out->ByVtable[head[0]];
                out->Order.push_back(id);
            }
        }
    }
    if (out->ById.size() < 100 || disagreed > out->ById.size() / 100) {
        logf("templates: the manager at image+%#lx did not check out (%zu read, %zu disagreed)",
             (unsigned long)kRecordedManagerGlobal, out->ById.size(), disagreed);
        *out = Templates{};
        return false;
    }
    out->Built = true;
    return true;
}

bool build() {
    Templates found{};

    unsigned long long imageFrom = 0;
    unsigned long long imageTo = 0;
    if (!image_range(&imageFrom, &imageTo)) {
        logf("templates: cannot locate the executable's mapping");
        return false;
    }

    std::FILE* maps = std::fopen("/proc/self/maps", "r");
    if (maps == nullptr) return false;

    constexpr std::size_t kChunk = 1u << 20;
    constexpr std::size_t kObject = 48;
    static std::vector<unsigned char> block;
    block.resize(kChunk + kObject);

    char line[512];
    while (std::fgets(line, sizeof(line), maps) != nullptr) {
        unsigned long long from = 0;
        unsigned long long to = 0;
        if (!bg3le_scannable_region(line, &from, &to)) continue;

        for (unsigned long long base = from; base < to; base += kChunk) {
            std::size_t want = (std::size_t)(to - base);
            if (want > kChunk + kObject) want = kChunk + kObject;
            const std::size_t got =
                safe_read_some((void const*)base, block.data(), want);
            scan_yield();
            if (got < kObject) continue;

            for (std::size_t off = 0; off + kObject <= got; off += 8) {
                // Every rejection here is from the block already read, not
                // a syscall. Without that the scan calls is_template on
                // almost every slot in a multi-gigabyte address space and
                // takes hours -- the same mistake that once locked the
                // story thread.
                std::uint64_t vmt = 0;
                std::memcpy(&vmt, block.data() + off, sizeof(vmt));
                if (vmt < imageFrom || vmt >= imageTo) continue;
                if ((vmt & 7) != 0) continue;

                std::uint32_t id = 0;
                std::uint32_t templateName = 0;
                std::memcpy(&id, block.data() + off + kId, sizeof(id));
                std::memcpy(&templateName, block.data() + off + kTemplateName,
                            sizeof(templateName));
                if (id == 0 || id == kNullFixedString) continue;
                if (templateName == 0 || templateName == kNullFixedString) {
                    continue;
                }

                std::string key;
                std::uint64_t vtable = 0;
                if (!is_template((void const*)(base + off), &key, &vtable)) {
                    continue;
                }

                // The first one wins: a template can be referenced from
                // several places but only one object is the template.
                char const* type = type_name_of(vtable);
                Found entry{base + off, type != nullptr ? type : ""};
                if (found.ById.emplace(key, entry).second) {
                    ++found.ByVtable[vtable];
                }
            }
        }
    }
    std::fclose(maps);

    if (found.ById.size() < 100) {
        logf("templates: only %zu candidates found; treating that as not "
             "found rather than publishing a partial set",
             found.ById.size());
        return false;
    }

    found.Order.reserve(found.ById.size());
    for (auto const& entry : found.ById) found.Order.push_back(entry.first);

    found.Built = true;
    std::size_t typed = 0;
    for (auto const& entry : found.ById) {
        if (!entry.second.Type.empty()) ++typed;
    }
    logf("templates: the scan found %zu templates across %zu distinct vtables, "
         "%zu with a type name", found.ById.size(), found.ByVtable.size(), typed);

    if (std::getenv("BG3LE_DUMP_TEMPLATES") != nullptr) {
        std::unordered_map<std::string, std::size_t> byType;
        for (auto const& entry : found.ById) ++byType[entry.second.Type];
        for (auto const& entry : byType) {
            logf("templates: type \"%s\": %zu", entry.first.c_str(),
                 entry.second);
        }
        std::size_t shown = 0;
        for (auto const& entry : found.ByVtable) {
            logf("templates: vtable %#llx holds %zu templates",
                 (unsigned long long)entry.first, entry.second);
            if (shown++ < 2) dump_vtable(entry.first);
        }
    }

    // Published only now, so a lookup never waits on the scan itself.
    const std::lock_guard<std::mutex> held(templates_lock());
    for (auto const& id : found.Order) {
        if (state().ById.count(id) == 0) state().Order.push_back(id);
    }
    scanned() = std::move(found);
    return true;
}

// The root templates, from the manager: cheap, so any thread may ask, and
// a failure is retried a few seconds later rather than on every call.
// Called with templates_lock held.
bool root_ready() {
    if (state().Built) return true;
    static std::time_t lastAttempt = 0;
    const std::time_t now = std::time(nullptr);
    if (lastAttempt != 0 && now - lastAttempt < 3) return false;
    lastAttempt = now;

    Templates found{};
    if (!build_from_manager(&found)) return false;
    for (auto const& id : scanned().Order) {
        if (found.ById.count(id) == 0) found.Order.push_back(id);
    }
    state() = std::move(found);
    logf("templates: %zu root templates from the GlobalTemplateManager",
         state().ById.size());
    return true;
}

// The scan, for what the manager does not hold. The warming thread only.
bool scan_ready() {
    if (scanned().Built) return true;
    if (!scan_allowed()) return false;
    static int attempts = 0;
    if (attempts >= 40) return false;
    ++attempts;
    return build();
}

Found const* lookup(char const* id) {
    auto it = state().ById.find(id);
    if (it != state().ById.end()) return &it->second;
    it = scanned().ById.find(id);
    return it != scanned().ById.end() ? &it->second : nullptr;
}

}  // namespace

extern "C" bool bg3le_templates_ready() {
    bool root = false;
    {
        const std::lock_guard<std::mutex> held(templates_lock());
        root = root_ready();
    }
    // The warming thread goes on to scan for local templates.
    return scan_ready() || root;
}

extern "C" std::size_t bg3le_templates_count() {
    const std::lock_guard<std::mutex> held(templates_lock());
    root_ready();
    return state().Order.size();
}

extern "C" char const* bg3le_templates_id_at(std::size_t index) {
    const std::lock_guard<std::mutex> held(templates_lock());
    root_ready();
    if (index >= state().Order.size()) return nullptr;
    return state().Order[index].c_str();
}

extern "C" void* bg3le_templates_find(char const* id) {
    if (id == nullptr) return nullptr;
    const std::lock_guard<std::mutex> held(templates_lock());
    root_ready();
    Found const* found = lookup(id);
    return found != nullptr ? (void*)(std::uintptr_t)found->Address : nullptr;
}

// The engine's own name for a template's type: "character", "item" and so
// on, from the class's static FixedString.
extern "C" char const* bg3le_templates_type(char const* id) {
    if (id == nullptr) return nullptr;
    const std::lock_guard<std::mutex> held(templates_lock());
    root_ready();
    Found const* found = lookup(id);
    if (found == nullptr || found->Type.empty()) return nullptr;
    return found->Type.c_str();
}

}  // namespace bg3le
