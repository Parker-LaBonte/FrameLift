#pragma once
#include "capture.h"
#include "common.h"

#include <condition_variable>
#include <deque>
#include <thread>

// Collects 61-frame runs of consecutive real frames while a game genuinely runs at 60 fps, and
// writes them as PNGs (%LOCALAPPDATA%\FrameLift\captures\clip-...\0000.png ...) on a low-priority thread.
// Those runs have true in-between frames, so they are ideal benchmark material (--bench).
bool SavePngBgr(const std::wstring& path, const std::vector<uint8_t>& bgr, int W, int H);

class ClipCollector {
public:
    ~ClipCollector() { Stop(); }
    void Start(const std::wstring& dir, int maxClips, double minFreeGB);
    void Stop();
    // Call with every unique frame. atSixty: the game is running at the output rate right now.
    void Offer(const FramePtr& f, bool atSixty);
    int Saved() const { return saved_; }
    bool Writing() const { return busy_; }

private:
    void Writer();
    std::wstring dir_;
    int maxClips_ = 0;
    double minFreeGB_ = 10;
    std::vector<FramePtr> run_;
    double cooldownUntil_ = 0;
    std::thread thread_;
    std::mutex m_;
    std::condition_variable cv_;
    std::deque<std::vector<FramePtr>> queue_;
    bool stop_ = false;
    std::atomic<int> saved_{0}, queued_{0};
    std::atomic<bool> busy_{false};
};
