#pragma once
#include "capture.h"
#include "common.h"

#include <d3d11_1.h>
#include <dxgi1_6.h>

#include <algorithm>

#include "nvflow.h"

// Frame-generation settings. Defaults are the shipped, tuned values; --bench sweeps them.
// 2026-11-22: lambda 0.5 -> 0.25 and radius16 8 -> 6 scored +0.18 dB (all) / +0.21 dB (moving) on
// 11 true-60fps PS5 clips (319 midpoints), +0.11 dB at 2x motion, and -7% match error on the HD60 S
// spin captures (130+ px/frame motion is still tracked via the previous frame's vectors).
struct Tuning {
    float lambda = 0.25f;     // block-search smoothness penalty
    int radius16 = 6;         // coarsest (1/16) full-search radius => +-16*radius px
    int local = 2;            // small-object search radius at 1/4 and 1/2 (0 = off)
    float localPen = 1.0f;    // size penalty multiplier for small-object candidates
    float refineBias = 0.6f;  // per-pixel refine: penalty for non-blended block candidates
    float zeroBias = 0.6f;    // per-pixel refine: penalty for the zero-motion candidate
    bool denseMedian = true;  // 3x3 median on per-pixel vectors
    bool occlusion = false;   // two-way motion + occlusion-aware blend
    float occTol = 2.0f;      // px of disagreement tolerated before a side is distrusted
    float occSpan = 6.0f;     // px over which trust fades to zero
    bool blendOnly = false;   // bench baseline: plain crossfade, no motion
    int matchHP = 0;          // >0: match on luma minus its local mean (radius matchHP), lighting-robust
    int engine = 0;           // 0 = block search (any GPU), 1 = NVIDIA Optical Flow hardware (falls back to 0)
    int nvPerf = 5;           // NVIDIA OF quality: 5 best, 10 medium, 20 fast
    bool nvHybrid = false;    // also offer the block search's vector as a per-pixel candidate
    float nvBias = 0.3f;      // per-pixel candidate penalty for non-forward candidates
    bool nvBilinear = false;  // interpolate the 4x4 flow grid instead of nearest cell
    int motionScale = 0;       // motion analysis base: 0 = auto (4K analysed like 1080p), else full/(2*n) for the 1/2 level
    int scaler = 1;            // output scaling: 0 = bilinear, 1 = sharp (Lanczos + adaptive sharpening)
    float sharpness = 0.5f;    // 0..1, sharp scaler only
    bool srcResDraw = true;   // generate in-between frames at source resolution, then scale up once
    int nvScale = 1;          // run the hardware on a 1/nvScale image (2 = half size: ~4x less work)
    float staticThr = 2.0f / 255.0f;  // static guard: max per-channel change (3x3) to count as static; <0 = off
};

struct DrawResult {
    bool any = false;
    bool interpolated = false;
    double contentTs = 0;  // capture-clock time of the moment being shown
    bool flowLate = false;  // the pair needed now was still being analysed; showed the older frame
};

// D3D11 device + flip-model swap chain + the frame generation pipeline.
class Renderer {
public:
    bool Init(HWND hwnd);
    void Shutdown();
    void Resize(UINT w, UINT h);

    ID3D11Device* Device() const { return dev_.Get(); }
    ID3D11DeviceContext* Ctx() const { return ctx_.Get(); }
    LUID AdapterLuid() const { return luid_; }
    bool IsNvidia() const { return vendor_ == 0x10DE; }
    const std::string& AdapterName() const { return adapterName_; }
    HANDLE Waitable() const { return waitable_; }
    double RefreshHz();

    // Adds one unique source frame to the timeline; runs motion estimation against the previous one.
    void Upload(const Frame& f, bool runMotion, double maxGapMs);
    // Queues the deferred post-processing of hardware flow (call right after Present).
    void FinishMotion(bool force = true);
    void KickMotion();  // starts motion for the newest pair (call right after Present)
    void ResetTimeline();
    bool HasVideo() const;
    double NewestTs() const;
    int VideoWidth() const { return vw_; }
    int VideoHeight() const { return vh_; }

