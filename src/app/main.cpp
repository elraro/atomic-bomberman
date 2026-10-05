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

#include "app/editor.hpp"
#include "app/names.hpp"
#include "app/net_ui.hpp"
#include "app/touch.hpp"
#include "audio/audio.hpp"
#include "free/free_data.hpp"
#include "game/ai.hpp"
#include "game/match.hpp"
#include "game/roulette.hpp"
#include "game/world.hpp"
#include "rendering/renderer.hpp"
#include "resources/asset_import.hpp"
#include "resources/campaign_file.hpp"
#include "resources/help_file.hpp"
#include "resources/mve_file.hpp"
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
    bool allSounds = false;   // --all-sounds: import also the sounds the game never plays (for the sound test)
    int menuShot = 0;         // automated: 1 = capture the main menu, 2 = the player list, ... 10-13 the network screens
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
    int movieShot = 0;        // automated: show the intro movie up to picture N, capture, exit
    int introShot = 0;        // automated: start on intro screen N (1-3)
    int attractSeconds = -1;  // --attract-seconds N: idle time on the menu before the demo (default: value 92)
    bool debug = false;       // --debug: the original's debug keys (it used the KWD environment variable)
    bool noIntro = false;     // --no-intro: go straight to the main menu
#ifdef __ANDROID__
    bool gles = true;         // OpenGL ES 3.0 (always on a phone)
    bool touch = true;        // on-screen controls
#else
    bool gles = false;        // --gles: OpenGL ES 3.0 instead of OpenGL 3.3
    bool touch = false;       // --touch: on-screen controls, worked with the mouse
#endif
    bool titleOnly = false;   // free asset set: the intro is the title screen and its call alone
    bool freeAssets = false;  // --free: the free asset set even if original game data is installed
    std::string connect;      // --connect ADDRESS: join that network game at once
    int host = 0;             // --host [PORT]: host a network game at once
    bool playersSet = false;  // --players or --humans given
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
        if (a == "--version") {
            std::puts("Atomic Bomberman (modern) " AB_VERSION);
            std::exit(0);
        }
        if (a == "--help" || a == "-h") {
            std::puts("Usage: atomic [options]\n"
                      "  --version            print the release number and exit\n"
                      "  --import-assets DIR  import the data of an original game copy, then exit\n"
                      "  --assets-dir DIR     where --import-assets writes (default: per-user data folder)\n"
                      "  --all-sounds         with --import-assets: also the sounds on the disc that the game never plays\n"
                      "                       (about 180 MB more; hear them under Options, Sound Test)\n"
                      "  --game-dir DIR       imported assets or an original game folder to play from\n"
                      "  --free               play with the free asset set even if original game data is found\n"
                      "  --start              skip the menu and start a match at once\n"
                      "  --scheme NAME        scheme (map) to play, e.g. BASIC\n"
                      "  --level N            level theme 0-10\n"
                      "  --players N          number of players (default 4)\n"
                      "  --humans N           keyboard players, 0-2\n"
                      "  --wins N             round wins needed for the match\n"
                      "  --campaign NAME      play a campaign file of the game data (simple, ghosts, crouton)\n"
                      "  --attract-seconds N  idle seconds on the menu before a demo round starts (default 30)\n"
                      "  --debug              debug keys in a match: Ctrl-A animation list, Alt-D information, F10 clears a campaign stage\n"
                      "  --no-intro           skip the intro movie, the logo and the title screens\n"
                      "  --roulette           the round winner spins for a prize before the next round\n"
                      "  --connect ADDRESS    join the network game at host or host:port at once\n"
                      "  --host [PORT]        host a network game at once (default port 27410)\n"
                      "  --seed N             random seed\n"
                      "  --mute               no sound\n"
                      "  --native             640x480 window\n"
                      "  --gles               OpenGL ES 3.0 instead of OpenGL 3.3 (what phones use)\n"
                      "  --touch              on-screen controls (as on a phone), worked with the mouse\n"
                      "  --shapes             plain shapes instead of the game's graphics\n"
                      "Testing: --demo --frames N --screenshot FILE --result-shot --menu-shot N --script KEYS");
            std::exit(0);
        }
        else if (a == "--game-dir") o.gameDir = next();
        else if (a == "--scheme") o.scheme = next(), o.schemeSet = true;
        else if (a == "--players") o.players = std::atoi(next().c_str()), o.playersSet = true;
        else if (a == "--humans") o.humans = std::clamp(std::atoi(next().c_str()), 0, 2), o.playersSet = true;
        else if (a == "--frames") o.frames = std::atoi(next().c_str());
        else if (a == "--screenshot") o.screenshot = next();
        else if (a == "--seed") o.seed = static_cast<std::uint32_t>(std::atoi(next().c_str()));
        else if (a == "--demo") o.demo = true;
        else if (a == "--start") o.menu = false;
        else if (a == "--import-assets") o.importFrom = next();
        else if (a == "--assets-dir") o.assetsDir = next();
        else if (a == "--all-sounds") o.allSounds = true;
        else if (a == "--result-shot") o.resultShot = true;
        else if (a == "--script") {
            // Comma-separated keys: up, down, left, right, enter, esc, space, backspace, t, f2, f3,
            // text=CHARACTERS (typed), wait (nothing). One every 10 frames.
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
        else if (a == "--gles") o.gles = true;
        else if (a == "--touch") o.touch = true;
        else if (a == "--free") o.freeAssets = true;
        else if (a == "--debug") o.debug = true;
        else if (a == "--attract-seconds") o.attractSeconds = std::atoi(next().c_str());
        else if (a == "--intro-shot") o.introShot = std::atoi(next().c_str());
        else if (a == "--movie-shot") o.movieShot = std::atoi(next().c_str());
        else if (a == "--campaign") o.campaign = next();
        else if (a == "--connect") o.connect = next(), o.noIntro = true;
        else if (a == "--host") {
            o.host = ab::net::kDefaultPort;
            o.noIntro = true;
            if (i + 1 < argc && std::isdigit(static_cast<unsigned char>(argv[i + 1][0]))) o.host = std::clamp(std::atoi(argv[++i]), 1, 65535);
        }
        else if (a == "--roulette-shot") o.roulette = o.rouletteShot = true;
        else {
            std::fprintf(stderr, "ERROR unknown argument %s (see --help)\n", a.c_str());
            std::exit(2);
        }
    }
    o.players = std::clamp(o.players, 1, ab::kMaxPlayers);
    if (o.demo || o.frames > 0) o.menu = o.menuShot != 0 || !o.script.empty() || !o.connect.empty() || o.host > 0;
    return o;
}

