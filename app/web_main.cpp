// viron-strata in a browser: the same poll loop, world and scene on Strata's
// WebGL 2 backend (Emscripten). RFC 0030, section 6.
//
// Two modes, chosen by the page (web/shell.html):
//
//   demo  A recorded run (web/demo/recording.json, built in) played back
//         through PollCore by DemoTransport. No server, no token: the page
//         works for anyone.
//   live  A Viron URL, an environment and a token the viewer types in (or
//         passes in the page's #fragment). AsyncPoll drives PollCore, one
//         emscripten_fetch at a time. Viron must list the page's origin in
//         VIRON_CORS_ALLOWED_ORIGINS (viron#244), and the token should be a
//         read-only one (scope viron:read, viron#245).
//
// Read-only by construction, as on the desktop: every request below is a GET
// (the fetch method is fixed here, and Transport has only get()). The token
// lives in this module's memory only: it is never logged, stored or drawn,
// and it is not part of the bundle.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include <emscripten/emscripten.h>
#include <emscripten/fetch.h>
#include <emscripten/html5.h>

#include "strata/strata.hpp"

#include "../src/async_poll.h"
#include "../src/demo.h"
#include "../src/poller.h"
#include "../src/scene.h"

using namespace viron_strata;

namespace {

enum : int32_t {
    kA = 4, kD = 7, kE = 8, kQ = 20, kS = 22, kW = 26, kF = 9,
    kTab = 43, kMinus = 45, kEquals = 46,
    kRight = 79, kLeft = 80, kDown = 81, kUp = 82, kKpMinus = 86, kKpPlus = 87
};

enum class Mode { None, Demo, Live };

double nowSeconds() { return emscripten_get_now() / 1000.0; }

std::string printable(const std::string& s) {
    std::string out;
    for (unsigned char c : s) out += (c >= 32 && c < 127) ? static_cast<char>(c) : '?';
    return out;
}

void jsonString(std::string& out, const std::string& s) {
    out += '"';
    for (unsigned char ch : s) {
        if (ch == '"' || ch == '\\') {
            out += '\\';
            out += static_cast<char>(ch);
        } else if (ch < 0x20) {
            out += ' ';
        } else {
            out += static_cast<char>(ch);
        }
    }
    out += '"';
}

struct App {
    std::unique_ptr<strata::Engine> engine;
    StrataLighting light{};
    Mode mode = Mode::None;
    double period = 0.25;

    // demo
    std::unique_ptr<DemoTransport> demo;
    std::unique_ptr<PollCore> demoCore;

    // live
    std::unique_ptr<AsyncPoll> live;
    std::string baseUrl;
    std::string bearer;  // "Bearer <token>", or empty
    std::string fetchPath;
    double fetchStarted = 0;
    bool inFlight = false;
    uint32_t generation = 0;  // fetches from an earlier connection are ignored
    double cycleStart = -1;

    double nextAt = 0;
    bool stopped = false;
    Snapshot snapshot;
    Status status;
    uint64_t requests = 0;

    LiveWorld world;
    Scene scene;
    OrbitCamera camera;
    bool cameraFitted = false;
    bool hud = true;
    int pickGrid = -1, pickX = -1, pickY = -1;
    double staleSince = -1;

    int32_t w = 0, h = 0;       // drawable pixels
    double cssW = 0, cssH = 0;  // the canvas in CSS pixels
    double winW = 0, winH = 0;  // SDL's window size (its pointer units)
    bool compact = false;

    bool dragging = false;
    float downX = 0, downY = 0, lastX = 0, lastY = 0;

    std::vector<std::string> strings;
    std::vector<StrataText> texts;
    std::string statusJson;

    float aspect() const { return static_cast<float>(w) / static_cast<float>(std::max(1, h)); }

    // SDL reports the pointer in its window's units, which are not drawable
    // pixels when the page caps the device pixel ratio (asn1-viz#9).
    void toDrawable(float& x, float& y) const {
        if (winW > 0 && winH > 0) {
            x = static_cast<float>(x * w / winW);
            y = static_cast<float>(y * h / winH);
        }
    }