    void BeginGpuTimer();
    void BeginDrawTimer();
    void EndDrawTimer();
    float DrawMsPeak() const { return drawMsMax_; }  // recent peak GPU time of the final draw
    void Flush() { if (ctx_) ctx_->Flush(); }
    void BeginFrame();
    // displayTime is on the capture clock. interpolate=false shows the newest frame.
    DrawResult DrawVideo(double displayTime, bool interpolate, const D3D11_VIEWPORT& vp, bool showMotion);
    void BindBackbuffer();
    UINT Present(UINT syncInterval, bool allowTearing);
    bool ReadBackbuffer(std::vector<uint8_t>& bgr, int& w, int& h);  // UI snapshots  // returns the present id
    // Last present that actually reached the screen, and the vblank time it did so (ms, QPC clock).
    bool FrameStats(UINT& presentCount, double& syncMs);
    float GpuMs() const { return gpuMs_; }
    Tuning tuning;
    float NvMs() const { return nvMs_; }               // main-thread time spent in the hardware flow step
    bool NvActive() const { return nv_.Ok() && tuning.engine == 1; }
    const char* EngineName() const { return NvActive() ? "NVIDIA Optical Flow" : "Shader (any GPU)"; }
    // Headless init for --bench: device only, no window/swap chain.
    bool InitHeadless();
    // --bench: render the newest pair's midpoint (T) offscreen and read it back as RGBA.
    bool RenderNewestMid(std::vector<uint8_t>& rgba, float T, UINT modeOverride = 0);
    // --bench drawcheck: GPU ms of the final draw at 4K, direct vs. source-res + scale-up, and the
    // PSNR between the two images (they should be visually identical).
    bool DrawCheck(double& msDirect, double& msSrc, double& psnr);
    // Scaler check on the newest frame: shrink to 2/3, enlarge back with each scaler, PSNR vs the
    // original; plus GPU cost of 1080p->4K and ->1440p. out: psnrBil, psnrSharp0, psnrSharp50, msBil4K, msSharp4K, msSharp1440
    bool ScaleCheck(double out[6]);
    // Diagnostics: saves the GPU-converted newest frame (1/4 size) as a 24-bit BMP.
    bool SaveNewestColorBmp(const std::wstring& path);
    // Diagnostics: for the newest pair saves A, B, old-method mid frame, new-method mid frame, motion map.
    bool SaveInterpolationSet(const std::wstring& dirAndPrefix);
    // Diagnostics: saves every held frame (up to 4 consecutive real frames) at full res, oldest first.
    int SaveSequence(const std::wstring& dirAndPrefix);

private:
    struct Tex {
        ComPtr<ID3D11Texture2D> tex;
        ComPtr<ID3D11ShaderResourceView> srv;
        ComPtr<ID3D11UnorderedAccessView> uav;
        UINT w = 0, h = 0;
    };
    struct Slot {
        Tex color, l2, l4, l8, l16, field, dense, fwd, bwd;
        Tex h2, h4, h8, h16;  // high-passed luma pyramid (when tuning.matchHP)
        Tex luma8, nvF, nvB;  // NVIDIA OF: full-res luma in; 4x4-grid forward/backward vectors out
        Tex still;            // static guard mask for the pair ending at this frame
        bool hasDir = false;
        double ts = 0;
        uint64_t seq = 0;
        bool valid = false, hasField = false, cut = false;
    };
    static constexpr int kSlots = 4;

    bool MakeTex(UINT w, UINT h, DXGI_FORMAT fmt, Tex& t);
    bool CreateVideoResources(int w, int h);
    void CreateRTV();
    void RunCS(ID3D11ComputeShader* cs, std::initializer_list<ID3D11ShaderResourceView*> srvs,
               ID3D11UnorderedAccessView* uav, ID3D11Buffer* cb, const void* data, UINT gx, UINT gy);
    void Motion(Slot& a, Slot& b, Slot* temporal);
    void MotionBlocks(Slot& a, Slot& b, Slot* temporal);
    bool MotionNv(Slot& a, Slot& b, Slot* temporal);
    Slot *kickA_ = nullptr, *kickB_ = nullptr;  // pair waiting for motion to start
    Slot *pendA_ = nullptr, *pendB_ = nullptr;
public:
    float nvDoneMax_ = 0;  // diagnostics, reset by the stats log
    int nvForced_ = 0;
private:  // NV pair whose flow conversion is deferred
    Slot* FindSeq(uint64_t seq);
    Slot* Newest();
    void PollGpuTimers();

    ComPtr<ID3D11Device> dev_;
    ComPtr<ID3D11DeviceContext> ctx_;
    ComPtr<IDXGISwapChain2> sc_;
    ComPtr<ID3D11RenderTargetView> rtv_;
    HANDLE waitable_ = nullptr;
    bool tearing_ = false;
    UINT scFlags_ = 0;
    LUID luid_{};
    UINT vendor_ = 0;
    int mu_ = 1;  // motion vectors are stored in units of mu_ full-res px; the pyramid starts at 1/(2*mu_)
    std::string adapterName_;
    double refreshHz_ = 60;
    double refreshCheckedMs_ = -1e9;

