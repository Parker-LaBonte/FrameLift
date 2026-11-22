#include "framegen.h"

#include "shaders.h"
#include "log.h"

#include <d3dcompiler.h>

#include <algorithm>
#include <cmath>

namespace {

struct ConvertCB { UINT W, H, Fmt, M709, Full, Transfer, Bt2020, pad; };
struct DownCB { UINT SW, SH, DW, DH; };
struct SearchCB {
    float LevelSize[2], InvLevelSize[2];
    UINT Grid[2], PredGrid[2];
    UINT TempGrid[2];
    float Scale, Step;
    INT Radius;
    UINT Mode, UseTemporal;
    float Lambda;
    INT Local;
    float LocalPen;
    UINT Asym;
    float TempSign;
};
struct MedianCB { UINT Grid[2], pad[2]; };
struct HighPassCB { UINT W, H; INT R; UINT pad; };
struct Luma8CB { UINT W, H, S, pad; };
struct StaticCB { UINT W, H; float Thr; UINT pad; };
struct NvDenseCB { UINT Size[2]; UINT FlowSize[2]; float InvL2[2]; UINT Hybrid; float Bias; UINT Bilin; float FlowScale; UINT pad[2]; };
struct NvGridCB { UINT Grid[2]; UINT FlowSize[2]; float FlowScale; UINT pad[3]; };
struct RefineCB { float InvLevelSize[2]; UINT Size[2]; UINT Grid[2]; float Bias, ZeroBias; };
struct PresentCB {
    float SrcSize[2], FieldUVScale[2];
    float T;
    UINT Mode, ShowMotion, Occl;
    float OccTol, OccSpan, pad[2];
};
static_assert(sizeof(SearchCB) == 80, "cbuffer layout");
static_assert(sizeof(PresentCB) == 48, "cbuffer layout");

ComPtr<ID3DBlob> Compile(const char* src, size_t len, const char* entry, const char* target) {
    ComPtr<ID3DBlob> blob, err;
    HRESULT hr = D3DCompile(src, len, entry, nullptr, nullptr, entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &err);
    if (FAILED(hr)) {
        std::string msg = "Shader compile failed (";
        msg += entry;
        msg += "):\n";
        if (err) msg.append((const char*)err->GetBufferPointer(), err->GetBufferSize());
        Log("%s", msg.c_str());
        MessageBoxA(nullptr, msg.c_str(), "FrameLift", MB_ICONERROR);
        return nullptr;
    }
    return blob;
}

UINT Div(UINT a, UINT b) { return (a + b - 1) / b; }

}  // namespace

bool Renderer::Init(HWND hwnd) {
    ComPtr<IDXGIFactory2> factory;
    if (FAILED(CreateDXGIFactory2(0, __uuidof(IDXGIFactory2), (void**)factory.GetAddressOf()))) return false;

    // Laptops: use the discrete GPU, not the integrated one.
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<IDXGIFactory6> f6;
    if (SUCCEEDED(factory.As(&f6)))
        f6->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, __uuidof(IDXGIAdapter1), (void**)adapter.GetAddressOf());
    if (!adapter) factory->EnumAdapters1(0, &adapter);
    if (!adapter) return false;
    DXGI_ADAPTER_DESC1 ad{};
    adapter->GetDesc1(&ad);
    luid_ = ad.AdapterLuid;
    vendor_ = ad.VendorId;
    adapterName_ = Narrow(ad.Description);

    D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    HRESULT hr = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 2,
                                   D3D11_SDK_VERSION, &dev_, nullptr, &ctx_);
    if (hr == E_INVALIDARG)
        hr = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels + 1, 1,
                               D3D11_SDK_VERSION, &dev_, nullptr, &ctx_);
    if (FAILED(hr)) {
        Log("render: D3D11CreateDevice failed hr=0x%08lx", (unsigned long)hr);
        return false;
    }

    ComPtr<IDXGIFactory5> f5;
    BOOL allowTearing = FALSE;
    if (SUCCEEDED(factory.As(&f5))) f5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allowTearing, sizeof(allowTearing));
    tearing_ = allowTearing != FALSE;

    // Flip model, 2 buffers, max frame latency 1 + waitable object: nothing queues up behind us.
    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    scFlags_ = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT | (tearing_ ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0);
    sd.Flags = scFlags_;
    ComPtr<IDXGISwapChain1> sc1;
    hr = factory->CreateSwapChainForHwnd(dev_.Get(), hwnd, &sd, nullptr, nullptr, &sc1);
    if (FAILED(hr)) {
        Log("render: CreateSwapChainForHwnd failed hr=0x%08lx", (unsigned long)hr);
        return false;
    }
    if (FAILED(sc1.As(&sc_))) return false;
    sc_->SetMaximumFrameLatency(1);
    waitable_ = sc_->GetFrameLatencyWaitableObject();
    factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
    CreateRTV();

    return CreatePipeline();
}

bool Renderer::InitHeadless() {
    ComPtr<IDXGIFactory2> factory;
    if (FAILED(CreateDXGIFactory2(0, __uuidof(IDXGIFactory2), (void**)factory.GetAddressOf()))) return false;
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<IDXGIFactory6> f6;
    if (SUCCEEDED(factory.As(&f6)))
        f6->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, __uuidof(IDXGIAdapter1), (void**)adapter.GetAddressOf());
    if (!adapter) factory->EnumAdapters1(0, &adapter);
    if (!adapter) return false;
    DXGI_ADAPTER_DESC1 ad{};
    adapter->GetDesc1(&ad);
    luid_ = ad.AdapterLuid;
    vendor_ = ad.VendorId;
    adapterName_ = Narrow(ad.Description);
    D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    HRESULT hr = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, levels, 2, D3D11_SDK_VERSION, &dev_, nullptr, &ctx_);
    if (hr == E_INVALIDARG)
        hr = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, levels + 1, 1, D3D11_SDK_VERSION, &dev_, nullptr, &ctx_);
    if (FAILED(hr)) return false;
    return CreatePipeline();
}

bool Renderer::RenderNewestMid(std::vector<uint8_t>& rgba, float T, UINT modeOverride) {
    FinishMotion();
    Slot* B = Newest();
    if (!B || !B->hasField) return false;
    Slot* A = FindSeq(B->seq - 1);
    if (!A) return false;
    D3D11_TEXTURE2D_DESC td{};
    if (evalTex_) evalTex_->GetDesc(&td);
    if (!evalTex_ || td.Width != (UINT)vw_ || td.Height != (UINT)vh_) {
        evalTex_.Reset();
        evalStage_.Reset();
        evalRtv_.Reset();
        td = {};
        td.Width = (UINT)vw_;
        td.Height = (UINT)vh_;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET;
        if (FAILED(dev_->CreateTexture2D(&td, nullptr, &evalTex_)) || FAILED(dev_->CreateRenderTargetView(evalTex_.Get(), nullptr, &evalRtv_)))
            return false;
        td.Usage = D3D11_USAGE_STAGING;
        td.BindFlags = 0;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(dev_->CreateTexture2D(&td, nullptr, &evalStage_))) return false;
    }
    D3D11_VIEWPORT vp{0, 0, (float)vw_, (float)vh_, 0, 1};
    DrawPair(A, B, T, modeOverride ? modeOverride : (tuning.blendOnly ? 4u : 1u), false, vp, evalRtv_.Get());
    ctx_->CopyResource(evalStage_.Get(), evalTex_.Get());
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(ctx_->Map(evalStage_.Get(), 0, D3D11_MAP_READ, 0, &m))) return false;
    rgba.resize((size_t)vw_ * vh_ * 4);
    for (int y = 0; y < vh_; y++) memcpy(&rgba[(size_t)y * vw_ * 4], (const uint8_t*)m.pData + (size_t)y * m.RowPitch, (size_t)vw_ * 4);
    ctx_->Unmap(evalStage_.Get(), 0);
    return true;
}

