#include <backend.h>
#include "web_server.h"

#include <utils/flog.h>
#include <version.h>
#include <core.h>
#include <signal_path/signal_path.h>
#include <gui/gui.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#endif

#include "../../../core/src/json.hpp"

namespace backend {

// ---------------------------------------------------------------------------
// Internal state
// ---------------------------------------------------------------------------

static predator::PredatorWebServer  gServer;
static std::string                  gWebRoot;
static int                          gPort = 5555;
static std::atomic<bool>            gRunning{false};

// Unix control socket for predator-rfctl
#ifndef _WIN32
static int gCtrlSock = -1;
static std::string gCtrlPath;
static std::thread gCtrlThread;
#endif

// Ring buffer of events pushed from main_window → web clients
static std::mutex                   gEventMtx;
static std::vector<nlohmann::json>  gEventRing;
static uint64_t                     gEventBase = 0;
static constexpr size_t             EVENT_RING_SIZE = 512;
static uint64_t                     gLastEventId = 0;

// Spectrum snapshot written by releaseFFTBuffer path
static std::mutex               gSpecMtx;
static std::vector<float>       gSpecBins;
static double                   gSpecCenter  = 0.0;
static double                   gSpecBandwidth = 0.0;
static float                    gSpecFftMin  = -120.0f;
static float                    gSpecFftMax  = 0.0f;
static uint64_t                 gSpecSerial  = 0;

// Snapshot of fleet peers and track list, refreshed from draw() snapshots
static std::mutex               gStateMtx;
static nlohmann::json           gStateNodes  = nlohmann::json::array();
static nlohmann::json           gStateTracks = nlohmann::json::array();

// ---------------------------------------------------------------------------
// Helper: push an event into the ring (callable from any thread)
// ---------------------------------------------------------------------------
static void pushEvent(nlohmann::json ev) {
    std::lock_guard<std::mutex> lk(gEventMtx);
    ev["id"] = ++gLastEventId;
    gEventRing.push_back(std::move(ev));
    while(gEventRing.size() > EVENT_RING_SIZE) {
        gEventRing.erase(gEventRing.begin());
        gEventBase++;
    }
    // Push SSE to connected browser clients
    gServer.pushSse(gEventRing.back().dump());
}

// ---------------------------------------------------------------------------
// REST route handlers
// ---------------------------------------------------------------------------

static void routeIdentify(predator::PwsContext& ctx) {
    nlohmann::json j;
    j["device"]     = "predator-rfd";
    j["version"]    = VERSION_STR;
    j["role"]       = "device";
    j["hwProfile"]  = "linux";
    j["webPort"]    = gPort;
    predator::pwsHttpReply(ctx.sock, 200, "application/json", j.dump());
}

static void routeState(predator::PwsContext& ctx) {
    std::lock_guard<std::mutex> lk(gStateMtx);
    nlohmann::json j;
    j["nodes"]  = gStateNodes;
    j["tracks"] = gStateTracks;
    predator::pwsHttpReply(ctx.sock, 200, "application/json", j.dump());
}

static void routeNodes(predator::PwsContext& ctx) {
    std::lock_guard<std::mutex> lk(gStateMtx);
    predator::pwsHttpReply(ctx.sock, 200, "application/json", gStateNodes.dump());
}

static void routeTracks(predator::PwsContext& ctx) {
    std::lock_guard<std::mutex> lk(gStateMtx);
    predator::pwsHttpReply(ctx.sock, 200, "application/json", gStateTracks.dump());
}

static void routeEvents(predator::PwsContext& ctx) {
    uint64_t since = 0;
    auto it = ctx.query.find("since");
    if(it != ctx.query.end()) {
        try { since = std::stoull(it->second); } catch(...) {}
    }
    std::lock_guard<std::mutex> lk(gEventMtx);
    nlohmann::json arr = nlohmann::json::array();
    for(auto& ev : gEventRing) {
        uint64_t id = ev.value("id", (uint64_t)0);
        if(id > since) arr.push_back(ev);
    }
    nlohmann::json j;
    j["events"] = arr;
    j["lastId"]  = gLastEventId;
    predator::pwsHttpReply(ctx.sock, 200, "application/json", j.dump());
}

static void routeSpectrum(predator::PwsContext& ctx) {
    int bins = 256;
    auto it = ctx.query.find("bins");
    if(it != ctx.query.end()) {
        try { bins = std::stoi(it->second); } catch(...) {}
    }
    bins = std::max(64, std::min(bins, 8192));

    std::lock_guard<std::mutex> lk(gSpecMtx);
    nlohmann::json j;
    j["serial"]    = gSpecSerial;
    j["center"]    = gSpecCenter;
    j["bandwidth"] = gSpecBandwidth;
    j["fft_min"]   = gSpecFftMin;
    j["fft_max"]   = gSpecFftMax;

    std::vector<float> out;
    if(!gSpecBins.empty()) {
        size_t src = gSpecBins.size();
        out.resize(bins);
        for(int i = 0; i < bins; i++) {
            size_t si = (size_t)((double)i / bins * src);
            if(si >= src) si = src - 1;
            out[i] = gSpecBins[si];
        }
    }
    j["bins"] = out;
    predator::pwsHttpReply(ctx.sock, 200, "application/json", j.dump());
}

static void routeCommand(predator::PwsContext& ctx) {
    if(ctx.req.method != "POST") {
        predator::pwsHttpReply(ctx.sock, 405, "application/json", "{\"error\":\"method not allowed\"}");
        return;
    }
    // Reject TX class
    nlohmann::json body;
    try { body = nlohmann::json::parse(ctx.req.body); } catch(...) {
        predator::pwsHttpReply(ctx.sock, 400, "application/json", "{\"error\":\"bad json\"}");
        return;
    }
    std::string cls = body.value("class", "");
    if(cls.rfind("tx", 0) == 0) {
        predator::pwsHttpReply(ctx.sock, 403, "application/json", "{\"error\":\"tx commands rejected\"}");
        return;
    }
    // Enqueue into core command queue (same path as Kujhad device server)
    // For now emit an event so the operator sees it in the live stream
    nlohmann::json ev;
    ev["type"]   = "web_command";
    ev["class"]  = cls;
    ev["action"] = body.value("action", "");
    ev["args"]   = body.value("args", nlohmann::json::object());
    pushEvent(ev);
    predator::pwsHttpReply(ctx.sock, 200, "application/json", "{\"ok\":true}");
}

// ---------------------------------------------------------------------------
// Unix control socket (predator-rfctl)
// ---------------------------------------------------------------------------
#ifndef _WIN32
static void ctrlSocketLoop() {
    char buf[8192];
    while(gRunning) {
        fd_set rset; FD_ZERO(&rset); FD_SET(gCtrlSock, &rset);
        timeval tv{0, 200000};
        if(::select(gCtrlSock + 1, &rset, nullptr, nullptr, &tv) <= 0) continue;
        sockaddr_un peer{}; socklen_t plen = sizeof(peer);
        int c = (int)::accept(gCtrlSock, (sockaddr*)&peer, &plen);
        if(c < 0) continue;
        int n = (int)::recv(c, buf, sizeof(buf) - 1, 0);
        if(n > 0) {
            buf[n] = 0;
            nlohmann::json cmd, resp;
            try { cmd = nlohmann::json::parse(buf); } catch(...) {
                resp["ok"] = false; resp["error"] = "bad json";
                auto s = resp.dump();
                ::send(c, s.c_str(), (int)s.size(), 0);
                ::close(c);
                continue;
            }
            std::string cls = cmd.value("class", "");
            if(cls.rfind("tx", 0) == 0) {
                resp["ok"] = false; resp["error"] = "tx commands rejected";
            } else {
                // Emit into event ring so WebSocket clients see the command
                nlohmann::json ev;
                ev["type"]   = "rfctl_command";
                ev["class"]  = cls;
                ev["action"] = cmd.value("action", "");
                ev["args"]   = cmd.value("args", nlohmann::json::object());
                pushEvent(ev);
                resp["ok"] = true;
            }
            auto s = resp.dump();
            ::send(c, s.c_str(), (int)s.size(), 0);
        }
        ::close(c);
    }
}

static bool startCtrlSocket(const std::string& path) {
    gCtrlPath = path;
    ::unlink(path.c_str());
    gCtrlSock = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if(gCtrlSock < 0) return false;
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    ::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
    if(::bind(gCtrlSock, (sockaddr*)&addr, sizeof(addr)) != 0) {
        ::close(gCtrlSock); gCtrlSock = -1; return false;
    }
    ::chmod(path.c_str(), 0600);
    if(::listen(gCtrlSock, 8) != 0) {
        ::close(gCtrlSock); gCtrlSock = -1; return false;
    }
    gCtrlThread = std::thread(ctrlSocketLoop);
    return true;
}
#endif

// ---------------------------------------------------------------------------
// Periodic push: FFT bins → WebSocket clients
// ---------------------------------------------------------------------------
static void spectrumPushLoop() {
    uint64_t lastPushed = 0;
    while(gRunning) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        uint64_t serial;
        {
            std::lock_guard<std::mutex> lk(gSpecMtx);
            serial = gSpecSerial;
        }
        if(serial == lastPushed || gSpecBins.empty()) continue;
        lastPushed = serial;

        nlohmann::json j;
        j["type"]   = "spectrum";
        {
            std::lock_guard<std::mutex> lk(gSpecMtx);
            j["serial"]    = gSpecSerial;
            j["center"]    = gSpecCenter;
            j["bandwidth"] = gSpecBandwidth;
            j["fft_min"]   = gSpecFftMin;
            j["fft_max"]   = gSpecFftMax;
            constexpr int WS_BINS = 256;
            std::vector<float> out(WS_BINS);
            size_t src = gSpecBins.size();
            for(int i = 0; i < WS_BINS; i++) {
                size_t si = (size_t)((double)i / WS_BINS * src);
                if(si >= src) si = src - 1;
                out[i] = gSpecBins[si];
            }
            j["bins"] = out;
        }
        gServer.broadcastWs(j.dump());
    }
}

