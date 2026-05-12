/*
    Predator RF — Native rtl_433 decoder module.

    Architecture
    ------------
    SDRPP signal path → VFO @ 250 kHz BW → handler sink → this module
        ↓ (per buffer)
        compute envelope (AM int16) + FM-demod (FM int16)
        ↓
        rtl_433 pulse_detect_package(...)
        ↓
        run_ook_demods / run_fsk_demods on cfg->demod->r_devs
        ↓ (when a decoder fires)
        rtl_433 fans the data_t out via cfg->output_handler[]
        ↓
        our PredatorDataOutput::output_print catches it
        ↓
        convert data_t → predator::DecoderIngestEvent → enqueue

    The bridge ingester (predator::Rtl433Ingester) keeps working as a fallback
    for users who already have a desktop rtl_433 companion. This native module
    is additive — events from both sources can coexist in the UI.

    Status: SCAFFOLDING. Compiles + registers as a SDRPP module. The full
    pulse → demod → event path is in place but the AM/FM scaling and pulse
    detector tuning may need iteration against real captures on the S22.
*/

#include <imgui.h>
#include <config.h>
#include <core.h>
#include <gui/style.h>
#include <gui/gui.h>
#include <signal_path/signal_path.h>
#include <signal_path/vfo_manager.h>
#include <module.h>
#include <utils/flog.h>
#include <dsp/stream.h>
#include <dsp/sink/handler_sink.h>
#include <dsp/types.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <mutex>
#include <queue>
#include <string>
#include <vector>

#include "../../../core/src/predator/decoder_ingest.h"
#include "../../../core/src/predator/native_decoder_registry.h"
#include "../../../core/src/predator/hold_binding_registry.h"

extern "C" {
    #include "rtl_433.h"
    #include "r_api.h"
    #include "r_device.h"
    #include "r_private.h"
    #include "data.h"
    #include "pulse_detect.h"
    #include "pulse_data.h"
    #include "baseband.h"
    #include "list.h"
}

#define CONCAT(a, b) ((std::string(a) + b).c_str())

static constexpr uint32_t RTL433_INPUT_RATE = 250000;  // upstream default
static constexpr int      RTL433_BLOCK_HINT = 16384;

SDRPP_MOD_INFO{
    /* Name:            */ "rtl433_decoder",
    /* Description:     */ "Native rtl_433 ISM decoder for Predator RF",
    /* Author:          */ "Predator",
    /* Version:         */ 0, 1, 0,
    /* Max instances    */ -1
};

static ConfigManager config;

namespace {

class Rtl433DecoderModule;

// data_output_t with an owner backpointer so the rtl_433 vtable trampolines
// can find their way back to our module instance.
struct PredatorDataOutput {
    data_output_t          base;
    Rtl433DecoderModule*   owner;
};

class Rtl433DecoderModule : public ModuleManager::Instance {
public:
    explicit Rtl433DecoderModule(std::string name) : name_(std::move(name)) {
        // Hold-bound mode: HoldDecoderBinder pre-registers a target VFO
        // name for instances it spawns (predator::hold::setBoundVfoFor)
        // BEFORE calling moduleManager.createInstance.  When the binding
        // is present we skip our own createVFO and instead bind the DSP
        // handler to the existing held VFO.  See
        // core/src/predator/hold_binding_registry.h for the contract.
        boundVfoName_ = predator::hold::getBoundVfoFor(name_);
        boundMode_    = !boundVfoName_.empty();

        // Persist + load module config.
        config.acquire();
        if (!config.conf.contains(name_)) {
            config.conf[name_] = nlohmann::json::object();
        }
        if (!config.conf[name_].contains("enabled"))     config.conf[name_]["enabled"] = false;
        if (!config.conf[name_].contains("amScale"))     config.conf[name_]["amScale"] = 24000.0;
        enabled_ = config.conf[name_]["enabled"];
        amScale_ = config.conf[name_]["amScale"];
        config.release(true);

        // Bound instances are spawned solely to decode their bound VFO,
        // so default enabled=true regardless of persisted state — the
        // operator paused/disabled state lives on the HoldEntry itself.
        if (boundMode_) enabled_ = true;

        // Working buffers + pulse_data scratch.
        amBuf_.resize(RTL433_BLOCK_HINT);
        fmBuf_.resize(RTL433_BLOCK_HINT);
        std::memset(&pulseData_,    0, sizeof(pulseData_));
        std::memset(&fskPulseData_, 0, sizeof(fskPulseData_));
        std::memset(&fmState_,      0, sizeof(fmState_));

        gui::menu.registerEntry(name_, menuHandler, this, this);

        // Register with the process-wide native decoder registry so
        // main_window.cpp can drain decoded events into predatorEvents
        // each frame (same path the bridge ingesters use).
        predator::registerNativeDecoder(this, "RTL433",
            [this](std::size_t maxItems) { return drain(maxItems); });

        if (enabled_) startPipeline();
    }

