// FrameLift: console capture -> low-latency frame generation -> your screen.
#include "audio.h"
#include "bench.h"
#include "capture.h"
#include "clips.h"
#include "common.h"
#include "framegen.h"
#include "gpumon.h"
#include "log.h"
#include "nvprofile.h"
#include "version.h"
#include "settings.h"

#include <avrt.h>
#include <dbt.h>
#include <mfapi.h>
#include <mmsystem.h>
#include <shellapi.h>

#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <thread>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

namespace {

constexpr double kPi = 3.14159265358979323846;
const ImVec4 kAccent = ImVec4(0.30f, 0.85f, 0.56f, 1.0f);
const ImVec4 kAmber = ImVec4(0.98f, 0.73f, 0.27f, 1.0f);
const ImVec4 kRed = ImVec4(0.96f, 0.38f, 0.36f, 1.0f);
const ImVec4 kDim = ImVec4(0.56f, 0.58f, 0.62f, 1.0f);

struct Pacing {
    bool fgActive = false;
    bool vsync = false;
    UINT sync = 0;
    bool tear = false;
    double refresh = 60, outHz = 60, O = 16.67, D = 0, srcFps = 0, srcLowFps = 0;
    bool fillingDips = false;  // target ~= game rate; only interpolating to cover slow frames
    const char* reason = "";
};

// Test clips go next to the settings (%LOCALAPPDATA%\FrameLift\captures), never a fixed drive path.
static std::wstring ClipsDir() {
    wchar_t buf[MAX_PATH] = {};
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
    std::wstring d = (n && n < MAX_PATH) ? std::wstring(buf) : std::wstring(L".");
    d += L"\\FrameLift";
    CreateDirectoryW(d.c_str(), nullptr);
    return d + L"\\captures";
}

struct App {
    HWND hwnd = nullptr;
    Renderer r;
    Capture cap;
    AudioPassthrough audio;
    GpuMonitor gpu;
    ClipCollector clips;
    Settings s;
    bool running = true;

    // devices
    std::vector<VideoDevice> devices;
    double enumAt = 0;
    std::wstring failedSymlink;
    std::string failedName;
    double failedAt = -1e9;
    std::vector<AudioDevice> audioList;
    std::wstring listenRestoreId;  // we switched off Windows "Listen" on this endpoint; restore on exit
    bool listenWarning = false;
    bool audioListValid = false;

    // window
    bool fullscreen = false, dashboard = true;
    WINDOWPLACEMENT prevPlacement{sizeof(WINDOWPLACEMENT)};
    bool resizePending = false;
    UINT newW = 0, newH = 0;
    float dpi = 1.0f;
    bool fontsDirty = false;
    bool displayChanged = false;  // resolution/refresh changed: re-apply fullscreen + focus
    ImFont *fontBody = nullptr, *fontBig = nullptr, *fontSmall = nullptr;
    double lastMouse = 0;
    std::string toast;  // brief confirmation when a hotkey is pressed
    double toastUntil = 0;

    // source timing (unique frames only)
    std::deque<double> intervals;
    double sourcePeriod = 0, periodHi = 0, lastUniqueTs = 0;  // median / 90th-percentile frame interval
    double dCur = -1;  // the hold actually in use; glides toward the target instead of jumping
    float latchWaitMs = 0;  // average late-latch wait (how much fresher the shown moment is)
    std::wstring savedKey;  // settings as last written to disk
    double settingsChangedAt = 0;
    // Work tier by the game's live frame rate (relative to output): 0 = ~30 fps (NVIDIA best, half
    // size), 1 = 30-56 fps (NVIDIA medium, half size), 2 = at/above target (no motion work).
    int tier = 0, tierCandidate = 0;
    double tierCandidateSince = 0;
    double periodPeak = 0;      // slowest recent frame gap; jumps up on a dip, decays over ~3 s
    double lastRenderT = 0;
    bool starving = false;      // currently waiting on a late real frame
    std::deque<double> stutterTimes;  // when frames had to be held (last 60 s)

    // output pacing
    HANDLE timer = nullptr;
    double nextTick = 0, margin = 2.0;
    double motionPad = 0.0;
    double interpSince = 0;  // when frame generation (re)started: its first seconds are warm-up  // extra hold so motion analysis finishes before its frame is drawn
    int flowLate = 0;        // ticks shown un-interpolated because motion wasn't ready (per stats window)
    double phaseC = 1, phaseS = 0, lastOff = -1;
    int phaseSamples = 0;