bool Renderer::CreatePipeline() {
    // Shaders
    auto cs = [&](const char* src, size_t len, ComPtr<ID3D11ComputeShader>& out) {
        auto b = Compile(src, len, "main", "cs_5_0");
        return b && SUCCEEDED(dev_->CreateComputeShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &out));
    };
    if (!cs(kConvertCS, sizeof(kConvertCS) - 1, csConvert_) || !cs(kLumaHalfCS, sizeof(kLumaHalfCS) - 1, csLumaHalf_) ||
        !cs(kDownCS, sizeof(kDownCS) - 1, csDown_) || !cs(kSearchCS, sizeof(kSearchCS) - 1, csSearch_) ||
        !cs(kMedianCS, sizeof(kMedianCS) - 1, csMedian_) || !cs(kRefineCS, sizeof(kRefineCS) - 1, csRefine_) ||
        !cs(kHighPassCS, sizeof(kHighPassCS) - 1, csHighPass_) || !cs(kLuma8CS, sizeof(kLuma8CS) - 1, csLuma8_) ||
        !cs(kNvToDenseCS, sizeof(kNvToDenseCS) - 1, csNvDense_) || !cs(kNvToGridCS, sizeof(kNvToGridCS) - 1, csNvGrid_) ||
        !cs(kStaticMaskCS, sizeof(kStaticMaskCS) - 1, csStatic_))
        return false;
    auto vsb = Compile(kPresentHLSL, sizeof(kPresentHLSL) - 1, "VSMain", "vs_5_0");
    auto psb = Compile(kPresentHLSL, sizeof(kPresentHLSL) - 1, "PSMain", "ps_5_0");
    if (!vsb || !psb) return false;
    dev_->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &vs_);
    dev_->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &ps_);
    auto pub = Compile(kPresentHLSL, sizeof(kPresentHLSL) - 1, "PSUpscale", "ps_5_0");
    if (!pub) return false;
    dev_->CreatePixelShader(pub->GetBufferPointer(), pub->GetBufferSize(), nullptr, &psUp_);
    auto plb = Compile(kScaleHLSL, sizeof(kScaleHLSL) - 1, "PSLanczos", "ps_5_0");
    auto prb = Compile(kScaleHLSL, sizeof(kScaleHLSL) - 1, "PSRcas", "ps_5_0");
    if (!plb || !prb) return false;
    dev_->CreatePixelShader(plb->GetBufferPointer(), plb->GetBufferSize(), nullptr, &psLanczos_);
    dev_->CreatePixelShader(prb->GetBufferPointer(), prb->GetBufferSize(), nullptr, &psRcas_);

    D3D11_SAMPLER_DESC smp{};
    smp.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    smp.AddressU = smp.AddressV = smp.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    smp.MaxLOD = D3D11_FLOAT32_MAX;
    dev_->CreateSamplerState(&smp, &sampler_);

    auto cb = [&](UINT size, ComPtr<ID3D11Buffer>& out) {
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = (size + 15) & ~15u;
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        dev_->CreateBuffer(&bd, nullptr, &out);
    };
    cb(sizeof(ConvertCB), cbConvert_);
    cb(sizeof(DownCB), cbDown_);
    cb(sizeof(SearchCB), cbSearch_);
    cb(sizeof(MedianCB), cbMedian_);
    cb(sizeof(RefineCB), cbRefine_);
    cb(sizeof(HighPassCB), cbHighPass_);
    cb(sizeof(Luma8CB), cbLuma8_);
    cb(sizeof(NvDenseCB), cbNvDense_);
    cb(sizeof(NvGridCB), cbNvGrid_);
    cb(sizeof(StaticCB), cbStatic_);
    cb(sizeof(PresentCB), cbPresent_);
    cb(32, cbScale_);
    Log("render: %s, tearing %s, all shaders compiled", adapterName_.c_str(), tearing_ ? "supported" : "unsupported");

    for (auto& q : queries_) {
        D3D11_QUERY_DESC qd{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
        dev_->CreateQuery(&qd, &q.disjoint);
        qd.Query = D3D11_QUERY_TIMESTAMP;
        dev_->CreateQuery(&qd, &q.t0);
        dev_->CreateQuery(&qd, &q.t1);
    }
    return true;
}

void Renderer::Shutdown() {
    nv_.Shutdown();  // unregister CUDA interop textures while the D3D device still exists
    if (ctx_) ctx_->ClearState();
    for (auto& s : slots_) s = Slot{};
    rtv_.Reset();
    if (waitable_) CloseHandle(waitable_);
    waitable_ = nullptr;
    sc_.Reset();
    ctx_.Reset();
    dev_.Reset();
}

void Renderer::CreateRTV() {
    ComPtr<ID3D11Texture2D> bb;
    if (SUCCEEDED(sc_->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)bb.GetAddressOf())))
        dev_->CreateRenderTargetView(bb.Get(), nullptr, &rtv_);
}

void Renderer::Resize(UINT w, UINT h) {
    if (!sc_ || !w || !h) return;
    ctx_->OMSetRenderTargets(0, nullptr, nullptr);
    rtv_.Reset();
    sc_->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, scFlags_);
    CreateRTV();
    refreshCheckedMs_ = -1e9;  // window may have moved to another monitor
}

