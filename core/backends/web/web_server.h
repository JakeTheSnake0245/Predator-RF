#pragma once

// Predator RF web backend — embedded HTTP/WebSocket/SSE server.
//
// No external deps beyond POSIX sockets and <openssl/sha.h> when present
// (KUJHAD_HAVE_OPENSSL). Falls back to a bundled public-domain SHA-1
// when OpenSSL is absent.
//
// Usage:
//   PredatorWebServer srv;
//   srv.addRoute("GET", "/api/state", myHandler);
//   srv.setStaticRoot("/usr/share/predator-rf/web");
//   srv.start(5555);
//   srv.broadcastWs("{\"type\":\"spectrum\",\"bins\":[...]}");
//   srv.pushSse("{\"type\":\"event\",\"data\":{...}}");
//   srv.stop();
//
// Thread safety: broadcastWs / pushSse are safe to call from any thread.
// Route handlers are called on per-connection worker threads.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <sstream>
#include <thread>
#include <vector>
#include <fstream>
#include <set>

#ifndef _WIN32
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
using pws_sock_t = int;
#define PWS_INVALID_SOCK (-1)
#define PWS_CLOSESOCK ::close
#else
#include <winsock2.h>
using pws_sock_t = SOCKET;
#define PWS_INVALID_SOCK INVALID_SOCKET
#define PWS_CLOSESOCK closesocket
#endif

#ifdef KUJHAD_HAVE_OPENSSL
#include <openssl/sha.h>
#endif

