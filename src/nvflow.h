#pragma once
#include "common.h"

#include <d3d11.h>
#include <dxgi.h>

#include <map>

// NVIDIA Optical Flow (hardware motion estimation on Turing and newer), driven through CUDA with
// D3D11 interop: frames never leave the GPU. Everything is loaded dynamically from the driver
// (nvcuda.dll, nvofapi64.dll), so FrameLift still runs on AMD/Intel/older NVIDIA without it.
class NvFlow {
public:
    ~NvFlow() { Shutdown(); }
    // perfs: quality levels to prepare up front (5 = best, 10 = medium, 20 = fast). All share one
    // CUDA context and texture registrations, so switching between them per frame costs nothing.
    bool Init(IDXGIAdapter* adapter, int width, int height, std::initializer_list<int> perfs);
    void Shutdown();
    bool Ok() const { return ok_; }
    const std::string& Error() const { return error_; }
    int Width() const { return w_; }
    int Height() const { return h_; }

    // lumaA/lumaB: R8_UNORM WxH. outFwd/outBwd: R16G16_SINT (W/4)x(H/4), S10.5 vectors.
    // Forward = motion A->B measured from A; backward = motion B->A measured from B.
    bool Compute(ID3D11DeviceContext* ctx, ID3D11Texture2D* lumaA, ID3D11Texture2D* lumaB, ID3D11Texture2D* outFwd,
                 ID3D11Texture2D* outBwd, bool resetHints, int perf);
    bool Has(int perf) const;
    // After Compute the outputs stay owned by CUDA; Busy() polls, Release() hands them back to D3D11.
    bool Busy() const;
    double LastDoneMs() const;  // Compute -> first seen finished, for the last pair (-1 = not seen)
    void Release();

private:
    struct Impl;
    Impl* p_ = nullptr;
    bool ok_ = false;
    int w_ = 0, h_ = 0;
    std::string error_;
};
