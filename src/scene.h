// The render side: polled snapshots turned into Strata draw calls
// (RFC 0030, sections 3 and 4).
//
// LiveWorld keeps one track per entity: where it was at the previous poll,
// where it is now, and when the new position arrived. Positions are tweened
// linearly over one poll period; a move of more than one cell, or onto
// another grid, snaps. A new entity grows in over 150 ms and a removed one
// shrinks out (Strata does not blend primitives, so fading is done by scale).
//
// Scene draws the floors (one static mesh per grid; one mesh for all grids
// past 200 of them), grid outlines and cell lines, the entities, a picked
// cell, and finds the cell under the mouse with Strata's mesh raycast.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "strata/strata.h"

#include "mapping.h"
#include "poller.h"

namespace strata {
class Engine;
}

namespace viron_strata {

constexpr double kGrowSeconds = 0.15;
constexpr int kMaxMeshGrids = 200;

struct Track {
    int64_t entityId = 0;
    std::string name;
    int gridIndex = 0;
    int x = 0, y = 0;
    std::array<double, 3> from{}, to{};  // cube centres (Strata world)
    double arrived = 0;                  // when `to` was polled
    double born = 0;                     // first seen
    double died = -1;                    // >= 0: removed at this time
    bool snap = false;
    std::array<float, 3> color{};
};

class LiveWorld {
public:
    // A new snapshot, polled at `now` (seconds, any steady clock); `period` is
    // the poll period the tween spans.
    void apply(const Snapshot& snapshot, double now, double period);
    // The cube centre and scale factor (0..1) of a track at `now`.
    std::array<double, 3> position(const Track& t, double now) const;
    double grow(const Track& t, double now) const;
    // Drops shrunk-out tracks.
    void prune(double now);

    const std::vector<Grid>& grids() const { return grids_; }
    const std::vector<Track>& tracks() const { return tracks_; }
    std::vector<const Track*> at(int gridIndex, int x, int y) const;
    size_t placed() const;
    const std::string& environmentName() const { return environmentName_; }
    int64_t environmentId() const { return environmentId_; }
    uint64_t gridRevision() const { return gridRevision_; }
    double period() const { return period_; }
    double lastUpdate() const { return lastUpdate_; }
    double latencyMs() const { return latencyMs_; }

private:
    std::vector<Grid> grids_;
    std::vector<Track> tracks_;
    std::unordered_map<int64_t, size_t> index_;
    std::string environmentName_;
    int64_t environmentId_ = 0;
    uint64_t gridRevision_ = 0;
    double period_ = 0.25;
    double lastUpdate_ = -1;
    double latencyMs_ = 0;
};

GridLayout layoutOf(const Grid& g);

struct OrbitCamera {
    float targetX = 0, targetZ = 0;
    float distance = 20;
    float yawDegrees = 0;
    float pitchDegrees = 55;
    StrataCamera toStrata(float aspect) const;
    void pan(float dx, float dz);
    void zoom(float factor);
};

// Frames every grid.
OrbitCamera fitCamera(const std::vector<Grid>& grids, float aspect);
bool projectToScreen(const StrataCamera& camera, int width, int height, const float world[3], float& px, float& py);

class Scene {
public:
    // Rebuilds the floor meshes when the grid layout changed.
    void updateFloors(strata::Engine& engine, const LiveWorld& world);
    void release(strata::Engine& engine);

    // Entities at `now`; dimmed when the view is stale.
    const std::vector<StrataRenderItem>& buildItems(const LiveWorld& world, double now, bool dim);
    const std::vector<StrataLineVertex>& buildLines(const LiveWorld& world, int pickGrid, int pickX, int pickY);

    // The cell under drawable pixel (px, py): grid index and (x, y).
    bool pick(const strata::Engine& engine, const StrataCamera& camera, int width, int height, float px, float py,
              int& gridIndex, int& x, int& y) const;

    size_t meshCount() const { return meshes_.size(); }

private:
    struct FloorMesh {
        uint32_t id = 0;
        std::vector<int> grids;           // grid indices, in mesh order
        std::vector<int64_t> firstCell;   // cumulative cell count before each grid
    };
    std::vector<FloorMesh> meshes_;
    uint64_t revision_ = UINT64_MAX;
    std::vector<Grid> grids_;
    std::vector<StrataRenderItem> items_;
    std::vector<StrataLineVertex> lines_;
};

}  // namespace viron_strata
