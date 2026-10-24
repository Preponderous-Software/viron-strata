// Demo mode: a recorded run of a real Viron, played back through the same
// poll loop, so a page works for anyone without a server or a token.
//
// A recording (format "viron-strata-demo/1", written by tools/record_demo.py)
// holds what Viron's read routes answered while tools/driver.py changed a
// world: the environment, its grids and its entities once, and the placements
// every `periodMs`. DemoTransport answers PollCore's GETs from it, picking the
// placements frame by the time since playback started and looping at the end,
// so everything downstream (the poll loop, the tween, the scene) runs exactly
// as it does against a live Viron.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "transport.h"

namespace viron_strata {

struct Recording {
    std::string environmentJson;  // EnvironmentDTO
    int64_t environmentId = 0;
    std::string environmentName;
    std::string gridsJson;        // GridDTO[]
    std::string entitiesJson;     // EntityDTO[]
    std::vector<std::string> entityJson;  // one EntityDTO per entity, by index
    std::vector<int64_t> entityIds;
    std::vector<std::string> framesJson;  // PlacementDTO[] per frame
    double periodSeconds = 0.25;
    double holdSeconds = 0;       // the last frame is shown this long before the loop restarts
    std::string note;             // where it came from
    double duration() const { return periodSeconds * static_cast<double>(framesJson.size()) + holdSeconds; }
};

// Throws std::runtime_error on anything that is not a viron-strata-demo/1 file.
Recording parseRecording(const std::string& json);

class DemoTransport : public Transport {
public:
    // `clock` returns seconds on any steady clock; playback starts at the first get().
    DemoTransport(Recording recording, std::function<double()> clock);
    Response get(const std::string& path) override;
    // The frame the last placements answer came from, and how many loops so far.
    size_t frame() const { return frame_; }
    int loops() const { return loops_; }
    const Recording& recording() const { return rec_; }

private:
    Recording rec_;
    std::function<double()> clock_;
    double start_ = -1;
    size_t frame_ = 0;
    int loops_ = 0;
};

}  // namespace viron_strata
