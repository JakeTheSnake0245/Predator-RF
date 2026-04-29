#pragma once

// Kujhad Fleet Console — hub-and-spoke peer protocol.
//
// Each Predator RF instance can run as a Device (publishes its own SDR
// state, GPS, and event stream over a small embedded HTTP+JSON server)
// or as a Controller (connects to one or more Devices, mirrors their
// state into the local UI, and issues typed commands).
//
// Wire protocol for v1: HTTP/1.1 + JSON, single API key in the
// `X-Kujhad-Key` header on every request. Endpoints exposed by a
// device:
//
//   GET  /v1/identify      → {device, version, role, hwProfile}
//   GET  /v1/gps           → {hasFix, lat, lon, accuracy}
//   GET  /v1/state         → {vfos, markers, mission, decoders, hits}
//   GET  /v1/events?since= → {events: [...], lastId}
//   POST /v1/command       → {ok, error?}  body: {class, action, ...}
//
// Every endpoint returns 401 when the API key header is missing or
// wrong, and 400 / 404 / 405 for malformed / unknown / wrong-method.
//
// Command schema is typed by `class` so a future `tx.*` class can be
// added behind an explicit per-device permission gate later, without
// reshaping the protocol. v1 honours: `tune`, `scan`, `mission`,
// `identify`. Anything in the `tx` class is rejected.
//
// TLS note: v1 ships plaintext over loopback / VPN (ZeroTier or
// Tailscale). The socket layer below is connection-typed so a future
// release can swap the raw socket read/write for an OpenSSL BIO pair
// without touching the protocol or auth code. Operators who need
// transport encryption today should run the listener behind stunnel /
// nginx / a VPN.
//
// Safety boundary: receive, observe, command (RX-only). Any inbound
// command in the `tx` class is rejected. The whole module never opens
// a transmit path.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <queue>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "../json.hpp"

#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #include <iphlpapi.h>
  #pragma comment(lib, "Ws2_32.lib")
  #pragma comment(lib, "Iphlpapi.lib")
  using kujhad_socket_t = SOCKET;
  #define KUJHAD_INVALID_SOCK INVALID_SOCKET
  #define KUJHAD_CLOSESOCK closesocket
  #define KUJHAD_LAST_ERR WSAGetLastError()
#else
  #include <sys/socket.h>
  #include <sys/types.h>
  #include <sys/select.h>
  #include <netinet/in.h>
  #include <netinet/tcp.h>
  #include <arpa/inet.h>
  #include <netdb.h>
  #include <ifaddrs.h>
  #include <unistd.h>
  #include <fcntl.h>
  #include <errno.h>
  using kujhad_socket_t = int;
  #define KUJHAD_INVALID_SOCK (-1)
  #define KUJHAD_CLOSESOCK ::close
  #define KUJHAD_LAST_ERR errno
#endif

