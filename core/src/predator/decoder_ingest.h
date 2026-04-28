#pragma once

// Predator RF decoder bridge ingestion.
//
// Receive-only socket reader for external decoder companion processes
// (rtl_433, dump1090, ais-dispatcher, multimon-ng, OP25, DSD-FME, etc.).
// This thread NEVER transmits — it only opens a listener (TCP client or
// UDP server) and parses inbound newline-delimited JSON records emitted
// by the companion process. Parsed records are pushed to a thread-safe
// queue that the UI thread drains each frame and folds into the
// existing predatorEvents stream.
//
// Safety boundary (see docs/rf_predator_alignment.md):
//   Receive, analyze, log, map, export. No transmit, no jamming,
//   no offensive operations.

#include <atomic>
#include <thread>
#include <mutex>
#include <queue>
#include <string>
#include <vector>
#include <chrono>
#include <cstring>
#include <cerrno>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <algorithm>

#include "../json.hpp"

#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #pragma comment(lib, "Ws2_32.lib")
  using predator_socket_t = SOCKET;
  #define PREDATOR_INVALID_SOCK INVALID_SOCKET
  #define PREDATOR_CLOSESOCK closesocket
  #define PREDATOR_LAST_ERR WSAGetLastError()
  #define PREDATOR_CONNECT_INPROGRESS(e) ((e) == WSAEWOULDBLOCK || (e) == WSAEINPROGRESS)
#else
  #include <sys/socket.h>
  #include <sys/types.h>
  #include <sys/select.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <netdb.h>
  #include <unistd.h>
  #include <fcntl.h>
  using predator_socket_t = int;
  #define PREDATOR_INVALID_SOCK (-1)
  #define PREDATOR_CLOSESOCK ::close
  #define PREDATOR_LAST_ERR errno
  #define PREDATOR_CONNECT_INPROGRESS(e) ((e) == EINPROGRESS || (e) == EWOULDBLOCK)
#endif

namespace predator {

// Normalised event handed to the UI thread.
struct DecoderIngestEvent {
    std::string decoder;     // "RTL433", "ADSB", ... matches the bridge key namespace
    std::string protocol;    // human protocol label
    std::string networkId;   // device/aircraft/ship identifier
    std::string talkgroup;   // sub-channel / type / capcode group
    std::string radioId;     // unit / id / serial
    std::string label;       // short display label
    double frequencyHz = 0.0;
    float strengthDb = 0.0f;
    nlohmann::json raw;      // original JSON record from companion process
};

class Rtl433Ingester {
public:
    Rtl433Ingester() = default;
    ~Rtl433Ingester() { stop(); }

    Rtl433Ingester(const Rtl433Ingester&) = delete;
    Rtl433Ingester& operator=(const Rtl433Ingester&) = delete;

    // (Re)start the worker thread bound to host/port/mode. Mode strings
    // recognised: anything containing "UDP" -> UDP server bind on port,
    // anything containing "Stdin" -> parked (companion process drives stdin
    // of a future helper, not this thread), everything else -> TCP client
    // connecting to host:port and reading newline-delimited JSON.
    void start(const std::string& host, int port, const std::string& mode) {
        stop();
        host_ = host;
        port_ = port;
        mode_ = mode;
        stopFlag_ = false;
        running_ = true;
        eventsReceived_ = 0;
        worker_ = std::thread([this]() { workerLoop(); });
    }

    void stop() {
        if (!running_.load()) {
            if (worker_.joinable()) worker_.join();
            return;
        }
        stopFlag_ = true;
        running_ = false;
        if (worker_.joinable()) worker_.join();
        connected_ = false;
        setStatus("Stopped");
    }

    bool isRunning() const   { return running_.load(); }
    bool isConnected() const { return connected_.load(); }
    int eventsReceived() const { return eventsReceived_.load(); }

    std::string status() const {
        std::lock_guard<std::mutex> lk(statusMtx_);
        return statusMsg_;
    }

    // Drain queued events (FIFO). Called from the UI thread each frame.
    std::vector<DecoderIngestEvent> drain(size_t maxItems = 64) {
        std::vector<DecoderIngestEvent> out;
        std::lock_guard<std::mutex> lk(queueMtx_);
        while (!queue_.empty() && out.size() < maxItems) {
            out.push_back(std::move(queue_.front()));
            queue_.pop();
        }
        return out;
    }

private:
    void setStatus(const std::string& s) {
        std::lock_guard<std::mutex> lk(statusMtx_);
        statusMsg_ = s;
    }

    static void setRecvTimeout(predator_socket_t sock, int ms) {
#ifdef _WIN32
        DWORD tv = (DWORD)ms;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
#else
        timeval tv;
        tv.tv_sec = ms / 1000;
        tv.tv_usec = (ms % 1000) * 1000;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
    }

    // Set socket non-blocking (or back to blocking with nonblock=false).
    static bool setNonBlocking(predator_socket_t sock, bool nonblock) {
#ifdef _WIN32
        u_long m = nonblock ? 1 : 0;
        return ioctlsocket(sock, FIONBIO, &m) == 0;
#else
        int fl = fcntl(sock, F_GETFL, 0);
        if (fl == -1) return false;
        if (nonblock) fl |= O_NONBLOCK; else fl &= ~O_NONBLOCK;
        return fcntl(sock, F_SETFL, fl) == 0;
#endif
    }

