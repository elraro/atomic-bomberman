// On-screen controls for touch screens: a direction pad, two buttons and a
// back key. In a match they are a player's controller; everywhere they also
// act as the keys the screens are worked with (arrows, Enter, Space, Esc), so
// that menus need no keyboard. With --touch on a desktop the mouse is a finger.
#pragma once

#include <SDL3/SDL.h>

#include <array>
#include <cstdint>

#include "game/world.hpp"
#include "rendering/renderer.hpp"

namespace ab {

class TouchPad {
public:
    void enable(bool on) { enabled_ = on; }
    bool enabled() const { return enabled_; }

    // Finger (and, on a desktop, mouse) events. Sizes are the window's, in the units
    // the mouse reports. Key presses for the menus are pushed onto SDL's event queue.
    void handle(const SDL_Event& e, int windowW, int windowH);
    // Once per frame: repeats a held direction for the menus.
    void update(std::uint64_t nowMs);
    // The controller state for a match.
    PlayerInput input() const;
    void draw(Renderer& r, int pixelW, int pixelH);

private:
    enum Button { kA, kB, kBack, kChat, kButtons };
    struct Finger {
        bool down = false;
        SDL_FingerID id = 0;
        float x = 0, y = 0;  // 0-1 of the window
        int role = -1;       // -1 none, 0 pad, 1 + button
    };
    struct Zone {
        float x, y, r;  // centre and radius, in window heights from the left/top edge
    };
    Zone zone(int what, float aspect) const;  // 0 pad, 1 + button
    void press(SDL_Keycode key) const;
    void place(Finger& f, float aspect);
    void release(int role);
    Dir padDirection() const;

    bool enabled_ = false;
    std::array<Finger, 6> fingers_{};
    float aspect_ = 4.0f / 3.0f;
    Dir held_ = kNoDir;             // direction the pad is pushed
    std::uint64_t repeatAt_ = 0;    // when a held direction next counts as a key press
    std::array<bool, kButtons> pressed_{};
    unsigned disc_ = 0;             // textures
    unsigned ring_ = 0;
};

}  // namespace ab
