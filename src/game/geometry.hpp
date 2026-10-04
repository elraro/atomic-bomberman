// Grid geometry and coordinate conversions.
// Spec: docs/specifications/maps.md, docs/specifications/physics.md
#pragma once

#include <array>

namespace ab {

inline constexpr int kGridW = 15;
inline constexpr int kGridH = 11;
inline constexpr int kCellW = 40;
inline constexpr int kCellH = 36;
inline constexpr int kOriginX = 20;
inline constexpr int kOriginY = 68;

// 0 north, 1 east, 2 south, 3 west; -1 none.
using Dir = int;
inline constexpr Dir kNoDir = -1;
inline constexpr std::array<int, 4> kDx{0, 1, 0, -1};
inline constexpr std::array<int, 4> kDy{-1, 0, 1, 0};

constexpr Dir opposite(Dir d) { return (d + 2) & 3; }
constexpr Dir turnRight(Dir d) { return (d + 1) & 3; }
constexpr Dir turnLeft(Dir d) { return (d + 3) & 3; }

struct Cell {
    int x = 0;
    int y = 0;
    friend constexpr bool operator==(Cell, Cell) = default;
};

constexpr Cell step(Cell c, Dir d) { return {c.x + kDx[static_cast<unsigned>(d)], c.y + kDy[static_cast<unsigned>(d)]}; }
constexpr bool inGrid(Cell c) { return c.x >= 0 && c.x < kGridW && c.y >= 0 && c.y < kGridH; }

// Reference point of a cell: horizontal centre, bottom row.
constexpr int cellToPixelX(int cx) { return kOriginX + kCellW / 2 + cx * kCellW; }
constexpr int cellToPixelY(int cy) { return kOriginY + kCellH - 1 + cy * kCellH; }

// The two functions below keep the original's treatment of points left of /
// above the playfield (used only by bombs flying off the grid).
constexpr int pixelToCellX(int px) {
    int v = px < kOriginX ? px - kOriginX : px;
    return (v - kOriginX) / kCellW;
}
constexpr int pixelToCellY(int py) {
    const int half = kCellH / 2 - 1;
    int v = (py - kOriginY - half) < 0 ? py - (kOriginY - 1) : py;
    return (v - kOriginY - half) / kCellH;
}
constexpr Cell pixelToCell(int px, int py) { return {pixelToCellX(px), pixelToCellY(py)}; }

// Offset from the cell reference point: x in [-20, 19], y in [-18, 17].
constexpr int offsetInCellX(int px) {
    int v = px;
    while (v < kOriginX) v += kCellW;
    return (v - kOriginX) % kCellW - kCellW / 2;
}
constexpr int offsetInCellY(int py) {
    const int half = kCellH / 2 - 1;
    int v = py;
    while (v - kOriginY - half < 0) v += kCellH;
    return (v - kOriginY - half) % kCellH - (kCellH - 1) + half;
}

}  // namespace ab