namespace predator {

// ---------------------------------------------------------------------------
// SHA-1 (public domain; used only for WebSocket handshake when no OpenSSL)
// ---------------------------------------------------------------------------
#ifndef KUJHAD_HAVE_OPENSSL
namespace detail {
struct Sha1Ctx {
    uint32_t h[5];
    uint8_t  buf[64];
    uint64_t bits;
    uint32_t len;
};
inline void sha1Init(Sha1Ctx& c) {
    c.h[0]=0x67452301; c.h[1]=0xEFCDAB89; c.h[2]=0x98BADCFE;
    c.h[3]=0x10325476; c.h[4]=0xC3D2E1F0;
    c.bits=0; c.len=0;
}
inline uint32_t sha1Rol(uint32_t v,int n){return (v<<n)|(v>>(32-n));}
inline void sha1Block(Sha1Ctx& c){
    uint32_t w[80];
    for(int i=0;i<16;i++){
        int b=i*4;
        w[i]=((uint32_t)c.buf[b]<<24)|((uint32_t)c.buf[b+1]<<16)|
             ((uint32_t)c.buf[b+2]<<8)|(uint32_t)c.buf[b+3];
    }
    for(int i=16;i<80;i++) w[i]=sha1Rol(w[i-3]^w[i-8]^w[i-14]^w[i-16],1);
    uint32_t a=c.h[0],b=c.h[1],cc=c.h[2],d=c.h[3],e=c.h[4];
    for(int i=0;i<80;i++){
        uint32_t f,k;
        if(i<20){f=(b&cc)|(~b&d);k=0x5A827999;}
        else if(i<40){f=b^cc^d;k=0x6ED9EBA1;}
        else if(i<60){f=(b&cc)|(b&d)|(cc&d);k=0x8F1BBCDC;}
        else{f=b^cc^d;k=0xCA62C1D6;}
        uint32_t t=sha1Rol(a,5)+f+e+k+w[i];
        e=d;d=cc;cc=sha1Rol(b,30);b=a;a=t;
    }
    c.h[0]+=a;c.h[1]+=b;c.h[2]+=cc;c.h[3]+=d;c.h[4]+=e;
}
inline void sha1Update(Sha1Ctx& c,const uint8_t* data,size_t n){
    for(size_t i=0;i<n;i++){
        c.buf[c.len++]=data[i];
        c.bits+=8;
        if(c.len==64){sha1Block(c);c.len=0;}
    }
}
inline void sha1Final(Sha1Ctx& c,uint8_t out[20]){
    c.buf[c.len++]=0x80;
    if(c.len>56){while(c.len<64)c.buf[c.len++]=0;sha1Block(c);c.len=0;}
    while(c.len<56)c.buf[c.len++]=0;
    for(int i=7;i>=0;i--){c.buf[56+(7-i)]=(uint8_t)(c.bits>>(i*8));}
    sha1Block(c);
    for(int i=0;i<5;i++){out[i*4]=(uint8_t)(c.h[i]>>24);out[i*4+1]=(uint8_t)(c.h[i]>>16);
                          out[i*4+2]=(uint8_t)(c.h[i]>>8);out[i*4+3]=(uint8_t)(c.h[i]);}
}
}
#endif

inline void pwsSha1(const uint8_t* data, size_t len, uint8_t out[20]) {
#ifdef KUJHAD_HAVE_OPENSSL
    SHA1(data, len, out);
#else
    detail::Sha1Ctx c; detail::sha1Init(c);
    detail::sha1Update(c, data, len);
    detail::sha1Final(c, out);
#endif
}

// ---------------------------------------------------------------------------
// Base64 encode
// ---------------------------------------------------------------------------
inline std::string pwsBase64(const uint8_t* data, size_t n) {
    static const char t[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out; out.reserve(((n+2)/3)*4);
    for(size_t i=0;i<n;i+=3){
        uint32_t v=(uint32_t)data[i]<<16;
        if(i+1<n) v|=(uint32_t)data[i+1]<<8;
        if(i+2<n) v|=(uint32_t)data[i+2];
        out+=t[(v>>18)&63]; out+=t[(v>>12)&63];
        out+=(i+1<n)?t[(v>>6)&63]:'=';
        out+=(i+2<n)?t[v&63]:'=';
    }
    return out;
}

// ---------------------------------------------------------------------------
// WebSocket accept-key computation
// ---------------------------------------------------------------------------
inline std::string pwsWsAcceptKey(const std::string& clientKey) {
    static const char* magic = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    std::string cat = clientKey + magic;
    uint8_t hash[20];
    pwsSha1((const uint8_t*)cat.data(), cat.size(), hash);
    return pwsBase64(hash, 20);
}

// ---------------------------------------------------------------------------
// HTTP helpers
// ---------------------------------------------------------------------------
struct PwsRequest {
    std::string method;
    std::string path;
    std::string query;
    std::map<std::string,std::string> headers;
    std::string body;
};

inline std::string pwsUrlDecode(const std::string& s) {
    std::string out;
    for(size_t i=0;i<s.size();i++){
        if(s[i]=='%'&&i+2<s.size()){
            char hex[3]={s[i+1],s[i+2],0};
            out+=(char)std::strtol(hex,nullptr,16);
            i+=2;
        } else if(s[i]=='+') out+=' ';
        else out+=s[i];
    }
    return out;
}

inline std::map<std::string,std::string> pwsParseQuery(const std::string& q) {
    std::map<std::string,std::string> out;
    std::string tok; std::istringstream ss(q);
    while(std::getline(ss,tok,'&')){
        auto p=tok.find('=');
        if(p==std::string::npos) out[pwsUrlDecode(tok)]="";
        else out[pwsUrlDecode(tok.substr(0,p))]=pwsUrlDecode(tok.substr(p+1));
    }
    return out;
}

inline bool pwsReadRequest(pws_sock_t sock, PwsRequest& req) {
    std::string raw;
    char buf[4096];
    // Read until end of headers
    while(true){
        int n=(int)::recv(sock,buf,sizeof(buf)-1,0);
        if(n<=0) return false;
        buf[n]=0; raw.append(buf,n);
        if(raw.find("\r\n\r\n")!=std::string::npos) break;
        if(raw.size()>131072) return false;
    }
    // Split headers and any bytes already read past \r\n\r\n
    auto splitAt = raw.find("\r\n\r\n");
    std::string headerPart = raw.substr(0, splitAt);
    std::string bodyAlready = raw.substr(splitAt + 4);

    std::istringstream ss(headerPart);
    std::string line;
    std::getline(ss,line);
    if(!line.empty()&&line.back()=='\r') line.pop_back();
    std::istringstream ls(line);
    ls>>req.method>>req.path;
    auto qp=req.path.find('?');
    if(qp!=std::string::npos){req.query=req.path.substr(qp+1);req.path=req.path.substr(0,qp);}
    while(std::getline(ss,line)){
        if(line=="\r"||line.empty()) break;
        if(line.back()=='\r') line.pop_back();
        auto c=line.find(':');
        if(c!=std::string::npos){
            std::string k=line.substr(0,c),v=line.substr(c+2);
            std::transform(k.begin(),k.end(),k.begin(),::tolower);
            // Trim leading whitespace from value
            while(!v.empty()&&(v.front()==' '||v.front()=='\t')) v.erase(v.begin());
            req.headers[k]=v;
        }
    }
    // Read body if Content-Length is present
    size_t contentLen = 0;
    auto clIt = req.headers.find("content-length");
    if(clIt != req.headers.end()) {
        try { contentLen = (size_t)std::stoull(clIt->second); } catch(...) {}
    }
    if(contentLen > 0 && contentLen <= 1048576) {
        req.body = bodyAlready;
        while(req.body.size() < contentLen) {
            int need = (int)(contentLen - req.body.size());
            if(need > (int)sizeof(buf)-1) need = (int)sizeof(buf)-1;
            int n = (int)::recv(sock, buf, need, 0);
            if(n <= 0) break;
            req.body.append(buf, n);
        }
    }
    return true;
}

inline std::string pwsMimeType(const std::string& path) {
    if(path.size()>=5&&path.substr(path.size()-5)==".html") return "text/html; charset=utf-8";
    if(path.size()>=3&&path.substr(path.size()-3)==".js")   return "application/javascript";
    if(path.size()>=4&&path.substr(path.size()-4)==".css")  return "text/css";
    if(path.size()>=5&&path.substr(path.size()-5)==".json") return "application/json";
    if(path.size()>=4&&path.substr(path.size()-4)==".svg")  return "image/svg+xml";
    if(path.size()>=4&&path.substr(path.size()-4)==".png")  return "image/png";
    return "application/octet-stream";
}

inline void pwsSendAll(pws_sock_t sock, const char* buf, size_t n) {
    size_t sent=0;
    while(sent<n){
        int r=(int)::send(sock,buf+sent,(int)(n-sent),0);
        if(r<=0) break;
        sent+=r;
    }
}

inline void pwsHttpReply(pws_sock_t sock, int code, const std::string& ct,
                          const std::string& body, const std::string& extra="") {
    static const char* phrases[] = {"OK","Created","No Content","Bad Request",
                                    "Not Found","Method Not Allowed","Internal Server Error"};
    static const int   codes[]   = {200,201,204,400,404,405,500};
    const char* phrase="OK";
    for(int i=0;i<7;i++) if(codes[i]==code){phrase=phrases[i];break;}
    std::string hdr =
        "HTTP/1.1 "+std::to_string(code)+" "+phrase+"\r\n"
        "Content-Type: "+ct+"\r\n"
        "Content-Length: "+std::to_string(body.size())+"\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Cache-Control: no-cache\r\n"
        +extra+
        "\r\n";
    pwsSendAll(sock, hdr.c_str(), hdr.size());
    if(!body.empty()) pwsSendAll(sock, body.c_str(), body.size());
}

// ---------------------------------------------------------------------------
// WebSocket frame builder
// ---------------------------------------------------------------------------
inline std::string pwsWsFrame(uint8_t opcode, const std::string& payload) {
    std::string f;
    f+=(char)(0x80|opcode);
    size_t n=payload.size();
    if(n<=125) f+=(char)n;
    else if(n<=65535){ f+='\x7E';f+=(char)(n>>8);f+=(char)(n&0xFF);}
    else{
        f+='\x7F';
        for(int i=7;i>=0;i--) f+=(char)((n>>(i*8))&0xFF);
    }
    f+=payload;
    return f;
}

// ---------------------------------------------------------------------------
// PredatorWebServer
// ---------------------------------------------------------------------------
struct PwsContext {
    PwsRequest req;
    pws_sock_t sock = PWS_INVALID_SOCK;
    std::map<std::string,std::string> query;
};

using PwsHandler = std::function<void(PwsContext&)>;

class PredatorWebServer {
public:
    PredatorWebServer() = default;
    ~PredatorWebServer() { stop(); }

    void addRoute(const std::string& method, const std::string& path, PwsHandler h) {
        routes_.push_back({method, path, std::move(h)});
    }

    void setStaticRoot(const std::string& dir) { staticRoot_ = dir; }

    // Set the API key required on all non-static, non-identify requests.
    // Empty string = no auth (dev/loopback-only mode).
    void setApiKey(const std::string& key) {
        std::lock_guard<std::mutex> lk(apiKeyMtx_);
        apiKey_ = key;
    }

    // When true, bind to all interfaces (0.0.0.0); otherwise loopback only.
    // Loopback is the safe default — operator explicitly opts in to exposure.
    void setBindAll(bool v) { bindAll_ = v; }

    bool start(int port) {
        port_ = port;
        listenSock_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if(listenSock_==PWS_INVALID_SOCK) return false;
        int reuse=1;
        ::setsockopt(listenSock_, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse));
        sockaddr_in addr{};
        addr.sin_family=AF_INET;
        addr.sin_addr.s_addr = htonl(bindAll_ ? INADDR_ANY : INADDR_LOOPBACK);
        addr.sin_port=htons((uint16_t)port);
        if(::bind(listenSock_,(sockaddr*)&addr,sizeof(addr))!=0){PWS_CLOSESOCK(listenSock_);return false;}
        if(::listen(listenSock_,32)!=0){PWS_CLOSESOCK(listenSock_);return false;}
        running_=true;
        acceptThread_=std::thread([this]{acceptLoop();});
        return true;
    }

    void stop() {
        running_=false;
        if(listenSock_!=PWS_INVALID_SOCK){PWS_CLOSESOCK(listenSock_);listenSock_=PWS_INVALID_SOCK;}
        if(acceptThread_.joinable()) acceptThread_.join();
    }

    void broadcastWs(const std::string& json) {
        auto frame = pwsWsFrame(0x01, json);
        std::lock_guard<std::mutex> lk(wsMtx_);
        for(auto& c : wsClients_) {
            pwsSendAll(c, frame.c_str(), frame.size());
        }
    }

    void pushSse(const std::string& json) {
        std::string msg = "data: " + json + "\n\n";
        std::lock_guard<std::mutex> lk(sseMtx_);
        for(auto& c : sseClients_) {
            pwsSendAll(c, msg.c_str(), msg.size());
        }
    }

    int port() const { return port_; }

private:
    struct Route { std::string method, path; PwsHandler handler; };

    void acceptLoop() {
        while(running_) {
            fd_set rset; FD_ZERO(&rset); FD_SET(listenSock_,&rset);
            timeval tv{0,200000};
            if(::select((int)listenSock_+1,&rset,nullptr,nullptr,&tv)<=0) continue;
            sockaddr_in peer{}; socklen_t plen=sizeof(peer);
            pws_sock_t c=::accept(listenSock_,(sockaddr*)&peer,&plen);
            if(c==PWS_INVALID_SOCK) continue;
            int nodelay=1;
            ::setsockopt(c,IPPROTO_TCP,TCP_NODELAY,(const char*)&nodelay,sizeof(nodelay));
            timeval tv2{5,0};
            ::setsockopt(c,SOL_SOCKET,SO_RCVTIMEO,(const char*)&tv2,sizeof(tv2));
            std::thread([this,c]{handleConn(c);}).detach();
        }
    }

    // Paths that don't require auth (public / identification)
    static bool isPublicPath(const std::string& path) {
        return path == "/" || path == "/index.html" ||
               path == "/api/v1/identify" || path == "/v1/identify" ||
               path == "/api/identify";
    }

    // Returns true if the request carries a valid API key when one is configured.
    bool checkAuth(const PwsRequest& req) const {
        std::lock_guard<std::mutex> lk(apiKeyMtx_);
        if(apiKey_.empty()) return true; // no auth configured
        // Check X-Kujhad-Key header (primary, matches Kujhad fleet protocol)
        auto it = req.headers.find("x-kujhad-key");
        if(it != req.headers.end() && it->second == apiKey_) return true;
        // Check Authorization: Bearer <key>
        auto auth = req.headers.find("authorization");
        if(auth != req.headers.end()) {
            const std::string& v = auth->second;
            if(v.size() > 7 && v.substr(0,7) == "Bearer " && v.substr(7) == apiKey_) return true;
        }
        // Check ?key= query param (last resort, only for browser GET requests)
        return false;
    }

    void handleConn(pws_sock_t sock) {
        PwsRequest req;
        if(!pwsReadRequest(sock, req)) { PWS_CLOSESOCK(sock); return; }

        // Handle OPTIONS preflight BEFORE auth — browsers send it without creds.
        if(req.method == "OPTIONS") {
            pwsHttpReply(sock, 204, "text/plain", "",
                         "Access-Control-Allow-Methods: GET,POST,OPTIONS\r\n"
                         "Access-Control-Allow-Headers: X-Kujhad-Key,Authorization,Content-Type\r\n");
            PWS_CLOSESOCK(sock);
            return;
        }

        // Auth gate: static files and public identify are exempt; everything else needs a key
        if(!isPublicPath(req.path) && !isStaticPath(req.path)) {
            if(!checkAuth(req)) {
                pwsHttpReply(sock, 401, "application/json",
                             "{\"error\":\"X-Kujhad-Key required\"}");
                PWS_CLOSESOCK(sock);
                return;
            }
        }

        bool isWsUpgrade = false;
        auto upg = req.headers.find("upgrade");
        if(upg!=req.headers.end()) {
            std::string v=upg->second;
            std::transform(v.begin(),v.end(),v.begin(),::tolower);
            isWsUpgrade=(v.find("websocket")!=std::string::npos);
        }
        bool isSse = false;
        {auto it=req.headers.find("accept");
         if(it!=req.headers.end()&&it->second.find("text/event-stream")!=std::string::npos) isSse=true;}

        if(isWsUpgrade) { handleWs(sock, req); return; }
        if(isSse)       { handleSse(sock, req); return; }

        // Regular HTTP
        PwsContext ctx; ctx.sock=sock; ctx.req=req;
        ctx.query = pwsParseQuery(req.query);

        for(auto& r : routes_) {
            if(r.method==req.method && r.path==req.path) {
                r.handler(ctx);
                PWS_CLOSESOCK(sock);
                return;
            }
        }
        // Static file fallback
        if(req.method=="GET" && !staticRoot_.empty()) {
            std::string fp = staticRoot_ + req.path;
            if(fp.back()=='/') fp+="index.html";
            // Sanitise: no ".." traversal
            if(fp.find("..")!=std::string::npos) {
                pwsHttpReply(sock,400,"text/plain","bad path");
            } else {
                std::ifstream f(fp,std::ios::binary);
                if(!f) {
                    pwsHttpReply(sock,404,"text/plain","not found");
                } else {
                    std::string body((std::istreambuf_iterator<char>(f)),{});
                    pwsHttpReply(sock,200,pwsMimeType(fp),body);
                }
            }
        } else {
            pwsHttpReply(sock,404,"text/plain","not found");
        }
        PWS_CLOSESOCK(sock);
    }

    void handleWs(pws_sock_t sock, const PwsRequest& req) {
        auto kIt=req.headers.find("sec-websocket-key");
        if(kIt==req.headers.end()){PWS_CLOSESOCK(sock);return;}
        std::string accept=pwsWsAcceptKey(kIt->second);
        std::string hs =
            "HTTP/1.1 101 Switching Protocols\r\n"
            "Upgrade: websocket\r\nConnection: Upgrade\r\n"
            "Sec-WebSocket-Accept: "+accept+"\r\n\r\n";
        pwsSendAll(sock,hs.c_str(),hs.size());
        {std::lock_guard<std::mutex> lk(wsMtx_); wsClients_.insert(sock);}
        // Keep alive: read pings, send pongs, exit on close/error
        char buf[256];
        while(running_) {
            int n=(int)::recv(sock,buf,sizeof(buf),0);
            if(n<=0) break;
            uint8_t op=(uint8_t)buf[0]&0x0F;
            if(op==0x09) { // ping → pong
                auto pong=pwsWsFrame(0x0A,"");
                pwsSendAll(sock,pong.c_str(),pong.size());
            } else if(op==0x08) break; // close
        }
        {std::lock_guard<std::mutex> lk(wsMtx_); wsClients_.erase(sock);}
        PWS_CLOSESOCK(sock);
    }

    void handleSse(pws_sock_t sock, const PwsRequest& req) {
        // Check if there's a route for this path with SSE
        for(auto& r : routes_) {
            if(r.method=="SSE" && r.path==req.path) {
                PwsContext ctx; ctx.sock=sock; ctx.req=req;
                r.handler(ctx);
                return;
            }
        }
        std::string hdr =
            "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
            "Cache-Control: no-cache\r\nAccess-Control-Allow-Origin: *\r\n"
            "Connection: keep-alive\r\n\r\n";
        pwsSendAll(sock,hdr.c_str(),hdr.size());
        {std::lock_guard<std::mutex> lk(sseMtx_); sseClients_.insert(sock);}
        // Keep-alive until disconnect
        timeval tv{30,0};
        ::setsockopt(sock,SOL_SOCKET,SO_RCVTIMEO,(const char*)&tv,sizeof(tv));
        char buf[4];
        while(running_) {
            int n=(int)::recv(sock,buf,sizeof(buf),0);
            if(n==0) break;
            if(n<0&&errno!=EAGAIN&&errno!=EWOULDBLOCK&&errno!=EINTR) break;
        }
        {std::lock_guard<std::mutex> lk(sseMtx_); sseClients_.erase(sock);}
        PWS_CLOSESOCK(sock);
    }

    bool isStaticPath(const std::string& path) const {
        if(staticRoot_.empty()) return false;
        // Paths that don't start with /api/ or /v1/ are assumed to be static
        return path.rfind("/api/",0) != 0 && path.rfind("/v1/",0) != 0 &&
               path.rfind("/ws",0) != 0;
    }

    std::vector<Route>   routes_;
    std::string          staticRoot_;
    pws_sock_t           listenSock_ = PWS_INVALID_SOCK;
    std::atomic<bool>    running_{false};
    std::thread          acceptThread_;
    int                  port_ = 5555;
    bool                 bindAll_ = false;

    mutable std::mutex   apiKeyMtx_;
    std::string          apiKey_;

    std::mutex           wsMtx_;
    std::set<pws_sock_t> wsClients_;

    std::mutex           sseMtx_;
    std::set<pws_sock_t> sseClients_;
};

} // namespace predator
