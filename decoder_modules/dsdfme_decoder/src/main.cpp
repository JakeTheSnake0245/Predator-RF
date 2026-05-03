/*
 * Predator RF — Native DSD-FME decoder module (P25 + DMR + voice).
 *
 * SDRPP module wrapper around the vendored DSD-FME demodulator + mbelib AMBE+2
 * voice codec. Runs as an in-APK plugin; no external companion processes.
 *
 * Pipeline (Phase 3b — runtime hookup):
 *
 *   SDRPP DSP graph (CF32 baseband)
 *     -> VFO @ 48 kHz, 12.5 kHz BW
 *     -> dsp::demod::Quadrature FM demod -> float
 *     -> Handler<float> sampleHandler -> int16 PCM
 *     -> predator_dsd_push_input_samples()           (predator_dsd_bridge.h)
 *
 *   Decoder worker thread:
 *     -> predator_dsd_run_decoder_loop()
 *        ->  initOpts()/initState()/init_audio_filters() once
 *        ->  liveScanner() blocks on getFrameSync/processFrame loop
 *        ->  getSymbol() (audio_in_type==9 path) pulls from input ring
 *        ->  processFrame -> processMbeFrame -> mbelib synthesizes 8 kHz int16
 *        ->  pa_simple_write() stub captures into the voice ring
 *
 *   Audio pump thread:
 *     -> predator_dsd_pull_voice_samples() 8 kHz int16 mono
 *     -> convert to float [-1, 1]
 *     -> push into rawVoice_ stream (writeBuf + swap)
 *
 *   Audio sink chain:
 *     rawVoice_ (8 kHz float mono)
 *     -> RationalResampler<float> -> audioSampRate_ float mono
 *     -> MonoToStereo -> dsp::stream<dsp::stereo_t>
 *     -> SinkManager::Stream registered with sigpath::sinkManager
 *
 *   Metadata stream:
 *     -> predator_dsd_set_event_cb() callback fires for each protocol event
 *     -> queue<DecoderIngestEvent> drained by main_window per-frame via the
 *        registerNativeDecoder() hook.
 */

#include <imgui.h>
#include <module.h>
#include <gui/gui.h>
#include <signal_path/signal_path.h>
#include <signal_path/sink.h>
#include <dsp/stream.h>
#include <dsp/buffer/reshaper.h>
#include <dsp/multirate/rational_resampler.h>
#include <dsp/convert/mono_to_stereo.h>
#include <dsp/demod/quadrature.h>
#include <dsp/sink/handler_sink.h>
#include <utils/flog.h>
#include <utils/event.h>
#include <config.h>
#include <atomic>
#include <thread>
#include <mutex>
#include <deque>
#include <chrono>
#include <cstring>
#include <cstdio>
#include <cmath>

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

namespace {

constexpr double VFO_SAMPLE_RATE  = 48000.0;   // dsd-fme expects 48 kHz int16
constexpr double VFO_BANDWIDTH    = 12500.0;   // P25/DMR channel bandwidth
constexpr double FM_DEVIATION     = 5000.0;    // typical narrowband FM deviation
constexpr double VOICE_INPUT_RATE = 8000.0;    // mbelib synthesizes at 8 kHz
constexpr size_t VOICE_PUMP_BATCH = 1024;      // samples per pump iteration
constexpr int    VOICE_IDLE_MS    = 5;         // sleep when ring is empty

class DsdFmeDecoderModule : public ModuleManager::Instance {
public:
    DsdFmeDecoderModule(std::string name) : name_(std::move(name)) {
        // ----- VFO + FM demod (RF input side) -----
        vfo_ = sigpath::vfoManager.createVFO(name_, ImGui::WaterfallVFO::REF_CENTER,
                                              0, VFO_BANDWIDTH, VFO_SAMPLE_RATE,
                                              VFO_BANDWIDTH, VFO_BANDWIDTH, true);
        fmDemod_.init(vfo_->output, FM_DEVIATION, VFO_SAMPLE_RATE);
        // Handler<T>::init(stream<T>*, void(*)(T*,int,void*), void*) — 3 args.
        floatToShort_.init(&fmDemod_.out, sampleHandler, this);

        // ----- Audio sink chain (voice output side) -----
        // rawVoice_ is a hand-pumped stream (no Processor feeding it). We pre-
        // allocate its buffer; the pump thread fills writeBuf and calls swap().
        // resamp_ pulls from rawVoice_ and produces audioSampRate_ floats.
        // monoToStereo_ duplicates each float into stereo_t for the sink.
        resamp_.init(&rawVoice_, VOICE_INPUT_RATE, audioSampRate_);
        monoToStereo_.init(&resamp_.out);

        srChangeHandler_.ctx     = this;
        srChangeHandler_.handler = sampleRateChangeHandler;
        stream_.init(&monoToStereo_.out, &srChangeHandler_, audioSampRate_);
        sigpath::sinkManager.registerStream(name_, &stream_);

        // ----- Metadata events: register with native decoder registry -----
        // NativeDrainFn returns std::vector<DecoderIngestEvent>; the registry
        // tags each batch with the sourceKey ("DSDFME") on its own.
        predator::registerNativeDecoder(this, "DSDFME",
            [this](std::size_t maxItems) -> std::vector<predator::DecoderIngestEvent> {
                return drainEvents(maxItems);
            });
        predator_dsd_set_event_cb(&DsdFmeDecoderModule::onDsdEvent, this);

        // Pre-build the decoder state once at construct time so the first
        // enable() doesn't pay the ~6 MB initState() malloc latency.
        predator_dsd_init_decoder();

        gui::menu.registerEntry(name_, menuHandler, this, this);
        flog::info("[DSDFME] module instance '{}' constructed", name_);
    }