    ~Rtl433DecoderModule() {
        predator::unregisterNativeDecoder(this);
        gui::menu.removeEntry(name_);
        stopPipeline();
    }

    void postInit() override {}
    void enable()   override { enabled_ = true;  startPipeline(); persistFlag("enabled", true);  }
    void disable()  override { enabled_ = false; stopPipeline();  persistFlag("enabled", false); }
    bool isEnabled() override { return enabled_; }

    // Drained by the per-frame UI loop in main_window.cpp (next iteration —
    // for now the events live here and surface in this module's menu panel).
    std::vector<predator::DecoderIngestEvent> drain(size_t maxItems = 64) {
        std::vector<predator::DecoderIngestEvent> out;
        std::lock_guard<std::mutex> lk(queueMtx_);
        while (!queue_.empty() && out.size() < maxItems) {
            out.push_back(std::move(queue_.front()));
            queue_.pop();
        }
        return out;
    }

private:
    // ----------------------------------------------------------------- pipeline
    void startPipeline() {
        std::lock_guard<std::mutex> lk(pipeMtx_);
        if (running_) return;

        cfg_ = r_create_cfg();
        if (!cfg_) {
            flog::error("[rtl433_decoder] r_create_cfg() failed");
            return;
        }
        cfg_->samp_rate = RTL433_INPUT_RATE;

        register_all_protocols(cfg_, 0);

        // Push our custom data_output onto cfg->output_handler. r_free_cfg
        // will free() it via list_free_elems on shutdown.
        output_ = (PredatorDataOutput*)std::calloc(1, sizeof(PredatorDataOutput));
        output_->base.output_print = &Rtl433DecoderModule::dataOutputPrint;
        output_->base.output_free  = &Rtl433DecoderModule::dataOutputFree;
        output_->base.log_level    = 9999;
        output_->owner             = this;
        list_push(&cfg_->output_handler, output_);

        pulseDetect_ = pulse_detect_create();

        // Hook the SDRPP signal path.
        VFOManager::VFO* targetVfo = nullptr;
        if (boundMode_) {
            // Bound mode: HoldManager owns the VFO; we just hook the
            // sample stream.  We do NOT delete this VFO on stop — see
            // stopPipeline() below.
            targetVfo = sigpath::vfoManager.findVFO(boundVfoName_);
            if (!targetVfo) {
                flog::error("[rtl433_decoder] bound VFO '{}' not found",
                            boundVfoName_);
                cleanupCfg();
                return;
            }
            vfo_ = nullptr;  // caller owns it
        } else {
            // Legacy mode: own a VFO named after our instance.  Sample
            // rate == bandwidth == RTL433_INPUT_RATE so we don't need a
            // resampler in front.
            vfo_ = sigpath::vfoManager.createVFO(
                name_,
                ImGui::WaterfallVFO::REF_CENTER,
                0.0,                                  // offset (centered on tuned freq)
                (double)RTL433_INPUT_RATE,            // bandwidth
                (double)RTL433_INPUT_RATE,            // output sample rate
                (double)RTL433_INPUT_RATE,            // ref bandwidth
                (double)RTL433_INPUT_RATE,            // min bandwidth
                true                                  // bandwidth locked
            );
            if (!vfo_) {
                flog::error("[rtl433_decoder] failed to create VFO");
                cleanupCfg();
                return;
            }
            targetVfo = vfo_;
        }

        handler_.init(targetVfo->output, &Rtl433DecoderModule::sampleHandler, this);
        handler_.start();

        running_ = true;
        flog::info("[rtl433_decoder] pipeline started ({} protocols, mode={}, vfo={})",
                   (int)cfg_->demod->r_devs.len,
                   boundMode_ ? "bound" : "legacy",
                   boundMode_ ? boundVfoName_.c_str() : name_.c_str());
    }

    void stopPipeline() {
        std::lock_guard<std::mutex> lk(pipeMtx_);
        if (!running_) return;

        handler_.stop();
        // Bound mode: HoldManager owns the VFO — we never created it
        // and we must NOT delete it (would leak the held entry's runtime
        // state and confuse the next preTick() reconciliation).  vfo_ is
        // always nullptr in bound mode.
        if (vfo_) {
            sigpath::vfoManager.deleteVFO(vfo_);
            vfo_ = nullptr;
        }
        cleanupCfg();
        running_ = false;
    }

