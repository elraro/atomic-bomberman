#include "rendering/renderer.hpp"

#include "rendering/gl.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>
#include <cstdio>

namespace ab {

namespace {

constexpr float kScreenW = 640.0f;
constexpr float kScreenH = 480.0f;

const char* kVertexSrc = R"(#version 330 core
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aUv;
layout(location = 2) in vec4 aColor;
uniform mat4 uProj;
out vec4 vColor;
out vec2 vUv;
void main() { vColor = aColor; vUv = aUv; gl_Position = uProj * vec4(aPos, 0.0, 1.0); }
)";

const char* kFragmentSrc = R"(#version 330 core
in vec4 vColor;
in vec2 vUv;
uniform sampler2D uTex;
out vec4 oColor;
void main() { oColor = texture(uTex, vUv) * vColor; }
)";

unsigned compile(GLenum type, const char* src) {
    const unsigned s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (ok == 0) {
        char log[512];
        glGetShaderInfoLog(s, sizeof log, nullptr, log);
        std::fprintf(stderr, "ERROR shader compile: %s\n", log);
    }
    return s;
}

// Colours for the ten player slots (the original's .rmp order: white, black,
// red, blue, green, yellow, cyan, magenta, orange, purple).
constexpr float kPlayerColor[kMaxPlayers][3] = {
    {0.95f, 0.95f, 0.95f}, {0.25f, 0.25f, 0.25f}, {0.90f, 0.15f, 0.15f}, {0.20f, 0.35f, 0.95f}, {0.15f, 0.80f, 0.20f},
    {0.95f, 0.90f, 0.15f}, {0.15f, 0.85f, 0.85f}, {0.90f, 0.20f, 0.90f}, {0.95f, 0.55f, 0.10f}, {0.55f, 0.20f, 0.90f}};

float lerp(int a, int b, float t) { return static_cast<float>(a) + static_cast<float>(b - a) * t; }

// Height above the ground at which a bomb is drawn: an arc while flying
// (original values 660/661: 65 px for the first three-cell hop, 20 px for later
// one-cell bounces), and over the head while carried.
float bombLift(const Bomb& b) {
    if (b.mode == BombMode::Held) return 44.0f;
    if (b.mode != BombMode::Flying) return 0.0f;
    const bool vertical = (b.dir & 1) == 0;
    const float cell = static_cast<float>(vertical ? kCellH : kCellW);
    const bool firstHop = b.hops < 3;
    const float length = firstHop ? 3.0f * cell : cell;
    const float t = std::clamp(static_cast<float>(b.flightPx) / length, 0.0f, 1.0f);
    return std::sin(t * 3.14159265f) * (firstHop ? 65.0f : 20.0f);
}

// A bomb that wrapped around the field must not be drawn sliding across the screen.
float lerpBomb(int a, int b, float t) { return std::abs(b - a) > 100 ? static_cast<float>(b) : lerp(a, b, t); }

}  // namespace

void RenderSnapshot::capture(const World& w) {
    for (int i = 0; i < kMaxPlayers; ++i)
        players[static_cast<std::size_t>(i)] = {w.player(i).x, w.player(i).y};
    for (std::size_t i = 0; i < w.bombs().size() && i < bombs.size(); ++i) bombs[i] = {w.bombs()[i].x, w.bombs()[i].y};
}

Renderer::Renderer() {
    const unsigned vs = compile(GL_VERTEX_SHADER, kVertexSrc);
    const unsigned fs = compile(GL_FRAGMENT_SHADER, kFragmentSrc);
    program_ = glCreateProgram();
    glAttachShader(program_, vs);
    glAttachShader(program_, fs);
    glLinkProgram(program_);
    glDeleteShader(vs);
    glDeleteShader(fs);
    projLoc_ = glGetUniformLocation(program_, "uProj");

    glGenVertexArrays(1, &vao_);
    glGenBuffers(1, &vbo_);
    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), nullptr);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                          reinterpret_cast<const void*>(2 * sizeof(float)));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                          reinterpret_cast<const void*>(4 * sizeof(float)));
    glBindVertexArray(0);

    const unsigned char whitePixel[4] = {255, 255, 255, 255};
    glGenTextures(1, &white_);
    glBindTexture(GL_TEXTURE_2D, white_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, whitePixel);
    current_ = white_;
}