double Renderer::RefreshHz() {
    double now = NowMs();
    if (now - refreshCheckedMs_ < 2000) return refreshHz_;
    refreshCheckedMs_ = now;
    ComPtr<IDXGIOutput> out;
    if (sc_ && SUCCEEDED(sc_->GetContainingOutput(&out))) {
        DXGI_OUTPUT_DESC od{};
        out->GetDesc(&od);
        DEVMODEW dm{};
        dm.dmSize = sizeof(dm);
        if (EnumDisplaySettingsW(od.DeviceName, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1) {
            double hz = dm.dmDisplayFrequency;
            // Windows reports 59.94/119.88 as 59/119.
            if (dm.dmDisplayFrequency % 60 == 59) hz = (dm.dmDisplayFrequency + 1) * 1000.0 / 1001.0;
            refreshHz_ = hz;
        }
    }
    return refreshHz_;
}

bool Renderer::MakeTex(UINT w, UINT h, DXGI_FORMAT fmt, Tex& t) {
    D3D11_TEXTURE2D_DESC td{};
    td.Width = std::max(1u, w);
    td.Height = std::max(1u, h);
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = fmt;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    t = Tex{};
    t.w = td.Width;
    t.h = td.Height;
    return SUCCEEDED(dev_->CreateTexture2D(&td, nullptr, &t.tex)) &&
           SUCCEEDED(dev_->CreateShaderResourceView(t.tex.Get(), nullptr, &t.srv)) &&
           SUCCEEDED(dev_->CreateUnorderedAccessView(t.tex.Get(), nullptr, &t.uav));
}

bool Renderer::CreateVideoResources(int w, int h) {
    Log("render: video resources %dx%d", w, h);
    vw_ = w;
    vh_ = h;
    mu_ = MotionUnit();
    l2w_ = Div(w, 2 * mu_); l2h_ = Div(h, 2 * mu_);
    l4w_ = Div(l2w_, 2); l4h_ = Div(l2h_, 2);
    l8w_ = Div(l4w_, 2); l8h_ = Div(l4h_, 2);
    l16w_ = Div(l8w_, 2); l16h_ = Div(l8h_, 2);
    g2w_ = Div(l2w_, 8); g2h_ = Div(l2h_, 8);
    g4w_ = Div(l4w_, 8); g4h_ = Div(l4h_, 8);
    g8w_ = Div(l8w_, 8); g8h_ = Div(l8h_, 8);
    g16w_ = Div(l16w_, 8); g16h_ = Div(l16h_, 8);
    bool ok = true;
    for (auto& s : slots_) {
        s = Slot{};
        ok &= MakeTex(w, h, DXGI_FORMAT_R8G8B8A8_UNORM, s.color);
        ok &= MakeTex(l2w_, l2h_, DXGI_FORMAT_R16_FLOAT, s.l2);
        ok &= MakeTex(l4w_, l4h_, DXGI_FORMAT_R16_FLOAT, s.l4);
        ok &= MakeTex(l8w_, l8h_, DXGI_FORMAT_R16_FLOAT, s.l8);
        ok &= MakeTex(l16w_, l16h_, DXGI_FORMAT_R16_FLOAT, s.l16);
        ok &= MakeTex(l2w_, l2h_, DXGI_FORMAT_R16_FLOAT, s.h2);
        ok &= MakeTex(l4w_, l4h_, DXGI_FORMAT_R16_FLOAT, s.h4);
        ok &= MakeTex(l8w_, l8h_, DXGI_FORMAT_R16_FLOAT, s.h8);
        ok &= MakeTex(l16w_, l16h_, DXGI_FORMAT_R16_FLOAT, s.h16);
        ok &= MakeTex((UINT)w, (UINT)h, DXGI_FORMAT_R8_UNORM, s.luma8);
        ok &= MakeTex((UINT)w, (UINT)h, DXGI_FORMAT_R8_UNORM, s.still);
        ok &= MakeTex((UINT)w / 4, (UINT)h / 4, DXGI_FORMAT_R16G16_SINT, s.nvF);
        ok &= MakeTex((UINT)w / 4, (UINT)h / 4, DXGI_FORMAT_R16G16_SINT, s.nvB);
        ok &= MakeTex(l2w_, l2h_, DXGI_FORMAT_R16G16B16A16_FLOAT, s.dense);
        ok &= MakeTex(g2w_, g2h_, DXGI_FORMAT_R16G16B16A16_FLOAT, s.fwd);
        ok &= MakeTex(g2w_, g2h_, DXGI_FORMAT_R16G16B16A16_FLOAT, s.bwd);
        ok &= MakeTex(g2w_, g2h_, DXGI_FORMAT_R16G16B16A16_FLOAT, s.field);
    }
    ok &= MakeTex(g16w_, g16h_, DXGI_FORMAT_R16G16B16A16_FLOAT, f16_);
    ok &= MakeTex(g8w_, g8h_, DXGI_FORMAT_R16G16B16A16_FLOAT, f8_);
    ok &= MakeTex(g4w_, g4h_, DXGI_FORMAT_R16G16B16A16_FLOAT, f4_);
    ok &= MakeTex(g2w_, g2h_, DXGI_FORMAT_R16G16B16A16_FLOAT, f2a_);
    ok &= MakeTex(g2w_, g2h_, DXGI_FORMAT_R16G16B16A16_FLOAT, f2b_);
    ok &= MakeTex(l2w_, l2h_, DXGI_FORMAT_R16G16B16A16_FLOAT, denseTmp_);
    if (!ok) vw_ = vh_ = 0;
    return ok;
}

void Renderer::RunCS(ID3D11ComputeShader* cs, std::initializer_list<ID3D11ShaderResourceView*> srvs,
                     ID3D11UnorderedAccessView* uav, ID3D11Buffer* cb, const void* data, UINT gx, UINT gy) {
    ctx_->UpdateSubresource(cb, 0, nullptr, data, 0, 0);
    ctx_->CSSetShader(cs, nullptr, 0);
    ID3D11ShaderResourceView* s[4] = {};
    UINT n = 0;
    for (auto v : srvs)
        if (n < 4) s[n++] = v;
    ctx_->CSSetShaderResources(0, 4, s);
    ctx_->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
    ctx_->CSSetConstantBuffers(0, 1, &cb);
    ID3D11SamplerState* smp = sampler_.Get();
    ctx_->CSSetSamplers(0, 1, &smp);
    ctx_->Dispatch(std::max(1u, gx), std::max(1u, gy), 1);
    ID3D11ShaderResourceView* ns[4] = {};
    ctx_->CSSetShaderResources(0, 4, ns);
    ID3D11UnorderedAccessView* nu = nullptr;
    ctx_->CSSetUnorderedAccessViews(0, 1, &nu, nullptr);
}

Renderer::Slot* Renderer::FindSeq(uint64_t seq) {
    for (auto& s : slots_)
        if (s.valid && s.seq == seq) return &s;
    return nullptr;
}

Renderer::Slot* Renderer::Newest() {
    Slot* best = nullptr;
    for (auto& s : slots_)
        if (s.valid && (!best || s.seq > best->seq)) best = &s;
    return best;
}

bool Renderer::HasVideo() const {
    for (auto& s : slots_)
        if (s.valid) return true;
    return false;
}

double Renderer::NewestTs() const {
    double t = 0;
    uint64_t seq = 0;
    for (auto& s : slots_)
        if (s.valid && s.seq >= seq) {
            seq = s.seq;
            t = s.ts;
        }
    return t;
}

void Renderer::ResetTimeline() {
    for (auto& s : slots_) s.valid = false;
}

void Renderer::Upload(const Frame& f, bool runMotion, double maxGapMs) {
    FinishMotion();  // the previous pair's flow feeds this pair's hints, and its slot may be reused
    if (f.width != vw_ || f.height != vh_ || (vh_ && MotionUnit() != mu_))
        if (!CreateVideoResources(f.width, f.height)) return;

    // Upload the raw frame as a typed integer texture; the GPU unpacks the bytes itself.
    //   YUY2: R8G8B8A8_UINT, W/2 x H, each texel = (Y0, U, Y1, V)
    //   BGRA: R8G8B8A8_UINT, W x H,   each texel = (B, G, R, A)
    //   NV12: R8_UINT, W x (H + H/2), luma plane then interleaved UV plane
    UINT tw, th, pitch;
    DXGI_FORMAT tf;
    if (f.fmt == PixFmt::YUY2) { tw = (UINT)f.width / 2; th = (UINT)f.height; pitch = (UINT)f.width * 2; tf = DXGI_FORMAT_R8G8B8A8_UINT; }
    else if (f.fmt == PixFmt::BGRA) { tw = (UINT)f.width; th = (UINT)f.height; pitch = (UINT)f.width * 4; tf = DXGI_FORMAT_R8G8B8A8_UINT; }
    else if (f.fmt == PixFmt::P010) { tw = (UINT)f.width; th = (UINT)f.height + ((UINT)f.height + 1) / 2; pitch = (UINT)f.width * 2; tf = DXGI_FORMAT_R16_UINT; }
    else { tw = (UINT)f.width; th = (UINT)f.height + ((UINT)f.height + 1) / 2; pitch = (UINT)f.width; tf = DXGI_FORMAT_R8_UINT; }
    if ((size_t)pitch * th > f.data.size()) return;
    if (!srcTex_ || tw != srcW_ || th != srcH_ || tf != srcFmt_) {
        srcTex_.Reset();
        srcSrv_.Reset();
        D3D11_TEXTURE2D_DESC td{};
        td.Width = tw;
        td.Height = th;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = tf;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(dev_->CreateTexture2D(&td, nullptr, &srcTex_)) ||
            FAILED(dev_->CreateShaderResourceView(srcTex_.Get(), nullptr, &srcSrv_))) {
            Log("render: could not create source texture %ux%u fmt %d", tw, th, (int)tf);
            srcTex_.Reset();
            return;
        }
        srcW_ = tw;
        srcH_ = th;
        srcFmt_ = tf;
    }
    ctx_->UpdateSubresource(srcTex_.Get(), 0, nullptr, f.data.data(), pitch, 0);

    // Reuse the oldest slot.
    Slot* s = &slots_[0];
    for (auto& c : slots_)
        if (!c.valid || c.seq < s->seq) {
            s = &c;
            if (!c.valid) break;
        }

    ConvertCB cc{(UINT)f.width, (UINT)f.height, (UINT)f.fmt, f.bt709 ? 1u : 0u, f.fullRange ? 1u : 0u, f.transfer, f.bt2020 ? 1u : 0u, 0};
    const bool planar = f.fmt == PixFmt::NV12 || f.fmt == PixFmt::P010;
    RunCS(csConvert_.Get(), {planar ? nullptr : srcSrv_.Get(), planar ? srcSrv_.Get() : nullptr}, s->color.uav.Get(), cbConvert_.Get(), &cc, Div(f.width, 16), Div(f.height, 16));
    DownCB d1{(UINT)f.width, (UINT)f.height, l2w_, l2h_};
    RunCS(csLumaHalf_.Get(), {s->color.srv.Get()}, s->l2.uav.Get(), cbDown_.Get(), &d1, Div(l2w_, 16), Div(l2h_, 16));
    DownCB d2{l2w_, l2h_, l4w_, l4h_};
    RunCS(csDown_.Get(), {s->l2.srv.Get()}, s->l4.uav.Get(), cbDown_.Get(), &d2, Div(l4w_, 16), Div(l4h_, 16));
    DownCB d3{l4w_, l4h_, l8w_, l8h_};
    RunCS(csDown_.Get(), {s->l4.srv.Get()}, s->l8.uav.Get(), cbDown_.Get(), &d3, Div(l8w_, 16), Div(l8h_, 16));
    DownCB d4{l8w_, l8h_, l16w_, l16h_};
    RunCS(csDown_.Get(), {s->l8.srv.Get()}, s->l16.uav.Get(), cbDown_.Get(), &d4, Div(l16w_, 16), Div(l16h_, 16));
    if (tuning.engine == 1) {
        const UINT sc = (UINT)NvScale();
        const UINT lw = (UINT)f.width / sc, lh = (UINT)f.height / sc;
        if (s->luma8.w != lw || s->luma8.h != lh) {
            MakeTex(lw, lh, DXGI_FORMAT_R8_UNORM, s->luma8);
            MakeTex(lw / 4, lh / 4, DXGI_FORMAT_R16G16_SINT, s->nvF);
            MakeTex(lw / 4, lh / 4, DXGI_FORMAT_R16G16_SINT, s->nvB);
        }
        Luma8CB lc{lw, lh, sc, 0};
        RunCS(csLuma8_.Get(), {s->color.srv.Get()}, s->luma8.uav.Get(), cbLuma8_.Get(), &lc, Div(lw, 16), Div(lh, 16));
    }
    if (tuning.matchHP > 0) {
        auto hp = [&](Tex& in, Tex& out, UINT w, UINT h) {
            HighPassCB hc{w, h, tuning.matchHP, 0};
            RunCS(csHighPass_.Get(), {in.srv.Get()}, out.uav.Get(), cbHighPass_.Get(), &hc, Div(w, 16), Div(h, 16));
        };
        hp(s->l2, s->h2, l2w_, l2h_);
        hp(s->l4, s->h4, l4w_, l4h_);
        hp(s->l8, s->h8, l8w_, l8h_);
        hp(s->l16, s->h16, l16w_, l16h_);
    }

    s->ts = f.ts;
    s->seq = ++seq_;
    s->valid = true;
    s->hasField = false;
    s->cut = f.sceneCut;
    Slot* prev = FindSeq(s->seq - 1);
    if (!prev || f.ts - prev->ts > maxGapMs) s->cut = true;
    if (runMotion && prev && !s->cut) {
        // Start motion as soon as the frame is here. The NV outputs stay owned by CUDA until it's
        // done (FinishMotion), so draws queued meanwhile don't wait for it.
        StaticCB sc{(UINT)vw_, (UINT)vh_, tuning.staticThr, 0};
        RunCS(csStatic_.Get(), {prev->color.srv.Get(), s->color.srv.Get()}, s->still.uav.Get(), cbStatic_.Get(), &sc, Div(vw_, 16),
              Div(vh_, 16));
        s->hasField = false;  // set by KickMotion (shader engine) or FinishMotion (NV)
        kickA_ = prev;
        kickB_ = s;
        KickMotion();
    }
}

void Renderer::Motion(Slot& a, Slot& b, Slot* temporal) {
    if (tuning.engine == 1 && MotionNv(a, b, temporal)) return;
    MotionBlocks(a, b, temporal);
}

