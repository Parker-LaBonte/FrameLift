#include <atomic>
#include "capture.h"
#include "log.h"

#include <cfgmgr32.h>
#include <devpropdef.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>

#include <algorithm>
#include <cmath>
#include <deque>

std::atomic<int> gCaptureTraceFrames{0};

namespace {

const DEVPROPKEY kDevInstanceId = {{0x78c34fc8, 0x104a, 0x4aca, {0x9e, 0xa4, 0x52, 0x4d, 0x52, 0x99, 0x6e, 0x57}}, 256};
const DEVPROPKEY kDevContainerId = {{0x8c7ed206, 0x3f8a, 0x4827, {0xb3, 0xab, 0xae, 0x9e, 0x1f, 0xae, 0xfc, 0x6c}}, 2};

std::wstring ContainerIdForInterface(const std::wstring& symlink) {
    DEVPROPTYPE type = 0;
    wchar_t inst[512] = {};
    ULONG size = sizeof(inst);
    if (CM_Get_Device_Interface_PropertyW(symlink.c_str(), &kDevInstanceId, &type, (PBYTE)inst, &size, 0) != CR_SUCCESS)
        return L"";
    DEVINST dn = 0;
    if (CM_Locate_DevNodeW(&dn, inst, CM_LOCATE_DEVNODE_NORMAL) != CR_SUCCESS) return L"";
    GUID g{};
    size = sizeof(g);
    if (CM_Get_DevNode_PropertyW(dn, &kDevContainerId, &type, (PBYTE)&g, &size, 0) != CR_SUCCESS) return L"";
    wchar_t s[64];
    StringFromGUID2(g, s, 64);
    return s;
}

// Heuristic: how likely is this video device an HDMI capture card (vs. a webcam)?
std::atomic<int> gAutoMaxHeight{1080};

int ScoreName(const std::wstring& raw) {
    std::wstring n = Lower(raw);
    auto has = [&](const wchar_t* k) { return n.find(k) != std::wstring::npos; };
    for (const wchar_t* bad : {L"virtual", L"obs ", L"obs-", L"integrated", L"webcam", L"facetime", L"ir camera",
                               L"front", L"rear", L"snap camera", L"nvidia broadcast"})
        if (has(bad)) return -100;
    int s = 0;
    for (const wchar_t* k : {L"elgato", L"game capture", L"hd60", L"4k60", L"4k x", L"4k s", L"4k pro", L"cam link",
                             L"neo", L"avermedia", L"live gamer", L"ripsaw", L"magewell", L"ezcap", L"genki",
                             L"capture", L"hdmi"})
        if (has(k)) s += 40;
    if (has(L"usb video") || has(L"uvc")) s += 20;
    return s;
}

int FormatPref(const GUID& g) {
    if (g == MFVideoFormat_NV12) return 8;
    if (g == MFVideoFormat_YUY2) return 6;
    if (g == MFVideoFormat_RGB32 || g == MFVideoFormat_ARGB32) return 4;
    if (g == MFVideoFormat_P010) return 3;
    if (g == MFVideoFormat_MJPG) return 2;
    return 0;
}

const char* FormatName(const GUID& g) {
    if (g == MFVideoFormat_NV12) return "NV12";
    if (g == MFVideoFormat_YUY2) return "YUY2";
    if (g == MFVideoFormat_RGB32) return "RGB32";
    if (g == MFVideoFormat_ARGB32) return "ARGB32";
    if (g == MFVideoFormat_MJPG) return "MJPEG";
    if (g == MFVideoFormat_P010) return "P010";
    return "?";
}

// A 10-bit mode the card marks as HDR wins: its 8-bit modes of an HDR signal are either the
// card's own tone mapping or clipped. Otherwise 8-bit formats are preferred (half the bytes).
int ModePref(const VideoMode& m) { return (m.subtype == MFVideoFormat_P010 && m.hdr) ? 10 : FormatPref(m.subtype); }
}  // namespace

void SetCaptureAutoMaxHeight(int h) { gAutoMaxHeight = h; }