    ~DsdFmeDecoderModule() {
        gui::menu.removeEntry(name_);
        if (enabled_) disable();
        predator_dsd_set_event_cb(nullptr, nullptr);
        predator::unregisterNativeDecoder(this);
        sigpath::sinkManager.unregisterStream(name_);
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
    // ===== RF input handler: float -> int16 -> input ring =====

    static void sampleHandler(float* data, int count, void* ctx) {
        auto* self = static_cast<DsdFmeDecoderModule*>(ctx);
        if (!self->running_.load(std::memory_order_acquire)) return;

        std::vector<int16_t> pcm(count);
        for (int i = 0; i < count; i++) {
            float s = data[i] * 32767.0f;
            if      (s >  32767.0f) s =  32767.0f;
            else if (s < -32768.0f) s = -32768.0f;
            pcm[i] = static_cast<int16_t>(s);
        }
        predator_dsd_push_input_samples(pcm.data(), pcm.size());
    }

    // ===== Voice pump: voice ring (8 kHz int16) -> rawVoice_ stream (float) =====

    static void voicePumpThread(DsdFmeDecoderModule* self) {
        flog::info("[DSDFME] voice pump thread started");
        int16_t buf[VOICE_PUMP_BATCH];
        while (self->running_.load(std::memory_order_acquire)) {
            size_t n = predator_dsd_pull_voice_samples(buf, VOICE_PUMP_BATCH);
            if (n == 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(VOICE_IDLE_MS));
                continue;
            }
            // int16 -> float [-1.0, 1.0)
            for (size_t i = 0; i < n; i++) {
                self->rawVoice_.writeBuf[i] = static_cast<float>(buf[i]) / 32768.0f;
            }
            if (!self->rawVoice_.swap(static_cast<int>(n))) break;  // stream stopped
        }
        flog::info("[DSDFME] voice pump thread exited");
    }

    // ===== Decoder worker: drives the upstream liveScanner loop =====

    static void decoderWorkerThread() {
        flog::info("[DSDFME] decoder worker thread started");
        // Blocks until exitflag flips. Set by predator_dsd_set_running(0).
        predator_dsd_run_decoder_loop();
        flog::info("[DSDFME] decoder worker thread exited");
    }

    // ===== Lifecycle =====

    void startPipeline() {
        bool was_running = running_.exchange(true);
        if (was_running) return;

        predator_dsd_clear_input();
        predator_dsd_clear_voice();
        predator_dsd_set_running(1);

        // RF input chain
        fmDemod_.start();
        floatToShort_.start();

        // Audio output chain
        resamp_.start();
        monoToStereo_.start();
        stream_.start();

        // Worker threads
        decoderWorker_ = std::thread(&decoderWorkerThread);
        voicePump_     = std::thread(&voicePumpThread, this);

        flog::info("[DSDFME] pipeline started (audioSampRate={} Hz)", audioSampRate_);
    }

    void stopPipeline() {
        bool was_running = running_.exchange(false);
        if (!was_running) return;

        // Signal both worker threads to wind down.
        predator_dsd_set_running(0);

        // Stop the pumped stream first; this makes rawVoice_.swap() return false
        // so the voice pump thread breaks out of its loop instead of blocking.
        rawVoice_.stopWriter();

        if (voicePump_.joinable())     voicePump_.join();
        if (decoderWorker_.joinable()) decoderWorker_.join();

        // Re-arm the stream for the next start cycle.
        rawVoice_.clearWriteStop();

        // Stop DSP chain
        stream_.stop();
        monoToStereo_.stop();
        resamp_.stop();
        floatToShort_.stop();
        fmDemod_.stop();

        flog::info("[DSDFME] pipeline stopped");
    }

    // ===== Sample-rate change (audio device switched) =====

