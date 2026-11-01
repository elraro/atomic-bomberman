#include "resources/steam_art.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>

#include "free/free_art.hpp"
#include "resources/ani_file.hpp"
#include "resources/bomb_redraw.hpp"
#include "resources/hires_pack.hpp"
#include "resources/png.hpp"
#include "resources/sprite_refine.hpp"
#include "resources/upscale.hpp"

namespace fs = std::filesystem;

namespace ab {

namespace {

// RGBA, straight alpha.
struct Img {
    int w = 0;
    int h = 0;
    std::vector<std::uint8_t> px;
    bool empty() const { return w < 1 || h < 1; }
    std::uint8_t* at(int x, int y) { return &px[(static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x)) * 4]; }
    const std::uint8_t* at(int x, int y) const { return &px[(static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x)) * 4]; }
};

Img blank(int w, int h, std::uint8_t r = 0, std::uint8_t g = 0, std::uint8_t b = 0, std::uint8_t a = 0) {
    Img i;
    i.w = w;
    i.h = h;
    i.px.resize(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4);
    for (std::size_t k = 0; k < i.px.size(); k += 4) {
        i.px[k] = r;
        i.px[k + 1] = g;
        i.px[k + 2] = b;
        i.px[k + 3] = a;
    }
    return i;
}

Img fromRgba(int w, int h, std::vector<std::uint8_t> rgba) {
    Img i;
    i.w = w;
    i.h = h;
    i.px = std::move(rgba);
    if (i.px.size() != static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4) return {};
    return i;
}

// The part of a picture that is not transparent.
Img trimmed(const Img& s) {
    int x0 = s.w, y0 = s.h, x1 = -1, y1 = -1;
    for (int y = 0; y < s.h; ++y)
        for (int x = 0; x < s.w; ++x)
            if (s.at(x, y)[3] > 8) {
                x0 = std::min(x0, x);
                x1 = std::max(x1, x);
                y0 = std::min(y0, y);
                y1 = std::max(y1, y);
            }
    if (x1 < 0) return {};
    Img o = blank(x1 - x0 + 1, y1 - y0 + 1);
    for (int y = 0; y < o.h; ++y) std::copy_n(s.at(x0, y0 + y), static_cast<std::size_t>(o.w) * 4, o.at(0, y));
    return o;
}

// Nearest neighbour (sharp pixels) or bilinear on premultiplied colours.
Img resized(const Img& s, int W, int H, bool smooth) {
    if (s.empty() || W < 1 || H < 1) return {};
    Img o = blank(W, H);
    for (int y = 0; y < H; ++y) {
        const double fy = (y + 0.5) * s.h / H - 0.5;
        const int y0 = static_cast<int>(std::floor(fy));
        const double ty = fy - y0;
        for (int x = 0; x < W; ++x) {
            std::uint8_t* d = o.at(x, y);
            if (!smooth) {
                const std::uint8_t* q = s.at(std::min(s.w - 1, x * s.w / W), std::min(s.h - 1, y * s.h / H));
                std::copy_n(q, 4, d);
                continue;
            }
            const double fx = (x + 0.5) * s.w / W - 0.5;
            const int x0 = static_cast<int>(std::floor(fx));
            const double tx = fx - x0;
            double sum[4] = {0, 0, 0, 0};
            for (int j = 0; j < 2; ++j)
                for (int i = 0; i < 2; ++i) {
                    const std::uint8_t* q = s.at(std::clamp(x0 + i, 0, s.w - 1), std::clamp(y0 + j, 0, s.h - 1));
                    const double wt = (i ? tx : 1 - tx) * (j ? ty : 1 - ty);
                    const double a = q[3] / 255.0;
                    sum[0] += q[0] * a * wt;
                    sum[1] += q[1] * a * wt;
                    sum[2] += q[2] * a * wt;
                    sum[3] += a * wt;
                }
            if (sum[3] > 0.002)
                for (int c = 0; c < 3; ++c) d[c] = static_cast<std::uint8_t>(std::clamp(sum[c] / sum[3], 0.0, 255.0) + 0.5);
            d[3] = static_cast<std::uint8_t>(std::clamp(sum[3], 0.0, 1.0) * 255.0 + 0.5);
        }
    }
    return o;
}

