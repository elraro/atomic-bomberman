#include "free/free_art.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "game/values.hpp"

namespace ab {

const float kFreePlayerColour[10][3] = {
    {0.95f, 0.95f, 0.95f}, {0.30f, 0.30f, 0.34f}, {0.90f, 0.15f, 0.15f}, {0.20f, 0.35f, 0.95f}, {0.15f, 0.80f, 0.20f},
    {0.95f, 0.90f, 0.15f}, {0.15f, 0.85f, 0.85f}, {0.90f, 0.20f, 0.90f}, {0.95f, 0.55f, 0.10f}, {0.55f, 0.20f, 0.90f}};

namespace {

constexpr float kPi = 3.14159265f;

struct Col {
    float r = 0, g = 0, b = 0, a = 1;
};
constexpr Col rgb(int r, int g, int b, float a = 1.0f) {
    return {static_cast<float>(r) / 255.0f, static_cast<float>(g) / 255.0f, static_cast<float>(b) / 255.0f, a};
}
constexpr Col grey(float v, float a = 1.0f) { return {v, v, v, a}; }
Col mix(Col a, Col b, float t) { return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t}; }
Col scaled(Col c, float f) { return {std::min(1.0f, c.r * f), std::min(1.0f, c.g * f), std::min(1.0f, c.b * f), c.a}; }

// Cheap repeatable noise, 0-1.
float noise(int x, int y, int seed) {
    auto h = static_cast<std::uint32_t>(x) * 374761393u + static_cast<std::uint32_t>(y) * 668265263u + static_cast<std::uint32_t>(seed) * 2147483647u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return static_cast<float>((h ^ (h >> 16)) & 0xFFFF) / 65535.0f;
}

// A small software canvas. Shapes are given as distance functions and drawn
// with one pixel of soft edge.
class Canvas {
public:
    Canvas(int w, int h) : w_(w), h_(h), px_(static_cast<std::size_t>(w * h)), tint_(static_cast<std::size_t>(w * h), 0) {
        for (Col& c : px_) c = {0, 0, 0, 0};
    }
    int width() const { return w_; }
    int height() const { return h_; }

    void blend(int x, int y, Col c, float cover, bool tint = false) {
        if (x < 0 || y < 0 || x >= w_ || y >= h_) return;
        const float a = c.a * std::clamp(cover, 0.0f, 1.0f);
        if (a <= 0.0f) return;
        Col& d = px_[static_cast<std::size_t>(y * w_ + x)];
        const float outA = a + d.a * (1.0f - a);
        d.r = (c.r * a + d.r * d.a * (1.0f - a)) / outA;
        d.g = (c.g * a + d.g * d.a * (1.0f - a)) / outA;
        d.b = (c.b * a + d.b * d.a * (1.0f - a)) / outA;
        d.a = outA;
        if (a > 0.5f) tint_[static_cast<std::size_t>(y * w_ + x)] = tint ? 1 : 0;
    }
    template <class Distance>
    void shape(float x0, float y0, float x1, float y1, Distance d, Col c, bool tint = false) {
        for (int y = static_cast<int>(std::floor(y0)) - 1; y <= static_cast<int>(std::ceil(y1)) + 1; ++y)
            for (int x = static_cast<int>(std::floor(x0)) - 1; x <= static_cast<int>(std::ceil(x1)) + 1; ++x)
                blend(x, y, c, 0.5f - d(static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f), tint);
    }
    void ellipse(float cx, float cy, float rx, float ry, Col c, bool tint = false) {
        if (rx <= 0 || ry <= 0) return;
        const float m = std::min(rx, ry);
        shape(cx - rx, cy - ry, cx + rx, cy + ry,
              [=](float x, float y) { return (std::sqrt((x - cx) * (x - cx) / (rx * rx) + (y - cy) * (y - cy) / (ry * ry)) - 1.0f) * m; }, c, tint);
    }
    void circle(float cx, float cy, float r, Col c, bool tint = false) { ellipse(cx, cy, r, r, c, tint); }
    void ring(float cx, float cy, float r, float thickness, Col c) {
        shape(cx - r - thickness, cy - r - thickness, cx + r + thickness, cy + r + thickness,
              [=](float x, float y) { return std::abs(std::sqrt((x - cx) * (x - cx) + (y - cy) * (y - cy)) - r) - thickness / 2.0f; }, c);
    }
    // Box with rounded corners.
    void box(float x0, float y0, float x1, float y1, float radius, Col c, bool tint = false) {
        const float cx = (x0 + x1) / 2, cy = (y0 + y1) / 2, hx = (x1 - x0) / 2 - radius, hy = (y1 - y0) / 2 - radius;
        shape(x0, y0, x1, y1,
              [=](float x, float y) {
                  const float qx = std::abs(x - cx) - hx, qy = std::abs(y - cy) - hy;
                  const float ox = std::max(qx, 0.0f), oy = std::max(qy, 0.0f);
                  return std::sqrt(ox * ox + oy * oy) + std::min(std::max(qx, qy), 0.0f) - radius;
              },
              c, tint);
    }
    void line(float x0, float y0, float x1, float y1, float thickness, Col c, bool tint = false) {
        const float dx = x1 - x0, dy = y1 - y0, len2 = std::max(0.0001f, dx * dx + dy * dy);
        shape(std::min(x0, x1) - thickness, std::min(y0, y1) - thickness, std::max(x0, x1) + thickness, std::max(y0, y1) + thickness,
              [=](float x, float y) {
                  const float t = std::clamp(((x - x0) * dx + (y - y0) * dy) / len2, 0.0f, 1.0f);
                  const float px = x - (x0 + dx * t), py = y - (y0 + dy * t);
                  return std::sqrt(px * px + py * py) - thickness / 2.0f;
              },
              c, tint);
    }
    void triangle(float ax, float ay, float bx, float by, float cx, float cy, Col c) {
        auto edge = [](float px, float py, float x0, float y0, float x1, float y1) {
            const float dx = x1 - x0, dy = y1 - y0;
            return ((px - x0) * dy - (py - y0) * dx) / std::sqrt(dx * dx + dy * dy);
        };
        const float sign = edge(cx, cy, ax, ay, bx, by) < 0 ? 1.0f : -1.0f;
        shape(std::min({ax, bx, cx}), std::min({ay, by, cy}), std::max({ax, bx, cx}), std::max({ay, by, cy}),
              [=](float x, float y) {
                  return std::max({sign * edge(x, y, ax, ay, bx, by), sign * edge(x, y, bx, by, cx, cy), sign * edge(x, y, cx, cy, ax, ay)});
              },
              c);
    }
    void rect(int x0, int y0, int x1, int y1, Col c) {
        for (int y = y0; y < y1; ++y)
            for (int x = x0; x < x1; ++x) blend(x, y, c, 1.0f);
    }
    // Multiplies everything drawn so far (alpha too with fade).
    void fade(float alpha) {
        for (Col& c : px_) c.a *= alpha;
    }
    void whiten(float amount) {
        for (Col& c : px_) c = {c.r + (1 - c.r) * amount, c.g + (1 - c.g) * amount, c.b + (1 - c.b) * amount, c.a};
        if (amount > 0.6f) std::fill(tint_.begin(), tint_.end(), static_cast<std::uint8_t>(0));
    }
    const Col& at(int x, int y) const { return px_[static_cast<std::size_t>(y * w_ + x)]; }
    bool tinted(int x, int y) const { return tint_[static_cast<std::size_t>(y * w_ + x)] != 0; }