Renderer::~Renderer() {
    glDeleteTextures(1, &white_);
    glDeleteBuffers(1, &vbo_);
    glDeleteVertexArrays(1, &vao_);
    glDeleteProgram(program_);
}

void Renderer::setTexture(unsigned texture) {
    if (texture == current_) return;
    flush();
    current_ = texture;
}

void Renderer::quad(float x, float y, float w, float h, float r, float g, float b, float a) {
    setTexture(white_);
    const Vertex v[6] = {{x, y, 0, 0, r, g, b, a},     {x + w, y, 0, 0, r, g, b, a},     {x + w, y + h, 0, 0, r, g, b, a},
                         {x, y, 0, 0, r, g, b, a},     {x + w, y + h, 0, 0, r, g, b, a}, {x, y + h, 0, 0, r, g, b, a}};
    batch_.insert(batch_.end(), v, v + 6);
}

void Renderer::textured(unsigned texture, float x, float y, float w, float h) {
    setTexture(texture);
    const Vertex v[6] = {{x, y, 0, 0, 1, 1, 1, 1},     {x + w, y, 1, 0, 1, 1, 1, 1},     {x + w, y + h, 1, 1, 1, 1, 1, 1},
                         {x, y, 0, 0, 1, 1, 1, 1},     {x + w, y + h, 1, 1, 1, 1, 1, 1}, {x, y + h, 0, 1, 1, 1, 1, 1}};
    batch_.insert(batch_.end(), v, v + 6);
}

void Renderer::text(const SpriteBank& bank, const std::string& s, float x, float y, float r, float g, float b) {
    const SpriteBank::Font& f = bank.font();
    if (f.texture == 0) return;
    setTexture(f.texture);
    const auto aw = static_cast<float>(f.atlasWidth);
    for (unsigned char ch : s) {
        if (ch >= f.width.size()) continue;
        const auto w = static_cast<float>(f.width[ch]);
        const auto h = static_cast<float>(f.height);
        const float u0 = static_cast<float>(f.x[ch]) / aw;
        const float u1 = (static_cast<float>(f.x[ch]) + w) / aw;
        const Vertex v[6] = {{x, y, u0, 0, r, g, b, 1},     {x + w, y, u1, 0, r, g, b, 1},     {x + w, y + h, u1, 1, r, g, b, 1},
                             {x, y, u0, 0, r, g, b, 1},     {x + w, y + h, u1, 1, r, g, b, 1}, {x, y + h, u0, 1, r, g, b, 1}};
        batch_.insert(batch_.end(), v, v + 6);
        x += w + static_cast<float>(f.spacing);
    }
}

bool Renderer::sprite(SpriteBank& bank, const std::string& sequence, int index, int colour, float x, float y) {
    const auto s = bank.sprite(sequence, index, colour);
    if (!s) return false;
    textured(s->texture, x - static_cast<float>(s->hotX), y - static_cast<float>(s->hotY),
             static_cast<float>(s->width), static_cast<float>(s->height));
    return true;
}

void Renderer::flush() {
    if (batch_.empty()) return;
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, current_);
    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(batch_.size() * sizeof(Vertex)), batch_.data(),
                 GL_STREAM_DRAW);
    glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(batch_.size()));
    glBindVertexArray(0);
    batch_.clear();
}

void Renderer::begin(int windowW, int windowH) {
    // Letterbox the 640x480 logical screen into the window.
    const float scale = std::min(static_cast<float>(windowW) / kScreenW, static_cast<float>(windowH) / kScreenH);
    const int vw = static_cast<int>(kScreenW * scale);
    const int vh = static_cast<int>(kScreenH * scale);
    glViewport(0, 0, windowW, windowH);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glViewport((windowW - vw) / 2, (windowH - vh) / 2, vw, vh);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    // Orthographic projection, origin top-left, y down, as on the original screen.
    const float proj[16] = {2.0f / kScreenW, 0, 0, 0, 0, -2.0f / kScreenH, 0, 0, 0, 0, -1, 0, -1, 1, 0, 1};
    glUseProgram(program_);
    glUniformMatrix4fv(projLoc_, 1, GL_FALSE, proj);
    glUniform1i(glGetUniformLocation(program_, "uTex"), 0);
}