// `s` put on `dst` with its top left corner at (x0, y0); `opacity` 0-1.
void over(Img& dst, const Img& s, int x0, int y0, double opacity = 1.0) {
    for (int y = 0; y < s.h; ++y) {
        const int dy = y0 + y;
        if (dy < 0 || dy >= dst.h) continue;
        for (int x = 0; x < s.w; ++x) {
            const int dx = x0 + x;
            if (dx < 0 || dx >= dst.w) continue;
            const std::uint8_t* p = s.at(x, y);
            const double sa = p[3] / 255.0 * opacity;
            if (sa <= 0) continue;
            std::uint8_t* q = dst.at(dx, dy);
            const double da = q[3] / 255.0;
            const double oa = sa + da * (1 - sa);
            for (int c = 0; c < 3; ++c) q[c] = static_cast<std::uint8_t>(std::clamp((p[c] * sa + q[c] * da * (1 - sa)) / oa, 0.0, 255.0) + 0.5);
            q[3] = static_cast<std::uint8_t>(oa * 255.0 + 0.5);
        }
    }
}

// ---------------------------------------------------------------------------------------------
// The material: a level background, the players, a bomb, a font.

struct Font {
    int spacing = 1;
    int height = 0;
    std::vector<Img> glyph;  // by font slot; white with the shape in alpha; width 0: none
};

struct Source {
    std::string label;
    Img field;
    std::vector<Img> players;  // standing, facing the viewer, in different colours
    Img bomb;
    Font font;
    bool smooth = false;       // the sprites are enlarged ones: scale them smoothly
};

const AniSequence* findSequence(const AniFile& f, const std::string& name) {
    for (const AniSequence& s : f.sequences)
        if (s.name == name && !s.steps.empty()) return &s;
    return nullptr;
}

Img pcxPicture(const PcxImage& pcx) {
    Img i = blank(pcx.width, pcx.height);
    for (std::size_t k = 0; k < pcx.indices.size(); ++k) {
        const std::uint8_t* c = &pcx.palette[static_cast<std::size_t>(pcx.indices[k]) * 3];
        i.px[k * 4] = c[0];
        i.px[k * 4 + 1] = c[1];
        i.px[k * 4 + 2] = c[2];
        i.px[k * 4 + 3] = 255;
    }
    return i;
}

Img fromPng(const PngImage& p) { return fromRgba(p.width, p.height, p.rgba); }

constexpr int kPlayerColours[4] = {0, 1, 2, 3};

bool loadOriginal(const std::string& dir, bool useHires, Source& out, std::string& why) {
    const auto pal = loadPaletteFile(dir + "/color.pal");
    if (!pal) {
        why = "cannot read color.pal in " + dir;
        return false;
    }
    std::array<std::vector<std::uint8_t>, 10> remap;
    for (int c = 0; c < 10; ++c)
        if (auto r = readFileBytes(dir + "/" + std::to_string(c) + ".rmp"); r && r->size() >= 256) remap[static_cast<std::size_t>(c)].assign(r->begin(), r->begin() + 256);
    const std::string hires = hiresDirFor(dir);
    auto packOf = [&](const char* stem) { return useHires ? HiresPack::open(hires + "/ani/" + stem + ".hani") : std::nullopt; };

    // One frame of a sequence, enlarged if the pack has it; `bomb` redraws the ball as the game does.
    auto frameImage = [&](const char* file, const char* sequence, int colour, bool bomb, bool& enlarged) -> Img {
        const auto ani = loadAniFile(dir + "/data/ani/" + file + ".ani");
        const AniSequence* seq = ani ? findSequence(*ani, sequence) : nullptr;
        if (!seq) return {};
        const int index = seq->steps[0].frame;
        if (index < 0 || static_cast<std::size_t>(index) >= ani->frames.size()) return {};
        const AniFrame& f = ani->frames[static_cast<std::size_t>(index)];
        if (f.width < 1 || f.height < 1 || f.pixels.empty()) return {};
        const std::vector<std::uint8_t>* table = colour >= 0 && !remap[static_cast<std::size_t>(colour)].empty() ? &remap[static_cast<std::size_t>(colour)] : nullptr;
        if (const auto pack = packOf(file)) {
            std::optional<std::vector<std::uint8_t>> bytes;
            if (colour >= 0) bytes = pack->read(index, colour);
            if (!bytes) bytes = pack->read(index, -1);
            if (bytes)
                if (auto big = decodePng(*bytes); big && big->width % f.width == 0 && big->height % f.height == 0 && big->width / f.width == big->height / f.height) {
                    refineSilhouette(*big, f.width, f.height, aniFrameRgba(f, *pal, nullptr));
                    if (bomb) redrawBomb(*big, f, *pal, table);
                    enlarged = true;
                    return fromPng(*big);
                }
        }
        return fromRgba(f.width, f.height, aniFrameRgba(f, *pal, table));
    };

    // Only if every sprite came enlarged are they scaled smoothly; mixed, they stay sharp.
    bool all = true;
    for (int c : kPlayerColours) {
        bool enlarged = false;
        Img p = frameImage("stand", "stand south", c, false, enlarged);
        all = all && enlarged;
        if (!p.empty()) out.players.push_back(std::move(p));
    }
    bool enlarged = false;
    out.bomb = frameImage("bombs", "bomb regular green", 1, true, enlarged);  // in the second player's colour: black
    all = all && enlarged;
    if (out.players.empty() || out.bomb.empty()) {
        why = "stand.ani or bombs.ani do not have the frames needed";
        return false;
    }
    out.smooth = all;

    if (useHires)
        if (auto big = loadPngFile(hires + "/res/field0.png")) out.field = fromPng(*big);
    if (out.field.empty())
        if (auto pcx = loadPcxFile(dir + "/data/res/field0.pcx")) out.field = pcxPicture(*pcx);
    if (const auto ff = loadFontFile(dir + "/font1.fon")) {
        out.font.height = ff->height;
        out.font.spacing = ff->spacing;
        for (const FontGlyph& g : ff->glyphs) {
            Img i = blank(g.width, ff->height, 255, 255, 255, 0);
            if (g.alpha.size() == static_cast<std::size_t>(g.width) * static_cast<std::size_t>(ff->height))
                for (int y = 0; y < ff->height; ++y)
                    for (int x = 0; x < g.width; ++x) i.at(x, y)[3] = g.alpha[static_cast<std::size_t>(y * g.width + x)];
            out.font.glyph.push_back(std::move(i));
        }
    }
    out.label = "original";
    return true;
}