    // stats
    double lastRender = 0, lastContentTs = -1;
    struct InFlight {
        UINT id;
        double contentTs, presentedMs;
        bool interp;
    };
    std::deque<InFlight> inFlight;
    double statsOkMs = -1e9;
    std::deque<float> lat;
    float latAvg = 0, latP95 = 0, passLat = 0, holdMs = 0;
    std::vector<float> latHist;
    double statsT0 = 0;
    int outFrames = 0;
    float outFps = 0;
    // UI snapshots (--uishot): render with demo data, save the frame before Present, then exit.
    int shotCountdown = -1;
    std::wstring shotPath;
    bool openAdvanced = false;
    Pacing lastPacing;
};
App* g = nullptr;

double Wrap(double x, double period) { return x - period * std::floor(x / period + 0.5); }

// ------------------------------------------------------------------------------------------
// --dump diagnostics: saves the first frames after the picture has been static, as 1/4-size BMPs
// next to the exe, and logs their brightness. Used to see what a card sends with no HDMI input.
// ------------------------------------------------------------------------------------------
bool gDump = false;
int gDumpCount = 0;

void DumpFrame(const Frame& f, int burstIndex) {
    if (gDumpCount >= 24) return;
    const int W = f.width, H = f.height, dw = W / 4, dh = H / 4;
    auto luma = [&](int x, int y) -> int {
        if (f.fmt == PixFmt::NV12) return f.data[(size_t)y * W + x];
        if (f.fmt == PixFmt::YUY2) return f.data[((size_t)y * W + x) * 2];
        return f.data[((size_t)y * W + x) * 4 + 1];
    };
    auto chroma = [&](int x, int y, int& u, int& v) {
        if (f.fmt == PixFmt::NV12) {
            size_t o = (size_t)W * H + (size_t)(y / 2) * W + (x & ~1);
            u = f.data[o];
            v = f.data[o + 1];
        } else if (f.fmt == PixFmt::YUY2) {
            size_t o = ((size_t)y * W + (x & ~1)) * 2;
            u = f.data[o + 1];
            v = f.data[o + 3];
        } else {
            u = v = 128;
        }
    };
    std::vector<uint8_t> px((size_t)dw * dh * 3);
    long long sum = 0;
    int mn = 255, mx = 0;
    for (int y = 0; y < dh; y++)
        for (int x = 0; x < dw; x++) {
            int sx = x * 4 + 1, sy = y * 4;  // odd pixel: checks the 2nd luma of a YUY2 pair
            int Y = luma(sx, sy), U = 128, V = 128;
            sum += Y;
            mn = std::min(mn, Y);
            mx = std::max(mx, Y);
            uint8_t* o = &px[((size_t)(dh - 1 - y) * dw + x) * 3];  // BMP rows are bottom-up
            if (f.fmt == PixFmt::BGRA) {
                const uint8_t* s = &f.data[((size_t)sy * W + sx) * 4];
                o[0] = s[0];
                o[1] = s[1];
                o[2] = s[2];
                continue;
            }
            chroma(sx, sy, U, V);
            double yy = (Y - 16) / 219.0, uu = (U - 128) / 224.0, vv = (V - 128) / 224.0;
            auto c = [](double d) { return (uint8_t)std::clamp((int)std::lround(d * 255), 0, 255); };
            o[2] = c(yy + 1.5748 * vv);
            o[1] = c(yy - 0.1873 * uu - 0.4681 * vv);
            o[0] = c(yy + 1.8556 * uu);
        }
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring dir = exe;
    dir = dir.substr(0, dir.find_last_of(L'\\')) + L"\\dumps";
    CreateDirectoryW(dir.c_str(), nullptr);
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t name[96];
    swprintf(name, 96, L"\\burst-%02d%02d%02d-%02d.bmp", t.wHour, t.wMinute, t.wSecond, burstIndex);
    FILE* fp = _wfopen((dir + name).c_str(), L"wb");
    if (!fp) return;
    uint32_t rowBytes = (uint32_t)dw * 3, pad = (4 - rowBytes % 4) % 4, imgSize = (rowBytes + pad) * dh;
    uint8_t hdr[54] = {'B', 'M'};
    auto put32 = [&](int off, uint32_t v) { memcpy(hdr + off, &v, 4); };
    put32(2, 54 + imgSize);
    put32(10, 54);
    put32(14, 40);
    put32(18, (uint32_t)dw);
    put32(22, (uint32_t)dh);
    hdr[26] = 1;
    hdr[28] = 24;
    put32(34, imgSize);
    fwrite(hdr, 1, 54, fp);
    const uint8_t zeros[3] = {};
    for (int y = 0; y < dh; y++) {
        fwrite(&px[(size_t)y * rowBytes], 1, rowBytes, fp);
        fwrite(zeros, 1, pad, fp);
    }
    fclose(fp);
    gDumpCount++;
    Log("dump: %s  luma mean %.1f min %d max %d%s", Narrow(name + 1).c_str(), (double)sum / ((double)dw * dh), mn, mx,
        f.sceneCut ? " (scene cut)" : "");
}

// ------------------------------------------------------------------------------------------
// Devices
// ------------------------------------------------------------------------------------------
void CloseDevice(App& a) {
    a.audio.Stop();
    a.cap.Close();
    a.r.ResetTimeline();
    a.intervals.clear();
    a.sourcePeriod = 0;
    a.periodHi = 0;
    a.lastUniqueTs = 0;
}

void StartAudio(App& a, const std::wstring& id) {
    a.listenWarning = false;
    bool stillOn = false;
    if (!id.empty() && AudioPassthrough::DisableListen(id, &stillOn)) {
        if (stillOn) a.listenWarning = true;
        else a.listenRestoreId = id;
    }
    std::wstring name;
    for (auto& d : AudioPassthrough::ListCaptureDevices())
        if (d.id == id) name = d.name;
    a.audio.Start(id, name);
}

void OpenDevice(App& a, const VideoDevice& d, int mode) {
    CloseDevice(a);
    {
        MONITORINFO mi{sizeof(mi)};
        if (GetMonitorInfoW(MonitorFromWindow(a.hwnd, MONITOR_DEFAULTTOPRIMARY), &mi))
            SetCaptureAutoMaxHeight(mi.rcMonitor.bottom - mi.rcMonitor.top);
    }
    if (!a.cap.Open(d, mode)) {
        Log("device: could not open \"%s\"", Narrow(d.name).c_str());
        a.failedSymlink = d.symlink;
        a.failedName = Narrow(d.name);
        a.failedAt = NowMs();
        return;
    }
    a.failedSymlink.clear();
    a.s.device = d.name;
    Log("device: opened \"%s\" (container %s)", Narrow(d.name).c_str(), Narrow(d.containerId).c_str());
    std::wstring aid = AudioPassthrough::FindForVideo(d.containerId, d.name);
    for (auto& ad : AudioPassthrough::ListCaptureDevices())
        Log("audio:   endpoint \"%s\" container %s%s", Narrow(ad.name).c_str(), Narrow(ad.containerId).c_str(),
            ad.id == aid ? "  <= matched" : "");
    if (aid.empty()) Log("audio: no capture-card audio endpoint matched");
    StartAudio(a, aid);
}

void ManageDevices(App& a, double now) {
    if (a.cap.IsOpen() && a.cap.Failed()) {  // unplugged or driver error
        Log("device: lost \"%s\"", Narrow(a.cap.Device().name).c_str());
        CloseDevice(a);
        a.enumAt = now + 300;
    }
    if (now < a.enumAt) return;
    {
        auto list = Capture::Enumerate();
        bool changed = list.size() != a.devices.size();
        for (size_t i = 0; !changed && i < list.size(); i++) changed = list[i].symlink != a.devices[i].symlink;
        a.devices = std::move(list);
        if (changed) {
            Log("device: %zu video device(s)", a.devices.size());
            for (auto& d : a.devices) Log("device:   \"%s\" score %d", Narrow(d.name).c_str(), d.score);
        }
    }
    a.enumAt = now + 2000;
    if (a.cap.IsOpen()) {
        bool present = std::any_of(a.devices.begin(), a.devices.end(),
                                   [&](const VideoDevice& d) { return d.symlink == a.cap.Device().symlink; });
        if (present) {
            a.enumAt = 1e300;  // nothing to do until Windows reports a device change
            return;
        }
        CloseDevice(a);
    }
    const VideoDevice* pick = nullptr;
    for (auto& d : a.devices)
        if (!a.s.device.empty() && d.name == a.s.device) pick = &d;
    if (!pick)
        for (auto& d : a.devices)
            if (d.score > 0) {
                pick = &d;
                break;
            }
    if (pick && !(pick->symlink == a.failedSymlink && now - a.failedAt < 5000)) OpenDevice(a, *pick, -1);
    if (a.cap.IsOpen()) a.enumAt = 1e300;
}

// ------------------------------------------------------------------------------------------
// Pacing: how fast we output and how far behind "now" we display (the frame-gen hold)
// ------------------------------------------------------------------------------------------
Pacing ComputePacing(App& a, double now) {
    Pacing p;
    p.refresh = a.r.RefreshHz();
    double target = std::max(30, a.s.targetFps);
    if (a.s.vsync) {
        int n = std::max(1, (int)std::lround(p.refresh / target));
        p.vsync = true;
        p.sync = (UINT)n;
        p.outHz = p.refresh / n;
    } else {
        p.outHz = target;
        p.tear = true;
    }
    p.O = 1000.0 / p.outHz;
    p.srcFps = a.sourcePeriod > 0 ? 1000.0 / a.sourcePeriod : 0;
    p.srcLowFps = a.periodHi > 0 ? 1000.0 / a.periodHi : 0;
    // Frame gen stays engaged whenever it's switched on and we have video. It never drops to
    // passthrough for pauses, menus or slow motion: switching modes is what made those stutter.
    // A static screen simply holds the last real frame; uneven frames just move along the timeline.
    if (!a.s.frameGen) {
        p.reason = "Off";
        a.lastOff = -1;
        a.phaseSamples = 0;
        a.dCur = -1;
        return p;
    }
    if (!a.r.HasVideo() || a.sourcePeriod <= 0) {
        p.reason = "Waiting for game frames";
        return p;
    }
    p.fgActive = true;
    p.fillingDips = p.outHz <= p.srcFps * 1.05;  // game already at/above target: only covering slow frames

    // Hold = slow end (90th percentile) of the last 20 real frame intervals, so uneven frames
    // (slow motion, hitches) are still bracketed by two real frames. Capped at 3x typical.
    double period = std::min(std::max({a.sourcePeriod, a.periodHi, a.periodPeak}), a.sourcePeriod * 1.5);
    double dmin = (a.s.smooth ? period : std::max(0.0, period - p.O)) + a.margin + (a.tier < 2 ? a.motionPad : 0.0);
    p.D = dmin;
    if (a.tier == 2) {
        // Game already at the output rate: no interpolation, so no need to align ticks to frames.
        p.D = a.margin;
        return p;
    }
    if (p.vsync && a.phaseSamples > 30) {
        // With vsync we can't move the ticks, so shift the hold until a tick lands exactly on
        // each real frame's timestamp. Keeps motion even when console and monitor drift.
        double psi = std::atan2(a.phaseS, a.phaseC) / (2 * kPi) * p.O;
        if (psi < 0) psi += p.O;
        double off = std::fmod(psi - dmin, p.O);
        if (off < 0) off += p.O;
        if (a.lastOff >= 0) {
            double alt = off - p.O;
            if (std::fabs(alt - a.lastOff) < std::fabs(off - a.lastOff) && alt > -0.25 * p.O) off = alt;
        }
        a.lastOff = off;
        p.D = dmin + off;
    }
    return p;
}

bool DrainCapture(App& a, const Pacing& p) {
    auto frames = a.cap.Drain();
    if (frames.empty()) return false;
    // Pick the work tier from the game's live frame rate, switching only after it has settled for a
    // second (benchmarked on 28 clips: half-size best ~= full-size best for ~8 ms less GPU per pair;
    // half-size medium ~= shader cost with near-best quality; full-size medium is never worth it).
    {
        const double now = NowMs();
        const double fps = a.sourcePeriod > 0 ? 1000.0 / a.sourcePeriod : 0;
        int want = 0;
        if (fps > 0) want = fps < p.outHz * 0.62 ? 0 : (fps < p.outHz * 0.93 ? 1 : 2);
        if (want != a.tierCandidate) {
            a.tierCandidate = want;
            a.tierCandidateSince = now;
        }
        if (want != a.tier && now - a.tierCandidateSince > 1000) {
            Log("tier: %d -> %d (game %.1f fps, output %.0f)", a.tier, want, fps, p.outHz);
            a.tier = want;
        }
    }
    a.r.tuning.engine = a.s.engine;
    a.r.tuning.scaler = a.s.scaler;
    a.r.tuning.sharpness = a.s.sharpness / 100.0f;
    a.r.tuning.nvScale = 0;  // auto: ~960x540 analysis for any capture size
    a.r.tuning.nvPerf = 10;  // bench: medium at half size is within 0.03 dB of best, ~35% cheaper
    a.r.BeginGpuTimer();
    if (frames.size() > 3) frames.erase(frames.begin(), frames.end() - 3);
    bool wantMotion = a.s.frameGen && a.tier < 2;  // at/above target there's nothing to fill in
    for (auto& f : frames) {
        if (gDump) {
            static int burstFrames = 0;
            if (a.lastUniqueTs <= 0 || f->ts - a.lastUniqueTs > 2000) burstFrames = 0;
            if (burstFrames < 3) DumpFrame(*f, burstFrames);
            burstFrames++;
        }
        if (a.lastUniqueTs > 0) {
            double iv = f->ts - a.lastUniqueTs;
            if (iv > 4 && iv < 250) {
                a.intervals.push_back(iv);
                if (a.intervals.size() > 20) a.intervals.pop_front();
                std::vector<double> tmp(a.intervals.begin(), a.intervals.end());
                std::sort(tmp.begin(), tmp.end());
                a.sourcePeriod = tmp[tmp.size() / 2];
                a.periodHi = tmp[std::min(tmp.size() - 1, (size_t)(tmp.size() * 0.9))];
            }
        }
        // Interpolate across any normal-for-this-game gap; only a real stall/pause counts as a cut.
        double maxGap = a.sourcePeriod > 0 ? std::min(250.0, std::max(a.sourcePeriod * 2.6, a.periodHi * 1.6)) : 120;
        // A dip raises the hold target immediately (it then decays over a few seconds), so the
        // next slow frames in a slow-motion stretch are already covered.
        if (a.lastUniqueTs > 0) {
            double iv = f->ts - a.lastUniqueTs;
            // Only moderate slowdowns raise the hold; a long hitch (menus, loading) would otherwise
            // add 100+ ms of latency for seconds. Those get a brief stutter instead.
            if (iv > 4 && iv <= a.sourcePeriod * 1.6) a.periodPeak = std::max(a.periodPeak, iv);
        }
        a.lastUniqueTs = f->ts;
        a.r.Upload(*f, wantMotion, maxGap);
        // Test-clip collection: only while the game genuinely runs at the output rate (tier 2) and
        // every captured frame is new (no repeats), so each odd frame is a true in-between frame.
        if (a.s.collectClips) {
            CaptureStats cs = a.cap.Stats();
            a.clips.Offer(f, a.tier == 2 && cs.signalFps > 0 && cs.uniqueFps >= cs.signalFps * 0.97);
        }
        if (gDump && p.fgActive && f->sceneCut == false) {
            static double lastSet = 0;
            static int sets = 0;
            double now = NowMs();
            static double lastSeq = 0;
            static int seqs = 0;
            if (seqs < 12 && now - lastSeq > 10000 && a.sourcePeriod > 0) {
                wchar_t exe[MAX_PATH];
                GetModuleFileNameW(nullptr, exe, MAX_PATH);
                std::wstring dir = exe;
                dir = dir.substr(0, dir.find_last_of(L'\\')) + L"\\dumps";
                CreateDirectoryW(dir.c_str(), nullptr);
                SYSTEMTIME t;
                GetLocalTime(&t);
                wchar_t name[64];
                swprintf(name, 64, L"\\seq-%02d%02d%02d", t.wHour, t.wMinute, t.wSecond);
                int got = a.r.SaveSequence(dir + name);
                if (got) {
                    lastSeq = now;
                    seqs++;
                    Log("dump: %s-f0..f%d (consecutive real frames)", Narrow(name + 1).c_str(), got - 1);
                }
            }
            if (sets < 4 && now - lastSet > 15000 && a.sourcePeriod > 0) {
                lastSet = now;
                wchar_t exe[MAX_PATH];
                GetModuleFileNameW(nullptr, exe, MAX_PATH);
                std::wstring dir = exe;
                dir = dir.substr(0, dir.find_last_of(L'\\')) + L"\\dumps";
                CreateDirectoryW(dir.c_str(), nullptr);
                SYSTEMTIME t;
                GetLocalTime(&t);
                wchar_t name[64];
                swprintf(name, 64, L"\\set-%02d%02d%02d", t.wHour, t.wMinute, t.wSecond);
                if (a.r.SaveInterpolationSet(dir + name)) {
                    sets++;
                    Log("dump: %s-{1-A,2-mid-old,3-mid-new,4-motion,5-B}.bmp", Narrow(name + 1).c_str());
                }
            }
        }
        if (gDump && gDumpCount < 24) {
            static double lastPair = 0;
            double now = NowMs();
            if (now - lastPair > 10000) {
                lastPair = now;
                DumpFrame(*f, 90);  // CPU conversion
                wchar_t exe[MAX_PATH];
                GetModuleFileNameW(nullptr, exe, MAX_PATH);
                std::wstring dir = exe;
                dir = dir.substr(0, dir.find_last_of(L'\\')) + L"\\dumps";
                SYSTEMTIME t;
                GetLocalTime(&t);
                wchar_t name[96];
                swprintf(name, 96, L"\\gpu-%02d%02d%02d.bmp", t.wHour, t.wMinute, t.wSecond);
                bool ok = a.r.SaveNewestColorBmp(dir + name);
                Log("dump: %s GPU-converted copy of the same frame (fmt %d, 709 %d, full %d)%s", Narrow(name + 1).c_str(),
                    (int)f->fmt, f->bt709 ? 1 : 0, f->fullRange ? 1 : 0, ok ? "" : " FAILED");
            }
        }
        // Timer pacing: phase-lock output ticks so one lands exactly on each real frame.
        if (p.fgActive && !p.vsync && a.nextTick > 0 && a.dCur >= 0) a.nextTick -= 0.3 * Wrap(a.nextTick - (f->ts + a.dCur), p.O);
    }
    return true;
}

// ------------------------------------------------------------------------------------------
// UI
// ------------------------------------------------------------------------------------------
float PanelWidth(const App& a) { return 360.0f * a.dpi; }

D3D11_VIEWPORT VideoViewport(App& a) {
    RECT rc;
    GetClientRect(a.hwnd, &rc);
    float cw = (float)rc.right, ch = (float)rc.bottom;
    float panel = (a.dashboard && !a.fullscreen) ? PanelWidth(a) : 0;
    float aw = std::max(1.0f, cw - panel), ah = std::max(1.0f, ch);
    float vw = a.r.VideoWidth() ? (float)a.r.VideoWidth() : 16, vh = a.r.VideoHeight() ? (float)a.r.VideoHeight() : 9;
    float sc = std::min(aw / vw, ah / vh);
    D3D11_VIEWPORT vp{};
    vp.Width = vw * sc;
    vp.Height = vh * sc;
    vp.TopLeftX = (aw - vp.Width) * 0.5f;
    vp.TopLeftY = (ah - vp.Height) * 0.5f;
    vp.MaxDepth = 1;
    return vp;
}

void LoadFonts(App& a) {
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();
    static const ImWchar ranges[] = {0x0020, 0x00FF, 0x2190, 0x2193, 0x2022, 0x2022, 0};
    auto load = [&](const char* path, float px) -> ImFont* {
        if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) return nullptr;
        return io.Fonts->AddFontFromFileTTF(path, px * a.dpi, nullptr, ranges);
    };
    a.fontBody = load("C:\\Windows\\Fonts\\segoeui.ttf", 17);
    if (!a.fontBody) {
        ImFontConfig c;
        c.SizePixels = 13 * a.dpi;
        a.fontBody = io.Fonts->AddFontDefault(&c);
    }
    a.fontBig = load("C:\\Windows\\Fonts\\segoeuib.ttf", 34);
    if (!a.fontBig) a.fontBig = a.fontBody;
    a.fontSmall = load("C:\\Windows\\Fonts\\segoeui.ttf", 14);
    if (!a.fontSmall) a.fontSmall = a.fontBody;
    io.FontDefault = a.fontBody;
}

