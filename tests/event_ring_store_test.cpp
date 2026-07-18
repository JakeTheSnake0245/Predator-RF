// EventRingStore unit tests — durable Kujhad device event ring.
//
// Build & run:
//   g++ -std=c++17 -O2 -Icore/src tests/event_ring_store_test.cpp -o /tmp/erst && /tmp/erst
//
// Covers: append→restart→rehydrate round trip, serial continuity (counter
// restarts above the highest persisted serial, no reuse), since= replay
// equivalence pre/post restart, rotation disk bound, corrupt/truncated
// tail tolerance, open-failure memory-only degradation, capacity trim.

#include "../core/src/predator/event_ring_store.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <unistd.h>

static int gChecks = 0;
static int gFails = 0;

#define CHECK(cond) do { \
    gChecks++; \
    if (!(cond)) { gFails++; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

using predator::EventRingStore;
using nlohmann::json;

static std::string makeTempDir(const char* tag) {
    static int counter = 0;
    std::string d = std::string("/tmp/erst_test_") + tag + "_" + std::to_string(::getpid()) + "_" + std::to_string(counter++);
    std::string rm = "rm -rf " + d;
    std::system(rm.c_str());
    return d;
}

static json makeEvent(uint64_t id, const char* type) {
    json e;
    e["id"] = id;
    e["type"] = type;
    e["ts_ms"] = 1000000 + id;
    return e;
}

// since= replay identical to routeEvents in the web backend.
static std::vector<json> replaySince(const std::vector<json>& ring, uint64_t since) {
    std::vector<json> out;
    for (const auto& ev : ring) {
        if (ev.value("id", (uint64_t)0) > since) out.push_back(ev);
    }
    return out;
}

static void testRoundTripAndSerialContinuity() {
    std::string dir = makeTempDir("rt");
    // "First boot": append 10 events.
    {
        EventRingStore s;
        CHECK(s.open(dir, 512, "id"));
        CHECK(s.loaded().empty());
        CHECK(s.maxSerial() == 0);
        for (uint64_t i = 1; i <= 10; i++) s.append(makeEvent(i, "hit"));
    } // destructor closes — simulated shutdown/power loss after flush
    // "Restart": rehydrate.
    EventRingStore s2;
    CHECK(s2.open(dir, 512, "id"));
    CHECK(s2.loaded().size() == 10);
    CHECK(s2.maxSerial() == 10);
    // Content + order + timestamps preserved.
    for (size_t i = 0; i < s2.loaded().size(); i++) {
        CHECK(s2.loaded()[i]["id"].get<uint64_t>() == i + 1);
        CHECK(s2.loaded()[i]["ts_ms"].get<uint64_t>() == 1000000 + i + 1);
        CHECK(s2.loaded()[i]["type"] == "hit");
    }
    // since= replay against the rehydrated ring matches pre-restart.
    auto tail = replaySince(s2.loaded(), 7);
    CHECK(tail.size() == 3);
    CHECK(tail[0]["id"].get<uint64_t>() == 8);
    CHECK(tail[2]["id"].get<uint64_t>() == 10);
    // Serial continuity: new events start above maxSerial — appending 11
    // then reopening yields 11 with no duplicates.
    s2.append(makeEvent(s2.maxSerial() + 1, "decode"));
    EventRingStore s3;
    CHECK(s3.open(dir, 512, "id"));
    CHECK(s3.loaded().size() == 11);
    CHECK(s3.maxSerial() == 11);
    uint64_t prev = 0;
    for (const auto& ev : s3.loaded()) {
        uint64_t id = ev["id"].get<uint64_t>();
        CHECK(id > prev); // strictly monotonic — no serial reuse
        prev = id;
    }
}

static void testRotationBound() {
    std::string dir = makeTempDir("rot");
    const size_t cap = 50;
    {
        EventRingStore s;
        CHECK(s.open(dir, cap, "id"));
        for (uint64_t i = 1; i <= 5 * cap; i++) s.append(makeEvent(i, "hit"));
    }
    // Disk holds at most 2*cap events (cur + prev segments).
    EventRingStore s2;
    CHECK(s2.open(dir, cap, "id"));
    // Load trims to capacity — newest cap events.
    CHECK(s2.loaded().size() <= cap);
    CHECK(!s2.loaded().empty());
    CHECK(s2.maxSerial() == 5 * cap);
    CHECK(s2.loaded().back()["id"].get<uint64_t>() == 5 * cap);
    // Oldest retained is within the 2*cap disk bound.
    CHECK(s2.loaded().front()["id"].get<uint64_t>() >= 5 * cap - 2 * cap + 1);
    // Raw disk bound: count lines across both segments.
    size_t lines = 0;
    for (const char* f : { "/events.cur.jsonl", "/events.prev.jsonl" }) {
        std::FILE* fp = std::fopen((dir + f).c_str(), "rb");
        if (!fp) continue;
        int c;
        while ((c = std::fgetc(fp)) != EOF) if (c == '\n') lines++;
        std::fclose(fp);
    }
    CHECK(lines <= 2 * cap);
}

static void testCorruptTailTolerated() {
    std::string dir = makeTempDir("corrupt");
    {
        EventRingStore s;
        CHECK(s.open(dir, 512, "id"));
        for (uint64_t i = 1; i <= 5; i++) s.append(makeEvent(i, "hit"));
    }
    // Simulate a mid-write power cut: truncated garbage tail, no newline.
    {
        std::FILE* f = std::fopen((dir + "/events.cur.jsonl").c_str(), "ab");
        CHECK(f != nullptr);
        const char* junk = "{\"id\":6,\"type\":\"hi";
        std::fwrite(junk, 1, std::strlen(junk), f);
        std::fclose(f);
    }
    EventRingStore s2;
    CHECK(s2.open(dir, 512, "id"));
    CHECK(s2.loaded().size() == 5); // junk tail skipped, ring intact
    CHECK(s2.maxSerial() == 5);
    // Appending after the junk tail still yields parseable lines.
    s2.append(makeEvent(6, "hit"));
    EventRingStore s3;
    CHECK(s3.open(dir, 512, "id"));
    CHECK(s3.loaded().size() == 6);
    CHECK(s3.maxSerial() == 6);
}

static void testOpenFailureDegrades() {
    EventRingStore s;
    // Parent path is a file, not a dir — mkdir must fail.
    CHECK(!s.open("/dev/null/nope", 512, "id"));
    CHECK(!s.isOpen());
    s.append(makeEvent(1, "hit")); // must not crash
    CHECK(s.loaded().empty());
}

static void testSerialKeyVariant() {
    // The GUI ring uses "serial" instead of "id".
    std::string dir = makeTempDir("skey");
    {
        EventRingStore s;
        CHECK(s.open(dir, 512, "serial"));
        json e; e["serial"] = (uint64_t)42; e["type"] = "hit";
        s.append(e);
        json e2; e2["serial"] = (int64_t)43; e2["type"] = "hit"; // signed int path
        s.append(e2);
        json e3; e3["type"] = "untagged"; // no serial — still stored
        s.append(e3);
    }
    EventRingStore s2;
    CHECK(s2.open(dir, 512, "serial"));
    CHECK(s2.loaded().size() == 3);
    CHECK(s2.maxSerial() == 43);
}

static void testCapacityTrimOnLoad() {
    std::string dir = makeTempDir("trim");
    {
        EventRingStore s;
        CHECK(s.open(dir, 100, "id"));
        for (uint64_t i = 1; i <= 100; i++) s.append(makeEvent(i, "hit"));
    }
    // Reopen with a smaller capacity — only the newest 10 survive load.
    EventRingStore s2;
    CHECK(s2.open(dir, 10, "id"));
    CHECK(s2.loaded().size() == 10);
    CHECK(s2.loaded().front()["id"].get<uint64_t>() == 91);
    CHECK(s2.loaded().back()["id"].get<uint64_t>() == 100);
    CHECK(s2.maxSerial() == 100); // max tracked across ALL rows, not just kept ones
}

int main() {
    testRoundTripAndSerialContinuity();
    testRotationBound();
    testCorruptTailTolerated();
    testOpenFailureDegrades();
    testSerialKeyVariant();
    testCapacityTrimOnLoad();
    std::printf("%d checks, %d failures\n", gChecks, gFails);
    return gFails ? 1 : 0;
}
