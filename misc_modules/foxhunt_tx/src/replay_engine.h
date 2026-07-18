#pragma once
#include <atomic>
#include <thread>
#include <mutex>
#include <chrono>
#include <functional>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include "iq_file_reader.h"
#include "cw_id.h"

// Fox Hunt replay engine — the TX state machine.
//
// Owns the worker thread that streams IQ from an IqFileReader (or generates a
// built-in tone/CW beacon) into a TX sink callback. The sink is injected as a
// std::function so the engine is testable without SDR hardware or SDR++ deps.
//
// Safety model (all enforced HERE, not in the UI, so no UI bug can bypass it):
//   * ARM gate       — start() refuses unless armed() was set. Any stop
//                      (manual, dead-man, error, EOF in once-mode) DISARMS.
//   * Dead-man timer — hard cap on continuous seconds of key-down. Exceeding
//                      it stops TX and disarms; the operator must re-ARM.
//   * Clipping       — |I| or |Q| >= 0.999 counts as a clipped sample;
//                      clippedRatio() drives a UI warning.

namespace foxhunt {

    enum class TxState { IDLE, TRANSMITTING, DUTY_SILENT, STOPPING };

    struct ReplayConfig {
        // Source: file path (WAV/cf32/cs16) OR builtin tone when path empty.
        std::string filePath;
        double fileSampleRateOverride = 0.0; // for raw files with no rate token

        bool builtinTone = false;   // true = ignore file, TX a CW beacon tone
        double toneOffsetHz = 600.0;

        double sampleRate = 1000000.0; // TX sample rate (builtin tone / driver)

        bool repeat = false;
        bool dutyCycle = false;    // only honored when repeat==true
        double dutyOnSec = 10.0;
        double dutyOffSec = 20.0;

        std::string callsign;      // empty = no CW ID
        double cwIntervalMin = 10.0;
        double cwWpm = 20.0;
        float cwAmplitude = 0.7f;

        double deadManSec = 600.0; // max continuous TX (0 = disabled — UI clamps >0)
        float digitalScale = 1.0f; // extra digital backoff applied to file samples
    };

    class ReplayEngine {
    public:
        // Sink: blocking write of interleaved IQ pairs, returns pairs consumed
        // or <0 on error.
        using TxSink = std::function<int(const float*, int)>;
        // Called (from the worker thread) when TX stops for any reason.
        using StopCb = std::function<void(const std::string& reason)>;

        ~ReplayEngine() { stop(); join(); }

        void setSink(TxSink sink) { this->sink = sink; }
        void setStopCallback(StopCb cb) { stopCb = cb; }

        void arm() { armed_ = true; }
        void disarm() { armed_ = false; }
        bool armed() const { return armed_; }

        TxState state() const { return state_; }
        bool running() const { return state_ != TxState::IDLE; }

        double elapsedTxSec() const { return txSecTotal_; }
        double continuousTxSec() const { return txSecContinuous_; }
        double secToNextBurst() const { return dutyOffRemain_; }
        double secToNextId() const { return idRemain_; }
        float clippedRatio() const {
            uint64_t tot = samplesSeen_;
            return tot ? (float)((double)samplesClipped_ / (double)tot) : 0.0f;
        }
        std::string lastStopReason() {
            std::lock_guard<std::mutex> lck(mtx);
            return lastStop;
        }

        // Start transmitting. Returns false (with reason in outErr) when not
        // armed, the file cannot be opened, or no sink is set.
        bool start(const ReplayConfig& cfg, std::string& outErr) {
            if (running()) { outErr = "Already transmitting"; return false; }
            if (!armed_) { outErr = "Not armed"; return false; }
            if (!sink) { outErr = "No TX device opened"; return false; }
            join();

            this->cfg = cfg;
            if (!cfg.builtinTone) {
                if (!reader.open(cfg.filePath, cfg.fileSampleRateOverride)) {
                    outErr = "IQ file: " + reader.error();
                    return false;
                }
                if (reader.sampleRate() <= 0.0 && cfg.fileSampleRateOverride > 0.0) {
                    reader.setSampleRate(cfg.fileSampleRateOverride);
                }
                if (reader.sampleRate() <= 0.0) {
                    outErr = "Unknown sample rate for raw IQ file — set one";
                    reader.close();
                    return false;
                }
            }

            samplesSeen_ = 0;
            samplesClipped_ = 0;
            txSecTotal_ = 0.0;
            txSecContinuous_ = 0.0;
            dutyOffRemain_ = 0.0;
            idRemain_ = (cfg.callsign.empty()) ? -1.0 : cfg.cwIntervalMin * 60.0;
            stopRequested_ = false;
            state_ = TxState::TRANSMITTING;
            worker = std::thread(&ReplayEngine::run, this);
            return true;
        }

        // Request stop (big STOP button). Non-blocking; worker winds down.
        void stop() {
            if (!running()) { return; }
            stopRequested_ = true;
        }

        void join() {
            if (worker.joinable()) { worker.join(); }
        }

    private:
        void finish(const std::string& reason) {
            {
                std::lock_guard<std::mutex> lck(mtx);
                lastStop = reason;
            }
            reader.close();
            state_ = TxState::IDLE;
            armed_ = false; // every stop disarms — deliberate
            if (stopCb) { stopCb(reason); }
        }

