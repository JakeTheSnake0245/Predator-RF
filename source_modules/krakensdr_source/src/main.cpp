/*
    Predator RF — KrakenSDR Source stub module.

    KrakenSDR is a 5-channel coherent SDR that processes IQ samples
    internally and outputs Direction-of-Arrival (DOA) results via its
    krakensdr_doa companion process.  Raw IQ samples are NOT exposed to
    SDR++ — the hardware's USB interface is consumed exclusively by the
    daq_fw daemon which performs the coherent phase alignment.

    This stub module:
      • Registers "KrakenSDR" in the SDRPP source selector.
      • Displays an architecture overview panel so the operator knows
        what to configure elsewhere.
      • Points to the kraken_lob_decoder module (the actual data path).

    Correct deployment:
      1. Flash KrakenSDR firmware; start daq_fw on the host RPi.
      2. Start krakensdr_doa (Python) pointing at daq_fw on port 5000.
      3. Enable the kraken_lob_decoder module in SDRPP Module Manager and
         set Host/Port to the krakensdr_doa WebSocket endpoint (default
         ws://127.0.0.1:8082/ws).
      4. LOB bearing events appear in the Predator RF Hits panel and are
         forwarded to the Python intelligence backend for triangulation.
*/

#include <imgui.h>
#include <module.h>
#include <gui/gui.h>
#include <utils/flog.h>

#define CONCAT(a, b) ((std::string(a) + b).c_str())

SDRPP_MOD_INFO{
    /* Name:        */ "krakensdr_source",
    /* Description: */ "KrakenSDR Source (DOA only — see kraken_lob_decoder)",
    /* Author:      */ "Predator",
    /* Version:     */ 0, 1, 0,
    /* Max instances*/ 1
};

class KrakenSdrSourceModule : public ModuleManager::Instance {
public:
    KrakenSdrSourceModule(std::string name) : name_(std::move(name)) {
        gui::menu.registerEntry(name_, drawMenuStatic, this, this);
    }

    ~KrakenSdrSourceModule() override {
        gui::menu.removeEntry(name_);
    }

    void postInit() override {}
    void enable()   override {}
    void disable()  override {}
    bool isEnabled() override { return false; }

private:
    static void drawMenuStatic(void* ctx) {
        static_cast<KrakenSdrSourceModule*>(ctx)->drawMenu();
    }

    void drawMenu() {
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
        ImGui::Bullet(); ImGui::TextWrapped("Set Host = krakensdr_doa IP, Port = 8082");
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.55f, 0.55f, 0.55f, 1.0f), "companion processes:");
        ImGui::Text("  daq_fw         port 5000  (firmware bridge)");
        ImGui::Text("  krakensdr_doa  port 8082  (DOA / WebSocket)");
    }

    std::string name_;
};

SDRPP_MOD_EXPORT ModuleManager::Instance* createInstance(std::string name) {
    return new KrakenSdrSourceModule(std::move(name));
}

SDRPP_MOD_EXPORT void deleteInstance(ModuleManager::Instance* instance) {
    delete static_cast<KrakenSdrSourceModule*>(instance);
}