// ---------------------------------------------------------------------------------------------
// CaptureSession: owns the async IMFSourceReader. COM ref-counted so late callbacks stay safe.
// ---------------------------------------------------------------------------------------------
class CaptureSession : public IMFSourceReaderCallback {
public:
    explicit CaptureSession(HANDLE ev) : event_(ev) { flushed_ = CreateEventW(nullptr, TRUE, FALSE, nullptr); }
    virtual ~CaptureSession() { CloseHandle(flushed_); }

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID iid, void** ppv) override {
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IMFSourceReaderCallback)) {
            *ppv = static_cast<IMFSourceReaderCallback*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return (ULONG)++ref_; }
    STDMETHODIMP_(ULONG) Release() override {
        ULONG r = (ULONG)--ref_;
        if (r == 0) delete this;
        return r;
    }

    // IMFSourceReaderCallback
    STDMETHODIMP OnReadSample(HRESULT hr, DWORD, DWORD flags, LONGLONG, IMFSample* sample) override {
        if (closing_) return S_OK;
        if (FAILED(hr) || (flags & (MF_SOURCE_READERF_ERROR | MF_SOURCE_READERF_ENDOFSTREAM))) {
            Log("capture: stream error hr=0x%08lx flags=0x%lx", (unsigned long)hr, (unsigned long)flags);
            failed_ = true;
            SetEvent(event_);
            return S_OK;
        }
        if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) {
            ReadCurrentFormat();
            Log("capture: format changed -> %s", FormatLabel().c_str());
        }
        if (sample) Process(sample);
        std::lock_guard<std::mutex> l(readerMutex_);
        if (!closing_ && reader_) {
            if (FAILED(reader_->ReadSample((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, nullptr, nullptr, nullptr, nullptr))) {
                failed_ = true;
                SetEvent(event_);
            }
        }
        return S_OK;
    }
    STDMETHODIMP OnFlush(DWORD) override {
        SetEvent(flushed_);
        return S_OK;
    }
    STDMETHODIMP OnEvent(DWORD, IMFMediaEvent*) override { return S_OK; }

    bool Start(const VideoDevice& dev, int modeIndex) {
        ComPtr<IMFAttributes> a;
        if (FAILED(MFCreateAttributes(&a, 2))) return false;
        a->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
        a->SetString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, dev.symlink.c_str());
        HRESULT hr = MFCreateDeviceSource(a.Get(), &source_);
        if (FAILED(hr)) {
            Log("capture: MFCreateDeviceSource failed hr=0x%08lx (device busy or unplugged?)", (unsigned long)hr);
            return false;
        }

        ComPtr<IMFAttributes> ra;
        MFCreateAttributes(&ra, 4);
        ra->SetUnknown(MF_SOURCE_READER_ASYNC_CALLBACK, static_cast<IMFSourceReaderCallback*>(this));
        ra->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
        ra->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE);  // lets MJPEG cards decode to NV12
        ra->SetUINT32(MF_LOW_LATENCY, TRUE);
        hr = MFCreateSourceReaderFromMediaSource(source_.Get(), ra.Get(), &reader_);
        if (FAILED(hr)) {
            Log("capture: MFCreateSourceReaderFromMediaSource failed hr=0x%08lx", (unsigned long)hr);
            return Fail();
        }

        EnumerateModes();
        for (auto& m : modes_) Log("capture:   mode %s", m.label.c_str());
        if (modes_.empty()) {
            Log("capture: no usable video modes");
            return Fail();
        }
        activeMode_ = (modeIndex >= 0 && modeIndex < (int)modes_.size()) ? modeIndex : PickAutoMode();
        if (!ApplyMode(modes_[activeMode_])) {
            Log("capture: could not apply mode %s", modes_[activeMode_].label.c_str());
            return Fail();
        }
        ReadCurrentFormat();
        Log("capture: opened, mode %s -> output %s, %s %s range", modes_[activeMode_].label.c_str(), FormatLabel().c_str(),
            bt709_ ? "BT.709" : "BT.601", fullRange_ ? "full" : "limited");

        rawPeriod_ = modes_[activeMode_].fps > 1 ? 1000.0 / modes_[activeMode_].fps : 16.67;
        hr = reader_->ReadSample((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, nullptr, nullptr, nullptr, nullptr);
        if (FAILED(hr)) {
            Log("capture: first ReadSample failed hr=0x%08lx", (unsigned long)hr);
            return Fail();
        }
        return true;
    }

    void Stop() {
        {
            std::lock_guard<std::mutex> l(readerMutex_);
            closing_ = true;
            if (reader_) {
                ResetEvent(flushed_);
                if (SUCCEEDED(reader_->Flush((DWORD)MF_SOURCE_READER_ALL_STREAMS)))
                    ; // wait below, outside the lock, so a running callback can finish
            }
        }
        WaitForSingleObject(flushed_, 1000);
        std::lock_guard<std::mutex> l(readerMutex_);
        reader_.Reset();
        if (source_) source_->Shutdown();
        source_.Reset();
    }

    std::vector<FramePtr> Drain() {
        std::lock_guard<std::mutex> l(queueMutex_);
        std::vector<FramePtr> out(queue_.begin(), queue_.end());
        queue_.clear();
        return out;
    }

    CaptureStats Stats() {
        std::lock_guard<std::mutex> l(queueMutex_);
        CaptureStats s = stats_;
        double now = NowMs();
        // Expire the rate readouts when frames stop (no HDMI signal).
        if (now - s.lastRawMs > 1500) s.signalFps = 0;
        if (now - s.lastUniqueMs > 1500) s.uniqueFps = 0;
        return s;
    }

    bool Failed() const { return failed_; }
    const std::vector<VideoMode>& Modes() const { return modes_; }
    int ActiveMode() const { return activeMode_; }
    std::string FormatLabel() const {
        char b[128];
        snprintf(b, sizeof(b), "%dx%d %s", width_, height_, FormatName(outSubtype_));
        return b;
    }

