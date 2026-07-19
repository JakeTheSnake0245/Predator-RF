/*
    Predator RF — KrakenSDR LOB decoder module.

    Architecture
    ============
    krakensdr_doa companion process
        ↓ WebSocket ws://host:port/ws  (JSON text frames)
        KrakenWsIngester (background thread in decoder_ingest.h)
        ↓ DecoderIngestEvent { decoder="KRAKEN_LOB", raw={…} }
        predator::registerNativeDecoder drain-path
        ↓ main_window per-frame bridge → Python backend ingest_lob()

    Wire format (krakensdr_doa default output):
    {
      "type": "doa_result",
      "freq_hz": 433920000,
      "bearing_deg": 127.5,
      "bearing_std_deg": 5.2,
      "confidence": 0.83,
      "power_dbfs": -42.1,
      "snr_db": 12.3,
      "gps_lat": 37.4,
      "gps_lon": -122.1,
      "heading_deg": 0.0,
      "timestamp_unix": 1718035200.123,
      "node_id": "kraken-0"
    }

    Messages whose "type" is not "doa_result" are silently ignored so that
    the module survives krakensdr_doa status/config messages on the same feed.

    Configuration (stored in SDRPP config.json):
        krakenHost       string   "127.0.0.1"
        krakenPort       int      8082
        krakenPath       string   "/ws"
        krakenNodeId     string   "kraken-0"
        krakenEnabled    bool     false

    Status: PRODUCTION — WebSocket client + JSON parse fully implemented.
    The full krakensdr_doa DOA pipeline runs externally; this module is the
    SDRPP-side bridge only.
*/

#include <imgui.h>
#include <config.h>
#include <core.h>
#include <gui/style.h>
#include <gui/gui.h>
#include <module.h>
#include <utils/flog.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "../../../core/src/predator/decoder_ingest.h"
#include "../../../core/src/predator/kraken_ctl_client.h"
#include "../../../core/src/predator/native_decoder_registry.h"
#include <gui/widgets/ime_scroll.h>

#define CONCAT(a, b) ((std::string(a) + b).c_str())

SDRPP_MOD_INFO{
    /* Name:        */ "kraken_lob_decoder",
    /* Description: */ "KrakenSDR LOB decoder (krakensdr_doa WebSocket bridge)",
    /* Author:      */ "Predator",
    /* Version:     */ 0, 1, 0,
    /* Max instances*/ -1
};

ConfigManager config;

// ── Default settings ────────────────────────────────────────────────────────

static const json DEFAULT_CONFIG = {
    {"krakenHost",    "127.0.0.1"},
    {"krakenPort",    8081},    // krakensdr_doa default; 8082 is legacy
    {"krakenPath",    "/ws"},
    {"krakenNodeId",  "kraken-0"},
    {"krakenEnabled", false},
    {"krakenCtlPort", 8042},    // krakensdr_doa remote-control HTTP API
    {"krakenCtlEnabled", false}
};

// ── Module class ─────────────────────────────────────────────────────────────

class KrakenLobDecoderModule : public ModuleManager::Instance {
public:
    KrakenLobDecoderModule(std::string name) : name_(std::move(name)) {
        config.acquire();
        if (!config.conf.contains("krakenHost"))    config.conf["krakenHost"]    = DEFAULT_CONFIG["krakenHost"];
        if (!config.conf.contains("krakenPort"))    config.conf["krakenPort"]    = DEFAULT_CONFIG["krakenPort"];
        if (!config.conf.contains("krakenPath"))    config.conf["krakenPath"]    = DEFAULT_CONFIG["krakenPath"];
        if (!config.conf.contains("krakenNodeId"))  config.conf["krakenNodeId"]  = DEFAULT_CONFIG["krakenNodeId"];
        if (!config.conf.contains("krakenEnabled")) config.conf["krakenEnabled"] = DEFAULT_CONFIG["krakenEnabled"];
        if (!config.conf.contains("krakenCtlPort"))    config.conf["krakenCtlPort"]    = DEFAULT_CONFIG["krakenCtlPort"];
        if (!config.conf.contains("krakenCtlEnabled")) config.conf["krakenCtlEnabled"] = DEFAULT_CONFIG["krakenCtlEnabled"];

        strncpy(hostBuf_,   std::string(config.conf["krakenHost"]).c_str(),   sizeof(hostBuf_) - 1);
        strncpy(pathBuf_,   std::string(config.conf["krakenPath"]).c_str(),   sizeof(pathBuf_) - 1);
        strncpy(nodeIdBuf_, std::string(config.conf["krakenNodeId"]).c_str(), sizeof(nodeIdBuf_) - 1);
        port_    = config.conf["krakenPort"].get<int>();
        enabled_ = config.conf["krakenEnabled"].get<bool>();
        ctlPort_    = config.conf["krakenCtlPort"].get<int>();
        ctlEnabled_ = config.conf["krakenCtlEnabled"].get<bool>();
        config.release(true);

        ingester_  = std::make_unique<predator::KrakenWsIngester>();
        ctlClient_ = std::make_unique<predator::KrakenCtlClient>();

        // Register with the Predator native decoder registry.
        // main_window's per-frame tick calls the drain lambda to harvest
        // pending LOB events and route them to the Python backend via the
        // standard DecoderIngestEvent bridge path (identical to DSDFME).
        predator::registerNativeDecoder(this, "KRAKEN_LOB",
            [this](std::size_t maxItems) -> std::vector<predator::DecoderIngestEvent> {
                return ingester_->drain(maxItems);
            });

        if (enabled_) {
            startIngester();
        }
        if (ctlEnabled_) {
            ctlClient_->start(hostBuf_, ctlPort_);
        }

        gui::menu.registerEntry(name_, drawMenuStatic, this, this);
        flog::info("[KrakenLOB] module instance '{}' constructed", name_);
    }