bool loadFree(Source& out, std::string& why) {
    const FreeArt art = makeFreeArt();
    auto frame = [&](const std::string& sequence, int colour) -> Img {
        const auto it = art.sequences.find(sequence);
        if (it == art.sequences.end() || it->second.empty()) return {};
        const FreeFrame& f = art.frames[static_cast<std::size_t>(it->second[0])];
        return fromRgba(f.width, f.height, colouredFrame(f, colour));
    };
    for (int c : kPlayerColours) {
        Img p = frame("stand south", c);
        if (!p.empty()) out.players.push_back(std::move(p));
    }
    out.bomb = frame("bomb regular green", 1);
    if (out.players.empty() || out.bomb.empty()) {
        why = "the free set does not have the frames needed";
        return false;
    }
    if (const auto pic = makeFreePicture("field0")) out.field = fromRgba(pic->width, pic->height, pic->rgba);
    const FreeFont ff = makeFreeFont();
    out.font.height = ff.height;
    out.font.spacing = ff.spacing;
    for (std::size_t c = 0; c < ff.x.size() && c < ff.width.size(); ++c) {
        Img i = blank(ff.width[c], ff.height, 255, 255, 255, 0);
        for (int y = 0; y < ff.height; ++y)
            for (int x = 0; x < ff.width[c]; ++x) {
                const std::size_t k = static_cast<std::size_t>(y * ff.atlasWidth + ff.x[c] + x);
                if (k < ff.alpha.size()) i.at(x, y)[3] = ff.alpha[k];
            }
        out.font.glyph.push_back(std::move(i));
    }
    out.label = "free";
    return true;
}

// ---------------------------------------------------------------------------------------------
// Drawing.

struct Rgb {
    int r, g, b;
};