bool Renderer::MotionNv(Slot& a, Slot& b, Slot* temporal) {
    const int sc = NvScale();
    const int fw = vw_ / sc, fh = vh_ / sc;
    if (b.luma8.w != (UINT)fw || a.luma8.w != (UINT)fw) return false;  // luma not prepared at this scale yet
    if (!nv_.Ok() || nv_.Width() != fw || nv_.Height() != fh || nvScaleActive_ != sc || !nv_.Has(tuning.nvPerf)) {
        if (nvTried_ && !nv_.Ok() && nvScaleActive_ == sc) return false;
        nvTried_ = true;
        nvScaleActive_ = sc;
        ComPtr<IDXGIDevice> dd;
        ComPtr<IDXGIAdapter> ad;
        // Prepare best and medium together (plus whatever was asked for), so live switching is instant.
        bool okInit = SUCCEEDED(dev_.As(&dd)) && SUCCEEDED(dd->GetAdapter(&ad));
        if (okInit) {
            if (tuning.nvPerf == 5 || tuning.nvPerf == 10) okInit = nv_.Init(ad.Get(), fw, fh, {5, 10});
            else okInit = nv_.Init(ad.Get(), fw, fh, {5, 10, tuning.nvPerf});
        }
        if (!okInit) {
            Log("nvflow: unavailable (%s); using block search", nv_.Error().c_str());
            return false;
        }
    }
    if (tuning.nvHybrid) MotionBlocks(a, b, temporal);  // its per-pixel vectors become a candidate
    const double nt0 = NowMs();
    // Each quality level keeps its own motion history; after switching, start that level fresh.
    const bool fresh = temporal == nullptr || nvLastPerf_ != tuning.nvPerf;
    nvLastPerf_ = tuning.nvPerf;
    const bool nvOk = nv_.Compute(ctx_.Get(), a.luma8.tex.Get(), b.luma8.tex.Get(), b.nvF.tex.Get(), b.nvB.tex.Get(), fresh, tuning.nvPerf);
    nvMs_ = nvMs_ * 0.9f + (float)(NowMs() - nt0) * 0.1f;
    if (!nvOk) {
        Log("nvflow: compute failed (%s); using block search", nv_.Error().c_str());
        return false;
    }
    // The D3D11 queue runs in order: anything queued now that reads the hardware flow would make
    // the next present wait for CUDA too. So the conversion passes run later (FinishMotion), after
    // the next frame has been presented, or right away if a draw actually needs this pair.
    pendA_ = &a;
    pendB_ = &b;
    return true;
}

void Renderer::KickMotion() {
    if (!kickB_) return;
    Slot& a = *kickA_;
    Slot& b = *kickB_;
    kickA_ = kickB_ = nullptr;
    if (!a.valid || !b.valid || b.seq != a.seq + 1) return;
    Motion(a, b, a.hasField ? &a : nullptr);
    if (pendB_ != &b) b.hasField = true;  // shader engine: done; NV: FinishMotion sets it
}

void Renderer::FinishMotion(bool force) {
    if (force) KickMotion();
    if (!pendB_) return;
    if (!force && nv_.Busy()) return;  // still running; draws carry on without waiting for it
    if (!nv_.Busy() && nv_.LastDoneMs() >= 0) nvDoneMax_ = std::max(nvDoneMax_, (float)nv_.LastDoneMs());
    else nvForced_++;
    nv_.Release();
    Slot& a = *pendA_;
    Slot& b = *pendB_;
    pendA_ = pendB_ = nullptr;
    if (!b.valid || !a.valid) return;
    const int sc = NvScale();
    const int fw = vw_ / sc, fh = vh_ / sc;
    // Coarse fields (hints, occlusion) and per-pixel midpoint vectors from the hardware flow.
    NvGridCB gc{{g2w_, g2h_}, {(UINT)fw / 4, (UINT)fh / 4}, (float)sc / mu_, {}};
    ctx_->UpdateSubresource(cbNvGrid_.Get(), 0, nullptr, &gc, 0, 0);
    ctx_->CSSetShader(csNvGrid_.Get(), nullptr, 0);
    ID3D11ShaderResourceView* gs[2] = {b.nvF.srv.Get(), b.nvB.srv.Get()};
    ctx_->CSSetShaderResources(0, 2, gs);
    ID3D11UnorderedAccessView* gu[3] = {b.field.uav.Get(), b.fwd.uav.Get(), b.bwd.uav.Get()};
    ctx_->CSSetUnorderedAccessViews(0, 3, gu, nullptr);
    ID3D11Buffer* gcb = cbNvGrid_.Get();
    ctx_->CSSetConstantBuffers(0, 1, &gcb);
    ctx_->Dispatch(Div(g2w_, 8), Div(g2h_, 8), 1);
    ID3D11UnorderedAccessView* nu[3] = {};
    ctx_->CSSetUnorderedAccessViews(0, 3, nu, nullptr);
    ID3D11ShaderResourceView* ns[2] = {};
    ctx_->CSSetShaderResources(0, 2, ns);
    NvDenseCB dc{{l2w_, l2h_}, {(UINT)fw / 4, (UINT)fh / 4}, {1.0f / l2w_, 1.0f / l2h_}, tuning.nvHybrid ? 1u : 0u, tuning.nvBias,
                 tuning.nvBilinear ? 1u : 0u, (float)sc / mu_, {}};
    // RunCS binds up to 4 SRVs; the prior (t4) is bound separately.
    ctx_->UpdateSubresource(cbNvDense_.Get(), 0, nullptr, &dc, 0, 0);
    ctx_->CSSetShader(csNvDense_.Get(), nullptr, 0);
    ID3D11ShaderResourceView* ds[5] = {b.nvF.srv.Get(), b.nvB.srv.Get(), a.l2.srv.Get(), b.l2.srv.Get(),
                                       tuning.nvHybrid ? b.dense.srv.Get() : nullptr};
    ctx_->CSSetShaderResources(0, 5, ds);
    const bool viaTmp = tuning.denseMedian || tuning.nvHybrid;  // hybrid reads b.dense as the prior
    ID3D11UnorderedAccessView* du = viaTmp ? denseTmp_.uav.Get() : b.dense.uav.Get();
    ctx_->CSSetUnorderedAccessViews(0, 1, &du, nullptr);
    ID3D11Buffer* dcb = cbNvDense_.Get();
    ctx_->CSSetConstantBuffers(0, 1, &dcb);
    ID3D11SamplerState* smp = sampler_.Get();
    ctx_->CSSetSamplers(0, 1, &smp);
    ctx_->Dispatch(Div(l2w_, 8), Div(l2h_, 8), 1);
    ID3D11ShaderResourceView* ds0[5] = {};
    ctx_->CSSetShaderResources(0, 5, ds0);
    ID3D11UnorderedAccessView* du0 = nullptr;
    ctx_->CSSetUnorderedAccessViews(0, 1, &du0, nullptr);
    if (tuning.denseMedian) {
        MedianCB dm{{l2w_, l2h_}, {}};
        RunCS(csMedian_.Get(), {denseTmp_.srv.Get()}, b.dense.uav.Get(), cbMedian_.Get(), &dm, Div(l2w_, 8), Div(l2h_, 8));
    } else if (viaTmp) {
        ctx_->CopyResource(b.dense.tex.Get(), denseTmp_.tex.Get());
    }
    b.hasDir = true;
    b.hasField = true;
}

