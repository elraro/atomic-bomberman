#include "app/editor.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <map>

namespace ab {

namespace {
// Original messages 800-812, as shown in the powerup table.
const char* const kPowerText[13] = {"an extra bomb", "longer flame length", "a disease", "the ability to kick bombs", "extra speed",
                                    "the ability to punch bombs", "the ability to grab bombs", "the spooger", "goldflame",
                                    "a trigger mechanism", "jelly (bouncy) bombs", "super bad disease", "random"};

void shadowed(Renderer& r, const SpriteBank& bank, const std::string& s, float x, float y, float cr, float cg, float cb) {
    r.text(bank, s, x + 1, y + 1, 0, 0, 0);
    r.text(bank, s, x, y, cr, cg, cb);
}
}  // namespace

SchemeEditor::SchemeEditor(std::string gameSchemesDir, std::string userSchemesDir)
    : gameDir_(std::move(gameSchemesDir)), userDir_(std::move(userSchemesDir)) {}

void SchemeEditor::open() {
    mode_ = Mode::Menu;
    notice_.clear();
}

bool SchemeEditor::takeHelpRequest() {
    const bool was = helpRequested_;
    helpRequested_ = false;
    return was;
}

bool SchemeEditor::takeSaved() {
    const bool was = saved_;
    saved_ = false;
    return was;
}

// "Restores the Basic Grid Pattern" (Ctrl-B): solid at odd column and odd row, bricks elsewhere.
void SchemeEditor::basicGrid() {
    for (int y = 0; y < kGridH; ++y)
        for (int x = 0; x < kGridW; ++x)
            scheme_.scheme.tiles[static_cast<std::size_t>(y)][static_cast<std::size_t>(x)] =
                (x & 1) != 0 && (y & 1) != 0 ? Tile::Solid : Tile::Brick;
}

void SchemeEditor::newScheme() {
    scheme_ = SchemeFile{};
    scheme_.name = "No Scheme Name";  // message 727
    scheme_.scheme.brickDensity = 90;
    basicGrid();
    // The start positions and teams of the original's basic scheme.
    const Cell starts[kMaxPlayers] = {{0, 0}, {14, 10}, {0, 10}, {14, 0}, {6, 4}, {8, 0}, {12, 4}, {2, 6}, {10, 8}, {6, 10}};
    for (int p = 0; p < kMaxPlayers; ++p) {
        scheme_.scheme.start[static_cast<std::size_t>(p)] = starts[p];
        scheme_.team[static_cast<std::size_t>(p)] = p & 1;
    }
    fileName_ = "Newfile";  // message 729
    changed_ = false;
    player_ = 0;
    brush_ = Tile::Brick;
}

void SchemeEditor::prompt(std::string title, std::string initial, std::function<void(const std::string&)> done) {
    back_ = mode_;
    promptTitle_ = std::move(title);
    promptText_ = std::move(initial);
    promptDone_ = std::move(done);
    mode_ = Mode::Prompt;
}

void SchemeEditor::ask(std::string title, std::function<void(bool)> done) {
    back_ = mode_;
    promptTitle_ = std::move(title);
    questionDone_ = std::move(done);
    questionYes_ = false;
    mode_ = Mode::Question;
}

void SchemeEditor::save(const std::string& fileName) {
    std::string clean;
    for (char ch : fileName) {
        const auto c = static_cast<unsigned char>(ch);
        if (std::isalnum(c) || ch == '_' || ch == '-') clean += static_cast<char>(std::tolower(c));
    }
    if (clean.empty()) clean = "newfile";
    std::error_code ec;
    std::filesystem::create_directories(userDir_, ec);
    const std::string path = userDir_ + "/" + clean + ".sch";
    if (saveSchemeFile(path, scheme_)) {
        fileName_ = clean;
        changed_ = false;
        saved_ = true;
        notice_ = "Saved " + path;
    } else {
        notice_ = "Unable to write Scheme file '" + path + "'";  // message 710
    }
}

void SchemeEditor::leaveEdit() {
    if (!changed_) {
        mode_ = Mode::Menu;
        return;
    }
    ask("Schemefile has changed!!  Save changes?", [this](bool yes) {  // message 735
        if (!yes) {
            mode_ = Mode::Menu;
            return;
        }
        mode_ = Mode::Edit;
        prompt("Enter schemefilename (or press <Enter>):", fileName_, [this](const std::string& name) {  // message 736
            save(name.empty() ? fileName_ : name);
            mode_ = Mode::Menu;
        });
    });
}

// The original's "Modify": born-with, forbidden, override, asked one after another (messages 762-768).
void SchemeEditor::modifyPower(int type) {
    const std::string name = kPowerText[type];
    prompt("Enter bornwith for '" + name + "':", std::to_string(scheme_.powers[static_cast<std::size_t>(type)].bornWith),
           [this, type, name](const std::string& born) {
               scheme_.powers[static_cast<std::size_t>(type)].bornWith = std::clamp(std::atoi(born.c_str()), 0, 99);
               changed_ = true;
               mode_ = Mode::Powers;
               ask("Is '" + name + "' forbidden from randoms?", [this, type, name](bool forbidden) {
                   scheme_.powers[static_cast<std::size_t>(type)].forbidden = forbidden;
                   mode_ = Mode::Powers;
                   ask("Does '" + name + "' have an override?", [this, type, name](bool has) {
                       scheme_.powers[static_cast<std::size_t>(type)].hasOverride = has;
                       mode_ = Mode::Powers;
                       if (!has) return;
                       prompt("Enter override for '" + name + "':", std::to_string(scheme_.powers[static_cast<std::size_t>(type)].overrideValue),
                              [this, type](const std::string& value) {
                                  scheme_.powers[static_cast<std::size_t>(type)].overrideValue = std::clamp(std::atoi(value.c_str()), -99, 99);
                                  mode_ = Mode::Powers;
                              });
                   });
               });
           });
}

void SchemeEditor::text(const char* utf8) {
    if (mode_ != Mode::Prompt || utf8 == nullptr) return;
    for (const char* p = utf8; *p != '\0'; ++p)
        if (*p >= 32 && *p < 127 && promptText_.size() < 40) promptText_ += *p;
}

void SchemeEditor::key(unsigned key, bool ctrl) {
    if (key == SDLK_F1 && mode_ != Mode::Prompt) {
        helpRequested_ = true;
        return;
    }
    switch (mode_) {
        case Mode::Closed: break;
        case Mode::Menu:
            if (key == SDLK_1) {
                files_ = listSchemes(gameDir_, userDir_);
                fileRow_ = 0;
                mode_ = Mode::Files;
            } else if (key == SDLK_2) {
                newScheme();
                mode_ = Mode::Edit;
            } else if (key == SDLK_Q || key == SDLK_ESCAPE) {
                mode_ = Mode::Closed;
            }
            break;
        case Mode::Files: {
            const int n = static_cast<int>(files_.size());
            if (key == SDLK_UP && n > 0) fileRow_ = (fileRow_ + n - 1) % n;
            if (key == SDLK_DOWN && n > 0) fileRow_ = (fileRow_ + 1) % n;
            if (key == SDLK_PAGEUP) fileRow_ = std::max(0, fileRow_ - 15);
            if (key == SDLK_PAGEDOWN) fileRow_ = std::min(std::max(0, n - 1), fileRow_ + 15);
            if (key == SDLK_ESCAPE) mode_ = Mode::Menu;
            if (key == SDLK_RETURN && n > 0) {
                const SchemeEntry& entry = files_[static_cast<std::size_t>(fileRow_)];
                if (auto sf = loadSchemeFile(entry.path)) {
                    scheme_ = *sf;
                    fileName_ = entry.file;
                    changed_ = false;
                    player_ = 0;
                    mode_ = Mode::Edit;
                } else {
                    notice_ = "Unable to read Scheme file '" + entry.file + "'";  // message 711
                    mode_ = Mode::Menu;
                }
            }
            break;
        }
        case Mode::Edit:
            if (ctrl && key == SDLK_B) {
                basicGrid();
                changed_ = true;
            } else if (ctrl && key == SDLK_F) {
                ask("Warning!  Fill entire screen with this brick?", [this](bool yes) {  // message 760
                    mode_ = Mode::Edit;
                    if (!yes) return;
                    for (auto& row : scheme_.scheme.tiles) row.fill(brush_);
                    changed_ = true;
                });
            } else if (key == SDLK_TAB) {
                brush_ = brush_ == Tile::Blank ? Tile::Solid : brush_ == Tile::Solid ? Tile::Brick : Tile::Blank;
            } else if (key == SDLK_1) {
                brush_ = Tile::Blank;
            } else if (key == SDLK_2) {
                brush_ = Tile::Solid;
            } else if (key == SDLK_3) {
                brush_ = Tile::Brick;
            } else if (key == SDLK_PLUS || key == SDLK_EQUALS || key == SDLK_KP_PLUS) {
                player_ = (player_ + 1) % kMaxPlayers;
            } else if (key == SDLK_MINUS || key == SDLK_KP_MINUS) {
                player_ = (player_ + kMaxPlayers - 1) % kMaxPlayers;
            } else if (key == SDLK_0) {
                lineView_ = !lineView_;
            } else if (key == SDLK_N) {
                prompt("Enter new internal Scheme Name", scheme_.name, [this](const std::string& name) {  // message 728
                    if (!name.empty()) scheme_.name = name, changed_ = true;
                    mode_ = Mode::Edit;
                });
            } else if (key == SDLK_D) {
                prompt("Enter new brick density (0 - 100 percent)", std::to_string(scheme_.scheme.brickDensity),  // message 739
                       [this](const std::string& value) {
                           if (!value.empty()) scheme_.scheme.brickDensity = std::clamp(std::atoi(value.c_str()), 0, 100), changed_ = true;
                           mode_ = Mode::Edit;
                       });
            } else if (key == SDLK_P) {
                powerRow_ = 0;
                mode_ = Mode::Powers;
            } else if (key == SDLK_T) {
                int& team = scheme_.team[static_cast<std::size_t>(player_)];
                team = team == 0 ? 1 : 0;
                changed_ = true;
            } else if (key == SDLK_ESCAPE) {
                leaveEdit();
            }
            break;
        case Mode::Powers:
            if (key == SDLK_UP) powerRow_ = (powerRow_ + 12) % 13;
            if (key == SDLK_DOWN) powerRow_ = (powerRow_ + 1) % 13;
            if (key == SDLK_RETURN || key == SDLK_M) modifyPower(powerRow_);
            if (key == SDLK_ESCAPE) mode_ = Mode::Edit;
            break;
        case Mode::Prompt:
            if (key == SDLK_BACKSPACE && !promptText_.empty()) promptText_.pop_back();
            if (key == SDLK_ESCAPE) mode_ = back_;
            if (key == SDLK_RETURN || key == SDLK_KP_ENTER) {
                const auto done = std::move(promptDone_);
                const std::string value = promptText_;
                mode_ = back_;
                if (done) done(value);
            }
            break;
        case Mode::Question: {
            if (key == SDLK_LEFT || key == SDLK_RIGHT || key == SDLK_TAB) questionYes_ = !questionYes_;
            const bool enter = key == SDLK_RETURN || key == SDLK_SPACE;
            const bool yes = key == SDLK_Y || (enter && questionYes_);
            const bool no = key == SDLK_N || key == SDLK_ESCAPE || (enter && !questionYes_);
            if (yes || no) {
                const auto done = std::move(questionDone_);
                mode_ = back_;
                if (done) done(yes);
            }
            break;
        }
    }
}

void SchemeEditor::mouse(float x, float y, bool left, bool right) {
    if (mode_ != Mode::Edit) return;
    const int cx = static_cast<int>(x - static_cast<float>(kOriginX)) / kCellW;
    const int cy = static_cast<int>(y - static_cast<float>(kOriginY)) / kCellH;
    if (x < static_cast<float>(kOriginX) || y < static_cast<float>(kOriginY) || cx >= kGridW || cy >= kGridH) return;
    if (left) {
        Tile& t = scheme_.scheme.tiles[static_cast<std::size_t>(cy)][static_cast<std::size_t>(cx)];
        if (t != brush_) t = brush_, changed_ = true;
    }
    if (right) {
        Cell& start = scheme_.scheme.start[static_cast<std::size_t>(player_)];
        if (!(start == Cell{cx, cy})) start = {cx, cy}, changed_ = true;
    }
}

void SchemeEditor::draw(Renderer& r, SpriteBank& bank, int level, int frame) {
    const Mode shown = mode_ == Mode::Prompt || mode_ == Mode::Question ? back_ : mode_;
    r.image(0);
    if (shown == Mode::Menu) {
        shadowed(r, bank, "Atomic Bomberman Level Editor", 50, 100, 1.0f, 0.95f, 0.3f);  // message 700 at value 810
        const char* const lines[4] = {"Editor Main Menu.  You may:", "1) Edit a scheme file", "2) Create a new scheme file", "Q) Exit"};  // 730-733
        for (int i = 0; i < 4; ++i) shadowed(r, bank, lines[i], 80, 140.0f + 20.0f * static_cast<float>(i), 1, 1, 1);  // value 815
        if (!notice_.empty()) shadowed(r, bank, notice_, 50, 260, 0.4f, 1.0f, 1.0f);
        shadowed(r, bank, "Press F1 for help", 250, 440, 0.4f, 1.0f, 1.0f);  // message 330
    } else if (shown == Mode::Files) {
        shadowed(r, bank, files_.empty() ? "No Scheme files found!" : "Available Scheme Files:", 50, 30, 1, 1, 1);  // messages 720, 721
        const int first = std::max(0, std::min(fileRow_ - 8, static_cast<int>(files_.size()) - 17));
        for (int row = 0; row < 17 && first + row < static_cast<int>(files_.size()); ++row) {
            const SchemeEntry& entry = files_[static_cast<std::size_t>(first + row)];
            const bool on = first + row == fileRow_;
            shadowed(r, bank, entry.file + "   " + entry.title, 80, 60.0f + 22.0f * static_cast<float>(row), on ? 1.0f : 0.85f, on ? 0.95f : 0.85f,
                     on ? 0.3f : 0.85f);
            if (on) r.sprite(bank, "cursor1", frame / 8, -1, 62.0f, 75.0f + 22.0f * static_cast<float>(row));
        }
        shadowed(r, bank, "Up/Down: select   Enter: edit   Esc: back", 60, 440, 0.4f, 1.0f, 1.0f);
    } else if (shown == Mode::Edit) {
        const std::string lv = std::to_string(lineView_ ? -1 : level);
        for (int y = 0; y < kGridH; ++y)
            for (int x = 0; x < kGridW; ++x) {
                const Tile t = scheme_.scheme.tiles[static_cast<std::size_t>(y)][static_cast<std::size_t>(x)];
                const auto px = static_cast<float>(kOriginX + x * kCellW), py = static_cast<float>(kOriginY + y * kCellH);
                r.quad(px, py, static_cast<float>(kCellW - 1), static_cast<float>(kCellH - 1), 0.10f, 0.22f, 0.12f);
                const char* kind = t == Tile::Solid ? " solid" : t == Tile::Brick ? " brick" : nullptr;
                if (kind != nullptr && !r.sprite(bank, "tile " + lv + kind, 0, -1, static_cast<float>(cellToPixelX(x)), static_cast<float>(cellToPixelY(y))))
                    r.quad(px, py, static_cast<float>(kCellW - 1), static_cast<float>(kCellH - 1), t == Tile::Solid ? 0.45f : 0.60f,
                           t == Tile::Solid ? 0.45f : 0.35f, t == Tile::Solid ? 0.50f : 0.15f);
            }
        // Start positions: the player number, white or red by team; the selected one is marked.
        for (int p = 0; p < kMaxPlayers; ++p) {
            const Cell c = scheme_.scheme.start[static_cast<std::size_t>(p)];
            if (!inGrid(c)) continue;
            const auto px = static_cast<float>(kOriginX + c.x * kCellW), py = static_cast<float>(kOriginY + c.y * kCellH);
            const bool red = scheme_.team[static_cast<std::size_t>(p)] != 0;
            if (p == player_) r.quad(px + 6, py + 6, 28, 24, 1.0f, 0.9f, 0.2f, 0.9f);
            r.quad(px + 8, py + 8, 24, 20, 0.0f, 0.0f, 0.0f, 0.75f);
            r.text(bank, std::to_string(p + 1), px + (p >= 9 ? 11.0f : 15.0f), py + 10, 1.0f, red ? 0.25f : 1.0f, red ? 0.25f : 1.0f);
        }
        shadowed(r, bank, "Schemfile: " + fileName_ + " - Name: '" + scheme_.name + "'" + (changed_ ? " *" : ""), 8, 4, 1, 1, 1);  // message 742
        const char* const brush = brush_ == Tile::Blank ? "Open Space" : brush_ == Tile::Solid ? "Unbreakable" : "Breakable";
        shadowed(r, bank,
                 "Brick Density: " + std::to_string(scheme_.scheme.brickDensity) + "%   Player: " + std::to_string(player_ + 1) + " (" +
                     (scheme_.team[static_cast<std::size_t>(player_)] != 0 ? "red" : "white") + ")   Brick: " + brush,
                 8, 24, 1.0f, 0.95f, 0.3f);  // messages 743, 738
        shadowed(r, bank, "Schemefile Editing - press F1 for editor help", 8, 44, 0.4f, 1.0f, 1.0f);  // message 737
    } else if (shown == Mode::Powers) {
        shadowed(r, bank, "Powertype table modifier editor", 50, 30, 1.0f, 0.95f, 0.3f);  // message 754
        for (int t = 0; t < 13; ++t) {
            const SchemePower& pw = scheme_.powers[static_cast<std::size_t>(t)];
            const bool on = t == powerRow_;
            const float y = 64.0f + 26.0f * static_cast<float>(t);
            const std::string line = std::string("'") + kPowerText[t] + "'   Bornwith: " + std::to_string(pw.bornWith) + ", Forbid: " +
                                     (pw.forbidden ? "Yes" : "No") + "   " +
                                     (pw.hasOverride ? "Override: " + std::to_string(pw.overrideValue) : std::string("No Override"));  // 756-759
            shadowed(r, bank, line, 70, y, on ? 1.0f : 0.85f, on ? 0.95f : 0.85f, on ? 0.3f : 0.85f);
            if (on) r.sprite(bank, "cursor1", frame / 8, -1, 52.0f, y + 15.0f);
        }
        shadowed(r, bank, "Up/Down: select   Enter: Modify   Esc: back", 60, 440, 0.4f, 1.0f, 1.0f);
    }
    if (mode_ == Mode::Prompt || mode_ == Mode::Question) {
        r.quad(60, 190, 520, 96, 0.05f, 0.05f, 0.25f, 0.96f);
        r.text(bank, promptTitle_, 320.0f - r.textWidth(bank, promptTitle_) / 2.0f, 202, 1.0f, 0.95f, 0.3f);
        if (mode_ == Mode::Prompt) {
            const std::string shownText = promptText_ + ((frame / 20) % 2 == 0 ? "_" : " ");
            r.text(bank, shownText, 320.0f - r.textWidth(bank, shownText) / 2.0f, 236, 1, 1, 1);
        } else {
            r.text(bank, "No", 250, 244, questionYes_ ? 0.6f : 1.0f, questionYes_ ? 0.6f : 0.95f, questionYes_ ? 0.6f : 0.3f);  // messages 25, 26
            r.text(bank, "Yes", 360, 244, questionYes_ ? 1.0f : 0.6f, questionYes_ ? 0.95f : 0.6f, questionYes_ ? 0.3f : 0.6f);
        }
    }
}

}  // namespace ab