void SetupStyle(App& a) {
    ImGuiStyle& st = ImGui::GetStyle();
    st = ImGuiStyle();
    ImGui::StyleColorsDark(&st);
    st.WindowRounding = 0;
    st.FrameRounding = 6;
    st.GrabRounding = 6;
    st.PopupRounding = 6;
    st.FramePadding = ImVec2(10, 6);
    st.ItemSpacing = ImVec2(8, 8);
    st.WindowPadding = ImVec2(18, 16);
    st.WindowBorderSize = 0;
    st.ScrollbarSize = 10;
    ImVec4* c = st.Colors;
    c[ImGuiCol_WindowBg] = ImVec4(0.07f, 0.075f, 0.085f, 1);
    c[ImGuiCol_PopupBg] = ImVec4(0.10f, 0.105f, 0.12f, 1);
    c[ImGuiCol_FrameBg] = ImVec4(0.14f, 0.15f, 0.17f, 1);
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.18f, 0.19f, 0.22f, 1);
    c[ImGuiCol_FrameBgActive] = ImVec4(0.20f, 0.22f, 0.25f, 1);
    c[ImGuiCol_Button] = ImVec4(0.16f, 0.17f, 0.19f, 1);
    c[ImGuiCol_ButtonHovered] = ImVec4(0.21f, 0.22f, 0.25f, 1);
    c[ImGuiCol_ButtonActive] = ImVec4(0.25f, 0.27f, 0.30f, 1);
    c[ImGuiCol_Header] = ImVec4(0.16f, 0.17f, 0.19f, 1);
    c[ImGuiCol_HeaderHovered] = ImVec4(0.21f, 0.22f, 0.25f, 1);
    c[ImGuiCol_HeaderActive] = ImVec4(0.25f, 0.27f, 0.30f, 1);
    c[ImGuiCol_CheckMark] = kAccent;
    c[ImGuiCol_SliderGrab] = kAccent;
    c[ImGuiCol_SliderGrabActive] = kAccent;
    c[ImGuiCol_Separator] = ImVec4(1, 1, 1, 0.07f);
    c[ImGuiCol_Text] = ImVec4(0.92f, 0.93f, 0.95f, 1);
    c[ImGuiCol_TextDisabled] = kDim;
    st.ScaleAllSizes(a.dpi);
}

void Section(const char* t) {
    ImGui::Dummy(ImVec2(0, 4 * g->dpi));
    ImGui::PushFont(g->fontSmall);
    ImGui::PushStyleColor(ImGuiCol_Text, kDim);
    ImGui::TextUnformatted(t);
    ImGui::PopStyleColor();
    ImGui::PopFont();
}

bool Toggle(const char* label, bool* v) {
    ImGuiStyle& st = ImGui::GetStyle();
    float h = ImGui::GetFrameHeight(), w = h * 1.8f;
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::PushID(label);
    bool clicked = ImGui::InvisibleButton("##t", ImVec2(w, h));
    ImGui::PopID();
    if (clicked) *v = !*v;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImU32 bg = *v ? ImGui::GetColorU32(kAccent) : ImGui::GetColorU32(ImGuiCol_FrameBgActive);
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), bg, h * 0.5f);
    float r = h * 0.5f - 3 * g->dpi;
    float cx = *v ? p.x + w - h * 0.5f : p.x + h * 0.5f;
    dl->AddCircleFilled(ImVec2(cx, p.y + h * 0.5f), r, IM_COL32(255, 255, 255, 255));
    ImGui::SameLine(0, st.ItemInnerSpacing.x * 2);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    return clicked;
}

void Graph(const char* id, const std::vector<float>& v, float maxV, float height, ImVec4 color, size_t slots) {
    ImVec2 p = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x;
    ImGui::InvisibleButton(id, ImVec2(w, height));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + height), IM_COL32(255, 255, 255, 10), 6 * g->dpi);
    for (int i = 1; i < 4; i++) {
        float y = p.y + height * i / 4.0f;
        dl->AddLine(ImVec2(p.x, y), ImVec2(p.x + w, y), IM_COL32(255, 255, 255, 12));
    }
    if (v.size() < 2) return;
    size_t n = std::min(v.size(), slots);
    const float* d = v.data() + (v.size() - n);
    float dx = w / (float)(slots - 1);
    std::vector<ImVec2> pts(n);
    for (size_t i = 0; i < n; i++) {
        float x = p.x + w - (float)(n - 1 - i) * dx;
        float y = p.y + height - std::clamp(d[i] / maxV, 0.0f, 1.0f) * (height - 2);
        pts[i] = ImVec2(x, y);
    }
    ImVec4 fc = color;
    fc.w = 0.18f;
    ImU32 fill = ImGui::GetColorU32(fc);
    float base = p.y + height;
    for (size_t i = 1; i < n; i++)
        dl->AddQuadFilled(ImVec2(pts[i - 1].x, base), pts[i - 1], pts[i], ImVec2(pts[i].x, base), fill);
    dl->AddPolyline(pts.data(), (int)n, ImGui::GetColorU32(color), 0, 1.6f * g->dpi);
}

ImVec4 LatencyColor(float ms) { return ms < 25 ? kAccent : (ms < 40 ? kAmber : kRed); }

enum class Status { NoDevice, OpenFailed, NoSignal, Live };

Status GetStatus(App& a, const CaptureStats& cs) {
    if (!a.cap.IsOpen()) return (!a.failedSymlink.empty() && NowMs() - a.failedAt < 6000) ? Status::OpenFailed : Status::NoDevice;
    if (NowMs() - cs.lastRawMs > 1500 || cs.noSignal) return Status::NoSignal;
    return Status::Live;
}

// Windows only hides the taskbar for the *focused* fullscreen window, and a window started in the
// background (e.g. by a scheduled task) doesn't get focus. Borrow the foreground thread's input
// state so SetForegroundWindow is allowed.
void ForceForeground(HWND h) {
    HWND fgw = GetForegroundWindow();
    if (fgw == h) return;
    DWORD fg = fgw ? GetWindowThreadProcessId(fgw, nullptr) : 0, me = GetCurrentThreadId();
    if (fg && fg != me) AttachThreadInput(fg, me, TRUE);
    SetForegroundWindow(h);
    BringWindowToTop(h);
    SetFocus(h);
    if (fg && fg != me) AttachThreadInput(fg, me, FALSE);
}

