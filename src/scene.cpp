#include "scene.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "strata/strata.hpp"

namespace viron_strata {

namespace {

constexpr float kPi = 3.14159265358979f;

}  // namespace

GridLayout layoutOf(const Grid& g) { return GridLayout{g.index, g.rows, g.columns}; }

void LiveWorld::apply(const Snapshot& s, double now, double period) {
    period_ = std::max(period, 1e-3);
    lastUpdate_ = now;
    latencyMs_ = s.latencyMs;
    environmentName_ = s.environmentName;
    environmentId_ = s.environmentId;
    if (!(s.grids == grids_)) {
        grids_ = s.grids;
        ++gridRevision_;
    }
    std::unordered_map<int64_t, const Grid*> byId;
    for (const Grid& g : grids_) byId[g.gridId] = &g;

    std::vector<bool> seen(tracks_.size(), false);
    for (const Placement& p : s.placements) {
        const auto g = byId.find(p.gridId);
        if (g == byId.end()) continue;  // a grid not refreshed yet: drawn from the next grid refresh
        const GridLayout layout = layoutOf(*g->second);
        const auto pos = entityPosition(layout, p.x, p.y, p.entityId);
        const auto name = s.names.find(p.entityId);
        auto it = index_.find(p.entityId);
        if (it == index_.end() || tracks_[it->second].died >= 0) {
            if (it != index_.end()) tracks_[it->second].died = -2;  // replaced: dropped by prune
            Track t;
            t.entityId = p.entityId;
            t.gridIndex = g->second->index;
            t.x = p.x;
            t.y = p.y;
            t.from = t.to = pos;
            t.arrived = t.born = now;
            t.snap = true;
            const auto c = entityColor(p.entityId);
            t.color = {static_cast<float>(c[0]), static_cast<float>(c[1]), static_cast<float>(c[2])};
            if (name != s.names.end()) t.name = name->second;
            index_[p.entityId] = tracks_.size();
            tracks_.push_back(t);
            seen.push_back(true);
            continue;
        }
        Track& t = tracks_[it->second];
        seen[it->second] = true;
        if (name != s.names.end()) t.name = name->second;
        if (t.gridIndex == g->second->index && t.x == p.x && t.y == p.y) continue;
        const bool adjacent = t.gridIndex == g->second->index && std::abs(t.x - p.x) + std::abs(t.y - p.y) <= 1;
        t.from = t.to;
        t.to = pos;
        t.snap = !adjacent;
        t.arrived = now;
        t.gridIndex = g->second->index;
        t.x = p.x;
        t.y = p.y;
    }
    for (size_t i = 0; i < tracks_.size() && i < seen.size(); ++i)
        if (!seen[i] && tracks_[i].died == -1) tracks_[i].died = now;
    prune(now);
}

void LiveWorld::prune(double now) {
    std::vector<Track> kept;
    kept.reserve(tracks_.size());
    for (Track& t : tracks_) {
        if (t.died == -2) continue;
        if (t.died >= 0 && now - t.died > kGrowSeconds) continue;
        kept.push_back(std::move(t));
    }
    tracks_ = std::move(kept);
    index_.clear();
    for (size_t i = 0; i < tracks_.size(); ++i) index_[tracks_[i].entityId] = i;
}

std::array<double, 3> LiveWorld::position(const Track& t, double now) const {
    if (t.snap) return t.to;
    const double k = std::min(std::max((now - t.arrived) / period_, 0.0), 1.0);
    return {t.from[0] + (t.to[0] - t.from[0]) * k, t.from[1] + (t.to[1] - t.from[1]) * k,
            t.from[2] + (t.to[2] - t.from[2]) * k};
}

double LiveWorld::grow(const Track& t, double now) const {
    if (t.died >= 0) return std::max(0.0, 1.0 - (now - t.died) / kGrowSeconds);
    return std::min(1.0, std::max(0.0, (now - t.born) / kGrowSeconds));
}

std::vector<const Track*> LiveWorld::at(int gridIndex, int x, int y) const {
    std::vector<const Track*> out;
    for (const Track& t : tracks_)
        if (t.died < 0 && t.gridIndex == gridIndex && t.x == x && t.y == y) out.push_back(&t);
    std::sort(out.begin(), out.end(), [](const Track* a, const Track* b) { return a->entityId < b->entityId; });
    return out;
}

size_t LiveWorld::placed() const {
    return static_cast<size_t>(std::count_if(tracks_.begin(), tracks_.end(), [](const Track& t) { return t.died < 0; }));
}

StrataCamera OrbitCamera::toStrata(float aspect) const {
    const float yaw = yawDegrees * kPi / 180.0f;
    const float pitch = pitchDegrees * kPi / 180.0f;
    const float eye[3] = {targetX + distance * std::cos(pitch) * std::sin(yaw), distance * std::sin(pitch),
                          targetZ + distance * std::cos(pitch) * std::cos(yaw)};
    const float target[3] = {targetX, 0.0f, targetZ};
    const float up[3] = {0.0f, 1.0f, 0.0f};
    return strata::cameraLookAt(eye, target, up, 40.0f, aspect, std::max(0.05f, distance * 0.02f), distance * 6.0f);
}

void OrbitCamera::pan(float dx, float dz) {
    const float yaw = yawDegrees * kPi / 180.0f;
    targetX += dx * std::cos(yaw) - dz * std::sin(yaw);
    targetZ += -dx * std::sin(yaw) - dz * std::cos(yaw);
}

void OrbitCamera::zoom(float factor) { distance = std::min(std::max(distance * factor, 1.5f), 20000.0f); }

OrbitCamera fitCamera(const std::vector<Grid>& grids, float aspect) {
    OrbitCamera cam;
    float maxX = 1.0f, maxZ = 1.0f;
    for (const Grid& g : grids) {
        const auto o = gridOrigin(layoutOf(g));
        maxX = std::max(maxX, static_cast<float>(o[0]) + static_cast<float>(g.rows));
        maxZ = std::max(maxZ, static_cast<float>(g.columns));
    }
    cam.targetX = maxX * 0.5f;
    cam.targetZ = maxZ * 0.5f + maxZ * 0.04f;
    const float a = std::max(aspect, 0.1f);
    if (a < 0.9f && maxX > maxZ * 1.2f) {
        // A tall view (a phone held upright) of grids laid side by side: turn
        // the camera a quarter so they stack down the screen instead of
        // shrinking to fit across it.
        cam.yawDegrees = 90.0f;
        cam.targetX = maxX * 0.5f + maxX * 0.04f;
        cam.targetZ = maxZ * 0.5f;
        cam.distance = std::max(maxZ / a, maxX) * 1.5f + 2.0f;
        return cam;
    }
    const float fit = std::max(maxX / a, maxZ);
    cam.distance = fit * 1.5f + 2.0f;
    return cam;
}

bool projectToScreen(const StrataCamera& c, int width, int height, const float p[3], float& px, float& py) {
    float v[4];
    for (int r = 0; r < 4; ++r)
        v[r] = c.view[0 * 4 + r] * p[0] + c.view[1 * 4 + r] * p[1] + c.view[2 * 4 + r] * p[2] + c.view[3 * 4 + r];
    float clip[4];
    for (int r = 0; r < 4; ++r)
        clip[r] = c.projection[0 * 4 + r] * v[0] + c.projection[1 * 4 + r] * v[1] + c.projection[2 * 4 + r] * v[2] +
                  c.projection[3 * 4 + r] * v[3];
    if (clip[3] <= 1e-6f) return false;
    px = (clip[0] / clip[3] * 0.5f + 0.5f) * static_cast<float>(width);
    py = (0.5f - clip[1] / clip[3] * 0.5f) * static_cast<float>(height);
    return true;
}

void Scene::updateFloors(strata::Engine& engine, const LiveWorld& world) {
    if (world.gridRevision() == revision_) return;
    revision_ = world.gridRevision();
    release(engine);
    grids_ = world.grids();
    const bool batch = grids_.size() > static_cast<size_t>(kMaxMeshGrids);
    std::vector<std::vector<int>> groups;
    for (size_t i = 0; i < grids_.size(); ++i) {
        if (batch) {
            if (groups.empty()) groups.emplace_back();
            groups[0].push_back(static_cast<int>(i));
        } else {
            groups.push_back({static_cast<int>(i)});
        }
    }
    for (const auto& group : groups) {
        FloorMesh fm;
        std::vector<float> positions, normals, colors;
        std::vector<uint32_t> indices;
        int64_t cells = 0;
        for (int gi : group) {
            const Grid& g = grids_[static_cast<size_t>(gi)];
            fm.grids.push_back(gi);
            fm.firstCell.push_back(cells);
            const GridLayout layout = layoutOf(g);
            // Cell order x * columns + y: viron-usd's face_index.
            for (int x = 0; x < g.rows; ++x) {
                for (int y = 0; y < g.columns; ++y) {
                    const auto c = cellCenter(layout, x, y);
                    const float x0 = static_cast<float>(c[0]) - 0.5f, x1 = x0 + 1.0f;
                    const float z0 = static_cast<float>(c[2]) - 0.5f, z1 = z0 + 1.0f;
                    const float corners[4][3] = {{x0, 0, z0}, {x0, 0, z1}, {x1, 0, z1}, {x1, 0, z0}};
                    const bool even = (x + y) % 2 == 0;
                    const float rgb[3] = {even ? 0.30f : 0.25f, even ? 0.31f : 0.26f, even ? 0.34f : 0.29f};
                    const uint32_t base = static_cast<uint32_t>(positions.size() / 3);
                    for (const auto& corner : corners) {
                        positions.insert(positions.end(), corner, corner + 3);
                        normals.insert(normals.end(), {0.0f, 1.0f, 0.0f});
                        colors.insert(colors.end(), rgb, rgb + 3);
                    }
                    indices.insert(indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
                }
            }
            cells += static_cast<int64_t>(g.rows) * g.columns;
        }
        if (indices.empty()) continue;
        StrataMesh mesh{};
        mesh.positions = positions.data();
        mesh.normals = normals.data();
        mesh.colors = colors.data();
        mesh.indices = indices.data();
        mesh.vertex_count = static_cast<int32_t>(positions.size() / 3);
        mesh.index_count = static_cast<int32_t>(indices.size());
        mesh.color[0] = mesh.color[1] = mesh.color[2] = mesh.color[3] = 1.0f;
        fm.id = engine.addMesh(mesh);
        meshes_.push_back(std::move(fm));
    }
}

void Scene::release(strata::Engine& engine) {
    for (const FloorMesh& m : meshes_) engine.removeMesh(m.id);
    meshes_.clear();
}

const std::vector<StrataRenderItem>& Scene::buildItems(const LiveWorld& world, double now, bool dim) {
    items_.clear();
    const float k = dim ? 0.4f : 1.0f;
    for (const Grid& g : world.grids()) {
        // A slab under each grid, so it reads as a board.
        const auto o = gridOrigin(layoutOf(g));
        StrataRenderItem slab{};
        slab.kind = STRATA_PRIMITIVE_CUBE;
        slab.position[0] = static_cast<float>(o[0]) + static_cast<float>(g.rows) * 0.5f;
        slab.position[1] = -0.26f;
        slab.position[2] = static_cast<float>(g.columns) * 0.5f;
        slab.scale[0] = static_cast<float>(g.rows) + 0.3f;
        slab.scale[1] = 0.5f;
        slab.scale[2] = static_cast<float>(g.columns) + 0.3f;
        slab.color[0] = 0.10f * k;
        slab.color[1] = 0.10f * k;
        slab.color[2] = 0.12f * k;
        slab.color[3] = 1.0f;
        items_.push_back(slab);
    }
    for (const Track& t : world.tracks()) {
        const double s = world.grow(t, now);
        if (s <= 0.0) continue;
        const auto p = world.position(t, now);
        StrataRenderItem it{};
        it.kind = STRATA_PRIMITIVE_CUBE;
        const float size = static_cast<float>(kEntitySize * s);
        it.position[0] = static_cast<float>(p[0]);
        it.position[1] = size * 0.5f;  // resting on the floor while it grows
        it.position[2] = static_cast<float>(p[2]);
        it.scale[0] = it.scale[1] = it.scale[2] = size;
        it.color[0] = t.color[0] * k;
        it.color[1] = t.color[1] * k;
        it.color[2] = t.color[2] * k;
        it.color[3] = 1.0f;
        items_.push_back(it);
    }
    return items_;
}

const std::vector<StrataLineVertex>& Scene::buildLines(const LiveWorld& world, int pickGrid, int pickX, int pickY) {
    lines_.clear();
    auto line = [this](float x0, float z0, float x1, float z1, float y, const float* c) {
        StrataLineVertex a{}, b{};
        a.position[0] = x0; a.position[1] = y; a.position[2] = z0;
        b.position[0] = x1; b.position[1] = y; b.position[2] = z1;
        std::copy(c, c + 4, a.color);
        std::copy(c, c + 4, b.color);
        lines_.push_back(a);
        lines_.push_back(b);
    };
    const float cellColor[4] = {0.16f, 0.17f, 0.20f, 1.0f};
    const float edgeColor[4] = {0.55f, 0.6f, 0.7f, 1.0f};
    const float pickColor[4] = {1.0f, 0.78f, 0.15f, 1.0f};
    for (const Grid& g : world.grids()) {
        const float ox = static_cast<float>(gridOrigin(layoutOf(g))[0]);
        const float w = static_cast<float>(g.rows), d = static_cast<float>(g.columns);
        if (static_cast<int64_t>(g.rows) * g.columns <= 4096) {
            for (int i = 1; i < g.rows; ++i) line(ox + i, 0, ox + i, d, 0.004f, cellColor);
            for (int j = 1; j < g.columns; ++j) line(ox, static_cast<float>(j), ox + w, static_cast<float>(j), 0.004f, cellColor);
        }
        line(ox, 0, ox + w, 0, 0.006f, edgeColor);
        line(ox + w, 0, ox + w, d, 0.006f, edgeColor);
        line(ox + w, d, ox, d, 0.006f, edgeColor);
        line(ox, d, ox, 0, 0.006f, edgeColor);
    }
    if (pickGrid >= 0 && pickGrid < static_cast<int>(world.grids().size())) {
        const auto c = cellCenter(layoutOf(world.grids()[static_cast<size_t>(pickGrid)]), pickX, pickY);
        const float cx = static_cast<float>(c[0]), cz = static_cast<float>(c[2]);
        for (float r : {0.47f, 0.44f, 0.41f}) {
            line(cx - r, cz - r, cx + r, cz - r, 0.01f, pickColor);
            line(cx + r, cz - r, cx + r, cz + r, 0.01f, pickColor);
            line(cx + r, cz + r, cx - r, cz + r, 0.01f, pickColor);
            line(cx - r, cz + r, cx - r, cz - r, 0.01f, pickColor);
        }
    }
    return lines_;
}

bool Scene::pick(const strata::Engine& engine, const StrataCamera& c, int width, int height, float px, float py,
                 int& gridIndex, int& x, int& y) const {
    if (meshes_.empty()) return false;
    float right[3], up[3], forward[3];
    for (int i = 0; i < 3; ++i) {
        right[i] = c.view[i * 4 + 0];
        up[i] = c.view[i * 4 + 1];
        forward[i] = -c.view[i * 4 + 2];
    }
    const float nx = 2.0f * px / static_cast<float>(std::max(1, width)) - 1.0f;
    const float ny = 1.0f - 2.0f * py / static_cast<float>(std::max(1, height));
    const float sx = nx / std::max(std::fabs(c.projection[0]), 1e-6f);
    const float sy = ny / std::max(std::fabs(c.projection[5]), 1e-6f);
    float dir[3];
    for (int i = 0; i < 3; ++i) dir[i] = forward[i] + right[i] * sx + up[i] * sy;
    const StrataRayHit hit = engine.raycastMeshes(c.eye, dir, c.far_plane * 2.0f);
    if (hit.mesh_id == 0 || hit.triangle < 0) return false;
    for (const FloorMesh& m : meshes_) {
        if (m.id != hit.mesh_id) continue;
        const int64_t cell = hit.triangle / 2;
        for (size_t i = m.grids.size(); i-- > 0;) {
            if (cell < m.firstCell[i]) continue;
            const Grid& g = grids_[static_cast<size_t>(m.grids[i])];
            const int64_t local = cell - m.firstCell[i];
            gridIndex = m.grids[i];
            x = static_cast<int>(local / std::max(1, g.columns));
            y = static_cast<int>(local % std::max(1, g.columns));
            return true;
        }
    }
    return false;
}

}  // namespace viron_strata