// The level picture, made to cover W x H, darkened towards a deep blue, with the corners darker.
Img backdrop(const Source& src, int W, int H, double leftDark) {
    Img out = blank(W, H, 10, 25, 60, 255);
    if (!src.field.empty()) {
        // Only the middle of the level picture: the strip above the field and the rock at its sides are left out.
        Img middle = blank(src.field.w * 85 / 100, src.field.h * 78 / 100);
        for (int y = 0; y < middle.h; ++y) std::copy_n(src.field.at(src.field.w * 75 / 1000, src.field.h * 17 / 100 + y), static_cast<std::size_t>(middle.w) * 4, middle.at(0, y));
        const double k = std::max(static_cast<double>(W) / middle.w, static_cast<double>(H) / middle.h);
        const int cw = static_cast<int>(std::ceil(middle.w * k)), ch = static_cast<int>(std::ceil(middle.h * k));
        const Img big = resized(middle, cw, ch, true);
        for (int y = 0; y < H; ++y)
            std::copy_n(big.at((cw - W) / 2, (ch - H) / 2 + y), static_cast<std::size_t>(W) * 4, out.at(0, y));
    }
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const double dx = (x + 0.5) / W * 2 - 1, dy = (y + 0.5) / H * 2 - 1;
            const double vignette = std::clamp(1.0 - 0.38 * (dx * dx + dy * dy), 0.25, 1.0);
            const double side = 1.0 - leftDark * (1.0 - (x + 0.5) / W);
            std::uint8_t* p = out.at(x, y);
            const int navy[3] = {10, 25, 60};
            for (int c = 0; c < 3; ++c) p[c] = static_cast<std::uint8_t>(std::clamp((p[c] * 0.5 + navy[c] * 0.5) * vignette * side, 0.0, 255.0));
            p[3] = 255;
        }
    return out;
}

// A sprite scaled to about `height` tall and put on `dst`, standing on a shadow, centred on cx with its feet at `bottom`.
void place(Img& dst, const Img& sprite, const Source& src, int cx, int bottom, int height) {
    const Img t = trimmed(sprite);
    if (t.empty()) return;
    Img s;
    if (src.smooth) {
        s = resized(t, std::max(1, t.w * height / t.h), height, true);
    } else {
        const int k = std::max(1, static_cast<int>(std::lround(static_cast<double>(height) / t.h)));
        s = resized(t, t.w * k, t.h * k, false);
    }
    // A flat shadow under the feet.
    const int sw = s.w * 8 / 10, sh = std::max(4, s.h / 9);
    Img shadow = blank(sw, sh);
    for (int y = 0; y < sh; ++y)
        for (int x = 0; x < sw; ++x) {
            const double dx = (x + 0.5) / sw * 2 - 1, dy = (y + 0.5) / sh * 2 - 1;
            const double d = dx * dx + dy * dy;
            if (d < 1) shadow.at(x, y)[3] = static_cast<std::uint8_t>(130 * (1 - d));
        }
    over(dst, shadow, cx - sw / 2, bottom - sh * 2 / 3);
    over(dst, s, cx - s.w / 2, bottom - s.h);
}

