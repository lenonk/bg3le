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

#include <SDL.h>

#include "log.h"
#include "lua_host.h"

namespace bg3le {

bool imgui_overlay_wanted();

// src/vendor/sdl_linux.cpp, which is the half that can see bg3se's headers.
void sdl_on_create_window(SDL_Window* window);
int sdl_on_poll_event(int (*next)(SDL_Event*), SDL_Event* event);
void sdl_on_text_input_active(bool active);
bool sdl_wants_text_input();

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

    int result = bg3le::imgui_overlay_wanted()
        ? bg3le::sdl_on_poll_event(next, event) : next(event);
    // After the overlay has had it, as upstream orders them.
    if (result == 1 && dispatch_input(event)) result = 0;
    return result;
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
