#include "settings.h"

#include <algorithm>
#include <cstdlib>

namespace {
std::wstring IniPath() {
    const wchar_t* appdata = _wgetenv(L"APPDATA");
    std::wstring dir = appdata ? std::wstring(appdata) + L"\\FrameLift" : L".";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir + L"\\settings.ini";
}
int GetInt(const wchar_t* key, int def, const std::wstring& path) {
    return (int)GetPrivateProfileIntW(L"FrameLift", key, def, path.c_str());
}
void PutInt(const wchar_t* key, int v, const std::wstring& path) {
    WritePrivateProfileStringW(L"FrameLift", key, std::to_wstring(v).c_str(), path.c_str());
}
}  // namespace

Settings LoadSettings() {
    Settings s;
    auto p = IniPath();
    s.frameGen = GetInt(L"frameGen", s.frameGen, p) != 0;
    // Only 30 / 60 / 90 / 120 are offered; snap anything else to the nearest.
    int t = GetInt(L"targetFps", s.targetFps, p);
    s.targetFps = 30;
    for (int v : {60, 90, 120})
        if (std::abs(t - v) < std::abs(t - s.targetFps)) s.targetFps = v;
    s.vsync = GetInt(L"vsync", s.vsync, p) != 0;
    s.smooth = GetInt(L"smooth", s.smooth, p) != 0;
    s.volume = std::clamp(GetInt(L"volume", 100, p), 0, 200) / 100.0f;
    s.mute = GetInt(L"mute", s.mute, p) != 0;
    s.avSync = GetInt(L"avSync", s.avSync, p) != 0;
    s.overlay = GetInt(L"overlay", s.overlay, p) != 0;
    s.engine = GetInt(L"engine", s.engine, p) ? 1 : 0;
    s.lateLatch = GetInt(L"lateLatch", s.lateLatch, p) != 0;
    s.collectClips = GetInt(L"collectClips", s.collectClips, p) != 0;
    s.nvMaxPerf = GetInt(L"nvMaxPerf", s.nvMaxPerf, p) != 0;
    s.scaler = std::clamp(GetInt(L"scaler", s.scaler, p), 0, 1);
    s.sharpness = std::clamp(GetInt(L"sharpness", s.sharpness, p), 0, 100);
    wchar_t buf[512] = {};
    GetPrivateProfileStringW(L"FrameLift", L"device", L"", buf, 512, p.c_str());
    s.device = buf;
    return s;
}

std::wstring SettingsKey(const Settings& s) {
    wchar_t b[256];
    swprintf(b, 256, L"%d|%d|%d|%d|%d|%d|%d|%d|%d|%d|%d|%d|%d|%d|%d|", s.frameGen, s.targetFps, s.vsync, s.smooth, (int)(s.volume * 100 + 0.5f),
             s.mute, s.avSync, s.overlay, s.showMotion, s.engine, s.lateLatch, s.collectClips, s.nvMaxPerf, s.scaler, s.sharpness);
    return std::wstring(b) + s.device;
}

void SaveSettings(const Settings& s) {
    auto p = IniPath();
    PutInt(L"frameGen", s.frameGen, p);
    PutInt(L"targetFps", s.targetFps, p);
    PutInt(L"vsync", s.vsync, p);
    PutInt(L"smooth", s.smooth, p);
    PutInt(L"volume", (int)(s.volume * 100 + 0.5f), p);
    PutInt(L"mute", s.mute, p);
    PutInt(L"avSync", s.avSync, p);
    PutInt(L"overlay", s.overlay, p);
    PutInt(L"engine", s.engine, p);
    PutInt(L"lateLatch", s.lateLatch, p);
    PutInt(L"collectClips", s.collectClips, p);
    PutInt(L"nvMaxPerf", s.nvMaxPerf, p);
    PutInt(L"scaler", s.scaler, p);
    PutInt(L"sharpness", s.sharpness, p);
    WritePrivateProfileStringW(L"FrameLift", L"device", s.device.c_str(), p.c_str());
}