    // Connect with periodic stopFlag polling so shutdown doesn't hang
    // for the OS-default ~21s SYN timeout. Returns true if connected.
    bool connectWithStopPolling(predator_socket_t sock, sockaddr* addr, socklen_t addrLen, int totalTimeoutMs) {
        if (!setNonBlocking(sock, true)) return false;

        int rc = ::connect(sock, addr, addrLen);
        if (rc == 0) {
            setNonBlocking(sock, false);
            return true;
        }
        int err = PREDATOR_LAST_ERR;
        if (!PREDATOR_CONNECT_INPROGRESS(err)) {
            return false;
        }

        const int sliceMs = 200;
        int waited = 0;
        while (waited < totalTimeoutMs && !stopFlag_.load()) {
            fd_set wset;
            FD_ZERO(&wset);
            FD_SET(sock, &wset);
            timeval tv;
            tv.tv_sec = 0;
            tv.tv_usec = sliceMs * 1000;
            int sel = ::select((int)sock + 1, nullptr, &wset, nullptr, &tv);
            if (sel > 0 && FD_ISSET(sock, &wset)) {
                int soerr = 0;
                socklen_t soerrLen = sizeof(soerr);
                ::getsockopt(sock, SOL_SOCKET, SO_ERROR, (char*)&soerr, &soerrLen);
                if (soerr == 0) {
                    setNonBlocking(sock, false);
                    return true;
                }
                return false;
            }
            // sel == 0 (timeout) or sel < 0: just loop and re-check stopFlag
            waited += sliceMs;
        }
        return false;
    }

    void sleepBackoff(int& backoffMs, int maxMs) {
        for (int waited = 0; waited < backoffMs && !stopFlag_.load(); waited += 100) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        backoffMs = std::min(backoffMs * 2, maxMs);
    }

    void workerLoop() {
        bool isUdp   = (mode_.find("UDP") != std::string::npos);
        bool isStdin = (mode_.find("Stdin") != std::string::npos);

#ifdef _WIN32
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
#endif

        if (isStdin) {
            setStatus("Stdin mode: waiting for companion process (not yet wired)");
            while (!stopFlag_.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
            }
#ifdef _WIN32
            WSACleanup();
#endif
            return;
        }

        int backoffMs = 500;
        const int backoffMaxMs = 8000;

        while (!stopFlag_.load()) {
            predator_socket_t sock = PREDATOR_INVALID_SOCK;

            if (isUdp) {
                sock = ::socket(AF_INET, SOCK_DGRAM, 0);
                if (sock == PREDATOR_INVALID_SOCK) {
                    setStatus("UDP socket() failed");
                    sleepBackoff(backoffMs, backoffMaxMs);
                    continue;
                }
                int reuse = 1;
                ::setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse));

                sockaddr_in addr{};
                addr.sin_family = AF_INET;
                addr.sin_addr.s_addr = htonl(INADDR_ANY);
                addr.sin_port = htons((uint16_t)port_);
                if (::bind(sock, (sockaddr*)&addr, sizeof(addr)) != 0) {
                    setStatus(std::string("UDP bind failed on :") + std::to_string(port_));
                    PREDATOR_CLOSESOCK(sock);
                    sleepBackoff(backoffMs, backoffMaxMs);
                    continue;
                }

                connected_ = true;
                setStatus(std::string("Listening UDP :") + std::to_string(port_));
                backoffMs = 500;
                setRecvTimeout(sock, 500);

                char buf[4096];
                while (!stopFlag_.load()) {
                    int n = (int)::recv(sock, buf, sizeof(buf) - 1, 0);
                    if (n > 0) {
                        buf[n] = '\0';
                        parseAndQueue(std::string(buf, n));
                    }
                    // on timeout / error just loop and re-check stopFlag
                }
                PREDATOR_CLOSESOCK(sock);
                connected_ = false;
            } else {
                // TCP client
                addrinfo hints{};
                hints.ai_family = AF_INET;
                hints.ai_socktype = SOCK_STREAM;
                addrinfo* res = nullptr;
                std::string portStr = std::to_string(port_);
                int gai = ::getaddrinfo(host_.c_str(), portStr.c_str(), &hints, &res);
                if (gai != 0 || res == nullptr) {
                    setStatus(std::string("DNS resolve failed: ") + host_);
                    if (res) ::freeaddrinfo(res);
                    sleepBackoff(backoffMs, backoffMaxMs);
                    continue;
                }

                sock = ::socket(res->ai_family, res->ai_socktype, res->ai_protocol);
                if (sock == PREDATOR_INVALID_SOCK) {
                    setStatus("TCP socket() failed");
                    ::freeaddrinfo(res);
                    sleepBackoff(backoffMs, backoffMaxMs);
                    continue;
                }

                setStatus(std::string("Connecting ") + host_ + ":" + portStr);
                bool ok = connectWithStopPolling(sock, res->ai_addr, (socklen_t)res->ai_addrlen, 5000);
                ::freeaddrinfo(res);
                if (!ok) {
                    if (stopFlag_.load()) {
                        PREDATOR_CLOSESOCK(sock);
                        break; // outer reconnect loop will exit on stopFlag
                    }
                    setStatus(std::string("Connect failed: ") + host_ + ":" + portStr);
                    PREDATOR_CLOSESOCK(sock);
                    sleepBackoff(backoffMs, backoffMaxMs);
                    continue;
                }

                connected_ = true;
                setStatus(std::string("Connected ") + host_ + ":" + portStr);
                backoffMs = 500;
                setRecvTimeout(sock, 500);

                std::string lineBuf;
                char buf[4096];
                while (!stopFlag_.load()) {
                    int n = (int)::recv(sock, buf, sizeof(buf) - 1, 0);
                    if (n > 0) {
                        lineBuf.append(buf, n);
                        size_t pos;
                        while ((pos = lineBuf.find('\n')) != std::string::npos) {
                            std::string line = lineBuf.substr(0, pos);
                            lineBuf.erase(0, pos + 1);
                            if (!line.empty() && line.back() == '\r') line.pop_back();
                            if (!line.empty()) parseAndQueue(line);
                        }
                        // guard against runaway buffer if peer never sends '\n'
                        if (lineBuf.size() > (1 << 20)) lineBuf.clear();
                    } else if (n == 0) {
                        setStatus("Disconnected");
                        break;
                    } else {
#ifdef _WIN32
                        int err = WSAGetLastError();
                        if (err != WSAETIMEDOUT && err != WSAEINTR) {
                            setStatus("Socket error");
                            break;
                        }
#else
                        if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                            setStatus("Socket error");
                            break;
                        }
#endif
                    }
                }
                PREDATOR_CLOSESOCK(sock);
                connected_ = false;
            }