// The lettering: the game's font enlarged by a whole factor, with a gradient, an outline and a shadow.
Img lettering(const Font& font, const std::string& text, int scale, Rgb top, Rgb bottom) {
    int width = 0;
    for (char ch : text) {
        const auto c = static_cast<std::size_t>(static_cast<unsigned char>(ch));
        width += (c < font.glyph.size() ? font.glyph[c].w : 0) + font.spacing;
    }
    width = std::max(1, width - font.spacing);
    Img mask = blank(width, font.height);
    int x = 0;
    for (char ch : text) {
        const auto c = static_cast<std::size_t>(static_cast<unsigned char>(ch));
        if (c >= font.glyph.size()) continue;
        const Img& g = font.glyph[c];
        for (int yy = 0; yy < g.h && yy < mask.h; ++yy)
            for (int xx = 0; xx < g.w; ++xx)
                if (g.at(xx, yy)[3] > 127) mask.at(x + xx, yy)[3] = 255;
        x += g.w + font.spacing;
    }
    const Img a = resized(mask, mask.w * scale, mask.h * scale, false);
    const int ring = std::max(2, scale * 3 / 4), drop = std::max(2, scale);
    const int pad = ring + drop;
    Img out = blank(a.w + 2 * pad, a.h + 2 * pad);
    // The shape grown by `ring` pixels (two passes of a box maximum).
    std::vector<std::uint8_t> grown(static_cast<std::size_t>(a.w + 2 * ring) * static_cast<std::size_t>(a.h + 2 * ring), 0);
    const int gw = a.w + 2 * ring, gh = a.h + 2 * ring;
    std::vector<std::uint8_t> rows(grown.size(), 0);
    for (int y = 0; y < a.h; ++y)
        for (int xx = 0; xx < gw; ++xx) {
            std::uint8_t m = 0;
            for (int d = -ring; d <= ring; ++d) {
                const int sx = xx - ring + d;
                if (sx >= 0 && sx < a.w) m = std::max(m, a.at(sx, y)[3]);
            }
            rows[static_cast<std::size_t>(y + ring) * static_cast<std::size_t>(gw) + static_cast<std::size_t>(xx)] = m;
        }
    for (int y = 0; y < gh; ++y)
        for (int xx = 0; xx < gw; ++xx) {
            std::uint8_t m = 0;
            for (int d = -ring; d <= ring; ++d) {
                const int sy = y + d;
                if (sy >= 0 && sy < gh) m = std::max(m, rows[static_cast<std::size_t>(sy) * static_cast<std::size_t>(gw) + static_cast<std::size_t>(xx)]);
            }
            grown[static_cast<std::size_t>(y) * static_cast<std::size_t>(gw) + static_cast<std::size_t>(xx)] = m;
        }
    for (int y = 0; y < gh; ++y)
        for (int xx = 0; xx < gw; ++xx) {
            if (!grown[static_cast<std::size_t>(y) * static_cast<std::size_t>(gw) + static_cast<std::size_t>(xx)]) continue;
            const int ox = xx + drop, oy = y + drop;  // shadow
            if (ox < out.w && oy < out.h) {
                std::uint8_t* q = out.at(ox, oy);
                q[0] = q[1] = q[2] = 0;
                q[3] = 120;
            }
        }
    for (int y = 0; y < gh; ++y)
        for (int xx = 0; xx < gw; ++xx) {
            if (!grown[static_cast<std::size_t>(y) * static_cast<std::size_t>(gw) + static_cast<std::size_t>(xx)]) continue;
            std::uint8_t* q = out.at(xx, y);
            q[0] = 28;
            q[1] = 12;
            q[2] = 6;
            q[3] = 255;
        }
    for (int y = 0; y < a.h; ++y) {
        const double t = a.h > 1 ? static_cast<double>(y) / (a.h - 1) : 0.0;
        const int r = static_cast<int>(top.r + (bottom.r - top.r) * t), g = static_cast<int>(top.g + (bottom.g - top.g) * t), b = static_cast<int>(top.b + (bottom.b - top.b) * t);
        for (int xx = 0; xx < a.w; ++xx) {
            if (a.at(xx, y)[3] == 0) continue;
            std::uint8_t* q = out.at(xx + ring, y + ring);
            q[0] = static_cast<std::uint8_t>(r);
            q[1] = static_cast<std::uint8_t>(g);
            q[2] = static_cast<std::uint8_t>(b);
            q[3] = 255;
        }
    }
    return out;
}

constexpr Rgb kTop = {255, 238, 130};
constexpr Rgb kBottom = {255, 146, 28};

// The width a text has at font scale 1.
int baseWidth(const Font& font, const std::string& text) {
    const Img one = lettering(font, text, 1, kTop, kBottom);
    return one.w;
}

// ATOMIC over BOMBERMAN, as large as the whole-number scale allows within `width`.
Img titleBlock(const Font& font, int width) {
    const int scale = std::max(1, width / std::max(1, baseWidth(font, "BOMBERMAN")));
    const Img a = lettering(font, "ATOMIC", scale, kTop, kBottom);
    const Img b = lettering(font, "BOMBERMAN", scale, kTop, kBottom);
    const int gap = std::max(2, scale * 2);
    Img out = blank(std::max(a.w, b.w), a.h + gap + b.h);
    over(out, a, (out.w - a.w) / 2, 0);
    over(out, b, (out.w - b.w) / 2, a.h + gap);
    return out;
}

struct Picture {
    std::string name;
    Img img;
    bool alpha;
};

Img makeCapsule(const Source& s) {
    Img img = backdrop(s, 600, 900, 0.0);
    const Img title = titleBlock(s.font, 540);
    over(img, title, (600 - title.w) / 2, 60);
    place(img, s.bomb, s, 300, 640, 300);
    const int x[4] = {95, 235, 365, 505};
    for (std::size_t i = 0; i < 4 && i < s.players.size(); ++i) place(img, s.players[i], s, x[i], 850, 230);
    return img;
}

Img makeWide(const Source& s) {
    Img img = backdrop(s, 920, 430, 0.35);
    const Img title = titleBlock(s.font, 400);
    over(img, title, 36, (430 - title.h) / 2);
    place(img, s.bomb, s, 490, 395, 120);
    const int x[4] = {610, 695, 780, 865};
    for (std::size_t i = 0; i < 4 && i < s.players.size(); ++i) place(img, s.players[i], s, x[i], 400, 190);
    return img;
}

