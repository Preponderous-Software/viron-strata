// viron-strata: a live, read-only 3D view of one Viron environment, drawn
// with Strata (RFC 0030 in Preponderous-Software/rfcs).
//
//   viron-strata --environment <id|name> [--viron-url URL] [--token JWT]
//                [--period-ms N] [--grid-every K] [--name-every M]
//                [--yaw DEG] [--pitch DEG] [--zoom F] [--pick G,X,Y] [--no-hud]
//                [--width W --height H]
//                [--headless --duration S [--fps N] [--frames DIR] [--screenshot out.png] [--verify]]
//
// The token comes from --token or VIRON_TOKEN (the same names viron-usd
// uses; --url is accepted for --viron-url). It is never written to disk,
// logged or drawn. The client sends only GET.
//
// --verify (headless): at the end, waits for a fresh poll and compares the
// client's placements with an independent GET /locations/placements; exits 1
// on a mismatch. The integration test uses it after a driver stops.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "strata/strata.hpp"

#include "../src/poller.h"
#include "../src/scene.h"
#include "../src/transport.h"

using namespace viron_strata;

namespace {

enum : int32_t {
    kA = 4, kD = 7, kE = 8, kQ = 20, kS = 22, kW = 26, kF = 9,
    kEscape = 41, kTab = 43, kMinus = 45, kEquals = 46,
    kRight = 79, kLeft = 80, kDown = 81, kUp = 82, kKpMinus = 86, kKpPlus = 87
};

struct Args {
    std::string url = "http://localhost:9999";
    std::string environment;
    std::string token;
    double periodMs = 250;
    int gridEvery = 20;
    int nameEvery = 40;
    bool headless = false;
    double duration = 10;
    double fps = 15;
    std::string framesDir;
    std::string screenshot;
    bool verify = false;
    bool hud = true;
    int width = 1280, height = 720;
    float yaw = 0, pitch = 55, zoom = 1;
    int pickGrid = -1, pickX = -1, pickY = -1;
};

void usage(const char* argv0) {
    std::fprintf(stderr,
                 "usage: %s --environment <id|name> [--viron-url URL] [--token JWT]\n"
                 "          [--period-ms N] [--grid-every K] [--name-every M]\n"
                 "          [--yaw DEG] [--pitch DEG] [--zoom F] [--pick G,X,Y] [--no-hud] [--width W --height H]\n"
                 "          [--headless --duration S [--fps N] [--frames DIR] [--screenshot out.png] [--verify]]\n"
                 "The token may also come from VIRON_TOKEN.\n",
                 argv0);
}

bool parse(int argc, char** argv, Args& a) {
    if (const char* t = std::getenv("VIRON_TOKEN")) a.token = t;
    for (int i = 1; i < argc; ++i) {
        const std::string k = argv[i];
        auto val = [&]() -> std::string {
            if (i + 1 >= argc) throw std::invalid_argument(k + " needs a value");
            return argv[++i];
        };
        if (k == "--viron-url" || k == "--url") a.url = val();
        else if (k == "--environment") a.environment = val();
        else if (k == "--token") a.token = val();
        else if (k == "--period-ms") a.periodMs = std::max(10.0, std::atof(val().c_str()));
        else if (k == "--grid-every") a.gridEvery = std::max(1, std::atoi(val().c_str()));
        else if (k == "--name-every") a.nameEvery = std::max(1, std::atoi(val().c_str()));
        else if (k == "--headless") a.headless = true;
        else if (k == "--duration") a.duration = std::max(0.1, std::atof(val().c_str()));
        else if (k == "--fps") a.fps = std::min(120.0, std::max(1.0, std::atof(val().c_str())));
        else if (k == "--frames") a.framesDir = val();
        else if (k == "--screenshot") a.screenshot = val();
        else if (k == "--verify") a.verify = true;
        else if (k == "--no-hud") a.hud = false;
        else if (k == "--width") a.width = std::atoi(val().c_str());
        else if (k == "--height") a.height = std::atoi(val().c_str());
        else if (k == "--yaw") a.yaw = static_cast<float>(std::atof(val().c_str()));
        else if (k == "--pitch") a.pitch = static_cast<float>(std::atof(val().c_str()));
        else if (k == "--zoom") a.zoom = std::max(0.05f, static_cast<float>(std::atof(val().c_str())));
        else if (k == "--pick") {
            if (std::sscanf(val().c_str(), "%d,%d,%d", &a.pickGrid, &a.pickX, &a.pickY) != 3)
                throw std::invalid_argument("--pick takes GRID,X,Y (grid index in gridId order)");
        } else return false;
    }
    if (a.environment.empty()) {
        std::fprintf(stderr, "--environment is required\n");
        return false;
    }
    return true;
}

double now() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::string printable(const std::string& s) {
    std::string out;
    for (unsigned char c : s) out += (c >= 32 && c < 127) ? static_cast<char>(c) : '?';
    return out;
}

struct App {
    Args a;
    std::unique_ptr<Poller> poller;
    std::unique_ptr<strata::Engine> engine;
    LiveWorld world;
    Scene scene;
    OrbitCamera camera;
    bool cameraFitted = false;
    StrataLighting light{};
    int w = 0, h = 0;
    Snapshot snapshot;
    Status status;
    uint64_t seq = 0;
    double staleSince = -1;
    std::vector<std::string> strings;
    std::vector<StrataText> texts;
    State lastReported = State::Connecting;

