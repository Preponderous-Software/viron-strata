// The poll loop against a fake Viron: which requests are made, and how each
// answer is handled (RFC 0030, section 2).
#include <deque>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "check.h"
#include "poller.h"

using namespace viron_strata;

namespace {

// A scripted Viron. Transport has only get(), so this fake can only ever be
// asked for a GET: the read-only rule is enforced by the type.
class FakeViron : public Transport {
public:
    std::map<std::string, Response> routes;           // path -> standing answer
    std::map<std::string, std::deque<Response>> once;  // path -> answers used first, in order
    std::vector<std::string> log;

    Response get(const std::string& path) override {
        log.push_back(path);
        auto q = once.find(path);
        if (q != once.end() && !q->second.empty()) {
            Response r = q->second.front();
            q->second.pop_front();
            return r;
        }
        auto it = routes.find(path);
        if (it != routes.end()) return it->second;
        Response r;
        r.status = 404;
        r.body = R"({"status":404,"message":"not found"})";
        return r;
    }
    size_t count(const std::string& prefix) const {
        size_t n = 0;
        for (const auto& p : log) n += p.rfind(prefix, 0) == 0;
        return n;
    }
};

Response ok(const std::string& body) {
    Response r;
    r.status = 200;
    r.body = body;
    return r;
}

Response status(int code, std::map<std::string, std::string> headers = {}) {
    Response r;
    r.status = code;
    r.headers = std::move(headers);
    r.body = "{}";
    return r;
}

const char* kPlacements = "/api/v1/locations/placements?environmentId=4";

void standardWorld(FakeViron& v) {
    v.routes["/api/v1/environments/4"] = ok(R"({"environmentId":4,"name":"Meadow","creationDate":"x"})");
    v.routes["/api/v1/environments/name/Meadow"] = v.routes["/api/v1/environments/4"];
    v.routes["/api/v1/grids/environment/4"] =
        ok(R"([{"gridId":9,"rows":3,"columns":3,"name":"east"},{"gridId":2,"rows":5,"columns":4,"name":"west"}])");
    v.routes[kPlacements] = ok(
        R"([{"entityId":1,"environmentId":4,"gridId":2,"locationId":11,"x":0,"y":1},)"
        R"({"entityId":7,"environmentId":4,"gridId":9,"locationId":40,"x":2,"y":2}])");
    v.routes["/api/v1/entities/environment/4"] =
        ok(R"([{"entityId":1,"name":"alice","creationDate":"x"},{"entityId":7,"name":"bob","creationDate":"x"}])");
}

PollConfig config(const std::string& env = "4") {
    PollConfig c;
    c.environment = env;
    c.periodSeconds = 0.25;
    c.gridEvery = 3;
    c.nameEvery = 5;
    return c;
}

}  // namespace

TEST("the first poll resolves the environment, reads grids, placements and names", "[unit]") {
    FakeViron v;
    standardWorld(v);
    PollCore core(v, config());
    const double wait = core.step();
    CHECK(wait >= 0 && wait <= 0.25);
    REQUIRE(core.hasSnapshot());
    const Snapshot& s = core.snapshot();
    CHECK(s.environmentId == 4);
    CHECK(s.environmentName == "Meadow");
    REQUIRE(s.grids.size() == 2);
    CHECK(s.grids[0].gridId == 2 && s.grids[0].index == 0);  // gridId order
    CHECK(s.grids[1].gridId == 9 && s.grids[1].index == 1);
    CHECK(s.grids[0].rows == 5 && s.grids[0].columns == 4 && s.grids[0].name == "west");
    REQUIRE(s.placements.size() == 2);
    CHECK(s.placements[1].entityId == 7 && s.placements[1].x == 2 && s.placements[1].y == 2);
    CHECK(s.names.at(1) == "alice");
    CHECK(core.status().state == State::Live);
    CHECK(v.log.size() == 4);
}

TEST("a name resolves the environment by name (URL-encoded)", "[unit]") {
    FakeViron v;
    standardWorld(v);
    v.routes["/api/v1/environments/name/Big%20Meadow"] = v.routes["/api/v1/environments/4"];
    PollCore core(v, config("Big Meadow"));
    core.step();
    CHECK(core.status().state == State::Live);
    CHECK(v.log.front() == "/api/v1/environments/name/Big%20Meadow");
}

