// The folder holding the user's copy of the original game, as chosen with the
// system's folder chooser and remembered for later releases.
//
// On a desktop the chooser is SDL's folder dialog, the choice is kept in a small
// file and the folder is read by its path. In the Android app the work is done by
// the Java side (android/app/src/main/java/.../GameActivity.java): a chosen folder
// cannot be read by path there, so the files the importer needs are first copied
// into a folder that can ("staging").
#pragma once

#include <string>

struct SDL_Window;

namespace ab::folder {

// Desktop: the file the choice is remembered in. Call before anything else.
void setStore(const std::string& file);

bool available();

// Failed: the system has no folder chooser (pickError() says more).
enum class Pick { None, Open, Chosen, Cancelled, Failed };
void pick(SDL_Window* window);  // opens the system's chooser; on a phone the game goes to the background meanwhile
Pick pickState();
std::string pickError();

std::string saved();      // the remembered folder as the system names it; empty if none
std::string savedName();  // the same for showing
// Desktop: remembers a folder given another way (the --import-assets command).
void remember(const std::string& path);

// True where a chosen folder has to be copied to the staging folder before it can be read.
bool staged();

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