// ---------------------------------------------------------------------------
// backend:: interface
// ---------------------------------------------------------------------------

int init(std::string resDir) {
    flog::info("Predator web backend: init");

    core::configManager.acquire();

    // Web port
    if(core::configManager.conf.contains("webBackendPort")) {
        gPort = core::configManager.conf["webBackendPort"].get<int>();
    }

    // Web asset root: env var > config > resDir/web > fallback to resDir
    const char* envRoot = ::getenv("PREDATOR_WEB_ROOT");
    if(envRoot && *envRoot) {
        gWebRoot = envRoot;
    } else if(core::configManager.conf.contains("webRoot")) {
        gWebRoot = core::configManager.conf["webRoot"].get<std::string>();
    } else if(!resDir.empty()) {
        gWebRoot = resDir + "/web";
    }

    // Control socket path
    std::string ctrlPath = "/run/predator-rfd/control.sock";
    if(core::configManager.conf.contains("webCtrlSocket")) {
        ctrlPath = core::configManager.conf["webCtrlSocket"].get<std::string>();
    }
    const char* envCtrl = ::getenv("PREDATOR_CTRL_SOCK");
    if(envCtrl && *envCtrl) ctrlPath = envCtrl;

    core::configManager.release();

    // Register routes
    gServer.setStaticRoot(gWebRoot);

    gServer.addRoute("GET",  "/api/identify",      routeIdentify);
    gServer.addRoute("GET",  "/api/state",          routeState);
    gServer.addRoute("GET",  "/api/spectrum",        routeSpectrum);
    gServer.addRoute("GET",  "/api/events",          routeEvents);
    gServer.addRoute("POST", "/api/command",         routeCommand);

    // Python-backend-compatible endpoints (same paths preview.html uses)
    gServer.addRoute("GET",  "/nodes/",  routeNodes);
    gServer.addRoute("GET",  "/tracks/", routeTracks);
    // /events/stream is handled via SSE upgrade (Accept: text/event-stream)
    // on the path /events/stream; the SSE handler is the fallback inside
    // PredatorWebServer::handleSse() which pushes sseClients_ on any path
    // whose request has Accept: text/event-stream. The route below sets up
    // the dedicated path so the server picks it up before the 404 path.
    gServer.addRoute("GET",  "/events/stream", [](predator::PwsContext& ctx){
        // SSE clients are held open by handleSse(); this route is unused
        // but prevents a 404 on a non-SSE GET.
        predator::pwsHttpReply(ctx.sock, 200, "text/plain", "");
    });

    // Kujhad-compatible v1 aliases (so the fleet console still works)
    gServer.addRoute("GET", "/v1/identify", routeIdentify);
    gServer.addRoute("GET", "/v1/state",    routeState);
    gServer.addRoute("GET", "/v1/events",   routeEvents);
    gServer.addRoute("POST","/v1/command",  routeCommand);

    if(!gServer.start(gPort)) {
        flog::error("Predator web backend: failed to bind port {}", gPort);
        return 1;
    }
    flog::info("Predator web backend: HTTP+WS on port {}", gPort);
    if(!gWebRoot.empty()) flog::info("Predator web backend: serving static from '{}'", gWebRoot);

    gRunning = true;

#ifndef _WIN32
    if(!startCtrlSocket(ctrlPath)) {
        flog::warn("Predator web backend: control socket unavailable at '{}'", ctrlPath);
    } else {
        flog::info("Predator web backend: control socket at '{}'", ctrlPath);
    }
#endif

    // Start background spectrum push thread
    std::thread(spectrumPushLoop).detach();

    return 0;
}

