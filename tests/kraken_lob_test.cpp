/*
    KrakenSDR LOB decoder — C++ unit tests.

    Tests the KrakenWsIngester's JSON parseLine logic directly:
      - Well-formed doa_result messages → DecoderIngestEvent populated correctly
      - Missing / invalid fields → event suppressed
      - Non-doa_result types → suppressed
      - Bearing / confidence clamping
      - Node ID override from config vs. JSON

    Build (no SDR++ required):
        g++ -std=c++17 -O2 \
            -Icore/src \
            tests/kraken_lob_test.cpp \
            -o /tmp/kraken_lob_test && /tmp/kraken_lob_test

    The test runner calls KrakenWsIngester::testParseLine() — a
    test-only friend method that exposes parseLine() without a live
    socket.
*/

#include <cassert>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

// Pull in the full decoder_ingest.h header (header-only library).
#include "../core/src/predator/decoder_ingest.h"

// ── Minimal assert helpers ───────────────────────────────────────────────────

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond) \
    do { if (cond) { ++g_pass; } else { \
        fprintf(stderr, "FAIL  %s:%d  %s\n", __FILE__, __LINE__, #cond); \
        ++g_fail; } } while(0)

#define CHECK_EQ(a, b)   CHECK((a) == (b))
#define CHECK_NEAR(a, b, eps) CHECK(std::abs((double)(a) - (double)(b)) < (eps))
#define CHECK_CONTAINS(str, sub) CHECK((str).find(sub) != std::string::npos)

// ── Test fixture ─────────────────────────────────────────────────────────────

// A thin wrapper that exposes parseLine for unit testing.
class TestableKrakenIngester : public predator::KrakenWsIngester {
public:
    using predator::KrakenWsIngester::KrakenWsIngester;

    // Parse one line and return drained events (0 or 1).
    std::vector<predator::DecoderIngestEvent> parse(const std::string& line) {
        testParseLine(line);
        return drain(16);
    }
};

// ── Helpers ──────────────────────────────────────────────────────────────────

static std::string good_msg(double bearing = 127.5, double conf = 0.83,
                             double freq = 433920000.0,
                             double power = -42.1, double snr = 12.3,
                             double lat = 37.4, double lon = -122.1,
                             const std::string& node_id = "kraken-0") {
    return R"({"type":"doa_result","freq_hz":)" + std::to_string(freq)
        + R"(,"bearing_deg":)"     + std::to_string(bearing)
        + R"(,"bearing_std_deg":5.2)"
        + R"(,"confidence":)"      + std::to_string(conf)
        + R"(,"power_dbfs":)"      + std::to_string(power)
        + R"(,"snr_db":)"          + std::to_string(snr)
        + R"(,"gps_lat":)"         + std::to_string(lat)
        + R"(,"gps_lon":)"         + std::to_string(lon)
        + R"(,"heading_deg":0.0)"
        + R"(,"timestamp_unix":1718035200.123)"
        + R"(,"node_id":")" + node_id + R"("})"
    ;
}

// Build a clean JSON message using a helper.
static std::string make_doa(
        double bearing = 127.5, double conf = 0.83,
        double freq    = 433920000.0, double power = -42.1,
        double lat     = 37.4,        double lon   = -122.1,
        const std::string& node_id = "kraken-0",
        const std::string& type    = "doa_result") {
    nlohmann::json j;
    j["type"]           = type;
    j["freq_hz"]        = freq;
    j["bearing_deg"]    = bearing;
    j["bearing_std_deg"]= 5.2;
    j["confidence"]     = conf;
    j["power_dbfs"]     = power;
    j["snr_db"]         = 12.3;
    j["gps_lat"]        = lat;
    j["gps_lon"]        = lon;
    j["heading_deg"]    = 0.0;
    j["timestamp_unix"] = 1718035200.123;
    j["node_id"]        = node_id;
    return j.dump();
}

// ── Test cases ───────────────────────────────────────────────────────────────

void test_well_formed_message_produces_event() {
    TestableKrakenIngester ing;
    auto evs = ing.parse(make_doa());
    CHECK_EQ(evs.size(), 1u);
    if (evs.empty()) return;

    auto& ev = evs[0];
    CHECK_EQ(ev.decoder, "KRAKEN_LOB");
    CHECK_NEAR(ev.frequencyHz, 433920000.0, 1.0);
    CHECK_NEAR(ev.raw["bearing_deg"].get<double>(), 127.5, 0.01);
    CHECK_NEAR(ev.raw["confidence"].get<double>(), 0.83, 0.001);
    CHECK_NEAR(ev.raw["gps_lat"].get<double>(), 37.4, 0.0001);
    CHECK_NEAR(ev.raw["gps_lon"].get<double>(), -122.1, 0.0001);
}

void test_non_doa_type_suppressed() {
    TestableKrakenIngester ing;
    auto evs = ing.parse(make_doa(127.5, 0.83, 433920000.0, -42.1, 37.4, -122.1,
                                  "kraken-0", "status"));
    CHECK_EQ(evs.size(), 0u);
}

void test_config_type_suppressed() {
    TestableKrakenIngester ing;
    auto evs = ing.parse(make_doa(127.5, 0.83, 433920000.0, -42.1, 37.4, -122.1,
                                  "kraken-0", "config"));
    CHECK_EQ(evs.size(), 0u);
}

void test_missing_bearing_suppressed() {
    TestableKrakenIngester ing;
    nlohmann::json j;
    j["type"]       = "doa_result";
    j["freq_hz"]    = 433920000.0;
    j["confidence"] = 0.83;
    j["gps_lat"]    = 37.4;
    j["gps_lon"]    = -122.1;
    // no bearing_deg
    auto evs = ing.parse(j.dump());
    CHECK_EQ(evs.size(), 0u);
}

void test_missing_gps_suppressed() {
    TestableKrakenIngester ing;
    nlohmann::json j;
    j["type"]        = "doa_result";
    j["freq_hz"]     = 433920000.0;
    j["bearing_deg"] = 90.0;
    j["confidence"]  = 0.8;
    // no gps_lat / gps_lon
    auto evs = ing.parse(j.dump());
    CHECK_EQ(evs.size(), 0u);
}

void test_invalid_json_suppressed() {
    TestableKrakenIngester ing;
    auto evs = ing.parse("{this is not valid json}");
    CHECK_EQ(evs.size(), 0u);
}

void test_empty_line_suppressed() {
    TestableKrakenIngester ing;
    CHECK_EQ(ing.parse("").size(), 0u);
    CHECK_EQ(ing.parse("   ").size(), 0u);
}

void test_confidence_clamp() {
    // Confidence > 1.0 in the wire message should be clamped.
    TestableKrakenIngester ing;
    auto evs = ing.parse(make_doa(90.0, 1.5));   // 1.5 > 1.0
    CHECK_EQ(evs.size(), 1u);
    if (evs.empty()) return;
    double conf = evs[0].raw.value("confidence", -1.0);
    CHECK(conf >= 0.0 && conf <= 1.0);
}

void test_bearing_range_preserved() {
    TestableKrakenIngester ing;
    // 0° is valid
    auto evs0 = ing.parse(make_doa(0.0));
    CHECK_EQ(evs0.size(), 1u);
    // 359.9° is valid
    auto evs359 = ing.parse(make_doa(359.9));
    CHECK_EQ(evs359.size(), 1u);
    // Negative bearing is suppressed (unphysical in 0-360 convention)
    auto evsNeg = ing.parse(make_doa(-5.0));
    CHECK_EQ(evsNeg.size(), 0u);
}

void test_node_id_from_json() {
    TestableKrakenIngester ing;
    auto evs = ing.parse(make_doa(90.0, 0.9, 433920000.0, -40.0, 37.4, -122.1, "kraken-east"));
    CHECK_EQ(evs.size(), 1u);
    if (evs.empty()) return;
    std::string nid = evs[0].raw.value("node_id", "");
    CHECK_EQ(nid, std::string("kraken-east"));
}

void test_node_id_config_override() {
    // When the JSON node_id is empty, the ingester substitutes its configured node ID.
    TestableKrakenIngester ing;
    ing.setNodeId("override-node");
    nlohmann::json j;
    j["type"]        = "doa_result";
    j["freq_hz"]     = 433920000.0;
    j["bearing_deg"] = 90.0;
    j["confidence"]  = 0.8;
    j["gps_lat"]     = 37.4;
    j["gps_lon"]     = -122.1;
    j["node_id"]     = "";   // empty → should be replaced
    auto evs = ing.parse(j.dump());
    CHECK_EQ(evs.size(), 1u);
    if (evs.empty()) return;
    std::string nid = evs[0].raw.value("node_id", "");
    CHECK_EQ(nid, std::string("override-node"));
}

void test_frequency_stored_in_event() {
    TestableKrakenIngester ing;
    auto evs = ing.parse(make_doa(90.0, 0.8, 868300000.0));
    CHECK_EQ(evs.size(), 1u);
    if (evs.empty()) return;
    CHECK_NEAR(evs[0].frequencyHz, 868300000.0, 1.0);
}

void test_power_and_snr_stored() {
    TestableKrakenIngester ing;
    auto evs = ing.parse(make_doa(90.0, 0.8, 433920000.0, -55.0));
    CHECK_EQ(evs.size(), 1u);
    if (evs.empty()) return;
    CHECK_NEAR(evs[0].raw.value("power_dbfs", 0.0), -55.0, 0.1);
}

void test_multiple_sequential_messages() {
    TestableKrakenIngester ing;
    for (int i = 0; i < 5; ++i) {
        auto evs = ing.parse(make_doa(static_cast<double>(i * 30)));
        CHECK_EQ(evs.size(), 1u);
    }
}

// ── main ─────────────────────────────────────────────────────────────────────

int main() {
    test_well_formed_message_produces_event();
    test_non_doa_type_suppressed();
    test_config_type_suppressed();
    test_missing_bearing_suppressed();
    test_missing_gps_suppressed();
    test_invalid_json_suppressed();
    test_empty_line_suppressed();
    test_confidence_clamp();
    test_bearing_range_preserved();
    test_node_id_from_json();
    test_node_id_config_override();
    test_frequency_stored_in_event();
    test_power_and_snr_stored();
    test_multiple_sequential_messages();

    printf("\nKrakenSDR LOB decoder tests: %d passed, %d failed\n", g_pass, g_fail);
    return (g_fail > 0) ? 1 : 0;
}
