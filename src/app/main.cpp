// SDL3 + OpenGL front end. All platform code lives here and in rendering/;
// the gameplay core (src/game) knows nothing about SDL or OpenGL.
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include "rendering/gl.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "audio/audio.hpp"
#include "game/ai.hpp"
#include "game/world.hpp"
#include "rendering/renderer.hpp"
#include "resources/asset_import.hpp"
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
    std::string importFrom;   // --import-assets: build the asset folder from this original game folder and exit
    std::string assetsDir;    // --assets-dir: where --import-assets writes (default: the per-user data folder)
    int menuShot = 0;         // automated: 1 = capture the main menu, 2 = the player list
    bool resultShot = false;  // automated: capture the result screen of the first decided round and exit
    std::vector<std::string> script;  // automated: key names pressed one after another (see --script)
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
        if (a == "--help" || a == "-h") {
            std::puts("Usage: atomic [options]\n"
                      "  --import-assets DIR  import the data of an original game copy, then exit\n"
                      "  --assets-dir DIR     where --import-assets writes (default: per-user data folder)\n"
                      "  --game-dir DIR       imported assets or an original game folder to play from\n"
                      "  --start              skip the menu and start a match at once\n"
                      "  --scheme NAME        scheme (map) to play, e.g. BASIC\n"
                      "  --level N            level theme 0-10\n"
                      "  --players N          number of players (default 4)\n"
                      "  --humans N           keyboard players, 0-2\n"
                      "  --wins N             round wins needed for the match\n"
                      "  --seed N             random seed\n"
                      "  --mute               no sound\n"
                      "  --native             640x480 window\n"
                      "  --shapes             plain shapes instead of the game's graphics\n"
                      "Testing: --demo --frames N --screenshot FILE --result-shot --menu-shot N --script KEYS");
            std::exit(0);
        }
        else if (a == "--game-dir") o.gameDir = next();
        else if (a == "--scheme") o.scheme = next();
        else if (a == "--players") o.players = std::atoi(next().c_str());
        else if (a == "--humans") o.humans = std::clamp(std::atoi(next().c_str()), 0, 2);
        else if (a == "--frames") o.frames = std::atoi(next().c_str());
        else if (a == "--screenshot") o.screenshot = next();
        else if (a == "--seed") o.seed = static_cast<std::uint32_t>(std::atoi(next().c_str()));
        else if (a == "--demo") o.demo = true;
        else if (a == "--start") o.menu = false;
        else if (a == "--import-assets") o.importFrom = next();
        else if (a == "--assets-dir") o.assetsDir = next();
        else if (a == "--result-shot") o.resultShot = true;
        else if (a == "--script") {
            // Comma-separated keys: up, down, left, right, enter, esc. One is pressed every 10 frames.
            std::string list = next(), item;
            for (char ch : list + ",") {
                if (ch == ',') {
                    if (!item.empty()) o.script.push_back(item);
                    item.clear();
                } else {
                    item += ch;
                }
            }
        }
        else if (a == "--menu-shot") o.menuShot = std::atoi(next().c_str());
        else if (a == "--shapes") o.shapes = true;
        else if (a == "--mute") o.mute = true;
        else if (a == "--level") o.level = std::clamp(std::atoi(next().c_str()), 0, 10);
        else if (a == "--wins") o.wins = std::max(1, std::atoi(next().c_str()));
        else if (a == "--native") o.native = true;
        else {
            std::fprintf(stderr, "ERROR unknown argument %s (see --help)\n", a.c_str());
            std::exit(2);
        }
    }
    o.players = std::clamp(o.players, 1, ab::kMaxPlayers);
    if (o.demo || o.frames > 0) o.menu = o.menuShot != 0 || !o.script.empty();
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

// Gamepad as a player's controller. Directions follow the original's joystick rule
// (axis below 30 % or above 70 % of its range), plus the d-pad.
ab::PlayerInput gamepadInput(SDL_Gamepad* pad) {
    ab::PlayerInput in;
    if (pad == nullptr) return in;
    const int x = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX);
    const int y = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTY);
    const int dead = 13107;  // 40 % of full deflection
    in.dir = {y < -dead || SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_UP),
              x > dead || SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_RIGHT),
              y > dead || SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_DOWN),
              x < -dead || SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_LEFT)};
    in.button1 = SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_SOUTH);
    in.button2 = SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_EAST) || SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_WEST);
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

