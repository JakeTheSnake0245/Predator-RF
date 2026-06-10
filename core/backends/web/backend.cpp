#include <backend.h>
#include "web_server.h"

#include <utils/flog.h>
#include <version.h>
#include <core.h>
#include <signal_path/signal_path.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <queue>

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
static std::string                  gApiKey;

// Unix control socket for predator-rfctl
#ifndef _WIN32
static int gCtrlSock = -1;
static std::string gCtrlPath;
static std::thread gCtrlThread;
#endif

// Ring buffer of events pushed from signal path → web clients
static std::mutex                   gEventMtx;
static std::vector<nlohmann::json>  gEventRing;
static constexpr size_t             EVENT_RING_SIZE = 512;
static std::atomic<uint64_t>        gLastEventId{0};

// Spectrum snapshot
static std::mutex               gSpecMtx;
static std::vector<float>       gSpecBins;
static double                   gSpecCenter  = 0.0;
static double                   gSpecBandwidth = 0.0;
static float                    gSpecFftMin  = -120.0f;
static float                    gSpecFftMax  = 0.0f;
static uint64_t                 gSpecSerial  = 0;

// Fleet state snapshots
static std::mutex               gStateMtx;
static nlohmann::json           gStateNodes  = nlohmann::json::array();
static nlohmann::json           gStateTracks = nlohmann::json::array();

// Daemon config snapshot (reported by /api/v1/status)
static std::mutex               gCfgMtx;
static std::string              gDeviceName;
static std::string              gRole = "device";
static bool                     gSdrRunning = false;
static double                   gCenterFreq = 0.0;
static double                   gBandwidth  = 0.0;
static std::string              gSourceName;
static int                      gMissionMode = 0;
static bool                     gScanRunning = false;
static std::string              gScanStatus = "idle";

// Pending command queue (drains in renderLoop on the main thread)
struct PendingCmd {
    std::string cls;
    std::string action;
    nlohmann::json args;
    std::string origin; // "web" or "rfctl"
};
static std::mutex               gCmdMtx;
static std::queue<PendingCmd>   gCmdQueue;

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

static void pushEvent(nlohmann::json ev) {
    ev["id"] = ++gLastEventId;
    {
        std::lock_guard<std::mutex> lk(gEventMtx);
        gEventRing.push_back(ev);
        while(gEventRing.size() > EVENT_RING_SIZE) gEventRing.erase(gEventRing.begin());
    }
    gServer.pushSse(ev.dump());
}

static void enqueueCmd(const std::string& cls, const std::string& action,
                        const nlohmann::json& args, const std::string& origin) {
    // Hard-reject tx.* class at the enqueue point
    if(cls.rfind("tx",0)==0) return;
    std::lock_guard<std::mutex> lk(gCmdMtx);
    gCmdQueue.push({cls, action, args, origin});
}

// ---------------------------------------------------------------------------
// Route handlers — REST API
// ---------------------------------------------------------------------------

static void routeIdentify(predator::PwsContext& ctx) {
    std::lock_guard<std::mutex> lk(gCfgMtx);
    nlohmann::json j;
    j["device"]    = gDeviceName.empty() ? std::string("predator-rfd") : gDeviceName;
    j["version"]   = VERSION_STR;
    j["role"]      = gRole;
    j["hwProfile"] = "linux";
    j["webPort"]   = gPort;
    predator::pwsHttpReply(ctx.sock, 200, "application/json", j.dump());
}

