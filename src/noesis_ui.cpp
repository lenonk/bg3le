// The game's Noesis, reached by symbol.
//
// Upstream links Noesis.dll; here Noesis is compiled into the executable and
// every one of its functions is a local symbol in .symtab, so they are called
// through addresses resolved by mangled name. The UI root is the content of
// the one Noesis::View, found by its vtable and then recorded as a static
// path so later runs read it directly.

#include "noesis_ui.h"

#include <cstdint>
#include <cstring>
#include <string>

#include "elf_symbols.h"
#include "hook.h"
#include "log.h"
#include "mem.h"

extern "C" bool bg3le_scannable_region(char const* line,
                                       unsigned long long* from,
                                       unsigned long long* to);
extern "C" std::size_t bg3le_static_count(char const* key);
extern "C" void* bg3le_static_get(char const* key, std::size_t which);
extern "C" void bg3le_static_confirm(char const* key, std::size_t which);
extern "C" bool bg3le_static_record(char const* key, void const* object);

namespace bg3le {
namespace {

const SymbolTable* g_symbols = nullptr;

void* resolve(char const* mangled) {
    return g_symbols != nullptr ? g_symbols->find(mangled) : nullptr;
}

// Noesis::View's address point: the vtable symbol plus offset-to-top and
// typeinfo.
std::uintptr_t view_vtable() {
    void* vt = resolve("_ZTVN6Noesis4ViewE");
    return vt != nullptr ? reinterpret_cast<std::uintptr_t>(vt) + 16 : 0;
}

bool is_view(void* object) {
    std::uintptr_t vt = 0;
    return object != nullptr && safe_read(object, &vt, sizeof(vt))
        && vt == view_vtable();
}

void* scan_for_view() {
    const std::uintptr_t want = view_vtable();
    if (want == 0) return nullptr;
    std::FILE* maps = std::fopen("/proc/self/maps", "r");
    if (maps == nullptr) return nullptr;
    constexpr std::size_t kChunk = 1u << 20;
    static unsigned char block[kChunk];
    char line[512];
    void* found = nullptr;
    while (found == nullptr && std::fgets(line, sizeof(line), maps) != nullptr) {
        unsigned long long from = 0, to = 0;
        if (!bg3le_scannable_region(line, &from, &to)) continue;
        for (unsigned long long base = from; base < to && found == nullptr;
             base += kChunk) {
            const std::size_t want_n = (std::size_t)(to - base) < kChunk
                ? (std::size_t)(to - base) : kChunk;
            const std::size_t got = safe_read_some((void const*)base, block, want_n);
            for (std::size_t off = 0; off + 8 <= got; off += 8) {
                std::uintptr_t v;
                std::memcpy(&v, block + off, 8);
                if (v == want) {
                    found = reinterpret_cast<void*>(base + off);
                    break;
                }
            }
        }
    }
    std::fclose(maps);
    return found;
}

}  // namespace

void noesis_set_symbols(const SymbolTable* symbols) { g_symbols = symbols; }

void* noesis_view() {
    static void* cached = nullptr;
    if (is_view(cached)) return cached;

    constexpr char const* kKey = "noesis.view";
    for (std::size_t i = 0; i < bg3le_static_count(kKey); ++i) {
        void* candidate = bg3le_static_get(kKey, i);
        if (is_view(candidate)) {
            bg3le_static_confirm(kKey, i);
            return cached = candidate;
        }
    }
    void* view = scan_for_view();
    if (view != nullptr) {
        logf("noesis: View at %p (scanned)", view);
        bg3le_static_record(kKey, view);
    }
    return cached = view;
}

void* noesis_root() {
    using GetContent = void* (*)(void const*);
    static auto get = reinterpret_cast<GetContent>(
        resolve("_ZNK6Noesis4View10GetContentEv"));
    void* view = noesis_view();
    return view != nullptr && get != nullptr ? get(view) : nullptr;
}

void* noesis_find_name(void* element, char const* name) {
    using FindName = void* (*)(void const*, char const*);
    static auto find = reinterpret_cast<FindName>(
        resolve("_ZNK6Noesis16FrameworkElement8FindNameEPKc"));
    return element != nullptr && find != nullptr ? find(element, name) : nullptr;
}

char const* noesis_name(void* element) {
    using GetName = char const* (*)(void const*);
    static auto get = reinterpret_cast<GetName>(
        resolve("_ZNK6Noesis16FrameworkElement7GetNameEv"));
    return element != nullptr && get != nullptr ? get(element) : nullptr;
}

}  // namespace bg3le
