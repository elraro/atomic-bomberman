// Which layer a bomb is drawn in. Kept apart from the renderer so that a test can check it
// without a window.
#pragma once

#include "game/world.hpp"

namespace ab {

// A bomb on the ground (resting, sliding) is drawn under the players. A carried or a flying
// bomb is drawn above them: it is lifted over the head or arcs through the air, and drawn
// under the player it looked as if it were behind the player.
inline bool bombDrawnOverPlayers(const Bomb& b) { return b.mode == BombMode::Flying || b.mode == BombMode::Held; }

}  // namespace ab
