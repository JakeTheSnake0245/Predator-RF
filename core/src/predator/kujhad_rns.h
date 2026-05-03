#pragma once
// Predator RF — RNS daemon control socket helpers used by the Kujhad
// tab "RNS Interfaces" sub-panel. Wire format: line-delimited JSON
// over a Unix socket (/run/predator-rns.sock for root installs,
// $HOME/.local/state/predator-rns/control.sock otherwise). The full
// daemon API is documented in `backend/rns/README.md` and the field /
// method / UI parity matrix in `docs/rns_parity.md`.
//
// These helpers are deliberately header-only and have no JSON-library
// dependency — they assemble request lines as strings and return raw
// JSON response strings for the panel to parse with the same
// minimal helpers used elsewhere in the GUI. The panel itself
// renders every per-interface form in `core/src/gui/main_window.cpp`.
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

namespace predator {

inline std::string kujhadRnsSocketPath() {
    if (geteuid() == 0) return "/run/predator-rns.sock";
    const char* home = std::getenv("HOME");
    if (!home) home = "/tmp";
    return std::string(home) + "/.local/state/predator-rns/control.sock";
}

// One-shot request/response. Returns the raw JSON response (one line).
// On failure returns an empty string and writes the error into `errOut`.
inline std::string kujhadRnsRequest(const std::string& jsonLine,
                                     std::string& errOut) {
    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { errOut = "socket() failed"; return ""; }
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    auto path = kujhadRnsSocketPath();
    if (path.size() >= sizeof(addr.sun_path)) {
        errOut = "socket path too long"; ::close(fd); return "";
    }
    std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        errOut = "daemon down (could not connect to " + path + ")";
        ::close(fd);
        return "";
    }
    std::string out = jsonLine;
    if (out.empty() || out.back() != '\n') out += '\n';
    if (::send(fd, out.data(), out.size(), 0) < 0) {
        errOut = "send() failed"; ::close(fd); return "";
    }
    std::string resp;
    char buf[8192];
    while (true) {
        ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) break;
        resp.append(buf, buf + n);
        if (resp.find('\n') != std::string::npos) break;
    }
    ::close(fd);
    if (auto nl = resp.find('\n'); nl != std::string::npos) resp.resize(nl);
    return resp;
}

// JSON-string escape helper used to safely embed user-supplied form
// values into the request lines below. Header-only and minimal.
inline std::string kujhadRnsJsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 2);
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if ((unsigned char)c < 0x20) {
                    char tmp[8];
                    std::snprintf(tmp, sizeof(tmp), "\\u%04x", c);
                    out += tmp;
                } else {
                    out += c;
                }
        }
    }
    return out;
}

// Per-method helpers. Each one returns the daemon's response as a JSON
// string (never empty on success) or the error banner on failure.

inline std::string kujhadRnsCallStatus(std::string& errOut) {
    auto resp = kujhadRnsRequest(R"({"id":1,"method":"status"})", errOut);
    if (resp.empty()) return errOut;
    return resp;
}

inline std::string kujhadRnsListInterfaces(std::string& errOut) {
    auto resp = kujhadRnsRequest(
        R"({"id":2,"method":"list_interfaces"})", errOut);
    if (resp.empty()) return errOut;
    return resp;
}

inline std::string kujhadRnsGetInterface(const std::string& iid,
                                          std::string& errOut) {
    std::string req = std::string(R"({"id":3,"method":"get_interface","params":{"iid":")")
                      + kujhadRnsJsonEscape(iid) + R"("}})";
    auto resp = kujhadRnsRequest(req, errOut);
    if (resp.empty()) return errOut;
    return resp;
}

inline std::string kujhadRnsAddInterface(const std::string& cfgJson,
                                          std::string& errOut) {
    // cfgJson MUST already be a valid JSON object literal (the panel
    // builds it from the form fields). We pass it through as the
    // `cfg` param.
    std::string req = std::string(R"({"id":4,"method":"add_interface","params":{"cfg":)")
                      + cfgJson + R"(}})";
    auto resp = kujhadRnsRequest(req, errOut);
    if (resp.empty()) return errOut;
    return resp;
}

