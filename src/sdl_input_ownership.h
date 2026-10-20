#pragma once

#include <SDL.h>
#include <array>
#include <unordered_set>

namespace bg3le {
// Once the game has received a press, overlay capture must preserve its matching
// release. Later input handlers still observe that rescued release, but cannot
// cancel it; releases outside overlay capture retain their cancellation semantics.
class GameInputOwnership {
public:
    bool OwnsRelease(SDL_Event const& event) const {
        if (event.type == SDL_KEYUP) {
            const int key = event.key.keysym.scancode;
            return key > 0 && key < SDL_NUM_SCANCODES && keys_[key];
        }
        return event.type == SDL_MOUSEBUTTONUP && event.button.button != 0
            && buttons_.find(MouseKey(event)) != buttons_.end();
    }

    // Call only when SDL_PollEvent is about to return this event to the game.
    void Delivered(SDL_Event const& event) {
        if (event.type == SDL_KEYDOWN || event.type == SDL_KEYUP) {
            const int key = event.key.keysym.scancode;
            if (key > 0 && key < SDL_NUM_SCANCODES)
                keys_[key] = event.type == SDL_KEYDOWN;
        } else if ((event.type == SDL_MOUSEBUTTONDOWN || event.type == SDL_MOUSEBUTTONUP)
                   && event.button.button != 0) {
            if (event.type == SDL_MOUSEBUTTONDOWN) buttons_.insert(MouseKey(event));
            else buttons_.erase(MouseKey(event));
        }
    }
private:
    std::array<bool, SDL_NUM_SCANCODES> keys_{};
    static uint64_t MouseKey(SDL_Event const& event) {
        return (uint64_t(event.button.which) << 8) | event.button.button;
    }
    std::unordered_set<uint64_t> buttons_;
};

// The overlay asks before discarding a release. Ownership is not consumed here;
// later Lua/plugin filters must see the same decision until delivery is final.
// Releases not rescued from overlay capture retain normal cancellation semantics.
bool sdl_preserve_game_release(SDL_Event const* event);
}
