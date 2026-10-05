#include "free/free_data.hpp"

#include <filesystem>
#include <fstream>
#include <sstream>

#include "game/values.hpp"
#include "resources/scheme_file.hpp"

namespace ab {

namespace {

// Raised whenever the written files change, so an older folder is renewed.
const char* const kMarker = "atomic-bomberman-modern free asset set, version 2\n"
                            "Made by the program itself; contains nothing from the original game.\n"
                            "This folder is rewritten when the program is updated: do not keep your own files here.\n";

bool writeText(const std::filesystem::path& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary);
    out << text;
    return static_cast<bool>(out);
}

// 22050 Hz, 16 bit, stereo: the format the game's sound code expects.
bool writeWav(const std::filesystem::path& path, const std::vector<std::int16_t>& mono) {
    std::string data;
    data.reserve(mono.size() * 4 + 44);
    auto u32 = [&](std::uint32_t v) {
        for (int i = 0; i < 4; ++i) data.push_back(static_cast<char>((v >> (8 * i)) & 255));
    };
    auto u16 = [&](std::uint32_t v) {
        data.push_back(static_cast<char>(v & 255));
        data.push_back(static_cast<char>((v >> 8) & 255));
    };
    const auto bytes = static_cast<std::uint32_t>(mono.size() * 4);
    data += "RIFF";
    u32(36 + bytes);
    data += "WAVEfmt ";
    u32(16);
    u16(1);
    u16(2);
    u32(22050);
    u32(22050 * 4);
    u16(4);
    u16(16);
    data += "data";
    u32(bytes);
    for (std::int16_t v : mono)
        for (int channel = 0; channel < 2; ++channel) u16(static_cast<std::uint16_t>(v));
    return writeText(path, data);
}

// The ten start cells of every arena: the corners first, then spread over the field.
constexpr Cell kStart[kMaxPlayers] = {{0, 0}, {14, 10}, {14, 0}, {0, 10}, {6, 4}, {8, 6}, {6, 10}, {8, 0}, {0, 4}, {14, 6}};

struct Arena {
    const char* file;
    const char* title;
    int density;
    const char* rows[kGridH];  // '#' solid, ':' brick, '.' blank
};

const Arena kArenas[] = {
    {"basic", "Basic Grid (10)", 80,
     {":::::::::::::::", ":#:#:#:#:#:#:#:", ":::::::::::::::", ":#:#:#:#:#:#:#:", ":::::::::::::::", ":#:#:#:#:#:#:#:",
      ":::::::::::::::", ":#:#:#:#:#:#:#:", ":::::::::::::::", ":#:#:#:#:#:#:#:", ":::::::::::::::"}},
    {"open", "Wide Open (10)", 55,
     {":::::::::::::::", ":::::::::::::::", ":::::::::::::::", ":::::::::::::::", ":::::::::::::::", ":::::::::::::::",
      ":::::::::::::::", ":::::::::::::::", ":::::::::::::::", ":::::::::::::::", ":::::::::::::::"}},
    {"arena", "Bare Arena (10)", 0,
     {"...............", ".#.#.#.#.#.#.#.", "...............", ".#.#.#.#.#.#.#.", "...............", ".#.#.#.#.#.#.#.",
      "...............", ".#.#.#.#.#.#.#.", "...............", ".#.#.#.#.#.#.#.", "..............."}},
    {"cross", "Crossroads (10)", 75,
     {":::::::::::::::", ":##:::###:::##:", ":#:::::#:::::#:", ":::::::#:::::::", ":::::::::::::::", ":###:::::::###:",
      ":::::::::::::::", ":::::::#:::::::", ":#:::::#:::::#:", ":##:::###:::##:", ":::::::::::::::"}},
    {"rooms", "Four Rooms (10)", 70,
     {":::::::::::::::", ":::::::#:::::::", ":::::::#:::::::", ":::::::::::::::", ":::::::#:::::::", "###:#######:###",
      ":::::::#:::::::", ":::::::::::::::", ":::::::#:::::::", ":::::::#:::::::", ":::::::::::::::"}},
    {"ring", "Ring Road (10)", 70,
     {":::::::::::::::", ":::::::::::::::", "::####:::####::", "::#:::::::::#::", "::#:::###:::#::", ":::::::::::::::",
      "::#:::###:::#::", "::#:::::::::#::", "::####:::####::", ":::::::::::::::", ":::::::::::::::"}},
    {"diagonal", "Diagonals (10)", 75,
     {":::::::::::::::", ":#:::::::::::#:", "::#:::::::::#::", ":::#:::::::#:::", "::::#:::::#::::", ":::::::::::::::",
      "::::#:::::#::::", ":::#:::::::#:::", "::#:::::::::#::", ":#:::::::::::#:", ":::::::::::::::"}},
    {"lanes", "Long Lanes (10)", 85,
     {":::::::::::::::", ":#####:::#####:", ":::::::::::::::", ":#####:::#####:", ":::::::::::::::", ":::::::::::::::",
      ":::::::::::::::", ":#####:::#####:", ":::::::::::::::", ":#####:::#####:", ":::::::::::::::"}},
};

std::string arenaText(const Arena& a) {
    SchemeFile file;
    file.name = a.title;
    file.scheme.brickDensity = a.density;
    for (int y = 0; y < kGridH; ++y)
        for (int x = 0; x < kGridW; ++x) {
            const char ch = a.rows[y][x];
            file.scheme.tiles[static_cast<std::size_t>(y)][static_cast<std::size_t>(x)] = ch == '#' ? Tile::Solid : ch == ':' ? Tile::Brick : Tile::Blank;
        }
    for (int p = 0; p < kMaxPlayers; ++p) {
        const Cell c = kStart[p];
        file.scheme.start[static_cast<std::size_t>(p)] = c;
        file.team[static_cast<std::size_t>(p)] = p & 1;
        // Nobody starts inside a wall or walled in: the start cell and its neighbours are never solid.
        for (const Cell n : {c, Cell{c.x + 1, c.y}, Cell{c.x - 1, c.y}, Cell{c.x, c.y + 1}, Cell{c.x, c.y - 1}})
            if (inGrid(n) && file.scheme.tiles[static_cast<std::size_t>(n.y)][static_cast<std::size_t>(n.x)] == Tile::Solid)
                file.scheme.tiles[static_cast<std::size_t>(n.y)][static_cast<std::size_t>(n.x)] = Tile::Brick;
    }
    return serializeScheme(file);
}

// Level extras of the free set, by level (lines as in an extraN.res file).
const char* const kExtras[11] = {
    "",
    "",
    "-A,E,2,2\n-A,S,12,2\n-A,W,12,8\n-A,N,2,8\n",                                             // ice rink: arrows round the field
    "-A,E,4,0\n-A,W,10,10\n-A,S,0,4\n-A,N,14,6\n-A,E,6,5\n-A,W,8,5\n",                         // arrows
    "-T,2,2\n-T,12,8\n-T,7,5\n",                                                               // springs
    "-T,H,H\n-T,H,H\n-T,H,H\n-T,H,H\n",                                                        // springs in random places
    "-W,0,1,2,2,2\n-W,0,2,12,8,1\n-W,0,3,12,2,4\n-W,0,4,2,8,3\n",                             // two pairs of warp holes
    "",
    "-W,0,1,7,2,2\n-W,0,2,7,8,1\n-A,E,2,5\n-A,W,12,5\n",
    "-T,4,4\n-T,10,6\n-A,S,7,0\n-A,N,7,10\n",
    "-C,E,3,2\n-C,E,4,2\n-C,E,5,2\n-C,E,6,2\n-C,E,7,2\n-C,E,8,2\n-C,E,9,2\n-C,E,10,2\n-C,E,11,2\n"
    "-C,W,3,8\n-C,W,4,8\n-C,W,5,8\n-C,W,6,8\n-C,W,7,8\n-C,W,8,8\n-C,W,9,8\n-C,W,10,8\n-C,W,11,8\n",  // two conveyor belts
};

const char* const kManual =
    "ATOMIC BOMBERMAN - MODERN\n"
    "Manual of the free asset set\n"
    "\n"
    "You are playing with the free graphics, sounds and arenas that come\n"
    "with this program. If you own the original Atomic Bomberman you can\n"
    "import its data instead: atomic --import-assets PATH (see INSTALL.md).\n"
    "\n"
    "THE GAME\n"
    "Drop bombs, get out of the way, and be the last one standing.\n"
    "Bombs burst after two seconds in a cross of flame. Flames destroy\n"
    "bricks, set off other bombs and knock out anyone they touch,\n"
    "including whoever laid the bomb. Solid blocks stop them.\n"
    "When the clock runs low the walls start closing in.\n"
    "\n"
    "KEYS\n"
    "Player on KEY 0:  arrow keys, Space = bomb, Enter = action\n"
    "Player on KEY 1:  R D F G, S = bomb, A = action\n"
    "Change them under Options, Define keyboard layouts.\n"
    "In a match: Esc leaves, P pauses, F1 shows these pages.\n"
    "\n"
    "POWERUPS (hidden under bricks)\n"
    "Bomb         one more bomb at a time\n"
    "Flame        longer flames\n"
    "Gold flame   flames as long as the field\n"
    "Skate        faster\n"
    "Boot         walk into a bomb to kick it; action stops it\n"
    "Glove        action punches the bomb in front of you over a block\n"
    "Hand         bomb button on your own bomb picks it up; let go to throw\n"
    "Row of bombs bomb button on your own bomb lays a line of them\n"
    "Red button   your bombs wait for the action button\n"
    "Jelly        your kicked bombs bounce off walls\n"
    "Skull        a disease: slow, fast, no bombs, short fuse, reversed\n"
    "             keys and more. It wears off, and it is catching.\n"
    "Weight       slower\n"
    "?            any of the above\n"
    "\n"
    "LEVELS\n"
    "Some levels have arrows that turn kicked bombs, moving belts,\n"
    "springs that throw you into the air and holes that take you\n"
    "somewhere else. On the ice rink you keep sliding for a moment.\n"
    "\n"
    "NETWORK GAMES\n"
    "Start Network Game makes you the host; the others choose\n"
    "Join Network Game. Type to chat in the lobby, T in a match.\n"
    "\n"
    "HIDDEN\n"
    "Ctrl-E six times on the main menu opens the arena editor.\n";

const char* const kCredits =
    "ATOMIC BOMBERMAN - MODERN\n"
    "\n"
    "A new program that plays by the rules of Atomic Bomberman\n"
    "(Interplay, 1997), worked out by studying the original.\n"
    "\n"
    "It is an unofficial fan project and is not connected with\n"
    "Interplay, Hudson Soft or Konami.\n"
    "\n"
    "You are seeing the free asset set: its pictures, sounds, music\n"
    "and arenas are made by this program's own code and contain\n"
    "nothing from the original game. Like the program they are\n"
    "free software under the GNU General Public License, version 3\n"
    "or later.\n"
    "\n"
    "Source and documentation:\n"
    "github.com/elraro/atomic-bomberman\n";

}  // namespace