    void cleanupCfg() {
        if (pulseDetect_) {
            pulse_detect_free(pulseDetect_);
            pulseDetect_ = nullptr;
        }
        if (cfg_) {
            // r_free_cfg() releases every owned member but explicitly does
            // NOT free(cfg) itself (see vendor/rtl_433/src/r_api.c — the
            // upstream call ends with `//free(cfg);`). It also walks
            // cfg->output_handler with data_output_free, which calls our
            // dataOutputFree (below) — that's where we release the
            // PredatorDataOutput allocation.
            r_free_cfg(cfg_);
            std::free(cfg_);
            cfg_    = nullptr;
            output_ = nullptr;
        }
    }

    // ------------------------------------------------------ DSP sample handler
    static void sampleHandler(dsp::complex_t* iq, int count, void* ctx) {
        static_cast<Rtl433DecoderModule*>(ctx)->processSamples(iq, count);
    }

    void processSamples(dsp::complex_t* iq, int count) {
        if (!cfg_ || !pulseDetect_ || count <= 0) return;

        if ((size_t)count > amBuf_.size()) {
            amBuf_.resize(count);
            fmBuf_.resize(count);
        }
        if ((int)iqCs16Scratch_.size() < count * 2) {
            iqCs16Scratch_.resize(count * 2);
        }

        // CF32 → AM envelope (int16) + CS16 (for FM demod input)
        const float scale = (float)amScale_;
        for (int i = 0; i < count; ++i) {
            float re  = iq[i].re;
            float im  = iq[i].im;
            float mag = std::sqrt(re * re + im * im);
            int   am  = (int)(mag * scale);
            if (am > 32767)  am = 32767;
            if (am < -32768) am = -32768;
            amBuf_[i] = (int16_t)am;

            iqCs16Scratch_[i * 2 + 0] = (int16_t)(re * 32767.0f);
            iqCs16Scratch_[i * 2 + 1] = (int16_t)(im * 32767.0f);
        }

        baseband_demod_FM_cs16(iqCs16Scratch_.data(), fmBuf_.data(),
                               (unsigned long)count, RTL433_INPUT_RATE,
                               0.0f, &fmState_);

        unsigned fpdm = cfg_->fsk_pulse_detect_mode;
        int pkg = pulse_detect_package(pulseDetect_, amBuf_.data(), fmBuf_.data(),
                                       count, RTL433_INPUT_RATE, samplePos_,
                                       &pulseData_, &fskPulseData_, fpdm);

        if (pkg == PULSE_DATA_OOK) {
            run_ook_demods(&cfg_->demod->r_devs, &pulseData_);
        } else if (pkg == PULSE_DATA_FSK) {
            run_fsk_demods(&cfg_->demod->r_devs, &fskPulseData_);
        }

        samplePos_ += (uint64_t)count;
    }

    // ------------------------------------------------- data_output_t vtable
    static void dataOutputPrint(data_output_t* output, data_t* data) {
        auto* po = (PredatorDataOutput*)output;
        if (po && po->owner) po->owner->onDecoded(data);
    }

    // r_free_cfg() walks output_handler with data_output_free(), which only
    // forwards to output->output_free(output) — it does NOT free the struct
    // itself (see vendor/rtl_433/src/data.c). So our output_free callback
    // is responsible for releasing the PredatorDataOutput allocation made
    // in startPipeline().
    static void dataOutputFree(data_output_t* output) {
        if (output) std::free((PredatorDataOutput*)output);
    }