static void routeStatus(predator::PwsContext& ctx) {
    std::lock_guard<std::mutex> lk(gCfgMtx);
    nlohmann::json j;
    j["device"]      = gDeviceName.empty() ? std::string("predator-rfd") : gDeviceName;
    j["version"]     = VERSION_STR;
    j["role"]        = gRole;
    j["sdr_running"] = gSdrRunning;
    j["center_freq"] = gCenterFreq;
    j["bandwidth"]   = gBandwidth;
    j["source"]      = gSourceName;
    j["mission_mode"]= gMissionMode;
    j["scan_running"]= gScanRunning;
    j["scan_status"] = gScanStatus;
    j["web_port"]    = gPort;
    {
        std::lock_guard<std::mutex> lk2(gEventMtx);
        j["event_count"] = gLastEventId;
    }
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

static void routeState(predator::PwsContext& ctx) {
    nlohmann::json j;
    {
        std::lock_guard<std::mutex> lk(gStateMtx);
        j["nodes"]  = gStateNodes;
        j["tracks"] = gStateTracks;
    }
    {
        std::lock_guard<std::mutex> lk(gCfgMtx);
        j["role"]         = gRole;
        j["sdr_running"]  = gSdrRunning;
        j["center_freq"]  = gCenterFreq;
        j["bandwidth"]    = gBandwidth;
        j["source"]       = gSourceName;
        j["mission_mode"] = gMissionMode;
        j["scan_running"] = gScanRunning;
    }
    predator::pwsHttpReply(ctx.sock, 200, "application/json", j.dump());
}

static void routeEvents(predator::PwsContext& ctx) {
    uint64_t since = 0;
    auto it = ctx.query.find("since");
    if(it != ctx.query.end()) {
        try { since = std::stoull(it->second); } catch(...) {}
    }
    nlohmann::json arr = nlohmann::json::array();
    {
        std::lock_guard<std::mutex> lk(gEventMtx);
        for(auto& ev : gEventRing) {
            uint64_t id = ev.value("id", (uint64_t)0);
            if(id > since) arr.push_back(ev);
        }
    }
    nlohmann::json j;
    j["events"] = arr;
    j["lastId"]  = gLastEventId;
    predator::pwsHttpReply(ctx.sock, 200, "application/json", j.dump());
}

static void routeSpectrum(predator::PwsContext& ctx) {
    int bins = 256;
    auto it = ctx.query.find("bins");
    if(it != ctx.query.end()) { try { bins=std::stoi(it->second); } catch(...) {} }
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
        predator::pwsHttpReply(ctx.sock, 405, "application/json",
                               "{\"error\":\"POST required\"}");
        return;
    }
    if(ctx.req.body.empty()) {
        predator::pwsHttpReply(ctx.sock, 400, "application/json",
                               "{\"error\":\"empty body\"}");
        return;
    }
    nlohmann::json body;
    try { body = nlohmann::json::parse(ctx.req.body); } catch(...) {
        predator::pwsHttpReply(ctx.sock, 400, "application/json",
                               "{\"error\":\"bad json\"}");
        return;
    }
    std::string cls    = body.value("class", "");
    std::string action = body.value("action", "");
    if(cls.rfind("tx",0)==0) {
        predator::pwsHttpReply(ctx.sock, 403, "application/json",
                               "{\"error\":\"tx commands rejected\"}");
        return;
    }
    enqueueCmd(cls, action, body.value("args", nlohmann::json::object()), "web");
    predator::pwsHttpReply(ctx.sock, 200, "application/json", "{\"ok\":true}");
}

// Key management endpoints (read-only from the web side)
static void routeKeyShow(predator::PwsContext& ctx) {
    // Never return the raw key — only confirm it's set and show length
    bool hasKey = !gApiKey.empty();
    nlohmann::json j;
    j["configured"] = hasKey;
    j["length"]     = (int)gApiKey.size();
    predator::pwsHttpReply(ctx.sock, 200, "application/json", j.dump());
}

static void routePortShow(predator::PwsContext& ctx) {
    nlohmann::json j; j["port"] = gPort;
    predator::pwsHttpReply(ctx.sock, 200, "application/json", j.dump());
}

static void routeRoleShow(predator::PwsContext& ctx) {
    std::lock_guard<std::mutex> lk(gCfgMtx);
    nlohmann::json j; j["role"] = gRole;
    predator::pwsHttpReply(ctx.sock, 200, "application/json", j.dump());
}

static void routePeerList(predator::PwsContext& ctx) {
    std::lock_guard<std::mutex> lk(gStateMtx);
    predator::pwsHttpReply(ctx.sock, 200, "application/json", gStateNodes.dump());
}

// ---------------------------------------------------------------------------
// Command dispatch (runs on main/render thread)
// ---------------------------------------------------------------------------

