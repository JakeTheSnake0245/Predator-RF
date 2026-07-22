// Fox Hunt HackRF TX driver (libhackrf TX stream). Compiled into
// hackrf_source and self-registers with the process-wide TxDriverRegistry at
// shared-object load time. Entirely compiled out unless OPT_BUILD_FOXHUNT.
//
// This is the primary Android TX path for HackRF hardware: the previous
// drivers were SoapySDR (desktop only — OPT_BUILD_SOAPY_SOURCE=OFF on
// Android) and PlutoSDR (network device), so a HackRF plugged into the
// phone could never be selected in the Fox Hunt tab.
//
// LOCAL-HARDWARE-ONLY: only reachable from the Fox Hunt tab UI. The Kujhad /
// RNS / control-socket tx.* hard-rejects are untouched.
//
// Threading: open/start/stop/close on the UI thread; write() from the replay
// engine worker. libhackrf's TX callback runs on its own USB thread and
// drains a ring buffer that write() fills (blocking = backpressure paces the
// replay worker, per the TxDriver contract).
#ifdef OPT_BUILD_FOXHUNT

#include <predator/foxhunt/tx_driver.h>
#include <utils/flog.h>

#ifndef __ANDROID__
#include <libhackrf/hackrf.h>
#else
#include <android_backend.h>
#include <hackrf.h>
#include <libusb.h>
#endif

#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <vector>

namespace {

    class HackRFTxDriver : public predator::foxhunt::TxDriver {
    public:
        std::string key() const override { return "hackrf"; }
        std::string displayName() const override { return "HackRF TX"; }

        // hackrf_init() is refcounted inside libhackrf; safe alongside the
        // RX source module's own init. On Android, LIBUSB_OPTION_NO_DEVICE_
        // DISCOVERY MUST be set (NULL ctx) before the first libusb_init in
        // the process or init fails and the first open segfaults (see
        // docs/android_gotchas.md). Do it here too — self-sufficient, no
        // reliance on the RX module's constructor having run first.
        static bool initHackrf() {
#ifdef __ANDROID__
            libusb_set_option(NULL, LIBUSB_OPTION_NO_DEVICE_DISCOVERY, NULL);
#endif
            int e = hackrf_init();
            if (e != HACKRF_SUCCESS) {
                flog::error("FoxHunt hackrf: hackrf_init() failed: {}", hackrf_error_name((hackrf_error)e));
                return false;
            }
            return true;
        }

        std::vector<predator::foxhunt::TxDeviceInfo> enumerate() override {
            std::vector<predator::foxhunt::TxDeviceInfo> out;
            if (!initHackrf()) { return out; }

#ifndef __ANDROID__
            hackrf_device_list_t* list = hackrf_device_list();
            if (!list) { return out; }
            for (int i = 0; i < list->devicecount; i++) {
                if (!list->serial_numbers[i]) { continue; }
                predator::foxhunt::TxDeviceInfo info;
                fillCommon(info);
                std::string serial = list->serial_numbers[i];
                info.name = "HackRF " + serial.substr(serial.size() > 8 ? serial.size() - 8 : 0);
                info.id = serial;
                out.push_back(info);
            }
            hackrf_device_list_free(list);
#else
            // Android: no usbfs enumeration — ask the activity's granted-
            // device list for a HackRF fd. Do NOT cache the fd here; it is
            // re-acquired at open() time (stale fds segfault in
            // libusb_wrap_sys_device).
            int vid = 0, pid = 0;
            int fd = backend::getDeviceFD(vid, pid, backend::HACKRF_VIDPIDS);
            if (fd >= 0) {
                predator::foxhunt::TxDeviceInfo info;
                fillCommon(info);
                info.name = "HackRF USB";
                info.id = "android_fd";
                out.push_back(info);
            }
#endif
            return out;
        }

        bool open(const predator::foxhunt::TxDeviceInfo& devInfo, std::string& err) override {
            close();
            if (!initHackrf()) {
                err = "hackrf_init() failed";
                return false;
            }
#ifndef __ANDROID__
            hackrf_error e = (hackrf_error)hackrf_open_by_serial(devInfo.id.c_str(), &dev);
#else
            int vid = 0, pid = 0;
            int fd = backend::getDeviceFD(vid, pid, backend::HACKRF_VIDPIDS);
            if (fd < 0) {
                err = "No HackRF USB fd (device disconnected or permission not granted)";
                return false;
            }
            hackrf_error e = (hackrf_error)hackrf_open_by_fd(fd, &dev);
#endif
            if (e != HACKRF_SUCCESS) {
                dev = NULL;
                err = std::string("Could not open HackRF: ") + hackrf_error_name(e);
                err += " (stop the HackRF RX source first — the device is single-user)";
                return false;
            }
            return true;
        }

        bool start(double freqHz, double sampleRate, double bandwidthHz,
                   double gainDb, std::string& err) override {
            if (!dev) { err = "No device open"; return false; }
            stop();

            hackrf_set_sample_rate(dev, sampleRate);
            uint32_t bw = (bandwidthHz > 0.0)
                ? hackrf_compute_baseband_filter_bw((uint32_t)std::llround(bandwidthHz))
                : hackrf_compute_baseband_filter_bw((uint32_t)std::llround(sampleRate));
            hackrf_set_baseband_filter_bandwidth(dev, bw);
            hackrf_set_freq(dev, (uint64_t)std::llround(freqHz));
            hackrf_set_amp_enable(dev, 0);
            setGain(gainDb);

            {
                std::lock_guard<std::mutex> lck(ringMtx);
                ring.clear();
                ringHead = 0;
                streamDead.store(false);
                stopping.store(false);
            }

            hackrf_error e = (hackrf_error)hackrf_start_tx(dev, txCallback, this);
            if (e != HACKRF_SUCCESS) {
                err = std::string("hackrf_start_tx failed: ") + hackrf_error_name(e);
                streamDead.store(true);
                return false;
            }
            return true;
        }

