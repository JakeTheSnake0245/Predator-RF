// Fox Hunt PlutoSDR TX driver (libiio TX channels). Compiled into
// plutosdr_source and self-registers with the process-wide TxDriverRegistry
// at shared-object load time. Entirely compiled out unless OPT_BUILD_FOXHUNT.
//
// This is the Android TX path — the Android sdr-kit ships libiio/libad9361
// but not SoapySDR, so on Android the Pluto driver is the only one present.
//
// LOCAL-HARDWARE-ONLY: only reachable from the Fox Hunt tab UI. The Kujhad /
// RNS / control-socket tx.* hard-rejects are untouched.
#ifdef OPT_BUILD_FOXHUNT

#include <predator/foxhunt/tx_driver.h>
#include <config.h>
#include <utils/flog.h>
#include <iio.h>
#include <ad9361.h>
#include <atomic>
#include <cmath>
#include <cstring>

// Shares the module's config (declared in main.cpp) so the enumerate probe
// targets the same IP the operator configured for RX.
extern ConfigManager config;

namespace {

    class PlutoTxDriver : public predator::foxhunt::TxDriver {
    public:
        std::string key() const override { return "plutosdr"; }
        std::string displayName() const override { return "PlutoSDR TX"; }

        std::vector<predator::foxhunt::TxDeviceInfo> enumerate() override {
            std::vector<predator::foxhunt::TxDeviceInfo> out;
            std::string ip = "192.168.2.1";
            config.acquire();
            if (config.conf.contains("IP")) { ip = config.conf["IP"]; }
            config.release();
            std::string uri = "ip:" + ip;

            // Quick reachability probe.
            struct iio_context* probe = iio_create_context_from_uri(uri.c_str());
            if (!probe) {
                flog::warn("FoxHunt pluto probe: no context at {}", uri);
                return out;
            }
            bool hasTx = iio_context_find_device(probe, "cf-ad9361-dds-core-lpc") != NULL
                      && iio_context_find_device(probe, "ad9361-phy") != NULL;
            iio_context_destroy(probe);
            if (!hasTx) { return out; }

            predator::foxhunt::TxDeviceInfo info;
            info.driver = key();
            info.name = "PlutoSDR (" + ip + ")";
            info.id = uri;
            // AD9361 TX attenuation is 0..-89.75 dB; we expose it as gain.
            info.minGainDb = -89.75;
            info.maxGainDb = 0.0;
            info.minSampleRate = 520833.0;
            info.maxSampleRate = 61440000.0;
            info.minBandwidthHz = 200000.0;
            info.maxBandwidthHz = 40000000.0;
            info.hasPowerEstimate = true;
            info.estMaxPowerDbm = 7.0;  // typical AD9361 max output
            out.push_back(info);
            return out;
        }

        bool open(const predator::foxhunt::TxDeviceInfo& devInfo, std::string& err) override {
            close();
            ctx = iio_create_context_from_uri(devInfo.id.c_str());
            if (!ctx) { err = "Could not open " + devInfo.id; return false; }
            phy = iio_context_find_device(ctx, "ad9361-phy");
            dac = iio_context_find_device(ctx, "cf-ad9361-dds-core-lpc");
            if (!phy || !dac) {
                err = "Pluto phy/DAC devices not found";
                close();
                return false;
            }
            return true;
        }