inline std::string kujhadRnsUpdateInterface(const std::string& iid,
                                             const std::string& cfgJson,
                                             std::string& errOut) {
    std::string req = std::string(R"({"id":5,"method":"update_interface","params":{"iid":")")
                      + kujhadRnsJsonEscape(iid) + R"(","cfg":)" + cfgJson + R"(}})";
    auto resp = kujhadRnsRequest(req, errOut);
    if (resp.empty()) return errOut;
    return resp;
}

inline std::string kujhadRnsRemoveInterface(const std::string& iid,
                                             std::string& errOut) {
    std::string req = std::string(R"({"id":6,"method":"remove_interface","params":{"iid":")")
                      + kujhadRnsJsonEscape(iid) + R"("}})";
    auto resp = kujhadRnsRequest(req, errOut);
    if (resp.empty()) return errOut;
    return resp;
}

inline std::string kujhadRnsSetEnabled(const std::string& iid, bool enabled,
                                        std::string& errOut) {
    std::string req = std::string(R"({"id":7,"method":"set_enabled","params":{"iid":")")
                      + kujhadRnsJsonEscape(iid) + R"(","enabled":)"
                      + (enabled ? "true" : "false") + R"(}})";
    auto resp = kujhadRnsRequest(req, errOut);
    if (resp.empty()) return errOut;
    return resp;
}

inline std::string kujhadRnsRestartInterface(const std::string& iid,
                                              std::string& errOut) {
    std::string req = std::string(R"({"id":8,"method":"restart_interface","params":{"iid":")")
                      + kujhadRnsJsonEscape(iid) + R"("}})";
    auto resp = kujhadRnsRequest(req, errOut);
    if (resp.empty()) return errOut;
    return resp;
}

inline std::string kujhadRnsRestartAll() {
    std::string err;
    auto resp = kujhadRnsRequest(R"({"id":9,"method":"restart_all"})", err);
    return resp.empty() ? err : resp;
}

inline std::string kujhadRnsValidate(const std::string& cfgJson,
                                      std::string& errOut) {
    std::string req = std::string(R"({"id":10,"method":"validate_interface","params":{"cfg":)")
                      + cfgJson + R"(}})";
    auto resp = kujhadRnsRequest(req, errOut);
    if (resp.empty()) return errOut;
    return resp;
}

inline std::string kujhadRnsExport(const std::string& passphrase,
                                    bool includeIdentity,
                                    std::string& errOut) {
    std::string req = std::string(R"({"id":11,"method":"export_config","params":{"passphrase":")")
                      + kujhadRnsJsonEscape(passphrase)
                      + R"(","include_identity":)"
                      + (includeIdentity ? "true" : "false")
                      + R"(}})";
    auto resp = kujhadRnsRequest(req, errOut);
    if (resp.empty()) return errOut;
    return resp;
}

inline std::string kujhadRnsImport(const std::string& token,
                                    const std::string& passphrase,
                                    const std::string& placeholdersJson,
                                    std::string& errOut) {
    std::string req = std::string(R"({"id":12,"method":"import_config","params":{"token":")")
                      + kujhadRnsJsonEscape(token)
                      + R"(","passphrase":")"
                      + kujhadRnsJsonEscape(passphrase)
                      + R"(","placeholders":)" + placeholdersJson + R"(}})";
    auto resp = kujhadRnsRequest(req, errOut);
    if (resp.empty()) return errOut;
    return resp;
}

inline std::string kujhadRnsMint(const std::string& passphrase,
                                  bool includeIdentity,
                                  std::string& errOut) {
    std::string req = std::string(R"({"id":13,"method":"mint_replication_token","params":{"new_passphrase":")")
                      + kujhadRnsJsonEscape(passphrase)
                      + R"(","include_identity":)"
                      + (includeIdentity ? "true" : "false")
                      + R"(}})";
    auto resp = kujhadRnsRequest(req, errOut);
    if (resp.empty()) return errOut;
    return resp;
}

inline std::string kujhadRnsLogs(const std::string& level,
                                  long sinceMs, int limit,
                                  std::string& errOut) {
    char num[64];
    std::snprintf(num, sizeof(num), R"("since_ms":%ld,"limit":%d)",
                  sinceMs, limit);
    std::string req = std::string(R"({"id":14,"method":"get_logs","params":{"level":")")
                      + kujhadRnsJsonEscape(level) + R"(",)" + num + R"(}})";
    auto resp = kujhadRnsRequest(req, errOut);
    if (resp.empty()) return errOut;
    return resp;
}

}  // namespace predator
