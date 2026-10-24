#include "demo.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

#include <nlohmann/json.hpp>

#include "poller.h"

namespace viron_strata {

using nlohmann::json;

Recording parseRecording(const std::string& text) {
    const json j = json::parse(text);
    if (!j.is_object() || j.value("format", "") != "viron-strata-demo/1")
        throw std::runtime_error("not a viron-strata-demo/1 recording");
    Recording r;
    r.periodSeconds = j.at("periodMs").get<double>() / 1000.0;
    if (!(r.periodSeconds > 0)) throw std::runtime_error("recording: periodMs must be positive");
    r.holdSeconds = std::max(0.0, j.value("holdMs", 0.0) / 1000.0);
    r.note = j.value("note", "");
    const json& env = j.at("environment");
    r.environmentJson = env.dump();
    parseEnvironment(r.environmentJson, r.environmentId, r.environmentName);
    r.gridsJson = j.at("grids").dump();
    parseGrids(r.gridsJson);  // validates
    const json& entities = j.at("entities");
    if (!entities.is_array()) throw std::runtime_error("recording: entities must be an array");
    r.entitiesJson = entities.dump();
    for (const auto& e : entities) {
        r.entityIds.push_back(e.at("entityId").get<int64_t>());
        r.entityJson.push_back(e.dump());
    }
    // Frames are compact: [entityId, gridId, locationId, x, y] per placement.
    const json& frames = j.at("frames");
    if (!frames.is_array() || frames.empty()) throw std::runtime_error("recording: no frames");
    for (const auto& f : frames) {
        json out = json::array();
        for (const auto& p : f) {
            if (!p.is_array() || p.size() != 5) throw std::runtime_error("recording: a placement must be 5 numbers");
            out.push_back({{"entityId", p[0]}, {"environmentId", r.environmentId}, {"gridId", p[1]},
                           {"locationId", p[2]}, {"x", p[3]}, {"y", p[4]}});
        }
        r.framesJson.push_back(out.dump());
    }
    return r;
}

DemoTransport::DemoTransport(Recording recording, std::function<double()> clock)
    : rec_(std::move(recording)), clock_(std::move(clock)) {}

namespace {

Response answer(int status, std::string body) {
    Response r;
    r.status = status;
    r.body = std::move(body);
    r.millis = 1.0;
    r.headers["content-type"] = "application/json";
    return r;
}

Response notFound() { return answer(404, R"({"status":404,"message":"not in the recording"})"); }

}  // namespace

Response DemoTransport::get(const std::string& path) {
    const double now = clock_();
    if (start_ < 0) start_ = now;
    const std::string id = std::to_string(rec_.environmentId);
    const std::string api = "/api/v1/";
    if (path == api + "environments/" + id) return answer(200, rec_.environmentJson);
    if (path.rfind(api + "environments/name/", 0) == 0) {
        // PollCore percent-encodes the name; compare encoded forms.
        if (path == api + "environments/name/" + urlEncode(rec_.environmentName)) return answer(200, rec_.environmentJson);
        return notFound();
    }
    if (path == api + "grids/environment/" + id) return answer(200, rec_.gridsJson);
    if (path == api + "entities/environment/" + id) return answer(200, rec_.entitiesJson);
    if (path == api + "locations/placements?environmentId=" + id) {
        const double cycle = rec_.duration();
        const double t = now - start_;
        loops_ = static_cast<int>(std::floor(t / cycle));
        const double inCycle = t - static_cast<double>(loops_) * cycle;
        size_t f = static_cast<size_t>(std::floor(inCycle / rec_.periodSeconds));
        if (f >= rec_.framesJson.size()) f = rec_.framesJson.size() - 1;  // the hold
        frame_ = f;
        return answer(200, rec_.framesJson[f]);
    }
    if (path.rfind(api + "entities/", 0) == 0) {
        const std::string rest = path.substr((api + "entities/").size());
        for (size_t i = 0; i < rec_.entityIds.size(); ++i)
            if (rest == std::to_string(rec_.entityIds[i])) return answer(200, rec_.entityJson[i]);
    }
    return notFound();
}

}  // namespace viron_strata
