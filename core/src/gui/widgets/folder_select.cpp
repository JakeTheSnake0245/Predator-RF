#include <gui/widgets/folder_select.h>
#include <regex>
#include <filesystem>
#include <core.h>
#ifdef __ANDROID__
#include "saf_bridge.h"
#else
#include <gui/file_dialogs.h>
#endif

FolderSelect::FolderSelect(std::string defaultPath) {
    root = (std::string)core::args["root"];
    setPath(defaultPath);
}

bool FolderSelect::render(std::string id) {
    bool _pathChanged = false;
    float menuColumnWidth = ImGui::GetContentRegionAvail().x;

    float buttonWidth = ImGui::CalcTextSize("...").x + 20.0f;
    bool lastPathValid = pathValid;
    if (!lastPathValid) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.0f, 0.0f, 1.0f));
    }
    ImGui::SetNextItemWidth(menuColumnWidth - buttonWidth);
    if (ImGui::InputText(id.c_str(), strPath, 2047)) {
        path = std::string(strPath);
        std::string expandedPath = expandString(strPath);
        if (!std::filesystem::is_directory(expandedPath)) {
            pathValid = false;
        }
        else {
            pathValid = true;
            _pathChanged = true;
        }
    }
    if (!lastPathValid) {
        ImGui::PopStyleColor();
    }
    ImGui::SameLine();
    if (ImGui::Button(("..." + id + "_winselect").c_str(), ImVec2(buttonWidth - 8.0f, 0)) && !dialogOpen) {
        dialogOpen = true;
        if (workerThread.joinable()) { workerThread.join(); }
        workerThread = std::thread(&FolderSelect::worker, this);
    }

    _pathChanged |= pathChanged;
    pathChanged = false;
    return _pathChanged;
}

// FolderSelect::setPath is defined further down (Android branch needs a
// content:// URI to count as "valid" so the picker UI shows the green
// state after a SAF tree pick).

std::string FolderSelect::expandString(std::string input) {
    input = std::regex_replace(input, std::regex("%ROOT%"), root);
    return std::regex_replace(input, std::regex("//"), "/");
}

bool FolderSelect::pathIsValid() {
    return pathValid;
}

void FolderSelect::setPath(std::string path, bool markChanged) {
    this->path = path;
    std::string expandedPath = expandString(path);
#ifdef __ANDROID__
    pathValid = !path.empty() &&
                (path.rfind("content://", 0) == 0 ||
                 std::filesystem::is_directory(expandedPath));
#else
    pathValid = std::filesystem::is_directory(expandedPath);
#endif
    if (markChanged) { pathChanged = true; }
    strcpy(strPath, path.c_str());
}

void FolderSelect::worker() {
#ifdef __ANDROID__
    // SAF returns a content:// tree URI, NOT a filesystem path. We
    // store it in `path` so the user sees feedback that something was
    // picked, but downstream consumers that fopen()/std::ofstream the
    // path will fail. Recorder and other write-heavy modules need a
    // separate ContentResolver-based file abstraction (TODO). For now
    // this picker at least works as a UI element and the URI is
    // available for any module that learns to handle it.
    std::string picked = android_saf::pickFolderBlocking();
    if (!picked.empty()) {
        path = picked;
        strncpy(strPath, path.c_str(), sizeof(strPath) - 1);
        strPath[sizeof(strPath) - 1] = '\0';
        pathChanged = true;
    }
    // Accept any non-empty SAF URI as "valid" for display purposes;
    // std::filesystem::is_directory will return false for content URIs.
    pathValid = !path.empty() &&
                (path.rfind("content://", 0) == 0 ||
                 std::filesystem::is_directory(expandString(path)));
#else
    auto fold = pfd::select_folder("Select Folder", pathValid ? std::filesystem::path(expandString(path)).parent_path().string() : "");
    std::string res = fold.result();

    if (res != "") {
        path = res;
        strcpy(strPath, path.c_str());
        pathChanged = true;
    }

    pathValid = std::filesystem::is_directory(expandString(path));
#endif
    dialogOpen = false;
}