namespace predator {

using kujhad_json = nlohmann::json;

struct KujhadInterfaceCandidate {
    std::string name;
    std::string address;
    bool isLoopback = false;
    bool isZerotier = false;
    bool isTailscale = false;
    bool isPrivate = false;
    int score = 0; // higher = more preferred
};

inline std::vector<KujhadInterfaceCandidate> kujhadEnumerateInterfaces() {
    std::vector<KujhadInterfaceCandidate> out;
#ifndef _WIN32
    ifaddrs* head = nullptr;
    if (::getifaddrs(&head) != 0 || !head) return out;
    for (ifaddrs* it = head; it != nullptr; it = it->ifa_next) {
        if (!it->ifa_addr || it->ifa_addr->sa_family != AF_INET) continue;
        char buf[INET_ADDRSTRLEN] = {0};
        sockaddr_in* sa = (sockaddr_in*)it->ifa_addr;
        inet_ntop(AF_INET, &sa->sin_addr, buf, sizeof(buf));
        KujhadInterfaceCandidate c;
        c.name = it->ifa_name ? it->ifa_name : "";
        c.address = buf;
        c.isLoopback = (c.address.rfind("127.", 0) == 0);
        // ZeroTier interfaces are typically named "zt..." with /24 inside
        // managed networks; Tailscale interfaces are typically "tailscale0"
        // or "utun*" on macOS with 100.64.0.0/10 CGNAT addresses.
        c.isZerotier  = (c.name.rfind("zt", 0) == 0);
        c.isTailscale = (c.name.find("tailscale") != std::string::npos) ||
                        (c.address.rfind("100.", 0) == 0);
        // RFC1918 private space is what we treat as "LAN"
        c.isPrivate = (c.address.rfind("10.", 0) == 0) ||
                      (c.address.rfind("192.168.", 0) == 0) ||
                      (c.address.rfind("172.", 0) == 0);
        if (c.isLoopback) continue;
        c.score = 0;
        if (c.isZerotier)   c.score += 100;
        if (c.isTailscale)  c.score += 90;
        if (c.isPrivate)    c.score += 10;
        out.push_back(c);
    }
    if (head) ::freeifaddrs(head);
    std::sort(out.begin(), out.end(), [](const KujhadInterfaceCandidate& a,
                                          const KujhadInterfaceCandidate& b) {
        return a.score > b.score;
    });
#endif
    return out;
}

// Pick the best interface address for a Device to publish to peers.
// Prefers ZeroTier/Tailscale when present; falls back to the first
// RFC1918 LAN address; falls back to 127.0.0.1 when offline.
inline std::string kujhadSuggestedListenAddress() {
    auto cands = kujhadEnumerateInterfaces();
    if (!cands.empty()) return cands.front().address;
    return "127.0.0.1";
}

// Cryptographically-light random API key. Not a secret-strength PRNG
// but good enough for a per-device shared secret on a private overlay
// network; operators can regenerate at any time. 32 hex chars.
inline std::string kujhadGenerateApiKey() {
    static const char hex[] = "0123456789abcdef";
    std::random_device rd;
    std::mt19937_64 mt((uint64_t)rd() ^ ((uint64_t)std::chrono::steady_clock::now().time_since_epoch().count()));
    std::string out(32, '0');
    for (int i = 0; i < 32; i++) out[i] = hex[mt() & 0xF];
    return out;
}

// ---------------------------------------------------------------------------
// Tiny HTTP/1.1 helpers shared by both the device server and controller
// client. Synchronous, blocking, single-threaded-per-connection. The whole
// fleet protocol is small JSON request/response so a full async stack
// would be overkill.
// ---------------------------------------------------------------------------

struct KujhadHttpRequest {
    std::string method;
    std::string path;        // includes any query string
    std::map<std::string, std::string> headers; // lower-cased keys
    std::string body;
};

struct KujhadHttpResponse {
    int status = 200;
    std::string contentType = "application/json";
    std::string body;
};

inline std::string kujhadToLower(std::string s) {
    for (char& c : s) { if (c >= 'A' && c <= 'Z') c = (char)(c + 32); }
    return s;
}

inline bool kujhadReadAll(kujhad_socket_t sock, char* buf, int n) {
    int got = 0;
    while (got < n) {
        int r = (int)::recv(sock, buf + got, n - got, 0);
        if (r <= 0) return false;
        got += r;
    }
    return true;
}

inline bool kujhadWriteAll(kujhad_socket_t sock, const char* buf, int n) {
    int sent = 0;
    while (sent < n) {
        int r = (int)::send(sock, buf + sent, n - sent, 0);
        if (r <= 0) return false;
        sent += r;
    }
    return true;
}

inline bool kujhadParseRequest(kujhad_socket_t sock, KujhadHttpRequest& req,
                                int maxBytes = 1 << 20 /* 1 MiB */) {
    std::string buf;
    buf.reserve(2048);
    char ch;
    // Read until \r\n\r\n header terminator.
    while ((int)buf.size() < maxBytes) {
        int r = (int)::recv(sock, &ch, 1, 0);
        if (r <= 0) return false;
        buf.push_back(ch);
        if (buf.size() >= 4 && buf.compare(buf.size() - 4, 4, "\r\n\r\n") == 0) break;
    }
    size_t headerEnd = buf.size();
    // Parse the request line.
    size_t lineEnd = buf.find("\r\n");
    if (lineEnd == std::string::npos) return false;
    std::string reqLine = buf.substr(0, lineEnd);
    size_t sp1 = reqLine.find(' ');
    size_t sp2 = (sp1 == std::string::npos) ? std::string::npos : reqLine.find(' ', sp1 + 1);
    if (sp1 == std::string::npos || sp2 == std::string::npos) return false;
    req.method = reqLine.substr(0, sp1);
    req.path   = reqLine.substr(sp1 + 1, sp2 - sp1 - 1);
    // Parse headers.
    size_t pos = lineEnd + 2;
    while (pos < headerEnd - 4) {
        size_t eol = buf.find("\r\n", pos);
        if (eol == std::string::npos || eol > headerEnd - 4) break;
        std::string line = buf.substr(pos, eol - pos);
        size_t colon = line.find(':');
        if (colon != std::string::npos) {
            std::string key = kujhadToLower(line.substr(0, colon));
            size_t v = colon + 1;
            while (v < line.size() && (line[v] == ' ' || line[v] == '\t')) v++;
            req.headers[key] = line.substr(v);
        }
        pos = eol + 2;
    }
    // Read content body if Content-Length present.
    auto cl = req.headers.find("content-length");
    if (cl != req.headers.end()) {
        int n = std::atoi(cl->second.c_str());
        if (n < 0 || n > maxBytes) return false;
        req.body.resize(n);
        if (n > 0 && !kujhadReadAll(sock, &req.body[0], n)) return false;
    }
    return true;
}

inline bool kujhadSendResponse(kujhad_socket_t sock, const KujhadHttpResponse& res) {
    const char* statusText = "OK";
    switch (res.status) {
        case 200: statusText = "OK"; break;
        case 204: statusText = "No Content"; break;
        case 400: statusText = "Bad Request"; break;
        case 401: statusText = "Unauthorized"; break;
        case 403: statusText = "Forbidden"; break;
        case 404: statusText = "Not Found"; break;
        case 405: statusText = "Method Not Allowed"; break;
        case 500: statusText = "Internal Server Error"; break;
        default:  statusText = "OK"; break;
    }
    char header[512];
    int n = snprintf(header, sizeof(header),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "\r\n",
        res.status, statusText, res.contentType.c_str(), (int)res.body.size());
    if (!kujhadWriteAll(sock, header, n)) return false;
    if (!res.body.empty() && !kujhadWriteAll(sock, res.body.data(), (int)res.body.size())) return false;
    return true;
}

// ---------------------------------------------------------------------------
// Operator console: a tiny self-contained single-page HTML application
// served on GET /. Loaded by a browser, it prompts for the API key and
// then polls /v1/identify, /v1/state, /v1/gps, /v1/events and lets the
// operator issue typed commands. This is the Linux web GUI surface in
// v1; native ImGui keeps running in parallel for full SDR control.
// ---------------------------------------------------------------------------

inline std::string kujhadWebConsoleHtml() {
    return std::string(
"<!doctype html><html lang=en><head><meta charset=utf-8>"
"<title>Predator RF — Kujhad Console</title>"
"<meta name=viewport content='width=device-width,initial-scale=1'>"
"<style>"
"body{background:#05080a;color:#c8d8e0;font-family:'JetBrains Mono',Consolas,monospace;font-size:13px;margin:0;padding:14px}"
"h1{color:#3fd17d;letter-spacing:.18em;font-size:13px;margin:0 0 12px;text-transform:uppercase}"
"h2{color:#4ad8e8;font-size:12px;text-transform:uppercase;letter-spacing:.1em;margin:18px 0 6px;border-bottom:1px solid #1f3540;padding-bottom:3px}"
"input,button,select{background:#0f171c;color:#c8d8e0;border:1px solid #2a4a5a;padding:5px 8px;font-family:inherit;font-size:12px}"
"button{cursor:pointer;color:#3fd17d}"
"button:hover{border-color:#3fd17d}"
".kv{display:grid;grid-template-columns:160px 1fr;gap:4px 12px;margin:6px 0}"
".kv label{color:#6a8090}"
"table{width:100%;border-collapse:collapse;font-size:12px}"
"th,td{padding:3px 6px;text-align:left;border-bottom:1px solid #16222a}"
"th{color:#6a8090;font-weight:400;text-transform:uppercase;font-size:11px}"
".pill{display:inline-block;padding:1px 6px;border:1px solid #1f3540;color:#6a8090;font-size:11px;text-transform:uppercase;letter-spacing:.1em}"
".pill.ok{color:#3fd17d;border-color:#1b6a3d}"
".pill.bad{color:#ff5040;border-color:#7a2020}"
".panel{background:#0a1014;border:1px solid #1f3540;padding:10px 14px;margin:0 0 12px;border-radius:2px}"
".bar{display:flex;gap:8px;flex-wrap:wrap;align-items:center}"
"</style></head><body>"
"<h1>Predator RF — Kujhad Operator Console</h1>"
"<div class=panel><div class=bar>"
"<label>API Key:</label>"
"<input id=k type=password style='flex:1;min-width:200px' placeholder='X-Kujhad-Key'>"
"<button onclick=connect()>Connect</button>"
"<span id=status class=pill>disconnected</span>"
"</div></div>"
"<div id=info class=panel><h2>Identify</h2><div class=kv id=identify><label>(connect to load)</label><span></span></div></div>"
"<div class=panel><h2>State</h2><div class=kv id=state><label>(no data)</label><span></span></div></div>"
"<div class=panel><h2>GPS</h2><div class=kv id=gps><label>(no data)</label><span></span></div></div>"
"<div class=panel><h2>Commands</h2><div class=bar>"
"<label>Tune Hz:</label>"
"<input id=cmdFreq type=number value=433920000 style='width:140px'>"
"<button onclick=cmd('tune','set',{frequencyHz:Number(document.getElementById(\"cmdFreq\").value)})>Send tune.set</button>"
"<button onclick=cmd('identify','ping',{})>Identify</button>"
"<button onclick=cmd('scan','start',{})>Scan start</button>"
"<button onclick=cmd('scan','stop',{})>Scan stop</button>"
"</div><div id=cmdResult style='margin-top:8px;color:#6a8090'></div></div>"
"<div class=panel><h2>Events (recent)</h2><table id=evTable><thead>"
"<tr><th>Time</th><th>Source Device</th><th>Type</th><th>Frequency</th><th>Label</th><th>Strength</th></tr>"
"</thead><tbody id=evBody></tbody></table></div>"
"<script>"
"let key='';let pollTimer=null;"
"function $(i){return document.getElementById(i)}"
"function connect(){key=$('k').value.trim();if(!key){$('status').textContent='no key';return}"
"$('status').textContent='connecting';$('status').className='pill';"
"if(pollTimer)clearInterval(pollTimer);"
"pollAll();pollTimer=setInterval(pollAll,1500)}"
"async function api(path,opts){opts=opts||{};opts.headers=Object.assign({'X-Kujhad-Key':key,'Content-Type':'application/json'},opts.headers||{});"
"const r=await fetch(path,opts);if(!r.ok)throw new Error('HTTP '+r.status);return r.json()}"
"function setKv(id,obj){const e=$(id);e.innerHTML='';if(!obj){e.innerHTML='<label>(no data)</label><span></span>';return}"
"for(const k of Object.keys(obj)){const l=document.createElement('label');l.textContent=k;const v=document.createElement('span');"
"v.textContent=typeof obj[k]==='object'?JSON.stringify(obj[k]):String(obj[k]);e.appendChild(l);e.appendChild(v)}}"
"async function pollAll(){try{const id=await api('/v1/identify');setKv('identify',id);"
"$('status').textContent='connected';$('status').className='pill ok';"
"const st=await api('/v1/state');setKv('state',st);"
"const gp=await api('/v1/gps');setKv('gps',gp);"
"const ev=await api('/v1/events?since=0');"
"const tb=$('evBody');tb.innerHTML='';"
"for(const e of (ev.events||[]).slice().reverse()){const tr=document.createElement('tr');"
"tr.innerHTML='<td>'+(e.time||'')+'</td><td>'+(e.sourceDevice||'local')+'</td><td>'+(e.type||'')+'</td>"
"<td>'+(e.frequency!=null?Number(e.frequency).toFixed(0):'')+'</td>"
"<td>'+(e.label||'')+'</td><td>'+(e.strengthDb!=null?Number(e.strengthDb).toFixed(1)+' dB':'')+'</td>';tb.appendChild(tr)}}"
"catch(err){$('status').textContent='error: '+err.message;$('status').className='pill bad'}}"
"async function cmd(c,a,args){try{const r=await api('/v1/command',{method:'POST',body:JSON.stringify({class:c,action:a,args:args})});"
"$('cmdResult').textContent=c+'.'+a+': '+JSON.stringify(r)}catch(err){$('cmdResult').textContent='error: '+err.message}}"
"</script></body></html>");
}

// ---------------------------------------------------------------------------
// Device-side server. Owns a listener thread + a small worker thread per
// inbound connection. State and command callbacks are wired by the UI.
// ---------------------------------------------------------------------------

struct KujhadDeviceCommand {
    std::string commandClass; // "tune", "scan", "mission", "identify"
    std::string action;
    kujhad_json args;
};

class KujhadDeviceServer {
public:
    using CommandHandler = std::function<bool(const KujhadDeviceCommand&, std::string& errOut)>;
    using SnapshotProvider = std::function<kujhad_json()>;

