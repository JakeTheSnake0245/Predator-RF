#pragma once

// Predator RF decoder bridge ingestion.
//
// Receive-only socket reader for external decoder companion processes
// (rtl_433, dump1090, ais-dispatcher, multimon-ng, OP25, DSD-FME, etc.).
// Each ingester thread NEVER transmits — it only opens a listener
// (TCP client or UDP server) and reads newline-delimited records emitted
// by the companion process. Parsed records are pushed to a thread-safe
// queue that the UI thread drains each frame and folds into the
// existing predatorEvents stream.
//
// Architecture: LineIngester is the abstract socket+thread plumbing.
// Per-decoder subclasses (Rtl433Ingester, AdsbIngester, ...) override
// parseLine() to convert one wire-format record into a DecoderIngestEvent
// and call enqueue(). Sockets, reconnect, status, queue, and shutdown
// are shared.
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
#include <cctype>
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
    nlohmann::json raw;      // original record (parsed JSON or echoed CSV fields)
};

// Abstract base: socket/thread plumbing shared by every line-oriented decoder
// bridge. Subclasses implement parseLine(line) and call enqueue() on success.
class LineIngester {
public:
    virtual ~LineIngester() { stop(); }

    LineIngester(const LineIngester&) = delete;
    LineIngester& operator=(const LineIngester&) = delete;

    // (Re)start the worker thread bound to host/port/mode. Mode strings
    // recognised: anything containing "UDP" -> UDP server bind on port,
    // anything containing "Stdin" -> parked (companion process drives stdin
    // of a future helper, not this thread), everything else -> TCP client
    // connecting to host:port and reading newline-delimited records.
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
    int  eventsReceived() const { return eventsReceived_.load(); }

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

protected:
    explicit LineIngester(std::string decoderName)
        : decoderName_(std::move(decoderName)) {}

    // Subclasses parse one wire-format record and (if valid) call enqueue().
    virtual void parseLine(const std::string& line) = 0;

    // Push a parsed event to the UI-side queue. Bounded at 1000 to prevent
    // unbounded growth if the UI thread stalls.
    void enqueue(DecoderIngestEvent&& ev) {
        if (ev.decoder.empty()) ev.decoder = decoderName_;
        std::lock_guard<std::mutex> lk(queueMtx_);
        while (queue_.size() > 1000) queue_.pop();
        queue_.push(std::move(ev));
        eventsReceived_++;
    }

    std::string decoderName_;

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