static void applyCommand(const PendingCmd& cmd) {
    flog::info("predator-web: cmd [{}/{}] from {}", cmd.cls, cmd.action, cmd.origin);

    nlohmann::json ev;
    ev["type"]   = "command_applied";
    ev["class"]  = cmd.cls;
    ev["action"] = cmd.action;
    ev["args"]   = cmd.args;
    ev["origin"] = cmd.origin;

    if(cmd.cls == "tune" && cmd.action == "set") {
        double freq = cmd.args.value("freq", 0.0);
        if(freq > 0.0) {
            // Apply to signal path: the VFO manager is the correct entry point.
            // In headless mode the source manager's setFrequency is used directly.
            // This requires the source to be running; silently succeeds otherwise.
            try {
                sigpath::sourceManager.setFrequency(freq);
                {std::lock_guard<std::mutex> lk(gCfgMtx); gCenterFreq = freq;}
                ev["applied"] = true;
            } catch(...) {
                ev["applied"] = false;
                ev["note"] = "tune failed — source may not be running";
            }
        }
    } else if(cmd.cls == "scan") {
        if(cmd.action == "start") {
            std::lock_guard<std::mutex> lk(gCfgMtx);
            gScanRunning = true; gScanStatus = "running";
            ev["applied"] = true;
        } else if(cmd.action == "stop" || cmd.action == "pause") {
            std::lock_guard<std::mutex> lk(gCfgMtx);
            gScanRunning = false;
            gScanStatus = (cmd.action == "pause") ? "paused" : "idle";
            ev["applied"] = true;
        }
    } else if(cmd.cls == "mission" && cmd.action == "set-mode") {
        std::string mode = cmd.args.value("mode", "");
        int modeInt = 0;
        if(mode=="classify")  modeInt=1;
        else if(mode=="scan") modeInt=2;
        else if(mode=="quickscan") modeInt=3;
        {std::lock_guard<std::mutex> lk(gCfgMtx); gMissionMode=modeInt;}
        ev["applied"] = true;
    } else if(cmd.cls == "role" && cmd.action == "set") {
        std::string role = cmd.args.value("role","device");
        {std::lock_guard<std::mutex> lk(gCfgMtx); gRole=role;}
        ev["applied"] = true;
    } else {
        ev["applied"] = false;
        ev["note"] = "unrecognised command — queued for future wire-up";
    }

    pushEvent(ev);
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
                auto s = resp.dump(); ::send(c, s.c_str(), (int)s.size(), 0);
                ::close(c); continue;
            }
            std::string cls    = cmd.value("class", "");
            std::string action = cmd.value("action", "");

            if(cls.rfind("tx",0)==0) {
                resp["ok"] = false; resp["error"] = "tx commands rejected";
            } else if(cls == "query") {
                // Immediate read-only queries are served inline without queueing
                if(action == "identify" || action == "status") {
                    std::lock_guard<std::mutex> lk(gCfgMtx);
                    resp["ok"]          = true;
                    resp["device"]      = gDeviceName.empty()
                                         ? std::string("predator-rfd") : gDeviceName;
                    resp["version"]     = VERSION_STR;
                    resp["role"]        = gRole;
                    resp["sdr_running"] = gSdrRunning;
                    resp["center_freq"] = gCenterFreq;
                    resp["bandwidth"]   = gBandwidth;
                    resp["source"]      = gSourceName;
                    resp["mission_mode"]= gMissionMode;
                    resp["scan_running"]= gScanRunning;
                    resp["scan_status"] = gScanStatus;
                    resp["web_port"]    = gPort;
                } else if(action == "events") {
                    uint64_t since = cmd.value("args",
                        nlohmann::json::object()).value("since", (uint64_t)0);
                    std::lock_guard<std::mutex> lk(gEventMtx);
                    nlohmann::json arr = nlohmann::json::array();
                    for(auto& ev : gEventRing) {
                        if(ev.value("id",(uint64_t)0) > since) arr.push_back(ev);
                    }
                    resp["ok"]     = true;
                    resp["events"] = arr;
                    resp["lastId"] = gLastEventId;
                } else if(action == "key") {
                    resp["ok"]         = true;
                    resp["configured"] = !gApiKey.empty();
                    resp["length"]     = (int)gApiKey.size();
                } else if(action == "port") {
                    resp["ok"] = true; resp["port"] = gPort;
                } else if(action == "role") {
                    std::lock_guard<std::mutex> lk(gCfgMtx);
                    resp["ok"] = true; resp["role"] = gRole;
                } else if(action == "peers") {
                    std::lock_guard<std::mutex> lk(gStateMtx);
                    resp["ok"]    = true;
                    resp["peers"] = gStateNodes;
                } else {
                    resp["ok"] = false; resp["error"] = "unknown query action";
                }
            } else if(cls == "peer") {
                // Peer management requires Kujhad fleet config changes not yet
                // wired; return an honest error rather than silently succeeding.
                resp["ok"]    = false;
                resp["error"] = "peer add/remove not yet implemented via rfctl — "
                                "edit kujhadPeers in config.json and restart";
            } else if(cls == "key" && action == "regenerate") {
                // Key regeneration requires config write + server hot-reload;
                // deferred — return an honest error.
                resp["ok"]    = false;
                resp["error"] = "key regenerate not yet implemented — "
                                "set kujhadApiKey in config.json and restart";
            } else if(cls == "source") {
                // source start/stop needs sigpath::sourceManager wire-up
                // (follow-up task); return an honest error for now.
                resp["ok"]    = false;
                resp["error"] = "source start/stop not yet wired to sigpath; "
                                "start/stop the daemon process instead";
            } else {
                enqueueCmd(cls, action,
                           cmd.value("args", nlohmann::json::object()), "rfctl");
                resp["ok"]     = true;
                resp["status"] = "queued";
            }
            auto s = resp.dump(); ::send(c, s.c_str(), (int)s.size(), 0);
        }
        ::close(c);
    }
}