    void reset(Mode m) {
        ++generation;
        inFlight = false;
        demo.reset();
        demoCore.reset();
        live.reset();
        bearer.clear();
        baseUrl.clear();
        scene.release(*engine);
        scene = Scene();
        world = LiveWorld();
        snapshot = Snapshot();
        status = Status();
        cameraFitted = false;
        pickGrid = pickX = pickY = -1;
        staleSince = -1;
        stopped = false;
        nextAt = 0;
        cycleStart = -1;
        requests = 0;
        mode = m;
    }

    void adopt(const Snapshot& s, const Status& st, double t) {
        status = st;
        if (s.poll != snapshot.poll && s.poll > 0) {
            snapshot = s;
            world.apply(snapshot, t, period);
            scene.updateFloors(*engine, world);
            if (!cameraFitted && !world.grids().empty()) {
                camera = fitCamera(world.grids(), aspect());
                cameraFitted = true;
            }
        }
    }

    // ---- live: one fetch at a time
    void startFetch(const std::string& path);

    void pump(double t) {
        if (!live) return;
        for (;;) {
            const AsyncPoll::Next n = live->advance();
            if (n.needs) {
                startFetch(n.path);
                return;
            }
            adopt(live->snapshot(), live->status(), t);
            requests = live->core().requests();
            if (n.delay < 0) {
                stopped = true;
                return;
            }
            double delay = n.delay;
            // PollCore timed its step without the fetches; the cycle really began at cycleStart.
            if (live->status().state == State::Live && cycleStart >= 0)
                delay = std::max(0.0, period - (t - cycleStart));
            nextAt = t + delay;
            cycleStart = -1;
            return;
        }
    }

    void onFetched(Response r) {
        inFlight = false;
        if (!live) return;
        live->provide(fetchPath, std::move(r));
        pump(nowSeconds());
    }

    void tickPoll(double t) {
        if (stopped || t < nextAt) return;
        if (mode == Mode::Demo && demoCore) {
            const double d = demoCore->step();
            adopt(demoCore->snapshot(), demoCore->status(), t);
            requests = demoCore->requests();
            if (d < 0) stopped = true;
            else nextAt = t + d;
        } else if (mode == Mode::Live && live && !inFlight) {
            cycleStart = t;
            pump(t);
        }
    }

    // ---- input
    void handle(const StrataEvent& e) {
        if (e.type == STRATA_EVENT_MOUSE_WHEEL) {
            camera.zoom(e.y > 0 ? 0.88f : 1.0f / 0.88f);
        } else if (e.type == STRATA_EVENT_MOUSE_BUTTON_DOWN && e.button == 1) {
            float x = e.x, y = e.y;
            toDrawable(x, y);
            dragging = true;
            downX = lastX = x;
            downY = lastY = y;
        } else if (e.type == STRATA_EVENT_MOUSE_BUTTON_UP && e.button == 1) {
            float x = e.x, y = e.y;
            toDrawable(x, y);
            dragging = false;
            const float slop = 6.0f * static_cast<float>(w / std::max(1.0, cssW));
            if (std::fabs(x - downX) + std::fabs(y - downY) < slop) pickAt(x, y);
        } else if (e.type == STRATA_EVENT_MOUSE_MOTION && dragging) {
            float x = e.x, y = e.y;
            toDrawable(x, y);
            const float perPixel = camera.distance / static_cast<float>(std::max(1, h)) * 0.75f;
            camera.pan(-(x - lastX) * perPixel, (y - lastY) * perPixel);
            lastX = x;
            lastY = y;
        } else if (e.type == STRATA_EVENT_KEY_DOWN) {
            key(e.scancode, e.repeat != 0);
        }
    }

