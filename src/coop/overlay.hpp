#pragma once
#include <string>
#include <vector>

namespace coop {
struct OverlayColor { float r, g, b, a; };
struct OverlayPrimitive {
    float x, y, width, height, fontSize;
    OverlayColor color;
    std::string text;
};
struct OverlayFrame {
    int width = 0, height = 0;
    std::vector<OverlayPrimitive> primitives;
};
void StartOverlay();
bool OverlayReady();
void PublishOverlay(OverlayFrame frame);
} // namespace coop