static bool startCtrlSocket(const std::string& path) {
    // Ensure parent directory exists
    auto slash = path.rfind('/');
    if(slash != std::string::npos) {
        std::string dir = path.substr(0, slash);
        ::mkdir(dir.c_str(), 0750);
    }
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
    gCtrlPath = path;
    gCtrlThread = std::thread(ctrlSocketLoop);
    return true;
}
#endif

// ---------------------------------------------------------------------------
// Periodic spectrum push to WebSocket clients
// ---------------------------------------------------------------------------
static void spectrumPushLoop() {
    uint64_t lastPushed = 0;
    while(gRunning) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        uint64_t serial;
        { std::lock_guard<std::mutex> lk(gSpecMtx); serial = gSpecSerial; }
        if(serial == lastPushed || gSpecBins.empty()) continue;
        lastPushed = serial;
        nlohmann::json j;
        j["type"] = "spectrum";
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

    if(core::configManager.conf.contains("webBackendPort"))
        gPort = core::configManager.conf["webBackendPort"].get<int>();

    if(core::configManager.conf.contains("deviceName"))
        gDeviceName = core::configManager.conf["deviceName"].get<std::string>();

    // API key: loaded from config (same key as Kujhad fleet)
    if(core::configManager.conf.contains("kujhadApiKey"))
        gApiKey = core::configManager.conf["kujhadApiKey"].get<std::string>();

    // Bind all interfaces only if explicitly enabled (loopback is the safe default)
    bool bindAll = false;
    if(core::configManager.conf.contains("webBindAll"))
        bindAll = core::configManager.conf["webBindAll"].get<bool>();

    const char* envRoot = ::getenv("PREDATOR_WEB_ROOT");
    if(envRoot && *envRoot) gWebRoot = envRoot;
    else if(core::configManager.conf.contains("webRoot"))
        gWebRoot = core::configManager.conf["webRoot"].get<std::string>();
    else if(!resDir.empty()) gWebRoot = resDir + "/web";

    std::string ctrlPath = "/run/predator-rfd/control.sock";
    if(core::configManager.conf.contains("webCtrlSocket"))
        ctrlPath = core::configManager.conf["webCtrlSocket"].get<std::string>();
    const char* envCtrl = ::getenv("PREDATOR_CTRL_SOCK");
    if(envCtrl && *envCtrl) ctrlPath = envCtrl;

    core::configManager.release();

    // Register routes — both /api/v1/* (frontend path) and short aliases
    auto addBoth = [&](const std::string& method, const std::string& shortPath,
                       predator::PwsHandler h) {
        gServer.addRoute(method, shortPath, h);
        gServer.addRoute(method, "/api/v1" + shortPath, h);
    };

    gServer.setStaticRoot(gWebRoot);
    if(!gApiKey.empty()) gServer.setApiKey(gApiKey);
    if(bindAll) gServer.setBindAll(true);

    addBoth("GET",  "/identify",      routeIdentify);
    addBoth("GET",  "/status",        routeStatus);
    addBoth("GET",  "/state",         routeState);
    addBoth("GET",  "/nodes/",        routeNodes);
    addBoth("GET",  "/tracks/",       routeTracks);
    addBoth("GET",  "/spectrum",      routeSpectrum);
    addBoth("GET",  "/events",        routeEvents);
    addBoth("POST", "/command",       routeCommand);
    addBoth("GET",  "/key",           routeKeyShow);
    addBoth("GET",  "/port",          routePortShow);
    addBoth("GET",  "/role",          routeRoleShow);
    addBoth("GET",  "/peers/",        routePeerList);

    // /events/stream — served as SSE (the browser sends Accept: text/event-stream)
    // The SSE upgrade is handled inside PredatorWebServer::handleSse() which
    // fires for any request whose Accept header contains text/event-stream.
    // Register the path so it doesn't 404 on a normal GET from curl.
    addBoth("GET", "/events/stream", [](predator::PwsContext& ctx) {
        predator::pwsHttpReply(ctx.sock, 200, "text/plain",
                               "Use EventSource or Accept: text/event-stream");
    });

    if(!gServer.start(gPort)) {
        flog::error("Predator web backend: failed to bind port {}", gPort);
        return 1;
    }
    flog::info("Predator web backend: HTTP+WS on {}:{}", bindAll ? "0.0.0.0" : "127.0.0.1", gPort);
    if(!gWebRoot.empty())
        flog::info("Predator web backend: static root '{}'", gWebRoot);
    if(!gApiKey.empty())
        flog::info("Predator web backend: API key auth enabled ({} chars)", gApiKey.size());
    else
        flog::warn("Predator web backend: no API key configured — endpoints unprotected (loopback only)");

    gRunning = true;

#ifndef _WIN32
    if(!startCtrlSocket(ctrlPath))
        flog::warn("Predator web backend: control socket unavailable at '{}'", ctrlPath);
    else
        flog::info("Predator web backend: control socket at '{}'", ctrlPath);
#endif

    std::thread(spectrumPushLoop).detach();
    return 0;
}