        void trackClipping(const float* iq, int pairs) {
            for (int i = 0; i < pairs * 2; i++) {
                if (std::fabs(iq[i]) >= 0.999f) { samplesClipped_++; }
            }
            samplesSeen_ += (uint64_t)pairs * 2;
        }

        // Send one block through the sink; returns false on sink error/stop.
        bool push(const float* iq, int pairs) {
            int sent = 0;
            while (sent < pairs) {
                if (stopRequested_) { return false; }
                int r = sink(iq + sent * 2, pairs - sent);
                if (r <= 0) { return false; }
                sent += r;
            }
            return true;
        }

        // Transmit the CW ID inline. Returns false on stop/error.
        bool sendCwId(double sr) {
            std::vector<float> id = CwId::render(cfg.callsign, sr, cfg.cwWpm,
                                                 cfg.toneOffsetHz, cfg.cwAmplitude);
            int pairs = (int)(id.size() / 2);
            const int chunk = 8192;
            for (int off = 0; off < pairs; off += chunk) {
                int n = std::min(chunk, pairs - off);
                if (!push(id.data() + off * 2, n)) { return false; }
                accountTx((double)n / sr);
                if (deadManTripped()) { return false; }
            }
            return true;
        }

        void accountTx(double sec) {
            txSecTotal_ = txSecTotal_ + sec;
            txSecContinuous_ = txSecContinuous_ + sec;
        }

        bool deadManTripped() {
            if (cfg.deadManSec > 0.0 && txSecContinuous_ >= cfg.deadManSec) {
                deadMan_ = true;
                return true;
            }
            return false;
        }

        void run() {
            const double sr = cfg.builtinTone ? cfg.sampleRate : reader.sampleRate();
            const int blockPairs = std::max(1024, (int)(sr / 200.0));
            std::vector<float> buf((size_t)blockPairs * 2);
            deadMan_ = false;
            bool sinkErr = false;

            double tonePhase = 0.0;
            const double toneStep = 2.0 * 3.14159265358979323846 * cfg.toneOffsetHz / sr;
            double burstSec = 0.0; // TX seconds inside the current duty burst

            while (!stopRequested_) {
                int got;
                if (cfg.builtinTone) {
                    got = blockPairs;
                    for (int i = 0; i < got; i++) {
                        buf[i * 2] = cfg.cwAmplitude * (float)cos(tonePhase);
                        buf[i * 2 + 1] = cfg.cwAmplitude * (float)sin(tonePhase);
                        tonePhase += toneStep;
                        if (tonePhase > 6.28318530717958647692) { tonePhase -= 6.28318530717958647692; }
                    }
                }
                else {
                    got = reader.read(buf.data(), blockPairs);
                    if (got == 0) {
                        // EOF
                        if (!cfg.repeat) { finish("File finished"); return; }
                        reader.rewind();
                        continue;
                    }
                    if (cfg.digitalScale != 1.0f) {
                        for (int i = 0; i < got * 2; i++) { buf[i] *= cfg.digitalScale; }
                    }
                }

                trackClipping(buf.data(), got);
                if (!push(buf.data(), got)) { sinkErr = !stopRequested_; break; }
                double blkSec = (double)got / sr;
                accountTx(blkSec);
                burstSec += blkSec;

                // CW ID scheduling
                if (idRemain_ >= 0.0) {
                    idRemain_ = idRemain_ - blkSec;
                    if (idRemain_ <= 0.0) {
                        if (!sendCwId(sr)) {
                            if (deadMan_) { break; }
                            sinkErr = !stopRequested_;
                            break;
                        }
                        idRemain_ = cfg.cwIntervalMin * 60.0;
                    }
                }

                if (deadManTripped()) { break; }

                // Duty cycle: after dutyOnSec of burst, go silent dutyOffSec.
                if (cfg.repeat && cfg.dutyCycle && burstSec >= cfg.dutyOnSec) {
                    state_ = TxState::DUTY_SILENT;
                    txSecContinuous_ = 0.0; // key is up — continuous timer resets
                    double remain = cfg.dutyOffSec;
                    while (remain > 0.0 && !stopRequested_) {
                        dutyOffRemain_ = remain;
                        std::this_thread::sleep_for(std::chrono::milliseconds(50));
                        remain -= 0.05;
                        // Silent time still counts toward the ID interval clock.
                        if (idRemain_ > 0.0) { idRemain_ = idRemain_ - 0.05; }
                    }
                    dutyOffRemain_ = 0.0;
                    burstSec = 0.0;
                    state_ = TxState::TRANSMITTING;
                }
            }

            if (deadMan_) { finish("Dead-man timer tripped — re-ARM required"); }
            else if (sinkErr) { finish("TX device write failed"); }
            else { finish("Stopped by operator"); }
        }

        TxSink sink;
        StopCb stopCb;
        ReplayConfig cfg;
        IqFileReader reader;
        std::thread worker;
        std::mutex mtx;
        std::string lastStop;

        std::atomic<bool> armed_{ false };
        std::atomic<bool> stopRequested_{ false };
        std::atomic<TxState> state_{ TxState::IDLE };
        std::atomic<double> txSecTotal_{ 0.0 };
        std::atomic<double> txSecContinuous_{ 0.0 };
        std::atomic<double> dutyOffRemain_{ 0.0 };
        std::atomic<double> idRemain_{ -1.0 };
        std::atomic<uint64_t> samplesSeen_{ 0 };
        std::atomic<uint64_t> samplesClipped_{ 0 };
        bool deadMan_ = false;
    };
}
