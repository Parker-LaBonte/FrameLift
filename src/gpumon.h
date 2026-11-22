#pragma once
#include "common.h"
#include <thread>

// GPU utilization from the same "GPU Engine" performance counters Task Manager uses,
// so it works on NVIDIA, AMD and Intel. Busiest engine on our adapter = overall %.
class GpuMonitor {
public:
    ~GpuMonitor() { Stop(); }
    void Start(LUID adapter);
    void Stop();
    float Total() const { return total_; }
    float Self() const { return self_; }
    bool Available() const { return available_; }
    std::vector<float> History() const;
    void SetDemo(float total, float self, std::vector<float> hist) {  // UI snapshots only
        total_ = total;
        self_ = self;
        available_ = true;
        std::lock_guard<std::mutex> l(m_);
        hist_ = std::move(hist);
    }

private:
    void Run();
    std::thread thread_;
    HANDLE stop_ = nullptr;
    LUID luid_{};
    mutable std::mutex m_;
    std::vector<float> hist_;
    std::atomic<float> total_{0}, self_{0};
    std::atomic<bool> available_{false};
};
