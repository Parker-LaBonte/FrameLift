#include "audio.h"
#include "log.h"

#include <audioclient.h>
#include <avrt.h>
#include <mmdeviceapi.h>
#include <mmreg.h>
#include <propsys.h>

#include <algorithm>
#include <cmath>

#ifndef AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM
#define AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM 0x80000000
#endif
#ifndef AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY
#define AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY 0x08000000
#endif

namespace {
const PROPERTYKEY kFriendlyName = {{0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};
const PROPERTYKEY kContainerId = {{0x8c7ed206, 0x3f8a, 0x4827, {0xb3, 0xab, 0xae, 0x9e, 0x1f, 0xae, 0xfc, 0x6c}}, 2};
const GUID kSubtypeFloat = {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
const CLSID kCLSID_MMDeviceEnumerator = {0xBCDE0395, 0xE52F, 0x467C, {0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E}};
const wchar_t* kNullContainer = L"{00000000-0000-0000-ffff-ffffffffffff}";

std::wstring PropString(IPropertyStore* ps, const PROPERTYKEY& k) {
    PROPVARIANT v;
    PropVariantInit(&v);
    std::wstring r;
    if (SUCCEEDED(ps->GetValue(k, &v))) {
        if (v.vt == VT_LPWSTR && v.pwszVal) r = v.pwszVal;
        else if (v.vt == VT_CLSID && v.puuid) {
            wchar_t b[64];
            StringFromGUID2(*v.puuid, b, 64);
            r = b;
        }
    }
    PropVariantClear(&v);
    return r;
}

ComPtr<IMMDeviceEnumerator> Enumerator() {
    ComPtr<IMMDeviceEnumerator> en;
    CoCreateInstance(kCLSID_MMDeviceEnumerator, nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)en.GetAddressOf());
    return en;
}

std::vector<std::wstring> Tokens(const std::wstring& s) {
    std::vector<std::wstring> out;
    std::wstring cur;
    for (wchar_t c : Lower(s)) {
        if (iswalnum(c)) cur += c;
        else {
            if (cur.size() >= 2) out.push_back(cur);
            cur.clear();
        }
    }
    if (cur.size() >= 2) out.push_back(cur);
    return out;
}
}  // namespace

namespace {
const PROPERTYKEY kListen = {{0x24dbb0fc, 0x9311, 0x4b3d, {0x9c, 0xf0, 0x18, 0xff, 0x15, 0x56, 0x39, 0xd4}}, 1};

bool ReadListen(IPropertyStore* ps) {
    PROPVARIANT v;
    PropVariantInit(&v);
    bool on = SUCCEEDED(ps->GetValue(kListen, &v)) && v.vt == VT_BOOL && v.boolVal != VARIANT_FALSE;
    PropVariantClear(&v);
    return on;
}
}  // namespace

bool AudioPassthrough::DisableListen(const std::wstring& deviceId, bool* stillOn) {
    *stillOn = false;
    auto en = Enumerator();
    ComPtr<IMMDevice> d;
    if (!en || FAILED(en->GetDevice(deviceId.c_str(), &d))) return false;
    ComPtr<IPropertyStore> ro;
    if (FAILED(d->OpenPropertyStore(STGM_READ, &ro)) || !ReadListen(ro.Get())) return false;
    ComPtr<IPropertyStore> rw;
    HRESULT hr = d->OpenPropertyStore(STGM_READWRITE, &rw);
    if (SUCCEEDED(hr)) {
        PROPVARIANT v;
        PropVariantInit(&v);
        v.vt = VT_BOOL;
        v.boolVal = VARIANT_FALSE;
        hr = rw->SetValue(kListen, v);
        if (SUCCEEDED(hr)) hr = rw->Commit();
    }
    ComPtr<IPropertyStore> check;
    *stillOn = FAILED(d->OpenPropertyStore(STGM_READ, &check)) || ReadListen(check.Get());
    Log("audio: Windows 'Listen to this device' was ON for the card (doubled audio); turning it off: %s (hr=0x%08lx)",
        *stillOn ? "FAILED" : "ok", (unsigned long)hr);
    return true;
}

void AudioPassthrough::SetListen(const std::wstring& deviceId, bool on) {
    auto en = Enumerator();
    ComPtr<IMMDevice> d;
    ComPtr<IPropertyStore> rw;
    if (!en || FAILED(en->GetDevice(deviceId.c_str(), &d)) || FAILED(d->OpenPropertyStore(STGM_READWRITE, &rw))) return;
    PROPVARIANT v;
    PropVariantInit(&v);
    v.vt = VT_BOOL;
    v.boolVal = on ? VARIANT_TRUE : VARIANT_FALSE;
    if (SUCCEEDED(rw->SetValue(kListen, v))) rw->Commit();
    Log("audio: restored Windows 'Listen to this device' = %s", on ? "on" : "off");
}

std::vector<AudioDevice> AudioPassthrough::ListCaptureDevices() {
    std::vector<AudioDevice> out;
    auto en = Enumerator();
    if (!en) return out;
    ComPtr<IMMDeviceCollection> col;
    if (FAILED(en->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &col))) return out;
    UINT n = 0;
    col->GetCount(&n);
    for (UINT i = 0; i < n; i++) {
        ComPtr<IMMDevice> d;
        if (FAILED(col->Item(i, &d))) continue;
        AudioDevice a;
        LPWSTR id = nullptr;
        if (SUCCEEDED(d->GetId(&id))) {
            a.id = id;
            CoTaskMemFree(id);
        }
        ComPtr<IPropertyStore> ps;
        if (SUCCEEDED(d->OpenPropertyStore(STGM_READ, &ps))) {
            a.name = PropString(ps.Get(), kFriendlyName);
            a.containerId = PropString(ps.Get(), kContainerId);
        }
        out.push_back(std::move(a));
    }
    return out;
}