void Renderer::end() { flush(); }

void Renderer::image(unsigned texture) {
    if (texture != 0)
        textured(texture, 0, 0, kScreenW, kScreenH);
    else
        quad(0, 0, kScreenW, kScreenH, 0.05f, 0.05f, 0.12f);
}

void Renderer::draw(const World& world, const RenderSnapshot& prev, float alpha, int windowW, int windowH,
                    SpriteBank* sprites, const std::array<int, kMaxPlayers>* wins) {
    begin(windowW, windowH);
    if (sprites != nullptr && sprites->loaded())
        drawSprites(world, prev, alpha, *sprites);
    else
        drawShapes(world, prev, alpha);
    drawHud(world, sprites, wins);
    flush();
}

// Seven-segment digit, 14 x 24 px. Stand-in until the original font files are decoded.
void Renderer::digit(int value, float x, float y, float r, float g, float b) {
    static const unsigned char kSegments[10] = {0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F};
    const unsigned char s = kSegments[value % 10];
    const float t = 3.0f;
    if (s & 0x01) quad(x, y, 14, t, r, g, b);
    if (s & 0x02) quad(x + 14 - t, y, t, 12, r, g, b);
    if (s & 0x04) quad(x + 14 - t, y + 12, t, 12, r, g, b);
    if (s & 0x08) quad(x, y + 24 - t, 14, t, r, g, b);
    if (s & 0x10) quad(x, y + 12, t, 12, r, g, b);
    if (s & 0x20) quad(x, y, t, 12, r, g, b);
    if (s & 0x40) quad(x, y + 12 - t / 2, 14, t, r, g, b);
}

void Renderer::drawHud(const World& world, SpriteBank* bank, const std::array<int, kMaxPlayers>* wins) {
    const int left = world.secondsLeft();
    const bool original = bank != nullptr && bank->loaded() && bank->sequenceLength("numeric font") >= 11;
    if (original) {
        // The original's clock: "numeric font" digits (index 10 is the colon) starting at
        // x 525, baseline y 36, 4 px between glyphs (values 110-112).
        if (left >= 0) {
            const int glyphs[4] = {left / 60, 10, (left % 60) / 10, left % 10};
            float x = 525.0f;
            for (int g : glyphs) {
                const auto s = bank->sprite("numeric font", g, -1);
                if (!s) continue;
                textured(s->texture, x - static_cast<float>(s->hotX), 36.0f - static_cast<float>(s->hotY),
                         static_cast<float>(s->width), static_cast<float>(s->height));
                x += static_cast<float>(s->width + 4);
            }
        } else {
            sprite(*bank, "infinity", 0, -1, 525.0f, 36.0f);
        }
        // The banner blinks in the middle of the screen while the warning is on.
        if (world.hurry() && (world.tickCount() & 4) != 0) sprite(*bank, "hurry", 0, -1, 320.0f, 240.0f);
    } else if (left >= 0) {
        const bool red = left < 31;
        const float r = red ? 0.95f : 0.85f, g = red ? 0.25f : 0.75f, b = red ? 0.20f : 0.35f;
        const float x = 520.0f, y = 10.0f;
        digit(left / 60, x, y, r, g, b);
        quad(x + 20, y + 6, 3, 3, r, g, b);
        quad(x + 20, y + 16, 3, 3, r, g, b);
        digit((left % 60) / 10, x + 29, y, r, g, b);
        digit(left % 10, x + 49, y, r, g, b);
    }
    if (!original && world.hurry() && (world.tickCount() / 4) % 2 == 0) quad(250, 14, 140, 16, 0.95f, 0.85f, 0.20f);
    if (original && bank->font().texture != 0) {
        // Score labels as in the original: message 37 "S:%d K:%d" (round wins, kills) in the
        // player's colour; columns at x 10, 110, ... (values 115-119), rows at y 6 and 26 (113/114).
        for (int i = 0; i < kMaxPlayers; ++i) {
            const Player& p = world.player(i);
            if (!p.present) continue;
            const float* c = kPlayerColor[world.displayColour(i)];  // team colour in team play
            const std::string label = "S:" + std::to_string(wins != nullptr ? (*wins)[static_cast<std::size_t>(i)] : 0) +
                                      " K:" + std::to_string(p.kills);
            const float lx = 10.0f + static_cast<float>(i / 2) * 100.0f;
            const float ly = 6.0f + static_cast<float>(i & 1) * 20.0f;
            const bool dark = c[0] + c[1] + c[2] < 1.0f;
            if (dark) {
                // The black player's label is dark with a light outline, as in the original.
                for (int o = 0; o < 4; ++o)
                    text(*bank, label, lx + static_cast<float>(kDx[static_cast<unsigned>(o)]),
                         ly + static_cast<float>(kDy[static_cast<unsigned>(o)]), 0.80f, 0.80f, 0.80f);
                text(*bank, label, lx, ly, 0.0f, 0.0f, 0.0f);
            } else {
                text(*bank, label, lx, ly, c[0], c[1], c[2]);
            }
        }
        return;
    }
    // Kill scores: one coloured pip per player with kills shown as small bars.
    for (int i = 0; i < kMaxPlayers; ++i) {
        const Player& p = world.player(i);
        if (!p.present) continue;
        const float* c = kPlayerColor[i];
        const float px = 12.0f + static_cast<float>(i % 5) * 100.0f;
        const float py = 8.0f + static_cast<float>(i / 5) * 20.0f;
        quad(px, py, 10, 10, c[0], c[1], c[2], p.alive ? 1.0f : 0.35f);
        for (int k = 0; k < p.kills && k < 12; ++k) quad(px + 14 + static_cast<float>(k) * 5, py + 2, 3, 6, 0.9f, 0.9f, 0.9f);
    }
}

