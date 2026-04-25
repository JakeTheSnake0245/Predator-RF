#pragma once
#include <string>

namespace backend {
    int init(std::string resDir = "");
    void beginFrame();
    void render(bool vsync = true);
    void getMouseScreenPos(double& x, double& y);
    void setMouseScreenPos(double x, double y);
    bool getPhoneLocation(double& lat, double& lon, float& accuracy, bool& hasFix);
    bool openMapView();
    int renderLoop();
    int end();
}
