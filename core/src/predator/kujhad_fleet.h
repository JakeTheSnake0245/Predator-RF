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
// TLS: when the build links against OpenSSL (KUJHAD_HAVE_OPENSSL) the
// device server can wrap its listener in TLS using a self-signed cert
// and the controller client can connect over TLS with a pinned-cert
// fingerprint as the trust anchor (no CA chain — operators verify the
// cert SHA-256 fingerprint out-of-band, the same way SSH host keys are
// pinned). When TLS is on, the plain HTTP path is locked to loopback
// only; non-loopback peers are rejected at the listener so the API key
// never crosses the network in the clear. When OpenSSL isn't available
// the TLS toggle is hidden and behaviour falls back to plain HTTP over
// a private overlay (ZeroTier / Tailscale) like before.
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

#ifdef KUJHAD_HAVE_OPENSSL
  #include <openssl/ssl.h>
  #include <openssl/err.h>
  #include <openssl/pem.h>
  #include <openssl/x509.h>
  #include <openssl/x509v3.h>
  #include <openssl/rsa.h>
  #include <openssl/bn.h>
  #include <openssl/sha.h>
  #include <openssl/evp.h>
  #include <openssl/rand.h>
  #include <mutex>
#endif

#include <ctime>
#include <fstream>
#include <sstream>

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
// Connection wrapper. Either holds a raw socket (plain HTTP) or a socket
// + SSL handle (TLS). All the HTTP helpers below take a KujhadConnection&
// so the protocol code never has to care which transport it's on. The
// destructor closes both layers in the right order.
// ---------------------------------------------------------------------------

struct KujhadConnection {
    kujhad_socket_t sock = KUJHAD_INVALID_SOCK;
#ifdef KUJHAD_HAVE_OPENSSL
    SSL* ssl = nullptr;
#endif
    KujhadConnection() = default;
    KujhadConnection(KujhadConnection&& o) noexcept { *this = std::move(o); }
    KujhadConnection& operator=(KujhadConnection&& o) noexcept {
        close();
        sock = o.sock; o.sock = KUJHAD_INVALID_SOCK;
#ifdef KUJHAD_HAVE_OPENSSL
        ssl  = o.ssl;  o.ssl  = nullptr;
#endif
        return *this;
    }
    KujhadConnection(const KujhadConnection&) = delete;
    KujhadConnection& operator=(const KujhadConnection&) = delete;
    ~KujhadConnection() { close(); }

    void close() {
#ifdef KUJHAD_HAVE_OPENSSL
        if (ssl) {
            // Best-effort shutdown — peer may already be gone, swallow errors.
            SSL_shutdown(ssl);
            SSL_free(ssl);
            ssl = nullptr;
        }
#endif
        if (sock != KUJHAD_INVALID_SOCK) { KUJHAD_CLOSESOCK(sock); sock = KUJHAD_INVALID_SOCK; }
    }

    bool valid() const { return sock != KUJHAD_INVALID_SOCK; }

    int recvOnce(char* buf, int n) {
#ifdef KUJHAD_HAVE_OPENSSL
        if (ssl) return SSL_read(ssl, buf, n);
#endif
        return (int)::recv(sock, buf, n, 0);
    }

    int sendOnce(const char* buf, int n) {
#ifdef KUJHAD_HAVE_OPENSSL
        if (ssl) return SSL_write(ssl, buf, n);
#endif
        return (int)::send(sock, buf, n, 0);
    }
};

// ---------------------------------------------------------------------------
// TLS plumbing. All of this is no-op when the build is missing OpenSSL.
// ---------------------------------------------------------------------------

#ifdef KUJHAD_HAVE_OPENSSL
inline void kujhadEnsureOpenSsl() {
    static std::once_flag once;
    std::call_once(once, []() {
        SSL_library_init();
        SSL_load_error_strings();
        OpenSSL_add_all_algorithms();
    });
}
#else
inline void kujhadEnsureOpenSsl() {}
#endif

// True at runtime when this build links against OpenSSL.
inline bool kujhadTlsAvailable() {
#ifdef KUJHAD_HAVE_OPENSSL
    return true;
#else
    return false;
#endif
}

// Format raw bytes as colon-separated upper-case hex (e.g. "AB:CD:..."),
// the canonical layout for cert fingerprints in browsers / openssl tools.
inline std::string kujhadHexFingerprint(const unsigned char* data, size_t len) {
    static const char hex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(len * 3);
    for (size_t i = 0; i < len; i++) {
        if (i) out.push_back(':');
        out.push_back(hex[(data[i] >> 4) & 0xF]);
        out.push_back(hex[data[i] & 0xF]);
    }
    return out;
}

// Normalise a fingerprint string for comparison: strip whitespace and
// colons, upper-case the hex digits. So "ab:cd EF" matches "ABCDEF".
inline std::string kujhadNormaliseFingerprint(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c == ':' || c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '-') continue;
        if (c >= 'a' && c <= 'f') out.push_back((char)(c - 32));
        else out.push_back(c);
    }
    return out;
}

