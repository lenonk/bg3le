// Client game state, for what upstream's ecl::ScriptExtender does with it:
// the menu line and the client's mod bootstraps as the game leaves
// LoadModule, GameStateChanged, and a client tick every frame.
//
// - The state is EoCClient->GameStateMachine->State, as upstream reads it.
// - LoadModule's Exit is ecl::GameStateLoadModule's vtable slot 7.
// - The per-frame call is the client's state-queue update, which EoCClient's
//   update calls once a frame; upstream's ticks come from the same place.
// - ecl::GameStateMachine::SetTargetState (it logs "ecl::GameStateQueued:
//   %s") is hooked only to log what is queued.

#include "game_state.h"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>

#include "hook.h"
#include "log.h"
#include "lua_host.h"
#include "mem.h"
#include "stackdump.h"

namespace bg3le {

void translated_string_show_version(char const* suffix);
bool note_session_ended();

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

// ecl::GameStateMachine::Update(machine): runs the current state's per-frame
// slot and switches to the next queued state. Called once a frame.
constexpr std::uintptr_t kMachineUpdate = 0x2d1d9a0;
constexpr unsigned char kMachineUpdatePrologue[] = {
    0x41, 0x56, 0x53, 0x50, 0x80, 0x7f, 0x08, 0x00, 0x75, 0x08};

// char const* names[35], relocated in .data.rel.ro.
constexpr std::uintptr_t kStateNames = 0x79dfae8;
constexpr int kStateCount = 35;

constexpr std::size_t kStateId = 0x10;

// ecl::EoCClient* and its GameStateMachine {CurrentState, State}: upstream's
// GetClientState. At +0x88 here, a field earlier than on Windows.
constexpr std::uintptr_t kEoCClient = 0x7b75b50;
constexpr std::size_t kClientMachine = 0x88;
constexpr std::size_t kMachineState = 0x08;

using SetTargetStateProc = void (*)(void*, void*);
SetTargetStateProc g_set_target_state = nullptr;
// State methods may return a result -- Enter returns whether it succeeded,
// and dropping it made the engine reject the load order -- so arguments and
// result pass straight through.
using StateProc = std::uint64_t (*)(void*, void*, void*, void*);
StateProc g_load_module_exit = nullptr;
std::atomic<bool> g_left_load_module{false};

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
        install_crash_handler();
        show_version_number();
        lua_load_client_scripts();
    }
    return result;
}

StateProc g_machine_update = nullptr;

// Every frame is a client tick, and a state that differs from the last
// frame's is GameStateChanged -- upstream compares around its update call.
std::uint64_t machine_update_hook(void* machine, void* a, void* b, void* c) {
    static std::atomic<bool> first{true};
    if (first.exchange(false)) logf("gamestate: client frames are ticking");
    const std::uint64_t result = g_machine_update(machine, a, b, c);

    static char const* last = nullptr;
    char const* now = client_game_state();
    if (now != nullptr && last != nullptr && now != last) {
        logf("gamestate: client %s -> %s", last, now);
        // Upstream's client resets on UnloadSession and loads again leaving
        // LoadMenu; its server resets there too and loads at the next
        // LoadSession, which is the story work here.
        if (std::strcmp(now, "UnloadSession") == 0 && note_session_ended()) {
            logf("gamestate: session unloaded; rebuilding the Lua states");
            lua_reset(false);
        }
        if (std::strcmp(last, "LoadMenu") == 0) {
            show_version_number();
            lua_load_client_scripts();
        }
        char const* from = last;
        last = now;
        lua_client_tick(from, now);
    } else {
        if (now != nullptr) last = now;
        lua_client_tick(nullptr, nullptr);
    }
    return result;
}

void set_target_state_hook(void* machine, void* state) {
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
    void* client = nullptr;
    void* machine = nullptr;
    std::uint32_t state = 0;
    if (!safe_read(reinterpret_cast<void*>(load_bias() + kEoCClient), &client,
                   sizeof(client))
        || client == nullptr
        || !safe_read(static_cast<char*>(client) + kClientMachine, &machine,
                      sizeof(machine))
        || machine == nullptr
        || !safe_read(static_cast<char*>(machine) + kMachineState, &state,
                      sizeof(state))) {
        return nullptr;
    }
    return state_name(state);
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

    if (bytes_match(kMachineUpdate, kMachineUpdatePrologue,
                    sizeof(kMachineUpdatePrologue))
        && hook_call_sites(kMachineUpdate,
                           reinterpret_cast<void*>(&machine_update_hook),
                           &original) > 0) {
        g_machine_update = reinterpret_cast<StateProc>(original);
    } else {
        logf("gamestate: ecl::GameStateMachine::Update not at %#lx; the client "
             "ticks with the server", (unsigned long)kMachineUpdate);
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
