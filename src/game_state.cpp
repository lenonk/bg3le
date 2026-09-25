// Client game state changes, for the work upstream does when the game
// leaves LoadModule (ecl::ScriptExtender::OnGameStateChanged): the menu's
// version line and the client's mod bootstraps, both before the menu is
// built.
//
// Upstream wraps ecl::GameStateMachine::Update and compares the state before
// and after. Here the same moment is ecl::GameStateLoadModule's Exit, its
// vtable slot 7, which the machine calls as it switches to the next state.
//
// ecl::GameStateMachine::SetTargetState -- the function that logs
// "ecl::GameStateQueued: %s" -- is hooked too, for the machine pointer: its
// current state is at +0x28, and a state's id at +0x10 indexes the engine's
// state-name table.

#include "game_state.h"

#include <atomic>
#include <cstdint>
#include <string>

#include "hook.h"
#include "log.h"
#include "lua_host.h"
#include "mem.h"

namespace bg3le {

void translated_string_show_version(char const* suffix);

namespace {

constexpr std::uintptr_t kSetTargetState = 0x4017ab0;
constexpr unsigned char kSetTargetStatePrologue[] = {
    0x55, 0x41, 0x57, 0x41, 0x56, 0x41, 0x54, 0x53, 0x48, 0x83, 0xec, 0x20,
    0x4c, 0x8b, 0x25, 0xbd, 0x3b, 0xd8, 0x03, 0x49, 0x89, 0xfe, 0x48, 0x89,
    0xf3};

// ecl::GameStateLoadModule's vtable slot 7 (Exit) and what it holds. Slot 4
// is Enter, which starts the load; 5 and 6 run every frame until it is done.
constexpr std::uintptr_t kLoadModuleExitSlot = 0x79e0320 + 7 * 8;
constexpr std::uintptr_t kLoadModuleExit = 0x6482430;

// char const* names[35], relocated in .data.rel.ro.
constexpr std::uintptr_t kStateNames = 0x79dfae8;
constexpr int kStateCount = 35;

constexpr std::size_t kCurrentState = 0x28;
constexpr std::size_t kStateId = 0x10;

using SetTargetStateProc = void (*)(void*, void*);
SetTargetStateProc g_set_target_state = nullptr;
// State methods may return a result -- Enter returns whether it succeeded,
// and dropping it made the engine reject the load order -- so arguments and
// result pass straight through.
using StateProc = std::uint64_t (*)(void*, void*, void*, void*);
StateProc g_load_module_exit = nullptr;
std::atomic<bool> g_left_load_module{false};
std::atomic<void*> g_machine{nullptr};

char const* state_name(std::uint32_t state) {
    if (state >= kStateCount) return "Unknown";
    auto* const* names = reinterpret_cast<char const* const*>(load_bias() + kStateNames);
    return names[state] != nullptr ? names[state] : "Unknown";
}

bool state_id(void* state, std::uint32_t* out) {
    return state != nullptr
        && safe_read(static_cast<char*>(state) + kStateId, out, sizeof(*out));
}

// ecl::ScriptExtender::ShowVersionNumber, queued until the copyright
// string is in the repository.
void show_version_number() {
    translated_string_show_version(
        "\r\nbg3le loaded, Script Extender v32 API, built on " __DATE__
        " " __TIME__ ".");
}

std::uint64_t load_module_exit_hook(void* state, void* a, void* b, void* c) {
    const std::uint64_t result = g_load_module_exit(state, a, b, c);
    if (!g_left_load_module.exchange(true)) {
        logf("gamestate: client left LoadModule");
        show_version_number();
        lua_load_client_scripts();
    }
    return result;
}

void set_target_state_hook(void* machine, void* state) {
    g_machine.store(machine);
    std::uint32_t to = 0;
    if (state_id(state, &to)) {
        void* vtable = *static_cast<void**>(state);
        logf("gamestate: client queued %s (vtable image+%#lx)", state_name(to),
             (unsigned long)(reinterpret_cast<std::uintptr_t>(vtable) - load_bias()));
    }
    g_set_target_state(machine, state);
}

}  // namespace

char const* client_state_name() {
    char const* state = client_game_state();
    return state != nullptr ? state : "unknown";
}

char const* client_game_state() {
    void* machine = g_machine.load();
    void* current = nullptr;
    std::uint32_t id = 0;
    if (machine == nullptr
        || !safe_read(static_cast<char*>(machine) + kCurrentState, &current,
                      sizeof(current))
        || !state_id(current, &id)) {
        return nullptr;
    }
    return state_name(id);
}

void install_game_state_hook() {
    void* original = nullptr;
    if (hook_slot(kLoadModuleExitSlot, kLoadModuleExit,
                  reinterpret_cast<void*>(&load_module_exit_hook), &original)) {
        g_load_module_exit = reinterpret_cast<StateProc>(original);
    } else {
        logf("gamestate: LoadModule's Exit is not where it was; the menu line "
             "and early client mods are off");
    }

    if (!bytes_match(kSetTargetState, kSetTargetStatePrologue,
                     sizeof(kSetTargetStatePrologue))) {
        logf("gamestate: ecl::GameStateMachine::SetTargetState not at %#lx",
             (unsigned long)kSetTargetState);
        return;
    }
    if (hook_call_sites(kSetTargetState,
                        reinterpret_cast<void*>(&set_target_state_hook),
                        &original) > 0) {
        g_set_target_state = reinterpret_cast<SetTargetStateProc>(original);
    }
}

}  // namespace bg3le
