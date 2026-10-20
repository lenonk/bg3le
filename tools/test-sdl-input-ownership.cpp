#include "sdl_input_ownership.h"
#include "lua_host.h"
#include <cassert>
#include <cstdio>

extern "C" void fixture_queue(SDL_Event const*);
extern "C" void bg3le_sdl_inject_key(bool, int, int);

static bool textCapture, mouseCapture, luaCancel, pluginCancel;
static int overlayEvents, luaEvents, pluginEvents;
static bool nestedPoll;
static SDL_Scancode captureKey = SDL_SCANCODE_UNKNOWN;

namespace bg3le {
bool imgui_overlay_wanted() { return true; }
void sdl_on_create_window(SDL_Window*) {}
void sdl_on_text_input_active(bool) {}
bool sdl_wants_text_input() { return false; }
void plugins_load() {}
void logf(char const*, ...) {}
bool plugins_dispatch_event(SDL_Event*) { ++pluginEvents; return pluginCancel; }
bool lua_client_input(InputKind, long long, long long, long long, long long, long long, double, double) {
    ++luaEvents;
    return luaCancel;
}
int sdl_on_poll_event(int (*next)(SDL_Event*), SDL_Event* event) {
    if (event == nullptr) return next(event);
    while (next(event)) {
        // Production SDLManager feeds ImGui before making this capture decision.
        ++overlayEvents;
        if (nestedPoll) {
            nestedPoll = false;
            SDL_Event nested{};
            assert(SDL_PollEvent(&nested) == 1 && nested.type == SDL_KEYUP);
        }
        const bool capture = (textCapture && (event->type == SDL_KEYDOWN || event->type == SDL_KEYUP)
            && (captureKey == SDL_SCANCODE_UNKNOWN || event->key.keysym.scancode == captureKey))
            || (mouseCapture && (event->type == SDL_MOUSEBUTTONDOWN || event->type == SDL_MOUSEBUTTONUP));
        if (!capture || sdl_preserve_game_release(event)) return 1;
    }
    return 0;
}
}

static SDL_Event Key(Uint32 type, SDL_Scancode key, bool repeat = false) {
    SDL_Event event{};
    event.type = type;
    event.key.keysym.scancode = key;
    event.key.repeat = repeat;
    return event;
}
static SDL_Event Mouse(Uint32 type, Uint8 button, Uint32 device = 0) {
    SDL_Event event{};
    event.type = type;
    event.button.button = button;
    event.button.which = device;
    return event;
}
static void Queue(SDL_Event event) { fixture_queue(&event); }
static void Receive(Uint32 type) {
    SDL_Event event{};
    assert(SDL_PollEvent(&event) == 1);
    assert(event.type == type);
}
static void Empty() { SDL_Event event{}; assert(SDL_PollEvent(&event) == 0); }

int main() {
    // The actual SDL_PollEvent forwarder records only events returned to game.
    auto down = Key(SDL_KEYDOWN, SDL_SCANCODE_D);
    auto up = Key(SDL_KEYUP, SDL_SCANCODE_D);
    Queue(down); Receive(SDL_KEYDOWN);
    textCapture = true;
    Queue(up); Receive(SDL_KEYUP);
    Empty();

    // Fully captured input and captured repeat events stay hidden.
    Queue(down); Queue(up); Empty();
    textCapture = false;
    Queue(down); Receive(SDL_KEYDOWN);
    textCapture = true;
    Queue(Key(SDL_KEYDOWN, SDL_SCANCODE_D, true)); Queue(up);
    Receive(SDL_KEYUP); Empty();

    // A rescued release survives later Lua and native-plugin cancellation.
    textCapture = false;
    Queue(down); Receive(SDL_KEYDOWN);
    textCapture = luaCancel = pluginCancel = true;
    const int luaBefore = luaEvents, pluginBefore = pluginEvents, overlayBefore = overlayEvents;
    Queue(up); Receive(SDL_KEYUP);
    assert(luaEvents == luaBefore + 1 && pluginEvents == pluginBefore + 1);
    assert(overlayEvents == overlayBefore + 1);
    luaCancel = pluginCancel = false;

    // Lua-canceled downs do not create game ownership.
    textCapture = false; luaCancel = true;
    Queue(down); Empty();
    luaCancel = false; textCapture = true;
    Queue(up); Empty();

    textCapture = false; pluginCancel = true;
    Queue(down); Empty();
    pluginCancel = false; textCapture = true;
    Queue(up); Empty();

    // Outside overlay capture, existing cancelable-release behavior is retained.
    textCapture = false;
    Queue(down); Receive(SDL_KEYDOWN);
    luaCancel = true; Queue(up); Empty();
    luaCancel = false; Queue(up); Receive(SDL_KEYUP);

    // A nested poll's rescued release must not exempt the outer event's filter.
    Queue(down); Receive(SDL_KEYDOWN);
    textCapture = luaCancel = nestedPoll = true;
    captureKey = SDL_SCANCODE_D;
    Queue(Key(SDL_KEYDOWN, SDL_SCANCODE_W)); Queue(up);
    Empty();
    textCapture = luaCancel = false;
    captureKey = SDL_SCANCODE_UNKNOWN;

    // Two independent keys, button identities, and mouse-device identities.
    Queue(down); Queue(Key(SDL_KEYDOWN, SDL_SCANCODE_W));
    Receive(SDL_KEYDOWN); Receive(SDL_KEYDOWN);
    textCapture = true;
    Queue(up); Receive(SDL_KEYUP);
    Queue(Key(SDL_KEYUP, SDL_SCANCODE_W)); Receive(SDL_KEYUP);
    Queue(Mouse(SDL_MOUSEBUTTONDOWN, SDL_BUTTON_LEFT, 7)); Receive(SDL_MOUSEBUTTONDOWN);
    mouseCapture = true;
    Queue(Mouse(SDL_MOUSEBUTTONUP, SDL_BUTTON_LEFT, 8)); Empty();
    Queue(Mouse(SDL_MOUSEBUTTONUP, SDL_BUTTON_RIGHT, 7)); Empty();
    Queue(Mouse(SDL_MOUSEBUTTONUP, SDL_BUTTON_LEFT, 7)); Receive(SDL_MOUSEBUTTONUP);
    Queue(Mouse(SDL_MOUSEBUTTONUP, SDL_BUTTON_LEFT, 7)); Empty();
    mouseCapture = false;

    // The InjectKey* early-return path participates in the same ownership ledger.
    bg3le_sdl_inject_key(true, SDL_SCANCODE_D, 0); Receive(SDL_KEYDOWN);
    Queue(up); Receive(SDL_KEYUP);
    Queue(up); Empty();

    textCapture = false;
    Queue(down); Receive(SDL_KEYDOWN);
    textCapture = true;
    bg3le_sdl_inject_key(false, SDL_SCANCODE_D, 0); Receive(SDL_KEYUP);
    Queue(up); Empty();

    // A null-event peek is not a delivered keydown and must not claim ownership.
    // This checks the forwarder ledger; the fake overlay delegates null peeks
    // and does not establish production ImGui null-event handling.
    textCapture = false;
    Queue(down);
    assert(SDL_PollEvent(nullptr) == 1);
    textCapture = true;
    Queue(up); Empty();
    std::puts("SDL forwarder capture/release regression checks passed");
}