    void onDecoded(data_t* data) {
        predator::DecoderIngestEvent ev;
        ev.decoder = "RTL433";

        nlohmann::json raw = nlohmann::json::object();
        for (data_t* d = data; d; d = d->next) {
            if (!d->key) continue;
            std::string key = d->key;

            switch (d->type) {
                case DATA_INT:    raw[key] = d->value.v_int; break;
                case DATA_DOUBLE: raw[key] = d->value.v_dbl; break;
                case DATA_STRING:
                    if (d->value.v_ptr) raw[key] = (const char*)d->value.v_ptr;
                    break;
                default: break;  // arrays / nested handled in next iteration
            }

            // Map well-known fields onto DecoderIngestEvent shape.
            if (key == "model" && d->type == DATA_STRING && d->value.v_ptr) {
                ev.protocol = (const char*)d->value.v_ptr;
                ev.label    = ev.protocol;
            } else if (key == "id") {
                if (d->type == DATA_INT)        ev.networkId = std::to_string(d->value.v_int);
                else if (d->type == DATA_STRING && d->value.v_ptr) ev.networkId = (const char*)d->value.v_ptr;
            } else if (key == "device" && ev.networkId.empty()) {
                if (d->type == DATA_INT)        ev.networkId = std::to_string(d->value.v_int);
                else if (d->type == DATA_STRING && d->value.v_ptr) ev.networkId = (const char*)d->value.v_ptr;
            } else if (key == "channel" && d->type == DATA_INT) {
                ev.talkgroup = std::to_string(d->value.v_int);
            } else if (key == "freq" && d->type == DATA_DOUBLE) {
                // rtl_433 reports MHz; promote to Hz for our event shape.
                ev.frequencyHz = d->value.v_dbl * 1e6;
            }
        }
        ev.raw = std::move(raw);

        {
            std::lock_guard<std::mutex> lk(queueMtx_);
            while (queue_.size() > 1000) queue_.pop();
            queue_.push(ev);
        }
        eventsTotal_++;

        // Track last-event preview for the menu panel.
        std::lock_guard<std::mutex> lk(lastMtx_);
        lastProto_ = ev.protocol;
        lastId_    = ev.networkId;
    }

    // ---------------------------------------------------------------- menu UI
    static void menuHandler(void* ctx) {
        auto* _this = static_cast<Rtl433DecoderModule*>(ctx);

        ImGui::TextUnformatted("Native rtl_433 decoder");
        ImGui::Separator();
        ImGui::Text("Status:    %s", _this->running_ ? "Running" : "Stopped");
        ImGui::Text("Events:    %d", _this->eventsTotal_.load());

        {
            std::lock_guard<std::mutex> lk(_this->lastMtx_);
            if (!_this->lastProto_.empty()) {
                ImGui::TextWrapped("Last:      %s  id=%s",
                                   _this->lastProto_.c_str(),
                                   _this->lastId_.c_str());
            } else {
                ImGui::TextUnformatted("Last:      (none yet)");
            }
        }

        ImGui::Separator();

        if (_this->enabled_) {
            if (ImGui::Button(CONCAT("Disable##rtl433_", _this->name_))) {
                _this->enabled_ = false;
                _this->stopPipeline();
                _this->persistFlag("enabled", false);
            }
        } else {
            if (ImGui::Button(CONCAT("Enable##rtl433_", _this->name_))) {
                _this->enabled_ = true;
                _this->startPipeline();
                _this->persistFlag("enabled", true);
            }
        }

        ImGui::TextWrapped(
            "Tap I/Q at 250 kHz around the tuned center frequency. "
            "ISM bands of interest: 315, 433.92, 868.3, 915 MHz.");
    }

    void persistFlag(const char* key, bool v) {
        config.acquire();
        config.conf[name_][key] = v;
        config.release(true);
    }

    // ----------------------------------------------------------------- state
    std::string name_;
    bool        enabled_     = false;
    bool        boundMode_   = false;
    std::string boundVfoName_;
    double      amScale_     = 24000.0;

    std::mutex                                pipeMtx_;
    std::atomic<bool>                         running_{false};
    VFOManager::VFO*                          vfo_     = nullptr;
    dsp::sink::Handler<dsp::complex_t>        handler_;

    r_cfg_t*           cfg_         = nullptr;
    pulse_detect_t*    pulseDetect_ = nullptr;
    PredatorDataOutput* output_     = nullptr;

    pulse_data_t       pulseData_{};
    pulse_data_t       fskPulseData_{};
    demodfm_state_t    fmState_{};
    std::vector<int16_t> amBuf_;
    std::vector<int16_t> fmBuf_;
    std::vector<int16_t> iqCs16Scratch_;
    uint64_t           samplePos_ = 0;

    std::queue<predator::DecoderIngestEvent> queue_;
    mutable std::mutex                       queueMtx_;
    std::atomic<int>                         eventsTotal_{0};

    std::mutex   lastMtx_;
    std::string  lastProto_;
    std::string  lastId_;
};

} // namespace

MOD_EXPORT void _INIT_() {
    nlohmann::json def = nlohmann::json::object();
    config.setPath(core::args["root"].s() + "/rtl433_decoder_config.json");
    config.load(def);
    config.enableAutoSave();
}

MOD_EXPORT ModuleManager::Instance* _CREATE_INSTANCE_(std::string name) {
    return new Rtl433DecoderModule(std::move(name));
}

MOD_EXPORT void _DELETE_INSTANCE_(void* instance) {
    delete (Rtl433DecoderModule*)instance;
}

MOD_EXPORT void _END_() {
    config.disableAutoSave();
    config.save();
}
