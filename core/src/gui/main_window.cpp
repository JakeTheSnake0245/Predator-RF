#include <gui/main_window.h>
#include <gui/gui.h>
#include "imgui.h"
#include <stdio.h>
#include <thread>
#include <complex>
#include <gui/widgets/waterfall.h>
#include <gui/widgets/frequency_select.h>
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
#include <signal_path/source.h>
#include <gui/dialogs/loading_screen.h>
#include <gui/colormaps.h>
#include <gui/widgets/snr_meter.h>
#include <gui/tuner.h>
#include <backend.h>

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
    predatorQuickFilter = std::clamp<int>((int)core::configManager.conf["predatorQuickFilter"], 0, 3);

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
    return gui::waterfall.getFFTBuffer();
}

void MainWindow::releaseFFTBuffer(void* ctx) {
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

    const char* missionModes[] = {
        "Manual",
        "Classify",
        "Scan",
        "QuickScan"
    };

    const char* missionModeDescriptions[] = {
        "Direct operator tuning and marker ownership.",
        "Keep manual control while idle resources watch the band.",
        "Automated search and target workflow across configured bands.",
        "Rapid single-marker sweep for quick checks."
    };

    const char* tabLabels[] = {
        "SPEC",
        "HITS",
        "NET",
        "MAP",
        "MIS",
        "SYS"
    };

    const char* tabTitles[] = {
        "Spectrum",
        "Hits & Events",
        "Network",
        "Map",
        "Mission Config",
        "System"
    };

    const char* tabDescriptions[] = {
        "Tune, shape, and monitor the live spectrum picture.",
        "Review operational queues, filters, and retained frequencies of interest.",
        "Hold the Predator SDR navigation slot for decoder-backed structure and labels.",
        "Launch the touch-first phone map tied to handset GPS.",
        "Drive search bands, targets, excludes, dwell, and quick-scan workflow.",
        "Health, theme, legacy modules, and operator-level status."
    };

    const char* quickFilterLabels[] = {
        "All",
        "Target",
        "Exclude",
        "Unknown"
    };

    auto savePredatorState = [&]() {
        core::configManager.acquire();
        core::configManager.conf["showMenu"] = showMenu;
        core::configManager.conf["predatorMissionMode"] = predatorMissionMode;
        core::configManager.conf["predatorTab"] = predatorTab;
        core::configManager.conf["predatorQuickFilter"] = predatorQuickFilter;
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
        if (ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) &&
            ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f) &&
            !ImGui::IsAnyItemActive()) {
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
    float missionThreshold = -55.0f;
    int dwellMs = 1000;
    int quickScanDelayMs = 250;
    int quickScanDurationMs = 5000;
    bool recordAudio = true;
    core::configManager.acquire();
    searchBands = core::configManager.conf["predatorSearchBands"];
    targets = core::configManager.conf["predatorTargets"];
    excludes = core::configManager.conf["predatorExcludes"];
    missionThreshold = core::configManager.conf["predatorThreshold"];
    dwellMs = core::configManager.conf["predatorDwellMs"];
    quickScanDelayMs = core::configManager.conf["predatorQuickScanDelayMs"];
    quickScanDurationMs = core::configManager.conf["predatorQuickScanDurationMs"];
    recordAudio = core::configManager.conf["predatorRecordAudio"];
    core::configManager.release();

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

    auto readJsonString = [](const json& row, const char* key, const char* fallback) {
        if (!row.is_object() || !row.contains(key) || !row[key].is_string()) {
            return std::string(fallback);
        }
        return (std::string)row[key];
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
    ImGui::TextUnformatted("SDR Predator");

    ImGui::SameLine();
    if (drawBadge(playing ? "LIVE" : (sourceName.empty() ? "NO SDR" : "READY"), playing ? ImVec4(0.42f, 0.78f, 0.48f, 1.0f) : (sourceName.empty() ? ImVec4(0.83f, 0.63f, 0.24f, 1.0f) : ImVec4(0.64f, 0.71f, 0.41f, 1.0f))) && !(playButtonLocked && !playing)) {
        setPlayState(!playing);
    }

    ImGui::SameLine();
    if (drawBadge(sourceName.empty() ? "Select SDR" : sourceName.c_str(), ImVec4(0.73f, 0.70f, 0.45f, 1.0f))) {
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
    if (drawBadge(phoneHasFix ? "GPS READY" : "GPS WAIT", phoneHasFix ? ImVec4(0.55f, 0.74f, 0.46f, 1.0f) : ImVec4(0.45f, 0.49f, 0.41f, 1.0f))) {
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

    lockWaterfallControls = showMenu;

    ImGui::SetCursorPos(ImVec2(pad, contentTop));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.04f, 0.05f, 0.04f, 0.98f));
    ImGui::BeginChild("Waterfall", ImVec2(waterfallWidth, contentHeight), true);
    gui::waterfall.draw();
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
            if (ImGui::CollapsingHeader("Display Controls", ImGuiTreeNodeFlags_DefaultOpen)) {
                displaymenu::draw(NULL);
            }
            if (ImGui::CollapsingHeader("Band Plan", ImGuiTreeNodeFlags_DefaultOpen)) {
                bandplanmenu::draw(NULL);
            }
            if (ImGui::CollapsingHeader("Current Mission Mode", ImGuiTreeNodeFlags_DefaultOpen)) {
                ImGui::TextUnformatted(missionModes[predatorMissionMode]);
                ImGui::TextWrapped("%s", missionModeDescriptions[predatorMissionMode]);
            }
            if (ImGui::CollapsingHeader("Quick Actions", ImGuiTreeNodeFlags_DefaultOpen)) {
                if (ImGui::Button("Open SDR / Settings", ImVec2(ImGui::GetContentRegionAvail().x, 0))) {
                    predatorTab = PREDATOR_TAB_SYSTEM;
                    savePredatorState();
                }
                if (ImGui::Button("Open Mission Config", ImVec2(ImGui::GetContentRegionAvail().x, 0))) {
                    predatorTab = PREDATOR_TAB_MISSION;
                    savePredatorState();
                }
            }
        }
        else if (predatorTab == PREDATOR_TAB_HITS) {
            if (ImGui::CollapsingHeader("Quick Filter", ImGuiTreeNodeFlags_DefaultOpen)) {
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
                ImGui::TextWrapped("Live hit/event clustering is not fully wired yet, but the operational lists and filter shell now match the Predator SDR flow we are building toward.");
            }

            auto drawFreqRows = [&](const char* header, json& rows, bool showBandwidth) {
                if (!ImGui::CollapsingHeader(header, ImGuiTreeNodeFlags_DefaultOpen)) { return false; }
                bool changed = false;
                if (rows.empty()) {
                    ImGui::TextDisabled("No entries.");
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
        }
        else if (predatorTab == PREDATOR_TAB_NETWORK) {
            if (ImGui::CollapsingHeader("Network Workflow", ImGuiTreeNodeFlags_DefaultOpen)) {
                ImGui::TextWrapped("The Predator SDR network tab is reserved for decoder-backed structure, talkgroups, and node promotion. Navigation and operator intent is preserved while decoder/event wiring catches up.");
            }
            if (ImGui::CollapsingHeader("Current Workflow Assets", ImGuiTreeNodeFlags_DefaultOpen)) {
                ImGui::Text("Search Bands: %d", (int)searchBands.size());
                ImGui::Text("Targets: %d", (int)targets.size());
                ImGui::Text("Excludes: %d", (int)excludes.size());
                ImGui::TextWrapped("Use the Mission tab to define the operational picture. Those lists will feed future network and event promotion paths.");
            }
        }
        else if (predatorTab == PREDATOR_TAB_MAP) {
            if (ImGui::CollapsingHeader("Phone Map", ImGuiTreeNodeFlags_DefaultOpen)) {
                ImGui::TextWrapped("The tactical map launches as a dedicated Android touch screen so pan, zoom, and pinch behavior feel like a normal map app instead of an ImGui widget.");
                if (phoneHasFix) {
                    ImGui::Text("Phone GPS: %.6f, %.6f", phoneLat, phoneLon);
                    ImGui::Text("Accuracy: %.1f m", phoneAccuracy);
                }
                else {
                    ImGui::TextDisabled("Phone GPS fix not available yet.");
                }
                if (ImGui::Button("Open Tactical Map", ImVec2(ImGui::GetContentRegionAvail().x, 0))) {
                    backend::openMapView();
                }
            }
            if (ImGui::CollapsingHeader("DF Status", ImGuiTreeNodeFlags_DefaultOpen)) {
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

            if (ImGui::CollapsingHeader("Mission Modes", ImGuiTreeNodeFlags_DefaultOpen)) {
                for (int i = 0; i < 4; i++) {
                    bool activeMode = (predatorMissionMode == i);
                    if (activeMode) {
                        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.28f, 0.39f, 0.21f, 1.0f));
                        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.32f, 0.45f, 0.24f, 1.0f));
                        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.35f, 0.50f, 0.27f, 1.0f));
                    }
                    if (ImGui::Button(missionModes[i], ImVec2(ImGui::GetContentRegionAvail().x, 0))) {
                        setMissionMode(i);
                    }
                    if (activeMode) {
                        ImGui::PopStyleColor(3);
                    }
                    ImGui::TextWrapped("%s", missionModeDescriptions[i]);
                    if (i < 3) { ImGui::Spacing(); }
                }
            }

            bool missionChanged = false;

            if (ImGui::CollapsingHeader("Search Bands", ImGuiTreeNodeFlags_DefaultOpen)) {
                if (ImGui::InputText("Band Name", newBandName, sizeof(newBandName))) {}
                if (ImGui::InputDouble("Start Hz", &newBandStart, 1000.0, 100000.0, "%.0f")) {}
                if (ImGui::InputDouble("Stop Hz", &newBandStop, 1000.0, 100000.0, "%.0f")) {}
                if (ImGui::Button("Add Search Band", ImVec2(ImGui::GetContentRegionAvail().x, 0))) {
                    json row;
                    row["name"] = std::string(newBandName);
                    row["start"] = std::min(newBandStart, newBandStop);
                    row["stop"] = std::max(newBandStart, newBandStop);
                    row["enabled"] = true;
                    searchBands.push_back(row);
                    missionChanged = true;
                }
                ImGui::Separator();
                for (int i = 0; i < searchBands.size(); i++) {
                    ImGui::PushID(3000 + i);
                    bool enabled = readJsonBool(searchBands[i], "enabled", true);
                    if (ImGui::Checkbox("##search_enabled", &enabled)) {
                        searchBands[i]["enabled"] = enabled;
                        missionChanged = true;
                    }
                    ImGui::SameLine();
                    std::string bandName = readJsonString(searchBands[i], "name", "Band");
                    double bandStart = readJsonDouble(searchBands[i], "start", 0.0);
                    double bandStop = readJsonDouble(searchBands[i], "stop", 0.0);
                    ImGui::Text("%s  %.0f - %.0f Hz", bandName.c_str(), bandStart, bandStop);
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Delete")) {
                        searchBands.erase(searchBands.begin() + i);
                        missionChanged = true;
                        ImGui::PopID();
                        break;
                    }
                    ImGui::PopID();
                }
            }

            if (ImGui::CollapsingHeader("Targets", ImGuiTreeNodeFlags_DefaultOpen)) {
                if (ImGui::Button("Use Current Frequency as Target", ImVec2(ImGui::GetContentRegionAvail().x, 0))) {
                    json row;
                    row["name"] = "Current Target";
                    row["frequency"] = gui::freqSelect.frequency;
                    row["bandwidth"] = (vfo != NULL) ? vfo->bandwidth : 12500.0;
                    row["enabled"] = true;
                    targets.push_back(row);
                    missionChanged = true;
                }
                if (ImGui::InputDouble("Target Hz", &newTargetFreq, 1000.0, 100000.0, "%.0f")) {}
                if (ImGui::InputDouble("Target BW", &newTargetBandwidth, 100.0, 1000.0, "%.0f")) {}
                if (ImGui::Button("Add Target", ImVec2(ImGui::GetContentRegionAvail().x, 0))) {
                    json row;
                    row["name"] = "Target";
                    row["frequency"] = newTargetFreq;
                    row["bandwidth"] = newTargetBandwidth;
                    row["enabled"] = true;
                    targets.push_back(row);
                    missionChanged = true;
                }
                ImGui::Separator();
                for (int i = 0; i < targets.size(); i++) {
                    ImGui::PushID(4000 + i);
                    bool enabled = readJsonBool(targets[i], "enabled", true);
                    if (ImGui::Checkbox("##target_enabled", &enabled)) {
                        targets[i]["enabled"] = enabled;
                        missionChanged = true;
                    }
                    ImGui::SameLine();
                    double targetFrequency = readJsonDouble(targets[i], "frequency", 0.0);
                    double targetBandwidth = readJsonDouble(targets[i], "bandwidth", 12500.0);
                    ImGui::Text("%.0f Hz  BW %.0f", targetFrequency, targetBandwidth);
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Delete")) {
                        targets.erase(targets.begin() + i);
                        missionChanged = true;
                        ImGui::PopID();
                        break;
                    }
                    ImGui::PopID();
                }
            }

            if (ImGui::CollapsingHeader("Excludes", ImGuiTreeNodeFlags_DefaultOpen)) {
                if (ImGui::Button("Use Current Frequency as Exclude", ImVec2(ImGui::GetContentRegionAvail().x, 0))) {
                    json row;
                    row["name"] = "Current Exclude";
                    row["frequency"] = gui::freqSelect.frequency;
                    row["bandwidth"] = (vfo != NULL) ? vfo->bandwidth : 12500.0;
                    row["enabled"] = true;
                    excludes.push_back(row);
                    missionChanged = true;
                }
                if (ImGui::InputDouble("Exclude Hz", &newExcludeFreq, 1000.0, 100000.0, "%.0f")) {}
                if (ImGui::InputDouble("Exclude BW", &newExcludeBandwidth, 100.0, 1000.0, "%.0f")) {}
                if (ImGui::Button("Add Exclude", ImVec2(ImGui::GetContentRegionAvail().x, 0))) {
                    json row;
                    row["name"] = "Exclude";
                    row["frequency"] = newExcludeFreq;
                    row["bandwidth"] = newExcludeBandwidth;
                    row["enabled"] = true;
                    excludes.push_back(row);
                    missionChanged = true;
                }
                ImGui::Separator();
                for (int i = 0; i < excludes.size(); i++) {
                    ImGui::PushID(5000 + i);
                    bool enabled = readJsonBool(excludes[i], "enabled", true);
                    if (ImGui::Checkbox("##exclude_enabled", &enabled)) {
                        excludes[i]["enabled"] = enabled;
                        missionChanged = true;
                    }
                    ImGui::SameLine();
                    double excludeFrequency = readJsonDouble(excludes[i], "frequency", 0.0);
                    double excludeBandwidth = readJsonDouble(excludes[i], "bandwidth", 12500.0);
                    ImGui::Text("%.0f Hz  BW %.0f", excludeFrequency, excludeBandwidth);
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Delete")) {
                        excludes.erase(excludes.begin() + i);
                        missionChanged = true;
                        ImGui::PopID();
                        break;
                    }
                    ImGui::PopID();
                }
            }

            if (ImGui::CollapsingHeader("Scan / QuickScan Settings", ImGuiTreeNodeFlags_DefaultOpen)) {
                if (ImGui::InputInt("Dwell (ms)", &dwellMs, 100, 500)) {
                    dwellMs = std::max<int>(100, dwellMs);
                    missionChanged = true;
                }
                if (ImGui::InputInt("QuickScan Delay (ms)", &quickScanDelayMs, 50, 250)) {
                    quickScanDelayMs = std::max<int>(50, quickScanDelayMs);
                    missionChanged = true;
                }
                if (ImGui::InputInt("QuickScan Duration (ms)", &quickScanDurationMs, 100, 500)) {
                    quickScanDurationMs = std::max<int>(100, quickScanDurationMs);
                    missionChanged = true;
                }
                if (ImGui::SliderFloat("Threshold", &missionThreshold, -120.0f, 0.0f, "%.1f dB")) {
                    missionChanged = true;
                }
                if (ImGui::Checkbox("Record Audio", &recordAudio)) {
                    missionChanged = true;
                }
            }

            if (ImGui::CollapsingHeader("Operator Note", ImGuiTreeNodeFlags_DefaultOpen)) {
                ImGui::TextWrapped("This shell carries the Predator SDR mission control concepts: mode, search bands, targets, excludes, dwell, quick filters, and map launch.");
            }

            if (missionChanged) {
                saveMissionConfig(searchBands, targets, excludes, missionThreshold, dwellMs, quickScanDelayMs, quickScanDurationMs, recordAudio);
            }
        }
        else {
            if (ImGui::CollapsingHeader("Source & Device", ImGuiTreeNodeFlags_DefaultOpen)) {
                sourcemenu::draw(NULL);
            }
            if (ImGui::CollapsingHeader("Audio / Sinks", ImGuiTreeNodeFlags_DefaultOpen)) {
                sinkmenu::draw(NULL);
            }
            if (ImGui::CollapsingHeader("Display & Band Plan", ImGuiTreeNodeFlags_DefaultOpen)) {
                displaymenu::draw(NULL);
                bandplanmenu::draw(NULL);
            }
            if (ImGui::CollapsingHeader("Appearance", ImGuiTreeNodeFlags_DefaultOpen)) {
                thememenu::draw(NULL);
                vfo_color_menu::draw(NULL);
            }
            if (ImGui::CollapsingHeader("Module Manager", ImGuiTreeNodeFlags_DefaultOpen)) {
                module_manager_menu::draw(NULL);
            }
            if (ImGui::CollapsingHeader("Status", ImGuiTreeNodeFlags_DefaultOpen)) {
                ImGui::Text("Mission Mode: %s", missionModes[predatorMissionMode]);
                ImGui::Text("Selected Source: %s", sourceName.empty() ? "None" : sourceName.c_str());
                ImGui::Text("Playback State: %s", playing ? "Streaming" : "Idle");
                ImGui::Text("Center Frequency: %.0f Hz", gui::waterfall.getCenterFrequency());
                ImGui::Text("GPS Fix: %s", phoneHasFix ? "Ready" : "Waiting");
                if (phoneHasFix) {
                    ImGui::Text("GPS: %.6f, %.6f  +/-%.1fm", phoneLat, phoneLon, phoneAccuracy);
                }
                ImGui::TextWrapped("Maps are now wired through the phone GPS path. DF remains intentionally excluded.");
            }
            if (ImGui::CollapsingHeader("Legacy Advanced Menus", ImGuiTreeNodeFlags_DefaultOpen)) {
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
            if (ImGui::CollapsingHeader("Debug", ImGuiTreeNodeFlags_DefaultOpen)) {
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
    ImGui::BeginChild("PredatorRightRail", ImVec2(railWidth, contentHeight), true);

    for (int i = 0; i < 6; i++) {
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

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    ImVec2 wfSliderSize(18.0f * style::uiScale, 120.0f * style::uiScale);

    ImGui::SetCursorPosX((ImGui::GetWindowSize().x - ImGui::CalcTextSize("Zoom").x) * 0.5f);
    ImGui::TextUnformatted("Zoom");
    ImGui::SetCursorPosX((ImGui::GetWindowSize().x - wfSliderSize.x) * 0.5f);
    if (ImGui::VSliderFloat("##_7_", wfSliderSize, &bw, 1.0, 0.0, "")) {
        double factor = (double)bw * (double)bw;
        double wfBw = gui::waterfall.getBandwidth();
        double delta = wfBw - 1000.0;
        double finalBw = std::min<double>(1000.0 + (factor * delta), wfBw);
        gui::waterfall.setViewBandwidth(finalBw);
        if (vfo != NULL) {
            gui::waterfall.setViewOffset(vfo->centerOffset);
        }
    }

    ImGui::NewLine();

    ImGui::SetCursorPosX((ImGui::GetWindowSize().x - ImGui::CalcTextSize("Max").x) * 0.5f);
    ImGui::TextUnformatted("Max");
    ImGui::SetCursorPosX((ImGui::GetWindowSize().x - wfSliderSize.x) * 0.5f);
    if (ImGui::VSliderFloat("##_8_", wfSliderSize, &fftMax, 0.0, -160.0f, "")) {
        fftMax = std::max<float>(fftMax, fftMin + 10);
        core::configManager.acquire();
        core::configManager.conf["max"] = fftMax;
        core::configManager.release(true);
    }

    ImGui::NewLine();

    ImGui::SetCursorPosX((ImGui::GetWindowSize().x - ImGui::CalcTextSize("Min").x) * 0.5f);
    ImGui::TextUnformatted("Min");
    ImGui::SetCursorPosX((ImGui::GetWindowSize().x - wfSliderSize.x) * 0.5f);
    ImGui::SetItemUsingMouseWheel();
    if (ImGui::VSliderFloat("##_9_", wfSliderSize, &fftMin, 0.0, -160.0f, "")) {
        fftMin = std::min<float>(fftMax - 10, fftMin);
        core::configManager.acquire();
        core::configManager.conf["min"] = fftMin;
        core::configManager.release(true);
    }

    ImGui::EndChild();
    ImGui::PopStyleColor();

    gui::waterfall.setFFTMin(fftMin);
    gui::waterfall.setFFTMax(fftMax);
    gui::waterfall.setWaterfallMin(fftMin);
    gui::waterfall.setWaterfallMax(fftMax);

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
        if (sigpath::sourceManager.getSelectedSourceName().empty()) { return; }
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