        bool setFrequency(double freqHz) override {
            if (!dev) { return false; }
            // hackrf_set_freq is a control transfer — safe to issue from the
            // UI thread while the TX streaming callback keeps running
            // (Fox Hunt sweep mode retunes live at a few steps/second).
            return hackrf_set_freq(dev, (uint64_t)std::llround(freqHz)) == HACKRF_SUCCESS;
        }

        void setGain(double gainDb) override {
            if (!dev) { return; }
            // HackRF TX VGA range is 0..47 dB in 1 dB steps.
            int g = (int)std::lround(gainDb);
            if (g < 0) { g = 0; }
            if (g > 47) { g = 47; }
            hackrf_set_txvga_gain(dev, g);
        }

        int write(const std::complex<float>* samples, int count) override {
            if (!dev || streamDead.load()) { return -1; }
            std::unique_lock<std::mutex> lck(ringMtx);
            // Backpressure: block until the ring has room (or the stream
            // dies). Cap the ring at ~256k samples (~0.25 s at 1 Msps) so
            // gain changes stay responsive.
            ringCv.wait(lck, [&] {
                return streamDead.load() || stopping.load()
                    || (ring.size() - ringHead) + (size_t)count <= MAX_RING_SAMPLES;
            });
            if (streamDead.load() || stopping.load()) { return -1; }
            // Compact consumed samples occasionally.
            if (ringHead > MAX_RING_SAMPLES) {
                ring.erase(ring.begin(), ring.begin() + ringHead);
                ringHead = 0;
            }
            ring.insert(ring.end(), samples, samples + count);
            return count;
        }

        void stop() override {
            {
                std::lock_guard<std::mutex> lck(ringMtx);
                stopping.store(true);
            }
            ringCv.notify_all();
            if (dev && !streamDead.load()) {
                hackrf_stop_tx(dev);
            }
            {
                std::lock_guard<std::mutex> lck(ringMtx);
                ring.clear();
                ringHead = 0;
                streamDead.store(true);
            }
        }

        void close() override {
            stop();
            if (dev) {
                hackrf_close(dev);
                dev = NULL;
            }
        }

    private:
        static void fillCommon(predator::foxhunt::TxDeviceInfo& info) {
            info.driver = "hackrf";
            info.minGainDb = 0.0;
            info.maxGainDb = 47.0;
            info.minSampleRate = 2000000.0;
            info.maxSampleRate = 20000000.0;
            info.minBandwidthHz = 1750000.0;
            info.maxBandwidthHz = 28000000.0;
            info.hasPowerEstimate = true;
            info.estMaxPowerDbm = 10.0; // ~+10 dBm typ. below 2.4 GHz, VGA max, amp off
        }

        static int txCallback(hackrf_transfer* transfer) {
            HackRFTxDriver* _this = (HackRFTxDriver*)transfer->tx_ctx;
            int8_t* dst = (int8_t*)transfer->buffer;
            int need = transfer->valid_length / 2; // complex samples
            int filled = 0;
            {
                std::lock_guard<std::mutex> lck(_this->ringMtx);
                size_t avail = _this->ring.size() - _this->ringHead;
                int take = (int)((avail < (size_t)need) ? avail : (size_t)need);
                const std::complex<float>* src = _this->ring.data() + _this->ringHead;
                for (int i = 0; i < take; i++) {
                    float re = src[i].real(), im = src[i].imag();
                    if (re > 1.0f) { re = 1.0f; } else if (re < -1.0f) { re = -1.0f; }
                    if (im > 1.0f) { im = 1.0f; } else if (im < -1.0f) { im = -1.0f; }
                    dst[i * 2]     = (int8_t)std::lround(re * 127.0f);
                    dst[i * 2 + 1] = (int8_t)std::lround(im * 127.0f);
                }
                _this->ringHead += take;
                filled = take;
            }
            // Underrun: pad with silence rather than aborting the stream —
            // the replay worker may just be between files/loops.
            if (filled < need) {
                memset(dst + filled * 2, 0, (need - filled) * 2);
            }
            _this->ringCv.notify_all();
            return 0; // keep streaming; stop() ends the stream explicitly
        }

        hackrf_device* dev = NULL;

        static constexpr size_t MAX_RING_SAMPLES = 262144;
        std::vector<std::complex<float>> ring;
        size_t ringHead = 0;
        std::mutex ringMtx;
        std::condition_variable ringCv;
        std::atomic<bool> streamDead{true};
        std::atomic<bool> stopping{false};
    };

    HackRFTxDriver hackrfTxDriver;

    struct Registrar {
        Registrar() { predator::foxhunt::TxDriverRegistry::instance().registerDriver(&hackrfTxDriver); }
        ~Registrar() {
            hackrfTxDriver.close();
            predator::foxhunt::TxDriverRegistry::instance().unregisterDriver(&hackrfTxDriver);
        }
    } registrar;
}

#endif // OPT_BUILD_FOXHUNT
