#include "clips.h"

#include "log.h"

#include <wincodec.h>

#include <algorithm>
#include <cmath>

namespace {
// Same conversion as the GPU (kConvertCS), so clips look exactly like what FrameLift sees.
void ToBGR(const Frame& f, std::vector<uint8_t>& out) {
    const int W = f.width, H = f.height;
    out.resize((size_t)W * H * 3);
    auto put = [&](int x, int y, float Y, float U, float V) {
        float yy, uu, vv;
        if (!f.fullRange) { yy = (Y - 16) / 219.f; uu = (U - 128) / 224.f; vv = (V - 128) / 224.f; }
        else { yy = Y / 255.f; uu = (U - 128) / 255.f; vv = (V - 128) / 255.f; }
        float r, g, b;
        if (f.bt709) { r = yy + 1.5748f * vv; g = yy - 0.1873f * uu - 0.4681f * vv; b = yy + 1.8556f * uu; }
        else { r = yy + 1.402f * vv; g = yy - 0.344136f * uu - 0.714136f * vv; b = yy + 1.772f * uu; }
        auto c = [](float v) { return (uint8_t)std::clamp((int)std::lround(v * 255.f), 0, 255); };
        uint8_t* o = &out[((size_t)y * W + x) * 3];
        o[0] = c(b); o[1] = c(g); o[2] = c(r);
    };
    const uint8_t* d = f.data.data();
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            if (f.fmt == PixFmt::YUY2) {
                size_t o = ((size_t)y * W + (x & ~1)) * 2;
                put(x, y, d[o + (x & 1) * 2], d[o + 1], d[o + 3]);
            } else if (f.fmt == PixFmt::NV12) {
                size_t uv = (size_t)W * H + (size_t)(y / 2) * W + (x & ~1);
                put(x, y, d[(size_t)y * W + x], d[uv], d[uv + 1]);
            } else {
                const uint8_t* p = &d[((size_t)y * W + x) * 4];
                uint8_t* o = &out[((size_t)y * W + x) * 3];
                o[0] = p[0]; o[1] = p[1]; o[2] = p[2];
            }
        }
}

bool WritePng(IWICImagingFactory* wic, const std::wstring& path, const std::vector<uint8_t>& bgr, int W, int H) {
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> enc;
    ComPtr<IWICBitmapFrameEncode> frame;
    if (FAILED(wic->CreateStream(&stream)) || FAILED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE))) return false;
    if (FAILED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) || FAILED(enc->Initialize(stream.Get(), WICBitmapEncoderNoCache)))
        return false;
    if (FAILED(enc->CreateNewFrame(&frame, nullptr)) || FAILED(frame->Initialize(nullptr))) return false;
    WICPixelFormatGUID fmt = GUID_WICPixelFormat24bppBGR;
    if (FAILED(frame->SetSize((UINT)W, (UINT)H)) || FAILED(frame->SetPixelFormat(&fmt)) || fmt != GUID_WICPixelFormat24bppBGR) return false;
    if (FAILED(frame->WritePixels((UINT)H, (UINT)W * 3, (UINT)bgr.size(), (BYTE*)bgr.data()))) return false;
    return SUCCEEDED(frame->Commit()) && SUCCEEDED(enc->Commit());
}
}  // namespace

bool SavePngBgr(const std::wstring& path, const std::vector<uint8_t>& bgr, int W, int H) {
    ComPtr<IWICImagingFactory> wic;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)))) return false;
    return WritePng(wic.Get(), path, bgr, W, H);
}

void ClipCollector::Start(const std::wstring& dir, int maxClips, double minFreeGB) {
    Stop();
    dir_ = dir;
    maxClips_ = maxClips;
    minFreeGB_ = minFreeGB;
    stop_ = false;
    CreateDirectoryW(dir_.c_str(), nullptr);
    thread_ = std::thread([this] { Writer(); });
    Log("clips: collecting 60 fps test clips into %s (max %d)", Narrow(dir_).c_str(), maxClips_);
}

void ClipCollector::Stop() {
    if (!thread_.joinable()) return;
    {
        std::lock_guard<std::mutex> l(m_);
        stop_ = true;
    }
    cv_.notify_all();
    thread_.join();
}

void ClipCollector::Offer(const FramePtr& f, bool atSixty) {
    if (!thread_.joinable() || saved_ + queued_ >= maxClips_) return;
    if (f->fmt == PixFmt::P010 || f->transfer != 0) atSixty = false;  // test clips are SDR 8-bit only
    const double now = NowMs();
    if (!atSixty || now < cooldownUntil_) {
        run_.clear();
        return;
    }
    // Consecutive 60 fps frames only: a gap (repeat, hitch) means a midpoint we can't trust.
    if (!run_.empty() && (f->ts - run_.back()->ts > 1000.0 / 60.0 * 1.3 || f->ts <= run_.back()->ts)) run_.clear();
    run_.push_back(f);  // shared_ptr: no copy; the frame was already allocated for display
    if (run_.size() < 61) return;
    {
        std::lock_guard<std::mutex> l(m_);
        queue_.push_back(std::move(run_));
        queued_++;
    }
    run_.clear();
    cv_.notify_one();
    cooldownUntil_ = now + 30000;  // spread clips across the session
}

void ClipCollector::Writer() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ComPtr<IWICImagingFactory> wic;
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic));
    std::vector<uint8_t> bgr;
    for (;;) {
        std::vector<FramePtr> clip;
        {
            std::unique_lock<std::mutex> l(m_);
            cv_.wait(l, [&] { return stop_ || !queue_.empty(); });
            if (stop_ && queue_.empty()) break;
            clip = std::move(queue_.front());
            queue_.pop_front();
        }
        busy_ = true;
        ULARGE_INTEGER freeBytes{};
        GetDiskFreeSpaceExW(dir_.c_str(), &freeBytes, nullptr, nullptr);
        if (freeBytes.QuadPart / 1e9 < minFreeGB_) {
            Log("clips: skipped (less than %.0f GB free)", minFreeGB_);
        } else if (wic) {
            SYSTEMTIME t;
            GetLocalTime(&t);
            wchar_t name[64];
            swprintf(name, 64, L"\\clip-%04d%02d%02d-%02d%02d%02d", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
            std::wstring cdir = dir_ + name;
            CreateDirectoryW(cdir.c_str(), nullptr);
            bool ok = true;
            for (size_t i = 0; i < clip.size() && ok; i++) {
                ToBGR(*clip[i], bgr);
                wchar_t fn[32];
                swprintf(fn, 32, L"\\%04zu.png", i);
                ok = WritePng(wic.Get(), cdir + fn, bgr, clip[i]->width, clip[i]->height);
            }
            if (ok) saved_++;
            Log("clips: %s %s", ok ? "saved" : "FAILED", Narrow(cdir).c_str());
        }
        queued_--;
        busy_ = false;
    }
    wic.Reset();
    CoUninitialize();
}
