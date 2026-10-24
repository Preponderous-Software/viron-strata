#include "poller.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <stdexcept>
#include <unordered_set>

#include <nlohmann/json.hpp>

namespace viron_strata {

using nlohmann::json;

namespace {

double nowSeconds() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

bool allDigits(const std::string& s) {
    return !s.empty() && std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isdigit(c); });
}

int64_t asId(const json& j, const char* key) {
    if (!j.contains(key) || !j[key].is_number_integer()) throw std::runtime_error(std::string("missing integer ") + key);
    return j[key].get<int64_t>();
}

}  // namespace

const char* stateName(State s) {
    switch (s) {
        case State::Connecting: return "connecting";
        case State::Live: return "live";
        case State::Busy: return "Viron busy";
        case State::Stale: return "stale";
        case State::TokenRejected: return "token rejected";
        case State::EnvironmentGone: return "environment gone";
        case State::Unsupported: return "Viron too old";
        case State::Failed: return "stopped";
    }
    return "?";
}

bool isStopped(State s) {
    return s == State::TokenRejected || s == State::EnvironmentGone || s == State::Unsupported || s == State::Failed;
}

std::vector<Grid> parseGrids(const std::string& body) {
    const json j = json::parse(body);
    if (!j.is_array()) throw std::runtime_error("grids: expected an array");
    std::vector<Grid> grids;
    for (const auto& g : j) {
        Grid grid;
        grid.gridId = asId(g, "gridId");
        grid.rows = static_cast<int>(asId(g, "rows"));
        grid.columns = static_cast<int>(asId(g, "columns"));
        grid.name = g.contains("name") && g["name"].is_string() ? g["name"].get<std::string>() : "";
        grids.push_back(grid);
    }
    std::sort(grids.begin(), grids.end(), [](const Grid& a, const Grid& b) { return a.gridId < b.gridId; });
    for (size_t i = 0; i < grids.size(); ++i) grids[i].index = static_cast<int>(i);
    return grids;
}

std::vector<Placement> parsePlacements(const std::string& body) {
    const json j = json::parse(body);
    if (!j.is_array()) throw std::runtime_error("placements: expected an array");
    std::vector<Placement> out;
    out.reserve(j.size());
    for (const auto& p : j) {
        Placement pl;
        pl.entityId = asId(p, "entityId");
        pl.gridId = asId(p, "gridId");
        pl.locationId = asId(p, "locationId");
        pl.x = static_cast<int>(asId(p, "x"));
        pl.y = static_cast<int>(asId(p, "y"));
        out.push_back(pl);
    }
    return out;
}

std::unordered_map<int64_t, std::string> parseEntityNames(const std::string& body) {
    const json j = json::parse(body);
    std::unordered_map<int64_t, std::string> names;
    auto one = [&names](const json& e) {
        names[asId(e, "entityId")] = e.contains("name") && e["name"].is_string() ? e["name"].get<std::string>() : "";
    };
    if (j.is_array()) {
        for (const auto& e : j) one(e);
    } else if (j.is_object()) {
        one(j);
    } else {
        throw std::runtime_error("entities: expected an array or an object");
    }
    return names;
}

void parseEnvironment(const std::string& body, int64_t& id, std::string& name) {
    const json j = json::parse(body);
    id = asId(j, "environmentId");
    name = j.contains("name") && j["name"].is_string() ? j["name"].get<std::string>() : "";
}

std::string urlEncode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out += static_cast<char>(c);
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

PollCore::PollCore(Transport& transport, PollConfig config) : transport_(transport), config_(std::move(config)) {}

Response PollCore::get(const std::string& path) {
    ++requests_;
    return transport_.get(path);
}

PollCore::Outcome PollCore::classify(const Response& r, const std::string& what, bool environmentRoute) {
    retryAfter_ = -1;
    if (r.status >= 200 && r.status < 300) return Outcome::Ok;
    if (r.status == 0) {
        status_.state = State::Stale;
        status_.message = "unreachable (" + r.error + ")";
        return Outcome::Retry;
    }
    if (r.status == 401) {
        status_.state = State::TokenRejected;
        status_.message = "token rejected: restart with a new token";
        return Outcome::Stop;
    }
    if (r.status == 404 && environmentRoute) {
        status_.state = State::EnvironmentGone;
        status_.message = "environment gone";
        return Outcome::Stop;
    }
    if (what == "placements" && (r.status == 400 || r.status == 404 || r.status == 405)) {
        status_.state = State::Unsupported;
        status_.message = "this Viron has no GET /locations/placements (it is older than c96362b); upgrade Viron";
        return Outcome::Stop;
    }
    if (r.status == 503) {
        const auto it = r.headers.find("retry-after");
        if (it != r.headers.end() && allDigits(it->second)) {
            retryAfter_ = std::atof(it->second.c_str());
            status_.state = State::Busy;
            status_.message = "Viron busy";
            return Outcome::Retry;
        }
    }
    if (r.status >= 500) {
        status_.state = State::Stale;
        status_.message = what + ": HTTP " + std::to_string(r.status);
        return Outcome::Retry;
    }
    status_.state = State::Failed;
    status_.message = what + ": HTTP " + std::to_string(r.status);
    return Outcome::Stop;
}

double PollCore::fail() {
    ++status_.failures;
    double delay;
    if (retryAfter_ >= 0) {
        delay = retryAfter_;
    } else {
        delay = config_.backoffMin * std::pow(2.0, std::min(status_.failures - 1, 16));
        delay = std::min(delay, config_.backoffMax);
    }
    status_.nextDelay = delay;
    return delay;
}