    void key(int32_t scancode, bool repeat) {
        const float step = camera.distance * 0.04f;
        switch (scancode) {
            case kW: case kUp: camera.pan(0, step); break;
            case kS: case kDown: camera.pan(0, -step); break;
            case kA: case kLeft: camera.pan(-step, 0); break;
            case kD: case kRight: camera.pan(step, 0); break;
            case kQ: camera.yawDegrees -= 15.0f; break;
            case kE: camera.yawDegrees += 15.0f; break;
            case kEquals: case kKpPlus: camera.zoom(0.9f); break;
            case kMinus: case kKpMinus: camera.zoom(1.0f / 0.9f); break;
            case kF: if (!world.grids().empty()) camera = fitCamera(world.grids(), aspect()); break;
            case kTab: if (!repeat) hud = !hud; break;
            default: break;
        }
    }

    void pickAt(float px, float py) {
        if (!scene.pick(*engine, camera.toStrata(aspect()), w, h, px, py, pickGrid, pickX, pickY))
            pickGrid = pickX = pickY = -1;
    }

    // ---- drawing
    void text(float x, float y, float size, const float* color, const std::string& s) {
        StrataText t{};
        t.x = x;
        t.y = y;
        t.pixel_size = size;
        t.flags = STRATA_TEXT_SHADOW;
        std::copy(color, color + 4, t.color);
        strings.push_back(printable(s));
        texts.push_back(t);
    }

    std::string stateLine(double t) const {
        char line[256];
        const double age = world.lastUpdate() >= 0 ? t - world.lastUpdate() : 0;
        switch (status.state) {
            case State::Live:
                if (mode == Mode::Demo)
                    std::snprintf(line, sizeof line, "demo: a recorded run, frame %zu of %zu", demo ? demo->frame() + 1 : 0,
                                  demo ? demo->recording().framesJson.size() : 0);
                else
                    std::snprintf(line, sizeof line, "live  updated %.1f s ago  poll %.0f ms", age, world.latencyMs());
                break;
            case State::Connecting:
                std::snprintf(line, sizeof line, "connecting to %s", mode == Mode::Demo ? "the recording" : baseUrl.c_str());
                break;
            case State::Busy:
                std::snprintf(line, sizeof line, "Viron busy  retrying in %.1f s", status.nextDelay);
                break;
            case State::Stale:
                std::snprintf(line, sizeof line, "stale for %.0f s  %s", staleSince >= 0 ? t - staleSince : 0.0,
                              status.message.c_str());
                break;
            default:
                std::snprintf(line, sizeof line, "%s", status.message.c_str());
                break;
        }
        return line;
    }

