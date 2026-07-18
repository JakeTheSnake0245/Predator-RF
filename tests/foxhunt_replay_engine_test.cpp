// Fox Hunt replay engine unit tests.
// Build: g++ -std=c++17 -O2 -Icore/src tests/foxhunt_replay_engine_test.cpp -o /tmp/fhre -pthread && /tmp/fhre
#include <predator/foxhunt/replay_engine.h>
#include <cassert>
#include <cstdio>
#include <cstdlib>

using namespace predator::foxhunt;

static int checks = 0;
#define CHECK(cond) do { checks++; if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); exit(1); } } while (0)

// Virtual-clock write sink: every write() advances a fake clock by the chunk
// duration so duty-cycle / dead-man tests run instantly without sleeping.
struct FakeSink {
    double sampleRate;
    double t = 0.0;
    long long total = 0;
    long long nonZero = 0;
    int failAfterWrites = -1;
    int writes = 0;

    int write(const std::complex<float>* s, int n) {
        writes++;
        if (failAfterWrites >= 0 && writes > failAfterWrites) { return -1; }
        for (int i = 0; i < n; i++) {
            if (std::abs(s[i].real()) > 1e-6f || std::abs(s[i].imag()) > 1e-6f) { nonZero++; }
        }
        total += n;
        t += (double)n / sampleRate;
        return n;
    }
};

static void waitDone(ReplayEngine& e) {
    while (e.running()) { std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
}

// ── WAV / raw loaders ────────────────────────────────────────────────────────
static void writeTestWav(const char* path, bool floatFmt, int frames, float amp) {
    FILE* f = fopen(path, "wb");
    int bytesPer = floatFmt ? 4 : 2;
    uint32_t dataSz = frames * 2 * bytesPer;
    uint32_t riffSz = 36 + dataSz;
    uint16_t fmt = floatFmt ? 3 : 1, ch = 2, bits = floatFmt ? 32 : 16;
    uint32_t rate = 48000, byteRate = rate * ch * bytesPer;
    uint16_t block = ch * bytesPer;
    fwrite("RIFF", 1, 4, f); fwrite(&riffSz, 4, 1, f); fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f); uint32_t fsz = 16; fwrite(&fsz, 4, 1, f);
    fwrite(&fmt, 2, 1, f); fwrite(&ch, 2, 1, f); fwrite(&rate, 4, 1, f);
    fwrite(&byteRate, 4, 1, f); fwrite(&block, 2, 1, f); fwrite(&bits, 2, 1, f);
    fwrite("data", 1, 4, f); fwrite(&dataSz, 4, 1, f);
    for (int i = 0; i < frames; i++) {
        if (floatFmt) { float v[2] = { amp, -amp }; fwrite(v, 4, 2, f); }
        else { int16_t v[2] = { (int16_t)(amp * 32767), (int16_t)(-amp * 32767) }; fwrite(v, 2, 2, f); }
    }
    fclose(f);
}