    static void sampleRateChangeHandler(float sampleRate, void* ctx) {
        // setOutSamplerate() already takes ctrlMtx + does tempStop/reconfigure/
        // tempStart internally — calling it directly avoids a redundant
        // stop/start cycle while the voice pump may be writing.
        auto* self = static_cast<DsdFmeDecoderModule*>(ctx);
        self->audioSampRate_ = sampleRate;
        self->resamp_.setOutSamplerate(sampleRate);
        flog::info("[DSDFME] audio sample rate -> {} Hz", sampleRate);
    }

    // ===== Event callback (C -> C++) =====

    static void onDsdEvent(const char* protocol, const char* kind,
                            const char* payload_json, void* userdata) {
        auto* self = static_cast<DsdFmeDecoderModule*>(userdata);
        if (!self) return;

        // DecoderIngestEvent fields: decoder, protocol, networkId, talkgroup,
        // radioId, label, frequencyHz, strengthDb, raw (nlohmann::json).
        // We fold kind/timestamp/serial/payload into `raw` since the canonical
        // struct doesn't carry them as first-class fields.
        const char* kindStr = kind ? kind : "info";
        const char* protoStr = protocol ? protocol : "DSDFME";
        const char* payloadStr = payload_json ? payload_json : "{}";
        const auto nowUs = std::chrono::duration_cast<std::chrono::microseconds>(
                              std::chrono::system_clock::now().time_since_epoch()).count();
        const uint64_t serial = ++self->serial_;

        predator::DecoderIngestEvent ev;
        ev.decoder     = "DSDFME";
        ev.protocol    = protoStr;
        ev.frequencyHz = 0.0;
        ev.strengthDb  = 0.0f;
        ev.label       = std::string(protoStr) + ":" + kindStr;
        ev.raw = {
            {"kind",        kindStr},
            {"timestampUs", static_cast<int64_t>(nowUs)},
            {"serial",      serial},
            {"payload",     payloadStr},
        };

        std::lock_guard<std::mutex> lk(self->queueMutex_);
        if (self->queue_.size() >= 256) self->queue_.pop_front();
        self->queue_.push_back(std::move(ev));
    }

    std::vector<predator::DecoderIngestEvent> drainEvents(std::size_t maxItems) {
        std::vector<predator::DecoderIngestEvent> out;
        std::lock_guard<std::mutex> lk(queueMutex_);
        const std::size_t n = std::min(maxItems, queue_.size());
        out.reserve(n);
        for (std::size_t i = 0; i < n; i++) {
            out.push_back(std::move(queue_.front()));
            queue_.pop_front();
        }
        return out;
    }

    // ===== UI =====

    static void menuHandler(void* ctx) {
        auto* self = static_cast<DsdFmeDecoderModule*>(ctx);
        ImGui::TextWrapped("Native P25/DMR decoder (mbelib voice)");
        ImGui::TextDisabled("input ring:    %zu samples", predator_dsd_input_pending());
        ImGui::TextDisabled("voice ring:    %zu samples", predator_dsd_voice_pending());
        ImGui::TextDisabled("audio rate:    %.0f Hz",     self->audioSampRate_);
        ImGui::TextDisabled("metadata queue: %zu",        self->queueDepth());
        if (ImGui::Button(self->enabled_ ? "Stop##dsdfme" : "Start##dsdfme")) {
            if (self->enabled_) self->disable();
            else                 self->enable();
        }
    }

    size_t queueDepth() {
        std::lock_guard<std::mutex> lk(queueMutex_);
        return queue_.size();
    }

    // ===== State =====

    std::string                                       name_;

    // RF input chain
    VFOManager::VFO*                                  vfo_       = nullptr;
    dsp::demod::Quadrature                            fmDemod_;
    dsp::sink::Handler<float>                         floatToShort_;

    // Audio output chain
    dsp::stream<float>                                rawVoice_;
    dsp::multirate::RationalResampler<float>          resamp_;
    dsp::convert::MonoToStereo                        monoToStereo_;
    SinkManager::Stream                               stream_;
    EventHandler<float>                               srChangeHandler_;
    double                                            audioSampRate_ = 48000.0;

    // Worker threads
    std::thread                                       decoderWorker_;
    std::thread                                       voicePump_;

    // Lifecycle / metadata
    std::atomic<bool>                                 enabled_   { false };
    std::atomic<bool>                                 running_   { false };
    std::atomic<uint64_t>                             serial_    { 0 };
    std::mutex                                        queueMutex_;
    std::deque<predator::DecoderIngestEvent>          queue_;
};

}  // namespace

MOD_EXPORT void _INIT_() {
    flog::info("[DSDFME] module loaded");
}

MOD_EXPORT ModuleManager::Instance* _CREATE_INSTANCE_(std::string name) {
    return new DsdFmeDecoderModule(std::move(name));
}

MOD_EXPORT void _DELETE_INSTANCE_(ModuleManager::Instance* inst) {
    delete inst;
}

MOD_EXPORT void _END_() {
    flog::info("[DSDFME] module unloaded");
}