    ~KujhadDeviceServer() { stop(); }

    // Wire the snapshot providers BEFORE start(). They are read from the
    // server thread on every request and must be cheap + thread-safe.
    void setIdentifyProvider(SnapshotProvider fn) { identifyProvider_ = std::move(fn); }
    void setStateProvider(SnapshotProvider fn)    { stateProvider_    = std::move(fn); }
    void setGpsProvider(SnapshotProvider fn)      { gpsProvider_      = std::move(fn); }
    void setEventsProvider(std::function<kujhad_json(uint64_t since)> fn) { eventsProvider_ = std::move(fn); }
    void setCommandHandler(CommandHandler fn)     { commandHandler_   = std::move(fn); }

    void setApiKey(const std::string& key) {
        std::lock_guard<std::mutex> lk(mtx_);
        apiKey_ = key;
    }

    bool start(int port, const std::string& apiKey) {
        stop();
        port_ = port;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            apiKey_ = apiKey;
        }
        stopFlag_ = false;
        running_ = true;
        listenerOk_ = false;
        statusMsg_ = "Starting...";
        worker_ = std::thread([this]() { listenerLoop(); });
        // Give the listener a moment to bind so callers can read status.
        for (int i = 0; i < 20 && running_.load() && !listenerOk_.load() && statusMsg_.find("failed") == std::string::npos; i++) {
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }
        return listenerOk_.load();
    }

