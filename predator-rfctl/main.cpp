// predator-rfctl — CLI tool for predator-rfd (web backend daemon).
//
// Connects to the Unix control socket at /run/predator-rfd/control.sock
// (or $PREDATOR_CTRL_SOCK), sends a typed JSON command, and prints the
// JSON response to stdout.
//
// Usage:
//   predator-rfctl state
//   predator-rfctl identify
//   predator-rfctl tune 433.92e6
//   predator-rfctl scan start
//   predator-rfctl scan stop
//   predator-rfctl mission set-mode classify
//   predator-rfctl events [--since N]
//   predator-rfctl raw '{"class":"tune","action":"set","args":{"freq":433920000}}'

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <sstream>

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
        "  state                        Print daemon state snapshot\n"
        "  identify                     Print device identity\n"
        "  tune <freq_hz>               Retune the SDR to freq (Hz; accepts 433.92e6)\n"
        "  scan start                   Start mission scan\n"
        "  scan stop                    Stop mission scan\n"
        "  scan pause                   Pause mission scan\n"
        "  mission set-mode <mode>      Set mission mode (manual|classify|scan|quickscan)\n"
        "  events [--since N]           Fetch event log (optional since=id cursor)\n"
        "  raw <json>                   Send raw JSON command\n"
        "\n"
        "Environment:\n"
        "  PREDATOR_CTRL_SOCK           Override control socket path\n",
        argv0);
}

struct Cmd {
    std::string cls;
    std::string action;
    std::string argsJson;
};

static std::string buildJson(const Cmd& c) {
    std::string j = "{\"class\":\"" + c.cls + "\",\"action\":\"" + c.action + "\",\"args\":" +
                    (c.argsJson.empty() ? "{}" : c.argsJson) + "}";
    return j;
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
    std::strncpy(addr.sun_path, sockPath.c_str(), sizeof(addr.sun_path) - 1);
    if(::connect(s, (sockaddr*)&addr, sizeof(addr)) != 0) {
        std::fprintf(stderr, "error: connect %s: %s\n", sockPath.c_str(), ::strerror(errno));
        ::close(s);
        return "";
    }
    if(::send(s, msg.c_str(), (int)msg.size(), 0) < 0) {
        std::fprintf(stderr, "error: send: %s\n", ::strerror(errno));
        ::close(s);
        return "";
    }
    // Half-close so the server sees EOF
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

// Minimal JSON double-quote escape (no full parser needed for argv)
static std::string jsStr(const std::string& s) {
    std::string out = "\"";
    for(char c : s) {
        if(c=='"') out+="\\\"";
        else if(c=='\\') out+="\\\\";
        else if(c=='\n') out+="\\n";
        else out+=c;
    }
    out += "\"";
    return out;
}

int main(int argc, char* argv[]) {
    std::string sockPath = DEFAULT_SOCK;
    const char* env = ::getenv("PREDATOR_CTRL_SOCK");
    if(env && *env) sockPath = env;

    // Parse global flags
    int argi = 1;
    while(argi < argc && argv[argi][0] == '-') {
        if(std::strcmp(argv[argi], "--sock") == 0 && argi + 1 < argc) {
            sockPath = argv[++argi];
            argi++;
        } else {
            std::fprintf(stderr, "unknown flag: %s\n", argv[argi]);
            usage(argv[0]); return 1;
        }
    }

    if(argi >= argc) { usage(argv[0]); return 1; }
    std::string verb = argv[argi++];

    Cmd cmd;
    std::string rawJson;

    if(verb == "state") {
        cmd = {"query", "state", "{}"};
    } else if(verb == "identify") {
        cmd = {"query", "identify", "{}"};
    } else if(verb == "tune") {
        if(argi >= argc) {
            std::fprintf(stderr, "error: tune requires a frequency\n");
            return 1;
        }
        double freq = std::strtod(argv[argi++], nullptr);
        std::ostringstream a;
        a << "{\"freq\":" << (long long)freq << "}";
        cmd = {"tune", "set", a.str()};
    } else if(verb == "scan") {
        if(argi >= argc) {
            std::fprintf(stderr, "error: scan requires start|stop|pause\n");
            return 1;
        }
        std::string sub = argv[argi++];
        cmd = {"scan", sub, "{}"};
    } else if(verb == "mission") {
        if(argi >= argc) {
            std::fprintf(stderr, "error: mission requires a sub-command\n");
            return 1;
        }
        std::string sub = argv[argi++];
        if(sub == "set-mode") {
            if(argi >= argc) {
                std::fprintf(stderr, "error: set-mode requires a mode name\n");
                return 1;
            }
            std::string mode = argv[argi++];
            cmd = {"mission", "set-mode", "{\"mode\":" + jsStr(mode) + "}"};
        } else {
            std::fprintf(stderr, "unknown mission sub-command: %s\n", sub.c_str());
            return 1;
        }
    } else if(verb == "events") {
        std::string since = "0";
        while(argi < argc) {
            if(std::strcmp(argv[argi], "--since") == 0 && argi + 1 < argc) {
                since = argv[++argi]; argi++;
            } else argi++;
        }
        cmd = {"query", "events", "{\"since\":" + since + "}"};
    } else if(verb == "raw") {
        if(argi >= argc) {
            std::fprintf(stderr, "error: raw requires a JSON string\n");
            return 1;
        }
        rawJson = argv[argi++];
    } else {
        std::fprintf(stderr, "unknown command: %s\n", verb.c_str());
        usage(argv[0]);
        return 1;
    }

    std::string msg = rawJson.empty() ? buildJson(cmd) : rawJson;
    std::string resp = sendRecv(sockPath, msg);
    if(resp.empty()) return 1;

    std::printf("%s\n", resp.c_str());
    return 0;
}
