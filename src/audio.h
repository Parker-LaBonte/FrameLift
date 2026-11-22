#pragma once
#include "common.h"
#include <thread>

struct AudioDevice {
    std::wstring id, name, containerId;
};

// Low-latency WASAPI passthrough: capture card's audio endpoint -> default speakers.
// Runs one MMCSS "Pro Audio" thread that services both the capture and render clients.
class AudioPassthrough {
public:
    ~AudioPassthrough() { Stop(); }
    static std::vector<AudioDevice> ListCaptureDevices();
    // Finds the audio endpoint that belongs to the same physical device as the video card.
    static std::wstring FindForVideo(const std::wstring& containerId, const std::wstring& videoName);

    void Start(const std::wstring& deviceId, const std::wstring& deviceName);

    // Windows "Listen to this device" on the card's input plays it a second time (doubled audio).
    // Returns true if it was on; *stillOn reports whether switching it off failed.
    static bool DisableListen(const std::wstring& deviceId, bool* stillOn);
    static void SetListen(const std::wstring& deviceId, bool on);
    void Stop();
    const std::wstring& DeviceId() const { return deviceId_; }
    const std::wstring& DeviceName() const { return deviceName_; }
    void SetDemo(const std::wstring& id, const std::wstring& name) { deviceId_ = id; deviceName_ = name; }  // UI snapshots

    std::atomic<float> volume{1.0f};
    std::atomic<bool> muted{false};
    std::atomic<float> extraDelayMs{0.0f};  // A/V sync: matches the frame-gen video hold
    std::atomic<float> bufferedMs{0.0f};
    std::atomic<bool> streaming{false};
    std::atomic<int> underruns{0}, jumps{0};  // diagnostics (cumulative)

private:
    void Run();
    bool RunOnce();
    std::thread thread_;
    HANDLE stopEvent_ = nullptr;
    std::wstring deviceId_, deviceName_;
};
