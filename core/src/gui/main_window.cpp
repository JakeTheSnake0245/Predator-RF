#include <gui/main_window.h>
#include <gui/gui.h>
#include "imgui.h"
#include "imgui_internal.h"
#include <stdio.h>
#include <thread>
#include <complex>
#include <gui/widgets/waterfall.h>
#include <gui/widgets/frequency_select.h>
#include <gui/widgets/folder_select.h>
#include <signal_path/iq_frontend.h>
#include <gui/icons.h>
#include <gui/widgets/bandplan.h>
#include <gui/style.h>
#include <config.h>
#include <signal_path/signal_path.h>
#include <core.h>
#include <gui/menus/source.h>
#include <gui/menus/display.h>
#include <gui/menus/bandplan.h>
#include <gui/menus/sink.h>
#include <gui/menus/vfo_color.h>
#include <gui/menus/module_manager.h>
#include <gui/menus/theme.h>
#include <gui/dialogs/credits.h>
#include <filesystem>
#include <fstream>
#include <cmath>
#include <cstring>
#include <ctime>
#include <chrono>
#include <algorithm>
#include <map>
#include <cctype>
#include <signal_path/source.h>
#include <gui/dialogs/loading_screen.h>
#include <gui/colormaps.h>
#include <gui/widgets/snr_meter.h>
#include <gui/tuner.h>
#include <backend.h>
#include "../predator/decoder_ingest.h"
#include "../predator/kujhad_fleet.h"
#include "../predator/native_decoder_registry.h"
#include <ctime>

void MainWindow::init() {
    LoadingScreen::show("Initializing UI");
    gui::waterfall.init();
    gui::waterfall.setRawFFTSize(fftSize);

    credits::init();

    core::configManager.acquire();
    json menuElements = core::configManager.conf["menuElements"];
    std::string modulesDir = core::configManager.conf["modulesDirectory"];
    std::string resourcesDir = core::configManager.conf["resourcesDirectory"];
    core::configManager.release();

    // Assert that directories are absolute
    modulesDir = std::filesystem::absolute(modulesDir).string();
    resourcesDir = std::filesystem::absolute(resourcesDir).string();

    // Load menu elements
    gui::menu.order.clear();
    for (auto& elem : menuElements) {
        if (!elem.contains("name")) {
            flog::error("Menu element is missing name key");
            continue;
        }
        if (!elem["name"].is_string()) {
            flog::error("Menu element name isn't a string");
            continue;
        }
        if (!elem.contains("open")) {
            flog::error("Menu element is missing open key");
            continue;
        }
        if (!elem["open"].is_boolean()) {
            flog::error("Menu element name isn't a string");
            continue;
        }
        Menu::MenuOption_t opt;
        opt.name = elem["name"];
        opt.open = elem["open"];
        gui::menu.order.push_back(opt);
    }

    gui::menu.registerEntry("Source", sourcemenu::draw, NULL);
    gui::menu.registerEntry("Sinks", sinkmenu::draw, NULL);
    gui::menu.registerEntry("Band Plan", bandplanmenu::draw, NULL);
    gui::menu.registerEntry("Display", displaymenu::draw, NULL);
    gui::menu.registerEntry("Theme", thememenu::draw, NULL);
    gui::menu.registerEntry("VFO Color", vfo_color_menu::draw, NULL);
    gui::menu.registerEntry("Module Manager", module_manager_menu::draw, NULL);

    gui::freqSelect.init();

    // Set default values for waterfall in case no source init's it
    gui::waterfall.setBandwidth(8000000);
    gui::waterfall.setViewBandwidth(8000000);

    fft_in = (fftwf_complex*)fftwf_malloc(sizeof(fftwf_complex) * fftSize);
    fft_out = (fftwf_complex*)fftwf_malloc(sizeof(fftwf_complex) * fftSize);
    fftwPlan = fftwf_plan_dft_1d(fftSize, fft_in, fft_out, FFTW_FORWARD, FFTW_ESTIMATE);

    sigpath::iqFrontEnd.init(&dummyStream, 8000000, true, 1, false, 1024, 20.0, IQFrontEnd::FFTWindow::NUTTALL, acquireFFTBuffer, releaseFFTBuffer, this);
    sigpath::iqFrontEnd.start();

    vfoCreatedHandler.handler = vfoAddedHandler;
    vfoCreatedHandler.ctx = this;
    sigpath::vfoManager.onVfoCreated.bindHandler(&vfoCreatedHandler);

    flog::info("Loading modules");

    // Load modules from /module directory
    if (std::filesystem::is_directory(modulesDir)) {
        for (const auto& file : std::filesystem::directory_iterator(modulesDir)) {
            std::string path = file.path().generic_string();
            if (file.path().extension().generic_string() != SDRPP_MOD_EXTENTSION) {
                continue;
            }
            if (!file.is_regular_file()) { continue; }
            flog::info("Loading {0}", path);
            LoadingScreen::show("Loading " + file.path().filename().string());
            core::moduleManager.loadModule(path);
        }
    }
    else {
        flog::warn("Module directory {0} does not exist, not loading modules from directory", modulesDir);
    }

    // Read module config
    core::configManager.acquire();
    std::vector<std::string> modules = core::configManager.conf["modules"];
    auto modList = core::configManager.conf["moduleInstances"].items();
    core::configManager.release();

    // Load additional modules specified through config
    for (auto const& path : modules) {
#ifndef __ANDROID__
        std::string apath = std::filesystem::absolute(path).string();
        flog::info("Loading {0}", apath);
        LoadingScreen::show("Loading " + std::filesystem::path(path).filename().string());
        core::moduleManager.loadModule(apath);
#else
        core::moduleManager.loadModule(path);
#endif
    }

    // Create module instances
    for (auto const& [name, _module] : modList) {
        std::string mod = _module["module"];
        bool enabled = _module["enabled"];
        flog::info("Initializing {0} ({1})", name, mod);
        LoadingScreen::show("Initializing " + name + " (" + mod + ")");
        core::moduleManager.createInstance(name, mod);
        if (!enabled) { core::moduleManager.disableInstance(name); }
    }

    // Load color maps
    LoadingScreen::show("Loading color maps");
    flog::info("Loading color maps");
    if (std::filesystem::is_directory(resourcesDir + "/colormaps")) {
        for (const auto& file : std::filesystem::directory_iterator(resourcesDir + "/colormaps")) {
            std::string path = file.path().generic_string();
            LoadingScreen::show("Loading " + file.path().filename().string());
            flog::info("Loading {0}", path);
            if (file.path().extension().generic_string() != ".json") {
                continue;
            }
            if (!file.is_regular_file()) { continue; }
            colormaps::loadMap(path);
        }
    }
    else {
        flog::warn("Color map directory {0} does not exist, not loading modules from directory", modulesDir);
    }

    gui::waterfall.updatePalletteFromArray(colormaps::maps["Turbo"].map, colormaps::maps["Turbo"].entryCount);

    sourcemenu::init();
    sinkmenu::init();
    bandplanmenu::init();
    displaymenu::init();
    vfo_color_menu::init();
    module_manager_menu::init();

    // TODO for 0.2.5
    // Fix gain not updated on startup, soapysdr

    // Update UI settings
    LoadingScreen::show("Loading configuration");
    core::configManager.acquire();
    fftMin = core::configManager.conf["min"];
    fftMax = core::configManager.conf["max"];
    gui::waterfall.setFFTMin(fftMin);
    gui::waterfall.setWaterfallMin(fftMin);
    gui::waterfall.setFFTMax(fftMax);
    gui::waterfall.setWaterfallMax(fftMax);

    double frequency = core::configManager.conf["frequency"];

    showMenu = core::configManager.conf["showMenu"];
    startedWithMenuClosed = !showMenu;

    gui::freqSelect.setFrequency(frequency);
    gui::freqSelect.frequencyChanged = false;
    sigpath::sourceManager.tune(frequency);
    gui::waterfall.setCenterFrequency(frequency);
    bw = 1.0;
    gui::waterfall.vfoFreqChanged = false;
    gui::waterfall.centerFreqMoved = false;
    gui::waterfall.selectFirstVFO();

    menuWidth = core::configManager.conf["menuWidth"];
    newWidth = menuWidth;

    fftHeight = core::configManager.conf["fftHeight"];
    gui::waterfall.setFFTHeight(fftHeight);

    predatorMissionMode = std::clamp<int>((int)core::configManager.conf["predatorMissionMode"], PREDATOR_MODE_MANUAL, PREDATOR_MODE_QUICKSCAN);
    predatorTab = std::clamp<int>((int)core::configManager.conf["predatorTab"], PREDATOR_TAB_SPECTRUM, PREDATOR_TAB_SYSTEM);
    if (core::configManager.conf.contains("predatorWfControlsOpen")) {
        predatorWfControlsOpen = (bool)core::configManager.conf["predatorWfControlsOpen"];
    }
    predatorQuickFilter = std::clamp<int>((int)core::configManager.conf["predatorQuickFilter"], 0, 3);
    predatorHitSortMode = std::clamp<int>((int)core::configManager.conf["predatorHitSortMode"], 0, 5);
    predatorEventFilter = std::clamp<int>((int)core::configManager.conf["predatorEventFilter"], 0, 5);
    predatorLanguage = (std::string)core::configManager.conf["predatorLanguage"];
    predatorPeakDetectionEnabled = core::configManager.conf["predatorPeakDetectionEnabled"];
    predatorPeakSnrDb = core::configManager.conf["predatorPeakSnrDb"];
    predatorPeakMinSpacingHz = core::configManager.conf["predatorPeakMinSpacingHz"];
    predatorPeakMaxPerDwell = core::configManager.conf["predatorPeakMaxPerDwell"];
    predatorMarkerSlots = core::configManager.conf["predatorMarkerSlots"];
    predatorHoldOnNewHit = core::configManager.conf["predatorHoldOnNewHit"];
    predatorSuppressDuplicateHits = core::configManager.conf["predatorSuppressDuplicateHits"];
    predatorDuplicateHitWindowSec = core::configManager.conf["predatorDuplicateHitWindowSec"];
    predatorExtendDwellOnStrongHit = core::configManager.conf["predatorExtendDwellOnStrongHit"];
    predatorStrongHitSnrDb = core::configManager.conf["predatorStrongHitSnrDb"];
    predatorClassifyAutoMarker = core::configManager.conf["predatorClassifyAutoMarker"];

    // Kujhad fleet console state. Defaults are populated by core::init() so
    // these reads are always present.
    {
        auto& cfg = core::configManager.conf;
        predatorRole = std::clamp<int>((int)cfg.value("predatorRole", (int)PREDATOR_ROLE_DEVICE),
                                       (int)PREDATOR_ROLE_DEVICE, (int)PREDATOR_ROLE_CONTROLLER);
        kujhadDeviceServerEnabled = cfg.value("kujhadDeviceServerEnabled", false);
        kujhadDeviceListenPort = std::clamp<int>(cfg.value("kujhadDeviceListenPort", 41947), 1, 65535);
        kujhadApiKey = cfg.value("kujhadApiKey", std::string(""));
        kujhadDeviceName = cfg.value("kujhadDeviceName", std::string(""));
        kujhadAdvertiseAddress = cfg.value("kujhadAdvertiseAddress", std::string(""));
        kujhadSpectrumIntervalMs.store(std::clamp<int>(cfg.value("kujhadSpectrumIntervalMs", 200), 50, 5000), std::memory_order_relaxed);
        kujhadSpectrumBins.store(std::clamp<int>(cfg.value("kujhadSpectrumBins", 256), 32, 1024), std::memory_order_relaxed);
        kujhadMirrorPeerSpectrum = cfg.value("kujhadMirrorPeerSpectrum", false);
        kujhadTlsEnabled = cfg.value("kujhadTlsEnabled", false);
        kujhadTlsCertPath = cfg.value("kujhadTlsCertPath", std::string(""));
        kujhadTlsKeyPath = cfg.value("kujhadTlsKeyPath", std::string(""));
        if (kujhadApiKey.empty()) {
            kujhadApiKey = predator::kujhadGenerateApiKey();
            cfg["kujhadApiKey"] = kujhadApiKey;
        }
        if (kujhadDeviceName.empty()) {
            kujhadDeviceName = std::string("predator-") + std::to_string((int)(std::time(nullptr) % 100000));
            cfg["kujhadDeviceName"] = kujhadDeviceName;
        }
        // Default cert/key live next to the user config so a fresh
        // install can flip TLS on without first picking paths. Files
        // aren't created until the operator clicks "Regenerate".
        if (kujhadTlsCertPath.empty() || kujhadTlsKeyPath.empty()) {
            std::string root = (std::string)core::args["root"];
            if (kujhadTlsCertPath.empty()) {
                kujhadTlsCertPath = (std::filesystem::path(root) / "kujhad_tls_cert.pem").string();
                cfg["kujhadTlsCertPath"] = kujhadTlsCertPath;
            }
            if (kujhadTlsKeyPath.empty()) {
                kujhadTlsKeyPath = (std::filesystem::path(root) / "kujhad_tls_key.pem").string();
                cfg["kujhadTlsKeyPath"] = kujhadTlsKeyPath;
            }
        }
        // Pre-populate the displayed fingerprint if a cert already
        // exists on disk, so the operator sees it before starting the
        // server. Empty when there's no usable cert yet.
        kujhadTlsFingerprint = predator::kujhadCertFingerprintFromPemFile(kujhadTlsCertPath);
    }

    tuningMode = core::configManager.conf["centerTuning"] ? tuner::TUNER_MODE_CENTER : tuner::TUNER_MODE_NORMAL;
    gui::waterfall.VFOMoveSingleClick = (tuningMode == tuner::TUNER_MODE_CENTER);

    core::configManager.release();

    // Correct the offset of all VFOs so that they fit on the screen
    float finalBwHalf = gui::waterfall.getBandwidth() / 2.0;
    for (auto& [_name, _vfo] : gui::waterfall.vfos) {
        if (_vfo->lowerOffset < -finalBwHalf) {
            sigpath::vfoManager.setCenterOffset(_name, (_vfo->bandwidth / 2) - finalBwHalf);
            continue;
        }
        if (_vfo->upperOffset > finalBwHalf) {
            sigpath::vfoManager.setCenterOffset(_name, finalBwHalf - (_vfo->bandwidth / 2));
            continue;
        }
    }

    autostart = core::args["autostart"].b();
    initComplete = true;

    core::moduleManager.doPostInitAll();
}

float* MainWindow::acquireFFTBuffer(void* ctx) {
    // getFFTBuffer() locks waterfall.buf_mtx and advances the row cursor.
    // The matching unlock happens inside pushFFT(). We cache the pointer
    // and current row size so releaseFFTBuffer can snapshot the row for
    // the Kujhad spectrum provider WITHOUT re-acquiring the buffer
    // (which would unbalance the lock and double-advance the cursor).
    float* buf = gui::waterfall.getFFTBuffer();
    MainWindow* self = (MainWindow*)ctx;
    if (self) {
        self->kujhadLastAcquiredBuf  = buf;
        self->kujhadLastAcquiredSize = gui::waterfall.getRawFFTSize();
    }
    return buf;
}

void MainWindow::releaseFFTBuffer(void* ctx) {
    // This runs on the FFT worker thread between acquireFFTBuffer and
    // pushFFT(), while waterfall.buf_mtx is still held — the cached
    // pointer from acquire is therefore valid and exclusive.
    MainWindow* self = (MainWindow*)ctx;
    if (self && self->kujhadLastAcquiredBuf && self->kujhadLastAcquiredSize > 0) {
        int   n   = self->kujhadLastAcquiredSize;
        float* buf = self->kujhadLastAcquiredBuf;

        // 1) Snapshot the freshly written local FFT row for the device
        //    server, but only when the device server is actually enabled
        //    (the controller may not be hosting). Done before any peer
        //    overwrite below, so peers see TRUE local SDR data.
        if (self->kujhadDeviceServerEnabled) {
            std::lock_guard<std::mutex> lk(self->kujhadSpectrumMtx);
            if ((int)self->kujhadSpectrumRaw.size() != n) {
                self->kujhadSpectrumRaw.assign(n, -150.0f);
            }
            std::memcpy(self->kujhadSpectrumRaw.data(), buf, n * sizeof(float));
            self->kujhadSpectrumRawSize = n;
            self->kujhadSpectrumHaveRaw = true;
            self->kujhadSpectrumLocalSerial++;
        }

        // 2) EXCLUSIVE peer-mirror substitution: if the operator selected
        //    the peer view, OVERWRITE the local row with the most recent
        //    cached peer frame so pushFFT() scrolls peer data — not local
        //    SDR data. This makes the UI a true source switch, not a
        //    flickering overlay. When no peer frame is cached yet, the
        //    row is filled with the floor so stale local content cannot
        //    leak through.
        if (self->kujhadMirrorActive.load(std::memory_order_acquire)) {
            std::lock_guard<std::mutex> lk(self->kujhadSpectrumMtx);
            int peerN = (int)self->kujhadPeerCachedBins.size();
            if (peerN > 0) {
                // Linear resample peerN -> n
                if (peerN == n) {
                    std::memcpy(buf, self->kujhadPeerCachedBins.data(),
                                n * sizeof(float));
                } else {
                    for (int i = 0; i < n; i++) {
                        float t   = (n > 1) ? ((float)i * (peerN - 1) / (n - 1))
                                            : 0.0f;
                        int   i0  = (int)t;
                        int   i1  = std::min(i0 + 1, peerN - 1);
                        float f   = t - i0;
                        buf[i] = self->kujhadPeerCachedBins[i0] * (1.0f - f)
                               + self->kujhadPeerCachedBins[i1] * f;
                    }
                }
            } else {
                std::fill(buf, buf + n, -150.0f);
            }
        }
    }
    if (self) {
        self->kujhadLastAcquiredBuf  = nullptr;
        self->kujhadLastAcquiredSize = 0;
    }
    gui::waterfall.pushFFT();
}

void MainWindow::vfoAddedHandler(VFOManager::VFO* vfo, void* ctx) {
    MainWindow* _this = (MainWindow*)ctx;
    std::string name = vfo->getName();
    core::configManager.acquire();
    if (!core::configManager.conf["vfoOffsets"].contains(name)) {
        core::configManager.release();
        return;
    }
    double offset = core::configManager.conf["vfoOffsets"][name];
    core::configManager.release();

    double viewBW = gui::waterfall.getViewBandwidth();
    double viewOffset = gui::waterfall.getViewOffset();

    double viewLower = viewOffset - (viewBW / 2.0);
    double viewUpper = viewOffset + (viewBW / 2.0);

    double newOffset = std::clamp<double>(offset, viewLower, viewUpper);

    sigpath::vfoManager.setCenterOffset(name, _this->initComplete ? newOffset : offset);
}

void MainWindow::draw() {
    ImGui::Begin("Main", NULL, WINDOW_FLAGS);
    ImVec4 textCol = ImGui::GetStyleColorVec4(ImGuiCol_Text);
#ifdef __ANDROID__
    ImGuiStyle& imguiStyle = ImGui::GetStyle();
    imguiStyle.TouchExtraPadding = ImVec2(7.0f * style::uiScale, 7.0f * style::uiScale);
#endif

    ImGui::WaterfallVFO* vfo = NULL;
    if (gui::waterfall.selectedVFO != "") {
        vfo = gui::waterfall.vfos[gui::waterfall.selectedVFO];
    }
    if (gui::waterfall.selectedVFO.rfind("Predator M", 0) == 0) {
        std::string receiverName = "";
        if (gui::waterfall.vfos.find("Radio") != gui::waterfall.vfos.end()) {
            receiverName = "Radio";
        }
        else {
            for (auto const& [name, _vfo] : gui::waterfall.vfos) {
                if (name.rfind("Predator M", 0) != 0) {
                    receiverName = name;
                    break;
                }
            }
        }
        if (!receiverName.empty()) {
            gui::waterfall.selectedVFO = receiverName;
            gui::waterfall.selectedVFOChanged = true;
            vfo = gui::waterfall.vfos[receiverName];
        }
    }

    // Handle VFO movement
    if (vfo != NULL) {
        if (vfo->centerOffsetChanged) {
            if (tuningMode == tuner::TUNER_MODE_CENTER) {
                tuner::tune(tuner::TUNER_MODE_CENTER, gui::waterfall.selectedVFO, gui::waterfall.getCenterFrequency() + vfo->generalOffset);
            }
            gui::freqSelect.setFrequency(gui::waterfall.getCenterFrequency() + vfo->generalOffset);
            gui::freqSelect.frequencyChanged = false;
            core::configManager.acquire();
            core::configManager.conf["vfoOffsets"][gui::waterfall.selectedVFO] = vfo->generalOffset;
            core::configManager.release(true);
        }
    }

    sigpath::vfoManager.updateFromWaterfall(&gui::waterfall);

    // Handle selection of another VFO
    if (gui::waterfall.selectedVFOChanged) {
        gui::freqSelect.setFrequency((vfo != NULL) ? (vfo->generalOffset + gui::waterfall.getCenterFrequency()) : gui::waterfall.getCenterFrequency());
        gui::waterfall.selectedVFOChanged = false;
        gui::freqSelect.frequencyChanged = false;
    }

    // Handle change in selected frequency
    if (gui::freqSelect.frequencyChanged) {
        gui::freqSelect.frequencyChanged = false;
        tuner::tune(tuningMode, gui::waterfall.selectedVFO, gui::freqSelect.frequency);
        if (vfo != NULL) {
            vfo->centerOffsetChanged = false;
            vfo->lowerOffsetChanged = false;
            vfo->upperOffsetChanged = false;
        }
        core::configManager.acquire();
        core::configManager.conf["frequency"] = gui::waterfall.getCenterFrequency();
        if (vfo != NULL) {
            core::configManager.conf["vfoOffsets"][gui::waterfall.selectedVFO] = vfo->generalOffset;
        }
        core::configManager.release(true);
    }

    // Handle dragging the frequency scale
    if (gui::waterfall.centerFreqMoved) {
        gui::waterfall.centerFreqMoved = false;
        sigpath::sourceManager.tune(gui::waterfall.getCenterFrequency());
        if (vfo != NULL) {
            gui::freqSelect.setFrequency(gui::waterfall.getCenterFrequency() + vfo->generalOffset);
        }
        else {
            gui::freqSelect.setFrequency(gui::waterfall.getCenterFrequency());
        }
        core::configManager.acquire();
        core::configManager.conf["frequency"] = gui::waterfall.getCenterFrequency();
        core::configManager.release(true);
    }

    int _fftHeight = gui::waterfall.getFFTHeight();
    if (fftHeight != _fftHeight) {
        fftHeight = _fftHeight;
        core::configManager.acquire();
        core::configManager.conf["fftHeight"] = fftHeight;
        core::configManager.release(true);
    }

    bool chineseUi = (predatorLanguage == "zh-Hant");
    auto T = [&](const char* text) -> const char* {
        if (!chineseUi) { return text; }
        if (strcmp(text, "Manual") == 0) { return "\u624b\u52d5"; }
        if (strcmp(text, "Classify") == 0) { return "\u5206\u985e"; }
        if (strcmp(text, "Scan") == 0) { return "\u6383\u63cf"; }
        if (strcmp(text, "QuickScan") == 0) { return "\u5feb\u901f\u6383\u63cf"; }
        if (strcmp(text, "Spectrum") == 0) { return "\u983b\u8b5c"; }
        if (strcmp(text, "Hits & Events") == 0) { return "\u547d\u4e2d\u8207\u4e8b\u4ef6"; }
        if (strcmp(text, "Network") == 0) { return "\u7db2\u8def"; }
        if (strcmp(text, "Map") == 0) { return "\u5730\u5716"; }
        if (strcmp(text, "Mission Config") == 0) { return "\u4efb\u52d9\u8a2d\u5b9a"; }
        if (strcmp(text, "System") == 0) { return "\u7cfb\u7d71"; }
        if (strcmp(text, "All") == 0) { return "\u5168\u90e8"; }
        if (strcmp(text, "Target") == 0 || strcmp(text, "Targets") == 0) { return "\u76ee\u6a19"; }
        if (strcmp(text, "Exclude") == 0 || strcmp(text, "Excludes") == 0) { return "\u6392\u9664"; }
        if (strcmp(text, "Unknown") == 0) { return "\u672a\u77e5"; }
        if (strcmp(text, "LIVE") == 0) { return "\u5373\u6642"; }
        if (strcmp(text, "READY") == 0 || strcmp(text, "Ready") == 0) { return "\u5c31\u7dd2"; }
        if (strcmp(text, "NOT READY") == 0) { return "\u672a\u5c31\u7dd2"; }
        if (strcmp(text, "Select SDR") == 0) { return "\u9078\u64c7 SDR"; }
        if (strcmp(text, "GPS READY") == 0) { return "GPS \u5c31\u7dd2"; }
        if (strcmp(text, "GPS WAIT") == 0) { return "\u7b49\u5f85 GPS"; }
        if (strcmp(text, "Running") == 0 || strcmp(text, "Streaming") == 0) { return "\u57f7\u884c\u4e2d"; }
        if (strcmp(text, "Paused") == 0) { return "\u5df2\u66ab\u505c"; }
        if (strcmp(text, "Idle") == 0) { return "\u9592\u7f6e"; }
        if (strcmp(text, "Start Listening") == 0) { return "\u958b\u59cb\u76e3\u807d"; }
        if (strcmp(text, "Stop Listening") == 0) { return "\u505c\u6b62\u76e3\u807d"; }
        if (strcmp(text, "Start Scan") == 0) { return "\u958b\u59cb\u6383\u63cf"; }
        if (strcmp(text, "Start QuickScan") == 0) { return "\u958b\u59cb\u5feb\u901f\u6383\u63cf"; }
        if (strcmp(text, "Pause") == 0) { return "\u66ab\u505c"; }
        if (strcmp(text, "Resume") == 0) { return "\u7e7c\u7e8c"; }
        if (strcmp(text, "Stop") == 0) { return "\u505c\u6b62"; }
        if (strcmp(text, "Previous") == 0) { return "\u4e0a\u4e00\u500b"; }
        if (strcmp(text, "Next") == 0) { return "\u4e0b\u4e00\u500b"; }
        if (strcmp(text, "Target Current") == 0) { return "\u76ee\u524d\u8a2d\u70ba\u76ee\u6a19"; }
        if (strcmp(text, "Exclude Current") == 0) { return "\u76ee\u524d\u8a2d\u70ba\u6392\u9664"; }
        if (strcmp(text, "Log Event") == 0) { return "\u8a18\u9304\u4e8b\u4ef6"; }
        if (strcmp(text, "Peak Detection") == 0) { return "\u5cf0\u503c\u5075\u6e2c"; }
        if (strcmp(text, "Detect Peaks") == 0) { return "\u5075\u6e2c\u5cf0\u503c"; }
        if (strcmp(text, "Peak SNR") == 0) { return "\u5cf0\u503c SNR"; }
        if (strcmp(text, "Peak Spacing Hz") == 0) { return "\u5cf0\u503c\u9593\u8ddd Hz"; }
        if (strcmp(text, "Max Peaks / Dwell") == 0) { return "\u6bcf\u6b21\u505c\u7559\u6700\u591a\u5cf0\u503c"; }
        if (strcmp(text, "Detected Hits") == 0) { return "\u5075\u6e2c\u547d\u4e2d"; }
        if (strcmp(text, "Selected Hit") == 0) { return "\u9078\u53d6\u547d\u4e2d"; }
        if (strcmp(text, "Marker Pool") == 0) { return "\u6a19\u8a18\u6c60"; }
        if (strcmp(text, "Select") == 0) { return "\u9078\u53d6"; }
        if (strcmp(text, "Tune") == 0) { return "\u8abf\u8ae7"; }
        if (strcmp(text, "Mark Viewed") == 0) { return "\u6a19\u8a18\u5df2\u8b80"; }
        if (strcmp(text, "Rename / Notes") == 0) { return "\u91cd\u547d\u540d / \u5099\u8a3b"; }
        if (strcmp(text, "Hit Sort") == 0) { return "\u547d\u4e2d\u6392\u5e8f"; }
        if (strcmp(text, "Event Filter") == 0) { return "\u4e8b\u4ef6\u7be9\u9078"; }
        if (strcmp(text, "Session Export") == 0) { return "\u5de5\u4f5c\u968e\u6bb5\u532f\u51fa"; }
        if (strcmp(text, "Export Session JSON") == 0) { return "\u532f\u51fa\u5de5\u4f5c\u968e\u6bb5 JSON"; }
        if (strcmp(text, "Export CSV") == 0) { return "\u532f\u51fa CSV"; }
        if (strcmp(text, "Export Hits CSV") == 0) { return "\u532f\u51fa\u547d\u4e2d CSV"; }
        if (strcmp(text, "Export Events CSV") == 0) { return "\u532f\u51fa\u4e8b\u4ef6 CSV"; }
        if (strcmp(text, "Export Status") == 0) { return "\u532f\u51fa\u72c0\u614b"; }
        if (strcmp(text, "Assign Marker") == 0) { return "\u6307\u6d3e\u6a19\u8a18"; }
        if (strcmp(text, "Release Marker") == 0) { return "\u91cb\u653e\u6a19\u8a18"; }
        if (strcmp(text, "Decoder") == 0) { return "\u89e3\u78bc\u5668"; }
        if (strcmp(text, "Promote Target") == 0) { return "\u5347\u7d1a\u70ba\u76ee\u6a19"; }
        if (strcmp(text, "Promote Exclude") == 0) { return "\u5347\u7d1a\u70ba\u6392\u9664"; }
        if (strcmp(text, "Decoder Outputs") == 0) { return "\u89e3\u78bc\u8f38\u51fa"; }
        if (strcmp(text, "Voice Extractions") == 0) { return "\u8a9e\u97f3\u64f7\u53d6"; }
        if (strcmp(text, "Data Extractions") == 0) { return "\u8cc7\u6599\u64f7\u53d6"; }
        if (strcmp(text, "Network Topology") == 0) { return "\u7db2\u8def\u62d3\u64b2"; }
        if (strcmp(text, "Network Node Actions") == 0) { return "\u7db2\u8def\u7bc0\u9ede\u64cd\u4f5c"; }
        if (strcmp(text, "Target Node") == 0) { return "\u7bc0\u9ede\u8a2d\u70ba\u76ee\u6a19"; }
        if (strcmp(text, "Exclude Node") == 0) { return "\u7bc0\u9ede\u8a2d\u70ba\u6392\u9664"; }
        if (strcmp(text, "Mark Hits") == 0) { return "\u6a19\u8a18\u547d\u4e2d"; }
        if (strcmp(text, "Route VFO") == 0) { return "\u8def\u7531 VFO"; }
        if (strcmp(text, "Release Route") == 0) { return "\u91cb\u653e\u8def\u7531"; }
        if (strcmp(text, "Alias") == 0) { return "\u5225\u540d"; }
        if (strcmp(text, "Decoder Events") == 0) { return "\u89e3\u78bc\u4e8b\u4ef6"; }
        if (strcmp(text, "Marker Slots") == 0) { return "\u6a19\u8a18\u6578\u91cf"; }
        if (strcmp(text, "Hold on New Hit") == 0) { return "\u65b0\u547d\u4e2d\u6642\u505c\u7559"; }
        if (strcmp(text, "Suppress Duplicate Hits") == 0) { return "\u6291\u5236\u91cd\u8907\u547d\u4e2d"; }
        if (strcmp(text, "Duplicate Window (s)") == 0) { return "\u91cd\u8907\u8996\u7a97\uff08\u79d2\uff09"; }
        if (strcmp(text, "Extend Dwell on Strong Hit") == 0) { return "\u5f37\u8a0a\u865f\u5ef6\u9577\u505c\u7559"; }
        if (strcmp(text, "Strong Hit SNR") == 0) { return "\u5f37\u547d\u4e2d SNR"; }
        if (strcmp(text, "Classify Auto-Marker") == 0) { return "\u5206\u985e\u81ea\u52d5\u6a19\u8a18"; }
        if (strcmp(text, "Scan Progress") == 0) { return "\u6383\u63cf\u9032\u5ea6"; }
        if (strcmp(text, "Display Controls") == 0) { return "\u986f\u793a\u63a7\u5236"; }
        if (strcmp(text, "Band Plan") == 0) { return "\u983b\u6bb5\u8868"; }
        if (strcmp(text, "Current Mission Mode") == 0) { return "\u76ee\u524d\u4efb\u52d9\u6a21\u5f0f"; }
        if (strcmp(text, "Mission Run") == 0) { return "\u4efb\u52d9\u57f7\u884c"; }
        if (strcmp(text, "Quick Actions") == 0) { return "\u5feb\u901f\u64cd\u4f5c"; }
        if (strcmp(text, "Open SDR / Settings") == 0) { return "\u958b\u555f SDR / \u8a2d\u5b9a"; }
        if (strcmp(text, "Open Mission Config") == 0) { return "\u958b\u555f\u4efb\u52d9\u8a2d\u5b9a"; }
        if (strcmp(text, "Quick Filter") == 0) { return "\u5feb\u901f\u7be9\u9078"; }
        if (strcmp(text, "Event Log") == 0) { return "\u4e8b\u4ef6\u8a18\u9304"; }
        if (strcmp(text, "No entries.") == 0) { return "\u6c92\u6709\u9805\u76ee\u3002"; }
        if (strcmp(text, "Clear Events") == 0) { return "\u6e05\u9664\u4e8b\u4ef6"; }
        if (strcmp(text, "Search Bands") == 0) { return "\u641c\u5c0b\u983b\u6bb5"; }
        if (strcmp(text, "Network Workflow") == 0) { return "\u7db2\u8def\u6d41\u7a0b"; }
        if (strcmp(text, "Current Workflow Assets") == 0) { return "\u76ee\u524d\u6d41\u7a0b\u8cc7\u7522"; }
        if (strcmp(text, "Phone Map") == 0) { return "\u624b\u6a5f\u5730\u5716"; }
        if (strcmp(text, "Open Tactical Map") == 0) { return "\u958b\u555f\u6230\u8853\u5730\u5716"; }
        if (strcmp(text, "DF Status") == 0) { return "\u6e2c\u5411\u72c0\u614b"; }
        if (strcmp(text, "Mission Modes") == 0) { return "\u4efb\u52d9\u6a21\u5f0f"; }
        if (strcmp(text, "Scan / QuickScan Settings") == 0) { return "\u6383\u63cf / \u5feb\u901f\u6383\u63cf\u8a2d\u5b9a"; }
        if (strcmp(text, "Operator Note") == 0) { return "\u64cd\u4f5c\u5099\u8a3b"; }
        if (strcmp(text, "DSD-FME Digital Voice") == 0) { return "DSD-FME \u6578\u4f4d\u8a9e\u97f3"; }
        if (strcmp(text, "Enable DSD-FME Bridge") == 0) { return "\u555f\u7528 DSD-FME \u6a4b\u63a5"; }
        if (strcmp(text, "Bridge Host") == 0) { return "\u6a4b\u63a5\u4e3b\u6a5f"; }
        if (strcmp(text, "Bridge Port") == 0) { return "\u6a4b\u63a5\u57e0"; }
        if (strcmp(text, "Bridge Mode") == 0) { return "\u6a4b\u63a5\u6a21\u5f0f"; }
        if (strcmp(text, "Language") == 0) { return "\u8a9e\u8a00"; }
        if (strcmp(text, "Source & Device") == 0) { return "\u4f86\u6e90\u8207\u88dd\u7f6e"; }
        if (strcmp(text, "Audio / Sinks") == 0) { return "\u97f3\u8a0a / \u8f38\u51fa"; }
        if (strcmp(text, "Display & Band Plan") == 0) { return "\u986f\u793a\u8207\u983b\u6bb5\u8868"; }
        if (strcmp(text, "Appearance") == 0) { return "\u5916\u89c0"; }
        if (strcmp(text, "Module Manager") == 0) { return "\u6a21\u7d44\u7ba1\u7406"; }
        if (strcmp(text, "Status") == 0) { return "\u72c0\u614b"; }
        if (strcmp(text, "Legacy Advanced Menus") == 0) { return "\u820a\u7248\u9032\u968e\u9078\u55ae"; }
        if (strcmp(text, "Debug") == 0) { return "\u9664\u932f"; }
        if (strcmp(text, "None") == 0) { return "\u7121"; }
        if (strcmp(text, "Waiting") == 0) { return "\u7b49\u5f85\u4e2d"; }
        return text;
    };
    const char* missionModes[] = {
        T("Manual"),
        T("Classify"),
        T("Scan"),
        T("QuickScan")
    };

    const char* missionModeDescriptions[] = {
        T("Direct operator tuning and marker ownership."),
        T("Keep manual control while idle resources watch the band."),
        T("Automated search and target workflow across configured bands."),
        T("Rapid single-marker sweep for quick checks.")
    };

    const char* tabLabels[] = {
        "SPEC",
        "HITS",
        "NET",
        "MAP",
        "MIS",
        "KUJ",
        "SYS"
    };

    const char* tabTitles[] = {
        T("Spectrum"),
        T("Hits & Events"),
        T("Network"),
        T("Map"),
        T("Mission Config"),
        T("Kujhad Fleet"),
        T("System")
    };

    const char* tabDescriptions[] = {
        T("Tune, shape, and monitor the live spectrum picture."),
        T("Review operational queues, filters, and retained frequencies of interest."),
        T("Hold the Predator RF navigation slot for decoder-backed structure and labels."),
        T("Launch the touch-first phone map tied to handset GPS."),
        T("Drive search bands, targets, excludes, dwell, and quick-scan workflow."),
        T("Operate this unit as a Device or pull peer state from a Controller."),
        T("Health, theme, legacy modules, and operator-level status.")
    };

    const char* quickFilterLabels[] = {
        T("All"),
        T("Target"),
        T("Exclude"),
        T("Unknown")
    };

    auto savePredatorState = [&]() {
        core::configManager.acquire();
        core::configManager.conf["showMenu"] = showMenu;
        core::configManager.conf["predatorMissionMode"] = predatorMissionMode;
        core::configManager.conf["predatorTab"] = predatorTab;
        core::configManager.conf["predatorWfControlsOpen"] = predatorWfControlsOpen;
        core::configManager.conf["predatorQuickFilter"] = predatorQuickFilter;
        core::configManager.conf["predatorHitSortMode"] = predatorHitSortMode;
        core::configManager.conf["predatorEventFilter"] = predatorEventFilter;
        core::configManager.conf["predatorLanguage"] = predatorLanguage;
        core::configManager.conf["predatorRole"] = predatorRole;
        core::configManager.conf["kujhadDeviceServerEnabled"] = kujhadDeviceServerEnabled;
        core::configManager.conf["kujhadDeviceListenPort"] = kujhadDeviceListenPort;
        core::configManager.conf["kujhadApiKey"] = kujhadApiKey;
        core::configManager.conf["kujhadDeviceName"] = kujhadDeviceName;
        core::configManager.conf["kujhadAdvertiseAddress"] = kujhadAdvertiseAddress;
        core::configManager.conf["kujhadSpectrumIntervalMs"] = kujhadSpectrumIntervalMs.load(std::memory_order_relaxed);
        core::configManager.conf["kujhadSpectrumBins"] = kujhadSpectrumBins.load(std::memory_order_relaxed);
        core::configManager.conf["kujhadMirrorPeerSpectrum"] = kujhadMirrorPeerSpectrum;
        core::configManager.conf["kujhadTlsEnabled"] = kujhadTlsEnabled;
        core::configManager.conf["kujhadTlsCertPath"] = kujhadTlsCertPath;
        core::configManager.conf["kujhadTlsKeyPath"] = kujhadTlsKeyPath;
        core::configManager.release(true);
    };

    auto saveKujhadPeers = [&](const json& peers) {
        core::configManager.acquire();
        core::configManager.conf["kujhadPeers"] = peers;
        core::configManager.release(true);
    };

    auto saveLegacyMenuState = [&]() {
        core::configManager.acquire();
        json arr = json::array();
        for (int i = 0; i < gui::menu.order.size(); i++) {
            arr[i]["name"] = gui::menu.order[i].name;
            arr[i]["open"] = gui::menu.order[i].open;
        }
        core::configManager.conf["menuElements"] = arr;
        for (auto [_name, inst] : core::moduleManager.instances) {
            if (!core::configManager.conf["moduleInstances"].contains(_name)) { continue; }
            core::configManager.conf["moduleInstances"][_name]["enabled"] = inst.instance->isEnabled();
        }
        core::configManager.release(true);
    };

    auto drawBadge = [&](const char* label, const ImVec4& col) {
        ImGui::PushStyleColor(ImGuiCol_Button, col);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, col);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, col);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.05f, 0.06f, 0.05f, 1.0f));
        bool pressed = ImGui::Button(label);
        ImGui::PopStyleColor(4);
        return pressed;
    };

    auto applyTouchScroll = [&]() {
#ifdef __ANDROID__
        ImGuiIO& io = ImGui::GetIO();
        // Threshold of 8px distinguishes a swipe from a tap; ChildWindows flag lets
        // hover detection work even when a nested item captured the active state.
        if (ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem | ImGuiHoveredFlags_ChildWindows) &&
            ImGui::IsMouseDragging(ImGuiMouseButton_Left, 8.0f * style::uiScale)) {
            float nextScrollY = std::clamp(ImGui::GetScrollY() - io.MouseDelta.y, 0.0f, ImGui::GetScrollMaxY());
            ImGui::SetScrollY(nextScrollY);
            if (ImGui::GetScrollMaxX() > 0.0f) {
                float nextScrollX = std::clamp(ImGui::GetScrollX() - io.MouseDelta.x, 0.0f, ImGui::GetScrollMaxX());
                ImGui::SetScrollX(nextScrollX);
            }
        }
