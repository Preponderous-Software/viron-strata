// Demo mode: a recording played back through PollCore.
#include <fstream>
#include <sstream>
#include <string>

#include "check.h"
#include "demo.h"
#include "poller.h"

using namespace viron_strata;

namespace {

const char* kRecording = R"({
  "format": "viron-strata-demo/1",
  "periodMs": 250,
  "holdMs": 500,
  "note": "test",
  "environment": {"environmentId": 3, "name": "demo world", "creationDate": "x"},
  "grids": [{"gridId": 8, "rows": 4, "columns": 4, "name": "b"}, {"gridId": 6, "rows": 4, "columns": 4, "name": "a"}],
  "entities": [{"entityId": 1, "name": "walker-1", "creationDate": "x"},
               {"entityId": 2, "name": "walker-2", "creationDate": "x"}],
  "frames": [
    [[1, 6, 100, 0, 0]],
    [[1, 6, 101, 1, 0], [2, 8, 200, 3, 3]],
    [[2, 8, 201, 3, 2]]
  ]
})";

struct Clock {
    double t = 100;
    double operator()() const { return t; }
};

}  // namespace

TEST("a demo recording parses, frames expanded to PlacementDTOs", "[unit]") {
    const Recording r = parseRecording(kRecording);
    CHECK(r.environmentId == 3);
    CHECK(r.environmentName == "demo world");
    CHECK(r.framesJson.size() == 3);
    CHECK(r.periodSeconds == 0.25);
    CHECK(r.duration() == 1.25);
    const auto p = parsePlacements(r.framesJson[1]);
    REQUIRE(p.size() == 2);
    CHECK(p[1].entityId == 2 && p[1].gridId == 8 && p[1].locationId == 200 && p[1].x == 3 && p[1].y == 3);
}

TEST("anything but a viron-strata-demo/1 recording is refused", "[unit]") {
    bool threw = false;
    try {
        parseRecording(R"({"format":"usd-replay/1"})");
    } catch (const std::exception&) {
        threw = true;
    }
    CHECK(threw);
    threw = false;
    try {
        parseRecording(R"({"format":"viron-strata-demo/1","periodMs":250,"environment":{"environmentId":1,"name":"x"},)"
                       R"("grids":[],"entities":[],"frames":[[[1,2,3]]]})");
    } catch (const std::exception&) {
        threw = true;
    }
    CHECK(threw);
}

TEST("the poll loop plays a recording: frames follow the clock, then loop", "[unit]") {
    Clock clock;
    DemoTransport demo(parseRecording(kRecording), [&clock] { return clock.t; });
    PollConfig c;
    c.environment = "demo world";  // by name, as a page might
    PollCore core(demo, c);
    CHECK(core.step() >= 0);
    REQUIRE(core.hasSnapshot());
    CHECK(core.status().state == State::Live);
    CHECK(core.snapshot().environmentId == 3);
    CHECK(core.snapshot().grids.size() == 2 && core.snapshot().grids[0].gridId == 6);
    CHECK(core.snapshot().placements.size() == 1);
    CHECK(core.snapshot().names.at(1) == "walker-1");

    clock.t += 0.3;
    core.step();
    CHECK(demo.frame() == 1);
    CHECK(core.snapshot().placements.size() == 2);
    CHECK(core.snapshot().names.at(2) == "walker-2");

    clock.t += 0.25;
    core.step();
    CHECK(demo.frame() == 2);
    clock.t += 0.5;  // inside the hold: still the last frame
    core.step();
    CHECK(demo.frame() == 2);
    CHECK(demo.loops() == 0);
    clock.t += 0.25;  // 1.3 s in: the loop restarts
    core.step();
    CHECK(demo.loops() == 1);
    CHECK(demo.frame() == 0);
    CHECK(core.snapshot().placements.size() == 1);
}

TEST("the demo transport answers unknown routes 404", "[unit]") {
    Clock clock;
    DemoTransport demo(parseRecording(kRecording), [&clock] { return clock.t; });
    CHECK(demo.get("/api/v1/environments/4").status == 404);
    CHECK(demo.get("/api/v1/entities/99").status == 404);
    CHECK(demo.get("/api/v1/entities/2").status == 200);
    CHECK(demo.get("/api/v1/environments/name/demo%20world").status == 200);
}

#ifdef VIRON_STRATA_DEMO_RECORDING
TEST("the bundled demo recording is a real run: several entities moving over many frames", "[unit]") {
    std::ifstream in(VIRON_STRATA_DEMO_RECORDING, std::ios::binary);
    REQUIRE(in.good());
    std::stringstream ss;
    ss << in.rdbuf();
    const Recording r = parseRecording(ss.str());
    CHECK(r.framesJson.size() >= 100);
    CHECK(r.entityIds.size() >= 8);
    REQUIRE(parseGrids(r.gridsJson).size() >= 2);
    size_t changed = 0;
    for (size_t i = 1; i < r.framesJson.size(); ++i) changed += r.framesJson[i] != r.framesJson[i - 1];
    CHECK(changed >= r.framesJson.size() / 4);
}
#endif
