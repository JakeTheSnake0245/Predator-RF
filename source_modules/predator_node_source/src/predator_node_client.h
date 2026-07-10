#pragma once
/*
 * PredatorNodeClient — HTTP/WebSocket client for the predator-rfd daemon.
 *
 * Responsibilities:
 *  - Auto-detect the node address (phone hotspot gateway, RPi AP default,
 *    or a user-configured static IP) by probing /v1/identify in parallel.
 *  - Stream spectrum bins from /ws (WebSocket) for the waterfall.
 *  - Send typed commands (tune, scan, mission, source, tx.*) via POST /v1/command.
 *  - Expose the node's event stream via GET /v1/events?since=N (SSE polling).
 *
 * TX commands are forwarded without any class-level restriction — this is the
 * full-capability remote-cockpit path.  The operator is responsible for
 * complying with applicable radio regulations.
 *
 * All network I/O runs on a dedicated background thread.  Public methods are
 * thread-safe.
 */

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#else
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

namespace predator_node {

struct NodeInfo {
    std::string device;
    std::string version;
    std::string role;
    std::string hw_profile;
    int         web_port = 5555;
};

struct SpectrumFrame {
    uint64_t          serial     = 0;
    double            center_hz  = 0.0;
    double            bw_hz      = 0.0;
    float             fft_min    = -120.0f;
    float             fft_max    = 0.0f;
    std::vector<float> bins;
};

enum class ConnState { DISCONNECTED, PROBING, CONNECTED, ERROR };

class PredatorNodeClient {
public:
    PredatorNodeClient() = default;
    ~PredatorNodeClient() { stop(); }

    void setHosts(const std::vector<std::string>& hosts, int port = 5555) {
        std::lock_guard<std::mutex> lk(_cfg_mtx);
        _probe_hosts = hosts;
        _port        = port;
    }

    void setApiKey(const std::string& key) {
        std::lock_guard<std::mutex> lk(_cfg_mtx);
        _api_key = key;
    }

    void setOnSpectrum(std::function<void(const SpectrumFrame&)> cb) {
        std::lock_guard<std::mutex> lk(_cb_mtx);
        _on_spectrum = std::move(cb);
    }

    void setOnEvent(std::function<void(const std::string& json)> cb) {
        std::lock_guard<std::mutex> lk(_cb_mtx);
        _on_event = std::move(cb);
    }

    void setOnStateChange(std::function<void(ConnState)> cb) {
        std::lock_guard<std::mutex> lk(_cb_mtx);
        _on_state = std::move(cb);
    }

    bool start();
    void stop();

    ConnState state() const { return _state.load(); }

    NodeInfo nodeInfo() const {
        std::lock_guard<std::mutex> lk(_info_mtx);
        return _info;
    }

    bool sendCommand(const std::string& cls, const std::string& action,
                     const std::string& args_json = "{}");

    bool tune(double freq_hz) {
        char buf[128];
        snprintf(buf, sizeof(buf), "{\"freq\":%f}", freq_hz);
        return sendCommand("tune", "set", buf);
    }

    bool startSdr() { return sendCommand("source", "start"); }
    bool stopSdr()  { return sendCommand("source", "stop"); }
    bool startScan(){ return sendCommand("scan",   "start"); }
    bool stopScan() { return sendCommand("scan",   "stop"); }

    bool setMissionMode(const std::string& mode) {
        // Escape the mode so a value containing a quote/backslash cannot break
        // out of the JSON string (no fixed-size buffer → no silent truncation).
        return sendCommand("mission", "set-mode",
                           "{\"mode\":\"" + _jsonEscape(mode) + "\"}");
    }

    std::string activeHost() const {
        std::lock_guard<std::mutex> lk(_cfg_mtx);
        return _active_host;
    }

private:
    void _workerLoop();
    bool _probe();
    bool _getIdentify(const std::string& host, int port, NodeInfo& out);
    bool _pollSpectrum();
    bool _pollEvents();

    std::string _httpGet(const std::string& path);
    std::string _httpPost(const std::string& path, const std::string& body);

    std::string _buildBaseUrl() const {
        return "http://" + _active_host + ":" + std::to_string(_port);
    }

    static std::string _jsonEscape(const std::string& s) {
        std::string out;
        out.reserve(s.size() + 8);
        for (char c : s) {
            switch (c) {
                case '"':  out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n";  break;
                case '\r': out += "\\r";  break;
                case '\t': out += "\\t";  break;
                default:   out.push_back(c);
            }
        }
        return out;
    }

    mutable std::mutex _cfg_mtx;
    std::vector<std::string> _probe_hosts = {
        "192.168.43.1",
        "192.168.4.1",
        "192.168.1.1"
    };
    int         _port       = 5555;
    std::string _api_key;
    std::string _active_host;

    mutable std::mutex _info_mtx;
    NodeInfo _info;

    mutable std::mutex _cb_mtx;
    std::function<void(const SpectrumFrame&)>  _on_spectrum;
    std::function<void(const std::string&)>    _on_event;
    std::function<void(ConnState)>             _on_state;

    std::atomic<ConnState> _state{ConnState::DISCONNECTED};
    std::atomic<bool>      _running{false};
    std::thread            _worker;
    uint64_t               _last_event_id = 0;
    uint64_t               _last_spec_serial = 0;
};

}
