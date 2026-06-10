/*
    Predator RF — KrakenSDR Source module.

    KrakenSDR is a 5-channel coherent SDR that processes IQ samples
    internally and outputs Direction-of-Arrival (DOA) results via its
    krakensdr_doa companion process.  Raw IQ samples are NOT exposed to
    SDR++ — the hardware's USB interface is consumed exclusively by the
    daq_fw daemon which performs the coherent phase alignment.

    This module:
      • Registers "KrakenSDR" in the SDRPP source selector.
      • Displays an architecture overview panel and setup instructions.
      • Points to the kraken_lob_decoder module (the actual data path).
      • Start/stop/tune handlers are intentional no-ops; the source
        never produces IQ samples (the selector is informational only).

    Correct deployment:
      1. Flash KrakenSDR firmware; start daq_fw on the host RPi.
      2. Start krakensdr_doa (Python) pointing at daq_fw on port 5000.
      3. Enable the kraken_lob_decoder module in SDRPP Module Manager and
         set Host/Port to the krakensdr_doa WebSocket endpoint (default
         ws://127.0.0.1:8082/ws).
      4. LOB bearing events appear in the Predator RF Hits panel and are
         forwarded to the Python intelligence backend for triangulation.
*/

#define NOMINMAX
#include <imgui.h>
#include <module.h>
#include <gui/gui.h>
#include <signal_path/signal_path.h>
#include <utils/flog.h>
#include <dsp/stream.h>
#include <dsp/types.h>

#define CONCAT(a, b) ((std::string(a) + b).c_str())

SDRPP_MOD_INFO{
    /* Name:        */ "krakensdr_source",
    /* Description: */ "KrakenSDR Source (DOA array — IQ not available; use kraken_lob_decoder for bearing data)",
    /* Author:      */ "Predator",
    /* Version:     */ 0, 1, 0,
    /* Max instances*/ 1
};

class KrakenSdrSourceModule : public ModuleManager::Instance {
public:
    KrakenSdrSourceModule(std::string name) : name_(std::move(name)) {
        // Register in the source selector.  The handler callbacks are
        // minimal: KrakenSDR never produces IQ, so start/stop/tune are
        // intentional no-ops.  The menu handler renders the info panel.
        handler_.ctx              = this;
        handler_.selectHandler    = menuSelected;
        handler_.deselectHandler  = menuDeselected;
        handler_.menuHandler      = menuHandler;
        handler_.startHandler     = start;
        handler_.stopHandler      = stop;
        handler_.tuneHandler      = tune;
        handler_.stream           = &stream_;
        sigpath::sourceManager.registerSource("KrakenSDR", &handler_);
        flog::info("[KrakenSDR-src] registered in source selector");
    }

    ~KrakenSdrSourceModule() override {
        stop(this);
        sigpath::sourceManager.unregisterSource("KrakenSDR");
        flog::info("[KrakenSDR-src] unregistered from source selector");
    }

    void postInit() override {}

    void enable()  override { enabled_ = true; }
    void disable() override { enabled_ = false; }
    bool isEnabled() override { return enabled_; }

private:
    // ── Source selector callbacks ─────────────────────────────────────────

    static void menuSelected(void* ctx) {
        // KrakenSDR does not produce IQ.  Set a nominal 2.4 MHz sample
        // rate so the UI doesn't display 0 Hz; the actual DSP graph
        // never runs against this source.
        core::setInputSampleRate(2'400'000.0);
        flog::info("[KrakenSDR-src] selected (IQ not available; use kraken_lob_decoder)");
    }

    static void menuDeselected(void* ctx) {
        flog::info("[KrakenSDR-src] deselected");
    }

    static void menuHandler(void* ctx) {
        static_cast<KrakenSdrSourceModule*>(ctx)->drawMenu();
    }

    static void start(void* ctx) {
        // No IQ output — emit a warning and do nothing.
        flog::warn("[KrakenSDR-src] start() called but KrakenSDR does not provide "
                   "raw IQ to SDR++.  Enable kraken_lob_decoder for bearing data.");
    }

    static void stop(void* ctx) {}

    static void tune(double freq, void* ctx) {}

    // ── GUI ──────────────────────────────────────────────────────────────

