// The screens shown before the game proper when the original game's data is
// taken from a folder the user fills (the Android app; --import-folder on a
// desktop): a note on first start saying where that folder is, and the
// conversion with its progress bar. Text is drawn with the free asset set's
// font, the only one there is at that point.
#pragma once

#include <functional>
#include <string>

struct SDL_Window;

namespace ab {

struct ImportScreens {
    std::string folder;      // where the user leaves a copy of the original game
    std::string converted;   // where the converted files go (the game's asset folder)
    std::string release;     // this program's release: a folder converted by another one is made again
    std::string noticeFile;  // exists once the first-start note has been shown
    std::string fontDir;     // a folder of the free asset set
    bool automated = false;  // unattended run: screens do not wait for a key
    // For unattended checks: called with a name ("notice", "progress", "done") when that
    // screen is on display, to save a picture of it.
    std::function<void(const std::string& name, int w, int h)> picture;
};

// Shows what is due. False if the program was closed meanwhile.
bool runImportScreens(SDL_Window* window, const ImportScreens& setup);

}  // namespace ab