    void stop() {
        if (!running_.load()) {
            if (worker_.joinable()) worker_.join();
            return;
        }
        stopFlag_ = true;
        running_ = false;
        // Poke the listener by connecting to ourselves so accept() returns.
        kujhad_socket_t poke = ::socket(AF_INET, SOCK_STREAM, 0);
        if (poke != KUJHAD_INVALID_SOCK) {
            sockaddr_in a{};
            a.sin_family = AF_INET;
            a.sin_port = htons((uint16_t)port_);
            inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
            ::connect(poke, (sockaddr*)&a, sizeof(a));
            KUJHAD_CLOSESOCK(poke);
        }
        if (worker_.joinable()) worker_.join();
        listenerOk_ = false;
        statusMsg_ = "Stopped";
    }

    bool isRunning() const  { return running_.load(); }
    bool isListening() const { return listenerOk_.load(); }
    int  port() const       { return port_; }
    int  inboundRequests() const { return inboundRequests_.load(); }
    int  inboundCommands() const { return inboundCommands_.load(); }
    int  rejectedCommands() const { return rejectedCommands_.load(); }

    std::string status() const {
        std::lock_guard<std::mutex> lk(statusMtx_);
        return statusMsg_;
    }

private:
    void setStatus(const std::string& s) {
        std::lock_guard<std::mutex> lk(statusMtx_);
        statusMsg_ = s;
    }