private:
    bool Fail() {
        reader_.Reset();
        if (source_) source_->Shutdown();
        source_.Reset();
        return false;
    }

    void EnumerateModes() {
        modes_.clear();
        for (DWORD i = 0;; i++) {
            ComPtr<IMFMediaType> t;
            if (FAILED(reader_->GetNativeMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, i, &t))) break;
            GUID sub{};
            t->GetGUID(MF_MT_SUBTYPE, &sub);
            if (!FormatPref(sub)) continue;
            UINT32 w = 0, h = 0, num = 0, den = 0;
            MFGetAttributeSize(t.Get(), MF_MT_FRAME_SIZE, &w, &h);
            MFGetAttributeRatio(t.Get(), MF_MT_FRAME_RATE, &num, &den);
            if (!w || !h || !den) continue;
            VideoMode m;
            m.width = (int)w;
            m.height = (int)h;
            m.fps = (double)num / den;
            m.subtype = sub;
            m.nativeIndex = i;
            UINT32 tf = 0;
            m.hdr = SUCCEEDED(t->GetUINT32(MF_MT_TRANSFER_FUNCTION, &tf)) && (tf == MFVideoTransFunc_2084 || tf == MFVideoTransFunc_HLG);
            // Keep one entry per (size, rate): the best pixel format.
            auto it = std::find_if(modes_.begin(), modes_.end(), [&](const VideoMode& o) {
                return o.width == m.width && o.height == m.height && std::fabs(o.fps - m.fps) < 0.05;
            });
            if (it != modes_.end()) {
                if (ModePref(m) > ModePref(*it)) *it = m;
                continue;
            }
            modes_.push_back(m);
        }
        std::sort(modes_.begin(), modes_.end(), [](const VideoMode& a, const VideoMode& b) {
            if (a.height != b.height) return a.height > b.height;
            return a.fps > b.fps;
        });
        for (auto& m : modes_) {
            char b[96];
            snprintf(b, sizeof(b), "%dp  \xC2\xB7  %.4g Hz  \xC2\xB7  %s%s", m.height, std::round(m.fps * 100) / 100, FormatName(m.subtype),
                     m.hdr ? " HDR" : "");
            m.label = b;
        }
    }

    // Auto: highest refresh first (so a 60 Hz signal stays 60), then the largest size that the
    // screen can show (a 4K card on a 4K TV runs at 4K; on a 1080p screen extra pixels are wasted
    // work), then the cheapest pixel format.
    int PickAutoMode() const {
        int best = 0;
        const int cap = std::max(1080, gAutoMaxHeight.load());
        auto key = [cap](const VideoMode& m) {
            int fpsClass = m.fps >= 55 ? 2 : (m.fps >= 29 ? 1 : 0);
            int fits = m.height <= cap ? 1 : 0;
            int size = fits ? m.height : -m.height;
            return std::make_tuple(fpsClass, fits, size, ModePref(m));
        };
        for (int i = 1; i < (int)modes_.size(); i++)
            if (key(modes_[i]) > key(modes_[best])) best = i;
        return best;
    }

    bool ApplyMode(const VideoMode& m) {
        ComPtr<IMFMediaType> native;
        if (FAILED(reader_->GetNativeMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, m.nativeIndex, &native)))
            return false;
        // Select the device format on the source itself...
        ComPtr<IMFPresentationDescriptor> pd;
        if (SUCCEEDED(source_->CreatePresentationDescriptor(&pd))) {
            BOOL selected = FALSE;
            ComPtr<IMFStreamDescriptor> sd;
            if (SUCCEEDED(pd->GetStreamDescriptorByIndex(0, &selected, &sd))) {
                ComPtr<IMFMediaTypeHandler> h;
                if (SUCCEEDED(sd->GetMediaTypeHandler(&h))) h->SetCurrentMediaType(native.Get());
            }
        }
        // ...then ask the reader for something the GPU converter understands.
        GUID want = m.subtype;
        if (want == MFVideoFormat_MJPG) want = MFVideoFormat_NV12;
        if (want == MFVideoFormat_ARGB32) want = MFVideoFormat_RGB32;
        ComPtr<IMFMediaType> out;
        MFCreateMediaType(&out);
        out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        out->SetGUID(MF_MT_SUBTYPE, want);
        MFSetAttributeSize(out.Get(), MF_MT_FRAME_SIZE, (UINT32)m.width, (UINT32)m.height);
        HRESULT hr = reader_->SetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, out.Get());
        if (FAILED(hr) && want != MFVideoFormat_NV12) {
            out->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
            hr = reader_->SetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, out.Get());
        }
        return SUCCEEDED(hr);
    }

    void ReadCurrentFormat() {
        ComPtr<IMFMediaType> t;
        if (FAILED(reader_->GetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, &t))) return;
        t->GetGUID(MF_MT_SUBTYPE, &outSubtype_);
        UINT32 w = 0, h = 0;
        MFGetAttributeSize(t.Get(), MF_MT_FRAME_SIZE, &w, &h);
        width_ = (int)w;
        height_ = (int)h;
        if (outSubtype_ == MFVideoFormat_NV12) fmt_ = PixFmt::NV12;
        else if (outSubtype_ == MFVideoFormat_YUY2) fmt_ = PixFmt::YUY2;
        else if (outSubtype_ == MFVideoFormat_P010) fmt_ = PixFmt::P010;
        else fmt_ = PixFmt::BGRA;
        UINT32 stride = 0;
        if (SUCCEEDED(t->GetUINT32(MF_MT_DEFAULT_STRIDE, &stride))) defaultStride_ = (LONG)(INT32)stride;
        else defaultStride_ = fmt_ == PixFmt::NV12 ? width_ : ((fmt_ == PixFmt::YUY2 || fmt_ == PixFmt::P010) ? width_ * 2 : width_ * 4);
        UINT32 matrix = 0, range = 0;
        bt709_ = height_ >= 720;
        if (SUCCEEDED(t->GetUINT32(MF_MT_YUV_MATRIX, &matrix))) {
            if (matrix == MFVideoTransferMatrix_BT709) bt709_ = true;
            else if (matrix == MFVideoTransferMatrix_BT601) bt709_ = false;
        }
        fullRange_ = false;
        if (SUCCEEDED(t->GetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, &range))) fullRange_ = (range == MFNominalRange_0_255);
        // HDR: a 10-bit capture with no colour tags is HDR10 (PQ, BT.2020) in practice.
        UINT32 tf = 0, prim = 0;
        const bool tenBit = fmt_ == PixFmt::P010;
        transfer_ = tenBit ? 1 : 0;
        if (SUCCEEDED(t->GetUINT32(MF_MT_TRANSFER_FUNCTION, &tf)))
            transfer_ = tf == MFVideoTransFunc_2084 ? 1 : (tf == MFVideoTransFunc_HLG ? 2 : (tenBit && tf == 0 ? 1 : 0));
        bt2020_ = transfer_ != 0;
        if (SUCCEEDED(t->GetUINT32(MF_MT_VIDEO_PRIMARIES, &prim)) && prim) bt2020_ = prim == MFVideoPrimaries_BT2020;
        if (SUCCEEDED(t->GetUINT32(MF_MT_YUV_MATRIX, &matrix)) && (matrix == MFVideoTransferMatrix_BT2020_10 || matrix == MFVideoTransferMatrix_BT2020_12))
            bt2020_ = true;
        if (transfer_ || bt2020_)
            Log("capture: HDR video (%s, %s); tone-mapping to SDR", transfer_ == 1 ? "PQ / HDR10" : (transfer_ == 2 ? "HLG" : "SDR"),
                bt2020_ ? "BT.2020" : "BT.709");
        haveGrid_ = false;
        noiseCount_ = 0;
        cleanCard_ = false;
        cleanRepeats_ = 0;
    }

    void Process(IMFSample* sample) {
        const double arrival = NowMs();
        ComPtr<IMFMediaBuffer> buf;
        if (FAILED(sample->ConvertToContiguousBuffer(&buf))) return;
        BYTE* p = nullptr;
        LONG pitch = 0;
        DWORD len = 0;
        ComPtr<IMF2DBuffer> b2;
        bool locked2d = false;
        if (SUCCEEDED(buf.As(&b2)) && SUCCEEDED(b2->Lock2D(&p, &pitch))) {
            locked2d = true;
        } else {
            if (FAILED(buf->Lock(&p, nullptr, &len))) return;
            pitch = defaultStride_;
            if (pitch < 0) p += (size_t)(height_ - 1) * (size_t)(-pitch);  // bottom-up RGB
        }
        auto unlock = [&] {
            if (locked2d) b2->Unlock2D();
            else buf->Unlock();
        };
        const int W = width_, H = height_;
        if (W <= 0 || H <= 0 || !p) {
            unlock();
            return;
        }

        // ---- Raw signal timing: smooth the arrival clock (USB delivery jitters by ~1 ms) ----
        double interval = lastRawArrival_ > 0 ? arrival - lastRawArrival_ : 0;
        if (interval > 2 && interval < 200 && interval < rawPeriod_ * 1.5) rawPeriod_ = rawPeriod_ * 0.98 + interval * 0.02;
        double ts = arrival;
        if (lastRawTs_ > 0) {
            double predicted = lastRawTs_ + rawPeriod_;
            double err = arrival - predicted;
            if (std::fabs(err) < rawPeriod_ * 0.5) ts = predicted + err * 0.1;
        }
        lastRawArrival_ = arrival;
        lastRawTs_ = ts;

        // ---- Duplicate detection on a sparse luma grid (cheap; runs before any copy) ----
        constexpr int GX = 128, GY = 72, N = GX * GY;
        if (grid_.size() != N) grid_.assign(N, 0);
        int bpp = fmt_ == PixFmt::NV12 ? 1 : ((fmt_ == PixFmt::YUY2 || fmt_ == PixFmt::P010) ? 2 : 4);
        int lumaOff = (fmt_ == PixFmt::BGRA || fmt_ == PixFmt::P010) ? 1 : 0;  // P010: high byte = top 8 bits
        int changed = 0, maxY = 0, over3 = 0, over6 = 0, maxD = 0;
        long long sum = 0;
        for (int gy = 0; gy < GY; gy++) {
            int y = (int)((gy + 0.5) * H / GY);
            const BYTE* row = p + (ptrdiff_t)y * pitch;
            for (int gx = 0; gx < GX; gx++) {
                int x = (int)((gx + 0.5) * W / GX);
                uint8_t v = row[x * bpp + lumaOff];
                if (v > maxY) maxY = v;
                uint8_t& g = grid_[gy * GX + gx];
                int d = std::abs((int)v - (int)g);
                sum += d;
                if (d > 12) changed++;
                if (d > 3) over3++;
                if (d > 6) over6++;
                if (d > maxD) maxD = d;
                g = v;
            }
        }
        double meanDiff = (double)sum / N;

        // ---- No-signal filler ----
        // With nothing on HDMI, cards send frames of luma 0 (below video black, 16). Some, like the
        // HD60 S, also flash the last frame they ever captured every so often. Treat the filler as
        // "no signal" and only trust video again after half a second without any filler.
        if (fmt_ != PixFmt::BGRA && !fullRange_ && maxY <= 2) {
            if (!noSignal_) Log("capture: no HDMI signal (card is sending filler frames)");
            noSignal_ = true;
            lastFillerMs_ = arrival;
            haveGrid_ = false;
            unlock();
            std::lock_guard<std::mutex> l(queueMutex_);
            Rate(rawTimes_, arrival, stats_.signalFps);
            stats_.lastRawMs = arrival;
            stats_.noSignal = true;
            return;
        }
        if (noSignal_ && arrival - lastFillerMs_ < 500) {
            unlock();
            std::lock_guard<std::mutex> l(queueMutex_);
            Rate(rawTimes_, arrival, stats_.signalFps);
            stats_.lastRawMs = arrival;
            return;
        }
        if (noSignal_) {
            Log("capture: HDMI signal present");
            noSignal_ = false;
            std::lock_guard<std::mutex> l(queueMutex_);
            stats_.noSignal = false;
        }
        // Repeated frames aren't always bit-identical: the HD60 S adds ~0.3 of noise. Learn this
        // card's noise level from frames with no sharp changes, and call a frame a repeat only if
        // it's within ~2x that noise. A fixed tolerance (1.5) swallowed subtle real changes, so
        // slow motion and slow pans looked like the game stalling.
        if (haveGrid_ && changed == 0 && meanDiff < 1.0) {
            noiseHist_[noiseIdx_++ % kNoiseN] = (float)meanDiff;
            noiseCount_ = std::min(noiseCount_ + 1, kNoiseN);
        }
        double dupThresh = 1.5;  // until we've learned the card
        if (noiseCount_ >= 30) {
            float tmp[kNoiseN];
            std::copy(noiseHist_, noiseHist_ + noiseCount_, tmp);
            std::nth_element(tmp, tmp + noiseCount_ / 2, tmp + noiseCount_);
            noiseFloor_ = tmp[noiseCount_ / 2];
            dupThresh = std::clamp(std::max(noiseFloor_ * 2.0, noiseFloor_ + 0.2), 0.2, 1.5);
        }
        // Clean cards: a repeat never moves any sample by more than ~2 levels, while every real new
        // frame moves at least one by 5+ (measured on the HD60 S: 900/900 split, zero overlap).
        // Once a card shows clean repeats, "any sample changed by more than 3" decides. Noisy
        // cards (MJPEG) never show clean repeats and keep the learned-average rule above.
        if (haveGrid_ && maxD <= 3 && ++cleanRepeats_ >= 20 && !cleanCard_) {
            cleanCard_ = true;
            Log("capture: clean repeats detected; a frame is new if any sample changes by more than 3");
        }
        bool duplicate = haveGrid_ && (cleanCard_ ? over3 == 0 : (changed <= 2 && meanDiff < dupThresh));
        if (gCaptureTraceFrames > 0 && haveGrid_) {
            gCaptureTraceFrames--;
            Log("trace: t=%.2f mean=%.3f over3=%d over6=%d over12=%d max=%d -> %s", arrival, meanDiff, over3, over6, changed,
                maxD, duplicate ? "repeat" : "new");
        }
        // 2-second summary of the decisions, for tuning.
        if (duplicate) {
            dupCount_++;
            dupMaxMean_ = std::max(dupMaxMean_, meanDiff);
        } else if (haveGrid_) {
            uniqCount_++;
            uniqMinMean_ = std::min(uniqMinMean_, meanDiff);
        }
        if (arrival - dedupeLogMs_ > 2000) {
            if (dedupeLogMs_ > 0)
                Log("dedupe: %d repeats (max diff %.3f) | %d new (min diff %.3f) | rule %s", dupCount_, dupMaxMean_, uniqCount_,
                    uniqCount_ ? uniqMinMean_ : 0.0, cleanCard_ ? "clean (any sample > 3)" : "noisy (learned average)");
            dedupeLogMs_ = arrival;
            dupCount_ = uniqCount_ = 0;
            dupMaxMean_ = 0;
            uniqMinMean_ = 1e9;
        }
        bool sceneCut = haveGrid_ && meanDiff > 40.0;
        haveGrid_ = true;

        {
            std::lock_guard<std::mutex> l(queueMutex_);
            Rate(rawTimes_, arrival, stats_.signalFps);
            stats_.lastRawMs = arrival;
        }
        if (duplicate) {
            unlock();
            return;  // the PS5 "30 fps in a 60 Hz signal" case lands here every other frame
        }

        // ---- Copy the unique frame tightly packed ----
        auto f = std::make_shared<Frame>();
        f->width = W;
        f->height = H;
        f->fmt = fmt_;
        f->bt709 = bt709_;
        f->fullRange = fullRange_;
        f->transfer = transfer_;
        f->bt2020 = bt2020_;
        f->ts = ts;
        f->sceneCut = sceneCut;
        int rowBytes = W * bpp;
        const bool planar = fmt_ == PixFmt::NV12 || fmt_ == PixFmt::P010;
        size_t total = planar ? (size_t)rowBytes * H + (size_t)rowBytes * ((H + 1) / 2) : (size_t)rowBytes * H;
        f->data.resize(total);
        uint8_t* dst = f->data.data();
        for (int y = 0; y < H; y++) memcpy(dst + (size_t)y * rowBytes, p + (ptrdiff_t)y * pitch, rowBytes);
        if (planar) {
            // The chroma plane follows the luma plane. Some drivers pad the luma plane's height,
            // which shows up as a larger buffer; derive the real plane height when we can.
            int planeH = H;
            if (!locked2d && len && pitch > 0) {
                int rows = (int)(len / (DWORD)pitch);
                if (rows * 2 / 3 > H) planeH = rows * 2 / 3;
            }
            const BYTE* uv = p + (ptrdiff_t)planeH * pitch;
            uint8_t* dstUV = dst + (size_t)rowBytes * H;
            for (int y = 0; y < (H + 1) / 2; y++) memcpy(dstUV + (size_t)y * rowBytes, uv + (ptrdiff_t)y * pitch, rowBytes);
        }
        unlock();

        {
            std::lock_guard<std::mutex> l(queueMutex_);
            queue_.push_back(std::move(f));
            while (queue_.size() > 6) queue_.pop_front();
            Rate(uniqueTimes_, arrival, stats_.uniqueFps);
            stats_.lastUniqueMs = arrival;
        }
        SetEvent(event_);
    }

    static void Rate(std::deque<double>& times, double now, double& out) {
        times.push_back(now);
        while (!times.empty() && now - times.front() > 1000.0) times.pop_front();
        if (times.size() >= 2) out = (times.size() - 1) * 1000.0 / (times.back() - times.front());
    }

    std::atomic<long> ref_{1};
    HANDLE event_;
    HANDLE flushed_;
    std::mutex readerMutex_;
    std::atomic<bool> closing_{false};
    std::atomic<bool> failed_{false};
    ComPtr<IMFMediaSource> source_;
    ComPtr<IMFSourceReader> reader_;
    std::vector<VideoMode> modes_;
    int activeMode_ = 0;

    GUID outSubtype_{};
    PixFmt fmt_ = PixFmt::NV12;
    uint8_t transfer_ = 0;
    bool bt2020_ = false;
    int width_ = 0, height_ = 0;
    LONG defaultStride_ = 0;
    bool bt709_ = true, fullRange_ = false;

    double rawPeriod_ = 16.67, lastRawArrival_ = 0, lastRawTs_ = 0;
    std::vector<uint8_t> grid_;
    bool haveGrid_ = false;
    bool noSignal_ = false;
    double lastFillerMs_ = 0;
    static constexpr int kNoiseN = 240;
    float noiseHist_[kNoiseN] = {};
    int noiseIdx_ = 0, noiseCount_ = 0;
    double noiseFloor_ = 0;
    bool cleanCard_ = false;
    int cleanRepeats_ = 0;
    int dupCount_ = 0, uniqCount_ = 0;
    double dupMaxMean_ = 0, uniqMinMean_ = 1e9, dedupeLogMs_ = 0;

    std::mutex queueMutex_;
    std::deque<FramePtr> queue_;
    std::deque<double> rawTimes_, uniqueTimes_;
    CaptureStats stats_;
};

