// The folder chooser of the Android app, as seen from the game: the user picks
// the folder holding their copy of the original game, the choice is remembered,
// and the files the importer needs are copied from it into a folder the game can
// read by path ("staging"). The work is done by the app's Java side
// (android/app/src/main/java/.../GameActivity.java). Elsewhere: not available.
#pragma once

#include <string>

namespace ab::folder {

bool available();

enum class Pick { None, Open, Chosen, Cancelled };
void pick();       // opens the system's chooser; the game goes to the background meanwhile
Pick pickState();

std::string saved();      // the remembered folder as the system names it; empty if none
std::string savedName();  // the same for showing

struct Staging {
    enum class State { Idle, Running, Done, Failed } state = State::Idle;
    int done = 0;
    int total = 0;  // 0 while the folder is still being looked through
    std::string item;
    std::string error;
};
void startStaging(const std::string& destination);
Staging staging();
void stopStaging();

}  // namespace ab::folder
