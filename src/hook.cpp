#include "hook.h"

#include <elf.h>
#include <link.h>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <sys/mman.h>
#include <unistd.h>

#include "log.h"
#include "mem.h"

namespace bg3le {
namespace {

int main_object(struct dl_phdr_info* info, std::size_t, void* data) {
    if (info->dlpi_name != nullptr && info->dlpi_name[0] != '\0') return 0;
    *static_cast<std::uintptr_t*>(data) = info->dlpi_addr;
    return 1;
}

// .text bounds, so call sites can be scanned without reading the file again.
bool find_text(Elf64_Addr* addr, std::size_t* size) {
    struct Ctx { Elf64_Addr addr; std::size_t size; } ctx{0, 0};
    ::dl_iterate_phdr(
        [](struct dl_phdr_info* info, std::size_t, void* data) {
            if (info->dlpi_name != nullptr && info->dlpi_name[0] != '\0') return 0;
            auto* c = static_cast<Ctx*>(data);
            for (int i = 0; i < info->dlpi_phnum; ++i) {
                const ElfW(Phdr)& ph = info->dlpi_phdr[i];
                if (ph.p_type == PT_LOAD && (ph.p_flags & PF_X)) {
                    c->addr = ph.p_vaddr;
                    c->size = ph.p_memsz;
                    return 1;
                }
            }
            return 1;
        },
        &ctx);
    *addr = ctx.addr;
    *size = ctx.size;
    return ctx.size > 0;
}


// A 12-byte absolute jump, placed near the executable's text so a rel32 call
// can reach it: movabs rax, target; jmp rax. Stubs share pages, since each
// page claimed near .text is one fewer for the next hook.
void* make_trampoline(std::uintptr_t anchor, std::size_t text_size, void* target) {
    static std::mutex lock;
    static unsigned char* page_at = nullptr;
    static std::size_t used = 0;
    constexpr std::size_t kStub = 16;
    const auto page = static_cast<std::size_t>(::sysconf(_SC_PAGESIZE));
    std::lock_guard<std::mutex> guard(lock);

    if (page_at == nullptr || used + kStub > page) {
        page_at = nullptr;
        // Every site in .text must reach the page, not just its start.
        const std::intptr_t reach = INT32_MAX - static_cast<std::intptr_t>(text_size) - 0x10000;
        for (std::intptr_t delta = 0x100000; delta < reach && page_at == nullptr; delta += 0x1000000) {
            for (int sign = -1; sign <= 1 && page_at == nullptr; sign += 2) {
                const std::uintptr_t base = sign < 0 ? anchor : anchor + text_size;
                auto hint = reinterpret_cast<void*>(
                    (base + sign * delta) & ~static_cast<std::uintptr_t>(page - 1));
                void* mem = ::mmap(hint, page, PROT_READ | PROT_WRITE | PROT_EXEC,
                                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
                if (mem == MAP_FAILED) continue;
                const auto at = reinterpret_cast<std::intptr_t>(mem);
                const auto lo = static_cast<std::intptr_t>(anchor);
                const auto hi = lo + static_cast<std::intptr_t>(text_size);
                if (hi - at > INT32_MAX - 0x10000 || at - lo > INT32_MAX - 0x10000) {
                    ::munmap(mem, page);
                    continue;
                }
                page_at = static_cast<unsigned char*>(mem);
                used = 0;
            }
        }
        if (page_at == nullptr) return nullptr;
    }

    unsigned char* code = page_at + used;
    used += kStub;
    ::mprotect(page_at, page, PROT_READ | PROT_WRITE | PROT_EXEC);
    code[0] = 0x48;  // movabs rax, imm64
    code[1] = 0xB8;
    std::memcpy(code + 2, &target, 8);
    code[10] = 0xFF;  // jmp rax
    code[11] = 0xE0;
    ::mprotect(page_at, page, PROT_READ | PROT_EXEC);
    return code;
}

}  // namespace

std::uintptr_t load_bias() {
    std::uintptr_t bias = 0;
    ::dl_iterate_phdr(main_object, &bias);
    return bias;
}

std::size_t hook_call_sites(std::uintptr_t func_offset, void* replacement,
                            void** original, bool tail_jumps) {
    const std::uintptr_t bias = load_bias();
    const std::uintptr_t target = bias + func_offset;
    if (original != nullptr) *original = reinterpret_cast<void*>(target);

    Elf64_Addr text_addr = 0;
    std::size_t text_size = 0;
    if (!find_text(&text_addr, &text_size)) {
        logf("hook: cannot locate .text");
        return 0;
    }

    void* tramp = make_trampoline(bias + text_addr, text_size, replacement);
    if (tramp == nullptr) {
        logf("hook: no trampoline within rel32 range of .text");
        return 0;
    }

    auto* code = reinterpret_cast<unsigned char*>(bias + text_addr);
    const long page = ::sysconf(_SC_PAGESIZE);
    std::size_t patched = 0;

    for (std::size_t i = 0; i + 5 <= text_size; ++i) {
        if (code[i] != 0xE8 && !(tail_jumps && code[i] == 0xE9)) continue;
        std::int32_t disp = 0;
        std::memcpy(&disp, code + i + 1, 4);
        const auto site = reinterpret_cast<std::uintptr_t>(code + i);
        if (site + 5 + disp != target) continue;

        // Only the 4 displacement bytes change; the opcode stays put.
        auto* start = reinterpret_cast<void*>((site + 1) & ~(std::uintptr_t)(page - 1));
        const std::size_t span = (site + 5) - reinterpret_cast<std::uintptr_t>(start);
        if (::mprotect(start, span, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) continue;

        const std::intptr_t rel =
            reinterpret_cast<std::intptr_t>(tramp) - static_cast<std::intptr_t>(site + 5);
        const auto rel32 = static_cast<std::int32_t>(rel);
        std::memcpy(code + i + 1, &rel32, 4);
        ::mprotect(start, span, PROT_READ | PROT_EXEC);
        ++patched;
    }

    logf("hook: redirected %zu call site(s) of %#lx via trampoline %p", patched,
         (unsigned long)func_offset, tramp);
    return patched;
}

bool hook_slot(std::uintptr_t slot_offset, std::uintptr_t expected_offset,
               void* replacement, void** original) {
    const std::uintptr_t bias = load_bias();
    auto* slot = reinterpret_cast<void**>(bias + slot_offset);
    void* expected = reinterpret_cast<void*>(bias + expected_offset);

    void* held = nullptr;
    if (!safe_read(slot, &held, sizeof(held)) || held != expected) {
        logf("hook: slot %#lx holds %p, expected %p -- refusing to patch",
             (unsigned long)slot_offset, held, expected);
        return false;
    }

    if (!hook_pointer(slot, replacement, original)) {
        logf("hook: mprotect failed for slot %#lx", (unsigned long)slot_offset);
        return false;
    }

    logf("hook: slot %#lx -> %p (was %p)", (unsigned long)slot_offset, replacement,
         original != nullptr ? *original : nullptr);
    return true;
}

bool hook_pointer(void** slot, void* replacement, void** original) {
    // The table lives in .data.rel.ro, which is read-only once relocated.
    const long page = ::sysconf(_SC_PAGESIZE);
    auto addr = reinterpret_cast<std::uintptr_t>(slot);
    auto* page_start = reinterpret_cast<void*>(addr & ~(std::uintptr_t)(page - 1));
    const std::size_t span = (addr + sizeof(void*)) - (std::uintptr_t)page_start;
    if (::mprotect(page_start, span, PROT_READ | PROT_WRITE) != 0) return false;

    if (original != nullptr) *original = *slot;
    *slot = replacement;
    ::mprotect(page_start, span, PROT_READ);
    return true;
}

// True when [offset, offset+len) lies inside the main object's .text.
// Guards LD_PRELOAD hosts that are not bg3 (steam-launch-wrapper).
bool in_text(std::uintptr_t offset, std::size_t len) {
    Elf64_Addr addr = 0;
    std::size_t size = 0;
    if (!find_text(&addr, &size)) return false;
    return offset >= addr && offset + len <= addr + size;
}

bool patch_bytes(std::uintptr_t offset, const unsigned char* expected,
                 const unsigned char* patch, std::size_t len) {
    return patch_bytes(offset, expected, len, patch, len);
}

bool patch_bytes(std::uintptr_t offset, const unsigned char* expected,
                 std::size_t expected_len, const unsigned char* patch,
                 std::size_t patch_len) {
    if (!in_text(offset, expected_len)) {
        logf("hook: %#lx is outside the main object's .text -- not bg3?",
             (unsigned long)offset);
        return false;
    }
    if (patch_len > expected_len) {
        logf("hook: patch at %#lx longer than verified span -- refusing",
             (unsigned long)offset);
        return false;
    }
    const std::uintptr_t bias = load_bias();
    auto* addr = reinterpret_cast<unsigned char*>(bias + offset);

    if (std::memcmp(addr, expected, expected_len) != 0) {
        logf("hook: bytes at %#lx don't match expected -- refusing to patch",
             (unsigned long)offset);
        return false;
    }

    const long page = ::sysconf(_SC_PAGESIZE);
    const auto start_addr = reinterpret_cast<std::uintptr_t>(addr);
    auto* page_start = reinterpret_cast<void*>(start_addr & ~(std::uintptr_t)(page - 1));
    const std::size_t span =
        (start_addr + patch_len) - reinterpret_cast<std::uintptr_t>(page_start);
    if (::mprotect(page_start, span, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
        logf("hook: mprotect failed for patch at %#lx", (unsigned long)offset);
        return false;
    }

    std::memcpy(addr, patch, patch_len);
    ::mprotect(page_start, span, PROT_READ | PROT_EXEC);

    logf("hook: patched %zu byte(s) at %#lx", patch_len, (unsigned long)offset);
    return true;
}

bool bytes_match(std::uintptr_t offset, const unsigned char* expected, std::size_t len) {
    if (!in_text(offset, len)) return false;
    const std::uintptr_t bias = load_bias();
    auto* addr = reinterpret_cast<unsigned char*>(bias + offset);
    return std::memcmp(addr, expected, len) == 0;
}

}  // namespace bg3le