void Renderer::MotionBlocks(Slot& a0, Slot& b0, Slot* temporal) {
    // Matching signal: raw luma, or its local-pattern (high-passed) version.
    struct Lv { Tex &l2, &l4, &l8, &l16; };
    const bool hpOn = tuning.matchHP > 0;
    Lv a{hpOn ? a0.h2 : a0.l2, hpOn ? a0.h4 : a0.l4, hpOn ? a0.h8 : a0.l8, hpOn ? a0.h16 : a0.l16};
    Lv bl{hpOn ? b0.h2 : b0.l2, hpOn ? b0.h4 : b0.l4, hpOn ? b0.h8 : b0.l8, hpOn ? b0.h16 : b0.l16};
    Slot& b = b0;
    Tex* tempSrc = temporal ? &temporal->field : nullptr;  // motion hint
    UINT asym = 0;
    float tempSign = 1.0f;
    auto run = [&](Tex& la, Tex& lb, Tex* pred, UINT pgw, UINT pgh, Tex& out, UINT lw, UINT lh, UINT gw, UINT gh, float scale,
                   float step, int radius, UINT mode, int local = 0) {
        SearchCB c{};
        c.LevelSize[0] = (float)lw;
        c.LevelSize[1] = (float)lh;
        c.InvLevelSize[0] = 1.0f / lw;
        c.InvLevelSize[1] = 1.0f / lh;
        c.Grid[0] = gw;
        c.Grid[1] = gh;
        c.PredGrid[0] = pgw;
        c.PredGrid[1] = pgh;
        c.TempGrid[0] = g2w_;
        c.TempGrid[1] = g2h_;
        c.Scale = scale;
        c.Step = step;
        c.Radius = radius;
        c.Mode = mode;
        c.UseTemporal = tempSrc ? 1u : 0u;
        c.Asym = asym;
        c.TempSign = tempSign;
        c.Lambda = tuning.lambda;
        c.Local = local;
        c.LocalPen = tuning.localPen;
        RunCS(csSearch_.Get(),
              {la.srv.Get(), lb.srv.Get(), pred ? pred->srv.Get() : nullptr, tempSrc ? tempSrc->srv.Get() : nullptr},
              out.uav.Get(), cbSearch_.Get(), &c, gw, gh);
    };
    // 1/16 res full search, radius 8 => +-128 px of motion per source frame at full res,
    // then refine at 1/8 (more candidates from neighbours + last frame's motion).
    run(a.l16, bl.l16, nullptr, 1, 1, f16_, l16w_, l16h_, g16w_, g16h_, 16.0f, 1.0f, tuning.radius16, 0);
    run(a.l8, bl.l8, &f16_, g16w_, g16h_, f8_, l8w_, l8h_, g8w_, g8h_, 8.0f, 1.0f, 1, 1);
    // Small-object search (+-2 at 1/4 and 1/2 = +-8 / +-4 px full res, full size penalty): keeps a
    // camera-followed character's head solid while the background whips past. Tuned in
    // tools/fgsim.py on the on-foot spin captures; neutral (-0.07 dB) on the driving captures.
    run(a.l4, bl.l4, &f8_, g8w_, g8h_, f4_, l4w_, l4h_, g4w_, g4h_, 4.0f, 1.0f, 1, 1, tuning.local);
    run(a.l2, bl.l2, &f4_, g4w_, g4h_, f2a_, l2w_, l2h_, g2w_, g2h_, 2.0f, 1.0f, 1, 1, tuning.local);
    run(a.l2, bl.l2, &f2a_, g2w_, g2h_, f2b_, l2w_, l2h_, g2w_, g2h_, 2.0f, 0.5f, 1, 2);
    MedianCB mc{{g2w_, g2h_}, {}};
    RunCS(csMedian_.Get(), {f2b_.srv.Get()}, b.field.uav.Get(), cbMedian_.Get(), &mc, Div(g2w_, 8), Div(g2h_, 8));
    RefineCB rc{{1.0f / l2w_, 1.0f / l2h_}, {l2w_, l2h_}, {g2w_, g2h_}, tuning.refineBias, tuning.zeroBias};
    RunCS(csRefine_.Get(), {a.l2.srv.Get(), bl.l2.srv.Get(), b.field.srv.Get()},
          tuning.denseMedian ? denseTmp_.uav.Get() : b.dense.uav.Get(), cbRefine_.Get(), &rc, Div(l2w_, 8), Div(l2h_, 8));
    if (tuning.denseMedian) {
        // 3x3 median on the per-pixel vectors (+0.1 dB, removes isolated wrong picks).
        MedianCB dm{{l2w_, l2h_}, {}};
        RunCS(csMedian_.Get(), {denseTmp_.srv.Get()}, b.dense.uav.Get(), cbMedian_.Get(), &dm, Div(l2w_, 8), Div(l2h_, 8));
    }
    b.hasDir = false;
    if (tuning.occlusion) {
        // One-sided fields for occlusion checks, seeded by the midpoint field just computed.
        auto oneSided = [&](Tex& l2x, Tex& l4x, Tex& l8x, Tex& l16x, Tex& l2y, Tex& l4y, Tex& l8y, Tex& l16y, float sign, Tex& dst) {
            asym = 1;
            tempSrc = &b.field;
            tempSign = sign;
            run(l16x, l16y, nullptr, 1, 1, f16_, l16w_, l16h_, g16w_, g16h_, 16.0f, 1.0f, 2, 0);
            run(l8x, l8y, &f16_, g16w_, g16h_, f8_, l8w_, l8h_, g8w_, g8h_, 8.0f, 1.0f, 1, 1);
            run(l4x, l4y, &f8_, g8w_, g8h_, f4_, l4w_, l4h_, g4w_, g4h_, 4.0f, 1.0f, 1, 1, tuning.local);
            run(l2x, l2y, &f4_, g4w_, g4h_, f2a_, l2w_, l2h_, g2w_, g2h_, 2.0f, 1.0f, 1, 1, tuning.local);
            MedianCB m2{{g2w_, g2h_}, {}};
            RunCS(csMedian_.Get(), {f2a_.srv.Get()}, dst.uav.Get(), cbMedian_.Get(), &m2, Div(g2w_, 8), Div(g2h_, 8));
        };
        oneSided(a.l2, a.l4, a.l8, a.l16, bl.l2, bl.l4, bl.l8, bl.l16, 1.0f, b.fwd);    // A -> B, from A
        oneSided(bl.l2, bl.l4, bl.l8, bl.l16, a.l2, a.l4, a.l8, a.l16, -1.0f, b.bwd);   // B -> A, from B
        asym = 0;
        tempSrc = temporal ? &temporal->field : nullptr;
        tempSign = 1.0f;
        b.hasDir = true;
    }
}

bool Renderer::SaveNewestColorBmp(const std::wstring& path) {
    Slot* s = Newest();
    if (!s || !s->color.tex) return false;
    D3D11_TEXTURE2D_DESC d{};
    s->color.tex->GetDesc(&d);
    d.Usage = D3D11_USAGE_STAGING;
    d.BindFlags = 0;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    d.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> st;
    if (FAILED(dev_->CreateTexture2D(&d, nullptr, &st))) return false;
    ctx_->CopyResource(st.Get(), s->color.tex.Get());
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(ctx_->Map(st.Get(), 0, D3D11_MAP_READ, 0, &m))) return false;
    const int dw = (int)d.Width / 4, dh = (int)d.Height / 4;
    const uint32_t rowBytes = (uint32_t)dw * 3, pad = (4 - rowBytes % 4) % 4, imgSize = (rowBytes + pad) * dh;
    std::vector<uint8_t> out(imgSize, 0);
    for (int y = 0; y < dh; y++) {
        const uint8_t* src = (const uint8_t*)m.pData + (size_t)(y * 4) * m.RowPitch;
        uint8_t* dst = &out[(size_t)(dh - 1 - y) * (rowBytes + pad)];
        for (int x = 0; x < dw; x++) {
            const uint8_t* p = src + (size_t)(x * 4 + 1) * 4;  // RGBA; odd pixel, checks the 2nd luma of a pair
            dst[x * 3 + 0] = p[2];
            dst[x * 3 + 1] = p[1];
            dst[x * 3 + 2] = p[0];
        }
    }
    ctx_->Unmap(st.Get(), 0);
    FILE* fp = _wfopen(path.c_str(), L"wb");
    if (!fp) return false;
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
    fwrite(out.data(), 1, out.size(), fp);
    fclose(fp);
    return true;
}

