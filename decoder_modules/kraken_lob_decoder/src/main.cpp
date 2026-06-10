/*
    Predator RF — KrakenSDR LOB decoder module.

    Architecture
    ============
    krakensdr_doa companion process
        ↓ WebSocket ws://host:port/ws  (JSON text frames)
        KrakenWsIngester (background thread in decoder_ingest.h)
        ↓ DecoderIngestEvent { decoder="KRAKEN_LOB", raw={…} }
        Predator bridge → backend/fusion/lob_triangulator.py

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
    {"krakenPort",    8082},
    {"krakenPath",    "/ws"},
    {"krakenNodeId",  "kraken-0"},
    {"krakenEnabled", false}
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

        strncpy(hostBuf_,   std::string(config.conf["krakenHost"]).c_str(),   sizeof(hostBuf_) - 1);
        strncpy(pathBuf_,   std::string(config.conf["krakenPath"]).c_str(),   sizeof(pathBuf_) - 1);
        strncpy(nodeIdBuf_, std::string(config.conf["krakenNodeId"]).c_str(), sizeof(nodeIdBuf_) - 1);
        port_    = config.conf["krakenPort"].get<int>();
        enabled_ = config.conf["krakenEnabled"].get<bool>();
        config.release(true);

        ingester_ = std::make_unique<predator::KrakenWsIngester>();

        if (enabled_) {
            startIngester();
        }

        gui::menu.registerEntry(name_, drawMenuStatic, this, this);
    }

    ~KrakenLobDecoderModule() override {
        gui::menu.removeEntry(name_);
        ingester_->stop();
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

    // ── Per-frame drain ──────────────────────────────────────────────────
    // Called by the Predator bridge's main-loop tick to harvest pending
    // LOB events and route them to the Python backend.
    std::vector<predator::DecoderIngestEvent> drainEvents() {
        return ingester_->drain(64);
    }

private:
    // ── GUI ──────────────────────────────────────────────────────────────

    static void drawMenuStatic(void* ctx) {
        auto* self = static_cast<KrakenLobDecoderModule*>(ctx);
        self->drawMenu();
    }

    void drawMenu() {
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.7f);
        if (ImGui::InputText(CONCAT("##krakenHost", name_), hostBuf_, sizeof(hostBuf_))) {
            saveConfig();
        }
        ImGui::SameLine();
        ImGui::TextUnformatted("Host");

        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.4f);
        if (ImGui::InputInt(CONCAT("##krakenPort", name_), &port_, 0, 0)) {
            port_ = std::max(1, std::min(65535, port_));
            saveConfig();
        }
        ImGui::SameLine();
        ImGui::TextUnformatted("Port");

        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.7f);
        if (ImGui::InputText(CONCAT("##krakenPath", name_), pathBuf_, sizeof(pathBuf_))) {
            saveConfig();
        }
        ImGui::SameLine();
        ImGui::TextUnformatted("Path");

        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.7f);
        if (ImGui::InputText(CONCAT("##krakenNodeId", name_), nodeIdBuf_, sizeof(nodeIdBuf_))) {
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
        config.release(true);
    }

    // ── State ────────────────────────────────────────────────────────────

    std::string name_;
    bool enabled_ = false;
    int  port_ = 8082;

    char hostBuf_[256]   = "127.0.0.1";
    char pathBuf_[128]   = "/ws";
    char nodeIdBuf_[64]  = "kraken-0";

    std::unique_ptr<predator::KrakenWsIngester> ingester_;
};

// ── SDRPP module entry points ─────────────────────────────────────────────────

SDRPP_MOD_EXPORT ModuleManager::Instance* createInstance(std::string name) {
    return new KrakenLobDecoderModule(std::move(name));
}

SDRPP_MOD_EXPORT void deleteInstance(ModuleManager::Instance* instance) {
    delete static_cast<KrakenLobDecoderModule*>(instance);
}