    float aspect() const { return static_cast<float>(w) / static_cast<float>(std::max(1, h)); }

    void refresh(double t) {
        const uint64_t before = seq;
        poller->latest(snapshot, status, seq);
        if (seq != before) {
            world.apply(snapshot, t, a.periodMs / 1000.0);
            scene.updateFloors(*engine, world);
            if (!cameraFitted && !world.grids().empty()) {
                camera = fitCamera(world.grids(), aspect());
                camera.yawDegrees = a.yaw;
                camera.pitchDegrees = a.pitch;
                camera.distance /= a.zoom;
                cameraFitted = true;
            }
        }
        world.prune(t);
        if (status.state == State::Live) staleSince = -1;
        else if (staleSince < 0 && world.lastUpdate() >= 0) staleSince = t;
        if (status.state != lastReported) {
            if (status.state == State::Live)
                std::fprintf(stderr, "viron-strata: live\n");
            else
                std::fprintf(stderr, "viron-strata: %s\n", status.message.c_str());
            lastReported = status.state;
        }
    }

    void text(float x, float y, float size, const float* color, std::string s) {
        StrataText t{};
        t.x = x;
        t.y = y;
        t.pixel_size = size;
        t.flags = STRATA_TEXT_SHADOW;
        std::copy(color, color + 4, t.color);
        strings.push_back(printable(s));
        texts.push_back(t);
    }

    void buildHud(const StrataCamera& cam, double t) {
        strings.clear();
        texts.clear();
        if (!a.hud) return;
        static const float white[4] = {0.95f, 0.95f, 0.92f, 1}, dim[4] = {0.6f, 0.63f, 0.68f, 1},
                           green[4] = {0.45f, 0.95f, 0.5f, 1}, amber[4] = {1.0f, 0.75f, 0.2f, 1},
                           red[4] = {1.0f, 0.35f, 0.3f, 1}, pick[4] = {1.0f, 0.78f, 0.15f, 1};
        const float s = h >= 1000 ? 24.0f : (h >= 560 ? 16.0f : 8.0f);
        const float lh = std::round(s * 1.35f), m = s;
        char line[256];
        std::snprintf(line, sizeof line, "Viron  %s (#%lld)  %zu entities  %zu grid%s", world.environmentName().c_str(),
                      static_cast<long long>(world.environmentId()), world.placed(), world.grids().size(),
                      world.grids().size() == 1 ? "" : "s");
        text(m, m, s, white, line);
        const double age = world.lastUpdate() >= 0 ? t - world.lastUpdate() : 0;
        const float* c = green;
        switch (status.state) {
            case State::Live:
                std::snprintf(line, sizeof line, "live  updated %.1f s ago  poll %.0f ms", age, world.latencyMs());
                break;
            case State::Connecting:
                std::snprintf(line, sizeof line, "connecting to %s", a.url.c_str());
                c = dim;
                break;
            case State::Busy:
                std::snprintf(line, sizeof line, "Viron busy  retrying in %.1f s", status.nextDelay);
                c = amber;
                break;
            case State::Stale:
                std::snprintf(line, sizeof line, "stale for %.0f s  %s", staleSince >= 0 ? t - staleSince : 0.0,
                              status.message.c_str());
                c = amber;
                break;
            default:
                std::snprintf(line, sizeof line, "%s", status.message.c_str());
                c = red;
                break;
        }
        text(m, m + lh, s, c, line);
        std::snprintf(line, sizeof line, "polled every %.0f ms; not tick-exact", a.periodMs);
        text(m, m + 2 * lh, std::max(8.0f, s - 8.0f), dim, line);
        for (const Grid& g : world.grids()) {
            const auto o = gridOrigin(layoutOf(g));
            const float p[3] = {static_cast<float>(o[0]) + static_cast<float>(g.rows) * 0.5f, 0.0f, -0.8f};
            float px = 0, py = 0;
            if (!projectToScreen(cam, w, h, p, px, py)) continue;
            const std::string label = g.name.empty() ? "grid " + std::to_string(g.gridId) : g.name;
            text(std::round(px - static_cast<float>(label.size()) * s * 0.5f), std::round(py - s), s, dim, label);
        }
        if (a.pickGrid >= 0) {
            const auto here = world.at(a.pickGrid, a.pickX, a.pickY);
            std::snprintf(line, sizeof line, "grid %d (%d, %d): %zu here", a.pickGrid, a.pickX, a.pickY, here.size());
            float y = m + 4 * lh;
            text(m, y, s, pick, line);
            for (size_t i = 0; i < here.size() && i < 12; ++i) {
                y += lh;
                std::snprintf(line, sizeof line, "  #%lld %s", static_cast<long long>(here[i]->entityId),
                              here[i]->name.c_str());
                text(m, y, s, white, line);
            }
        }
        for (size_t i = 0; i < texts.size(); ++i) texts[i].utf8 = strings[i].c_str();
    }

