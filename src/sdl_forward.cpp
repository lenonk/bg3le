// The SDL entry points the ImGui overlay needs to see.
//
// The game imports SDL_CreateWindow, SDL_PollEvent, SDL_StartTextInput,
// SDL_StopTextInput and SDL_IsTextInputActive from libSDL2.so by name, so
// exporting them here is enough: the dynamic linker binds the game's calls
// to these, and each one calls the real function and then tells bg3se's
// SDLManager what happened. That is what upstream gets from detouring them
// on Windows -- see src/vendor/sdl_linux.cpp.
//
// With the overlay off (BG3LE_IMGUI=0) every one of these is a straight call
// through; SDL_PollEvent runs thousands of times a second. Its input events
// also become the client's KeyInput and friends, overlay or not.

#include <dlfcn.h>

#include <atomic>
#include <cstring>
#include <deque>
#include <mutex>

#include <SDL.h>

#include "log.h"
#include "lua_host.h"
#include "sdl_input_ownership.h"

namespace bg3le {

bool imgui_overlay_wanted();

// src/vendor/sdl_linux.cpp, which is the half that can see bg3se's headers.
void sdl_on_create_window(SDL_Window* window);
int sdl_on_poll_event(int (*next)(SDL_Event*), SDL_Event* event);
void sdl_on_text_input_active(bool active);
bool sdl_wants_text_input();

// src/plugins.cpp.
void plugins_load();
bool plugins_dispatch_event(SDL_Event* event);

}  // namespace bg3le

namespace {

// The real function, past ours. dlsym(RTLD_NEXT) finds libSDL2 because the
// game already pulled it in; the explicit handle is the fallback for the
// case where it has not, the same way src/vulkan_forward.cpp does it.
void* sdl_library() {
    static void* handle = [] () -> void* {
        void* h = ::dlopen("libSDL2.so", RTLD_LAZY | RTLD_NOLOAD);
        if (h == nullptr) h = ::dlopen("libSDL2-2.0.so.0", RTLD_LAZY | RTLD_NOLOAD);
        if (h == nullptr) h = ::dlopen("libSDL2.so", RTLD_LAZY);
        return h;
    }();
    return handle;
}

template <typename Fn>
Fn real(char const* name) {
    if (void* next = ::dlsym(RTLD_NEXT, name)) return (Fn)next;
    if (void* h = sdl_library()) {
        if (void* found = ::dlsym(h, name)) return (Fn)found;
    }
    return nullptr;
}

}  // namespace

namespace {

// Ext.Input.InjectKey*'s events, handed out ahead of SDL's own as upstream's
// SDLManager::InjectEvent does; like upstream's, they skip the input events.
std::mutex g_inject_mutex;
std::deque<SDL_Event> g_injected;
bg3le::GameInputOwnership g_game_input;
struct PollInputScope;
thread_local PollInputScope* g_poll_input = nullptr;
struct PollInputScope {
    bool preservedRelease = false;
    PollInputScope* previous = g_poll_input;
    PollInputScope() { g_poll_input = this; }
    ~PollInputScope() { g_poll_input = previous; }
};
std::atomic<bool> g_have_injected{false};

bool take_injected(SDL_Event* event) {
    if (!g_have_injected.load(std::memory_order_acquire)) return false;
    std::lock_guard<std::mutex> lock(g_inject_mutex);
    if (g_injected.empty()) return false;
    *event = g_injected.front();
    g_injected.pop_front();
    g_have_injected.store(!g_injected.empty(), std::memory_order_release);
    return true;
}

using bg3le::InputKind;

// KeyInput and friends; true if a handler prevented a cancelable event.
bool dispatch_input(SDL_Event const* e) {
    switch (e->type) {
    case SDL_KEYDOWN:
    case SDL_KEYUP:
        return bg3le::lua_client_input(
            InputKind::Key, e->type == SDL_KEYDOWN, e->key.keysym.scancode,
            e->key.keysym.mod, e->key.state == SDL_PRESSED, e->key.repeat != 0,
            0, 0);
    case SDL_MOUSEBUTTONDOWN:
    case SDL_MOUSEBUTTONUP:
        return bg3le::lua_client_input(
            InputKind::MouseButton, e->button.button,
            e->button.state == SDL_PRESSED, e->button.clicks, e->button.x,
            e->button.y, 0, 0);
    case SDL_MOUSEWHEEL:
        return bg3le::lua_client_input(InputKind::MouseWheel, e->wheel.x,
                                       e->wheel.y, 0, 0, 0,
                                       e->wheel.preciseX, e->wheel.preciseY);
    case SDL_CONTROLLERAXISMOTION:
        bg3le::lua_client_input(InputKind::ControllerAxis, e->caxis.which,
                                e->caxis.axis, 0, 0, 0,
                                e->caxis.value / 32768.0, 0);
        return false;
    case SDL_CONTROLLERBUTTONDOWN:
    case SDL_CONTROLLERBUTTONUP:
        return bg3le::lua_client_input(
            InputKind::ControllerButton, e->cbutton.which,
            e->type == SDL_CONTROLLERBUTTONDOWN, e->cbutton.button,
            e->cbutton.state == SDL_PRESSED, 0, 0, 0);
    case SDL_WINDOWEVENT:
        if (e->window.event == SDL_WINDOWEVENT_RESIZED) {
            bg3le::lua_client_input(InputKind::ViewportResized,
                                    e->window.data1, e->window.data2, 0, 0,
                                    0, 0, 0);
        }
        return false;
    default:
        return false;
    }
}

}  // namespace