// Compute the SHA-256 fingerprint of a PEM cert file. Returns empty
// string if the file can't be read or parsed.
inline std::string kujhadCertFingerprintFromPemFile(const std::string& path) {
#ifdef KUJHAD_HAVE_OPENSSL
    kujhadEnsureOpenSsl();
    FILE* fp = std::fopen(path.c_str(), "rb");
    if (!fp) return "";
    X509* cert = PEM_read_X509(fp, nullptr, nullptr, nullptr);
    std::fclose(fp);
    if (!cert) return "";
    unsigned char md[EVP_MAX_MD_SIZE];
    unsigned int mdLen = 0;
    int ok = X509_digest(cert, EVP_sha256(), md, &mdLen);
    X509_free(cert);
    if (!ok) return "";
    return kujhadHexFingerprint(md, mdLen);
#else
    (void)path;
    return "";
#endif
}

#ifdef KUJHAD_HAVE_OPENSSL
// Compute the SHA-256 fingerprint of an in-memory X509 cert.
inline std::string kujhadCertFingerprintFromX509(X509* cert) {
    if (!cert) return "";
    unsigned char md[EVP_MAX_MD_SIZE];
    unsigned int mdLen = 0;
    if (!X509_digest(cert, EVP_sha256(), md, &mdLen)) return "";
    return kujhadHexFingerprint(md, mdLen);
}
#endif

// Generate a 10-year self-signed RSA-2048 cert and write the cert + key
// to the configured PEM paths. `commonName` is stamped into CN= so the
// operator can recognise the cert in fingerprint dialogs. Returns the
// SHA-256 fingerprint of the new cert on success, empty on failure.
inline std::string kujhadGenerateSelfSignedCert(const std::string& certPath,
                                                 const std::string& keyPath,
                                                 const std::string& commonName) {
#ifdef KUJHAD_HAVE_OPENSSL
    kujhadEnsureOpenSsl();
    EVP_PKEY* pkey = EVP_PKEY_new();
    if (!pkey) return "";
    BIGNUM* bn = BN_new();
    BN_set_word(bn, RSA_F4);
    RSA* rsa = RSA_new();
    if (!RSA_generate_key_ex(rsa, 2048, bn, nullptr)) {
        RSA_free(rsa); BN_free(bn); EVP_PKEY_free(pkey); return "";
    }
    BN_free(bn);
    if (!EVP_PKEY_assign_RSA(pkey, rsa)) { // takes ownership of rsa on success
        RSA_free(rsa); EVP_PKEY_free(pkey); return "";
    }

    X509* cert = X509_new();
    if (!cert) { EVP_PKEY_free(pkey); return ""; }
    // Random 64-bit serial — OpenSSL refuses cert chains with duplicate
    // serials from the same issuer, even self-signed.
    unsigned char serialBytes[8];
    if (RAND_bytes(serialBytes, sizeof(serialBytes)) != 1) {
        // Fall back to clock if RNG is somehow broken.
        uint64_t now = (uint64_t)std::time(nullptr);
        std::memcpy(serialBytes, &now, sizeof(serialBytes));
    }
    BIGNUM* serialBn = BN_bin2bn(serialBytes, sizeof(serialBytes), nullptr);
    if (serialBn) {
        ASN1_INTEGER* serialAsn = BN_to_ASN1_INTEGER(serialBn, nullptr);
        if (serialAsn) {
            X509_set_serialNumber(cert, serialAsn);
            ASN1_INTEGER_free(serialAsn);
        }
        BN_free(serialBn);
    }
    X509_set_version(cert, 2); // X509 v3
    X509_gmtime_adj(X509_get_notBefore(cert), 0);
    X509_gmtime_adj(X509_get_notAfter(cert), (long)(60L * 60L * 24L * 365L * 10L));
    X509_set_pubkey(cert, pkey);
    X509_NAME* name = X509_get_subject_name(cert);
    std::string cn = commonName.empty() ? std::string("predator-kujhad") : commonName;
    X509_NAME_add_entry_by_txt(name, "O",  MBSTRING_ASC, (const unsigned char*)"Predator RF", -1, -1, 0);
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, (const unsigned char*)cn.c_str(), -1, -1, 0);
    X509_set_issuer_name(cert, name);
    if (!X509_sign(cert, pkey, EVP_sha256())) {
        X509_free(cert); EVP_PKEY_free(pkey); return "";
    }

    // Write key (PEM, no passphrase — file lives next to the config so
    // filesystem perms are the operator's responsibility, same as the
    // API key in the config file).
    FILE* kf = std::fopen(keyPath.c_str(), "wb");
    if (!kf) { X509_free(cert); EVP_PKEY_free(pkey); return ""; }
    bool keyOk = (PEM_write_PrivateKey(kf, pkey, nullptr, nullptr, 0, nullptr, nullptr) == 1);
    std::fclose(kf);
    if (!keyOk) { X509_free(cert); EVP_PKEY_free(pkey); return ""; }

    FILE* cf = std::fopen(certPath.c_str(), "wb");
    if (!cf) { X509_free(cert); EVP_PKEY_free(pkey); return ""; }
    bool certOk = (PEM_write_X509(cf, cert) == 1);
    std::fclose(cf);
    if (!certOk) { X509_free(cert); EVP_PKEY_free(pkey); return ""; }

    std::string fp = kujhadCertFingerprintFromX509(cert);
    X509_free(cert);
    EVP_PKEY_free(pkey);
    return fp;
#else
    (void)certPath; (void)keyPath; (void)commonName;
    return "";
#endif
}

