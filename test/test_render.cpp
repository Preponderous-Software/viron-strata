// Headless render tests (EGL surfaceless; CI uses Mesa llvmpipe). The golden
// image is a fixed two-grid world at 320x200 averaged to 16x10 blocks and kept
// as text, so small rasteriser differences pass and a moved grid, a missing
// cube or a changed colour does not. Regenerate it with
// VIRON_STRATA_WRITE_GOLDEN=1 after a deliberate change to the look.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "check.h"
#include "scene.h"
#include "strata/strata.hpp"

using namespace viron_strata;

namespace {

constexpr int kW = 320, kH = 200, kBX = 16, kBY = 10;

std::unique_ptr<strata::Engine> engine() {
    strata::EngineDesc d;
    d.surface = STRATA_SURFACE_HEADLESS;
    d.width = kW;
    d.height = kH;
    return strata::Engine::create(d);
}

Snapshot fixedWorld() {
    Snapshot s;
    s.environmentId = 1;
    s.environmentName = "golden";
    s.grids = {Grid{1, 8, 8, "a", 0}, Grid{2, 6, 8, "b", 1}};
    int64_t id = 1;
    for (int i = 0; i < 12; ++i) s.placements.push_back(Placement{id++, 1, 0, (i * 3) % 8, (i * 5) % 8});
    for (int i = 0; i < 6; ++i) s.placements.push_back(Placement{id++, 2, 0, (i * 2) % 6, (i * 3 + 1) % 8});
    s.placements.push_back(Placement{id++, 2, 0, 4, 4});  // two in one cell: fanned out
    s.placements.push_back(Placement{id++, 2, 0, 4, 4});
    s.poll = 1;
    return s;
}

void draw(strata::Engine& e, Scene& scene, const LiveWorld& w, const StrataCamera& cam, double t) {
    StrataLighting light = strata::lightingDefaults(STRATA_LIGHTING_SUN);
    light.time_of_day = 0.4f;
    light.latitude_degrees = 35.0f;
    light.fog = 0;
    light.cascade_count = 2;
    e.setLighting(light);
    e.setCamera(cam);
    scene.updateFloors(e, w);
    StrataFrameDesc fd{};
    fd.clear_flags = STRATA_CLEAR_COLOR | STRATA_CLEAR_DEPTH;
    fd.clear_color[3] = 1;
    e.beginFrame(fd);
    const auto& items = scene.buildItems(w, t, false);
    e.submitPrimitives(items.data(), items.size());
    const auto& lines = scene.buildLines(w, -1, -1, -1);
    e.submitLines(lines.data(), lines.size());
    e.endFrame();
}

}  // namespace