void ToggleFullscreen(App& a) {
    DWORD style = (DWORD)GetWindowLongW(a.hwnd, GWL_STYLE);
    if (!a.fullscreen) {
        MONITORINFO mi{sizeof(mi)};
        GetWindowPlacement(a.hwnd, &a.prevPlacement);
        GetMonitorInfoW(MonitorFromWindow(a.hwnd, MONITOR_DEFAULTTONEAREST), &mi);
        SetWindowLongW(a.hwnd, GWL_STYLE, (style & ~WS_OVERLAPPEDWINDOW) | WS_POPUP);
        // Topmost while fullscreen so the taskbar can never sit on top of the game.
        SetWindowPos(a.hwnd, HWND_TOPMOST, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_NOOWNERZORDER | SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        ForceForeground(a.hwnd);
        a.fullscreen = true;
        a.dashboard = false;
    } else {
        SetWindowLongW(a.hwnd, GWL_STYLE, (style & ~WS_POPUP) | WS_OVERLAPPEDWINDOW);
        SetWindowPos(a.hwnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        SetWindowPlacement(a.hwnd, &a.prevPlacement);
        SetWindowPos(a.hwnd, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        a.fullscreen = false;
        a.dashboard = true;
    }
    a.lastMouse = NowMs();
}

// "Start with Windows": the per-user Run key (no admin, no scheduled task), launches fullscreen.
bool AutoStartEnabled() {
    wchar_t buf[MAX_PATH * 2];
    DWORD sz = sizeof(buf);
    return RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", L"FrameLift", RRF_RT_REG_SZ,
                        nullptr, buf, &sz) == ERROR_SUCCESS;
}

void SetAutoStart(bool on) {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_SET_VALUE, &k) != ERROR_SUCCESS)
        return;
    if (on) {
        wchar_t exe[MAX_PATH];
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring cmd = L"\"" + std::wstring(exe) + L"\" --fullscreen";
        RegSetValueExW(k, L"FrameLift", 0, REG_SZ, (const BYTE*)cmd.c_str(), (DWORD)((cmd.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(k, L"FrameLift");
    }
    RegCloseKey(k);
}

// A rounded tile with a caption, a big value, a sub line and a sparkline.
void StatTile(const char* id, const char* caption, const char* value, const char* unit, const char* sub, ImVec4 valueColor,
              const std::vector<float>& hist, float maxV, ImVec4 lineColor, float w, const char* tip) {
    App& a = *g;
    const float h = 128 * a.dpi;
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(1, 1, 1, 0.035f));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 8 * a.dpi);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12 * a.dpi, 10 * a.dpi));
    ImGui::BeginChild(id, ImVec2(w, h), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoScrollbar);
    ImGui::PushFont(a.fontSmall);
    ImGui::TextColored(kDim, "%s", caption);
    ImGui::PopFont();
    ImGui::PushFont(a.fontBig);
    ImGui::TextColored(valueColor, "%s", value);
    ImGui::PopFont();
    if (unit && *unit) {
        ImGui::SameLine(0, 4 * a.dpi);
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 12 * a.dpi);
        ImGui::TextColored(kDim, "%s", unit);
    }
    ImGui::PushFont(a.fontSmall);
    ImGui::TextColored(kDim, "%s", sub);
    ImGui::PopFont();
    Graph((std::string(id) + "g").c_str(), hist, maxV, ImGui::GetContentRegionAvail().y, lineColor, 120);
    ImGui::EndChild();
    if (tip && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
}

void Hint(const char* tip) {
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
}

void SubHeading(const char* t) {
    ImGui::Dummy(ImVec2(0, 2 * g->dpi));
    ImGui::PushFont(g->fontSmall);
    ImGui::TextColored(kDim, "%s", t);
    ImGui::PopFont();
}

// Label | slider | value: ImGui centres the value inside the slider, where the handle covers it.
bool LabeledSlider(const char* label, const char* id, int* v, int lo, int hi, const char* valueFmt, float labelW) {
    App& a = *g;
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::SameLine(labelW + ImGui::GetStyle().WindowPadding.x);
    const float valW = 46 * a.dpi;
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - valW);
    bool changed = ImGui::SliderInt(id, v, lo, hi, "");
    ImGui::SameLine();
    char b[32];
    snprintf(b, sizeof(b), valueFmt, *v);
    float tw = ImGui::CalcTextSize(b).x;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + valW - ImGui::GetStyle().ItemSpacing.x - tw);
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(kDim, "%s", b);
    return changed;
}

