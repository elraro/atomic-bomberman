#include "app/screen_keyboard.hpp"

#include <cctype>

namespace ab {

namespace {
// What the fields of the network screens need: letters, digits, and the marks of
// addresses (. : @ - _ /) and of ordinary sentences.
const char* const kRows[5] = {"1234567890", "qwertyuiop", "asdfghjkl'", "zxcvbnm,.?", "-_:@/!()+="};
}  // namespace

void ScreenKeyboard::open(bool digitsOnly) {
    shown_ = true;
    digits_ = digitsOnly;
    shift_ = false;
    row_ = digitsOnly ? 0 : 1;
    col_ = 0;
}

int ScreenKeyboard::charRows() const { return digits_ ? 1 : 5; }
int ScreenKeyboard::rows() const { return charRows() + 1; }

// The bottom row: wide keys. A field of digits has no use for Shift and Space.
int ScreenKeyboard::wideAt(int col) const {
    if (digits_) return col < 5 ? 2 : 3;
    return col < 2 ? 0 : col < 6 ? 1 : col < 8 ? 2 : 3;
}

std::string ScreenKeyboard::charAt(int row, int col) const {
    char c = kRows[row][col];
    if (shift_) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return std::string(1, c);
}

void ScreenKeyboard::move(int dir) {
    if (!shown_) return;
    const int n = rows();
    if (dir == 0) row_ = (row_ + n - 1) % n;
    if (dir == 2) row_ = (row_ + 1) % n;
    if (dir != 1 && dir != 3) return;
    if (row_ < charRows()) {
        col_ = (col_ + (dir == 1 ? 1 : kColumns - 1)) % kColumns;
        return;
    }
    // Along the wide keys: to the first column of the next one.
    const int here = wideAt(col_);
    for (int step = 1; step <= kColumns; ++step) {
        const int col = (col_ + (dir == 1 ? step : kColumns - step)) % kColumns;
        if (wideAt(col) != here) {
            col_ = col;
            while (col_ > 0 && wideAt(col_ - 1) == wideAt(col)) --col_;
            return;
        }
    }
}

ScreenKeyboard::Press ScreenKeyboard::confirm() {
    if (!shown_) return {};
    if (row_ < charRows()) return {Action::Text, charAt(row_, col_)};
    switch (wideAt(col_)) {
        case 0: shift_ = !shift_; return {};
        case 1: return {Action::Text, " "};
        case 2: return {Action::Erase, {}};
        default: return {Action::Done, {}};
    }
}

std::vector<ScreenKeyboard::Cell> ScreenKeyboard::cells() const {
    std::vector<Cell> out;
    for (int row = 0; row < charRows(); ++row)
        for (int col = 0; col < kColumns; ++col) out.push_back({charAt(row, col), row, col, 1, row == row_ && col == col_});
    const int bottom = charRows();
    const bool on = row_ == bottom;
    if (digits_) {
        out.push_back({"Erase", bottom, 0, 5, on && wideAt(col_) == 2});
        out.push_back({"Done", bottom, 5, 5, on && wideAt(col_) == 3});
    } else {
        out.push_back({shift_ ? "SHIFT" : "Shift", bottom, 0, 2, on && wideAt(col_) == 0});
        out.push_back({"Space", bottom, 2, 4, on && wideAt(col_) == 1});
        out.push_back({"Erase", bottom, 6, 2, on && wideAt(col_) == 2});
        out.push_back({"Done", bottom, 8, 2, on && wideAt(col_) == 3});
    }
    return out;
}

}  // namespace ab
