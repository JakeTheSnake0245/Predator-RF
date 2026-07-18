// Fox Hunt SoapySDR TX driver. Compiled into soapy_source (the module CMake
// globs src/*.cpp) and self-registers with the process-wide TxDriverRegistry
// at shared-object load time. Entirely compiled out unless OPT_BUILD_FOXHUNT.
//
// LOCAL-HARDWARE-ONLY: this driver is only reachable from the Fox Hunt tab UI.
// The Kujhad / RNS / control-socket tx.* hard-rejects are untouched.
#ifdef OPT_BUILD_FOXHUNT

#include <predator/foxhunt/tx_driver.h>
#include <SoapySDR/Device.hpp>
#include <SoapySDR/Constants.h>
#include <utils/flog.h>
#include <algorithm>
#include <atomic>

namespace {

    class SoapyTxDriver : public predator::foxhunt::TxDriver {
    public:
        std::string key() const override { return "soapy"; }
        std::string displayName() const override { return "SoapySDR TX"; }

        std::vector<predator::foxhunt::TxDeviceInfo> enumerate() override {
            std::vector<predator::foxhunt::TxDeviceInfo> out;
            SoapySDR::KwargsList devList;
            try {
                devList = SoapySDR::Device::enumerate();
            }
            catch (const std::exception& e) {
                flog::error("FoxHunt soapy enumerate failed: {}", e.what());
                return out;
            }
            for (auto& args : devList) {
                try {
                    SoapySDR::Device* d = SoapySDR::Device::make(args);
                    if (!d) { continue; }
                    if (d->getNumChannels(SOAPY_SDR_TX) > 0) {
                        predator::foxhunt::TxDeviceInfo info;
                        info.driver = key();
                        info.name = args.count("label") ? args.at("label")
                                  : args.count("driver") ? args.at("driver") : "Soapy device";
                        info.id = SoapySDR::KwargsToString(args);
                        SoapySDR::Range gr = d->getGainRange(SOAPY_SDR_TX, 0);
                        info.minGainDb = gr.minimum();
                        info.maxGainDb = gr.maximum();
                        auto srs = d->getSampleRateRange(SOAPY_SDR_TX, 0);
                        if (!srs.empty()) {
                            info.minSampleRate = srs.front().minimum();
                            info.maxSampleRate = srs.back().maximum();
                        }
                        auto bws = d->getBandwidthRange(SOAPY_SDR_TX, 0);
                        if (!bws.empty()) {
                            info.minBandwidthHz = bws.front().minimum();
                            info.maxBandwidthHz = bws.back().maximum();
                        }
                        out.push_back(info);
                    }
                    SoapySDR::Device::unmake(d);
                }
                catch (const std::exception& e) {
                    flog::warn("FoxHunt soapy probe failed for a device: {}", e.what());
                }
            }
            return out;
        }

        bool open(const predator::foxhunt::TxDeviceInfo& devInfo, std::string& err) override {
            close();
            try {
                dev = SoapySDR::Device::make(SoapySDR::KwargsFromString(devInfo.id));
            }
            catch (const std::exception& e) {
                err = e.what();
                dev = NULL;
                return false;
            }
            if (!dev) {
                err = "SoapySDR::Device::make returned null";
                return false;
            }
            return true;
        }

        bool start(double freqHz, double sampleRate, double bandwidthHz,
                   double gainDb, std::string& err) override {
            if (!dev) { err = "No device open"; return false; }
            if (stream) { stop(); }
            try {
                dev->setSampleRate(SOAPY_SDR_TX, 0, sampleRate);
                if (bandwidthHz > 0.0) { dev->setBandwidth(SOAPY_SDR_TX, 0, bandwidthHz); }
                dev->setFrequency(SOAPY_SDR_TX, 0, freqHz);
                dev->setGain(SOAPY_SDR_TX, 0, gainDb);
                stream = dev->setupStream(SOAPY_SDR_TX, SOAPY_SDR_CF32);
                if (!stream) { err = "setupStream failed"; return false; }
                if (dev->activateStream(stream) != 0) {
                    dev->closeStream(stream);
                    stream = NULL;
                    err = "activateStream failed";
                    return false;
                }
            }
            catch (const std::exception& e) {
                err = e.what();
                if (stream) { dev->closeStream(stream); stream = NULL; }
                return false;
            }
            streamDead.store(false);
            return true;
        }

        void setGain(double gainDb) override {
            if (!dev) { return; }
            try {
                dev->setGain(SOAPY_SDR_TX, 0, gainDb);
            }
            catch (const std::exception& e) {
                flog::warn("FoxHunt soapy setGain failed: {}", e.what());
            }
        }

        int write(const std::complex<float>* samples, int count) override {
            if (!dev || !stream || streamDead.load()) { return -1; }
            const void* buffs[1];
            int sent = 0;
            while (sent < count) {
                buffs[0] = (const void*)(samples + sent);
                int flags = 0;
                int ret = dev->writeStream(stream, buffs, count - sent, flags, 0, 1000000 /*1s*/);
                if (ret == SOAPY_SDR_TIMEOUT) { continue; }
                if (ret <= 0) {
                    flog::error("FoxHunt soapy writeStream error: {}", ret);
                    streamDead.store(true);
                    return -1;
                }
                sent += ret;
            }
            return sent;
        }

        void stop() override {
            if (dev && stream) {
                try {
                    dev->deactivateStream(stream);
                    dev->closeStream(stream);
                }
                catch (const std::exception& e) {
                    flog::warn("FoxHunt soapy stream teardown: {}", e.what());
                }
            }
            stream = NULL;
            streamDead.store(true);
        }

        void close() override {
            stop();
            if (dev) {
                try { SoapySDR::Device::unmake(dev); }
                catch (const std::exception&) {}
                dev = NULL;
            }
        }

    private:
        SoapySDR::Device* dev = NULL;
        SoapySDR::Stream* stream = NULL;
        std::atomic<bool> streamDead{true};
    };

    SoapyTxDriver soapyTxDriver;

    struct Registrar {
        Registrar() { predator::foxhunt::TxDriverRegistry::instance().registerDriver(&soapyTxDriver); }
        ~Registrar() {
            soapyTxDriver.close();
            predator::foxhunt::TxDriverRegistry::instance().unregisterDriver(&soapyTxDriver);
        }
    } registrar;
}

#endif // OPT_BUILD_FOXHUNT