#endif
    };

    auto setMissionMode = [&](int mode) {
        predatorMissionMode = mode;
        if (mode == PREDATOR_MODE_MANUAL || mode == PREDATOR_MODE_CLASSIFY) {
            predatorScanRunning = false;
            predatorScanPaused = false;
            predatorQuickScanStartedAt = 0.0;
            predatorScanStatus = "Idle";
        }
        savePredatorState();
    };

    sourceName = sigpath::sourceManager.getSelectedSourceName();
    double phoneLat = 0.0;
    double phoneLon = 0.0;
    float phoneAccuracy = 0.0f;
    bool phoneHasFix = false;
    backend::getPhoneLocation(phoneLat, phoneLon, phoneAccuracy, phoneHasFix);

    json searchBands;
    json targets;
    json excludes;
    json hits;
    json events;
    json networkAliases;
    float missionThreshold = -55.0f;
    int dwellMs = 1000;
    int quickScanDelayMs = 250;
    int quickScanDurationMs = 5000;
    bool recordAudio = true;
    bool dsdFmeEnabled = false;
    std::string dsdFmeHost = "127.0.0.1";
    int dsdFmePort = 7355;
    std::string dsdFmeMode = "TCP Direct Link Audio";
    json decoderBridges;
    std::string voiceOutputPath = "%ROOT%/voice";
    std::string dataOutputPath = "%ROOT%/data";
    std::string sessionNote = "";
    double hitClusterHz = 3000.0;
    core::configManager.acquire();
    searchBands = core::configManager.conf["predatorSearchBands"];
    targets = core::configManager.conf["predatorTargets"];
    excludes = core::configManager.conf["predatorExcludes"];
    hits = core::configManager.conf["predatorHits"];
    events = core::configManager.conf["predatorEvents"];
    networkAliases = core::configManager.conf.contains("predatorNetworkAliases") && core::configManager.conf["predatorNetworkAliases"].is_object() ? core::configManager.conf["predatorNetworkAliases"] : json::object();
    decoderBridges = core::configManager.conf.contains("predatorDecoderBridges") && core::configManager.conf["predatorDecoderBridges"].is_object() ? core::configManager.conf["predatorDecoderBridges"] : json::object();
    missionThreshold = core::configManager.conf["predatorThreshold"];
    dwellMs = core::configManager.conf["predatorDwellMs"];
    quickScanDelayMs = core::configManager.conf["predatorQuickScanDelayMs"];
    quickScanDurationMs = core::configManager.conf["predatorQuickScanDurationMs"];
    recordAudio = core::configManager.conf["predatorRecordAudio"];
    dsdFmeEnabled = core::configManager.conf["predatorDsdFmeEnabled"];
    dsdFmeHost = (std::string)core::configManager.conf["predatorDsdFmeHost"];
    dsdFmePort = core::configManager.conf["predatorDsdFmePort"];
    dsdFmeMode = (std::string)core::configManager.conf["predatorDsdFmeMode"];
    voiceOutputPath = (std::string)core::configManager.conf["predatorVoiceOutputPath"];
    dataOutputPath = (std::string)core::configManager.conf["predatorDataOutputPath"];
    sessionNote = (std::string)core::configManager.conf["predatorSessionNote"];
    hitClusterHz = core::configManager.conf["predatorHitClusterHz"];
    core::configManager.release();

    static FolderSelect voiceFolderSelect("%ROOT%/voice");
    static FolderSelect dataFolderSelect("%ROOT%/data");
    static bool decoderFolderSelectorsInitialized = false;
    if (!decoderFolderSelectorsInitialized) {
        voiceFolderSelect.setPath(voiceOutputPath);
        dataFolderSelect.setPath(dataOutputPath);
        std::error_code ec;
        std::filesystem::create_directories(voiceFolderSelect.expandString(voiceOutputPath), ec);
        std::filesystem::create_directories(dataFolderSelect.expandString(dataOutputPath), ec);
        decoderFolderSelectorsInitialized = true;
    }
    static char sessionNoteBuf[512] = "";
    static bool sessionNoteInitialized = false;
    if (!sessionNoteInitialized) {
        snprintf(sessionNoteBuf, sizeof(sessionNoteBuf), "%s", sessionNote.c_str());
        sessionNoteInitialized = true;
    }
    static std::string exportStatus = "";

    auto saveMissionConfig = [&](const json& newSearchBands, const json& newTargets, const json& newExcludes, float newThreshold, int newDwellMs, int newQuickScanDelayMs, int newQuickScanDurationMs, bool newRecordAudio) {
        core::configManager.acquire();
        core::configManager.conf["predatorSearchBands"] = newSearchBands;
        core::configManager.conf["predatorTargets"] = newTargets;
        core::configManager.conf["predatorExcludes"] = newExcludes;
        core::configManager.conf["predatorThreshold"] = newThreshold;
        core::configManager.conf["predatorDwellMs"] = newDwellMs;
        core::configManager.conf["predatorQuickScanDelayMs"] = newQuickScanDelayMs;
        core::configManager.conf["predatorQuickScanDurationMs"] = newQuickScanDurationMs;
        core::configManager.conf["predatorRecordAudio"] = newRecordAudio;
        core::configManager.release(true);
    };

    auto savePredatorEvents = [&](const json& newEvents) {
        core::configManager.acquire();
        core::configManager.conf["predatorEvents"] = newEvents;
        core::configManager.release(true);
    };

    auto savePredatorHits = [&](const json& newHits) {
        core::configManager.acquire();
        core::configManager.conf["predatorHits"] = newHits;
        core::configManager.release(true);
    };

    auto saveNetworkAliases = [&](const json& newAliases) {
        core::configManager.acquire();
        core::configManager.conf["predatorNetworkAliases"] = newAliases;
        core::configManager.release(true);
    };

    auto saveDecoderBridges = [&](const json& newBridges) {
        core::configManager.acquire();
        core::configManager.conf["predatorDecoderBridges"] = newBridges;
        core::configManager.release(true);
    };

    auto ensureDecoderBridge = [&](const std::string& key, const std::string& defaultHost, int defaultPort, const std::string& defaultMode, const std::string& defaultNotes) {
        if (!decoderBridges.is_object()) { decoderBridges = json::object(); }
        if (!decoderBridges.contains(key) || !decoderBridges[key].is_object()) {
            json b;
            b["enabled"] = false;
            b["host"] = defaultHost;
            b["port"] = defaultPort;
            b["mode"] = defaultMode;
            b["notes"] = defaultNotes;
            decoderBridges[key] = b;
        } else {
            if (!decoderBridges[key].contains("enabled") || !decoderBridges[key]["enabled"].is_boolean()) { decoderBridges[key]["enabled"] = false; }
            if (!decoderBridges[key].contains("host")    || !decoderBridges[key]["host"].is_string())     { decoderBridges[key]["host"]    = defaultHost; }
            if (!decoderBridges[key].contains("port")    || !decoderBridges[key]["port"].is_number())     { decoderBridges[key]["port"]    = defaultPort; }
            if (!decoderBridges[key].contains("mode")    || !decoderBridges[key]["mode"].is_string())     { decoderBridges[key]["mode"]    = defaultMode; }
            if (!decoderBridges[key].contains("notes")   || !decoderBridges[key]["notes"].is_string())    { decoderBridges[key]["notes"]   = defaultNotes; }
        }
    };

    auto saveSessionNote = [&](const std::string& note) {
        core::configManager.acquire();
        core::configManager.conf["predatorSessionNote"] = note;
        core::configManager.release(true);
    };

    auto saveDsdFmeConfig = [&](bool enabled, const std::string& host, int port, const std::string& mode) {
        core::configManager.acquire();
        core::configManager.conf["predatorDsdFmeEnabled"] = enabled;
        core::configManager.conf["predatorDsdFmeHost"] = host;
        core::configManager.conf["predatorDsdFmePort"] = port;
        core::configManager.conf["predatorDsdFmeMode"] = mode;
        core::configManager.conf["predatorVoiceOutputPath"] = voiceOutputPath;
        core::configManager.conf["predatorDataOutputPath"] = dataOutputPath;
        core::configManager.release(true);
    };

    auto savePeakDetectionConfig = [&]() {
        core::configManager.acquire();
        core::configManager.conf["predatorPeakDetectionEnabled"] = predatorPeakDetectionEnabled;
        core::configManager.conf["predatorPeakSnrDb"] = predatorPeakSnrDb;
        core::configManager.conf["predatorPeakMinSpacingHz"] = predatorPeakMinSpacingHz;
        core::configManager.conf["predatorPeakMaxPerDwell"] = predatorPeakMaxPerDwell;
        core::configManager.conf["predatorMarkerSlots"] = predatorMarkerSlots;
        core::configManager.conf["predatorHoldOnNewHit"] = predatorHoldOnNewHit;
        core::configManager.conf["predatorSuppressDuplicateHits"] = predatorSuppressDuplicateHits;
        core::configManager.conf["predatorDuplicateHitWindowSec"] = predatorDuplicateHitWindowSec;
        core::configManager.conf["predatorExtendDwellOnStrongHit"] = predatorExtendDwellOnStrongHit;
        core::configManager.conf["predatorStrongHitSnrDb"] = predatorStrongHitSnrDb;
        core::configManager.conf["predatorClassifyAutoMarker"] = predatorClassifyAutoMarker;
        core::configManager.release(true);
    };

    auto readJsonBool = [](const json& row, const char* key, bool fallback) {
        if (!row.is_object() || !row.contains(key) || !row[key].is_boolean()) {
            return fallback;
        }
        return (bool)row[key];
    };

    auto readJsonDouble = [](const json& row, const char* key, double fallback) {
        if (!row.is_object() || !row.contains(key) || !row[key].is_number()) {
            return fallback;
        }
        return (double)row[key];
    };

    auto readJsonString = [](const json& row, const char* key, const std::string& fallback) {
        if (!row.is_object() || !row.contains(key) || !row[key].is_string()) {
            return fallback;
        }
        return (std::string)row[key];
    };

    struct PredatorScanCandidate {
        std::string name;
        double frequency;
        double bandwidth;
        bool target;
    };

    auto isExcludedFrequency = [&](double frequency) {
        for (int i = 0; i < excludes.size(); i++) {
            if (!readJsonBool(excludes[i], "enabled", true)) { continue; }
            double excludeFrequency = readJsonDouble(excludes[i], "frequency", 0.0);
            double excludeBandwidth = std::max<double>(readJsonDouble(excludes[i], "bandwidth", 12500.0), 1.0);
            if (std::abs(frequency - excludeFrequency) <= (excludeBandwidth / 2.0)) {
                return true;
            }
        }
        return false;
    };

    auto isInEnabledSearchBand = [&](double frequency) {
        for (int i = 0; i < searchBands.size(); i++) {
            if (!readJsonBool(searchBands[i], "enabled", true)) { continue; }
            double start = readJsonDouble(searchBands[i], "start", 0.0);
            double stop = readJsonDouble(searchBands[i], "stop", 0.0);
            if (start > stop) { std::swap(start, stop); }
            if (frequency >= start && frequency <= stop) {
                return true;
            }
        }
        return false;
    };

    auto isKnownTargetFrequency = [&](double frequency) {
        for (int i = 0; i < targets.size(); i++) {
            double targetFrequency = readJsonDouble(targets[i], "frequency", 0.0);
            double targetBandwidth = std::max<double>(readJsonDouble(targets[i], "bandwidth", predatorPeakMinSpacingHz), predatorPeakMinSpacingHz);
            if (std::abs(frequency - targetFrequency) <= (targetBandwidth / 2.0)) {
                return true;
            }
        }
        return false;
    };

    auto buildScanCandidates = [&]() {
        std::vector<PredatorScanCandidate> candidates;
        for (int i = 0; i < targets.size(); i++) {
            if (!readJsonBool(targets[i], "enabled", true)) { continue; }
            double frequency = readJsonDouble(targets[i], "frequency", 0.0);
            if (frequency <= 0.0 || isExcludedFrequency(frequency)) { continue; }
            PredatorScanCandidate cand;
            cand.name = readJsonString(targets[i], "name", "Target");
            cand.frequency = frequency;
            cand.bandwidth = readJsonDouble(targets[i], "bandwidth", 12500.0);
            cand.target = true;
            candidates.push_back(cand);
        }

        double stepHz = std::max<double>(gui::waterfall.getViewBandwidth() * 0.75, 1000000.0);
        for (int i = 0; i < searchBands.size(); i++) {
            if (!readJsonBool(searchBands[i], "enabled", true)) { continue; }
            double start = readJsonDouble(searchBands[i], "start", 0.0);
            double stop = readJsonDouble(searchBands[i], "stop", 0.0);
            if (start <= 0.0 || stop <= 0.0 || start == stop) { continue; }
            if (start > stop) { std::swap(start, stop); }
            std::string name = readJsonString(searchBands[i], "name", "Band");
            double span = stop - start;
            int steps = std::max<int>(1, (int)std::ceil(span / stepHz));
            for (int step = 0; step <= steps; step++) {
                double frac = (steps == 0) ? 0.0 : ((double)step / (double)steps);
                double frequency = start + (span * frac);
                if (frequency <= 0.0 || isExcludedFrequency(frequency)) { continue; }
                PredatorScanCandidate cand;
                cand.name = name;
                cand.frequency = frequency;
                cand.bandwidth = stepHz;
                cand.target = false;
                candidates.push_back(cand);
            }
        }
        return candidates;
    };

    auto receiverVfoName = [&]() {
        if (sigpath::vfoManager.vfoExists("Radio")) {
            return std::string("Radio");
        }
        for (auto const& [name, _vfo] : gui::waterfall.vfos) {
            if (name.rfind("Predator M", 0) != 0) {
                return name;
            }
        }
        return std::string("");
    };

    auto tuneReceiverFrequency = [&](double frequency) {
        std::string rxVfo = receiverVfoName();
        tuner::centerTuning(rxVfo, frequency);
        if (!rxVfo.empty() && sigpath::vfoManager.vfoExists(rxVfo)) {
            gui::waterfall.selectedVFO = rxVfo;
            gui::waterfall.selectedVFOChanged = true;
        }
        gui::freqSelect.setFrequency(frequency);
        gui::freqSelect.frequencyChanged = false;
        core::configManager.acquire();
        core::configManager.conf["frequency"] = gui::waterfall.getCenterFrequency();
        if (!rxVfo.empty() && gui::waterfall.vfos.find(rxVfo) != gui::waterfall.vfos.end()) {
            core::configManager.conf["vfoOffsets"][rxVfo] = gui::waterfall.vfos[rxVfo]->generalOffset;
        }
        core::configManager.release(true);
    };

    auto tunePredatorFrequency = [&](double frequency) {
        tuneReceiverFrequency(frequency);
    };

    auto stopPredatorScan = [&]() {
        predatorScanRunning = false;
        predatorScanPaused = false;
        predatorQuickScanStartedAt = 0.0;
        predatorScanStatus = "Idle";
    };

    auto formatFrequency = [](double frequency) {
        char buf[64];
        if (frequency >= 1000000000.0) {
            snprintf(buf, sizeof(buf), "%.6f GHz", frequency / 1000000000.0);
        }
        else if (frequency >= 1000000.0) {
            snprintf(buf, sizeof(buf), "%.6f MHz", frequency / 1000000.0);
        }
        else if (frequency >= 1000.0) {
            snprintf(buf, sizeof(buf), "%.3f kHz", frequency / 1000.0);
        }
        else {
            snprintf(buf, sizeof(buf), "%.0f Hz", frequency);
        }
        return std::string(buf);
    };

    auto currentTimestamp = []() {
        char buf[32];
        std::time_t now = std::time(nullptr);
        std::tm* tm = std::localtime(&now);
        if (tm == NULL) {
            snprintf(buf, sizeof(buf), "unknown");
        }
        else {
            std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", tm);
        }
        return std::string(buf);
    };

    auto filenameTimestamp = []() {
        char buf[32];
        std::time_t now = std::time(nullptr);
        std::tm* tm = std::localtime(&now);
        if (tm == NULL) {
            snprintf(buf, sizeof(buf), "unknown");
        }
        else {
            std::strftime(buf, sizeof(buf), "%Y%m%d_%H%M%S", tm);
        }
        return std::string(buf);
    };

    auto extractionPath = [&](bool voice, const std::string& baseName, const char* extension) {
        FolderSelect& selector = voice ? voiceFolderSelect : dataFolderSelect;
        std::string configuredPath = voice ? voiceOutputPath : dataOutputPath;
        std::filesystem::path dir = selector.expandString(configuredPath);
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        return (dir / (baseName + extension)).string();
    };

    auto appendPredatorEvent = [&](const char* type, double frequency, const std::string& label, float strengthDb, bool persist, const std::string& decoder = "None", const std::string& hitState = "unknown") {
        json row;
        char eventIdBuf[128];
        snprintf(eventIdBuf, sizeof(eventIdBuf), "%s_%s_%lld_%d", filenameTimestamp().c_str(), type, (long long)std::llround(frequency), (int)events.size());
        std::string eventId = eventIdBuf;
        row["time"] = currentTimestamp();
        row["eventId"] = eventId;
        row["type"] = type;
        row["frequency"] = frequency;
        row["label"] = label;
        row["strengthDb"] = strengthDb;
        row["decoder"] = decoder;
        row["hitState"] = hitState;
        row["protocol"] = (decoder == "DSD-FME") ? "Digital Voice" : ((decoder == "RTL433") ? "RTL433" : "Unknown");
        row["networkId"] = "Unknown";
        row["talkgroup"] = "Unknown";
        row["radioId"] = "Unknown";
        row["voicePath"] = voiceOutputPath;
        row["dataPath"] = dataOutputPath;
        row["clipBaseName"] = eventId;
        row["voiceClipPath"] = extractionPath(true, eventId, ".wav");
        row["dataClipPath"] = extractionPath(false, eventId, ".json");
        row["hasAudio"] = false;
        row["hasData"] = false;
        row["source"] = sourceName.empty() ? "None" : sourceName;
        row["sourceDevice"] = "local";
        row["mode"] = predatorMissionMode;
        row["gpsFix"] = phoneHasFix;
        if (phoneHasFix) {
            row["lat"] = phoneLat;
            row["lon"] = phoneLon;
            row["accuracyM"] = phoneAccuracy;
        }
        row["serial"] = ++predatorEventSerial;
        events.insert(events.begin(), row);
        while (events.size() > 200) {
            events.erase(events.end() - 1);
        }
        if (persist) {
            savePredatorEvents(events);
        }
    };

    // -------- Decoder bridge ingestion (RTL433) --------
    // Ensure RTL433 bridge config is normalized every frame, even if the user
    // never opened the Decoder Bridges section. This lets the ingester start
    // automatically on app launch when enabled was persisted as true.
    ensureDecoderBridge("rtl433", "127.0.0.1", 1433, "TCP JSON Lines",
        "rtl_433 ISM device telemetry (315/433/868/915 MHz). Each JSON line becomes one Network event; protocol = device model, networkId = device id, talkgroup = channel.");

    static predator::Rtl433Ingester rtl433Ingester;
    {
        json& b = decoderBridges["rtl433"];
        bool wantActive   = readJsonBool(b, "enabled", false);
        std::string wHost = readJsonString(b, "host", "127.0.0.1");
        int         wPort = (int)readJsonDouble(b, "port", 1433.0);
        std::string wMode = readJsonString(b, "mode", "TCP JSON Lines");

        static bool        active = false;
        static std::string activeHost;
        static int         activePort = 0;
        static std::string activeMode;

        if (wantActive && (!active || activeHost != wHost || activePort != wPort || activeMode != wMode)) {
            rtl433Ingester.start(wHost, wPort, wMode);
            active = true;
            activeHost = wHost;
            activePort = wPort;
            activeMode = wMode;
        } else if (!wantActive && active) {
            rtl433Ingester.stop();
            active = false;
        }
    }

    {
        auto pending = rtl433Ingester.drain(64);
        if (!pending.empty()) {
            for (auto& e : pending) {
                json row;
                char idBuf[160];
                snprintf(idBuf, sizeof(idBuf), "%s_rtl433_%lld_%d",
                         filenameTimestamp().c_str(),
                         (long long)std::llround(e.frequencyHz),
                         (int)events.size());
                row["time"]         = currentTimestamp();
                row["eventId"]      = std::string(idBuf);
                row["type"]         = "decoder";
                row["frequency"]    = e.frequencyHz;
                row["label"]        = e.label.empty() ? std::string("RTL433") : e.label;
                row["strengthDb"]   = e.strengthDb;
                row["decoder"]      = "RTL433";
                row["hitState"]     = "decoded";
                row["protocol"]     = e.protocol;
                row["networkId"]    = e.networkId;
                row["talkgroup"]    = e.talkgroup;
                row["radioId"]      = e.radioId;
                row["voicePath"]    = voiceOutputPath;
                row["dataPath"]     = dataOutputPath;
                row["clipBaseName"] = std::string(idBuf);
                row["voiceClipPath"] = "";
                row["dataClipPath"]  = "";
                row["hasAudio"]     = false;
                row["hasData"]      = true;
                row["source"]       = "Bridge:RTL433";
                row["mode"]         = predatorMissionMode;
                row["gpsFix"]       = phoneHasFix;
                if (phoneHasFix) {
                    row["lat"]       = phoneLat;
                    row["lon"]       = phoneLon;
                    row["accuracyM"] = phoneAccuracy;
                }
                row["raw"] = e.raw;
                row["serial"] = ++predatorEventSerial;
                events.insert(events.begin(), row);
            }
            while (events.size() > 200) events.erase(events.end() - 1);
            savePredatorEvents(events);
        }
    }

    // -------- Native in-APK decoder modules (rtl_433, future P25, ...) --------
    // Plugins like decoder_modules/rtl433_decoder register a drain callback
    // with predator::registerNativeDecoder() on construction. We pull from
    // every registered native module here and fold their events into the
    // same predatorEvents stream the bridge ingesters feed, so the Hits
    // tab, Network tree, Map, and exporter all just work.
    {
        auto batches = predator::drainAllNativeDecoders(64);
        bool anyAdded = false;
        for (auto& batch : batches) {
            const std::string& src = batch.sourceKey;
            const std::string  sourceLabel = std::string("Native:") + src;
            for (auto& e : batch.events) {
                json row;
                char idBuf[160];
                snprintf(idBuf, sizeof(idBuf), "%s_native%s_%lld_%d",
                         filenameTimestamp().c_str(),
                         src.c_str(),
                         (long long)std::llround(e.frequencyHz),
                         (int)events.size());
                row["time"]          = currentTimestamp();
                row["eventId"]       = std::string(idBuf);
                row["type"]          = "decoder";
                row["frequency"]     = e.frequencyHz;
                row["label"]         = e.label.empty() ? src : e.label;
                row["strengthDb"]    = e.strengthDb;
                row["decoder"]       = src;
                row["hitState"]      = "decoded";
                row["protocol"]      = e.protocol;
                row["networkId"]     = e.networkId;
                row["talkgroup"]     = e.talkgroup;
                row["radioId"]       = e.radioId;
                row["voicePath"]     = voiceOutputPath;
                row["dataPath"]      = dataOutputPath;
                row["clipBaseName"]  = std::string(idBuf);
                row["voiceClipPath"] = "";
                row["dataClipPath"]  = "";
                row["hasAudio"]      = false;
                row["hasData"]       = true;
                row["source"]        = sourceLabel;
                row["mode"]          = predatorMissionMode;
                row["gpsFix"]        = phoneHasFix;
                if (phoneHasFix) {
                    row["lat"]       = phoneLat;
                    row["lon"]       = phoneLon;
                    row["accuracyM"] = phoneAccuracy;
                }
                row["raw"]    = e.raw;
                row["serial"] = ++predatorEventSerial;
                events.insert(events.begin(), row);
                anyAdded = true;
            }
        }
        if (anyAdded) {
            while (events.size() > 200) events.erase(events.end() - 1);
            savePredatorEvents(events);
        }
    }

    // -------- Decoder bridge ingestion (ADS-B / dump1090) --------
    // BaseStation port 30003 CSV feed from dump1090 / readsb. Each MSG line
    // becomes one Network event tagged decoder=ADSB; lat/lon/altitude live in
    // the raw payload for the tactical map to consume.
    ensureDecoderBridge("adsb", "127.0.0.1", 30003, "BaseStation 30003",
        "dump1090 / readsb feed. networkId = ICAO hex, talkgroup = callsign, radioId = squawk. Aircraft positions can be forwarded to the tactical map.");

    static predator::AdsbIngester adsbIngester;
    {
        json& b = decoderBridges["adsb"];
        bool wantActive   = readJsonBool(b, "enabled", false);
        std::string wHost = readJsonString(b, "host", "127.0.0.1");
        int         wPort = (int)readJsonDouble(b, "port", 30003.0);
        std::string wMode = readJsonString(b, "mode", "BaseStation 30003");

        static bool        active = false;
        static std::string activeHost;
        static int         activePort = 0;
        static std::string activeMode;

        if (wantActive && (!active || activeHost != wHost || activePort != wPort || activeMode != wMode)) {
            adsbIngester.start(wHost, wPort, wMode);
            active = true;
            activeHost = wHost;
            activePort = wPort;
            activeMode = wMode;
        } else if (!wantActive && active) {
            adsbIngester.stop();
            active = false;
        }
    }

    {
        auto pending = adsbIngester.drain(64);
        if (!pending.empty()) {
            for (auto& e : pending) {
                json row;
                char idBuf[160];
                snprintf(idBuf, sizeof(idBuf), "%s_adsb_%s_%d",
                         filenameTimestamp().c_str(),
                         e.networkId.empty() ? "unk" : e.networkId.c_str(),
                         (int)events.size());
                row["time"]         = currentTimestamp();
                row["eventId"]      = std::string(idBuf);
                row["type"]         = "decoder";
                row["frequency"]    = e.frequencyHz;
                row["label"]        = e.label.empty() ? std::string("ADS-B") : e.label;
                row["strengthDb"]   = e.strengthDb;
                row["decoder"]      = "ADSB";
                row["hitState"]     = "decoded";
                row["protocol"]     = e.protocol;
                row["networkId"]    = e.networkId;
                row["talkgroup"]    = e.talkgroup;
                row["radioId"]      = e.radioId;
                row["voicePath"]    = voiceOutputPath;
                row["dataPath"]     = dataOutputPath;
                row["clipBaseName"] = std::string(idBuf);
                row["voiceClipPath"] = "";
                row["dataClipPath"]  = "";
                row["hasAudio"]     = false;
                row["hasData"]      = true;
                row["source"]       = "Bridge:ADSB";
                row["mode"]         = predatorMissionMode;
                row["gpsFix"]       = phoneHasFix;
                if (phoneHasFix) {
                    row["lat"]       = phoneLat;
                    row["lon"]       = phoneLon;
                    row["accuracyM"] = phoneAccuracy;
                }
                // Surface aircraft position at top level when present so the
                // tactical map and exporter can plot it without digging into raw.
                if (e.raw.is_object() && e.raw.contains("lat") && e.raw.contains("lon")
                    && e.raw["lat"].is_number() && e.raw["lon"].is_number()) {
                    row["aircraftLat"] = e.raw["lat"];
                    row["aircraftLon"] = e.raw["lon"];
                }
                row["raw"] = e.raw;
                row["serial"] = ++predatorEventSerial;
                events.insert(events.begin(), row);
            }
            while (events.size() > 200) events.erase(events.end() - 1);
            savePredatorEvents(events);
        }
    }

    // -------- Decoder bridge ingestion (P25 trunked voice) --------
    // JSON-line feed from a DSD-FME or OP25 companion process. Each line is
    // one P25 control-channel or call event. networkId = WACN (or WACN/SYSID
    // when both are reported), talkgroup = TG alias if available else "TG <id>",
    // radioId = source unit id. Encrypted flag, ALGID and KEYID are surfaced
    // in raw so the topology tree can highlight encrypted calls.
    ensureDecoderBridge("p25", "127.0.0.1", 7355, "DSD-FME Direct",
        "P25 Phase 1 (C4FM) and Phase 2 (H-DQPSK). Routes through the DSD-FME bridge below; Talkgroup, Network ID, and Radio ID populate the topology tree as decoded WACN/RFSS metadata arrives.");

    static predator::P25Ingester p25Ingester;
    {
        json& b = decoderBridges["p25"];
        bool wantActive   = readJsonBool(b, "enabled", false);
        std::string wHost = readJsonString(b, "host", "127.0.0.1");
        int         wPort = (int)readJsonDouble(b, "port", 7355.0);
        std::string wMode = readJsonString(b, "mode", "DSD-FME Direct");

        static bool        active = false;
        static std::string activeHost;
        static int         activePort = 0;
        static std::string activeMode;

        if (wantActive && (!active || activeHost != wHost || activePort != wPort || activeMode != wMode)) {
            p25Ingester.start(wHost, wPort, wMode);
            active = true;
            activeHost = wHost;
            activePort = wPort;
            activeMode = wMode;
        } else if (!wantActive && active) {
            p25Ingester.stop();
            active = false;
        }
    }

    {
        auto pending = p25Ingester.drain(64);
        if (!pending.empty()) {
            for (auto& e : pending) {
                json row;
                char idBuf[160];
                snprintf(idBuf, sizeof(idBuf), "%s_p25_%s_%d",
                         filenameTimestamp().c_str(),
                         e.networkId.empty() ? "unk" : e.networkId.c_str(),
                         (int)events.size());
                row["time"]         = currentTimestamp();
                row["eventId"]      = std::string(idBuf);
                row["type"]         = "decoder";
                row["frequency"]    = e.frequencyHz;
                row["label"]        = e.label.empty() ? std::string("P25") : e.label;
                row["strengthDb"]   = e.strengthDb;
                row["decoder"]      = "P25";
                row["hitState"]     = "decoded";
                row["protocol"]     = e.protocol;
                row["networkId"]    = e.networkId;
                row["talkgroup"]    = e.talkgroup;
                row["radioId"]      = e.radioId;
                row["voicePath"]    = voiceOutputPath;
                row["dataPath"]     = dataOutputPath;
                row["clipBaseName"] = std::string(idBuf);
                row["voiceClipPath"] = "";
                row["dataClipPath"]  = "";
                row["hasAudio"]     = false;
                row["hasData"]      = true;
                row["source"]       = "Bridge:P25";
                row["mode"]         = predatorMissionMode;
                row["gpsFix"]       = phoneHasFix;
                if (phoneHasFix) {
                    row["lat"]       = phoneLat;
                    row["lon"]       = phoneLon;
                    row["accuracyM"] = phoneAccuracy;
                }
                // Surface the encrypted flag at row top level so the event
                // log and tactical map can colour encrypted calls without
                // digging into raw on every frame.
                if (e.raw.is_object() && e.raw.contains("encrypted")
                    && e.raw["encrypted"].is_boolean()) {
                    row["encrypted"] = e.raw["encrypted"];
                }
                row["raw"] = e.raw;
                row["serial"] = ++predatorEventSerial;
                events.insert(events.begin(), row);
            }
            while (events.size() > 200) events.erase(events.end() - 1);
            savePredatorEvents(events);
        }
    }

    // -------- Kujhad fleet lifecycle --------
    // Device-side server runs when role==Device and the server is enabled.
    // Controller-side clients are reloaded from the persisted peer list
    // any time it changes. Both halves are reused across roles so an
    // operator can run as a Device while still browsing peers; only the
    // active workflow differs by role.
    static predator::KujhadDeviceServer kujhadServer;
    static bool kujhadProvidersBound = false;

    // Bind snapshot providers exactly once. The captures see the current
    // frame's locals via reference; this is safe because all callers run
    // from worker threads that read snapshots through atomics or copies.
    if (!kujhadProvidersBound) {
        kujhadProvidersBound = true;
        kujhadServer.setIdentifyProvider([this]() {
            std::lock_guard<std::mutex> lk(kujhadSnapshotMtx);
            json j;
            j["device"]      = kujhadDeviceName;
            j["version"]     = "Predator RF 1.x (Kujhad v1)";
            j["role"]        = (predatorRole == PREDATOR_ROLE_CONTROLLER) ? "controller" : "device";
            j["api"]         = 1;
            j["rxOnly"]      = true;
            // Operator-supplied advertised address. Empty means "let
            // the controller use whatever IP it dialed in on" — the
            // Reachable Addresses panel surfaces auto-detected NICs as
            // a fallback.
            j["advertise"]   = kujhadAdvertiseAddress;
            j["hwProfile"]   = {
                {"source", kujhadSourceNameSnapshot.empty() ? std::string("None") : kujhadSourceNameSnapshot},
                {"centerFreq", kujhadCenterFreqSnapshot}
            };
            return j;
        });
        kujhadServer.setStateProvider([this]() {
            std::lock_guard<std::mutex> lk(kujhadSnapshotMtx);
            json j;
            j["centerFreq"]  = kujhadCenterFreqSnapshot;
            j["playing"]     = kujhadPlayingSnapshot;
            j["missionMode"] = kujhadMissionModeSnapshot;
            j["scanRunning"] = kujhadScanRunningSnapshot;
            j["scanStatus"]  = kujhadScanStatusSnapshot;
            // Mission edit surface — mirrored so a controller can render
            // and edit the peer's bands/targets/excludes/settings via
            // mission.set* commands without first guessing the contents.
            j["searchBands"]         = kujhadSearchBandsSnapshot;
            j["targets"]             = kujhadTargetsSnapshot;
            j["excludes"]            = kujhadExcludesSnapshot;
            j["thresholdDb"]         = kujhadThresholdSnapshot;
            j["dwellMs"]             = kujhadDwellMsSnapshot;
            j["quickScanDelayMs"]    = kujhadQuickScanDelayMsSnapshot;
            j["quickScanDurationMs"] = kujhadQuickScanDurationMsSnapshot;
            j["recordAudio"]         = kujhadRecordAudioSnapshot;
            return j;
        });
        kujhadServer.setGpsProvider([this]() {
            std::lock_guard<std::mutex> lk(kujhadSnapshotMtx);
            json j;
            j["hasFix"]   = kujhadGpsHasFix;
            j["lat"]      = kujhadGpsLat;
            j["lon"]      = kujhadGpsLon;
            j["accuracy"] = kujhadGpsAccuracy;
            return j;
        });
        kujhadServer.setEventsProvider([this](uint64_t since) {
            // Incremental cursor: only events with serial > since are
            // returned. lastId is the max serial seen in the snapshot,
            // so the controller advances its `since` watermark and
            // never receives the same row twice. The snapshot is the
            // newest-first tail kept by the UI thread; we filter and
            // re-emit oldest-first so the controller can append in
            // order without sorting.
            std::lock_guard<std::mutex> lk(kujhadSnapshotMtx);
            json events = json::array();
            uint64_t lastId = since;
            // kujhadEventsSnapshot is newest-first; iterate in reverse
            // to deliver oldest-first.
            for (int i = (int)kujhadEventsSnapshot.size() - 1; i >= 0; i--) {
                const auto& row = kujhadEventsSnapshot[i];
                if (!row.is_object()) continue;
                uint64_t serial = row.contains("serial") && row["serial"].is_number_unsigned()
                                ? row["serial"].get<uint64_t>()
                                : (row.contains("serial") && row["serial"].is_number_integer()
                                   ? (uint64_t)row["serial"].get<int64_t>()
                                   : 0);
                if (serial == 0) continue; // untagged rows skip the wire
                if (serial <= since) continue;
                events.push_back(row);
                if (serial > lastId) lastId = serial;
            }
            json j;
            j["events"] = events;
            j["lastId"] = lastId;
            return j;
        });
        kujhadServer.setSpectrumProvider([this](json& out) -> bool {
            // Downsample the latest captured FFT row to ~kujhadSpectrumBins
            // bins via max-bucketing. Returns false when no frame has ever
            // been captured, so the device server skips the tick. Frames
            // carry the local serial so a controller can detect a stalled
            // capture.
            std::vector<float> raw;
            uint64_t serial = 0;
            int srcSize = 0;
            {
                std::lock_guard<std::mutex> lk(kujhadSpectrumMtx);
                if (!kujhadSpectrumHaveRaw || kujhadSpectrumRawSize <= 0) return false;
                raw = kujhadSpectrumRaw; // small copy under lock
                srcSize = kujhadSpectrumRawSize;
                serial = kujhadSpectrumLocalSerial;
            }
            int targetBins = kujhadSpectrumBins.load(std::memory_order_relaxed);
            if (targetBins < 32) targetBins = 32;
            if (targetBins > 1024) targetBins = 1024;
            if (targetBins > srcSize) targetBins = srcSize;
            std::vector<float> bins(targetBins, -150.0f);
            // Max-bucketing keeps narrow peaks visible after downsample —
            // averaging would smear narrowband emitters into noise.
            double step = (double)srcSize / (double)targetBins;
            for (int i = 0; i < targetBins; i++) {
                int a = (int)(i * step);
                int b = (int)((i + 1) * step);
                if (b > srcSize) b = srcSize;
                if (b <= a) b = a + 1;
                float m = -INFINITY;
                for (int k = a; k < b; k++) {
                    if (raw[k] > m) m = raw[k];
                }
                if (!std::isfinite(m)) m = -150.0f;
                bins[i] = m;
            }
            double centerFreq = 0.0, bandwidth = 0.0;
            float fftMin = -120.0f, fftMax = 0.0f;
            {
                std::lock_guard<std::mutex> lk(kujhadSnapshotMtx);
                centerFreq = kujhadCenterFreqSnapshot;
                bandwidth  = kujhadBandwidthSnapshot;
                fftMin     = kujhadFFTMinSnapshot;
                fftMax     = kujhadFFTMaxSnapshot;
            }
            // Overlay arrays (hits / search bands / targets / excludes)
            // travel alongside the bins so a mirroring controller can
            // paint the same vertical hit markers and band shading the
            // operator sees locally on this device. Snapshot copies are
            // refreshed once per draw() and small in size.
            json hitsCopy, bandsCopy, targetsCopy, excludesCopy;
            {
                std::lock_guard<std::mutex> lk(kujhadSnapshotMtx);
                hitsCopy     = kujhadHitsSnapshot;
                bandsCopy    = kujhadSearchBandsSnapshot;
                targetsCopy  = kujhadTargetsSnapshot;
                excludesCopy = kujhadExcludesSnapshot;
            }
            out = json::object();
            out["serial"]      = serial;
            out["tsMs"]        = (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::steady_clock::now().time_since_epoch()).count();
            out["centerFreq"]  = centerFreq;
            out["bandwidth"]   = bandwidth;
            out["fftMinDb"]    = fftMin;
            out["fftMaxDb"]    = fftMax;
            out["bins"]        = bins;
            out["hits"]        = hitsCopy;
            out["searchBands"] = bandsCopy;
            out["targets"]     = targetsCopy;
            out["excludes"]    = excludesCopy;
            return true;
        });
        kujhadServer.setCommandHandler([this](const predator::KujhadDeviceCommand& cmd, std::string& errOut) {
            // Validate basic shape on the worker thread, then enqueue
            // for the UI thread to actually mutate SDR state. The
            // protocol still returns ok=true here because the command
            // was accepted; rejection happens at the wire/protocol layer
            // (tx.* class) and is reported as 403 separately.
            if (cmd.commandClass == "tune" && cmd.action == "set") {
                if (cmd.args.value("frequencyHz", 0.0) <= 0) {
                    errOut = "frequencyHz required"; return false;
                }
            }
            else if (cmd.commandClass == "mission" && cmd.action == "setMode") {
                int mode = cmd.args.value("mode", -1);
                if (mode < PREDATOR_MODE_MANUAL || mode > PREDATOR_MODE_QUICKSCAN) {
                    errOut = "invalid mode"; return false;
                }
            }
            else if (cmd.commandClass == "mission" &&
                     (cmd.action == "setSearchBands" || cmd.action == "setTargets" ||
                      cmd.action == "setExcludes")) {
                // Per-action payload key — explicit so a malformed
                // command (wrong key, wrong type) is rejected at the
                // protocol layer instead of corrupting peer config.
                const char* key = (cmd.action == "setSearchBands") ? "bands"
                                : (cmd.action == "setTargets")     ? "targets"
                                                                   : "excludes";
                if (!cmd.args.contains(key) || !cmd.args[key].is_array()) {
                    errOut = std::string(key) + " array required"; return false;
                }
            }
            else if (cmd.commandClass == "mission" && cmd.action == "setSettings") {
                // All fields optional; presence is what triggers the
                // overwrite on the peer. Empty body is a no-op rather
                // than an error.
                if (!cmd.args.is_object()) {
                    errOut = "settings object required"; return false;
                }
            }
            else if (cmd.commandClass == "identify") {
                // Ack only — no action needed.
                return true;
            }
            else if (cmd.commandClass == "scan") {
                if (cmd.action != "start" && cmd.action != "stop") {
                    errOut = "unknown scan action"; return false;
                }
            }
            else {
                errOut = "unhandled command";
                return false;
            }
            std::lock_guard<std::mutex> lk(kujhadCommandMtx);
            kujhadPendingCommands.push_back({ cmd.commandClass, cmd.action, cmd.args });
            return true;
        });
    }

    // Refresh the snapshot members so the device-server providers have
    // current data on the next request. Cheap copies under a short lock.
    {
        std::lock_guard<std::mutex> lk(kujhadSnapshotMtx);
        kujhadCenterFreqSnapshot   = gui::waterfall.getCenterFrequency();
        kujhadBandwidthSnapshot    = gui::waterfall.getBandwidth();
        kujhadFFTMinSnapshot       = gui::waterfall.getFFTMin();
        kujhadFFTMaxSnapshot       = gui::waterfall.getFFTMax();
        kujhadPlayingSnapshot      = playing;
        kujhadMissionModeSnapshot  = predatorMissionMode;
        kujhadScanRunningSnapshot  = predatorScanRunning;
        kujhadScanStatusSnapshot   = predatorScanStatus;
        kujhadSourceNameSnapshot   = sourceName;
        kujhadGpsHasFix            = phoneHasFix;
        kujhadGpsLat               = phoneLat;
        kujhadGpsLon               = phoneLon;
        kujhadGpsAccuracy          = phoneAccuracy;
        // Tail of last 32 events.
        kujhadEventsSnapshot = json::array();
        int n = (int)std::min<size_t>(events.size(), 32);
        for (int i = n - 1; i >= 0; i--) {
            kujhadEventsSnapshot.push_back(events[i]);
        }
        // Mission edit surface, shipped to controllers via /v1/state so
        // the Mission tab on a controller can render the peer's bands /
        // targets / excludes / settings 1:1 with the operator's local
        // experience instead of falling back to the smaller Kujhad-tab
        // command surface.
        kujhadSearchBandsSnapshot         = searchBands;
        kujhadTargetsSnapshot             = targets;
        kujhadExcludesSnapshot            = excludes;
        // Hits ride along on /v1/spectrum frames so a controller can
        // paint peer hit markers without an extra round-trip.
        kujhadHitsSnapshot                = hits;
        kujhadThresholdSnapshot           = missionThreshold;
        kujhadDwellMsSnapshot             = dwellMs;
        kujhadQuickScanDelayMsSnapshot    = quickScanDelayMs;
        kujhadQuickScanDurationMsSnapshot = quickScanDurationMs;
        kujhadRecordAudioSnapshot         = recordAudio;
    }

    // Drain any pending commands from the device server worker.
    {
        std::vector<KujhadPendingCommand> drained;
        {
            std::lock_guard<std::mutex> lk(kujhadCommandMtx);
            drained.swap(kujhadPendingCommands);
        }
        for (auto& cmd : drained) {
            if (cmd.commandClass == "tune" && cmd.action == "set") {
                double freq = cmd.args.value("frequencyHz", 0.0);
                if (freq > 0) {
                    gui::freqSelect.setFrequency(freq);
                    sigpath::sourceManager.tune(freq);
                    gui::waterfall.setCenterFrequency(freq);
                }
            }
            else if (cmd.commandClass == "scan") {
                if (cmd.action == "start") predatorScanRunning = true;
                else if (cmd.action == "stop") predatorScanRunning = false;
            }
            else if (cmd.commandClass == "mission" && cmd.action == "setMode") {
                int mode = cmd.args.value("mode", -1);
                if (mode >= PREDATOR_MODE_MANUAL && mode <= PREDATOR_MODE_QUICKSCAN) {
                    predatorMissionMode = mode;
                }
            }
            else if (cmd.commandClass == "mission" && cmd.action == "setSearchBands") {
                if (cmd.args.contains("bands") && cmd.args["bands"].is_array()) {
                    searchBands = cmd.args["bands"];
                    saveMissionConfig(searchBands, targets, excludes, missionThreshold,
                                      dwellMs, quickScanDelayMs, quickScanDurationMs, recordAudio);
                }
            }
            else if (cmd.commandClass == "mission" && cmd.action == "setTargets") {
                if (cmd.args.contains("targets") && cmd.args["targets"].is_array()) {
                    targets = cmd.args["targets"];
                    saveMissionConfig(searchBands, targets, excludes, missionThreshold,
                                      dwellMs, quickScanDelayMs, quickScanDurationMs, recordAudio);
                }
            }
            else if (cmd.commandClass == "mission" && cmd.action == "setExcludes") {
                if (cmd.args.contains("excludes") && cmd.args["excludes"].is_array()) {
                    excludes = cmd.args["excludes"];
                    saveMissionConfig(searchBands, targets, excludes, missionThreshold,
                                      dwellMs, quickScanDelayMs, quickScanDurationMs, recordAudio);
                }
            }
            else if (cmd.commandClass == "mission" && cmd.action == "setSettings") {
                // Only fields actually present in the payload overwrite
                // peer state; missing fields keep their current value
                // so a controller can edit one slider in isolation.
                if (cmd.args.contains("thresholdDb") && cmd.args["thresholdDb"].is_number()) {
                    missionThreshold = (float)cmd.args["thresholdDb"].get<double>();
                }
                if (cmd.args.contains("dwellMs") && cmd.args["dwellMs"].is_number()) {
                    dwellMs = std::max<int>(100, cmd.args["dwellMs"].get<int>());
                }
                if (cmd.args.contains("quickScanDelayMs") && cmd.args["quickScanDelayMs"].is_number()) {
                    quickScanDelayMs = std::max<int>(50, cmd.args["quickScanDelayMs"].get<int>());
                }
                if (cmd.args.contains("quickScanDurationMs") && cmd.args["quickScanDurationMs"].is_number()) {
                    quickScanDurationMs = std::max<int>(100, cmd.args["quickScanDurationMs"].get<int>());
                }
                if (cmd.args.contains("recordAudio") && cmd.args["recordAudio"].is_boolean()) {
                    recordAudio = cmd.args["recordAudio"].get<bool>();
                }
                saveMissionConfig(searchBands, targets, excludes, missionThreshold,
                                  dwellMs, quickScanDelayMs, quickScanDurationMs, recordAudio);
            }
        }
    }

    // Match desired vs running state for the Device server.
    {
        bool wantRun = (predatorRole == PREDATOR_ROLE_DEVICE) && kujhadDeviceServerEnabled;
        if (wantRun && !kujhadServer.isRunning()) {
            // Apply current TLS settings before binding. setTlsConfig
            // returns false (and clears tlsEnabled internally) when
            // cert/key fail to load — we still call start() so the
            // listener comes up in plain-loopback mode and the UI can
            // surface the error rather than silently dropping the
            // server. tlsEnabled() reflects what's actually active.
            std::string tlsErr;
            kujhadServer.setTlsConfig(kujhadTlsEnabled, kujhadTlsCertPath, kujhadTlsKeyPath, tlsErr);
            kujhadTlsConfigError = tlsErr;
            kujhadTlsFingerprint = kujhadServer.tlsFingerprint();
            if (kujhadTlsFingerprint.empty()) {
                // Fall back to whatever's on disk so the operator at
                // least sees a hint when TLS is off.
                kujhadTlsFingerprint = predator::kujhadCertFingerprintFromPemFile(kujhadTlsCertPath);
            }
            kujhadServer.start(kujhadDeviceListenPort, kujhadApiKey);
            kujhadDeviceServerRunning = kujhadServer.isListening();
        }
        if (!wantRun && kujhadServer.isRunning()) {
            kujhadServer.stop();
            kujhadDeviceServerRunning = false;
        }
        // Push the operator-tunable spectrum cadence into the server so
        // existing chunked streams pick up the change on the next tick.
        kujhadServer.setSpectrumIntervalMs(kujhadSpectrumIntervalMs.load(std::memory_order_relaxed));
        kujhadDeviceServerStatus = kujhadServer.status();
        kujhadDeviceServerRunning = kujhadServer.isListening();
    }

    // Controller side: persisted peers turn into per-peer worker clients.
    // We diff the persisted list against the live client list each frame
    // and reconcile (add new, remove deleted, restart on config change).
    static std::vector<std::unique_ptr<predator::KujhadControllerClient>> kujhadClients;
    static std::vector<json> kujhadActivePeers; // mirror of persisted peers
    {
        json& persistedPeers = core::configManager.conf["kujhadPeers"];
        if (!persistedPeers.is_array()) persistedPeers = json::array();
        bool wantRun = (predatorRole == PREDATOR_ROLE_CONTROLLER);
        // Detect change via crude content compare against active mirror.
        bool changed = (persistedPeers.size() != kujhadActivePeers.size());
        if (!changed) {
            for (size_t i = 0; i < persistedPeers.size() && !changed; i++) {
                if (persistedPeers[i] != kujhadActivePeers[i]) changed = true;
            }
        }
        if (changed || (wantRun && kujhadClients.size() != persistedPeers.size())) {
            // Tear down current clients and rebuild from scratch — the
            // peer count is small (typically 1-4) so a full rebuild is
            // simpler than incremental diffing.
            for (auto& c : kujhadClients) { if (c) c->stop(); }
            kujhadClients.clear();
            kujhadActivePeers.clear();
            if (wantRun) {
                for (auto& p : persistedPeers) {
                    auto client = std::make_unique<predator::KujhadControllerClient>();
                    bool enabled = readJsonBool(p, "enabled", true);
                    if (enabled) {
                        // Per-peer TLS toggle + pinned fingerprint. An
                        // empty pin means "trust on first use" — the
                        // first observed cert is surfaced in the UI so
                        // the operator can copy it back into the peer
                        // config to lock the pin in.
                        bool tls = readJsonBool(p, "tls", false);
                        std::string pin = readJsonString(p, "pinnedFingerprint", "");
                        client->start(readJsonString(p, "host", "127.0.0.1"),
                                      (int)readJsonDouble(p, "port", 41947.0),
                                      readJsonString(p, "apiKey", kujhadApiKey),
                                      tls, pin);
                    }
                    kujhadClients.push_back(std::move(client));
                    kujhadActivePeers.push_back(p);
                }
            }
        }
        if (!wantRun && !kujhadClients.empty()) {
            for (auto& c : kujhadClients) { if (c) c->stop(); }
            kujhadClients.clear();
            kujhadActivePeers.clear();
        }
        // Drain peer events into the local event log, tagged with the
        // sourceDevice = peer name so the Hits/Network tables can show
        // origin without a join.
        for (size_t i = 0; i < kujhadClients.size() && i < kujhadActivePeers.size(); i++) {
            auto& c = kujhadClients[i];
            if (!c) continue;
            std::string peerName = readJsonString(kujhadActivePeers[i], "name", "peer");
            auto pendingEvents = c->drainEvents(64);
            for (auto& e : pendingEvents) {
                if (!e.is_object()) continue;
                json row = e;
                row["sourceDevice"] = peerName;
                if (!row.contains("source")) row["source"] = "Peer:" + peerName;
                // Re-stamp serial on local insertion so the local
                // /v1/events stream has its own monotonic cursor —
                // the peer's own serial may collide with ours.
                row["peerSerial"] = row.value("serial", (uint64_t)0);
                row["serial"] = ++predatorEventSerial;
                events.insert(events.begin(), row);
            }
            while (events.size() > 200) events.erase(events.end() - 1);
        }
        // Spectrum subscription follows the active peer + mirror toggle.
        // Only the selected peer's stream is opened, since the controller
        // can only display one mirrored peer at a time. Bandwidth is
        // capped at the device's spectrum cadence, but this also keeps
        // the link cost tied to a single connection.
        bool wantMirror = wantRun && kujhadMirrorPeerSpectrum
                          && kujhadActivePeerIdx >= 0
                          && kujhadActivePeerIdx < (int)kujhadClients.size()
                          && kujhadClients[kujhadActivePeerIdx];
        for (size_t i = 0; i < kujhadClients.size(); i++) {
            auto& c = kujhadClients[i];
            if (!c) continue;
            bool shouldStream = wantMirror && (int)i == kujhadActivePeerIdx;
            if (shouldStream && !c->spectrumActive()) c->startSpectrum();
            if (!shouldStream && c->spectrumActive())  c->stopSpectrum();
        }
        // When the operator turns the mirror OFF (or switches active peer
        // away), drop the cached peer frame so a stale peer row cannot
        // leak into the next FFT push, reset the per-frame serial tracker
        // so re-enabling repaints from a clean state, AND restore the
        // local waterfall view (center freq + bandwidth) that was in
        // effect just before the operator entered mirror mode. Without
        // this restore the waterfall would stay tuned to the peer's
        // last frame, defeating the source-switch contract.
        if (!wantMirror) {
            bool wasMirroring = kujhadMirrorActive.exchange(false, std::memory_order_release);
            if (wasMirroring && kujhadLocalViewSaved) {
                if (gui::waterfall.getCenterFrequency() != kujhadLocalSavedCenter) {
                    gui::waterfall.setCenterFrequency(kujhadLocalSavedCenter);
                }
                if (kujhadLocalSavedBW > 0.0
                    && gui::waterfall.getBandwidth() != kujhadLocalSavedBW) {
                    gui::waterfall.setBandwidth(kujhadLocalSavedBW);
                }
                if (kujhadLocalSavedViewBW > 0.0) {
                    gui::waterfall.setViewBandwidth(kujhadLocalSavedViewBW);
                }
                kujhadLocalViewSaved = false;
            }
            std::lock_guard<std::mutex> lk(kujhadSpectrumMtx);
            kujhadPeerCachedBins.clear();
            kujhadPeerCachedSerial = 0;
            kujhadLastPeerSpectrumSerial = 0;
            kujhadMirroredFromPeerIdx = -1;
            // Drop the peer overlay caches as well so an old hit/band
            // set can't bleed onto the next mirror session.
            kujhadPeerCachedHits        = json::array();
            kujhadPeerCachedSearchBands = json::array();
            kujhadPeerCachedTargets     = json::array();
            kujhadPeerCachedExcludes    = json::array();
        }
    }

    // Mission-tab peer routing. The operator's *selection* in the Kujhad
    // tab is the source of truth: while a peer is selected, the Mission
    // tab MUST route edits through the fleet command pipe — even if the
    // peer's snapshot is stale, never-synced, or temporarily unreachable.
    // Falling back to local mutation while a peer is selected would
    // silently corrupt the operator's local rig and break the operator's
    // mental model of "I am driving that peer right now". When the
    // snapshot is missing the displayed lists default to empty + the
    // banner shows a "no link / stale" indicator so the operator knows
    // why the form is blank, but routing stays peer-bound.
    bool missionPeerActive = false;
    bool missionPeerSnapshotFresh = false;
    std::string missionPeerName;
    json missionPeerState = json::object();
    int missionPeerMode = predatorMissionMode;
    bool missionPeerScanRunning = predatorScanRunning;
    bool missionPeerPlaying = playing;
    uint64_t missionPeerLastSyncMs = 0;
    if (predatorRole == PREDATOR_ROLE_CONTROLLER
        && kujhadActivePeerIdx >= 0
        && kujhadActivePeerIdx < (int)kujhadClients.size()
        && kujhadClients[kujhadActivePeerIdx]
        && kujhadActivePeerIdx < (int)kujhadActivePeers.size()) {
        missionPeerActive = true;
        missionPeerName   = readJsonString(kujhadActivePeers[kujhadActivePeerIdx], "name", "peer");
        auto& client = kujhadClients[kujhadActivePeerIdx];
        predator::KujhadPeerSnapshot snap = client->snapshot();
        missionPeerLastSyncMs = snap.lastSyncMs;
        // Treat the snapshot as "fresh" only when the worker is
        // currently reaching the peer AND a state body has actually
        // arrived. Stale state is still rendered (so the operator
        // sees the last-known mission config) but the banner reports
        // the staleness so they know commands may not land.
        if (snap.state.is_object()) {
            missionPeerState        = snap.state;
            missionPeerMode         = snap.state.value("missionMode", predatorMissionMode);
            missionPeerScanRunning  = snap.state.value("scanRunning", false);
            missionPeerPlaying      = snap.state.value("playing",     false);
            missionPeerSnapshotFresh = snap.reachable;
        }
    }
    // Send a command to the active peer and surface ok / failure in
    // the persistent status banner. Returns true on success.
    auto missionRoutePeerCmd = [&](const std::string& cls, const std::string& action,
                                    const json& args) -> bool {
        if (!missionPeerActive) return false;
        auto& client = kujhadClients[kujhadActivePeerIdx];
        std::string err;
        bool ok = client && client->sendCommand(cls, action, args, err);
        char buf[256];
        if (ok) {
            std::snprintf(buf, sizeof(buf), "%s.%s -> %s ok",
                          cls.c_str(), action.c_str(), missionPeerName.c_str());
        } else {
            std::snprintf(buf, sizeof(buf), "%s.%s -> %s FAILED: %s",
                          cls.c_str(), action.c_str(), missionPeerName.c_str(), err.c_str());
        }
        kujhadStatusBanner = buf;
        kujhadStatusBannerUntil = ImGui::GetTime() + 4.0;
        return ok;
    };
    // Mission lists rendered by the Mission tab. When peer-driven these
    // mirror the peer's /v1/state payload; when local they are aliases
    // for the configManager-backed arrays. Edits in peer mode build a
    // mutated copy and ship it via mission.set* without ever touching
    // the operator's local config.
    auto missionPeerArray = [&](const char* key) -> json {
        if (!missionPeerActive) return json::array();
        if (!missionPeerState.contains(key) || !missionPeerState[key].is_array()) return json::array();
        return missionPeerState[key];
    };

    auto exportPredatorSession = [&]() {
        std::string root = (std::string)core::args["root"];
        std::filesystem::path exportDir = std::filesystem::path(root) / "exports";
        std::error_code ec;
        std::filesystem::create_directories(exportDir, ec);
        if (ec) {
            exportStatus = "Export failed: " + ec.message();
            return;
        }

        json session;
        session["app"] = "Predator RF";
        session["exportedAt"] = currentTimestamp();
        session["note"] = std::string(sessionNoteBuf);
        session["mode"] = predatorMissionMode;
        session["source"] = sourceName.empty() ? "None" : sourceName;
        session["centerFrequency"] = gui::waterfall.getCenterFrequency();
        session["threshold"] = missionThreshold;
        session["dwellMs"] = dwellMs;
        session["quickScanDelayMs"] = quickScanDelayMs;
        session["quickScanDurationMs"] = quickScanDurationMs;
        session["recordAudio"] = recordAudio;
        session["voiceOutputPath"] = voiceOutputPath;
        session["dataOutputPath"] = dataOutputPath;
        session["gpsFix"] = phoneHasFix;
        if (phoneHasFix) {
            session["lat"] = phoneLat;
            session["lon"] = phoneLon;
            session["accuracyM"] = phoneAccuracy;
        }
        session["searchBands"] = searchBands;
        session["targets"] = targets;
        session["excludes"] = excludes;
        session["hits"] = hits;
        session["events"] = events;
        session["networkAliases"] = networkAliases;

        std::filesystem::path exportPath = exportDir / ("predator_session_" + filenameTimestamp() + ".json");
        std::ofstream out(exportPath);
        if (!out.is_open()) {
            exportStatus = "Export failed: unable to open file";
            return;
        }
        out << session.dump(2);
        out.close();
        exportStatus = "Exported " + exportPath.string();
    };

    // Export hits as CSV
    auto exportHitsCsv = [&]() {
        std::string root = (std::string)core::args["root"];
        std::filesystem::path exportDir = std::filesystem::path(root) / "exports";
        std::error_code ec;
        std::filesystem::create_directories(exportDir, ec);
        if (ec) { exportStatus = "Export failed: " + ec.message(); return; }

        std::filesystem::path outPath = exportDir / ("predator_hits_" + filenameTimestamp() + ".csv");
        std::ofstream out(outPath);
        if (!out.is_open()) { exportStatus = "Export failed: cannot open file"; return; }

        // Header
        out << "timestamp,frequency_hz,frequency,name,state,snr_db,last_rssi_db,max_rssi_db,"
               "hit_count,event_count,unread_count,decoder,marker_assigned,last_seen,note\n";

        std::string now = currentTimestamp();
        for (int i = 0; i < (int)hits.size(); i++) {
            double freq   = readJsonDouble(hits[i], "frequency", 0.0);
            std::string st = readJsonString(hits[i], "state", "unknown");
            // Escape CSV fields that may contain commas or quotes
            auto csvEsc = [](const std::string& s) {
                bool needsQuote = s.find(',') != std::string::npos || s.find('"') != std::string::npos || s.find('\n') != std::string::npos;
                if (!needsQuote) return s;
                std::string out2 = "\"";
                for (char c : s) { if (c == '"') out2 += '"'; out2 += c; }
                out2 += '"';
                return out2;
            };
            out << now << ","
                << std::llround(freq) << ","
                << csvEsc(formatFrequency(freq)) << ","
                << csvEsc(readJsonString(hits[i], "name", "Hit")) << ","
                << csvEsc(st) << ","
                << readJsonDouble(hits[i], "snrDb", 0.0) << ","
                << readJsonDouble(hits[i], "lastRssi", 0.0) << ","
                << readJsonDouble(hits[i], "maxRssi", 0.0) << ","
                << (int)readJsonDouble(hits[i], "hitCount", 0.0) << ","
                << (int)readJsonDouble(hits[i], "eventCount", 0.0) << ","
                << (int)readJsonDouble(hits[i], "unreadCount", 0.0) << ","
                << csvEsc(readJsonString(hits[i], "decoder", "None")) << ","
                << (readJsonBool(hits[i], "markerAssigned", false) ? "1" : "0") << ","
                << csvEsc(readJsonString(hits[i], "lastSeen", "")) << ","
                << csvEsc(readJsonString(hits[i], "note", "")) << "\n";
        }
        out.close();
        exportStatus = "Hits exported: " + outPath.string() + " (" + std::to_string((int)hits.size()) + " rows)";
    };

    // Export events as CSV
    auto exportEventsCsv = [&]() {
        std::string root = (std::string)core::args["root"];
        std::filesystem::path exportDir = std::filesystem::path(root) / "exports";
        std::error_code ec;
        std::filesystem::create_directories(exportDir, ec);
        if (ec) { exportStatus = "Export failed: " + ec.message(); return; }

        std::filesystem::path outPath = exportDir / ("predator_events_" + filenameTimestamp() + ".csv");
        std::ofstream out(outPath);
        if (!out.is_open()) { exportStatus = "Export failed: cannot open file"; return; }

        out << "time,type,label,frequency_hz,frequency,strength_db,decoder,network_id,talkgroup\n";

        auto csvEsc = [](const std::string& s) {
            bool needsQuote = s.find(',') != std::string::npos || s.find('"') != std::string::npos || s.find('\n') != std::string::npos;
            if (!needsQuote) return s;
            std::string out2 = "\"";
            for (char c : s) { if (c == '"') out2 += '"'; out2 += c; }
            out2 += '"';
            return out2;
        };

        for (int i = 0; i < (int)events.size(); i++) {
            double freq = readJsonDouble(events[i], "frequency", 0.0);
            out << csvEsc(readJsonString(events[i], "time",      "")) << ","
                << csvEsc(readJsonString(events[i], "type",      "event")) << ","
                << csvEsc(readJsonString(events[i], "label",     "")) << ","
                << std::llround(freq) << ","
                << csvEsc(formatFrequency(freq)) << ","
                << readJsonDouble(events[i], "strengthDb", 0.0) << ","
                << csvEsc(readJsonString(events[i], "decoder",   "None")) << ","
                << csvEsc(readJsonString(events[i], "networkId", "")) << ","
                << csvEsc(readJsonString(events[i], "talkgroup", "")) << "\n";
        }
        out.close();
        exportStatus = "Events exported: " + outPath.string() + " (" + std::to_string((int)events.size()) + " rows)";
    };

    auto hitStateForFrequency = [&](double frequency, const json& row) {
        if (isExcludedFrequency(frequency)) { return std::string("exclude"); }
        if (isKnownTargetFrequency(frequency)) { return std::string("target"); }
        return readJsonString(row, "state", "unknown");
    };

    auto assignedMarkerCount = [&]() {
        int count = 0;
        for (int i = 0; i < hits.size(); i++) {
            if (readJsonBool(hits[i], "markerAssigned", false)) {
                count++;
            }
        }
        return count;
    };

    auto markerSlotForHitIndex = [&](int hitIndex) {
        int slot = 1;
        for (int i = 0; i < hits.size(); i++) {
            if (!readJsonBool(hits[i], "markerAssigned", false)) { continue; }
            if (i == hitIndex) { return slot; }
            slot++;
        }
        return std::max<int>(1, std::min<int>(predatorMarkerSlots, hitIndex + 1));
    };

    auto routeHitToVfo = [&](int hitIndex) {
        if (hitIndex < 0 || hitIndex >= hits.size()) { return false; }
        json& hit = hits[hitIndex];
        double frequency = readJsonDouble(hit, "frequency", 0.0);
        if (frequency <= 0.0) { return false; }

        double bandwidth = std::max<double>(readJsonDouble(hit, "bandwidth", predatorPeakMinSpacingHz), 200.0);
        int markerSlot = markerSlotForHitIndex(hitIndex);
        std::string vfoName = "Predator M" + std::to_string(markerSlot);
        double wholeBandwidth = std::max<double>(gui::waterfall.getBandwidth(), bandwidth);
        double centerFrequency = gui::waterfall.getCenterFrequency();

        bool inScanMode = predatorScanRunning && (predatorMissionMode == PREDATOR_MODE_SCAN || predatorMissionMode == PREDATOR_MODE_QUICKSCAN);
        if (inScanMode) {
            hit["routeState"] = "tracked";
            hit["routeVfo"] = "";
            hit["routeFrequency"] = frequency;
            hit["routeBandwidth"] = bandwidth;
            hit["markerSlot"] = markerSlot;
            return true;
        }

        if (std::abs(frequency - centerFrequency) > (wholeBandwidth * 0.48)) {
            tunePredatorFrequency(frequency);
            centerFrequency = gui::waterfall.getCenterFrequency();
            wholeBandwidth = std::max<double>(gui::waterfall.getBandwidth(), bandwidth);
        }

        double offset = frequency - centerFrequency;
        double sampleRate = std::max<double>(wholeBandwidth, bandwidth);
        if (!sigpath::vfoManager.vfoExists(vfoName)) {
            sigpath::vfoManager.createVFO(vfoName, ImGui::WaterfallVFO::REF_CENTER, offset, bandwidth, sampleRate, 200.0, std::max<double>(sampleRate, bandwidth), false);
        }
        else {
            sigpath::vfoManager.setReference(vfoName, ImGui::WaterfallVFO::REF_CENTER);
            sigpath::vfoManager.setSampleRate(vfoName, sampleRate, bandwidth);
            sigpath::vfoManager.setCenterOffset(vfoName, offset);
            sigpath::vfoManager.setBandwidth(vfoName, bandwidth);
        }
        sigpath::vfoManager.setColor(vfoName, IM_COL32(120, 220, 95, 255));

        hit["routeState"] = "routed";
        hit["routeVfo"] = vfoName;
        hit["routeFrequency"] = frequency;
        hit["routeBandwidth"] = bandwidth;
        hit["markerSlot"] = markerSlot;
        return true;
    };

    auto releaseHitRoute = [&](int hitIndex) {
        if (hitIndex < 0 || hitIndex >= hits.size()) { return false; }
        json& hit = hits[hitIndex];
        std::string vfoName = readJsonString(hit, "routeVfo", "");
        if (vfoName.rfind("Predator M", 0) == 0 && sigpath::vfoManager.vfoExists(vfoName)) {
            sigpath::vfoManager.deleteVFO(vfoName);
        }
        hit["routeState"] = "released";
        hit["routeVfo"] = "";
        hit["markerSlot"] = 0;
        return true;
    };

    auto recordPeakHit = [&](double frequency, float strengthDb, float noiseDb, float snrDb) {
        if (frequency <= 0.0 || isExcludedFrequency(frequency) ||
            (predatorMissionMode != PREDATOR_MODE_CLASSIFY && !isInEnabledSearchBand(frequency))) {
            return 0;
        }

        int hitIndex = -1;
        bool newHit = false;
        double clusterWidth = std::max<double>(hitClusterHz, 100.0);
        for (int i = 0; i < hits.size(); i++) {
            double hitFrequency = readJsonDouble(hits[i], "frequency", 0.0);
            if (std::abs(hitFrequency - frequency) <= clusterWidth) {
                hitIndex = i;
                break;
            }
        }

        if (hitIndex < 0) {
            json row;
            row["name"] = "Hit " + formatFrequency(frequency);
            row["frequency"] = frequency;
            row["clusterHz"] = clusterWidth;
            row["bandwidth"] = std::max<double>(predatorPeakMinSpacingHz, 1.0);
            row["state"] = isKnownTargetFrequency(frequency) ? "target" : "unknown";
            row["hitCount"] = 0;
            row["eventCount"] = 0;
            row["unreadCount"] = 0;
            row["markerAssigned"] = false;
            row["decoder"] = "None";
            row["source"] = "fft_peak";
            bool autoAssignMarker = (predatorMissionMode == PREDATOR_MODE_SCAN || predatorMissionMode == PREDATOR_MODE_QUICKSCAN) ||
                                    (predatorMissionMode == PREDATOR_MODE_CLASSIFY && predatorClassifyAutoMarker);
            if (autoAssignMarker && assignedMarkerCount() < predatorMarkerSlots) {
                row["markerAssigned"] = true;
            }
            hits.push_back(row);
            hitIndex = (int)hits.size() - 1;
            if (readJsonBool(hits[hitIndex], "markerAssigned", false)) {
                routeHitToVfo(hitIndex);
            }
            newHit = true;
        }

        json& hit = hits[hitIndex];
        int hitCount = (int)readJsonDouble(hit, "hitCount", 0.0) + 1;
        float maxRssi = (float)std::max<double>(readJsonDouble(hit, "maxRssi", strengthDb), strengthDb);
        std::string state = hitStateForFrequency(frequency, hit);
        std::string decoder = readJsonString(hit, "decoder", "None");
        double now = ImGui::GetTime();
        double lastEventClock = readJsonDouble(hit, "lastEventClock", -999999.0);
        bool suppressEvent = predatorSuppressDuplicateHits && !newHit && ((now - lastEventClock) < (double)std::max<int>(1, predatorDuplicateHitWindowSec));

        hit["hitCount"] = hitCount;
        hit["lastSeen"] = currentTimestamp();
        hit["lastRssi"] = strengthDb;
        hit["maxRssi"] = maxRssi;
        hit["noiseDb"] = noiseDb;
        hit["snrDb"] = snrDb;
        hit["state"] = state;

        if (!suppressEvent) {
            int eventCount = (int)readJsonDouble(hit, "eventCount", 0.0) + 1;
            int unreadCount = (int)readJsonDouble(hit, "unreadCount", 0.0) + 1;
            hit["eventCount"] = eventCount;
            hit["unreadCount"] = unreadCount;
            hit["lastEventClock"] = now;
            appendPredatorEvent("hit", frequency, readJsonString(hit, "name", "Hit"), strengthDb, false, decoder, state);
        }
        savePredatorHits(hits);
        if (!suppressEvent) {
            savePredatorEvents(events);
        }
        predatorScanStatus = "Hit " + formatFrequency(frequency);
        if (newHit && state == "unknown") { return 2; }
        return suppressEvent ? 0 : 1;
    };

    auto detectScanPeaks = [&]() {
        bool scanningMode = (predatorMissionMode == PREDATOR_MODE_SCAN || predatorMissionMode == PREDATOR_MODE_QUICKSCAN);
        bool classifyMode = (predatorMissionMode == PREDATOR_MODE_CLASSIFY && predatorClassifyAutoMarker);
        if (!predatorPeakDetectionEnabled || !playing || (!scanningMode && !classifyMode)) {
            return 0;
        }
        int width = 0;
        float* fft = gui::waterfall.acquireLatestFFT(width);
        if (fft == NULL || width < 8) {
            return 0;
        }

        double lowFreq = gui::waterfall.getCenterFrequency() + gui::waterfall.getViewOffset() - (gui::waterfall.getViewBandwidth() / 2.0);
        double pixelToFreq = gui::waterfall.getViewBandwidth() / (double)width;
        double visibleStart = lowFreq;
        double visibleStop = lowFreq + gui::waterfall.getViewBandwidth();

        double noiseSum = 0.0;
        int noiseCount = 0;
        for (int i = 0; i < width; i++) {
            if (fft[i] <= -900.0f || !std::isfinite(fft[i])) { continue; }
            double freq = lowFreq + ((double)i + 0.5) * pixelToFreq;
            if (freq < visibleStart || freq > visibleStop ||
                (predatorMissionMode != PREDATOR_MODE_CLASSIFY && !isInEnabledSearchBand(freq)) ||
                isExcludedFrequency(freq)) { continue; }
            noiseSum += fft[i];
            noiseCount++;
        }
        if (noiseCount < 8) {
            gui::waterfall.releaseLatestFFT();
            return 0;
        }

        float noiseDb = (float)(noiseSum / (double)noiseCount);
        struct PeakCandidate {
            double frequency;
            float strengthDb;
            float snrDb;
            int bin;
        };
        std::vector<PeakCandidate> rawPeaks;
        std::vector<PeakCandidate> peaks;
        int guardBins = std::max<int>(2, (int)std::ceil(predatorPeakMinSpacingHz / std::max<double>(pixelToFreq, 1.0)));
        int edgeGuardBins = std::max<int>(2, width / 40);
        int dcGuardBins = std::max<int>(2, (int)std::ceil(1500.0 / std::max<double>(pixelToFreq, 1.0)));
        int centerBin = width / 2;
        for (int i = 1 + edgeGuardBins; i < width - 1 - edgeGuardBins; i++) {
            if (fft[i] <= -900.0f || !std::isfinite(fft[i])) { continue; }
            if (std::abs(i - centerBin) <= dcGuardBins) { continue; }
            if (fft[i] <= fft[i - 1] || fft[i] < fft[i + 1]) { continue; }
            float snrDb = fft[i] - noiseDb;
            if (fft[i] < missionThreshold || snrDb < predatorPeakSnrDb) { continue; }
            double freq = lowFreq + ((double)i + 0.5) * pixelToFreq;
            if ((predatorMissionMode != PREDATOR_MODE_CLASSIFY && !isInEnabledSearchBand(freq)) || isExcludedFrequency(freq)) { continue; }

            double weightedFreq = 0.0;
            double weightedPower = 0.0;
            int left = std::max<int>(edgeGuardBins, i - guardBins);
            int right = std::min<int>(width - 1 - edgeGuardBins, i + guardBins);
            float includeFloor = noiseDb + std::max<float>(3.0f, snrDb * 0.35f);
            for (int b = left; b <= right; b++) {
                if (fft[b] <= includeFloor || !std::isfinite(fft[b])) { continue; }
                double bf = lowFreq + ((double)b + 0.5) * pixelToFreq;
                if ((predatorMissionMode != PREDATOR_MODE_CLASSIFY && !isInEnabledSearchBand(bf)) || isExcludedFrequency(bf)) { continue; }
                double weight = std::max<double>(0.0, fft[b] - includeFloor);
                weightedFreq += bf * weight;
                weightedPower += weight;
            }
            if (weightedPower > 0.0) {
                freq = weightedFreq / weightedPower;
            }

            PeakCandidate peak;
            peak.frequency = freq;
            peak.strengthDb = fft[i];
            peak.snrDb = snrDb;
            peak.bin = i;
            rawPeaks.push_back(peak);
        }
        gui::waterfall.releaseLatestFFT();

        std::sort(rawPeaks.begin(), rawPeaks.end(), [](const PeakCandidate& a, const PeakCandidate& b) {
            return a.strengthDb > b.strengthDb;
        });

        for (auto const& peak : rawPeaks) {
            bool nearExistingPeak = false;
            for (auto const& selectedPeak : peaks) {
                if (std::abs(selectedPeak.frequency - peak.frequency) <= predatorPeakMinSpacingHz ||
                    std::abs(selectedPeak.bin - peak.bin) <= guardBins) {
                    nearExistingPeak = true;
                    break;
                }
            }
            if (nearExistingPeak) { continue; }
            peaks.push_back(peak);
        }

        int recorded = 0;
        int newUnknownHits = 0;
        bool strongHit = false;
        int maxPeaks = std::max<int>(1, predatorPeakMaxPerDwell);
        for (auto const& peak : peaks) {
            if (recorded >= maxPeaks) { break; }
            int result = recordPeakHit(peak.frequency, peak.strengthDb, noiseDb, peak.snrDb);
            if (result > 0) {
                recorded++;
                if (peak.snrDb >= predatorStrongHitSnrDb) {
                    strongHit = true;
                }
                if (result == 2) {
                    newUnknownHits++;
                }
            }
        }
        if (scanningMode && predatorHoldOnNewHit && newUnknownHits > 0) {
            predatorScanPaused = true;
            predatorScanStatus = "Paused on new hit";
        }
        else if (scanningMode && predatorExtendDwellOnStrongHit && strongHit) {
            predatorScanLastStepAt = ImGui::GetTime();
            predatorScanStatus = "Extended dwell on strong hit";
        }
        return recorded;
    };

    std::vector<PredatorScanCandidate> scanCandidates = buildScanCandidates();

    auto tuneScanCandidate = [&](int direction) {
        if (scanCandidates.empty()) {
            predatorScanStatus = "No enabled targets or search bands";
            return false;
        }

        int count = (int)scanCandidates.size();
        predatorScanIndex = ((predatorScanIndex + direction) % count + count) % count;
        PredatorScanCandidate cand = scanCandidates[predatorScanIndex];
        tunePredatorFrequency(cand.frequency);
        predatorScanLastFrequency = cand.frequency;
        predatorScanLastStepAt = ImGui::GetTime();
        predatorScanStatus = cand.name + (cand.target ? " target " : " band ") + formatFrequency(cand.frequency);
        double now = ImGui::GetTime();
        if (cand.target && (std::abs(predatorLastAutoEventFrequency - cand.frequency) > 1.0 || (now - predatorLastAutoEventAt) > 30.0)) {
            appendPredatorEvent("target", cand.frequency, cand.name, gui::waterfall.selectedVFOSNR, true);
            predatorLastAutoEventFrequency = cand.frequency;
            predatorLastAutoEventAt = now;
        }
        return true;
    };

    if (predatorScanRunning && (predatorMissionMode == PREDATOR_MODE_SCAN || predatorMissionMode == PREDATOR_MODE_QUICKSCAN)) {
        if (scanCandidates.empty()) {
            predatorScanStatus = "No enabled targets or search bands";
        }
        else if (!predatorScanPaused) {
            double now = ImGui::GetTime();
            double stepSeconds = (double)std::max<int>(100, dwellMs) / 1000.0;
            if (predatorMissionMode == PREDATOR_MODE_QUICKSCAN) {
                stepSeconds += (double)std::max<int>(50, quickScanDelayMs) / 1000.0;
                if (predatorQuickScanStartedAt <= 0.0) {
                    predatorQuickScanStartedAt = now;
                }
                double maxSeconds = (double)std::max<int>(100, quickScanDurationMs) / 1000.0;
                if ((now - predatorQuickScanStartedAt) >= maxSeconds) {
                    predatorScanRunning = false;
                    predatorScanPaused = false;
                    predatorQuickScanStartedAt = 0.0;
                    predatorScanStatus = "QuickScan complete";
                }
            }
            if (predatorScanRunning && (predatorScanLastStepAt <= 0.0 || (now - predatorScanLastStepAt) >= stepSeconds)) {
                bool skipTuneAfterPeakSweep = false;
                if (predatorScanLastStepAt > 0.0 && predatorLastPeakSweepAt != predatorScanLastStepAt) {
                    std::string beforePeakStatus = predatorScanStatus;
                    detectScanPeaks();
                    predatorLastPeakSweepAt = predatorScanLastStepAt;
                    scanCandidates = buildScanCandidates();
                    if (predatorScanStatus == "Extended dwell on strong hit" && beforePeakStatus != predatorScanStatus) {
                        skipTuneAfterPeakSweep = true;
                    }
                }
                if (!predatorScanPaused && !skipTuneAfterPeakSweep) {
                    tuneScanCandidate((predatorScanLastStepAt <= 0.0) ? 0 : 1);
                }
            }
        }
        else {
            predatorScanStatus = "Paused at " + formatFrequency(predatorScanLastFrequency > 0.0 ? predatorScanLastFrequency : gui::freqSelect.frequency);
        }
    }
    else if (predatorScanRunning) {
        stopPredatorScan();
    }

    // Every frame: re-anchor all Predator marker VFOs to their stored absolute frequency.
    // VFO offsets are relative to the current SDR center, so any retune (scan step, manual
    // pan, etc.) would shift the colored band away from the actual signal without this.
    {
        double currentCenter = gui::waterfall.getCenterFrequency();
        for (int i = 0; i < (int)hits.size(); i++) {
            if (!readJsonBool(hits[i], "markerAssigned", false)) continue;
            std::string vfoName = readJsonString(hits[i], "routeVfo", "");
            if (vfoName.empty() || !sigpath::vfoManager.vfoExists(vfoName)) continue;
            double hitFreq = readJsonDouble(hits[i], "frequency", 0.0);
            if (hitFreq <= 0.0) continue;
            sigpath::vfoManager.setReference(vfoName, ImGui::WaterfallVFO::REF_CENTER);
            sigpath::vfoManager.setCenterOffset(vfoName, hitFreq - currentCenter);
        }
    }

    if (!predatorScanRunning && predatorMissionMode == PREDATOR_MODE_CLASSIFY && predatorClassifyAutoMarker && playing) {
        double now = ImGui::GetTime();
        double classifySweepSeconds = (double)std::max<int>(250, dwellMs) / 1000.0;
        if ((now - predatorLastClassifySweepAt) >= classifySweepSeconds) {
            detectScanPeaks();
            predatorLastClassifySweepAt = now;
        }
    }

    auto addCurrentFrequencyRow = [&](json& rows, const char* name, bool target) {
        json row;
        row["name"] = name;
        row["frequency"] = gui::freqSelect.frequency;
        row["bandwidth"] = (vfo != NULL) ? vfo->bandwidth : 12500.0;
        row["enabled"] = true;
        rows.push_back(row);
        saveMissionConfig(searchBands, target ? rows : targets, target ? excludes : rows, missionThreshold, dwellMs, quickScanDelayMs, quickScanDurationMs, recordAudio);
    };

    auto startPredatorScan = [&](int mode) {
        // When the operator has taken control of a peer, the local rig
        // stays untouched: the mode + scan-start commands ride the
        // fleet pipe so the peer is the one tuning, dwelling, and
        // logging hits. The local "scanCandidates empty" / "no SDR"
        // guards intentionally do not run here — the peer enforces
        // its own preconditions and reports back via /v1/state.
        if (missionPeerActive) {
            json modeArgs; modeArgs["mode"] = mode;
            missionRoutePeerCmd("mission", "setMode", modeArgs);
            missionRoutePeerCmd("scan", "start", json::object());
            return;
        }
        predatorMissionMode = mode;
        savePredatorState();
        if (sourceName.empty()) {
            predatorScanStatus = "Select SDR first";
            predatorScanRunning = false;
            return;
        }
        if (scanCandidates.empty()) {
            predatorScanStatus = "No enabled targets or search bands";
            predatorScanRunning = false;
            return;
        }
        if (!playing) {
            setPlayState(true);
        }
        predatorScanRunning = true;
        predatorScanPaused = false;
        predatorScanIndex = std::clamp<int>(predatorScanIndex, 0, (int)scanCandidates.size() - 1);
        predatorScanLastStepAt = 0.0;
        predatorQuickScanStartedAt = (mode == PREDATOR_MODE_QUICKSCAN) ? ImGui::GetTime() : 0.0;
        tuneScanCandidate(0);
    };

    auto drawMissionRunControls = [&]() {
        // Display state mirrors the peer when peer-active so the operator
        // can confirm "the unit I'm driving is actually scanning" without
        // tab-flipping to Kujhad. Pause / Previous / Next / Log Event do
        // not have peer commands in the current schema and stay disabled
        // in peer mode rather than silently mutating the local rig.
        bool runRunning = missionPeerActive ? missionPeerScanRunning : predatorScanRunning;
        bool runPaused  = missionPeerActive ? missionPeerState.value("scanPaused", false)
                                            : predatorScanPaused;
        bool runPlaying = missionPeerActive ? missionPeerPlaying : playing;
        std::string runStatus = missionPeerActive
                                  ? readJsonString(missionPeerState, "scanStatus", "")
                                  : predatorScanStatus;
        int runQueue = missionPeerActive
                         ? (missionPeerArray("searchBands").size()
                            + missionPeerArray("targets").size())
                         : (int)scanCandidates.size();
        ImGui::Text(T("State: %s"), runRunning ? T(runPaused ? "Paused" : "Running") : T("Idle"));
        ImGui::TextWrapped(T("Queue: %d candidate%s"), runQueue, runQueue == 1 ? "" : "s");
        ImGui::TextWrapped(T("Current: %s"), runStatus.c_str());
        if (!missionPeerActive && ImGui::CollapsingHeader(T("Scan Progress"))) {
            int candidateCount = (int)scanCandidates.size();
            int currentStep = (candidateCount > 0) ? (predatorScanIndex + 1) : 0;
            double now = ImGui::GetTime();
            double dwellSeconds = (double)std::max<int>(100, dwellMs) / 1000.0;
            double elapsed = (predatorScanLastStepAt > 0.0) ? std::max<double>(0.0, now - predatorScanLastStepAt) : 0.0;
            double remaining = predatorScanPaused ? dwellSeconds : std::max<double>(0.0, dwellSeconds - elapsed);
            ImGui::Text("Step: %d / %d", currentStep, candidateCount);
            ImGui::Text("Dwell remaining: %.1fs", remaining);
            ImGui::Text("Markers: %d / %d", assignedMarkerCount(), predatorMarkerSlots);
            ImGui::Text("Hits: %d  Events: %d", (int)hits.size(), (int)events.size());
        }

        if (ImGui::Button(runPlaying ? T("Stop Listening") : T("Start Listening"), ImVec2(ImGui::GetContentRegionAvail().x, 0))) {
            if (missionPeerActive) {
                // No symmetrical peer command for "playing"; keep the
                // local rig untouched and surface a hint instead of
                // toggling local audio.
                kujhadStatusBanner = std::string(T("Listening control is local-only while driving a peer."));
                kujhadStatusBannerUntil = ImGui::GetTime() + 4.0;
            }
            else if (sourceName.empty() && !playing) {
                predatorTab = PREDATOR_TAB_SYSTEM;
                showMenu = true;
                savePredatorState();
            }
            else {
                setPlayState(!playing);
            }
        }

        if (ImGui::Button(T("Start Scan"), ImVec2(ImGui::GetContentRegionAvail().x, 0))) {
            startPredatorScan(PREDATOR_MODE_SCAN);
        }
        if (ImGui::Button(T("Start QuickScan"), ImVec2(ImGui::GetContentRegionAvail().x, 0))) {
            startPredatorScan(PREDATOR_MODE_QUICKSCAN);
        }

        float halfWidth = (ImGui::GetContentRegionAvail().x - (6.0f * style::uiScale)) * 0.5f;
        if (missionPeerActive) ImGui::BeginDisabled();
        if (ImGui::Button(runPaused ? T("Resume") : T("Pause"), ImVec2(halfWidth, 0))) {
            if (predatorScanRunning) {
                predatorScanPaused = !predatorScanPaused;
                predatorScanLastStepAt = ImGui::GetTime();
            }
        }
        if (missionPeerActive) ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button(T("Stop"), ImVec2(halfWidth, 0))) {
            if (missionPeerActive) {
                missionRoutePeerCmd("scan", "stop", json::object());
            } else {
                stopPredatorScan();
            }
        }

        if (missionPeerActive) ImGui::BeginDisabled();
        if (ImGui::Button(T("Previous"), ImVec2(halfWidth, 0))) {
            predatorScanRunning = true;
            predatorScanPaused = true;
            tuneScanCandidate(-1);
        }
        ImGui::SameLine();
        if (ImGui::Button(T("Next"), ImVec2(halfWidth, 0))) {
            predatorScanRunning = true;
            predatorScanPaused = true;
            tuneScanCandidate(1);
        }
        if (missionPeerActive) ImGui::EndDisabled();

        if (ImGui::Button(T("Target Current"), ImVec2(halfWidth, 0))) {
            if (missionPeerActive) {
                json updated = missionPeerArray("targets");
                json row;
                row["name"] = "Current Target";
                row["frequency"] = gui::freqSelect.frequency;
                row["bandwidth"] = (vfo != NULL) ? vfo->bandwidth : 12500.0;
                row["enabled"] = true;
                updated.push_back(row);
                json args; args["targets"] = updated;
                missionRoutePeerCmd("mission", "setTargets", args);
            } else {
                addCurrentFrequencyRow(targets, "Current Target", true);
                scanCandidates = buildScanCandidates();
            }
        }
        ImGui::SameLine();
        if (ImGui::Button(T("Exclude Current"), ImVec2(halfWidth, 0))) {
            if (missionPeerActive) {
                json updated = missionPeerArray("excludes");
                json row;
                row["name"] = "Current Exclude";
                row["frequency"] = gui::freqSelect.frequency;
                row["bandwidth"] = (vfo != NULL) ? vfo->bandwidth : 12500.0;
                row["enabled"] = true;
                updated.push_back(row);
                json args; args["excludes"] = updated;
                missionRoutePeerCmd("mission", "setExcludes", args);
            } else {
                addCurrentFrequencyRow(excludes, "Current Exclude", false);
                scanCandidates = buildScanCandidates();
            }
        }

        if (missionPeerActive) ImGui::BeginDisabled();
        if (ImGui::Button(T("Log Event"), ImVec2(ImGui::GetContentRegionAvail().x, 0))) {
            appendPredatorEvent("manual", gui::freqSelect.frequency, "Manual Event", gui::waterfall.selectedVFOSNR, true);
        }
        if (missionPeerActive) ImGui::EndDisabled();
    };

    // Handle auto-start
    if (autostart) {
        autostart = false;
        setPlayState(true);
    }

    if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        showCredits = false;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        showCredits = false;
    }

    ImVec2 winSize = ImGui::GetWindowSize();
    float pad = 8.0f * style::uiScale;
    float statusBarHeight = 42.0f * style::uiScale;
    float controlBarHeight = 46.0f * style::uiScale;
    float railWidth = 64.0f * style::uiScale;
    float contentTop = pad + statusBarHeight + pad + controlBarHeight + pad;
    float contentHeight = std::max<float>(winSize.y - contentTop - pad, 120.0f * style::uiScale);
    float waterfallWidth = std::max<float>(winSize.x - railWidth - (3.0f * pad), 120.0f * style::uiScale);
    float railX = pad + waterfallWidth + pad;
    float overlayMinWidth = std::min<float>(320.0f * style::uiScale, waterfallWidth);
    float overlayMaxWidth = std::max<float>(overlayMinWidth, waterfallWidth - (28.0f * style::uiScale));