TEST("steady state is one request per poll; grids every K, names every M", "[unit]") {
    FakeViron v;
    standardWorld(v);
    PollCore core(v, config());
    core.step();
    v.log.clear();
    core.step();  // poll 2
    CHECK(v.log.size() == 1);
    CHECK(v.log[0] == kPlacements);
    v.log.clear();
    core.step();  // poll 3: polls_ == 2, not due
    core.step();  // poll 4: polls_ == 3 -> grids (and the environment) refreshed
    CHECK(v.count("/api/v1/grids/") == 1);
    CHECK(v.count("/api/v1/environments/4") == 1);
    v.log.clear();
    core.step();  // poll 5
    core.step();  // poll 6: polls_ == 5 -> names refreshed
    CHECK(v.count("/api/v1/entities/environment/4") == 1);
}

TEST("a new entity id fetches names at once; a still-missing one falls back to /entities/{id}", "[unit]") {
    FakeViron v;
    standardWorld(v);
    PollCore core(v, config());
    core.step();
    v.routes[kPlacements] = ok(
        R"([{"entityId":1,"environmentId":4,"gridId":2,"locationId":11,"x":0,"y":1},)"
        R"({"entityId":12,"environmentId":4,"gridId":2,"locationId":12,"x":1,"y":1}])");
    v.routes["/api/v1/entities/12"] = ok(R"({"entityId":12,"name":"carol","creationDate":"x"})");
    v.log.clear();
    core.step();
    CHECK(v.count("/api/v1/entities/environment/4") == 1);
    CHECK(v.count("/api/v1/entities/12") == 1);
    CHECK(core.snapshot().names.at(12) == "carol");
}

TEST("401 stops polling with 'token rejected'", "[unit]") {
    FakeViron v;
    standardWorld(v);
    PollCore core(v, config());
    core.step();
    v.once[kPlacements].push_back(status(401));
    CHECK(core.step() < 0);
    CHECK(core.status().state == State::TokenRejected);
    CHECK(core.status().message.find("token rejected") != std::string::npos);
    const size_t n = v.log.size();
    CHECK(core.step() < 0);
    CHECK(v.log.size() == n);  // stopped: no more requests
    CHECK(core.hasSnapshot());  // the last world is kept
}

TEST("404 for the environment stops with 'environment gone'", "[unit]") {
    FakeViron v;
    standardWorld(v);
    PollCore core(v, config());
    core.step();
    core.step();
    core.step();
    v.routes.erase("/api/v1/environments/4");  // deleted: the K-th poll notices
    CHECK(core.step() < 0);
    CHECK(core.status().state == State::EnvironmentGone);

    FakeViron none;
    PollCore missing(none, config("99"));
    CHECK(missing.step() < 0);
    CHECK(missing.status().state == State::EnvironmentGone);
}

TEST("503 with Retry-After waits as told; 5xx and refusals back off exponentially to 10 s", "[unit]") {
    FakeViron v;
    standardWorld(v);
    PollCore core(v, config());
    core.step();
    v.once[kPlacements].push_back(status(503, {{"retry-after", "3"}}));
    CHECK(core.step() == 3.0);
    CHECK(core.status().state == State::Busy);
    std::vector<double> waits;
    for (int i = 0; i < 7; ++i) {
        Response refused;
        refused.status = 0;
        refused.error = "Couldn't connect to server";
        v.once[kPlacements].push_back(i % 2 ? status(500) : refused);
    }
    for (int i = 0; i < 7; ++i) waits.push_back(core.step());
    CHECK(core.status().state == State::Stale);
    CHECK(waits[0] == 1.0);  // the 503 counted as failure 1
    CHECK(waits[1] == 2.0);
    CHECK(waits[2] == 4.0);
    CHECK(waits[3] == 8.0);
    CHECK(waits[4] == 10.0);
    CHECK(waits[6] == 10.0);
    const double back = core.step();  // the route answers again
    CHECK(back >= 0 && back <= 0.25);
    CHECK(core.status().state == State::Live);
    CHECK(core.status().failures == 0);
}

TEST("a Viron without /locations/placements stops with a clear message", "[unit]") {
    FakeViron v;
    standardWorld(v);
    v.routes[kPlacements] = status(400);
    PollCore core(v, config());
    CHECK(core.step() < 0);
    CHECK(core.status().state == State::Unsupported);
    CHECK(core.status().message.find("c96362b") != std::string::npos);
}

TEST("the parsers reject a body that does not match the schema", "[unit]") {
    bool threw = false;
    try {
        parsePlacements(R"([{"entityId":"one"}])");
    } catch (const std::exception&) {
        threw = true;
    }
    CHECK(threw);
    CHECK(parsePlacements("[]").empty());
    const auto names = parseEntityNames(R"({"entityId":5,"name":"solo"})");
    CHECK(names.at(5) == "solo");
}
