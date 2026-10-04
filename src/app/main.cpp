// SDL3 + OpenGL front end. All platform code lives here and in rendering/;
// the gameplay core (src/game) knows nothing about SDL or OpenGL.
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#define GL_GLEXT_PROTOTYPES 1
#include <GL/gl.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

#include "game/world.hpp"
#include "rendering/renderer.hpp"
#include "resources/scheme_file.hpp"

namespace {

constexpr int kStepMs = 50;  // fixed simulation step: the original's nominal frame

struct Options {
    std::string gameDir;
    std::string scheme = "basic";
    int players = 2;
    int frames = -1;          // stop after this many rendered frames (for automated runs)
    std::string screenshot;   // write the last frame as a PPM file
    bool demo = false;        // scripted input instead of the keyboard
    std::uint32_t seed = 1;
};

Options parseArgs(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--game-dir") o.gameDir = next();
        else if (a == "--scheme") o.scheme = next();
        else if (a == "--players") o.players = std::atoi(next().c_str());
        else if (a == "--frames") o.frames = std::atoi(next().c_str());
        else if (a == "--screenshot") o.screenshot = next();
        else if (a == "--seed") o.seed = static_cast<std::uint32_t>(std::atoi(next().c_str()));
        else if (a == "--demo") o.demo = true;
        else std::fprintf(stderr, "WARN  unknown argument %s\n", a.c_str());
    }
    o.players = std::clamp(o.players, 1, ab::kMaxPlayers);
    return o;
}

ab::PlayerInput keyboardInput(const bool* keys, int player) {
    ab::PlayerInput in;
    if (player == 0) {
        in.dir = {keys[SDL_SCANCODE_UP], keys[SDL_SCANCODE_RIGHT], keys[SDL_SCANCODE_DOWN], keys[SDL_SCANCODE_LEFT]};
        in.button1 = keys[SDL_SCANCODE_SPACE] || keys[SDL_SCANCODE_RCTRL];
        in.button2 = keys[SDL_SCANCODE_RETURN] || keys[SDL_SCANCODE_RSHIFT];
    } else if (player == 1) {
        in.dir = {keys[SDL_SCANCODE_W], keys[SDL_SCANCODE_D], keys[SDL_SCANCODE_S], keys[SDL_SCANCODE_A]};
        in.button1 = keys[SDL_SCANCODE_TAB] || keys[SDL_SCANCODE_LCTRL];
        in.button2 = keys[SDL_SCANCODE_Q] || keys[SDL_SCANCODE_LSHIFT];
    }
    return in;
}

// Deterministic stand-in for players: wander and drop bombs. Not an AI.
ab::PlayerInput scriptedInput(int player, int step, ab::Rng& rng) {
    static std::array<int, ab::kMaxPlayers> dir{};
    ab::PlayerInput in;
    if (step % 8 == 0 || rng.below(12) == 0) dir[static_cast<std::size_t>(player)] = rng.below(4);
    in.dir[static_cast<std::size_t>(dir[static_cast<std::size_t>(player)])] = true;
    in.button1 = rng.below(40) == 0;
    return in;
}

void startRound(ab::World& world, const ab::Scheme& scheme, int players) {
    world.startRound(scheme, true);
    for (int i = 0; i < players; ++i) world.addPlayer(i);
    std::fprintf(stderr, "INFO  Round started players=%d\n", players);
}

void writePpm(const std::string& path, int w, int h) {
    std::vector<unsigned char> px(static_cast<std::size_t>(w * h * 3));
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, px.data());
    std::ofstream out(path, std::ios::binary);
    out << "P6\n" << w << " " << h << "\n255\n";
    for (int y = h - 1; y >= 0; --y)
        out.write(reinterpret_cast<const char*>(px.data() + static_cast<std::size_t>(y * w * 3)), w * 3);
    std::fprintf(stderr, "INFO  Screenshot written path=%s size=%dx%d\n", path.c_str(), w, h);
}

}  // namespace

