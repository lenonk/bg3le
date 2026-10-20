// SDLManager for the native Linux build.
//
// The ImGui overlay needs two things from SDL that nothing else can give it:
// the window, so imgui knows how big the viewport is, and the event stream,
// so a click reaches a button. Without them the overlay draws into a
// zero-sized display and everything is clipped -- which is exactly what it
// did while this was a stub: 58 vertices a frame and no input at all.
//
// Upstream gets there by detouring SDL_PollEvent and friends. The Linux
// build has no inline hooks, but it does not need them: the game imports
// every one of these from libSDL2.so by name, so src/sdl_forward.cpp exports
// them and the dynamic linker routes the calls here.
//
// The logic is upstream's, from Extender/Client/SDL.cpp, which bg3le does not
// build because it is written against Detours. By Norbyte and the bg3se
// contributors (https://github.com/Norbyte/bg3se).

#include <stdafx.h>

#include <Extender/Client/SDLManager.h>
#include <Extender/ScriptExtender.h>

#include <imgui.h>

#include <backends/imgui_impl_sdl2.h>

#include <mutex>

#include "../log.h"
#include "../sdl_input_ownership.h"

namespace bg3le {
void imgui_apply_injected_input();
void imgui_overlay_tick();
}

BEGIN_SE()

SDLManager::SDLManager() = default;

SDLManager::~SDLManager()
{
    DestroyUI();
}

// Nothing to install: the forwarders are exported by the library itself and
// are in place before the game's first SDL call.
void SDLManager::EnableHooks()
{
    bg3le::logf("SDLManager: SDL calls arrive through bg3le's exported "
                "forwarders; no hooks to install");
}

void SDLManager::DisableHooks() {}

void SDLManager::InitializeUI()
{
    if (enableUI_) return;

    if (window_ == nullptr) {
        bg3le::logf("SDLManager: no window yet; the overlay will have no "
                    "viewport or input");
        return;
    }

    ImGui_ImplSDL2_InitForVulkan(window_);
    enableUI_ = true;
    bg3le::logf("SDLManager: imgui bound to the game's SDL window");
}

void SDLManager::DestroyUI()
{
    if (!enableUI_) return;

    enableUI_ = false;
    ImGui_ImplSDL2_Shutdown();
}

void SDLManager::NewFrame()
{
    if (!enableUI_) return;

    // The backend shows the OS cursor every frame, which undoes the game
    // hiding it (controller input, for one). It may change the cursor only
    // while the pointer is over the overlay; the game's is restored after.
    static bool overOverlay = false;
    static SDL_Cursor* gameCursor = nullptr;
    static int gameCursorShown = SDL_ENABLE;
    auto& io = ImGui::GetIO();
    const bool over = io.WantCaptureMouse;
    if (over && !overOverlay) {
        gameCursor = SDL_GetCursor();
        gameCursorShown = SDL_ShowCursor(SDL_QUERY);
    } else if (!over && overOverlay) {
        // SDL ignores a cursor the game has freed since.
        if (gameCursor != nullptr) SDL_SetCursor(gameCursor);
        SDL_ShowCursor(gameCursorShown);
    }
    overOverlay = over;
    if (over) io.ConfigFlags &= ~ImGuiConfigFlags_NoMouseCursorChange;
    else io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;

    ImGui_ImplSDL2_NewFrame();

    // After the backend, not before: see imgui_apply_injected_input.
    bg3le::imgui_apply_injected_input();
}

void SDLManager::InjectEvent(SDL_Event const& evt)
{
    std::lock_guard _(injectedEventMutex_);
    injectedEvents_.push_back(evt);
}

void SDLManager::OnCreateWindow(SDL_Window* window)
{
    window_ = window;

    // The overlay's hooks are installed from the first vkCreateInstance,
    // which the engine reaches before it makes its window, so by the time
    // there is a window IMGUIManager::InitializeUI has already run and
    // found none. Bind now instead.
    if (!enableUI_ && gExtender != nullptr
        && gExtender->IMGUI().WasUIInitialized()) {
        InitializeUI();
    }
}

bool SDLManager::WantsTextInput() const
{
    return enableUI_ && ImGui::GetCurrentContext() != nullptr
           && ImGui::GetIO().WantTextInput;
}