    void listenerLoop() {
#ifdef _WIN32
        WSADATA wsa; WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
        kujhad_socket_t srv = ::socket(AF_INET, SOCK_STREAM, 0);
        if (srv == KUJHAD_INVALID_SOCK) { setStatus("socket() failed"); running_ = false; return; }
        int reuse = 1;
        ::setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        addr.sin_port = htons((uint16_t)port_);
        if (::bind(srv, (sockaddr*)&addr, sizeof(addr)) != 0) {
            setStatus(std::string("bind failed on :") + std::to_string(port_));
            KUJHAD_CLOSESOCK(srv);
            running_ = false;
            return;
        }
        if (::listen(srv, 8) != 0) {
            setStatus("listen failed");
            KUJHAD_CLOSESOCK(srv);
            running_ = false;
            return;
        }
        listenerOk_ = true;
        setStatus(std::string("Listening :") + std::to_string(port_));

        while (!stopFlag_.load()) {
            sockaddr_in client{};
            socklen_t cl = sizeof(client);
            kujhad_socket_t conn = ::accept(srv, (sockaddr*)&client, &cl);
            if (stopFlag_.load()) {
                if (conn != KUJHAD_INVALID_SOCK) KUJHAD_CLOSESOCK(conn);
                break;
            }
            if (conn == KUJHAD_INVALID_SOCK) continue;
            // Detached worker — small per-connection thread is fine; the
            // protocol is request/response and clients close immediately.
            std::thread([this, conn]() { handleConnection(conn); }).detach();
        }
        KUJHAD_CLOSESOCK(srv);
        listenerOk_ = false;
#ifdef _WIN32
        WSACleanup();
#endif
    }

