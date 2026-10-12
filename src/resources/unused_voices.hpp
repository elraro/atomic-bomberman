// The voice clips on the original's disc that its sound list never names (about as many
// again as the game plays), and where each is put when the player asks to hear them.
// Not behaviour of the original: see docs/specifications/unused-voices.md.
#pragma once

#include <string>

namespace ab {

// The first sound id of the series a clip is added to, by its file name without
// extension (lower case); 0: this clip is not placed anywhere.
int unusedVoiceSeries(const std::string& name);

}  // namespace ab