// ---------------------------------------------------------------------------------------------

Capture::Capture() { event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr); }
Capture::~Capture() {
    Close();
    CloseHandle(event_);
}

std::vector<VideoDevice> Capture::Enumerate() {
    std::vector<VideoDevice> out;
    ComPtr<IMFAttributes> a;
    if (FAILED(MFCreateAttributes(&a, 1))) return out;
    a->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    IMFActivate** devs = nullptr;
    UINT32 n = 0;
    if (FAILED(MFEnumDeviceSources(a.Get(), &devs, &n))) return out;
    for (UINT32 i = 0; i < n; i++) {
        VideoDevice d;
        WCHAR* s = nullptr;
        UINT32 len = 0;
        if (SUCCEEDED(devs[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &s, &len))) {
            d.name = s;
            CoTaskMemFree(s);
        }
        if (SUCCEEDED(devs[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, &s, &len))) {
            d.symlink = s;
            CoTaskMemFree(s);
        }
        devs[i]->Release();
        if (d.symlink.empty()) continue;
        d.containerId = ContainerIdForInterface(d.symlink);
        d.score = ScoreName(d.name);
        out.push_back(std::move(d));
    }
    CoTaskMemFree(devs);
    std::stable_sort(out.begin(), out.end(), [](const VideoDevice& a, const VideoDevice& b) { return a.score > b.score; });
    return out;
}