void beginFrame() {}

void render(bool /*vsync*/) {}

void getMouseScreenPos(double& x, double& y) { x = 0.0; y = 0.0; }
void setMouseScreenPos(double /*x*/, double /*y*/) {}

bool getPhoneLocation(double& lat, double& lon, float& accuracy, bool& hasFix) {
    lat = 0.0; lon = 0.0; accuracy = 0.0f; hasFix = false;
    return false;
}

bool openMapView() { return false; }
float getNativeUiScale() { return 1.0f; }
bool isTouchPrimary() { return false; }
int getImeBottomInset() { return 0; }
SafeAreaInsets getSafeAreaInsets() { return {}; }

int renderLoop() {
    flog::info("Predator web backend: headless loop running");
    while(gRunning) {
        // Drain the command queue on the main thread
        {
            std::queue<PendingCmd> local;
            { std::lock_guard<std::mutex> lk(gCmdMtx); std::swap(local, gCmdQueue); }
            while(!local.empty()) {
                applyCommand(local.front());
                local.pop();
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return 0;
}

int end() {
    gRunning = false;
    gServer.stop();
#ifndef _WIN32
    if(gCtrlSock >= 0) { ::close(gCtrlSock); gCtrlSock = -1; }
    if(gCtrlThread.joinable()) gCtrlThread.join();
    if(!gCtrlPath.empty()) ::unlink(gCtrlPath.c_str());
#endif
    flog::info("Predator web backend: stopped");
    return 0;
}

// ---------------------------------------------------------------------------
// Hooks called from signal path to feed live data
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

void webBackendPushEvent(const nlohmann::json& ev) { pushEvent(ev); }

void webBackendUpdateNodes(const nlohmann::json& nodes) {
    std::lock_guard<std::mutex> lk(gStateMtx);
    gStateNodes = nodes;
}

// webBackendUpdateTracks — replace the cached track list served at GET /tracks/.
//
// `tracks` is a JSON array of track objects.  Each element must include the
// full Python-parity schema (EmitterTrack.to_dict()), which includes the
// following LOB / KrakenSDR fields (null when no KrakenSDR data has arrived):
//
//   "lob_bearing_deg"          float | null   most-confident bearing (true-N)
//   "lob_bearing_uncert_deg"   float | null   1-sigma uncertainty in degrees
//   "lob_confidence"           float | null   0–1 bearing confidence
//   "lob_node_ids"             array<string>  contributing KrakenSDR node IDs
//   "_lob_node_lat"            float | null   anchor lat for map wedge
//   "_lob_node_lon"            float | null   anchor lon for map wedge
//   "lob_crosscut_lat"         float | null   crosscut fix latitude
//   "lob_crosscut_lon"         float | null   crosscut fix longitude
//   "lob_crosscut_radius_m"    float | null   1-sigma error circle radius
//   "lob_crosscut_confidence"  float | null   0–1 crosscut confidence
//
// Callers that construct track JSON from C++ structs MUST emit all of the
// above keys (null-initialised when absent) to maintain Python↔C++ parity.
// The routeTracks handler and the SSE /events/stream share gStateTracks, so
// any missing key will silently drop from the dashboard.
void webBackendUpdateTracks(const nlohmann::json& tracks) {
    std::lock_guard<std::mutex> lk(gStateMtx);
    gStateTracks = tracks;
}

void webBackendUpdateStatus(bool sdrRunning, double centerHz, double bwHz,
                             const std::string& sourceName, int missionMode,
                             bool scanRunning, const std::string& scanStatus) {
    std::lock_guard<std::mutex> lk(gCfgMtx);
    gSdrRunning  = sdrRunning;
    gCenterFreq  = centerHz;
    gBandwidth   = bwHz;
    gSourceName  = sourceName;
    gMissionMode = missionMode;
    gScanRunning = scanRunning;
    gScanStatus  = scanStatus;
}

} // namespace backend
