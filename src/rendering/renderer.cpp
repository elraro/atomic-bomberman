#include "rendering/renderer.hpp"

#define GL_GLEXT_PROTOTYPES 1
#include <GL/gl.h>
#include <GL/glext.h>

#include <algorithm>
#include <cstdio>

namespace ab {

namespace {

constexpr float kScreenW = 640.0f;
constexpr float kScreenH = 480.0f;

const char* kVertexSrc = R"(#version 330 core
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec4 aColor;
uniform mat4 uProj;
out vec4 vColor;
void main() { vColor = aColor; gl_Position = uProj * vec4(aPos, 0.0, 1.0); }
)";

const char* kFragmentSrc = R"(#version 330 core
in vec4 vColor;
out vec4 oColor;
void main() { oColor = vColor; }
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
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                          reinterpret_cast<const void*>(2 * sizeof(float)));
    glBindVertexArray(0);
}

Renderer::~Renderer() {
    glDeleteBuffers(1, &vbo_);
    glDeleteVertexArrays(1, &vao_);
    glDeleteProgram(program_);
}

void Renderer::quad(float x, float y, float w, float h, float r, float g, float b, float a) {
    const Vertex v[6] = {{x, y, r, g, b, a},         {x + w, y, r, g, b, a}, {x + w, y + h, r, g, b, a},
                         {x, y, r, g, b, a},         {x + w, y + h, r, g, b, a}, {x, y + h, r, g, b, a}};
    batch_.insert(batch_.end(), v, v + 6);
}

void Renderer::flush() {
    if (batch_.empty()) return;
    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(batch_.size() * sizeof(Vertex)), batch_.data(),
                 GL_STREAM_DRAW);
    glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(batch_.size()));
    glBindVertexArray(0);
    batch_.clear();
}

void Renderer::draw(const World& world, const RenderSnapshot& prev, float alpha, int windowW, int windowH) {
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
