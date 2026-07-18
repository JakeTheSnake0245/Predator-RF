#pragma once
#include <string>
#include <vector>
#include <fstream>
#include <cstring>
#include <cstdint>
#include <cmath>
#include <regex>
#include <algorithm>

// IQ file reader for the Fox Hunt replay engine.
//
// Supports:
//   *.wav                — 2-channel IQ WAV as written by the recorder module
//                          (Uint8 / Int16 / Int32 PCM or Float32).
//   *.cf32 *.fc32 *.cfile — raw interleaved complex float32.
//   *.cs16 *.sc16        — raw interleaved complex int16.
//
// Unlike core's WavReader, this reader reports EOF explicitly (read() returns
// the pairs actually read, 0 at EOF) so "play once" mode can stop. rewind()
// restarts for repeat mode. Pure stdlib — unit-testable off-target.

namespace foxhunt {

    enum class IqFormat { NONE, WAV, RAW_CF32, RAW_CS16 };

    class IqFileReader {
    public:
        IqFileReader() {}
        ~IqFileReader() { close(); }

        // sampleRateHint is used for raw files when the filename carries no
        // rate; 0 leaves sampleRate()==0 and the caller must supply one.
        bool open(const std::string& path, double sampleRateHint = 0.0) {
            close();
            err.clear();
            fmt = detectFormat(path);
            if (fmt == IqFormat::NONE) { err = "Unsupported file extension"; return false; }

            file.open(path, std::ios::binary);
            if (!file.is_open()) { err = "Cannot open file"; return false; }

            file.seekg(0, std::ios::end);
            uint64_t fileSize = (uint64_t)file.tellg();
            file.seekg(0);

            if (fmt == IqFormat::WAV) {
                if (!parseWav(fileSize)) { close(); return false; }
            }
            else {
                dataOffset = 0;
                dataBytes = fileSize;
                channels = 2;
                if (fmt == IqFormat::RAW_CF32) { sampType = ST_F32; bytesPerFrame = 8; }
                else { sampType = ST_I16; bytesPerFrame = 4; }
                rate = rateFromFilename(path);
                if (rate <= 0.0) { rate = sampleRateHint; }
            }
            if (bytesPerFrame == 0) { err = "Bad frame size"; close(); return false; }
            totalPairs_ = dataBytes / bytesPerFrame;
            if (totalPairs_ == 0) { err = "File contains no samples"; close(); return false; }
            centerFreq_ = freqFromFilename(path);
            pairsRead_ = 0;
            return true;
        }

        void close() {
            if (file.is_open()) { file.close(); }
            fmt = IqFormat::NONE;
            totalPairs_ = 0;
            pairsRead_ = 0;
        }

        bool isOpen() const { return fmt != IqFormat::NONE; }
        double sampleRate() const { return rate; }
        void setSampleRate(double sr) { rate = sr; }
        uint64_t totalPairs() const { return totalPairs_; }
        uint64_t pairsRead() const { return pairsRead_; }
        // Center frequency parsed from a `<freq>Hz` token in the filename
        // (recorder naming convention); 0 when absent.
        double centerFreqHint() const { return centerFreq_; }
        const std::string& error() const { return err; }

        double durationSec() const {
            return (rate > 0.0) ? (double)totalPairs_ / rate : 0.0;
        }

        // Read up to maxPairs IQ pairs into out (interleaved float, ±1.0 full
        // scale). Returns pairs read; 0 at EOF.
        int read(float* out, int maxPairs) {
            if (!isOpen() || maxPairs <= 0) { return 0; }
            uint64_t remain = totalPairs_ - pairsRead_;
            int want = (int)std::min<uint64_t>((uint64_t)maxPairs, remain);
            if (want <= 0) { return 0; }

            int got = 0;
            switch (sampType) {
            case ST_F32: {
                scratch.resize((size_t)want * 8);
                file.read((char*)scratch.data(), (std::streamsize)want * 8);
                got = (int)(file.gcount() / 8);
                memcpy(out, scratch.data(), (size_t)got * 8);
                break;
            }
            case ST_I16: {
                scratch.resize((size_t)want * 4);
                file.read((char*)scratch.data(), (std::streamsize)want * 4);
                got = (int)(file.gcount() / 4);
                const int16_t* in = (const int16_t*)scratch.data();
                for (int i = 0; i < got * 2; i++) { out[i] = (float)in[i] / 32768.0f; }
                break;
            }
            case ST_I32: {
                scratch.resize((size_t)want * 8);
                file.read((char*)scratch.data(), (std::streamsize)want * 8);
                got = (int)(file.gcount() / 8);
                const int32_t* in = (const int32_t*)scratch.data();
                for (int i = 0; i < got * 2; i++) { out[i] = (float)in[i] / 2147483648.0f; }
                break;
            }
            case ST_U8: {
                scratch.resize((size_t)want * 2);
                file.read((char*)scratch.data(), (std::streamsize)want * 2);
                got = (int)(file.gcount() / 2);
                const uint8_t* in = (const uint8_t*)scratch.data();
                for (int i = 0; i < got * 2; i++) { out[i] = ((float)in[i] - 128.0f) / 128.0f; }
                break;
            }
            }
            if (got < 0) { got = 0; }
            pairsRead_ += (uint64_t)got;
            if (file.eof()) { file.clear(); }
            return got;
        }

