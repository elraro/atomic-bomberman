// The on-screen keyboard: a grid of keys worked with the arrows and a confirm
// button, for typing a name, an address, a password or a chat line with a
// gamepad. It only says what was pressed; the screen that shows it owns the text.
#pragma once

#include <string>
#include <vector>

namespace ab {

class ScreenKeyboard {
public:
    enum class Action { None, Text, Erase, Done };
    struct Press {
        Action action = Action::None;
        std::string text;  // for Text
    };
    // One key as drawn: its place in a grid ten columns wide.
    struct Cell {
        std::string label;
        int row = 0, col = 0, span = 1;
        bool selected = false;
    };

    static constexpr int kColumns = 10;

    void open(bool digitsOnly);
    void close() { shown_ = false; }
    bool shown() const { return shown_; }
    bool shifted() const { return shift_; }

    // dir: 0 up, 1 right, 2 down, 3 left. Both ways wrap round.
    void move(int dir);
    // The selected key is pressed.
    Press confirm();
    int rows() const;
    std::vector<Cell> cells() const;

private:
    struct Wide {
        const char* label;
        int col, span;
    };
    int charRows() const;
    int wideAt(int col) const;  // which key of the bottom row is under that column
    std::string charAt(int row, int col) const;

    bool shown_ = false;
    bool digits_ = false;
    bool shift_ = false;
    int row_ = 0, col_ = 0;
};

}  // namespace ab