    FreeFrame frame(int hotX, int hotY) const {
        FreeFrame f;
        f.width = w_;
        f.height = h_;
        f.hotX = hotX;
        f.hotY = hotY;
        f.rgba.resize(px_.size() * 4);
        f.tint = tint_;
        for (std::size_t i = 0; i < px_.size(); ++i) {
            f.rgba[i * 4] = static_cast<std::uint8_t>(std::clamp(px_[i].r, 0.0f, 1.0f) * 255.0f + 0.5f);
            f.rgba[i * 4 + 1] = static_cast<std::uint8_t>(std::clamp(px_[i].g, 0.0f, 1.0f) * 255.0f + 0.5f);
            f.rgba[i * 4 + 2] = static_cast<std::uint8_t>(std::clamp(px_[i].b, 0.0f, 1.0f) * 255.0f + 0.5f);
            f.rgba[i * 4 + 3] = static_cast<std::uint8_t>(std::clamp(px_[i].a, 0.0f, 1.0f) * 255.0f + 0.5f);
        }
        return f;
    }
    FreePicture picture() const {
        const FreeFrame f = frame(0, 0);
        FreePicture p;
        p.width = w_;
        p.height = h_;
        p.rgba = f.rgba;
        for (std::size_t i = 3; i < p.rgba.size(); i += 4) p.rgba[i] = 255;
        return p;
    }

private:
    int w_, h_;
    std::vector<Col> px_;
    std::vector<std::uint8_t> tint_;
};

// --- lettering ---------------------------------------------------------------
// The classic 5 x 7 dot-matrix character shapes for codes 32-126: five column
// bytes per character, bit 0 at the top.
const std::uint8_t kGlyphs[95][5] = {
    {0x00, 0x00, 0x00, 0x00, 0x00}, {0x00, 0x00, 0x5F, 0x00, 0x00}, {0x00, 0x07, 0x00, 0x07, 0x00}, {0x14, 0x7F, 0x14, 0x7F, 0x14},
    {0x24, 0x2A, 0x7F, 0x2A, 0x12}, {0x23, 0x13, 0x08, 0x64, 0x62}, {0x36, 0x49, 0x55, 0x22, 0x50}, {0x00, 0x05, 0x03, 0x00, 0x00},
    {0x00, 0x1C, 0x22, 0x41, 0x00}, {0x00, 0x41, 0x22, 0x1C, 0x00}, {0x14, 0x08, 0x3E, 0x08, 0x14}, {0x08, 0x08, 0x3E, 0x08, 0x08},
    {0x00, 0x50, 0x30, 0x00, 0x00}, {0x08, 0x08, 0x08, 0x08, 0x08}, {0x00, 0x60, 0x60, 0x00, 0x00}, {0x20, 0x10, 0x08, 0x04, 0x02},
    {0x3E, 0x51, 0x49, 0x45, 0x3E}, {0x00, 0x42, 0x7F, 0x40, 0x00}, {0x42, 0x61, 0x51, 0x49, 0x46}, {0x21, 0x41, 0x45, 0x4B, 0x31},
    {0x18, 0x14, 0x12, 0x7F, 0x10}, {0x27, 0x45, 0x45, 0x45, 0x39}, {0x3C, 0x4A, 0x49, 0x49, 0x30}, {0x01, 0x71, 0x09, 0x05, 0x03},
    {0x36, 0x49, 0x49, 0x49, 0x36}, {0x06, 0x49, 0x49, 0x29, 0x1E}, {0x00, 0x36, 0x36, 0x00, 0x00}, {0x00, 0x56, 0x36, 0x00, 0x00},
    {0x08, 0x14, 0x22, 0x41, 0x00}, {0x14, 0x14, 0x14, 0x14, 0x14}, {0x00, 0x41, 0x22, 0x14, 0x08}, {0x02, 0x01, 0x51, 0x09, 0x06},
    {0x32, 0x49, 0x79, 0x41, 0x3E}, {0x7E, 0x11, 0x11, 0x11, 0x7E}, {0x7F, 0x49, 0x49, 0x49, 0x36}, {0x3E, 0x41, 0x41, 0x41, 0x22},
    {0x7F, 0x41, 0x41, 0x22, 0x1C}, {0x7F, 0x49, 0x49, 0x49, 0x41}, {0x7F, 0x09, 0x09, 0x09, 0x01}, {0x3E, 0x41, 0x49, 0x49, 0x7A},
    {0x7F, 0x08, 0x08, 0x08, 0x7F}, {0x00, 0x41, 0x7F, 0x41, 0x00}, {0x20, 0x40, 0x41, 0x3F, 0x01}, {0x7F, 0x08, 0x14, 0x22, 0x41},
    {0x7F, 0x40, 0x40, 0x40, 0x40}, {0x7F, 0x02, 0x0C, 0x02, 0x7F}, {0x7F, 0x04, 0x08, 0x10, 0x7F}, {0x3E, 0x41, 0x41, 0x41, 0x3E},
    {0x7F, 0x09, 0x09, 0x09, 0x06}, {0x3E, 0x41, 0x51, 0x21, 0x5E}, {0x7F, 0x09, 0x19, 0x29, 0x46}, {0x46, 0x49, 0x49, 0x49, 0x31},
    {0x01, 0x01, 0x7F, 0x01, 0x01}, {0x3F, 0x40, 0x40, 0x40, 0x3F}, {0x1F, 0x20, 0x40, 0x20, 0x1F}, {0x3F, 0x40, 0x38, 0x40, 0x3F},
    {0x63, 0x14, 0x08, 0x14, 0x63}, {0x07, 0x08, 0x70, 0x08, 0x07}, {0x61, 0x51, 0x49, 0x45, 0x43}, {0x00, 0x7F, 0x41, 0x41, 0x00},
    {0x02, 0x04, 0x08, 0x10, 0x20}, {0x00, 0x41, 0x41, 0x7F, 0x00}, {0x04, 0x02, 0x01, 0x02, 0x04}, {0x40, 0x40, 0x40, 0x40, 0x40},
    {0x00, 0x01, 0x02, 0x04, 0x00}, {0x20, 0x54, 0x54, 0x54, 0x78}, {0x7F, 0x48, 0x44, 0x44, 0x38}, {0x38, 0x44, 0x44, 0x44, 0x20},
    {0x38, 0x44, 0x44, 0x48, 0x7F}, {0x38, 0x54, 0x54, 0x54, 0x18}, {0x08, 0x7E, 0x09, 0x01, 0x02}, {0x0C, 0x52, 0x52, 0x52, 0x3E},
    {0x7F, 0x08, 0x04, 0x04, 0x78}, {0x00, 0x44, 0x7D, 0x40, 0x00}, {0x20, 0x40, 0x44, 0x3D, 0x00}, {0x7F, 0x10, 0x28, 0x44, 0x00},
    {0x00, 0x41, 0x7F, 0x40, 0x00}, {0x7C, 0x04, 0x18, 0x04, 0x78}, {0x7C, 0x08, 0x04, 0x04, 0x78}, {0x38, 0x44, 0x44, 0x44, 0x38},
    {0x7C, 0x14, 0x14, 0x14, 0x08}, {0x08, 0x14, 0x14, 0x18, 0x7C}, {0x7C, 0x08, 0x04, 0x04, 0x08}, {0x48, 0x54, 0x54, 0x54, 0x20},
    {0x04, 0x3F, 0x44, 0x40, 0x20}, {0x3C, 0x40, 0x40, 0x20, 0x7C}, {0x1C, 0x20, 0x40, 0x20, 0x1C}, {0x3C, 0x40, 0x30, 0x40, 0x3C},
    {0x44, 0x28, 0x10, 0x28, 0x44}, {0x0C, 0x50, 0x50, 0x50, 0x3C}, {0x44, 0x64, 0x54, 0x4C, 0x44}, {0x00, 0x08, 0x36, 0x41, 0x00},
    {0x00, 0x00, 0x7F, 0x00, 0x00}, {0x00, 0x41, 0x36, 0x08, 0x00}, {0x08, 0x04, 0x08, 0x10, 0x08}};

bool glyphDot(char ch, int col, int row) {
    const auto u = static_cast<unsigned char>(ch);
    if (u < 32 || u > 126 || col < 0 || col > 4 || row < 0 || row > 6) return false;
    return (kGlyphs[u - 32][col] >> row) & 1;
}

// Block lettering for the pictures: each dot a square of `scale` pixels, 6 columns per character.
void drawText(Canvas& c, const std::string& s, float x, float y, int scale, Col col) {
    for (std::size_t i = 0; i < s.size(); ++i)
        for (int cx = 0; cx < 5; ++cx)
            for (int cy = 0; cy < 7; ++cy)
                if (glyphDot(s[i], cx, cy)) {
                    const int px = static_cast<int>(x) + (static_cast<int>(i) * 6 + cx) * scale, py = static_cast<int>(y) + cy * scale;
                    c.rect(px, py, px + scale, py + scale, col);
                }
}
float textWidth(const std::string& s, int scale) { return static_cast<float>(static_cast<int>(s.size()) * 6 * scale - scale); }
void drawTitle(Canvas& c, const std::string& s, float cy, int scale, Col col) {
    const float x = (static_cast<float>(c.width()) - textWidth(s, scale)) / 2.0f;
    drawText(c, s, x + static_cast<float>(scale) / 2.0f + 1.0f, cy + static_cast<float>(scale) / 2.0f + 1.0f, scale, rgb(0, 0, 0, 0.8f));
    drawText(c, s, x, cy, scale, col);
}

// --- the player figure -------------------------------------------------------

struct Pose {
    int dir = 2;            // 0 north, 1 east, 2 south, 3 west
    float walk = -1.0f;     // phase 0-1 of a step, or -1 standing
    bool armsUp = false;    // carrying a bomb overhead
    float kick = 0.0f;      // 0-1: leading foot forward
    float punch = 0.0f;     // 0-1: leading hand forward
    float look = 0.0f;      // -1..1: eyes aside (trapped animation)
    float squash = 0.0f;    // 0-1: flattened
    bool blink = false;
};

// A small robot, this project's own design. Drawn around (cx, feet): about 30 px wide
// and 46 px tall at scale 1.
void drawPlayer(Canvas& c, float cx, float feet, float s, const Pose& p) {
    const Col outline = rgb(16, 16, 24);
    const Col limb = rgb(84, 90, 110);        // hands and feet: steel
    const Col visor = rgb(22, 26, 44);
    const Col eye = rgb(120, 240, 255);
    const float swing = p.walk >= 0 ? std::sin(p.walk * 2.0f * kPi) : 0.0f;
    const float bob = p.walk >= 0 ? std::abs(std::cos(p.walk * 2.0f * kPi)) * 1.5f * s : 0.0f;
    const float fx = static_cast<float>(p.dir == 1 ? 1 : p.dir == 3 ? -1 : 0);  // facing, sideways
    const bool side = p.dir == 1 || p.dir == 3;
    const float flat = 1.0f - 0.5f * p.squash;

    // Feet.
    for (int f = -1; f <= 1; f += 2) {
        float x = cx + static_cast<float>(f) * 6.0f * s, y = feet - 3.0f * s;
        const float lead = static_cast<float>(f) * swing;
        if (side) x = cx + lead * 6.0f * s, y -= std::max(0.0f, lead) * 2.0f * s;
        else y -= std::max(0.0f, lead) * 3.0f * s;
        if (f == 1 && p.kick > 0) {
            x += fx * 10.0f * s * p.kick;
            y += (p.dir == 2 ? 3.0f : p.dir == 0 ? -6.0f : -2.0f) * s * p.kick;
        }
        c.ellipse(x, y, 6.0f * s, 4.0f * s, outline);
        c.ellipse(x, y, 5.0f * s, 3.0f * s, limb);
    }
    // Body.
    const float bodyTop = feet - (22.0f * flat + 2.0f) * s - bob, bodyBottom = feet - 5.0f * s - bob * 0.5f;
    c.box(cx - 9.0f * s, bodyTop, cx + 9.0f * s, bodyBottom, 5.0f * s, outline);
    c.box(cx - 8.0f * s, bodyTop + s, cx + 8.0f * s, bodyBottom - s, 4.0f * s, grey(0.80f), true);
    c.box(cx - 8.0f * s, bodyBottom - 6.0f * s, cx + 8.0f * s, bodyBottom - 4.0f * s, 0.5f * s, grey(0.45f), true);  // belt
    // Head: a rounded box with a bolt on each side (a robot's, not a helmet).
    const float hy = bodyTop - 9.0f * s * flat, hr = 11.0f * s;
    if (!side) {
        c.circle(cx - 12.5f * s, hy + s, 3.5f * s, outline);
        c.circle(cx + 12.5f * s, hy + s, 3.5f * s, outline);
        c.circle(cx - 12.5f * s, hy + s, 2.5f * s, grey(0.55f), true);
        c.circle(cx + 12.5f * s, hy + s, 2.5f * s, grey(0.55f), true);
    }
    c.box(cx - 12.0f * s, hy - (hr + s) * flat, cx + 12.0f * s, hy + (hr + s) * flat, 6.0f * s, outline);
    c.box(cx - 11.0f * s, hy - hr * flat, cx + 11.0f * s, hy + hr * flat, 5.0f * s, grey(1.0f), true);
    c.box(cx - 8.0f * s, hy - (hr - 2.0f * s) * flat, cx + 8.0f * s, hy - (hr - 4.0f * s) * flat, s, grey(0.7f), true);  // seam
    if (side) c.circle(cx - fx * 3.0f * s, hy + s, 3.0f * s, grey(0.55f), true);  // the bolt, seen from the side
    // Visor and eyes.
    if (p.dir != 0) {
        const float px = cx + fx * 4.0f * s, pw = (side ? 6.0f : 8.5f) * s, ph = 4.5f * s * flat;
        c.box(px - pw, hy - ph + 2.0f * s, px + pw, hy + ph + 2.0f * s, 3.0f * s, visor);
        const float ex = p.look * 2.0f * s;
        const float eh = p.blink ? 0.7f * s : 2.6f * s * flat;
        if (side) {
            c.ellipse(px + fx * 2.5f * s + ex, hy + 2.0f * s, 1.8f * s, eh, eye);
        } else {
            c.ellipse(px - 3.5f * s + ex, hy + 2.0f * s, 1.8f * s, eh, eye);
            c.ellipse(px + 3.5f * s + ex, hy + 2.0f * s, 1.8f * s, eh, eye);
        }
    } else {
        for (int v = 0; v < 3; ++v)  // vents on the back of the head
            c.box(cx - 5.0f * s, hy - 2.0f * s + 3.0f * s * static_cast<float>(v), cx + 5.0f * s, hy - 0.8f * s + 3.0f * s * static_cast<float>(v), 0.5f * s, grey(0.55f), true);
    }
    // Hands.
    for (int h = -1; h <= 1; h += 2) {
        float x = cx + static_cast<float>(h) * 11.0f * s, y = bodyTop + 9.0f * s - static_cast<float>(h) * swing * 2.0f * s;
        if (side) x = cx - static_cast<float>(h) * swing * 7.0f * s + fx * s;
        if (p.armsUp) x = cx + static_cast<float>(h) * 9.0f * s, y = hy - (hr + 3.0f * s) * flat;
        if (h == 1 && p.punch > 0) {
            x = cx + fx * (6.0f + 12.0f * p.punch) * s + (side ? 0.0f : 9.0f * s);
            y = bodyTop + 6.0f * s + (p.dir == 2 ? 8.0f : p.dir == 0 ? -10.0f : 0.0f) * s * p.punch;
        }
        c.circle(x, y, 4.5f * s, outline);
        c.circle(x, y, 3.5f * s, limb);
    }
}

FreeFrame playerFrame(const Pose& p) {
    Canvas c(48, 64);
    drawPlayer(c, 24, 60, 1.0f, p);
    return c.frame(24, 60);
}

void drawBomb(Canvas& c, float cx, float cy, float r, int kind, float spark) {
    // kind 0 regular, 1 trigger, 2 jelly, 3 dud
    const Col outline = rgb(8, 8, 14);
    const Col body = kind == 2 ? rgb(40, 110, 60) : kind == 3 ? rgb(90, 90, 96) : rgb(34, 36, 48);
    c.circle(cx, cy, r + 1.0f, outline);
    c.circle(cx, cy, r, body);
    c.box(cx - r * 0.95f, cy - r * 0.12f, cx + r * 0.95f, cy + r * 0.22f, 1.0f, grey(0.9f, 0.85f), true);  // the owner's band
    c.circle(cx - r * 0.4f, cy - r * 0.45f, r * 0.28f, grey(1.0f, 0.55f));
    c.box(cx - 3.0f, cy - r - 4.0f, cx + 3.0f, cy - r + 1.0f, 1.0f, outline);
    c.box(cx - 2.0f, cy - r - 3.0f, cx + 2.0f, cy - r, 0.5f, rgb(150, 150, 160));
    if (kind == 1) {
        c.circle(cx, cy - r - 5.0f, 3.5f, outline);
        c.circle(cx, cy - r - 5.0f, 2.5f, spark > 0.5f ? rgb(255, 60, 40) : rgb(120, 20, 20));
    } else if (kind == 3) {
        c.circle(cx + 3.0f, cy - r - 7.0f - spark * 4.0f, 2.5f + spark * 2.0f, grey(0.8f, 0.5f - spark * 0.4f));
    } else {
        c.line(cx, cy - r - 3.0f, cx + 4.0f, cy - r - 8.0f, 1.5f, rgb(170, 130, 80));
        c.circle(cx + 4.0f, cy - r - 9.0f, 2.0f + spark * 2.0f, rgb(255, 200, 60));
        c.circle(cx + 4.0f, cy - r - 9.0f, 1.0f + spark, rgb(255, 255, 220));
    }
}

// --- level themes ------------------------------------------------------------

struct Theme {
    Col ground, ground2, solid, brick, frame;
    int brickStyle;  // 0 bricks, 1 crate, 2 block
};

const Theme kTheme[11] = {
    {rgb(58, 150, 62), rgb(50, 136, 56), rgb(140, 144, 152), rgb(176, 96, 52), rgb(60, 70, 60), 0},      // green acres
    {rgb(92, 176, 72), rgb(82, 162, 66), rgb(98, 102, 112), rgb(196, 70, 56), rgb(52, 60, 52), 0},       // classic green
    {rgb(206, 232, 244), rgb(190, 220, 238), rgb(46, 76, 140), rgb(236, 244, 250), rgb(40, 60, 100), 2}, // hockey rink
    {rgb(222, 190, 120), rgb(210, 176, 108), rgb(150, 110, 60), rgb(232, 206, 150), rgb(110, 80, 40), 2}, // egypt
    {rgb(92, 78, 68), rgb(82, 70, 62), rgb(44, 44, 50), rgb(160, 116, 64), rgb(36, 32, 30), 1},          // coal mine
    {rgb(236, 214, 150), rgb(226, 202, 138), rgb(130, 130, 124), rgb(70, 150, 210), rgb(60, 110, 150), 1}, // beach
    {rgb(108, 72, 150), rgb(96, 62, 138), rgb(70, 160, 130), rgb(210, 80, 170), rgb(40, 28, 70), 2},     // aliens
    {rgb(84, 80, 100), rgb(74, 70, 90), rgb(150, 150, 160), rgb(92, 62, 48), rgb(36, 32, 46), 1},        // haunted house
    {rgb(40, 140, 150), rgb(34, 126, 138), rgb(230, 120, 130), rgb(90, 210, 200), rgb(20, 70, 90), 2},   // under the ocean
    {rgb(38, 104, 54), rgb(32, 92, 48), rgb(122, 84, 50), rgb(70, 150, 60), rgb(24, 52, 30), 0},         // deep forest
    {rgb(112, 114, 120), rgb(102, 104, 110), rgb(176, 176, 170), rgb(84, 110, 130), rgb(50, 50, 56), 1}, // inner city
};

void drawSolid(Canvas& c, const Theme& t, int level) {
    c.rect(0, 0, 40, 36, scaled(t.solid, 0.45f));
    c.rect(1, 1, 39, 35, t.solid);
    c.rect(1, 1, 39, 4, scaled(t.solid, 1.3f));
    c.rect(1, 1, 4, 35, scaled(t.solid, 1.18f));
    c.rect(1, 32, 39, 35, scaled(t.solid, 0.7f));
    c.rect(36, 1, 39, 35, scaled(t.solid, 0.8f));
    for (int y = 4; y < 32; ++y)
        for (int x = 4; x < 36; ++x)
            if (noise(x, y, 31 + level) > 0.86f) c.blend(x, y, scaled(t.solid, 0.8f), 1.0f);
    c.circle(8, 8, 1.5f, scaled(t.solid, 0.6f));
    c.circle(32, 8, 1.5f, scaled(t.solid, 0.6f));
    c.circle(8, 28, 1.5f, scaled(t.solid, 0.6f));
    c.circle(32, 28, 1.5f, scaled(t.solid, 0.6f));
}

void drawBrick(Canvas& c, const Theme& t, int level) {
    const Col dark = scaled(t.brick, 0.55f), light = scaled(t.brick, 1.22f);
    c.rect(0, 0, 40, 36, scaled(t.brick, 0.35f));
    c.rect(1, 1, 39, 35, t.brick);
    if (t.brickStyle == 0) {
        for (int row = 0; row < 4; ++row) {
            const int y = 1 + row * 9;
            c.rect(1, y + 8, 39, y + 9, dark);
            for (int x = (row & 1) != 0 ? 10 : 20; x < 39; x += 20) c.rect(x, y, x + 1, y + 8, dark);
            c.rect(1, y, 39, y + 1, light);
        }
    } else if (t.brickStyle == 1) {
        c.rect(1, 1, 39, 5, light);
        c.rect(1, 31, 39, 35, dark);
        c.rect(1, 1, 5, 35, light);
        c.rect(35, 1, 39, 35, dark);
        c.line(6, 6, 34, 30, 3.0f, dark);
        c.line(34, 6, 6, 30, 3.0f, dark);
    } else {
        c.rect(1, 1, 39, 3, light);
        c.rect(1, 1, 3, 35, light);
        c.rect(1, 33, 39, 35, dark);
        c.rect(37, 1, 39, 35, dark);
        c.box(9, 8, 31, 28, 3.0f, dark);
        c.box(10, 9, 30, 27, 2.5f, scaled(t.brick, 0.9f));
    }
    for (int y = 2; y < 34; ++y)
        for (int x = 2; x < 38; ++x)
            if (noise(x, y, 77 + level) > 0.9f) c.blend(x, y, dark, 0.6f);
}

void drawFlamePiece(Canvas& c, int piece, int dir, float size) {
    // piece 0 centre, 1 arm, 2 tip; the frame is one cell, 40 x 36.
    const float w = 15.0f * size;                // half width of an arm
    auto layer = [&](float k, Col col, bool tint) {
        const float hw = w * k;
        if (piece == 0) {
            c.box(20 - hw, -2, 20 + hw, 38, hw * 0.3f, col, tint);
            c.box(-2, 18 - hw, 42, 18 + hw, hw * 0.3f, col, tint);
            c.circle(20, 18, hw * 1.25f, col, tint);
            return;
        }
        const bool horizontal = dir == 1 || dir == 3;
        float x0 = horizontal ? -2.0f : 20 - hw, x1 = horizontal ? 42.0f : 20 + hw;
        float y0 = horizontal ? 18 - hw : -2.0f, y1 = horizontal ? 18 + hw : 38.0f;
        if (piece == 2) {  // rounded end, a third of a cell short
            if (dir == 0) y0 = 10;
            if (dir == 1) x1 = 30;
            if (dir == 2) y1 = 26;
            if (dir == 3) x0 = 10;
        }
        c.box(x0, y0, x1, y1, piece == 2 ? hw * 0.95f : 0.0f, col, tint);
    };
    layer(1.0f, grey(0.95f, 0.9f), true);          // the owner's colour outside
    layer(0.68f, rgb(255, 210, 70), false);
    layer(0.36f, rgb(255, 255, 230), false);
}

void drawPowerPanel(Canvas& c, Col back) {
    c.box(3, 3, 37, 35, 5.0f, rgb(20, 20, 30));
    c.box(4, 4, 36, 34, 4.5f, back);
    c.box(6, 6, 34, 12, 3.0f, rgb(255, 255, 255, 0.28f));
}

void drawPower(Canvas& c, int type, float shine) {
    static const Col kBack[14] = {rgb(60, 90, 200),  rgb(210, 90, 40),  rgb(70, 150, 60),  rgb(60, 160, 190), rgb(200, 170, 50),
                                  rgb(190, 70, 70),  rgb(120, 90, 190), rgb(80, 80, 110),  rgb(230, 180, 40), rgb(170, 50, 50),
                                  rgb(50, 170, 120), rgb(60, 90, 50),   rgb(140, 60, 170), rgb(110, 110, 110)};
    drawPowerPanel(c, scaled(kBack[type], 1.0f + 0.15f * shine));
    const Col ink = rgb(16, 16, 24), white = rgb(250, 250, 250);
    switch (type) {
        case 0: drawBomb(c, 20, 22, 8, 0, shine); break;
        case 1:
        case 8: {
            const Col outer = type == 8 ? rgb(255, 215, 60) : rgb(255, 120, 40), inner = type == 8 ? rgb(255, 250, 200) : rgb(255, 230, 90);
            c.triangle(20, 6, 11, 22, 29, 22, outer);
            c.circle(20, 23, 9, outer);
            c.triangle(20, 14, 15, 24, 25, 24, inner);
            c.circle(20, 25, 5, inner);
            break;
        }
        case 2:
        case 11: {
            const Col bone = type == 11 ? rgb(200, 230, 120) : white;
            c.circle(20, 18, 9, ink);
            c.circle(20, 18, 8, bone);
            c.box(14, 22, 26, 31, 2, ink);
            c.box(15, 22, 25, 30, 1.5f, bone);
            c.circle(16.5f, 18, 2.5f, ink);
            c.circle(23.5f, 18, 2.5f, ink);
            c.rect(18, 26, 19, 30, ink);
            c.rect(21, 26, 22, 30, ink);
            break;
        }
        case 3:  // kicker: a boot
            c.box(13, 9, 22, 27, 2, ink);
            c.box(13, 21, 31, 30, 3, ink);
            c.box(14, 10, 21, 26, 1.5f, white);
            c.box(14, 22, 30, 29, 2.5f, white);
            c.rect(14, 28, 30, 29, rgb(120, 120, 130));
            break;
        case 4:  // skate
            c.box(11, 12, 29, 24, 3, ink);
            c.box(12, 13, 28, 23, 2.5f, white);
            c.circle(15, 28, 3.5f, ink);
            c.circle(25, 28, 3.5f, ink);
            c.circle(15, 28, 2.0f, rgb(255, 200, 60));
            c.circle(25, 28, 2.0f, rgb(255, 200, 60));
            break;
        case 5:  // punch: a glove
            c.box(11, 12, 29, 27, 6, ink);
            c.box(12, 13, 28, 26, 5.5f, rgb(240, 60, 60));
            c.box(15, 26, 25, 32, 1, ink);
            c.box(16, 26, 24, 31, 0.5f, white);
            c.line(16, 16, 16, 22, 1.2f, rgb(160, 30, 30));
            c.line(20, 16, 20, 22, 1.2f, rgb(160, 30, 30));
            c.line(24, 16, 24, 22, 1.2f, rgb(160, 30, 30));
            break;
        case 6:  // grab: an open hand
            c.circle(20, 23, 7, ink);
            c.circle(20, 23, 6, rgb(90, 170, 240));
            for (int f = 0; f < 4; ++f) {
                const float x = 13.0f + 4.6f * static_cast<float>(f);
                c.line(x, 20, x, 10, 4.0f, ink);
                c.line(x, 20, x, 10, 2.6f, rgb(90, 170, 240));
            }
            break;
        case 7:  // spooge: a row of bombs
            drawBomb(c, 12, 25, 4.5f, 0, 0);
            drawBomb(c, 20, 23, 4.5f, 0, 0);
            drawBomb(c, 28, 25, 4.5f, 0, 0);
            break;
        case 9: drawBomb(c, 20, 23, 8, 1, shine); break;
        case 10:
            c.ellipse(20, 23, 10, 7.5f + shine, ink);
            c.ellipse(20, 23, 9, 6.5f + shine, rgb(120, 240, 150));
            c.circle(16, 20, 2, rgb(255, 255, 255, 0.7f));
            break;
        case 12: drawText(c, "?", 14, 9, 3, ink), drawText(c, "?", 13, 8, 3, white); break;
        default:  // clog: a weight
            c.box(11, 16, 29, 30, 2, ink);
            c.box(12, 17, 28, 29, 1.5f, rgb(170, 170, 180));
            c.ring(20, 13, 4, 2.5f, ink);
            c.rect(15, 21, 25, 24, rgb(90, 90, 100));
            break;
    }
}

struct Builder {
    FreeArt art;
    int add(FreeFrame f) {
        art.frames.push_back(std::move(f));
        return static_cast<int>(art.frames.size()) - 1;
    }
    void put(const std::string& name, FreeFrame f) { art.sequences[name].push_back(add(std::move(f))); }
};

const char* const kDir[4] = {"north", "east", "south", "west"};

void buildPlayers(Builder& b, int deaths, int cornerheads) {
    for (int d = 0; d < 4; ++d) {
        const std::string dir = kDir[d];
        Pose stand;
        stand.dir = d;
        b.put("stand " + dir, playerFrame(stand));
        for (int i = 0; i < 8; ++i) {
            Pose p = stand;
            p.walk = static_cast<float>(i) / 8.0f;
            b.put("walk " + dir, playerFrame(p));
            p.armsUp = true;
            b.put("walkbomb " + dir, playerFrame(p));
        }
        Pose carry = stand;
        carry.armsUp = true;
        b.put("standbomb " + dir, playerFrame(carry));
        for (int i = 0; i < 8; ++i) {
            Pose p = stand;
            p.kick = std::sin(static_cast<float>(i) / 7.0f * kPi);
            b.put("kick " + dir, playerFrame(p));
        }
        for (int i = 0; i < 10; ++i) {
            Pose p = stand;
            p.punch = std::sin(static_cast<float>(i) / 9.0f * kPi);
            b.put("punch " + dir, playerFrame(p));
        }
        for (int i = 0; i < 10; ++i) {
            Pose p = stand;
            p.squash = 0.5f * std::sin(static_cast<float>(i) / 9.0f * kPi);
            p.armsUp = i > 4;
            b.put("pickup " + dir, playerFrame(p));
        }
    }
    // Spinning into a warp hole: the four views in turn.
    for (int i = 0; i < 8; ++i) {
        Pose p;
        p.dir = (2 + i) & 3;
        b.put("spin", playerFrame(p));
    }
    // Trapped: looking about, each variant in its own rhythm.
    for (int n = 0; n < cornerheads; ++n)
        for (int i = 0; i < 50; ++i) {
            Pose p;
            const float t = static_cast<float>(i) / 50.0f;
            p.look = std::sin(t * 2.0f * kPi * static_cast<float>(1 + n % 3));
            p.blink = (i + n * 7) % 17 < 2;
            p.squash = (n & 1) != 0 ? 0.25f * std::abs(std::sin(t * kPi * static_cast<float>(2 + n % 4))) : 0.0f;
            p.armsUp = n % 4 == 3 && i % 10 < 5;
            b.put("cornerhead " + std::to_string(n), playerFrame(p));
        }
    // Deaths: the figure flashes, turns and shrinks while sparks fly; each variant differs
    // in speed, spark count and whether it sinks or pops. Number 9 is a rising ghost.
    for (int k = 1; k <= deaths; ++k) {
        const std::string name = "die green " + std::to_string(k);
        if (k == 9) {
            for (int i = 0; i < 8; ++i) {
                Canvas c(48, 64);
                Pose p;
                drawPlayer(c, 24.0f + 2.0f * std::sin(static_cast<float>(i) / 8.0f * 2.0f * kPi), 58, 0.9f, p);
                c.whiten(0.55f);
                c.fade(0.7f);
                c.ring(24, 9, 6, 2, rgb(255, 230, 90));
                b.put(name, c.frame(24, 60));
            }
            continue;
        }
        const int frames = 14 + (k * 5) % 12;
        const int sparks = 5 + k % 6;
        for (int i = 0; i < frames; ++i) {
            const float t = static_cast<float>(i) / static_cast<float>(frames - 1);
            Canvas c(72, 80);
            Pose p;
            p.dir = (2 + (i * (1 + k % 3)) / 2) & 3;
            p.squash = (k & 1) != 0 ? t : 0.0f;
            const float size = (k % 4 == 0) ? 1.0f + 0.25f * std::sin(t * kPi) - t * 0.9f : 1.0f - t * 0.75f;
            if (size > 0.12f) drawPlayer(c, 36, 68, std::max(0.12f, size), p);
            c.whiten((i % 4 < 2 ? 0.75f : 0.15f) * (1.0f - t * 0.5f));
            if (t > 0.6f) c.fade(1.0f - (t - 0.6f) / 0.4f * 0.8f);
            for (int s = 0; s < sparks; ++s) {
                const float angle = 2.0f * kPi * static_cast<float>(s) / static_cast<float>(sparks) + static_cast<float>(k);
                const float reach = 6.0f + 26.0f * t;
                c.circle(36 + std::cos(angle) * reach, 46 + std::sin(angle) * reach * 0.8f, 3.0f * (1.0f - t) + 0.5f,
                         s % 2 == 0 ? rgb(255, 230, 90, 1.0f - t * 0.6f) : rgb(255, 255, 255, 1.0f - t * 0.6f));
            }
            b.put(name, c.frame(36, 68));
        }
    }
    Canvas shadow(40, 14);
    shadow.ellipse(20, 7, 14, 5, rgb(0, 0, 0, 0.32f));
    b.put("shadow", shadow.frame(20, 9));
}

void buildObjects(Builder& b, int flameFrames) {
    static const char* const kBombName[4] = {"bomb regular green", "bomb trigger green", "bomb jelly green", "bomb regular green dud"};
    for (int kind = 0; kind < 4; ++kind)
        for (int i = 0; i < 12; ++i) {
            const float t = static_cast<float>(i) / 12.0f, pulse = std::sin(t * 2.0f * kPi);
            Canvas c(44, 48);
            const float r = 12.0f + (kind == 2 ? 0.0f : pulse);
            if (kind == 2) {
                c.ellipse(22, 31, 13 + pulse * 1.5f, 13 - pulse * 1.5f, rgb(8, 8, 14));
                c.ellipse(22, 31, 12 + pulse * 1.5f, 12 - pulse * 1.5f, rgb(60, 190, 110));
                c.box(22 - 11, 29, 22 + 11, 33, 1, grey(0.9f, 0.85f), true);
                c.circle(17, 26, 3, rgb(255, 255, 255, 0.6f));
                c.line(22, 19, 26, 13, 1.5f, rgb(170, 130, 80));
                c.circle(26, 12, 2.0f + (pulse + 1), rgb(255, 200, 60));
            } else {
                drawBomb(c, 22, 31, r, kind, (pulse + 1.0f) / 2.0f);
            }
            b.put(kBombName[kind], c.frame(22, 43));
        }
    // Flames live for `flameFrames` steps: they open quickly and thin out at the end.
    const int n = std::max(4, flameFrames + 1);
    for (int i = 0; i < n; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(n - 1);
        const float size = t < 0.2f ? 0.6f + 2.0f * t : t > 0.6f ? 1.0f - (t - 0.6f) * 1.9f : 1.0f - 0.08f * static_cast<float>(i & 1);
        auto piece = [&](const std::string& name, int kind, int dir) {
            Canvas c(40, 36);
            drawFlamePiece(c, kind, dir, std::max(0.18f, size));
            b.put("flame " + name + " green", c.frame(20, 18));
        };
        piece("center", 0, 0);
        for (int d = 0; d < 4; ++d) {
            piece(std::string("mid") + kDir[d], 1, d);
            piece(std::string("tip") + kDir[d], 2, d);
        }
    }
    static const char* const kPower[14] = {"bomb", "flame", "disease", "kicker", "skate", "punch", "grab",
                                           "spooge", "goldflame", "trigger", "jelly", "disease3", "random", "clog"};
    for (int type = 0; type < 14; ++type)
        for (int i = 0; i < 16; ++i) {
            Canvas c(40, 38);
            drawPower(c, type, i < 8 ? static_cast<float>(i) / 8.0f : static_cast<float>(16 - i) / 8.0f);
            b.put(std::string("power ") + kPower[type], c.frame(20, 37));
        }
    // Level extras, one cell each.
    for (int d = 0; d < 4; ++d) {
        Canvas arrow(40, 36);
        arrow.box(4, 3, 36, 33, 4, rgb(30, 30, 40, 0.75f));
        const float ax = static_cast<float>(kDir[d][0] == 'e' ? 1 : kDir[d][0] == 'w' ? -1 : 0), ay = static_cast<float>(d == 0 ? -1 : d == 2 ? 1 : 0);
        arrow.triangle(20 + ax * 11, 18 + ay * 11, 20 - ax * 6 - ay * 10, 18 - ay * 6 - ax * 10, 20 - ax * 6 + ay * 10, 18 - ay * 6 + ax * 10, rgb(255, 220, 60));
        b.put(std::string("extra arrow ") + kDir[d], arrow.frame(20, 35));
        for (int i = 0; i < 4; ++i) {
            Canvas belt(40, 36);
            belt.rect(0, 0, 40, 36, rgb(60, 62, 70));
            for (int s = -1; s < 6; ++s) {
                const float o = static_cast<float>(s) * 10.0f + static_cast<float>(i) * 2.5f * (d == 1 || d == 2 ? 1.0f : -1.0f);
                if (d == 1 || d == 3) belt.rect(static_cast<int>(o), 3, static_cast<int>(o) + 4, 33, rgb(120, 126, 140));
                else belt.rect(3, static_cast<int>(o), 37, static_cast<int>(o) + 4, rgb(120, 126, 140));
            }
            belt.triangle(20 + ax * 8, 18 + ay * 8, 20 - ax * 4 - ay * 6, 18 - ay * 4 - ax * 6, 20 - ax * 4 + ay * 6, 18 - ay * 4 + ax * 6, rgb(255, 220, 60, 0.85f));
            b.put(std::string("extra conveyor ") + kDir[d], belt.frame(20, 35));
        }
    }
    for (int i = 0; i < 8; ++i) {
        Canvas warp(40, 36);
        warp.ellipse(20, 18, 17, 14, rgb(20, 10, 40));
        for (int arm = 0; arm < 3; ++arm)
            for (int s = 0; s < 14; ++s) {
                const float u = static_cast<float>(s) / 14.0f;
                const float angle = 2.0f * kPi * (static_cast<float>(arm) / 3.0f + u * 0.9f - static_cast<float>(i) / 8.0f);
                warp.circle(20 + std::cos(angle) * 15 * u, 18 + std::sin(angle) * 12 * u, 1.2f + 1.5f * u, rgb(150, 90, 255, 0.4f + 0.6f * u));
            }
        b.put("extra warp 1", warp.frame(20, 35));
    }
    for (int i = 0; i <= 12; ++i) {
        Canvas pad(40, 36);
        const float press = i == 0 ? 0.0f : std::sin(static_cast<float>(i) / 12.0f * kPi);
        pad.box(5, 24, 35, 33, 2, rgb(40, 40, 50));
        for (int s = 0; s < 3; ++s) pad.line(9 + 11.0f * static_cast<float>(s), 30, 9 + 11.0f * static_cast<float>(s), 18 + 6 * press, 2.0f, rgb(180, 180, 190));
        pad.box(4, 12 + 6 * press, 36, 20 + 6 * press, 3, rgb(20, 20, 30));
        pad.box(5, 13 + 6 * press, 35, 19 + 6 * press, 2.5f, rgb(240, 70, 70));
        b.put("extra trampoline", pad.frame(20, 35));
    }
    // Campaign enemies.
    for (int d = 0; d < 4; ++d)
        for (int i = 0; i < 8; ++i) {
            const float t = static_cast<float>(i) / 8.0f;
            const float ex = static_cast<float>(d == 1 ? 3 : d == 3 ? -3 : 0);
            Canvas ghost(44, 52);
            const float lift = 2.0f * std::sin(t * 2.0f * kPi);
            ghost.ellipse(22, 22 + lift, 14, 15, rgb(240, 240, 255, 0.85f));
            ghost.rect(8, static_cast<int>(22 + lift), 36, static_cast<int>(40 + lift), rgb(240, 240, 255, 0.85f));
            for (int s = 0; s < 4; ++s) ghost.circle(11.5f + 7.0f * static_cast<float>(s), 40 + lift, 3.5f, rgb(240, 240, 255, 0.85f));
            if (d != 0) {
                ghost.circle(17 + ex, 20 + lift, 3, rgb(20, 20, 40));
                ghost.circle(27 + ex, 20 + lift, 3, rgb(20, 20, 40));
            }
            b.put(std::string("ghost ") + kDir[d], ghost.frame(22, 48));
            Canvas rover(44, 44);
            rover.box(6, 12, 38, 32, 5, rgb(16, 16, 24));
            rover.box(7, 13, 37, 31, 4.5f, rgb(90, 200, 120));
            rover.box(12, 6, 32, 16, 5, rgb(16, 16, 24));
            rover.box(13, 7, 31, 15, 4.5f, rgb(160, 230, 255));
            for (int w = 0; w < 3; ++w) {
                const float wx = 11.0f + 11.0f * static_cast<float>(w);
                rover.circle(wx, 34, 5, rgb(16, 16, 24));
                rover.line(wx, 34, wx + 3.5f * std::cos(t * 2.0f * kPi), 34 + 3.5f * std::sin(t * 2.0f * kPi), 1.5f, rgb(200, 200, 210));
            }
            if (d != 0) rover.circle(22 + ex * 2, 11, 2.5f, rgb(255, 60, 60));
            b.put(std::string("rover ") + kDir[d], rover.frame(22, 40));
        }
    for (int i = 0; i < 13; ++i) {
        Canvas star(16, 16);
        const float size = 6.5f * std::sin(static_cast<float>(i + 1) / 14.0f * kPi);
        star.line(8 - size, 8, 8 + size, 8, 1.6f, rgb(255, 235, 120));
        star.line(8, 8 - size, 8, 8 + size, 1.6f, rgb(255, 235, 120));
        star.circle(8, 8, size * 0.35f, rgb(255, 255, 255));
        b.put("goldman", star.frame(8, 8));
    }
    // HUD and menu pieces.
    Canvas cross(64, 16);
    cross.line(2, 2, 58, 13, 2.5f, rgb(230, 40, 40));
    cross.line(2, 13, 58, 2, 2.5f, rgb(230, 40, 40));
    b.put("xxx", cross.frame(0, 0));
    for (int g = 0; g < 11; ++g) {
        Canvas digit(22, 30);
        const std::string s(1, g < 10 ? static_cast<char>('0' + g) : ':');
        drawText(digit, s, 2, 2, 4, rgb(60, 30, 0));
        drawText(digit, s, 0, 0, 4, rgb(255, 200, 60));
        b.put("numeric font", digit.frame(0, 30));
    }
    Canvas hurry(260, 60);
    hurry.box(2, 2, 258, 58, 10, rgb(20, 10, 10, 0.8f));
    drawText(hurry, "HURRY!", 26, 10, 6, rgb(80, 0, 0));
    drawText(hurry, "HURRY!", 22, 8, 6, rgb(255, 70, 50));
    b.put("hurry", hurry.frame(130, 30));
    for (int i = 0; i < 8; ++i) {
        Canvas cursor(32, 32);
        drawBomb(cursor, 16, 19, 8.0f + 0.8f * std::sin(static_cast<float>(i) / 8.0f * 2.0f * kPi), 0, static_cast<float>(i & 1));
        b.put("cursor1", cursor.frame(16, 26));
    }
    Canvas ringPic(64, 64);
    ringPic.ring(32, 32, 26, 5, rgb(20, 20, 30));
    ringPic.ring(32, 32, 26, 3, rgb(255, 215, 70));
    b.put("ring", ringPic.frame(32, 46));
}

void buildTiles(Builder& b, int brickBurnFrames) {
    for (int level = 0; level < 11; ++level) {
        const Theme& t = kTheme[level];
        const std::string lv = std::to_string(level);
        Canvas solid(40, 36);
        drawSolid(solid, t, level);
        b.put("tile " + lv + " solid", solid.frame(20, 35));
        Canvas brick(40, 36);
        drawBrick(brick, t, level);
        b.put("tile " + lv + " brick", brick.frame(20, 35));
        // A burning brick glows, breaks into pieces and is gone.
        const int n = std::max(4, brickBurnFrames);
        for (int i = 0; i < n; ++i) {
            const float u = static_cast<float>(i) / static_cast<float>(n - 1);
            Canvas burn(40, 36);
            drawBrick(burn, t, level);
            burn.whiten(0.6f * (1.0f - u));
            Canvas out(40, 36);
            for (int y = 0; y < 36; ++y)
                for (int x = 0; x < 40; ++x) {
                    if (noise(x / 4, y / 4, 500 + level) < u * 1.1f - 0.05f) continue;  // pieces drop out
                    const Col c = mix(burn.at(x, y), rgb(255, 150, 40), 0.55f * u + 0.2f);
                    out.blend(x, y, {c.r, c.g, c.b, 1.0f}, 1.0f);
                }
            b.put("flame brick " + lv, out.frame(20, 35));
        }
    }
}

// --- full-screen pictures ----------------------------------------------------

Canvas backdrop(Col top, Col bottom, int seed) {
    Canvas c(640, 480);
    for (int y = 0; y < 480; ++y) {
        const Col row = mix(top, bottom, static_cast<float>(y) / 479.0f);
        for (int x = 0; x < 640; ++x) c.blend(x, y, scaled(row, 0.94f + 0.12f * noise(x, y, seed)), 1.0f);
    }
    return c;
}

void scatterStars(Canvas& c, int count, int seed) {
    for (int i = 0; i < count; ++i) {
        const float x = noise(i, 1, seed) * 640.0f, y = noise(i, 2, seed) * 480.0f, r = 0.6f + noise(i, 3, seed) * 1.6f;
        c.circle(x, y, r, rgb(255, 255, 230, 0.4f + 0.6f * noise(i, 4, seed)));
    }
}

void scatterBombs(Canvas& c, int seed, float alpha) {
    for (int gy = 0; gy < 6; ++gy)
        for (int gx = 0; gx < 8; ++gx) {
            Canvas one(60, 64);
            drawBomb(one, 30, 38, 16, (gx + gy + seed) % 5 == 0 ? 1 : 0, 0.5f);
            const int ox = gx * 84 + ((gy & 1) != 0 ? 42 : 0) - 10, oy = gy * 84 - 6;
            for (int y = 0; y < 64; ++y)
                for (int x = 0; x < 60; ++x) {
                    Col p = one.at(x, y);
                    if (one.tinted(x, y)) p = {p.r * 0.9f, p.g * 0.3f, p.b * 0.2f, p.a};
                    p.a *= alpha;
                    c.blend(ox + x, oy + y, p, 1.0f);
                }
        }
}

// The player figure, large, in a player's colour.
void bigPlayer(Canvas& c, float cx, float feet, float scale, int colour, const Pose& pose) {
    Canvas fig(c.width(), c.height());
    drawPlayer(fig, cx, feet, scale, pose);
    const float* pc = kFreePlayerColour[std::clamp(colour, 0, 9)];
    for (int y = 0; y < c.height(); ++y)
        for (int x = 0; x < c.width(); ++x) {
            Col p = fig.at(x, y);
            if (p.a <= 0) continue;
            if (fig.tinted(x, y)) p = {p.r * pc[0], p.g * pc[1], p.b * pc[2], p.a};
            c.blend(x, y, p, 1.0f);
        }
}

Canvas fieldPicture(int level) {
    const Theme& t = kTheme[level];
    Canvas c(640, 480);
    c.rect(0, 0, 640, 480, t.frame);
    for (int y = 0; y < 480; ++y)
        for (int x = 0; x < 640; ++x)
            if (noise(x / 2, y / 2, 900 + level) > 0.7f) c.blend(x, y, scaled(t.frame, 1.25f), 0.5f);
    c.rect(0, 0, 640, 48, rgb(8, 8, 12));           // the score bar
    c.rect(0, 48, 640, 50, scaled(t.frame, 1.6f));
    c.rect(16, 64, 624, 468, scaled(t.frame, 0.5f));
    for (int cy = 0; cy < 11; ++cy)
        for (int cx = 0; cx < 15; ++cx) {
            const Col base = ((cx + cy) & 1) != 0 ? t.ground2 : t.ground;
            for (int y = 0; y < 36; ++y)
                for (int x = 0; x < 40; ++x) {
                    const int px = 20 + cx * 40 + x, py = 68 + cy * 36 + y;
                    c.blend(px, py, scaled(base, 0.93f + 0.14f * noise(px, py, level)), 1.0f);
                }
        }
    return c;
}

}  // namespace