        void rewind() {
            if (!isOpen()) { return; }
            file.clear();
            file.seekg((std::streamoff)dataOffset);
            pairsRead_ = 0;
        }

        static IqFormat detectFormat(const std::string& path) {
            std::string ext = lowerExt(path);
            if (ext == "wav") { return IqFormat::WAV; }
            if (ext == "cf32" || ext == "fc32" || ext == "cfile") { return IqFormat::RAW_CF32; }
            if (ext == "cs16" || ext == "sc16") { return IqFormat::RAW_CS16; }
            return IqFormat::NONE;
        }

        static double freqFromFilename(const std::string& path) {
            std::string name = baseName(path);
            std::smatch m;
            if (std::regex_search(name, m, std::regex("([0-9]+)Hz"))) {
                return std::atof(m[1].str().c_str());
            }
            return 0.0;
        }

        static double rateFromFilename(const std::string& path) {
            std::string name = baseName(path);
            std::smatch m;
            // Accept "<n>sps" and "<n>Msps" / "<n.k>Msps" tokens.
            if (std::regex_search(name, m, std::regex("([0-9]+(?:\\.[0-9]+)?)Msps", std::regex::icase))) {
                return std::atof(m[1].str().c_str()) * 1e6;
            }
            if (std::regex_search(name, m, std::regex("([0-9]+)sps", std::regex::icase))) {
                return std::atof(m[1].str().c_str());
            }
            return 0.0;
        }

    private:
        enum SampType { ST_U8, ST_I16, ST_I32, ST_F32 };

        static std::string lowerExt(const std::string& path) {
            size_t dot = path.find_last_of('.');
            if (dot == std::string::npos) { return ""; }
            std::string ext = path.substr(dot + 1);
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
            return ext;
        }

        static std::string baseName(const std::string& path) {
            size_t slash = path.find_last_of("/\\");
            return (slash == std::string::npos) ? path : path.substr(slash + 1);
        }

        // Chunk-walking RIFF/WAVE parser (handles LIST/fact chunks the naive
        // fixed-offset header parser chokes on).
        bool parseWav(uint64_t fileSize) {
            char id[4];
            uint32_t sz;
            file.read(id, 4);
            if (file.gcount() != 4 || memcmp(id, "RIFF", 4)) { err = "Not a RIFF file"; return false; }
            file.read((char*)&sz, 4);
            file.read(id, 4);
            if (file.gcount() != 4 || memcmp(id, "WAVE", 4)) { err = "Not a WAVE file"; return false; }

            bool haveFmt = false;
            uint16_t codec = 0, bits = 0;
            while (true) {
                file.read(id, 4);
                if (file.gcount() != 4) { break; }
                file.read((char*)&sz, 4);
                if (file.gcount() != 4) { break; }
                uint64_t next = (uint64_t)file.tellg() + sz + (sz & 1);
                if (!memcmp(id, "fmt ", 4)) {
                    struct { uint16_t codec, ch; uint32_t rate, bps; uint16_t bpf, bits; } f;
                    if (sz < sizeof(f)) { err = "Short fmt chunk"; return false; }
                    file.read((char*)&f, sizeof(f));
                    codec = f.codec; channels = f.ch; rate = f.rate; bits = f.bits;
                    bytesPerFrame = f.bpf;
                    haveFmt = true;
                }
                else if (!memcmp(id, "data", 4)) {
                    dataOffset = (uint64_t)file.tellg();
                    dataBytes = sz;
                    // Recorder can leave dataSize stale on crash; clamp to file.
                    if (dataOffset + dataBytes > fileSize || dataBytes == 0) {
                        dataBytes = fileSize - dataOffset;
                    }
                    break;
                }
                if (next >= fileSize) { break; }
                file.seekg((std::streamoff)next);
            }
            if (!haveFmt || dataOffset == 0) { err = "Missing fmt/data chunk"; return false; }
            if (channels != 2) { err = "IQ WAV must have 2 channels"; return false; }
            if (codec == 3 && bits == 32) { sampType = ST_F32; }
            else if (codec == 1 && bits == 16) { sampType = ST_I16; }
            else if (codec == 1 && bits == 32) { sampType = ST_I32; }
            else if (codec == 1 && bits == 8) { sampType = ST_U8; }
            else { err = "Unsupported WAV sample format"; return false; }
            if (bytesPerFrame == 0) { bytesPerFrame = (uint16_t)(channels * bits / 8); }
            file.seekg((std::streamoff)dataOffset);
            return true;
        }

        std::ifstream file;
        IqFormat fmt = IqFormat::NONE;
        SampType sampType = ST_F32;
        uint16_t channels = 2;
        uint32_t bytesPerFrame = 0;
        uint64_t dataOffset = 0;
        uint64_t dataBytes = 0;
        uint64_t totalPairs_ = 0;
        uint64_t pairsRead_ = 0;
        double rate = 0.0;
        double centerFreq_ = 0.0;
        std::string err;
        std::vector<uint8_t> scratch;
    };
}
