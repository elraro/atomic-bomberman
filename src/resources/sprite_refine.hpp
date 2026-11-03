// Smooth outline for enlarged sprites.
//
// The original sprites have a hard edge (a pixel is there or it is not), often with a ragged
// line of dark dither pixels. The upscaling model enlarges that faithfully, so the edge of a
// bomb comes out as a ring of dots. Here the outline is taken from the original's mask instead:
// the mask is enlarged smoothly and given a soft edge, and the colours of the inside are carried
// outwards so that no dark rim shows at the soft edge.
#pragma once

#include <cstdint>
#include <vector>

#include "resources/png.hpp"

namespace ab {

// A mask (width*height, non-zero = inside) enlarged by `scale` with a soft edge: the alpha of
// width*scale by height*scale pixels.
std::vector<std::uint8_t> smoothMask(const std::vector<std::uint8_t>& mask, int width, int height, int scale);

// `big` is an enlarged frame (an integer multiple of the original's size); `source` is the
// original frame's RGBA, width*height*4. Changes `big` in place; leaves it alone if the sizes do
// not fit.
void refineSilhouette(PngImage& big, int width, int height, const std::vector<std::uint8_t>& source);

}  // namespace ab
