// OpenGL 3.3 renderer. Owns all GL objects. Knows about the game state only
// through the read-only World interface; gameplay never calls into this.
#pragma once

#include <vector>

#include "game/world.hpp"

namespace ab {

// Positions of moving objects at the previous simulation step, used to
// interpolate between steps when drawing.
struct RenderSnapshot {
    struct Pos {
        int x = 0;
        int y = 0;
    };
    std::array<Pos, kMaxPlayers> players{};
    std::array<Pos, kMaxBombs> bombs{};
    void capture(const World& w);
};

class Renderer {
public:
    Renderer();
    ~Renderer();
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    // alpha in [0,1]: how far the frame is between the previous and the current step.
    void draw(const World& world, const RenderSnapshot& previous, float alpha, int windowW, int windowH);

private:
    struct Vertex {
        float x, y;
        float r, g, b, a;
    };
    void quad(float x, float y, float w, float h, float r, float g, float b, float a = 1.0f);
    void flush();

    unsigned program_ = 0;
    unsigned vao_ = 0;
    unsigned vbo_ = 0;
    int projLoc_ = -1;
    std::vector<Vertex> batch_;
};

}  // namespace ab
