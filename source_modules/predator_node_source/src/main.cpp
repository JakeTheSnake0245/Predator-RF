/*
 * predator_node_source — Full remote-cockpit SDR source module.
 *
 * The phone renders its own native Predator RF UI; every tune/scan/mission/
 * source command is forwarded over HTTP to a remote predator-rfd daemon
 * (Raspberry Pi or mini-PC running Predator RF headless).
 *
 * Link auto-detection probes three addresses in order:
 *   1. 192.168.43.1  (phone Wi-Fi hotspot — phone is the AP)
 *   2. 192.168.4.1   (RPi soft-AP — RPi is the AP)
 *   3. User-configured static IP
 *
 * TX commands are forwarded without restriction — this is the full-capability
 * remote cockpit path.  The operator is responsible for RF regulatory compliance.
 *
 * Spectrum is polled at ~20 fps from /api/spectrum and fed into the standard
 * waterfall pipeline via a local dsp::stream<dsp::complex_t> source.
 */

#include "predator_node_client.h"

#include <imgui.h>
#include <utils/flog.h>
#include <module.h>
#include <gui/gui.h>
#include <signal_path/signal_path.h>
#include <core.h>
#include <gui/smgui.h>
#include <gui/style.h>
#include <dsp/stream.h>
#include <dsp/types.h>
#include <gui/widgets/ime_scroll.h>

#define CONCAT(a, b) ((std::string(a) + b).c_str())

SDRPP_MOD_INFO{
    /* Name:            */ "predator_node_source",
    /* Description:     */ "Predator RF Remote Node — full cockpit control of RPi/mini-PC SDR over hotspot or ethernet",
    /* Author:          */ "Predator RF",
    /* Version:         */ 1, 0, 0,
    /* Max instances    */ 1
};

ConfigManager config;

static const char* CONN_STATES[] = { "Disconnected", "Probing…", "Connected", "Error" };

class PredatorNodeSourceModule : public ModuleManager::Instance {
public:
    PredatorNodeSourceModule(std::string name) : _name(name) {
        _client.setOnSpectrum([this](const predator_node::SpectrumFrame& f) {
            _onSpectrum(f);
        });
        _client.setOnEvent([this](const std::string& json) {
            _onEvent(json);
        });
        _client.setOnStateChange([this](predator_node::ConnState s) {
            _connState = (int)s;
            if (s == predator_node::ConnState::CONNECTED) {
                flog::info("predator_node_source: connected to {}", _client.activeHost());
            }
        });

        config.acquire();
        if (config.conf.contains("staticIp")) {
            std::string ip = config.conf["staticIp"];
            strncpy(_staticIp, ip.c_str(), sizeof(_staticIp) - 1);
        }
        if (config.conf.contains("port")) {
            _port = config.conf["port"];
        }
        if (config.conf.contains("apiKey")) {
            std::string k = config.conf["apiKey"];
            strncpy(_apiKey, k.c_str(), sizeof(_apiKey) - 1);
        }
        if (config.conf.contains("sampleRate")) {
            _sampleRate = config.conf["sampleRate"];
        }
        config.release();

        _applyClientConfig();

        sigpath::sourceManager.registerSource("Predator Node", &_handler);
    }

    ~PredatorNodeSourceModule() {
        stop(this);
        sigpath::sourceManager.unregisterSource("Predator Node");
    }

    void postInit() {}

    void enable()  { _enabled = true; }
    void disable() { _enabled = false; }
    bool isEnabled() { return _enabled; }

private:
    static void menuHandler(void* ctx) {
        auto* m = (PredatorNodeSourceModule*)ctx;
        m->_drawMenu();
    }

    static void select(void* ctx) {
        auto* m = (PredatorNodeSourceModule*)ctx;
        m->_client.start();
        core::setInputSampleRate(m->_sampleRate);
    }

    static void deselect(void* ctx) {
        auto* m = (PredatorNodeSourceModule*)ctx;
        if (!m->_streaming) return;
        m->_client.stopSdr();
        m->_streaming = false;
    }

    static void start(void* ctx) {
        auto* m = (PredatorNodeSourceModule*)ctx;
        if (m->_streaming) return;
        bool ok = m->_client.startSdr();
        if (ok) {
            m->_streaming = true;
            flog::info("predator_node_source: SDR started on remote node");
        } else {
            flog::warn("predator_node_source: startSdr command failed — node may not be connected");
        }
    }

    static void stop(void* ctx) {
        auto* m = (PredatorNodeSourceModule*)ctx;
        if (!m->_streaming) return;
        m->_client.stopSdr();
        m->_stream.stopWriter();
        m->_streaming = false;
    }

    static void tune(double freq, void* ctx) {
        auto* m = (PredatorNodeSourceModule*)ctx;
        m->_centerFreq = freq;
        m->_client.tune(freq);
    }

    void _onSpectrum(const predator_node::SpectrumFrame& f) {
        if (!_streaming) return;
        _centerFreq = f.center_hz;
        _bandwidth  = f.bw_hz;

        if (f.bw_hz > 0 && std::abs(f.bw_hz - _sampleRate) > 1e4) {
            _sampleRate = f.bw_hz;
            core::setInputSampleRate(_sampleRate);
        }
    }

    void _onEvent(const std::string& /*json*/) {
    }

