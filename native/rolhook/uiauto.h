// Interface scale "auto" ([ui] layout_scale=0, the ui_scale mod's default): the scale for the height the game runs
// at. Header-only so that shadertest.exe checks it without the game.
#pragma once
#include <cstdlib>

namespace rol {

// The scales the ui_scale mod builds layouts for (modpack/ui_scale/build.py VARIANTS), 100 = as the game.
constexpr int kUiScales[] = {100, 125, 150, 175, 200};

// The built scale nearest to height / 9.6, ties to the smaller: 1440 lines -> 150% (the scale chosen at 3440 x 1440),
// 1080 -> 100% (halfway to 125), 1200 -> 125%, 1600 -> 175%, 1800 -> 175% (halfway to 200), 2160 and up -> 200%.
inline int ui_auto_scale(int height) {
    int best = kUiScales[0];
    for (int s : kUiScales)   // |height / 9.6 - s| in tenths: |10 height - 96 s|
        if (std::abs(10 * height - 96 * s) < std::abs(10 * height - 96 * best)) best = s;
    return best;
}

}  // namespace rol