#ifdef __ANDROID__
    float overlayPreferredWidth = waterfallWidth * 0.78f;
#else
    float overlayPreferredWidth = (float)menuWidth;
#endif
    float overlayWidth = std::clamp<float>(overlayPreferredWidth, overlayMinWidth, overlayMaxWidth);
    float overlayX = pad + waterfallWidth - overlayWidth;

    ImGui::SetCursorPos(ImVec2(pad, pad));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.09f, 0.11f, 0.09f, 0.96f));
    ImGui::BeginChild("PredatorMissionStatus", ImVec2(winSize.x - (2.0f * pad), statusBarHeight), true);

    ImVec2 btnSize(30 * style::uiScale, 30 * style::uiScale);
    ImGui::PushID(ImGui::GetID("sdrpp_menu_btn"));
    if (ImGui::ImageButton(icons::MENU, btnSize, ImVec2(0, 0), ImVec2(1, 1), 5, ImVec4(0, 0, 0, 0), textCol) || ImGui::IsKeyPressed(ImGuiKey_Menu, false)) {
        showMenu = !showMenu;
        savePredatorState();
    }
    ImGui::PopID();

    ImGui::SameLine();
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (2.0f * style::uiScale));
    ImGui::TextUnformatted("Predator RF");

    ImGui::SameLine();
    const char* liveStateLabel = playing ? T("LIVE") : (sourceName.empty() ? T("NOT READY") : T("READY"));
    ImVec4 liveStateColor = playing ? ImVec4(0.42f, 0.78f, 0.48f, 1.0f) : (sourceName.empty() ? ImVec4(0.83f, 0.42f, 0.32f, 1.0f) : ImVec4(0.64f, 0.71f, 0.41f, 1.0f));
    if (drawBadge(liveStateLabel, liveStateColor) && !(playButtonLocked && !playing)) {
        if (sourceName.empty() && !playing) {
            predatorTab = PREDATOR_TAB_SYSTEM;
            showMenu = true;
            savePredatorState();
        }
        else {
            setPlayState(!playing);
        }
    }

    ImGui::SameLine();
    if (drawBadge(sourceName.empty() ? T("Select SDR") : sourceName.c_str(), ImVec4(0.73f, 0.70f, 0.45f, 1.0f))) {
        predatorTab = PREDATOR_TAB_SYSTEM;
        showMenu = true;
        savePredatorState();
    }

    ImGui::SameLine();
    ImGui::SetNextItemWidth(150.0f * style::uiScale);
    if (ImGui::Combo("##predator_mission_mode", &predatorMissionMode, "Manual\0Classify\0Scan\0QuickScan\0")) {
        savePredatorState();
    }

    ImGui::SameLine();
    if (drawBadge(phoneHasFix ? T("GPS READY") : T("GPS WAIT"), phoneHasFix ? ImVec4(0.55f, 0.74f, 0.46f, 1.0f) : ImVec4(0.45f, 0.49f, 0.41f, 1.0f))) {
        predatorTab = PREDATOR_TAB_MAP;
        showMenu = true;
        savePredatorState();
    }

    ImGui::SetCursorPosX(ImGui::GetWindowSize().x - (44.0f * style::uiScale));
    ImGui::SetCursorPosY((ImGui::GetWindowSize().y - (32.0f * style::uiScale)) * 0.5f);
    if (ImGui::ImageButton(icons::LOGO, ImVec2(32 * style::uiScale, 32 * style::uiScale), ImVec2(0, 0), ImVec2(1, 1), 0)) {
        showCredits = true;
    }

    ImGui::EndChild();
    ImGui::PopStyleColor();

    ImGui::SetCursorPos(ImVec2(pad, pad + statusBarHeight + pad));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.07f, 0.09f, 0.07f, 0.94f));
    ImGui::BeginChild("PredatorControlBar", ImVec2(winSize.x - (2.0f * pad), controlBarHeight), true);

    float origY = ImGui::GetCursorPosY();
    ImGui::SetCursorPosY(origY);
    gui::freqSelect.draw();

    ImGui::SameLine();
    ImGui::SetCursorPosY(origY);
    if (tuningMode == tuner::TUNER_MODE_CENTER) {
        ImGui::PushID(ImGui::GetID("sdrpp_ena_st_btn"));
        if (ImGui::ImageButton(icons::CENTER_TUNING, btnSize, ImVec2(0, 0), ImVec2(1, 1), 5, ImVec4(0, 0, 0, 0), textCol)) {
            tuningMode = tuner::TUNER_MODE_NORMAL;
            gui::waterfall.VFOMoveSingleClick = false;
            core::configManager.acquire();
            core::configManager.conf["centerTuning"] = false;
            core::configManager.release(true);
        }
        ImGui::PopID();
    }
    else {
        ImGui::PushID(ImGui::GetID("sdrpp_dis_st_btn"));
        if (ImGui::ImageButton(icons::NORMAL_TUNING, btnSize, ImVec2(0, 0), ImVec2(1, 1), 5, ImVec4(0, 0, 0, 0), textCol)) {
            tuningMode = tuner::TUNER_MODE_CENTER;
            gui::waterfall.VFOMoveSingleClick = true;
            tuner::tune(tuner::TUNER_MODE_CENTER, gui::waterfall.selectedVFO, gui::freqSelect.frequency);
            core::configManager.acquire();
            core::configManager.conf["centerTuning"] = true;
            core::configManager.release(true);
        }
        ImGui::PopID();
    }
    ImGui::SameLine();
    ImGui::SetCursorPosY(origY + (5.0f * style::uiScale));
    ImGui::TextDisabled("Select a right-side tab to overlay controls on the spectrum.");

    ImGui::EndChild();
    ImGui::PopStyleColor();

    // Overlay rect for the Zoom/Max/Min dropdown that lives in the
    // top-right of the waterfall. Computed BEFORE the waterfall is
    // drawn so we can lock waterfall input under our overlay (the
    // waterfall uses raw IsMouseClicked / IsMouseHoveringRect for
    // tuning and would otherwise also fire when the operator taps
    // our toggle button or the slider panel).
    const float wfOvPad   = 8.0f  * style::uiScale;
    const float wfOvBtnW  = 60.0f * style::uiScale;
    const float wfOvBtnH  = 30.0f * style::uiScale;
    // Never exceed ~25 % of the waterfall width, regardless of how
    // narrow the spectrum gets, so the bulk of the FFT remains
    // visible while the operator drags a slider. The 180 * uiScale
    // cap stops the panel from looking absurdly wide on big screens.
    const float wfOvPanelW = std::min(180.0f * style::uiScale,
                                      (float)waterfallWidth * 0.24f);
    const float wfOvPanelH = 200.0f * style::uiScale;
    ImGui::SetCursorPos(ImVec2(pad, contentTop));
    ImVec2 wfChildScreen = ImGui::GetCursorScreenPos();
    float wfBtnX = wfChildScreen.x + (float)waterfallWidth - wfOvBtnW - wfOvPad;
    float wfBtnY = wfChildScreen.y + wfOvPad;
    ImVec2 wfOvBtnMin(wfBtnX, wfBtnY);
    ImVec2 wfOvBtnMax(wfBtnX + wfOvBtnW, wfBtnY + wfOvBtnH);
    float wfPanelX = wfChildScreen.x + (float)waterfallWidth - wfOvPanelW - wfOvPad;
    float wfPanelY = wfBtnY + wfOvBtnH + (4.0f * style::uiScale);
    ImVec2 wfOvPanelMin(wfPanelX, wfPanelY);
    ImVec2 wfOvPanelMax(wfPanelX + wfOvPanelW, wfPanelY + wfOvPanelH);
    bool mouseOverWfBtn   = ImGui::IsMouseHoveringRect(wfOvBtnMin, wfOvBtnMax, false);
    bool mouseOverWfPanel = predatorWfControlsOpen
                            && ImGui::IsMouseHoveringRect(wfOvPanelMin, wfOvPanelMax, false);

    lockWaterfallControls = showMenu || mouseOverWfBtn || mouseOverWfPanel;

    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.04f, 0.05f, 0.04f, 0.98f));
    ImGui::BeginChild("Waterfall", ImVec2(waterfallWidth, contentHeight), true);

    // Kujhad: if mirroring an active peer's spectrum, retune the waterfall
    // to the peer's center/bandwidth, cache the latest peer frame for the
    // FFT worker thread to substitute into every local push (making the
    // source switch *exclusive* — local SDR rows are overwritten in
    // releaseFFTBuffer so the displayed waterfall is deterministic peer
    // data only). When the local SDR is not running and no FFT pushes
    // arrive, this UI path also pushes a single frame on each new peer
    // serial so the waterfall still scrolls. Only one peer is mirrored
    // at a time (the "Take control" target).
    std::string kujhadMirrorPeerName;
    bool kujhadMirrorBannerActive = false;
    if (predatorRole == PREDATOR_ROLE_CONTROLLER && kujhadMirrorPeerSpectrum
        && kujhadActivePeerIdx >= 0 && kujhadActivePeerIdx < (int)kujhadClients.size()
        && kujhadClients[kujhadActivePeerIdx]
        && kujhadActivePeerIdx < (int)kujhadActivePeers.size()) {
        auto& client = kujhadClients[kujhadActivePeerIdx];
        predator::KujhadSpectrumFrame frame;
        if (client->latestSpectrum(frame) && !frame.bins.empty() && frame.bandwidth > 0.0) {
            // First-frame capture: snapshot the operator's current local
            // waterfall view BEFORE we retune to the peer's center/BW,
            // so toggling mirror off can restore exactly that state.
            if (!kujhadLocalViewSaved) {
                kujhadLocalSavedCenter = gui::waterfall.getCenterFrequency();
                kujhadLocalSavedBW     = gui::waterfall.getBandwidth();
                kujhadLocalSavedViewBW = gui::waterfall.getViewBandwidth();
                kujhadLocalViewSaved   = true;
            }
            // Always retune the waterfall to the peer's view so the axis
            // labels match what we are now displaying. Cheap idempotent ops.
            if (gui::waterfall.getCenterFrequency() != frame.centerFreq) {
                gui::waterfall.setCenterFrequency(frame.centerFreq);
            }
            if (gui::waterfall.getBandwidth() != frame.bandwidth) {
                gui::waterfall.setBandwidth(frame.bandwidth);
                gui::waterfall.setViewBandwidth(frame.bandwidth);
            }
            // Publish the peer frame to the FFT worker. The worker reads
            // this on every releaseFFTBuffer to substitute over the local
            // row before pushFFT(), so peer data is the ONLY thing the
            // user sees while the mirror is on.
            //
            // The overlay arrays (peer hits + bands + targets + excludes)
            // are cached under the same lock so the marker-drawing block
            // below picks up a consistent set: bins and overlays from
            // the same frame, never split across two updates.
            {
                std::lock_guard<std::mutex> lk(kujhadSpectrumMtx);
                kujhadPeerCachedBins        = frame.bins;
                kujhadPeerCachedSerial      = frame.serial;
                kujhadPeerCachedHits        = frame.hits;
                kujhadPeerCachedSearchBands = frame.searchBands;
                kujhadPeerCachedTargets     = frame.targets;
                kujhadPeerCachedExcludes    = frame.excludes;
            }
            kujhadMirrorActive.store(true, std::memory_order_release);

            // Fallback: if the local SDR isn't producing FFT pushes (e.g.
            // operator stopped the source) the waterfall would freeze on
            // FFT-thread-driven substitution alone. So on each new peer
            // serial, also do one direct push from the UI thread. Safe
            // concurrently with FFT-thread pushes — buf_mtx is recursive
            // and either writer ends up writing peer data to the row.
            if (frame.serial != kujhadLastPeerSpectrumSerial) {
                kujhadLastPeerSpectrumSerial = frame.serial;
                int dst = gui::waterfall.getRawFFTSize();
                float* buf = gui::waterfall.getFFTBuffer();
                if (buf && dst > 0) {
                    int src = (int)frame.bins.size();
                    for (int i = 0; i < dst; i++) {
                        int j = (int)((double)i * (double)src / (double)dst);
                        if (j < 0) j = 0;
                        if (j >= src) j = src - 1;
                        buf[i] = frame.bins[j];
                    }
                }
                gui::waterfall.pushFFT();
            }

            kujhadMirrorBannerActive = true;
            kujhadMirrorPeerName = readJsonString(kujhadActivePeers[kujhadActivePeerIdx], "name", "peer");
            kujhadMirroredFromPeerIdx = kujhadActivePeerIdx;
        }
    }

    gui::waterfall.draw();

    // PEER overlay banner: drawn over the FFT area while a mirror is live
    // so the operator can never confuse a peer's view with their own.
    if (kujhadMirrorBannerActive) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 fMin = gui::waterfall.fftAreaMin;
        ImVec2 fMax = gui::waterfall.fftAreaMax;
        if (fMax.x > fMin.x && fMax.y > fMin.y) {
            char banner[160];
            std::snprintf(banner, sizeof(banner), "PEER: %s", kujhadMirrorPeerName.c_str());
            ImVec2 tsz = ImGui::CalcTextSize(banner);
            float padX = 10.0f * style::uiScale;
            float padY = 5.0f  * style::uiScale;
            float bx = fMin.x + 12.0f * style::uiScale;
            float by = fMin.y + 8.0f  * style::uiScale;
            dl->AddRectFilled(ImVec2(bx - padX, by - padY),
                              ImVec2(bx + tsz.x + padX, by + tsz.y + padY),
                              IM_COL32(180, 50, 30, 220),
                              4.0f * style::uiScale);
            dl->AddRect(ImVec2(bx - padX, by - padY),
                        ImVec2(bx + tsz.x + padX, by + tsz.y + padY),
                        IM_COL32(255, 220, 200, 230),
                        4.0f * style::uiScale, 0, 1.5f * style::uiScale);
            dl->AddText(ImVec2(bx, by), IM_COL32(255, 255, 255, 255), banner);
        }
    }

    // Draw hit / target / search-band overlays on the live spectrum.
    //
    // Two source modes share the same render path:
    //   * Local: pulls from `hits` / `searchBands` / `targets` /
    //     `excludes` and uses the original yellow/green palette.
    //   * Peer (mirror active): pulls from the cached peer overlay
    //     arrays attached to the most recent /v1/spectrum frame and
    //     paints in a peer cyan palette with dashed vertical lines so
    //     the operator can never confuse a peer's marker with their
    //     own. Hits/targets/bands are tagged with a "P" prefix on
    //     labels so an unlabelled local marker can't be mistaken for a
    //     peer marker (and vice versa).
    {
        bool peerMirror = kujhadMirrorBannerActive;
        json overlayHits, overlayBands, overlayTargets, overlayExcludes;
        if (peerMirror) {
            std::lock_guard<std::mutex> lk(kujhadSpectrumMtx);
            overlayHits     = kujhadPeerCachedHits;
            overlayBands    = kujhadPeerCachedSearchBands;
            overlayTargets  = kujhadPeerCachedTargets;
            overlayExcludes = kujhadPeerCachedExcludes;
        } else {
            overlayHits     = hits;
            overlayBands    = searchBands;
            overlayTargets  = targets;
            overlayExcludes = excludes;
        }
        bool haveAny = (overlayHits.is_array()     && !overlayHits.empty()) ||
                       (overlayBands.is_array()    && !overlayBands.empty()) ||
                       (overlayTargets.is_array()  && !overlayTargets.empty()) ||
                       (overlayExcludes.is_array() && !overlayExcludes.empty());
        double viewBW  = gui::waterfall.getViewBandwidth();
        double viewOff = gui::waterfall.getViewOffset();
        double cFreq   = gui::waterfall.getCenterFrequency();
        double lowF    = cFreq + viewOff - viewBW * 0.5;
        double highF   = cFreq + viewOff + viewBW * 0.5;
        ImVec2 fMin    = gui::waterfall.fftAreaMin;
        ImVec2 fMax    = gui::waterfall.fftAreaMax;
        ImVec2 wMin    = gui::waterfall.wfMin;
        ImVec2 wMax    = gui::waterfall.wfMax;
        float  fftW    = fMax.x - fMin.x;
        if (haveAny && fftW > 1.0f && highF > lowF) {
            ImDrawList* dl = ImGui::GetWindowDrawList();
            float labelY   = fMin.y + 4.0f * style::uiScale;
            float labelH   = ImGui::GetTextLineHeight();
            // Peer overlays use a cyan palette with dashed vertical
            // lines; local overlays keep the original solid yellow /
            // green palette so an operator's eyes can tell at a glance
            // whether a marker is theirs or a peer's.
            ImU32 hitCol     = peerMirror ? IM_COL32( 80, 220, 255, 230) : IM_COL32(255, 200,  40, 220);
            ImU32 hitWfCol   = peerMirror ? IM_COL32( 80, 220, 255,  80) : IM_COL32(255, 200,  40,  80);
            ImU32 targetCol  = peerMirror ? IM_COL32(120, 255, 220, 230) : IM_COL32( 80, 255, 120, 220);
            ImU32 targetWfCol= peerMirror ? IM_COL32(120, 255, 220,  80) : IM_COL32( 80, 255, 120,  80);
            ImU32 bandCol    = peerMirror ? IM_COL32( 80, 220, 255,  28) : IM_COL32( 80, 200, 255,  18);
            ImU32 bandEdgeCol= peerMirror ? IM_COL32( 80, 220, 255, 120) : IM_COL32( 80, 200, 255,  90);
            // Exclude bands: faint red wash so the operator can see
            // "do-not-scan" zones at a glance. Same peer/local hue
            // discipline as the rest of the overlays.
            ImU32 excludeCol     = peerMirror ? IM_COL32(255, 120, 160,  32) : IM_COL32(255, 100, 100,  22);
            ImU32 excludeEdgeCol = peerMirror ? IM_COL32(255, 120, 160, 130) : IM_COL32(255, 100, 100, 100);
            const char* labelPrefix = peerMirror ? "P" : "";
            // Helper: dashed vertical line. ImGui has no native dashed
            // primitive so we tile fixed-length on/off segments. dashLen
            // / gapLen are in pixels (post-uiScale) so the cadence
            // looks consistent across DPI.
            auto dashedVLine = [&](float x, float y0, float y1, ImU32 col, float thickness) {
                if (y1 <= y0) return;
                float dashLen = 6.0f * style::uiScale;
                float gapLen  = 4.0f * style::uiScale;
                for (float y = y0; y < y1; y += dashLen + gapLen) {
                    float ye = y + dashLen;
                    if (ye > y1) ye = y1;
                    dl->AddLine(ImVec2(x, y), ImVec2(x, ye), col, thickness);
                }
            };
            auto vLine = [&](float x, float y0, float y1, ImU32 col, float thickness) {
                if (peerMirror) dashedVLine(x, y0, y1, col, thickness);
                else            dl->AddLine(ImVec2(x, y0), ImVec2(x, y1), col, thickness);
            };
            // 1) Search-band shading. Draw first so vertical markers
            //    sit on top of the band tint instead of being washed
            //    out by it. Only enabled bands paint.
            if (overlayBands.is_array()) {
                for (int bi = 0; bi < (int)overlayBands.size(); bi++) {
                    if (!readJsonBool(overlayBands[bi], "enabled", true)) continue;
                    double bs = readJsonDouble(overlayBands[bi], "start", 0.0);
                    double be = readJsonDouble(overlayBands[bi], "stop",  0.0);
                    if (bs <= 0.0 || be <= 0.0 || bs == be) continue;
                    if (bs > be) std::swap(bs, be);
                    if (be < lowF || bs > highF) continue;
                    double cs = std::max(bs, lowF);
                    double ce = std::min(be, highF);
                    float xs = fMin.x + (float)((cs - lowF) / (highF - lowF)) * fftW;
                    float xe = fMin.x + (float)((ce - lowF) / (highF - lowF)) * fftW;
                    if (xe < xs + 1.0f) xe = xs + 1.0f;
                    dl->AddRectFilled(ImVec2(xs, fMin.y), ImVec2(xe, fMax.y), bandCol);
                    // Edge ticks at band boundaries when they fall in view.
                    if (bs >= lowF) vLine(xs, fMin.y, fMax.y, bandEdgeCol, 1.0f * style::uiScale);
                    if (be <= highF) vLine(xe, fMin.y, fMax.y, bandEdgeCol, 1.0f * style::uiScale);
                }
            }
            // 1b) Exclude bands — paint after search bands so the
            //     red "do-not-scan" tint sits on top of any green
            //     band tint underneath, making the exclusion obvious.
            if (overlayExcludes.is_array()) {
                for (int ei = 0; ei < (int)overlayExcludes.size(); ei++) {
                    double ef = readJsonDouble(overlayExcludes[ei], "frequency", 0.0);
                    double ew = readJsonDouble(overlayExcludes[ei], "bandwidth", 0.0);
                    if (ef <= 0.0) continue;
                    // Default to a narrow 25 kHz half-width when no
                    // bandwidth is specified — same convention used
                    // by the local exclude editor.
                    if (ew <= 0.0) ew = 50000.0;
                    double es = ef - ew * 0.5;
                    double ee = ef + ew * 0.5;
                    if (ee < lowF || es > highF) continue;
                    double cs = std::max(es, lowF);
                    double ce = std::min(ee, highF);
                    float xs = fMin.x + (float)((cs - lowF) / (highF - lowF)) * fftW;
                    float xe = fMin.x + (float)((ce - lowF) / (highF - lowF)) * fftW;
                    if (xe < xs + 1.0f) xe = xs + 1.0f;
                    dl->AddRectFilled(ImVec2(xs, fMin.y), ImVec2(xe, fMax.y), excludeCol);
                    if (es >= lowF) vLine(xs, fMin.y, fMax.y, excludeEdgeCol, 1.0f * style::uiScale);
                    if (ee <= highF) vLine(xe, fMin.y, fMax.y, excludeEdgeCol, 1.0f * style::uiScale);
                }
            }
            // 2) Target ticks — short marks at the top of the FFT area
            //    so the operator can see configured target slots even
            //    before they produce a hit. Drawn before hits so a
            //    target-promoted hit's full-height line lands on top.
            if (overlayTargets.is_array()) {
                float tickH = 10.0f * style::uiScale;
                for (int ti = 0; ti < (int)overlayTargets.size(); ti++) {
                    double tf = readJsonDouble(overlayTargets[ti], "frequency", 0.0);
                    if (tf <= 0.0 || tf < lowF || tf > highF) continue;
                    if (overlayTargets[ti].contains("enabled")
                        && !readJsonBool(overlayTargets[ti], "enabled", true)) continue;
                    float x = fMin.x + (float)((tf - lowF) / (highF - lowF)) * fftW;
                    vLine(x, fMin.y, fMin.y + tickH, targetCol, 1.0f * style::uiScale);
                }
            }
            // 3) Hits — full-height labelled vertical markers.
            if (overlayHits.is_array()) {
                for (int hi = 0; hi < (int)overlayHits.size(); hi++) {
                    double hf = readJsonDouble(overlayHits[hi], "frequency", 0.0);
                    if (hf <= 0.0 || hf < lowF || hf > highF) continue;
                    float x = fMin.x + (float)((hf - lowF) / (highF - lowF)) * fftW;
                    std::string state = readJsonString(overlayHits[hi], "state", "unknown");
                    bool isTarget = (state == "target");
                    ImU32 lineCol = isTarget ? targetCol   : hitCol;
                    ImU32 wfCol   = isTarget ? targetWfCol : hitWfCol;
                    vLine(x, fMin.y, fMax.y, lineCol, 1.5f * style::uiScale);
                    if (wMax.y > wMin.y) {
                        vLine(x, wMin.y, wMax.y, wfCol, 1.0f * style::uiScale);
                    }
                    char label[40];
                    bool assigned = readJsonBool(overlayHits[hi], "markerAssigned", false);
                    if (assigned) {
                        int slot = (int)readJsonDouble(overlayHits[hi], "markerSlot", 0.0);
                        snprintf(label, sizeof(label), "%sM%d", labelPrefix, slot > 0 ? slot : (hi + 1));
                    } else {
                        double mhz = hf / 1e6;
                        if (mhz >= 1000.0) snprintf(label, sizeof(label), "%s%.2f GHz", labelPrefix, mhz / 1000.0);
                        else if (mhz >= 1.0)  snprintf(label, sizeof(label), "%s%.3f MHz", labelPrefix, mhz);
                        else                  snprintf(label, sizeof(label), "%s%.1f kHz", labelPrefix, mhz * 1000.0);
                    }
                    ImVec2 tsz = ImGui::CalcTextSize(label);
                    float tx = x - tsz.x * 0.5f;
                    if (tx < fMin.x) tx = fMin.x;
                    if (tx + tsz.x > fMax.x) tx = fMax.x - tsz.x;
                    dl->AddRectFilled(ImVec2(tx - 2, labelY - 1), ImVec2(tx + tsz.x + 2, labelY + labelH + 1), IM_COL32(0, 0, 0, 160));
                    dl->AddText(ImVec2(tx, labelY), lineCol, label);
                }
            }
        }
    }

    // ----- Top-right Zoom/Max/Min dropdown -----
    // Toggle button is anchored to the top-right of the waterfall child.
    // The slider panel opens just below it. Slider values write through
    // the same configManager keys ("min", "max") as the legacy rail
    // controls so they survive restart.
    {
        const float btnLocalX = (float)waterfallWidth - wfOvBtnW - wfOvPad
                                - ImGui::GetStyle().WindowPadding.x;
        const float btnLocalY = wfOvPad - ImGui::GetStyle().WindowPadding.y;
        ImGui::SetCursorPos(ImVec2(btnLocalX, btnLocalY));
        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.10f, 0.13f, 0.10f, 0.85f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.18f, 0.22f, 0.16f, 0.95f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.22f, 0.28f, 0.20f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Border,        ImVec4(0.30f, 0.45f, 0.20f, 0.85f));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
        ImGui::PushID("predator_wfctl_toggle");
        if (ImGui::Button(predatorWfControlsOpen ? "View*" : "View",
                          ImVec2(wfOvBtnW, wfOvBtnH))) {
            predatorWfControlsOpen = !predatorWfControlsOpen;
            savePredatorState();
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Zoom / Max / Min");
        }
        ImGui::PopID();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(4);

        if (predatorWfControlsOpen) {
            const float panelLocalX = (float)waterfallWidth - wfOvPanelW - wfOvPad
                                      - ImGui::GetStyle().WindowPadding.x;
            const float panelLocalY = btnLocalY + wfOvBtnH + (4.0f * style::uiScale);
            ImGui::SetCursorPos(ImVec2(panelLocalX, panelLocalY));
            ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.07f, 0.10f, 0.07f, 0.96f));
            ImGui::PushStyleColor(ImGuiCol_Border,  ImVec4(0.30f, 0.45f, 0.20f, 0.85f));
            ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1.0f);
            ImGui::BeginChild("PredatorWfControls", ImVec2(wfOvPanelW, wfOvPanelH), true);

            ImVec2 wfSliderSize(18.0f * style::uiScale, 120.0f * style::uiScale);
            float colW = ImGui::GetContentRegionAvail().x / 3.0f;
            float colY = ImGui::GetCursorPosY();

            // Zoom column
            ImGui::SetCursorPos(ImVec2((colW - ImGui::CalcTextSize("Zoom").x) * 0.5f, colY));
            ImGui::TextUnformatted("Zoom");
            ImGui::SetCursorPosX((colW - wfSliderSize.x) * 0.5f);
            if (ImGui::VSliderFloat("##wfctl_zoom", wfSliderSize, &bw, 1.0, 0.0, "")) {
                double factor = (double)bw * (double)bw;
                double wfBw = gui::waterfall.getBandwidth();
                double delta = wfBw - 1000.0;
                double finalBw = std::min<double>(1000.0 + (factor * delta), wfBw);
                gui::waterfall.setViewBandwidth(finalBw);
                if (vfo != NULL) {
                    gui::waterfall.setViewOffset(vfo->centerOffset);
                }
            }

            // Max column
            ImGui::SetCursorPos(ImVec2(colW + (colW - ImGui::CalcTextSize("Max").x) * 0.5f, colY));
            ImGui::TextUnformatted("Max");
            ImGui::SetCursorPosX(colW + (colW - wfSliderSize.x) * 0.5f);
            if (ImGui::VSliderFloat("##wfctl_max", wfSliderSize, &fftMax, 0.0, -160.0f, "")) {
                fftMax = std::max<float>(fftMax, fftMin + 10);
                core::configManager.acquire();
                core::configManager.conf["max"] = fftMax;
                core::configManager.release(true);
            }

            // Min column
            ImGui::SetCursorPos(ImVec2(2.0f * colW + (colW - ImGui::CalcTextSize("Min").x) * 0.5f, colY));
            ImGui::TextUnformatted("Min");
            ImGui::SetCursorPosX(2.0f * colW + (colW - wfSliderSize.x) * 0.5f);
            ImGui::SetItemUsingMouseWheel();
            if (ImGui::VSliderFloat("##wfctl_min", wfSliderSize, &fftMin, 0.0, -160.0f, "")) {
                fftMin = std::min<float>(fftMax - 10, fftMin);
                core::configManager.acquire();
                core::configManager.conf["min"] = fftMin;
                core::configManager.release(true);
            }

            ImGui::EndChild();
            ImGui::PopStyleVar();
            ImGui::PopStyleColor(2);

            // Outside-tap close: tap landed neither in the panel rect
            // nor in the toggle button rect. Use IsMouseHoveringRect
            // with clip=false so child clipping doesn't filter the test.
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)
                && !ImGui::IsMouseHoveringRect(wfOvPanelMin, wfOvPanelMax, false)
                && !ImGui::IsMouseHoveringRect(wfOvBtnMin,   wfOvBtnMax,   false)) {
                predatorWfControlsOpen = false;
                savePredatorState();
            }
        }
    }

    ImGui::EndChild();
    ImGui::PopStyleColor();

    if (showMenu) {
        ImGui::SetCursorPos(ImVec2(overlayX, contentTop));
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.08f, 0.10f, 0.08f, 0.97f));
        ImGui::BeginChild("PredatorOverlay", ImVec2(overlayWidth, contentHeight), true);
        ImGui::TextUnformatted(tabTitles[predatorTab]);
        ImGui::SameLine();
        ImGui::SetCursorPosX(ImGui::GetWindowWidth() - (38.0f * style::uiScale));
        if (ImGui::Button("X", ImVec2(26.0f * style::uiScale, 26.0f * style::uiScale))) {
            showMenu = false;
            savePredatorState();
        }
        ImGui::TextWrapped("%s", tabDescriptions[predatorTab]);
        ImGui::Separator();
        ImGui::BeginChild("PredatorOverlayBody", ImVec2(0, 0), false);

        if (predatorTab == PREDATOR_TAB_SPECTRUM) {
            if (ImGui::CollapsingHeader(T("Display Controls"), ImGuiTreeNodeFlags_DefaultOpen)) {
                displaymenu::draw(NULL);
            }
            if (ImGui::CollapsingHeader(T("Band Plan"), ImGuiTreeNodeFlags_DefaultOpen)) {
                bandplanmenu::draw(NULL);
            }
            if (ImGui::CollapsingHeader(T("Current Mission Mode"), ImGuiTreeNodeFlags_DefaultOpen)) {
                ImGui::TextUnformatted(missionModes[predatorMissionMode]);
                ImGui::TextWrapped("%s", missionModeDescriptions[predatorMissionMode]);
            }
            if (ImGui::CollapsingHeader(T("Mission Run"), ImGuiTreeNodeFlags_DefaultOpen)) {
                drawMissionRunControls();
            }
            if (ImGui::CollapsingHeader(T("Quick Actions"), ImGuiTreeNodeFlags_DefaultOpen)) {
                if (ImGui::Button(T("Open SDR / Settings"), ImVec2(ImGui::GetContentRegionAvail().x, 0))) {
                    predatorTab = PREDATOR_TAB_SYSTEM;
                    savePredatorState();
                }
                if (ImGui::Button(T("Open Mission Config"), ImVec2(ImGui::GetContentRegionAvail().x, 0))) {
                    predatorTab = PREDATOR_TAB_MISSION;
                    savePredatorState();
                }
            }
        }
        else if (predatorTab == PREDATOR_TAB_HITS) {
            if (ImGui::CollapsingHeader(T("Quick Filter"), ImGuiTreeNodeFlags_DefaultOpen)) {
                for (int i = 0; i < 4; i++) {
                    if (i > 0) { ImGui::SameLine(); }
                    bool active = (predatorQuickFilter == i);
                    if (active) {
                        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.28f, 0.39f, 0.21f, 1.0f));
                        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.32f, 0.45f, 0.24f, 1.0f));
                        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.35f, 0.50f, 0.27f, 1.0f));
                    }
                    if (ImGui::Button(quickFilterLabels[i])) {
                        predatorQuickFilter = i;
                        savePredatorState();
                    }
                    if (active) {
                        ImGui::PopStyleColor(3);
                    }
                }
                ImGui::TextWrapped("%s", chineseUi ? "\u4e8b\u4ef6\u8a18\u9304\u5df2\u9023\u63a5\u4efb\u52d9\u6383\u63cf\u8207\u624b\u52d5\u6a19\u8a18\u3002" : "Event logging is wired to mission scan targets and manual operator marks.");
                ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
                if (ImGui::Combo(T("Hit Sort"), &predatorHitSortMode, "Newest\0Strongest\0Most Hits\0Most Events\0Unread First\0Marked First\0")) {
                    predatorHitSortMode = std::clamp<int>(predatorHitSortMode, 0, 5);
                    savePredatorState();
                }
                ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
                if (ImGui::Combo(T("Event Filter"), &predatorEventFilter, "All Events\0Hits\0Manual\0Targets\0Decoder\0Current Hit\0")) {
                    predatorEventFilter = std::clamp<int>(predatorEventFilter, 0, 5);
                    savePredatorState();
                }
            }

            if (ImGui::CollapsingHeader(T("Marker Pool"), ImGuiTreeNodeFlags_DefaultOpen)) {
                int assigned = assignedMarkerCount();
                ImGui::Text("Assigned: %d / %d", assigned, predatorMarkerSlots);
                if (assigned == 0) {
                    ImGui::TextDisabled("%s", T("No entries."));
                }
                for (int i = 0; i < hits.size(); i++) {
                    if (!readJsonBool(hits[i], "markerAssigned", false)) { continue; }
                    double hitFrequency = readJsonDouble(hits[i], "frequency", 0.0);
                    ImGui::TextWrapped("M%d  %s  %s  %s",
                        i + 1,
                        formatFrequency(hitFrequency).c_str(),
                        readJsonString(hits[i], "decoder", "None").c_str(),
                        readJsonString(hits[i], "name", "Hit").c_str());
                    std::string routeVfo = readJsonString(hits[i], "routeVfo", "");
                    if (!routeVfo.empty()) {
                        ImGui::TextDisabled("Route: %s", routeVfo.c_str());
                    }
                }
            }

            if (ImGui::CollapsingHeader(T("Detected Hits"), ImGuiTreeNodeFlags_DefaultOpen)) {
                if (!hits.empty()) {
                    float bw = (ImGui::GetContentRegionAvail().x - 4.0f * style::uiScale) * 0.5f;
                    if (ImGui::Button(T("Clear Unknown Hits"), ImVec2(bw, 0))) {
                        for (int i = (int)hits.size() - 1; i >= 0; i--) {
                            if (readJsonString(hits[i], "state", "unknown") != "target") {
                                releaseHitRoute(i);
                                hits.erase(hits.begin() + i);
                            }
                        }
                        savePredatorHits(hits);
                    }
                    ImGui::SameLine(0, 4.0f * style::uiScale);
                    if (ImGui::Button(T("Clear All Hits"), ImVec2(bw, 0))) {
                        for (int i = (int)hits.size() - 1; i >= 0; i--) {
                            releaseHitRoute(i);
                        }
                        hits = json::array();
                        savePredatorHits(hits);
                    }
                    ImGui::Separator();
                }
                if (hits.empty()) {
                    ImGui::TextDisabled("%s", T("No entries."));
                }
                else {
                    bool detectedHitsChanged = false;
                    bool missionRowsChanged = false;
                    const char* decoderLabels[] = { "None", "Analog Voice", "DSD-FME", "M17", "RTL433", "Raw IQ/Data" };
                    const char* decoderCombo = "None\0Analog Voice\0DSD-FME\0M17\0RTL433\0Raw IQ/Data\0";
                    std::vector<int> hitOrder;
                    for (int i = 0; i < hits.size(); i++) {
                        hitOrder.push_back(i);
                    }
                    // Scrollable hit list — max ~5 entries visible at once
                    float hitListH = std::min<float>((float)hitOrder.size(), 5.0f) *
                                     (ImGui::GetTextLineHeightWithSpacing() * 4.5f + 6.0f * style::uiScale);
                    hitListH = std::max(hitListH, 120.0f * style::uiScale);
                    ImGui::BeginChild("##hit_list_scroll", ImVec2(0, hitListH), false, ImGuiWindowFlags_HorizontalScrollbar);
                    std::sort(hitOrder.begin(), hitOrder.end(), [&](int a, int b) {
                        switch (predatorHitSortMode) {
                        case 1:
                            return readJsonDouble(hits[a], "maxRssi", -999.0) > readJsonDouble(hits[b], "maxRssi", -999.0);
                        case 2:
                            return readJsonDouble(hits[a], "hitCount", 0.0) > readJsonDouble(hits[b], "hitCount", 0.0);
                        case 3:
                            return readJsonDouble(hits[a], "eventCount", 0.0) > readJsonDouble(hits[b], "eventCount", 0.0);
                        case 4:
                            return readJsonDouble(hits[a], "unreadCount", 0.0) > readJsonDouble(hits[b], "unreadCount", 0.0);
                        case 5:
                            return readJsonBool(hits[a], "markerAssigned", false) > readJsonBool(hits[b], "markerAssigned", false);
                        default:
                            return readJsonDouble(hits[a], "lastEventClock", 0.0) > readJsonDouble(hits[b], "lastEventClock", 0.0);
                        }
                    });
                    for (int orderIndex = 0; orderIndex < hitOrder.size(); orderIndex++) {
                        int i = hitOrder[orderIndex];
                        double hitFrequency = readJsonDouble(hits[i], "frequency", 0.0);
                        std::string state = hitStateForFrequency(hitFrequency, hits[i]);
                        if ((predatorQuickFilter == 1 && state != "target") ||
                            (predatorQuickFilter == 2 && state != "exclude") ||
                            (predatorQuickFilter == 3 && state != "unknown")) {
                            continue;
                        }

                        ImGui::PushID(7000 + i);
                        std::string name = readJsonString(hits[i], "name", "Hit");
                        int hitCount = (int)readJsonDouble(hits[i], "hitCount", 0.0);
                        int eventCount = (int)readJsonDouble(hits[i], "eventCount", 0.0);
                        int unreadCount = (int)readJsonDouble(hits[i], "unreadCount", 0.0);
                        double lastRssi = readJsonDouble(hits[i], "lastRssi", 0.0);
                        double maxRssi  = readJsonDouble(hits[i], "maxRssi",  -120.0);
                        double snrDb = readJsonDouble(hits[i], "snrDb", 0.0);
                        bool markerAssigned = readJsonBool(hits[i], "markerAssigned", false);
                        bool isSelected = (predatorSelectedHitFrequency > 0.0 &&
                            std::abs(hitFrequency - predatorSelectedHitFrequency) <=
                            std::max<double>(readJsonDouble(hits[i], "clusterHz", hitClusterHz), 100.0));
                        std::string decoder = readJsonString(hits[i], "decoder", "None");
                        int decoderIndex = 0;
                        for (int d = 0; d < 6; d++) {
                            if (decoder == decoderLabels[d]) { decoderIndex = d; break; }
                        }

                        // ── Row background highlight ───────────────────────
                        // Unread → warm amber, Selected → subtle blue tint
                        if (isSelected) {
                            ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.15f, 0.22f, 0.35f, 1.0f));
                        } else if (unreadCount > 0) {
                            ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.28f, 0.22f, 0.06f, 1.0f));
                        }

                        // ── State color dot ────────────────────────────────
                        ImU32 stateDotCol;
                        if      (state == "target")  { stateDotCol = IM_COL32(50, 210, 90,  255); }
                        else if (state == "exclude") { stateDotCol = IM_COL32(220, 60, 60,  255); }
                        else                         { stateDotCol = IM_COL32(180, 180, 60, 255); }

                        ImVec2 dotPos = ImVec2(ImGui::GetCursorScreenPos().x + 4.0f * style::uiScale,
                                               ImGui::GetCursorScreenPos().y + ImGui::GetTextLineHeight() * 0.5f);
                        ImGui::GetWindowDrawList()->AddCircleFilled(dotPos, 5.0f * style::uiScale, stateDotCol);
                        ImGui::Dummy(ImVec2(14.0f * style::uiScale, 0.0f));
                        ImGui::SameLine();

                        // ── Primary row: freq + SNR + marker badge ─────────
                        {
                            char badge[32] = "";
                            if (markerAssigned) snprintf(badge, sizeof(badge), " [M]");
                            if (unreadCount > 0) {
                                ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.2f, 1.0f), "%s  SNR %.0f dB  hits:%d%s",
                                    formatFrequency(hitFrequency).c_str(), snrDb, hitCount, badge);
                            } else {
                                ImGui::Text("%s  SNR %.0f dB  hits:%d%s",
                                    formatFrequency(hitFrequency).c_str(), snrDb, hitCount, badge);
                            }
                        }

                        // ── Secondary row: name + state + last-seen + unread badge ──
                        ImGui::Dummy(ImVec2(14.0f * style::uiScale, 0.0f));
                        ImGui::SameLine();
                        {
                            char unreadBuf[24] = "";
                            if (unreadCount > 0) snprintf(unreadBuf, sizeof(unreadBuf), "  (%d unread)", unreadCount);
                            ImGui::TextDisabled("%s  [%s]  RSSI %.0f  last: %s%s",
                                name.c_str(), state.c_str(), lastRssi,
                                readJsonString(hits[i], "lastSeen", "?").c_str(), unreadBuf);
                        }

                        // ── RSSI bar ───────────────────────────────────────
                        {
                            float barW = ImGui::GetContentRegionAvail().x;
                            float barH = 3.0f * style::uiScale;
                            ImVec2 barTL = ImGui::GetCursorScreenPos();
                            ImVec2 barBR = ImVec2(barTL.x + barW, barTL.y + barH);
                            ImGui::GetWindowDrawList()->AddRectFilled(barTL, barBR, IM_COL32(50, 50, 50, 200), 1.0f);
                            double clampedRssi = std::clamp(maxRssi, -120.0, -20.0);
                            float fill = (float)((clampedRssi + 120.0) / 100.0);
                            ImU32 barFill = (fill > 0.65f) ? IM_COL32(220, 70, 50, 220)
                                          : (fill > 0.35f) ? IM_COL32(220, 180, 40, 220)
                                          :                  IM_COL32(50, 200, 90, 220);
                            ImGui::GetWindowDrawList()->AddRectFilled(barTL, ImVec2(barTL.x + barW * fill, barBR.y), barFill, 1.0f);
                            ImGui::Dummy(ImVec2(barW, barH + 2.0f * style::uiScale));
                        }

                        if (isSelected || unreadCount > 0) { ImGui::PopStyleColor(); }

                        if (ImGui::Button(T("Select"))) {
                            predatorSelectedHitFrequency = hitFrequency;
                        }
                        ImGui::SameLine();
                        if (ImGui::Button(markerAssigned ? T("Release Marker") : T("Assign Marker"))) {
                            if (markerAssigned || assignedMarkerCount() < predatorMarkerSlots) {
                                bool nextAssigned = !markerAssigned;
                                hits[i]["markerAssigned"] = nextAssigned;
                                if (nextAssigned) {
                                    routeHitToVfo(i);
                                }
                                else {
                                    releaseHitRoute(i);
                                }
                            }
                            detectedHitsChanged = true;
                        }
                        ImGui::SameLine();
                        ImGui::SetNextItemWidth(std::max<float>(140.0f * style::uiScale, ImGui::GetContentRegionAvail().x * 0.45f));
                        if (ImGui::Combo(T("Decoder"), &decoderIndex, decoderCombo)) {
                            hits[i]["decoder"] = decoderLabels[decoderIndex];
                            if (decoderIndex > 0) {
                                if (readJsonBool(hits[i], "markerAssigned", false) || assignedMarkerCount() < predatorMarkerSlots) {
                                    hits[i]["markerAssigned"] = true;
                                    routeHitToVfo(i);
                                }
                            }
                            detectedHitsChanged = true;
                        }

                        float halfWidth = (ImGui::GetContentRegionAvail().x - (6.0f * style::uiScale)) * 0.5f;
                        if (ImGui::Button(T("Promote Target"), ImVec2(halfWidth, 0))) {
                            if (!isKnownTargetFrequency(hitFrequency)) {
                                json row;
                                row["name"] = name;
                                row["frequency"] = hitFrequency;
                                row["bandwidth"] = readJsonDouble(hits[i], "bandwidth", 12500.0);
                                row["enabled"] = true;
                                row["source"] = "hit_promotion";
                                targets.push_back(row);
                            }
                            hits[i]["state"] = "target";
                            missionRowsChanged = true;
                            detectedHitsChanged = true;
                        }
                        ImGui::SameLine();
                        if (ImGui::Button(T("Promote Exclude"), ImVec2(halfWidth, 0))) {
                            if (!isExcludedFrequency(hitFrequency)) {
                                json row;
                                row["name"] = name;
                                row["frequency"] = hitFrequency;
                                row["bandwidth"] = readJsonDouble(hits[i], "bandwidth", 12500.0);
                                row["enabled"] = true;
                                row["source"] = "hit_promotion";
                                excludes.push_back(row);
                            }
                            hits[i]["state"] = "exclude";
                            missionRowsChanged = true;
                            detectedHitsChanged = true;
                        }
                        ImGui::Separator();
                        ImGui::PopID();
                    }
                    ImGui::EndChild();
                    if (detectedHitsChanged) {
                        savePredatorHits(hits);
                    }
                    if (missionRowsChanged) {
                        saveMissionConfig(searchBands, targets, excludes, missionThreshold, dwellMs, quickScanDelayMs, quickScanDurationMs, recordAudio);
                    }
                }
            }

            if (ImGui::CollapsingHeader(T("Selected Hit"), ImGuiTreeNodeFlags_DefaultOpen)) {
                int selectedIndex = -1;
                for (int i = 0; i < hits.size(); i++) {
                    double hitFrequency = readJsonDouble(hits[i], "frequency", 0.0);
                    double clusterWidth = std::max<double>(readJsonDouble(hits[i], "clusterHz", hitClusterHz), 100.0);
                    if (predatorSelectedHitFrequency > 0.0 && std::abs(hitFrequency - predatorSelectedHitFrequency) <= clusterWidth) {
                        selectedIndex = i;
                        break;
                    }
                }

                if (selectedIndex < 0) {
                    ImGui::TextDisabled("%s", T("No entries."));
                }
                else {
                    bool selectedChanged = false;
                    bool selectedMissionChanged = false;
                    json& hit = hits[selectedIndex];
                    double hitFrequency = readJsonDouble(hit, "frequency", 0.0);
                    std::string name = readJsonString(hit, "name", "Hit");
                    bool markerAssigned = readJsonBool(hit, "markerAssigned", false);
                    static double editHitFrequency = 0.0;
                    static char hitNameBuf[128] = "";
                    static char hitNoteBuf[512] = "";
                    if (std::abs(editHitFrequency - hitFrequency) > 1.0) {
                        snprintf(hitNameBuf, sizeof(hitNameBuf), "%s", name.c_str());
                        snprintf(hitNoteBuf, sizeof(hitNoteBuf), "%s", readJsonString(hit, "note", "").c_str());
                        editHitFrequency = hitFrequency;
                    }

                    // ── Selected hit header ────────────────────────────────
                    {
                        std::string selState = hitStateForFrequency(hitFrequency, hit);
                        ImVec4 selStatCol;
                        if      (selState == "target")  { selStatCol = ImVec4(0.20f, 0.85f, 0.35f, 1.0f); }
                        else if (selState == "exclude") { selStatCol = ImVec4(0.88f, 0.24f, 0.24f, 1.0f); }
                        else                            { selStatCol = ImVec4(0.75f, 0.75f, 0.24f, 1.0f); }

                        ImGui::TextColored(ImVec4(1.0f, 1.0f, 1.0f, 1.0f), "%s", formatFrequency(hitFrequency).c_str());
                        ImGui::SameLine();
                        ImGui::TextColored(selStatCol, "[%s]", selState.c_str());
                        ImGui::SameLine();
                        ImGui::TextDisabled("%s", name.c_str());

                        double selSnr    = readJsonDouble(hit, "snrDb",    0.0);
                        double selLastRssi = readJsonDouble(hit, "lastRssi", 0.0);
                        double selMaxRssi  = readJsonDouble(hit, "maxRssi",  -120.0);
                        int    selHits   = (int)readJsonDouble(hit, "hitCount",   0.0);
                        int    selEvents = (int)readJsonDouble(hit, "eventCount", 0.0);
                        int    selUnread = (int)readJsonDouble(hit, "unreadCount", 0.0);

                        ImGui::Text("SNR %.1f dB  RSSI %.0f / %.0f dB  hits:%d  events:%d",
                            selSnr, selLastRssi, selMaxRssi, selHits, selEvents);
                        if (selUnread > 0) {
                            ImGui::TextColored(ImVec4(1.0f, 0.80f, 0.15f, 1.0f), "%d unread", selUnread);
                            ImGui::SameLine();
                        }

                        // RSSI bar
                        float selBarW = ImGui::GetContentRegionAvail().x;
                        float selBarH = 4.0f * style::uiScale;
                        ImVec2 selBarTL = ImGui::GetCursorScreenPos();
                        ImVec2 selBarBR = ImVec2(selBarTL.x + selBarW, selBarTL.y + selBarH);
                        ImGui::GetWindowDrawList()->AddRectFilled(selBarTL, selBarBR, IM_COL32(50, 50, 50, 200), 1.0f);
                        float selFill = (float)(std::clamp(selMaxRssi, -120.0, -20.0) + 120.0) / 100.0f;
                        ImU32 selBarFill = (selFill > 0.65f) ? IM_COL32(220, 70, 50, 220)
                                         : (selFill > 0.35f) ? IM_COL32(220, 180, 40, 220)
                                         :                     IM_COL32(50, 200, 90, 220);
                        ImGui::GetWindowDrawList()->AddRectFilled(selBarTL, ImVec2(selBarTL.x + selBarW * selFill, selBarBR.y), selBarFill, 1.0f);
                        ImGui::Dummy(ImVec2(selBarW, selBarH + 3.0f * style::uiScale));

                        ImGui::Text("Decoder: %s   Route: %s",
                            readJsonString(hit, "decoder", "None").c_str(),
                            readJsonString(hit, "routeVfo", "—").c_str());
                    }

                    if (ImGui::CollapsingHeader(T("Rename / Notes"))) {
                        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
                        if (ImGui::InputText("Name##selected_hit_name", hitNameBuf, sizeof(hitNameBuf))) {
                            hit["name"] = std::string(hitNameBuf);
                            selectedChanged = true;
                        }
                        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
                        if (ImGui::InputTextMultiline("Note##selected_hit_note", hitNoteBuf, sizeof(hitNoteBuf), ImVec2(ImGui::GetContentRegionAvail().x, 88.0f * style::uiScale))) {
                            hit["note"] = std::string(hitNoteBuf);
                            selectedChanged = true;
                        }
                    }

                    float halfWidth = (ImGui::GetContentRegionAvail().x - (6.0f * style::uiScale)) * 0.5f;
                    if (ImGui::Button(T("Tune"), ImVec2(halfWidth, 0))) {
                        tunePredatorFrequency(hitFrequency);
                    }
                    ImGui::SameLine();
                    if (ImGui::Button(markerAssigned ? T("Release Marker") : T("Assign Marker"), ImVec2(halfWidth, 0))) {
                        if (markerAssigned || assignedMarkerCount() < predatorMarkerSlots) {
                            bool nextAssigned = !markerAssigned;
                            hit["markerAssigned"] = nextAssigned;
                            if (nextAssigned) {
                                routeHitToVfo(selectedIndex);
                            }
                            else {
                                releaseHitRoute(selectedIndex);
                            }
                            selectedChanged = true;
                        }
                    }

                    if (ImGui::Button(T("Route VFO"), ImVec2(halfWidth, 0))) {
                        if (markerAssigned || assignedMarkerCount() < predatorMarkerSlots) {
                            hit["markerAssigned"] = true;
                            routeHitToVfo(selectedIndex);
                            selectedChanged = true;
                        }
                    }
                    ImGui::SameLine();
                    if (ImGui::Button(T("Release Route"), ImVec2(halfWidth, 0))) {
                        releaseHitRoute(selectedIndex);
                        hit["markerAssigned"] = false;
                        selectedChanged = true;
                    }

                    if (ImGui::Button(T("Promote Target"), ImVec2(halfWidth, 0))) {
                        if (!isKnownTargetFrequency(hitFrequency)) {
                            json row;
                            row["name"] = name;
                            row["frequency"] = hitFrequency;
                            row["bandwidth"] = readJsonDouble(hit, "bandwidth", 12500.0);
                            row["enabled"] = true;
                            row["source"] = "hit_detail";
                            targets.push_back(row);
                        }
                        hit["state"] = "target";
                        selectedChanged = true;
                        selectedMissionChanged = true;
                    }
                    ImGui::SameLine();
                    if (ImGui::Button(T("Promote Exclude"), ImVec2(halfWidth, 0))) {
                        if (!isExcludedFrequency(hitFrequency)) {
                            json row;
                            row["name"] = name;
                            row["frequency"] = hitFrequency;
                            row["bandwidth"] = readJsonDouble(hit, "bandwidth", 12500.0);
                            row["enabled"] = true;
                            row["source"] = "hit_detail";
                            excludes.push_back(row);
                        }
                        hit["state"] = "exclude";
                        selectedChanged = true;
                        selectedMissionChanged = true;
                    }

                    if (ImGui::Button(T("Mark Viewed"), ImVec2(ImGui::GetContentRegionAvail().x, 0))) {
                        hit["unreadCount"] = 0;
                        hit["viewed"] = true;
                        selectedChanged = true;
                    }

                    ImGui::Separator();
                    ImGui::TextUnformatted(T("Event Log"));
                    double clusterWidth = std::max<double>(readJsonDouble(hit, "clusterHz", hitClusterHz), 100.0);
                    // Collect matching events, show newest first
                    std::vector<int> hitEventIdx;
                    for (int e = 0; e < (int)events.size(); e++) {
                        double eventFrequency = readJsonDouble(events[e], "frequency", 0.0);
                        if (std::abs(eventFrequency - hitFrequency) > clusterWidth) { continue; }
                        std::string eventType   = readJsonString(events[e], "type",    "event");
                        std::string eventDecoder = readJsonString(events[e], "decoder", "None");
                        if ((predatorEventFilter == 1 && eventType != "hit")    ||
                            (predatorEventFilter == 2 && eventType != "manual") ||
                            (predatorEventFilter == 3 && eventType != "target") ||
                            (predatorEventFilter == 4 && eventDecoder == "None")) { continue; }
                        hitEventIdx.push_back(e);
                    }
                    float hitEventH = std::min<float>((float)hitEventIdx.size(), 6.0f) * ImGui::GetTextLineHeightWithSpacing() + 8.0f;
                    ImGui::BeginChild("##hit_event_log", ImVec2(0, std::max(hitEventH, 60.0f * style::uiScale)), true);
                    if (hitEventIdx.empty()) {
                        ImGui::TextDisabled("%s", T("No entries."));
                    } else {
                        for (int ei = (int)hitEventIdx.size() - 1; ei >= 0; ei--) {
                            int e = hitEventIdx[ei];
                            std::string evType    = readJsonString(events[e], "type",      "event");
                            std::string evDecoder = readJsonString(events[e], "decoder",   "None");
                            std::string evLabel   = readJsonString(events[e], "label",     evType);
                            std::string evTime    = readJsonString(events[e], "time",      "?");
                            double      evStrength = readJsonDouble(events[e], "strengthDb", 0.0);
                            ImVec4 evCol;
                            if      (evType == "hit")    { evCol = ImVec4(0.20f, 0.86f, 0.40f, 1.0f); }
                            else if (evType == "manual") { evCol = ImVec4(0.95f, 0.80f, 0.15f, 1.0f); }
                            else if (evType == "target") { evCol = ImVec4(0.25f, 0.60f, 0.95f, 1.0f); }
                            else if (evDecoder != "None"){ evCol = ImVec4(0.75f, 0.35f, 0.90f, 1.0f); }
                            else                         { evCol = ImVec4(0.65f, 0.65f, 0.65f, 1.0f); }
                            ImGui::TextColored(evCol, "[%s] %s  %.0f dB  %s", evTime.c_str(), evLabel.c_str(), evStrength, evDecoder.c_str());
                        }
                    }
                    ImGui::EndChild();

                    if (selectedChanged) {
                        savePredatorHits(hits);
                    }
                    if (selectedMissionChanged) {
                        saveMissionConfig(searchBands, targets, excludes, missionThreshold, dwellMs, quickScanDelayMs, quickScanDurationMs, recordAudio);
                    }
                }
            }

            auto drawFreqRows = [&](const char* header, json& rows, bool showBandwidth) {
                if (!ImGui::CollapsingHeader(T(header), ImGuiTreeNodeFlags_DefaultOpen)) { return false; }
                bool changed = false;
                if (rows.empty()) {
                    ImGui::TextDisabled("%s", T("No entries."));
                    return changed;
                }
                for (int i = 0; i < rows.size(); i++) {
                    ImGui::PushID(i + (showBandwidth ? 1000 : 2000));
                    bool enabled = readJsonBool(rows[i], "enabled", true);
                    if (ImGui::Checkbox("##enabled", &enabled)) {
                        rows[i]["enabled"] = enabled;
                        changed = true;
                    }
                    ImGui::SameLine();
                    std::string rowName = readJsonString(rows[i], "name", showBandwidth ? "Entry" : "Band");
                    if (rows[i].contains("frequency")) {
                        double frequency = readJsonDouble(rows[i], "frequency", 0.0);
                        double bandwidth = readJsonDouble(rows[i], "bandwidth", 12500.0);
                        ImGui::Text("%s  %.0f Hz  BW %.0f", rowName.c_str(), frequency, bandwidth);
                    }
                    else {
                        double start = readJsonDouble(rows[i], "start", 0.0);
                        double stop = readJsonDouble(rows[i], "stop", 0.0);
                        ImGui::Text("%s  %.0f - %.0f Hz", rowName.c_str(), start, stop);
                    }
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Delete")) {
                        rows.erase(rows.begin() + i);
                        changed = true;
                        ImGui::PopID();
                        break;
                    }
                    ImGui::PopID();
                }
                return changed;
            };

            bool hitsChanged = false;
            if (ImGui::CollapsingHeader(T("Event Log"), ImGuiTreeNodeFlags_DefaultOpen)) {
                // Build filtered index for display
                std::vector<int> filteredEvIdx;
                for (int i = 0; i < (int)events.size(); i++) {
                    std::string type    = readJsonString(events[i], "type",    "event");
                    std::string decoder = readJsonString(events[i], "decoder", "None");
                    double frequency    = readJsonDouble(events[i], "frequency", 0.0);
                    bool currentHitEvent = (predatorSelectedHitFrequency > 0.0) &&
                        (std::abs(frequency - predatorSelectedHitFrequency) <= std::max<double>(hitClusterHz, 100.0));
                    if ((predatorEventFilter == 1 && type != "hit")    ||
                        (predatorEventFilter == 2 && type != "manual") ||
                        (predatorEventFilter == 3 && type != "target") ||
                        (predatorEventFilter == 4 && decoder == "None") ||
                        (predatorEventFilter == 5 && !currentHitEvent)) { continue; }
                    filteredEvIdx.push_back(i);
                }

                // Scrollable region — cap at 10 visible rows
                float logH = std::min<float>((float)std::max((int)filteredEvIdx.size(), 3), 10) * ImGui::GetTextLineHeightWithSpacing() + 8.0f;
                ImGui::BeginChild("##global_event_log", ImVec2(0, logH), true);
                if (filteredEvIdx.empty()) {
                    ImGui::TextDisabled("%s", T("No entries."));
                } else {
                    // Newest first
                    for (int ei = (int)filteredEvIdx.size() - 1; ei >= 0; ei--) {
                        int i = filteredEvIdx[ei];
                        std::string evTime    = readJsonString(events[i], "time",      "?");
                        std::string evType    = readJsonString(events[i], "type",      "event");
                        std::string evLabel   = readJsonString(events[i], "label",     evType);
                        std::string evDecoder = readJsonString(events[i], "decoder",   "None");
                        std::string evDevice  = readJsonString(events[i], "sourceDevice", "local");
                        double      evFreq    = readJsonDouble(events[i], "frequency",  0.0);
                        double      evStr     = readJsonDouble(events[i], "strengthDb", 0.0);
                        double      evLat     = readJsonDouble(events[i], "lat",        0.0);
                        double      evLon     = readJsonDouble(events[i], "lon",        0.0);
                        bool        evHasGps  = (evLat != 0.0 || evLon != 0.0);

                        ImVec4 evCol;
                        if      (evType == "hit")    { evCol = ImVec4(0.20f, 0.86f, 0.40f, 1.0f); }
                        else if (evType == "manual") { evCol = ImVec4(0.95f, 0.80f, 0.15f, 1.0f); }
                        else if (evType == "target") { evCol = ImVec4(0.25f, 0.60f, 0.95f, 1.0f); }
                        else if (evDecoder != "None"){ evCol = ImVec4(0.75f, 0.35f, 0.90f, 1.0f); }
                        else                         { evCol = ImVec4(0.65f, 0.65f, 0.65f, 1.0f); }

                        // Source-device column: highlight peer-origin rows in
                        // amber so the operator can see at a glance which
                        // events came in through the fleet console.
                        ImVec4 devCol = (evDevice == "local")
                            ? ImVec4(0.55f, 0.55f, 0.55f, 1.0f)
                            : ImVec4(0.95f, 0.65f, 0.20f, 1.0f);
                        ImGui::TextColored(devCol, "[%-10s]", evDevice.c_str());
                        ImGui::SameLine();
                        ImGui::TextColored(evCol, "[%s] %-8s %s  %.0f dB  %s",
                            evTime.c_str(), evType.c_str(),
                            formatFrequency(evFreq).c_str(), evStr,
                            (evDecoder != "None") ? evDecoder.c_str() : evLabel.c_str());
                        if (evHasGps) {
                            ImGui::SameLine();
                            ImGui::PushID(i);
                            if (ImGui::SmallButton(T("\u2192 Map"))) {
                                predatorTab = PREDATOR_TAB_MAP;
                            }
                            ImGui::PopID();
                        }
                    }
                }
                ImGui::EndChild();

                if (ImGui::Button(T("Clear Events"), ImVec2(ImGui::GetContentRegionAvail().x, 0))) {
                    events = json::array();
                    savePredatorEvents(events);
                }
            }
            if (predatorQuickFilter == 0 || predatorQuickFilter == 1) {
                hitsChanged |= drawFreqRows("Targets", targets, true);
            }
            if (predatorQuickFilter == 0 || predatorQuickFilter == 2) {
                hitsChanged |= drawFreqRows("Excludes", excludes, true);
            }
            if (predatorQuickFilter == 0 || predatorQuickFilter == 3) {
                hitsChanged |= drawFreqRows("Search Bands", searchBands, false);
            }
            if (hitsChanged) {
                saveMissionConfig(searchBands, targets, excludes, missionThreshold, dwellMs, quickScanDelayMs, quickScanDurationMs, recordAudio);
            }

            // ── CSV Export ─────────────────────────────────────────────────
            if (ImGui::CollapsingHeader(T("Export CSV"))) {
                if (hits.empty()) {
                    ImGui::TextDisabled("No hits to export");
                } else {
                    ImGui::TextDisabled("%d hits  %d events", (int)hits.size(), (int)events.size());
                }
                float halfW = (ImGui::GetContentRegionAvail().x - 4.0f * style::uiScale) * 0.5f;
                bool hitsDisabled = hits.empty();
                bool eventsDisabled = events.empty();

                if (hitsDisabled) { style::beginDisabled(); }
                if (ImGui::Button(T("Export Hits CSV"), ImVec2(halfW, 0))) {
                    exportHitsCsv();
                }
                if (hitsDisabled) { style::endDisabled(); }

                ImGui::SameLine(0, 4.0f * style::uiScale);

                if (eventsDisabled) { style::beginDisabled(); }
                if (ImGui::Button(T("Export Events CSV"), ImVec2(halfW, 0))) {
                    exportEventsCsv();
                }
                if (eventsDisabled) { style::endDisabled(); }

                if (!exportStatus.empty()) {
                    ImGui::TextWrapped("%s", exportStatus.c_str());
                }
            }
            // ── End CSV Export ─────────────────────────────────────────────
        }
        else if (predatorTab == PREDATOR_TAB_NETWORK) {
            if (ImGui::CollapsingHeader(T("Network Workflow"), ImGuiTreeNodeFlags_DefaultOpen)) {
                ImGui::TextWrapped("Decoder output is normalized into event rows first, then grouped here by protocol, network, talkgroup, radio ID, and active frequency. DSD-FME metadata slots are present even before live DSD-FME ingestion is attached.");
            }
            if (ImGui::CollapsingHeader(T("Current Workflow Assets"), ImGuiTreeNodeFlags_DefaultOpen)) {
                ImGui::Text("Search Bands: %d", (int)searchBands.size());
                ImGui::Text("Targets: %d", (int)targets.size());
                ImGui::Text("Excludes: %d", (int)excludes.size());
                ImGui::Text("Hits: %d", (int)hits.size());
                ImGui::Text("Events: %d", (int)events.size());
                int activeBridges = 0;
                std::string activeBridgeList;
                if (decoderBridges.is_object()) {
                    for (auto it = decoderBridges.begin(); it != decoderBridges.end(); ++it) {
                        if (it.value().is_object() && it.value().contains("enabled") && it.value()["enabled"].is_boolean() && it.value()["enabled"].get<bool>()) {
                            if (activeBridges) { activeBridgeList += ", "; }
                            activeBridgeList += it.key();
                            activeBridges++;
                        }
                    }
                }
                if (dsdFmeEnabled) {
                    if (activeBridges) { activeBridgeList += ", "; }
                    activeBridgeList += "dsd-fme";
                    activeBridges++;
                }
                ImGui::Text("Active Decoder Bridges: %d", activeBridges);
                if (activeBridges > 0) {
                    ImGui::TextDisabled("  %s", activeBridgeList.c_str());
                }
            }
            if (ImGui::CollapsingHeader(T("Network Topology"), ImGuiTreeNodeFlags_DefaultOpen)) {
                struct NetworkSummary {
                    std::string key;
                    std::string protocol;
                    std::string networkId;
                    std::string talkgroup;
                    int events;
                    double lastFrequency;
                    double strongest;
                };
                auto networkKeyForParts = [](const std::string& protocol, const std::string& networkId, const std::string& talkgroup) {
                    return protocol + "|" + networkId + "|" + talkgroup;
                };
                auto networkKeyForEvent = [&](const json& row) {
                    return networkKeyForParts(
                        readJsonString(row, "protocol", "Unknown"),
                        readJsonString(row, "networkId", "Unknown"),
                        readJsonString(row, "talkgroup", "Unknown"));
                };
                auto networkAliasForKey = [&](const std::string& key, const std::string& fallback) {
                    if (networkAliases.is_object() && networkAliases.contains(key) && networkAliases[key].is_string()) {
                        return (std::string)networkAliases[key];
                    }
                    return fallback;
                };
                auto findHitIndexByFrequency = [&](double frequency) {
                    for (int h = 0; h < hits.size(); h++) {
                        double hitFrequency = readJsonDouble(hits[h], "frequency", 0.0);
                        double clusterWidth = std::max<double>(readJsonDouble(hits[h], "clusterHz", hitClusterHz), 100.0);
                        if (std::abs(hitFrequency - frequency) <= clusterWidth) {
                            return h;
                        }
                    }
                    return -1;
                };
                auto ensureHitForFrequency = [&](double frequency, const std::string& name) {
                    int hitIndex = findHitIndexByFrequency(frequency);
                    if (hitIndex >= 0) { return hitIndex; }
                    json row;
                    row["name"] = name;
                    row["frequency"] = frequency;
                    row["clusterHz"] = std::max<double>(hitClusterHz, 100.0);
                    row["bandwidth"] = 12500.0;
                    row["state"] = isKnownTargetFrequency(frequency) ? "target" : (isExcludedFrequency(frequency) ? "exclude" : "unknown");
                    row["hitCount"] = 0;
                    row["eventCount"] = 0;
                    row["unreadCount"] = 0;
                    row["markerAssigned"] = false;
                    row["decoder"] = "None";
                    row["source"] = "network_action";
                    hits.push_back(row);
                    return (int)hits.size() - 1;
                };
                auto applyNetworkAction = [&](const std::string& key, const std::string& action, const std::string& alias) {
                    int changedCount = 0;
                    for (int e = 0; e < events.size(); e++) {
                        if (networkKeyForEvent(events[e]) != key) { continue; }
                        double frequency = readJsonDouble(events[e], "frequency", 0.0);
                        if (frequency <= 0.0) { continue; }
                        std::string rowName = alias.empty() ? readJsonString(events[e], "label", "Network Node") : alias;
                        int hitIndex = ensureHitForFrequency(frequency, rowName);
                        if (action == "target") {
                            if (!isKnownTargetFrequency(frequency)) {
                                json row;
                                row["name"] = rowName;
                                row["frequency"] = frequency;
                                row["bandwidth"] = readJsonDouble(hits[hitIndex], "bandwidth", 12500.0);
                                row["enabled"] = true;
                                row["source"] = "network_node";
                                targets.push_back(row);
                            }
                            hits[hitIndex]["state"] = "target";
                            changedCount++;
                        }
                        else if (action == "exclude") {
                            if (!isExcludedFrequency(frequency)) {
                                json row;
                                row["name"] = rowName;
                                row["frequency"] = frequency;
                                row["bandwidth"] = readJsonDouble(hits[hitIndex], "bandwidth", 12500.0);
                                row["enabled"] = true;
                                row["source"] = "network_node";
                                excludes.push_back(row);
                            }
                            hits[hitIndex]["state"] = "exclude";
                            changedCount++;
                        }
                        else if (action == "marker") {
                            if (readJsonBool(hits[hitIndex], "markerAssigned", false) || assignedMarkerCount() < predatorMarkerSlots) {
                                hits[hitIndex]["markerAssigned"] = true;
                                routeHitToVfo(hitIndex);
                                changedCount++;
                            }
                        }
                    }
                    savePredatorHits(hits);
                    if (action == "target" || action == "exclude") {
                        saveMissionConfig(searchBands, targets, excludes, missionThreshold, dwellMs, quickScanDelayMs, quickScanDurationMs, recordAudio);
                    }
                    return changedCount;
                };

                static std::string selectedNetworkKey = "";
                static char selectedNetworkAlias[128] = "";
                static std::string networkActionStatus = "";
                static char networkSearchBuf[128] = "";

                struct TalkgroupNode {
                    std::string key;
                    std::string protocol;
                    std::string networkId;
                    std::string talkgroup;
                    int events;
                    double lastFrequency;
                    double strongest;
                    std::string lastSeen;
                    std::vector<double> frequencies;
                    std::vector<std::string> radioIds;
                };
                auto pushUniqueDouble = [](std::vector<double>& vec, double v, double tolHz) {
                    for (size_t i = 0; i < vec.size(); i++) {
                        if (std::abs(vec[i] - v) <= tolHz) { return; }
                    }
                    vec.push_back(v);
                };
                auto pushUniqueString = [](std::vector<std::string>& vec, const std::string& v) {
                    if (v.empty() || v == "Unknown" || v == "unknown") { return; }
                    for (size_t i = 0; i < vec.size(); i++) {
                        if (vec[i] == v) { return; }
                    }
                    vec.push_back(v);
                };

                std::vector<TalkgroupNode> nodes;
                for (int i = 0; i < events.size(); i++) {
                    std::string protocol = readJsonString(events[i], "protocol", "Unknown");
                    std::string networkId = readJsonString(events[i], "networkId", "Unknown");
                    std::string talkgroup = readJsonString(events[i], "talkgroup", "Unknown");
                    std::string radioId = readJsonString(events[i], "radioId", "");
                    if (radioId.empty()) { radioId = readJsonString(events[i], "eventId", ""); }
                    std::string key = networkKeyForParts(protocol, networkId, talkgroup);
                    int match = -1;
                    for (int s = 0; s < (int)nodes.size(); s++) {
                        if (nodes[s].key == key) { match = s; break; }
                    }
                    if (match < 0) {
                        TalkgroupNode row;
                        row.key = key;
                        row.protocol = protocol;
                        row.networkId = networkId;
                        row.talkgroup = talkgroup;
                        row.events = 0;
                        row.lastFrequency = 0.0;
                        row.strongest = -999.0;
                        nodes.push_back(row);
                        match = (int)nodes.size() - 1;
                    }
                    nodes[match].events++;
                    double freq = readJsonDouble(events[i], "frequency", 0.0);
                    if (freq > 0.0) {
                        nodes[match].lastFrequency = freq;
                        pushUniqueDouble(nodes[match].frequencies, freq, 100.0);
                    }
                    nodes[match].strongest = std::max<double>(nodes[match].strongest, readJsonDouble(events[i], "strengthDb", -999.0));
                    nodes[match].lastSeen = readJsonString(events[i], "time", nodes[match].lastSeen.c_str());
                    pushUniqueString(nodes[match].radioIds, radioId);
                }

                ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
                ImGui::InputTextWithHint("##predator_network_filter", T("Filter protocol / network / TG / radio / alias"), networkSearchBuf, sizeof(networkSearchBuf));
                std::string filter = networkSearchBuf;
                std::string filterLower = filter;
                std::transform(filterLower.begin(), filterLower.end(), filterLower.begin(), [](unsigned char c){ return std::tolower(c); });
                auto matchesFilter = [&](const TalkgroupNode& n) {
                    if (filter.empty()) { return true; }
                    auto containsCi = [&](const std::string& s) {
                        std::string lo = s;
                        std::transform(lo.begin(), lo.end(), lo.begin(), [](unsigned char c){ return std::tolower(c); });
                        return lo.find(filterLower) != std::string::npos;
                    };
                    if (containsCi(n.protocol) || containsCi(n.networkId) || containsCi(n.talkgroup)) { return true; }
                    if (containsCi(networkAliasForKey(n.key, ""))) { return true; }
                    for (size_t i = 0; i < n.radioIds.size(); i++) { if (containsCi(n.radioIds[i])) { return true; } }
                    return false;
                };

                struct ProtoBucket { std::string protocol; std::vector<int> nodeIdx; int totalEvents; };
                std::vector<ProtoBucket> protocols;
                int filteredNodeCount = 0;
                for (int i = 0; i < (int)nodes.size(); i++) {
                    if (!matchesFilter(nodes[i])) { continue; }
                    filteredNodeCount++;
                    int pidx = -1;
                    for (int p = 0; p < (int)protocols.size(); p++) {
                        if (protocols[p].protocol == nodes[i].protocol) { pidx = p; break; }
                    }
                    if (pidx < 0) {
                        ProtoBucket pb; pb.protocol = nodes[i].protocol; pb.totalEvents = 0;
                        protocols.push_back(pb);
                        pidx = (int)protocols.size() - 1;
                    }
                    protocols[pidx].nodeIdx.push_back(i);
                    protocols[pidx].totalEvents += nodes[i].events;
                }
                std::sort(protocols.begin(), protocols.end(), [](const ProtoBucket& a, const ProtoBucket& b){ return a.protocol < b.protocol; });

                if (nodes.empty()) {
                    ImGui::TextDisabled("%s", T("No entries."));
                } else if (filteredNodeCount == 0) {
                    ImGui::TextDisabled("No matches for filter.");
                } else {
                    ImGui::Text("Protocols: %d   Talkgroups: %d", (int)protocols.size(), filteredNodeCount);
                }

                auto bridgeKeyForProtocol = [](const std::string& proto) -> std::string {
                    std::string lo = proto;
                    std::transform(lo.begin(), lo.end(), lo.begin(), [](unsigned char c){ return std::tolower(c); });
                    if (lo.find("p25") != std::string::npos || lo.find("digital voice") != std::string::npos || lo.find("dmr") != std::string::npos || lo.find("nxdn") != std::string::npos) { return "p25"; }
                    if (lo.find("rtl433") != std::string::npos || lo.find("rtl_433") != std::string::npos || lo.find("ism") != std::string::npos) { return "rtl433"; }
                    if (lo.find("pocsag") != std::string::npos || lo.find("flex") != std::string::npos || lo.find("paging") != std::string::npos) { return "pocsag"; }
                    if (lo.find("ads-b") != std::string::npos || lo.find("adsb") != std::string::npos || lo.find("aircraft") != std::string::npos) { return "adsb"; }
                    if (lo.find("ais") != std::string::npos || lo.find("marine") != std::string::npos) { return "ais"; }
                    return "";
                };
                auto bridgeActive = [&](const std::string& bridgeKey) -> bool {
                    if (bridgeKey.empty()) { return false; }
                    if (!decoderBridges.is_object() || !decoderBridges.contains(bridgeKey)) { return false; }
                    const json& b = decoderBridges[bridgeKey];
                    return b.is_object() && b.contains("enabled") && b["enabled"].is_boolean() && b["enabled"].get<bool>();
                };

                int treeUid = 9000;
                for (int p = 0; p < (int)protocols.size(); p++) {
                    ImGui::PushID(treeUid++);
                    std::string bridgeKey = bridgeKeyForProtocol(protocols[p].protocol);
                    bool hasActiveBridge = bridgeActive(bridgeKey);
                    std::string indicator = hasActiveBridge ? "\xE2\x97\x8F " : (bridgeKey.empty() ? "" : "\xE2\x97\x8B ");
                    std::string protoLabel = indicator + protocols[p].protocol + "  (" + std::to_string((int)protocols[p].nodeIdx.size()) + " TGs, " + std::to_string(protocols[p].totalEvents) + " events)";
                    if (hasActiveBridge) {
                        ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(120, 220, 120, 255));
                    }
                    bool open = ImGui::TreeNodeEx(protoLabel.c_str(), ImGuiTreeNodeFlags_DefaultOpen);
                    if (hasActiveBridge) {
                        ImGui::PopStyleColor();
                    }
                    if (open) {
                        if (!bridgeKey.empty()) {
                            ImGui::TextDisabled(hasActiveBridge ? "Bridge active: %s" : "Bridge configured but disabled: %s", bridgeKey.c_str());
                        }
                        std::vector<std::string> netOrder;
                        std::map<std::string, std::vector<int>> netGroups;
                        for (size_t k = 0; k < protocols[p].nodeIdx.size(); k++) {
                            int ni = protocols[p].nodeIdx[k];
                            const std::string& nid = nodes[ni].networkId;
                            if (netGroups.find(nid) == netGroups.end()) { netOrder.push_back(nid); }
                            netGroups[nid].push_back(ni);
                        }
                        std::sort(netOrder.begin(), netOrder.end());
                        for (size_t n = 0; n < netOrder.size(); n++) {
                            ImGui::PushID(treeUid++);
                            const std::vector<int>& tgList = netGroups[netOrder[n]];
                            int netEvents = 0;
                            for (size_t k = 0; k < tgList.size(); k++) { netEvents += nodes[tgList[k]].events; }
                            std::string netLabel = "Net " + netOrder[n] + "  (" + std::to_string((int)tgList.size()) + " TGs, " + std::to_string(netEvents) + " events)";
                            if (ImGui::TreeNodeEx(netLabel.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
                                for (size_t k = 0; k < tgList.size(); k++) {
                                    int ni = tgList[k];
                                    ImGui::PushID(treeUid++);
                                    std::string baseLabel = "TG " + nodes[ni].talkgroup;
                                    std::string aliasLabel = networkAliasForKey(nodes[ni].key, "");
                                    std::string tgLabel = aliasLabel.empty() ? baseLabel : (baseLabel + "  \xE2\x80\x94 " + aliasLabel);
                                    bool selected = (selectedNetworkKey == nodes[ni].key);
                                    if (selected) { ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 200, 90, 255)); }
                                    bool open = ImGui::TreeNode(tgLabel.c_str());
                                    if (selected) { ImGui::PopStyleColor(); }
                                    if (open) {
                                        ImGui::Text("Events: %d", nodes[ni].events);
                                        ImGui::Text("Strongest: %.1f dB", nodes[ni].strongest);
                                        if (!nodes[ni].lastSeen.empty()) {
                                            ImGui::Text("Last Seen: %s", nodes[ni].lastSeen.c_str());
                                        }
                                        if (!nodes[ni].frequencies.empty()) {
                                            std::string freqs = "Frequencies (" + std::to_string((int)nodes[ni].frequencies.size()) + "): ";
                                            for (size_t f = 0; f < nodes[ni].frequencies.size() && f < 8; f++) {
                                                if (f) { freqs += ", "; }
                                                freqs += formatFrequency(nodes[ni].frequencies[f]);
                                            }
                                            if (nodes[ni].frequencies.size() > 8) { freqs += ", ..."; }
                                            ImGui::TextWrapped("%s", freqs.c_str());
                                        }
                                        if (!nodes[ni].radioIds.empty()) {
                                            std::string rids = "Radio IDs (" + std::to_string((int)nodes[ni].radioIds.size()) + "): ";
                                            for (size_t r = 0; r < nodes[ni].radioIds.size() && r < 12; r++) {
                                                if (r) { rids += ", "; }
                                                rids += nodes[ni].radioIds[r];
                                            }
                                            if (nodes[ni].radioIds.size() > 12) { rids += ", ..."; }
                                            ImGui::TextWrapped("%s", rids.c_str());
                                        } else {
                                            ImGui::TextDisabled("No radio IDs yet (populates when DSD-FME / native decoder ingests metadata).");
                                        }
                                        if (ImGui::Button(T("Select"), ImVec2(ImGui::GetContentRegionAvail().x, 0))) {
                                            selectedNetworkKey = nodes[ni].key;
                                            std::string fallback = nodes[ni].protocol + " / Net " + nodes[ni].networkId + " / TG " + nodes[ni].talkgroup;
                                            snprintf(selectedNetworkAlias, sizeof(selectedNetworkAlias), "%s", networkAliasForKey(nodes[ni].key, fallback).c_str());
                                        }
                                        ImGui::TreePop();
                                    }
                                    ImGui::PopID();
                                }
                                ImGui::TreePop();
                            }
                            ImGui::PopID();
                        }
                        ImGui::TreePop();
                    }
                    ImGui::PopID();
                }

                static std::string topologyExportStatus = "";
                ImGui::Separator();
                bool topoDisabled = nodes.empty();
                if (topoDisabled) { ImGui::BeginDisabled(); }
                if (ImGui::Button(T("Export Topology CSV"), ImVec2(ImGui::GetContentRegionAvail().x, 0))) {
                    auto csvEsc = [](const std::string& s) {
                        bool needs = false;
                        for (size_t i = 0; i < s.size(); i++) {
                            char c = s[i];
                            if (c == ',' || c == '"' || c == '\n' || c == '\r') { needs = true; break; }
                        }
                        if (!needs) { return std::string("\"") + s + "\""; }
                        std::string out = "\"";
                        for (size_t i = 0; i < s.size(); i++) {
                            char c = s[i];
                            if (c == '"') { out += "\"\""; } else { out += c; }
                        }
                        out += "\"";
                        return out;
                    };
                    std::filesystem::path dir = std::filesystem::path(core::args["root"].s()) / "exports";
                    std::error_code ec;
                    std::filesystem::create_directories(dir, ec);
                    std::string fname = "predator_network_" + filenameTimestamp() + ".csv";
                    std::filesystem::path full = dir / fname;
                    std::ofstream f(full);
                    if (f.is_open()) {
                        f << "protocol,network_id,talkgroup,alias,events,last_seen,last_freq_hz,strongest_db,frequencies,radio_ids\n";
                        for (size_t i = 0; i < nodes.size(); i++) {
                            std::string alias = networkAliasForKey(nodes[i].key, "");
                            std::string freqList;
                            for (size_t k = 0; k < nodes[i].frequencies.size(); k++) {
                                if (k) { freqList += ";"; }
                                freqList += std::to_string((long long)std::llround(nodes[i].frequencies[k]));
                            }
                            std::string ridList;
                            for (size_t k = 0; k < nodes[i].radioIds.size(); k++) {
                                if (k) { ridList += ";"; }
                                ridList += nodes[i].radioIds[k];
                            }
                            f << csvEsc(nodes[i].protocol) << ","
                              << csvEsc(nodes[i].networkId) << ","
                              << csvEsc(nodes[i].talkgroup) << ","
                              << csvEsc(alias) << ","
                              << nodes[i].events << ","
                              << csvEsc(nodes[i].lastSeen) << ","
                              << (long long)std::llround(nodes[i].lastFrequency) << ","
                              << nodes[i].strongest << ","
                              << csvEsc(freqList) << ","
                              << csvEsc(ridList) << "\n";
                        }
                        f.close();
                        topologyExportStatus = "Saved " + std::to_string((int)nodes.size()) + " nodes \xE2\x86\x92 " + full.string();
                    } else {
                        topologyExportStatus = "Failed to open " + full.string();
                    }
                }
                if (topoDisabled) { ImGui::EndDisabled(); }
                if (!topologyExportStatus.empty()) {
                    ImGui::TextWrapped("%s", topologyExportStatus.c_str());
                }

                if (!selectedNetworkKey.empty() && ImGui::CollapsingHeader(T("Network Node Actions"), ImGuiTreeNodeFlags_DefaultOpen)) {
                    ImGui::TextDisabled("Selected: %s", selectedNetworkKey.c_str());
                    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
                    if (ImGui::InputText(T("Alias"), selectedNetworkAlias, sizeof(selectedNetworkAlias))) {
                        networkAliases[selectedNetworkKey] = std::string(selectedNetworkAlias);
                        saveNetworkAliases(networkAliases);
                    }
                    float thirdWidth = (ImGui::GetContentRegionAvail().x - (12.0f * style::uiScale)) / 3.0f;
                    if (ImGui::Button(T("Target Node"), ImVec2(thirdWidth, 0))) {
                        int count = applyNetworkAction(selectedNetworkKey, "target", selectedNetworkAlias);
                        networkActionStatus = "Targeted " + std::to_string(count) + " node frequencies";
                    }
                    ImGui::SameLine();
                    if (ImGui::Button(T("Exclude Node"), ImVec2(thirdWidth, 0))) {
                        int count = applyNetworkAction(selectedNetworkKey, "exclude", selectedNetworkAlias);
                        networkActionStatus = "Excluded " + std::to_string(count) + " node frequencies";
                    }
                    ImGui::SameLine();
                    if (ImGui::Button(T("Mark Hits"), ImVec2(thirdWidth, 0))) {
                        int count = applyNetworkAction(selectedNetworkKey, "marker", selectedNetworkAlias);
                        networkActionStatus = "Marked " + std::to_string(count) + " node hits";
                    }
                    if (ImGui::Button(T("Clear Selection"), ImVec2(ImGui::GetContentRegionAvail().x, 0))) {
                        selectedNetworkKey = "";
                        selectedNetworkAlias[0] = '\0';
                        networkActionStatus = "";
                    }
                    if (!networkActionStatus.empty()) {
                        ImGui::TextWrapped("%s", networkActionStatus.c_str());
                    }
                }
            }
            if (ImGui::CollapsingHeader(T("Decoder Bridges"), ImGuiTreeNodeFlags_DefaultOpen)) {
                ensureDecoderBridge("p25",     "127.0.0.1",  7355,  "DSD-FME Direct",   "P25 Phase 1 (C4FM) and Phase 2 (H-DQPSK). Routes through the DSD-FME bridge below; Talkgroup, Network ID, and Radio ID populate the topology tree as decoded WACN/RFSS metadata arrives.");
                ensureDecoderBridge("rtl433",  "127.0.0.1",  1433,  "TCP JSON Lines",   "rtl_433 ISM device telemetry (315/433/868/915 MHz). Each JSON line becomes one Network event; protocol = device model, networkId = device id, talkgroup = channel.");
                ensureDecoderBridge("pocsag",  "127.0.0.1",  1444,  "TCP Lines",        "POCSAG / FLEX paging via multimon-ng -t raw, relayed over TCP. networkId = capcode, talkgroup = function bits, radioId = source paging service.");
                ensureDecoderBridge("adsb",    "127.0.0.1",  30003, "BaseStation 30003","dump1090 / readsb feed. networkId = ICAO hex, talkgroup = callsign, radioId = squawk. Aircraft positions can be forwarded to the tactical map.");
                ensureDecoderBridge("ais",     "127.0.0.1",  10110, "UDP NMEA",         "AIS marine VHF (161.975 / 162.025 MHz) via rtl_ais or aisdec. networkId = MMSI, talkgroup = ship type, radioId = call sign.");

                static char bridgeHostBuf[1024];
                static char bridgeNotesBuf[2048];

                auto renderBridge = [&](const std::string& key, const char* title, const char* protocolLabel, const char* const* modes, int modeCount) {
                    json& b = decoderBridges[key];
                    bool enabled = readJsonBool(b, "enabled", false);
                    std::string host = readJsonString(b, "host", "127.0.0.1");
                    int port = (int)readJsonDouble(b, "port", 0.0);
                    std::string mode = readJsonString(b, "mode", modes[0]);
                    std::string notes = readJsonString(b, "notes", "");

                    ImGui::PushID(key.c_str());
                    if (ImGui::TreeNodeEx(title, ImGuiTreeNodeFlags_DefaultOpen)) {
                        ImGui::TextDisabled("Feeds protocol: %s", protocolLabel);
                        if (enabled) {
                            ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(120, 220, 120, 255));
                            ImGui::Text("\xE2\x97\x8F %s", T("Bridge enabled"));
                            ImGui::PopStyleColor();
                        } else {
                            ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(180, 180, 180, 255));
                            ImGui::Text("\xE2\x97\x8B %s", T("Bridge disabled"));
                            ImGui::PopStyleColor();
                        }
                        bool changed = false;
                        if (ImGui::Checkbox(T("Enable"), &enabled)) { b["enabled"] = enabled; changed = true; }

                        snprintf(bridgeHostBuf, sizeof(bridgeHostBuf), "%s", host.c_str());
                        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.55f);
                        if (ImGui::InputText(T("Host"), bridgeHostBuf, sizeof(bridgeHostBuf))) { b["host"] = std::string(bridgeHostBuf); changed = true; }
                        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.4f);
                        if (ImGui::InputInt(T("Port"), &port, 1, 100)) { port = std::clamp<int>(port, 1, 65535); b["port"] = port; changed = true; }

                        int modeIndex = 0;
                        for (int m = 0; m < modeCount; m++) { if (mode == modes[m]) { modeIndex = m; break; } }
                        std::string comboBuf;
                        for (int m = 0; m < modeCount; m++) { comboBuf += modes[m]; comboBuf.push_back('\0'); }
                        comboBuf.push_back('\0');
                        if (ImGui::Combo(T("Mode"), &modeIndex, comboBuf.c_str())) {
                            modeIndex = std::clamp<int>(modeIndex, 0, modeCount - 1);
                            b["mode"] = std::string(modes[modeIndex]);
                            changed = true;
                        }

                        snprintf(bridgeNotesBuf, sizeof(bridgeNotesBuf), "%s", notes.c_str());
                        ImGui::TextWrapped("%s", T("Notes / Endpoint Format"));
                        if (ImGui::InputTextMultiline(("##" + key + "_notes").c_str(), bridgeNotesBuf, sizeof(bridgeNotesBuf), ImVec2(ImGui::GetContentRegionAvail().x, ImGui::GetTextLineHeight() * 3.0f))) {
                            b["notes"] = std::string(bridgeNotesBuf);
                            changed = true;
                        }

                        if (changed) { saveDecoderBridges(decoderBridges); }
                        ImGui::TreePop();
                    }
                    ImGui::PopID();
                };

                int activeBridgeCount = 0;
                for (auto it = decoderBridges.begin(); it != decoderBridges.end(); ++it) {
                    if (it.value().is_object() && it.value().contains("enabled") && it.value()["enabled"].is_boolean() && it.value()["enabled"].get<bool>()) {
                        activeBridgeCount++;
                    }
                }
                ImGui::Text("Active Bridges: %d / 5", activeBridgeCount);
                ImGui::TextWrapped("Each enabled bridge becomes a live source feeding the Network Topology tree above. Protocol, Network ID, Talkgroup, and Radio ID fields on incoming events are grouped automatically. The bridges below are scaffolded — their config is persisted and the event ingest path is the unified JSON event bus shared with the Hits/Events tab.");
                ImGui::Separator();

                static const char* p25Modes[]    = { "DSD-FME Direct", "OP25 Bridge", "External Companion" };
                static const char* rtl433Modes[] = { "TCP JSON Lines", "UDP JSON Lines", "Stdin Companion" };
                static const char* pocsagModes[] = { "TCP Lines", "UDP Lines", "Stdin Companion" };
                static const char* adsbModes[]   = { "BaseStation 30003", "Beast Binary 30005", "SBS-3", "JSON aircraft.json" };
                static const char* aisModes[]    = { "UDP NMEA", "TCP NMEA", "Stdin NMEA" };

                renderBridge("p25",    "P25 Trunked Voice (Phase 1 + 2)", "Digital Voice / P25", p25Modes,    3);
                renderBridge("rtl433", "RTL433 ISM Devices",              "RTL433",               rtl433Modes, 3);
                {
                    ImGui::Indent();
                    bool conn = rtl433Ingester.isConnected();
                    bool run = rtl433Ingester.isRunning();
                    ImVec4 col = conn ? ImVec4(0.25f, 0.85f, 0.25f, 1.0f)
                                      : (run ? ImVec4(0.95f, 0.75f, 0.20f, 1.0f)
                                             : ImVec4(0.55f, 0.55f, 0.55f, 1.0f));
                    ImGui::TextColored(col, "%s", conn ? "[LINK]" : (run ? "[WAIT]" : "[OFF] "));
                    ImGui::SameLine();
                    std::string st = rtl433Ingester.status();
                    ImGui::TextDisabled("%s  -  events received: %d",
                                        st.c_str(), rtl433Ingester.eventsReceived());
                    ImGui::Unindent();
                }
                renderBridge("pocsag", "POCSAG / FLEX Paging",            "Paging",               pocsagModes, 3);
                renderBridge("adsb",   "ADS-B Aircraft (dump1090)",       "ADS-B",                adsbModes,   4);
                {
                    ImGui::Indent();
                    bool conn = adsbIngester.isConnected();
                    bool run = adsbIngester.isRunning();
                    ImVec4 col = conn ? ImVec4(0.25f, 0.85f, 0.25f, 1.0f)
                                      : (run ? ImVec4(0.95f, 0.75f, 0.20f, 1.0f)
                                             : ImVec4(0.55f, 0.55f, 0.55f, 1.0f));
                    ImGui::TextColored(col, "%s", conn ? "[LINK]" : (run ? "[WAIT]" : "[OFF] "));
                    ImGui::SameLine();
                    std::string st = adsbIngester.status();
                    ImGui::TextDisabled("%s  -  events received: %d",
                                        st.c_str(), adsbIngester.eventsReceived());
                    // The CSV parser only understands the BaseStation 30003 wire
                    // format. The other dropdown options (Beast binary, SBS-3,
                    // aircraft.json) require their own decoders and would
                    // silently produce zero events otherwise -- warn explicitly.
                    {
                        std::string adsbMode = readJsonString(decoderBridges["adsb"], "mode", "BaseStation 30003");
                        if (adsbMode.find("BaseStation") == std::string::npos) {
                            ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.20f, 1.0f),
                                "[NOTICE] Mode '%s' is not yet decoded -- only 'BaseStation 30003' produces events. Run dump1090 with --net to expose port 30003.",
                                adsbMode.c_str());
                        }
                    }
                    ImGui::Unindent();
                }
                renderBridge("ais",    "AIS Marine VHF",                  "AIS",                  aisModes,    3);
            }
            if (ImGui::CollapsingHeader(T("DSD-FME Digital Voice"), ImGuiTreeNodeFlags_DefaultOpen)) {
                bool dsdChanged = false;
                dsdChanged |= ImGui::Checkbox(T("Enable DSD-FME Bridge"), &dsdFmeEnabled);
                static char dsdHostBuf[128] = "";
                static bool dsdHostInit = false;
                if (!dsdHostInit || strcmp(dsdHostBuf, dsdFmeHost.c_str()) == 0) {
                    snprintf(dsdHostBuf, sizeof(dsdHostBuf), "%s", dsdFmeHost.c_str());
                    dsdHostInit = true;
                }
                if (ImGui::InputText(T("Bridge Host"), dsdHostBuf, sizeof(dsdHostBuf))) {
                    dsdFmeHost = dsdHostBuf;
                    dsdChanged = true;
                }
                if (ImGui::InputInt(T("Bridge Port"), &dsdFmePort, 1, 100)) {
                    dsdFmePort = std::clamp<int>(dsdFmePort, 1, 65535);
                    dsdChanged = true;
                }
                const char* dsdModes[] = { "TCP Direct Link Audio", "RIGCTL Metadata", "External Companion" };
                int dsdModeIndex = (dsdFmeMode == dsdModes[1]) ? 1 : ((dsdFmeMode == dsdModes[2]) ? 2 : 0);
                if (ImGui::Combo(T("Bridge Mode"), &dsdModeIndex, "TCP Direct Link Audio\0RIGCTL Metadata\0External Companion\0")) {
                    dsdFmeMode = dsdModes[dsdModeIndex];
                    dsdChanged = true;
                }
                if (ImGui::CollapsingHeader(T("Decoder Outputs"), ImGuiTreeNodeFlags_DefaultOpen)) {
                    ImGui::TextUnformatted(T("Voice Extractions"));
                    if (voiceFolderSelect.render("##predator_voice_output")) {
                        voiceOutputPath = voiceFolderSelect.path;
                        std::error_code ec;
                        std::filesystem::create_directories(voiceFolderSelect.expandString(voiceOutputPath), ec);
                        dsdChanged = true;
                    }
                    ImGui::TextUnformatted(T("Data Extractions"));
                    if (dataFolderSelect.render("##predator_data_output")) {
                        dataOutputPath = dataFolderSelect.path;
                        std::error_code ec;
                        std::filesystem::create_directories(dataFolderSelect.expandString(dataOutputPath), ec);
                        dsdChanged = true;
                    }
                    ImGui::TextDisabled("The original SDR++ recorder folder remains managed by the Recorder module.");
                }
                ImGui::TextWrapped("%s", chineseUi ? "DSD-FME \u6a4b\u63a5\u8a2d\u5b9a\u5df2\u5132\u5b58\uff1b\u4e0b\u4e00\u968e\u6bb5\u6703\u63a5\u4e0a\u97f3\u8a0a\u8207\u89e3\u78bc\u4e8b\u4ef6\u8f38\u5165\u3002" : "DSD-FME bridge settings are persisted here; the next decoder pass will connect audio and decoded event input.");
                if (dsdChanged) {
                    saveDsdFmeConfig(dsdFmeEnabled, dsdFmeHost, dsdFmePort, dsdFmeMode);
                }
            }
            if (ImGui::CollapsingHeader(T("Decoder Events"), ImGuiTreeNodeFlags_DefaultOpen)) {
                std::map<std::string, int> decoderCounts;
                int totalDecoderEvents = 0;
                for (int i = 0; i < events.size(); i++) {
                    std::string decoder = readJsonString(events[i], "decoder", "None");
                    if (decoder == "None") { continue; }
                    decoderCounts[decoder]++;
                    totalDecoderEvents++;
                }
                if (totalDecoderEvents == 0) {
                    ImGui::TextDisabled("%s", T("No entries."));
                } else {
                    std::string countLine = "By decoder: ";
                    bool first = true;
                    for (auto& kv : decoderCounts) {
                        if (!first) { countLine += ", "; }
                        countLine += kv.first + " " + std::to_string(kv.second);
                        first = false;
                    }
                    ImGui::TextDisabled("%s", countLine.c_str());
                    ImGui::Separator();
                    ImGui::BeginChild("predator_decoder_events_list", ImVec2(0, ImGui::GetTextLineHeight() * 10.0f), true);
                    for (int i = (int)events.size() - 1; i >= 0; i--) {
                        std::string decoder = readJsonString(events[i], "decoder", "None");
                        if (decoder == "None") { continue; }
                        ImGui::TextWrapped("%s  %s  %s  %s  %s",
                            readJsonString(events[i], "time", "unknown").c_str(),
                            decoder.c_str(),
                            readJsonString(events[i], "protocol", "Unknown").c_str(),
                            readJsonString(events[i], "label", "Event").c_str(),
                            formatFrequency(readJsonDouble(events[i], "frequency", 0.0)).c_str());
                    }
                    ImGui::EndChild();
                }
            }
        }
        else if (predatorTab == PREDATOR_TAB_MAP) {
            if (ImGui::CollapsingHeader(T("Phone Map"), ImGuiTreeNodeFlags_DefaultOpen)) {
                ImGui::TextWrapped("The tactical map launches as a dedicated Android touch screen so pan, zoom, and pinch behavior feel like a normal map app instead of an ImGui widget.");
                if (phoneHasFix) {
                    ImGui::Text("Phone GPS: %.6f, %.6f", phoneLat, phoneLon);
                    ImGui::Text("Accuracy: %.1f m", phoneAccuracy);
                }
                else {
                    ImGui::TextDisabled("Phone GPS fix not available yet.");
                }
                if (ImGui::Button(T("Open Tactical Map"), ImVec2(ImGui::GetContentRegionAvail().x, 0))) {
                    backend::openMapView();
                }
            }
            if (ImGui::CollapsingHeader(T("Fleet Peers"), ImGuiTreeNodeFlags_DefaultOpen)) {
                // List every Kujhad peer with a known GPS fix as a marker
                // candidate. The Android map view uses these via its own
                // peer-marker layer; here we surface them so the operator
                // can confirm what will be plotted.
                json& peers = core::configManager.conf["kujhadPeers"];
                if (!peers.is_array() || peers.empty()) {
                    ImGui::TextDisabled("%s", T("No fleet peers configured. Add some in the Kujhad tab."));
                } else {
                    int markerCount = 0;
                    for (size_t i = 0; i < peers.size() && i < kujhadClients.size(); i++) {
                        if (!kujhadClients[i]) continue;
                        std::string name = readJsonString(peers[i], "name", "peer");
                        predator::KujhadPeerSnapshot snap = kujhadClients[i]->snapshot();
                        if (!snap.reachable) {
                            ImGui::TextDisabled("[%s] %s", name.c_str(), T("offline"));
                            continue;
                        }
                        bool hasFix = snap.gps.is_object() && snap.gps.value("hasFix", false);
                        if (!hasFix) {
                            ImGui::TextDisabled("[%s] %s", name.c_str(), T("no GPS fix"));
                            continue;
                        }
                        double pLat = snap.gps.value("lat", 0.0);
                        double pLon = snap.gps.value("lon", 0.0);
                        double pAcc = snap.gps.value("accuracy", 0.0);
                        std::string role = snap.identify.is_object()
                            ? snap.identify.value("role", std::string("?")) : std::string("?");
                        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.65f, 0.20f, 1.0f));
                        ImGui::Text("[%s]", name.c_str());
                        ImGui::PopStyleColor();
                        ImGui::SameLine();
                        ImGui::Text("%.6f, %.6f  +/-%.1fm  role=%s",
                                    pLat, pLon, pAcc, role.c_str());
                        markerCount++;
                    }
                    ImGui::TextDisabled("%s: %d", T("Peer markers ready"), markerCount);
                    ImGui::TextWrapped("%s", T("Peer markers are plotted on the tactical map alongside the local position. Tap one to see device, role, and last sync time."));
                }
            }
            if (ImGui::CollapsingHeader(T("DF Status"), ImGuiTreeNodeFlags_DefaultOpen)) {
                ImGui::TextWrapped("Direction-finding is intentionally excluded for now. Only the placeholder directory exists so we can add it cleanly later.");
            }
        }
        else if (predatorTab == PREDATOR_TAB_MISSION) {
            static char newBandName[64] = "New Band";
            static double newBandStart = 150000000.0;
            static double newBandStop = 170000000.0;
            static double newTargetFreq = 465000000.0;
            static double newTargetBandwidth = 12500.0;
            static double newExcludeFreq = 462500000.0;
            static double newExcludeBandwidth = 12500.0;

            // Persistent driver banner across the top of the Mission tab.
            // LOCAL = green, PEER fresh = amber, PEER stale/no-link = red.
            // Stays visible regardless of which section the operator scrolls
            // to so they always know whether edits will hit this rig or be
            // routed to a peer (and whether that peer link is healthy).
            {
                ImVec4 bannerCol;
                if (!missionPeerActive) {
                    bannerCol = ImVec4(0.18f, 0.42f, 0.20f, 1.0f);
                } else if (missionPeerSnapshotFresh) {
                    bannerCol = ImVec4(0.55f, 0.35f, 0.10f, 1.0f);
                } else {
                    bannerCol = ImVec4(0.55f, 0.18f, 0.18f, 1.0f);
                }
                ImGui::PushStyleColor(ImGuiCol_ChildBg, bannerCol);
                float bw = ImGui::GetContentRegionAvail().x;
                float bh = ImGui::GetTextLineHeightWithSpacing() + (10.0f * style::uiScale);
                ImGui::BeginChild("##mission_driver_banner", ImVec2(bw, bh),
                                  false, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
                if (missionPeerActive) {
                    if (missionPeerSnapshotFresh) {
                        ImGui::Text(T("Driving PEER: %s   (edits route to peer)"), missionPeerName.c_str());
                    } else if (missionPeerLastSyncMs > 0) {
                        ImGui::Text(T("Driving PEER: %s   (link STALE — commands may not land)"),
                                    missionPeerName.c_str());
                    } else {
                        ImGui::Text(T("Driving PEER: %s   (no link yet — commands may not land)"),
                                    missionPeerName.c_str());
                    }
                } else {
                    ImGui::TextUnformatted(T("Driving LOCAL rig"));
                }
                ImGui::EndChild();
                ImGui::PopStyleColor();
                ImGui::Spacing();
            }

            // Resolve the mission view source for this frame. When peer-
            // active these mirror the peer's /v1/state payload; otherwise
            // they alias the local configManager-backed arrays.
            json displayedBandsCopy    = missionPeerActive ? missionPeerArray("searchBands") : json::array();
            json displayedTargetsCopy  = missionPeerActive ? missionPeerArray("targets")     : json::array();
            json displayedExcludesCopy = missionPeerActive ? missionPeerArray("excludes")    : json::array();
            const json& displayedBands    = missionPeerActive ? displayedBandsCopy    : searchBands;
            const json& displayedTargets  = missionPeerActive ? displayedTargetsCopy  : targets;
            const json& displayedExcludes = missionPeerActive ? displayedExcludesCopy : excludes;
            int   displayedMode        = missionPeerActive ? missionPeerMode : predatorMissionMode;
            int   displayedDwellMs     = missionPeerActive ? missionPeerState.value("dwellMs", dwellMs) : dwellMs;
            int   displayedQsDelay     = missionPeerActive ? missionPeerState.value("quickScanDelayMs", quickScanDelayMs) : quickScanDelayMs;
            int   displayedQsDuration  = missionPeerActive ? missionPeerState.value("quickScanDurationMs", quickScanDurationMs) : quickScanDurationMs;
            float displayedThreshold   = missionPeerActive ? (float)missionPeerState.value("thresholdDb", (double)missionThreshold) : missionThreshold;
            bool  displayedRecordAudio = missionPeerActive ? missionPeerState.value("recordAudio", recordAudio) : recordAudio;

            // Commit helpers fan out to either the local config save path
            // or the fleet command pipe so the rest of the body never has
            // to spell the branch out at every edit site.
            auto commitSearchBands = [&](const json& v) {
                if (missionPeerActive) {
                    json args; args["bands"] = v;
                    missionRoutePeerCmd("mission", "setSearchBands", args);
                } else {
                    searchBands = v;
                    saveMissionConfig(searchBands, targets, excludes, missionThreshold, dwellMs, quickScanDelayMs, quickScanDurationMs, recordAudio);
                    scanCandidates = buildScanCandidates();
                }
            };
            auto commitTargets = [&](const json& v) {
                if (missionPeerActive) {
                    json args; args["targets"] = v;
                    missionRoutePeerCmd("mission", "setTargets", args);
                } else {
                    targets = v;
                    saveMissionConfig(searchBands, targets, excludes, missionThreshold, dwellMs, quickScanDelayMs, quickScanDurationMs, recordAudio);
                    scanCandidates = buildScanCandidates();
                }
            };
            auto commitExcludes = [&](const json& v) {
                if (missionPeerActive) {
                    json args; args["excludes"] = v;
                    missionRoutePeerCmd("mission", "setExcludes", args);
                } else {
                    excludes = v;
                    saveMissionConfig(searchBands, targets, excludes, missionThreshold, dwellMs, quickScanDelayMs, quickScanDurationMs, recordAudio);
                    scanCandidates = buildScanCandidates();
                }
            };
            auto commitMissionSettings = [&](float threshold, int dwell, int qsDelay,
                                             int qsDur, bool rec) {
                if (missionPeerActive) {
                    json args;
                    args["thresholdDb"]         = threshold;
                    args["dwellMs"]             = dwell;
                    args["quickScanDelayMs"]    = qsDelay;
                    args["quickScanDurationMs"] = qsDur;
                    args["recordAudio"]         = rec;
                    missionRoutePeerCmd("mission", "setSettings", args);
                } else {
                    missionThreshold     = threshold;
                    dwellMs              = dwell;
                    quickScanDelayMs     = qsDelay;
                    quickScanDurationMs  = qsDur;
                    recordAudio          = rec;
                    saveMissionConfig(searchBands, targets, excludes, missionThreshold, dwellMs, quickScanDelayMs, quickScanDurationMs, recordAudio);
                }
            };

            if (ImGui::CollapsingHeader(T("Mission Modes"), ImGuiTreeNodeFlags_DefaultOpen)) {
                for (int i = 0; i < 4; i++) {
                    bool activeMode = (displayedMode == i);
                    if (activeMode) {
                        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.28f, 0.39f, 0.21f, 1.0f));
                        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.32f, 0.45f, 0.24f, 1.0f));
                        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.35f, 0.50f, 0.27f, 1.0f));
                    }
                    if (ImGui::Button(missionModes[i], ImVec2(ImGui::GetContentRegionAvail().x, 0))) {
                        if (missionPeerActive) {
                            json args; args["mode"] = i;
                            missionRoutePeerCmd("mission", "setMode", args);
                        } else {
                            setMissionMode(i);
                        }
                    }
                    if (activeMode) {
                        ImGui::PopStyleColor(3);
                    }
                    ImGui::TextWrapped("%s", missionModeDescriptions[i]);
                    if (i < 3) { ImGui::Spacing(); }
                }
            }

            if (ImGui::CollapsingHeader(T("Mission Run"), ImGuiTreeNodeFlags_DefaultOpen)) {
                drawMissionRunControls();
            }

            // Scroll the panel to expose the focused input field when the keyboard opens.
            // Called after every InputText/InputDouble so the tapped field stays visible.
