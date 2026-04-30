#include "cot_reporter.h"
#include <utils/net.h>
#include <utils/flog.h>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <chrono>
#include <functional>

namespace predator {

// ─────────────────────────────────────────────────────── helpers ──────────

std::string CotReporter::cotTime(std::time_t t) {
    if (t == 0) t = std::time(nullptr);
    char buf[32];
    std::tm* tm = std::gmtime(&t);
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S.000Z", tm);
    return std::string(buf);
}

std::string CotReporter::cotStale(std::time_t base, int secondsAhead) {
    return cotTime(base + secondsAhead);
}

std::string CotReporter::formatFreq(double hz) {
    if (hz >= 1e9)  { char b[32]; std::snprintf(b, sizeof(b), "%.4f GHz", hz / 1e9); return b; }
    if (hz >= 1e6)  { char b[32]; std::snprintf(b, sizeof(b), "%.4f MHz", hz / 1e6); return b; }
    if (hz >= 1e3)  { char b[32]; std::snprintf(b, sizeof(b), "%.3f kHz", hz / 1e3); return b; }
    char b[32]; std::snprintf(b, sizeof(b), "%.0f Hz", hz); return b;
}

std::string CotReporter::xmlEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 16);
    for (char c : s) {
        switch (c) {
        case '<':  out += "&lt;";   break;
        case '>':  out += "&gt;";   break;
        case '&':  out += "&amp;";  break;
        case '"':  out += "&quot;"; break;
        case '\'': out += "&apos;"; break;
        default:   out += c;        break;
        }
    }
    return out;
}

std::string CotReporter::deriveUid(const std::string& callsign) {
    // Deterministic UID: "PREDATORF-" + sanitised callsign (no spaces/special).
    std::string san = callsign;
    for (char& c : san) {
        if (!std::isalnum((unsigned char)c) && c != '-' && c != '_') c = '-';
    }
    // Append a short hash so two sensors with the same callsign differ.
    size_t h = std::hash<std::string>{}(callsign);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04X", (unsigned)(h & 0xFFFF));
    return std::string("PREDATORF-") + san + "-" + buf;
}

// ─────────────────────────────────────────────────── CoT XML builders ──────

std::string CotReporter::buildSa() {
    std::lock_guard<std::mutex> lk(mtx_);
    std::time_t now = std::time(nullptr);

    double lat  = hasFix_ ? lat_  : 0.0;
    double lon  = hasFix_ ? lon_  : 0.0;
    float  ce   = hasFix_ ? ce_   : 9999999.f;
    float  hae  = 9999999.f;

    const std::string& uid      = cfg_.uid;
    const std::string& callsign = cfg_.callsign;
    std::string        csEsc    = xmlEscape(callsign);
    std::string        uidEsc   = xmlEscape(uid);

    char buf[2048];
    std::snprintf(buf, sizeof(buf),
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<event version=\"2.0\""
        " uid=\"%s\""
        " type=\"a-f-G-U-C\""
        " time=\"%s\""
        " start=\"%s\""
        " stale=\"%s\""
        " how=\"m-g\">"
        "<point lat=\"%.7f\" lon=\"%.7f\" hae=\"%.1f\" ce=\"%.1f\" le=\"9999999.0\"/>"
        "<detail>"
          "<contact callsign=\"%s\"/>"
          "<__group name=\"Cyan\" role=\"Team Member\"/>"
          "<status battery=\"100\"/>"
          "<takv version=\"4.10.0.0\" platform=\"Predator RF\" device=\"Android\" os=\"30\"/>"
          "<uid Droid=\"%s\"/>"
          "<precisionlocation geopointsrc=\"%s\" altsrc=\"NONE\"/>"
        "</detail>"
        "</event>",
        uidEsc.c_str(),
        cotTime(now).c_str(),
        cotTime(now).c_str(),
        cotStale(now, 300).c_str(),
        lat, lon, (double)hae, (double)ce,
        csEsc.c_str(),
        csEsc.c_str(),
        hasFix_ ? "GPS" : "????"
    );
    return std::string(buf);
}