void DrawDashboard(App& a, const Pacing& p, const CaptureStats& cs, Status status) {
    ImGuiIO& io = ImGui::GetIO();
    float W = PanelWidth(a);
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - W, 0));
    ImGui::SetNextWindowSize(ImVec2(W, io.DisplaySize.y));
    ImGui::SetNextWindowBgAlpha(a.fullscreen ? 0.9f : 1.0f);
    ImGui::Begin("##dash", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings);
    const float full = ImGui::GetContentRegionAvail().x;

    // ---- Header: name, and a status pill on the right ----
    {
        ImGui::PushFont(a.fontBig);
        ImGui::TextUnformatted("FrameLift");
        ImGui::PopFont();
        const char* label = "No capture card";
        ImVec4 col = kDim;
        if (status == Status::Live) { label = "Live"; col = kAccent; }
        else if (status == Status::NoSignal) { label = "No signal"; col = kAmber; }
        else if (status == Status::OpenFailed) { label = "Card busy"; col = kRed; }
        ImVec2 ts = ImGui::CalcTextSize(label);
        float r = 4 * a.dpi, padX = 10 * a.dpi, pillW = ts.x + padX * 2 + r * 2 + 6 * a.dpi, pillH = ts.y + 8 * a.dpi;
        ImVec2 wp = ImGui::GetWindowPos();
        ImVec2 o(wp.x + ImGui::GetStyle().WindowPadding.x + full - pillW, wp.y + ImGui::GetStyle().WindowPadding.y + 8 * a.dpi);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec4 bg = col;
        bg.w = 0.14f;
        dl->AddRectFilled(o, ImVec2(o.x + pillW, o.y + pillH), ImGui::GetColorU32(bg), pillH * 0.5f);
        dl->AddCircleFilled(ImVec2(o.x + padX + r, o.y + pillH * 0.5f), r, ImGui::GetColorU32(col));
        dl->AddText(ImVec2(o.x + padX + r * 2 + 6 * a.dpi, o.y + 4 * a.dpi), ImGui::GetColorU32(col), label);
    }

    // ---- Source ----
    Section("SOURCE");
    std::string cur = a.cap.IsOpen() ? Narrow(a.cap.Device().name) : std::string("Select a capture card");
    ImGui::SetNextItemWidth(-1);
    if (ImGui::BeginCombo("##dev", cur.c_str())) {
        if (a.devices.empty()) ImGui::TextDisabled("No video devices found");
        for (auto& d : a.devices) {
            std::string n = Narrow(d.name);
            if (d.score <= 0) n += "  (camera)";
            bool sel = a.cap.IsOpen() && d.symlink == a.cap.Device().symlink;
            if (ImGui::Selectable(n.c_str(), sel) && !sel) OpenDevice(a, d, -1);
        }
        ImGui::EndCombo();
    }
    if (a.cap.IsOpen()) {
        auto modes = a.cap.Modes();
        int am = a.cap.ActiveMode();
        ImGui::SetNextItemWidth(-1);
        if (ImGui::BeginCombo("##mode", am >= 0 && am < (int)modes.size() ? modes[am].label.c_str() : "Mode")) {
            for (int i = 0; i < (int)modes.size(); i++)
                if (ImGui::Selectable(modes[i].label.c_str(), i == am) && i != am) {
                    VideoDevice d = a.cap.Device();
                    OpenDevice(a, d, i);
                    break;
                }
            ImGui::EndCombo();
        }
        if (status == Status::Live) {
            // One line: what the game is doing, and how it's shown.
            D3D11_VIEWPORT vp = VideoViewport(a);
            char scale[48] = "";
            if (a.r.VideoHeight() && std::fabs(vp.Height - a.r.VideoHeight()) > 1)
                snprintf(scale, sizeof(scale), "  \xC2\xB7  %dp \xE2\x86\x92 %.0fp", a.r.VideoHeight(), vp.Height);
            ImGui::TextColored(kDim, "Game %.0f fps  \xC2\xB7  signal %.0f%s", cs.uniqueFps, cs.signalFps, scale);
            if (cs.uniqueFps < 1) {
                ImGui::PushTextWrapPos();
                ImGui::TextColored(kAmber, "The picture isn't changing. Is the console paused, off, or on another input?");
                ImGui::PopTextWrapPos();
            }
        } else if (status == Status::NoSignal) {
            ImGui::PushTextWrapPos();
            ImGui::TextColored(kAmber, "Card found. Turn the console on or check the HDMI cable.");
            ImGui::PopTextWrapPos();
        }
    } else if (status == Status::OpenFailed) {
        ImGui::PushTextWrapPos();
        ImGui::TextColored(kRed, "%s is in use by another app (OBS, Elgato Studio, Discord). Close it and FrameLift reconnects.",
                           a.failedName.c_str());
        ImGui::PopTextWrapPos();
    } else {
        ImGui::PushTextWrapPos();
        ImGui::TextColored(kDim, "Plug in a capture card. It's picked up automatically.");
        ImGui::PopTextWrapPos();
    }

    // ---- Frame generation ----
    Section("FRAME GENERATION");
    Toggle("Frame generation", &a.s.frameGen);
    Hint("Generates in-between frames to reach the target. Shortcut: G");
    {
        const int presets[] = {30, 60, 90, 120};
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4 * a.dpi, 0));
        float bw = (full - 4 * a.dpi * 3) / 4;
        for (int i = 0; i < 4; i++) {
            if (i) ImGui::SameLine();
            bool on = a.s.targetFps == presets[i];
            ImGui::PushStyleColor(ImGuiCol_Button, on ? ImVec4(kAccent.x * 0.42f, kAccent.y * 0.42f, kAccent.z * 0.42f, 1)
                                                      : ImGui::GetStyleColorVec4(ImGuiCol_FrameBg));
            char l[24];
            snprintf(l, sizeof(l), "%d fps", presets[i]);
            if (ImGui::Button(l, ImVec2(bw, 30 * a.dpi))) a.s.targetFps = presets[i];
            ImGui::PopStyleColor();
        }
        ImGui::PopStyleVar();
    }
    if (p.fgActive && p.fillingDips) {
        ImGui::TextColored(kAccent, "Steady %.0f fps  \xC2\xB7  filling dips (game low %.0f)", p.outHz, p.srcLowFps);
    } else if (p.fgActive) {
        ImGui::TextColored(kAccent, "%.0f \xE2\x86\x92 %.0f fps  (%.1f\xC3\x97)", p.srcFps, p.outHz, p.outHz / p.srcFps);
    } else {
        ImGui::TextColored(kDim, "%s", p.reason);
    }
    if (a.s.vsync && std::fabs(p.outHz - a.s.targetFps) > 1.0 && p.refresh > 0) {
        ImGui::PushTextWrapPos();
        ImGui::TextColored(kDim, "Your display runs at %.0f Hz, so output is %.0f fps. VSync off (Advanced) allows exact targets.",
                           p.refresh, p.outHz);
        ImGui::PopTextWrapPos();
    }

    // ---- Performance: two tiles ----
    Section("PERFORMANCE");
    {
        const float tw = (full - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
        const bool haveLat = a.latAvg > 0 && status == Status::Live && cs.uniqueFps >= 1;
        char v[32], sub[64];
        ImVec4 lc = kDim;
        if (haveLat) {
            snprintf(v, sizeof(v), "%.0f", a.latAvg);
            // Judge by what frame generation adds on top of the capture path itself.
            const float extra = (p.fgActive && a.passLat > 0) ? a.latAvg - a.passLat : 0;
            lc = extra < 18 ? kAccent : (extra < 30 ? kAmber : kRed);
            if (p.fgActive && a.passLat > 0) snprintf(sub, sizeof(sub), "+%.0f ms from frame gen", std::max(0.0f, extra));
            else snprintf(sub, sizeof(sub), "p95 %.0f ms", a.latP95);
        } else {
            snprintf(v, sizeof(v), "--");
            snprintf(sub, sizeof(sub), "waiting for video");
        }
        StatTile("##lat", "ADDED LATENCY", v, haveLat ? "ms" : "", sub, lc, a.latHist, 60.0f, lc, tw,
                 "Measured on every frame: from the capture card delivering a frame to the\n"
                 "refresh where that moment reached your screen. Includes the frame-gen hold.\n"
                 "Excludes the console and the card's own delay.");
        ImGui::SameLine();
        if (a.gpu.Available()) {
            snprintf(v, sizeof(v), "%.0f", a.gpu.Total());
            snprintf(sub, sizeof(sub), "FrameLift %.0f%%", a.gpu.Self());
        } else {
            snprintf(v, sizeof(v), "--");
            snprintf(sub, sizeof(sub), "counters unavailable");
        }
        static const std::string gpuTip = std::string("Overall GPU load (same counters as Task Manager).\n") + a.r.AdapterName();
        StatTile("##gpu", "GPU", v, "%", sub, ImGui::GetStyleColorVec4(ImGuiCol_Text), a.gpu.History(), 100.0f,
                 ImVec4(0.45f, 0.65f, 1.0f, 1.0f), tw, gpuTip.c_str());
    }

    // ---- Audio ----
    Section("AUDIO");
    {
        std::string an = a.audio.DeviceId().empty() ? std::string("No capture audio found") : Narrow(a.audio.DeviceName());
        ImGui::SetNextItemWidth(-1);
        if (ImGui::BeginCombo("##aud", an.c_str())) {
            if (!a.audioListValid) {
                a.audioList = AudioPassthrough::ListCaptureDevices();
                a.audioListValid = true;
            }
            if (ImGui::Selectable("None", a.audio.DeviceId().empty())) a.audio.Stop();
            for (auto& d : a.audioList)
                if (ImGui::Selectable(Narrow(d.name).c_str(), d.id == a.audio.DeviceId()) && d.id != a.audio.DeviceId())
                    a.audio.Start(d.id, d.name);
            ImGui::EndCombo();
        } else {
            a.audioListValid = false;
        }
        int vol = (int)(a.s.volume * 100 + 0.5f);
        if (LabeledSlider("Volume", "##vol", &vol, 0, 200, a.s.mute ? "muted" : "%d%%", 92 * a.dpi)) a.s.volume = vol / 100.0f;
        Toggle("Mute", &a.s.mute);
        Hint("Shortcut: M");
        ImGui::SameLine(full * 0.5f + ImGui::GetStyle().WindowPadding.x);
        Toggle("Keep in sync", &a.s.avSync);
        Hint("Delays the sound by the frame-gen hold so it lines up with the picture.");
        if (a.listenWarning) {
            ImGui::PushTextWrapPos();
            ImGui::TextColored(kAmber,
                               "Audio plays twice: Windows \"Listen to this device\" is on for this card. "
                               "Turn it off in Sound settings > More sound settings > Recording > the card > Properties > Listen.");
            ImGui::PopTextWrapPos();
        }
    }

    // ---- Fullscreen ----
    ImGui::Dummy(ImVec2(0, 4 * a.dpi));
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(kAccent.x * 0.42f, kAccent.y * 0.42f, kAccent.z * 0.42f, 1));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(kAccent.x * 0.52f, kAccent.y * 0.52f, kAccent.z * 0.52f, 1));
    if (ImGui::Button(a.fullscreen ? "Exit fullscreen" : "Play fullscreen", ImVec2(-1, 40 * a.dpi))) ToggleFullscreen(a);
    ImGui::PopStyleColor(2);
    Hint("F11, Alt+Enter or double-click the picture. Esc leaves fullscreen.");

    // ---- Advanced ----
    ImGui::Dummy(ImVec2(0, 2 * a.dpi));
    if (a.openAdvanced) ImGui::SetNextItemOpen(true, ImGuiCond_Always);
    if (ImGui::CollapsingHeader("Advanced")) {
        SubHeading("DISPLAY");
        Toggle("VSync", &a.s.vsync);
        Hint("On: no tearing; output snaps to your display's refresh.\nOff: exact target FPS and lowest latency. Best with G-Sync/FreeSync.");
        Toggle("Late present", &a.s.lateLatch);
        Hint("Waits until just before each refresh to pick the moment shown (VSync on).\nAbout 10 ms less latency.");
        Toggle("Stats in fullscreen", &a.s.overlay);
        Hint("Small line in the corner: fps out, game fps, latency, GPU. Shortcut: O");
        int pacing = a.s.smooth ? 1 : 0;
        ImGui::SetNextItemWidth(-1);
        if (ImGui::Combo("##pace", &pacing, "Pacing: lowest latency\0Pacing: smoothest (+1 frame)\0")) a.s.smooth = pacing == 1;
        int sc = a.s.scaler;
        ImGui::SetNextItemWidth(-1);
        if (ImGui::Combo("##scaler", &sc, "Scaling: bilinear\0Scaling: sharp\0")) a.s.scaler = sc;
        Hint("How the capture is resized to your screen (e.g. 1080p to 1440p or 4K).\n"
             "Sharp: edge-preserving Lanczos plus adaptive sharpening (about 1.8 ms at 4K).");
        if (a.s.scaler == 1) {
            LabeledSlider("Sharpening", "##sharp", &a.s.sharpness, 0, 100, "%d%%", 92 * a.dpi);
        }

        SubHeading("SYSTEM");
        {
            bool autostart = AutoStartEnabled();
            if (Toggle("Start with Windows", &autostart)) SetAutoStart(autostart);
            Hint("Opens FrameLift fullscreen when you sign in to Windows.");
        }
        if (a.r.IsNvidia()) {
            if (Toggle("Keep GPU at full speed", &a.s.nvMaxPerf)) {
                std::string r;
                NvSetMaxPerformance(a.s.nvMaxPerf, r);
                Log("nvprofile: %s", r.c_str());
            }
            Hint("Sets NVIDIA \"Prefer maximum performance\" for FrameLift only.\n"
                 "Stops the GPU down-clocking, which slows motion analysis. Applies next launch.");
        }

        SubHeading("DIAGNOSTICS");
        {
            int eng = a.s.engine;
            ImGui::SetNextItemWidth(-1);
            if (ImGui::Combo("##engine", &eng, "Motion: shader (any GPU)\0Motion: auto (NVIDIA hardware)\0")) a.s.engine = eng;
            static const char* tierName[] = {"game ~30 fps: best quality", "game 30-56 fps: balanced", "game at target: frames passed through"};
            ImGui::PushFont(a.fontSmall);
            ImGui::TextColored(kDim, "Using %s  \xC2\xB7  %s", a.tier == 2 ? "no motion" : a.r.EngineName(), tierName[std::clamp(a.tier, 0, 2)]);
            ImGui::PopFont();
        }
        Toggle("Show motion vectors", &a.s.showMotion);
        if (Toggle("Collect 60 fps test clips", &a.s.collectClips)) {
            if (a.s.collectClips) a.clips.Start(ClipsDir(), 12, 10.0);
            else a.clips.Stop();
        }
        Hint("Saves short clips while a game truly runs at 60 fps, for tuning frame generation.");
        ImGui::PushFont(a.fontSmall);
        if (a.s.collectClips)
            ImGui::TextColored(kDim, "%d clips saved%s", a.clips.Saved(), a.clips.Writing() ? " (writing)" : "");
        ImGui::TextColored(kDim, "Output %.0f fps  \xC2\xB7  display %.2f Hz  \xC2\xB7  held frames %zu/min", a.outFps, p.refresh,
                           a.stutterTimes.size());
        if (a.cap.IsOpen())
            ImGui::TextColored(kDim, "%s  \xC2\xB7  audio buffer %.0f ms", a.cap.FormatLabel().c_str(), a.audio.bufferedMs.load());
        ImGui::TextColored(kDim, "%s", a.r.AdapterName().c_str());
        ImGui::PopFont();
    }
    ImGui::Dummy(ImVec2(0, 2 * a.dpi));
    ImGui::PushFont(a.fontSmall);
    ImGui::TextColored(kDim, "F11 fullscreen  \xC2\xB7  Tab panel  \xC2\xB7  G frame gen  \xC2\xB7  M mute  \xC2\xB7  O stats");
    ImGui::TextColored(kDim, "FrameLift %s", FRAMELIFT_VERSION);
    ImGui::PopFont();
    ImGui::End();
}