bool Capture::Open(const VideoDevice& dev, int modeIndex) {
    Close();
    ComPtr<CaptureSession> s;
    s.Attach(new CaptureSession(event_));
    if (!s->Start(dev, modeIndex)) return false;
    session_ = s;
    dev_ = dev;
    return true;
}

void Capture::Close() {
    if (session_) {
        session_->Stop();
        session_.Reset();
    }
    dev_ = {};
}

bool Capture::IsOpen() const { return demo_ || session_ != nullptr; }
bool Capture::Failed() const { return session_ && session_->Failed(); }
std::vector<VideoMode> Capture::Modes() const { return demo_ ? demoModes_ : (session_ ? session_->Modes() : std::vector<VideoMode>{}); }
int Capture::ActiveMode() const { return demo_ ? demoActive_ : (session_ ? session_->ActiveMode() : -1); }
std::string Capture::FormatLabel() const { return demo_ ? demoFormat_ : (session_ ? session_->FormatLabel() : std::string()); }
std::vector<FramePtr> Capture::Drain() { return session_ ? session_->Drain() : std::vector<FramePtr>{}; }
CaptureStats Capture::Stats() const {
    if (demo_) {
        CaptureStats s = demoStats_;
        s.lastRawMs = s.lastUniqueMs = NowMs();
        return s;
    }
    return session_ ? session_->Stats() : CaptureStats{};
}
