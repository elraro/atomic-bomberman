#include "app/touch.hpp"

#include <cmath>
#include <vector>

namespace ab {

namespace {
constexpr float kPadReach = 1.7f;   // a finger this many radii from the pad's centre still steers
constexpr float kDeadZone = 0.28f;  // of the radius: no direction closer to the centre

std::vector<std::uint8_t> discPixels(bool ringOnly) {
    constexpr int n = 96;
    std::vector<std::uint8_t> px(static_cast<std::size_t>(n * n * 4), 255);
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x) {
            const float dx = (static_cast<float>(x) + 0.5f) / n * 2.0f - 1.0f, dy = (static_cast<float>(y) + 0.5f) / n * 2.0f - 1.0f;
            const float d = std::sqrt(dx * dx + dy * dy);
            float a = d < 0.97f ? 1.0f : d < 1.0f ? (1.0f - d) / 0.03f : 0.0f;
            if (ringOnly && d < 0.88f) a = 0.25f;
            px[static_cast<std::size_t>((y * n + x) * 4 + 3)] = static_cast<std::uint8_t>(a * 255.0f);
        }
    return px;
}
}  // namespace

// Where the controls sit. Coordinates are in window heights, x from the left edge for the
// pad and from the right edge for the buttons, so the layout holds for any screen shape.
TouchPad::Zone TouchPad::zone(int what, float aspect) const {
    switch (what) {
        case 0: return {0.20f, 0.74f, 0.13f};                 // direction pad
        case 1 + kA: return {aspect - 0.13f, 0.80f, 0.085f};  // bomb / Enter
        case 1 + kB: return {aspect - 0.30f, 0.86f, 0.075f};  // action / Space
        case 1 + kBack: return {0.07f, 0.08f, 0.05f};         // Esc
        default: return {aspect - 0.07f, 0.08f, 0.05f};       // chat (T)
    }
}

void TouchPad::press(SDL_Keycode key) const {
    SDL_Event e{};
    e.type = SDL_EVENT_KEY_DOWN;
    e.key.key = key;
    SDL_PushEvent(&e);
}

Dir TouchPad::padDirection() const {
    for (const Finger& f : fingers_) {
        if (!f.down || f.role != 0) continue;
        const Zone z = zone(0, aspect_);
        const float dx = f.x * aspect_ - z.x, dy = f.y - z.y;
        if (std::sqrt(dx * dx + dy * dy) < z.r * kDeadZone) return kNoDir;
        if (std::abs(dx) > std::abs(dy)) return dx > 0 ? 1 : 3;
        return dy > 0 ? 2 : 0;
    }
    return kNoDir;
}

// A finger has come down or moved: which control it is on. A finger keeps the control it
// first touched, so a thumb sliding off the pad's edge goes on steering.
void TouchPad::place(Finger& f, float aspect) {
    if (f.role >= 0) return;
    const float x = f.x * aspect, y = f.y;
    for (int what = kButtons; what >= 0; --what) {
        const Zone z = zone(what, aspect);
        const float reach = what == 0 ? z.r * kPadReach : z.r * 1.35f;
        const float dx = x - z.x, dy = y - z.y;
        if (dx * dx + dy * dy > reach * reach) continue;
        f.role = what;
        if (what >= 1) {
            const int b = what - 1;
            pressed_[static_cast<std::size_t>(b)] = true;
            press(b == kA ? SDLK_RETURN : b == kB ? SDLK_SPACE : b == kBack ? SDLK_ESCAPE : SDLK_T);
        }
        return;
    }
}

void TouchPad::release(int role) {
    if (role < 1) return;
    bool still = false;
    for (const Finger& f : fingers_) still = still || (f.down && f.role == role);
    if (!still) pressed_[static_cast<std::size_t>(role - 1)] = false;
}