TEST("a fixed two-grid world renders and matches the golden image", "[render]") {
    auto e = engine();
    LiveWorld w;
    w.apply(fixedWorld(), 0.0, 0.25);
    Scene scene;
    const float aspect = static_cast<float>(kW) / kH;
    const StrataCamera cam = fitCamera(w.grids(), aspect).toStrata(aspect);
    draw(*e, scene, w, cam, 1.0);
    CHECK(scene.meshCount() == 2);
    CHECK(e->frameStats().items_rejected == 0);
    CHECK(e->frameStats().instances == 2 + 20);  // two slabs + twenty cubes

    std::vector<uint8_t> rgba;
    int32_t width = 0, height = 0;
    e->readPixels(rgba, width, height);
    std::vector<int> got(static_cast<size_t>(kBX * kBY * 3));
    const int bw = width / kBX, bh = height / kBY;
    for (int by = 0; by < kBY; ++by)
        for (int bx = 0; bx < kBX; ++bx)
            for (int c = 0; c < 3; ++c) {
                long sum = 0;
                for (int y = by * bh; y < (by + 1) * bh; ++y)
                    for (int x = bx * bw; x < (bx + 1) * bw; ++x) sum += rgba[(static_cast<size_t>(y) * width + x) * 4 + c];
                got[static_cast<size_t>((by * kBX + bx) * 3 + c)] = static_cast<int>(sum / (bw * bh));
            }
    if (std::getenv("VIRON_STRATA_WRITE_GOLDEN")) {
        std::ofstream out(VIRON_STRATA_GOLDEN);
        out << "# 16x10 block means (RGB) of test_render.cpp's fixed two-grid world at 320x200\n";
        for (size_t i = 0; i < got.size(); ++i) out << got[i] << ((i + 1) % (kBX * 3) == 0 ? "\n" : " ");
        e->saveScreenshotPng("golden-two-grids.png");
        return;
    }
    std::ifstream in(VIRON_STRATA_GOLDEN);
    REQUIRE(in.good());
    std::string header;
    std::getline(in, header);
    std::vector<int> want;
    int v = 0;
    while (in >> v) want.push_back(v);
    REQUIRE(want.size() == got.size());
    int worst = 0;
    double total = 0;
    for (size_t i = 0; i < got.size(); ++i) {
        const int d = std::abs(got[i] - want[i]);
        worst = std::max(worst, d);
        total += d;
    }
    const double mean = total / static_cast<double>(got.size());
    std::printf("  golden: mean block difference %.2f, worst %d (limits 3.0, 40)\n", mean, worst);
    if (mean > 3.0 || worst > 40) e->saveScreenshotPng("golden-two-grids-actual.png");
    CHECK(mean <= 3.0);
    CHECK(worst <= 40);
}

TEST("clicking a cell picks that grid and cell", "[render]") {
    auto e = engine();
    LiveWorld w;
    w.apply(fixedWorld(), 0.0, 0.25);
    Scene scene;
    const float aspect = static_cast<float>(kW) / kH;
    const StrataCamera cam = fitCamera(w.grids(), aspect).toStrata(aspect);
    draw(*e, scene, w, cam, 1.0);
    struct Cell { int g, x, y; };
    for (const Cell& c : {Cell{0, 0, 0}, Cell{0, 7, 3}, Cell{1, 2, 6}, Cell{1, 5, 0}}) {
        const auto centre = cellCenter(layoutOf(w.grids()[static_cast<size_t>(c.g)]), c.x, c.y);
        const float p[3] = {static_cast<float>(centre[0]), 0.0f, static_cast<float>(centre[2])};
        float px = 0, py = 0;
        REQUIRE(projectToScreen(cam, kW, kH, p, px, py));
        int g = -1, x = -1, y = -1;
        CHECK(scene.pick(*e, cam, kW, kH, px, py, g, x, y));
        CHECK(g == c.g);
        CHECK(x == c.x);
        CHECK(y == c.y);
    }
}

TEST("past 200 grids the floors are batched into one mesh, and picking still works", "[render]") {
    auto e = engine();
    Snapshot s;
    s.environmentId = 1;
    for (int i = 0; i < 201; ++i) s.grids.push_back(Grid{i + 1, 2, 2, "", i});
    s.poll = 1;
    LiveWorld w;
    w.apply(s, 0.0, 0.25);
    Scene scene;
    scene.updateFloors(*e, w);
    CHECK(scene.meshCount() == 1);
    // Look straight down at grid 150, cell (1, 0).
    const auto c = cellCenter(layoutOf(w.grids()[150]), 1, 0);
    const float eye[3] = {static_cast<float>(c[0]), 10.0f, static_cast<float>(c[2]) + 0.001f};
    const float target[3] = {static_cast<float>(c[0]), 0.0f, static_cast<float>(c[2])};
    const float up[3] = {0, 1, 0};
    const StrataCamera cam = strata::cameraLookAt(eye, target, up, 40.0f, 1.6f, 0.1f, 100.0f);
    int g = -1, x = -1, y = -1;
    CHECK(scene.pick(*e, cam, kW, kH, kW / 2.0f, kH / 2.0f, g, x, y));
    CHECK(g == 150);
    CHECK(x == 1);
    CHECK(y == 0);
    scene.release(*e);
}