namespace {

const char* const kDirName[4] = {"north", "east", "south", "west"};
const char* const kPowerName[kPowTypeCount] = {"bomb",   "flame",     "disease", "kicker", "skate",    "punch",  "grab", "spooge",
                                               "goldflame", "trigger", "jelly",  "disease3", "random", "clog", "clog"};

}  // namespace

void Renderer::drawSprites(const World& world, const RenderSnapshot& prev, float alpha, SpriteBank& bank) {
    if (bank.background() != 0)
        textured(bank.background(), 0, 0, kScreenW, kScreenH);
    else
        quad(0, 0, kScreenW, kScreenH, 0.10f, 0.12f, 0.16f);

    const std::string lv = std::to_string(bank.level());
    const int frame = world.tickCount();  // one animation frame per 50 ms step

    for (int y = 0; y < kGridH; ++y)
        for (int x = 0; x < kGridW; ++x) {
            const auto rx = static_cast<float>(cellToPixelX(x));
            const auto ry = static_cast<float>(cellToPixelY(y));
            const Tile t = world.tile({x, y});
            const Flame& f = world.flame({x, y});
            if (t == Tile::Solid) sprite(bank, "tile " + lv + " solid", 0, -1, rx, ry);
            if (t == Tile::Brick && !(f.active && f.burningBrick)) sprite(bank, "tile " + lv + " brick", 0, -1, rx, ry);
            const Powerup& pu = world.powerup({x, y});
            if (pu.state == PowerupState::Revealed && t != Tile::Brick)
                sprite(bank, std::string("power ") + kPowerName[pu.type], frame, -1, rx, ry);
            if (f.active) {
                const int age = f.ageMs / kFrameMs;
                if (f.burningBrick) {
                    const std::string seq = "flame brick " + lv;
                    sprite(bank, seq, std::min(age, std::max(0, bank.sequenceLength(seq) - 1)), -1, rx, ry);
                } else {
                    std::string piece = "center";
                    if (f.dir != kNoDir) piece = std::string(f.tip ? "tip" : "mid") + kDirName[f.dir];
                    sprite(bank, "flame " + piece + " green", age, f.owner >= 0 ? world.displayColour(f.owner) : -1, rx, ry);
                }
            }
        }

    // Level extras lie on the ground, visible once their cell is open.
    for (const Extra& e : world.extras()) {
        if (world.tile(e.cell) != Tile::Blank) continue;
        const auto rx = static_cast<float>(cellToPixelX(e.cell.x));
        const auto ry = static_cast<float>(cellToPixelY(e.cell.y));
        switch (e.type) {
            case ExtraType::Arrow: sprite(bank, std::string("extra arrow ") + kDirName[e.dir & 3], 0, -1, rx, ry); break;
            case ExtraType::Conveyor: sprite(bank, std::string("extra conveyor ") + kDirName[e.dir & 3], frame / 3, -1, rx, ry); break;
            case ExtraType::Warp: sprite(bank, "extra warp 1", frame, -1, rx, ry); break;
            case ExtraType::Trampoline: sprite(bank, "extra trampoline", e.animFrame, -1, rx, ry); break;
        }
    }

    std::size_t bi = 0;
    for (const Bomb& b : world.bombs()) {
        const std::size_t idx = bi++;
        if (!b.active) continue;
        const float bx = lerpBomb(prev.bombs[idx].x, b.x, alpha);
        const float by = lerpBomb(prev.bombs[idx].y, b.y, alpha) - bombLift(b);
        const char* seq = b.dud                        ? "bomb regular green dud"
                        : b.type == BombType::Trigger ? "bomb trigger green"
                        : b.type == BombType::Jelly   ? "bomb jelly green"
                                                      : "bomb regular green";
        sprite(bank, seq, frame - b.createdTick, b.owner >= 0 ? world.displayColour(b.owner) : -1, bx, by);
    }

    // Players are drawn back to front.
    std::array<int, kMaxPlayers> order{};
    for (int i = 0; i < kMaxPlayers; ++i) order[static_cast<std::size_t>(i)] = i;
    std::sort(order.begin(), order.end(), [&](int a, int b2) { return world.player(a).y < world.player(b2).y; });
    for (int i : order) {
        const Player& p = world.player(i);
        if (!p.present) continue;
        const int colour = world.displayColour(i);
        if (!p.alive) {
            // Death animation, one step per 50 ms, shown once.
            const std::string seq = "die green " + std::to_string(std::max(1, p.deathAnim));
            if (p.dying && p.dyingFrames < bank.sequenceLength(seq))
                sprite(bank, seq, p.dyingFrames, colour, static_cast<float>(p.x), static_cast<float>(p.y));
            continue;
        }
        if (p.special == Special::WarpOut || p.special == Special::WarpIn) continue;  // inside the warp
        float px = lerp(prev.players[static_cast<std::size_t>(i)].x, p.x, alpha);
        float py = lerp(prev.players[static_cast<std::size_t>(i)].y, p.y, alpha);
        if (p.special == Special::Trampoline) {
            // Up for the first half of the jump, down for the second (original values 680/681).
            px = static_cast<float>(p.x);
            const int f = p.specialFrames;
            py = static_cast<float>(p.y) - static_cast<float>((f < 15 ? f : 30 - f) * 35);
        }
        sprite(bank, "shadow", 0, -1, px, static_cast<float>(p.y));
        const std::string dir = kDirName[static_cast<unsigned>(p.facing) & 3u];
        const bool carrying = p.holding >= 0;
        bool drawn = false;
        if (p.action == 1) drawn = sprite(bank, "kick " + dir, p.actionFrames, colour, px, py);
        if (p.action == 2) drawn = sprite(bank, "punch " + dir, p.actionFrames, colour, px, py);
        if (!drawn && carrying)
            drawn = sprite(bank, std::string(p.moving ? "walkbomb " : "standbomb ") + dir, p.moving ? p.animCounter / 3 : 0, colour, px, py);
        if (!drawn) {
            if (p.moving)
                sprite(bank, "walk " + dir, p.animCounter / 3, colour, px, py);
            else
                sprite(bank, "stand " + dir, 0, colour, px, py);
        }
    }
}