                std::string udpLineBuf;
                char buf[4096];
                while (!stopFlag_.load()) {
                    int n = (int)::recv(sock, buf, sizeof(buf) - 1, 0);
                    if (n > 0) {
                        // UDP datagrams may be one record or many newline-delimited
                        // records (some bridges, e.g. AIS NMEA, batch them).
                        udpLineBuf.append(buf, n);
                        size_t pos;
                        while ((pos = udpLineBuf.find('\n')) != std::string::npos) {
                            std::string line = udpLineBuf.substr(0, pos);
                            udpLineBuf.erase(0, pos + 1);
                            if (!line.empty() && line.back() == '\r') line.pop_back();
                            if (!line.empty()) parseLine(line);
                        }
                        // Flush any remaining single-record datagram without trailing '\n'
                        if (!udpLineBuf.empty() && udpLineBuf.find('\n') == std::string::npos) {
                            std::string leftover = udpLineBuf;
                            udpLineBuf.clear();
                            if (!leftover.empty() && leftover.back() == '\r') leftover.pop_back();
                            if (!leftover.empty()) parseLine(leftover);
                        }
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
                            if (!line.empty()) parseLine(line);
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

// ---------------------------------------------------------------------------
// rtl_433 ingester. Wire format: one JSON object per line.
// Example: {"time":"...","model":"Acurite-Tower","id":1234,"channel":"A",
//           "freq":433.92,"rssi":-72.5,"temperature_C":21.3, ...}
// ---------------------------------------------------------------------------
class Rtl433Ingester : public LineIngester {
public:
    Rtl433Ingester() : LineIngester("RTL433") {}

protected:
    void parseLine(const std::string& line) override {
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

        enqueue(std::move(ev));
    }
};

// ---------------------------------------------------------------------------
// ADS-B ingester. Wire format: dump1090 / readsb BaseStation port 30003.
// One CSV record per line, 22 comma-separated fields. Field 1 is message
// type (MSG, SEL, ID, AIR, STA, CLK); field 2 is transmission subtype
// (1..8 for MSG); field 5 is the 24-bit ICAO hex (the unique aircraft ID);
// field 11 is callsign; field 12 altitude (ft); field 15/16 lat/lon;
// field 18 squawk. Fields are blank when not present in that frame.
//
// Example MSG records:
//   MSG,1,1,1,A12345,1,2024/04/28,12:34:56.789,2024/04/28,12:34:56.789,UAL123,,,,,,,,,,
//   MSG,3,1,1,A12345,1,...,,38000,,,40.7589,-73.9851,,,,,,
//   MSG,4,1,1,A12345,1,...,,,450,180,,,,,,,
//   MSG,5,1,1,A12345,1,...,,38000,,,,,,,7777,0,0,0,0
//
// ADS-B is always on 1090 MHz. BaseStation does not transmit RSSI; we
// leave strengthDb at 0 unless the feed supplies it (some forks do).
// ---------------------------------------------------------------------------
class AdsbIngester : public LineIngester {
public:
    AdsbIngester() : LineIngester("ADSB") {}

protected:
    void parseLine(const std::string& line) override {
        // Split on ',' preserving empty fields.
        std::vector<std::string> f;
        f.reserve(24);
        std::string cur;
        for (char c : line) {
            if (c == ',') { f.push_back(std::move(cur)); cur.clear(); }
            else if (c == '\r') { /* tolerate stray CR */ }
            else cur.push_back(c);
        }
        f.push_back(std::move(cur));

        if (f.size() < 5) return;
        const std::string& msgType = f[0];
        // Accept the BaseStation message kinds. SEL/ID/AIR/STA/CLK are status
        // events (selection change, identification, new aircraft, status,
        // clock change); MSG is the data envelope. All carry a hex ident.
        if (msgType != "MSG" && msgType != "SEL" && msgType != "ID"
            && msgType != "AIR" && msgType != "STA" && msgType != "CLK") {
            return;
        }

        auto field = [&](size_t i) -> std::string {
            return (i < f.size()) ? f[i] : std::string();
        };
        auto trim = [](std::string s) {
            size_t a = s.find_first_not_of(" \t");
            if (a == std::string::npos) return std::string();
            size_t b = s.find_last_not_of(" \t");
            return s.substr(a, b - a + 1);
        };

        std::string hexIdent  = trim(field(4));   // ICAO 24-bit hex
        std::string callsign  = trim(field(10));
        std::string altitude  = trim(field(11));
        std::string gndSpeed  = trim(field(12));
        std::string track     = trim(field(13));
        std::string latStr    = trim(field(14));
        std::string lonStr    = trim(field(15));
        std::string vRate     = trim(field(16));
        std::string squawk    = trim(field(17));
        std::string txType    = trim(field(1));   // MSG transmission subtype

        if (hexIdent.empty()) return;

        // Normalise hex ident to upper case for stable networkId.
        std::transform(hexIdent.begin(), hexIdent.end(), hexIdent.begin(),
                       [](unsigned char c){ return (char)std::toupper(c); });

        DecoderIngestEvent ev;
        ev.decoder  = "ADSB";
        ev.protocol = "ADS-B";

        ev.networkId = hexIdent;                                  // ICAO hex
        ev.talkgroup = !callsign.empty() ? callsign : hexIdent;   // callsign or hex
        ev.radioId   = !squawk.empty() ? squawk : hexIdent;       // squawk or hex
        ev.label     = !callsign.empty() ? (callsign + " (" + hexIdent + ")")
                                         : hexIdent;
        ev.frequencyHz = 1090e6;
        ev.strengthDb  = 0.0f;

        // Echo parsed fields back into the JSON raw payload so the UI and
        // session export can show altitude/position/speed without re-parsing.
        nlohmann::json raw;
        raw["msg"]      = msgType;
        if (!txType.empty())   raw["txType"]   = txType;
        raw["hex"]      = hexIdent;
        if (!callsign.empty()) raw["callsign"] = callsign;
        if (!altitude.empty()) raw["altitudeFt"] = altitude;
        if (!gndSpeed.empty()) raw["groundSpeedKt"] = gndSpeed;
        if (!track.empty())    raw["trackDeg"] = track;
        if (!latStr.empty() && !lonStr.empty()) {
            try {
                raw["lat"] = std::stod(latStr);
                raw["lon"] = std::stod(lonStr);
            } catch (...) {
                // leave coords out if unparseable
            }
        }
        if (!vRate.empty())    raw["verticalRateFpm"] = vRate;
        if (!squawk.empty())   raw["squawk"] = squawk;
        ev.raw = std::move(raw);

        enqueue(std::move(ev));
    }
};

// ---------------------------------------------------------------------------
// P25 ingester. Wire format: one JSON object per line emitted by a P25
// trunked-voice decoder companion process (typically DSD-FME with a JSON
// event-log feed, or an OP25 metadata bridge). All P25-specific fields
// are accepted under multiple aliases so the user can point this at
// either decoder family without reformatting.
//
// Recognised JSON keys (first-match-wins, case-sensitive — companion
// scripts are expected to emit lowercase keys; uppercase aliases tolerated):
//   wacn / WACN / wacn_id           Wide Area Communication Network id (hex)
//   sysid / system_id / sys / system    System id within the WACN (hex)
//   rfss / rfss_id                  RF Sub-System id
//   site / site_id                  Site id within the RFSS
//   tg / tgid / talkgroup / tg_id   Talkgroup id (decimal)
//   alias / tg_alias / tg_tag       Talkgroup display name
//   src / src_id / radio_id / ruid  Source unit / radio id (decimal)
//   src_alias / src_tag             Source unit display name
//   freq / frequency / freq_hz      Frequency (MHz if "freq"/"frequency"/
//                                              "freq_mhz", Hz if "freq_hz")
//   freq_mhz                        Frequency in MHz (explicit alias)
//   rssi / level / strength_db      Signal level in dBFS or dB
//   snr                             SNR in dB (used as fallback if no rssi)
//   mode / phase / encoding         "P25P1"/"P25 Phase 1"/"P25P2"/"P25 Phase 2"
//   enc / encrypted                 Boolean: encrypted call indicator
//   alg / algorithm                 ALGID (DES-OFB, AES-256, ADP, ...)
//   kid / keyid                     KEYID for encrypted calls
//
// Output mapping into DecoderIngestEvent:
//   decoder    = "P25"
//   protocol   = "P25 Phase 1" | "P25 Phase 2" (default Phase 1 if unknown)
//   networkId  = "WACN" or "WACN/SYSID" if both present (hex, upper case)
//   talkgroup  = TG alias if present, else "TG <tgid>"
//   radioId    = SRC alias if present, else SRC id, else "?"
//   label      = "[P25Px] TG <tg> (alias?) SRC <src>" — short summary
//   raw        = original JSON merged with normalised {wacn, sysid, rfss,
//                site, tg, src, encrypted, alg, kid} so the topology tree
//                can render WACN/RFSS/Site/TG hierarchy without re-parsing.
// ---------------------------------------------------------------------------
class P25Ingester : public LineIngester {
public:
    P25Ingester() : LineIngester("P25") {}

protected:
    void parseLine(const std::string& line) override {
        nlohmann::json j;
        try {
            j = nlohmann::json::parse(line);
        } catch (...) {
            return; // ignore non-JSON / partial lines
        }
        if (!j.is_object()) return;

        // First-match-wins string lookup across alias keys. Coerces numbers
        // to decimal strings and booleans to "true"/"false" so the UI can
        // render any field without per-key type checks.
        auto getStrAlias = [&](std::initializer_list<const char*> keys) -> std::string {
            for (const char* k : keys) {
                if (!j.contains(k)) continue;
                const auto& v = j[k];
                if (v.is_string())          return v.get<std::string>();
                if (v.is_number_integer())  return std::to_string(v.get<long long>());
                if (v.is_number_unsigned()) return std::to_string(v.get<unsigned long long>());
                if (v.is_number_float()) {
                    char buf[32];
                    snprintf(buf, sizeof(buf), "%g", v.get<double>());
                    return std::string(buf);
                }
                if (v.is_boolean())         return v.get<bool>() ? "true" : "false";
            }
            return "";
        };
        auto getNumAlias = [&](std::initializer_list<const char*> keys, double dflt) -> double {
            for (const char* k : keys) {
                if (j.contains(k) && j[k].is_number()) return j[k].get<double>();
            }
            return dflt;
        };
        auto getBoolAlias = [&](std::initializer_list<const char*> keys, bool dflt) -> bool {
            for (const char* k : keys) {
                if (!j.contains(k)) continue;
                const auto& v = j[k];
                if (v.is_boolean())         return v.get<bool>();
                if (v.is_number_integer())  return v.get<long long>() != 0;
                if (v.is_string()) {
                    std::string s = v.get<std::string>();
                    std::transform(s.begin(), s.end(), s.begin(),
                                   [](unsigned char c){ return (char)std::tolower(c); });
                    return (s == "true" || s == "1" || s == "yes" || s == "y");
                }
            }
            return dflt;
        };

        std::string wacn      = getStrAlias({"wacn",  "WACN",       "wacn_id"});
        std::string sysid     = getStrAlias({"sysid", "system_id",  "sys", "system"});
        std::string rfss      = getStrAlias({"rfss",  "rfss_id"});
        std::string site      = getStrAlias({"site",  "site_id"});
        std::string tg        = getStrAlias({"tg",    "tgid",       "talkgroup", "tg_id", "talkgroup_id"});
        std::string tgAlias   = getStrAlias({"alias", "tg_alias",   "tg_tag",    "talkgroup_alias"});
        std::string src       = getStrAlias({"src",   "src_id",     "radio_id",  "ruid", "source", "srcaddr"});
        std::string srcAlias  = getStrAlias({"src_alias", "src_tag", "rid_alias"});
        std::string modeStr   = getStrAlias({"mode",  "phase",      "encoding",  "type"});
        std::string algorithm = getStrAlias({"alg",   "algorithm",  "algid"});
        std::string keyId     = getStrAlias({"kid",   "keyid"});

        bool encrypted = getBoolAlias({"enc", "encrypted", "is_encrypted"}, false);

        // Drop frames that carry no useful identifier at all. Accept anything
        // with a TG/SRC (call event) OR WACN/SYSID/RFSS/Site (control-channel
        // state) — DSD-FME and OP25 both emit site/system status records that
        // are operationally useful even without an active call.
        if (tg.empty() && src.empty() && wacn.empty()
            && sysid.empty() && rfss.empty() && site.empty()) return;

        // Frequency unit handling — different P25 decoders disagree:
        //   * OP25 emits "freq" in Hz (e.g. 851012500)
        //   * DSD-FME emits "freq"/"frequency" in MHz (e.g. 851.0125)
        // Strategy: explicit "freq_hz" / "freq_mhz" win when present.
        // For ambiguous "freq"/"frequency", heuristically treat values
        // >= 1e7 (10 MHz) as Hz (covers entire P25 spectrum 138-960 MHz)
        // and smaller values as MHz. Anything outside the plausible RF
        // range is rejected back to 0 so the map doesn't plot garbage.
        double freqHz = 0.0;
        if (j.contains("freq_hz") && j["freq_hz"].is_number()) {
            freqHz = j["freq_hz"].get<double>();
        } else if (j.contains("freq_mhz") && j["freq_mhz"].is_number()) {
            freqHz = j["freq_mhz"].get<double>() * 1e6;
        } else {
            double f = getNumAlias({"freq", "frequency"}, 0.0);
            if (f > 0.0) {
                freqHz = (f >= 1e7) ? f : (f * 1e6);
            }
        }
        // Sanity: 1 MHz .. 6 GHz keeps us in plausible RF land.
        if (freqHz < 1e6 || freqHz > 6e9) freqHz = 0.0;

        // Phase: P1 default. Match common spellings from DSD-FME / OP25.
        std::string protocol = "P25 Phase 1";
        {
            std::string lower = modeStr;
            std::transform(lower.begin(), lower.end(), lower.begin(),
                           [](unsigned char c){ return (char)std::tolower(c); });
            if (lower.find("p2") != std::string::npos
                || lower.find("phase 2") != std::string::npos
                || lower.find("phase2") != std::string::npos
                || lower.find("tdma") != std::string::npos) {
                protocol = "P25 Phase 2";
            }
        }

        // Normalise WACN to upper-case hex for stable networkId.
        auto upper = [](std::string s) {
            std::transform(s.begin(), s.end(), s.begin(),
                           [](unsigned char c){ return (char)std::toupper(c); });
            return s;
        };
        std::string wacnUp  = upper(wacn);
        std::string sysidUp = upper(sysid);

        DecoderIngestEvent ev;
        ev.decoder  = "P25";
        ev.protocol = protocol;

        if (!wacnUp.empty() && !sysidUp.empty()) ev.networkId = wacnUp + "/" + sysidUp;
        else if (!wacnUp.empty())                ev.networkId = wacnUp;
        else if (!sysidUp.empty())               ev.networkId = sysidUp;
        else                                     ev.networkId = "Unknown";

        if (!tgAlias.empty())   ev.talkgroup = tgAlias;
        else if (!tg.empty())   ev.talkgroup = std::string("TG ") + tg;
        else                    ev.talkgroup = "?";

        if (!srcAlias.empty())  ev.radioId = srcAlias;
        else if (!src.empty())  ev.radioId = src;
        else                    ev.radioId = "?";

        // Compact label: protocol short tag, talkgroup, source.
        // e.g. "[P25P1] TG 1234 (Tac 7) SRC 5001"
        std::string protoShort = (protocol == "P25 Phase 2") ? "P25P2" : "P25P1";
        std::string lbl = std::string("[") + protoShort + "] ";
        if (!tg.empty())        lbl += std::string("TG ") + tg;
        else                    lbl += "TG ?";
        if (!tgAlias.empty())   lbl += std::string(" (") + tgAlias + ")";
        if (!src.empty())       lbl += std::string(" SRC ") + src;
        if (encrypted)          lbl += " [ENC]";
        ev.label = lbl;

        ev.frequencyHz = freqHz;

        double rssi = getNumAlias({"rssi", "level", "strength_db"}, -200.0);
        if (rssi > -200.0) {
            ev.strengthDb = (float)rssi;
        } else {
            double snr = getNumAlias({"snr"}, -200.0);
            if (snr > -200.0) ev.strengthDb = (float)snr;
        }

        // Echo normalised P25 fields back into raw so the WACN/RFSS/Site/TG
        // topology tree and the session export can render them without
        // re-parsing the wire format. Original JSON is preserved as raw.original.
        nlohmann::json raw;
        raw["original"] = j;
        if (!wacnUp.empty())     raw["wacn"]      = wacnUp;
        if (!sysidUp.empty())    raw["sysid"]     = sysidUp;
        if (!rfss.empty())       raw["rfss"]      = rfss;
        if (!site.empty())       raw["site"]      = site;
        if (!tg.empty())         raw["tg"]        = tg;
        if (!tgAlias.empty())    raw["tgAlias"]   = tgAlias;
        if (!src.empty())        raw["src"]       = src;
        if (!srcAlias.empty())   raw["srcAlias"]  = srcAlias;
        raw["encrypted"]                          = encrypted;
        if (!algorithm.empty())  raw["algorithm"] = algorithm;
        if (!keyId.empty())      raw["keyId"]     = keyId;
        raw["phase"]                              = protocol;
        ev.raw = std::move(raw);

        enqueue(std::move(ev));
    }
};

} // namespace predator