// DirectInput key codes (PC scan codes), as stored in the original's key definitions,
// and the SDL scancodes they correspond to.
struct KeyPair {
    int dik;
    SDL_Scancode sdl;
};
constexpr KeyPair kKeyTable[] = {
    {0x01, SDL_SCANCODE_ESCAPE}, {0x02, SDL_SCANCODE_1}, {0x03, SDL_SCANCODE_2}, {0x04, SDL_SCANCODE_3}, {0x05, SDL_SCANCODE_4},
    {0x06, SDL_SCANCODE_5}, {0x07, SDL_SCANCODE_6}, {0x08, SDL_SCANCODE_7}, {0x09, SDL_SCANCODE_8}, {0x0A, SDL_SCANCODE_9},
    {0x0B, SDL_SCANCODE_0}, {0x0C, SDL_SCANCODE_MINUS}, {0x0D, SDL_SCANCODE_EQUALS}, {0x0E, SDL_SCANCODE_BACKSPACE},
    {0x0F, SDL_SCANCODE_TAB}, {0x10, SDL_SCANCODE_Q}, {0x11, SDL_SCANCODE_W}, {0x12, SDL_SCANCODE_E}, {0x13, SDL_SCANCODE_R},
    {0x14, SDL_SCANCODE_T}, {0x15, SDL_SCANCODE_Y}, {0x16, SDL_SCANCODE_U}, {0x17, SDL_SCANCODE_I}, {0x18, SDL_SCANCODE_O},
    {0x19, SDL_SCANCODE_P}, {0x1A, SDL_SCANCODE_LEFTBRACKET}, {0x1B, SDL_SCANCODE_RIGHTBRACKET}, {0x1C, SDL_SCANCODE_RETURN},
    {0x1D, SDL_SCANCODE_LCTRL}, {0x1E, SDL_SCANCODE_A}, {0x1F, SDL_SCANCODE_S}, {0x20, SDL_SCANCODE_D}, {0x21, SDL_SCANCODE_F},
    {0x22, SDL_SCANCODE_G}, {0x23, SDL_SCANCODE_H}, {0x24, SDL_SCANCODE_J}, {0x25, SDL_SCANCODE_K}, {0x26, SDL_SCANCODE_L},
    {0x27, SDL_SCANCODE_SEMICOLON}, {0x28, SDL_SCANCODE_APOSTROPHE}, {0x29, SDL_SCANCODE_GRAVE}, {0x2A, SDL_SCANCODE_LSHIFT},
    {0x2B, SDL_SCANCODE_BACKSLASH}, {0x2C, SDL_SCANCODE_Z}, {0x2D, SDL_SCANCODE_X}, {0x2E, SDL_SCANCODE_C}, {0x2F, SDL_SCANCODE_V},
    {0x30, SDL_SCANCODE_B}, {0x31, SDL_SCANCODE_N}, {0x32, SDL_SCANCODE_M}, {0x33, SDL_SCANCODE_COMMA}, {0x34, SDL_SCANCODE_PERIOD},
    {0x35, SDL_SCANCODE_SLASH}, {0x36, SDL_SCANCODE_RSHIFT}, {0x37, SDL_SCANCODE_KP_MULTIPLY}, {0x38, SDL_SCANCODE_LALT},
    {0x39, SDL_SCANCODE_SPACE}, {0x3A, SDL_SCANCODE_CAPSLOCK}, {0x47, SDL_SCANCODE_KP_7}, {0x48, SDL_SCANCODE_KP_8},
    {0x49, SDL_SCANCODE_KP_9}, {0x4A, SDL_SCANCODE_KP_MINUS}, {0x4B, SDL_SCANCODE_KP_4}, {0x4C, SDL_SCANCODE_KP_5},
    {0x4D, SDL_SCANCODE_KP_6}, {0x4E, SDL_SCANCODE_KP_PLUS}, {0x4F, SDL_SCANCODE_KP_1}, {0x50, SDL_SCANCODE_KP_2},
    {0x51, SDL_SCANCODE_KP_3}, {0x52, SDL_SCANCODE_KP_0}, {0x53, SDL_SCANCODE_KP_PERIOD}, {0x9C, SDL_SCANCODE_KP_ENTER},
    {0x9D, SDL_SCANCODE_RCTRL}, {0xB5, SDL_SCANCODE_KP_DIVIDE}, {0xB8, SDL_SCANCODE_RALT}, {0xC7, SDL_SCANCODE_HOME},
    {0xC8, SDL_SCANCODE_UP}, {0xC9, SDL_SCANCODE_PAGEUP}, {0xCB, SDL_SCANCODE_LEFT}, {0xCD, SDL_SCANCODE_RIGHT},
    {0xCF, SDL_SCANCODE_END}, {0xD0, SDL_SCANCODE_DOWN}, {0xD1, SDL_SCANCODE_PAGEDOWN}, {0xD2, SDL_SCANCODE_INSERT},
    {0xD3, SDL_SCANCODE_DELETE}};

SDL_Scancode scancodeOfDik(int dik) {
    for (const KeyPair& k : kKeyTable)
        if (k.dik == dik) return k.sdl;
    return SDL_SCANCODE_UNKNOWN;
}

int dikOfScancode(SDL_Scancode sc) {
    for (const KeyPair& k : kKeyTable)
        if (k.sdl == sc) return k.dik;
    return 0;
}

// One of the two keyboard key sets: up, right, down, left, action 1, action 2.
ab::PlayerInput keyboardInput(const bool* keys, const std::array<int, 6>& set) {
    ab::PlayerInput in;
    auto down = [&](int slot) { return keys[scancodeOfDik(set[static_cast<std::size_t>(slot)])]; };
    in.dir = {down(0), down(1), down(2), down(3)};
    in.button1 = down(4);
    in.button2 = down(5);
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
void playEvents(const ab::World& world, const std::vector<ab::Event>& events, ab::Audio& audio) {
    for (const ab::Event& e : events) {
        switch (e.kind) {
            // Sound ids as at the original's call sites; each is the first of a run of
            // consecutive ids from which one is taken.
            case ab::EventKind::BombDropped: audio.playSeries(100); break;
            case ab::EventKind::BombPooped: audio.playSeries(550); break;
            case ab::EventKind::BombString:
                if (std::rand() % 4 == 0) audio.playSeries(1200);  // 1 in value 650
                break;
            case ab::EventKind::BombKicked: audio.playSeries(120); break;
            case ab::EventKind::BombStopped: audio.playSeries(130); break;
            case ab::EventKind::BombPunched: audio.playSeries(150); break;
            case ab::EventKind::BombBounced: audio.playSeries(160); break;
            case ab::EventKind::JellyBounced: audio.playSeries(135); break;  // original 0x423776
            case ab::EventKind::BombGrabbed: audio.playSeries(170); break;
            case ab::EventKind::BombThrown: audio.playSeries(150); break;
            case ab::EventKind::BombExploded: audio.playSeries(200); break;
            case ab::EventKind::WallBlock: audio.playRange(140, 142); break;  // one of three, fixed in the original
            case ab::EventKind::Hurry: audio.playSeries(2700); break;
            case ab::EventKind::PlayerDied:
                audio.playSeries(300);
                // Plus the sound that belongs to the death animation chosen (340 + its number,
                // original 0x41DDD9); the shipped list defines only 341.
                if (e.player >= 0) audio.playRange(340 + world.player(e.player).deathAnim, 340 + world.player(e.player).deathAnim);
                break;
            case ab::EventKind::HeadHit: audio.playSeries(360); break;
            case ab::EventKind::Pickup: audio.playSeries(400); break;
            case ab::EventKind::PickupJelly: audio.playSeries(135); break;
            case ab::EventKind::PickupAwesome: audio.playSeries(1400); break;
            case ab::EventKind::DiseaseGot:
                // 1 in 3: the line for that disease (3000 + 50 per disease); else a general one.
                audio.playSeries(std::rand() % 3 == 0 ? 3000 + 50 * e.value : 2300);
                break;
            case ab::EventKind::DeathTaunt:
                if (std::rand() % 5 == 0) audio.playSeries(700);  // 1 in value 95
                break;
            case ab::EventKind::Warped: audio.playSeries(1330); break;
            case ab::EventKind::TrampolineJump: audio.playSeries(350); break;
        }
    }
}

void writePpm(const std::string& path, int w, int h) {
    std::vector<unsigned char> px(static_cast<std::size_t>(w * h * 3));
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    // RGBA is the one format OpenGL ES is sure to give back.
    std::vector<unsigned char> rgba(static_cast<std::size_t>(w * h * 4));
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    for (std::size_t i = 0; i < px.size() / 3; ++i)
        for (std::size_t c = 0; c < 3; ++c) px[i * 3 + c] = rgba[i * 4 + c];
    std::ofstream out(path, std::ios::binary);
    out << "P6\n" << w << " " << h << "\n255\n";
    for (int y = h - 1; y >= 0; --y)
        out.write(reinterpret_cast<const char*>(px.data() + static_cast<std::size_t>(y * w * 3)), w * 3);
    std::fprintf(stderr, "INFO  Screenshot written path=%s size=%dx%d\n", path.c_str(), w, h);
}

}  // namespace