void Renderer::drawShapes(const World& world, const RenderSnapshot& prev, float alpha) {
    quad(0, 0, kScreenW, kScreenH, 0.10f, 0.12f, 0.16f);

    const auto cw = static_cast<float>(kCellW);
    const auto ch = static_cast<float>(kCellH);
    for (int y = 0; y < kGridH; ++y)
        for (int x = 0; x < kGridW; ++x) {
            const float px = static_cast<float>(kOriginX + x * kCellW);
            const float py = static_cast<float>(kOriginY + y * kCellH);
            const bool dark = ((x + y) & 1) != 0;
            quad(px, py, cw, ch, dark ? 0.16f : 0.19f, dark ? 0.42f : 0.46f, dark ? 0.20f : 0.22f);
            const Tile t = world.tile({x, y});
            if (t == Tile::Solid) {
                quad(px + 1, py + 1, cw - 2, ch - 2, 0.50f, 0.52f, 0.56f);
                quad(px + 5, py + 5, cw - 10, ch - 10, 0.62f, 0.64f, 0.68f);
            } else if (t == Tile::Brick) {
                quad(px + 1, py + 1, cw - 2, ch - 2, 0.62f, 0.36f, 0.18f);
                quad(px + 3, py + 15, cw - 6, 3, 0.40f, 0.22f, 0.10f);
            }
            const Powerup& pu = world.powerup({x, y});
            if (pu.state == PowerupState::Revealed) {
                const float hue = static_cast<float>(pu.type) / 13.0f;
                quad(px + 8, py + 6, cw - 16, ch - 12, 0.95f, 0.95f, 0.30f);
                quad(px + 11, py + 9, cw - 22, ch - 18, 0.2f + 0.8f * hue, 0.9f - 0.7f * hue, 0.9f);
            }
            const Flame& f = world.flame({x, y});
            if (f.active) {
                const float fade = 1.0f - static_cast<float>(f.ageMs) / 700.0f;
                if (f.burningBrick)
                    quad(px + 2, py + 2, cw - 4, ch - 4, 1.0f, 0.45f, 0.10f, 0.85f * fade);
                else {
                    quad(px, py, cw, ch, 1.0f, 0.55f, 0.05f, 0.9f * fade);
                    quad(px + 8, py + 7, cw - 16, ch - 14, 1.0f, 0.95f, 0.50f, fade);
                }
            }
        }

    // Object reference points are bottom-centre of the cell (hot-spot 20,35).
    std::size_t bi = 0;
    for (const Bomb& b : world.bombs()) {
        const std::size_t idx = bi++;
        if (!b.active) continue;
        const float bx = lerp(prev.bombs[idx].x, b.x, alpha);
        const float by = lerp(prev.bombs[idx].y, b.y, alpha);
        const float pulse = 1.0f + 0.12f * static_cast<float>((b.elapsedMs / 100) % 2);
        const float s = 13.0f * pulse;
        const float cy = by - 17.0f;
        float r = 0.08f, g = 0.08f, bl = 0.10f;
        if (b.type == BombType::Trigger) r = 0.55f;
        if (b.type == BombType::Jelly) g = 0.45f;
        quad(bx - s, cy - s * 0.6f, 2 * s, 1.2f * s, r, g, bl);
        quad(bx - s * 0.6f, cy - s, 1.2f * s, 2 * s, r, g, bl);
        quad(bx - 3, cy - s - 5, 6, 6, 0.95f, 0.80f, 0.20f);
    }

    for (int i = 0; i < kMaxPlayers; ++i) {
        const Player& p = world.player(i);
        if (!p.present) continue;
        const float* c = kPlayerColor[i];
        const float pxf = p.alive ? lerp(prev.players[static_cast<std::size_t>(i)].x, p.x, alpha) : static_cast<float>(p.x);
        const float pyf = p.alive ? lerp(prev.players[static_cast<std::size_t>(i)].y, p.y, alpha) : static_cast<float>(p.y);
        if (!p.alive) {
            if (p.dyingFrames < 20) quad(pxf - 12, pyf - 8, 24, 8, c[0], c[1], c[2], 0.5f);
            continue;
        }
        quad(pxf - 11, pyf - 30, 22, 30, 0.05f, 0.05f, 0.05f);        // outline
        quad(pxf - 9, pyf - 28, 18, 26, c[0], c[1], c[2]);             // body
        quad(pxf - 9, pyf - 40, 18, 14, 0.98f, 0.92f, 0.85f);          // head
        const auto f = static_cast<unsigned>(p.facing);
        quad(pxf - 2 + static_cast<float>(kDx[f]) * 6, pyf - 35 + static_cast<float>(kDy[f]) * 4, 4, 4, 0.05f, 0.05f, 0.05f);
    }
    flush();
}

}  // namespace ab