std::wstring AudioPassthrough::FindForVideo(const std::wstring& containerId, const std::wstring& videoName) {
    auto devs = ListCaptureDevices();
    // 1) Same physical device (Elgato & most USB cards expose video + audio in one container).
    std::wstring cid = Lower(containerId);
    if (!cid.empty() && cid != kNullContainer)
        for (auto& d : devs)
            if (Lower(d.containerId) == cid) return d.id;
    // 2) Name similarity, e.g. "Game Capture HD60 X" vs "Digital Audio Interface (Game Capture HD60 X)".
    auto vt = Tokens(videoName);
    int bestScore = 0;
    std::wstring best;
    for (auto& d : devs) {
        std::wstring n = Lower(d.name);
        int s = 0;
        for (auto& t : vt)
            if (t != L"usb" && t != L"video" && t != L"device" && n.find(t) != std::wstring::npos) s += 2;
        for (const wchar_t* k : {L"elgato", L"capture", L"hdmi", L"avermedia", L"digital audio interface", L"live gamer"})
            if (n.find(k) != std::wstring::npos) s += 1;
        if (s > bestScore) {
            bestScore = s;
            best = d.id;
        }
    }
    return bestScore >= 2 ? best : L"";
}

void AudioPassthrough::Start(const std::wstring& deviceId, const std::wstring& deviceName) {
    Stop();
    if (deviceId.empty()) return;
    deviceId_ = deviceId;
    deviceName_ = deviceName;
    stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    thread_ = std::thread([this] { Run(); });
}

void AudioPassthrough::Stop() {
    if (thread_.joinable()) {
        SetEvent(stopEvent_);
        thread_.join();
    }
    if (stopEvent_) CloseHandle(stopEvent_);
    stopEvent_ = nullptr;
    deviceId_.clear();
    deviceName_.clear();
    streaming = false;
}

void AudioPassthrough::Run() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    DWORD task = 0;
    HANDLE mm = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task);
    while (WaitForSingleObject(stopEvent_, 0) != WAIT_OBJECT_0) {
        RunOnce();
        streaming = false;
        WaitForSingleObject(stopEvent_, 1000);  // device vanished or default output changed: retry
    }
    if (mm) AvRevertMmThreadCharacteristics(mm);
    CoUninitialize();
}

