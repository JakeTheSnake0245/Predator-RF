#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// Fox Hunt IQ replay engine (header-only, pure stdlib, unit-testable).
//
// Owns the TX worker thread for the Fox Hunt tab:
//   * loads an IQ file (recorder-style WAV 2-ch PCM16/F32, raw .cf32/.fc32,
//     raw .cs16/.sc16) fully into memory, reporting a clipping fraction;
//   * or generates a built-in tone / CW beacon;
//   * streams to an injected write callback (the TxDriver) honoring
//     once-vs-repeat, duty cycle (TX N s / silent M s), periodic Morse CW ID,
//     and a max-continuous-TX dead-man timer;
//   * publishes an atomic status snapshot for the UI.
//
// The TX driver is injected as std::function callbacks so this header has no
// dependency on tx_driver.h, ImGui or the sigpath — the whole engine builds
// and runs under a single `g++ -std=c++17` invocation (see
// tests/foxhunt_replay_engine_test.cpp).
//
// SAFETY: the engine never starts unless start() is called (UI requires the
// ARM switch). Any stop condition (STOP button, dead-man, write error, file
// end without repeat) drops samples immediately and calls the injected
// stopFn so the driver kills RF output.
// ─────────────────────────────────────────────────────────────────────────────
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace predator::foxhunt {

    // ── IQ file loading ─────────────────────────────────────────────────────
    struct IQFile {
        std::vector<std::complex<float>> samples;
        double sampleRate = 0.0;       // 0 = unknown (raw formats)
        double clipFraction = 0.0;     // fraction of samples with |I| or |Q| > 0.999
        std::string error;             // non-empty = load failed
        bool ok() const { return error.empty(); }
    };

    namespace detail {
        inline uint32_t rdU32(const uint8_t* p) {
            return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
        }
        inline uint16_t rdU16(const uint8_t* p) {
            return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
        }
        inline std::string lowerExt(const std::string& path) {
            auto dot = path.find_last_of('.');
            if (dot == std::string::npos) { return ""; }
            std::string ext = path.substr(dot + 1);
            for (auto& c : ext) { c = (char)std::tolower((unsigned char)c); }
            return ext;
        }
    }

    // Max file size loaded into RAM (samples are expanded to 8 bytes each).
    // 64M complex samples = 512 MB of float IQ — plenty for beacon loops and
    // a hard wall against accidentally selecting a multi-GB capture.
    inline constexpr size_t kMaxIQSamples = 64ull * 1024 * 1024;

    // Load a recorder-style RIFF WAV: 2 channels = I/Q, PCM16 or IEEE float32.
    inline IQFile loadWavIQ(const std::string& path) {
        IQFile out;
        std::ifstream f(path, std::ios::binary);
        if (!f) { out.error = "Cannot open file"; return out; }
        uint8_t hdr[12];
        if (!f.read((char*)hdr, 12) || std::memcmp(hdr, "RIFF", 4) || std::memcmp(hdr + 8, "WAVE", 4)) {
            out.error = "Not a RIFF WAVE file";
            return out;
        }
        uint16_t fmt = 0, channels = 0, bits = 0;
        uint32_t rate = 0;
        bool haveFmt = false;
        // Chunk walk
        uint8_t ch[8];
        while (f.read((char*)ch, 8)) {
            uint32_t sz = detail::rdU32(ch + 4);
            if (!std::memcmp(ch, "fmt ", 4)) {
                std::vector<uint8_t> body(sz < 16 ? 16 : sz, 0);
                if (!f.read((char*)body.data(), sz)) { out.error = "Truncated fmt chunk"; return out; }
                fmt      = detail::rdU16(body.data() + 0);
                channels = detail::rdU16(body.data() + 2);
                rate     = detail::rdU32(body.data() + 4);
                bits     = detail::rdU16(body.data() + 14);
                haveFmt = true;
                if (sz & 1) { f.seekg(1, std::ios::cur); }
            }
            else if (!std::memcmp(ch, "data", 4)) {
                if (!haveFmt) { out.error = "data chunk before fmt"; return out; }
                if (channels != 2) { out.error = "Need 2-channel (I/Q) WAV, got " + std::to_string(channels) + "ch"; return out; }
                bool pcm16 = (fmt == 1 && bits == 16);
                bool f32   = (fmt == 3 && bits == 32);
                if (!pcm16 && !f32) { out.error = "Unsupported WAV format (need PCM16 or float32)"; return out; }
                size_t frameBytes = (size_t)channels * (bits / 8);
                size_t frames = sz / frameBytes;
                if (frames > kMaxIQSamples) { out.error = "File too large (>64M samples)"; return out; }
                out.samples.resize(frames);
                size_t clipped = 0;
                std::vector<uint8_t> buf((size_t)sz);
                if (!f.read((char*)buf.data(), sz)) { out.error = "Truncated data chunk"; return out; }
                if (pcm16) {
                    const int16_t* s = (const int16_t*)buf.data();
                    for (size_t i = 0; i < frames; i++) {
                        float re = s[i * 2]     / 32768.0f;
                        float im = s[i * 2 + 1] / 32768.0f;
                        if (std::fabs(re) > 0.999f || std::fabs(im) > 0.999f) { clipped++; }
                        out.samples[i] = { re, im };
                    }
                }
                else {
                    const float* s = (const float*)buf.data();
                    for (size_t i = 0; i < frames; i++) {
                        float re = s[i * 2], im = s[i * 2 + 1];
                        if (std::fabs(re) > 0.999f || std::fabs(im) > 0.999f) { clipped++; }
                        out.samples[i] = { re, im };
                    }
                }
                out.sampleRate = (double)rate;
                out.clipFraction = frames ? (double)clipped / (double)frames : 0.0;
                return out;
            }
            else {
                f.seekg(sz + (sz & 1), std::ios::cur);
            }
        }
        out.error = "No data chunk found";
        return out;
    }

    // Raw interleaved complex float32 (.cf32/.fc32) or int16 (.cs16/.sc16).
    inline IQFile loadRawIQ(const std::string& path, bool int16) {
        IQFile out;
        std::ifstream f(path, std::ios::binary | std::ios::ate);
        if (!f) { out.error = "Cannot open file"; return out; }
        std::streamoff size = f.tellg();
        f.seekg(0);
        size_t sampleBytes = int16 ? 4 : 8;
        size_t frames = (size_t)size / sampleBytes;
        if (frames == 0) { out.error = "Empty file"; return out; }
        if (frames > kMaxIQSamples) { out.error = "File too large (>64M samples)"; return out; }
        std::vector<uint8_t> buf(frames * sampleBytes);
        if (!f.read((char*)buf.data(), buf.size())) { out.error = "Read failed"; return out; }
        out.samples.resize(frames);
        size_t clipped = 0;
        if (int16) {
            const int16_t* s = (const int16_t*)buf.data();
            for (size_t i = 0; i < frames; i++) {
                float re = s[i * 2]     / 32768.0f;
                float im = s[i * 2 + 1] / 32768.0f;
                if (std::fabs(re) > 0.999f || std::fabs(im) > 0.999f) { clipped++; }
                out.samples[i] = { re, im };
            }
        }
        else {
            const float* s = (const float*)buf.data();
            for (size_t i = 0; i < frames; i++) {
                float re = s[i * 2], im = s[i * 2 + 1];
                if (std::fabs(re) > 0.999f || std::fabs(im) > 0.999f) { clipped++; }
                out.samples[i] = { re, im };
            }
        }
        out.clipFraction = (double)clipped / (double)frames;
        return out;
    }

    // Dispatch by extension. wav→WAV, cf32/fc32/raw→float32, cs16/sc16→int16.
    inline IQFile loadIQFile(const std::string& path) {
        std::string ext = detail::lowerExt(path);
        if (ext == "wav") { return loadWavIQ(path); }
        if (ext == "cf32" || ext == "fc32" || ext == "raw") { return loadRawIQ(path, false); }
        if (ext == "cs16" || ext == "sc16") { return loadRawIQ(path, true); }
        IQFile out;
        out.error = "Unknown extension ." + ext + " (need .wav/.cf32/.fc32/.cs16/.sc16/.raw)";
        return out;
    }

    inline bool isIQFileName(const std::string& name) {
        std::string ext = detail::lowerExt(name);
        return ext == "wav" || ext == "cf32" || ext == "fc32" || ext == "cs16" || ext == "sc16" || ext == "raw";
    }

    // ── Morse / CW ──────────────────────────────────────────────────────────
    // Returns a dot/dash string per character, empty for unsupported chars.
    inline const char* morseFor(char c) {
        switch (std::toupper((unsigned char)c)) {
            case 'A': return ".-";    case 'B': return "-...";  case 'C': return "-.-.";
            case 'D': return "-..";   case 'E': return ".";     case 'F': return "..-.";
            case 'G': return "--.";   case 'H': return "....";  case 'I': return "..";
            case 'J': return ".---";  case 'K': return "-.-";   case 'L': return ".-..";
            case 'M': return "--";    case 'N': return "-.";    case 'O': return "---";
            case 'P': return ".--.";  case 'Q': return "--.-";  case 'R': return ".-.";
            case 'S': return "...";   case 'T': return "-";     case 'U': return "..-";
            case 'V': return "...-";  case 'W': return ".--";   case 'X': return "-..-";
            case 'Y': return "-.--";  case 'Z': return "--..";
            case '0': return "-----"; case '1': return ".----"; case '2': return "..---";
            case '3': return "...--"; case '4': return "....-"; case '5': return ".....";
            case '6': return "-...."; case '7': return "--..."; case '8': return "---..";
            case '9': return "----."; case '/': return "-..-."; case '-': return "-....-";
            default:  return "";
        }
    }

    // Generate keyed-carrier CW at `offsetHz` from center for `text` at
    // `wpm` words/min. Amplitude 0.9 to stay clear of full scale.
    inline std::vector<std::complex<float>> generateCW(const std::string& text, double sampleRate,
                                                       double offsetHz, int wpm) {
        std::vector<std::complex<float>> out;
        if (wpm < 5) { wpm = 5; }
        if (wpm > 60) { wpm = 60; }
        double unitSec = 1.2 / (double)wpm;            // PARIS timing
        size_t unitN = (size_t)(unitSec * sampleRate);
        if (unitN == 0) { unitN = 1; }
        double phase = 0.0;
        double dphi = 2.0 * 3.14159265358979323846 * offsetHz / sampleRate;
        auto emit = [&](size_t units, bool on) {
            size_t n = units * unitN;
            for (size_t i = 0; i < n; i++) {
                if (on) {
                    // 5 ms raised-cosine key shaping to avoid clicks.
                    size_t rampN = (size_t)(0.005 * sampleRate);
                    float env = 0.9f;
                    if (rampN > 0) {
                        if (i < rampN)          { env *= 0.5f * (1.0f - std::cos(3.14159265f * (float)i / (float)rampN)); }
                        else if (n - i <= rampN) { env *= 0.5f * (1.0f - std::cos(3.14159265f * (float)(n - i) / (float)rampN)); }
                    }
                    out.emplace_back(env * (float)std::cos(phase), env * (float)std::sin(phase));
                }
                else {
                    out.emplace_back(0.0f, 0.0f);
                }
                phase += dphi;
                if (phase > 3.14159265358979323846 * 2.0) { phase -= 3.14159265358979323846 * 2.0; }
            }
        };
        bool firstChar = true;
        for (char c : text) {
            if (c == ' ') { emit(7, false); firstChar = true; continue; }
            const char* code = morseFor(c);
            if (!*code) { continue; }
            if (!firstChar) { emit(3, false); }  // inter-character gap
            firstChar = false;
            for (const char* p = code; *p; p++) {
                if (p != code) { emit(1, false); }  // intra-character gap
                emit(*p == '-' ? 3 : 1, true);
            }
        }
        emit(3, false);  // trailing gap
        return out;
    }

    // ── Engine ──────────────────────────────────────────────────────────────
    enum class TxSource { FILE_IQ, TONE, CW_BEACON };
    enum class TxState  { IDLE, TRANSMITTING, DUTY_SILENT, STOPPED_ERROR, STOPPED_DONE, STOPPED_DEADMAN };

    struct EngineConfig {
        TxSource source = TxSource::FILE_IQ;
        std::vector<std::complex<float>> fileSamples;   // FILE_IQ payload
        double sampleRate   = 1000000.0;
        double toneOffsetHz = 1000.0;                   // TONE / CW offset from center
        bool   repeat       = false;
        bool   dutyEnabled  = false;                    // only honored when repeat
        double dutyOnSec    = 10.0;
        double dutyOffSec   = 20.0;
        std::string callsign;                           // empty = no CW ID
        bool   cwIdEnabled  = false;
        double cwIdPeriodSec = 600.0;                   // 10 min default
        int    cwWpm        = 20;
        double deadManSec   = 600.0;                    // max continuous session, 0 = off
        int    chunkSamples = 8192;
    };

    struct EngineStatus {
        TxState state = TxState::IDLE;
        double elapsedSec = 0.0;        // since start()
        double txSec = 0.0;             // cumulative carrier-on time
        double nextBurstInSec = -1.0;   // >=0 during DUTY_SILENT
        double nextIdInSec = -1.0;      // >=0 while running with CW ID enabled
        std::string error;
    };

    // Worker-thread replay engine. write/stop are injected; write must block
    // (driver backpressure paces us) and return <0 on stream failure.
    class ReplayEngine {
    public:
        using WriteFn = std::function<int(const std::complex<float>*, int)>;
        using StopFn  = std::function<void()>;
        // Test hook: virtual clock. Defaults to steady_clock seconds.
        using ClockFn = std::function<double()>;

        ~ReplayEngine() { stop(); }

        bool running() const { return workerRunning.load(); }

        EngineStatus status() {
            std::lock_guard<std::mutex> lck(stMtx);
            return st;
        }

        // Start the worker. Returns false if already running or config invalid.
        bool start(EngineConfig config, WriteFn writeFn, StopFn stopFn, ClockFn clockFn = nullptr) {
            if (workerRunning.load()) { return false; }
            if (!writeFn) { return false; }
            if (config.source == TxSource::FILE_IQ && config.fileSamples.empty()) { return false; }
            if (config.sampleRate <= 0) { return false; }
            if ((config.source == TxSource::CW_BEACON || config.cwIdEnabled) && config.callsign.empty()
                && config.source == TxSource::CW_BEACON) { return false; }
            stopRequested.store(false);
            cfg = std::move(config);
            wfn = std::move(writeFn);
            sfn = std::move(stopFn);
            clk = clockFn ? std::move(clockFn) : ClockFn([]() {
                return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
            });
            {
                std::lock_guard<std::mutex> lck(stMtx);
                st = EngineStatus{};
                st.state = TxState::TRANSMITTING;
            }
            workerRunning.store(true);
            worker = std::thread(&ReplayEngine::run, this);
            return true;
        }

        // Request stop and join. Safe to call from UI thread / destructor.
        void stop() {
            stopRequested.store(true);
            if (worker.joinable()) { worker.join(); }
        }

    private:
        void setState(TxState s, const std::string& err = "") {
            std::lock_guard<std::mutex> lck(stMtx);
            st.state = s;
            if (!err.empty()) { st.error = err; }
        }

        void run() {
            double t0 = clk();
            double lastIdAt = t0;
            // CW payloads are pre-generated once.
            std::vector<std::complex<float>> idSamples;
            if (cfg.cwIdEnabled && !cfg.callsign.empty()) {
                idSamples = generateCW(cfg.callsign, cfg.sampleRate, cfg.toneOffsetHz, cfg.cwWpm);
            }
            std::vector<std::complex<float>> beacon;
            if (cfg.source == TxSource::CW_BEACON) {
                beacon = generateCW(cfg.callsign, cfg.sampleRate, cfg.toneOffsetHz, cfg.cwWpm);
                // pad 2 s of silence between beacon repetitions
                beacon.resize(beacon.size() + (size_t)(2.0 * cfg.sampleRate), { 0.0f, 0.0f });
            }

            size_t filePos = 0;
            double tonePhase = 0.0;
            const double dphi = 2.0 * 3.14159265358979323846 * cfg.toneOffsetHz / cfg.sampleRate;
            std::vector<std::complex<float>> chunk((size_t)cfg.chunkSamples);
            // duty cycle bookkeeping
            bool dutyOn = true;
            double dutyPhaseStart = t0;
            const bool duty = cfg.repeat && cfg.dutyEnabled && cfg.dutyOnSec > 0.0 && cfg.dutyOffSec > 0.0;
            // ID playback state: when >=0, we are keying the ID instead of payload
            size_t idPos = (size_t)-1;

            TxState endState = TxState::STOPPED_DONE;
            std::string endErr;

            while (!stopRequested.load()) {
                double now = clk();
                {
                    std::lock_guard<std::mutex> lck(stMtx);
                    st.elapsedSec = now - t0;
                    st.nextIdInSec = (cfg.cwIdEnabled && !idSamples.empty())
                                         ? std::max(0.0, cfg.cwIdPeriodSec - (now - lastIdAt)) : -1.0;
                    st.nextBurstInSec = (!dutyOn && duty)
                                         ? std::max(0.0, cfg.dutyOffSec - (now - dutyPhaseStart)) : -1.0;
                }

                // Dead-man: hard stop the whole session.
                if (cfg.deadManSec > 0.0 && (now - t0) >= cfg.deadManSec) {
                    endState = TxState::STOPPED_DEADMAN;
                    endErr = "Dead-man timer expired";
                    break;
                }

                // Duty-cycle phase flip.
                if (duty) {
                    double phaseLen = dutyOn ? cfg.dutyOnSec : cfg.dutyOffSec;
                    if ((now - dutyPhaseStart) >= phaseLen) {
                        dutyOn = !dutyOn;
                        dutyPhaseStart = now;
                        setState(dutyOn ? TxState::TRANSMITTING : TxState::DUTY_SILENT);
                    }
                }

                // While duty-silent we stream zeros (keeps the stream clocked
                // and the driver buffer primed without radiating).
                if (duty && !dutyOn) {
                    std::fill(chunk.begin(), chunk.end(), std::complex<float>(0.0f, 0.0f));
                    if (wfn(chunk.data(), (int)chunk.size()) < 0) {
                        endState = TxState::STOPPED_ERROR;
                        endErr = "TX stream write failed";
                        break;
                    }
                    continue;
                }

                // Time for a CW ID? (only while carrier phase is on)
                if (cfg.cwIdEnabled && !idSamples.empty() && idPos == (size_t)-1
                    && (now - lastIdAt) >= cfg.cwIdPeriodSec) {
                    idPos = 0;
                }

                // Fill the chunk from the active generator.
                size_t n = chunk.size();
                if (idPos != (size_t)-1) {
                    // CW ID interrupts the payload.
                    size_t remain = idSamples.size() - idPos;
                    size_t take = remain < n ? remain : n;
                    std::copy(idSamples.begin() + idPos, idSamples.begin() + idPos + take, chunk.begin());
                    std::fill(chunk.begin() + take, chunk.end(), std::complex<float>(0.0f, 0.0f));
                    idPos += take;
                    if (idPos >= idSamples.size()) {
                        idPos = (size_t)-1;
                        lastIdAt = clk();
                    }
                }
                else if (cfg.source == TxSource::TONE) {
                    for (size_t i = 0; i < n; i++) {
                        chunk[i] = { 0.9f * (float)std::cos(tonePhase), 0.9f * (float)std::sin(tonePhase) };
                        tonePhase += dphi;
                        if (tonePhase > 3.14159265358979323846 * 2.0) { tonePhase -= 3.14159265358979323846 * 2.0; }
                    }
                }
                else if (cfg.source == TxSource::CW_BEACON) {
                    for (size_t i = 0; i < n; i++) {
                        chunk[i] = beacon[filePos];
                        filePos = (filePos + 1) % beacon.size();
                    }
                }
                else {  // FILE_IQ
                    bool done = false;
                    for (size_t i = 0; i < n; i++) {
                        if (filePos >= cfg.fileSamples.size()) {
                            if (cfg.repeat) { filePos = 0; }
                            else {
                                std::fill(chunk.begin() + i, chunk.end(), std::complex<float>(0.0f, 0.0f));
                                done = true;
                                n = i;  // count only real samples as TX time
                                break;
                            }
                        }
                        chunk[i] = cfg.fileSamples[filePos++];
                    }
                    if (wfn(chunk.data(), (int)chunk.size()) < 0) {
                        endState = TxState::STOPPED_ERROR;
                        endErr = "TX stream write failed";
                        break;
                    }
                    {
                        std::lock_guard<std::mutex> lck(stMtx);
                        st.txSec += (double)n / cfg.sampleRate;
                    }
                    if (done) {
                        endState = TxState::STOPPED_DONE;
                        break;
                    }
                    continue;
                }

                if (wfn(chunk.data(), (int)chunk.size()) < 0) {
                    endState = TxState::STOPPED_ERROR;
                    endErr = "TX stream write failed";
                    break;
                }
                {
                    std::lock_guard<std::mutex> lck(stMtx);
                    st.txSec += (double)n / cfg.sampleRate;
                }
            }

            if (stopRequested.load()) { endState = TxState::IDLE; }
            if (sfn) { sfn(); }
            setState(endState, endErr);
            workerRunning.store(false);
        }

        EngineConfig cfg;
        WriteFn wfn;
        StopFn sfn;
        ClockFn clk;
        std::thread worker;
        std::atomic<bool> workerRunning{false};
        std::atomic<bool> stopRequested{false};
        std::mutex stMtx;
        EngineStatus st;
    };
}
