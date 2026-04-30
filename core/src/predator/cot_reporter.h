#pragma once
/*
 * Predator RF — ATAK CoT (Cursor on Target) reporter.
 *
 * Sends CoT XML to a user-configured TAK endpoint (UDP multicast / UDP
 * unicast / TCP TAK Server).  Two CoT types are emitted:
 *
 *   a-f-G-U-C  — Sensor SA (Situation Awareness) heartbeat.  Sent
 *                periodically so the sensor shows up on the ATAK map
 *                as a friendly unit at the GPS fix reported by the phone.
 *
 *   b-t-f      — GeoChat message.  Sent to the configured chat room each
 *                time recordPeakHit() sees a non-suppressed hit event.
 *                Payload includes frequency, signal strength, SNR, and
 *                the hit state / label.
 *
 * The reporter is owned by MainWindow and lives for the duration of the
 * process.  configure() may be called any number of times (e.g. when the
 * user edits TAK settings).  updateGps() should be called every draw()
 * frame so SA beacons always carry the latest fix.  reportHit() is called
 * from recordPeakHit() for every non-suppressed hit event.
 *
 * Thread model:
 *   • SA beacon thread  — wakes every saIntervalSec, acquires mtx_ for a
 *     snapshot, sends one SA packet, goes back to sleep.
 *   • UI thread         — calls updateGps() and reportHit(); acquires mtx_
 *     only for the brief snapshot needed to build the XML.
 */
#include <string>
#include <atomic>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <ctime>

namespace predator {

class CotReporter {
public:
    struct Config {
        bool        enabled       = false;
        bool        useUdp        = true;
        std::string host          = "239.2.3.1";   // ATAK SA multicast
        int         port          = 6969;
        std::string uid;                            // auto-derived from callsign if empty
        std::string callsign      = "Predator RF";
        std::string chatRoom      = "All Chat Rooms";
        bool        sensorMode    = true;           // true = separate sensor entity
        float       saIntervalSec = 30.0f;          // 0 = disable SA beacon
    };

    ~CotReporter();

    // Apply (or re-apply) configuration.  Idempotent — safe to call every
    // frame if config hasn't changed.  Restarts the SA thread when needed.
    void configure(const Config& cfg);

    // Called from the UI thread every draw() frame with the latest GPS fix.
    void updateGps(double lat, double lon, float ceMeters, bool hasFix);

    // Fire a GeoChat hit report.  Non-blocking — returns immediately after
    // building and transmitting the UDP/TCP packet.
    void reportHit(double freqHz, float strengthDb, float snrDb,
                   int hitCount, const std::string& state,
                   const std::string& label = "");

private:
    void        saLoop();
    bool        sendPacket(const std::string& xml) noexcept;
    std::string buildChat(const std::string& message);
    std::string buildSa();
    std::string cotTime(std::time_t t = 0);
    std::string cotStale(std::time_t base, int secondsAhead);
    std::string formatFreq(double hz);
    static std::string xmlEscape(const std::string& s);
    static std::string deriveUid(const std::string& callsign);

    Config      cfg_;
    std::mutex  mtx_;

    // GPS state (protected by mtx_)
    double lat_    = 0.0;
    double lon_    = 0.0;
    float  ce_     = 9999999.f;
    bool   hasFix_ = false;

    // SA beacon management
    std::thread              saThread_;
    std::atomic<bool>        saRunning_{false};
    std::condition_variable  saCv_;
    std::mutex               saCvMtx_;
};

} // namespace predator
