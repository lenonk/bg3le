// ls::GlobalSwitches, which Ext.Utils.GetGlobalSwitches returns and bg3se's
// ImGui overlay reads its language from.
//
// The global has no symbol. tools/relocs-xref.py found it: the settings
// registration reads 102 of the switch names alongside 60 loads of one
// global, and pairing each name with the member it reads gives this build's
// offsets. bg3se's declared layout (by Norbyte and the bg3se contributors)
// matches all 46 of those once two things are corrected in
// vendor/.../GameDefinitions/Misc.h: a SoundSetting is 0x68 bytes here, and
// eight more bytes follow SomeSettings. See reference/GLOBAL-SWITCHES.md.
//
// Checked before use: the instruction that loads the global must still name
// it, and the object must hold a printable Language where it is declared.

#include <stdafx.h>

#include <GameDefinitions/Misc.h>

#include <cstdint>
#include <cstring>

#include "../log.h"
#include "../mem.h"

namespace bg3le {
std::uintptr_t load_bias();
void extender_set_global_switches(void* engineSwitches);

namespace {

constexpr std::uintptr_t kGlobal = 0x7d9d198;
// `mov rsi, [rip+disp]` loading it in the settings registration.
constexpr std::uintptr_t kLoad = 0x3f5c1c0;
constexpr unsigned char kLoadBytes[] = {0x48, 0x8b, 0x35};

static_assert(offsetof(bg3se::GlobalSwitches, Language) == 0xc0);
static_assert(offsetof(bg3se::GlobalSwitches, SoundVolumeDynamicRange) == 0x688);
static_assert(offsetof(bg3se::GlobalSwitches, TimelinesAD) == 0x1344);
static_assert(offsetof(bg3se::GlobalSwitches, StartDay) == 0x141c);

void* find() {
    const std::uintptr_t bias = load_bias();
    unsigned char code[7] = {};
    if (!safe_read((void const*)(bias + kLoad), code, sizeof(code))
        || std::memcmp(code, kLoadBytes, sizeof(kLoadBytes)) != 0) {
        logf("global switches: the loading instruction is not at image+%#lx",
             (unsigned long)kLoad);
        return nullptr;
    }
    std::int32_t disp = 0;
    std::memcpy(&disp, code + 3, sizeof(disp));
    if (kLoad + sizeof(code) + disp != kGlobal) {
        logf("global switches: image+%#lx no longer loads image+%#lx",
             (unsigned long)kLoad, (unsigned long)kGlobal);
        return nullptr;
    }

    char* object = nullptr;
    if (!safe_read((void const*)(bias + kGlobal), &object, sizeof(object)) || object == nullptr) {
        return nullptr;
    }
    // Language, "English" on an English install: inline or on the heap, read
    // from a copy so a bad address cannot fault.
    alignas(bg3se::STDString) unsigned char raw[sizeof(bg3se::STDString)] = {};
    if (!safe_read(object + offsetof(bg3se::GlobalSwitches, Language), raw, sizeof(raw))) {
        return nullptr;
    }
    auto const* copy = reinterpret_cast<bg3se::STDString const*>(raw);
    char language[64] = {};
    const std::size_t n = copy->size();
    if (n == 0 || n >= sizeof(language) || !safe_read(copy->data(), language, n)) {
        logf("global switches: no language where it is declared");
        return nullptr;
    }
    for (std::size_t i = 0; i < n; ++i) {
        if (language[i] < ' ' || language[i] > '~') return nullptr;
    }
    if (language[0] < 'A' || language[0] > 'Z') return nullptr;
    logf("global switches: at %p, language %s", (void*)object, language);
    return object;
}

}  // namespace
}  // namespace bg3le

extern "C" void* bg3le_global_switches() {
    static void* found = nullptr;
    if (found == nullptr) {
        found = bg3le::find();
        if (found != nullptr) bg3le::extender_set_global_switches(found);
    }
    return found;
}
