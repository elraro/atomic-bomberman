// The scheme (level) editor, after the original's "secret level editor"
// described in its own editor.bm: Ctrl-E six times on the main menu.
// Texts are the original's messages 700-768; the screen layout is this
// implementation's. Edited schemes are saved to the user's scheme folder,
// never into the original game files.
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "rendering/renderer.hpp"
#include "rendering/sprites.hpp"
#include "resources/scheme_file.hpp"

namespace ab {

struct SchemeEntry {
    std::string file;   // name without extension
    std::string title;  // the scheme's own name
    std::string path;
};

// Schemes of the game data and of the user's folder (a user file hides a
// game file of the same name), sorted by file name.
std::vector<SchemeEntry> listSchemes(const std::string& gameSchemesDir, const std::string& userSchemesDir);

class SchemeEditor {
public:
    SchemeEditor(std::string gameSchemesDir, std::string userSchemesDir);

    void open();
    bool finished() const { return mode_ == Mode::Closed; }
    bool takeHelpRequest();                      // F1 was pressed
    bool wantsTextInput() const { return mode_ == Mode::Prompt; }
    bool takeSaved();                            // a file was written since the last call

    void key(unsigned key, bool ctrl);           // an SDL keycode
    void text(const char* utf8);                 // typed characters, while wantsTextInput()
    void mouse(float x, float y, bool left, bool right);  // a held button at a logical screen position
    void draw(Renderer& r, SpriteBank& bank, int level, int frame);

private:
    enum class Mode { Closed, Menu, Files, Edit, Powers, Prompt, Question };

    void newScheme();
    void basicGrid();
    void prompt(std::string title, std::string initial, std::function<void(const std::string&)> done);
    void ask(std::string title, std::function<void(bool)> done);
    void leaveEdit();
    void save(const std::string& fileName);
    void modifyPower(int type);

    std::string gameDir_;
    std::string userDir_;
    Mode mode_ = Mode::Closed;
    Mode back_ = Mode::Menu;       // where a prompt or question returns to
    std::vector<SchemeEntry> files_;
    int fileRow_ = 0;

    SchemeFile scheme_;
    std::string fileName_ = "Newfile";
    bool changed_ = false;
    Tile brush_ = Tile::Brick;
    int player_ = 0;
    bool lineView_ = false;
    int powerRow_ = 0;

    std::string promptTitle_;
    std::string promptText_;
    std::function<void(const std::string&)> promptDone_;
    std::function<void(bool)> questionDone_;
    bool questionYes_ = false;
    std::string notice_;           // one-line message shown on the menu (e.g. a write error)
    bool helpRequested_ = false;
    bool saved_ = false;
};

}  // namespace ab