bool AudioPassthrough::RunOnce() {
    auto en = Enumerator();
    if (!en) return false;
    ComPtr<IMMDevice> capDev, renDev;
    if (FAILED(en->GetDevice(deviceId_.c_str(), &capDev))) {
        Log("audio: capture endpoint not found");
        return false;
    }
    if (FAILED(en->GetDefaultAudioEndpoint(eRender, eMultimedia, &renDev))) return false;
    std::wstring renId;
    {
        LPWSTR id = nullptr;
        if (SUCCEEDED(renDev->GetId(&id))) {
            renId = id;
            CoTaskMemFree(id);
        }
    }

    // Render client: IAudioClient3 lets us ask for the engine's minimum period (often ~3 ms).
    ComPtr<IAudioClient> ren;
    ComPtr<IAudioClient3> ren3;
    if (SUCCEEDED(renDev->Activate(__uuidof(IAudioClient3), CLSCTX_ALL, nullptr, (void**)ren3.GetAddressOf()))) ren = ren3;
    else if (FAILED(renDev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)ren.GetAddressOf()))) return false;

    WAVEFORMATEX* mix = nullptr;
    if (FAILED(ren->GetMixFormat(&mix))) return false;
    struct FreeMix {
        WAVEFORMATEX* p;
        ~FreeMix() { CoTaskMemFree(p); }
    } freeMix{mix};
    bool isFloat = mix->wBitsPerSample == 32 &&
                   (mix->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
                    (mix->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
                     IsEqualGUID(reinterpret_cast<WAVEFORMATEXTENSIBLE*>(mix)->SubFormat, kSubtypeFloat)));
    if (!isFloat) {
        Log("audio: speaker mix format is not float32 (tag %u, %u bits)", mix->wFormatTag, mix->wBitsPerSample);
        return false;
    }
    const UINT32 ch = mix->nChannels;
    const UINT32 rate = mix->nSamplesPerSec;

    UINT32 periodFrames = 0;
    bool initialized = false;
    if (ren3) {
        UINT32 def = 0, fund = 0, mn = 0, mx = 0;
        if (SUCCEEDED(ren3->GetSharedModeEnginePeriod(mix, &def, &fund, &mn, &mx)) &&
            SUCCEEDED(ren3->InitializeSharedAudioStream(AUDCLNT_STREAMFLAGS_EVENTCALLBACK, mn, mix, nullptr))) {
            periodFrames = mn;
            initialized = true;
        }
    }
    if (!initialized) {
        HRESULT ihr = ren->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, 100000, 0, mix, nullptr);
        if (FAILED(ihr)) {
            Log("audio: speaker Initialize failed hr=0x%08lx", (unsigned long)ihr);
            return false;
        }
        REFERENCE_TIME defP = 0, minP = 0;
        ren->GetDevicePeriod(&defP, &minP);
        periodFrames = (UINT32)(defP * rate / 10000000);
    }
    if (!periodFrames) periodFrames = rate / 100;

    // Capture client: ask Windows to convert the card's format to the speaker mix format.
    ComPtr<IAudioClient> cap;
    if (FAILED(capDev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)cap.GetAddressOf()))) return false;
    HRESULT chr = cap->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                  AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                                      AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                                  100000, 0, mix, nullptr);
    if (FAILED(chr)) {
        Log("audio: capture Initialize failed hr=0x%08lx", (unsigned long)chr);
        return false;
    }

    HANDLE capEvt = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    HANDLE renEvt = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    struct CloseEvts {
        HANDLE a, b;
        ~CloseEvts() {
            CloseHandle(a);
            CloseHandle(b);
        }
    } closeEvts{capEvt, renEvt};
    cap->SetEventHandle(capEvt);
    ren->SetEventHandle(renEvt);
    ComPtr<IAudioCaptureClient> capClient;
    ComPtr<IAudioRenderClient> renClient;
    if (FAILED(cap->GetService(__uuidof(IAudioCaptureClient), (void**)capClient.GetAddressOf()))) return false;
    if (FAILED(ren->GetService(__uuidof(IAudioRenderClient), (void**)renClient.GetAddressOf()))) return false;
    UINT32 renBufFrames = 0;
    ren->GetBufferSize(&renBufFrames);

    // Ring buffer (frames of `ch` floats). Only this thread touches it.
    const UINT32 ringCap = rate;  // 1 second
    std::vector<float> ring((size_t)ringCap * ch);
    UINT32 rHead = 0, rCount = 0;
    auto push = [&](const float* src, UINT32 frames) {
        for (UINT32 f = 0; f < frames; f++) {
            if (rCount == ringCap) {
                rHead = (rHead + 1) % ringCap;
                rCount--;
            }
            UINT32 w = (rHead + rCount) % ringCap;
            for (UINT32 c = 0; c < ch; c++) ring[(size_t)w * ch + c] = src ? src[(size_t)f * ch + c] : 0.0f;
            rCount++;
        }
    };
    bool priming = true;
    double readFrac = 0, smoothDelayMs = std::max(0.0f, extraDelayMs.load()), fillAvgMs = 0;

    if (FAILED(ren->Start()) || FAILED(cap->Start())) {
        Log("audio: Start failed");
        return false;
    }
    streaming = true;
    Log("audio: streaming %u Hz, %u ch, speaker period %u frames (%.1f ms)%s", rate, ch, periodFrames,
        periodFrames * 1000.0 / rate, initialized && ren3 ? " [IAudioClient3 low-latency]" : "");

    HANDLE hs[3] = {stopEvent_, capEvt, renEvt};
    double lastCheck = NowMs();
    bool ok = true;
    while (ok) {
        DWORD w = WaitForMultipleObjects(3, hs, FALSE, 200);
        if (w == WAIT_OBJECT_0) break;

        // --- pull everything the card has ---
        UINT32 pkt = 0;
        HRESULT hr;
        while (SUCCEEDED(hr = capClient->GetNextPacketSize(&pkt)) && pkt > 0) {
            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;
            if (FAILED(hr = capClient->GetBuffer(&data, &frames, &flags, nullptr, nullptr))) break;
            push((flags & AUDCLNT_BUFFERFLAGS_SILENT) ? nullptr : reinterpret_cast<float*>(data), frames);
            capClient->ReleaseBuffer(frames);
        }
        if (FAILED(hr)) {
            Log("audio: capture read failed hr=0x%08lx", (unsigned long)hr);
            ok = false;
            break;
        }

        // --- feed the speakers, keeping the device queue at ~2 engine periods ---
        UINT32 padding = 0;
        if (FAILED(ren->GetCurrentPadding(&padding))) {
            ok = false;
            break;
        }
        UINT32 deviceTarget = std::min(renBufFrames, periodFrames * 2);
        UINT32 want = deviceTarget > padding ? deviceTarget - padding : 0;
        if (want) {
            BYTE* out = nullptr;
            if (FAILED(renClient->GetBuffer(want, &out))) {
                ok = false;
                break;
            }
            float* o = reinterpret_cast<float*>(out);
            // Target buffer: a safety floor (card and speakers both move audio in ~10 ms chunks)
            // plus the A/V-sync delay, which glides instead of following every frame's hold.
            const double baseMs = 25.0;
            const double wantDelay = std::max(0.0f, extraDelayMs.load());
            smoothDelayMs += std::clamp(wantDelay - smoothDelayMs, -0.05, 0.05);  // <= 5 ms/s at 10 ms periods
            const double targetMs = baseMs + smoothDelayMs;
            const double fillMs = rCount * 1000.0 / rate;
            if (fillMs > targetMs + 150) {  // far too much buffered (stall or device hiccup): skip ahead once
                UINT32 drop = rCount - (UINT32)(targetMs * rate / 1000.0);
                rHead = (rHead + drop) % ringCap;
                rCount -= drop;
                jumps++;
            }
            if (priming && fillMs >= targetMs) priming = false;
            if (priming) {
                renClient->ReleaseBuffer(want, AUDCLNT_BUFFERFLAGS_SILENT);
            } else {
                // Clock drift + delay changes: play slightly fast/slow (max 0.5%, inaudible) with
                // linear interpolation. No dropped or repeated samples, so no clicks.
                fillAvgMs = fillAvgMs <= 0 ? fillMs : fillAvgMs * 0.98 + fillMs * 0.02;
                const double ratio = 1.0 + std::clamp((fillAvgMs - targetMs) * 0.0005, -0.005, 0.005);
                float gain = muted ? 0.0f : volume.load();
                for (UINT32 f = 0; f < want; f++) {
                    if (rCount < 2) {  // underrun: pad with silence and re-prime
                        std::fill(o + (size_t)f * ch, o + (size_t)want * ch, 0.0f);
                        priming = true;
                        underruns++;
                        break;
                    }
                    const size_t i0 = (size_t)rHead * ch, i1 = (size_t)((rHead + 1) % ringCap) * ch;
                    const float fr = (float)readFrac;
                    for (UINT32 c = 0; c < ch; c++) o[(size_t)f * ch + c] = (ring[i0 + c] + (ring[i1 + c] - ring[i0 + c]) * fr) * gain;
                    readFrac += ratio;
                    while (readFrac >= 1.0 && rCount > 1) {
                        readFrac -= 1.0;
                        rHead = (rHead + 1) % ringCap;
                        rCount--;
                    }
                }
                renClient->ReleaseBuffer(want, 0);
            }
        }
        bufferedMs = (float)(rCount * 1000.0 / rate);

        // --- follow the user's default output device ---
        double now = NowMs();
        if (now - lastCheck > 1000) {
            lastCheck = now;
            ComPtr<IMMDevice> cur;
            if (SUCCEEDED(en->GetDefaultAudioEndpoint(eRender, eMultimedia, &cur))) {
                LPWSTR id = nullptr;
                if (SUCCEEDED(cur->GetId(&id))) {
                    bool changed = renId != id;
                    CoTaskMemFree(id);
                    if (changed) {
                        Log("audio: default speaker changed, reconnecting");
                        break;
                    }
                }
            }
        }
    }
    cap->Stop();
    ren->Stop();
    return ok;
}