bool looksLikeGameDir(const std::string& dir) {
    if (ab::isFreeAssetDir(dir)) return true;
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

constexpr int kOptionRows = 16;
enum class Screen { MainMenu, PlayerList, LevelSetup, Match, Roulette, Options, Help, HelpList, Message, CampaignList, Intro, Keys, Editor, AudioAdjust, Movie, Net, SoundTest };

using ab::SchemeEntry;

int main(int argc, char** argv) {
    Options opt = parseArgs(argc, argv);
    if (!opt.importFrom.empty()) {
        const std::string to = opt.assetsDir.empty() ? userAssetsDir() : opt.assetsDir;
        std::printf("Importing game data\n  from: %s\n  to:   %s\n", opt.importFrom.c_str(), to.c_str());
        const ab::ImportReport r = ab::importAssets(opt.importFrom, to, opt.allSounds);
        if (!r.ok) {
            std::printf("Import failed: %s\n", r.error.c_str());
            return 1;
        }
        std::printf("Done: %d data files, %d sounds converted to .wav (%d listed sounds not found), %.1f MB.\n", r.dataFiles,
                    r.sounds, r.missingSounds, static_cast<double>(r.bytes) / 1.0e6);
        if (r.extraSounds > 0) std::printf("Also %d sounds that the game never plays (Options, Sound Test).\n", r.extraSounds);
        if (!r.missing.empty()) {
            // The original's own list names a few sounds its disc does not have (two are
            // marked there as dummies); nothing is wrong with the copy.
            std::printf("Listed in the game's sound list but not on the disc:");
            for (std::size_t i = 0; i < r.missing.size() && i < 10; ++i) std::printf(" %s", r.missing[i].c_str());
            if (r.missing.size() > 10) std::printf(" and %zu more (is the DATA\\SOUND folder complete?)", r.missing.size() - 10);
            std::printf("\n");
        }
        std::printf("Start the game without arguments to play.\n");
        return 0;
    }
    const std::string requested = opt.gameDir;
    opt.gameDir = opt.freeAssets ? std::string() : findGameDir(requested);
    if (opt.gameDir.empty()) {
        // No original game data: the free asset set, written once to the per-user folder.
        std::string freeDir = "free-assets";
        if (char* pref = SDL_GetPrefPath("atomic-bomberman-modern", "atomic")) {
            freeDir = std::string(pref) + "free-assets";
            SDL_free(pref);
        }
        if (ab::ensureFreeAssets(freeDir, true)) {
            opt.gameDir = freeDir;
            if (!opt.freeAssets)
                std::fprintf(stderr,
                             "INFO  No original game files found%s%s: playing with the free asset set.\n"
                             "INFO  To use the original graphics and sounds, import them from your copy of the game:\n"
                             "INFO      atomic --import-assets PATH_TO_ORIGINAL_GAME\n",
                             requested.empty() ? "" : " under ", requested.c_str());
        } else {
            std::fprintf(stderr, "WARN  Cannot write the free asset set to %s: running with placeholder shapes, no menu, no sound.\n", freeDir.c_str());
        }
    }
    if (!opt.gameDir.empty()) {
        std::fprintf(stderr, "INFO  Game files: %s%s\n", opt.gameDir.c_str(), ab::isFreeAssetDir(opt.gameDir) ? " (free asset set)" : "");
        opt.titleOnly = ab::isFreeAssetDir(opt.gameDir);  // the intro movie and logos are the original's: only the title screen
    }

    // Saved settings (the original's options.ini keys). Automated runs ignore the file so
    // that they stay reproducible; command-line choices win over it.
    ab::Settings cfg;
    std::string settingsPath;
    std::string userSchemesDir;  // schemes made with the editor
    if (char* pref = SDL_GetPrefPath("atomic-bomberman-modern", "atomic")) {
        settingsPath = std::string(pref) + "options.ini";
        userSchemesDir = std::string(pref) + "schemes";
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
    // Desktop: OpenGL 3.3. Phones (and --gles, for trying that path on a desktop): OpenGL ES 3.0.
    ab::setOpenGLES(opt.gles);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, opt.gles ? 0 : 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, opt.gles ? SDL_GL_CONTEXT_PROFILE_ES : SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_Window* window = SDL_CreateWindow(opt.gameDir.empty() ? "Atomic Bomberman (modern) - no game files found: run with --game-dir PATH"
                                                               : "Atomic Bomberman (modern)",
                                          opt.native ? 640 : 960, opt.native ? 480 : 720,
#ifdef __ANDROID__
                                          SDL_WINDOW_OPENGL | SDL_WINDOW_FULLSCREEN);
#else
                                          SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
#endif
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
        std::fprintf(stderr, "ERROR %s is not available on this system\n", opt.gles ? "OpenGL ES 3.0" : "OpenGL 3.3");
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
        if (!opt.gameDir.empty()) schemes = ab::listSchemes(opt.gameDir + "/data/schemes", userSchemesDir);
        ab::SchemeEditor editor(opt.gameDir + "/data/schemes", userSchemesDir);
        int editorKeyCount = 0;
        bool textInputOn = false;
        int schemeIndex = 0;
        for (std::size_t i = 0; i < schemes.size(); ++i)
            if (schemes[i].file == opt.scheme) schemeIndex = static_cast<int>(i);
        int setupRow = 0;
        bool teamPlay = cfg.teamPlay;
        std::array<int, ab::kMaxPlayers> teams{};
        std::array<ab::SchemePower, 13> schemePowers{};  // the scheme's -P lines
        int teamsFromScheme = -1;
        if (!opt.gameDir.empty())
            if (auto sf = ab::loadSchemeFile(opt.gameDir + "/data/schemes/" + opt.scheme + ".sch")) teams = sf->team, schemePowers = sf->powers;
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
                if (auto sf = ab::loadSchemeFile(schemes[static_cast<std::size_t>(schemeIndex)].path)) {
                    scheme = sf->scheme;
                    schemePowers = sf->powers;
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
        const bool sound = !opt.gameDir.empty() && !opt.mute && audio.init(opt.gameDir, cfg.smallMemory);
        audio.setVolumes(cfg.musicVolume, cfg.soundVolume);
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
        if (cfg.smallMemory) {
            // The normal (small) memory model of the original (0x41244B): one death animation
            // and one trapped animation instead of all of them; one sound per voice series.
            values.set(105, 1);
            values.set(330, 1);
        }
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
        if (!opt.playersSet && !opt.demo) {
            // The original's default list (0x42123F): each seat takes the next joystick if
            // there is one; otherwise the first such seat takes the keyboard (option
            // "Assign Keyboard Player"), and seat 2 becomes a computer player.
            control = {};
            bool keyboardGiven = !cfg.assignKeyboards;
            for (int i = 0; i < ab::kMaxPlayers; ++i) {
                if (i < padCount) control[static_cast<std::size_t>(i)] = static_cast<Control>(static_cast<int>(Control::Pad0) + i);
                else if (!keyboardGiven) control[static_cast<std::size_t>(i)] = Control::Key0, keyboardGiven = true;
                else if (i == 1) control[static_cast<std::size_t>(i)] = Control::Ai;
            }
        }

        // The menu screens need the original pictures; without them the match starts at once.
        const bool haveMenu = opt.menu && spritesPtr->loaded() && spritesPtr->picture("mainmenu") != 0;
        Screen screen = haveMenu ? Screen::MainMenu : Screen::Match;
        int menuItem = 0;
        int optionRow = 0;
        int keyRow = 0;
        int audioRow = 0;
        // Sound test: the catalogue, which part of it is listed (0 all, 1 only those the game
        // never plays, 2 only those it uses), and the row the pointer is on.
        std::vector<ab::Audio::Entry> soundList;
        int soundFilter = 0;
        int soundRow = 0;
        auto shownSounds = [&]() {
            std::vector<const ab::Audio::Entry*> shown;
            for (const ab::Audio::Entry& e : soundList)
                if (soundFilter == 0 || (soundFilter == 1) == e.ids.empty()) shown.push_back(&e);
            return shown;
        };
        bool keyCapture = false;  // waiting for the new key of the selected action
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
        if (opt.menuShot == 7) screen = Screen::Keys;
        if (opt.menuShot == 9) screen = Screen::AudioAdjust;
        if (opt.menuShot == 14) {
            soundList = audio.catalogue();
            if (opt.frames > 100) soundFilter = 1;  // automated: the second capture shows the unused ones
            screen = Screen::SoundTest;
        }
        if (opt.menuShot == 8) {  // the editor on a new scheme
            editor.open();
            editor.key(SDLK_2, false);
            screen = Screen::Editor;
        }
        if (opt.menuShot == 5) openHelp("credits.bm", Screen::MainMenu);
        const int netShot = opt.menuShot;  // 10 join, 11 host, 12 lobby of a hosted game, 13 a hosted match
        if (opt.menuShot == 6) openHelp("manual.bm", Screen::MainMenu);

        ab::RenderSnapshot previous;
        ab::MatchScore score;
        std::array<int, ab::kMaxPlayers>& wins = score.wins;  // round wins in the current match
        int roundOverSteps = 0;
        bool resultKey = false;    // a key was pressed on the result screen
        bool autoResults = false;  // Alt-W: result screens go on by themselves
        // Debug keys are on with --debug or, as in the original (0x412817), a non-zero KWD environment variable.
        const char* kwd = std::getenv("KWD");
        const bool debugKeys = opt.debug || (kwd != nullptr && std::atoi(kwd) != 0);
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
        std::array<int, ab::kMaxPlayers>& matchKills = score.kills;  // kills over the rounds of the match
        int& matchWinner = score.matchWinner;                        // player or team once the match is decided
        std::array<ab::Cell, ab::kMaxPlayers> startCells = scheme.start;
        ab::Rng appRng(opt.seed * 2654435761u + 99u);
        // Level music, unless switched off in the settings (original disable_game_music).
        auto playLevelMusic = [&]() {
            if (!sound) return;
            if (cfg.disableGameMusic) audio.stopMusic();
            else audio.playMusic(1100 + level);
        };
        // Network play: the screens behind "Start Network Game" and "Join Network Game".
        ab::NetUi::Hooks netHooks;
        netHooks.roundStarted = [&](int lv) {
            if (!opt.shapes && spritesPtr->level() != lv) {
                spritesPtr = std::make_unique<ab::SpriteBank>();
                spritesPtr->load(opt.gameDir, lv);
            }
            if (!sound) return;
            if (cfg.disableGameMusic) audio.stopMusic();
            else audio.playMusic(1100 + lv);
        };
        netHooks.stepped = [&](const ab::World& w, const std::vector<ab::Event>& events) { playEvents(w, events, audio); };
        netHooks.resultShown = [&](bool draw, bool decided) {
            if (!sound) return;
            audio.playMusic(1130);
            if (draw) audio.playRange(1700, 1999);
            else if (decided) audio.playRange(2000, 2299);
        };
        netHooks.lobbyEntered = [&]() {
            if (sound) audio.playMusic(1020);
        };
        netHooks.sound = [&](int id) {
            if (sound) audio.playRange(id, id == 40 ? 49 : id);
        };
        netHooks.settingsChanged = [&]() { saveSettings(); };
        ab::NetUi net(cfg, opt.gameDir, userSchemesDir, netHooks);
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
            // The original flushes its sound cache and picks a new selection of voice lines
            // every value 7 seconds (value 9 in the normal memory model), between rounds.
            if (sound && values.get(cfg.smallMemory ? 9 : 7) > 0 && audio.selectionAge() > values.get(cfg.smallMemory ? 9 : 7)) {
                audio.chooseSounds();
                std::fprintf(stderr, "INFO  Sound selection renewed\n");
            }
            ab::RoundSetup setup;
            setup.level = level;
            setup.scheme = scheme;
            setup.scheme.start = startCells;
            setup.powers = schemePowers;
            setup.extras = extras;
            setup.conveyorSpeed = cfg.conveyorSpeed;
            setup.teamPlay = teamPlay;
            setup.teams = teams;
            setup.enclosementDepth = cfg.enclosementDepth;
            setup.stompedBombsDetonate = cfg.stompedBombsDetonate;
            setup.diseasesDestroyable = cfg.diseasesDestroyable;
            setup.playTime = cfg.playTime;
            setup.winByKills = cfg.winByKills;
            setup.campaign = campaignMode;
            int n = 0;
            for (int i = 0; i < ab::kMaxPlayers; ++i) {
                const Control c = control[static_cast<std::size_t>(i)];
                setup.present[static_cast<std::size_t>(i)] = c != Control::Off;
                // Computer players are marked: the level's control delay is for humans only.
                setup.human[static_cast<std::size_t>(i)] = c != Control::Ai;
                n += c != Control::Off ? 1 : 0;
            }
            if (campaignMode) {
                const ab::CampaignStage& st = stages[static_cast<std::size_t>(stageIndex)];
                setup.ghosts = st.ghosts, setup.ghostSpeed = st.ghostSpeed;
                setup.rovers = st.rovers, setup.roverSpeed = st.roverSpeed;
            }
            // The roulette prize goes to the winner of the last match (to every member of the
            // winning team), in every round of this one.
            for (int i = 0; i < ab::kMaxPlayers && prizeType >= 0; ++i)
                if (setup.present[static_cast<std::size_t>(i)] && (teamPlay ? teams[static_cast<std::size_t>(i)] : i) == prizeWinner)
                    setup.prize[static_cast<std::size_t>(i)] = prizeType;
            ab::applyRoundSetup(world, values, setup);
            previous.capture(world);
            roundOverSteps = 0;
            resultKey = false;
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
        Screen helpListReturn = Screen::MainMenu;
        // Message 610: every *.BM file of the game folder.
        auto openHelpList = [&](Screen back) {
            helpFiles.clear();
            std::error_code ec;
            for (const auto& entry : std::filesystem::directory_iterator(opt.gameDir, ec)) {
                std::string ext = entry.path().extension().string();
                for (char& ch : ext) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                if (ext == ".bm") helpFiles.push_back(entry.path().filename().string());
            }
            std::sort(helpFiles.begin(), helpFiles.end());
            helpRow = 0;
            helpListReturn = back;
            screen = Screen::HelpList;
        };
        // Attract mode (original 0x42BB52): after value 92 (30) idle seconds on the main menu
        // a demo round is played by computer players, then the menu and its settings return.
        bool attract = false;
        Uint64 menuIdleSince = SDL_GetTicks();
        std::array<Control, ab::kMaxPlayers> savedControl{};
        int savedLevel = 0;
        bool savedRandomLevel = false, savedTeamPlay = false;
        auto endAttract = [&]() {
            attract = false;
            control = savedControl;
            level = savedLevel;
            randomLevel = savedRandomLevel;
            teamPlay = savedTeamPlay;
            screen = Screen::MainMenu;
            menuIdleSince = SDL_GetTicks();
            if (sound) audio.playMusic(1010);
        };
        int introStep = 0;
        Uint64 introStart = 0;
        std::unique_ptr<ab::MveDecoder> movie;
        Uint64 movieStart = 0;
        unsigned movieTexture = 0;
        // The original's intro (0x42B060): the title tune, the two logo pictures, then
        // an "Atomic Bomberman!" voice line with the title picture.
        auto startIntro = [&]() {
            screen = Screen::Intro;
            introStep = opt.titleOnly ? 2 : std::clamp(opt.introShot - 1, 0, 2);
            introStart = SDL_GetTicks();
            if (sound) audio.playMusic(1000);
            if (sound && introStep == 2) audio.playRange(2800, 2899);
        };
        auto endMovie = [&]() {
            audio.endStream();
            renderer.deleteTexture(movieTexture);
            movieTexture = 0;
            movie.reset();
            startIntro();
        };
        bool running = true;
        // A notice that waits for a key (the original's message boxes).
        std::vector<std::string> messageLines;
        std::function<void()> messageDone;
        bool messageAsks = false;  // a Yes / No question instead of a notice
        bool messageYes = false;
        std::function<void()> messageNo;
        auto showMessage = [&](std::vector<std::string> lines, std::function<void()> done) {
            messageLines = std::move(lines);
            messageDone = std::move(done);
            messageAsks = false;
            screen = Screen::Message;
        };
        auto askQuestion = [&](std::vector<std::string> lines, std::function<void()> yes, std::function<void()> no) {
            messageLines = std::move(lines);
            messageDone = std::move(yes);
            messageNo = std::move(no);
            messageAsks = true;
            messageYes = false;
            screen = Screen::Message;
        };
        bool farewell = false;  // leaving through the menu: say goodbye first
        // The original's quit routine (0x412987) asks first (message 10, buttons 25/26).
        auto askExit = [&]() {
            askQuestion({"Are you sure you want to exit?"}, [&]() { running = false, farewell = true; }, [&]() { screen = Screen::MainMenu; });
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
        } else if (screen == Screen::MainMenu && ((!opt.noIntro && opt.frames <= 0 && opt.script.empty()) || opt.introShot > 0 || opt.movieShot > 0)) {
            // First the intro movie, if the game data has it: the original ships it inside a
            // separate player program (intro/bmintro.exe); the import keeps it as intro.mve.
            if (opt.introShot == 0 && !opt.titleOnly) {
                movie = std::make_unique<ab::MveDecoder>();
                bool found = movie->open(opt.gameDir + "/intro.mve");
                for (const char* dir : {"intro", "INTRO", "Intro"})
                    for (const char* file : {"bmintro.exe", "BMINTRO.EXE", "Bmintro.exe"})
                        if (!found) found = movie->open(opt.gameDir + "/" + dir + "/" + file);
                if (found && movie->nextFrame()) {
                    screen = Screen::Movie;
                    movieStart = SDL_GetTicks();
                    if (sound) audio.beginStream(movie->sampleRate(), movie->channels());
                    if (sound) audio.pushStream(movie->takeAudio());
                    movieTexture = renderer.frameTexture(0, movie->width(), movie->height(), movie->rgba());
                    std::fprintf(stderr, "INFO  Intro movie %dx%d\n", movie->width(), movie->height());
                } else {
                    movie.reset();
                }
            }
            if (screen != Screen::Movie) startIntro();
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

        if (haveMenu && netShot >= 10 && netShot <= 13) {
            if (netShot == 10) net.openJoin();
            if (netShot == 11) net.openHost();
            if (netShot >= 12) net.hostNow(27497, netShot == 13);
            screen = Screen::Net;
        }
        if (haveMenu && !opt.connect.empty()) {
            net.joinNow(opt.connect);
            screen = Screen::Net;
        } else if (haveMenu && opt.host > 0) {
            net.hostNow(opt.host, false);
            screen = Screen::Net;
        }
        if (screen == Screen::Net && sound) audio.playMusic(1020);
        // Leaving the network screens: the menu again, with the local game's level graphics.
        auto leaveNet = [&]() {
            screen = Screen::MainMenu;
            menuIdleSince = SDL_GetTicks();
            if (!opt.shapes && spritesPtr->level() != level) {
                spritesPtr = std::make_unique<ab::SpriteBank>();
                spritesPtr->load(opt.gameDir, level);
            }
            if (sound) audio.playMusic(1010);
        };

        int netResultFrames = 0;
        ab::TouchPad touch;  // on-screen controls: always on a phone, --touch elsewhere
        touch.enable(opt.touch);
        bool paused = false;
        int frame = 0;
        int step = 0;
        Uint64 last = SDL_GetTicksNS();
        double accumulatorMs = 0.0;

        while (running) {
            // Scripted key presses for automated checks of the menu screens.
            if (!opt.script.empty() && frame % 10 == 5 && static_cast<std::size_t>(frame / 10) < opt.script.size()) {
                const std::string& k = opt.script[static_cast<std::size_t>(frame / 10)];
                SDL_Event press{};
                if (k.rfind("tap=", 0) == 0) {
                    // A tap at X:Y, in thousandths of the window (the mouse standing in for a finger).
                    int tx = 0, ty = 0, ww = 1, wh = 1;
                    std::sscanf(k.c_str() + 4, "%d:%d", &tx, &ty);
                    SDL_GetWindowSize(window, &ww, &wh);
                    press.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
                    press.button.x = static_cast<float>(tx * ww) / 1000.0f;
                    press.button.y = static_cast<float>(ty * wh) / 1000.0f;
                    SDL_PushEvent(&press);
                    press.type = SDL_EVENT_MOUSE_BUTTON_UP;
                } else if (k.rfind("text=", 0) == 0) {  // typed characters (the string outlives the event)
                    press.type = SDL_EVENT_TEXT_INPUT;
                    press.text.text = k.c_str() + 5;
                } else {
                    press.type = SDL_EVENT_KEY_DOWN;
                    press.key.key = k == "up" ? SDLK_UP : k == "down" ? SDLK_DOWN : k == "left" ? SDLK_LEFT : k == "right" ? SDLK_RIGHT
                                    : k == "esc" ? SDLK_ESCAPE : k == "f2" ? SDLK_F2 : k == "f3" ? SDLK_F3 : k == "f5" ? SDLK_F5 : k == "f6" ? SDLK_F6 : k == "t" ? SDLK_T
                                    : k == "space" ? SDLK_SPACE : k == "backspace" ? SDLK_BACKSPACE : SDLK_RETURN;
                }
                if (k != "wait") SDL_PushEvent(&press);
            }
            SDL_Event e;
            bool singleStep = false;
            while (SDL_PollEvent(&e)) {
                if (e.type == SDL_EVENT_QUIT) running = false;
                {
                    int ww = 1, wh = 1;
                    SDL_GetWindowSize(window, &ww, &wh);
                    touch.handle(e, ww, wh);
                }
                // A phone's back key is Esc.
                if (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_AC_BACK) e.key.key = SDLK_ESCAPE;
                if (screen == Screen::Editor) {
                    // The editor takes keys, typed text and the mouse (left: brick, right: start position).
                    if (e.type == SDL_EVENT_TEXT_INPUT) editor.text(e.text.text);
                    if (e.type == SDL_EVENT_KEY_DOWN) editor.key(e.key.key, (e.key.mod & SDL_KMOD_CTRL) != 0);
                    if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN || e.type == SDL_EVENT_MOUSE_MOTION) {
                        float mx = 0, my = 0;
                        const SDL_MouseButtonFlags held = SDL_GetMouseState(&mx, &my);
                        int ww = 1, wh = 1;
                        SDL_GetWindowSize(window, &ww, &wh);
                        const float scale = std::min(static_cast<float>(ww) / 640.0f, static_cast<float>(wh) / 480.0f);
                        const float lx = (mx - (static_cast<float>(ww) - 640.0f * scale) / 2.0f) / scale;
                        const float ly = (my - (static_cast<float>(wh) - 480.0f * scale) / 2.0f) / scale;
                        editor.mouse(lx, ly, (held & SDL_BUTTON_LMASK) != 0, (held & SDL_BUTTON_RMASK) != 0);
                    }
                    if (editor.takeHelpRequest()) openHelp("editor.bm", Screen::Editor);
                    if (editor.takeSaved()) schemes = ab::listSchemes(opt.gameDir + "/data/schemes", userSchemesDir);
                    if (editor.finished()) {
                        screen = Screen::MainMenu;
                        menuIdleSince = SDL_GetTicks();
                    }
                    continue;
                }
                if (screen == Screen::Net) {
                    if (e.type == SDL_EVENT_TEXT_INPUT) net.text(e.text.text);
                    // Held Backspace repeats; nothing else does.
                    if (e.type == SDL_EVENT_KEY_DOWN && (!e.key.repeat || e.key.key == SDLK_BACKSPACE))
                        net.key(e.key.key, (e.key.mod & SDL_KMOD_CTRL) != 0);
                    if (net.finished()) leaveNet();
                    continue;
                }
                if (e.type != SDL_EVENT_KEY_DOWN || e.key.repeat) continue;
                const SDL_Keycode key = e.key.key;
                menuIdleSince = SDL_GetTicks();
                if (attract) {  // any key ends the demo
                    endAttract();
                    continue;
                }
                // F1 opens the help file list from every game screen, as in the original (0x41431C).
                if (key == SDLK_F1 && haveMenu &&
                    (screen == Screen::MainMenu || screen == Screen::PlayerList || screen == Screen::LevelSetup ||
                     screen == Screen::Options || screen == Screen::Roulette || screen == Screen::Match)) {
                    openHelpList(screen);
                    continue;
                }
                if (screen == Screen::MainMenu) {
                    // The original's secret level editor: Ctrl-E six times (editor.bm).
                    if (key == SDLK_E && (e.key.mod & SDL_KMOD_CTRL) != 0) {
                        if (++editorKeyCount == 6) {
                            editorKeyCount = 0;
                            editor.open();
                            screen = Screen::Editor;
                        }
                    } else if (key != SDLK_LCTRL && key != SDLK_RCTRL) {
                        editorKeyCount = 0;
                    }
                    // Items as on the original's menu picture.
                    if (key == SDLK_UP) menuItem = (menuItem + 6) % 7;
                    if (key == SDLK_DOWN) menuItem = (menuItem + 1) % 7;
                    if (key == SDLK_ESCAPE) askExit();
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
                        if (menuItem == 1 || menuItem == 2) {  // Start Network Game, Join Network Game
                            if (sound) audio.playMusic(1020);
                            if (menuItem == 1) net.openHost();
                            else net.openJoin();
                            screen = Screen::Net;
                        }
                        if (menuItem == 3) {
                            screen = Screen::Options;
                            optionRow = 0;
                            // The original shows a random "glue" picture behind it (0x4148E5, value 16).
                            optionsGlue = appRng.below(std::max(1, values.get(16)));
                        }
                        if (menuItem == 4) openHelp("credits.bm", Screen::MainMenu);  // original 0x42BDE7
                        if (menuItem == 5) openHelpList(Screen::MainMenu);
                        if (menuItem == 6) askExit();
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
                } else if (screen == Screen::Movie) {
                    if (key == SDLK_RETURN || key == SDLK_SPACE || key == SDLK_ESCAPE) endMovie();  // skip
                } else if (screen == Screen::Intro) {
                    if (sound) audio.playRange(20, 20);
                    if (key == SDLK_RETURN || key == SDLK_SPACE || key == SDLK_ESCAPE) {
                        if (sound) audio.playRange(10, 10);
                        advanceIntro();
                    }
                } else if (screen == Screen::Message && messageAsks) {
                    if (key == SDLK_LEFT || key == SDLK_RIGHT || key == SDLK_TAB) messageYes = !messageYes;
                    const bool yes = key == SDLK_Y || (messageYes && (key == SDLK_RETURN || key == SDLK_SPACE));
                    const bool no = key == SDLK_N || key == SDLK_ESCAPE || (!messageYes && (key == SDLK_RETURN || key == SDLK_SPACE));
                    if (yes || no) {
                        const std::function<void()> act = yes ? std::move(messageDone) : std::move(messageNo);
                        messageDone = nullptr;
                        messageNo = nullptr;
                        if (act) act();
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
                    if (key == SDLK_ESCAPE) screen = helpListReturn;
                } else if (screen == Screen::AudioAdjust) {
                    // The original's "Adjust Audio" was never made (message 320 is its placeholder
                    // text); these two volumes are this implementation's.
                    const int d = key == SDLK_RIGHT ? 10 : key == SDLK_LEFT ? -10 : 0;
                    if (key == SDLK_UP || key == SDLK_DOWN) audioRow = 1 - audioRow;
                    if (d != 0) {
                        int& v = audioRow == 0 ? cfg.musicVolume : cfg.soundVolume;
                        v = std::clamp(v + d, 0, 100);
                        audio.setVolumes(cfg.musicVolume, cfg.soundVolume);
                        if (sound && audioRow == 1) audio.playSeries(100);  // hear the new level
                    }
                    if (key == SDLK_ESCAPE || key == SDLK_RETURN) {
                        saveSettings();
                        screen = Screen::Options;
                    }
                } else if (screen == Screen::SoundTest) {
                    const auto shown = shownSounds();
                    const int n = static_cast<int>(shown.size());
                    if (key == SDLK_UP && n > 0) soundRow = (soundRow + n - 1) % n;
                    if (key == SDLK_DOWN && n > 0) soundRow = (soundRow + 1) % n;
                    if (key == SDLK_PAGEUP || key == SDLK_LEFT) soundRow = std::max(0, soundRow - 15);
                    if (key == SDLK_PAGEDOWN || key == SDLK_RIGHT) soundRow = std::min(std::max(0, n - 1), soundRow + 15);
                    if (key == SDLK_HOME) soundRow = 0;
                    if (key == SDLK_END) soundRow = std::max(0, n - 1);
                    if (key == SDLK_TAB) soundFilter = (soundFilter + 1) % 3, soundRow = 0;
                    if ((key == SDLK_RETURN || key == SDLK_SPACE) && n > 0) audio.playNamed(shown[static_cast<std::size_t>(soundRow)]->name);
                    if (key == SDLK_BACKSPACE) audio.stopEffects();
                    if (key == SDLK_ESCAPE) {
                        audio.stopEffects();
                        if (sound) audio.playMusic(1010);
                        screen = Screen::Options;
                    }
                } else if (screen == Screen::Keys) {
                    // "Keyboard definitions" (messages 1100-1140): pick an action, press its new key.
                    constexpr int kRows = 13;
                    if (keyCapture) {
                        keyCapture = false;
                        const int dik = dikOfScancode(e.key.scancode);
                        if (key != SDLK_ESCAPE && dik != 0) cfg.keys[static_cast<std::size_t>(keyRow / 6)][static_cast<std::size_t>(keyRow % 6)] = dik;
                        else if (key != SDLK_ESCAPE && sound) audio.playRange(40, 49);
                    } else {
                        if (key == SDLK_UP) keyRow = (keyRow + kRows - 1) % kRows;
                        if (key == SDLK_DOWN) keyRow = (keyRow + 1) % kRows;
                        if (key == SDLK_RETURN || key == SDLK_SPACE) {
                            if (keyRow < 12) {
                                keyCapture = true;
                            } else {
                                cfg.resetKeys();
                                showMessage({"NOTE!", "Default key controls restored"}, [&]() { screen = Screen::Keys; });  // message 1131
                            }
                        }
                        if (key == SDLK_ESCAPE) {
                            saveSettings();
                            screen = Screen::Options;
                        }
                    }
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
                            case 9: cfg.disableGameMusic = !cfg.disableGameMusic; break;
                            case 10: cfg.assignKeyboards = !cfg.assignKeyboards; break;
                            case 11: cfg.netPrediction = !cfg.netPrediction; break;  // this implementation's network mode
                            case 12:
                                if (key != SDLK_LEFT) {
                                    screen = Screen::Keys;
                                    keyRow = 0;
                                    keyCapture = false;
                                }
                                break;
                            case 13:
                                // The memory model takes effect at the next start, so the original
                                // asks and then exits (messages 1320-1326).
                                askQuestion(cfg.smallMemory
                                                ? std::vector<std::string>{"NOTE!  To change to enhanced memory model you will need to",
                                                                           "exit and restart Bomberman. Do you want to do this?"}
                                                : std::vector<std::string>{"NOTE!  To change to normal memory model you will need to",
                                                                           "exit and restart Bomberman. Do you want to do this?"},
                                            [&]() {
                                                cfg.smallMemory = !cfg.smallMemory;
                                                saveSettings();
                                                showMessage({"Memory Model Changed!", "Now exiting."}, [&]() { running = false; });
                                            },
                                            [&]() { screen = Screen::Options; });
                                break;
                            case 14:
                                if (key != SDLK_LEFT) {
                                    screen = Screen::AudioAdjust;
                                    audioRow = 0;
                                }
                                break;
                            default:
                                // Sound test (this implementation's): every sound file of the game data.
                                if (key != SDLK_LEFT) {
                                    soundList = audio.catalogue();
                                    soundFilter = 0;
                                    soundRow = 0;
                                    audio.stopMusic();
                                    screen = Screen::SoundTest;
                                }
                                break;
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
                    // The original leaves a match with Ctrl-Q (0x42A56F); Esc is this implementation's addition.
                    if (key == SDLK_ESCAPE || (key == SDLK_Q && (e.key.mod & SDL_KMOD_CTRL) != 0)) {
                        campaignMode = false;
                        if (haveMenu) {
                            screen = Screen::MainMenu;
                            if (sound) audio.playMusic(1010);
                        } else {
                            running = false;
                        }
                    }
                    if ((key == SDLK_RETURN || key == SDLK_SPACE) && world.roundOver() && roundOverSteps > 20) resultKey = true;
                    const bool alt = (e.key.mod & SDL_KMOD_ALT) != 0, ctrl = (e.key.mod & SDL_KMOD_CTRL) != 0;
                    if (key == SDLK_W && alt) autoResults = true;  // original Alt-W (0x42A5ED)
                    if (debugKeys && key == SDLK_F10) world.debugClearStage();  // original 0x42A5AC
                    if (debugKeys && key == SDLK_A && ctrl) {
                        // Original Ctrl-A (0x42A325): the list of animation sequences, also written to anims.lst.
                        const std::vector<std::string> names = spritesPtr->sequenceNames();
                        helpLines.clear();
                        helpLines.push_back({{false, "Animation sequences available (" + std::to_string(names.size()) + " total):"}});  // message 20
                        for (const std::string& n : names) helpLines.push_back({{false, n}});
                        if (!settingsPath.empty()) {
                            std::ofstream list(std::filesystem::path(settingsPath).parent_path() / "anims.lst");
                            for (const std::string& n : names) list << n << "\n";
                        }
                        helpTop = 0;
                        helpReturn = Screen::Match;
                        screen = Screen::Help;
                    }
                    if (debugKeys && key == SDLK_D && alt) {
                        // Original Alt-D (0x413D45): messages 400-420. The network lines have nothing to show.
                        showMessage({"Internal debugging information window",
                                     "Total mem: " + std::to_string(ab::SpriteBank::textureBytes() + audio.cachedBytes()) +
                                         ", Audio mem: " + std::to_string(audio.cachedBytes()),
                                     "Our local net id: 0", "Critical retrans rate: 0.000",
                                     "Audio cache hits: " + std::to_string(audio.cacheHitPercent()) + "%"},
                                    [&]() { screen = Screen::Match; });
                    }
                    if (key == SDLK_R && !ctrl) beginMatch();
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

            if (screen == Screen::Net) {
                // The players at this computer: the two key sets, each with a gamepad, then two more gamepads.
                const bool* keys = SDL_GetKeyboardState(nullptr);
                std::array<ab::PlayerInput, ab::net::kMaxLocalPlayers> locals{};
                for (std::size_t l = 0; l < locals.size(); ++l) {
                    if (l < 2) locals[l] = keyboardInput(keys, cfg.keys[l]);
                    const ab::PlayerInput pad = gamepadInput(pads[l]);
                    const ab::PlayerInput screenPad = l == 0 ? touch.input() : ab::PlayerInput{};  // the on-screen controls are the first player's
                    for (std::size_t d = 0; d < 4; ++d) locals[l].dir[d] = locals[l].dir[d] || pad.dir[d] || screenPad.dir[d];
                    locals[l].button1 = locals[l].button1 || pad.button1 || screenPad.button1;
                    locals[l].button2 = locals[l].button2 || pad.button2 || screenPad.button2;
                }
                net.update(ab::net::clockMs(), locals);
                if (opt.frames > 0) SDL_Delay(5);  // automated runs: the network runs on the real clock
            }
            if (screen == Screen::Match) {
                accumulatorMs += std::min(frameMs, 250.0);
                if (paused) accumulatorMs = singleStep ? kStepMs : 0.0;
                const bool* keys = SDL_GetKeyboardState(nullptr);
                while (accumulatorMs >= kStepMs) {
                    std::array<ab::PlayerInput, ab::kMaxPlayers> input{};
                    for (int i = 0; i < ab::kMaxPlayers; ++i) {
                        const Control c = control[static_cast<std::size_t>(i)];
                        if (c == Control::Ai) input[static_cast<std::size_t>(i)] = ai[static_cast<std::size_t>(i)].decide(world, i, kStepMs);
                        if (c == Control::Key0) {
                            // The first key set, and with it the on-screen controls.
                            ab::PlayerInput& in = input[static_cast<std::size_t>(i)];
                            in = keyboardInput(keys, cfg.keys[0]);
                            const ab::PlayerInput pad = touch.input();
                            for (std::size_t d = 0; d < 4; ++d) in.dir[d] = in.dir[d] || pad.dir[d];
                            in.button1 = in.button1 || pad.button1;
                            in.button2 = in.button2 || pad.button2;
                        }
                        if (c == Control::Key1) input[static_cast<std::size_t>(i)] = keyboardInput(keys, cfg.keys[1]);
                        if (c >= Control::Pad0)
                            input[static_cast<std::size_t>(i)] = gamepadInput(pads[static_cast<std::size_t>(static_cast<int>(c) - static_cast<int>(Control::Pad0))]);
                    }
                    previous.capture(world);
                    world.tick(kStepMs, input);
                    playEvents(world, world.takeEvents(), audio);
                    accumulatorMs -= kStepMs;
                    ++step;
                    if (attract && world.roundOver()) {  // one round, no result screen
                        endAttract();
                        break;
                    }
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
                        const int win = score.roundDecided(world, winsNeeded, cfg.winByKills);
                        if (win >= 0)
                            std::fprintf(stderr, "INFO  Round over winner=%d kills=%d wins=%d\n", win, world.player(win).kills, wins[static_cast<std::size_t>(win)]);
                        else
                            std::fprintf(stderr, "INFO  Round over draw\n");
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
                    // The result screen waits for Enter or Space; with no human player, or after
                    // Alt-W, it goes on by itself after six seconds (original 0x42A779).
                    bool anyHuman = false;
                    for (Control c : control) anyHuman = anyHuman || (c != Control::Off && c != Control::Ai);
                    if (world.roundOver()) ++roundOverSteps;
                    if (world.roundOver() && roundOverSteps > 20 && (resultKey || ((!anyHuman || autoResults || opt.frames > 0) && roundOverSteps > 140))) {
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
            {
                const int idle = opt.attractSeconds >= 0 ? opt.attractSeconds : values.get(92);
                if (screen != Screen::MainMenu) menuIdleSince = SDL_GetTicks();
                if (screen == Screen::MainMenu && haveMenu && idle >= 5 && (opt.frames <= 0 || opt.attractSeconds >= 0) &&
                    SDL_GetTicks() - menuIdleSince > static_cast<Uint64>(idle) * 1000u) {
                    attract = true;
                    savedControl = control;
                    savedLevel = level;
                    savedRandomLevel = randomLevel;
                    savedTeamPlay = teamPlay;
                    // max(3, 1..10) computer players in the first seats, a random level, no teams.
                    const int n = std::max(3, appRng.below(10) + 1);
                    for (int i = 0; i < ab::kMaxPlayers; ++i) control[static_cast<std::size_t>(i)] = i < n ? Control::Ai : Control::Off;
                    randomLevel = false;
                    level = appRng.below(11);
                    teamPlay = false;
                    campaignMode = false;
                    newMatch();
                    screen = Screen::Match;
                    beginMatch();
                    playLevelMusic();
                    std::fprintf(stderr, "INFO  Attract mode players=%d level=%d\n", n, level);
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
            if (const bool want = (screen == Screen::Editor && editor.wantsTextInput()) || (screen == Screen::Net && net.wantsTextInput());
                want != textInputOn) {
                textInputOn = want;
                if (want) SDL_StartTextInput(window);
                else SDL_StopTextInput(window);
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
            } else if (screen == Screen::Net) {
                net.draw(renderer, *spritesPtr, w, h, frame, ab::net::clockMs());
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
            } else if (screen == Screen::Movie && movie) {
                // Pictures follow the clock; the sound is queued as it is decoded.
                const double elapsed = opt.movieShot > 0 ? 1.0e9 : static_cast<double>(SDL_GetTicks() - movieStart) / 1000.0;
                const int due = opt.movieShot > 0 ? opt.movieShot : static_cast<int>(elapsed / movie->frameSeconds()) + 1;
                bool more = true, fresh = false;
                for (int n = 0; more && movie->framesDecoded() < due && n < 30; ++n) {
                    more = movie->nextFrame();
                    fresh = fresh || more;
                    if (more && sound) audio.pushStream(movie->takeAudio());
                }
                if (fresh) movieTexture = renderer.frameTexture(movieTexture, movie->width(), movie->height(), movie->rgba());
                renderer.begin(w, h);
                renderer.quad(0, 0, 640, 480, 0, 0, 0);
                const float mh = 640.0f * static_cast<float>(movie->height()) / static_cast<float>(std::max(1, movie->width()));
                renderer.picture(movieTexture, 0, (480.0f - mh) / 2.0f, 640.0f, mh);
                renderer.end();
                if (opt.movieShot > 0 && movie->framesDecoded() >= opt.movieShot) {
                    if (!opt.screenshot.empty()) writePpm(opt.screenshot, w, h);
                    running = false;
                } else if (!more) {
                    endMovie();
                }
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
                if (messageAsks) {
                    // The two buttons, messages 25 and 26.
                    renderer.text(*spritesPtr, "No", 250, 180.0f + boxH - 30.0f, messageYes ? 0.6f : 1.0f, messageYes ? 0.6f : 0.95f, messageYes ? 0.6f : 0.3f);
                    renderer.text(*spritesPtr, "Yes", 360, 180.0f + boxH - 30.0f, messageYes ? 1.0f : 0.6f, messageYes ? 0.95f : 0.6f, messageYes ? 0.3f : 0.6f);
                } else {
                    const std::string ok = "Enter: Ok";
                    renderer.text(*spritesPtr, ok, 320.0f - renderer.textWidth(*spritesPtr, ok) / 2.0f, 180.0f + boxH - 30.0f, 0.4f, 1.0f, 1.0f);
                }
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
                        // Centred on its line, and cut off at the edges of the text area rather than
                        // left out when it does not fit whole (original 0x41302D: rows above y 34 and
                        // below 34 + 344 are not copied, nor columns beyond the 532 px text width).
                        int top = static_cast<int>(y) - (ph - lineH) / 2, skip = 0, rows = ph;
                        if (top < 34) skip = 34 - top, rows -= skip, top = 34;
                        rows = std::min(rows, 34 + 344 - top);
                        const int columns = std::min(pw, 34 + 532 - static_cast<int>(x));
                        if (rows > 0 && columns > 0)
                            renderer.pictureRegion(spritesPtr->picture(seg.text), pw, ph, 0, skip, columns, rows, x, static_cast<float>(top));
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
            } else if (screen == Screen::Editor) {
                renderer.begin(w, h);
                editor.draw(renderer, *spritesPtr, level, frame);
                renderer.end();
            } else if (screen == Screen::SoundTest) {
                const auto shown = shownSounds();
                const int n = static_cast<int>(shown.size());
                int unused = 0;
                for (const ab::Audio::Entry& entry : soundList) unused += entry.ids.empty() ? 1 : 0;
                renderer.begin(w, h);
                renderer.image(spritesPtr->picture("glue" + std::to_string(optionsGlue)));
                renderer.quad(30, 20, 580, 404, 0.0f, 0.0f, 0.10f, 0.85f);
                static const char* const kFilter[3] = {"all", "never played", "used by the game"};
                renderer.text(*spritesPtr, "Sound Test", 45, 28, 1.0f, 0.95f, 0.3f);
                renderer.text(*spritesPtr, std::to_string(soundList.size()) + " sounds, " + std::to_string(unused) + " never played.   Showing: " + kFilter[soundFilter], 45, 50, 0.85f, 0.85f, 0.85f);
                const int top = std::clamp(soundRow - 7, 0, std::max(0, n - 15));
                for (int r = 0; r < 15 && top + r < n; ++r) {
                    const ab::Audio::Entry& item = *shown[static_cast<std::size_t>(top + r)];
                    const bool on = top + r == soundRow;
                    const float y = 78.0f + 21.0f * static_cast<float>(r);
                    const float cr = on ? 1.0f : item.ids.empty() ? 0.6f : 0.85f, cg = on ? 0.95f : item.ids.empty() ? 0.8f : 0.85f, cb = on ? 0.3f : item.ids.empty() ? 1.0f : 0.85f;
                    renderer.text(*spritesPtr, item.name, 70, y, cr, cg, cb);
                    std::string use = "never played by the game";
                    if (!item.ids.empty()) {
                        use = "sound " + std::to_string(item.ids[0]);
                        if (item.ids.size() > 1) use += " and " + std::to_string(item.ids.size() - 1) + " more";
                    }
                    renderer.text(*spritesPtr, use, 230, y, cr, cg, cb);
                    char length[24];
                    std::snprintf(length, sizeof length, "%.1f s", item.seconds);
                    renderer.text(*spritesPtr, length, 540, y, cr, cg, cb);
                    if (on) renderer.sprite(*spritesPtr, "cursor1", frame / 8, -1, 52.0f, y + 15.0f);
                }
                if (unused == 0 && !soundList.empty() && !ab::isFreeAssetDir(opt.gameDir))
                    renderer.text(*spritesPtr, "For the disc's other sounds: atomic --import-assets PATH --all-sounds", 45, 398, 0.7f, 0.7f, 0.7f);
                renderer.text(*spritesPtr, "Up/Down: choose   Enter: play   Tab: show   Esc: done", 60, 440, 0.4f, 1.0f, 1.0f);
                renderer.end();
            } else if (screen == Screen::AudioAdjust) {
                renderer.begin(w, h);
                renderer.image(spritesPtr->picture("glue" + std::to_string(optionsGlue)));
                renderer.quad(40, 60, 420, 110, 0.0f, 0.0f, 0.10f, 0.82f);
                renderer.text(*spritesPtr, "Adjust Audio", 55, 68, 1, 1, 1);
                const std::string rows[2] = {"Music volume: " + std::to_string(cfg.musicVolume) + "%",
                                             "Sound volume: " + std::to_string(cfg.soundVolume) + "%"};
                for (int r = 0; r < 2; ++r) {
                    const bool on = r == audioRow;
                    renderer.text(*spritesPtr, rows[r], 80, 100.0f + 24.0f * static_cast<float>(r), on ? 1.0f : 0.85f, on ? 0.95f : 0.85f, on ? 0.3f : 0.85f);
                }
                renderer.sprite(*spritesPtr, "cursor1", frame / 8, -1, 62.0f, 115.0f + 24.0f * static_cast<float>(audioRow));
                renderer.text(*spritesPtr, "Up/Down: select   Left/Right: change   Esc: done", 60, 440, 0.4f, 1.0f, 1.0f);
                renderer.end();
            } else if (screen == Screen::Keys) {
                static const char* const kAction[6] = {"Move Up", "Move Right", "Move Down", "Move Left", "Action 1", "Action 2"};  // 1120-1125
                renderer.begin(w, h);
                renderer.image(spritesPtr->picture("glue" + std::to_string(optionsGlue)));
                renderer.quad(40, 30, 560, 380, 0.0f, 0.0f, 0.10f, 0.82f);
                renderer.text(*spritesPtr, "Keyboard definitions", 55, 40, 1, 1, 1);  // message 1100
                for (int r = 0; r < 13; ++r) {
                    std::string line = "Return to default keys";  // message 1130
                    if (r < 12) {
                        const int dik = cfg.keys[static_cast<std::size_t>(r / 6)][static_cast<std::size_t>(r % 6)];
                        const char* name = SDL_GetScancodeName(scancodeOfDik(dik));
                        line = "Key " + std::to_string(r / 6) + ", " + kAction[r % 6] + "    Key: '" + (name != nullptr ? name : "?") + "'";  // 1110, 1140
                    }
                    const bool on = r == keyRow;
                    const float y = 70.0f + 22.0f * static_cast<float>(r) + (r >= 6 ? 12.0f : 0.0f) + (r >= 12 ? 12.0f : 0.0f);
                    renderer.text(*spritesPtr, line, 80, y, on ? 1.0f : 0.85f, on ? 0.95f : 0.85f, on ? 0.3f : 0.85f);
                    if (on) renderer.sprite(*spritesPtr, "cursor1", frame / 8, -1, 62.0f, y + 15.0f);
                }
                if (keyCapture) {
                    const std::string ask = std::string("Press key for '") + kAction[keyRow % 6] + "'";  // message 1105
                    renderer.quad(140, 200, 360, 50, 0.1f, 0.1f, 0.3f, 0.95f);
                    renderer.text(*spritesPtr, ask, 320.0f - renderer.textWidth(*spritesPtr, ask) / 2.0f, 216, 1.0f, 0.95f, 0.3f);
                }
                renderer.text(*spritesPtr, "Up/Down: select   Enter: change   Esc: done", 60, 440, 0.4f, 1.0f, 1.0f);
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
                    std::string("Disable music during gameplay: ") + kYesNo[cfg.disableGameMusic],
                    std::string("Assign Keyboard Player: ") + kYesNo[cfg.assignKeyboards],
                    std::string("Network: Show Own Moves At Once: ") + kYesNo[cfg.netPrediction],
                    "Define keyboard layouts",
                    std::string("Use Enhanced Memory Model: ") + kYesNo[!cfg.smallMemory],  // message 267
                    "Adjust Audio",                                                          // message 268
                    "Sound Test"};
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
                            // The original puts each tile's top left corner at (400 + 40 x, 100 + 36 y)
                            // (0x406AA3: a plain copy, not the in-game sprite call). Sprites are placed
                            // by their reference point, the cell's bottom centre, hence the 20 and 35.
                            renderer.sprite(*spritesPtr, "tile " + std::to_string(lv) + (cell >= 100 ? " solid" : " brick"), 0, -1,
                                            400.0f + 20.0f + 40.0f * static_cast<float>(px), 100.0f + 35.0f + 36.0f * static_cast<float>(py));
                        }
                }
                const std::string lines[4] = {
                    randomLevel ? "Random Each Game" : ab::kLevelName[level],
                    schemes.empty() ? std::string("(built-in arena)") : "Scheme: " + schemes[static_cast<std::size_t>(schemeIndex)].title,
                    std::to_string(winsNeeded) + (cfg.winByKills ? " Kills" : " Wins") + " to win match",  // messages 211, 208/209
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
                // The release, top left, where the original shows its "V1.0".
                renderer.text(*spritesPtr, "V" AB_VERSION " Modern", 9, 7, 0, 0, 0);
                renderer.text(*spritesPtr, "V" AB_VERSION " Modern", 8, 6, 1, 1, 1);
                renderer.end();
            } else {
                renderer.begin(w, h);
                renderer.image(spritesPtr->picture("glue0"));
                // Original layout: heading at (40,140), list from (70,170) every 24 px (values 705/710).
                renderer.text(*spritesPtr, "Available players:", 41, 141, 0, 0, 0);
                renderer.text(*spritesPtr, "Available players:", 40, 140, 1, 1, 1);
                const auto& colour = ab::kSeatColour;
                for (int i = 0; i < ab::kMaxPlayers; ++i) {
                    std::string line = "Player " + std::to_string(i + 1) + ": " + controlName(control[static_cast<std::size_t>(i)]);
                    if (teamPlay && control[static_cast<std::size_t>(i)] != Control::Off)
                        line += std::string("   TEAM ") + (teams[static_cast<std::size_t>(i)] == 0 ? "1 (white)" : "2 (red)");
                    const float y = 170.0f + 24.0f * static_cast<float>(i);
                    renderer.text(*spritesPtr, line, 71, y + 1, 0, 0, 0);
                    renderer.text(*spritesPtr, line, 70, y, colour[i][0], colour[i][1], colour[i][2]);
                }
                renderer.sprite(*spritesPtr, "cursor1", frame / 8, -1, 56.0f, 185.0f + 24.0f * static_cast<float>(listRow));
                // Messages 40-42 at the original's positions (values 715, 720).
                renderer.text(*spritesPtr, "Available Joysticks:", 301, 141, 0, 0, 0);
                renderer.text(*spritesPtr, "Available Joysticks:", 300, 140, 1, 1, 1);
                if (padCount == 0) renderer.text(*spritesPtr, "None Detected", 320, 170, 0.85f, 0.85f, 0.85f);
                for (int j = 0; j < padCount; ++j) {
                    const char* name = pads[static_cast<std::size_t>(j)] != nullptr ? SDL_GetGamepadName(pads[static_cast<std::size_t>(j)]) : nullptr;
                    renderer.text(*spritesPtr, "Joy " + std::to_string(j) + " - " + (name != nullptr ? name : "?"), 320,
                                  170.0f + 24.0f * static_cast<float>(j), 0.85f, 0.85f, 0.85f);
                }
                renderer.text(*spritesPtr, teamPlay ? "Up/Down: select   Left: off   Right: change   T: team   Enter: start" : "Up/Down: select   Left: off   Right: change   Enter: start", 60, 440, 0.4f, 1.0f, 1.0f);
                renderer.end();
            }

            touch.update(SDL_GetTicks());
            touch.draw(renderer, w, h);
            ++frame;
            if (opt.resultShot && screen == Screen::Match && world.roundOver() && roundOverSteps == 50) {
                if (!opt.screenshot.empty()) writePpm(opt.screenshot, w, h);
                running = false;
            }
            if (opt.resultShot && screen == Screen::Net && net.showingResult() && ++netResultFrames == 30) {
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
