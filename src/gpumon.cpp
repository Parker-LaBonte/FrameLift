#include "gpumon.h"

#include <pdh.h>
#include <pdhmsg.h>

#include <algorithm>
#include <cwchar>
#include <map>

void GpuMonitor::Start(LUID adapter) {
    Stop();
    luid_ = adapter;
    stop_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    thread_ = std::thread([this] { Run(); });
}

void GpuMonitor::Stop() {
    if (thread_.joinable()) {
        SetEvent(stop_);
        thread_.join();
    }
    if (stop_) CloseHandle(stop_);
    stop_ = nullptr;
}

std::vector<float> GpuMonitor::History() const {
    std::lock_guard<std::mutex> l(m_);
    return hist_;
}

void GpuMonitor::Run() {
    PDH_HQUERY q = nullptr;
    if (PdhOpenQueryW(nullptr, 0, &q) != ERROR_SUCCESS) return;
    PDH_HCOUNTER c = nullptr;
    if (PdhAddEnglishCounterW(q, L"\\GPU Engine(*)\\Utilization Percentage", 0, &c) != ERROR_SUCCESS) {
        PdhCloseQuery(q);
        return;
    }
    PdhCollectQueryData(q);

    wchar_t luidStr[64];
    swprintf(luidStr, 64, L"luid_0x%08x_0x%08x", (unsigned)luid_.HighPart, (unsigned)luid_.LowPart);
    wchar_t pidStr[32];
    swprintf(pidStr, 32, L"pid_%lu_", (unsigned long)GetCurrentProcessId());
    std::vector<BYTE> buf;

    while (WaitForSingleObject(stop_, 500) == WAIT_TIMEOUT) {
        if (PdhCollectQueryData(q) != ERROR_SUCCESS) continue;
        DWORD size = 0, count = 0;
        if (PdhGetFormattedCounterArrayW(c, PDH_FMT_DOUBLE, &size, &count, nullptr) != (PDH_STATUS)PDH_MORE_DATA) continue;
        buf.resize(size);
        auto items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buf.data());
        if (PdhGetFormattedCounterArrayW(c, PDH_FMT_DOUBLE, &size, &count, items) != ERROR_SUCCESS) continue;

        std::map<std::wstring, double> engines, mine;  // key: "phys_N_eng_M"
        for (DWORD i = 0; i < count; i++) {
            std::wstring name = Lower(items[i].szName ? items[i].szName : L"");
            if (name.find(luidStr) == std::wstring::npos) continue;
            if (items[i].FmtValue.CStatus != PDH_CSTATUS_VALID_DATA && items[i].FmtValue.CStatus != PDH_CSTATUS_NEW_DATA)
                continue;
            size_t p = name.find(L"_phys_");
            size_t t = name.find(L"_engtype_");
            if (p == std::wstring::npos) continue;
            if (t == std::wstring::npos) continue;
            // Only engines frame generation competes for. Video encode (RustDesk, OBS, Moonlight),
            // decode and copy engines are separate hardware.
            std::wstring type = name.substr(t + 9);
            if (type.rfind(L"3d", 0) != 0 && type.rfind(L"compute", 0) != 0 && type.rfind(L"graphics", 0) != 0 &&
                type.rfind(L"cuda", 0) != 0)
                continue;
            std::wstring key = name.substr(p, t - p);
            double v = items[i].FmtValue.doubleValue;
            engines[key] += v;
            if (name.rfind(pidStr, 0) == 0) mine[key] += v;
        }
        double total = 0, self = 0;
        for (auto& e : engines) total = std::max(total, e.second);
        for (auto& e : mine) self = std::max(self, e.second);
        total_ = (float)std::min(100.0, total);
        self_ = (float)std::min(100.0, self);
        available_ = true;
        std::lock_guard<std::mutex> l(m_);
        hist_.push_back(total_);
        if (hist_.size() > 120) hist_.erase(hist_.begin());  // 60 s at 2 Hz
    }
    PdhCloseQuery(q);
}
