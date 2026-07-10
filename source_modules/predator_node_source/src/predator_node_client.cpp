#include "predator_node_client.h"

#include <chrono>
#include <cstring>
#include <sstream>
#include <stdexcept>

#ifndef _WIN32
#include <fcntl.h>
#include <errno.h>
#include <sys/select.h>
#endif

namespace predator_node {

// Cap on how many bytes we will read from a single HTTP response. Protects the
// worker thread against a buggy/malicious peer streaming an unbounded body.
static constexpr size_t kMaxResponseBytes = 4 * 1024 * 1024;  // 4 MiB

// connect() with a bounded timeout. SO_RCVTIMEO/SO_SNDTIMEO do NOT bound
// connect() on a blocking socket, so we switch the socket to non-blocking for
// the connect, wait on select() with an explicit deadline, then restore
// blocking mode. Returns true on a completed connection within timeout_ms.
static bool connect_with_timeout(int sock, const struct sockaddr* addr,
                                 socklen_t addrlen, int timeout_ms) {
#ifndef _WIN32
    int flags = fcntl(sock, F_GETFL, 0);
    if (flags < 0) return false;
    if (fcntl(sock, F_SETFL, flags | O_NONBLOCK) < 0) return false;

    int rc = ::connect(sock, addr, addrlen);
    if (rc == 0) {
        fcntl(sock, F_SETFL, flags);  // connected immediately
        return true;
    }
    if (errno != EINPROGRESS) {
        fcntl(sock, F_SETFL, flags);
        return false;
    }

    fd_set wset;
    FD_ZERO(&wset);
    FD_SET(sock, &wset);
    struct timeval tv;
    tv.tv_sec  = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;

    rc = ::select(sock + 1, nullptr, &wset, nullptr, &tv);
    if (rc <= 0) {                      // 0 = timeout, <0 = error
        fcntl(sock, F_SETFL, flags);
        return false;
    }

    int soerr = 0;
    socklen_t len = sizeof(soerr);
    if (getsockopt(sock, SOL_SOCKET, SO_ERROR, &soerr, &len) < 0 || soerr != 0) {
        fcntl(sock, F_SETFL, flags);
        return false;
    }

    fcntl(sock, F_SETFL, flags);        // restore blocking mode
    return true;
#else
    return ::connect(sock, addr, addrlen) == 0;
#endif
}

// send() the whole buffer, looping over short writes. Returns false on error.
static bool send_all(int sock, const std::string& data) {
    size_t sent = 0;
    while (sent < data.size()) {
        int n = (int)::send(sock, data.data() + sent, data.size() - sent, 0);
        if (n <= 0) return false;
        sent += (size_t)n;
    }
    return true;
}

// recv() until EOF or the response cap is hit.
static std::string recv_bounded(int sock) {
    std::string resp;
    char buf[4096];
    int n;
    while ((n = (int)::recv(sock, buf, sizeof(buf), 0)) > 0) {
        resp.append(buf, n);
        if (resp.size() > kMaxResponseBytes) break;
    }
    return resp;
}

static std::string simple_json_str(const std::string& json, const std::string& key) {
    // Match the quoted key, then tolerate whitespace before the opening quote
    // of the value. NOTE: these are deliberately lightweight, shape-specific
    // extractors for the known predator-rfd response schema — not a general
    // JSON parser. They handle whitespace and backslash escapes so a value
    // like "a\"b" or a reformatted response does not truncate/misparse.
    std::string needle = "\"" + key + "\":";
    auto pos = json.find(needle);
    if (pos == std::string::npos) return "";
    pos += needle.size();
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t' ||
                                 json[pos] == '\n' || json[pos] == '\r')) ++pos;
    if (pos >= json.size() || json[pos] != '"') return "";
    ++pos;  // skip opening quote
    std::string out;
    while (pos < json.size()) {
        char c = json[pos];
        if (c == '\\' && pos + 1 < json.size()) {   // escaped char — take next verbatim
            out.push_back(json[pos + 1]);
            pos += 2;
            continue;
        }
        if (c == '"') break;                          // closing quote
        out.push_back(c);
        ++pos;
    }
    return out;
}

static int simple_json_int(const std::string& json, const std::string& key, int def = 0) {
    std::string needle = "\"" + key + "\":";
    auto pos = json.find(needle);
    if (pos == std::string::npos) return def;
    pos += needle.size();
    try { return std::stoi(json.substr(pos)); } catch (...) { return def; }
}