            sleepBackoff(backoffMs, backoffMaxMs);
        }

#ifdef _WIN32
        WSACleanup();
#endif
    }

    void parseAndQueue(const std::string& line) {
        nlohmann::json j;
        try {
            j = nlohmann::json::parse(line);
        } catch (...) {
            return; // ignore non-JSON / partial lines
        }
        if (!j.is_object()) return;

        auto getStr = [&](const char* k) -> std::string {
            if (!j.contains(k)) return "";
            const auto& v = j[k];
            if (v.is_string()) return v.get<std::string>();
            if (v.is_number_integer()) return std::to_string(v.get<long long>());
            if (v.is_number_unsigned()) return std::to_string(v.get<unsigned long long>());
            if (v.is_number_float()) {
                char buf[32];
                snprintf(buf, sizeof(buf), "%g", v.get<double>());
                return std::string(buf);
            }
            if (v.is_boolean()) return v.get<bool>() ? "true" : "false";
            return "";
        };
        auto getNum = [&](const char* k, double dflt) -> double {
            if (j.contains(k) && j[k].is_number()) return j[k].get<double>();
            return dflt;
        };

        DecoderIngestEvent ev;
        ev.decoder  = "RTL433";
        ev.protocol = "RTL433";
        ev.raw      = j;

        std::string model   = getStr("model");
        std::string id      = getStr("id");
        std::string channel = getStr("channel");
        std::string subtype = getStr("subtype");

        ev.networkId = model.empty() ? std::string("Unknown") : model;
        if (!subtype.empty())      ev.talkgroup = subtype;
        else if (!channel.empty()) ev.talkgroup = std::string("ch") + channel;
        else if (!id.empty())      ev.talkgroup = std::string("dev") + id;
        else                       ev.talkgroup = "default";
        ev.radioId = !id.empty() ? id : (channel.empty() ? std::string("?") : channel);
        ev.label   = ev.networkId + (id.empty() ? std::string("") : (std::string(" #") + id));

        // rtl_433 emits "freq" in MHz
        double freqMHz = getNum("freq", 0.0);
        ev.frequencyHz = (freqMHz > 0.0) ? (freqMHz * 1e6) : 0.0;

        // rtl_433 emits "rssi" / "snr" in dB
        double rssi = getNum("rssi", -200.0);
        if (rssi > -200.0) ev.strengthDb = (float)rssi;
        else {
            double snr = getNum("snr", -200.0);
            if (snr > -200.0) ev.strengthDb = (float)snr;
        }

        {
            std::lock_guard<std::mutex> lk(queueMtx_);
            // bound queue at 1000 to prevent unbounded growth
            while (queue_.size() > 1000) queue_.pop();
            queue_.push(std::move(ev));
        }
        eventsReceived_++;
    }

    std::atomic<bool> running_{false};
    std::atomic<bool> connected_{false};
    std::atomic<bool> stopFlag_{false};
    std::atomic<int>  eventsReceived_{0};

    std::string host_;
    int port_ = 0;
    std::string mode_;

    std::thread worker_;
    std::mutex queueMtx_;
    std::queue<DecoderIngestEvent> queue_;

    mutable std::mutex statusMtx_;
    std::string statusMsg_ = "Idle";
};

} // namespace predator