    void drawMenu() {
#ifdef __ANDROID__
        // Android: KrakenSDR requires a companion RPi running krakensdr_doa.
        // Direct USB access is not possible from Android; the kraken_lob_decoder
        // connects over Wi-Fi/LAN WebSocket.
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.1f, 1.0f), "Android — Network mode only");
        ImGui::Spacing();
        ImGui::TextWrapped(
            "KrakenSDR requires a companion Raspberry Pi running\n"
            "krakensdr_doa. Android cannot access the USB interface\n"
            "directly. Connect over Wi-Fi or LAN."
        );
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.5f, 1.0f), "Setup:");
        ImGui::Bullet(); ImGui::TextWrapped("Run krakensdr_doa on RPi (port 8081)");
        ImGui::Bullet(); ImGui::TextWrapped("Enable  kraken_lob_decoder  in Module Manager");
        ImGui::Bullet(); ImGui::TextWrapped("Set Host = RPi IP address, Port = 8081");
        ImGui::Spacing();

        // Live connection status from the kraken_lob_decoder
        // (polled from sigpath config; shows last-seen confidence).
        float conf = 0.0f;
        bool connected = false;
        if (core::configManager.conf.contains("krakenLobConnected"))
            connected = core::configManager.conf["krakenLobConnected"].get<bool>();
        if (core::configManager.conf.contains("krakenLobLastConfidence"))
            conf = core::configManager.conf["krakenLobLastConfidence"].get<float>();

        ImGui::Separator();
        ImGui::Spacing();
        if (connected) {
            ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.3f, 1.0f), "Status: CONNECTED");
            ImGui::Text("  Last DOA confidence: %.2f", conf);
        } else {
            ImGui::TextColored(ImVec4(0.8f, 0.2f, 0.2f, 1.0f), "Status: NOT CONNECTED");
            ImGui::TextWrapped("  kraken_lob_decoder not connected to krakensdr_doa.");
        }
#else
        // Linux / Desktop: local krakensdr_doa on the same host.
        ImGui::TextColored(ImVec4(0.9f, 0.7f, 0.1f, 1.0f), "KrakenSDR — DOA hardware");
        ImGui::Spacing();
        ImGui::TextWrapped(
            "KrakenSDR does not provide raw IQ samples to SDR++.\n"
            "The daq_fw daemon consumes the USB interface for coherent\n"
            "phase alignment and feeds krakensdr_doa internally.\n"
        );
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.5f, 1.0f), "To receive LOB bearing data:");
        ImGui::Bullet(); ImGui::TextWrapped("Enable  kraken_lob_decoder  in Module Manager");
        ImGui::Bullet(); ImGui::TextWrapped("Set Host = krakensdr_doa IP, Port = 8081");
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.55f, 0.55f, 0.55f, 1.0f), "companion processes:");
        ImGui::Text("  daq_fw         port 5000  (firmware bridge)");
        ImGui::Text("  krakensdr_doa  port 8081  (DOA / WebSocket)");
        ImGui::Spacing();

        // Live status from kraken_lob_decoder config keys (set by the decoder
        // module whenever its WebSocket connects or receives a bearing).
        bool connected = false;
        float conf = 0.0f;
        if (core::configManager.conf.contains("krakenLobConnected"))
            connected = core::configManager.conf["krakenLobConnected"].get<bool>();
        if (core::configManager.conf.contains("krakenLobLastConfidence"))
            conf = core::configManager.conf["krakenLobLastConfidence"].get<float>();

        ImGui::Separator();
        ImGui::Spacing();
        if (connected) {
            ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.3f, 1.0f), "Status: CONNECTED");
            ImGui::Text("  Last DOA confidence: %.2f", conf);
        } else {
            ImGui::TextColored(ImVec4(0.8f, 0.2f, 0.2f, 1.0f), "Status: NOT CONNECTED");
            ImGui::TextWrapped("  Start krakensdr_doa and enable kraken_lob_decoder.");
        }
#endif
    }

    // ── State ────────────────────────────────────────────────────────────

    std::string                      name_;
    bool                             enabled_ = false;
    dsp::stream<dsp::complex_t>      stream_;
    SourceManager::SourceHandler     handler_;
};

// ── SDRPP module entry points ─────────────────────────────────────────────────

MOD_EXPORT void _INIT_() {
    flog::info("[KrakenSDR-src] module loaded");
}

MOD_EXPORT ModuleManager::Instance* _CREATE_INSTANCE_(std::string name) {
    return new KrakenSdrSourceModule(std::move(name));
}

MOD_EXPORT void _DELETE_INSTANCE_(ModuleManager::Instance* inst) {
    delete inst;
}

MOD_EXPORT void _END_() {
    flog::info("[KrakenSDR-src] module unloaded");
}