Img makeHero(const Source& s) {
    Img img = backdrop(s, 3840, 1240, 0.55);
    place(img, s.bomb, s, 2150, 900, 520);
    const int x[4] = {2500, 2900, 3300, 3650};
    for (std::size_t i = 0; i < 4 && i < s.players.size(); ++i) place(img, s.players[i], s, x[i], 1130, 640);
    return img;
}

Img makeLogo(const Source& s) {
    const Img title = titleBlock(s.font, 1240);
    Img img = blank(1280, 720);
    over(img, title, (1280 - title.w) / 2, (720 - title.h) / 2);
    return img;
}

Img makeIcon(const Source& s) {
    Img img = blank(256, 256);
    const int radius = 52;
    for (int y = 0; y < 256; ++y)
        for (int x = 0; x < 256; ++x) {
            const int cx = x < radius ? radius : (x >= 256 - radius ? 255 - radius : x);
            const int cy = y < radius ? radius : (y >= 256 - radius ? 255 - radius : y);
            const double d = std::hypot(x - cx, y - cy);
            if (d > radius + 0.5) continue;
            const double t = y / 255.0;
            std::uint8_t* p = img.at(x, y);
            p[0] = static_cast<std::uint8_t>(30 - 14 * t);
            p[1] = static_cast<std::uint8_t>(70 - 34 * t);
            p[2] = static_cast<std::uint8_t>(130 - 60 * t);
            p[3] = static_cast<std::uint8_t>(d > radius - 0.5 ? 255 * (radius + 0.5 - d) : 255);
        }
    place(img, s.players[0], s, 128, 232, 188);
    return img;
}

bool writeIco(const std::string& path, const Img& icon) {
    const std::vector<std::uint8_t> png = encodePng(icon.w, icon.h, icon.px.data(), true);
    std::vector<std::uint8_t> out = {0, 0, 1, 0, 1, 0, static_cast<std::uint8_t>(icon.w >= 256 ? 0 : icon.w), static_cast<std::uint8_t>(icon.h >= 256 ? 0 : icon.h), 0, 0, 1, 0, 32, 0};
    const std::uint32_t size = static_cast<std::uint32_t>(png.size()), offset = 22;
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::uint8_t>(size >> (8 * i)));
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::uint8_t>(offset >> (8 * i)));
    out.insert(out.end(), png.begin(), png.end());
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(out.data()), static_cast<std::streamsize>(out.size()));
    return static_cast<bool>(f);
}

}  // namespace

SteamArtReport makeSteamArt(const SteamArtOptions& opt) {
    SteamArtReport report;
    Source src;
    std::error_code ec;
    const bool haveOriginal = !opt.free && !opt.assetsDir.empty() && fs::exists(fs::path(opt.assetsDir) / "color.pal", ec);
    std::string why;
    if (haveOriginal ? !loadOriginal(opt.assetsDir, opt.useHires, src, why) : !loadFree(src, why)) {
        report.error = why;
        return report;
    }
    if (src.font.glyph.empty()) {
        report.error = "the font could not be read";
        return report;
    }
    report.source = src.label;
    fs::path out = opt.outDir;
    if (out.empty()) out = fs::path(hiresDirFor(opt.assetsDir.empty() ? "assets" : opt.assetsDir)).parent_path() / "steam";
    fs::create_directories(out, ec);
    if (ec) {
        report.error = "cannot create " + out.string();
        return report;
    }
    report.outDir = out.string();
    const std::vector<Picture> pictures = {{"capsule_600x900.png", makeCapsule(src), false},
                                           {"hero_3840x1240.png", makeHero(src), false},
                                           {"logo_1280x720.png", makeLogo(src), true},
                                           {"wide_capsule_920x430.png", makeWide(src), false},
                                           {"icon_256.png", makeIcon(src), true}};
    for (const Picture& p : pictures) {
        if (!writePngFile((out / p.name).string(), p.img.w, p.img.h, p.img.px.data(), p.alpha)) {
            report.error = "cannot write " + (out / p.name).string();
            return report;
        }
        report.files.push_back(p.name);
    }
    if (!writeIco((out / "icon.ico").string(), pictures.back().img)) {
        report.error = "cannot write " + (out / "icon.ico").string();
        return report;
    }
    report.files.push_back("icon.ico");
    report.ok = true;
    return report;
}

}  // namespace ab
