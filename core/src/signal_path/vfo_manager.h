#pragma once
#include "../dsp/channel/rx_vfo.h"
#include <gui/widgets/waterfall.h>
#include <utils/event.h>

class VFOManager {
public:
    VFOManager();

    class VFO {
    public:
        VFO(std::string name, int reference, double offset, double bandwidth, double sampleRate, double minBandwidth, double maxBandwidth, bool bandwidthLocked);
        ~VFO();

        void setOffset(double offset);
        double getOffset();
        void setCenterOffset(double offset);
        void setBandwidth(double bandwidth, bool updateWaterfall = true);
        void setSampleRate(double sampleRate, double bandwidth);
        void setReference(int ref);
        void setSnapInterval(double interval);
        void setBandwidthLimits(double minBandwidth, double maxBandwidth, bool bandwidthLocked);
        bool getBandwidthChanged(bool erase = true);
        double getBandwidth();
        int getReference();
        void setColor(ImU32 color);
        std::string getName();

        dsp::stream<dsp::complex_t>* output;

        friend class VFOManager;

        dsp::channel::RxVFO* dspVFO;
        ImGui::WaterfallVFO* wtfVFO;

    private:
        std::string name;
        double _bandwidth;

    };

    VFOManager::VFO* createVFO(std::string name, int reference, double offset, double bandwidth, double sampleRate, double minBandwidth, double maxBandwidth, bool bandwidthLocked);
    void deleteVFO(VFOManager::VFO* vfo);
    void deleteVFO(std::string name);

    void setOffset(std::string name, double offset);
    double getOffset(std::string name);
    void setCenterOffset(std::string name, double offset);
    void setBandwidth(std::string name, double bandwidth, bool updateWaterfall = true);
    void setSampleRate(std::string name, double sampleRate, double bandwidth);
    void setReference(std::string name, int ref);
    void setBandwidthLimits(std::string name, double minBandwidth, double maxBandwidth, bool bandwidthLocked);
    bool getBandwidthChanged(std::string name, bool erase = true);
    double getBandwidth(std::string name);
    void setColor(std::string name, ImU32 color);
    std::string getName();
    int getReference(std::string name);
    bool vfoExists(std::string name);

    // Returns the live VFO* for a given name, or nullptr if absent.
    // Added for Predator-RF hold-bound decoder modules (rtl433_decoder
    // bound mode) that need the underlying sample stream of a VFO they
    // do NOT own.  The pointer is owned by VFOManager — callers MUST
    // NOT delete it; subscribe to onVfoDelete to detach cleanly before
    // the VFO is destroyed.
    VFO* findVFO(const std::string& name);

    void updateFromWaterfall(ImGui::WaterFall* wtf);

    Event<VFOManager::VFO*> onVfoCreated;
    Event<VFOManager::VFO*> onVfoDelete;
    Event<std::string> onVfoDeleted;

private:
    std::map<std::string, VFO*> vfos;
};