    void handleConnection(kujhad_socket_t conn) {
        // Reasonable receive timeout so an idle peer doesn't pin the worker.
#ifndef _WIN32
        timeval tv{}; tv.tv_sec = 10; tv.tv_usec = 0;
        ::setsockopt(conn, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
        KujhadHttpRequest req;
        bool ok = kujhadParseRequest(conn, req);
        inboundRequests_++;
        KujhadHttpResponse res;
        if (!ok) {
            res.status = 400;
            res.body = "{\"error\":\"bad request\"}";
            kujhadSendResponse(conn, res);
            KUJHAD_CLOSESOCK(conn);
            return;
        }
        // CORS preflight. Always answer; auth check skipped on OPTIONS.
        if (req.method == "OPTIONS") {
            res.status = 204;
            res.contentType = "text/plain";
            res.body.clear();
            kujhadSendResponse(conn, res);
            KUJHAD_CLOSESOCK(conn);
            return;
        }
        // Parse path / query first so we can route the unauthenticated
        // operator console (GET /) before the API-key check. The console
        // page itself loads without a key; every /v1/* call it makes
        // still carries X-Kujhad-Key. This prevents browsers from
        // hitting a 401 on the very first GET.
        std::string path = req.path;
        size_t qm = path.find('?');
        std::string query;
        if (qm != std::string::npos) { query = path.substr(qm + 1); path = path.substr(0, qm); }

        // Operator console HTML — public route, no auth required to
        // load the page so the operator can paste their key into the
        // form. Limited strictly to GET / and GET /index.html.
        if (req.method == "GET" && (path == "/" || path == "/index.html")) {
            res.contentType = "text/html; charset=utf-8";
            res.body = kujhadWebConsoleHtml();
            kujhadSendResponse(conn, res);
            KUJHAD_CLOSESOCK(conn);
            return;
        }

        // API-key auth gate for everything under /v1/*. Header name is
        // X-Kujhad-Key (case-insensitive). Anything that is not the
        // public console must present the key.
        std::string expected;
        { std::lock_guard<std::mutex> lk(mtx_); expected = apiKey_; }
        auto kit = req.headers.find("x-kujhad-key");
        if (expected.empty() || kit == req.headers.end() || kit->second != expected) {
            res.status = 401;
            res.body = "{\"error\":\"unauthorized\"}";
            kujhadSendResponse(conn, res);
            KUJHAD_CLOSESOCK(conn);
            return;
        }

        if (req.method == "GET" && path == "/v1/identify") {
            kujhad_json j = identifyProvider_ ? identifyProvider_() : kujhad_json::object();
            res.body = j.dump();
        }
        else if (req.method == "GET" && path == "/v1/state") {
            kujhad_json j = stateProvider_ ? stateProvider_() : kujhad_json::object();
            res.body = j.dump();
        }
        else if (req.method == "GET" && path == "/v1/gps") {
            kujhad_json j = gpsProvider_ ? gpsProvider_() : kujhad_json::object();
            res.body = j.dump();
        }
        else if (req.method == "GET" && path == "/v1/events") {
            uint64_t since = 0;
            // crude query parser: since=NNN
            size_t s = query.find("since=");
            if (s != std::string::npos) {
                since = (uint64_t)std::strtoull(query.c_str() + s + 6, nullptr, 10);
            }
            kujhad_json j = eventsProvider_ ? eventsProvider_(since) : kujhad_json::object();
            res.body = j.dump();
        }
        else if (req.method == "POST" && path == "/v1/command") {
            kujhad_json body;
            try { body = kujhad_json::parse(req.body.empty() ? std::string("{}") : req.body); }
            catch (...) { body = kujhad_json::object(); }
            KujhadDeviceCommand cmd;
            cmd.commandClass = body.value("class", "");
            cmd.action       = body.value("action", "");
            cmd.args         = body.value("args", kujhad_json::object());
            // Hard RX-only gate: any tx.* command class is rejected at the
            // protocol level. The CommandHandler downstream is never asked.
            if (cmd.commandClass == "tx" || cmd.commandClass.rfind("tx.", 0) == 0) {
                rejectedCommands_++;
                res.status = 403;
                res.body = "{\"ok\":false,\"error\":\"tx commands disabled (RX-only build)\"}";
            }
            else if (cmd.commandClass != "tune" && cmd.commandClass != "scan" &&
                     cmd.commandClass != "mission" && cmd.commandClass != "identify") {
                res.status = 400;
                res.body = "{\"ok\":false,\"error\":\"unknown command class\"}";
            }
            else if (!commandHandler_) {
                res.status = 500;
                res.body = "{\"ok\":false,\"error\":\"no handler bound\"}";
            }
            else {
                std::string errOut;
                bool okCmd = commandHandler_(cmd, errOut);
                inboundCommands_++;
                if (!okCmd) rejectedCommands_++;
                kujhad_json resj;
                resj["ok"] = okCmd;
                if (!okCmd) resj["error"] = errOut.empty() ? std::string("rejected") : errOut;
                res.body = resj.dump();
            }
        }
        else {
            res.status = 404;
            res.body = "{\"error\":\"not found\"}";
        }
        kujhadSendResponse(conn, res);
        KUJHAD_CLOSESOCK(conn);
    }

    int port_ = 0;
    std::atomic<bool> running_{false};
    std::atomic<bool> stopFlag_{false};
    std::atomic<bool> listenerOk_{false};
    std::atomic<int>  inboundRequests_{0};
    std::atomic<int>  inboundCommands_{0};
    std::atomic<int>  rejectedCommands_{0};

    std::thread worker_;
    std::mutex mtx_;
    std::string apiKey_;

    mutable std::mutex statusMtx_;
    std::string statusMsg_ = "Idle";

    SnapshotProvider identifyProvider_;
    SnapshotProvider stateProvider_;
    SnapshotProvider gpsProvider_;
    std::function<kujhad_json(uint64_t)> eventsProvider_;
    CommandHandler commandHandler_;
};

// ---------------------------------------------------------------------------
// Controller-side client. One worker per peer. Polls identify+state+gps
// once per second and events on a tighter interval. Drained snapshots
// land in a thread-safe shared structure that the UI thread reads.
// ---------------------------------------------------------------------------

struct KujhadPeerSnapshot {
    bool reachable = false;
    std::string lastError;
    uint64_t lastSyncMs = 0;
    int linkLatencyMs = -1;
    kujhad_json identify;
    kujhad_json state;
    kujhad_json gps;
};

class KujhadControllerClient {
public:
    ~KujhadControllerClient() { stop(); }