        bool start(double freqHz, double sampleRate, double bandwidthHz,
                   double gainDb, std::string& err) override {
            if (!ctx) { err = "No device open"; return false; }
            stop();

            struct iio_channel* txLo = iio_device_find_channel(phy, "altvoltage1", true);
            struct iio_channel* txChan = iio_device_find_channel(phy, "voltage0", true);
            if (!txLo || !txChan) { err = "TX channels not found on phy"; return false; }

            iio_channel_attr_write(txChan, "rf_port_select", "A");
            iio_channel_attr_write_longlong(txChan, "sampling_frequency", (long long)std::llround(sampleRate));
            if (bandwidthHz > 0.0) {
                iio_channel_attr_write_longlong(txChan, "rf_bandwidth", (long long)std::llround(bandwidthHz));
            }
            iio_channel_attr_write_double(txChan, "hardwaregain", gainDb);
            iio_channel_attr_write_longlong(txLo, "frequency", (long long)std::llround(freqHz));
            iio_channel_attr_write_bool(txLo, "powerdown", false);
            ad9361_set_bb_rate(phy, (unsigned long)std::llround(sampleRate));

            tx0i = iio_device_find_channel(dac, "voltage0", true);
            tx0q = iio_device_find_channel(dac, "voltage1", true);
            if (!tx0i || !tx0q) {
                err = "DAC TX channels not found";
                iio_channel_attr_write_bool(txLo, "powerdown", true);
                return false;
            }
            iio_channel_enable(tx0i);
            iio_channel_enable(tx0q);
            gainChan = txChan;
            streamDead.store(false);
            return true;
        }

        void setGain(double gainDb) override {
            if (!ctx || !gainChan) { return; }
            iio_channel_attr_write_double(gainChan, "hardwaregain", gainDb);
        }

        int write(const std::complex<float>* samples, int count) override {
            if (!ctx || !tx0i || streamDead.load()) { return -1; }
            // (Re)create the buffer if the chunk size changed.
            if (!txbuf || bufSize != count) {
                if (txbuf) { iio_buffer_destroy(txbuf); txbuf = NULL; }
                txbuf = iio_device_create_buffer(dac, count, false);
                if (!txbuf) {
                    flog::error("FoxHunt pluto: could not create TX buffer ({} samples)", count);
                    streamDead.store(true);
                    return -1;
                }
                bufSize = count;
            }
            int16_t* dst = (int16_t*)iio_buffer_first(txbuf, tx0i);
            for (int i = 0; i < count; i++) {
                // AD9361 DAC wants 12-bit MSB-aligned samples (<<4).
                float re = samples[i].real(), im = samples[i].imag();
                if (re > 1.0f) { re = 1.0f; } else if (re < -1.0f) { re = -1.0f; }
                if (im > 1.0f) { im = 1.0f; } else if (im < -1.0f) { im = -1.0f; }
                dst[i * 2]     = (int16_t)std::lround(re * 2047.0f) << 4;
                dst[i * 2 + 1] = (int16_t)std::lround(im * 2047.0f) << 4;
            }
            ssize_t pushed = iio_buffer_push(txbuf);
            if (pushed < 0) {
                flog::error("FoxHunt pluto: buffer push failed ({})", (int)pushed);
                streamDead.store(true);
                return -1;
            }
            return count;
        }

        void stop() override {
            streamDead.store(true);
            if (txbuf) {
                iio_buffer_destroy(txbuf);
                txbuf = NULL;
                bufSize = 0;
            }
            if (tx0i) { iio_channel_disable(tx0i); tx0i = NULL; }
            if (tx0q) { iio_channel_disable(tx0q); tx0q = NULL; }
            if (phy) {
                struct iio_channel* txLo = iio_device_find_channel(phy, "altvoltage1", true);
                if (txLo) { iio_channel_attr_write_bool(txLo, "powerdown", true); }
            }
            gainChan = NULL;
        }

        void close() override {
            stop();
            if (ctx) {
                iio_context_destroy(ctx);
                ctx = NULL;
            }
            phy = NULL;
            dac = NULL;
        }

    private:
        struct iio_context* ctx = NULL;
        struct iio_device* phy = NULL;
        struct iio_device* dac = NULL;
        struct iio_channel* tx0i = NULL;
        struct iio_channel* tx0q = NULL;
        struct iio_channel* gainChan = NULL;
        struct iio_buffer* txbuf = NULL;
        int bufSize = 0;
        std::atomic<bool> streamDead{true};
    };

    PlutoTxDriver plutoTxDriver;

    struct Registrar {
        Registrar() { predator::foxhunt::TxDriverRegistry::instance().registerDriver(&plutoTxDriver); }
        ~Registrar() {
            plutoTxDriver.close();
            predator::foxhunt::TxDriverRegistry::instance().unregisterDriver(&plutoTxDriver);
        }
    } registrar;
}

#endif // OPT_BUILD_FOXHUNT
