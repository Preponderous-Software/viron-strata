// The mapping against viron-usd's own numbers (test/conformance/, generated
// by tools/gen_conformance.py from viron-usd's mapping.py), and SHA-256.
#include <cmath>
#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>

#include "check.h"
#include "mapping.h"
#include "sha256.h"

using namespace viron_strata;
using nlohmann::json;

namespace {

bool near(double a, double b) { return std::fabs(a - b) <= 1e-9; }

json load() {
    std::ifstream in(VIRON_STRATA_CONFORMANCE);
    if (!in) throw std::runtime_error("cannot open " VIRON_STRATA_CONFORMANCE);
    return json::parse(in);
}

GridLayout layout(const json& g) {
    return GridLayout{g["index"].get<int>(), g["rows"].get<int>(), g["columns"].get<int>()};
}

}  // namespace

TEST("SHA-256 matches the FIPS 180-4 test vectors", "[unit]") {
    CHECK(toHex(sha256("")) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(toHex(sha256("abc")) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(toHex(sha256("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")) ==
          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    CHECK(toHex(sha256(std::string(1000000, 'a'))) ==
          "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

TEST("constants match viron-usd", "[conformance]") {
    const json doc = load();
    CHECK(doc["constants"]["GRID_GAP"].get<int>() == kGridGap);
    CHECK(near(doc["constants"]["ENTITY_SIZE"].get<double>(), kEntitySize));
    CHECK(near(doc["constants"]["MAX_OFFSET"].get<double>(), kMaxOffset));
}

TEST("entity offsets and colours match viron-usd for every recorded id", "[conformance]") {
    const json doc = load();
    REQUIRE(doc["entities"].size() > 50);
    for (const auto& e : doc["entities"]) {
        const int64_t id = e["id"].get<int64_t>();
        const auto o = entityOffset(id);
        const auto c = entityColor(id);
        CHECK(near(o[0], e["offset"][0].get<double>()));
        CHECK(near(o[1], e["offset"][1].get<double>()));
        for (int i = 0; i < 3; ++i) CHECK(near(c[i], e["color"][i].get<double>()));
    }
}

TEST("grid origins and cell centres match viron-usd (USD x,y -> Strata x,z)", "[conformance]") {
    const json doc = load();
    for (const auto& cell : doc["cells"]) {
        const GridLayout g = layout(cell["grid"]);
        const int x = cell["x"].get<int>(), y = cell["y"].get<int>();
        const auto o = gridOrigin(g);
        CHECK(near(o[0], cell["origin"][0].get<double>()));
        CHECK(near(o[2], cell["origin"][1].get<double>()));
        const auto c = cellCenter(g, x, y);
        CHECK(near(c[0], cell["center"][0].get<double>()));
        CHECK(near(c[2], cell["center"][1].get<double>()));  // Viron y: USD +Y, Strata +Z
        CHECK(near(c[1], cell["center"][2].get<double>()));  // height: USD Z, Strata Y
        // The floor mesh orders cells as viron-usd's face_index does.
        CHECK(x * g.columns + y == cell["face"].get<int>());
    }
    for (const auto& p : doc["positions"]) {
        const GridLayout g = layout(p["grid"]);
        const auto pos = entityPosition(g, p["x"].get<int>(), p["y"].get<int>(), p["id"].get<int64_t>());
        CHECK(near(pos[0], p["position"][0].get<double>()));
        CHECK(near(pos[2], p["position"][1].get<double>()));
        CHECK(near(pos[1], p["position"][2].get<double>()));
    }
}