bool looksLikeGameDir(const std::string& dir) {
    return std::ifstream(dir + "/color.pal").good() && std::ifstream(dir + "/data/res/valuelst.res").good();
}

// Per-user folder where --import-assets puts the game data by default.
std::string userAssetsDir() {
    char* pref = SDL_GetPrefPath("atomic-bomberman-modern", "atomic");
    if (pref == nullptr) return "assets";
    const std::string dir = std::string(pref) + "assets";
    SDL_free(pref);
    return dir;
}

// The folder holding the game data: --game-dir, then the ATOMIC_GAME_DIR
// environment variable, then an imported "assets" folder (next to the
// executable, in the current folder, or in the per-user data folder), then a
// copy of the original game in "game" (current folder, next to the executable,
// or one level above it).
std::string findGameDir(const std::string& requested) {
    std::vector<std::string> candidates;
    if (!requested.empty()) candidates.push_back(requested);
    if (const char* env = std::getenv("ATOMIC_GAME_DIR")) candidates.emplace_back(env);
    const char* base = SDL_GetBasePath();
    if (base != nullptr) candidates.push_back(std::string(base) + "assets");
    candidates.emplace_back("assets");
    candidates.push_back(userAssetsDir());
    candidates.emplace_back("game");
    if (base != nullptr) {
        candidates.push_back(std::string(base) + "game");
        candidates.push_back(std::string(base) + "../game");
    }
    for (const std::string& c : candidates)
        if (looksLikeGameDir(c)) return c;
    return "";
}

// How each player slot is controlled, as on the original's player list.
enum class Control { Off, Key0, Key1, Ai, Pad0, Pad1, Pad2, Pad3 };

const char* controlName(Control c) {
    switch (c) {
        case Control::Key0: return "KEY 0";
        case Control::Key1: return "KEY 1";
        case Control::Ai: return "AI";
        case Control::Pad0: return "JOY 0";
        case Control::Pad1: return "JOY 1";
        case Control::Pad2: return "JOY 2";
        case Control::Pad3: return "JOY 3";
        default: return "OFF";
    }
}

enum class Screen { MainMenu, PlayerList, LevelSetup, Match };

// Level names, original messages 150-160.
const char* const kLevelName[11] = {"Green Acres",   "Classic Green Acres", "The Hockey Rink",  "Ancient Egypt",
                                    "The Coal Mine", "The Beach",           "Aliens",           "Haunted House",
                                    "Under the Ocean", "Deep Forest Green", "Inner City Trash"};

struct SchemeEntry {
    std::string file;  // name without extension
    std::string title;
};

std::vector<SchemeEntry> listSchemes(const std::string& gameDir) {
    std::vector<SchemeEntry> out;
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(gameDir + "/data/schemes", ec)) {
        if (e.path().extension() != ".sch") continue;
        if (auto sf = ab::loadSchemeFile(e.path().string())) out.push_back({e.path().stem().string(), sf->name});
    }
    std::sort(out.begin(), out.end(), [](const SchemeEntry& a, const SchemeEntry& b) { return a.file < b.file; });
    return out;
}