FreeArt makeFreeArt() {
    const Values values = Values::defaults();
    auto value = [&](int id, int fallback) { return values.has(id) ? values.get(id) : fallback; };
    Builder b;
    buildPlayers(b, std::clamp(value(vid::kDeathAnimations, 24), 1, 24), std::clamp(value(vid::kCornerheadCount, 8), 1, 16));
    buildObjects(b, std::clamp(value(vid::kFlameFrames, 10), 4, 40));
    buildTiles(b, std::clamp(value(vid::kBrickBurnFrames, 10), 4, 40));
    return std::move(b.art);
}

std::vector<std::uint8_t> colouredFrame(const FreeFrame& frame, int colour) {
    std::vector<std::uint8_t> out = frame.rgba;
    const float* c = kFreePlayerColour[colour >= 0 && colour < 10 ? colour : 0];
    for (std::size_t i = 0; i < frame.tint.size(); ++i) {
        if (frame.tint[i] == 0) continue;
        for (std::size_t k = 0; k < 3; ++k) out[i * 4 + k] = static_cast<std::uint8_t>(static_cast<float>(out[i * 4 + k]) * c[k]);
    }
    return out;
}

std::optional<FreePicture> makeFreePicture(const std::string& name) {
    auto number = [&](const char* prefix) -> int {
        const std::string p = prefix;
        if (name.rfind(p, 0) != 0 || name.size() == p.size()) return -1;
        for (std::size_t i = p.size(); i < name.size(); ++i)
            if (name[i] < '0' || name[i] > '9') return -1;
        return std::atoi(name.c_str() + p.size());
    };
    if (const int level = number("field"); level >= 0 && level <= 10) return fieldPicture(level).picture();
    if (const int n = number("glue"); n >= 0) {
        static const Col kTop[4] = {rgb(30, 40, 90), rgb(70, 30, 80), rgb(20, 70, 70), rgb(80, 50, 20)};
        Canvas c = backdrop(kTop[n % 4], scaled(kTop[n % 4], 0.35f), 40 + n);
        scatterBombs(c, n, 0.22f);
        return c.picture();
    }
    if (name == "mainmenu") {
        Canvas c = backdrop(rgb(24, 30, 80), rgb(8, 8, 24), 7);
        scatterStars(c, 160, 11);
        drawTitle(c, "ATOMIC BOMBERMAN", 26, 5, rgb(255, 210, 60));
        drawTitle(c, "MODERN  -  FREE GRAPHICS SET", 72, 2, rgb(200, 220, 255));
        Pose pose;
        bigPlayer(c, 150, 400, 4.6f, 0, pose);
        Canvas bomb(200, 200);
        drawBomb(bomb, 60, 70, 34, 0, 1.0f);
        for (int y = 0; y < 200; ++y)
            for (int x = 0; x < 200; ++x) {
                Col p = bomb.at(x, y);
                if (bomb.tinted(x, y)) p = {p.r * 0.9f, p.g * 0.2f, p.b * 0.2f, p.a};
                c.blend(210 + x, 340 + y, p, 1.0f);
            }
        // The item texts belong to the picture, as in the original: the game draws only the cursor.
        static const char* const kItems[7] = {"START GAME", "START NETWORK GAME", "JOIN NETWORK GAME", "OPTIONS",
                                              "ABOUT", "MANUAL", "EXIT"};
        c.box(312, 108, 628, 392, 12, rgb(0, 0, 20, 0.55f));
        for (int i = 0; i < 7; ++i) {
            drawText(c, kItems[i], 362, static_cast<float>(140 + 38 * i - 18 + 2), 2, rgb(0, 0, 0, 0.8f));
            drawText(c, kItems[i], 360, static_cast<float>(140 + 38 * i - 18), 2, rgb(240, 240, 255));
        }
        return c.picture();
    }
    if (name == "results" || name == "roulette") {
        Canvas c = backdrop(rgb(60, 24, 90), rgb(16, 8, 36), 21);
        scatterStars(c, 220, 23);
        drawTitle(c, name == "results" ? "RESULTS" : "BONUS GAME", 24, 6, rgb(255, 210, 60));
        return c.picture();
    }
    if (name == "draw") {
        Canvas c = backdrop(rgb(70, 70, 80), rgb(20, 20, 26), 31);
        scatterBombs(c, 3, 0.18f);
        drawTitle(c, "DRAW GAME", 190, 9, rgb(240, 240, 250));
        drawTitle(c, "NOBODY WINS THIS ROUND", 290, 3, rgb(255, 210, 60));
        return c.picture();
    }
    if (const int who = number("victory"); who >= 0 && who <= 9) {
        const float* pc = kFreePlayerColour[who];
        Canvas c = backdrop({pc[0] * 0.35f, pc[1] * 0.35f, pc[2] * 0.35f + 0.1f, 1}, rgb(8, 8, 20), 41 + who);
        scatterStars(c, 260, 43 + who);
        drawTitle(c, "VICTORY", 24, 9, rgb(255, 210, 60));
        Pose pose;
        pose.armsUp = true;
        bigPlayer(c, 320, 420, 6.0f, who, pose);
        return c.picture();
    }
    if (const int team = number("team"); team == 0 || team == 1) {
        Canvas c = backdrop(team == 0 ? rgb(70, 70, 90) : rgb(110, 24, 24), rgb(8, 8, 20), 61 + team);
        scatterStars(c, 220, 63);
        drawTitle(c, "TEAM VICTORY", 24, 7, rgb(255, 210, 60));
        Pose pose;
        pose.armsUp = true;
        for (int i = 0; i < 3; ++i) bigPlayer(c, 170.0f + 150.0f * static_cast<float>(i), 410, 3.6f, team == 0 ? 0 : 2, pose);
        return c.picture();
    }
    if (name == "title") {
        Canvas c = backdrop(rgb(24, 30, 80), rgb(8, 8, 24), 7);
        scatterStars(c, 200, 71);
        drawTitle(c, "ATOMIC BOMBERMAN", 170, 6, rgb(255, 210, 60));
        drawTitle(c, "MODERN", 240, 6, rgb(240, 240, 255));
        return c.picture();
    }
    return std::nullopt;
}

