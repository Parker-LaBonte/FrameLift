#pragma once
#include "common.h"

struct Settings {
    bool frameGen = true;
    int targetFps = 60;
    bool vsync = true;
    bool smooth = false;     // pacing: false = lowest latency, true = smoothest (+1 output frame)
    float volume = 1.0f;
    bool mute = false;
    bool avSync = true;
    bool overlay = true;     // small stats line in fullscreen
    bool showMotion = false;
    int engine = 1;          // 1 = Auto (NVIDIA Optical Flow hardware when available), 0 = shader block search
    bool lateLatch = true;   // vsync: draw just before the refresh for fresher content
    bool collectClips = false;  // save 60 fps test clips for the benchmark
    bool nvMaxPerf = true;   // NVIDIA: "Prefer maximum performance" for FrameLift.exe (driver profile)
    int scaler = 1;          // 0 = bilinear, 1 = sharp (Lanczos with de-ringing + contrast-adaptive sharpening)
    int sharpness = 50;      // 0..100, sharp scaler only
    std::wstring device;     // last used capture card (friendly name)
};

Settings LoadSettings();
std::wstring SettingsKey(const Settings& s);  // compact snapshot, used to detect changes
void SaveSettings(const Settings& s);