std::string CotReporter::buildChat(const std::string& message) {
    std::lock_guard<std::mutex> lk(mtx_);
    std::time_t now = std::time(nullptr);

    double lat = hasFix_ ? lat_ : 0.0;
    double lon = hasFix_ ? lon_ : 0.0;

    const std::string& uid       = cfg_.uid;
    const std::string& callsign  = cfg_.callsign;
    const std::string& chatRoom  = cfg_.chatRoom;

    std::string msgEsc   = xmlEscape(message);
    std::string csEsc    = xmlEscape(callsign);
    std::string uidEsc   = xmlEscape(uid);
    std::string roomEsc  = xmlEscape(chatRoom);
    std::string nowStr   = cotTime(now);

    // Message UID: stable prefix + millisecond timestamp
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    char msgUid[128];
    std::snprintf(msgUid, sizeof(msgUid), "GeoChat.%s.%s.%lld",
                  uid.c_str(), chatRoom.c_str(), (long long)ms);
    std::string msgUidEsc = xmlEscape(std::string(msgUid));

    char buf[4096];
    std::snprintf(buf, sizeof(buf),
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<event version=\"2.0\""
        " uid=\"%s\""
        " type=\"b-t-f\""
        " time=\"%s\""
        " start=\"%s\""
        " stale=\"%s\""
        " how=\"h-g-i-g-o\""
        " access=\"Undefined\""
        " qos=\"1-r-c\""
        " opex=\"*\">"
        "<point lat=\"%.7f\" lon=\"%.7f\" hae=\"9999999.0\" ce=\"9999999.0\" le=\"9999999.0\"/>"
        "<detail>"
          "<__chat parent=\"TeamBlue\" groupOwner=\"false\""
          " chatroom=\"%s\""
          " senderCallsign=\"%s\""
          " id=\"%s\">"
            "<chatgrp uid0=\"%s\" uid1=\"%s\" id=\"%s\"/>"
          "</__chat>"
          "<link uid=\"%s\" type=\"a-f-G-U-C\" relation=\"p-p\"/>"
          "<remarks source=\"BAO.F.ATAK.%s\" to=\"%s\" time=\"%s\">%s</remarks>"
          "<__serverdestination destinations=\"%s:broadcast\"/>"
          "<marti><dest callsign=\"%s\"/></marti>"
        "</detail>"
        "</event>",
        // event attrs
        msgUidEsc.c_str(), nowStr.c_str(), nowStr.c_str(),
        cotStale(now, 60).c_str(),
        // point
        lat, lon,
        // __chat
        roomEsc.c_str(), csEsc.c_str(), roomEsc.c_str(),
        uidEsc.c_str(), roomEsc.c_str(), roomEsc.c_str(),
        // link
        uidEsc.c_str(),
        // remarks
        uidEsc.c_str(), roomEsc.c_str(), nowStr.c_str(), msgEsc.c_str(),
        // serverdestination / marti
        roomEsc.c_str(),
        roomEsc.c_str()
    );
    return std::string(buf);
}

// ─────────────────────────────────────────────────────── transport ─────────

bool CotReporter::sendPacket(const std::string& xml) noexcept {
    std::string host;
    int         port;
    bool        udp;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        host = cfg_.host;
        port = cfg_.port;
        udp  = cfg_.useUdp;
    }
    try {
        if (udp) {
            auto sock = net::openudp(host, port);
            sock->sendstr(xml);
            sock->close();
        } else {
            auto sock = net::connect(host, port);
            sock->sendstr(xml);
            sock->close();
        }
        return true;
    } catch (const std::exception& ex) {
        flog::warn("[CoT] send failed ({}:{}): {}", host, port, ex.what());
        return false;
    }
}