FreeFont makeFreeFont() {
    // The 5 x 7 shapes stretched to 7 x 14 with soft edges, so that a text takes about
    // the room the game's screens were laid out for (8 pixels a character).
    constexpr int kW = 7, kH = 14, kTop = 2;
    FreeFont f;
    f.height = kH + kTop + 2;
    f.spacing = 1;
    f.x.assign(128, 0);
    f.width.assign(128, 0);
    int total = 0;
    for (int ch = 32; ch < 127; ++ch) {
        f.x[static_cast<std::size_t>(ch)] = total;
        f.width[static_cast<std::size_t>(ch)] = ch == ' ' ? 5 : kW;
        total += kW + 1;
    }
    f.atlasWidth = total;
    f.alpha.assign(static_cast<std::size_t>(total * f.height), 0);
    for (int ch = 33; ch < 127; ++ch)
        for (int y = 0; y < kH; ++y)
            for (int x = 0; x < kW; ++x) {
                // Coverage of this pixel by the lit dots, 4 x 4 samples.
                int lit = 0;
                for (int sy = 0; sy < 4; ++sy)
                    for (int sx = 0; sx < 4; ++sx) {
                        const float u = (static_cast<float>(x) + (static_cast<float>(sx) + 0.5f) / 4.0f) * 5.0f / static_cast<float>(kW);
                        const float v = (static_cast<float>(y) + (static_cast<float>(sy) + 0.5f) / 4.0f) * 7.0f / static_cast<float>(kH);
                        lit += glyphDot(static_cast<char>(ch), static_cast<int>(u), static_cast<int>(v)) ? 1 : 0;
                    }
                const float a = std::min(1.0f, static_cast<float>(lit) / 16.0f * 1.5f);
                f.alpha[static_cast<std::size_t>((y + kTop) * total + f.x[static_cast<std::size_t>(ch)] + x)] = static_cast<std::uint8_t>(a * 255.0f);
            }
    return f;
}

}  // namespace ab
