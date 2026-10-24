// AsyncPoll: PollCore's step taken one fetch at a time, as the browser does.
// Every test runs the same scripted Viron through PollCore directly and
// through AsyncPoll, and checks that both make the same requests and end in
// the same state.
#include <deque>
#include <map>
#include <string>
#include <vector>

#include "async_poll.h"
#include "check.h"
#include "poller.h"

using namespace viron_strata;

namespace {

class Script : public Transport {
public:
    std::map<std::string, std::deque<Response>> answers;  // the last answer for a path repeats
    std::vector<std::string> log;
    Response get(const std::string& path) override {
        log.push_back(path);
        auto it = answers.find(path);
        if (it == answers.end() || it->second.empty()) {
            Response r;
            r.status = 404;
            r.body = "{}";
            return r;
        }
        Response r = it->second.front();
        if (it->second.size() > 1) it->second.pop_front();
        return r;
    }
};

Response ok(const std::string& body) {
    Response r;
    r.status = 200;
    r.body = body;
    return r;
}

Response code(int status, std::map<std::string, std::string> headers = {}) {
    Response r;
    r.status = status;
    r.headers = std::move(headers);
    r.body = "{}";
    return r;
}

const std::string kPlacements = "/api/v1/locations/placements?environmentId=4";

Script world() {
    Script s;
    s.answers["/api/v1/environments/4"] = {ok(R"({"environmentId":4,"name":"Meadow","creationDate":"x"})")};
    s.answers["/api/v1/grids/environment/4"] = {ok(R"([{"gridId":2,"rows":5,"columns":4,"name":"west"}])")};
    s.answers[kPlacements] = {
        ok(R"([{"entityId":1,"environmentId":4,"gridId":2,"locationId":11,"x":0,"y":1}])"),
        ok(R"([{"entityId":1,"environmentId":4,"gridId":2,"locationId":12,"x":1,"y":1},)"
           R"({"entityId":5,"environmentId":4,"gridId":2,"locationId":13,"x":2,"y":1}])"),
        code(503, {{"retry-after", "2"}}),
        code(0),
        ok(R"([{"entityId":5,"environmentId":4,"gridId":2,"locationId":13,"x":2,"y":2}])"),
    };
    s.answers["/api/v1/entities/environment/4"] = {ok(R"([{"entityId":1,"name":"alice","creationDate":"x"}])")};
    s.answers["/api/v1/entities/5"] = {ok(R"({"entityId":5,"name":"eve","creationDate":"x"})")};
    return s;
}

PollConfig config() {
    PollConfig c;
    c.environment = "4";
    c.gridEvery = 3;
    c.nameEvery = 4;
    return c;
}

// One AsyncPoll step, fetching from `viron` whatever it asks for. Returns the
// number of fetches it took.
int stepAsync(AsyncPoll& a, Transport& viron, AsyncPoll::Next& out) {
    int fetches = 0;
    for (;;) {
        out = a.advance();
        if (!out.needs) return fetches;
        ++fetches;
        a.provide(out.path, viron.get(out.path));
        REQUIRE(fetches < 50);
    }
}

}  // namespace

TEST("AsyncPoll makes the same requests and reaches the same state as PollCore", "[unit]") {
    Script direct = world(), viaAsync = world();
    PollCore core(direct, config());
    AsyncPoll async(config());
    for (int i = 0; i < 8; ++i) {
        const double want = core.step();
        AsyncPoll::Next got;
        stepAsync(async, viaAsync, got);
        REQUIRE(!got.needs);
        CHECK(got.stepped);
        CHECK(async.status().state == core.status().state);
        CHECK(async.status().message == core.status().message);
        CHECK(async.status().failures == core.status().failures);
        CHECK((want < 0) == (got.delay < 0));
        if (core.status().state != State::Live) CHECK(want == got.delay);  // backoff and Retry-After agree
        CHECK(async.hasSnapshot() == core.hasSnapshot());
        if (core.hasSnapshot()) {
            CHECK(async.snapshot().poll == core.snapshot().poll);
            CHECK(async.snapshot().placements == core.snapshot().placements);
            CHECK(async.snapshot().grids == core.snapshot().grids);
            CHECK(async.snapshot().names == core.snapshot().names);
        }
        CHECK(async.cached() == 0);
    }
    CHECK(viaAsync.log == direct.log);
    CHECK(async.snapshot().names.at(5) == "eve");
}

TEST("AsyncPoll asks for one path at a time, starting with the environment", "[unit]") {
    AsyncPoll a(config());
    AsyncPoll::Next n = a.advance();
    REQUIRE(n.needs);
    CHECK(n.path == "/api/v1/environments/4");
    CHECK(!a.hasSnapshot());
    a.provide(n.path, ok(R"({"environmentId":4,"name":"Meadow","creationDate":"x"})"));
    n = a.advance();
    REQUIRE(n.needs);
    CHECK(n.path == "/api/v1/grids/environment/4");
    CHECK(a.status().state == State::Connecting);  // a miss changes nothing
    CHECK(a.cached() == 1);
}

TEST("AsyncPoll: a network failure (status 0) is stale with backoff, like PollCore", "[unit]") {
    AsyncPoll a(config());
    Response refused;
    refused.status = 0;
    refused.error = "Failed to fetch";
    AsyncPoll::Next n = a.advance();
    a.provide(n.path, refused);
    n = a.advance();
    CHECK(!n.needs);
    CHECK(n.stepped);
    CHECK(a.status().state == State::Stale);
    CHECK(n.delay == 0.5);
    CHECK(a.status().message.find("Failed to fetch") != std::string::npos);
}

TEST("AsyncPoll: 401 stops, and a stopped poll asks for nothing more", "[unit]") {
    AsyncPoll a(config());
    AsyncPoll::Next n = a.advance();
    a.provide(n.path, code(401));
    n = a.advance();
    CHECK(n.delay < 0);
    CHECK(a.status().state == State::TokenRejected);
    n = a.advance();
    CHECK(!n.needs);
    CHECK(n.delay < 0);
    CHECK(!n.stepped);
}
