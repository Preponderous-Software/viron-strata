#include "mapping.h"

#include <cmath>
#include <string>

#include "sha256.h"

namespace viron_strata {

namespace {

uint32_t be32(const std::array<uint8_t, 32>& d, size_t at) {
    return (uint32_t(d[at]) << 24) | (uint32_t(d[at + 1]) << 16) | (uint32_t(d[at + 2]) << 8) | uint32_t(d[at + 3]);
}

std::array<uint8_t, 32> digest(int64_t entityId) { return sha256(std::to_string(entityId)); }

// Python's colorsys.hsv_to_rgb, step for step.
std::array<double, 3> hsvToRgb(double h, double s, double v) {
    if (s == 0.0) return {v, v, v};
    double i = std::floor(h * 6.0);
    const double f = h * 6.0 - i;
    const double p = v * (1.0 - s);
    const double q = v * (1.0 - s * f);
    const double t = v * (1.0 - s * (1.0 - f));
    const int k = static_cast<int>(i) % 6;
    switch (k) {
        case 0: return {v, t, p};
        case 1: return {q, v, p};
        case 2: return {p, v, t};
        case 3: return {p, q, v};
        case 4: return {t, p, v};
        default: return {v, p, q};
    }
}

}  // namespace

std::array<double, 3> gridOrigin(const GridLayout& grid) {
    return {static_cast<double>(grid.index * (grid.rows + kGridGap)), 0.0, 0.0};
}

std::array<double, 3> cellCenter(const GridLayout& grid, int x, int y) {
    const auto o = gridOrigin(grid);
    return {o[0] + x + 0.5, 0.0, o[2] + y + 0.5};
}

std::array<double, 2> entityOffset(int64_t entityId) {
    const auto d = digest(entityId);
    const double scale = 2.0 * kMaxOffset / 4294967295.0;  // 0xFFFFFFFF, as mapping.py
    return {-kMaxOffset + be32(d, 0) * scale, -kMaxOffset + be32(d, 4) * scale};
}

std::array<double, 3> entityPosition(const GridLayout& grid, int x, int y, int64_t entityId) {
    const auto c = cellCenter(grid, x, y);
    const auto o = entityOffset(entityId);
    return {c[0] + o[0], kEntitySize / 2.0, c[2] + o[1]};
}

std::array<double, 3> entityColor(int64_t entityId) {
    const auto d = digest(entityId);
    const double hue = be32(d, 8) / 4294967296.0;  // 0x100000000, as mapping.py
    return hsvToRgb(hue, 0.65, 0.9);
}

}  // namespace viron_strata