int main(int argc, char** argv) {
    Options opt = parseArgs(argc, argv);
    if (!opt.importFrom.empty()) {
        const std::string to = opt.assetsDir.empty() ? userAssetsDir() : opt.assetsDir;
        std::printf("Importing game data\n  from: %s\n  to:   %s\n", opt.importFrom.c_str(), to.c_str());
        const ab::ImportReport r = ab::importAssets(opt.importFrom, to);
        if (!r.ok) {
            std::printf("Import failed: %s\n", r.error.c_str());
            return 1;
        }
        std::printf("Done: %d data files, %d sounds converted to .wav (%d listed sounds not found), %.1f MB.\n", r.dataFiles,
                    r.sounds, r.missingSounds, static_cast<double>(r.bytes) / 1.0e6);
        std::printf("Start the game without arguments to play.\n");
        return 0;
    }
    const std::string requested = opt.gameDir;
    opt.gameDir = findGameDir(requested);
    if (opt.gameDir.empty()) {
        std::fprintf(stderr,
                     "WARN  No original game files found%s%s.\n"
                     "WARN  Running without menu, original graphics and sound (placeholder shapes only).\n"
                     "WARN  Import them once from your copy of the original game:\n"
                     "WARN      atomic --import-assets PATH_TO_ORIGINAL_GAME\n"
                     "WARN  or point the program at it directly with --game-dir PATH.\n",
                     requested.empty() ? "" : " under ", requested.c_str());
    } else {
        std::fprintf(stderr, "INFO  Game files: %s\n", opt.gameDir.c_str());
    }

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

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        std::fprintf(stderr, "ERROR SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_Window* window = SDL_CreateWindow(opt.gameDir.empty() ? "Atomic Bomberman (modern) - no game files found: run with --game-dir PATH"
                                                               : "Atomic Bomberman (modern)",
                                          opt.native ? 640 : 960, opt.native ? 480 : 720,
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
    if (!ab::loadOpenGL()) {
        std::fprintf(stderr, "ERROR OpenGL 3.3 is not available on this system\n");
        SDL_GL_DestroyContext(gl);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
    SDL_GL_SetSwapInterval(opt.frames > 0 ? 0 : 1);  // automated runs do not wait for the display
    std::fprintf(stderr, "INFO  OpenGL %s\n", reinterpret_cast<const char*>(glGetString(GL_VERSION)));

    {
        ab::Renderer renderer;
        // Settings that the level screen can change.
        int level = opt.level;
        int winsNeeded = opt.wins;
        std::vector<SchemeEntry> schemes;
        if (!opt.gameDir.empty()) schemes = listSchemes(opt.gameDir);
        int schemeIndex = 0;
        for (std::size_t i = 0; i < schemes.size(); ++i)
            if (schemes[i].file == opt.scheme) schemeIndex = static_cast<int>(i);
        int setupRow = 0;
        bool teamPlay = false;
        std::array<int, ab::kMaxPlayers> teams{};
        if (!opt.gameDir.empty())
            if (auto sf = ab::loadSchemeFile(opt.gameDir + "/data/schemes/" + opt.scheme + ".sch")) teams = sf->team;

        auto spritesPtr = std::make_unique<ab::SpriteBank>();
        if (!opt.gameDir.empty() && !opt.shapes) spritesPtr->load(opt.gameDir, level);
        // Graphics, extras and scheme follow the level screen's choices.
        auto applySettings = [&]() {
            if (opt.gameDir.empty()) return;
            if (!opt.shapes && spritesPtr->level() != level) {
                spritesPtr = std::make_unique<ab::SpriteBank>();
                spritesPtr->load(opt.gameDir, level);
            }
            extras = ab::loadExtrasFile(opt.gameDir + "/data/res/extra" + std::to_string(level) + ".res");
            if (!schemes.empty())
                if (auto sf = ab::loadSchemeFile(opt.gameDir + "/data/schemes/" + schemes[static_cast<std::size_t>(schemeIndex)].file + ".sch")) {
                    scheme = sf->scheme;
                    teams = sf->team;
                }
        };
        ab::Audio audio;
        const bool sound = !opt.gameDir.empty() && !opt.mute && audio.init(opt.gameDir);
        // Up to four gamepads, opened once at start.
        std::array<SDL_Gamepad*, 4> pads{};
        int padCount = 0;
        if (SDL_JoystickID* ids = SDL_GetGamepads(&padCount)) {
            padCount = std::min(padCount, 4);
            for (int i = 0; i < padCount; ++i) pads[static_cast<std::size_t>(i)] = SDL_OpenGamepad(ids[i]);
            SDL_free(ids);
        } else {
            padCount = 0;
        }
        if (padCount > 0) std::fprintf(stderr, "INFO  Gamepads found count=%d\n", padCount);
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
        const bool haveMenu = opt.menu && spritesPtr->loaded() && spritesPtr->picture("mainmenu") != 0;
        Screen screen = haveMenu ? Screen::MainMenu : Screen::Match;
        int menuItem = 0;
        int listRow = 0;
        if (opt.menuShot == 2) screen = Screen::PlayerList;
        if (opt.menuShot == 3) screen = Screen::LevelSetup;

        ab::RenderSnapshot previous;
        std::array<int, ab::kMaxPlayers> wins{};  // round wins in the current match
        int roundOverSteps = 0;
        bool matchOver = false;
        auto beginMatch = [&]() {
            world.startRound(scheme, true);
            world.setExtras(extras);
            world.setTeamPlay(teamPlay, teams);
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
            if (sound) audio.playMusic(1100 + level);
        } else if (sound) {
            audio.playMusic(1010);  // main menu music
        }

        bool running = true;
        bool paused = false;
        int frame = 0;
        int step = 0;
        Uint64 last = SDL_GetTicksNS();
        double accumulatorMs = 0.0;

        bool farewell = false;  // leaving through the menu: say goodbye first
        while (running) {
            // Scripted key presses for automated checks of the menu screens.
            if (!opt.script.empty() && frame % 10 == 5 && static_cast<std::size_t>(frame / 10) < opt.script.size()) {
                const std::string& k = opt.script[static_cast<std::size_t>(frame / 10)];
                SDL_Event press{};
                press.type = SDL_EVENT_KEY_DOWN;
                press.key.key = k == "up" ? SDLK_UP : k == "down" ? SDLK_DOWN : k == "left" ? SDLK_LEFT : k == "right" ? SDLK_RIGHT
                                : k == "esc" ? SDLK_ESCAPE : SDLK_RETURN;
                SDL_PushEvent(&press);
            }
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
                    if (key == SDLK_ESCAPE) running = false, farewell = true;
                    if (key == SDLK_RETURN) {
                        if (menuItem == 0) {
                            screen = Screen::PlayerList;
                            if (sound) audio.playMusic(1020);  // pre-game screens tune
                        }
                        if (menuItem == 6) running = false, farewell = true;
                        if (sound) audio.playRange(10, 10);
                    }
                } else if (screen == Screen::PlayerList) {
                    Control& c = control[static_cast<std::size_t>(listRow)];
                    if (key == SDLK_UP) listRow = (listRow + ab::kMaxPlayers - 1) % ab::kMaxPlayers;
                    if (key == SDLK_DOWN) listRow = (listRow + 1) % ab::kMaxPlayers;
                    if (key == SDLK_LEFT) c = Control::Off;
                    if (key == SDLK_RIGHT)  // AI -> KEY 0 -> KEY 1 -> OFF -> AI, as observed on the original
                    {
                        // then any connected gamepads before OFF
                        const int k = static_cast<int>(c);
                        if (c == Control::Ai) c = Control::Key0;
                        else if (c == Control::Key0) c = Control::Key1;
                        else if (c == Control::Key1) c = padCount > 0 ? Control::Pad0 : Control::Off;
                        else if (k >= static_cast<int>(Control::Pad0))
                            c = k - static_cast<int>(Control::Pad0) + 1 < padCount ? static_cast<Control>(k + 1) : Control::Off;
                        else c = Control::Ai;
                    }
                    if (key == SDLK_ESCAPE) {
                        screen = Screen::MainMenu;
                        if (sound) audio.playMusic(1010);
                    }
                    if (key == SDLK_RETURN) {
                        int n = 0;
                        for (Control k : control) n += k != Control::Off ? 1 : 0;
                        if (n >= 2) screen = Screen::LevelSetup;
                    }
                } else if (screen == Screen::LevelSetup) {
                    // Level, scheme and match length, as on the original's second pre-game screen.
                    const int d = key == SDLK_RIGHT ? 1 : key == SDLK_LEFT ? -1 : 0;
                    if (key == SDLK_UP) setupRow = (setupRow + 3) % 4;
                    if (key == SDLK_DOWN) setupRow = (setupRow + 1) % 4;
                    if (d != 0 && setupRow == 3) teamPlay = !teamPlay;
                    if (d != 0 && setupRow == 0) level = (level + d + 11) % 11;
                    if (d != 0 && setupRow == 1 && !schemes.empty())
                        schemeIndex = (schemeIndex + d + static_cast<int>(schemes.size())) % static_cast<int>(schemes.size());
                    if (d != 0 && setupRow == 2) winsNeeded = std::clamp(winsNeeded + d, 1, 9);
                    if (key == SDLK_ESCAPE) screen = Screen::PlayerList;
                    if (key == SDLK_RETURN) {
                        applySettings();
                        wins = {};
                        matchOver = false;
                        screen = Screen::Match;
                        beginMatch();
                        if (sound) audio.playMusic(1100 + level);
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
                        if (c >= Control::Pad0)
                            input[static_cast<std::size_t>(i)] = gamepadInput(pads[static_cast<std::size_t>(static_cast<int>(c) - static_cast<int>(Control::Pad0))]);
                    }
                    previous.capture(world);
                    world.tick(kStepMs, input);
                    playEvents(world, audio);
                    accumulatorMs -= kStepMs;
                    ++step;
                    // A decided round stays on screen for three seconds, then the next one starts.
                    if (world.roundOver() && roundOverSteps == 0) {
                        const int win = world.teamPlay() ? world.winningTeam() : world.winner();
                        if (win >= 0) {
                            const int total = ++wins[static_cast<std::size_t>(win)];
                            std::fprintf(stderr, "INFO  Round over winner=%d kills=%d wins=%d\n", win, world.player(win).kills, total);
                            if (total >= winsNeeded) {
                                matchOver = true;
                                std::fprintf(stderr, "INFO  Match over winner=%d\n", win);
                            }
                        } else {
                            std::fprintf(stderr, "INFO  Round over draw\n");
                        }
                    }
                    if (world.roundOver() && roundOverSteps == 20 && sound) {
                        // The result screen comes up: the original plays its end-of-round tune here
                        // (sound 1130, draw.rss, for every result), then a random voice line: the
                        // 1700 series for a draw, the 2000 series when somebody won (Match_Run 0x42A6D8,
                        // 0x42A71C, 0x42ACB9).
                        audio.playMusic(1130);
                        if (world.teamPlay() ? world.winningTeam() < 0 : world.winner() < 0) audio.playRange(1700, 1999);
                        else audio.playRange(2000, 2299);
                    }
                    if (world.roundOver() && ++roundOverSteps > 100) {
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
                        if (sound) audio.playMusic(1100 + level);
                    }
                }
            }
            audio.update();

            if (screen == Screen::Match) {
                const float alpha = paused ? 1.0f : static_cast<float>(accumulatorMs / kStepMs);
                renderer.draw(world, previous, alpha, w, h, spritesPtr.get(), &wins);
                if (world.roundOver() && spritesPtr->loaded() && roundOverSteps > 20) {
                    // After a second on the frozen field: the original's result pictures.
                    const int win = world.teamPlay() ? world.winningTeam() : world.winner();
                    renderer.begin(w, h);
                    if (win < 0) {
                        renderer.image(spritesPtr->picture("draw"));
                    } else if (world.teamPlay()) {
                        // Team result: the original's team pictures (white team 0, red team 1).
                        renderer.image(spritesPtr->picture("team" + std::to_string(win)));
                        const std::string line = std::string(matchOver ? "TEAM " : "Team ") + std::to_string(win + 1) +
                                                 (matchOver ? " WINS THE MATCH!" : " wins the round   score: " + std::to_string(wins[static_cast<std::size_t>(win)]));
                        renderer.text(*spritesPtr, line, 211, 441, 0, 0, 0);
                        renderer.text(*spritesPtr, line, 210, 440, 1.0f, 0.95f, 0.3f);
                    } else if (matchOver) {
                        renderer.image(spritesPtr->picture("victory" + std::to_string(win)));
                        const std::string line = "PLAYER " + std::to_string(win + 1) + " WINS THE MATCH!";  // message 36
                        renderer.text(*spritesPtr, line, 211, 441, 0, 0, 0);  // below the artwork's own title
                        renderer.text(*spritesPtr, line, 210, 440, 1.0f, 0.95f, 0.3f);
                    } else {
                        renderer.image(spritesPtr->picture("results"));
                        renderer.text(*spritesPtr, "Winner was:", 151, 141, 0, 0, 0);
                        renderer.text(*spritesPtr, "Winner was:", 150, 140, 1, 1, 1);
                        int row = 0;
                        for (int i = 0; i < ab::kMaxPlayers; ++i) {
                            if (!world.player(i).present) continue;
                            const std::string line = std::string(i == win ? "> " : "  ") + "Player " + std::to_string(i + 1) +
                                                     "   wins " + std::to_string(wins[static_cast<std::size_t>(i)]) + "   kills " +
                                                     std::to_string(world.player(i).kills);
                            const float y = 210.0f + 20.0f * static_cast<float>(row++);
                            renderer.text(*spritesPtr, line, 151, y + 1, 0, 0, 0);
                            renderer.text(*spritesPtr, line, 150, y, i == win ? 1.0f : 0.8f, i == win ? 0.95f : 0.8f, i == win ? 0.3f : 0.8f);
                        }
                    }
                    renderer.end();
                }
            } else if (screen == Screen::LevelSetup) {
                renderer.begin(w, h);
                renderer.image(spritesPtr->picture("glue1"));
                const std::string lines[4] = {
                    kLevelName[level],
                    schemes.empty() ? std::string("(built-in arena)") : "Scheme: " + schemes[static_cast<std::size_t>(schemeIndex)].title,
                    std::to_string(winsNeeded) + " Wins to win match",
                    std::string("Team play: ") + (teamPlay ? "ON (teams from the scheme)" : "OFF")};
                for (int r = 0; r < 4; ++r) {
                    const float y = 180.0f + 24.0f * static_cast<float>(r);
                    renderer.text(*spritesPtr, lines[r], 71, y + 1, 0, 0, 0);
                    renderer.text(*spritesPtr, lines[r], 70, y, 1, 1, 1);
                }
                renderer.sprite(*spritesPtr, "cursor1", frame / 8, -1, 56.0f, 195.0f + 24.0f * static_cast<float>(setupRow));
                renderer.text(*spritesPtr, "Up/Down: select   Left/Right: change   Enter: start", 60, 440, 0.4f, 1.0f, 1.0f);
                renderer.end();
            } else if (screen == Screen::MainMenu) {
                renderer.begin(w, h);
                renderer.image(spritesPtr->picture("mainmenu"));
                // The picture carries the item texts; the game draws only the cursor
                // (original value 700: first item at x 332, y 140, 38 px apart).
                renderer.sprite(*spritesPtr, "cursor1", frame / 8, -1, 332.0f, 140.0f + 38.0f * static_cast<float>(menuItem));
                renderer.end();
            } else {
                renderer.begin(w, h);
                renderer.image(spritesPtr->picture("glue0"));
                // Original layout: heading at (40,140), list from (70,170) every 24 px (values 705/710).
                renderer.text(*spritesPtr, "Available players:", 41, 141, 0, 0, 0);
                renderer.text(*spritesPtr, "Available players:", 40, 140, 1, 1, 1);
                static const float colour[ab::kMaxPlayers][3] = {
                    {0.95f, 0.95f, 0.95f}, {0.55f, 0.55f, 0.55f}, {0.90f, 0.15f, 0.15f}, {0.20f, 0.35f, 0.95f}, {0.15f, 0.80f, 0.20f},
                    {0.95f, 0.90f, 0.15f}, {0.15f, 0.85f, 0.85f}, {0.90f, 0.20f, 0.90f}, {0.95f, 0.55f, 0.10f}, {0.55f, 0.20f, 0.90f}};
                for (int i = 0; i < ab::kMaxPlayers; ++i) {
                    const std::string line = "Player " + std::to_string(i + 1) + ": " + controlName(control[static_cast<std::size_t>(i)]);
                    const float y = 170.0f + 24.0f * static_cast<float>(i);
                    renderer.text(*spritesPtr, line, 71, y + 1, 0, 0, 0);
                    renderer.text(*spritesPtr, line, 70, y, colour[i][0], colour[i][1], colour[i][2]);
                }
                renderer.sprite(*spritesPtr, "cursor1", frame / 8, -1, 56.0f, 185.0f + 24.0f * static_cast<float>(listRow));
                renderer.text(*spritesPtr, "Up/Down: select   Left: off   Right: change   Enter: start", 60, 440, 0.4f, 1.0f, 1.0f);
                renderer.end();
            }

            ++frame;
            if (opt.resultShot && screen == Screen::Match && world.roundOver() && roundOverSteps == 50) {
                if (!opt.screenshot.empty()) writePpm(opt.screenshot, w, h);
                running = false;
            }
            if (opt.frames > 0 && frame >= opt.frames) {
                if (!opt.screenshot.empty()) writePpm(opt.screenshot, w, h);
                running = false;
            }
            SDL_GL_SwapWindow(window);
        }
        if (farewell && sound && opt.frames <= 0) {
            // As the original's quit routine (0x412987): the music stops, one of the
            // "leaving the program" lines plays (2600 series) and the program waits 4 s.
            audio.stopMusic();
            audio.playRange(2600, 2699);
            const Uint64 until = SDL_GetTicks() + 4000;
            bool waiting = true;
            while (waiting && SDL_GetTicks() < until) {
                SDL_Event e;
                while (SDL_PollEvent(&e))
                    if (e.type == SDL_EVENT_QUIT || (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat)) waiting = false;
                audio.update();
                SDL_Delay(10);
            }
        }
        std::fprintf(stderr, "INFO  Exiting frames=%d steps=%d alive=%d bombs=%d\n", frame, step, world.alivePlayers(),
                     world.activeBombs());
        for (SDL_Gamepad* pad : pads)
            if (pad != nullptr) SDL_CloseGamepad(pad);
    }
    SDL_GL_DestroyContext(gl);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
