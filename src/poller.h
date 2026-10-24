// Polling Viron's read routes into a Snapshot (RFC 0030, sections 1 and 2).
//
// PollCore is the synchronous state machine: step() makes the requests one
// poll needs and returns how long to wait before the next one. Poller runs it
// on a worker thread and hands finished snapshots to the render thread.
//
// Requests, all GET with the bearer token:
//   start          GET /api/v1/environments/{id}  (or /environments/name/{name})
//   start, every K GET /api/v1/grids/environment/{id}, and /environments/{id} to notice a deletion
//   every poll     GET /api/v1/locations/placements?environmentId={id}
//   new id, every M GET /api/v1/entities/environment/{id}
//   a name still missing  GET /api/v1/entities/{id}
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "transport.h"

namespace viron_strata {

struct Grid {
    int64_t gridId = 0;
    int rows = 0;
    int columns = 0;
    std::string name;
    int index = 0;  // position in gridId order
    bool operator==(const Grid& o) const {
        return gridId == o.gridId && rows == o.rows && columns == o.columns && name == o.name && index == o.index;
    }
};

struct Placement {
    int64_t entityId = 0;
    int64_t gridId = 0;
    int64_t locationId = 0;
    int x = 0, y = 0;
    bool operator==(const Placement& o) const {
        return entityId == o.entityId && gridId == o.gridId && locationId == o.locationId && x == o.x && y == o.y;
    }
};

struct Snapshot {
    int64_t environmentId = 0;
    std::string environmentName;
    std::vector<Grid> grids;            // gridId order
    std::vector<Placement> placements;  // entityId order, as Viron sends them
    std::unordered_map<int64_t, std::string> names;
    uint64_t poll = 0;                  // 1 for the first complete poll
    double latencyMs = 0;               // the placements request
};

enum class State {
    Connecting,       // no answer yet
    Live,             // the last poll succeeded
    Busy,             // 503 + Retry-After
    Stale,            // the last poll failed (5xx, timeout, refused); retrying with backoff
    TokenRejected,    // 401: stopped
    EnvironmentGone,  // 404 for the environment: stopped
    Unsupported,      // no /locations/placements (a Viron older than c96362b): stopped
    Failed,           // anything else that cannot be retried: stopped
};

const char* stateName(State s);
bool isStopped(State s);

struct Status {
    State state = State::Connecting;
    std::string message;    // for the HUD and stderr; never contains the token
    int failures = 0;       // consecutive failed polls
    double nextDelay = 0;   // seconds until the next poll
};

struct PollConfig {
    std::string environment;  // an id (all digits) or a name
    double periodSeconds = 0.25;
    int gridEvery = 20;       // K
    int nameEvery = 40;       // M
    double backoffMin = 0.5;
    double backoffMax = 10.0;
    int maxNameLookups = 20;  // per poll, for GET /entities/{id}
};

class PollCore {
public:
    PollCore(Transport& transport, PollConfig config);
    // One poll (resolving the environment and refreshing grids or names when
    // due). Returns the seconds to wait before the next step; negative once
    // stopped.
    double step();
    const Status& status() const { return status_; }
    bool hasSnapshot() const { return snapshot_.poll > 0; }
    const Snapshot& snapshot() const { return snapshot_; }
    uint64_t requests() const { return requests_; }

private:
    enum class Outcome { Ok, Retry, Stop };
    Response get(const std::string& path);
    Outcome classify(const Response& r, const std::string& what, bool environmentRoute);
    double fail();
    bool resolve();
    bool refreshGrids();
    bool refreshNames(bool all);

    Transport& transport_;
    PollConfig config_;
    Status status_;
    Snapshot snapshot_;
    Snapshot working_;
    bool resolved_ = false;
    bool gridsKnown_ = false;
    uint64_t polls_ = 0;
    uint64_t requests_ = 0;
    double retryAfter_ = -1;
};

// PollCore on a worker thread. latest() is safe to call from any thread.
class Poller {
public:
    Poller(std::unique_ptr<Transport> transport, PollConfig config);
    ~Poller();
    void start();
    void stop();
    // The newest snapshot and status; `seq` changes whenever a new snapshot lands.
    void latest(Snapshot& snapshot, Status& status, uint64_t& seq) const;
    // Blocks until a poll that started after this call has finished (or the
    // poller stopped, or the timeout passed). For --verify.
    bool waitForFreshPoll(double timeoutSeconds);

private:
    void run();
    std::unique_ptr<Transport> transport_;
    PollCore core_;
    std::thread thread_;
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::condition_variable polled_;
    std::atomic<bool> quit_{false};
    Snapshot snapshot_;
    Status status_;
    uint64_t seq_ = 0;
    uint64_t completed_ = 0;
};

// Parsers for Viron's JSON (exposed for tests). They throw std::runtime_error
// on a body that does not match the OpenAPI schema.
std::vector<Grid> parseGrids(const std::string& body);
std::vector<Placement> parsePlacements(const std::string& body);
std::unordered_map<int64_t, std::string> parseEntityNames(const std::string& body);
void parseEnvironment(const std::string& body, int64_t& id, std::string& name);

}  // namespace viron_strata