void DrawUI(App& a, const Pacing& p) {
    CaptureStats cs = a.cap.Stats();
    Status status = GetStatus(a, cs);
    D3D11_VIEWPORT vp = VideoViewport(a);

    if (!a.r.HasVideo() || status != Status::Live) {
        const char* title = "Connect a capture card";
        const char* sub = "Elgato, AVerMedia and most USB HDMI capture devices are detected automatically.";
        if (status == Status::NoSignal) {
            title = "No signal";
            sub = "The capture card is connected. Turn on the console or check the HDMI cable.";
        } else if (status == Status::OpenFailed) {
            title = "Capture card is busy";
            sub = "Another app is using it. Close it and FrameLift reconnects on its own.";
        }
        ImDrawList* dl = ImGui::GetBackgroundDrawList();
        float cx = vp.TopLeftX + vp.Width * 0.5f, cy = vp.TopLeftY + vp.Height * 0.5f;
        ImVec2 ts = a.fontBig->CalcTextSizeA(a.fontBig->FontSize, FLT_MAX, 0, title);
        dl->AddText(a.fontBig, a.fontBig->FontSize, ImVec2(cx - ts.x * 0.5f, cy - ts.y), IM_COL32(235, 237, 240, 255), title);
        float wrap = std::min(vp.Width - 40 * a.dpi, 520 * a.dpi);
        ImVec2 ss = a.fontBody->CalcTextSizeA(a.fontBody->FontSize, FLT_MAX, wrap, sub);
        dl->AddText(a.fontBody, a.fontBody->FontSize, ImVec2(cx - ss.x * 0.5f, cy + 8 * a.dpi), IM_COL32(150, 154, 160, 255),
                    sub, nullptr, wrap);
    }

    if (a.dashboard) DrawDashboard(a, p, cs, status);

    if (a.fullscreen && !a.dashboard && a.s.overlay && status == Status::Live) {
        char line[160];
        // "game in" = real new game frames counted over the last second (live, shows dips),
        // not the median frame time used for pacing.
        if (p.fgActive)
            snprintf(line, sizeof(line), "%.0f fps out  \xC2\xB7  %.0f game in  \xC2\xB7  +%.0f ms  \xC2\xB7  GPU %.0f%%", a.outFps,
                     cs.uniqueFps, a.latAvg, a.gpu.Total());
        else
            snprintf(line, sizeof(line), "%.0f fps out  \xC2\xB7  %.0f game in  \xC2\xB7  frame gen off  \xC2\xB7  +%.0f ms  \xC2\xB7  GPU %.0f%%",
                     a.outFps, cs.uniqueFps, a.latAvg, a.gpu.Total());
        ImDrawList* dl = ImGui::GetForegroundDrawList();
        ImFont* f = a.fontSmall;
        ImVec2 sz = f->CalcTextSizeA(f->FontSize, FLT_MAX, 0, line);
        float pad = 9 * a.dpi, dot = 3.5f * a.dpi;
        ImVec2 o(16 * a.dpi, 16 * a.dpi);
        const float extra = (p.fgActive && a.passLat > 0) ? a.latAvg - a.passLat : 0;
        ImVec4 lc = !p.fgActive ? kDim : (extra < 18 ? kAccent : (extra < 30 ? kAmber : kRed));
        dl->AddRectFilled(o, ImVec2(o.x + sz.x + pad * 2 + dot * 2 + 6 * a.dpi, o.y + sz.y + pad * 1.2f), IM_COL32(0, 0, 0, 160),
                          (sz.y + pad * 1.2f) * 0.5f);
        dl->AddCircleFilled(ImVec2(o.x + pad + dot, o.y + (sz.y + pad * 1.2f) * 0.5f), dot, ImGui::GetColorU32(lc));
        dl->AddText(f, f->FontSize, ImVec2(o.x + pad + dot * 2 + 6 * a.dpi, o.y + pad * 0.6f), IM_COL32(235, 237, 240, 255), line);
    }

    if (NowMs() < a.toastUntil && !a.toast.empty()) {
        ImDrawList* dl = ImGui::GetForegroundDrawList();
        ImVec2 sz = a.fontBody->CalcTextSizeA(a.fontBody->FontSize, FLT_MAX, 0, a.toast.c_str());
        float pad = 12 * a.dpi;
        float cx = vp.TopLeftX + vp.Width * 0.5f, y = vp.TopLeftY + 40 * a.dpi;
        dl->AddRectFilled(ImVec2(cx - sz.x * 0.5f - pad, y), ImVec2(cx + sz.x * 0.5f + pad, y + sz.y + pad), IM_COL32(0, 0, 0, 170),
                          8 * a.dpi);
        dl->AddText(a.fontBody, a.fontBody->FontSize, ImVec2(cx - sz.x * 0.5f, y + pad * 0.5f), IM_COL32(240, 242, 245, 255),
                    a.toast.c_str());
    }

    // Hide the cursor while playing fullscreen.
    if (a.fullscreen && !a.dashboard && NowMs() - a.lastMouse > 2000) ImGui::SetMouseCursor(ImGuiMouseCursor_None);
}

void UpdateStats(App& a, double now) {
    // Save settings ~2 s after any change, so they survive a crash or a forced restart.
    {
        std::wstring k = SettingsKey(a.s);
        if (k != a.savedKey) {
            if (a.settingsChangedAt == 0) a.settingsChangedAt = now;
            if (now - a.settingsChangedAt > 2000) {
                SaveSettings(a.s);
                a.savedKey = k;
                a.settingsChangedAt = 0;
            }
        } else {
            a.settingsChangedAt = 0;
        }
    }
    if (now - a.statsT0 < 500) return;
    double dt = now - a.statsT0;
    a.statsT0 = now;
    a.outFps = (float)(a.outFrames * 1000.0 / dt);
    a.outFrames = 0;
    if (!a.lat.empty()) {
        std::vector<float> v(a.lat.begin(), a.lat.end());
        float sum = 0;
        for (float x : v) sum += x;
        a.latAvg = sum / v.size();
        std::sort(v.begin(), v.end());
        a.latP95 = v[std::min(v.size() - 1, (size_t)(v.size() * 0.95))];
        a.latHist.push_back(a.latAvg);
        if (a.latHist.size() > 120) a.latHist.erase(a.latHist.begin());
    }
    static double lastLog = 0;
    if (now - lastLog >= 2000) {
        lastLog = now;
        CaptureStats cs = a.cap.Stats();
        const Pacing& p = a.lastPacing;
        Log("stats: signal %.2f fps, game %.2f fps (period %.2f ms) | out %.1f fps @ %.2f Hz vsync=%d | fg %s%s | "
            "latency avg %.1f p95 %.1f ms, hold %.1f, margin %.1f, fg-off est %.1f | GPU %.0f%% (self %.0f%%), %.2f ms/frame | "
            "audio %s buf %.0f ms (underruns %d, jumps %d) | %s | low %.1f fps%s | held frames %zu/min | engine %s (hw step %.1f ms) | tier %d | late-latch %.1f ms (draw peak %.1f) | motion pad %.1f ms, late %d, nv done max %.1f ms, forced %d",
            cs.signalFps, cs.uniqueFps, a.sourcePeriod, a.outFps, p.refresh, p.vsync ? 1 : 0, p.fgActive ? "ON" : "off ",
            p.fgActive ? "" : p.reason, a.latAvg, a.latP95, a.holdMs, a.margin, a.passLat, a.gpu.Total(), a.gpu.Self(),
            a.r.GpuMs(), a.audio.streaming ? "on" : "OFF", a.audio.bufferedMs.load(), a.audio.underruns.load(), a.audio.jumps.load(), a.fullscreen ? "fullscreen" : "windowed",
            p.srcLowFps, p.fillingDips ? " (filling dips)" : "", a.stutterTimes.size(), a.r.EngineName(), a.r.NvMs(), a.tier,
            a.latchWaitMs, a.r.DrawMsPeak(), a.motionPad, a.flowLate, a.r.nvDoneMax_, a.r.nvForced_);
        a.flowLate = 0;
        a.r.nvDoneMax_ = 0;
        a.r.nvForced_ = 0;
    }
}