    void buildHud(const StrataCamera& cam, double t) {
        strings.clear();
        texts.clear();
        if (!hud || mode == Mode::None) return;
        static const float white[4] = {0.95f, 0.95f, 0.92f, 1}, dim[4] = {0.6f, 0.63f, 0.68f, 1},
                           green[4] = {0.45f, 0.95f, 0.5f, 1}, amber[4] = {1.0f, 0.75f, 0.2f, 1},
                           red[4] = {1.0f, 0.35f, 0.3f, 1}, pick[4] = {1.0f, 0.78f, 0.15f, 1},
                           blue[4] = {0.5f, 0.75f, 1.0f, 1};
        // 8x8 font, scaled by drawable pixels per CSS pixel so it reads the same on any screen.
        const float ui = static_cast<float>(w / std::max(1.0, cssW));
        const float s = std::max(8.0f, std::round((compact ? 8.0f : 16.0f) * ui / 8.0f) * 8.0f);
        const float lh = std::round(s * 1.35f), m = std::round(s * 0.75f);
        char line[256];
        std::snprintf(line, sizeof line, "%s (#%lld)  %zu entities", world.environmentName().c_str(),
                      static_cast<long long>(world.environmentId()), world.placed());
        text(m, m, s, white, line);
        const float* c = status.state == State::Live ? (mode == Mode::Demo ? blue : green)
                         : status.state == State::Connecting ? dim
                         : (status.state == State::Busy || status.state == State::Stale) ? amber
                                                                                          : red;
        text(m, m + lh, s, c, stateLine(t));
        if (!compact) {
            std::snprintf(line, sizeof line, "polled every %.0f ms; not tick-exact", period * 1000.0);
            text(m, m + 2 * lh, std::max(8.0f, s * 0.5f), dim, line);
            for (const Grid& g : world.grids()) {
                const auto o = gridOrigin(layoutOf(g));
                const float p[3] = {static_cast<float>(o[0]) + static_cast<float>(g.rows) * 0.5f, 0.0f, -0.8f};
                float px = 0, py = 0;
                if (!projectToScreen(cam, w, h, p, px, py)) continue;
                const std::string label = g.name.empty() ? "grid " + std::to_string(g.gridId) : g.name;
                text(std::round(px - static_cast<float>(label.size()) * s * 0.5f), std::round(py - s), s, dim, label);
            }
            if (pickGrid >= 0) {
                const auto here = world.at(pickGrid, pickX, pickY);
                std::snprintf(line, sizeof line, "grid %d (%d, %d): %zu here", pickGrid, pickX, pickY, here.size());
                float y = m + 4 * lh;
                text(m, y, s, pick, line);
                for (size_t i = 0; i < here.size() && i < 12; ++i) {
                    y += lh;
                    std::snprintf(line, sizeof line, "  #%lld %s", static_cast<long long>(here[i]->entityId),
                                  here[i]->name.c_str());
                    text(m, y, s, white, line);
                }
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
        const bool dimmed = status.state != State::Live && status.state != State::Connecting;
        const auto& items = scene.buildItems(world, t, dimmed);
        if (!items.empty()) engine->submitPrimitives(items.data(), items.size());
        const auto& lines = scene.buildLines(world, pickGrid, pickX, pickY);
        if (!lines.empty()) engine->submitLines(lines.data(), lines.size());
        buildHud(cam, t);
        if (!texts.empty()) engine->submitText(texts.data(), texts.size());
        engine->endFrame();
    }

    void frame() {
        StrataEvent e;
        while (engine->pollEvent(e)) handle(e);
        const double t = nowSeconds();
        tickPoll(t);
        world.prune(t);
        if (status.state == State::Live) staleSince = -1;
        else if (staleSince < 0 && world.lastUpdate() >= 0) staleSince = t;
        draw(t);
        engine->present();
    }
};

App* app = nullptr;

// ---- emscripten_fetch: GET only
struct FetchTag {
    uint32_t generation;
};

std::string headerValue(const char* raw, const char* wanted) {
    std::istringstream in(raw ? raw : "");
    std::string line;
    while (std::getline(in, line)) {
        const size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string name = line.substr(0, colon);
        std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return std::tolower(c); });
        if (name != wanted) continue;
        std::string v = line.substr(colon + 1);
        while (!v.empty() && (v.back() == '\r' || v.back() == ' ')) v.pop_back();
        while (!v.empty() && v.front() == ' ') v.erase(v.begin());
        return v;
    }
    return "";
}

void fetchDone(emscripten_fetch_t* f) {
    auto* tag = static_cast<FetchTag*>(f->userData);
    const bool current = app && tag && tag->generation == app->generation;
    Response r;
    if (current) {
        r.status = f->status;
        r.millis = (nowSeconds() - app->fetchStarted) * 1000.0;
        if (f->data && f->numBytes > 0) r.body.assign(f->data, static_cast<size_t>(f->numBytes));
        const size_t n = emscripten_fetch_get_response_headers_length(f);
        if (n > 0) {
            std::vector<char> raw(n + 1, '\0');
            emscripten_fetch_get_response_headers(f, raw.data(), raw.size());
            const std::string retry = headerValue(raw.data(), "retry-after");
            if (!retry.empty()) r.headers["retry-after"] = retry;
        }
        if (r.status == 0)
            r.error = "no answer: Viron unreachable, or this page's origin is not in its VIRON_CORS_ALLOWED_ORIGINS";
    }
    delete tag;
    emscripten_fetch_close(f);
    if (current) app->onFetched(std::move(r));
}

void App::startFetch(const std::string& path) {
    fetchPath = path;
    fetchStarted = nowSeconds();
    inFlight = true;
    emscripten_fetch_attr_t attr;
    emscripten_fetch_attr_init(&attr);
    std::strcpy(attr.requestMethod, "GET");
    attr.attributes = EMSCRIPTEN_FETCH_LOAD_TO_MEMORY;
    attr.timeoutMSecs = 8000;
    const char* withToken[] = {"Authorization", bearer.c_str(), "Accept", "application/json", nullptr};
    const char* withoutToken[] = {"Accept", "application/json", nullptr};
    attr.requestHeaders = bearer.empty() ? withoutToken : withToken;
    attr.onsuccess = fetchDone;
    attr.onerror = fetchDone;
    attr.userData = new FetchTag{generation};
    const std::string url = baseUrl + path;
    emscripten_fetch(&attr, url.c_str());
}

void frame(void*) {
    try {
        app->frame();
    } catch (const std::exception& ex) {
        static std::string last;  // said once, not every frame
        if (last != ex.what()) std::fprintf(stderr, "viron-strata: %s\n", ex.what());
        last = ex.what();
    }
}

std::string trim(std::string s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
    size_t i = 0;
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
    return s.substr(i);
}

}  // namespace

