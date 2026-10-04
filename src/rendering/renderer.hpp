// OpenGL 3.3 renderer. Owns all GL objects. Knows about the game state only
// through the read-only World interface; gameplay never calls into this.
#pragma once

#include <string>
#include <vector>

#include "game/world.hpp"
#include "rendering/sprites.hpp"

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
    // With a loaded SpriteBank the original artwork is used; otherwise flat shapes.
    void draw(const World& world, const RenderSnapshot& previous, float alpha, int windowW, int windowH,
              SpriteBank* sprites = nullptr, const std::array<int, kMaxPlayers>* wins = nullptr);

    // Building blocks for menu screens, in the 640x480 logical screen.
    void begin(int windowW, int windowH);
    void end();
    void image(unsigned texture);  // full-screen picture (dark fill if 0)
    void text(const SpriteBank& bank, const std::string& s, float x, float y, float r, float g, float b);
    bool sprite(SpriteBank& bank, const std::string& sequence, int index, int colour, float x, float y);
    void quad(float x, float y, float w, float h, float r, float g, float b, float a = 1.0f);

private:
    struct Vertex {
        float x, y;
        float u, v;
        float r, g, b, a;
    };
    void textured(unsigned texture, float x, float y, float w, float h);
    void setTexture(unsigned texture);
    void flush();
    void drawHud(const World& world, SpriteBank* bank, const std::array<int, kMaxPlayers>* wins);
    void digit(int value, float x, float y, float r, float g, float b);
    void drawShapes(const World& world, const RenderSnapshot& previous, float alpha);
    void drawSprites(const World& world, const RenderSnapshot& previous, float alpha, SpriteBank& bank);

    // Sparkles around the gold player (original: up to 100, sequence "goldman").
    struct Sparkle {
        bool active = false;
        float x = 0;
        float y = 0;
        int frame = 0;
    };
    std::array<Sparkle, 100> sparkles_{};
    int sparkleTick_ = -1;
    std::uint32_t sparkleRng_ = 99;

    unsigned program_ = 0;
    unsigned vao_ = 0;
    unsigned vbo_ = 0;
    int projLoc_ = -1;
    unsigned white_ = 0;    // 1x1 texture for untextured quads
    unsigned current_ = 0;  // texture of the pending batch
    std::vector<Vertex> batch_;
};

}  // namespace ab