bool bg3le::sdl_preserve_game_release(SDL_Event const* event) {
    if (g_poll_input == nullptr || event == nullptr || !g_game_input.OwnsRelease(*event)) return false;
    g_poll_input->preservedRelease = true;
    return true;
}

extern "C" SDL_Window* SDL_CreateWindow(char const* title, int x, int y,
                                        int w, int h, Uint32 flags) {
    using Fn = SDL_Window* (*)(char const*, int, int, int, int, Uint32);
    static const Fn next = real<Fn>("SDL_CreateWindow");
    if (next == nullptr) return nullptr;

    SDL_Window* window = next(title, x, y, w, h, flags);

    if (window != nullptr && bg3le::imgui_overlay_wanted()) {
        bg3le::logf("sdl: window %p, %dx%d", (void*)window, w, h);
        bg3le::sdl_on_create_window(window);
    }
    return window;
}

extern "C" int SDL_PollEvent(SDL_Event* event) {
    using Fn = int (*)(SDL_Event*);
    static const Fn next = real<Fn>("SDL_PollEvent");
    if (next == nullptr) return 0;
    PollInputScope poll;
    // Native plugins start with the game's event loop, once the engine is up.
    bg3le::plugins_load();
    if (event != nullptr && take_injected(event)) {
        g_game_input.Delivered(*event);
        return 1;
    }

    int result = bg3le::imgui_overlay_wanted()
        ? bg3le::sdl_on_poll_event(next, event) : next(event);
    // After the overlay has had it, as upstream orders them.
    const bool ownedRelease = result == 1 && poll.preservedRelease;
    if (result == 1 && event != nullptr && dispatch_input(event) && !ownedRelease) result = 0;
    // Plugins see what is left for the game.
    if (result == 1 && event != nullptr && bg3le::plugins_dispatch_event(event) && !ownedRelease) result = 0;
    if (result == 1 && event != nullptr) g_game_input.Delivered(*event);
    return result;
}

// Upstream's InjectKeyEvent: a key event for the game's next SDL_PollEvent.
extern "C" void bg3le_sdl_inject_key(bool down, int scancode, int modifiers) {
    SDL_Event evt;
    std::memset(&evt, 0, sizeof(evt));
    evt.type = down ? SDL_KEYDOWN : SDL_KEYUP;
    evt.key.timestamp = SDL_GetTicks();
    evt.key.state = down ? SDL_PRESSED : SDL_RELEASED;
    evt.key.keysym.scancode = (SDL_Scancode)scancode;
    evt.key.keysym.sym = SDL_SCANCODE_TO_KEYCODE((SDL_Scancode)scancode);
    evt.key.keysym.mod = (Uint16)modifiers;
    std::lock_guard<std::mutex> lock(g_inject_mutex);
    g_injected.push_back(evt);
    g_have_injected.store(true, std::memory_order_release);
}

extern "C" SDL_bool SDL_IsTextInputActive(void) {
    using Fn = SDL_bool (*)(void);
    static const Fn next = real<Fn>("SDL_IsTextInputActive");
    if (next == nullptr) return SDL_FALSE;

    const SDL_bool active = next();
    if (bg3le::imgui_overlay_wanted()) {
        bg3le::sdl_on_text_input_active(active == SDL_TRUE);
    }
    return active;
}

extern "C" void SDL_StartTextInput(void) {
    using Fn = void (*)(void);
    static const Fn next = real<Fn>("SDL_StartTextInput");
    if (next != nullptr) next();
}

// Kept on while a widget wants the keyboard, or the game turns it off from
// under the text field being typed into.
extern "C" void SDL_StopTextInput(void) {
    using Fn = void (*)(void);
    static const Fn next = real<Fn>("SDL_StopTextInput");
    if (next == nullptr) return;

    if (bg3le::imgui_overlay_wanted() && bg3le::sdl_wants_text_input()) {
        return;
    }
    next();
}