// Server-side TLS context. Owns the SSL_CTX with cert + key loaded.
// `valid()` is true once both files have been parsed successfully.
class KujhadServerTlsContext {
public:
    KujhadServerTlsContext() = default;
    ~KujhadServerTlsContext() { reset(); }
    KujhadServerTlsContext(const KujhadServerTlsContext&) = delete;
    KujhadServerTlsContext& operator=(const KujhadServerTlsContext&) = delete;

    bool load(const std::string& certPath, const std::string& keyPath, std::string& errOut) {
#ifdef KUJHAD_HAVE_OPENSSL
        kujhadEnsureOpenSsl();
        reset();
        ctx_ = SSL_CTX_new(TLS_server_method());
        if (!ctx_) { errOut = "SSL_CTX_new failed"; return false; }
        // TLS 1.2 floor — covers every operator-grade browser / runtime
        // we'd realistically see, and shuts the door on SSLv3/TLSv1.0.
        SSL_CTX_set_min_proto_version(ctx_, TLS1_2_VERSION);
        SSL_CTX_set_options(ctx_, SSL_OP_NO_COMPRESSION);
        if (SSL_CTX_use_certificate_file(ctx_, certPath.c_str(), SSL_FILETYPE_PEM) != 1) {
            errOut = "cert load failed: " + certPath;
            reset();
            return false;
        }
        if (SSL_CTX_use_PrivateKey_file(ctx_, keyPath.c_str(), SSL_FILETYPE_PEM) != 1) {
            errOut = "key load failed: " + keyPath;
            reset();
            return false;
        }
        if (SSL_CTX_check_private_key(ctx_) != 1) {
            errOut = "cert/key mismatch";
            reset();
            return false;
        }
        fingerprint_ = kujhadCertFingerprintFromPemFile(certPath);
        return true;
#else
        (void)certPath; (void)keyPath;
        errOut = "TLS not built (no OpenSSL)";
        return false;
#endif
    }

    void reset() {
#ifdef KUJHAD_HAVE_OPENSSL
        if (ctx_) { SSL_CTX_free(ctx_); ctx_ = nullptr; }
#endif
        fingerprint_.clear();
    }

    bool valid() const {
#ifdef KUJHAD_HAVE_OPENSSL
        return ctx_ != nullptr;
#else
        return false;
#endif
    }

    const std::string& fingerprint() const { return fingerprint_; }

#ifdef KUJHAD_HAVE_OPENSSL
    SSL_CTX* ctx() const { return ctx_; }
#endif

private:
#ifdef KUJHAD_HAVE_OPENSSL
    SSL_CTX* ctx_ = nullptr;
#endif
    std::string fingerprint_;
};

// Wrap an already-accepted client socket in TLS using the server context.
// On success, ownership of `sock` moves into the returned connection. On
// failure, `sock` is closed and an empty connection is returned.
inline KujhadConnection kujhadAcceptTls(KujhadServerTlsContext& ctx, kujhad_socket_t sock) {
    KujhadConnection c;
    c.sock = sock;
#ifdef KUJHAD_HAVE_OPENSSL
    if (!ctx.valid()) { c.close(); return c; }
    SSL* ssl = SSL_new(ctx.ctx());
    if (!ssl) { c.close(); return c; }
    SSL_set_fd(ssl, (int)sock);
    int r = SSL_accept(ssl);
    if (r <= 0) { SSL_free(ssl); c.close(); return c; }
    c.ssl = ssl;
#else
    (void)ctx;
    c.close();
#endif
    return c;
}

