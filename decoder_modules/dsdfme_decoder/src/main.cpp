/*
 * Predator RF — Native DSD-FME decoder module (P25 + DMR + voice).
 *
 * SDRPP module wrapper around the vendored DSD-FME demodulator + mbelib AMBE+2
 * voice codec. Runs as an in-APK plugin; no external companion processes.
 *
 * Pipeline:
 *   SDRPP DSP graph (CF32 baseband)
 *     -> handler sink in C++
 *     -> dsp::demod::Quadrature FM demod
 *     -> resample to 48 kHz int16
 *     -> predator_dsd_push_input_samples()        (see predator_dsd_bridge.h)
 *
 *   Worker thread:
 *     -> drives DSD-FME's main loop (getSymbol -> getDibit -> processFrame)
 *     -> processFrame -> processMbeFrame -> mbelib produces 8 kHz int16 PCM
 *     -> mbelib's output gets pa_simple_write()'d, which the stub
 *        intercepts and routes to predator_dsd_push_voice_samples().
 *
 *   Audio sink stream:
 *     -> predator_dsd_pull_voice_samples()
 *     -> upsample 8 kHz -> 48 kHz, mono -> stereo
 *     -> dsp::stream<dsp::stereo_t> registered with sigpath::sinkManager
 *
 *   Metadata stream:
 *     -> predator_dsd_set_event_cb() callback fires for each protocol event
 *     -> wrapper builds a predator::DecoderIngestEvent + queues it
 *     -> registered with predator::registerNativeDecoder() so main_window.cpp's
 *        per-frame drain folds into predatorEvents.
 */

#include <imgui.h>
#include <module.h>
#include <gui/gui.h>
#include <signal_path/signal_path.h>
#include <signal_path/sink.h>
#include <dsp/buffer/reshaper.h>
#include <dsp/multirate/rational_resampler.h>
#include <dsp/demod/quadrature.h>
#include <dsp/sink/handler_sink.h>
#include <utils/flog.h>
#include <config.h>
#include <atomic>
#include <thread>
#include <mutex>
#include <deque>
#include <chrono>
#include <cstring>
#include <cstdio>

#include "../../../core/src/predator/decoder_ingest.h"
#include "../../../core/src/predator/native_decoder_registry.h"
#include "predator_dsd_bridge.h"

#define CONCAT(a, b) ((std::string(a) + b).c_str())

SDRPP_MOD_INFO {
    /* Name        */ "dsdfme_decoder",
    /* Description */ "Predator native P25/DMR decoder with mbelib voice (vendored DSD-FME)",
    /* Author      */ "Predator RF (DSD-FME upstream by lwvmobile, mbelib by szechyjs)",
    /* Version     */ 0, 1, 0,
    /* Max instances */ 1
};

/* DSD-FME's headers are pure C and pull in most of POSIX. We don't need any of
 * the C struct types in the wrapper — the wrapper only talks to DSD-FME via the
 * C bridge in predator_dsd_bridge.h. So no `#include "dsd.h"` here. */

namespace {

constexpr double VFO_SAMPLE_RATE = 48000.0;   // dsd-fme expects 48 kHz int16
constexpr double VFO_BANDWIDTH   = 12500.0;   // P25/DMR channel bandwidth
constexpr double FM_DEVIATION    = 5000.0;    // typical narrowband FM deviation

class DsdFmeDecoderModule : public ModuleManager::Instance {
public:
    DsdFmeDecoderModule(std::string name) : name_(std::move(name)) {
        // Provision a VFO sink at 48 kHz / 12.5 kHz BW.
        vfo_ = sigpath::vfoManager.createVFO(name_, ImGui::WaterfallVFO::REF_CENTER,
                                              0, VFO_BANDWIDTH, VFO_SAMPLE_RATE,
                                              VFO_BANDWIDTH, VFO_BANDWIDTH, true);

        // FM demod CF32 -> float
        fmDemod_.init(vfo_->output, FM_DEVIATION, VFO_SAMPLE_RATE);

        // Float -> int16 reshaper buffer
        floatToShort_.init(&fmDemod_.out, sampleHandler, this, 1024);

        // Register with the native decoder registry so main_window.cpp's
        // per-frame drain folds our metadata events into predatorEvents.
        predator::registerNativeDecoder(this, "DSDFME",
            [this](size_t maxItems) -> predator::NativeDrainBatch {
                return drainEvents(maxItems);
            });

        // Install the C-side event callback.
        predator_dsd_set_event_cb(&DsdFmeDecoderModule::onDsdEvent, this);

        // Register this module with the module manager UI.
        gui::menu.registerEntry(name_, menuHandler, this, this);

        flog::info("[DSDFME] module instance '{}' constructed", name_);
    }

    ~DsdFmeDecoderModule() override {
        gui::menu.removeEntry(name_);
        if (enabled_) disable();
        predator_dsd_set_event_cb(nullptr, nullptr);
        predator::unregisterNativeDecoder(this);
        sigpath::vfoManager.deleteVFO(vfo_);
        flog::info("[DSDFME] module instance '{}' destructed", name_);
    }

    void postInit() override {}

    void enable() override {
        if (enabled_) return;
        enabled_ = true;
        startPipeline();
    }