// ------------------------------------------------------------------------------------------
// Frame
// ------------------------------------------------------------------------------------------
void Render(App& a, const Pacing& p) {
    // Don't leave a stale picture up when the card reports no input.
    if (a.r.HasVideo() && a.cap.Stats().noSignal) {
        a.r.ResetTimeline();
        a.intervals.clear();
        a.sourcePeriod = a.periodHi = 0;
    }
    const bool interp = p.fgActive;
    double t0 = NowMs();
    if (interp) {
        const double dt = a.lastRenderT > 0 ? std::clamp(t0 - a.lastRenderT, 0.0, 100.0) : 0.0;
        // Slowest-recent-gap memory fades back to typical over ~3 s.
        if (a.periodPeak > a.sourcePeriod) a.periodPeak -= (a.periodPeak - a.sourcePeriod) * std::min(1.0, dt / 3000.0);
        // Grow fast, shrink slow: up to 2 ms per frame when frames get slower (a brief ~12% slow-down
        // of the picture, not a hitch), 0.25 ms per frame back down once they recover.
        if (a.dCur < 0) {
            a.dCur = p.D;
            a.interpSince = t0;
        }
        else if (p.D > a.dCur) a.dCur += std::min(p.D - a.dCur, 2.0);
        else a.dCur -= std::min(a.dCur - p.D, 0.75);  // recover from a dip within ~1 s (45 ms/s)
        // Elastic playback: if the next real frame is late, don't let the displayed moment run past
        // the newest real frame (that caused a freeze followed by a skip). Hold the clock there, so the
        // late frame continues smoothly from where we paused, and keep the larger hold afterwards.
        const double nts = a.r.NewestTs();
        const bool overdue = nts > 0 && t0 - nts > a.sourcePeriod * 1.15 && t0 - nts < 250.0;
        if (overdue && t0 - a.dCur > nts) {
            if (!a.starving) {
                a.stutterTimes.push_back(t0);
                a.starving = true;
            }
            // Bridge a late frame by up to half a normal frame; beyond that, stutter rather than lag.
            a.dCur = std::min(t0 - nts, p.D + a.sourcePeriod * 0.5);
        } else if (!overdue) {
            a.starving = false;
        }
        while (!a.stutterTimes.empty() && t0 - a.stutterTimes.front() > 60000) a.stutterTimes.pop_front();
    }
    a.r.BeginGpuTimer();
    a.r.BeginDrawTimer();
    a.r.BeginFrame();
    D3D11_VIEWPORT vp = VideoViewport(a);
    double newestTs = a.r.NewestTs();
    double d = t0 - (interp ? a.dCur : 0.0);
    DrawResult res;
    if (a.r.HasVideo()) res = a.r.DrawVideo(d, interp, vp, a.s.showMotion);

    if (interp && newestTs > 0) {
        if (p.vsync) {
            double ang = 2 * kPi * std::fmod(t0 - newestTs, p.O) / p.O;
            a.phaseC = a.phaseC * 0.95 + std::cos(ang) * 0.05;
            a.phaseS = a.phaseS * 0.95 + std::sin(ang) * 0.05;
            a.phaseSamples++;
        }
        // Starved = we ran past the newest real frame while the next one was due. Grow the margin.
        if (d > newestTs + p.O * 1.05 && t0 - newestTs < 2.5 * a.sourcePeriod + a.dCur) a.margin = std::min(4.0, a.margin + 0.5);
        else a.margin = std::max(1.0, a.margin - 0.002);
        // Motion not ready in time. With VSync the hold snaps to whole refreshes (ticks stay locked
        // to real frames), so anything less than a refresh changes nothing: step a full refresh at
        // once (one repeated frame, not a burst), then relax very slowly (~1.5 min per refresh).
        // Whether it's needed depends on how game frames line up with refreshes, which drifts.
        // Late ticks while frame gen is still warming up (first motion engine run, first pairs)
        // say nothing about steady state: ignore them for 4 s. A second refresh of pad (rare)
        // relaxes within ~15 s; the first one slowly.
        const bool warming = t0 - a.interpSince < 4000;
        if (res.flowLate && !warming) {
            a.motionPad = std::min(2.0 * p.O, a.motionPad + p.O);
            a.flowLate++;
        } else if (!res.flowLate) {
            a.motionPad = std::max(0.0, a.motionPad - (a.motionPad > p.O + 0.5 ? 0.02 : 0.003));
        }
    }
    a.lastRenderT = t0;
    a.holdMs = interp ? (float)a.dCur : 0.0f;
    a.audio.extraDelayMs = (a.s.avSync && interp) ? (float)a.dCur : 0.0f;
    a.audio.volume = a.s.volume;
    a.audio.muted = a.s.mute;

    if (a.fontsDirty) {
        a.fontsDirty = false;
        LoadFonts(a);
        SetupStyle(a);
        ImGui_ImplDX11_InvalidateDeviceObjects();
    }
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    DrawUI(a, p);
    ImGui::Render();
    a.r.BindBackbuffer();
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

    a.r.EndDrawTimer();
    if (a.shotCountdown >= 0 && a.shotCountdown-- == 0) {
        std::vector<uint8_t> bgr;
        int w = 0, h = 0;
        if (a.r.ReadBackbuffer(bgr, w, h)) SavePngBgr(a.shotPath, bgr, w, h);
    }
    UINT presentId = a.r.Present(p.sync, p.tear);
    a.r.KickMotion();  // motion for the newest pair gets a whole refresh before the next draw
    double tp = NowMs();
    if (res.any && res.contentTs != a.lastContentTs) {
        a.lastContentTs = res.contentTs;
        a.inFlight.push_back({presentId, res.contentTs, tp, interp});
        if (a.inFlight.size() > 16) a.inFlight.pop_front();
        a.outFrames++;
    }
    auto record = [&](float l, bool wasInterp) {
        if (l < 0 || l > 500) return;
        a.lat.push_back(l);
        if (a.lat.size() > 120) a.lat.pop_front();
        if (!wasInterp) a.passLat = a.passLat > 0 ? a.passLat * 0.95f + l * 0.05f : l;
    };
    // Preferred: DXGI tells us which present reached the screen and at which vblank.
    UINT shownId = 0;
    double vblankMs = 0;
    if (a.r.FrameStats(shownId, vblankMs)) {
        a.statsOkMs = tp;
        while (!a.inFlight.empty() && a.inFlight.front().id < shownId) a.inFlight.pop_front();
        if (!a.inFlight.empty() && a.inFlight.front().id == shownId) {
            record((float)(vblankMs - a.inFlight.front().contentTs), a.inFlight.front().interp);
            a.inFlight.pop_front();
        }
    } else if (tp - a.statsOkMs > 1000) {
        // Fallback when the driver/compositor gives no frame statistics: time to Present().
        for (auto& f : a.inFlight) record((float)(f.presentedMs - f.contentTs), f.interp);
        a.inFlight.clear();
    }
    a.lastRender = tp;
    UpdateStats(a, tp);
}