void beginFrame() {
    // No-op: no ImGui frame in headless mode
}

void render(bool /*vsync*/) {
    // No-op: no display to render
}

void getMouseScreenPos(double& x, double& y) { x = 0.0; y = 0.0; }
void setMouseScreenPos(double /*x*/, double /*y*/) {}

bool getPhoneLocation(double& lat, double& lon, float& accuracy, bool& hasFix) {
    // gpsd integration point (future): connect to gpsd and return fix
    lat = 0.0; lon = 0.0; accuracy = 0.0f; hasFix = false;
    return false;
}

bool openMapView() { return false; }
float getNativeUiScale() { return 1.0f; }
bool isTouchPrimary() { return false; }
int getImeBottomInset() { return 0; }
SafeAreaInsets getSafeAreaInsets() { return {}; }

int renderLoop() {
    flog::info("Predator web backend: headless render loop running");

    // Main loop: sleep, let the server threads handle requests.
    // A future iteration can tick the signal path here.
    while(gRunning) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return 0;
}

int end() {
    gRunning = false;
    gServer.stop();

#ifndef _WIN32
    if(gCtrlSock >= 0) {
        ::close(gCtrlSock);
        gCtrlSock = -1;
    }
    if(gCtrlThread.joinable()) gCtrlThread.join();
    if(!gCtrlPath.empty()) ::unlink(gCtrlPath.c_str());
#endif

    flog::info("Predator web backend: stopped");
    return 0;
}

// ---------------------------------------------------------------------------
// Hooks called from main_window.cpp or the signal path to feed live data.
// Called from the UI thread; all writes under appropriate mutexes.
// ---------------------------------------------------------------------------

void webBackendPushSpectrumSnapshot(const float* bins, int count,
                                     double centerHz, double bwHz,
                                     float fftMin, float fftMax) {
    std::lock_guard<std::mutex> lk(gSpecMtx);
    gSpecBins.assign(bins, bins + count);
    gSpecCenter    = centerHz;
    gSpecBandwidth = bwHz;
    gSpecFftMin    = fftMin;
    gSpecFftMax    = fftMax;
    gSpecSerial++;
}

void webBackendPushEvent(const nlohmann::json& ev) {
    pushEvent(ev);
}

void webBackendUpdateNodes(const nlohmann::json& nodes) {
    std::lock_guard<std::mutex> lk(gStateMtx);
    gStateNodes = nodes;
}

void webBackendUpdateTracks(const nlohmann::json& tracks) {
    std::lock_guard<std::mutex> lk(gStateMtx);
    gStateTracks = tracks;
}

} // namespace backend
