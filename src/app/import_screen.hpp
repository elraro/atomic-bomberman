// The screens shown before the game proper when the original game's data is
// taken by itself (the Android app; --import-folder on a desktop): on first
// start the offer to choose the folder holding a copy of the original game
// (Android's folder chooser; without one, a note saying where to put it), and
// the conversion with its progress bar. The chosen folder is remembered and
// read again when a new release has to convert anew. Text is drawn with the free asset set's
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
    std::string staging;     // where the needed files of a chosen folder are copied for the conversion (emptied after)
    std::string requestFile; // exists when a conversion is asked for although the release is the same (a new folder was chosen)
    std::string fontDir;     // a folder of the free asset set
    bool automated = false;  // unattended run: screens do not wait for a key
    // For unattended checks: called with a name ("notice", "progress", "done") when that
    // screen is on display, to save a picture of it.
    std::function<void(const std::string& name, int w, int h)> picture;
};

// Shows what is due. False if the program was closed meanwhile.
bool runImportScreens(SDL_Window* window, const ImportScreens& setup);

}  // namespace ab