static std::string tcp_get(const std::string& host, int port,
                            const std::string& path,
                            const std::string& api_key,
                            int timeout_ms = 3000)
{
    struct addrinfo hints{}, *res = nullptr;
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    std::string portStr = std::to_string(port);
    if (getaddrinfo(host.c_str(), portStr.c_str(), &hints, &res) != 0 || !res)
        return "";

    int sock = (int)::socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (sock < 0) { freeaddrinfo(res); return ""; }

#ifndef _WIN32
    struct timeval tv;
    tv.tv_sec  = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif

    if (!connect_with_timeout(sock, res->ai_addr, res->ai_addrlen, timeout_ms)) {
        freeaddrinfo(res); ::close(sock); return "";
    }
    freeaddrinfo(res);

    std::string req = "GET " + path + " HTTP/1.0\r\n"
                    + "Host: " + host + "\r\n"
                    + "X-Kujhad-Key: " + api_key + "\r\n"
                    + "Connection: close\r\n\r\n";
    if (!send_all(sock, req)) { ::close(sock); return ""; }

    std::string resp = recv_bounded(sock);
    ::close(sock);

    auto pos = resp.find("\r\n\r\n");
    return (pos != std::string::npos) ? resp.substr(pos + 4) : "";
}

static std::string tcp_post(const std::string& host, int port,
                             const std::string& path,
                             const std::string& body,
                             const std::string& api_key,
                             int timeout_ms = 3000)
{
    struct addrinfo hints{}, *res = nullptr;
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    std::string portStr = std::to_string(port);
    if (getaddrinfo(host.c_str(), portStr.c_str(), &hints, &res) != 0 || !res)
        return "";

    int sock = (int)::socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (sock < 0) { freeaddrinfo(res); return ""; }

#ifndef _WIN32
    struct timeval tv;
    tv.tv_sec  = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif

    if (!connect_with_timeout(sock, res->ai_addr, res->ai_addrlen, timeout_ms)) {
        freeaddrinfo(res); ::close(sock); return "";
    }
    freeaddrinfo(res);

    std::string req = "POST " + path + " HTTP/1.0\r\n"
                    + "Host: " + host + "\r\n"
                    + "X-Kujhad-Key: " + api_key + "\r\n"
                    + "Content-Type: application/json\r\n"
                    + "Content-Length: " + std::to_string(body.size()) + "\r\n"
                    + "Connection: close\r\n\r\n"
                    + body;
    if (!send_all(sock, req)) { ::close(sock); return ""; }

    std::string resp = recv_bounded(sock);
    ::close(sock);

    auto pos = resp.find("\r\n\r\n");
    return (pos != std::string::npos) ? resp.substr(pos + 4) : "";
}

bool PredatorNodeClient::start() {
    if (_running.exchange(true)) return true;
    _worker = std::thread([this]{ _workerLoop(); });
    return true;
}

void PredatorNodeClient::stop() {
    _running = false;
    if (_worker.joinable()) _worker.join();
    _state = ConnState::DISCONNECTED;
}

bool PredatorNodeClient::_getIdentify(const std::string& host, int port, NodeInfo& out) {
    std::string key;
    { std::lock_guard<std::mutex> lk(_cfg_mtx); key = _api_key; }
    std::string body = tcp_get(host, port, "/v1/identify", key, 2000);
    if (body.empty()) return false;
    out.device     = simple_json_str(body, "device");
    out.version    = simple_json_str(body, "version");
    out.role       = simple_json_str(body, "role");
    out.hw_profile = simple_json_str(body, "hwProfile");
    out.web_port   = simple_json_int(body, "webPort", port);
    return !out.device.empty();
}

bool PredatorNodeClient::_probe() {
    std::vector<std::string> hosts;
    int port;
    {
        std::lock_guard<std::mutex> lk(_cfg_mtx);
        hosts = _probe_hosts;
        port  = _port;
    }

    _state = ConnState::PROBING;
    {
        std::lock_guard<std::mutex> lk(_cb_mtx);
        if (_on_state) _on_state(ConnState::PROBING);
    }

    for (const auto& h : hosts) {
        NodeInfo info;
        if (_getIdentify(h, port, info)) {
            {
                std::lock_guard<std::mutex> lk(_cfg_mtx);
                _active_host = h;
            }
            {
                std::lock_guard<std::mutex> lk(_info_mtx);
                _info = info;
            }
            _state = ConnState::CONNECTED;
            {
                std::lock_guard<std::mutex> lk(_cb_mtx);
                if (_on_state) _on_state(ConnState::CONNECTED);
            }
            return true;
        }
    }

    _state = ConnState::ERROR;
    {
        std::lock_guard<std::mutex> lk(_cb_mtx);
        if (_on_state) _on_state(ConnState::ERROR);
    }
    return false;
}

