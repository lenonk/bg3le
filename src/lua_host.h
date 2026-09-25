#pragma once

#include <string>
#include <utility>
#include <vector>

#include "elf_symbols.h"
#include "osi.h"

namespace bg3le {

// Brings up the embedded Lua state. Safe to call more than once.
void lua_init();

// Lets Ext._Internal.SymbolAddr resolve engine symbols from the prompt, so
// structures can be explored live instead of rebuilding for every guess.
void lua_set_symbols(const SymbolTable* symbols);

// Publishes the enumerated Osiris functions as the global Osi table.
void lua_bind_osi(const std::vector<osi::Function>& functions);

// Runs a chunk, logging the result or the error. For bring-up checks.
void lua_run(const char* code);

// Runs any timer callbacks that have come due. Called once per server tick,
// so callbacks land on the story thread and may call Osiris.
void lua_tick();

// Loads loose-file mods from the search path. Deferred until Osiris is bound,
// since a mod's load-time code may call it.
void lua_load_mods();

// Evaluates a chunk, returning its stringified results or the error text.
// Must be called from the story thread.
// Evaluates in one context or the other. The console chooses: the
// LuaDebug protocol carries a context on every request, and bg3lua's
// :client / :server switch it.
void lua_eval_in(bool client, const char* code, std::string* result,
                 std::string* error);

// Whether a client context exists at all.
bool lua_has_client();

void lua_eval(const char* code, std::string* result, std::string* error);

// Runs the client context's mod bootstraps, once: upstream does this as the
// game leaves LoadModule, before the main menu is built.
void lua_load_client_scripts();

// One client frame, on the client's thread: GameStateChanged when the state
// moved (from/to non-null), then timers and Tick.
void lua_client_tick(char const* from, char const* to);

// Upstream's ClientState::OnInputEvent, from the thread that polls SDL.
// `kind` and the arguments are laid out by src/sdl_forward.cpp; returns
// true if a handler prevented the action.
enum class InputKind { Key = 1, MouseButton, MouseWheel, ControllerAxis,
                       ControllerButton, ViewportResized };
bool lua_client_input(InputKind kind, long long a, long long b, long long c,
                      long long d, long long e, double x, double y);

// Hands the PersistentVars of a save just read to the mods, if they are up;
// otherwise LoadMods does it after the bootstraps.
void lua_restore_persistent_vars();

// Both Lua states torn down and built again, as upstream's reset does; with
// load_mods false the caller loads them (a new session's story work).
void lua_reset(bool load_mods);

// Ext.Vars and persistent timers, the rest of the save region
// (src/savegame.cpp): what the server context holds, and a read handed back.
struct SaveExtras;
bool lua_extras_to_save(SaveExtras* out);
void lua_restore_save_extras();

// (mod UUID, JSON) for every mod whose PersistentVars a save should carry.
// False if the server context could not be asked.
bool lua_persistent_vars_to_save(
    std::vector<std::pair<std::string, std::string>>* out);

}  // namespace bg3le
