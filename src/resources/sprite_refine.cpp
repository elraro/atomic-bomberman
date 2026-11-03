#include "resources/sprite_refine.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace ab {

namespace {

// Gaussian blur, separable, edges repeated.
void blur(std::vector<float>& a, int w, int h, float sigma) {
    const int radius = std::max(1, static_cast<int>(sigma * 3.0f));
    std::vector<float> kernel(static_cast<std::size_t>(2 * radius + 1));
    float total = 0;
    for (int i = -radius; i <= radius; ++i) {
        const float v = std::exp(-static_cast<float>(i * i) / (2.0f * sigma * sigma));
        kernel[static_cast<std::size_t>(i + radius)] = v;
        total += v;
    }
    for (float& v : kernel) v /= total;
    std::vector<float> tmp(a.size());
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            float s = 0;
            for (int i = -radius; i <= radius; ++i) s += a[static_cast<std::size_t>(y) * w + std::clamp(x + i, 0, w - 1)] * kernel[static_cast<std::size_t>(i + radius)];
            tmp[static_cast<std::size_t>(y) * w + x] = s;
        }
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            float s = 0;
            for (int i = -radius; i <= radius; ++i) s += tmp[static_cast<std::size_t>(std::clamp(y + i, 0, h - 1)) * w + x] * kernel[static_cast<std::size_t>(i + radius)];
            a[static_cast<std::size_t>(y) * w + x] = s;
        }
}

}  // namespace

std::vector<std::uint8_t> smoothMask(const std::vector<std::uint8_t>& mask, int width, int height, int scale) {
    const int s = scale;
    const int w = width * s;
    const int h = height * s;
    const std::size_t count = static_cast<std::size_t>(w) * static_cast<std::size_t>(h);
    // The mask enlarged with bilinear filtering, blurred, and given a soft edge.
    auto at = [&](int x, int y) {
        x = std::clamp(x, 0, width - 1);
        y = std::clamp(y, 0, height - 1);
        return mask[static_cast<std::size_t>(y) * width + x] > 0 ? 255.0f : 0.0f;
    };
    std::vector<float> a(count);
    for (int y = 0; y < h; ++y) {
        const float fy = (static_cast<float>(y) + 0.5f) / static_cast<float>(s) - 0.5f;
        const int y0 = static_cast<int>(std::floor(fy));
        const float ty = fy - static_cast<float>(y0);
        for (int x = 0; x < w; ++x) {
            const float fx = (static_cast<float>(x) + 0.5f) / static_cast<float>(s) - 0.5f;
            const int x0 = static_cast<int>(std::floor(fx));
            const float tx = fx - static_cast<float>(x0);
            const float top = at(x0, y0) * (1 - tx) + at(x0 + 1, y0) * tx;
            const float bottom = at(x0, y0 + 1) * (1 - tx) + at(x0 + 1, y0 + 1) * tx;
            a[static_cast<std::size_t>(y) * w + x] = top * (1 - ty) + bottom * ty;
        }
    }
    blur(a, w, h, 0.55f * static_cast<float>(s));
    std::vector<std::uint8_t> alpha(count);
    for (std::size_t i = 0; i < count; ++i) {
        const float t = std::clamp((a[i] - 70.0f) / 120.0f, 0.0f, 1.0f);
        alpha[i] = static_cast<std::uint8_t>(std::lround(t * t * (3.0f - 2.0f * t) * 255.0f));
    }
    return alpha;
}

void refineSilhouette(PngImage& big, int width, int height, const std::vector<std::uint8_t>& source) {
    if (width < 1 || height < 1 || big.width % width != 0 || big.height % height != 0 || big.width / width != big.height / height) return;
    if (source.size() != static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4) return;
    if (big.rgba.size() != static_cast<std::size_t>(big.width) * static_cast<std::size_t>(big.height) * 4) return;
    const int s = big.width / width;
    const int w = big.width;
    const int h = big.height;
    const std::size_t count = static_cast<std::size_t>(w) * static_cast<std::size_t>(h);

    std::vector<std::uint8_t> original(static_cast<std::size_t>(width) * static_cast<std::size_t>(height));
    for (std::size_t i = 0; i < original.size(); ++i) original[i] = source[i * 4 + 3] > 0 ? 255 : 0;
    const std::vector<std::uint8_t> alpha = smoothMask(original, width, height, s);

    // Colours that can be trusted: solid in both, and a few pixels away from the edge.
    const int reach = 3;
    std::vector<std::uint8_t> solid(count);
    for (std::size_t i = 0; i < count; ++i) solid[i] = alpha[i] >= 250 && big.rgba[i * 4 + 3] >= 160 ? 1 : 0;
    std::vector<std::uint8_t> eroded(count), tmp(count);
    for (int y = 0; y < h; ++y)  // erosion in x, then in y
        for (int x = 0; x < w; ++x) {
            std::uint8_t v = 1;
            for (int d = -reach; d <= reach && v; ++d) {
                const int xx = x + d;
                if (xx >= 0 && xx < w && !solid[static_cast<std::size_t>(y) * w + xx]) v = 0;
            }
            tmp[static_cast<std::size_t>(y) * w + x] = v;
        }
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            std::uint8_t v = 1;
            for (int d = -reach; d <= reach && v; ++d) {
                const int yy = y + d;
                if (yy >= 0 && yy < h && !tmp[static_cast<std::size_t>(yy) * w + x]) v = 0;
            }
            eroded[static_cast<std::size_t>(y) * w + x] = v;
        }

    // Carry those colours outwards, a pixel ring per pass.
    std::vector<std::uint8_t> known = eroded;
    std::vector<std::uint8_t> rgb(count * 3);
    for (std::size_t i = 0; i < count; ++i)
        if (known[i]) std::copy_n(&big.rgba[i * 4], 3, &rgb[i * 3]);
    for (int pass = 0; pass < 14; ++pass) {
        std::vector<std::uint8_t> nextKnown = known, nextRgb = rgb;
        bool any = false;
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const std::size_t i = static_cast<std::size_t>(y) * w + x;
                if (known[i] || alpha[i] == 0) continue;
                int sum[3] = {0, 0, 0}, n = 0;
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx) {
                        const int xx = x + dx, yy = y + dy;
                        if (xx < 0 || yy < 0 || xx >= w || yy >= h) continue;
                        const std::size_t j = static_cast<std::size_t>(yy) * w + xx;
                        if (!known[j]) continue;
                        for (int c = 0; c < 3; ++c) sum[c] += rgb[j * 3 + static_cast<std::size_t>(c)];
                        ++n;
                    }
                if (n == 0) continue;
                for (int c = 0; c < 3; ++c) nextRgb[i * 3 + static_cast<std::size_t>(c)] = static_cast<std::uint8_t>(sum[c] / n);
                nextKnown[i] = 1;
                any = true;
            }
        known.swap(nextKnown);
        rgb.swap(nextRgb);
        if (!any) break;
    }

    for (std::size_t i = 0; i < count; ++i) {
        std::uint8_t* p = &big.rgba[i * 4];
        if (alpha[i] == 0) {
            p[3] = 0;
            continue;
        }
        if (known[i]) std::copy_n(&rgb[i * 3], 3, p);  // else the model's colour stays
        p[3] = alpha[i];
    }
}

}  // namespace ab
