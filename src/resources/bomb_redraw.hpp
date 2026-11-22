// A bomb of the original, drawn again at the enlarged size.
//
// The model that enlarges the sprites cannot do much with a bomb: the art is a few dozen pixels
// with a ragged dark outline, and what comes out is a blurry sphere with dots along the edge.
// A bomb is a ball with a soft shadow on one side, though, and that is easy to draw sharply at any
// size. Its shape (an ellipse) and its shading (a ramp of colours around a light point) are
// found in the original frame itself, so every frame, size and player colour is matched; the fuse
// cap on top, which is not part of the ball, is taken from the enlarged sprite.
#pragma once

#include <cstdint>
#include <vector>

#include "resources/ani_file.hpp"
#include "resources/png.hpp"

namespace ab {

// `big`: the enlarged frame (already with the smoothed outline), replaced by the redrawn bomb.
// `remap`: the player colour's remap table or null. False, and `big` untouched, if the frame is
// not a ball (too few body pixels, or the ellipse does not fit).
bool redrawBomb(PngImage& big, const AniFrame& frame, const GamePalette& palette, const std::vector<std::uint8_t>* remap);

}  // namespace ab
