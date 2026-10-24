// Viron coordinates -> Strata world positions and colours, with viron-usd's
// conventions (src/viron_usd/mapping.py), so a live view and a viron-usd
// recording of the same run line up. Pure functions; no HTTP, no GL.
//
// viron-usd is Z-up: Viron x -> USD +X, Viron y -> USD +Y. Strata is Y-up, so
// here Viron x -> Strata +X and Viron y -> Strata +Z. Everything else is the
// same: one cell is one unit; grid i (in gridId order) sits at
// x = i * (rows + GRID_GAP); a cell's centre is origin + (x + 0.5, y + 0.5);
// an entity is a 0.4 cube resting on the floor, offset inside its cell by the
// first 8 bytes of SHA-256 of its decimal id and coloured
// HSV(bytes 8..11 / 2^32, 0.65, 0.9).
//
// Colour space: viron-usd writes those values to displayColor, which USD
// treats as linear; Strata reads linear RGB too, so the same numbers are
// passed through unchanged and the two views look alike.
//
// test/conformance/viron_usd_mapping.json, generated from viron-usd's own
// mapping.py by tools/gen_conformance.py, pins this in CI.
#pragma once

#include <array>
#include <cstdint>

namespace viron_strata {

constexpr int kGridGap = 2;
constexpr double kEntitySize = 0.4;
constexpr double kMaxOffset = 0.25;

struct GridLayout {
    int index = 0;    // position in gridId order
    int rows = 0;     // extent along Viron x
    int columns = 0;  // extent along Viron y
};

// viron-usd's grid_origin: (index * (rows + GRID_GAP), 0, 0), in Strata axes.
std::array<double, 3> gridOrigin(const GridLayout& grid);
// viron-usd's cell_center, in Strata axes: (ox + x + 0.5, 0, y + 0.5).
std::array<double, 3> cellCenter(const GridLayout& grid, int x, int y);
// viron-usd's entity_offset (dx, dy) in [-0.25, 0.25]; Strata applies dx to X and dy to Z.
std::array<double, 2> entityOffset(int64_t entityId);
// viron-usd's entity_position, in Strata axes: the cube's centre (height 0.2).
std::array<double, 3> entityPosition(const GridLayout& grid, int x, int y, int64_t entityId);
// viron-usd's entity_color: linear RGB.
std::array<double, 3> entityColor(int64_t entityId);

}  // namespace viron_strata
