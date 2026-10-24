// LiveWorld: tweening, snapping, appearing and disappearing.
#include <cmath>

#include "check.h"
#include "scene.h"

using namespace viron_strata;

namespace {

bool near(double a, double b, double eps = 1e-9) { return std::fabs(a - b) <= eps; }

Snapshot world(std::vector<Placement> placements) {
    Snapshot s;
    s.environmentId = 4;
    s.environmentName = "Meadow";
    s.grids = {Grid{2, 5, 4, "west", 0}, Grid{9, 3, 3, "east", 1}};
    s.placements = std::move(placements);
    s.names = {{1, "alice"}, {7, "bob"}};
    s.poll = 1;
    return s;
}

Placement at(int64_t id, int64_t grid, int x, int y) { return Placement{id, grid, 0, x, y}; }

const Track* find(const LiveWorld& w, int64_t id) {
    for (const Track& t : w.tracks())
        if (t.entityId == id) return &t;
    return nullptr;
}

}  // namespace

TEST("positions follow viron-usd's layout on the right grid", "[unit]") {
    LiveWorld w;
    w.apply(world({at(1, 2, 0, 1), at(7, 9, 2, 2)}), 10.0, 0.25);
    REQUIRE(w.placed() == 2);
    const Track* bob = find(w, 7);
    REQUIRE(bob);
    CHECK(bob->gridIndex == 1);
    const auto expected = entityPosition(GridLayout{1, 3, 3}, 2, 2, 7);
    const auto p = w.position(*bob, 10.0);
    CHECK(near(p[0], expected[0]) && near(p[1], expected[1]) && near(p[2], expected[2]));
    CHECK(near(p[0], 1 * (3 + 2) + 2.5 + entityOffset(7)[0]));
    CHECK(bob->name == "bob");
    CHECK(w.at(1, 2, 2).size() == 1);
}

TEST("a one-cell move slides over one poll period; a jump or a grid change snaps", "[unit]") {
    LiveWorld w;
    w.apply(world({at(1, 2, 0, 1), at(7, 9, 2, 2)}), 10.0, 0.25);
    w.apply(world({at(1, 2, 1, 1), at(7, 2, 3, 3)}), 11.0, 0.25);
    const Track* a = find(w, 1);
    const Track* b = find(w, 7);
    REQUIRE(a && b);
    const auto from = entityPosition(GridLayout{0, 5, 4}, 0, 1, 1);
    const auto to = entityPosition(GridLayout{0, 5, 4}, 1, 1, 1);
    CHECK(near(w.position(*a, 11.0)[0], from[0]));
    CHECK(near(w.position(*a, 11.125)[0], (from[0] + to[0]) / 2));
    CHECK(near(w.position(*a, 11.25)[0], to[0]));
    CHECK(near(w.position(*a, 20.0)[0], to[0]));
    CHECK(b->snap);
    CHECK(near(w.position(*b, 11.0)[0], entityPosition(GridLayout{0, 5, 4}, 3, 3, 7)[0]));
}

TEST("a new entity grows in over 150 ms; a removed one shrinks out and is dropped", "[unit]") {
    LiveWorld w;
    w.apply(world({at(1, 2, 0, 1)}), 10.0, 0.25);
    const Track* a = find(w, 1);
    REQUIRE(a);
    CHECK(near(w.grow(*a, 10.0), 0.0));
    CHECK(near(w.grow(*a, 10.075), 0.5, 1e-6));
    CHECK(near(w.grow(*a, 10.2), 1.0));
    w.apply(world({}), 11.0, 0.25);
    a = find(w, 1);
    REQUIRE(a);  // still shrinking
    CHECK(w.placed() == 0);
    CHECK(near(w.grow(*a, 11.075), 0.5, 1e-6));
    w.prune(11.2);
    CHECK(find(w, 1) == nullptr);
}

TEST("an entity that comes back while shrinking starts a fresh track", "[unit]") {
    LiveWorld w;
    w.apply(world({at(1, 2, 0, 1)}), 10.0, 0.25);
    w.apply(world({}), 11.0, 0.25);
    w.apply(world({at(1, 2, 4, 3)}), 11.05, 0.25);
    CHECK(w.placed() == 1);
    size_t n = 0;
    for (const Track& t : w.tracks()) n += t.entityId == 1;
    CHECK(n == 1);
    CHECK(find(w, 1)->x == 4);
}

TEST("the grid revision changes only when the layout does", "[unit]") {
    LiveWorld w;
    w.apply(world({}), 1.0, 0.25);
    const uint64_t r = w.gridRevision();
    w.apply(world({at(1, 2, 0, 0)}), 2.0, 0.25);
    CHECK(w.gridRevision() == r);
    Snapshot s = world({});
    s.grids[1].name = "renamed";
    w.apply(s, 3.0, 0.25);
    CHECK(w.gridRevision() == r + 1);
}
