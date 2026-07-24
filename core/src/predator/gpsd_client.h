#pragma once
#include <atomic>
#include <thread>
#include <string>
#include <cstring>
#include <cmath>

#ifndef _WIN32
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#endif

#include <json.hpp>

/*
 * Minimal gpsd client for headless/desktop sensor nodes with a USB GPS
 * dongle. Connects to gpsd's JSON protocol (default 127.0.0.1:2947),
 * issues ?WATCH, and keeps the latest TPV fix in atomics. A fix is
 * considered live for 15 s after the last TPV with mode >= 2.
 *
 * Degrades gracefully: if gpsd is missing or unreachable it retries with
 * backoff forever and getFix() simply reports no fix. Windows builds
 * compile to a stub (fixed sensor sites there use the static position).
 */

namespace predator {

class GpsdClient {
public:
    ~GpsdClient() { stop(); }

    void start(const std::string& host = "127.0.0.1", int port = 2947) {
        if (running_.exchange(true)) return;
        host_ = host;
        port_ = port;
        thread_ = std::thread(&GpsdClient::loop, this);
    }

    void stop() {
        if (!running_.exchange(false)) return;
        if (thread_.joinable()) thread_.join();
    }

    bool isRunning() const { return running_.load(); }

    // Returns true only if a recent (<15 s) 2D+ fix is available.
    bool getFix(double& lat, double& lon) const {
        long long ageMs = nowMs() - lastFixMs_.load(std::memory_order_relaxed);
        if (lastFixMs_.load(std::memory_order_relaxed) == 0 || ageMs > 15000) return false;
        lat = lat_.load(std::memory_order_relaxed);
        lon = lon_.load(std::memory_order_relaxed);
        return std::isfinite(lat) && std::isfinite(lon) && !(lat == 0.0 && lon == 0.0);
    }

private:
    static long long nowMs() {
        return (long long)std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }

#ifdef _WIN32
    void loop() { running_.store(false); }
#else
    void loop() {
        int backoffS = 2;
        while (running_.load()) {
            int fd = ::socket(AF_INET, SOCK_STREAM, 0);
            if (fd < 0) { sleepWhileRunning(backoffS); continue; }
            struct timeval tv { 2, 0 };
            setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
            sockaddr_in addr {};
            addr.sin_family = AF_INET;
            addr.sin_port = htons((uint16_t)port_);
            if (inet_pton(AF_INET, host_.c_str(), &addr.sin_addr) != 1
                || ::connect(fd, (sockaddr*)&addr, sizeof(addr)) != 0) {
                ::close(fd);
                sleepWhileRunning(backoffS);
                backoffS = std::min(backoffS * 2, 30);
                continue;
            }
            const char* watch = "?WATCH={\"enable\":true,\"json\":true};\n";
            if (::send(fd, watch, strlen(watch), 0) < 0) {
                ::close(fd);
                sleepWhileRunning(backoffS);
                continue;
            }
            backoffS = 2;
            std::string buf;
            char tmp[2048];
            while (running_.load()) {
                ssize_t n = ::recv(fd, tmp, sizeof(tmp), 0);
                if (n == 0) break;                       // gpsd closed
                if (n < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
                    break;
                }
                buf.append(tmp, (size_t)n);
                size_t nl;
                while ((nl = buf.find('\n')) != std::string::npos) {
                    std::string line = buf.substr(0, nl);
                    buf.erase(0, nl + 1);
                    handleLine(line);
                }
                if (buf.size() > 65536) buf.clear();     // runaway guard
            }
            ::close(fd);
            sleepWhileRunning(2);
        }
    }
#endif

    void handleLine(const std::string& line) {
        try {
            auto j = nlohmann::json::parse(line, nullptr, false);
            if (!j.is_object()) return;
            if (j.value("class", "") != "TPV") return;
            if (j.value("mode", 0) < 2) return;
            if (!j.contains("lat") || !j.contains("lon")) return;
            double la = j["lat"].get<double>();
            double lo = j["lon"].get<double>();
            if (!std::isfinite(la) || !std::isfinite(lo)) return;
            lat_.store(la, std::memory_order_relaxed);
            lon_.store(lo, std::memory_order_relaxed);
            lastFixMs_.store(nowMs(), std::memory_order_relaxed);
        } catch (...) {}
    }

    void sleepWhileRunning(int seconds) {
        for (int i = 0; i < seconds * 10 && running_.load(); i++) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

    std::atomic<bool> running_ { false };
    std::thread thread_;
    std::string host_;
    int port_ = 2947;
    std::atomic<double> lat_ { 0.0 };
    std::atomic<double> lon_ { 0.0 };
    std::atomic<long long> lastFixMs_ { 0 };
};

} // namespace predator