    void start(const std::string& host, int port, const std::string& apiKey) {
        stop();
        host_ = host;
        port_ = port;
        apiKey_ = apiKey;
        stopFlag_ = false;
        running_ = true;
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
    }

    bool isRunning() const { return running_.load(); }

    KujhadPeerSnapshot snapshot() const {
        std::lock_guard<std::mutex> lk(snapMtx_);
        return snap_;
    }

    // Drain new events received since the last drain. Each event is
    // returned as the raw JSON object the device emitted; the caller is
    // responsible for tagging it with the source-device name.
    std::vector<kujhad_json> drainEvents(size_t max = 256) {
        std::vector<kujhad_json> out;
        std::lock_guard<std::mutex> lk(eventMtx_);
        while (!events_.empty() && out.size() < max) {
            out.push_back(std::move(events_.front()));
            events_.pop();
        }
        return out;
    }

    // Send a typed command. Synchronous, blocking — meant for UI-thread
    // calls in response to operator action. Returns ok + error message.
    bool sendCommand(const std::string& commandClass, const std::string& action,
                     const kujhad_json& args, std::string& errOut) {
        kujhad_json body;
        body["class"]  = commandClass;
        body["action"] = action;
        body["args"]   = args;
        std::string serialized = body.dump();
        KujhadHttpResponse res;
        if (!doRequest("POST", "/v1/command", serialized, res, 5000)) {
            errOut = "request failed";
            return false;
        }
        try {
            kujhad_json j = kujhad_json::parse(res.body);
            bool ok = j.value("ok", false) && (res.status >= 200 && res.status < 300);
            if (!ok) errOut = j.value("error", std::string("unknown error"));
            return ok;
        } catch (...) {
            errOut = "malformed response";
            return false;
        }
    }

private:
    void workerLoop() {
#ifdef _WIN32
        WSADATA wsa; WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
        uint64_t lastEventId = 0;
        auto lastIdentify = std::chrono::steady_clock::time_point::min();
        while (!stopFlag_.load()) {
            auto now = std::chrono::steady_clock::now();
            // Identify on first connect and every 30s after.
            bool wantIdentify = (lastIdentify == std::chrono::steady_clock::time_point::min()) ||
                (std::chrono::duration_cast<std::chrono::seconds>(now - lastIdentify).count() >= 30);
            if (wantIdentify) {
                KujhadHttpResponse res;
                auto t0 = std::chrono::steady_clock::now();
                bool ok = doRequest("GET", "/v1/identify", "", res, 5000);
                auto t1 = std::chrono::steady_clock::now();
                std::lock_guard<std::mutex> lk(snapMtx_);
                snap_.reachable = ok;
                snap_.linkLatencyMs = (int)std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
                snap_.lastSyncMs = (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
                if (ok) {
                    try { snap_.identify = kujhad_json::parse(res.body); } catch (...) { snap_.identify = kujhad_json::object(); }
                    snap_.lastError.clear();
                } else {
                    snap_.lastError = res.body.empty() ? "unreachable" : res.body;
                }
                lastIdentify = now;
            }
            // State + GPS each second.
            {
                KujhadHttpResponse res;
                if (doRequest("GET", "/v1/state", "", res, 3000)) {
                    std::lock_guard<std::mutex> lk(snapMtx_);
                    try { snap_.state = kujhad_json::parse(res.body); } catch (...) {}
                    snap_.reachable = true;
                }
            }
            {
                KujhadHttpResponse res;
                if (doRequest("GET", "/v1/gps", "", res, 3000)) {
                    std::lock_guard<std::mutex> lk(snapMtx_);
                    try { snap_.gps = kujhad_json::parse(res.body); } catch (...) {}
                }
            }
            // Events since last id.
            {
                std::string p = std::string("/v1/events?since=") + std::to_string(lastEventId);
                KujhadHttpResponse res;
                if (doRequest("GET", p, "", res, 3000)) {
                    try {
                        kujhad_json j = kujhad_json::parse(res.body);
                        if (j.contains("lastId") && j["lastId"].is_number()) {
                            lastEventId = j["lastId"].get<uint64_t>();
                        }
                        if (j.contains("events") && j["events"].is_array()) {
                            std::lock_guard<std::mutex> lk(eventMtx_);
                            for (auto& e : j["events"]) {
                                if (events_.size() > 1024) events_.pop();
                                events_.push(e);
                            }
                        }
                    } catch (...) {}
                }
            }
            // Sleep ~1s between polls but exit promptly on stop.
            for (int i = 0; i < 20 && !stopFlag_.load(); i++) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        }
#ifdef _WIN32
        WSACleanup();
#endif
    }

    // Open a fresh connection per request — peers are local, the
    // request volume is tiny, and per-connection state keeps the code
    // simple and TLS-swap-friendly later. Returns true on a parsed
    // response.
    bool doRequest(const std::string& method, const std::string& path,
                   const std::string& body, KujhadHttpResponse& out, int timeoutMs) {
        kujhad_socket_t sock = ::socket(AF_INET, SOCK_STREAM, 0);
        if (sock == KUJHAD_INVALID_SOCK) return false;
#ifndef _WIN32
        timeval tv{}; tv.tv_sec = timeoutMs / 1000; tv.tv_usec = (timeoutMs % 1000) * 1000;
        ::setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        ::setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif
        addrinfo hints{}; hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM;
        addrinfo* res = nullptr;
        std::string portStr = std::to_string(port_);
        int gai = ::getaddrinfo(host_.c_str(), portStr.c_str(), &hints, &res);
        if (gai != 0 || !res) {
            if (res) ::freeaddrinfo(res);
            KUJHAD_CLOSESOCK(sock);
            return false;
        }
        bool connected = (::connect(sock, res->ai_addr, (socklen_t)res->ai_addrlen) == 0);
        ::freeaddrinfo(res);
        if (!connected) {
            KUJHAD_CLOSESOCK(sock);
            return false;
        }
        std::string keyHeader;
        {
            keyHeader = std::string("X-Kujhad-Key: ") + apiKey_ + "\r\n";
        }
        char header[1024];
        int n = snprintf(header, sizeof(header),
            "%s %s HTTP/1.1\r\n"
            "Host: %s:%d\r\n"
            "%s"
            "Content-Type: application/json\r\n"
            "Content-Length: %d\r\n"
            "Connection: close\r\n"
            "\r\n",
            method.c_str(), path.c_str(), host_.c_str(), port_,
            keyHeader.c_str(), (int)body.size());
        if (!kujhadWriteAll(sock, header, n)) { KUJHAD_CLOSESOCK(sock); return false; }
        if (!body.empty() && !kujhadWriteAll(sock, body.data(), (int)body.size())) {
            KUJHAD_CLOSESOCK(sock);
            return false;
        }
        // Read response.
        std::string buf;
        char chunk[1024];
        for (;;) {
            int r = (int)::recv(sock, chunk, sizeof(chunk), 0);
            if (r <= 0) break;
            buf.append(chunk, r);
            if (buf.size() > (1 << 20)) break; // 1 MiB cap
        }
        KUJHAD_CLOSESOCK(sock);
        // Parse response: status line + headers + body.
        size_t headerEnd = buf.find("\r\n\r\n");
        if (headerEnd == std::string::npos) return false;
        size_t lineEnd = buf.find("\r\n");
        if (lineEnd == std::string::npos) return false;
        std::string statusLine = buf.substr(0, lineEnd);
        size_t sp1 = statusLine.find(' ');
        if (sp1 == std::string::npos) return false;
        size_t sp2 = statusLine.find(' ', sp1 + 1);
        if (sp2 == std::string::npos) sp2 = statusLine.size();
        out.status = std::atoi(statusLine.substr(sp1 + 1, sp2 - sp1 - 1).c_str());
        out.body   = buf.substr(headerEnd + 4);
        return true;
    }

    std::string host_;
    int port_ = 0;
    std::string apiKey_;

    std::atomic<bool> running_{false};
    std::atomic<bool> stopFlag_{false};
    std::thread worker_;

    mutable std::mutex snapMtx_;
    KujhadPeerSnapshot snap_;

    std::mutex eventMtx_;
    std::queue<kujhad_json> events_;
};

} // namespace predator
