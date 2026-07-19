#pragma once

// KrakenSDR remote-control client (krakensdr_doa settings API).
//
// The stock krakensdr_doa firmware exposes a headless remote-control HTTP
// API on the KrakenSDR RPi (default port 8042):
//
//   GET  /settings   → full settings JSON (includes "center_freq" in Hz)
//   POST /settings   → accept a full settings JSON blob back; setting
//                      "ext_upd_flag": true triggers a retune + automatic
//                      calibration cycle at the new "center_freq".
//
// Contract (must be honoured — the DOA engine rejects partial blobs):
//   1. Always GET the full settings first.
//   2. Patch ONLY the needed keys (center_freq, ext_upd_flag) in the blob.
//   3. POST the complete modified blob back.
//   4. Retune takes several seconds (calibration). Poll GET /settings and
//      only report success when the read-back center_freq matches the
//      request. Never assume instant success.
//
// A legacy read-only settings endpoint also exists on port 8081 — not used
// here (we need write access).
//
// Threading model: identical to the ingesters in decoder_ingest.h — one
// background worker thread owns all sockets; the UI thread only calls the
// atomic getters and enqueues tune requests. Nothing here ever blocks the
// UI thread.
//
// RX-only posture note: this client changes the *receive* centre frequency
// of a passive DF array. No transmit capability is exposed or implied.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

#include "../json.hpp"
#include "decoder_ingest.h"   // predator_socket_t + platform socket macros

namespace predator {

// ── Tune lifecycle state (UI feedback) ───────────────────────────────────────
enum class KrakenTuneState : int {
    IDLE        = 0,   // no tune in progress
    SENDING     = 1,   // GET+patch+POST in flight
    CALIBRATING = 2,   // POST accepted; polling readback until freq matches
    CONFIRMED   = 3,   // readback matches requested frequency
    FAILED      = 4,   // any step failed (see statusString())
};

// ── Pure settings-patch logic (unit-testable, no sockets) ────────────────────
//
// Given the full settings blob from GET /settings and the requested centre
// frequency in Hz, return the blob to POST back: identical except
// center_freq is replaced and ext_upd_flag is set true. Throws nothing;
// returns a discarded (null) json if the input is not an object.
inline nlohmann::json krakenPatchSettings(const nlohmann::json& settings,
                                          double centerFreqHz) {
    if (!settings.is_object()) return nlohmann::json();
    nlohmann::json out = settings;
    out["center_freq"]  = centerFreqHz;
    out["ext_upd_flag"] = true;
    return out;
}

// Extract the current centre frequency (Hz) from a settings blob.
// Returns 0.0 when absent / non-numeric.
inline double krakenSettingsCenterFreq(const nlohmann::json& settings) {
    if (!settings.is_object()) return 0.0;
    auto it = settings.find("center_freq");
    if (it == settings.end() || !it->is_number()) return 0.0;
    return it->get<double>();
}

// Readback match test: krakensdr_doa stores center_freq as a float; allow
// 1 Hz slack for representation error.
inline bool krakenFreqMatches(double readbackHz, double requestedHz) {
    return std::abs(readbackHz - requestedHz) <= 1.0;
}

// Minimal HTTP/1.1 response splitter. Returns status code (0 on parse
// failure) and fills body with everything after the header terminator.
// Handles Content-Length framing implicitly because the caller reads the
// socket until close (Connection: close is requested).
inline int krakenParseHttpResponse(const std::string& raw, std::string& body) {
    body.clear();
    // Status line: "HTTP/1.1 200 OK\r\n"
    if (raw.compare(0, 5, "HTTP/") != 0) return 0;
    size_t sp = raw.find(' ');
    if (sp == std::string::npos || sp + 4 > raw.size()) return 0;
    int status = 0;
    for (int i = 0; i < 3; ++i) {
        char c = raw[sp + 1 + i];
        if (c < '0' || c > '9') return 0;
        status = status * 10 + (c - '0');
    }
    size_t hdrEnd = raw.find("\r\n\r\n");
    if (hdrEnd != std::string::npos) {
        body = raw.substr(hdrEnd + 4);
    }
    return status;
}

// ── Client ───────────────────────────────────────────────────────────────────
class KrakenCtlClient {
public:
    KrakenCtlClient() = default;
    ~KrakenCtlClient() { stop(); }

    KrakenCtlClient(const KrakenCtlClient&) = delete;
    KrakenCtlClient& operator=(const KrakenCtlClient&) = delete;

    // (Re)start the background worker polling host:port.
    void start(const std::string& host, int port) {
        stop();
        host_ = host;
        port_ = port;
        stopFlag_ = false;
        running_  = true;
        tuneState_ = (int)KrakenTuneState::IDLE;
        worker_ = std::thread([this]() { workerLoop(); });
    }