    ComPtr<ID3D11ComputeShader> csConvert_, csLumaHalf_, csDown_, csSearch_, csMedian_, csRefine_, csHighPass_;
    ComPtr<ID3D11ComputeShader> csLuma8_, csNvDense_, csNvGrid_, csStatic_;
    ComPtr<ID3D11Buffer> cbStatic_;
    ComPtr<ID3D11Buffer> cbLuma8_, cbNvDense_, cbNvGrid_;
    NvFlow nv_;
    bool nvTried_ = false;
    int nvScaleActive_ = 0, nvLastPerf_ = 0;
    float nvMs_ = 0;
    ComPtr<ID3D11VertexShader> vs_;
    ComPtr<ID3D11PixelShader> ps_, psUp_, psLanczos_, psRcas_;
    ComPtr<ID3D11Buffer> cbScale_;
    Tex scaleTmp_;  // output-size intermediate for the sharp scaler
    ComPtr<ID3D11RenderTargetView> scaleTmpRtv_;
    Tex inter_;  // source-resolution in-between frame (scaled up to the output afterwards)
    ComPtr<ID3D11RenderTargetView> interRtv_;
    // NV motion input scale: explicit, or auto (0) so the analysed image stays about 960x540
    // whatever the capture size (1080p -> 1/2, 1440p -> 1/3, 4K -> 1/4): same cost and, per the
    // bench, nearly the same quality as full size.
    int MotionUnit() const { return tuning.motionScale > 0 ? tuning.motionScale : (vh_ >= 2000 ? 2 : 1); }
    int NvScale() const { return tuning.nvScale > 0 ? tuning.nvScale : std::clamp((vh_ + 270) / 540, 1, 4); }
    bool EnsureInter();
    bool ScaleSharp(ID3D11ShaderResourceView* src, const D3D11_VIEWPORT& vp, ID3D11RenderTargetView* rtv, int sw = 0, int sh = 0,
                    float sharpness = -1);
    void Upscale(ID3D11ShaderResourceView* src, const D3D11_VIEWPORT& vp, ID3D11RenderTargetView* rtv);
    ComPtr<ID3D11SamplerState> sampler_;
    ComPtr<ID3D11Buffer> cbConvert_, cbDown_, cbSearch_, cbMedian_, cbPresent_, cbRefine_, cbHighPass_;

    ComPtr<ID3D11Texture2D> srcTex_;
    ComPtr<ID3D11ShaderResourceView> srcSrv_;
    UINT srcW_ = 0, srcH_ = 0;
    DXGI_FORMAT srcFmt_ = DXGI_FORMAT_UNKNOWN;

    int vw_ = 0, vh_ = 0;
    UINT l2w_ = 0, l2h_ = 0, l4w_ = 0, l4h_ = 0, l8w_ = 0, l8h_ = 0, l16w_ = 0, l16h_ = 0;
    UINT g2w_ = 0, g2h_ = 0, g4w_ = 0, g4h_ = 0, g8w_ = 0, g8h_ = 0, g16w_ = 0, g16h_ = 0;
    Slot slots_[kSlots];
    Tex f16_, f8_, f4_, f2a_, f2b_, denseTmp_;
    ComPtr<ID3D11Texture2D> evalTex_, evalStage_;
    ComPtr<ID3D11RenderTargetView> evalRtv_;
    bool CreatePipeline();  // shaders, samplers, constant buffers, queries (shared by both inits)
    // Draws pair (A,B) at time T with the given mode into rtv/viewport (shared by screen + diagnostics).
    void DrawPair(Slot* A, Slot* B, float T, UINT mode, bool showMotion, const D3D11_VIEWPORT& vp, ID3D11RenderTargetView* rtv);
    bool ReadbackBmp(ID3D11Texture2D* tex, const std::wstring& path, int step);
    uint64_t seq_ = 0;

    struct GpuQuery {
        ComPtr<ID3D11Query> disjoint, t0, t1;
        bool pending = false, open = false;
    };
    GpuQuery queries_[4];
    int queryIdx_ = 0;
    float gpuMs_ = 0;
    GpuQuery drawQ_[4];
    int drawIdx_ = 0;
    float drawMsMax_ = 4.0f;
    void PollDrawTimers();
};