int main(int argc, char** argv) {
    const Options opt = parseArgs(argc, argv);

    ab::Values values = ab::Values::defaults();
    ab::Scheme scheme = ab::Scheme::pillars();
    if (!opt.gameDir.empty()) {
        // Original data files are read from the user's own copy at run time.
        ab::Values fromFile;
        if (fromFile.loadFile(opt.gameDir + "/data/res/valuelst.res")) {
            values = fromFile;
            std::fprintf(stderr, "INFO  Loaded valuelst.res\n");
        } else {
            std::fprintf(stderr, "WARN  valuelst.res not found under %s, using built-in defaults\n", opt.gameDir.c_str());
        }
        const std::string path = opt.gameDir + "/data/schemes/" + opt.scheme + ".sch";
        if (auto sf = ab::loadSchemeFile(path)) {
            scheme = sf->scheme;
            std::fprintf(stderr, "INFO  Loaded scheme name=\"%s\" density=%d\n", sf->name.c_str(), scheme.brickDensity);
        } else {
            std::fprintf(stderr, "WARN  cannot read scheme %s, using empty arena\n", path.c_str());
        }
    }

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::fprintf(stderr, "ERROR SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_Window* window = SDL_CreateWindow("Atomic Bomberman (modern)", 960, 720, SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
    if (window == nullptr) {
        std::fprintf(stderr, "ERROR SDL_CreateWindow: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }
    SDL_GLContext gl = SDL_GL_CreateContext(window);
    if (gl == nullptr) {
        std::fprintf(stderr, "ERROR SDL_GL_CreateContext: %s\n", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
    SDL_GL_SetSwapInterval(1);
    std::fprintf(stderr, "INFO  OpenGL %s\n", reinterpret_cast<const char*>(glGetString(GL_VERSION)));

    int rc = 0;
    {
        ab::Renderer renderer;
        ab::World world(values, opt.seed);
        ab::Rng scriptRng(opt.seed + 77);
        startRound(world, scheme, opt.players);
        ab::RenderSnapshot previous;
        previous.capture(world);

        bool running = true;
        bool paused = false;
        int frame = 0;
        int step = 0;
        Uint64 last = SDL_GetTicksNS();
        double accumulatorMs = 0.0;

        while (running) {
            SDL_Event e;
            bool singleStep = false;
            while (SDL_PollEvent(&e)) {
                if (e.type == SDL_EVENT_QUIT) running = false;
                if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat) {
                    if (e.key.key == SDLK_ESCAPE) running = false;
                    if (e.key.key == SDLK_R) startRound(world, scheme, opt.players);
                    if (e.key.key == SDLK_P) paused = !paused;
                    if (e.key.key == SDLK_N) singleStep = true;  // advance one step while paused
                }
            }

            const Uint64 now = SDL_GetTicksNS();
            double frameMs = static_cast<double>(now - last) / 1.0e6;
            last = now;
            if (opt.frames > 0) frameMs = 1000.0 / 60.0;  // automated runs are frame-count driven
            accumulatorMs += std::min(frameMs, 250.0);
            if (paused) accumulatorMs = singleStep ? kStepMs : 0.0;

            const bool* keys = SDL_GetKeyboardState(nullptr);
            while (accumulatorMs >= kStepMs) {
                std::array<ab::PlayerInput, ab::kMaxPlayers> input{};
                for (int i = 0; i < opt.players; ++i)
                    input[static_cast<std::size_t>(i)] = opt.demo ? scriptedInput(i, step, scriptRng) : keyboardInput(keys, i);
                previous.capture(world);
                world.tick(kStepMs, input);
                accumulatorMs -= kStepMs;
                ++step;
            }

            int w = 0;
            int h = 0;
            SDL_GetWindowSizeInPixels(window, &w, &h);
            const float alpha = paused ? 1.0f : static_cast<float>(accumulatorMs / kStepMs);
            renderer.draw(world, previous, alpha, w, h);
            ++frame;
            if (opt.frames > 0 && frame >= opt.frames) {
                if (!opt.screenshot.empty()) writePpm(opt.screenshot, w, h);
                running = false;
            }
            SDL_GL_SwapWindow(window);
        }
        std::fprintf(stderr, "INFO  Exiting frames=%d steps=%d alive=%d bombs=%d\n", frame, step, world.alivePlayers(),
                     world.activeBombs());
    }
    SDL_GL_DestroyContext(gl);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return rc;
}
