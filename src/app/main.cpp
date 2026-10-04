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

#include "audio/audio.hpp"
#include "game/ai.hpp"
#include "game/world.hpp"
#include "rendering/renderer.hpp"
#include "resources/scheme_file.hpp"

namespace {

constexpr int kStepMs = 50;  // fixed simulation step: the original's nominal frame

struct Options {
    std::string gameDir;
    std::string scheme = "basic";
    int players = 2;
    int humans = 1;           // slots 0..humans-1 use the keyboard (max 2); the rest are computer players
    int frames = -1;          // stop after this many rendered frames (for automated runs)
    std::string screenshot;   // write the last frame as a PPM file
    int menuShot = 0;         // automated: 1 = capture the main menu, 2 = the player list
    bool demo = false;        // scripted input instead of the keyboard
    bool menu = true;         // start at the main menu (off for --demo, --frames, --start)
    bool native = false;      // 640x480 window, the original's resolution
    int level = 0;            // level theme 0-10 (graphics)
    int wins = 2;             // round wins needed to take the match (original value 310)
    bool mute = false;
    bool shapes = false;      // draw flat shapes even when original graphics are available
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
        else if (a == "--humans") o.humans = std::clamp(std::atoi(next().c_str()), 0, 2);
        else if (a == "--frames") o.frames = std::atoi(next().c_str());
        else if (a == "--screenshot") o.screenshot = next();
        else if (a == "--seed") o.seed = static_cast<std::uint32_t>(std::atoi(next().c_str()));
        else if (a == "--demo") o.demo = true;
        else if (a == "--start") o.menu = false;
        else if (a == "--menu-shot") o.menuShot = std::atoi(next().c_str());
        else if (a == "--shapes") o.shapes = true;
        else if (a == "--mute") o.mute = true;
        else if (a == "--level") o.level = std::clamp(std::atoi(next().c_str()), 0, 10);
        else if (a == "--wins") o.wins = std::max(1, std::atoi(next().c_str()));
        else if (a == "--native") o.native = true;
        else std::fprintf(stderr, "WARN  unknown argument %s\n", a.c_str());
    }
    o.players = std::clamp(o.players, 1, ab::kMaxPlayers);
    if (o.demo || o.frames > 0) o.menu = o.menuShot != 0;
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

// Sound id ranges of the original's soundlst.res for each gameplay event.
void playEvents(ab::World& world, ab::Audio& audio) {
    for (const ab::Event& e : world.takeEvents()) {
        switch (e.kind) {
            case ab::EventKind::BombDropped: audio.playRange(100, 109); break;
            case ab::EventKind::BombKicked: audio.playRange(122, 129); break;
            case ab::EventKind::BombStopped: audio.playRange(130, 134); break;
            case ab::EventKind::BombPunched: audio.playRange(150, 159); break;
            case ab::EventKind::BombBounced: audio.playRange(160, 169); break;
            case ab::EventKind::BombGrabbed: audio.playRange(170, 171); break;
            case ab::EventKind::BombThrown: audio.playRange(172, 175); break;
            case ab::EventKind::BombExploded: audio.playRange(200, 299); break;
            case ab::EventKind::WallBlock: audio.playRange(140, 142); break;
            case ab::EventKind::Hurry: audio.playRange(2700, 2799); break;
            case ab::EventKind::PlayerDied: audio.playRange(300, 301); break;
            case ab::EventKind::HeadHit: audio.playRange(360, 369); break;
            case ab::EventKind::Pickup: audio.playRange(400, 499); break;
        }
    }
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

// How each player slot is controlled, as on the original's player list.
enum class Control { Off, Key0, Key1, Ai };

const char* controlName(Control c) {
    switch (c) {
        case Control::Key0: return "KEY 0";
        case Control::Key1: return "KEY 1";
        case Control::Ai: return "AI";
        default: return "OFF";
    }
}

enum class Screen { MainMenu, PlayerList, Match };

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

    // Arrows, warps, conveyors and trampolines belong to the level theme.
    std::vector<ab::Extra> extras;
    if (!opt.gameDir.empty()) {
        extras = ab::loadExtrasFile(opt.gameDir + "/data/res/extra" + std::to_string(opt.level) + ".res");
        if (!extras.empty()) std::fprintf(stderr, "INFO  Loaded level extras count=%zu\n", extras.size());
    }

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::fprintf(stderr, "ERROR SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_Window* window = SDL_CreateWindow("Atomic Bomberman (modern)", opt.native ? 640 : 960, opt.native ? 480 : 720,
                                          SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
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

    {
        ab::Renderer renderer;
        ab::SpriteBank sprites;
        if (!opt.gameDir.empty() && !opt.shapes) sprites.load(opt.gameDir, opt.level);
        ab::Audio audio;
        const bool sound = !opt.gameDir.empty() && !opt.mute && audio.init(opt.gameDir);
        ab::World world(values, opt.seed);
        std::vector<ab::AiPlayer> ai;
        for (int i = 0; i < ab::kMaxPlayers; ++i)
            ai.emplace_back(opt.seed * 31u + static_cast<std::uint32_t>(i) * 977u + 5u);

        // Player list. Defaults follow the command line; with none, the original's
        // default: player 1 on the first key set, player 2 a computer player.
        std::array<Control, ab::kMaxPlayers> control{};
        for (int i = 0; i < opt.players; ++i)
            control[static_cast<std::size_t>(i)] =
                opt.demo ? Control::Ai : i == 0 && opt.humans >= 1 ? Control::Key0 : i == 1 && opt.humans >= 2 ? Control::Key1 : Control::Ai;

        // The menu screens need the original pictures; without them the match starts at once.
        const bool haveMenu = opt.menu && sprites.loaded() && sprites.picture("mainmenu") != 0;
        Screen screen = haveMenu ? Screen::MainMenu : Screen::Match;
        int menuItem = 0;
        int listRow = 0;
        if (opt.menuShot == 2) screen = Screen::PlayerList;

        ab::RenderSnapshot previous;
        std::array<int, ab::kMaxPlayers> wins{};  // round wins in the current match
        int roundOverSteps = 0;
        bool matchOver = false;
        auto beginMatch = [&]() {
            world.startRound(scheme, true);
            world.setExtras(extras);
            int n = 0;
            for (int i = 0; i < ab::kMaxPlayers; ++i)
                if (control[static_cast<std::size_t>(i)] != Control::Off) {
                    world.addPlayer(i);
                    ++n;
                }
            previous.capture(world);
            roundOverSteps = 0;
            std::fprintf(stderr, "INFO  Round started players=%d\n", n);
        };
        if (screen == Screen::Match) {
            beginMatch();
            if (sound) audio.playMusic(1100 + opt.level);
        } else if (sound) {
            audio.playMusic(1010);  // main menu music
        }

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
                if (e.type != SDL_EVENT_KEY_DOWN || e.key.repeat) continue;
                const SDL_Keycode key = e.key.key;
                if (screen == Screen::MainMenu) {
                    // Items as on the original's menu picture.
                    if (key == SDLK_UP) menuItem = (menuItem + 6) % 7;
                    if (key == SDLK_DOWN) menuItem = (menuItem + 1) % 7;
                    if (key == SDLK_ESCAPE) running = false;
                    if (key == SDLK_RETURN) {
                        if (menuItem == 0) screen = Screen::PlayerList;
                        if (menuItem == 6) running = false;
                        if (sound) audio.playRange(10, 10);
                    }
                } else if (screen == Screen::PlayerList) {
                    Control& c = control[static_cast<std::size_t>(listRow)];
                    if (key == SDLK_UP) listRow = (listRow + ab::kMaxPlayers - 1) % ab::kMaxPlayers;
                    if (key == SDLK_DOWN) listRow = (listRow + 1) % ab::kMaxPlayers;
                    if (key == SDLK_LEFT) c = Control::Off;
                    if (key == SDLK_RIGHT)  // AI -> KEY 0 -> KEY 1 -> OFF -> AI, as observed on the original
                        c = c == Control::Ai ? Control::Key0 : c == Control::Key0 ? Control::Key1 : c == Control::Key1 ? Control::Off : Control::Ai;
                    if (key == SDLK_ESCAPE) screen = Screen::MainMenu;
                    if (key == SDLK_RETURN) {
                        int n = 0;
                        for (Control k : control) n += k != Control::Off ? 1 : 0;
                        if (n >= 2) {
                            wins = {};
                            matchOver = false;
                            screen = Screen::Match;
                            beginMatch();
                            if (sound) audio.playMusic(1100 + opt.level);
                        }
                    }
                } else {
                    if (key == SDLK_ESCAPE) {
                        if (haveMenu) {
                            screen = Screen::MainMenu;
                            if (sound) audio.playMusic(1010);
                        } else {
                            running = false;
                        }
                    }
                    if (key == SDLK_R) beginMatch();
                    if (key == SDLK_P) paused = !paused;
                    if (key == SDLK_N) singleStep = true;  // advance one step while paused
                }
            }

            const Uint64 now = SDL_GetTicksNS();
            double frameMs = static_cast<double>(now - last) / 1.0e6;
            last = now;
            if (opt.frames > 0) frameMs = 1000.0 / 60.0;  // automated runs are frame-count driven

            int w = 0;
            int h = 0;
            SDL_GetWindowSizeInPixels(window, &w, &h);

            if (screen == Screen::Match) {
                accumulatorMs += std::min(frameMs, 250.0);
                if (paused) accumulatorMs = singleStep ? kStepMs : 0.0;
                const bool* keys = SDL_GetKeyboardState(nullptr);
                while (accumulatorMs >= kStepMs) {
                    std::array<ab::PlayerInput, ab::kMaxPlayers> input{};
                    for (int i = 0; i < ab::kMaxPlayers; ++i) {
                        const Control c = control[static_cast<std::size_t>(i)];
                        if (c == Control::Ai) input[static_cast<std::size_t>(i)] = ai[static_cast<std::size_t>(i)].decide(world, i, kStepMs);
                        if (c == Control::Key0) input[static_cast<std::size_t>(i)] = keyboardInput(keys, 0);
                        if (c == Control::Key1) input[static_cast<std::size_t>(i)] = keyboardInput(keys, 1);
                    }
                    previous.capture(world);
                    world.tick(kStepMs, input);
                    playEvents(world, audio);
                    accumulatorMs -= kStepMs;
                    ++step;
                    // A decided round stays on screen for three seconds, then the next one starts.
                    if (world.roundOver() && roundOverSteps == 0) {
                        const int win = world.winner();
                        if (win >= 0) {
                            const int total = ++wins[static_cast<std::size_t>(win)];
                            std::fprintf(stderr, "INFO  Round over winner=%d kills=%d wins=%d\n", win, world.player(win).kills, total);
                            if (total >= opt.wins) {
                                matchOver = true;
                                std::fprintf(stderr, "INFO  Match over winner=%d\n", win);
                            }
                        } else {
                            std::fprintf(stderr, "INFO  Round over draw\n");
                        }
                    }
                    if (world.roundOver() && ++roundOverSteps > 60) {
                        if (matchOver) {
                            wins = {};
                            matchOver = false;
                            if (haveMenu) {
                                screen = Screen::MainMenu;
                                if (sound) audio.playMusic(1010);
                                break;
                            }
                        }
                        beginMatch();
                    }
                }
            }
            audio.update();

            if (screen == Screen::Match) {
                const float alpha = paused ? 1.0f : static_cast<float>(accumulatorMs / kStepMs);
                renderer.draw(world, previous, alpha, w, h, &sprites, &wins);
                if (world.roundOver() && sprites.loaded()) {
                    // Result line over the frozen field (the original shows full result screens).
                    const int win = world.winner();
                    const std::string line = win < 0 ? "DRAW GAME"
                                             : matchOver ? "PLAYER " + std::to_string(win + 1) + " WINS THE MATCH!"
                                                         : "PLAYER " + std::to_string(win + 1) + " WINS THE ROUND";
                    renderer.begin(w, h);
                    renderer.quad(150, 222, 340, 30, 0.0f, 0.0f, 0.0f, 0.75f);
                    renderer.text(sprites, line, 170, 229, 1.0f, 0.95f, 0.3f);
                    renderer.end();
                }
            } else if (screen == Screen::MainMenu) {
                renderer.begin(w, h);
                renderer.image(sprites.picture("mainmenu"));
                // The picture carries the item texts; the game draws only the cursor
                // (original value 700: first item at x 332, y 140, 38 px apart).
                renderer.sprite(sprites, "cursor1", frame / 8, -1, 332.0f, 140.0f + 38.0f * static_cast<float>(menuItem));
                renderer.end();
            } else {
                renderer.begin(w, h);
                renderer.image(sprites.picture("glue0"));
                // Original layout: heading at (40,140), list from (70,170) every 24 px (values 705/710).
                renderer.text(sprites, "Available players:", 41, 141, 0, 0, 0);
                renderer.text(sprites, "Available players:", 40, 140, 1, 1, 1);
                static const float colour[ab::kMaxPlayers][3] = {
                    {0.95f, 0.95f, 0.95f}, {0.55f, 0.55f, 0.55f}, {0.90f, 0.15f, 0.15f}, {0.20f, 0.35f, 0.95f}, {0.15f, 0.80f, 0.20f},
                    {0.95f, 0.90f, 0.15f}, {0.15f, 0.85f, 0.85f}, {0.90f, 0.20f, 0.90f}, {0.95f, 0.55f, 0.10f}, {0.55f, 0.20f, 0.90f}};
                for (int i = 0; i < ab::kMaxPlayers; ++i) {
                    const std::string line = "Player " + std::to_string(i + 1) + ": " + controlName(control[static_cast<std::size_t>(i)]);
                    const float y = 170.0f + 24.0f * static_cast<float>(i);
                    renderer.text(sprites, line, 71, y + 1, 0, 0, 0);
                    renderer.text(sprites, line, 70, y, colour[i][0], colour[i][1], colour[i][2]);
                }
                renderer.sprite(sprites, "cursor1", frame / 8, -1, 56.0f, 185.0f + 24.0f * static_cast<float>(listRow));
                renderer.text(sprites, "Up/Down: select   Left: off   Right: change   Enter: start", 60, 440, 0.4f, 1.0f, 1.0f);
                renderer.end();
            }

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
    return 0;
}