int SDLManager::OnPollEvent(SDLPollEventProc* wrapped, SDL_Event* event)
{
    if (!injectedEvents_.empty()) {
        std::lock_guard _(injectedEventMutex_);
        if (!injectedEvents_.empty()) {
            *event = *injectedEvents_.begin();
            injectedEvents_.ordered_remove_at(0);
            return 1;
        }
    }

    // An event the overlay keeps is skipped over, not answered with 0 as
    // upstream does: 0 tells the game the queue is empty, so it stopped
    // polling with events still queued. Linux delivers every pointer motion
    // (Windows coalesces them), so with the cursor over a window the game
    // took one event a frame, the rest backed up, and the overlay -- drawn
    // when the queue empties -- skipped frames.
    for (int skipped = 0;; ++skipped) {
        int result = wrapped(event);

        // The end of the frame's event batch, which is where the overlay is
        // drawn from: upstream draws it from the client's own update, on the
        // thread that polls SDL, and zero from SDL_PollEvent is that point.
        if (result == 0) {
            bg3le::imgui_overlay_tick();
            return 0;
        }

        if (!enableUI_ || ImGui::GetCurrentContext() == nullptr) return result;

        {
            std::lock_guard _(mutex_);
            ImGui_ImplSDL2_ProcessEvent(event);
        }

        // Kept rather than passed on, so typing in a widget does not also
        // drive the game.
        ImGuiIO& io = ImGui::GetIO();
        const bool text = io.WantTextInput
            && (event->type == SDL_KEYDOWN || event->type == SDL_KEYUP
                || event->type == SDL_TEXTEDITING
                || event->type == SDL_TEXTINPUT);
        const bool mouse = io.WantCaptureMouse
            && (event->type == SDL_MOUSEMOTION
                || event->type == SDL_MOUSEBUTTONDOWN
                || event->type == SDL_MOUSEBUTTONUP
                || event->type == SDL_MOUSEWHEEL);
        // Capture may begin while a game key/button is already held. ImGui has
        // processed this release too; keep the game's input state balanced.
        if ((!text && !mouse) || bg3le::sdl_preserve_game_release(event)) return 1;
        // A bound on a stream that never ends; the next call carries on.
        if (skipped >= 256) return 0;
    }
}

void SDLManager::OnIsTextInputActive(SDL_bool active)
{
    if (WantsTextInput() && !active) {
        SDL_StartTextInput();
    }
}

// The detoured forms upstream registers. Unused here -- the forwarders call
// the On* entry points above -- but defined because the class declares them.
void SDLManager::SDLCreateWindowHooked(const char* title, int x, int y, int w,
                                       int h, uint32_t flags,
                                       SDL_Window* window)
{
    (void)title;
    (void)x;
    (void)y;
    (void)w;
    (void)h;
    (void)flags;
    OnCreateWindow(window);
}

int SDLManager::SDLPollEventHooked(SDLPollEventProc* wrapped, SDL_Event* event)
{
    return OnPollEvent(wrapped, event);
}

int SDLManager::SDLPollEventInternal(SDL_Event* event, int result)
{
    (void)event;
    return result;
}

int SDLManager::SDLPollEventSEH(SDL_Event* event, int result)
{
    return SDLPollEventInternal(event, result);
}

void SDLManager::SDLIsTextInputActiveHooked(SDL_bool active)
{
    OnIsTextInputActive(active);
}

void SDLManager::SDLStartTextInputHooked(SDLStartTextInputProc* wrapped)
{
    wrapped();
}

void SDLManager::SDLStopTextInputHooked(SDLStartTextInputProc* wrapped)
{
    if (WantsTextInput()) {
        if (!SDL_IsTextInputActive()) SDL_StartTextInput();
    } else {
        wrapped();
    }
}

END_SE()

// ---------------------------------------------------------------------------
// What src/sdl_forward.cpp calls.
//
// Plain functions over SDL's own types rather than the manager itself:
// SDLManager.h pulls in the vendored headers, and the forwarders are in
// bg3le's own target, which does not compile against those.
// ---------------------------------------------------------------------------

namespace bg3le {

namespace {

// The one manager the overlay draws through, or null before the extender
// object exists.
bg3se::SDLManager* manager() {
    if (bg3se::gExtender == nullptr) return nullptr;
    return &bg3se::gExtender->GetClient().GetSDL();
}

}  // namespace

void sdl_on_create_window(SDL_Window* window) {
    if (auto* sdl = manager()) sdl->OnCreateWindow(window);
}

int sdl_on_poll_event(int (*next)(SDL_Event*), SDL_Event* event) {
    auto* sdl = manager();
    if (sdl == nullptr) return next(event);
    return sdl->OnPollEvent(next, event);
}

void sdl_on_text_input_active(bool active) {
    if (auto* sdl = manager()) {
        sdl->OnIsTextInputActive(active ? SDL_TRUE : SDL_FALSE);
    }
}

bool sdl_wants_text_input() {
    auto* sdl = manager();
    return sdl != nullptr && sdl->WantsTextInput();
}

}  // namespace bg3le
