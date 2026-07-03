#pragma once

#include "imgui.h"

struct AppFontSet {
    ImFont* ui = nullptr;
    ImFont* title = nullptr;
    ImFont* code = nullptr;
};

AppFontSet LoadAppFonts(ImGuiIO& io);