void TouchPad::handle(const SDL_Event& e, int windowW, int windowH) {
    if (!enabled_ || windowW <= 0 || windowH <= 0) return;
    aspect_ = static_cast<float>(windowW) / static_cast<float>(windowH);
    bool down = false, up = false, move = false;
    SDL_FingerID id = 0;
    float x = 0, y = 0;
    if (e.type == SDL_EVENT_FINGER_DOWN || e.type == SDL_EVENT_FINGER_MOTION || e.type == SDL_EVENT_FINGER_UP) {
        down = e.type == SDL_EVENT_FINGER_DOWN, up = e.type == SDL_EVENT_FINGER_UP, move = e.type == SDL_EVENT_FINGER_MOTION;
        id = e.tfinger.fingerID + 1;
        x = e.tfinger.x, y = e.tfinger.y;
    } else if ((e.type == SDL_EVENT_MOUSE_BUTTON_DOWN || e.type == SDL_EVENT_MOUSE_BUTTON_UP) && e.button.which != SDL_TOUCH_MOUSEID) {
        // The mouse as a finger, for trying the controls on a desktop.
        down = e.type == SDL_EVENT_MOUSE_BUTTON_DOWN, up = !down;
        x = e.button.x / static_cast<float>(windowW), y = e.button.y / static_cast<float>(windowH);
    } else if (e.type == SDL_EVENT_MOUSE_MOTION && e.motion.which != SDL_TOUCH_MOUSEID) {
        move = true;
        x = e.motion.x / static_cast<float>(windowW), y = e.motion.y / static_cast<float>(windowH);
    } else {
        return;
    }
    Finger* finger = nullptr;
    for (Finger& f : fingers_)
        if (f.down && f.id == id) finger = &f;
    if (down && finger == nullptr)
        for (Finger& f : fingers_)
            if (!f.down && finger == nullptr) finger = &f, f = Finger{};
    if (finger == nullptr) return;
    if (up) {
        const int role = finger->role;
        finger->down = false;
        release(role);
        if (role == 0) held_ = kNoDir;
        return;
    }
    if (!down && !move) return;
    finger->down = true;
    finger->id = id;
    finger->x = x;
    finger->y = y;
    if (down) place(*finger, aspect_);
    // A new direction on the pad is a key press for the menus (and repeats while held).
    const Dir now = padDirection();
    if (now != held_) {
        held_ = now;
        static constexpr SDL_Keycode kKey[4] = {SDLK_UP, SDLK_RIGHT, SDLK_DOWN, SDLK_LEFT};
        if (now != kNoDir) press(kKey[now]);
        repeatAt_ = SDL_GetTicks() + 400;
    }
}

void TouchPad::update(std::uint64_t nowMs) {
    if (!enabled_ || held_ == kNoDir || nowMs < repeatAt_) return;
    static constexpr SDL_Keycode kKey[4] = {SDLK_UP, SDLK_RIGHT, SDLK_DOWN, SDLK_LEFT};
    press(kKey[held_]);
    repeatAt_ = nowMs + 130;
}

PlayerInput TouchPad::input() const {
    PlayerInput in;
    if (!enabled_) return in;
    if (held_ != kNoDir) in.dir[static_cast<std::size_t>(held_)] = true;
    in.button1 = pressed_[kA];
    in.button2 = pressed_[kB];
    return in;
}

void TouchPad::draw(Renderer& r, int pixelW, int pixelH) {
    if (!enabled_ || pixelW <= 0 || pixelH <= 0) return;
    if (disc_ == 0) disc_ = r.frameTexture(0, 96, 96, discPixels(false));
    if (ring_ == 0) ring_ = r.frameTexture(0, 96, 96, discPixels(true));
    const auto h = static_cast<float>(pixelH);
    const float aspect = static_cast<float>(pixelW) / h;
    r.beginWindow(pixelW, pixelH);
    auto disc = [&](unsigned texture, float cx, float cy, float radius, float red, float green, float blue, float alpha) {
        r.tinted(texture, (cx - radius) * h, (cy - radius) * h, radius * 2 * h, radius * 2 * h, red, green, blue, alpha);
    };
    // The pad: a ring, and a knob that leans the way it is pushed.
    const Zone pad = zone(0, aspect);
    disc(ring_, pad.x, pad.y, pad.r, 1, 1, 1, 0.45f);
    const float lean = held_ == kNoDir ? 0.0f : pad.r * 0.5f;
    disc(disc_, pad.x + (held_ == kNoDir ? 0.0f : static_cast<float>(kDx[static_cast<std::size_t>(held_)])) * lean,
         pad.y + (held_ == kNoDir ? 0.0f : static_cast<float>(kDy[static_cast<std::size_t>(held_)])) * lean, pad.r * 0.45f, 1, 1, 1, held_ == kNoDir ? 0.35f : 0.7f);
    static constexpr float kColour[kButtons][3] = {{0.95f, 0.30f, 0.25f}, {0.25f, 0.55f, 0.95f}, {0.8f, 0.8f, 0.8f}, {0.8f, 0.8f, 0.8f}};
    for (int b = 0; b < kButtons; ++b) {
        const Zone z = zone(1 + b, aspect);
        const bool on = pressed_[static_cast<std::size_t>(b)];
        disc(disc_, z.x, z.y, z.r, kColour[b][0], kColour[b][1], kColour[b][2], on ? 0.85f : 0.4f);
        disc(ring_, z.x, z.y, z.r, 1, 1, 1, 0.5f);
    }
    r.end();
}

}  // namespace ab
