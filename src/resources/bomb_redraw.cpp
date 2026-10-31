#include "resources/bomb_redraw.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <utility>

#include "resources/sprite_refine.hpp"

namespace ab {

namespace {

struct Colour {
    float r = 0, g = 0, b = 0;
};

struct Ellipse {
    double cx = 0, cy = 0, rx = 1, ry = 1;
};

// Least squares fit of A x^2 + B y^2 + C x + D y = 1 (an ellipse with axes along x and y).
bool fitEllipse(const std::vector<std::pair<double, double>>& pts, Ellipse& out) {
    if (pts.size() < 12) return false;
    double m[4][5] = {};
    for (const auto& [x, y] : pts) {
        const double row[4] = {x * x, y * y, x, y};
        for (int i = 0; i < 4; ++i) {
            for (int j = 0; j < 4; ++j) m[i][j] += row[i] * row[j];
            m[i][4] += row[i];
        }
    }
    for (int i = 0; i < 4; ++i) {
        int pivot = i;
        for (int r = i + 1; r < 4; ++r)
            if (std::fabs(m[r][i]) > std::fabs(m[pivot][i])) pivot = r;
        if (std::fabs(m[pivot][i]) < 1e-12) return false;
        for (int c = 0; c < 5; ++c) std::swap(m[i][c], m[pivot][c]);
        for (int r = 0; r < 4; ++r) {
            if (r == i) continue;
            const double f = m[r][i] / m[i][i];
            for (int c = i; c < 5; ++c) m[r][c] -= f * m[i][c];
        }
    }
    const double a = m[0][4] / m[0][0], b = m[1][4] / m[1][1], c = m[2][4] / m[2][2], d = m[3][4] / m[3][3];
    if (a == 0 || b == 0) return false;
    out.cx = -c / (2 * a);
    out.cy = -d / (2 * b);
    const double k = 1 + a * out.cx * out.cx + b * out.cy * out.cy;
    if (k / a <= 0 || k / b <= 0) return false;  // (the sign of a, b and k depends on which side of the origin the ellipse lies)
    out.rx = std::sqrt(k / a);
    out.ry = std::sqrt(k / b);
    return true;
}

}  // namespace

bool redrawBomb(PngImage& big, const AniFrame& f, const GamePalette& palette, const std::vector<std::uint8_t>* remap) {
    const int w = f.width, h = f.height;
    if (w < 8 || h < 8 || big.width % w != 0 || big.height % h != 0 || big.width / w != big.height / h) return false;
    if (f.pixels.size() != static_cast<std::size_t>(w) * static_cast<std::size_t>(h) || palette.lookup.size() < 32768 || palette.rgb6.size() < 768) return false;
    const int s = big.width / w;
    const std::size_t n = static_cast<std::size_t>(w) * static_cast<std::size_t>(h);

    // Which pixels are the green ball (by the original's green palette), in what colour they are
    // drawn in this player colour.
    std::vector<std::uint8_t> opaque(n, 0), body(n, 0), cap(n, 0);
    std::vector<Colour> colour(n);
    for (std::size_t i = 0; i < n; ++i) {
        const std::uint16_t px = f.pixels[i];
        if (f.hasKey && px == f.key) continue;
        std::uint8_t idx = palette.lookup[px & 0x7FFF];
        if (idx == 0) continue;
        opaque[i] = 1;
        const std::uint8_t* plain = &palette.rgb6[static_cast<std::size_t>(idx) * 3];
        const float pr = plain[0] * 4.0f, pg = plain[1] * 4.0f, pb = plain[2] * 4.0f;
        body[i] = pg > 1.25f * pr && pg > 1.5f * pb ? 1 : 0;
        cap[i] = body[i] ? 0 : 1;
        if (remap != nullptr && (*remap)[idx] != 0) idx = (*remap)[idx];
        const std::uint8_t* c = &palette.rgb6[static_cast<std::size_t>(idx) * 3];
        colour[i] = {std::min(255.0f, c[0] * 4.0f), std::min(255.0f, c[1] * 4.0f), std::min(255.0f, c[2] * 4.0f)};
    }
    std::size_t bodyCount = 0, opaqueCount = 0;
    for (std::size_t i = 0; i < n; ++i) {
        bodyCount += body[i];
        opaqueCount += opaque[i];
    }
    if (bodyCount < 120 || bodyCount * 10 < opaqueCount * 6) return false;

    // The shape: the edge of the ball against the empty space around it.
    std::vector<std::pair<double, double>> edge;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            if (!body[static_cast<std::size_t>(y) * w + x]) continue;
            static const int d[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
            for (const auto& o : d) {
                const int xx = x + o[0], yy = y + o[1];
                if (xx < 0 || yy < 0 || xx >= w || yy >= h || !opaque[static_cast<std::size_t>(yy) * w + xx]) {
                    edge.emplace_back(x + 0.5 + o[0] * 0.5, y + 0.5 + o[1] * 0.5);
                    break;
                }
            }
        }
    Ellipse el;
    if (!fitEllipse(edge, el)) return false;
    // The edge of the art is the outer side of its dark outline; the soft edge drawn here must stay inside the frame.
    el.rx -= 0.35;
    el.ry -= 0.35;
    const double radius = (el.rx + el.ry) / 2;
    if (el.rx < 4 || el.ry < 4 || el.rx > 40 || el.ry > 40 || el.rx / el.ry > 1.7 || el.ry / el.rx > 1.7) return false;

    // Does the ellipse hold the ball? Inside it the ball must be (nearly) everywhere the
    // original has a pixel that is not the cap.
    {
        std::size_t inside = 0, agree = 0;
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const double ex = (x + 0.5 - el.cx) / el.rx, ey = (y + 0.5 - el.cy) / el.ry;
                if (ex * ex + ey * ey > 0.8) continue;
                const std::size_t i = static_cast<std::size_t>(y) * w + x;
                if (cap[i]) continue;
                ++inside;
                agree += body[i];
            }
        if (inside == 0 || agree * 100 < inside * 95) return false;
    }

