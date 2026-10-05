// SDL3 + OpenGL front end. All platform code lives here and in rendering/;
// the gameplay core (src/game) knows nothing about SDL or OpenGL.
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include "rendering/gl.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "audio/audio.hpp"
#include "game/ai.hpp"
#include "game/roulette.hpp"
#include "game/world.hpp"
#include "rendering/renderer.hpp"
#include "resources/asset_import.hpp"
#include "resources/campaign_file.hpp"
#include "resources/help_file.hpp"
#include "resources/scheme_file.hpp"
#include "resources/settings.hpp"

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
    bool rouletteShot = false;  // automated: capture the roulette once it has stopped, then exit
    bool roulette = false;    // the "goldman" roulette between rounds (original option goldman)
    std::string campaign;     // --campaign NAME: start that campaign file (data/res/NAME.cam) at once
    int introShot = 0;        // automated: start on intro screen N (1-3)
    bool noIntro = false;     // --no-intro: go straight to the main menu
    bool levelSet = false;    // --level, --wins, --scheme given: they win over the saved settings
    bool winsSet = false;
    bool schemeSet = false;
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
                      "  --campaign NAME      play a campaign file of the game data (simple, ghosts, crouton)\n"
                      "  --no-intro           skip the logo and title screens\n"
                      "  --roulette           the round winner spins for a prize before the next round\n"
                      "  --seed N             random seed\n"
                      "  --mute               no sound\n"
                      "  --native             640x480 window\n"
                      "  --shapes             plain shapes instead of the game's graphics\n"
                      "Testing: --demo --frames N --screenshot FILE --result-shot --menu-shot N --script KEYS");
            std::exit(0);
        }
        else if (a == "--game-dir") o.gameDir = next();
        else if (a == "--scheme") o.scheme = next(), o.schemeSet = true;
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
        else if (a == "--level") o.level = std::clamp(std::atoi(next().c_str()), 0, 10), o.levelSet = true;
        else if (a == "--wins") o.wins = std::max(1, std::atoi(next().c_str())), o.winsSet = true;
        else if (a == "--native") o.native = true;
        else if (a == "--roulette") o.roulette = true;
        else if (a == "--no-intro") o.noIntro = true;
        else if (a == "--intro-shot") o.introShot = std::atoi(next().c_str());
        else if (a == "--campaign") o.campaign = next();
        else if (a == "--roulette-shot") o.roulette = o.rouletteShot = true;
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

