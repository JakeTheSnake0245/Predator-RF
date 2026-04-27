#pragma once
#include <imgui/imgui.h>
#include <fftw3.h>
#include <dsp/types.h>
#include <dsp/stream.h>
#include <signal_path/vfo_manager.h>
#include <string>
#include <utils/event.h>
#include <mutex>
#include <gui/tuner.h>
#include <vector>

#define WINDOW_FLAGS ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoBackground

class MainWindow {
public:
    void init();
    void draw();
    void setViewBandwidthSlider(float bandwidth);
    bool sdrIsRunning();
    void setFirstMenuRender();

    static float* acquireFFTBuffer(void* ctx);
    static void releaseFFTBuffer(void* ctx);

    // TODO: Replace with it's own class
    void setVFO(double freq);

    void setPlayState(bool _playing);
    bool isPlaying();

    bool lockWaterfallControls = false;
    bool playButtonLocked = false;

    Event<bool> onPlayStateChange;

private:
    enum PredatorMissionMode {
        PREDATOR_MODE_MANUAL,
        PREDATOR_MODE_CLASSIFY,
        PREDATOR_MODE_SCAN,
        PREDATOR_MODE_QUICKSCAN
    };

    enum PredatorTab {
        PREDATOR_TAB_SPECTRUM,
        PREDATOR_TAB_HITS,
        PREDATOR_TAB_NETWORK,
        PREDATOR_TAB_MAP,
        PREDATOR_TAB_MISSION,
        PREDATOR_TAB_SYSTEM
    };

    static void vfoAddedHandler(VFOManager::VFO* vfo, void* ctx);

    // FFT Variables
    int fftSize = 8192 * 8;
    std::mutex fft_mtx;
    fftwf_complex *fft_in, *fft_out;
    fftwf_plan fftwPlan;

    // GUI Variables
    bool firstMenuRender = true;
    bool startedWithMenuClosed = false;
    float fftMin = -70.0;
    float fftMax = 0.0;
    float bw = 8000000;
    bool playing = false;
    bool showCredits = false;
    std::string audioStreamName = "";
    std::string sourceName = "";
    int menuWidth = 300;
    bool grabbingMenu = false;
    int newWidth = 300;
    int fftHeight = 300;
    bool showMenu = true;
    int tuningMode = tuner::TUNER_MODE_NORMAL;
    dsp::stream<dsp::complex_t> dummyStream;
    bool demoWindow = false;
    int selectedWindow = 0;
    int predatorMissionMode = PREDATOR_MODE_CLASSIFY;
    int predatorTab = PREDATOR_TAB_SPECTRUM;
    int predatorQuickFilter = 0;
    int predatorHitSortMode = 0;
    int predatorEventFilter = 0;
    std::string predatorLanguage = "en-US";
    bool predatorScanRunning = false;
    bool predatorScanPaused = false;
    bool predatorPeakDetectionEnabled = true;
    int predatorScanIndex = 0;
    double predatorScanLastStepAt = 0.0;
    double predatorLastPeakSweepAt = 0.0;
    double predatorQuickScanStartedAt = 0.0;
    double predatorScanLastFrequency = 0.0;
    double predatorLastAutoEventAt = 0.0;
    double predatorLastAutoEventFrequency = 0.0;
    double predatorSelectedHitFrequency = 0.0;
    bool predatorHoldOnNewHit = true;
    bool predatorSuppressDuplicateHits = true;
    bool predatorExtendDwellOnStrongHit = true;
    bool predatorClassifyAutoMarker = true;
    float predatorPeakSnrDb = 8.0f;
    float predatorStrongHitSnrDb = 18.0f;
    double predatorPeakMinSpacingHz = 12500.0;
    double predatorLastClassifySweepAt = 0.0;
    int predatorPeakMaxPerDwell = 3;
    int predatorDuplicateHitWindowSec = 20;
    int predatorMarkerSlots = 4;
    std::string predatorScanStatus = "Idle";

    bool initComplete = false;
    bool autostart = false;

    EventHandler<VFOManager::VFO*> vfoCreatedHandler;
};