void MainLoop(App& a) {
    HANDLE frameEvt = a.cap.FrameEvent();
    MSG msg;
    while (a.running) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) a.running = false;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (!a.running) break;
        if (a.displayChanged) {
            // A display mode change (e.g. 60 -> 59.94 Hz) drops the window off the direct-to-screen
            // path (measured: +30 ms). Re-cover the monitor, stay on top, and take focus again.
            a.displayChanged = false;
            Log("display: mode changed; re-applying fullscreen");
            if (a.fullscreen) {
                MONITORINFO mi{sizeof(mi)};
                GetMonitorInfoW(MonitorFromWindow(a.hwnd, MONITOR_DEFAULTTONEAREST), &mi);
                SetWindowPos(a.hwnd, HWND_TOPMOST, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                             mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_NOOWNERZORDER | SWP_FRAMECHANGED | SWP_SHOWWINDOW);
                ForceForeground(a.hwnd);
            }
        }
        if (a.resizePending) {
            a.resizePending = false;
            a.r.Resize(a.newW, a.newH);
        }
        double now = NowMs();
        ManageDevices(a, now);
        Pacing p = ComputePacing(a, now);
        a.lastPacing = p;

        if (p.fgActive && !p.vsync) {
            // Exact-rate pacing on a high-resolution timer; presents with tearing allowed.
            if (a.nextTick <= 0 || now - a.nextTick > p.O * 2 || a.nextTick - now > p.O * 2) a.nextTick = now;
            double wait = a.nextTick - now;
            if (wait > 0.25) {
                LARGE_INTEGER due;
                due.QuadPart = -(LONGLONG)(wait * 10000.0);
                SetWaitableTimer(a.timer, &due, 0, nullptr, nullptr, FALSE);
                HANDLE hs[2] = {frameEvt, a.timer};
                MsgWaitForMultipleObjectsEx(2, hs, (DWORD)wait + 5, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
                DrainCapture(a, p);
                if (NowMs() < a.nextTick - 0.25) continue;
            }
            DrainCapture(a, p);
            WaitForSingleObject(a.r.Waitable(), 30);
            Render(a, p);
            a.nextTick += p.O;
        } else if (p.fgActive) {
            // VSync pacing: the swap chain's waitable object fires once per refresh.
            a.nextTick = 0;
            // Also wake for each captured frame: its motion work starts the moment it arrives.
            HANDLE hs[2] = {a.r.Waitable(), frameEvt};
            DWORD w = MsgWaitForMultipleObjectsEx(2, hs, 50, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
            if (w == WAIT_OBJECT_0 + 1) {
                DrainCapture(a, p);
                continue;
            }
            if (w == WAIT_OBJECT_0 + 2) continue;  // input first; the waitable is still signalled
            const double tw = NowMs();  // a refresh just happened (the swap chain has room again)
            DrainCapture(a, p);
            if (a.s.lateLatch && w == WAIT_OBJECT_0) {
                // Late latching: the heavy motion work is already queued on the GPU; wait until just
                // before the next refresh, then pick the moment to show and do the cheap final draw.
                // The shown moment is up to ~10 ms fresher at no cost in quality.
                a.r.Flush();
                const double lead = std::clamp((double)a.r.DrawMsPeak() + 3.0, 4.0, p.O - 1.0);
                const double target = tw + p.O - lead;
                const double wait = target - NowMs();
                for (double left = wait; left > 0.5; left = target - NowMs()) {
                    LARGE_INTEGER due;
                    due.QuadPart = -(LONGLONG)(left * 10000.0);
                    SetWaitableTimer(a.timer, &due, 0, nullptr, nullptr, FALSE);
                    HANDLE ws[2] = {a.timer, frameEvt};
                    if (WaitForMultipleObjects(2, ws, FALSE, (DWORD)left + 5) != WAIT_OBJECT_0 + 1) break;
                    DrainCapture(a, p);  // a frame arrived mid-wait: start its motion now
                }
                a.latchWaitMs = a.latchWaitMs * 0.95f + (float)std::max(0.0, wait) * 0.05f;
            } else {
                a.latchWaitMs = 0;
            }
            Render(a, p);
        } else {
            // Passthrough: show each new game frame the moment it arrives.
            a.nextTick = 0;
            double since = now - a.lastRender;
            DWORD timeout = since >= 16 ? 0 : (DWORD)(16 - since);
            MsgWaitForMultipleObjectsEx(1, &frameEvt, timeout, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
            bool fresh = DrainCapture(a, p);
            if (fresh || NowMs() - a.lastRender >= 16) {
                WaitForSingleObject(a.r.Waitable(), 30);
                Render(a, p);
            }
        }
    }
}

LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (ImGui_ImplWin32_WndProcHandler(h, m, w, l)) return 1;
    App& a = *g;
    switch (m) {
        case WM_SIZE:
            if (w != SIZE_MINIMIZED) {
                a.resizePending = true;
                a.newW = LOWORD(l);
                a.newH = HIWORD(l);
            }
            return 0;
        case WM_MOUSEMOVE:
            a.lastMouse = NowMs();
            break;
        case WM_LBUTTONDBLCLK:
            if (!ImGui::GetIO().WantCaptureMouse) ToggleFullscreen(a);
            return 0;
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
            if (w == VK_F11 || (w == VK_RETURN && (GetKeyState(VK_MENU) & 0x8000))) {
                ToggleFullscreen(a);
                return 0;
            }
            if (ImGui::GetIO().WantTextInput) break;
            if (w == VK_ESCAPE && a.fullscreen) ToggleFullscreen(a);
            else if (w == VK_TAB) a.dashboard = !a.dashboard;
            else if (w == 'G') {
                a.s.frameGen = !a.s.frameGen;
                a.toast = a.s.frameGen ? "Frame generation on" : "Frame generation off";
                a.toastUntil = NowMs() + 1500;
            } else if (w == 'O') {
                a.s.overlay = !a.s.overlay;
                a.toast = a.s.overlay ? "Stats on" : "Stats off";
                a.toastUntil = NowMs() + 1500;
            } else if (w == 'M') {
                a.s.mute = !a.s.mute;
                a.toast = a.s.mute ? "Muted" : "Sound on";
                a.toastUntil = NowMs() + 1500;
            }
            if (m == WM_SYSKEYDOWN) break;
            return 0;
        case WM_DEVICECHANGE:
            if (w == DBT_DEVICEARRIVAL || w == DBT_DEVICEREMOVECOMPLETE || w == DBT_DEVNODES_CHANGED)
                a.enumAt = NowMs() + 600;  // debounce: Windows sends a burst per device
            return TRUE;
        case WM_DISPLAYCHANGE:
            a.displayChanged = true;
            break;
        case WM_DPICHANGED: {
            a.dpi = HIWORD(w) / 96.0f;
            a.fontsDirty = true;
            const RECT* r = reinterpret_cast<const RECT*>(l);
            SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }
        case WM_GETMINMAXINFO: {
            auto* mm = reinterpret_cast<MINMAXINFO*>(l);
            mm->ptMinTrackSize.x = (LONG)(800 * a.dpi);
            mm->ptMinTrackSize.y = (LONG)(560 * a.dpi);
            return 0;
        }
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

}  // namespace

// --uishot <out.png> [--frame <png>] [--size WxH] [--dpi 1.5] [--state live|nosignal|nodevice|busy]
//          [--fullscreen] [--nodash] [--advanced]
// Renders the real UI once with demo data (no capture card, audio or settings touched) and saves
// it, so the interface can be checked and compared without a console or a screen.
int RunUiShot(HINSTANCE inst, int argc, LPWSTR* argv) {
    static App app;
    g = &app;
    std::wstring framePath, state = L"live";
    int W = 1640, H = 760;
    bool fs = false, dash = true;
    for (int i = 1; i < argc; i++) {
        std::wstring k = argv[i];
        auto next = [&]() { return i + 1 < argc ? std::wstring(argv[++i]) : std::wstring(); };
        if (k == L"--uishot") app.shotPath = next();
        else if (k == L"--frame") framePath = next();
        else if (k == L"--size") swscanf(next().c_str(), L"%dx%d", &W, &H);
        else if (k == L"--dpi") app.dpi = (float)_wtof(next().c_str());
        else if (k == L"--state") state = next();
        else if (k == L"--fullscreen") fs = true;
        else if (k == L"--nodash") dash = false;
        else if (k == L"--advanced") app.openAdvanced = true;
    }
    if (app.dpi <= 0) app.dpi = 1;
    app.s = Settings{};
    app.savedKey = SettingsKey(app.s);  // never autosave demo settings over the user's
    app.statsT0 = NowMs() + 1e9;  // keep the demo numbers
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = inst;
    wc.lpszClassName = L"FrameLiftShot";
    RegisterClassExW(&wc);
    app.hwnd = CreateWindowExW(0, wc.lpszClassName, L"FrameLift", WS_POPUP, 0, 0, W, H, nullptr, nullptr, inst, nullptr);
    if (!app.hwnd || !app.r.Init(app.hwnd)) return 1;
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    LoadFonts(app);
    SetupStyle(app);
    ImGui_ImplWin32_Init(app.hwnd);
    ImGui_ImplDX11_Init(app.r.Device(), app.r.Ctx());
    app.fullscreen = fs;
    app.dashboard = dash;

    const bool live = state == L"live";
    if (live || state == L"nosignal") {
        VideoDevice d;
        d.name = L"Game Capture HD60 S";
        d.score = 100;
        std::vector<VideoMode> modes(3);
        const char* labels[] = {"1080p  \xC2\xB7  60 Hz  \xC2\xB7  YUY2", "1080p  \xC2\xB7  30 Hz  \xC2\xB7  YUY2", "720p  \xC2\xB7  60 Hz  \xC2\xB7  YUY2"};
        for (int i = 0; i < 3; i++) modes[i].label = labels[i];
        CaptureStats cs;
        cs.signalFps = 59.94;
        cs.uniqueFps = live ? 29.97 : 0;
        cs.noSignal = !live;
        app.cap.SetDemo(d, modes, 0, "1920x1080 YUY2 \xC2\xB7 BT.709 limited", cs);
        app.audio.SetDemo(L"demo", L"Game Capture HD60 S Audio");
    } else if (state == L"busy") {
        app.failedSymlink = L"demo";
        app.failedName = "Game Capture HD60 S";
        app.failedAt = NowMs();
    }
    std::vector<float> gh;
    for (int i = 0; i < 120; i++) gh.push_back(34 + 6 * std::sin(i * 0.21f) + 3 * std::sin(i * 0.9f));
    app.gpu.SetDemo(37, 33, gh);
    if (live) {
        int fw = 0, fh = 0;
        std::vector<uint8_t> px;
        if (!framePath.empty() && LoadPngBGRA(framePath, fw, fh, px)) {
            Frame f;
            f.data = px;
            f.width = fw;
            f.height = fh;
            f.fmt = PixFmt::BGRA;
            for (int i = 0; i < 3; i++) {
                f.ts = NowMs() - (3 - i) * 33.37;
                app.r.Upload(f, true, 1e9);
            }
            app.r.FinishMotion();
            app.sourcePeriod = app.periodHi = 33.37;
        }
        app.latAvg = 31.6f;
        app.latP95 = 33.2f;
        app.holdMs = 23.4f;
        app.passLat = 22.1f;
        for (int i = 0; i < 120; i++) app.latHist.push_back(31.5f + 1.6f * std::sin(i * 0.17f) + 0.8f * std::sin(i * 0.7f));
        app.outFps = 59.9f;
    }
    app.shotCountdown = 3;
    for (int i = 0; i < 4; i++) {
        Pacing p = ComputePacing(app, NowMs());
        app.lastPacing = p;
        Render(app, p);
    }
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    return 0;
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE, LPSTR, int show) {
    ImGui_ImplWin32_EnableDpiAwareness();
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    {
        // --bench <clips dir> --configs <file> --out <csv>: offline scoring on the GPU, no window.
        int argc = 0;
        LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        std::wstring benchDir, configs, outCsv = L"bench.csv";
        int stride = 2;
        for (int i = 1; argv && i + 1 < argc; i++) {
            std::wstring a = argv[i];
            if (a == L"--bench") benchDir = argv[++i];
            else if (a == L"--configs") configs = argv[++i];
            else if (a == L"--out") outCsv = argv[++i];
            else if (a == L"--stride") stride = _wtoi(argv[++i]);
        }
        for (int i = 1; argv && i < argc; i++)
            if (std::wstring(argv[i]) == L"--nv-reset") {  // run by the uninstaller
                std::string r;
                NvSetMaxPerformance(false, r);
                LocalFree(argv);
                CoUninitialize();
                return 0;
            }
        for (int i = 1; argv && i < argc; i++)
            if (std::wstring(argv[i]) == L"--uishot") {
                int rc = RunUiShot(inst, argc, argv);
                LocalFree(argv);
                CoUninitialize();
                return rc;
            }
        if (argv) LocalFree(argv);
        if (!benchDir.empty()) {
            if (AttachConsole(ATTACH_PARENT_PROCESS)) {
                freopen("CONOUT$", "w", stdout);
                freopen("CONOUT$", "w", stderr);
            }
            int rc = RunBench(benchDir, configs, outCsv, stride);
            CoUninitialize();
            return rc;
        }
    }
    MFStartup(MF_VERSION);
    timeBeginPeriod(1);
    SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS);

    // One FrameLift at a time: a second copy would fight the first for the capture card.
    HANDLE single = CreateMutexW(nullptr, TRUE, L"Local\\FrameLiftSingleInstance");
    if (single && GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HWND other = FindWindowW(L"FrameLiftWindow", nullptr)) {
            ShowWindow(other, SW_RESTORE);
            SetForegroundWindow(other);
        }
        CoUninitialize();
        return 0;
    }
    static App app;
    g = &app;
    LogInit();
    Log("FrameLift starting");
    app.s = LoadSettings();
    app.savedKey = SettingsKey(app.s);

    WNDCLASSEXW wc{sizeof(wc)};
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.hIconSm = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    wc.lpszClassName = L"FrameLiftWindow";
    RegisterClassExW(&wc);

    POINT origin{0, 0};
    HMONITOR mon = MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY);
    app.dpi = ImGui_ImplWin32_GetDpiScaleForMonitor(mon);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(mon, &mi);
    int workW = mi.rcWork.right - mi.rcWork.left, workH = mi.rcWork.bottom - mi.rcWork.top;
    int ww = std::min((int)((1280 + 360) * app.dpi), (int)(workW * 0.9));
    int wh = std::min((int)(760 * app.dpi), (int)(workH * 0.9));
    app.hwnd = CreateWindowExW(0, wc.lpszClassName, L"FrameLift", WS_OVERLAPPEDWINDOW, mi.rcWork.left + (workW - ww) / 2,
                               mi.rcWork.top + (workH - wh) / 2, ww, wh, nullptr, nullptr, inst, nullptr);
    {
        // Before the GPU device exists, so the driver reads the updated profile for this launch.
        std::string r;
        if (NvSetMaxPerformance(app.s.nvMaxPerf, r) || r != "no NVIDIA driver")
            Log("nvprofile: maximum performance %s: %s", app.s.nvMaxPerf ? "on" : "off", r.c_str());
    }
    if (!app.hwnd || !app.r.Init(app.hwnd)) {
        Log("startup failed: no usable D3D11 device / swap chain");
        MessageBoxW(nullptr, L"FrameLift needs a DirectX 11 GPU.", L"FrameLift", MB_ICONERROR);
        return 1;
    }
    ShowWindow(app.hwnd, show == SW_SHOWMINIMIZED ? SW_SHOWNORMAL : show);
    Log("window: dpi %.2f, display %.2f Hz, target %d fps, frame gen %s, vsync %d", app.dpi, app.r.RefreshHz(),
        app.s.targetFps, app.s.frameGen ? "on" : "off", app.s.vsync ? 1 : 0);

    DEV_BROADCAST_DEVICEINTERFACE_W filter{};
    filter.dbcc_size = sizeof(filter);
    filter.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
    HDEVNOTIFY notify =
        RegisterDeviceNotificationW(app.hwnd, &filter, DEVICE_NOTIFY_WINDOW_HANDLE | DEVICE_NOTIFY_ALL_INTERFACE_CLASSES);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    LoadFonts(app);
    SetupStyle(app);
    ImGui_ImplWin32_Init(app.hwnd);
    ImGui_ImplDX11_Init(app.r.Device(), app.r.Ctx());

    app.gpu.Start(app.r.AdapterLuid());
    if (app.s.collectClips) app.clips.Start(ClipsDir(), 12, 10.0);
    app.timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!app.timer) app.timer = CreateWaitableTimerW(nullptr, FALSE, nullptr);
    DWORD task = 0;
    HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Games", &task);
    app.lastMouse = app.statsT0 = NowMs();
    ForceForeground(app.hwnd);
    if (strstr(GetCommandLineA(), "--fullscreen")) ToggleFullscreen(app);
    gDump = strstr(GetCommandLineA(), "--dump") != nullptr;
    if (gDump) gCaptureTraceFrames = 1800;  // 30 s of per-frame dedupe measurements
    if (gDump) Log("diagnostics: --dump enabled");

    MainLoop(app);

    Log("exiting");
    if (!app.listenRestoreId.empty()) AudioPassthrough::SetListen(app.listenRestoreId, true);
    SaveSettings(app.s);
    if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
    if (notify) UnregisterDeviceNotification(notify);
    CloseDevice(app);
    app.clips.Stop();
    app.gpu.Stop();
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    app.r.Shutdown();
    CloseHandle(app.timer);
    MFShutdown();
    timeEndPeriod(1);
    CoUninitialize();
    return 0;
}
