#ifdef FOXHUNT_ENABLE_SOAPY
#include "tx_driver.h"
#include <utils/flog.h>
#include <SoapySDR/Device.hpp>
#include <SoapySDR/Formats.hpp>
#include <algorithm>

// SoapySDR TX driver for the Fox Hunt module (Linux/RPi builds).
// Mirrors the RX usage in source_modules/soapy_source with SOAPY_SDR_TX.

namespace foxhunt {

    class SoapyTxDriver : public TxDriver {
    public:
        std::string id() override { return "soapy"; }

        std::vector<TxDeviceInfo> enumerate() override {
            std::vector<TxDeviceInfo> out;
            try {
                auto devs = SoapySDR::Device::enumerate();
                for (auto& args : devs) {
                    // Only list devices that actually expose TX channels.
                    SoapySDR::Device* d = NULL;
                    try {
                        d = SoapySDR::Device::make(args);
                        if (d && d->getNumChannels(SOAPY_SDR_TX) > 0) {
                            TxDeviceInfo info;
                            info.driverId = "soapy";
                            info.label = args.count("label") ? args.at("label") : args.at("driver");
                            info.uri = SoapySDR::KwargsToString(args);
                            out.push_back(info);
                        }
                    }
                    catch (const std::exception& e) {
                        flog::warn("FoxHunt soapy probe failed: {}", e.what());
                    }
                    if (d) { SoapySDR::Device::unmake(d); }
                }
            }
            catch (const std::exception& e) {
                flog::error("FoxHunt soapy enumerate failed: {}", e.what());
            }
            return out;
        }

        bool caps(const TxDeviceInfo& devInfo, TxCaps& out, std::string& err) override {
            SoapySDR::Device* d = NULL;
            try {
                d = SoapySDR::Device::make(devInfo.uri);
                if (!d || d->getNumChannels(SOAPY_SDR_TX) < 1) {
                    err = "No TX channel";
                    if (d) { SoapySDR::Device::unmake(d); }
                    return false;
                }
                auto gr = d->getGainRange(SOAPY_SDR_TX, 0);
                out.gainMin = gr.minimum();
                out.gainMax = gr.maximum();
                out.gainStep = (gr.step() > 0.0) ? gr.step() : 1.0;
                out.sampleRates = d->listSampleRates(SOAPY_SDR_TX, 0);
                std::sort(out.sampleRates.begin(), out.sampleRates.end());
                auto srr = d->getSampleRateRange(SOAPY_SDR_TX, 0);
                if (!srr.empty()) { out.srMin = srr.front().minimum(); out.srMax = srr.back().maximum(); }
                auto bwr = d->getBandwidthRange(SOAPY_SDR_TX, 0);
                if (!bwr.empty()) { out.bwMin = bwr.front().minimum(); out.bwMax = bwr.back().maximum(); }
                auto fr = d->getFrequencyRange(SOAPY_SDR_TX, 0);
                if (!fr.empty()) { out.freqMin = fr.front().minimum(); out.freqMax = fr.back().maximum(); }
                out.hasPowerEstimate = false;
                SoapySDR::Device::unmake(d);
                return true;
            }
            catch (const std::exception& e) {
                err = e.what();
                if (d) { SoapySDR::Device::unmake(d); }
                return false;
            }
        }

        bool open(const TxDeviceInfo& devInfo, double sampleRate, double freqHz,
                  double bwHz, double gainDb, std::string& err) override {
            close();
            try {
                dev = SoapySDR::Device::make(devInfo.uri);
                if (!dev || dev->getNumChannels(SOAPY_SDR_TX) < 1) {
                    err = "No TX channel";
                    close();
                    return false;
                }
                dev->setSampleRate(SOAPY_SDR_TX, 0, sampleRate);
                if (bwHz > 0.0) { dev->setBandwidth(SOAPY_SDR_TX, 0, bwHz); }
                dev->setGain(SOAPY_SDR_TX, 0, gainDb);
                dev->setFrequency(SOAPY_SDR_TX, 0, freqHz);
                stream = dev->setupStream(SOAPY_SDR_TX, SOAPY_SDR_CF32);
                if (!stream) { err = "setupStream(TX) failed"; close(); return false; }
                mtu = (int)dev->getStreamMTU(stream);
                if (mtu <= 0) { mtu = 8192; }
                dev->activateStream(stream);
                return true;
            }
            catch (const std::exception& e) {
                err = e.what();
                close();
                return false;
            }
        }

        void setFrequency(double freqHz) override {
            if (dev) { try { dev->setFrequency(SOAPY_SDR_TX, 0, freqHz); } catch (...) {} }
        }

        void setGain(double gainDb) override {
            if (dev) { try { dev->setGain(SOAPY_SDR_TX, 0, gainDb); } catch (...) {} }
        }

        int write(const float* iq, int count) override {
            if (!dev || !stream) { return -1; }
            int n = std::min(count, mtu);
            const void* bufs[1] = { iq };
            int flags = 0;
            int r = dev->writeStream(stream, bufs, n, flags, 0, 400000);
            if (r == SOAPY_SDR_TIMEOUT) { return 0; }
            return r;
        }

        void close() override {
            if (dev && stream) {
                try {
                    dev->deactivateStream(stream);
                    dev->closeStream(stream);
                }
                catch (...) {}
            }
            stream = NULL;
            if (dev) {
                try { SoapySDR::Device::unmake(dev); } catch (...) {}
                dev = NULL;
            }
        }

    private:
        SoapySDR::Device* dev = NULL;
        SoapySDR::Stream* stream = NULL;
        int mtu = 8192;
    };

    void registerSoapyTxDriver() {
        driverRegistry().push_back(std::make_shared<SoapyTxDriver>());
    }
}
#endif // FOXHUNT_ENABLE_SOAPY