bool PollCore::resolve() {
    const bool byId = allDigits(config_.environment);
    const std::string path = byId ? "/api/v1/environments/" + config_.environment
                                  : "/api/v1/environments/name/" + urlEncode(config_.environment);
    const Response r = get(path);
    const Outcome o = classify(r, "environment", true);
    if (o != Outcome::Ok) return false;
    try {
        parseEnvironment(r.body, working_.environmentId, working_.environmentName);
    } catch (const std::exception& e) {
        status_.state = State::Failed;
        status_.message = std::string("environment: unexpected answer (") + e.what() + ")";
        return false;
    }
    resolved_ = true;
    return true;
}

bool PollCore::refreshGrids() {
    const std::string id = std::to_string(working_.environmentId);
    if (gridsKnown_) {
        // Notices a deleted environment (placements answers [] for an unknown one).
        const Response env = get("/api/v1/environments/" + id);
        if (classify(env, "environment", true) != Outcome::Ok) return false;
        try {
            parseEnvironment(env.body, working_.environmentId, working_.environmentName);
        } catch (const std::exception&) {
        }
    }
    const Response r = get("/api/v1/grids/environment/" + id);
    if (classify(r, "grids", false) != Outcome::Ok) return false;
    try {
        working_.grids = parseGrids(r.body);
    } catch (const std::exception& e) {
        status_.state = State::Failed;
        status_.message = std::string("grids: unexpected answer (") + e.what() + ")";
        return false;
    }
    gridsKnown_ = true;
    return true;
}

bool PollCore::refreshNames(bool all) {
    if (all) {
        const Response r = get("/api/v1/entities/environment/" + std::to_string(working_.environmentId));
        if (r.status != 404) {  // 404: nothing listed; the per-id fallback below still runs
            if (classify(r, "entities", false) != Outcome::Ok) return false;
            try {
                auto names = parseEntityNames(r.body);
                for (auto& [id, name] : names) working_.names[id] = name;
            } catch (const std::exception& e) {
                status_.state = State::Failed;
                status_.message = std::string("entities: unexpected answer (") + e.what() + ")";
                return false;
            }
        }
    }
    // The fallback for a name still missing (viron-usd does the same).
    int lookups = 0;
    for (const Placement& p : working_.placements) {
        if (working_.names.count(p.entityId) || lookups >= config_.maxNameLookups) continue;
        ++lookups;
        const Response r = get("/api/v1/entities/" + std::to_string(p.entityId));
        if (r.status == 404) {
            working_.names[p.entityId] = "";  // deleted since the placements read
            continue;
        }
        if (classify(r, "entity", false) != Outcome::Ok) return false;
        try {
            for (auto& [id, name] : parseEntityNames(r.body)) working_.names[id] = name;
        } catch (const std::exception&) {
            working_.names[p.entityId] = "";
        }
    }
    return true;
}

double PollCore::step() {
    if (isStopped(status_.state)) return -1;
    const double start = nowSeconds();
    auto bail = [this]() { return isStopped(status_.state) ? -1.0 : fail(); };

    if (!resolved_ && !resolve()) return bail();
    const bool gridsDue = !gridsKnown_ || (config_.gridEvery > 0 && polls_ % config_.gridEvery == 0);
    if (gridsDue && !refreshGrids()) return bail();

    const Response r = get("/api/v1/locations/placements?environmentId=" + std::to_string(working_.environmentId));
    if (classify(r, "placements", false) != Outcome::Ok) return bail();
    try {
        working_.placements = parsePlacements(r.body);
    } catch (const std::exception& e) {
        status_.state = State::Failed;
        status_.message = std::string("placements: unexpected answer (") + e.what() + ")";
        return -1;
    }
    working_.latencyMs = r.millis;

    bool unknown = false;
    for (const Placement& p : working_.placements)
        if (!working_.names.count(p.entityId)) unknown = true;
    const bool namesDue = config_.nameEvery > 0 && polls_ > 0 && polls_ % config_.nameEvery == 0;
    if ((unknown || namesDue) && !refreshNames(true)) return bail();

    working_.poll = ++polls_;
    snapshot_ = working_;
    status_.state = State::Live;
    status_.message = "live";
    status_.failures = 0;
    const double elapsed = nowSeconds() - start;
    status_.nextDelay = std::max(0.0, config_.periodSeconds - elapsed);
    return status_.nextDelay;
}

Poller::Poller(std::unique_ptr<Transport> transport, PollConfig config)
    : transport_(std::move(transport)), core_(*transport_, std::move(config)) {}

Poller::~Poller() { stop(); }

void Poller::start() {
    if (!thread_.joinable()) thread_ = std::thread([this] { run(); });
}

void Poller::stop() {
    quit_ = true;
    wake_.notify_all();
    if (thread_.joinable()) thread_.join();
}

void Poller::latest(Snapshot& snapshot, Status& status, uint64_t& seq) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (seq != seq_) snapshot = snapshot_;
    status = status_;
    seq = seq_;
}

bool Poller::waitForFreshPoll(double timeoutSeconds) {
    std::unique_lock<std::mutex> lock(mutex_);
    const uint64_t target = completed_ + 2;  // the one in flight may have started before this call
    return polled_.wait_for(lock, std::chrono::duration<double>(timeoutSeconds), [&] {
        return completed_ >= target || isStopped(status_.state) || quit_;
    });
}

void Poller::run() {
    while (!quit_) {
        const double delay = core_.step();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (core_.hasSnapshot() && core_.snapshot().poll != snapshot_.poll) {
                snapshot_ = core_.snapshot();
                ++seq_;
            }
            status_ = core_.status();
            ++completed_;
        }
        polled_.notify_all();
        if (delay < 0) break;
        std::unique_lock<std::mutex> lock(mutex_);
        wake_.wait_for(lock, std::chrono::duration<double>(delay), [this] { return quit_.load(); });
    }
}

}  // namespace viron_strata
