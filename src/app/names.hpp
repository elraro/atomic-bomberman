// Texts and colours shared by the front end's screens.
#pragma once

namespace ab {

// Level names, original messages 150-160.
inline constexpr const char* kLevelName[11] = {"Green Acres",   "Classic Green Acres", "The Hockey Rink",  "Ancient Egypt",
                                               "The Coal Mine", "The Beach",           "Aliens",           "Haunted House",
                                               "Under the Ocean", "Deep Forest Green", "Inner City Trash"};

// Text colour for each player seat (the players' own colours, readable on the menu pictures).
inline constexpr float kSeatColour[10][3] = {
    {0.95f, 0.95f, 0.95f}, {0.55f, 0.55f, 0.55f}, {0.90f, 0.15f, 0.15f}, {0.20f, 0.35f, 0.95f}, {0.15f, 0.80f, 0.20f},
    {0.95f, 0.90f, 0.15f}, {0.15f, 0.85f, 0.85f}, {0.90f, 0.20f, 0.90f}, {0.95f, 0.55f, 0.10f}, {0.55f, 0.20f, 0.90f}};

}  // namespace ab