bool isFreeAssetDir(const std::string& dir) { return std::ifstream(dir + "/free-assets.txt").good(); }

bool ensureFreeAssets(const std::string& dir, bool withSounds) {
    namespace fs = std::filesystem;
    const fs::path root(dir);
    const std::string wanted = std::string(kMarker) + (withSounds ? "sounds: yes\n" : "sounds: no\n");
    {
        std::ifstream in(root / "free-assets.txt", std::ios::binary);
        std::ostringstream have;
        have << in.rdbuf();
        // A folder with sounds also serves a caller that needs none.
        if (in && (have.str() == wanted || (!withSounds && have.str() == std::string(kMarker) + "sounds: yes\n"))) return true;
    }
    std::error_code ec;
    fs::create_directories(root / "data" / "res", ec);
    fs::create_directories(root / "data" / "schemes", ec);
    fs::create_directories(root / "data" / "sound", ec);
    bool ok = true;

    std::ostringstream values;
    values << "; Tuning values of the free asset set: the program's built-in defaults.\n";
    for (const auto& [id, value] : Values::defaults().entries()) values << id << "," << value << "\n";
    ok = writeText(root / "data" / "res" / "valuelst.res", values.str()) && ok;
    for (const Arena& a : kArenas) ok = writeText(root / "data" / "schemes" / (std::string(a.file) + ".sch"), arenaText(a)) && ok;
    for (int level = 0; level < 11; ++level)
        ok = writeText(root / "data" / "res" / ("extra" + std::to_string(level) + ".res"), kExtras[level]) && ok;
    ok = writeText(root / "manual.bm", kManual) && ok;
    ok = writeText(root / "credits.bm", kCredits) && ok;

    if (withSounds) {
        const FreeSounds sounds = makeFreeSounds();
        std::ostringstream list;
        list << "; Sounds of the free asset set (id,name)\n";
        for (const auto& [id, name] : sounds.ids) list << id << "," << name << "\n";
        ok = writeText(root / "data" / "res" / "soundlst.res", list.str()) && ok;
        for (const auto& [name, samples] : sounds.samples) ok = writeWav(root / "data" / "sound" / (name + ".wav"), samples) && ok;
    }
    // The marker last: a half-written folder is written again next time.
    return ok && writeText(root / "free-assets.txt", wanted);
}

}  // namespace ab
