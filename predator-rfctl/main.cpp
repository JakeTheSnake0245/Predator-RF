// predator-rfctl — CLI tool for the predator-rfd web backend daemon.
//
// Connects to the Unix control socket at /run/predator-rfd/control.sock
// (or $PREDATOR_CTRL_SOCK), sends a typed JSON command, and prints the
// JSON response to stdout. TX commands are hard-rejected.
//
// Usage:
//   predator-rfctl status
//   predator-rfctl identify
//   predator-rfctl tune 433.92e6
//   predator-rfctl scan start|stop|pause
//   predator-rfctl mission set-mode manual|classify|scan|quickscan
//   predator-rfctl role set device|controller
//   predator-rfctl role show
//   predator-rfctl key show
//   predator-rfctl key regenerate
//   predator-rfctl port show
//   predator-rfctl peer list
//   predator-rfctl peer add <name> <host> <port> <key>
//   predator-rfctl peer remove <name>
//   predator-rfctl events [--since N]
//   predator-rfctl start         (emit start-source command)
//   predator-rfctl stop          (emit stop-source command)
//   predator-rfctl raw <json>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sstream>
#include <vector>

#ifndef _WIN32
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <errno.h>
#endif

static const char* DEFAULT_SOCK = "/run/predator-rfd/control.sock";

static void usage(const char* argv0) {
    std::fprintf(stderr,
        "Usage: %s [--sock PATH] <command> [args...]\n"
        "\n"
        "Commands:\n"
        "  status                              Print full daemon status\n"
        "  identify                            Print device identity\n"
        "  tune <freq_hz>                      Retune SDR (Hz; accepts 433.92e6)\n"
        "  scan start|stop|pause               Control mission scan\n"
        "  mission set-mode <mode>             manual|classify|scan|quickscan\n"
        "  role show                           Show current role\n"
        "  role set device|controller          Set role\n"
        "  key show                            Show whether API key is configured\n"
        "  key regenerate                      Regenerate API key (returns new key)\n"
        "  port show                           Show web server port\n"
        "  peer list                           List known fleet peers\n"
        "  peer add <name> <host> <port> <key> Add a fleet peer\n"
        "  peer remove <name>                  Remove a fleet peer\n"
        "  events [--since N]                  Print events (optional cursor)\n"
        "  start                               Start SDR source\n"
        "  stop                                Stop SDR source\n"
        "  raw <json>                          Send raw JSON command\n"
        "\n"
        "Env:  PREDATOR_CTRL_SOCK  — override socket path\n",
        argv0);
}

// Minimal JSON escape (no parser; for building argv-sourced values only)
static std::string jsStr(const std::string& s) {
    std::string out = "\"";
    for(char c : s) {
        if(c=='"')  out+="\\\"";
        else if(c=='\\') out+="\\\\";
        else if(c=='\n') out+="\\n";
        else if(c=='\r') out+="\\r";
        else             out+=c;
    }
    out += "\"";
    return out;
}

static std::string buildCmd(const std::string& cls, const std::string& action,
                              const std::string& argsJson = "{}") {
    return "{\"class\":" + jsStr(cls) +
           ",\"action\":" + jsStr(action) +
           ",\"args\":" + argsJson + "}";
}

static std::string sendRecv(const std::string& sockPath, const std::string& msg) {
#ifndef _WIN32
    int s = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if(s < 0) {
        std::fprintf(stderr, "error: socket: %s\n", ::strerror(errno));
        return "";
    }
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    ::strncpy(addr.sun_path, sockPath.c_str(), sizeof(addr.sun_path) - 1);
    if(::connect(s, (sockaddr*)&addr, sizeof(addr)) != 0) {
        std::fprintf(stderr,
            "error: cannot connect to %s: %s\n"
            "  (Is predator-rfd running? Check: systemctl status predator-rfd)\n",
            sockPath.c_str(), ::strerror(errno));
        ::close(s);
        return "";
    }
    ::send(s, msg.c_str(), (int)msg.size(), 0);
    ::shutdown(s, SHUT_WR);
    std::string resp;
    char buf[4096];
    int n;
    while((n = (int)::recv(s, buf, sizeof(buf), 0)) > 0) resp.append(buf, n);
    ::close(s);
    return resp;
#else
    (void)sockPath; (void)msg;
    std::fprintf(stderr, "error: Unix sockets not supported on Windows\n");
    return "";
#endif
}