    void draw(double t) {
        const StrataCamera cam = camera.toStrata(aspect());
        engine->setCamera(cam);
        light.shadow_distance = camera.distance * 3.0f;
        engine->setLighting(light);
        StrataFrameDesc fd{};
        fd.clear_flags = STRATA_CLEAR_COLOR | STRATA_CLEAR_DEPTH;
        fd.clear_color[0] = 0.03f;
        fd.clear_color[1] = 0.035f;
        fd.clear_color[2] = 0.05f;
        fd.clear_color[3] = 1.0f;
        engine->beginFrame(fd);
        const bool dim = status.state != State::Live && status.state != State::Connecting;
        const auto& items = scene.buildItems(world, t, dim);
        if (!items.empty()) engine->submitPrimitives(items.data(), items.size());
        const auto& lines = scene.buildLines(world, a.pickGrid, a.pickX, a.pickY);
        if (!lines.empty()) engine->submitLines(lines.data(), lines.size());
        buildHud(cam, t);
        if (!texts.empty()) engine->submitText(texts.data(), texts.size());
        engine->endFrame();
    }

    int verify() {
        if (!poller->waitForFreshPoll(15.0)) {
            std::fprintf(stderr, "verify: no fresh poll within 15 s\n");
            return 1;
        }
        Snapshot s;
        Status st;
        uint64_t q = 0;
        poller->latest(s, st, q);
        auto transport = makeCurlTransport(a.url, a.token);
        const Response r =
            transport->get("/api/v1/locations/placements?environmentId=" + std::to_string(s.environmentId));
        if (r.status != 200) {
            std::fprintf(stderr, "verify: GET /locations/placements answered %d\n", r.status);
            return 1;
        }
        const auto truth = parsePlacements(r.body);
        if (!(truth == s.placements)) {
            std::fprintf(stderr, "verify: MISMATCH: client has %zu placements, Viron %zu\n", s.placements.size(),
                         truth.size());
            return 1;
        }
        // The drawn world agrees with the snapshot, cell for cell.
        world.apply(s, now(), a.periodMs / 1000.0);
        for (const Placement& p : truth) {
            int gi = -1;
            for (const Grid& g : world.grids())
                if (g.gridId == p.gridId) gi = g.index;
            bool found = false;
            for (const Track* t : world.at(gi, p.x, p.y)) found = found || t->entityId == p.entityId;
            if (!found) {
                std::fprintf(stderr, "verify: entity %lld is not drawn at grid %lld (%d, %d)\n",
                             static_cast<long long>(p.entityId), static_cast<long long>(p.gridId), p.x, p.y);
                return 1;
            }
        }
        std::printf("verify: ok, %zu placements match GET /locations/placements (poll %llu)\n", truth.size(),
                    static_cast<unsigned long long>(s.poll));
        return 0;
    }

    int headless() {
        const double start = now();
        const double frameDt = 1.0 / a.fps;
        int frame = 0;
        for (double next = start; next - start <= a.duration; next += frameDt) {
            const double wait = next - now();
            if (wait > 0) std::this_thread::sleep_for(std::chrono::duration<double>(wait));
            const double t = now();
            refresh(t);
            if (!a.framesDir.empty()) {
                draw(t);
                char name[64];
                std::snprintf(name, sizeof name, "/frame-%05d.png", frame++);
                engine->saveScreenshotPng(a.framesDir + name);
            }
        }
        std::this_thread::sleep_for(std::chrono::duration<double>(a.periodMs / 1000.0 + 0.05));  // let tweens settle
        const double t = now();
        refresh(t);
        draw(t);
        if (!a.screenshot.empty()) engine->saveScreenshotPng(a.screenshot);
        if (!a.framesDir.empty()) std::printf("frames: %d to %s\n", frame, a.framesDir.c_str());
        std::printf("final: %s, %zu entities, %zu grids, poll %llu\n", stateName(status.state), world.placed(),
                    world.grids().size(), static_cast<unsigned long long>(snapshot.poll));
        int code = isStopped(status.state) ? 1 : 0;
        if (a.verify && code == 0) code = verify();
        return code;
    }