    ~KrakenLobDecoderModule() {
        gui::menu.removeEntry(name_);
        ctlClient_->stop();
        ingester_->stop();
        predator::unregisterNativeDecoder(this);
        flog::info("[KrakenLOB] module instance '{}' destructed", name_);
    }

    void postInit() override {}

    void enable() override {
        enabled_ = true;
        startIngester();
        saveConfig();
    }

    void disable() override {
        enabled_ = false;
        ingester_->stop();
        saveConfig();
    }

    bool isEnabled() override { return enabled_; }

private:
    // ── GUI ──────────────────────────────────────────────────────────────

    static void drawMenuStatic(void* ctx) {
        auto* self = static_cast<KrakenLobDecoderModule*>(ctx);
        self->drawMenu();
    }

    void drawMenu() {
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.7f);
        if (ImGui::InputTextIME(CONCAT("##krakenHost", name_), hostBuf_, sizeof(hostBuf_))) {
            saveConfig();
        }
        ImGui::SameLine();
        ImGui::TextUnformatted("Host");

        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.4f);
        if (ImGui::InputIntIME(CONCAT("##krakenPort", name_), &port_, 0, 0)) {
            port_ = std::max(1, std::min(65535, port_));
            saveConfig();
        }
        ImGui::SameLine();
        ImGui::TextUnformatted("Port");

        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.7f);
        if (ImGui::InputTextIME(CONCAT("##krakenPath", name_), pathBuf_, sizeof(pathBuf_))) {
            saveConfig();
        }
        ImGui::SameLine();
        ImGui::TextUnformatted("Path");

        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.7f);
        if (ImGui::InputTextIME(CONCAT("##krakenNodeId", name_), nodeIdBuf_, sizeof(nodeIdBuf_))) {
            saveConfig();
        }
        ImGui::SameLine();
        ImGui::TextUnformatted("Node ID");

        // Status indicator
        if (ingester_->isConnected()) {
            ImGui::TextColored(ImVec4(0.2f, 0.9f, 0.4f, 1.0f), "● Connected");
        } else if (ingester_->isRunning()) {
            ImGui::TextColored(ImVec4(0.9f, 0.7f, 0.1f, 1.0f), "○ Connecting…");
        } else {
            ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "○ Stopped");
        }

        ImGui::Text("Events: %d", ingester_->eventsReceived());

        if (ingester_->isRunning()) {
            if (ImGui::Button(CONCAT("Stop##krakenStop", name_))) {
                enabled_ = false;
                ingester_->stop();
                saveConfig();
            }
        } else {
            if (ImGui::Button(CONCAT("Connect##krakenStart", name_))) {
                enabled_ = true;
                startIngester();
                saveConfig();
            }
        }

        // ── Kraken remote frequency control ──────────────────────────────
        ImGui::Separator();
        ImGui::TextUnformatted("Kraken Control (RX retune)");

        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.4f);
        if (ImGui::InputIntIME(CONCAT("##krakenCtlPort", name_), &ctlPort_, 0, 0)) {
            ctlPort_ = std::max(1, std::min(65535, ctlPort_));
            saveConfig();
        }
        ImGui::SameLine();
        ImGui::TextUnformatted("Ctl Port");

        // Control link status + current Kraken frequency
        auto tuneState = ctlClient_->tuneState();
        if (!ctlClient_->isRunning()) {
            ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "○ Control off");
        } else if (tuneState == predator::KrakenTuneState::SENDING) {
            ImGui::TextColored(ImVec4(0.9f, 0.7f, 0.1f, 1.0f), "○ Sending…");
        } else if (tuneState == predator::KrakenTuneState::CALIBRATING) {
            ImGui::TextColored(ImVec4(0.9f, 0.7f, 0.1f, 1.0f), "◐ Calibrating…");
        } else if (tuneState == predator::KrakenTuneState::FAILED) {
            ImGui::TextColored(ImVec4(0.9f, 0.25f, 0.2f, 1.0f), "✕ Tune failed");
        } else if (tuneState == predator::KrakenTuneState::CONFIRMED) {
            ImGui::TextColored(ImVec4(0.2f, 0.9f, 0.4f, 1.0f), "● Tune confirmed");
        } else if (ctlClient_->isReachable()) {
            ImGui::TextColored(ImVec4(0.2f, 0.9f, 0.4f, 1.0f), "● Control link OK");
        } else {
            ImGui::TextColored(ImVec4(0.9f, 0.7f, 0.1f, 1.0f), "○ Unreachable");
        }
        std::string ctlStatus = ctlClient_->statusString();
        if (!ctlStatus.empty()) {
            ImGui::TextWrapped("%s", ctlStatus.c_str());
        }

        double krakenFreq = ctlClient_->currentFreqHz();
        if (krakenFreq > 0.0) {
            ImGui::Text("Kraken freq: %.4f MHz", krakenFreq / 1e6);
        } else {
            ImGui::TextUnformatted("Kraken freq: —");
        }

        if (ctlClient_->isRunning()) {
            if (ImGui::Button(CONCAT("Stop Control##krakenCtlStop", name_))) {
                ctlEnabled_ = false;
                ctlClient_->stop();
                saveConfig();
            }
        } else {
            if (ImGui::Button(CONCAT("Start Control##krakenCtlStart", name_))) {
                ctlEnabled_ = true;
                ctlClient_->start(hostBuf_, ctlPort_);
                saveConfig();
            }
        }

        bool tuneBusy = !ctlClient_->isRunning() ||
                        tuneState == predator::KrakenTuneState::SENDING ||
                        tuneState == predator::KrakenTuneState::CALIBRATING;
        if (tuneBusy) ImGui::BeginDisabled();

        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.5f);
        if (ImGui::InputDoubleIME(CONCAT("##krakenTuneFreq", name_), &tuneFreqMHz_, 0.0, 0.0, "%.4f")) {
            tuneFreqMHz_ = std::max(0.0, tuneFreqMHz_);
        }
        ImGui::SameLine();
        ImGui::TextUnformatted("MHz");

        if (ImGui::Button(CONCAT("Tune Kraken##krakenTune", name_))) {
            ctlClient_->requestTune(tuneFreqMHz_ * 1e6);
        }
        ImGui::SameLine();
        if (ImGui::Button(CONCAT("Tune to VFO##krakenTuneVfo", name_))) {
            double vfoHz = gui::freqSelect.frequency;
            if (vfoHz > 0.0) {
                tuneFreqMHz_ = vfoHz / 1e6;
                ctlClient_->requestTune(vfoHz);
            }
        }

        if (tuneBusy) ImGui::EndDisabled();
    }

    // ── Helpers ──────────────────────────────────────────────────────────

    void startIngester() {
        ingester_->setNodeId(nodeIdBuf_);
        ingester_->start(hostBuf_, port_, pathBuf_);
    }

    void saveConfig() {
        config.acquire();
        config.conf["krakenHost"]    = std::string(hostBuf_);
        config.conf["krakenPort"]    = port_;
        config.conf["krakenPath"]    = std::string(pathBuf_);
        config.conf["krakenNodeId"]  = std::string(nodeIdBuf_);
        config.conf["krakenEnabled"] = enabled_;
        config.conf["krakenCtlPort"]    = ctlPort_;
        config.conf["krakenCtlEnabled"] = ctlEnabled_;
        config.release(true);
    }

    // ── State ────────────────────────────────────────────────────────────

    std::string name_;
    bool enabled_ = false;
    int  port_ = 8081;
    bool ctlEnabled_ = false;
    int  ctlPort_ = 8042;
    double tuneFreqMHz_ = 0.0;

    char hostBuf_[256]   = "127.0.0.1";
    char pathBuf_[128]   = "/ws";
    char nodeIdBuf_[64]  = "kraken-0";

    std::unique_ptr<predator::KrakenWsIngester> ingester_;
    std::unique_ptr<predator::KrakenCtlClient>  ctlClient_;
};

// ── SDRPP module entry points ─────────────────────────────────────────────────

MOD_EXPORT void _INIT_() {
    config.setPath(core::args["root"].s() + "/kraken_lob_decoder_config.json");
    config.load(DEFAULT_CONFIG);
    config.enableAutoSave();
    flog::info("[KrakenLOB] module loaded");
}

MOD_EXPORT ModuleManager::Instance* _CREATE_INSTANCE_(std::string name) {
    return new KrakenLobDecoderModule(std::move(name));
}

MOD_EXPORT void _DELETE_INSTANCE_(ModuleManager::Instance* inst) {
    delete inst;
}

MOD_EXPORT void _END_() {
    config.disableAutoSave();
    config.save();
    flog::info("[KrakenLOB] module unloaded");
}