int main(int argc, char* argv[]) {
    std::string sockPath = DEFAULT_SOCK;
    const char* env = ::getenv("PREDATOR_CTRL_SOCK");
    if(env && *env) sockPath = env;

    int argi = 1;
    while(argi < argc && argv[argi][0] == '-' && argv[argi][1] == '-') {
        if(std::strcmp(argv[argi], "--sock") == 0 && argi + 1 < argc) {
            sockPath = argv[++argi]; argi++;
        } else {
            std::fprintf(stderr, "unknown flag: %s\n", argv[argi]);
            usage(argv[0]); return 1;
        }
    }

    if(argi >= argc) { usage(argv[0]); return 1; }
    std::string verb = argv[argi++];

    std::string msg;

    if(verb == "status") {
        msg = buildCmd("query", "status");
    } else if(verb == "identify") {
        msg = buildCmd("query", "identify");
    } else if(verb == "tune") {
        if(argi >= argc) {
            std::fprintf(stderr, "error: tune requires a frequency (e.g. 433.92e6)\n");
            return 1;
        }
        double freq = std::strtod(argv[argi++], nullptr);
        std::ostringstream a;
        a << "{\"freq\":" << (long long)freq << "}";
        msg = buildCmd("tune", "set", a.str());
    } else if(verb == "scan") {
        if(argi >= argc) {
            std::fprintf(stderr, "error: scan requires start|stop|pause\n");
            return 1;
        }
        std::string sub = argv[argi++];
        if(sub!="start" && sub!="stop" && sub!="pause") {
            std::fprintf(stderr, "error: scan sub-command must be start|stop|pause\n");
            return 1;
        }
        msg = buildCmd("scan", sub);
    } else if(verb == "mission") {
        if(argi >= argc) {
            std::fprintf(stderr, "error: mission requires a sub-command\n");
            return 1;
        }
        std::string sub = argv[argi++];
        if(sub == "set-mode") {
            if(argi >= argc) {
                std::fprintf(stderr, "error: set-mode requires manual|classify|scan|quickscan\n");
                return 1;
            }
            std::string mode = argv[argi++];
            msg = buildCmd("mission", "set-mode", "{\"mode\":" + jsStr(mode) + "}");
        } else {
            std::fprintf(stderr, "unknown mission sub-command: %s\n", sub.c_str());
            return 1;
        }
    } else if(verb == "role") {
        if(argi >= argc) {
            std::fprintf(stderr, "error: role requires show|set\n");
            return 1;
        }
        std::string sub = argv[argi++];
        if(sub == "show") {
            msg = buildCmd("query", "role");
        } else if(sub == "set") {
            if(argi >= argc) {
                std::fprintf(stderr, "error: role set requires device|controller\n");
                return 1;
            }
            std::string role = argv[argi++];
            msg = buildCmd("role", "set", "{\"role\":" + jsStr(role) + "}");
        } else {
            std::fprintf(stderr, "unknown role sub-command: %s\n", sub.c_str());
            return 1;
        }
    } else if(verb == "key") {
        if(argi >= argc) {
            std::fprintf(stderr, "error: key requires show|regenerate\n");
            return 1;
        }
        std::string sub = argv[argi++];
        if(sub == "show") {
            msg = buildCmd("query", "key");
        } else if(sub == "regenerate") {
            msg = buildCmd("key", "regenerate");
        } else {
            std::fprintf(stderr, "unknown key sub-command: %s\n", sub.c_str());
            return 1;
        }
    } else if(verb == "port") {
        if(argi >= argc) {
            std::fprintf(stderr, "error: port requires show\n");
            return 1;
        }
        std::string sub = argv[argi++];
        if(sub == "show") {
            msg = buildCmd("query", "port");
        } else {
            std::fprintf(stderr, "unknown port sub-command: %s\n", sub.c_str());
            return 1;
        }
    } else if(verb == "peer") {
        if(argi >= argc) {
            std::fprintf(stderr, "error: peer requires list|add|remove\n");
            return 1;
        }
        std::string sub = argv[argi++];
        if(sub == "list") {
            msg = buildCmd("query", "peers");
        } else if(sub == "add") {
            if(argi + 3 >= argc) {
                std::fprintf(stderr,
                    "error: peer add requires <name> <host> <port> <key>\n");
                return 1;
            }
            std::string name = argv[argi++];
            std::string host = argv[argi++];
            int         port = std::atoi(argv[argi++]);
            std::string key  = argv[argi++];
            std::ostringstream a;
            a << "{\"name\":" << jsStr(name)
              << ",\"host\":" << jsStr(host)
              << ",\"port\":" << port
              << ",\"key\":"  << jsStr(key) << "}";
            msg = buildCmd("peer", "add", a.str());
        } else if(sub == "remove") {
            if(argi >= argc) {
                std::fprintf(stderr, "error: peer remove requires a name\n");
                return 1;
            }
            std::string name = argv[argi++];
            msg = buildCmd("peer", "remove", "{\"name\":" + jsStr(name) + "}");
        } else {
            std::fprintf(stderr, "unknown peer sub-command: %s\n", sub.c_str());
            return 1;
        }
    } else if(verb == "events") {
        std::string since = "0";
        while(argi < argc) {
            if(std::strcmp(argv[argi], "--since") == 0 && argi + 1 < argc) {
                since = argv[argi + 1]; argi += 2;
            } else argi++;
        }
        msg = buildCmd("query", "events", "{\"since\":" + since + "}");
    } else if(verb == "start") {
        msg = buildCmd("source", "start");
    } else if(verb == "stop") {
        msg = buildCmd("source", "stop");
    } else if(verb == "raw") {
        if(argi >= argc) {
            std::fprintf(stderr, "error: raw requires a JSON string\n");
            return 1;
        }
        msg = argv[argi++];
    } else {
        std::fprintf(stderr, "unknown command: %s\n", verb.c_str());
        usage(argv[0]);
        return 1;
    }

    std::string resp = sendRecv(sockPath, msg);
    if(resp.empty()) return 1;
    std::printf("%s\n", resp.c_str());
    return 0;
}