    int window() {
        std::printf(
            "controls: drag or WASD/arrows pan, Q/E rotate, wheel or +/- zoom, click a cell to list its entities,\n"
            "          F re-frame, Tab HUD, Esc quit\n");
        bool running = true, dragging = false;
        float downX = 0, downY = 0, lastX = 0, lastY = 0;
        while (running) {
            StrataEvent e;
            while (engine->pollEvent(e)) {
                if (e.type == STRATA_EVENT_QUIT) running = false;
                else if (e.type == STRATA_EVENT_RESIZED) engine->drawableSize(w, h);
                else if (e.type == STRATA_EVENT_MOUSE_WHEEL) camera.zoom(e.y > 0 ? 0.88f : 1.0f / 0.88f);
                else if (e.type == STRATA_EVENT_MOUSE_BUTTON_DOWN && e.button == 1) {
                    dragging = true;
                    downX = lastX = e.x;
                    downY = lastY = e.y;
                } else if (e.type == STRATA_EVENT_MOUSE_BUTTON_UP && e.button == 1) {
                    dragging = false;
                    if (std::fabs(e.x - downX) + std::fabs(e.y - downY) < 5.0f) {
                        if (!scene.pick(*engine, camera.toStrata(aspect()), w, h, e.x, e.y, a.pickGrid, a.pickX,
                                        a.pickY))
                            a.pickGrid = a.pickX = a.pickY = -1;
                    }
                } else if (e.type == STRATA_EVENT_MOUSE_MOTION && dragging) {
                    const float perPixel = camera.distance / static_cast<float>(std::max(1, h)) * 0.75f;
                    camera.pan(-(e.x - lastX) * perPixel, (e.y - lastY) * perPixel);
                    lastX = e.x;
                    lastY = e.y;
                } else if (e.type == STRATA_EVENT_KEY_DOWN) {
                    const float step = camera.distance * 0.04f;
                    switch (e.scancode) {
                        case kEscape: running = false; break;
                        case kW: case kUp: camera.pan(0, step); break;
                        case kS: case kDown: camera.pan(0, -step); break;
                        case kA: case kLeft: camera.pan(-step, 0); break;
                        case kD: case kRight: camera.pan(step, 0); break;
                        case kQ: camera.yawDegrees -= 5.0f; break;
                        case kE: camera.yawDegrees += 5.0f; break;
                        case kEquals: case kKpPlus: camera.zoom(0.9f); break;
                        case kMinus: case kKpMinus: camera.zoom(1.0f / 0.9f); break;
                        case kF: camera = fitCamera(world.grids(), aspect()); break;
                        case kTab: if (!e.repeat) a.hud = !a.hud; break;
                        default: break;
                    }
                }
            }
            const double t = now();
            refresh(t);
            draw(t);
            engine->present();
        }
        return 0;
    }
};

}  // namespace

int main(int argc, char** argv) {
    App app;
    try {
        if (!parse(argc, argv, app.a)) {
            usage(argv[0]);
            return 2;
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "%s\n", e.what());
        usage(argv[0]);
        return 2;
    }
    if (app.a.token.empty()) std::fprintf(stderr, "viron-strata: no token (--token or VIRON_TOKEN); Viron will answer 401\n");

    PollConfig pc;
    pc.environment = app.a.environment;
    pc.periodSeconds = app.a.periodMs / 1000.0;
    pc.gridEvery = app.a.gridEvery;
    pc.nameEvery = app.a.nameEvery;
    app.poller = std::make_unique<Poller>(makeCurlTransport(app.a.url, app.a.token), pc);

    strata::EngineDesc desc;
    desc.surface = app.a.headless ? STRATA_SURFACE_HEADLESS : STRATA_SURFACE_WINDOW;
    desc.width = app.a.width;
    desc.height = app.a.height;
    desc.title = "Viron on Strata: " + app.a.environment;
    desc.flags = app.a.headless ? 0 : STRATA_FLAG_VSYNC;
    try {
        app.engine = strata::Engine::create(desc);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "viron-strata: cannot start the renderer: %s\n", e.what());
        return 1;
    }
    app.light = strata::lightingDefaults(STRATA_LIGHTING_SUN);
    app.light.time_of_day = 0.4f;
    app.light.latitude_degrees = 35.0f;
    app.light.fog = 0;
    app.light.cascade_count = 2;
    app.w = desc.width;
    app.h = desc.height;
    app.engine->drawableSize(app.w, app.h);
    app.camera.yawDegrees = app.a.yaw;
    app.camera.pitchDegrees = app.a.pitch;

    app.poller->start();
    int code = 1;
    try {
        code = app.a.headless ? app.headless() : app.window();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "viron-strata: %s\n", e.what());
    }
    app.poller->stop();
    app.scene.release(*app.engine);
    app.engine.reset();
    return code;
}
