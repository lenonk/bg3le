// Hooking by function-pointer table slot.
//
// Larian's functions have no symbols and no PLT entries, so neither dynamic
// interposition nor linker tricks reach them. But virtual and
// callback-dispatched functions have their address stored in a table via an
// R_X86_64_RELATIVE relocation, and replacing that pointer is a single
// aligned store: no prologue relocation, no instruction length decoder, no
// trampoline, and trivially reversible.
//
// Slot and function addresses come from tools/recover_symbols.py piped
// through tools/find_slots.py.
#pragma once

#include <cstddef>
#include <cstdint>

namespace bg3le {

// Replaces one pointer in a table that is read-only once relocated, which
// is where vtables live. The caller has already decided the slot is the
// right one; this only handles the page protection and the store. Used for
// vtables that belong to a shared library rather than to the executable,
// where there is no link-time offset to check against.
bool hook_pointer(void** slot, void* replacement, void** original);

// Replaces the pointer in slot_offset with replacement, first checking that
// it currently holds expected_offset. The check is what makes this safe
// across game patches: if the binary has shifted, the hook is refused rather
// than corrupting an unrelated table. Both offsets are link-time addresses;
// the load bias is applied internally.
bool hook_slot(std::uintptr_t slot_offset, std::uintptr_t expected_offset,
               void* replacement, void** original);

// Redirects every direct `call rel32` that targets func_offset to
// replacement, for functions that are called directly rather than through a
// pointer table. Each site is verified to be an E8 whose displacement
// actually resolves to the target before being touched.
//
// rel32 cannot reach our library from the executable's text, so the calls
// are pointed at a trampoline allocated within +/-2GB of the code. Returns
// the number of sites patched; original receives the real function address.
// With tail_jumps, E9 tail calls to the function are redirected too.
std::size_t hook_call_sites(std::uintptr_t func_offset, void* replacement,
                            void** original, bool tail_jumps = false);

// Overwrites len bytes at a link-time .text offset with patch,
// after verifying they still hold expected (as hook_slot does).
bool patch_bytes(std::uintptr_t offset, const unsigned char* expected,
                 const unsigned char* patch, std::size_t len);

// Same, verifying expected_len bytes and writing patch_len bytes.
// patch_len must not exceed expected_len.
bool patch_bytes(std::uintptr_t offset, const unsigned char* expected,
                 std::size_t expected_len, const unsigned char* patch,
                 std::size_t patch_len);

// Compares len bytes at offset against expected, without writing.
bool bytes_match(std::uintptr_t offset, const unsigned char* expected, std::size_t len);

// Where the executable was loaded; add it to a link-time address.
std::uintptr_t load_bias();

// True when the range lies inside the main object's .text.
bool in_text(std::uintptr_t offset, std::size_t len);

}  // namespace bg3le