bool PredatorNodeClient::_pollSpectrum() {
    std::string host, key;
    int port;
    {
        std::lock_guard<std::mutex> lk(_cfg_mtx);
        host = _active_host; port = _port; key = _api_key;
    }
    if (host.empty()) return false;

    std::string body = tcp_get(host, port, "/api/spectrum?bins=1024", key, 1000);
    if (body.empty()) return false;

    // A numeric token is at most a couple dozen chars; cap the substring so we
    // don't copy the entire (kilobyte-scale) body once per field.
    constexpr size_t kNumLen = 32;

    SpectrumFrame frame;
    auto sn_pos = body.find("\"serial\":");
    if (sn_pos != std::string::npos) {
        try { frame.serial = std::stoull(body.substr(sn_pos + 9, kNumLen)); } catch (...) {}
    }
    if (frame.serial == _last_spec_serial) return true;
    _last_spec_serial = frame.serial;

    auto cpos = body.find("\"center\":");
    if (cpos != std::string::npos)
        try { frame.center_hz = std::stod(body.substr(cpos + 9, kNumLen)); } catch (...) {}

    auto bpos = body.find("\"bandwidth\":");
    if (bpos != std::string::npos)
        try { frame.bw_hz = std::stod(body.substr(bpos + 12, kNumLen)); } catch (...) {}

    auto bins_pos = body.find("\"bins\":[");
    if (bins_pos != std::string::npos) {
        bins_pos += 8;
        auto end = body.find(']', bins_pos);
        if (end == std::string::npos) return true;   // malformed / truncated array
        std::string bins_str = body.substr(bins_pos, end - bins_pos);
        std::istringstream iss(bins_str);
        std::string token;
        while (std::getline(iss, token, ',')) {
            try { frame.bins.push_back(std::stof(token)); } catch (...) {}
        }
    }

    if (!frame.bins.empty()) {
        std::lock_guard<std::mutex> lk(_cb_mtx);
        if (_on_spectrum) _on_spectrum(frame);
    }
    return true;
}

bool PredatorNodeClient::_pollEvents() {
    std::string host, key;
    int port;
    {
        std::lock_guard<std::mutex> lk(_cfg_mtx);
        host = _active_host; port = _port; key = _api_key;
    }
    if (host.empty()) return false;

    std::string path = "/v1/events?since=" + std::to_string(_last_event_id);
    std::string body = tcp_get(host, port, path, key, 1000);
    if (body.empty()) return false;

    auto lid_pos = body.find("\"lastId\":");
    if (lid_pos != std::string::npos) {
        try { _last_event_id = std::stoull(body.substr(lid_pos + 9)); } catch (...) {}
    }

    auto evs_pos = body.find("\"events\":[");
    if (evs_pos != std::string::npos) {
        std::lock_guard<std::mutex> lk(_cb_mtx);
        if (_on_event) _on_event(body);
    }
    return true;
}

bool PredatorNodeClient::sendCommand(const std::string& cls,
                                     const std::string& action,
                                     const std::string& args_json)
{
    std::string host, key;
    int port;
    {
        std::lock_guard<std::mutex> lk(_cfg_mtx);
        host = _active_host; port = _port; key = _api_key;
    }
    if (host.empty() || _state != ConnState::CONNECTED) return false;

    std::string body = "{\"class\":\"" + cls + "\","
                      + "\"action\":\"" + action + "\","
                      + "\"args\":" + args_json + "}";

    std::string resp = tcp_post(host, port, "/v1/command", body, key, 2000);
    return resp.find("\"ok\":true") != std::string::npos;
}

void PredatorNodeClient::_workerLoop() {
    while (_running) {
        if (_state != ConnState::CONNECTED) {
            if (!_probe()) {
                std::this_thread::sleep_for(std::chrono::seconds(3));
                continue;
            }
        }
        bool spec_ok  = _pollSpectrum();
        bool event_ok = _pollEvents();

        if (!spec_ok && !event_ok) {
            _state = ConnState::DISCONNECTED;
            {
                std::lock_guard<std::mutex> lk(_cfg_mtx);
                _active_host.clear();
            }
            {
                std::lock_guard<std::mutex> lk(_cb_mtx);
                if (_on_state) _on_state(ConnState::DISCONNECTED);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            continue;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

}