    void stop() {
        stopFlag_ = true;
        running_  = false;
        if (worker_.joinable()) worker_.join();
        reachable_ = false;
        setStatus("Stopped");
    }

    bool isRunning()   const { return running_.load(); }
    // True when the last GET /settings round-trip succeeded.
    bool isReachable() const { return reachable_.load(); }

    // Last centre frequency read back from the Kraken (Hz). 0 = unknown.
    double currentFreqHz() const {
        return currentFreqMilliHz_.load() / 1000.0;
    }

    KrakenTuneState tuneState() const {
        return (KrakenTuneState)tuneState_.load();
    }

    std::string statusString() const {
        std::lock_guard<std::mutex> lk(statusMtx_);
        return statusMsg_;
    }

    // Request a retune. Returns false if a tune is already in flight or the
    // client is not running. The worker picks the request up within one
    // poll slice (≤200 ms).
    bool requestTune(double freqHz) {
        if (!running_.load()) return false;
        int st = tuneState_.load();
        if (st == (int)KrakenTuneState::SENDING ||
            st == (int)KrakenTuneState::CALIBRATING) return false;
        if (freqHz <= 0.0) return false;
        pendingFreqMilliHz_ = (int64_t)(freqHz * 1000.0);
        tunePending_ = true;
        return true;
    }

private:
    // ── Worker ───────────────────────────────────────────────────────────
    void workerLoop() {
        int pollMs = 0;   // fire the first status GET immediately
        while (!stopFlag_.load()) {
            if (tunePending_.exchange(false)) {
                doTune(pendingFreqMilliHz_.load() / 1000.0);
                pollMs = 3000;
                continue;
            }
            if (pollMs <= 0) {
                refreshStatus();
                pollMs = 3000;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            pollMs -= 200;
        }
    }

    // Periodic reachability + current-frequency refresh.
    void refreshStatus() {
        nlohmann::json settings;
        if (!getSettings(settings)) {
            reachable_ = false;
            setStatus("Kraken unreachable (" + host_ + ":" + std::to_string(port_) + ")");
            return;
        }
        reachable_ = true;
        double f = krakenSettingsCenterFreq(settings);
        if (f > 0.0) currentFreqMilliHz_ = (int64_t)(f * 1000.0);
        if (tuneState_.load() == (int)KrakenTuneState::IDLE) {
            setStatus("Control link OK");
        }
    }

    // Full GET → patch → POST → poll-readback tune cycle.
    void doTune(double freqHz) {
        tuneState_ = (int)KrakenTuneState::SENDING;
        setStatus("Fetching Kraken settings…");

        nlohmann::json settings;
        if (!getSettings(settings)) {
            reachable_ = false;
            fail("Kraken unreachable — settings GET failed");
            return;
        }
        reachable_ = true;

        nlohmann::json patched = krakenPatchSettings(settings, freqHz);
        if (patched.is_null()) {
            fail("Settings blob was not a JSON object");
            return;
        }

        setStatus("Sending retune request…");
        if (!postSettings(patched)) {
            fail("Settings POST failed");
            return;
        }

        // Calibration cycle: poll readback until match or timeout.
        tuneState_ = (int)KrakenTuneState::CALIBRATING;
        setStatus("Retuning + calibrating…");
        const int timeoutMs = 30000;
        int waited = 0;
        while (waited < timeoutMs && !stopFlag_.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1000));
            waited += 1000;
            nlohmann::json rb;
            if (!getSettings(rb)) continue;   // transient — keep polling
            double f = krakenSettingsCenterFreq(rb);
            if (f > 0.0) currentFreqMilliHz_ = (int64_t)(f * 1000.0);
            if (krakenFreqMatches(f, freqHz)) {
                tuneState_ = (int)KrakenTuneState::CONFIRMED;
                char buf[64];
                snprintf(buf, sizeof(buf), "Confirmed at %.4f MHz", f / 1e6);
                setStatus(buf);
                return;
            }
        }
        if (!stopFlag_.load()) {
            fail("Timeout — readback never matched requested frequency");
        }
    }

    void fail(const std::string& why) {
        tuneState_ = (int)KrakenTuneState::FAILED;
        setStatus(why);
    }

    // ── HTTP plumbing ────────────────────────────────────────────────────

    bool getSettings(nlohmann::json& out) {
        std::string req =
            "GET /settings HTTP/1.1\r\n"
            "Host: " + host_ + ":" + std::to_string(port_) + "\r\n"
            "Accept: application/json\r\n"
            "Connection: close\r\n"
            "\r\n";
        std::string body;
        int status = httpRoundTrip(req, body);
        if (status != 200) return false;
        try {
            out = nlohmann::json::parse(body);
        } catch (...) {
            return false;
        }
        return out.is_object();
    }