extern "C" {

// Plays the built-in recording. 1 on success.
EMSCRIPTEN_KEEPALIVE int vs_demo() {
    if (!app) return 0;
    try {
        std::ifstream in("/demo/recording.json", std::ios::binary);
        std::stringstream ss;
        ss << in.rdbuf();
        Recording rec = parseRecording(ss.str());
        app->reset(Mode::Demo);
        app->period = rec.periodSeconds;
        PollConfig pc;
        pc.environment = std::to_string(rec.environmentId);
        pc.periodSeconds = rec.periodSeconds;
        app->demo = std::make_unique<DemoTransport>(std::move(rec), nowSeconds);
        app->demoCore = std::make_unique<PollCore>(*app->demo, pc);
        return 1;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "viron-strata: the demo recording: %s\n", e.what());
        return 0;
    }
}

// Watches environment `environment` (an id or a name) on the Viron at `url`
// with `token` (may be empty). 1 if the arguments were usable.
EMSCRIPTEN_KEEPALIVE int vs_connect(const char* url, const char* token, const char* environment, double periodMs) {
    if (!app || !url || !environment) return 0;
    std::string base = trim(url);
    while (!base.empty() && base.back() == '/') base.pop_back();
    if (base.rfind("http://", 0) != 0 && base.rfind("https://", 0) != 0) return 0;
    const std::string env = trim(environment);
    if (env.empty()) return 0;
    app->reset(Mode::Live);
    app->baseUrl = base;
    const std::string tok = token ? trim(token) : "";
    if (!tok.empty()) app->bearer = "Bearer " + tok;
    app->period = std::min(10.0, std::max(0.05, periodMs / 1000.0));
    PollConfig pc;
    pc.environment = env;
    pc.periodSeconds = app->period;
    app->live = std::make_unique<AsyncPoll>(pc);
    return 1;
}

// The canvas is cssW x cssH CSS pixels, drawn at w x h window units (the page
// caps the device pixel ratio); `compact` is a narrow page.
EMSCRIPTEN_KEEPALIVE void vs_resize(double w, double h, double cssW, double cssH, int compact) {
    if (!app || w < 1 || h < 1 || cssW < 1 || cssH < 1) return;
    try {
        app->engine->resize(static_cast<int32_t>(w), static_cast<int32_t>(h));
        app->engine->drawableSize(app->w, app->h);
        app->winW = w;
        app->winH = h;
        app->cssW = cssW;
        app->cssH = cssH;
        app->compact = compact != 0;
        if (!app->world.grids().empty()) app->camera = fitCamera(app->world.grids(), app->aspect());
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "viron-strata: resize: %s\n", ex.what());
    }
}

// A key from a page button (an SDL scancode, as the keyboard sends).
EMSCRIPTEN_KEEPALIVE void vs_key(int scancode) {
    if (app) app->key(scancode, false);
}