int main() {
    // 1. WAV PCM16 loads with rate + no clipping at 0.5 amp
    writeTestWav("/tmp/fh_a.wav", false, 1000, 0.5f);
    auto a = loadWavIQ("/tmp/fh_a.wav");
    CHECK(a.ok()); CHECK(a.samples.size() == 1000); CHECK(a.sampleRate == 48000.0);
    CHECK(a.clipFraction == 0.0);
    CHECK(std::abs(a.samples[0].real() - 0.5f) < 0.01f);

    // 2. WAV float32 full-scale flags clipping
    writeTestWav("/tmp/fh_b.wav", true, 500, 1.0f);
    auto b = loadWavIQ("/tmp/fh_b.wav");
    CHECK(b.ok()); CHECK(b.clipFraction > 0.99);

    // 3. Mono WAV rejected
    {
        FILE* f = fopen("/tmp/fh_mono.wav", "wb");
        uint32_t riffSz = 36, fsz = 16, dataSz = 0, rate = 48000, br = 96000;
        uint16_t fmt = 1, ch = 1, bits = 16, block = 2;
        fwrite("RIFF", 1, 4, f); fwrite(&riffSz, 4, 1, f); fwrite("WAVE", 1, 4, f);
        fwrite("fmt ", 1, 4, f); fwrite(&fsz, 4, 1, f);
        fwrite(&fmt, 2, 1, f); fwrite(&ch, 2, 1, f); fwrite(&rate, 4, 1, f);
        fwrite(&br, 4, 1, f); fwrite(&block, 2, 1, f); fwrite(&bits, 2, 1, f);
        fwrite("data", 1, 4, f); fwrite(&dataSz, 4, 1, f);
        fclose(f);
        auto m = loadWavIQ("/tmp/fh_mono.wav");
        CHECK(!m.ok());
    }

    // 4. Raw cf32 + cs16 loaders
    {
        FILE* f = fopen("/tmp/fh_c.cf32", "wb");
        for (int i = 0; i < 256; i++) { float v[2] = { 0.25f, 0.25f }; fwrite(v, 4, 2, f); }
        fclose(f);
        auto c = loadIQFile("/tmp/fh_c.cf32");
        CHECK(c.ok()); CHECK(c.samples.size() == 256); CHECK(c.sampleRate == 0.0);

        f = fopen("/tmp/fh_d.cs16", "wb");
        for (int i = 0; i < 128; i++) { int16_t v[2] = { 16384, -16384 }; fwrite(v, 2, 2, f); }
        fclose(f);
        auto d = loadIQFile("/tmp/fh_d.cs16");
        CHECK(d.ok()); CHECK(d.samples.size() == 128);
        CHECK(std::abs(d.samples[0].real() - 0.5f) < 0.01f);
    }

    // 5. Unknown extension rejected; isIQFileName filter
    CHECK(!loadIQFile("/tmp/foo.mp4").ok());
    CHECK(isIQFileName("x.WAV")); CHECK(isIQFileName("x.cs16")); CHECK(!isIQFileName("x.mp4")); CHECK(!isIQFileName("noext"));

    // 6. Morse generator: "E" = 1 dot on; energy present, key-shaped
    {
        auto cw = generateCW("E", 48000, 1000.0, 20);
        CHECK(!cw.empty());
        bool any = false;
        for (auto& s : cw) { if (std::abs(s) > 0.5f) { any = true; break; } }
        CHECK(any);
        // Unsupported chars skipped without crash
        auto cw2 = generateCW("@#$", 48000, 1000.0, 20);
        double e2 = 0; for (auto& s : cw2) e2 += std::abs(s);
        CHECK(e2 < 1e-3);
    }

    // 7. FILE once: plays exactly file length then STOPPED_DONE + stopFn fired
    {
        ReplayEngine e;
        FakeSink sink{ 48000.0 };
        bool stopped = false;
        EngineConfig cfg;
        cfg.source = TxSource::FILE_IQ;
        cfg.fileSamples.assign(10000, { 0.5f, 0.5f });
        cfg.sampleRate = 48000.0;
        cfg.repeat = false;
        cfg.deadManSec = 0.0;
        CHECK(e.start(cfg, [&](const std::complex<float>* s, int n) { return sink.write(s, n); },
                      [&]() { stopped = true; }, [&]() { return sink.t; }));
        waitDone(e);
        auto st = e.status();
        CHECK(st.state == TxState::STOPPED_DONE);
        CHECK(stopped);
        CHECK(sink.nonZero == 10000);
        CHECK(std::abs(st.txSec - 10000.0 / 48000.0) < 1e-6);
    }

    // 8. FILE repeat: loops until dead-man fires
    {
        ReplayEngine e;
        FakeSink sink{ 48000.0 };
        bool stopped = false;
        EngineConfig cfg;
        cfg.source = TxSource::FILE_IQ;
        cfg.fileSamples.assign(1000, { 0.5f, 0.5f });
        cfg.sampleRate = 48000.0;
        cfg.repeat = true;
        cfg.deadManSec = 2.0;
        CHECK(e.start(cfg, [&](const std::complex<float>* s, int n) { return sink.write(s, n); },
                      [&]() { stopped = true; }, [&]() { return sink.t; }));
        waitDone(e);
        auto st = e.status();
        CHECK(st.state == TxState::STOPPED_DEADMAN);
        CHECK(stopped);
        CHECK(sink.nonZero > 10000);  // looped well past one file length
    }

    // 9. Duty cycle: zeros streamed during silent phase
    {
        ReplayEngine e;
        FakeSink sink{ 48000.0 };
        EngineConfig cfg;
        cfg.source = TxSource::TONE;
        cfg.sampleRate = 48000.0;
        cfg.repeat = true;
        cfg.dutyEnabled = true;
        cfg.dutyOnSec = 0.5;
        cfg.dutyOffSec = 0.5;
        cfg.deadManSec = 2.0;
        CHECK(e.start(cfg, [&](const std::complex<float>* s, int n) { return sink.write(s, n); },
                      nullptr, [&]() { return sink.t; }));
        waitDone(e);
        auto st = e.status();
        CHECK(st.state == TxState::STOPPED_DEADMAN);
        // ~50% duty: nonZero should be roughly half of total
        double frac = (double)sink.nonZero / (double)sink.total;
        CHECK(frac > 0.3 && frac < 0.7);
        // txSec only counts on-phase writes
        CHECK(st.txSec < 1.5);
    }

    // 10. CW ID interrupts tone periodically
    {
        ReplayEngine e;
        FakeSink sink{ 48000.0 };
        EngineConfig cfg;
        cfg.source = TxSource::TONE;
        cfg.sampleRate = 48000.0;
        cfg.repeat = true;
        cfg.cwIdEnabled = true;
        cfg.callsign = "N0CALL";
        cfg.cwIdPeriodSec = 0.5;
        cfg.deadManSec = 3.0;
        CHECK(e.start(cfg, [&](const std::complex<float>* s, int n) { return sink.write(s, n); },
                      nullptr, [&]() { return sink.t; }));
        waitDone(e);
        // Some samples were CW-keyed zeros (gaps) — tone alone is 100% non-zero.
        CHECK(sink.nonZero < sink.total);
    }

    // 11. Write failure → STOPPED_ERROR
    {
        ReplayEngine e;
        FakeSink sink{ 48000.0 };
        sink.failAfterWrites = 3;
        EngineConfig cfg;
        cfg.source = TxSource::TONE;
        cfg.sampleRate = 48000.0;
        cfg.repeat = true;
        cfg.deadManSec = 0.0;
        CHECK(e.start(cfg, [&](const std::complex<float>* s, int n) { return sink.write(s, n); },
                      nullptr, [&]() { return sink.t; }));
        waitDone(e);
        CHECK(e.status().state == TxState::STOPPED_ERROR);
        CHECK(!e.status().error.empty());
    }

    // 12. Operator stop → IDLE; double-start rejected while running
    {
        ReplayEngine e;
        FakeSink sink{ 48000.0 };
        EngineConfig cfg;
        cfg.source = TxSource::TONE;
        cfg.sampleRate = 48000.0;
        cfg.repeat = true;
        cfg.deadManSec = 0.0;
        // Real clock here so it keeps running until we stop it.
        CHECK(e.start(cfg, [&](const std::complex<float>* s, int n) {
            std::this_thread::sleep_for(std::chrono::microseconds(200));
            return sink.write(s, n);
        }, nullptr));
        CHECK(!e.start(cfg, [&](const std::complex<float>*, int n) { return n; }, nullptr));
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        e.stop();
        CHECK(e.status().state == TxState::IDLE);
    }

    // 13. Invalid configs rejected
    {
        ReplayEngine e;
        EngineConfig cfg;
        cfg.source = TxSource::FILE_IQ;  // no samples
        CHECK(!e.start(cfg, [](const std::complex<float>*, int n) { return n; }, nullptr));
        cfg.source = TxSource::CW_BEACON;  // no callsign
        CHECK(!e.start(cfg, [](const std::complex<float>*, int n) { return n; }, nullptr));
    }

    printf("OK — %d checks passed\n", checks);
    return 0;
}