    bool postSettings(const nlohmann::json& blob) {
        std::string payload = blob.dump();
        std::string req =
            "POST /settings HTTP/1.1\r\n"
            "Host: " + host_ + ":" + std::to_string(port_) + "\r\n"
            "Content-Type: application/json\r\n"
            "Content-Length: " + std::to_string(payload.size()) + "\r\n"
            "Connection: close\r\n"
            "\r\n" + payload;
        std::string body;
        int status = httpRoundTrip(req, body);
        return status >= 200 && status < 300;
    }

    // One request/response over a fresh TCP connection (Connection: close
    // framing — read until peer closes). Returns HTTP status, 0 on error.
    int httpRoundTrip(const std::string& request, std::string& body) {
#ifdef _WIN32
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
        struct addrinfo hints{}, *res = nullptr;
        hints.ai_family   = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        std::string portStr = std::to_string(port_);
        if (::getaddrinfo(host_.c_str(), portStr.c_str(), &hints, &res) != 0 || !res)
            return 0;

        predator_socket_t sock = ::socket(res->ai_family, res->ai_socktype, res->ai_protocol);
        if (sock == PREDATOR_INVALID_SOCK) { ::freeaddrinfo(res); return 0; }

        // Non-blocking connect with stopFlag polling (5 s budget).
        bool ok = false;
#ifndef _WIN32
        int fl = fcntl(sock, F_GETFL, 0);
        fcntl(sock, F_SETFL, fl | O_NONBLOCK);
#else
        u_long m = 1; ioctlsocket(sock, FIONBIO, &m);
#endif
        int rc = ::connect(sock, res->ai_addr, (socklen_t)res->ai_addrlen);
        if (rc == 0) {
            ok = true;
        } else if (PREDATOR_CONNECT_INPROGRESS(PREDATOR_LAST_ERR)) {
            for (int waited = 0; waited < 5000 && !stopFlag_.load(); waited += 200) {
                fd_set wset; FD_ZERO(&wset); FD_SET(sock, &wset);
                timeval tv{0, 200000};
                if (::select((int)sock + 1, nullptr, &wset, nullptr, &tv) > 0) {
                    int soerr = 0; socklen_t soerrLen = sizeof(soerr);
                    ::getsockopt(sock, SOL_SOCKET, SO_ERROR, (char*)&soerr, &soerrLen);
                    ok = (soerr == 0);
                    break;
                }
            }
        }
#ifndef _WIN32
        fcntl(sock, F_SETFL, fl & ~O_NONBLOCK);
#else
        m = 0; ioctlsocket(sock, FIONBIO, &m);
#endif
        ::freeaddrinfo(res);
        if (!ok) { PREDATOR_CLOSESOCK(sock); return 0; }

        // 5 s recv timeout so a wedged server can't hang the worker.
#ifndef _WIN32
        timeval tv{5, 0};
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#else
        DWORD tvMs = 5000;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tvMs, sizeof(tvMs));
#endif

        if (::send(sock, request.c_str(), (int)request.size(), 0) != (int)request.size()) {
            PREDATOR_CLOSESOCK(sock);
            return 0;
        }

        std::string raw;
        char buf[4096];
        while (raw.size() < (4u << 20)) {   // 4 MB cap
            int n = (int)::recv(sock, buf, sizeof(buf), 0);
            if (n <= 0) break;              // close / timeout / error → done
            raw.append(buf, n);
        }
        PREDATOR_CLOSESOCK(sock);

        if (raw.empty()) return 0;
        return krakenParseHttpResponse(raw, body);
    }

    void setStatus(const std::string& s) {
        std::lock_guard<std::mutex> lk(statusMtx_);
        statusMsg_ = s;
    }

    // ── State ────────────────────────────────────────────────────────────
    std::string host_ = "127.0.0.1";
    int         port_ = 8042;

    std::atomic<bool> stopFlag_{true};
    std::atomic<bool> running_{false};
    std::atomic<bool> reachable_{false};
    std::atomic<bool> tunePending_{false};
    // Frequencies stored as integer millihertz so they fit lock-free atomics.
    std::atomic<int64_t> currentFreqMilliHz_{0};
    std::atomic<int64_t> pendingFreqMilliHz_{0};
    std::atomic<int>     tuneState_{(int)KrakenTuneState::IDLE};

    std::thread worker_;
    mutable std::mutex statusMtx_;
    std::string statusMsg_ = "Idle";
};

} // namespace predator
