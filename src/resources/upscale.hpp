// Enlarged copies of the imported graphics, made once with an AI upscaling program
// (Real-ESRGAN, "realesrgan-ncnn-vulkan"), for a sharp picture at a large window.
//
// Layout of the folder "hires" that sits next to the asset folder (so that a new import,
// which replaces the asset folder, does not delete it):
//   hires/ani/<file>.hani              the frames of data/ani/<file>.ani, drawn at <scale> times the size, in one
//                                      container (hires_pack.hpp): each frame, and in player colour N (only where it differs)
//   hires/res/<name>.png               data/res/<name>.pcx
// Earlier runs made a folder hires/ani/<file>/ of <frame>.png and <frame>_c<N>.png instead; the game still reads
// it, and the next run puts it into a container.
//   hires/manifest.txt                 what each part was made from, by which model, at what scale
// Only what is missing or out of date is made again; an interrupted run goes on where it stopped.
#pragma once

#include <functional>
#include <string>
#include <vector>

namespace ab {

struct UpscaleOptions {
    std::string assetsDir;         // imported game data (color.pal, data/ani, data/res)
    std::string hiresDir;          // default: hiresDirFor(assetsDir)
    std::string esrganExe;         // realesrgan-ncnn-vulkan(.exe)
    std::string modelsDir;         // its "models" folder; default: next to the program
    std::string model = "realesrgan-x4plus";
    int scale = 4;
    bool sprites = true;           // data/ani/*.ani
    bool pictures = true;          // data/res/*.pcx
    int gpu = -1;                  // Vulkan device of the program (-g); -1: its own choice
    int tile = 0;                  // tile size (-t); 0: its own choice
    bool cleanEdges = false;       // replace dark dither pixels at a sprite's edge by their neighbours before enlarging
    std::vector<std::string> only; // only these parts ("bombs", "ani/walk", "res/mainmenu"); empty: all
    bool verify = false;           // also check the pictures that are there and make bad ones again
    bool adopt = false;            // take over complete, correctly sized pictures that the manifest does not know (after a crash)
};

struct UpscaleProgress {
    int done = 0;
    int total = 0;
    std::string item;
};

struct UpscaleReport {
    bool ok = false;
    bool cancelled = false;
    int made = 0;     // parts made in this run
    int current = 0;  // parts that were up to date
    int repaired = 0; // single pictures made again because they were noise, empty or of the wrong size
    int plain = 0;    // pictures the model could not do, enlarged without it
    int adopted = 0;  // parts taken over from an earlier run (see UpscaleOptions::adopt)
    int failed = 0;
    std::string error;
};

// "<parent of the asset folder>/hires".
std::string hiresDirFor(const std::string& assetsDir);

// False from `progress` stops the run (after the part being made).
UpscaleReport upscaleAssets(const UpscaleOptions& options, const std::function<bool(const UpscaleProgress&)>& progress = {});

}  // namespace ab