// Draw-only GPU timer (final present pass + UI), used to decide how late we can start drawing.
void Renderer::BeginDrawTimer() {
    PollDrawTimers();
    GpuQuery& q = drawQ_[drawIdx_];
    if (q.pending || q.open) return;
    if (!q.disjoint) {
        D3D11_QUERY_DESC qd{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
        dev_->CreateQuery(&qd, &q.disjoint);
        qd.Query = D3D11_QUERY_TIMESTAMP;
        dev_->CreateQuery(&qd, &q.t0);
        dev_->CreateQuery(&qd, &q.t1);
    }
    ctx_->Begin(q.disjoint.Get());
    ctx_->End(q.t0.Get());
    q.open = true;
}

void Renderer::EndDrawTimer() {
    GpuQuery& q = drawQ_[drawIdx_];
    if (!q.open) return;
    ctx_->End(q.t1.Get());
    ctx_->End(q.disjoint.Get());
    q.open = false;
    q.pending = true;
    drawIdx_ = (drawIdx_ + 1) % 4;
}

void Renderer::PollDrawTimers() {
    for (auto& q : drawQ_) {
        if (!q.pending) continue;
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
        if (ctx_->GetData(q.disjoint.Get(), &dj, sizeof(dj), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) continue;
        UINT64 t0 = 0, t1 = 0;
        if (ctx_->GetData(q.t0.Get(), &t0, sizeof(t0), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) continue;
        if (ctx_->GetData(q.t1.Get(), &t1, sizeof(t1), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) continue;
        q.pending = false;
        if (!dj.Disjoint && dj.Frequency && t1 > t0) {
            float ms = (float)((t1 - t0) * 1000.0 / dj.Frequency);
            drawMsMax_ = std::max(ms, drawMsMax_ * 0.98f);  // recent peak, decays slowly
        }
    }
}

void Renderer::BeginGpuTimer() {
    PollGpuTimers();
    GpuQuery& q = queries_[queryIdx_];
    if (q.pending || q.open) return;
    ctx_->Begin(q.disjoint.Get());
    ctx_->End(q.t0.Get());
    q.open = true;
}

void Renderer::PollGpuTimers() {
    for (auto& q : queries_) {
        if (!q.pending) continue;
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
        if (ctx_->GetData(q.disjoint.Get(), &dj, sizeof(dj), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) continue;
        UINT64 t0 = 0, t1 = 0;
        if (ctx_->GetData(q.t0.Get(), &t0, sizeof(t0), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) continue;
        if (ctx_->GetData(q.t1.Get(), &t1, sizeof(t1), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) continue;
        q.pending = false;
        if (!dj.Disjoint && dj.Frequency && t1 > t0) {
            float ms = (float)((t1 - t0) * 1000.0 / dj.Frequency);
            gpuMs_ = gpuMs_ * 0.9f + ms * 0.1f;
        }
    }
}

void Renderer::BindBackbuffer() {
    ID3D11RenderTargetView* rtv = rtv_.Get();
    ctx_->OMSetRenderTargets(1, &rtv, nullptr);
}

void Renderer::BeginFrame() {
    BindBackbuffer();
    const float black[4] = {0, 0, 0, 1};
    ctx_->ClearRenderTargetView(rtv_.Get(), black);
}

DrawResult Renderer::DrawVideo(double d, bool interpolate, const D3D11_VIEWPORT& vp, bool showMotion) {
    DrawResult r;
    FinishMotion(false);  // hand finished flow back to D3D11 without waiting
    Slot* newest = Newest();
    if (!newest || !vw_) return r;
    Slot *A = nullptr, *B = newest;
    PresentCB cb{};
    cb.SrcSize[0] = (float)vw_;
    cb.SrcSize[1] = (float)vh_;
    cb.FieldUVScale[0] = (float)vw_ / (16.0f * mu_ * g2w_);
    cb.FieldUVScale[1] = (float)vh_ / (16.0f * mu_ * g2h_);
    cb.pad[0] = (float)mu_;
    cb.Mode = 0;
    cb.ShowMotion = showMotion ? 1u : 0u;
    r.contentTs = newest->ts;

    if (interpolate && d < newest->ts) {
        // B = oldest frame later than the display time; A = the one before it.
        Slot* nb = nullptr;
        for (auto& s : slots_)
            if (s.valid && s.ts > d && (!nb || s.seq < nb->seq)) nb = &s;
        Slot* na = nb ? FindSeq(nb->seq - 1) : nullptr;
        if (nb && na && d >= na->ts && nb->ts > na->ts) {
            if (nb == kickB_) KickMotion();
            const double tPair = (d - na->ts) / (nb->ts - na->ts);
            if (nb == pendB_ && nv_.Busy()) {
                // Waiting here would stall this present (and lock output to half rate). Show the
                // nearer real frame for this one tick instead. Right next to a real frame that IS the
                // right picture (vsync ticks are locked onto real frames, give or take a little), so
                // only a tick well inside the pair counts as late; the caller then grows the hold.
                r.flowLate = tPair > 0.15 && tPair < 0.85;
                B = tPair < 0.5 ? na : nb;
                r.contentTs = B->ts;
            } else {
                if (nb == pendB_) FinishMotion(false);
                A = na;
                B = nb;
                cb.T = (float)std::clamp((d - na->ts) / (nb->ts - na->ts), 0.0, 1.0);
                cb.Mode = (nb->cut || !nb->hasField) ? 2u : 1u;
                r.contentTs = d;
                r.interpolated = cb.Mode == 1;
            }
        } else if (nb) {
            B = nb;  // display time is before everything we hold
            r.contentTs = nb->ts;
        }
    }

    // The generated frame only needs computing at source resolution; scaling it up once afterwards
    // is far cheaper than running the warp/blend for every 4K output pixel.
    // With the sharp scaler every frame (real ones too) takes the same path, so sharpness never
    // flickers between real and generated frames; shrinking (4K capture on a smaller screen) uses
    // it as well, since a direct bilinear draw would alias.
    const bool sized = std::fabs(vp.Width - vw_) > 0.5f || std::fabs(vp.Height - vh_) > 0.5f;
    const bool enlarging = vp.Width > vw_ * 1.05f || vp.Height > vh_ * 1.05f;
    const bool viaSrc = sized && ((tuning.scaler == 1) || (tuning.srcResDraw && cb.Mode != 0 && enlarging)) && EnsureInter();
    if (viaSrc) {
        D3D11_VIEWPORT sv{0, 0, (float)vw_, (float)vh_, 0, 1};
        DrawPair(A, B, cb.T, cb.Mode, showMotion, sv, interRtv_.Get());
        Upscale(inter_.srv.Get(), vp, rtv_.Get());
    } else {
        DrawPair(A, B, cb.T, cb.Mode, showMotion, vp, rtv_.Get());
    }
    r.any = true;
    return r;
}

bool Renderer::EnsureInter() {
    if (inter_.tex && inter_.w == (UINT)vw_ && inter_.h == (UINT)vh_) return true;
    inter_ = Tex{};
    interRtv_.Reset();
    D3D11_TEXTURE2D_DESC td{};
    td.Width = (UINT)vw_;
    td.Height = (UINT)vh_;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(dev_->CreateTexture2D(&td, nullptr, &inter_.tex)) || FAILED(dev_->CreateShaderResourceView(inter_.tex.Get(), nullptr, &inter_.srv)) ||
        FAILED(dev_->CreateRenderTargetView(inter_.tex.Get(), nullptr, &interRtv_))) {
        inter_ = Tex{};
        return false;
    }
    inter_.w = td.Width;
    inter_.h = td.Height;
    return true;
}

bool Renderer::ScaleSharp(ID3D11ShaderResourceView* src, const D3D11_VIEWPORT& vp, ID3D11RenderTargetView* rtv, int sw, int sh,
                          float sharpness) {
    if (sw <= 0) sw = vw_, sh = vh_;
    if (sharpness < 0) sharpness = tuning.sharpness;
    const UINT ow = (UINT)(vp.Width + 0.5f), oh = (UINT)(vp.Height + 0.5f);
    if (!ow || !oh || sw <= 0 || sh <= 0) return false;
    if (scaleTmp_.w != ow || scaleTmp_.h != oh) {
        scaleTmp_ = Tex{};
        scaleTmpRtv_.Reset();
        D3D11_TEXTURE2D_DESC td{};
        td.Width = ow;
        td.Height = oh;
        td.MipLevels = td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(dev_->CreateTexture2D(&td, nullptr, &scaleTmp_.tex)) ||
            FAILED(dev_->CreateShaderResourceView(scaleTmp_.tex.Get(), nullptr, &scaleTmp_.srv)) ||
            FAILED(dev_->CreateRenderTargetView(scaleTmp_.tex.Get(), nullptr, &scaleTmpRtv_))) {
            scaleTmp_ = Tex{};
            return false;
        }
        scaleTmp_.w = ow;
        scaleTmp_.h = oh;
    }
    struct {
        float srcSize[2], dstSize[2], dstOrigin[2], sharp, ratio;
    } c{{(float)sw, (float)sh}, {(float)ow, (float)oh}, {0, 0}, 0, std::max((float)sw / ow, (float)sh / oh)};
    // Slider 0..1 maps to FSR's 2..0.2 "stops" (0.2 = its default, fairly strong).
    c.sharp = sharpness <= 0.001f ? 0.0f : std::exp2(-(2.0f - 1.8f * std::clamp(sharpness, 0.0f, 1.0f)));
    ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx_->IASetInputLayout(nullptr);
    ctx_->VSSetShader(vs_.Get(), nullptr, 0);
    ctx_->RSSetState(nullptr);
    ctx_->OMSetBlendState(nullptr, nullptr, 0xffffffff);
    ctx_->OMSetDepthStencilState(nullptr, 0);
    ID3D11Buffer* cbs = cbScale_.Get();
    // Pass 1: resample into the intermediate (outputs first: see Upscale).
    ID3D11RenderTargetView* t = scaleTmpRtv_.Get();
    ctx_->OMSetRenderTargets(1, &t, nullptr);
    ctx_->UpdateSubresource(cbScale_.Get(), 0, nullptr, &c, 0, 0);
    ctx_->PSSetConstantBuffers(0, 1, &cbs);
    ctx_->PSSetShader(psLanczos_.Get(), nullptr, 0);
    ctx_->PSSetShaderResources(0, 1, &src);
    D3D11_VIEWPORT tv{0, 0, (float)ow, (float)oh, 0, 1};
    ctx_->RSSetViewports(1, &tv);
    ctx_->Draw(3, 0);
    ID3D11ShaderResourceView* none = nullptr;
    ctx_->PSSetShaderResources(0, 1, &none);
    // Pass 2: sharpen into the destination.
    c.dstOrigin[0] = vp.TopLeftX;
    c.dstOrigin[1] = vp.TopLeftY;
    ctx_->OMSetRenderTargets(1, &rtv, nullptr);
    ctx_->UpdateSubresource(cbScale_.Get(), 0, nullptr, &c, 0, 0);
    ctx_->PSSetShader(psRcas_.Get(), nullptr, 0);
    ID3D11ShaderResourceView* ts = scaleTmp_.srv.Get();
    ctx_->PSSetShaderResources(0, 1, &ts);
    ctx_->RSSetViewports(1, &vp);
    ctx_->Draw(3, 0);
    ctx_->PSSetShaderResources(0, 1, &none);
    return true;
}

void Renderer::Upscale(ID3D11ShaderResourceView* src, const D3D11_VIEWPORT& vp, ID3D11RenderTargetView* rtv) {
    if (tuning.scaler == 1 && ScaleSharp(src, vp, rtv)) return;
    ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx_->IASetInputLayout(nullptr);
    ctx_->VSSetShader(vs_.Get(), nullptr, 0);
    ctx_->PSSetShader(psUp_.Get(), nullptr, 0);
    // Switch the output first: the source was the previous pass's render target, and D3D11 silently
    // drops a shader input that is still bound as an output (the result would be black).
    ctx_->OMSetRenderTargets(1, &rtv, nullptr);
    ctx_->PSSetShaderResources(0, 1, &src);
    ID3D11SamplerState* smp = sampler_.Get();
    ctx_->PSSetSamplers(0, 1, &smp);
    ctx_->RSSetState(nullptr);
    ctx_->RSSetViewports(1, &vp);
    ctx_->OMSetBlendState(nullptr, nullptr, 0xffffffff);
    ctx_->OMSetDepthStencilState(nullptr, 0);
    ctx_->Draw(3, 0);
    ID3D11ShaderResourceView* none = nullptr;
    ctx_->PSSetShaderResources(0, 1, &none);
}

bool Renderer::ScaleCheck(double out[6]) {
    FinishMotion();
    Slot* B = Newest();
    if (!B) return false;
    auto make = [&](UINT w, UINT h, Tex& t, ComPtr<ID3D11RenderTargetView>& v, ComPtr<ID3D11Texture2D>* st) {
        D3D11_TEXTURE2D_DESC td{};
        td.Width = w;
        td.Height = h;
        td.MipLevels = td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(dev_->CreateTexture2D(&td, nullptr, &t.tex)) || FAILED(dev_->CreateRenderTargetView(t.tex.Get(), nullptr, &v)) ||
            FAILED(dev_->CreateShaderResourceView(t.tex.Get(), nullptr, &t.srv)))
            return false;
        t.w = w;
        t.h = h;
        if (!st) return true;
        td.Usage = D3D11_USAGE_STAGING;
        td.BindFlags = 0;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        return SUCCEEDED(dev_->CreateTexture2D(&td, nullptr, st->GetAddressOf()));
    };
    const UINT W = (UINT)vw_, H = (UINT)vh_, sw = W * 2 / 3, sh = H * 2 / 3;
    Tex small, back, ref, big4, big14;
    ComPtr<ID3D11RenderTargetView> vSmall, vBack, vRef, v4, v14;
    ComPtr<ID3D11Texture2D> stBack, stRef;
    if (!make(sw, sh, small, vSmall, nullptr) || !make(W, H, back, vBack, &stBack) || !make(W, H, ref, vRef, &stRef) ||
        !make(3840, 2160, big4, v4, nullptr) || !make(2560, 1440, big14, v14, nullptr))
        return false;
    // The color slot is a UAV texture: copy into an RGBA8 reference first.
    D3D11_VIEWPORT vpW{0, 0, (float)W, (float)H, 0, 1}, vpS{0, 0, (float)sw, (float)sh, 0, 1};
    const int keep = tuning.scaler;
    tuning.scaler = 0;
    Upscale(B->color.srv.Get(), vpW, vRef.Get());  // same size: bilinear = exact copy
    ScaleSharp(ref.srv.Get(), vpS, vSmall.Get(), W, H, 0.0f);  // antialiased shrink
    auto psnrOf = [&]() {
        ctx_->CopyResource(stBack.Get(), back.tex.Get());
        ctx_->CopyResource(stRef.Get(), ref.tex.Get());
        D3D11_MAPPED_SUBRESOURCE m1{}, m2{};
        if (FAILED(ctx_->Map(stBack.Get(), 0, D3D11_MAP_READ, 0, &m1))) return 0.0;
        if (FAILED(ctx_->Map(stRef.Get(), 0, D3D11_MAP_READ, 0, &m2))) {
            ctx_->Unmap(stBack.Get(), 0);
            return 0.0;
        }
        double sse = 0;
        for (UINT y = 0; y < H; y++) {
            const uint8_t* a = (const uint8_t*)m1.pData + (size_t)y * m1.RowPitch;
            const uint8_t* b = (const uint8_t*)m2.pData + (size_t)y * m2.RowPitch;
            for (UINT x = 0; x < W * 4; x++)
                if ((x & 3) != 3) {
                    double d = (a[x] - b[x]) / 255.0;
                    sse += d * d;
                }
        }
        ctx_->Unmap(stBack.Get(), 0);
        ctx_->Unmap(stRef.Get(), 0);
        return 10.0 * std::log10(1.0 / std::max(sse / ((double)W * H * 3), 1e-12));
    };
    Upscale(small.srv.Get(), vpW, vBack.Get());
    out[0] = psnrOf();
    ScaleSharp(small.srv.Get(), vpW, vBack.Get(), sw, sh, 0.0f);
    out[1] = psnrOf();
    ScaleSharp(small.srv.Get(), vpW, vBack.Get(), sw, sh, 0.5f);
    out[2] = psnrOf();
    auto timed = [&](auto&& work) {
        ComPtr<ID3D11Query> dj, q0, q1;
        D3D11_QUERY_DESC qd{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
        dev_->CreateQuery(&qd, &dj);
        qd.Query = D3D11_QUERY_TIMESTAMP;
        dev_->CreateQuery(&qd, &q0);
        dev_->CreateQuery(&qd, &q1);
        double best = 1e9;
        for (int rep = 0; rep < 8; rep++) {
            ctx_->Begin(dj.Get());
            ctx_->End(q0.Get());
            work();
            ctx_->End(q1.Get());
            ctx_->End(dj.Get());
            D3D11_QUERY_DATA_TIMESTAMP_DISJOINT d{};
            while (ctx_->GetData(dj.Get(), &d, sizeof(d), 0) != S_OK) {}
            UINT64 a = 0, b = 0;
            ctx_->GetData(q0.Get(), &a, sizeof(a), 0);
            ctx_->GetData(q1.Get(), &b, sizeof(b), 0);
            if (!d.Disjoint && d.Frequency) best = std::min(best, (b - a) * 1000.0 / d.Frequency);
        }
        return best;
    };
    D3D11_VIEWPORT vp4{0, 0, 3840, 2160, 0, 1}, vp14{0, 0, 2560, 1440, 0, 1};
    out[3] = timed([&] { Upscale(ref.srv.Get(), vp4, v4.Get()); });
    out[4] = timed([&] { ScaleSharp(ref.srv.Get(), vp4, v4.Get(), W, H, 0.5f); });
    out[5] = timed([&] { ScaleSharp(ref.srv.Get(), vp14, v14.Get(), W, H, 0.5f); });
    tuning.scaler = keep;
    BindBackbuffer();
    return true;
}

bool Renderer::DrawCheck(double& msDirect, double& msSrc, double& psnr) {
    FinishMotion();
    Slot* B = Newest();
    if (!B || !B->hasField) return false;
    Slot* A = FindSeq(B->seq - 1);
    if (!A || !EnsureInter()) return false;
    const UINT OW = 3840, OH = 2160;
    auto makeRT = [&](ComPtr<ID3D11Texture2D>& t, ComPtr<ID3D11RenderTargetView>& v, ComPtr<ID3D11Texture2D>& st) {
        D3D11_TEXTURE2D_DESC td{};
        td.Width = OW;
        td.Height = OH;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET;
        if (FAILED(dev_->CreateTexture2D(&td, nullptr, &t)) || FAILED(dev_->CreateRenderTargetView(t.Get(), nullptr, &v))) return false;
        td.Usage = D3D11_USAGE_STAGING;
        td.BindFlags = 0;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        return SUCCEEDED(dev_->CreateTexture2D(&td, nullptr, &st));
    };
    ComPtr<ID3D11Texture2D> t1, t2, s1, s2;
    ComPtr<ID3D11RenderTargetView> v1, v2;
    if (!makeRT(t1, v1, s1) || !makeRT(t2, v2, s2)) return false;
    D3D11_VIEWPORT big{0, 0, (float)OW, (float)OH, 0, 1};
    D3D11_VIEWPORT small{0, 0, (float)vw_, (float)vh_, 0, 1};
    auto timed = [&](auto&& work) {
        ComPtr<ID3D11Query> dj, q0, q1;
        D3D11_QUERY_DESC qd{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
        dev_->CreateQuery(&qd, &dj);
        qd.Query = D3D11_QUERY_TIMESTAMP;
        dev_->CreateQuery(&qd, &q0);
        dev_->CreateQuery(&qd, &q1);
        double best = 1e9;
        for (int rep = 0; rep < 8; rep++) {
            ctx_->Begin(dj.Get());
            ctx_->End(q0.Get());
            work();
            ctx_->End(q1.Get());
            ctx_->End(dj.Get());
            D3D11_QUERY_DATA_TIMESTAMP_DISJOINT d{};
            while (ctx_->GetData(dj.Get(), &d, sizeof(d), 0) != S_OK) {}
            UINT64 a = 0, b = 0;
            ctx_->GetData(q0.Get(), &a, sizeof(a), 0);
            ctx_->GetData(q1.Get(), &b, sizeof(b), 0);
            if (!d.Disjoint && d.Frequency) best = std::min(best, (b - a) * 1000.0 / d.Frequency);
        }
        return best;
    };
    msDirect = timed([&] { DrawPair(A, B, 0.5f, 1, false, big, v1.Get()); });
    msSrc = timed([&] {
        DrawPair(A, B, 0.5f, 1, false, small, interRtv_.Get());
        Upscale(inter_.srv.Get(), big, v2.Get());
    });
    ctx_->CopyResource(s1.Get(), t1.Get());
    ctx_->CopyResource(s2.Get(), t2.Get());
    D3D11_MAPPED_SUBRESOURCE m1{}, m2{};
    if (FAILED(ctx_->Map(s1.Get(), 0, D3D11_MAP_READ, 0, &m1))) return false;
    if (FAILED(ctx_->Map(s2.Get(), 0, D3D11_MAP_READ, 0, &m2))) {
        ctx_->Unmap(s1.Get(), 0);
        return false;
    }
    double sse = 0;
    for (UINT y = 0; y < OH; y++) {
        const uint8_t* a = (const uint8_t*)m1.pData + (size_t)y * m1.RowPitch;
        const uint8_t* b = (const uint8_t*)m2.pData + (size_t)y * m2.RowPitch;
        for (UINT x = 0; x < OW * 4; x++)
            if ((x & 3) != 3) {
                double d = (a[x] - b[x]) / 255.0;
                sse += d * d;
            }
    }
    ctx_->Unmap(s1.Get(), 0);
    ctx_->Unmap(s2.Get(), 0);
    psnr = 10.0 * std::log10(1.0 / std::max(sse / ((double)OW * OH * 3), 1e-12));
    BindBackbuffer();
    return true;
}

void Renderer::DrawPair(Slot* A, Slot* B, float T, UINT mode, bool showMotion, const D3D11_VIEWPORT& vp,
                        ID3D11RenderTargetView* rtv) {
    PresentCB cb{};
    cb.SrcSize[0] = (float)vw_;
    cb.SrcSize[1] = (float)vh_;
    cb.FieldUVScale[0] = (float)vw_ / (16.0f * mu_ * g2w_);
    cb.FieldUVScale[1] = (float)vh_ / (16.0f * mu_ * g2h_);
    cb.pad[0] = (float)mu_;
    cb.T = T;
    cb.Mode = mode;
    cb.ShowMotion = showMotion ? 1u : 0u;
    cb.Occl = (tuning.occlusion && B->hasDir) ? 1u : 0u;
    cb.OccTol = tuning.occTol;
    cb.OccSpan = tuning.occSpan;
    ctx_->UpdateSubresource(cbPresent_.Get(), 0, nullptr, &cb, 0, 0);
    ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx_->IASetInputLayout(nullptr);
    ctx_->VSSetShader(vs_.Get(), nullptr, 0);
    ctx_->PSSetShader(ps_.Get(), nullptr, 0);
    ID3D11ShaderResourceView* srvs[7] = {A ? A->color.srv.Get() : B->color.srv.Get(), B->color.srv.Get(), B->field.srv.Get(),
                                         B->dense.srv.Get(), B->fwd.srv.Get(), B->bwd.srv.Get(), B->still.srv.Get()};
    ctx_->PSSetShaderResources(0, 7, srvs);
    ID3D11SamplerState* smp = sampler_.Get();
    ctx_->PSSetSamplers(0, 1, &smp);
    ID3D11Buffer* cbp = cbPresent_.Get();
    ctx_->PSSetConstantBuffers(0, 1, &cbp);
    ctx_->RSSetState(nullptr);
    ctx_->RSSetViewports(1, &vp);
    ctx_->OMSetBlendState(nullptr, nullptr, 0xffffffff);
    ctx_->OMSetDepthStencilState(nullptr, 0);
    ctx_->OMSetRenderTargets(1, &rtv, nullptr);
    ctx_->Draw(3, 0);
    ID3D11ShaderResourceView* none[7] = {};
    ctx_->PSSetShaderResources(0, 7, none);
}

bool Renderer::ReadbackBmp(ID3D11Texture2D* tex, const std::wstring& path, int step) {
    D3D11_TEXTURE2D_DESC d{};
    tex->GetDesc(&d);
    d.Usage = D3D11_USAGE_STAGING;
    d.BindFlags = 0;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    d.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> st;
    if (FAILED(dev_->CreateTexture2D(&d, nullptr, &st))) return false;
    ctx_->CopyResource(st.Get(), tex);
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(ctx_->Map(st.Get(), 0, D3D11_MAP_READ, 0, &m))) return false;
    const int dw = (int)d.Width / step, dh = (int)d.Height / step;
    const uint32_t rowBytes = (uint32_t)dw * 3, pad = (4 - rowBytes % 4) % 4, imgSize = (rowBytes + pad) * dh;
    std::vector<uint8_t> out(imgSize, 0);
    for (int y = 0; y < dh; y++) {
        const uint8_t* src = (const uint8_t*)m.pData + (size_t)(y * step) * m.RowPitch;
        uint8_t* dst = &out[(size_t)(dh - 1 - y) * (rowBytes + pad)];
        for (int x = 0; x < dw; x++) {
            const uint8_t* p = src + (size_t)(x * step) * 4;  // RGBA
            dst[x * 3 + 0] = p[2];
            dst[x * 3 + 1] = p[1];
            dst[x * 3 + 2] = p[0];
        }
    }
    ctx_->Unmap(st.Get(), 0);
    FILE* fp = _wfopen(path.c_str(), L"wb");
    if (!fp) return false;
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
    fwrite(out.data(), 1, out.size(), fp);
    fclose(fp);
    return true;
}

bool Renderer::SaveInterpolationSet(const std::wstring& prefix) {
    FinishMotion();
    Slot* B = Newest();
    if (!B || !B->hasField) return false;
    Slot* A = FindSeq(B->seq - 1);
    if (!A) return false;
    D3D11_TEXTURE2D_DESC td{};
    td.Width = (UINT)vw_;
    td.Height = (UINT)vh_;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> t;
    ComPtr<ID3D11RenderTargetView> rtv;
    if (FAILED(dev_->CreateTexture2D(&td, nullptr, &t)) || FAILED(dev_->CreateRenderTargetView(t.Get(), nullptr, &rtv))) return false;
    D3D11_VIEWPORT vp{0, 0, (float)vw_, (float)vh_, 0, 1};
    bool ok = ReadbackBmp(A->color.tex.Get(), prefix + L"-1-A.bmp", 1);
    DrawPair(A, B, 0.5f, 3, false, vp, rtv.Get());
    ok &= ReadbackBmp(t.Get(), prefix + L"-2-mid-old.bmp", 1);
    DrawPair(A, B, 0.5f, 1, false, vp, rtv.Get());
    ok &= ReadbackBmp(t.Get(), prefix + L"-3-mid-new.bmp", 1);
    DrawPair(A, B, 0.5f, 1, true, vp, rtv.Get());
    ok &= ReadbackBmp(t.Get(), prefix + L"-4-motion.bmp", 2);
    ok &= ReadbackBmp(B->color.tex.Get(), prefix + L"-5-B.bmp", 1);
    BindBackbuffer();
    return ok;
}

int Renderer::SaveSequence(const std::wstring& prefix) {
    Slot* B = Newest();
    if (!B) return 0;
    // Only a run of consecutive frames with no scene cut is useful for evaluation.
    int n = 1;
    while (n < kSlots) {
        Slot* s = FindSeq(B->seq - n);
        Slot* next = FindSeq(B->seq - n + 1);
        if (!s || next->cut) break;
        n++;
    }
    if (n < 3) return 0;
    int saved = 0;
    for (int i = 0; i < n; i++) {
        Slot* s = FindSeq(B->seq - (n - 1) + i);
        wchar_t suffix[32];
        swprintf(suffix, 32, L"-f%d-%.1fms.bmp", i, s->ts - FindSeq(B->seq - (n - 1))->ts);
        if (ReadbackBmp(s->color.tex.Get(), prefix + suffix, 1)) saved++;
    }
    BindBackbuffer();
    return saved;
}

bool Renderer::ReadBackbuffer(std::vector<uint8_t>& bgr, int& w, int& h) {
    ComPtr<ID3D11Texture2D> bb, st;
    if (FAILED(sc_->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)bb.GetAddressOf()))) return false;
    D3D11_TEXTURE2D_DESC td{};
    bb->GetDesc(&td);
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    td.MiscFlags = 0;
    if (FAILED(dev_->CreateTexture2D(&td, nullptr, &st))) return false;
    ctx_->CopyResource(st.Get(), bb.Get());
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(ctx_->Map(st.Get(), 0, D3D11_MAP_READ, 0, &m))) return false;
    w = (int)td.Width;
    h = (int)td.Height;
    bgr.resize((size_t)w * h * 3);
    for (int y = 0; y < h; y++) {
        const uint8_t* src = (const uint8_t*)m.pData + (size_t)y * m.RowPitch;
        uint8_t* dst = &bgr[(size_t)y * w * 3];
        for (int x = 0; x < w; x++) {  // B8G8R8A8
            dst[x * 3] = src[x * 4];
            dst[x * 3 + 1] = src[x * 4 + 1];
            dst[x * 3 + 2] = src[x * 4 + 2];
        }
    }
    ctx_->Unmap(st.Get(), 0);
    return true;
}

UINT Renderer::Present(UINT syncInterval, bool allowTearing) {
    GpuQuery& q = queries_[queryIdx_];
    if (q.open) {
        ctx_->End(q.t1.Get());
        ctx_->End(q.disjoint.Get());
        q.open = false;
        q.pending = true;
        queryIdx_ = (queryIdx_ + 1) % 4;
    }
    UINT flags = (syncInterval == 0 && allowTearing && tearing_) ? DXGI_PRESENT_ALLOW_TEARING : 0;
    sc_->Present(syncInterval, flags);
    UINT id = 0;
    sc_->GetLastPresentCount(&id);
    return id;
}

bool Renderer::FrameStats(UINT& presentCount, double& syncMs) {
    static const double qpcToMs = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return 1000.0 / (double)f.QuadPart;
    }();
    DXGI_FRAME_STATISTICS st{};
    if (!sc_ || FAILED(sc_->GetFrameStatistics(&st)) || st.SyncQPCTime.QuadPart == 0) return false;
    presentCount = st.PresentCount;
    syncMs = (double)st.SyncQPCTime.QuadPart * qpcToMs;
    return true;
}