constexpr int kOptionRows = 10;
enum class Screen { MainMenu, PlayerList, LevelSetup, Match, Roulette, Options, Help, HelpList, Message, CampaignList, Intro };

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

    // Saved settings (the original's options.ini keys). Automated runs ignore the file so
    // that they stay reproducible; command-line choices win over it.
    ab::Settings cfg;
    std::string settingsPath;
    if (char* pref = SDL_GetPrefPath("atomic-bomberman-modern", "atomic")) {
        settingsPath = std::string(pref) + "options.ini";
        SDL_free(pref);
    }
    const bool useSettings = opt.frames <= 0 && !opt.demo && opt.script.empty() && !settingsPath.empty();
    if (useSettings && cfg.load(settingsPath)) std::fprintf(stderr, "INFO  Settings: %s\n", settingsPath.c_str());
    bool randomLevel = !opt.levelSet && cfg.level < 0;
    if (!opt.levelSet) opt.level = std::max(0, cfg.level);
    if (!opt.winsSet) opt.wins = cfg.winsNeeded;
    if (!opt.schemeSet && useSettings) {
        opt.scheme = cfg.scheme;
        for (char& ch : opt.scheme) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    if (opt.roulette) cfg.goldman = true;

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
        bool teamPlay = cfg.teamPlay;
        std::array<int, ab::kMaxPlayers> teams{};
        int teamsFromScheme = -1;
        if (!opt.gameDir.empty())
            if (auto sf = ab::loadSchemeFile(opt.gameDir + "/data/schemes/" + opt.scheme + ".sch")) teams = sf->team;
        teamsFromScheme = schemeIndex;

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
                    // The scheme gives the teams; choices made with T on the player list stay
                    // until another scheme is chosen.
                    if (teamsFromScheme != schemeIndex) teams = sf->team;
                    teamsFromScheme = schemeIndex;
                }
        };
        auto saveSettings = [&]() {
            cfg.level = randomLevel ? -1 : level;
            cfg.winsNeeded = winsNeeded;
            cfg.teamPlay = teamPlay;
            if (!schemes.empty()) {
                cfg.scheme = schemes[static_cast<std::size_t>(schemeIndex)].file;
                for (char& ch : cfg.scheme) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
            }
            if (useSettings && !cfg.save(settingsPath)) std::fprintf(stderr, "WARN  Could not write %s\n", settingsPath.c_str());
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
        int optionRow = 0;
        // Sample arena on the level screen (original 0x406AA3): 5 x 5 cells at (400,100)
        // (value 730). Each cell holds the level a tile is taken from, or -1 for none;
        // rebuilt whenever the level choice changes.
        std::array<std::array<int, 5>, 5> previewBrick{};
        int previewField = 0;
        int previewFor = -99;
        // Help viewer (About Bomberman shows credits.bm; Online Manual lists every *.bm).
        std::vector<ab::HelpLine> helpLines;
        int helpTop = 0;
        Screen helpReturn = Screen::MainMenu;
        std::vector<std::string> helpFiles;
        int helpRow = 0;
        auto openHelp = [&](const std::string& file, Screen back) {
            helpLines.clear();
            if (auto page = ab::loadHelpFile(opt.gameDir + "/" + file)) helpLines = std::move(*page);
            else helpLines.push_back({{false, "Help file not found: " + file}});
            helpTop = 0;
            helpReturn = back;
            screen = Screen::Help;
        };
        int optionsGlue = 2;
        int listRow = 0;
        if (opt.menuShot == 2) screen = Screen::PlayerList;
        if (opt.menuShot == 3) screen = Screen::LevelSetup;
        if (opt.menuShot == 4) screen = Screen::Options;
        if (opt.menuShot == 5) openHelp("credits.bm", Screen::MainMenu);
        if (opt.menuShot == 6) openHelp("manual.bm", Screen::MainMenu);

        ab::RenderSnapshot previous;
        std::array<int, ab::kMaxPlayers> wins{};  // round wins in the current match
        int roundOverSteps = 0;
        bool matchOver = false;
        // The roulette between rounds, and what it gave the last round's winner.
        bool& goldman = cfg.goldman;
        std::unique_ptr<ab::Roulette> roulette;
        int rouletteFrames = 0;
        double rouletteMs = 0.0;
        int prizeWinner = -1;  // winner of the last match: player, or team in team play
        int prizeType = -1;
        // Original match setup (0x410FB8): with the option on and a match winner
        // on record, the roulette comes first.
        std::uint32_t rouletteCount = 0;
        auto startRoulette = [&]() {
            prizeType = -1;
            roulette = std::make_unique<ab::Roulette>(values, opt.seed * 7919u + static_cast<std::uint32_t>(SDL_GetTicks()) * (opt.frames > 0 ? 0u : 1u) + 17u * ++rouletteCount);
            rouletteFrames = 0;
            rouletteMs = 0.0;
        };
        // Campaign mode (hidden in the original: C five times on the player list).
        bool campaignMode = false;
        std::vector<ab::CampaignStage> stages;
        int stageIndex = -1;
        std::array<int, ab::kMaxPlayers> campaignScore{};  // points from earlier stages
        int campaignKeyCount = 0;
        std::vector<std::string> campaignFiles;
        int campaignRow = 0;
        std::array<int, ab::kMaxPlayers> matchKills{};  // kills over the rounds of the match
        int matchWinner = -1;                           // player or team once the match is decided
        std::array<ab::Cell, ab::kMaxPlayers> startCells = scheme.start;
        ab::Rng appRng(opt.seed * 2654435761u + 99u);
        // Level music, unless switched off in the settings (original disable_game_music).
        auto playLevelMusic = [&]() {
            if (!sound) return;
            if (cfg.disableGameMusic) audio.stopMusic();
            else audio.playMusic(1100 + level);
        };
        // What the original resets once per match (0x421793): scores, kills and, with
        // "random start", the start positions: 200 swaps of two of the ten.
        auto newMatch = [&]() {
            wins = {};
            matchKills = {};
            matchOver = false;
            matchWinner = -1;
            if (randomLevel) {
                // "Random Each Game": values 1150-1160 say which levels may come up.
                for (int tries = 0; tries < 100; ++tries) {
                    level = appRng.below(11);
                    if (values.get(1150 + level) != 0) break;
                }
            }
            applySettings();
            startCells = scheme.start;
            if (cfg.randomStart)
                for (int n = 0; n < 200; ++n) {
                    const auto a = static_cast<std::size_t>(appRng.below(ab::kMaxPlayers));
                    const auto b = static_cast<std::size_t>(appRng.below(ab::kMaxPlayers));
                    std::swap(startCells[a], startCells[b]);
                }
        };
        auto beginMatch = [&]() {
            // Settings the core reads as tuning values.
            world.setValue(ab::vid::kEnclosementDepth, cfg.enclosementDepth);
            world.setValue(ab::vid::kWallsDetonateBombs, cfg.stompedBombsDetonate ? 1 : 0);
            world.setValue(ab::vid::kDiseasesDestroyable, cfg.diseasesDestroyable ? 1 : 0);
            world.setValue(ab::vid::kRoundSeconds, cfg.playTime);
            world.setWinByKills(cfg.winByKills);
            ab::Scheme placed = scheme;
            placed.start = startCells;
            world.setCampaign(campaignMode);
            world.startRound(placed, true);
            if (cfg.playTime >= ab::Settings::kInfiniteTime) world.setRoundSeconds(-1);
            world.setExtras(extras, cfg.conveyorSpeed);
            world.setTeamPlay(teamPlay, teams);
            int n = 0;
            for (int i = 0; i < ab::kMaxPlayers; ++i)
                if (control[static_cast<std::size_t>(i)] != Control::Off) {
                    world.addPlayer(i);
                    ++n;
                }
            if (campaignMode) {
                for (int i = 0; i < ab::kMaxPlayers; ++i)
                    if (world.player(i).present) world.setHuman(i, control[static_cast<std::size_t>(i)] != Control::Ai);
                const ab::CampaignStage& st = stages[static_cast<std::size_t>(stageIndex)];
                world.spawnAliens(ab::AlienType::Ghost, st.ghosts, st.ghostSpeed);
                world.spawnAliens(ab::AlienType::Rover, st.rovers, st.roverSpeed);
            }
            // The roulette prize goes to the winner of the last match (to every member of the
            // winning team), in every round of this one.
            for (int i = 0; i < ab::kMaxPlayers && prizeType >= 0; ++i)
                if (world.player(i).present && (teamPlay ? world.player(i).team : i) == prizeWinner) world.grantPrize(i, prizeType);
            previous.capture(world);
            roundOverSteps = 0;
            std::fprintf(stderr, "INFO  Round started players=%d\n", n);
        };
        // After the roulette the match setup goes on: the player list, or straight into the match.
        auto leaveRoulette = [&]() {
            if (haveMenu) {
                screen = Screen::PlayerList;
                if (sound) audio.playMusic(1020);
            } else {
                screen = Screen::Match;
                beginMatch();
                playLevelMusic();
            }
        };
        int introStep = 0;
        Uint64 introStart = 0;
        bool running = true;
        // A notice that waits for a key (the original's message boxes).
        std::vector<std::string> messageLines;
        std::function<void()> messageDone;
        auto showMessage = [&](std::vector<std::string> lines, std::function<void()> done) {
            messageLines = std::move(lines);
            messageDone = std::move(done);
            screen = Screen::Message;
        };
        // Next campaign stage (original 0x40133F and 0x40151B): level and scheme from the
        // campaign file, the computer players replaced by the stage's number of them in
        // random free seats, then the notice and the stage itself.
        std::function<void()> startCampaignStage = [&]() {
            ++stageIndex;
            if (stageIndex >= static_cast<int>(stages.size())) {
                showMessage({"Congratulations!", "You made it through the whole campaign!"}, [&]() {  // messages 1220, 1225
                    campaignMode = false;
                    if (haveMenu) {
                        screen = Screen::MainMenu;
                        if (sound) audio.playMusic(1010);
                    } else {
                        running = false;
                    }
                });
                return;
            }
            const ab::CampaignStage& st = stages[static_cast<std::size_t>(stageIndex)];
            level = std::clamp(st.level, 0, 10);
            for (std::size_t i = 0; i < schemes.size(); ++i)
                if (schemes[i].file == st.scheme) schemeIndex = static_cast<int>(i);
            applySettings();
            startCells = scheme.start;
            if (cfg.randomStart)
                for (int n = 0; n < 200; ++n)
                    std::swap(startCells[static_cast<std::size_t>(appRng.below(ab::kMaxPlayers))],
                              startCells[static_cast<std::size_t>(appRng.below(ab::kMaxPlayers))]);
            for (Control& c : control)
                if (c == Control::Ai) c = Control::Off;
            for (int n = 0; n < st.computerPlayers; ++n)
                for (int tries = 0; tries < 100; ++tries) {
                    Control& c = control[static_cast<std::size_t>(appRng.below(ab::kMaxPlayers))];
                    if (c != Control::Off) continue;
                    c = Control::Ai;
                    break;
                }
            std::fprintf(stderr, "INFO  Campaign stage=%d name=\"%s\"\n", stageIndex, st.name.c_str());
            showMessage({"Prepare to begin Campaign!", "(" + st.name + ")"}, [&]() {  // messages 1230, 1235
                screen = Screen::Match;
                beginMatch();
                playLevelMusic();
            });
        };
        auto startCampaign = [&](const std::string& file) {
            auto loaded = ab::loadCampaignFile(opt.gameDir + "/data/res/" + file);
            if (!loaded || loaded->empty()) return false;
            stages = std::move(*loaded);
            campaignMode = true;
            stageIndex = -1;
            campaignScore = {};
            teamPlay = false;
            return true;
        };
        if (!opt.campaign.empty() && startCampaign(opt.campaign + ".cam")) {
            startCampaignStage();
        } else if (screen == Screen::Match) {
            newMatch();
            beginMatch();
            playLevelMusic();
        } else if (screen == Screen::MainMenu && ((!opt.noIntro && opt.frames <= 0 && opt.script.empty()) || opt.introShot > 0)) {
            // The original's intro (0x42B060): the title tune, the two logo pictures, then
            // an "Atomic Bomberman!" voice line with the title picture.
            screen = Screen::Intro;
            introStep = std::clamp(opt.introShot - 1, 0, 2);
            introStart = SDL_GetTicks();
            if (sound) audio.playMusic(1000);
            if (sound && introStep == 2) audio.playRange(2800, 2899);
        } else if (sound) {
            audio.playMusic(1010);  // main menu music
        }
        // Each intro picture stays until Enter, Space or Esc, or for value 12 (7) seconds.
        auto advanceIntro = [&]() {
            ++introStep;
            introStart = SDL_GetTicks();
            if (introStep == 2 && sound) audio.playRange(2800, 2899);
            if (introStep > 2) {
                screen = Screen::MainMenu;
                if (sound) audio.playMusic(1010);
            }
        };

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
                            if (sound) audio.playMusic(1020);  // pre-game screens tune
                            if (goldman && prizeWinner >= 0) {
                                startRoulette();
                                screen = Screen::Roulette;
                            } else {
                                screen = Screen::PlayerList;
                            }
                        }
                        if (menuItem == 3) {
                            screen = Screen::Options;
                            optionRow = 0;
                            // The original shows a random "glue" picture behind it (0x4148E5, value 16).
                            optionsGlue = appRng.below(std::max(1, values.get(16)));
                        }
                        if (menuItem == 4) openHelp("credits.bm", Screen::MainMenu);  // original 0x42BDE7
                        if (menuItem == 5) {
                            // Message 610: every *.BM file of the game folder.
                            helpFiles.clear();
                            std::error_code ec;
                            for (const auto& entry : std::filesystem::directory_iterator(opt.gameDir, ec)) {
                                std::string ext = entry.path().extension().string();
                                for (char& ch : ext) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                                if (ext == ".bm") helpFiles.push_back(entry.path().filename().string());
                            }
                            std::sort(helpFiles.begin(), helpFiles.end());
                            helpRow = 0;
                            screen = Screen::HelpList;
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
                    if (key == SDLK_C && ++campaignKeyCount == 5) {
                        // The original's hidden campaign chooser (0x41186D): message 1250 and the *.cam files.
                        campaignKeyCount = 0;
                        campaignFiles.clear();
                        std::error_code ec;
                        for (const auto& entry : std::filesystem::directory_iterator(opt.gameDir + "/data/res", ec)) {
                            std::string ext = entry.path().extension().string();
                            for (char& ch : ext) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                            if (ext == ".cam") campaignFiles.push_back(entry.path().filename().string());
                        }
                        std::sort(campaignFiles.begin(), campaignFiles.end());
                        campaignRow = 0;
                        screen = Screen::CampaignList;
                    }
                    if (key == SDLK_T) {
                        // Original 0x4119DD: the selected player changes team; a seat that is off refuses.
                        if (c != Control::Off) {
                            int& t = teams[static_cast<std::size_t>(listRow)];
                            t = t == 0 ? 1 : 0;
                            teamsFromScheme = schemeIndex;
                        } else if (sound) {
                            audio.playRange(40, 49);
                        }
                    }
                    if (key == SDLK_0) c = Control::Off;  // original key '0'
                    if (key == SDLK_RETURN) {
                        int n = 0, humans = 0;
                        std::array<int, 2> perTeam{};
                        bool shared = false;
                        for (int i = 0; i < ab::kMaxPlayers; ++i) {
                            const Control k = control[static_cast<std::size_t>(i)];
                            if (k == Control::Off) continue;
                            ++n;
                            humans += k != Control::Ai ? 1 : 0;
                            ++perTeam[static_cast<std::size_t>(teams[static_cast<std::size_t>(i)] & 1)];
                            for (int j = 0; j < i; ++j)
                                if (k != Control::Ai && control[static_cast<std::size_t>(j)] == k) shared = true;
                        }
                        // The original's three checks (0x411BA9-0x411C62), each a "Problem!!" notice.
                        auto back = [&]() { screen = Screen::PlayerList; };
                        if (shared) {
                            showMessage({"Problem!!", "More than one player selected to an input device!"}, back);  // message 45
                        } else if (campaignMode) {
                            // No level screen and no two-player minimum in a campaign.
                            if (humans >= 1) startCampaignStage();
                        } else if (n < 2) {
                            showMessage({"Problem!!", "Must have at least two players selected!"}, back);  // message 46
                        } else if (teamPlay && (perTeam[0] == 0 || perTeam[1] == 0)) {
                            showMessage({"Problem!!", "Must have at least one player on each team!"}, back);  // message 48
                        } else {
                            screen = Screen::LevelSetup;
                        }
                    }
                } else if (screen == Screen::Roulette && roulette) {
                    if (sound) audio.playRange(20, 20);
                    if (key == SDLK_RETURN || key == SDLK_SPACE) {
                        if (roulette->press()) {
                            prizeType = roulette->prize();
                            std::fprintf(stderr, "INFO  Roulette prize winner=%d type=%d\n", prizeWinner, prizeType);
                            roulette.reset();
                            leaveRoulette();
                        }
                    } else if (key == SDLK_ESCAPE) {
                        // Leaves the match, as on the original.
                        roulette.reset();
                        prizeWinner = -1;
                        wins = {};
                        if (haveMenu) {
                            screen = Screen::MainMenu;
                            if (sound) audio.playMusic(1010);
                        } else {
                            running = false;
                        }
                    }
                } else if (screen == Screen::Intro) {
                    if (sound) audio.playRange(20, 20);
                    if (key == SDLK_RETURN || key == SDLK_SPACE || key == SDLK_ESCAPE) {
                        if (sound) audio.playRange(10, 10);
                        advanceIntro();
                    }
                } else if (screen == Screen::Message) {
                    if (key == SDLK_RETURN || key == SDLK_SPACE || key == SDLK_ESCAPE) {
                        const std::function<void()> done = std::move(messageDone);
                        messageDone = nullptr;
                        if (done) done();
                    }
                } else if (screen == Screen::CampaignList) {
                    const int n = static_cast<int>(campaignFiles.size());
                    if (key == SDLK_UP && n > 0) campaignRow = (campaignRow + n - 1) % n;
                    if (key == SDLK_DOWN && n > 0) campaignRow = (campaignRow + 1) % n;
                    if (key == SDLK_ESCAPE) screen = Screen::PlayerList;
                    if (key == SDLK_RETURN && n > 0 && startCampaign(campaignFiles[static_cast<std::size_t>(campaignRow)])) {
                        for (Control& c : control)
                            if (c == Control::Ai) c = Control::Off;
                        showMessage({"NOTE!", "Campaign Mode Activated!"}, [&]() { screen = Screen::PlayerList; });  // messages 95, 1210
                    }
                } else if (screen == Screen::Help) {
                    // Keys of the original's viewer: a line or a page at a time; Enter or Esc closes.
                    const int page = std::max(2, 344 / (spritesPtr->font().height + 2));
                    const int lastTop = std::max(0, static_cast<int>(helpLines.size()) - page);
                    if (key == SDLK_UP) helpTop = std::max(0, helpTop - 1);
                    if (key == SDLK_DOWN) helpTop = std::min(lastTop, helpTop + 1);
                    if (key == SDLK_PAGEUP || key == SDLK_LEFT) helpTop = std::max(0, helpTop - (page - 1));
                    if (key == SDLK_PAGEDOWN || key == SDLK_RIGHT || key == SDLK_SPACE) helpTop = std::min(lastTop, helpTop + page - 1);
                    if (key == SDLK_HOME) helpTop = 0;
                    if (key == SDLK_END) helpTop = lastTop;
                    if (key == SDLK_ESCAPE || key == SDLK_RETURN) screen = helpReturn;
                } else if (screen == Screen::HelpList) {
                    const int n = static_cast<int>(helpFiles.size());
                    if (key == SDLK_UP && n > 0) helpRow = (helpRow + n - 1) % n;
                    if (key == SDLK_DOWN && n > 0) helpRow = (helpRow + 1) % n;
                    if (key == SDLK_RETURN && n > 0) openHelp(helpFiles[static_cast<std::size_t>(helpRow)], Screen::HelpList);
                    if (key == SDLK_ESCAPE) screen = Screen::MainMenu;
                } else if (screen == Screen::Options) {
                    // The original's settings screen (0x4080DC), without its network, keyboard-layout,
                    // memory and audio-adjustment rows.
                    const int d = key == SDLK_RIGHT || key == SDLK_RETURN || key == SDLK_SPACE ? 1 : key == SDLK_LEFT ? -1 : 0;
                    if (key == SDLK_UP) optionRow = (optionRow + kOptionRows - 1) % kOptionRows;
                    if (key == SDLK_DOWN) optionRow = (optionRow + 1) % kOptionRows;
                    if (d != 0) {
                        if (sound) audio.playRange(20, 20);
                        switch (optionRow) {
                            case 0: teamPlay = !teamPlay; break;
                            case 1: cfg.randomStart = !cfg.randomStart; break;
                            case 2: cfg.conveyorSpeed = (cfg.conveyorSpeed + d + 3) % 3; break;
                            case 3: cfg.stompedBombsDetonate = !cfg.stompedBombsDetonate; break;
                            case 4: cfg.winByKills = !cfg.winByKills; break;
                            case 5: cfg.goldman = !cfg.goldman; break;
                            case 6: cfg.enclosementDepth = (cfg.enclosementDepth + d + 4) % 4; break;
                            case 7: cfg.playTime = ab::Settings::nextPlayTime(cfg.playTime, d); break;
                            case 8: cfg.diseasesDestroyable = !cfg.diseasesDestroyable; break;
                            default: cfg.disableGameMusic = !cfg.disableGameMusic; break;
                        }
                    }
                    if (key == SDLK_ESCAPE) {
                        saveSettings();
                        screen = Screen::MainMenu;
                    }
                } else if (screen == Screen::LevelSetup) {
                    // Level, scheme and match length, as on the original's second pre-game screen.
                    const int d = key == SDLK_RIGHT ? 1 : key == SDLK_LEFT ? -1 : 0;
                    if (key == SDLK_UP) setupRow = (setupRow + 3) % 4;
                    if (key == SDLK_DOWN) setupRow = (setupRow + 1) % 4;
                    if (d != 0 && setupRow == 3) teamPlay = !teamPlay;
                    if (d != 0 && setupRow == 0) {
                        // Random Each Game, then the eleven levels.
                        const int choice = ((randomLevel ? -1 : level) + d + 1 + 12) % 12 - 1;
                        randomLevel = choice < 0;
                        if (!randomLevel) level = choice;
                    }
                    if (d != 0 && setupRow == 1 && !schemes.empty())
                        schemeIndex = (schemeIndex + d + static_cast<int>(schemes.size())) % static_cast<int>(schemes.size());
                    if (d != 0 && setupRow == 2) winsNeeded = std::clamp(winsNeeded + d, 1, 9);
                    if (key == SDLK_ESCAPE) screen = Screen::PlayerList;
                    if (key == SDLK_RETURN) {
                        newMatch();
                        saveSettings();
                        screen = Screen::Match;
                        beginMatch();
                        playLevelMusic();
                    }
                } else {
                    if (key == SDLK_ESCAPE) {
                        campaignMode = false;
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
                    if (campaignMode && world.roundOver()) {
                        // Stage result (original 0x42A63B): a notice on failure, then the next
                        // stage, or the same one again if no human player was left.
                        for (int i = 0; i < ab::kMaxPlayers; ++i)
                            if (world.player(i).present) campaignScore[static_cast<std::size_t>(i)] += world.player(i).score;
                        std::fprintf(stderr, "INFO  Campaign stage over result=%d retry=%d\n", world.campaignResult(), world.campaignRetry() ? 1 : 0);
                        if (world.campaignRetry()) --stageIndex;
                        if (world.campaignResult() == 2)
                            showMessage({"Oh Well!", "Campaign unsuccessful!"}, [&]() { startCampaignStage(); });  // messages 1240, 1245
                        else
                            startCampaignStage();
                        break;
                    }
                    // A decided round stays on screen for three seconds, then the next one starts.
                    if (world.roundOver() && roundOverSteps == 0) {
                        const int win = world.teamPlay() ? world.winningTeam() : world.winner();
                        if (win >= 0) {
                            const int total = ++wins[static_cast<std::size_t>(win)];
                            std::fprintf(stderr, "INFO  Round over winner=%d kills=%d wins=%d\n", win, world.player(win).kills, total);
                        } else {
                            std::fprintf(stderr, "INFO  Round over draw\n");
                        }
                        for (int i = 0; i < ab::kMaxPlayers; ++i)
                            if (world.player(i).present) matchKills[static_cast<std::size_t>(i)] += world.player(i).kills;
                        // Match winner, as the original's results code (0x42AB04): by wins, or with
                        // "win by kills" (not in team play) the single player with the most kills
                        // once that reaches the target.
                        if (cfg.winByKills && !world.teamPlay()) {
                            int best = -1000, holders = 0, who = -1;
                            for (int i = 0; i < ab::kMaxPlayers; ++i) {
                                if (!world.player(i).present) continue;
                                const int k = matchKills[static_cast<std::size_t>(i)];
                                if (k > best) best = k, holders = 1, who = i;
                                else if (k == best) ++holders;
                            }
                            if (best >= winsNeeded && holders == 1) matchWinner = who;
                        } else if (win >= 0 && wins[static_cast<std::size_t>(win)] >= winsNeeded) {
                            matchWinner = win;
                        }
                        if (matchWinner >= 0) {
                            matchOver = true;
                            std::fprintf(stderr, "INFO  Match over winner=%d\n", matchWinner);
                        }
                    }
                    if (world.roundOver() && roundOverSteps == 20 && sound) {
                        // The result screen comes up: the original plays its end-of-round tune here
                        // (sound 1130, draw.rss, for every result), then a random voice line: the
                        // 1700 series for a draw, the 2000 series when the match has a winner
                        // (Match_Run 0x42A6D8, 0x42A71C, 0x42ACB9).
                        audio.playMusic(1130);
                        if (world.teamPlay() ? world.winningTeam() < 0 : world.winner() < 0) audio.playRange(1700, 1999);
                        else if (matchOver) audio.playRange(2000, 2299);
                    }
                    if (world.roundOver() && ++roundOverSteps > 100) {
                        if (matchOver) {
                            // The match winner is remembered for the roulette (0x42AC58).
                            prizeWinner = goldman ? matchWinner : -1;
                            newMatch();
                            if (haveMenu) {
                                screen = Screen::MainMenu;
                                if (sound) audio.playMusic(1010);
                                break;
                            }
                            if (goldman && prizeWinner >= 0 && spritesPtr->loaded()) {
                                startRoulette();
                                screen = Screen::Roulette;
                                break;
                            }
                        }
                        beginMatch();
                        playLevelMusic();
                    }
                }
            }
            if (screen == Screen::Roulette && roulette) {
                // The original advances the wheel once per drawn frame, with no frame limit;
                // 25 steps a second is this implementation's choice.
                rouletteMs += frameMs;
                bool leave = false;
                while (rouletteMs >= 40.0) {
                    rouletteMs -= 40.0;
                    ++rouletteFrames;
                    const bool wasStopped = roulette->state() == ab::Roulette::State::Stopped;
                    const int ticks = roulette->step();
                    if (sound && ticks > 0) audio.playRange(1300, 1300);
                    if (!wasStopped && roulette->state() == ab::Roulette::State::Stopped && sound)
                        audio.playRange(roulette->prize() == ab::kPowClog ? 1320 : 1310, roulette->prize() == ab::kPowClog ? 1320 : 1310);
                    // Unattended runs press the key themselves.
                    if (opt.demo && (rouletteFrames == 50 || (roulette->state() == ab::Roulette::State::Stopped && rouletteFrames % 50 == 0)))
                        leave = roulette->press();
                }
                if (leave) {
                    prizeType = roulette->prize();
                    std::fprintf(stderr, "INFO  Roulette prize winner=%d type=%d\n", prizeWinner, prizeType);
                    roulette.reset();
                    leaveRoulette();
                }
            }
            audio.update();

            if (screen == Screen::Match) {
                const float alpha = paused ? 1.0f : static_cast<float>(accumulatorMs / kStepMs);
                std::array<int, ab::kMaxPlayers> shown = wins;
                if (campaignMode)  // the score boxes show campaign points
                    for (int i = 0; i < ab::kMaxPlayers; ++i)
                        shown[static_cast<std::size_t>(i)] = campaignScore[static_cast<std::size_t>(i)] + world.player(i).score;
                renderer.draw(world, previous, alpha, w, h, spritesPtr.get(), &shown);
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
                        renderer.image(spritesPtr->picture("victory" + std::to_string(matchWinner)));
                        const std::string line = "PLAYER " + std::to_string(matchWinner + 1) + " WINS THE MATCH!";  // message 36
                        renderer.text(*spritesPtr, line, 211, 441, 0, 0, 0);  // below the artwork's own title
                        renderer.text(*spritesPtr, line, 210, 440, 1.0f, 0.95f, 0.3f);
                    } else {
                        renderer.image(spritesPtr->picture("results"));
                        // Messages 30, 31 and 120/121 at the original's positions (values 780, 785, 800).
                        const std::string head = "Game Winner was Player " + std::to_string(win + 1) + " !";
                        renderer.text(*spritesPtr, head, 151, 141, 0, 0, 0);
                        renderer.text(*spritesPtr, head, 150, 140, 1, 1, 1);
                        const std::string need = "(Match winner must score " + std::to_string(winsNeeded) +
                                                 (cfg.winByKills ? " kills)" : " victories)");
                        renderer.text(*spritesPtr, need, 151, 95, 0, 0, 0);
                        renderer.text(*spritesPtr, need, 150, 94, 1, 1, 1);
                        int row = 0;
                        for (int i = 0; i < ab::kMaxPlayers; ++i) {
                            if (!world.player(i).present) continue;
                            const std::string line = "Player " + std::to_string(i + 1) + " score: " +
                                                     std::to_string(wins[static_cast<std::size_t>(i)]) + " (kills: " +
                                                     std::to_string(matchKills[static_cast<std::size_t>(i)]) + ")";
                            const float y = 210.0f + 20.0f * static_cast<float>(row++);
                            renderer.text(*spritesPtr, line, 151, y + 1, 0, 0, 0);
                            renderer.text(*spritesPtr, line, 150, y, i == win ? 1.0f : 0.8f, i == win ? 0.95f : 0.8f, i == win ? 0.3f : 0.8f);
                        }
                    }
                    renderer.end();
                }
            } else if (screen == Screen::Roulette && roulette) {
                static const char* const kPower[ab::kPowTypeCount] = {"bomb", "flame", "disease", "kicker", "skate", "punch", "grab",
                                                                    "spooge", "goldflame", "trigger", "jelly", "disease3", "random", "clog"};
                // Original messages 800-813.
                static const char* const kPrizeText[ab::kPowTypeCount] = {
                    "an extra bomb", "longer flame length", "a disease", "the ability to kick bombs", "extra speed",
                    "the ability to punch bombs", "the ability to grab bombs", "the spooger", "goldflame", "a trigger mechanism",
                    "jelly (bouncy) bombs", "super bad disease", "random", "a speed brake (slowness)"};
                renderer.begin(w, h);
                renderer.image(spritesPtr->picture("roulette"));
                float x = 0, y = 0;
                for (int s = 0; s < ab::Roulette::kSlots; ++s) {
                    roulette->screenPosition(roulette->slotPosition(s), &x, &y);
                    renderer.sprite(*spritesPtr, std::string("power ") + kPower[ab::Roulette::kPrize[static_cast<std::size_t>(s)]], 0, -1, x, y);
                }
                roulette->screenPosition(roulette->pointer(), &x, &y);
                renderer.sprite(*spritesPtr, "ring", 0, -1, x, y);
                if (roulette->state() == ab::Roulette::State::Stopped && roulette->prize() >= 0) {
                    // Messages 790, 800 + prize, 791, centred on the wheel 20 px apart.
                    const std::string lines[3] = {"The Gold Player has", kPrizeText[roulette->prize()], "for the next match!!"};
                    for (int r = 0; r < 3; ++r) {
                        const float tx = 320.0f - 4.0f * static_cast<float>(lines[r].size());
                        const float ty = 220.0f + 20.0f * static_cast<float>(r);
                        renderer.text(*spritesPtr, lines[r], tx + 1, ty + 1, 0, 0, 0);
                        renderer.text(*spritesPtr, lines[r], tx, ty, 1.0f, 0.95f, 0.3f);
                    }
                }
                renderer.end();
            } else if (screen == Screen::Intro) {
                static const char* const kIntroPicture[3] = {"iplogo", "hslogo", "title"};
                renderer.begin(w, h);
                renderer.image(spritesPtr->picture(kIntroPicture[std::clamp(introStep, 0, 2)]));
                renderer.end();
                if (opt.introShot == 0 && SDL_GetTicks() - introStart > static_cast<Uint64>(values.get(12)) * 1000u) advanceIntro();
            } else if (screen == Screen::Message) {
                renderer.begin(w, h);
                renderer.image(spritesPtr->picture("glue" + std::to_string(optionsGlue)));
                const float boxH = 70.0f + 24.0f * static_cast<float>(messageLines.size());
                renderer.quad(100, 180, 440, boxH, 0.0f, 0.0f, 0.10f, 0.88f);
                for (std::size_t r = 0; r < messageLines.size(); ++r) {
                    const float tx = 320.0f - renderer.textWidth(*spritesPtr, messageLines[r]) / 2.0f;
                    renderer.text(*spritesPtr, messageLines[r], tx, 196.0f + 24.0f * static_cast<float>(r), r == 0 ? 1.0f : 0.9f, r == 0 ? 0.95f : 0.9f, r == 0 ? 0.3f : 0.9f);
                }
                const std::string ok = "Enter: Ok";
                renderer.text(*spritesPtr, ok, 320.0f - renderer.textWidth(*spritesPtr, ok) / 2.0f, 180.0f + boxH - 30.0f, 0.4f, 1.0f, 1.0f);
                renderer.end();
            } else if (screen == Screen::CampaignList) {
                renderer.begin(w, h);
                renderer.image(spritesPtr->picture("glue0"));
                renderer.quad(40, 60, 420, 40.0f + 22.0f * static_cast<float>(campaignFiles.size()), 0.0f, 0.0f, 0.10f, 0.82f);
                renderer.text(*spritesPtr, "Please select a campaign file:", 55, 68, 1, 1, 1);  // message 1250
                for (std::size_t r = 0; r < campaignFiles.size(); ++r) {
                    const bool on = static_cast<int>(r) == campaignRow;
                    renderer.text(*spritesPtr, campaignFiles[r], 80, 92.0f + 22.0f * static_cast<float>(r), on ? 1.0f : 0.85f, on ? 0.95f : 0.85f, on ? 0.3f : 0.85f);
                }
                renderer.sprite(*spritesPtr, "cursor1", frame / 8, -1, 62.0f, 107.0f + 22.0f * static_cast<float>(campaignRow));
                renderer.text(*spritesPtr, "Up/Down: select   Enter: play   Esc: back", 60, 440, 0.4f, 1.0f, 1.0f);
                renderer.end();
            } else if (screen == Screen::Help) {
                renderer.begin(w, h);
                renderer.image(spritesPtr->picture("glue" + std::to_string(optionsGlue)));
                renderer.quad(20, 24, 600, 400, 0.0f, 0.0f, 0.10f, 0.82f);
                const int lineH = spritesPtr->font().height + 2;
                const int page = std::max(2, 344 / lineH);
                // Lines just above and below the page are drawn too, for pictures taller than a line.
                for (int r = -16; r < page + 16; ++r) {
                    const int index = helpTop + r;
                    if (index < 0 || index >= static_cast<int>(helpLines.size())) continue;
                    float x = 34.0f;
                    const float y = 34.0f + static_cast<float>(r * lineH);
                    for (const ab::HelpSegment& seg : helpLines[static_cast<std::size_t>(index)]) {
                        if (!seg.image) {
                            if (r >= 0 && r < page) renderer.text(*spritesPtr, seg.text, x, y, 1, 1, 1);
                            x += renderer.textWidth(*spritesPtr, seg.text);
                            continue;
                        }
                        int pw = 0, ph = 0;
                        if (!spritesPtr->pictureSize(seg.text, &pw, &ph)) continue;
                        // Centred on its line; kept inside the panel.
                        const float py = y + static_cast<float>(lineH - ph) / 2.0f;
                        if (py >= 24.0f && py + static_cast<float>(ph) <= 424.0f && x + static_cast<float>(pw) <= 620.0f)
                            renderer.picture(spritesPtr->picture(seg.text), x, py, static_cast<float>(pw), static_cast<float>(ph));
                        x += static_cast<float>(pw);
                    }
                }
                renderer.text(*spritesPtr, "Up/Down: line   PgUp/PgDn: page   Esc: done", 60, 440, 0.4f, 1.0f, 1.0f);
                renderer.end();
            } else if (screen == Screen::HelpList) {
                renderer.begin(w, h);
                renderer.image(spritesPtr->picture("glue" + std::to_string(optionsGlue)));
                renderer.quad(40, 60, 360, 40.0f + 22.0f * static_cast<float>(helpFiles.size()), 0.0f, 0.0f, 0.10f, 0.82f);
                renderer.text(*spritesPtr, "Available help files:", 55, 68, 1, 1, 1);  // message 600
                for (std::size_t r = 0; r < helpFiles.size(); ++r) {
                    const bool on = static_cast<int>(r) == helpRow;
                    renderer.text(*spritesPtr, helpFiles[r], 80, 92.0f + 22.0f * static_cast<float>(r), on ? 1.0f : 0.85f, on ? 0.95f : 0.85f, on ? 0.3f : 0.85f);
                }
                renderer.sprite(*spritesPtr, "cursor1", frame / 8, -1, 62.0f, 107.0f + 22.0f * static_cast<float>(helpRow));
                renderer.text(*spritesPtr, "Up/Down: select   Enter: read   Esc: back", 60, 440, 0.4f, 1.0f, 1.0f);
                renderer.end();
            } else if (screen == Screen::Options) {
                // Messages 250-263 with the original's value texts; rows from (55,40) every 22 px (value 745).
                static const char* const kYesNo[2] = {"No", "Yes"};
                static const char* const kSpeed[3] = {"Low", "Medium", "High"};
                static const char* const kDepth[4] = {"None", "A Little", "A Lot", "All the way!"};
                const std::string rows[kOptionRows] = {
                    std::string("Team Play: ") + kYesNo[teamPlay],
                    std::string("Random Start: ") + kYesNo[cfg.randomStart],
                    std::string("Conveyor Speed: ") + kSpeed[cfg.conveyorSpeed],
                    std::string("Stomped Bombs Detonate: ") + kYesNo[cfg.stompedBombsDetonate],
                    std::string("Win Matches By Kill Total: ") + kYesNo[cfg.winByKills],
                    std::string("Gold Bomberman: ") + kYesNo[cfg.goldman],
                    std::string("Enclosement Depth: ") + kDepth[cfg.enclosementDepth],
                    "Play Time: " + ab::Settings::playTimeText(cfg.playTime),
                    std::string("Diseases Can Be Destroyed: ") + kYesNo[cfg.diseasesDestroyable],
                    std::string("Disable music during gameplay: ") + kYesNo[cfg.disableGameMusic]};
                renderer.begin(w, h);
                renderer.image(spritesPtr->picture("glue" + std::to_string(optionsGlue)));
                for (int r = 0; r < kOptionRows; ++r) {
                    const float y = 40.0f + 22.0f * static_cast<float>(r);
                    renderer.text(*spritesPtr, rows[r], 56, y + 1, 0, 0, 0);
                    renderer.text(*spritesPtr, rows[r], 55, y, r == optionRow ? 1.0f : 0.85f, r == optionRow ? 0.95f : 0.85f, r == optionRow ? 0.3f : 0.85f);
                }
                renderer.sprite(*spritesPtr, "cursor1", frame / 8, -1, 41.0f, 55.0f + 22.0f * static_cast<float>(optionRow));
                renderer.text(*spritesPtr, "Up/Down: select   Left/Right: change   Esc: done", 60, 440, 0.4f, 1.0f, 1.0f);
                renderer.end();
            } else if (screen == Screen::LevelSetup) {
                const int shownLevel = randomLevel ? -1 : level;
                if (previewFor != shownLevel) {
                    previewFor = shownLevel;
                    auto pick = [&]() { return shownLevel >= 0 ? shownLevel : appRng.below(11); };
                    previewField = pick();
                    for (int py = 0; py < 5; ++py)
                        for (int px = 0; px < 5; ++px) {
                            // Solid at odd column and odd row; elsewhere, outside the top-left
                            // 2 x 2 corner, a brick 4 times in 5.
                            int cell = -1;
                            if ((px & 1) != 0 && (py & 1) != 0) cell = pick() + 100;
                            else if ((px > 1 || py > 1) && appRng.below(5) != 0) cell = pick();
                            previewBrick[static_cast<std::size_t>(py)][static_cast<std::size_t>(px)] = cell;
                        }
                }
                renderer.begin(w, h);
                renderer.image(spritesPtr->picture("glue1"));
                {
                    // The piece of the level's field picture behind the sample, then the tiles.
                    const std::string field = "field" + std::to_string(previewField);
                    int fw = 0, fh = 0;
                    if (spritesPtr->pictureSize(field, &fw, &fh))
                        renderer.pictureRegion(spritesPtr->picture(field), fw, fh, 0, 50, 5 * 40 + 20, 5 * 36 + 18, 380.0f, 82.0f);
                    for (int py = 0; py < 5; ++py)
                        for (int px = 0; px < 5; ++px) {
                            const int cell = previewBrick[static_cast<std::size_t>(py)][static_cast<std::size_t>(px)];
                            if (cell < 0) continue;
                            const int lv = cell % 100;
                            spritesPtr->ensureTiles(lv);
                            renderer.sprite(*spritesPtr, "tile " + std::to_string(lv) + (cell >= 100 ? " solid" : " brick"), 0, -1,
                                            400.0f + 40.0f * static_cast<float>(px), 100.0f + 36.0f * static_cast<float>(py));
                        }
                }
                const std::string lines[4] = {
                    randomLevel ? "Random Each Game" : kLevelName[level],
                    schemes.empty() ? std::string("(built-in arena)") : "Scheme: " + schemes[static_cast<std::size_t>(schemeIndex)].title,
                    std::to_string(winsNeeded) + " Wins to win match",
                    std::string("Team play: ") + (teamPlay ? "ON (teams from the scheme)" : "OFF")};
                for (int r = 0; r < 4; ++r) {
                    // Rows from (55,170) every 24 px (original value 735).
                    const float y = 170.0f + 24.0f * static_cast<float>(r);
                    renderer.text(*spritesPtr, lines[r], 56, y + 1, 0, 0, 0);
                    renderer.text(*spritesPtr, lines[r], 55, y, 1, 1, 1);
                }
                renderer.sprite(*spritesPtr, "cursor1", frame / 8, -1, 41.0f, 185.0f + 24.0f * static_cast<float>(setupRow));
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
                    std::string line = "Player " + std::to_string(i + 1) + ": " + controlName(control[static_cast<std::size_t>(i)]);
                    if (teamPlay && control[static_cast<std::size_t>(i)] != Control::Off)
                        line += std::string("   TEAM ") + (teams[static_cast<std::size_t>(i)] == 0 ? "1 (white)" : "2 (red)");
                    const float y = 170.0f + 24.0f * static_cast<float>(i);
                    renderer.text(*spritesPtr, line, 71, y + 1, 0, 0, 0);
                    renderer.text(*spritesPtr, line, 70, y, colour[i][0], colour[i][1], colour[i][2]);
                }
                renderer.sprite(*spritesPtr, "cursor1", frame / 8, -1, 56.0f, 185.0f + 24.0f * static_cast<float>(listRow));
                renderer.text(*spritesPtr, teamPlay ? "Up/Down: select   Left: off   Right: change   T: team   Enter: start" : "Up/Down: select   Left: off   Right: change   Enter: start", 60, 440, 0.4f, 1.0f, 1.0f);
                renderer.end();
            }

            ++frame;
            if (opt.resultShot && screen == Screen::Match && world.roundOver() && roundOverSteps == 50) {
                if (!opt.screenshot.empty()) writePpm(opt.screenshot, w, h);
                running = false;
            }
            if (opt.rouletteShot && screen == Screen::Roulette && roulette && roulette->state() == ab::Roulette::State::Stopped) {
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