// ────────────────────────────────────────────────── SA beacon thread ───────

void CotReporter::saLoop() {
    flog::info("[CoT] SA beacon thread started");
    while (saRunning_.load(std::memory_order_relaxed)) {
        float intervalSec;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            intervalSec = cfg_.saIntervalSec;
        }

        if (intervalSec > 0.1f) {
            std::string sa = buildSa();
            sendPacket(sa);
        }

        // Sleep for intervalSec (or until saRunning_ is cleared).
        std::unique_lock<std::mutex> lk(saCvMtx_);
        saCv_.wait_for(lk,
            std::chrono::milliseconds(static_cast<int>(intervalSec * 1000.0f)),
            [this]{ return !saRunning_.load(std::memory_order_relaxed); });
    }
    flog::info("[CoT] SA beacon thread stopped");
}

// ─────────────────────────────────────────────────────── public API ────────

CotReporter::~CotReporter() {
    saRunning_.store(false, std::memory_order_relaxed);
    saCv_.notify_all();
    if (saThread_.joinable()) saThread_.join();
}

void CotReporter::configure(const Config& cfg) {
    bool needRestart = false;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        Config patched = cfg;

        // Auto-derive UID if caller left it blank.
        if (patched.uid.empty()) {
            patched.uid = deriveUid(patched.callsign);
        }

        bool wasEnabled    = cfg_.enabled && cfg_.saIntervalSec > 0.1f;
        bool wantEnabled   = patched.enabled && patched.saIntervalSec > 0.1f;
        bool configChanged = (cfg_.host          != patched.host          ||
                              cfg_.port          != patched.port          ||
                              cfg_.uid           != patched.uid           ||
                              cfg_.callsign      != patched.callsign      ||
                              cfg_.saIntervalSec != patched.saIntervalSec);

        cfg_ = patched;
        needRestart = (wantEnabled && (!wasEnabled || configChanged));
        if (!wantEnabled && wasEnabled) needRestart = false; // stop only
    }

    // Stop existing thread if config changed or disabled.
    if (saThread_.joinable()) {
        saRunning_.store(false, std::memory_order_relaxed);
        saCv_.notify_all();
        saThread_.join();
    }

    // Restart if required.
    bool shouldRun;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        shouldRun = cfg_.enabled && cfg_.saIntervalSec > 0.1f;
    }
    if (shouldRun) {
        saRunning_.store(true, std::memory_order_relaxed);
        saThread_ = std::thread([this]{ saLoop(); });
    }
}

void CotReporter::updateGps(double lat, double lon, float ceMeters, bool hasFix) {
    std::lock_guard<std::mutex> lk(mtx_);
    lat_    = lat;
    lon_    = lon;
    ce_     = ceMeters > 0.f ? ceMeters : 9999999.f;
    hasFix_ = hasFix;
}

void CotReporter::reportHit(double freqHz, float strengthDb, float snrDb,
                             int hitCount, const std::string& state,
                             const std::string& label) {
    bool enabled;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        enabled = cfg_.enabled;
    }
    if (!enabled) return;

    // Build human-readable message.
    char msg[256];
    std::string freqStr = formatFreq(freqHz);
    const char* stateStr = state.empty() ? "hit" : state.c_str();
    const char* lbl = label.empty() ? "" : label.c_str();

    if (label.empty()) {
        std::snprintf(msg, sizeof(msg),
            "[Predator RF] HIT: %s | %.1f dB | SNR: %.1f dB | #%d | %s",
            freqStr.c_str(), strengthDb, snrDb, hitCount, stateStr);
    } else {
        std::snprintf(msg, sizeof(msg),
            "[Predator RF] HIT: %s | %.1f dB | SNR: %.1f dB | #%d | %s | %s",
            freqStr.c_str(), strengthDb, snrDb, hitCount, stateStr, lbl);
    }

    sendPacket(buildChat(std::string(msg)));
}

} // namespace predator