// Wrap a connected client socket in TLS for a controller client. The
// peer certificate is captured into `outFingerprint` so the caller can
// compare it against the operator-pinned value before sending any
// authenticated traffic.
inline bool kujhadConnectTls(kujhad_socket_t sock, KujhadConnection& outConn,
                              std::string& outFingerprint, std::string& errOut) {
#ifdef KUJHAD_HAVE_OPENSSL
    kujhadEnsureOpenSsl();
    static std::once_flag clientOnce;
    static SSL_CTX* clientCtx = nullptr;
    std::call_once(clientOnce, []() {
        clientCtx = SSL_CTX_new(TLS_client_method());
        if (clientCtx) {
            SSL_CTX_set_min_proto_version(clientCtx, TLS1_2_VERSION);
            // We pin by fingerprint, so disable chain verification —
            // a self-signed device cert is the expected case.
            SSL_CTX_set_verify(clientCtx, SSL_VERIFY_NONE, nullptr);
        }
    });
    if (!clientCtx) { errOut = "client SSL_CTX init failed"; return false; }
    SSL* ssl = SSL_new(clientCtx);
    if (!ssl) { errOut = "SSL_new failed"; return false; }
    SSL_set_fd(ssl, (int)sock);
    int r = SSL_connect(ssl);
    if (r <= 0) {
        errOut = "TLS handshake failed";
        SSL_free(ssl);
        return false;
    }
    X509* peer = SSL_get_peer_certificate(ssl);
    if (!peer) {
        errOut = "peer presented no certificate";
        SSL_shutdown(ssl); SSL_free(ssl);
        return false;
    }
    outFingerprint = kujhadCertFingerprintFromX509(peer);
    X509_free(peer);
    outConn.sock = sock;
    outConn.ssl = ssl;
    return true;
#else
    (void)sock; (void)outConn; (void)outFingerprint;
    errOut = "TLS not built (no OpenSSL)";
    return false;
#endif
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

inline bool kujhadReadAll(KujhadConnection& conn, char* buf, int n) {
    int got = 0;
    while (got < n) {
        int r = conn.recvOnce(buf + got, n - got);
        if (r <= 0) return false;
        got += r;
    }
    return true;
}

inline bool kujhadWriteAll(KujhadConnection& conn, const char* buf, int n) {
    int sent = 0;
    while (sent < n) {
        int r = conn.sendOnce(buf + sent, n - sent);
        if (r <= 0) return false;
        sent += r;
    }
    return true;
}

inline bool kujhadParseRequest(KujhadConnection& conn, KujhadHttpRequest& req,
                                int maxBytes = 1 << 20 /* 1 MiB */) {
    std::string buf;
    buf.reserve(2048);
    char ch;
    // Read until \r\n\r\n header terminator.
    while ((int)buf.size() < maxBytes) {
        int r = conn.recvOnce(&ch, 1);
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
        if (n > 0 && !kujhadReadAll(conn, &req.body[0], n)) return false;
    }
    return true;
}

inline bool kujhadSendResponse(KujhadConnection& conn, const KujhadHttpResponse& res) {
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
    if (!kujhadWriteAll(conn, header, n)) return false;
    if (!res.body.empty() && !kujhadWriteAll(conn, res.body.data(), (int)res.body.size())) return false;
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

// One downsampled spectrum frame. Bins are dB values (typically dBFS) of
// length `bins.size()`, covering `bandwidth` Hz centered at `centerFreq`.
// `serial` increments per published frame so a controller can detect a
// stalled stream. `tsMs` is a steady-clock millisecond stamp from the
// device for client-side latency estimates.
struct KujhadSpectrumFrame {
    uint64_t serial = 0;
    uint64_t tsMs = 0;
    double centerFreq = 0.0;
    double bandwidth = 0.0;
    float fftMinDb = -120.0f;
    float fftMaxDb = 0.0f;
    std::vector<float> bins;
};

class KujhadDeviceServer {
public:
    using CommandHandler = std::function<bool(const KujhadDeviceCommand&, std::string& errOut)>;
    using SnapshotProvider = std::function<kujhad_json()>;
    // Spectrum provider returns true and fills `out` if a fresh frame is
    // available. Returning false means "no new data, skip this tick".
    using SpectrumProvider = std::function<bool(kujhad_json& out)>;

    ~KujhadDeviceServer() { stop(); }

    // Wire the snapshot providers BEFORE start(). They are read from the
    // server thread on every request and must be cheap + thread-safe.
    void setIdentifyProvider(SnapshotProvider fn) { identifyProvider_ = std::move(fn); }
    void setStateProvider(SnapshotProvider fn)    { stateProvider_    = std::move(fn); }
    void setGpsProvider(SnapshotProvider fn)      { gpsProvider_      = std::move(fn); }
    void setEventsProvider(std::function<kujhad_json(uint64_t since)> fn) { eventsProvider_ = std::move(fn); }
    void setCommandHandler(CommandHandler fn)     { commandHandler_   = std::move(fn); }
    void setSpectrumProvider(SpectrumProvider fn) { spectrumProvider_ = std::move(fn); }
    // Bound floor of 50ms (20 fps) keeps a runaway operator from saturating
    // the link; default 200ms (5 fps) is enough for situational awareness.
    void setSpectrumIntervalMs(int ms) {
        if (ms < 50) ms = 50;
        if (ms > 5000) ms = 5000;
        spectrumIntervalMs_ = ms;
    }
    int spectrumIntervalMs() const { return spectrumIntervalMs_.load(); }
    int activeSpectrumStreams() const { return activeSpectrumStreams_.load(); }

    void setApiKey(const std::string& key) {
        std::lock_guard<std::mutex> lk(mtx_);
        apiKey_ = key;
    }

    // Configure TLS. When `enabled` is true the listener wraps every
    // accepted socket in TLS using the cert + key at `certPath` /
    // `keyPath`. When `enabled` is false the listener serves plain HTTP
    // and rejects any connection that doesn't originate from 127.0.0.1
    // / ::1, so the API key never crosses the wire in the clear.
    // Must be called before start(); changes take effect on the next
    // start(). Returns true if the cert/key loaded cleanly (or TLS was
    // disabled). On failure the TLS context is cleared and a future
    // start() will refuse to bind.
    bool setTlsConfig(bool enabled, const std::string& certPath,
                      const std::string& keyPath, std::string& errOut) {
        std::lock_guard<std::mutex> lk(mtx_);
        tlsEnabled_ = enabled;
        tlsCertPath_ = certPath;
        tlsKeyPath_ = keyPath;
        tlsCtx_.reset();
        if (!enabled) {
            errOut.clear();
            return true;
        }
        bool ok = tlsCtx_.load(certPath, keyPath, errOut);
        if (!ok) tlsEnabled_ = false;
        return ok;
    }

    bool tlsEnabled() const {
        std::lock_guard<std::mutex> lk(mtx_);
        return tlsEnabled_ && tlsCtx_.valid();
    }

    std::string tlsFingerprint() const {
        std::lock_guard<std::mutex> lk(mtx_);
        return tlsCtx_.fingerprint();
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
        // Always send a raw TCP RST-style close — the listener only needs
        // to wake from accept(); it discards bad handshakes anyway.
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
            // Snapshot TLS state under the same lock that protects the
            // SSL_CTX so a concurrent setTlsConfig() can't pull the
            // context out from under SSL_accept().
            bool useTls;
            {
                std::lock_guard<std::mutex> lk(mtx_);
                useTls = tlsEnabled_ && tlsCtx_.valid();
            }
            // Plain HTTP is loopback-only by policy: anything that comes
            // from a non-loopback peer when TLS is off would leak the
            // API key on the wire, so we slam the door before any I/O.
            // Convert to host order before masking so the check is
            // endian-safe across platforms (the AF_INET socket gives us
            // a network-order address regardless of CPU).
            if (!useTls) {
                uint32_t ipHbo = ntohl(client.sin_addr.s_addr);
                // 127.0.0.0/8 is the IPv4 loopback block.
                if ((ipHbo >> 24) != 127) {
                    KUJHAD_CLOSESOCK(conn);
                    continue;
                }
            }
            // Detached worker — small per-connection thread is fine; the
            // protocol is request/response and clients close immediately.
            std::thread([this, conn, useTls]() { handleConnection(conn, useTls); }).detach();
        }
        KUJHAD_CLOSESOCK(srv);
        listenerOk_ = false;
#ifdef _WIN32
        WSACleanup();
#endif
    }

    void handleConnection(kujhad_socket_t rawSock, bool useTls) {
        // Reasonable receive timeout so an idle peer doesn't pin the worker.
#ifndef _WIN32
        timeval tv{}; tv.tv_sec = 10; tv.tv_usec = 0;
        ::setsockopt(rawSock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
        // Wrap the accepted socket. KujhadConnection owns the lifetime
        // from here on; falling out of scope closes both the SSL handle
        // and the underlying socket. On TLS handshake failure the
        // connection is already closed by kujhadAcceptTls. The
        // SSL_CTX inside tlsCtx_ can be replaced by a concurrent
        // setTlsConfig() (operator clicks "Regenerate"), so we hold
        // mtx_ across SSL_accept to keep the context alive for the
        // duration of the handshake. Handshakes are tens of ms; the
        // brief serialisation is acceptable for a fleet console.
        KujhadConnection conn;
        if (useTls) {
            std::lock_guard<std::mutex> lk(mtx_);
            // Re-check tlsEnabled_ under the lock — the operator may
            // have flipped TLS off between accept() and here.
            if (!tlsEnabled_ || !tlsCtx_.valid()) {
                KUJHAD_CLOSESOCK(rawSock);
                return;
            }
            conn = kujhadAcceptTls(tlsCtx_, rawSock);
            if (!conn.valid()) return;
        } else {
            conn.sock = rawSock;
        }
        KujhadHttpRequest req;
        bool ok = kujhadParseRequest(conn, req);
        inboundRequests_++;
        KujhadHttpResponse res;
        if (!ok) {
            res.status = 400;
            res.body = "{\"error\":\"bad request\"}";
            kujhadSendResponse(conn, res);
            return;
        }
        // CORS preflight. Always answer; auth check skipped on OPTIONS.
        if (req.method == "OPTIONS") {
            res.status = 204;
            res.contentType = "text/plain";
            res.body.clear();
            kujhadSendResponse(conn, res);
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
        else if (req.method == "GET" && path == "/v1/spectrum") {
            // Long-lived chunked NDJSON stream of downsampled FFT frames.
            // Each chunk is one JSON object terminated by a newline so a
            // controller can parse them line-at-a-time without state.
            // Server-side rate limiting bounds bandwidth on slow links.
            if (!spectrumProvider_) {
                res.status = 503;
                res.body = "{\"error\":\"spectrum stream unavailable\"}";
                kujhadSendResponse(conn, res);
                return;
            }
            const char* hdr =
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: application/x-ndjson\r\n"
                "Transfer-Encoding: chunked\r\n"
                "Cache-Control: no-cache\r\n"
                "Connection: close\r\n"
                "X-Accel-Buffering: no\r\n"
                "Access-Control-Allow-Origin: *\r\n"
                "\r\n";
            if (!kujhadWriteAll(conn, hdr, (int)std::strlen(hdr))) {
                return;
            }
            // RAII guard for the active-stream counter so the decrement
            // always pairs with the increment, even if the loop body
            // throws or returns early through a future code path.
            struct StreamCountGuard {
                std::atomic<int>& c;
                explicit StreamCountGuard(std::atomic<int>& v) : c(v) { c++; }
                ~StreamCountGuard() { c--; }
            } streamGuard(activeSpectrumStreams_);
            int interval = spectrumIntervalMs_.load();
            if (interval < 50) interval = 50;
            uint64_t lastSerial = 0;
            while (!stopFlag_.load()) {
                kujhad_json frame;
                bool got = false;
                try { got = spectrumProvider_(frame); } catch (...) { got = false; }
                if (got) {
                    // Skip identical frames (provider may return the same
                    // snapshot repeatedly when the FFT thread hasn't ticked).
                    uint64_t serial = frame.is_object() && frame.contains("serial") && frame["serial"].is_number_unsigned()
                                      ? frame["serial"].get<uint64_t>() : 0;
                    if (serial == 0 || serial != lastSerial) {
                        lastSerial = serial;
                        std::string body = frame.dump();
                        body.push_back('\n');
                        char chunkHdr[32];
                        int hn = std::snprintf(chunkHdr, sizeof(chunkHdr), "%X\r\n", (unsigned)body.size());
                        if (!kujhadWriteAll(conn, chunkHdr, hn)) break;
                        if (!kujhadWriteAll(conn, body.data(), (int)body.size())) break;
                        const char* trailer = "\r\n";
                        if (!kujhadWriteAll(conn, trailer, 2)) break;
                    }
                }
                // Sleep in small slices so a stop() can tear us down.
                int slept = 0;
                int slice = 25;
                while (slept < interval && !stopFlag_.load()) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(slice));
                    slept += slice;
                }
                // Refresh interval each tick — operator may have changed it.
                interval = spectrumIntervalMs_.load();
                if (interval < 50) interval = 50;
            }
            const char* end = "0\r\n\r\n";
            kujhadWriteAll(conn, end, 5);
            // streamGuard's destructor decrements activeSpectrumStreams_.
            return;
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
    }

    int port_ = 0;
    std::atomic<bool> running_{false};
    std::atomic<bool> stopFlag_{false};
    std::atomic<bool> listenerOk_{false};
    std::atomic<int>  inboundRequests_{0};
    std::atomic<int>  inboundCommands_{0};
    std::atomic<int>  rejectedCommands_{0};

    std::thread worker_;
    mutable std::mutex mtx_;
    std::string apiKey_;
    bool tlsEnabled_ = false;
    std::string tlsCertPath_;
    std::string tlsKeyPath_;
    KujhadServerTlsContext tlsCtx_;

    mutable std::mutex statusMtx_;
    std::string statusMsg_ = "Idle";

    SnapshotProvider identifyProvider_;
    SnapshotProvider stateProvider_;
    SnapshotProvider gpsProvider_;
    std::function<kujhad_json(uint64_t)> eventsProvider_;
    CommandHandler commandHandler_;
    SpectrumProvider spectrumProvider_;
    std::atomic<int> spectrumIntervalMs_{200};
    std::atomic<int> activeSpectrumStreams_{0};
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

    // Start polling a peer. When `useTls` is true the worker speaks TLS
    // and refuses to send the API key unless the peer cert's SHA-256
    // fingerprint matches `pinnedFingerprint` (case- and separator-
    // insensitive). An empty pin means "trust on first use" — the
    // first observed fingerprint is reported via lastSeenFingerprint()
    // so the operator can copy it into the peer config to lock in the
    // pin. When `useTls` is false the existing plain-HTTP path runs
    // unchanged; safe for loopback / overlay-only deployments.
    void start(const std::string& host, int port, const std::string& apiKey,
               bool useTls = false, const std::string& pinnedFingerprint = "") {
        stop();
        host_ = host;
        port_ = port;
        apiKey_ = apiKey;
        useTls_ = useTls;
        pinnedFingerprint_ = kujhadNormaliseFingerprint(pinnedFingerprint);
        {
            std::lock_guard<std::mutex> lk(snapMtx_);
            snap_ = KujhadPeerSnapshot{};
            lastSeenFingerprint_.clear();
            fingerprintMismatch_ = false;
        }
        stopFlag_ = false;
        running_ = true;
        worker_ = std::thread([this]() { workerLoop(); });
    }

    // Backwards-compat overload kept for legacy call sites that don't
    // care about TLS yet.
    void start(const std::string& host, int port, const std::string& apiKey) {
        start(host, port, apiKey, false, std::string());
    }

    bool tlsEnabled() const { return useTls_; }

    // Most recently observed peer cert fingerprint (after a successful
    // TLS handshake). Empty when TLS is off, the peer is unreachable,
    // or no handshake has succeeded yet.
    std::string lastSeenFingerprint() const {
        std::lock_guard<std::mutex> lk(snapMtx_);
        return lastSeenFingerprint_;
    }

    // True when the most recent TLS handshake produced a fingerprint
    // different from the pinned one. Reset to false on a clean
    // (matching) handshake.
    bool fingerprintMismatch() const {
        std::lock_guard<std::mutex> lk(snapMtx_);
        return fingerprintMismatch_;
    }

    void stop() {
        if (!running_.load()) {
            if (worker_.joinable()) worker_.join();
            stopSpectrum();
            return;
        }
        stopFlag_ = true;
        running_ = false;
        if (worker_.joinable()) worker_.join();
        stopSpectrum();
    }

    bool isRunning() const { return running_.load(); }

    // Open a long-lived /v1/spectrum subscription on the same host/key.
    // The worker thread parses chunked NDJSON frames and stores the
    // most recent one for the UI thread to read via latestSpectrum().
    // Idempotent: a second start() restarts the worker.
    void startSpectrum() {
        stopSpectrum();
        spectrumStop_ = false;
        spectrumActive_ = true;
        spectrumWorker_ = std::thread([this]() { spectrumLoop(); });
    }

    void stopSpectrum() {
        if (!spectrumActive_.load()) {
            if (spectrumWorker_.joinable()) spectrumWorker_.join();
            return;
        }
        spectrumStop_ = true;
        spectrumActive_ = false;
        if (spectrumWorker_.joinable()) spectrumWorker_.join();
        std::lock_guard<std::mutex> lk(spectrumMtx_);
        spectrumStreaming_ = false;
    }

    bool spectrumActive() const { return spectrumActive_.load(); }
    bool spectrumStreaming() const {
        std::lock_guard<std::mutex> lk(spectrumMtx_);
        return spectrumStreaming_;
    }
    uint64_t spectrumFramesReceived() const { return spectrumFramesReceived_.load(); }

    // Returns true if a frame has ever been received and copies it into out.
    bool latestSpectrum(KujhadSpectrumFrame& out) const {
        std::lock_guard<std::mutex> lk(spectrumMtx_);
        if (!spectrumHaveFrame_) return false;
        out = spectrumLatest_;
        return true;
    }

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

    // Open a TCP connection to host_:port_, then upgrade to TLS when
    // useTls_ is set. On a successful TLS handshake the peer cert
    // fingerprint is captured and compared against pinnedFingerprint_;
    // a mismatch closes the connection and surfaces an error in the
    // snapshot so the operator can re-pin (or investigate). Returns an
    // owning KujhadConnection; check valid() before using.
    KujhadConnection openConnection(int timeoutMs, std::string& errOut) {
        KujhadConnection conn;
        kujhad_socket_t sock = ::socket(AF_INET, SOCK_STREAM, 0);
        if (sock == KUJHAD_INVALID_SOCK) { errOut = "socket() failed"; return conn; }
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
            errOut = "getaddrinfo failed";
            return conn;
        }
        bool connected = (::connect(sock, res->ai_addr, (socklen_t)res->ai_addrlen) == 0);
        ::freeaddrinfo(res);
        if (!connected) {
            KUJHAD_CLOSESOCK(sock);
            errOut = "connect refused";
            return conn;
        }
        if (useTls_) {
            std::string seenFp;
            std::string tlsErr;
            if (!kujhadConnectTls(sock, conn, seenFp, tlsErr)) {
                // kujhadConnectTls leaves sock un-owned on failure.
                KUJHAD_CLOSESOCK(sock);
                errOut = tlsErr;
                return KujhadConnection();
            }
            // Capture the observed fingerprint for the UI either way —
            // even on mismatch we want the operator to see what the
            // peer presented so they can decide whether to re-pin.
            std::string seenNorm = kujhadNormaliseFingerprint(seenFp);
            bool mismatch = false;
            {
                std::lock_guard<std::mutex> lk(snapMtx_);
                lastSeenFingerprint_ = seenFp;
                if (!pinnedFingerprint_.empty() && seenNorm != pinnedFingerprint_) {
                    fingerprintMismatch_ = true;
                    snap_.lastError = "cert fingerprint mismatch (pinned=" + pinnedFingerprint_ +
                                       ", seen=" + seenNorm + ")";
                    mismatch = true;
                } else {
                    fingerprintMismatch_ = false;
                }
            }
            if (mismatch) {
                // Refuse to send the API key over a connection whose
                // identity we can't verify. Close before returning.
                conn.close();
                errOut = "cert fingerprint mismatch";
                return KujhadConnection();
            }
        } else {
            conn.sock = sock;
        }
        return conn;
    }

    // Open a fresh connection per request — peers are local, the
    // request volume is tiny, and per-connection state keeps the code
    // simple. Returns true on a parsed response.
    bool doRequest(const std::string& method, const std::string& path,
                   const std::string& body, KujhadHttpResponse& out, int timeoutMs) {
        std::string err;
        KujhadConnection conn = openConnection(timeoutMs, err);
        if (!conn.valid()) {
            if (!err.empty()) {
                std::lock_guard<std::mutex> lk(snapMtx_);
                if (snap_.lastError.empty()) snap_.lastError = err;
            }
            return false;
        }
        std::string keyHeader = std::string("X-Kujhad-Key: ") + apiKey_ + "\r\n";
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
        if (!kujhadWriteAll(conn, header, n)) return false;
        if (!body.empty() && !kujhadWriteAll(conn, body.data(), (int)body.size())) {
            return false;
        }
        // Read response.
        std::string buf;
        char chunk[1024];
        for (;;) {
            int r = conn.recvOnce(chunk, sizeof(chunk));
            if (r <= 0) break;
            buf.append(chunk, r);
            if (buf.size() > (1 << 20)) break; // 1 MiB cap
        }
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

    // Long-poll /v1/spectrum and decode chunked NDJSON frames. Runs on a
    // dedicated worker so the main poll loop is unaffected by the longer
    // socket lifetime. Reconnects with backoff on transport failure.
    void spectrumLoop() {
#ifdef _WIN32
        WSADATA wsa; WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
        while (!spectrumStop_.load()) {
            // 1s connect/handshake timeout keeps the worker responsive
            // to spectrumStop_ when the peer is unreachable.
            std::string err;
            KujhadConnection conn = openConnection(1000, err);
            if (!conn.valid()) {
                spectrumBackoff();
                continue;
            }
            char req[1024];
            int rn = std::snprintf(req, sizeof(req),
                "GET /v1/spectrum HTTP/1.1\r\n"
                "Host: %s:%d\r\n"
                "X-Kujhad-Key: %s\r\n"
                "Accept: application/x-ndjson\r\n"
                "Connection: close\r\n"
                "\r\n",
                host_.c_str(), port_, apiKey_.c_str());
            if (!kujhadWriteAll(conn, req, rn)) {
                spectrumBackoff();
                continue;
            }
            // Buffered reader bound to this connection + stop flag.
            std::string buf;
            auto pump = [&]() -> bool {
                char tmp[4096];
                int r = conn.recvOnce(tmp, sizeof(tmp));
                if (r > 0) { buf.append(tmp, r); return true; }
                if (r == 0) return false;
#ifndef _WIN32
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    return !spectrumStop_.load();
                }
#endif
                return false;
            };
            auto readLine = [&](std::string& out) -> bool {
                while (true) {
                    size_t i = buf.find("\r\n");
                    if (i != std::string::npos) {
                        out = buf.substr(0, i);
                        buf.erase(0, i + 2);
                        return true;
                    }
                    if (buf.size() > 65536) return false;
                    if (!pump()) return false;
                    if (spectrumStop_.load()) return false;
                }
            };
            auto readBytes = [&](size_t n, std::string& out) -> bool {
                while (buf.size() < n) {
                    if (!pump()) return false;
                    if (spectrumStop_.load()) return false;
                }
                out = buf.substr(0, n);
                buf.erase(0, n);
                return true;
            };
            // Drain HTTP headers until blank line.
            bool ok200 = false;
            bool firstLine = true;
            std::string line;
            while (!spectrumStop_.load() && readLine(line)) {
                if (firstLine) {
                    firstLine = false;
                    // "HTTP/1.1 200 OK"
                    if (line.find(" 200 ") != std::string::npos) ok200 = true;
                }
                if (line.empty()) break;
            }
            if (!ok200) {
                spectrumBackoff();
                continue;
            }
            {
                std::lock_guard<std::mutex> lk(spectrumMtx_);
                spectrumStreaming_ = true;
            }
            // Read chunked body. Each chunk: <hex size>\r\n<data>\r\n.
            bool brokeOut = false;
            while (!spectrumStop_.load()) {
                if (!readLine(line)) { brokeOut = true; break; }
                if (line.empty()) continue;
                // Chunk extension after ';' is optional, skip it.
                size_t semi = line.find(';');
                std::string sizeStr = (semi == std::string::npos) ? line : line.substr(0, semi);
                unsigned long sz = std::strtoul(sizeStr.c_str(), nullptr, 16);
                if (sz == 0) { brokeOut = true; break; }
                if (sz > (1u << 20)) { brokeOut = true; break; }
                std::string body;
                if (!readBytes((size_t)sz, body)) { brokeOut = true; break; }
                std::string trailer;
                if (!readBytes(2, trailer)) { brokeOut = true; break; }
                // Parse JSON line into a frame.
                try {
                    kujhad_json j = kujhad_json::parse(body);
                    if (!j.is_object()) continue;
                    KujhadSpectrumFrame f;
                    f.serial = j.value("serial", (uint64_t)0);
                    f.tsMs = j.value("tsMs", (uint64_t)0);
                    f.centerFreq = j.value("centerFreq", 0.0);
                    f.bandwidth = j.value("bandwidth", 0.0);
                    f.fftMinDb = (float)j.value("fftMinDb", -120.0);
                    f.fftMaxDb = (float)j.value("fftMaxDb", 0.0);
                    if (j.contains("bins") && j["bins"].is_array()) {
                        f.bins.reserve(j["bins"].size());
                        for (auto& v : j["bins"]) {
                            if (v.is_number()) f.bins.push_back((float)v.get<double>());
                        }
                    }
                    {
                        std::lock_guard<std::mutex> lk(spectrumMtx_);
                        spectrumLatest_ = std::move(f);
                        spectrumHaveFrame_ = true;
                    }
                    spectrumFramesReceived_++;
                } catch (...) {
                    // Bad frame — drop and continue.
                }
            }
            (void)brokeOut;
            {
                std::lock_guard<std::mutex> lk(spectrumMtx_);
                spectrumStreaming_ = false;
            }
            // conn destructor closes SSL + socket on scope exit.
            if (!spectrumStop_.load()) spectrumBackoff();
        }
#ifdef _WIN32
        WSACleanup();
#endif
    }

    void spectrumBackoff() {
        // ~1s backoff broken into 25ms slices so stop unblocks fast.
        for (int i = 0; i < 40 && !spectrumStop_.load(); i++) {
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }
    }

    std::string host_;
    int port_ = 0;
    std::string apiKey_;
    bool useTls_ = false;
    // Pre-normalised hex (no separators, lowercase) for fast compare.
    std::string pinnedFingerprint_;

    std::atomic<bool> running_{false};
    std::atomic<bool> stopFlag_{false};
    std::thread worker_;

    mutable std::mutex snapMtx_;
    KujhadPeerSnapshot snap_;
    std::string lastSeenFingerprint_;
    bool fingerprintMismatch_ = false;

    std::mutex eventMtx_;
    std::queue<kujhad_json> events_;

    // Spectrum subscriber state.
    std::atomic<bool> spectrumActive_{false};
    std::atomic<bool> spectrumStop_{false};
    std::thread spectrumWorker_;
    mutable std::mutex spectrumMtx_;
    KujhadSpectrumFrame spectrumLatest_;
    bool spectrumHaveFrame_ = false;
    bool spectrumStreaming_ = false;
    std::atomic<uint64_t> spectrumFramesReceived_{0};
};

} // namespace predator