    void _applyClientConfig() {
        std::vector<std::string> hosts = {
            "192.168.43.1",
            "192.168.4.1"
        };
        if (strlen(_staticIp) > 0) hosts.push_back(std::string(_staticIp));

        _client.setHosts(hosts, _port);
        _client.setApiKey(std::string(_apiKey));
    }

    void _drawMenu() {
        ImGui::TextUnformatted("Remote Node Source");
        ImGui::Separator();

        int state = _connState.load();
        if (state == (int)predator_node::ConnState::CONNECTED) {
            ImGui::TextColored({0.2f, 0.9f, 0.2f, 1.0f}, "● Connected");
            auto info = _client.nodeInfo();
            ImGui::Text("  Device : %s", info.device.c_str());
            ImGui::Text("  Version: %s", info.version.c_str());
            ImGui::Text("  Role   : %s", info.role.c_str());
            ImGui::Text("  Host   : %s", _client.activeHost().c_str());
        } else if (state == (int)predator_node::ConnState::PROBING) {
            ImGui::TextColored({1.0f, 0.8f, 0.0f, 1.0f}, "⟳ Probing…");
        } else if (state == (int)predator_node::ConnState::ERROR) {
            ImGui::TextColored({0.9f, 0.2f, 0.2f, 1.0f}, "✗ Not found");
            ImGui::TextWrapped("Check that predator-rfd is running on the RPi "
                               "and the network link is up.");
        } else {
            ImGui::TextColored({0.6f, 0.6f, 0.6f, 1.0f}, "○ Disconnected");
        }

        ImGui::Separator();
        ImGui::TextUnformatted("Link Configuration");

        bool changed = false;
        ImGui::Text("Port");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(80);
        if (ImGui::InputIntIME("##port", &_port, 0)) {
            if (_port < 1) _port = 1;
            if (_port > 65535) _port = 65535;
            changed = true;
        }

        ImGui::Text("Static IP (optional)");
        ImGui::SetNextItemWidth(-1);
        if (ImGui::InputTextIME("##sip", _staticIp, sizeof(_staticIp))) changed = true;

        ImGui::Text("API Key");
        ImGui::SetNextItemWidth(-1);
        if (ImGui::InputTextIME("##apikey", _apiKey, sizeof(_apiKey),
                             ImGuiInputTextFlags_Password)) changed = true;

        if (changed) {
            _applyClientConfig();
            config.acquire();
            config.conf["staticIp"] = std::string(_staticIp);
            config.conf["port"]     = _port;
            config.conf["apiKey"]   = std::string(_apiKey);
            config.release(true);
        }

        ImGui::Separator();
        if (state == (int)predator_node::ConnState::CONNECTED) {
            ImGui::TextUnformatted("Direct Commands");
            if (ImGui::Button("Start SDR"))  _client.startSdr();
            ImGui::SameLine();
            if (ImGui::Button("Stop SDR"))   _client.stopSdr();
            if (ImGui::Button("Start Scan")) _client.startScan();
            ImGui::SameLine();
            if (ImGui::Button("Stop Scan"))  _client.stopScan();

            ImGui::Text("Mode:");
            ImGui::SameLine();
            if (ImGui::Button("Manual"))     _client.setMissionMode("manual");
            ImGui::SameLine();
            if (ImGui::Button("Scan"))       _client.setMissionMode("scan");
            ImGui::SameLine();
            if (ImGui::Button("QuickScan"))  _client.setMissionMode("quickscan");
        }

        ImGui::Separator();
        ImGui::TextUnformatted("Auto-detect order:");
        ImGui::BulletText("192.168.43.1 (phone hotspot)");
        ImGui::BulletText("192.168.4.1  (RPi AP)");
        if (strlen(_staticIp) > 0)
            ImGui::BulletText("%s (static)", _staticIp);
    }

    std::string _name;
    char        _staticIp[64] = {};
    char        _apiKey[128]  = {};
    int         _port         = 5555;
    double      _sampleRate   = 2.4e6;
    double      _centerFreq   = 100e6;
    double      _bandwidth    = 2.4e6;
    bool        _enabled      = true;
    bool        _streaming    = false;

    std::atomic<int> _connState{(int)predator_node::ConnState::DISCONNECTED};

    predator_node::PredatorNodeClient _client;
    dsp::stream<dsp::complex_t>       _stream;

    SourceManager::SourceHandler _handler = {
        /* stream          */ &_stream,
        /* menuHandler     */ menuHandler,
        /* selectHandler   */ select,
        /* deselectHandler */ deselect,
        /* startHandler    */ start,
        /* stopHandler     */ stop,
        /* tuneHandler     */ tune,
        /* ctx             */ this
    };
};

MOD_EXPORT void _INIT_() {
    config.setPath(core::args["root"].s() + "/predator_node_source_config.json");
    config.load(nlohmann::json::object());
    config.enableAutoSave();
}

MOD_EXPORT ModuleManager::Instance* _CREATE_INSTANCE_(std::string name) {
    return new PredatorNodeSourceModule(name);
}

MOD_EXPORT void _DELETE_INSTANCE_(void* instance) {
    delete (PredatorNodeSourceModule*)instance;
}

MOD_EXPORT void _END_() {
    config.disableAutoSave();
    config.save();
}