    // Shading: the colour depends on the distance from a light point (in units of the radius).
    // The point is searched so that pixels at the same distance have the most alike colours; the
    // dark ring along the edge, which is the art's outline, is left out.
    constexpr int bins = 64;
    constexpr double rhoMax = 2.0;  // distances beyond this (in radii) share the last bin
    struct Sample {
        double x, y;
        Colour c;
    };
    std::vector<Sample> data;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const std::size_t i = static_cast<std::size_t>(y) * w + x;
            if (!body[i]) continue;
            const double ex = (x + 0.5 - el.cx) / el.rx, ey = (y + 0.5 - el.cy) / el.ry;
            if (std::sqrt(ex * ex + ey * ey) > 1.0 - 1.15 / radius) continue;
            data.push_back({x + 0.5, y + 0.5, colour[i]});
        }
    if (data.size() < 60) return false;
    auto rho = [&](double x, double y, double lx, double ly) { return std::hypot((x - lx) / radius, (y - ly) / radius); };
    double bestError = 1e300, lightX = el.cx, lightY = el.cy;
    for (int ix = -16; ix <= 2; ++ix)
        for (int iy = -16; iy <= 2; ++iy) {
            const double lx = el.cx + ix * 0.04 * radius * 2, ly = el.cy + iy * 0.04 * radius * 2;
            std::array<double, bins * 3> sum{};
            std::array<double, bins * 3> sumSq{};
            std::array<int, bins> cnt{};
            for (const Sample& sm : data) {
                const int b = std::min(bins - 1, static_cast<int>(rho(sm.x, sm.y, lx, ly) / rhoMax * bins));
                const double v[3] = {sm.c.r, sm.c.g, sm.c.b};
                for (int k = 0; k < 3; ++k) {
                    sum[static_cast<std::size_t>(b * 3 + k)] += v[k];
                    sumSq[static_cast<std::size_t>(b * 3 + k)] += v[k] * v[k];
                }
                ++cnt[static_cast<std::size_t>(b)];
            }
            double err = 0;
            for (int b = 0; b < bins; ++b)
                if (cnt[static_cast<std::size_t>(b)] > 0)
                    for (int k = 0; k < 3; ++k) {
                        const std::size_t j = static_cast<std::size_t>(b * 3 + k);
                        err += sumSq[j] - sum[j] * sum[j] / cnt[static_cast<std::size_t>(b)];
                    }
            if (err < bestError) {
                bestError = err;
                lightX = lx;
                lightY = ly;
            }
        }

    // The ramp: the mean colour in every distance bin, gaps filled, a little smoothed. Inside the
    // nearest bin and beyond the farthest one the colour stays as it is there.
    std::array<Colour, bins> ramp{};
    std::array<int, bins> have{};
    for (const Sample& sm : data) {
        const int b = std::min(bins - 1, static_cast<int>(rho(sm.x, sm.y, lightX, lightY) / rhoMax * bins));
        ramp[static_cast<std::size_t>(b)].r += static_cast<float>(sm.c.r);
        ramp[static_cast<std::size_t>(b)].g += static_cast<float>(sm.c.g);
        ramp[static_cast<std::size_t>(b)].b += static_cast<float>(sm.c.b);
        ++have[static_cast<std::size_t>(b)];
    }
    int first = -1, last = -1;
    for (int b = 0; b < bins; ++b)
        if (have[static_cast<std::size_t>(b)] > 0) {
            ramp[static_cast<std::size_t>(b)].r /= static_cast<float>(have[static_cast<std::size_t>(b)]);
            ramp[static_cast<std::size_t>(b)].g /= static_cast<float>(have[static_cast<std::size_t>(b)]);
            ramp[static_cast<std::size_t>(b)].b /= static_cast<float>(have[static_cast<std::size_t>(b)]);
            if (first < 0) first = b;
            last = b;
        }
    if (first < 0) return false;
    for (int b = 0; b < bins; ++b) {
        if (have[static_cast<std::size_t>(b)] > 0) continue;
        if (b < first) { ramp[static_cast<std::size_t>(b)] = ramp[static_cast<std::size_t>(first)]; continue; }
        if (b > last) { ramp[static_cast<std::size_t>(b)] = ramp[static_cast<std::size_t>(last)]; continue; }
        int lo = b, hi = b;
        while (have[static_cast<std::size_t>(lo)] == 0) --lo;
        while (have[static_cast<std::size_t>(hi)] == 0) ++hi;
        const float u = static_cast<float>(b - lo) / static_cast<float>(hi - lo);
        const Colour& a = ramp[static_cast<std::size_t>(lo)];
        const Colour& c = ramp[static_cast<std::size_t>(hi)];
        ramp[static_cast<std::size_t>(b)] = {a.r + (c.r - a.r) * u, a.g + (c.g - a.g) * u, a.b + (c.b - a.b) * u};
    }
    const std::array<Colour, bins> mean = ramp;  // before any smoothing; the colours of the runs below come from here
    // A ring that is one or two bins wide is noise of the art (a stray lighter pixel or two), not
    // shading: a median over five bins removes it and keeps the steps between the bands.
    {
        const std::array<Colour, bins> raw = ramp;
        auto median5 = [&](int b, float Colour::*channel) {
            float v[5];
            for (int k = -2; k <= 2; ++k) v[k + 2] = raw[static_cast<std::size_t>(std::clamp(b + k, 0, bins - 1))].*channel;
            std::sort(v, v + 5);
            return v[2];
        };
        for (int b = 0; b < bins; ++b) {
            ramp[static_cast<std::size_t>(b)].r = median5(b, &Colour::r);
            ramp[static_cast<std::size_t>(b)].g = median5(b, &Colour::g);
            ramp[static_cast<std::size_t>(b)].b = median5(b, &Colour::b);
        }
    }
    // Neighbouring bins that differ by a few shades only are one run (the art's lit area is one
    // colour; in a player colour it can be two near ones), and the run gets the colour that most of
    // its pixels have. Taking the commonest one, not the first or an average, keeps the colour of
    // the lit area and of every band the same from frame to frame, so the pulsing does not flicker.
    {
        float lo = 255, hi = 0;  // the span of the ramp: how big a step must be to count as shading
        for (const Colour& c : ramp)
            for (const float v : {c.r, c.g, c.b}) {
                lo = std::min(lo, v);
                hi = std::max(hi, v);
            }
        const float step = std::max(2.5f, 0.16f * (hi - lo));
        for (int b = 0; b < bins;) {
            const Colour ref = ramp[static_cast<std::size_t>(b)];
            int e = b + 1;
            while (e < bins && std::fabs(ramp[static_cast<std::size_t>(e)].r - ref.r) < step && std::fabs(ramp[static_cast<std::size_t>(e)].g - ref.g) < step &&
                   std::fabs(ramp[static_cast<std::size_t>(e)].b - ref.b) < step)
                ++e;
            // The commonest colour of the run: bins of (nearly) the same colour are counted together,
            // for a flat area is spread over many bins, each with fewer pixels than a narrow band.
            int best = b, bestTotal = -1;
            for (int k = b; k < e; ++k) {
                int total = 0;
                for (int j = b; j < e; ++j)
                    if (std::fabs(mean[static_cast<std::size_t>(j)].r - mean[static_cast<std::size_t>(k)].r) < 2.0f && std::fabs(mean[static_cast<std::size_t>(j)].g - mean[static_cast<std::size_t>(k)].g) < 2.0f &&
                        std::fabs(mean[static_cast<std::size_t>(j)].b - mean[static_cast<std::size_t>(k)].b) < 2.0f)
                        total += have[static_cast<std::size_t>(j)];
                if (total > bestTotal) {
                    bestTotal = total;
                    best = k;
                }
            }
            const Colour c = bestTotal > 0 ? mean[static_cast<std::size_t>(best)] : ref;
            for (int k = b; k < e; ++k) ramp[static_cast<std::size_t>(k)] = c;
            b = e;
        }
    }
    auto shade = [&](double r) {
        const double t = std::clamp(r / rhoMax * bins - 0.5, 0.0, bins - 1.0);
        const int lo = static_cast<int>(std::floor(t));
        const int hi = std::min(bins - 1, lo + 1);
        const float u = static_cast<float>(t - lo);
        const Colour& a = ramp[static_cast<std::size_t>(lo)];
        const Colour& c = ramp[static_cast<std::size_t>(hi)];
        return Colour{a.r + (c.r - a.r) * u, a.g + (c.g - a.g) * u, a.b + (c.b - a.b) * u};
    };

    // The fuse cap (what is not the ball) from the enlarged sprite, with a soft edge.
    std::vector<std::uint8_t> capMask(n);
    for (std::size_t i = 0; i < n; ++i) capMask[i] = cap[i] ? 255 : 0;
    const std::vector<std::uint8_t> capAlpha = smoothMask(capMask, w, h, s);

    // The cap's colours: the original's cap pixels enlarged with a cubic filter, using the cap
    // pixels only (so no ball or empty pixel bleeds into it), in this player colour.
    auto cubic = [](double t) {  // Catmull-Rom: sharp; its overshoot is cut off below
        t = std::fabs(t);
        if (t < 1) return 1.5 * t * t * t - 2.5 * t * t + 1;
        if (t < 2) return -0.5 * t * t * t + 2.5 * t * t - 4 * t + 2;
        return 0.0;
    };
    auto capColour = [&](int X, int Y, double out[3]) {
        const double fx = (X + 0.5) / s - 0.5, fy = (Y + 0.5) / s - 0.5;
        const int x0 = static_cast<int>(std::floor(fx)), y0 = static_cast<int>(std::floor(fy));
        double sum[3] = {0, 0, 0}, weight = 0, near[3] = {0, 0, 0}, nearWeight = 0;
        double lowest[3] = {255, 255, 255}, highest[3] = {0, 0, 0};
        for (int dy = -1; dy <= 2; ++dy)
            for (int dx = -1; dx <= 2; ++dx) {
                const int x = x0 + dx, y = y0 + dy;
                if (x < 0 || y < 0 || x >= w || y >= h || !cap[static_cast<std::size_t>(y) * w + x]) continue;
                const Colour& c = colour[static_cast<std::size_t>(y) * w + x];
                const double wt = cubic(fx - x) * cubic(fy - y);
                sum[0] += c.r * wt; sum[1] += c.g * wt; sum[2] += c.b * wt;
                weight += wt;
                const double bw = std::max(0.0, 1 - std::fabs(fx - x)) * std::max(0.0, 1 - std::fabs(fy - y)) + 1e-6;
                near[0] += c.r * bw; near[1] += c.g * bw; near[2] += c.b * bw;
                nearWeight += bw;
                const double v[3] = {c.r, c.g, c.b};
                for (int k = 0; k < 3; ++k) {
                    lowest[k] = std::min(lowest[k], v[k]);
                    highest[k] = std::max(highest[k], v[k]);
                }
            }
        if (nearWeight <= 0) return false;
        for (int k = 0; k < 3; ++k) out[k] = std::clamp(weight > 0.35 ? sum[k] / weight : near[k] / nearWeight, lowest[k], highest[k]);
        return true;
    };

    std::vector<std::uint8_t> out(static_cast<std::size_t>(big.width) * static_cast<std::size_t>(big.height) * 4, 0);
    const double soft = 0.9;  // softness of the edge, in enlarged pixels
    for (int Y = 0; Y < big.height; ++Y) {
        const double py = (Y + 0.5) / s;
        for (int X = 0; X < big.width; ++X) {
            const double px = (X + 0.5) / s;
            const std::size_t o = (static_cast<std::size_t>(Y) * big.width + X) * 4;
            const double ex = (px - el.cx) / el.rx, ey = (py - el.cy) / el.ry;
            const double dist = (std::sqrt(ex * ex + ey * ey) - 1.0) * radius * s;  // positive outside
            const double ballA = std::clamp(0.5 - dist / soft, 0.0, 1.0);
            double r = 0, g = 0, b = 0, a = 0;
            if (ballA > 0) {
                const Colour c = shade(rho(px, py, lightX, lightY));
                r = c.r;
                g = c.g;
                b = c.b;
                a = ballA;
            }
            const double ca = capAlpha[static_cast<std::size_t>(Y) * big.width + X] / 255.0;
            if (ca > 0) {  // the cap over the ball
                double q[3] = {static_cast<double>(big.rgba[o]), static_cast<double>(big.rgba[o + 1]), static_cast<double>(big.rgba[o + 2])};  // (the model's, where no cap pixel is near)
                capColour(X, Y, q);
                const double na = ca + a * (1 - ca);
                r = (q[0] * ca + r * a * (1 - ca)) / na;
                g = (q[1] * ca + g * a * (1 - ca)) / na;
                b = (q[2] * ca + b * a * (1 - ca)) / na;
                a = na;
            }
            out[o] = static_cast<std::uint8_t>(std::clamp(r, 0.0, 255.0) + 0.5);
            out[o + 1] = static_cast<std::uint8_t>(std::clamp(g, 0.0, 255.0) + 0.5);
            out[o + 2] = static_cast<std::uint8_t>(std::clamp(b, 0.0, 255.0) + 0.5);
            out[o + 3] = static_cast<std::uint8_t>(std::clamp(a, 0.0, 1.0) * 255.0 + 0.5);
        }
    }
    big.rgba = std::move(out);
    return true;
}

}  // namespace ab