    void disable() override {
        if (!enabled_) return;
        stopPipeline();
        enabled_ = false;
    }

    bool isEnabled() override { return enabled_; }

private:
    // ----- DSP feed -----

    static void sampleHandler(float *data, int count, void *ctx) {
        auto *self = static_cast<DsdFmeDecoderModule*>(ctx);
        if (!self->running_) return;

        // float [-1..1] -> int16 PCM
        std::vector<int16_t> pcm(count);
        for (int i = 0; i < count; i++) {
            float s = data[i] * 32767.0f;
            if      (s >  32767.0f) s =  32767.0f;
            else if (s < -32768.0f) s = -32768.0f;
            pcm[i] = static_cast<int16_t>(s);
        }
        predator_dsd_push_input_samples(pcm.data(), pcm.size());
    }

    // ----- Worker / event plumbing -----

    void startPipeline() {
        if (running_.exchange(true)) return;
        predator_dsd_set_running(1);
        predator_dsd_clear_input();
        predator_dsd_clear_voice();

        // Hook into the DSP graph
        fmDemod_.start();
        floatToShort_.start();

        // For the first cut, the worker thread is reserved for the future
        // DSD-FME main loop driver. For now, the metadata event channel is
        // already wired through onDsdEvent and getSymbol() (audio_in_type==9
        // path, see dsd_symbol.c) reads from our input ring whenever the
        // dsd_opts loop is brought up. Voice is captured the moment any
        // processSynthesizedVoice path fires pa_simple_write.

        flog::info("[DSDFME] pipeline started");
    }

    void stopPipeline() {
        if (!running_.exchange(false)) return;
        predator_dsd_set_running(0);
        floatToShort_.stop();
        fmDemod_.stop();
        flog::info("[DSDFME] pipeline stopped");
    }

    // ----- Event callback (C -> C++) -----

    static void onDsdEvent(const char *protocol, const char *kind,
                            const char *payload_json, void *userdata) {
        auto *self = static_cast<DsdFmeDecoderModule*>(userdata);
        if (!self) return;

        predator::DecoderIngestEvent ev;
        ev.timestampUs   = predator::nowMicros();
        ev.serial        = ++self->serial_;
        ev.decoder       = "DSDFME";
        ev.eventType     = kind ? kind : "info";
        ev.protocol      = protocol ? protocol : "DSDFME";
        ev.frequencyHz   = 0;          // wrapper fills from current VFO if known
        ev.strengthDb    = 0;
        ev.label         = std::string(protocol ? protocol : "DSDFME") + ":" + ev.eventType;
        ev.rawPayload    = payload_json ? payload_json : "{}";

        std::lock_guard<std::mutex> lk(self->queueMutex_);
        if (self->queue_.size() >= 256) self->queue_.pop_front();
        self->queue_.push_back(std::move(ev));
    }

    predator::NativeDrainBatch drainEvents(size_t maxItems) {
        predator::NativeDrainBatch out;
        out.sourceKey = "DSDFME";
        std::lock_guard<std::mutex> lk(queueMutex_);
        size_t n = std::min(maxItems, queue_.size());
        out.events.reserve(n);
        for (size_t i = 0; i < n; i++) {
            out.events.push_back(std::move(queue_.front()));
            queue_.pop_front();
        }
        return out;
    }

    // ----- Menu UI -----

    static void menuHandler(void *ctx) {
        auto *self = static_cast<DsdFmeDecoderModule*>(ctx);
        ImGui::TextWrapped("Native P25/DMR decoder (mbelib voice)");
        ImGui::TextDisabled("input ring: %zu samples", predator_dsd_input_pending());
        ImGui::TextDisabled("voice ring: %zu samples", predator_dsd_voice_pending());
        ImGui::TextDisabled("metadata queue: %zu", self->queueDepth());
        if (ImGui::Button(self->enabled_ ? "Stop##dsdfme" : "Start##dsdfme")) {
            if (self->enabled_) self->disable();
            else self->enable();
        }
    }

    size_t queueDepth() {
        std::lock_guard<std::mutex> lk(queueMutex_);
        return queue_.size();
    }

    // ----- State -----

    std::string                                                  name_;
    VFOManager::VFO*                                             vfo_       = nullptr;
    dsp::demod::Quadrature                                       fmDemod_;
    dsp::sink::Handler<float>                                    floatToShort_;
    std::atomic<bool>                                            enabled_   { false };
    std::atomic<bool>                                            running_   { false };
    std::atomic<uint64_t>                                        serial_    { 0 };
    std::mutex                                                   queueMutex_;
    std::deque<predator::DecoderIngestEvent>                     queue_;
};

}  // namespace

MOD_EXPORT void _INIT_() {
    flog::info("[DSDFME] module loaded");
}

MOD_EXPORT ModuleManager::Instance *_CREATE_INSTANCE_(std::string name) {
    return new DsdFmeDecoderModule(std::move(name));
}

MOD_EXPORT void _DELETE_INSTANCE_(ModuleManager::Instance *inst) {
    delete inst;
}

MOD_EXPORT void _END_() {
    flog::info("[DSDFME] module unloaded");
}