#ifdef __ANDROID__
            auto scrollToActive = []() {
                if (ImGui::IsItemActivated()) { ImGui::SetScrollHereY(0.0f); }
            };
#else
            auto scrollToActive = []() {};
#endif

            if (ImGui::CollapsingHeader(T("Search Bands"), ImGuiTreeNodeFlags_DefaultOpen)) {
                float fw = ImGui::GetContentRegionAvail().x;
                float hw = (fw - 4.0f * style::uiScale) * 0.5f;
                ImGui::SetNextItemWidth(fw);
                if (ImGui::InputText("##BandName", newBandName, sizeof(newBandName))) {}
                ImGui::SameLine(0, 0); ImGui::TextDisabled(" Band Name");
                scrollToActive();
                ImGui::SetNextItemWidth(hw);
                if (ImGui::InputDouble("##BandStart", &newBandStart, 0, 0, "%.0f Hz")) {}
                scrollToActive();
                ImGui::SameLine(0, 4.0f * style::uiScale);
                ImGui::SetNextItemWidth(hw);
                if (ImGui::InputDouble("##BandStop", &newBandStop, 0, 0, "%.0f Hz")) {}
                scrollToActive();
                float stepBtnW = (fw - 4.0f * style::uiScale) * 0.5f;
                if (ImGui::Button("- 1 MHz##bs", ImVec2(stepBtnW, 0))) { newBandStart -= 1e6; newBandStop -= 1e6; }
                ImGui::SameLine(0, 4.0f * style::uiScale);
                if (ImGui::Button("+ 1 MHz##bs", ImVec2(stepBtnW, 0))) { newBandStart += 1e6; newBandStop += 1e6; }
                if (ImGui::Button("From Current View", ImVec2(fw, 0))) {
                    double ctr = gui::waterfall.getCenterFrequency();
                    double bw  = gui::waterfall.getViewBandwidth();
                    newBandStart = ctr - bw * 0.5;
                    newBandStop  = ctr + bw * 0.5;
                }
                if (ImGui::Button("Add Search Band", ImVec2(fw, 0))) {
                    json row;
                    row["name"] = std::string(newBandName);
                    row["start"] = std::min(newBandStart, newBandStop);
                    row["stop"] = std::max(newBandStart, newBandStop);
                    row["enabled"] = true;
                    json updated = displayedBands;
                    updated.push_back(row);
                    commitSearchBands(updated);
                }
                ImGui::Separator();
                for (int i = 0; i < (int)displayedBands.size(); i++) {
                    ImGui::PushID(3000 + i);
                    bool enabled = readJsonBool(displayedBands[i], "enabled", true);
                    if (ImGui::Checkbox("##search_enabled", &enabled)) {
                        json updated = displayedBands;
                        updated[i]["enabled"] = enabled;
                        commitSearchBands(updated);
                    }
                    ImGui::SameLine();
                    std::string bandName = readJsonString(displayedBands[i], "name", "Band");
                    double bandStart = readJsonDouble(displayedBands[i], "start", 0.0);
                    double bandStop = readJsonDouble(displayedBands[i], "stop", 0.0);
                    ImGui::TextWrapped("%s  %.3f - %.3f MHz", bandName.c_str(), bandStart/1e6, bandStop/1e6);
                    float delW = ImGui::GetContentRegionAvail().x;
                    if (ImGui::Button("Delete##sb", ImVec2(delW, 0))) {
                        json updated = displayedBands;
                        updated.erase(updated.begin() + i);
                        commitSearchBands(updated);
                        ImGui::PopID();
                        break;
                    }
                    ImGui::PopID();
                    ImGui::Spacing();
                }
            }

            if (ImGui::CollapsingHeader(T("Targets"), ImGuiTreeNodeFlags_DefaultOpen)) {
                float fw = ImGui::GetContentRegionAvail().x;
                if (ImGui::Button("Add Current Frequency as Target", ImVec2(fw, 0))) {
                    json row;
                    row["name"] = "Target";
                    row["frequency"] = gui::freqSelect.frequency;
                    row["bandwidth"] = (vfo != NULL) ? vfo->bandwidth : 12500.0;
                    row["enabled"] = true;
                    json updated = displayedTargets;
                    updated.push_back(row);
                    commitTargets(updated);
                }
                ImGui::SetNextItemWidth(fw);
                if (ImGui::InputDouble("##TargetHz", &newTargetFreq, 0, 0, "%.0f Hz")) {}
                ImGui::SameLine(0,0); ImGui::TextDisabled(" Target Hz");
                scrollToActive();
                ImGui::SetNextItemWidth(fw);
                if (ImGui::InputDouble("##TargetBW", &newTargetBandwidth, 0, 0, "%.0f Hz BW")) {}
                ImGui::SameLine(0,0); ImGui::TextDisabled(" Bandwidth");
                scrollToActive();
                if (ImGui::Button("From Current View##tgt", ImVec2(fw, 0))) {
                    newTargetFreq = gui::waterfall.getCenterFrequency();
                    newTargetBandwidth = (vfo != NULL) ? vfo->bandwidth : 12500.0;
                }
                if (ImGui::Button("Add Target", ImVec2(fw, 0))) {
                    json row;
                    row["name"] = "Target";
                    row["frequency"] = newTargetFreq;
                    row["bandwidth"] = newTargetBandwidth;
                    row["enabled"] = true;
                    json updated = displayedTargets;
                    updated.push_back(row);
                    commitTargets(updated);
                }
                ImGui::Separator();
                for (int i = 0; i < (int)displayedTargets.size(); i++) {
                    ImGui::PushID(4000 + i);
                    bool enabled = readJsonBool(displayedTargets[i], "enabled", true);
                    if (ImGui::Checkbox("##target_enabled", &enabled)) {
                        json updated = displayedTargets;
                        updated[i]["enabled"] = enabled;
                        commitTargets(updated);
                    }
                    ImGui::SameLine();
                    double targetFrequency = readJsonDouble(displayedTargets[i], "frequency", 0.0);
                    double targetBandwidth = readJsonDouble(displayedTargets[i], "bandwidth", 12500.0);
                    ImGui::TextWrapped("%.3f MHz  BW %.0f Hz", targetFrequency/1e6, targetBandwidth);
                    if (ImGui::Button("Delete##tgt", ImVec2(ImGui::GetContentRegionAvail().x, 0))) {
                        json updated = displayedTargets;
                        updated.erase(updated.begin() + i);
                        commitTargets(updated);
                        ImGui::PopID();
                        break;
                    }
                    ImGui::PopID();
                    ImGui::Spacing();
                }
            }

            if (ImGui::CollapsingHeader(T("Excludes"), ImGuiTreeNodeFlags_DefaultOpen)) {
                float fw = ImGui::GetContentRegionAvail().x;
                if (ImGui::Button("Add Current Frequency as Exclude", ImVec2(fw, 0))) {
                    json row;
                    row["name"] = "Exclude";
                    row["frequency"] = gui::freqSelect.frequency;
                    row["bandwidth"] = (vfo != NULL) ? vfo->bandwidth : 12500.0;
                    row["enabled"] = true;
                    json updated = displayedExcludes;
                    updated.push_back(row);
                    commitExcludes(updated);
                }
                ImGui::SetNextItemWidth(fw);
                if (ImGui::InputDouble("##ExclHz", &newExcludeFreq, 0, 0, "%.0f Hz")) {}
                ImGui::SameLine(0,0); ImGui::TextDisabled(" Exclude Hz");
                scrollToActive();
                ImGui::SetNextItemWidth(fw);
                if (ImGui::InputDouble("##ExclBW", &newExcludeBandwidth, 0, 0, "%.0f Hz BW")) {}
                ImGui::SameLine(0,0); ImGui::TextDisabled(" Bandwidth");
                scrollToActive();
                if (ImGui::Button("From Current View##excl", ImVec2(fw, 0))) {
                    newExcludeFreq = gui::waterfall.getCenterFrequency();
                    newExcludeBandwidth = (vfo != NULL) ? vfo->bandwidth : 12500.0;
                }
                if (ImGui::Button("Add Exclude", ImVec2(fw, 0))) {
                    json row;
                    row["name"] = "Exclude";
                    row["frequency"] = newExcludeFreq;
                    row["bandwidth"] = newExcludeBandwidth;
                    row["enabled"] = true;
                    json updated = displayedExcludes;
                    updated.push_back(row);
                    commitExcludes(updated);
                }
                ImGui::Separator();
                for (int i = 0; i < (int)displayedExcludes.size(); i++) {
                    ImGui::PushID(5000 + i);
                    bool enabled = readJsonBool(displayedExcludes[i], "enabled", true);
                    if (ImGui::Checkbox("##exclude_enabled", &enabled)) {
                        json updated = displayedExcludes;
                        updated[i]["enabled"] = enabled;
                        commitExcludes(updated);
                    }
                    ImGui::SameLine();
                    double excludeFrequency = readJsonDouble(displayedExcludes[i], "frequency", 0.0);
                    double excludeBandwidth = readJsonDouble(displayedExcludes[i], "bandwidth", 12500.0);
                    ImGui::TextWrapped("%.3f MHz  BW %.0f Hz", excludeFrequency/1e6, excludeBandwidth);
                    if (ImGui::Button("Delete##excl", ImVec2(ImGui::GetContentRegionAvail().x, 0))) {
                        json updated = displayedExcludes;
                        updated.erase(updated.begin() + i);
                        commitExcludes(updated);
                        ImGui::PopID();
                        break;
                    }
                    ImGui::PopID();
                    ImGui::Spacing();
                }
            }

            if (ImGui::CollapsingHeader(T("Scan / QuickScan Settings"), ImGuiTreeNodeFlags_DefaultOpen)) {
                // Local mirrors that ImGui binds to. When peer-active, the
                // values seed from the peer snapshot and edits ship a
                // mission.setSettings command; the on-screen value snaps
                // back to the peer's state on the next /v1/state poll.
                int   dwellEdit       = displayedDwellMs;
                int   qsDelayEdit     = displayedQsDelay;
                int   qsDurEdit       = displayedQsDuration;
                float thresholdEdit   = displayedThreshold;
                bool  recordEdit      = displayedRecordAudio;
                bool settingsChanged = false;
                if (ImGui::InputInt("Dwell (ms)", &dwellEdit, 100, 500)) {
                    dwellEdit = std::max<int>(100, dwellEdit);
                    settingsChanged = true;
                }
                if (ImGui::InputInt("QuickScan Delay (ms)", &qsDelayEdit, 50, 250)) {
                    qsDelayEdit = std::max<int>(50, qsDelayEdit);
                    settingsChanged = true;
                }
                if (ImGui::InputInt("QuickScan Duration (ms)", &qsDurEdit, 100, 500)) {
                    qsDurEdit = std::max<int>(100, qsDurEdit);
                    settingsChanged = true;
                }
                if (ImGui::SliderFloat("Threshold", &thresholdEdit, -120.0f, 0.0f, "%.1f dB")) {
                    settingsChanged = true;
                }
                if (ImGui::Checkbox("Record Audio", &recordEdit)) {
                    settingsChanged = true;
                }
                if (settingsChanged) {
                    commitMissionSettings(thresholdEdit, dwellEdit, qsDelayEdit, qsDurEdit, recordEdit);
                }
                if (missionPeerActive) {
                    ImGui::TextDisabled("%s", T("Scan UX preferences below stay local — they govern this controller's UI only."));
                }
                bool scanUxChanged = false;
                scanUxChanged |= ImGui::Checkbox(T("Hold on New Hit"), &predatorHoldOnNewHit);
                scanUxChanged |= ImGui::Checkbox(T("Suppress Duplicate Hits"), &predatorSuppressDuplicateHits);
                if (ImGui::InputInt(T("Duplicate Window (s)"), &predatorDuplicateHitWindowSec, 1, 5)) {
                    predatorDuplicateHitWindowSec = std::clamp<int>(predatorDuplicateHitWindowSec, 1, 600);
                    scanUxChanged = true;
                }
                scanUxChanged |= ImGui::Checkbox(T("Extend Dwell on Strong Hit"), &predatorExtendDwellOnStrongHit);
                if (ImGui::SliderFloat(T("Strong Hit SNR"), &predatorStrongHitSnrDb, 6.0f, 40.0f, "%.1f dB")) {
                    scanUxChanged = true;
                }
                scanUxChanged |= ImGui::Checkbox(T("Classify Auto-Marker"), &predatorClassifyAutoMarker);
                if (scanUxChanged) {
                    savePeakDetectionConfig();
                }
            }

            if (ImGui::CollapsingHeader(T("Peak Detection"), ImGuiTreeNodeFlags_DefaultOpen)) {
                bool peakChanged = false;
                peakChanged |= ImGui::Checkbox(T("Detect Peaks"), &predatorPeakDetectionEnabled);
                if (ImGui::SliderFloat(T("Peak SNR"), &predatorPeakSnrDb, 3.0f, 30.0f, "%.1f dB")) {
                    peakChanged = true;
                }
                if (ImGui::InputDouble(T("Peak Spacing Hz"), &predatorPeakMinSpacingHz, 1000.0, 12500.0, "%.0f")) {
                    predatorPeakMinSpacingHz = std::max<double>(1000.0, predatorPeakMinSpacingHz);
                    peakChanged = true;
                }
                if (ImGui::InputInt(T("Max Peaks / Dwell"), &predatorPeakMaxPerDwell, 1, 5)) {
                    predatorPeakMaxPerDwell = std::clamp<int>(predatorPeakMaxPerDwell, 1, 20);
                    peakChanged = true;
                }
                if (ImGui::InputInt(T("Marker Slots"), &predatorMarkerSlots, 1, 1)) {
                    predatorMarkerSlots = std::clamp<int>(predatorMarkerSlots, 1, 16);
                    peakChanged = true;
                }
                ImGui::TextWrapped("%s", chineseUi ? "\u6383\u63cf\u505c\u7559\u6642\uff0cPredator RF \u6703\u5c07\u901a\u904e\u9580\u6abb\u8207 SNR \u689d\u4ef6\u7684\u5cf0\u503c\u8a18\u9304\u70ba\u96c6\u7fa4\u547d\u4e2d\uff0c\u4e26\u7531\u547d\u4e2d\u9801\u9762\u6307\u6d3e\u6a19\u8a18\u3001\u89e3\u78bc\u5668\u3001\u76ee\u6a19\u6216\u6392\u9664\u3002" : "During scan dwell, Predator RF records peaks that clear threshold and SNR checks as clustered hits. Markers, decoders, targets, and excludes are assigned from the Hits page.");
                if (peakChanged) {
                    savePeakDetectionConfig();
                }
            }

            if (ImGui::CollapsingHeader(T("Operator Note"), ImGuiTreeNodeFlags_DefaultOpen)) {
                ImGui::TextWrapped("This shell carries the Predator RF mission control concepts: mode, search bands, targets, excludes, dwell, quick filters, and map launch.");
            }
            // Mission edits no longer accumulate a "missionChanged" flag —
            // each commit helper above either saves locally or ships a
            // mission.set* command to the active peer in-place.
        }
        else if (predatorTab == PREDATOR_TAB_KUJHAD) {
            // Role selector — toggling changes which workflow runs.
            if (ImGui::CollapsingHeader(T("Role"), ImGuiTreeNodeFlags_DefaultOpen)) {
                int role = predatorRole;
                if (ImGui::Combo(T("Role##kujhad_role"), &role, "Device\0Controller\0")) {
                    predatorRole = role;
                    savePredatorState();
                }
                ImGui::TextWrapped("%s",
                    (predatorRole == PREDATOR_ROLE_DEVICE)
                        ? T("Device: this unit publishes its SDR state to peers and accepts RX-only commands from a Controller.")
                        : T("Controller: this unit pulls state from one or more remote Devices and can take control of any peer."));
            }

            if (predatorRole == PREDATOR_ROLE_DEVICE) {
                if (ImGui::CollapsingHeader(T("Device Server"), ImGuiTreeNodeFlags_DefaultOpen)) {
                    bool enabled = kujhadDeviceServerEnabled;
                    if (ImGui::Checkbox(T("Enable peer access"), &enabled)) {
                        kujhadDeviceServerEnabled = enabled;
                        savePredatorState();
                    }
                    int port = kujhadDeviceListenPort;
                    if (ImGui::InputInt(T("Listen port"), &port)) {
                        kujhadDeviceListenPort = std::clamp(port, 1, 65535);
                        savePredatorState();
                    }
                    char nameBuf[64];
                    std::snprintf(nameBuf, sizeof(nameBuf), "%s", kujhadDeviceName.c_str());
                    if (ImGui::InputText(T("Device name"), nameBuf, sizeof(nameBuf))) {
                        kujhadDeviceName = nameBuf;
                        savePredatorState();
                    }
                    // Optional operator override for the address that will
                    // be advertised to controllers in the identify payload
                    // and shown in Reachable Addresses. Useful when the
                    // detected NICs aren't what the operator wants peers
                    // to dial — for example a NAT'd public IP, a DNS name,
                    // or the inside-VPN address when multi-homed.
                    char advBuf[128];
                    std::snprintf(advBuf, sizeof(advBuf), "%s", kujhadAdvertiseAddress.c_str());
                    if (ImGui::InputText(T("Advertise address (override)"), advBuf, sizeof(advBuf))) {
                        kujhadAdvertiseAddress = advBuf;
                        savePredatorState();
                    }
                    ImGui::TextDisabled("%s", T("Leave empty to publish only the auto-detected NICs below."));
                    char keyBuf[96];
                    std::snprintf(keyBuf, sizeof(keyBuf), "%s", kujhadApiKey.c_str());
                    if (ImGui::InputText(T("API key"), keyBuf, sizeof(keyBuf))) {
                        kujhadApiKey = keyBuf;
                        kujhadServer.setApiKey(kujhadApiKey);
                        savePredatorState();
                    }
                    if (ImGui::Button(T("Regenerate API key"))) {
                        kujhadApiKey = predator::kujhadGenerateApiKey();
                        kujhadServer.setApiKey(kujhadApiKey);
                        savePredatorState();
                    }
                    ImGui::Text("%s: %s", T("Status"), kujhadDeviceServerStatus.c_str());
                    ImGui::Text("%s: %s", T("Listening"), kujhadDeviceServerRunning ? "yes" : "no");
                    ImGui::Text("%s: %d / %s: %d / %s: %d",
                                T("Requests"), kujhadServer.inboundRequests(),
                                T("Commands"), kujhadServer.inboundCommands(),
                                T("Rejected"), kujhadServer.rejectedCommands());

                    // TLS toggle. The change only takes effect on the
                    // next start() — we surface that explicitly so the
                    // operator isn't surprised when the running server
                    // keeps its current mode after the click.
                    bool tlsAvailable = predator::kujhadTlsAvailable();
                    if (!tlsAvailable) {
                        ImGui::TextDisabled("%s", T("TLS not available in this build (OpenSSL not linked)."));
                    }
                    bool tlsToggle = kujhadTlsEnabled;
                    if (!tlsAvailable) ImGui::BeginDisabled();
                    if (ImGui::Checkbox(T("Enable TLS (HTTPS)"), &tlsToggle)) {
                        kujhadTlsEnabled = tlsToggle;
                        savePredatorState();
                    }
                    if (!tlsAvailable) ImGui::EndDisabled();
                    char certBuf[512];
                    char keyBufTls[512];
                    std::snprintf(certBuf, sizeof(certBuf), "%s", kujhadTlsCertPath.c_str());
                    std::snprintf(keyBufTls, sizeof(keyBufTls), "%s", kujhadTlsKeyPath.c_str());
                    if (ImGui::InputText(T("TLS cert (PEM)"), certBuf, sizeof(certBuf))) {
                        kujhadTlsCertPath = certBuf;
                        kujhadTlsFingerprint = predator::kujhadCertFingerprintFromPemFile(kujhadTlsCertPath);
                        savePredatorState();
                    }
                    if (ImGui::InputText(T("TLS key (PEM)"), keyBufTls, sizeof(keyBufTls))) {
                        kujhadTlsKeyPath = keyBufTls;
                        savePredatorState();
                    }
                    if (!tlsAvailable) ImGui::BeginDisabled();
                    if (ImGui::Button(T("Regenerate self-signed cert"))) {
                        // CN follows the device name so peers see a
                        // consistent identity in the cert as well as in
                        // /v1/identify; harmless for fingerprint pinning.
                        std::string cn = kujhadDeviceName.empty() ? std::string("kujhad-device") : kujhadDeviceName;
                        std::string fp = predator::kujhadGenerateSelfSignedCert(kujhadTlsCertPath, kujhadTlsKeyPath, cn);
                        if (!fp.empty()) {
                            kujhadTlsConfigError.clear();
                            kujhadTlsFingerprint = fp;
                            // If the server is running, push the new
                            // cert into the live SSL_CTX so the next
                            // accept uses it. The current connection
                            // count is small so the brief gap is fine.
                            std::string applyErr;
                            kujhadServer.setTlsConfig(kujhadTlsEnabled, kujhadTlsCertPath, kujhadTlsKeyPath, applyErr);
                            if (!applyErr.empty()) kujhadTlsConfigError = applyErr;
                        } else {
                            kujhadTlsConfigError = "cert generation failed (check write perms on cert/key paths)";
                        }
                    }
                    if (!tlsAvailable) ImGui::EndDisabled();
                    if (!kujhadTlsConfigError.empty()) {
                        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.30f, 0.30f, 1.0f));
                        ImGui::TextWrapped("TLS error: %s", kujhadTlsConfigError.c_str());
                        ImGui::PopStyleColor();
                    }
                    if (!kujhadTlsFingerprint.empty()) {
                        ImGui::TextWrapped("%s (SHA-256): %s",
                                           T("Cert fingerprint"), kujhadTlsFingerprint.c_str());
                        ImGui::TextDisabled("%s", T("Paste this fingerprint into a controller's peer config to pin trust."));
                    } else if (kujhadTlsEnabled) {
                        ImGui::TextDisabled("%s", T("No cert on disk yet — click Regenerate above."));
                    }
                    if (kujhadServer.tlsEnabled()) {
                        ImGui::TextWrapped("%s", T("HTTPS active. Controllers must use https:// and pin the fingerprint above."));
                    } else {
                        // Plain HTTP is a hard loopback-only path —
                        // the listener refuses non-127.0.0.0/8 peers
                        // at accept time regardless of how the build
                        // was configured. To accept a remote peer the
                        // operator MUST enable TLS above.
                        ImGui::TextWrapped("%s", T("Plain HTTP — listener accepts loopback (127.0.0.0/8) connections only. Remote controllers will be refused at accept until TLS is enabled above."));
                    }
                }

                if (ImGui::CollapsingHeader(T("Spectrum Stream"), ImGuiTreeNodeFlags_DefaultOpen)) {
                    int interval = kujhadSpectrumIntervalMs.load(std::memory_order_relaxed);
                    if (ImGui::SliderInt(T("Frame interval (ms)"), &interval, 50, 2000)) {
                        kujhadSpectrumIntervalMs.store(std::clamp(interval, 50, 5000), std::memory_order_relaxed);
                        savePredatorState();
                    }
                    int bins = kujhadSpectrumBins.load(std::memory_order_relaxed);
                    if (ImGui::SliderInt(T("Bins per frame"), &bins, 64, 1024)) {
                        kujhadSpectrumBins.store(std::clamp(bins, 32, 1024), std::memory_order_relaxed);
                        savePredatorState();
                    }
                    ImGui::Text("%s: %d", T("Active subscribers"), kujhadServer.activeSpectrumStreams());
                    // Quick estimate: ~6 bytes/bin (JSON float) + ~80 byte
                    // header per frame, times the configured cadence.
                    int curInterval = kujhadSpectrumIntervalMs.load(std::memory_order_relaxed);
                    int curBins     = kujhadSpectrumBins.load(std::memory_order_relaxed);
                    double framesPerSec = 1000.0 / std::max<int>(curInterval, 1);
                    double bytesPerFrame = 80.0 + (double)curBins * 6.0;
                    double kbps = (framesPerSec * bytesPerFrame * 8.0) / 1024.0;
                    ImGui::TextDisabled("~ %.1f kb/s per subscriber (peak)", kbps);
                    ImGui::TextWrapped("%s", T("Bandwidth cap: lower the bin count for slow links, raise the interval for low-watt operations. Defaults trade ~10 kb/s for 5 fps situational awareness."));
                }

                if (ImGui::CollapsingHeader(T("Reachable Addresses"), ImGuiTreeNodeFlags_DefaultOpen)) {
                    if (!kujhadAdvertiseAddress.empty()) {
                        // Operator override always wins; show it first
                        // and highlighted so peers know which entry to
                        // dial.
                        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.65f, 0.20f, 1.0f));
                        ImGui::Text("[OVERRIDE]   %s:%d", kujhadAdvertiseAddress.c_str(), kujhadDeviceListenPort);
                        ImGui::PopStyleColor();
                        ImGui::TextDisabled("%s", T("Published in /v1/identify as advertise=..."));
                        ImGui::Separator();
                    }
                    auto cands = predator::kujhadEnumerateInterfaces();
                    if (cands.empty() && kujhadAdvertiseAddress.empty()) {
                        ImGui::TextDisabled("%s", T("No non-loopback IPv4 interfaces detected."));
                    }
                    for (auto& c : cands) {
                        const char* tag = c.isZerotier ? "[ZT] "
                                       : c.isTailscale ? "[TS] "
                                       : c.isPrivate   ? "[LAN] "
                                       :                 "";
                        ImGui::Text("%s%-12s %s:%d", tag, c.name.c_str(), c.address.c_str(), kujhadDeviceListenPort);
                    }
                    ImGui::TextWrapped("%s", T("ZeroTier and Tailscale interfaces are preferred for cross-instance traffic. Override above to publish a different host (e.g. DNS name, NAT'd IP)."));
                }
            }
            else { // Controller
                if (ImGui::CollapsingHeader(T("Peers"), ImGuiTreeNodeFlags_DefaultOpen)) {
                    json& peers = core::configManager.conf["kujhadPeers"];
                    if (!peers.is_array()) peers = json::array();
                    if (peers.empty()) {
                        ImGui::TextDisabled("%s", T("No peers configured. Add one below."));
                    }
                    int delIdx = -1;
                    for (size_t i = 0; i < peers.size(); i++) {
                        ImGui::PushID((int)i);
                        std::string name = readJsonString(peers[i], "name", "peer");
                        std::string host = readJsonString(peers[i], "host", "127.0.0.1");
                        int port = (int)readJsonDouble(peers[i], "port", 41947.0);
                        bool enabled = readJsonBool(peers[i], "enabled", true);
                        bool live = (i < kujhadClients.size() && kujhadClients[i] && kujhadClients[i]->isRunning());
                        predator::KujhadPeerSnapshot snap;
                        if (i < kujhadClients.size() && kujhadClients[i]) snap = kujhadClients[i]->snapshot();
                        ImGui::Text("%s  %s:%d  %s", name.c_str(), host.c_str(), port,
                                    snap.reachable ? "REACHABLE" : (live ? "trying..." : "stopped"));
                        if (snap.reachable) {
                            ImGui::SameLine();
                            ImGui::TextDisabled("(%dms)", snap.linkLatencyMs);
                        }
                        if (ImGui::Checkbox(T("Enabled"), &enabled)) {
                            peers[i]["enabled"] = enabled;
                            saveKujhadPeers(peers);
                        }
                        ImGui::SameLine();
                        bool isActive = ((int)i == kujhadActivePeerIdx);
                        if (ImGui::RadioButton(T("Take control"), isActive)) {
                            kujhadActivePeerIdx = (int)i;
                        }
                        ImGui::SameLine();
                        if (ImGui::Button(T("Remove"))) {
                            delIdx = (int)i;
                        }
                        if (snap.reachable && snap.identify.is_object()) {
                            ImGui::TextDisabled("device=%s role=%s",
                                snap.identify.value("device", std::string("?")).c_str(),
                                snap.identify.value("role", std::string("?")).c_str());
                        }
                        if (!snap.lastError.empty() && !snap.reachable) {
                            ImGui::TextDisabled("err: %s", snap.lastError.c_str());
                        }
                        // Per-peer TLS controls. Toggle changes are
                        // persisted immediately; the diff loop above
                        // tears the worker down and reconnects with the
                        // new mode on the next frame.
                        bool peerTls = readJsonBool(peers[i], "tls", false);
                        std::string peerPin = readJsonString(peers[i], "pinnedFingerprint", "");
                        if (ImGui::Checkbox(T("TLS"), &peerTls)) {
                            peers[i]["tls"] = peerTls;
                            saveKujhadPeers(peers);
                        }
                        ImGui::SameLine();
                        char pinBuf[128];
                        std::snprintf(pinBuf, sizeof(pinBuf), "%s", peerPin.c_str());
                        if (ImGui::InputText(T("Pinned fingerprint"), pinBuf, sizeof(pinBuf))) {
                            peers[i]["pinnedFingerprint"] = std::string(pinBuf);
                            saveKujhadPeers(peers);
                        }
                        // Trust-on-first-use helper: copy the peer's
                        // currently observed cert fingerprint into the
                        // pinned slot so subsequent connections refuse
                        // any cert change. Only meaningful when TLS is
                        // on AND a handshake has succeeded.
                        std::string seen = (i < kujhadClients.size() && kujhadClients[i])
                                           ? kujhadClients[i]->lastSeenFingerprint() : std::string();
                        if (!seen.empty()) {
                            ImGui::TextDisabled("seen: %s", seen.c_str());
                            ImGui::SameLine();
                            if (ImGui::Button(T("Trust now"))) {
                                peers[i]["pinnedFingerprint"] = seen;
                                saveKujhadPeers(peers);
                            }
                        } else if (peerTls) {
                            ImGui::TextDisabled("%s", T("(no cert observed yet)"));
                        }
                        if (i < kujhadClients.size() && kujhadClients[i] && kujhadClients[i]->fingerprintMismatch()) {
                            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.30f, 0.30f, 1.0f));
                            ImGui::TextWrapped("%s", T("Peer cert fingerprint does NOT match the pin. Refusing to send the API key. Investigate before clicking Trust now."));
                            ImGui::PopStyleColor();
                        }
                        ImGui::Separator();
                        ImGui::PopID();
                    }
                    if (delIdx >= 0) {
                        peers.erase(peers.begin() + delIdx);
                        saveKujhadPeers(peers);
                        if (kujhadActivePeerIdx == delIdx) kujhadActivePeerIdx = -1;
                    }
                }

                if (ImGui::CollapsingHeader(T("Add Peer"), ImGuiTreeNodeFlags_DefaultOpen)) {
                    ImGui::InputText(T("Name"), kujhadAddPeerName, sizeof(kujhadAddPeerName));
                    ImGui::InputText(T("Host"), kujhadAddPeerHost, sizeof(kujhadAddPeerHost));
                    ImGui::InputInt(T("Port"), &kujhadAddPeerPort);
                    ImGui::InputText(T("API Key"), kujhadAddPeerKey, sizeof(kujhadAddPeerKey));
                    // New peers default to TLS-off so existing
                    // loopback/overlay setups keep working without
                    // surprise. The operator flips the per-row toggle
                    // (and pastes the fingerprint) once the device is
                    // verified.
                    static bool kujhadAddPeerTls = false;
                    static char kujhadAddPeerPin[128] = {0};
                    if (!predator::kujhadTlsAvailable()) ImGui::BeginDisabled();
                    ImGui::Checkbox(T("TLS (HTTPS)"), &kujhadAddPeerTls);
                    if (!predator::kujhadTlsAvailable()) ImGui::EndDisabled();
                    ImGui::SameLine();
                    ImGui::InputText(T("Pinned fingerprint (optional)"), kujhadAddPeerPin, sizeof(kujhadAddPeerPin));
                    if (ImGui::Button(T("Add peer"))) {
                        if (kujhadAddPeerName[0] && kujhadAddPeerHost[0]) {
                            json& peers = core::configManager.conf["kujhadPeers"];
                            if (!peers.is_array()) peers = json::array();
                            json p;
                            p["name"]    = std::string(kujhadAddPeerName);
                            p["host"]    = std::string(kujhadAddPeerHost);
                            p["port"]    = std::clamp(kujhadAddPeerPort, 1, 65535);
                            p["apiKey"]  = std::string(kujhadAddPeerKey);
                            p["enabled"] = true;
                            p["tls"]     = kujhadAddPeerTls;
                            p["pinnedFingerprint"] = std::string(kujhadAddPeerPin);
                            peers.push_back(p);
                            saveKujhadPeers(peers);
                            kujhadAddPeerName[0] = 0;
                            kujhadAddPeerHost[0] = 0;
                            kujhadAddPeerKey[0]  = 0;
                            kujhadAddPeerPin[0]  = 0;
                            kujhadAddPeerTls     = false;
                        }
                    }
                }

                if (ImGui::CollapsingHeader(T("View Source"), ImGuiTreeNodeFlags_DefaultOpen)) {
                    bool mirror = kujhadMirrorPeerSpectrum;
                    if (ImGui::Checkbox(T("Mirror active peer spectrum"), &mirror)) {
                        kujhadMirrorPeerSpectrum = mirror;
                        savePredatorState();
                    }
                    ImGui::TextDisabled("%s", mirror
                        ? T("Local waterfall shows PEER spectrum. Tuner controls still affect the local SDR.")
                        : T("Local waterfall shows LOCAL SDR spectrum."));
                    if (kujhadActivePeerIdx >= 0 && kujhadActivePeerIdx < (int)kujhadClients.size()
                        && kujhadClients[kujhadActivePeerIdx]) {
                        auto& client = kujhadClients[kujhadActivePeerIdx];
                        bool streaming = client->spectrumStreaming();
                        uint64_t frames = client->spectrumFramesReceived();
                        ImGui::Text("%s: %s  (%llu %s)",
                                    T("Stream"), streaming ? "live" : "idle",
                                    (unsigned long long)frames, T("frames"));
                    }
                    else if (mirror) {
                        ImGui::TextDisabled("%s", T("Select 'Take control' on a peer to start the spectrum stream."));
                    }
                }

                if (ImGui::CollapsingHeader(T("Active Peer Commands"), ImGuiTreeNodeFlags_DefaultOpen)) {
                    if (kujhadActivePeerIdx < 0 || kujhadActivePeerIdx >= (int)kujhadClients.size()) {
                        ImGui::TextDisabled("%s", T("Select 'Take control' on a peer above to enable commands."));
                    }
                    else {
                        auto& client = kujhadClients[kujhadActivePeerIdx];
                        static double cmdFreq = 433920000.0;
                        ImGui::InputDouble(T("Frequency Hz"), &cmdFreq, 1000.0, 1000000.0, "%.0f");
                        if (ImGui::Button(T("Send tune.set"))) {
                            std::string err;
                            json args; args["frequencyHz"] = cmdFreq;
                            bool ok = client && client->sendCommand("tune", "set", args, err);
                            kujhadStatusBanner = ok ? std::string(T("tune.set ok"))
                                                    : (std::string(T("tune.set failed: ")) + err);
                        }
                        ImGui::SameLine();
                        if (ImGui::Button(T("Identify"))) {
                            std::string err;
                            bool ok = client && client->sendCommand("identify", "ping", json::object(), err);
                            kujhadStatusBanner = ok ? std::string(T("identify ok"))
                                                    : (std::string(T("identify failed: ")) + err);
                        }
                        ImGui::SameLine();
                        if (ImGui::Button(T("Scan start"))) {
                            std::string err;
                            bool ok = client && client->sendCommand("scan", "start", json::object(), err);
                            kujhadStatusBanner = ok ? std::string(T("scan.start ok"))
                                                    : (std::string(T("scan.start failed: ")) + err);
                        }
                        ImGui::SameLine();
                        if (ImGui::Button(T("Scan stop"))) {
                            std::string err;
                            bool ok = client && client->sendCommand("scan", "stop", json::object(), err);
                            kujhadStatusBanner = ok ? std::string(T("scan.stop ok"))
                                                    : (std::string(T("scan.stop failed: ")) + err);
                        }
                        if (!kujhadStatusBanner.empty()) {
                            ImGui::TextWrapped("%s", kujhadStatusBanner.c_str());
                        }
                    }
                }
            }

            if (ImGui::CollapsingHeader(T("Protocol"), ImGuiTreeNodeFlags_DefaultOpen)) {
                ImGui::TextWrapped("%s", T(
                    "Kujhad v1 protocol: HTTP/JSON over a private overlay or loopback. "
                    "Auth = X-Kujhad-Key header. Endpoints: /v1/identify, /v1/state, "
                    "/v1/gps, /v1/events, /v1/command. Command classes: tune, scan, "
                    "mission, identify. The tx.* class is rejected at the wire (RX-only build)."));
            }
        }
        else {
            if (ImGui::CollapsingHeader(T("Language"), ImGuiTreeNodeFlags_DefaultOpen)) {
                int languageIndex = (predatorLanguage == "zh-Hant") ? 1 : 0;
                if (ImGui::Combo(T("Language"), &languageIndex, "English\0Traditional Chinese\0")) {
                    predatorLanguage = (languageIndex == 1) ? "zh-Hant" : "en-US";
                    savePredatorState();
                }
                ImGui::TextWrapped("%s", chineseUi ? "\u8a9e\u8a00\u5207\u63db\u6703\u7acb\u5373\u5957\u7528\u65bc Predator RF \u4efb\u52d9\u4ecb\u9762\u3002" : "Language changes apply immediately to the Predator RF mission interface.");
            }
            if (ImGui::CollapsingHeader(T("Source & Device"), ImGuiTreeNodeFlags_DefaultOpen)) {
                sourcemenu::draw(NULL);
            }
            if (ImGui::CollapsingHeader(T("Audio / Sinks"), ImGuiTreeNodeFlags_DefaultOpen)) {
                sinkmenu::draw(NULL);
            }
            if (ImGui::CollapsingHeader(T("Display & Band Plan"), ImGuiTreeNodeFlags_DefaultOpen)) {
                displaymenu::draw(NULL);
                bandplanmenu::draw(NULL);
            }
            if (ImGui::CollapsingHeader(T("Appearance"), ImGuiTreeNodeFlags_DefaultOpen)) {
                thememenu::draw(NULL);
                vfo_color_menu::draw(NULL);
            }
            if (ImGui::CollapsingHeader(T("Module Manager"), ImGuiTreeNodeFlags_DefaultOpen)) {
                module_manager_menu::draw(NULL);
            }
            if (ImGui::CollapsingHeader(T("Status"), ImGuiTreeNodeFlags_DefaultOpen)) {
                ImGui::Text("Mission Mode: %s", missionModes[predatorMissionMode]);
                ImGui::Text("Selected Source: %s", sourceName.empty() ? T("None") : sourceName.c_str());
                ImGui::Text("Playback State: %s", playing ? T("Streaming") : T("Idle"));
                ImGui::Text("Center Frequency: %.0f Hz", gui::waterfall.getCenterFrequency());
                ImGui::Text("GPS Fix: %s", phoneHasFix ? T("Ready") : T("Waiting"));
                if (phoneHasFix) {
                    ImGui::Text("GPS: %.6f, %.6f  +/-%.1fm", phoneLat, phoneLon, phoneAccuracy);
                }
                ImGui::TextWrapped("Maps are now wired through the phone GPS path. DF remains intentionally excluded.");
            }
            if (ImGui::CollapsingHeader(T("Session Export"), ImGuiTreeNodeFlags_DefaultOpen)) {
                ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
                if (ImGui::InputTextMultiline("Note##session_note", sessionNoteBuf, sizeof(sessionNoteBuf), ImVec2(ImGui::GetContentRegionAvail().x, 96.0f * style::uiScale))) {
                    saveSessionNote(std::string(sessionNoteBuf));
                }
                if (ImGui::Button(T("Export Session JSON"), ImVec2(ImGui::GetContentRegionAvail().x, 0))) {
                    exportPredatorSession();
                }
                if (!exportStatus.empty()) {
                    ImGui::TextWrapped("%s", exportStatus.c_str());
                }
            }
            if (ImGui::CollapsingHeader(T("Legacy Advanced Menus"), ImGuiTreeNodeFlags_DefaultOpen)) {
                if (gui::menu.draw(firstMenuRender)) {
                    saveLegacyMenuState();
                }
                if (startedWithMenuClosed) {
                    startedWithMenuClosed = false;
                }
                else {
                    firstMenuRender = false;
                }
            }
            if (ImGui::CollapsingHeader(T("Debug"), ImGuiTreeNodeFlags_DefaultOpen)) {
                ImGui::Text("Frame time: %.3f ms/frame", ImGui::GetIO().DeltaTime * 1000.0f);
                ImGui::Text("Framerate: %.1f FPS", ImGui::GetIO().Framerate);
                ImGui::Checkbox("Show demo window", &demoWindow);
                ImGui::Text("ImGui version: %s", ImGui::GetVersion());

                if (ImGui::Button("Open Credits")) {
                    showCredits = true;
                }
                if (ImGui::Button("Refresh Legacy Menu")) {
                    firstMenuRender = true;
                }

                ImGui::Checkbox("WF Single Click", &gui::waterfall.VFOMoveSingleClick);
                ImGui::Checkbox("Lock Menu Order", &gui::menu.locked);
            }
        }

        applyTouchScroll();
        ImGui::EndChild();
        ImGui::EndChild();
        ImGui::PopStyleColor();
    }

    if (!lockWaterfallControls) {
        // Handle arrow keys
        if (vfo != NULL && (gui::waterfall.mouseInFFT || gui::waterfall.mouseInWaterfall)) {
            bool freqChanged = false;
            if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow) && !gui::freqSelect.digitHovered) {
                double nfreq = gui::waterfall.getCenterFrequency() + vfo->generalOffset - vfo->snapInterval;
                nfreq = roundl(nfreq / vfo->snapInterval) * vfo->snapInterval;
                tuner::tune(tuningMode, gui::waterfall.selectedVFO, nfreq);
                freqChanged = true;
            }
            if (ImGui::IsKeyPressed(ImGuiKey_RightArrow) && !gui::freqSelect.digitHovered) {
                double nfreq = gui::waterfall.getCenterFrequency() + vfo->generalOffset + vfo->snapInterval;
                nfreq = roundl(nfreq / vfo->snapInterval) * vfo->snapInterval;
                tuner::tune(tuningMode, gui::waterfall.selectedVFO, nfreq);
                freqChanged = true;
            }
            if (freqChanged) {
                core::configManager.acquire();
                core::configManager.conf["frequency"] = gui::waterfall.getCenterFrequency();
                if (vfo != NULL) {
                    core::configManager.conf["vfoOffsets"][gui::waterfall.selectedVFO] = vfo->generalOffset;
                }
                core::configManager.release(true);
            }
        }

        // Handle scrollwheel
        int wheel = ImGui::GetIO().MouseWheel;
        if (wheel != 0 && (gui::waterfall.mouseInFFT || gui::waterfall.mouseInWaterfall)) {
            // Select factor depending on modifier keys
            double interval;
            if (ImGui::IsKeyDown(ImGuiKey_LeftShift)) {
                interval = vfo->snapInterval * 10.0;
            }
            else if (ImGui::IsKeyDown(ImGuiKey_LeftAlt)) {
                interval = vfo->snapInterval * 0.1;
            }
            else {
                interval = vfo->snapInterval;
            }

            double nfreq;
            if (vfo != NULL) {
                nfreq = gui::waterfall.getCenterFrequency() + vfo->generalOffset + (interval * wheel);
                nfreq = roundl(nfreq / interval) * interval;
            }
            else {
                nfreq = gui::waterfall.getCenterFrequency() - (gui::waterfall.getViewBandwidth() * wheel / 20.0);
            }
            tuner::tune(tuningMode, gui::waterfall.selectedVFO, nfreq);
            gui::freqSelect.setFrequency(nfreq);
            core::configManager.acquire();
            core::configManager.conf["frequency"] = gui::waterfall.getCenterFrequency();
            if (vfo != NULL) {
                core::configManager.conf["vfoOffsets"][gui::waterfall.selectedVFO] = vfo->generalOffset;
            }
            core::configManager.release(true);
        }
    }

    ImGui::SetCursorPos(ImVec2(railX, contentTop));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.09f, 0.11f, 0.08f, 0.96f));
    ImGuiWindowFlags railFlags = backend::isTouchPrimary()
        ? ImGuiWindowFlags_AlwaysVerticalScrollbar
        : ImGuiWindowFlags_None;
    ImGui::BeginChild("PredatorRightRail", ImVec2(railWidth, contentHeight), true, railFlags);

    for (int i = 0; i < 7; i++) {
        bool activeTab = (predatorTab == i);
        if (activeTab) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.28f, 0.39f, 0.21f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.32f, 0.45f, 0.24f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.35f, 0.50f, 0.27f, 1.0f));
        }
        if (ImGui::Button(tabLabels[i], ImVec2(ImGui::GetContentRegionAvail().x, 36.0f * style::uiScale))) {
            if (predatorTab == i && showMenu) {
                showMenu = false;
            }
            else {
                predatorTab = i;
                showMenu = true;
            }
            savePredatorState();
        }
        if (activeTab) {
            ImGui::PopStyleColor(3);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", tabTitles[i]);
        }
    }

    // Zoom / Max / Min sliders moved out of the rail and into the
    // top-right dropdown overlay above the waterfall (see the
    // "Top-right Zoom/Max/Min dropdown" block earlier in draw()).

    if (backend::isTouchPrimary() && ImGui::GetScrollMaxY() > 0.0f) {
        static bool  s_dragArmed   = false;
        static bool  s_dragActive  = false;
        static float s_scrollStart = 0.0f;
        const float dragThreshold = 12.0f * style::uiScale;

        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            bool noActiveItem = (ImGui::GetActiveID() == 0);

            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                s_dragArmed   = ImGui::IsWindowHovered() && noActiveItem;
                s_dragActive  = false;
                s_scrollStart = ImGui::GetScrollY();
            }

            if (!noActiveItem) {
                s_dragArmed  = false;
                s_dragActive = false;
            }

            if (s_dragArmed && !s_dragActive) {
                float dy = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left, 0.0f).y;
                if (std::fabs(dy) > dragThreshold) s_dragActive = true;
            }

            if (s_dragActive) {
                float dy = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left, 0.0f).y;
                float target = s_scrollStart - dy;
                float maxY = ImGui::GetScrollMaxY();
                if (target < 0.0f) target = 0.0f;
                if (target > maxY) target = maxY;
                ImGui::SetScrollY(target);
            }
        }
        else {
            s_dragArmed  = false;
            s_dragActive = false;
        }
    }

    ImGui::EndChild();
    ImGui::PopStyleColor();

    gui::waterfall.setFFTMin(fftMin);
    gui::waterfall.setFFTMax(fftMax);
    gui::waterfall.setWaterfallMin(fftMin);
    gui::waterfall.setWaterfallMax(fftMax);

    // === Soft-keyboard inset compensation ====================================
    // When the Android IME is up, treat the bottom strip of DisplaySize
    // covered by the keyboard as unusable. If ImGui has an active text
    // widget whose host window extends INTO that strip, scroll the
    // closest scrollable ancestor up by the overlap so the input lands
    // back in the visible region. Walks up ParentWindow because the
    // operator might be editing a field inside a BeginChild that is
    // itself non-scrollable (the parent menu does the scrolling).
    //
    // The compensation MUST only fire on a true edge — IME just opened
    // or the focused widget just changed. The window's screen rect
    // (Pos / Size) does NOT change when SetScrollY scrolls its content,
    // so re-running the calc every frame would add `overlap` to scroll
    // each frame and silently drag the focused field all the way down
    // to ScrollMax. The state below detects the edge once per
    // open / focus transition and locks out subsequent re-applications.
    {
        int imeBottom = backend::getImeBottomInsetPx();
        ImGuiContext* gctx = ImGui::GetCurrentContext();
        static int       s_imeLastBottom    = 0;
        static ImGuiID   s_imeLastActiveId  = 0;
        ImGuiID curActiveId = (gctx != nullptr) ? gctx->ActiveId : 0;
        // Retrigger when:
        //  • IME just appeared (last == 0, now > 0), OR
        //  • the focused widget changed while the IME stayed up, OR
        //  • the IME GREW (e.g. an autocomplete / suggestions panel
        //    expanded under the keyboard mid-session) so an input that
        //    was visible a frame ago is now covered. Shrinking the IME
        //    or closing it is intentionally NOT a retrigger — we don't
        //    want to snap content back and surprise the operator.
        bool    edge        = (imeBottom > 0)
                           && (curActiveId != 0)
                           && (s_imeLastBottom == 0
                                || s_imeLastActiveId != curActiveId
                                || imeBottom > s_imeLastBottom);
        if (edge && gctx != nullptr && gctx->ActiveIdWindow != nullptr) {
            float dispH     = ImGui::GetIO().DisplaySize.y;
            float visBottom = dispH - (float)imeBottom;
            ImGuiWindow* w = gctx->ActiveIdWindow;
            while (w != nullptr) {
                float winBottomScreen = w->Pos.y + w->Size.y;
                if (winBottomScreen > visBottom) {
                    float overlap = winBottomScreen - visBottom;
                    float curY    = w->Scroll.y;
                    float maxY    = w->ScrollMax.y;
                    float targetY = std::min(curY + overlap, maxY);
                    if (targetY > curY + 0.5f) {
                        ImGui::SetScrollY(w, targetY);
                        break;
                    }
                }
                w = w->ParentWindow;
            }
        }
        s_imeLastBottom   = imeBottom;
        s_imeLastActiveId = curActiveId;
    }

    ImGui::End();

    if (showCredits) {
        credits::show();
    }

    if (demoWindow) {
        ImGui::ShowDemoWindow();
    }
}

void MainWindow::setPlayState(bool _playing) {
    if (_playing == playing) { return; }
    if (_playing) {
        if (sigpath::sourceManager.getSelectedSourceName().empty()) {
            predatorScanStatus = "Select SDR first";
            predatorTab = PREDATOR_TAB_SYSTEM;
            showMenu = true;
            return;
        }
        sigpath::iqFrontEnd.flushInputBuffer();
        sigpath::sourceManager.start();
        sigpath::sourceManager.tune(gui::waterfall.getCenterFrequency());
        playing = true;
        onPlayStateChange.emit(true);
    }
    else {
        playing = false;
        onPlayStateChange.emit(false);
        sigpath::sourceManager.stop();
        sigpath::iqFrontEnd.flushInputBuffer();
    }
}

void MainWindow::setViewBandwidthSlider(float bandwidth) {
    bw = bandwidth;
}

bool MainWindow::sdrIsRunning() {
    return playing;
}

bool MainWindow::isPlaying() {
    return playing;
}

void MainWindow::setFirstMenuRender() {
    firstMenuRender = true;
}