EMSCRIPTEN_KEEPALIVE void vs_zoom(double factor) {
    if (app && factor > 0) app->camera.zoom(static_cast<float>(factor));
}

// Picks the cell under a point of the canvas, in CSS pixels (no SDL).
EMSCRIPTEN_KEEPALIVE void vs_click(double x, double y) {
    if (!app || app->cssW <= 0 || app->cssH <= 0) return;
    app->pickAt(static_cast<float>(x * app->w / app->cssW), static_cast<float>(y * app->h / app->cssH));
}

// The view's state for the page, as JSON. Never includes the token.
EMSCRIPTEN_KEEPALIVE const char* vs_status() {
    if (!app) return "{}";
    App& a = *app;
    const double t = nowSeconds();
    std::string o = "{\"mode\":";
    jsonString(o, a.mode == Mode::Demo ? "demo" : a.mode == Mode::Live ? "live" : "none");
    o += ",\"state\":";
    jsonString(o, stateName(a.status.state));
    o += ",\"line\":";
    jsonString(o, a.mode == Mode::None ? "" : a.stateLine(t));
    o += ",\"stopped\":";
    o += a.stopped ? "true" : "false";
    o += ",\"environment\":";
    jsonString(o, a.world.environmentName());
    o += ",\"environmentId\":" + std::to_string(a.world.environmentId());
    o += ",\"entities\":" + std::to_string(a.world.placed());
    o += ",\"grids\":" + std::to_string(a.world.grids().size());
    o += ",\"poll\":" + std::to_string(a.snapshot.poll);
    o += ",\"requests\":" + std::to_string(a.requests);
    if (a.demo) {
        o += ",\"frame\":" + std::to_string(a.demo->frame());
        o += ",\"frames\":" + std::to_string(a.demo->recording().framesJson.size());
        o += ",\"loops\":" + std::to_string(a.demo->loops());
    }
    o += ",\"pick\":";
    if (a.pickGrid >= 0) {
        o += "{\"grid\":" + std::to_string(a.pickGrid) + ",\"x\":" + std::to_string(a.pickX) +
             ",\"y\":" + std::to_string(a.pickY) + ",\"entities\":[";
        bool first = true;
        for (const Track* tr : a.world.at(a.pickGrid, a.pickX, a.pickY)) {
            if (!first) o += ',';
            first = false;
            o += "{\"id\":" + std::to_string(tr->entityId) + ",\"name\":";
            jsonString(o, tr->name);
            o += '}';
        }
        o += "]}";
    } else {
        o += "null";
    }
    o += '}';
    a.statusJson = std::move(o);
    return a.statusJson.c_str();
}

}  // extern "C"

int main() {
    static App a;
    app = &a;
    double cw = 0, ch = 0;
    emscripten_get_element_css_size("#canvas", &cw, &ch);
    if (cw < 1 || ch < 1) {
        cw = 1280;
        ch = 720;
    }
    strata::EngineDesc desc;
    desc.surface = STRATA_SURFACE_WINDOW;
    desc.width = static_cast<int32_t>(cw);
    desc.height = static_cast<int32_t>(ch);
    desc.title = "viron-strata";
    try {
        a.engine = strata::Engine::create(desc);
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "viron-strata: cannot start the renderer: %s\n", ex.what());
        std::string js = "if (Module.onRendererFailed) Module.onRendererFailed(";
        jsonString(js, ex.what());
        js += ")";
        emscripten_run_script(js.c_str());
        return 1;
    }
    a.light = strata::lightingDefaults(STRATA_LIGHTING_SUN);
    a.light.time_of_day = 0.4f;
    a.light.latitude_degrees = 35.0f;
    a.light.fog = 0;
    a.light.cascade_count = 2;
    a.engine->drawableSize(a.w, a.h);
    a.cssW = a.winW = cw;  // the window was created at the CSS size
    a.cssH = a.winH = ch;
    a.compact = cw < 760;
    emscripten_run_script("if (Module.onViewerReady) Module.onViewerReady()");
    emscripten_set_main_loop_arg(frame, nullptr, 0, false);
    return 0;
}
