#pragma once
#include "common.h"

enum class PixFmt : uint32_t { NV12 = 0, YUY2 = 1, BGRA = 2, P010 = 3 };  // P010: 10-bit NV12 layout, 16-bit samples

struct VideoDevice {
    std::wstring name;
    std::wstring symlink;
    std::wstring containerId;  // hardware container; used to find the card's own audio endpoint
    int score = 0;             // > 0 means "looks like a capture card"
};

struct VideoMode {
    int width = 0, height = 0;
    double fps = 0;
    GUID subtype{};
    DWORD nativeIndex = 0;
    bool hdr = false;  // the card says this mode carries HDR (PQ/HLG)
    std::string label;
};

// One *unique* game frame. Repeated frames (e.g. a 30 fps game inside a 60 Hz HDMI
// signal) are detected and dropped in the capture callback before they are copied.
struct Frame {
    std::vector<uint8_t> data;  // tightly packed pixels
    int width = 0, height = 0;
    PixFmt fmt = PixFmt::NV12;
    bool bt709 = true;
    bool fullRange = false;
    uint8_t transfer = 0;  // 0 = SDR, 1 = HDR10 (PQ), 2 = HLG: HDR is tone-mapped to SDR on the GPU
    bool bt2020 = false;   // BT.2020 matrix + primaries (HDR capture)
    double ts = 0;       // smoothed arrival time (ms, QPC clock)
    bool sceneCut = false;
};
using FramePtr = std::shared_ptr<Frame>;

struct CaptureStats {
    double signalFps = 0;  // frames/s arriving from the card (HDMI signal rate)
    double uniqueFps = 0;  // real game frames/s after duplicate removal
    double lastUniqueMs = 0;
    double lastRawMs = 0;
    bool noSignal = false;  // card is sending its "no HDMI input" filler
};

// Diagnostics: when > 0, the capture callback logs per-frame difference stats for this many frames.
extern std::atomic<int> gCaptureTraceFrames;

class CaptureSession;

// Auto mode picks the largest capture size up to this height (the screen's), min 1080.
void SetCaptureAutoMaxHeight(int h);

class Capture {
public:
    Capture();
    ~Capture();
    static std::vector<VideoDevice> Enumerate();

    bool Open(const VideoDevice& dev, int modeIndex /* -1 = auto */);
    void Close();
    bool IsOpen() const;
    bool Failed() const;  // device lost / unplugged
    const VideoDevice& Device() const { return dev_; }
    std::vector<VideoMode> Modes() const;
    int ActiveMode() const;
    std::string FormatLabel() const;

    HANDLE FrameEvent() const { return event_; }  // signalled when a unique frame is queued
    std::vector<FramePtr> Drain();
    CaptureStats Stats() const;

    // UI snapshots (--uishot): pretend a card is open with these modes/stats; no hardware touched.
    void SetDemo(const VideoDevice& dev, std::vector<VideoMode> modes, int active, std::string format, CaptureStats stats) {
        demo_ = true;
        dev_ = dev;
        demoModes_ = std::move(modes);
        demoActive_ = active;
        demoFormat_ = std::move(format);
        demoStats_ = stats;
    }

private:
    bool demo_ = false;
    std::vector<VideoMode> demoModes_;
    int demoActive_ = -1;
    std::string demoFormat_;
    CaptureStats demoStats_;
    HANDLE event_ = nullptr;
    VideoDevice dev_;
    ComPtr<CaptureSession> session_;
};
