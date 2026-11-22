#include "nvflow.h"

#include "log.h"

// Our minimal cuda.h (third_party/nvof) + NVIDIA's BSD-licensed Optical Flow headers.
#include "nvOpticalFlowCuda.h"

namespace {
typedef struct CUgraphicsResource_st* CUgraphicsResource;
#define CUAPI __stdcall
typedef CUresult(CUAPI* PcuInit)(unsigned);
typedef CUresult(CUAPI* PcuD3D11GetDevice)(CUdevice*, IDXGIAdapter*);
typedef CUresult(CUAPI* PcuDeviceGet)(CUdevice*, int);
typedef CUresult(CUAPI* PcuCtxCreate)(CUcontext*, unsigned, CUdevice);
typedef CUresult(CUAPI* PcuCtxDestroy)(CUcontext);
typedef CUresult(CUAPI* PcuCtxPushCurrent)(CUcontext);
typedef CUresult(CUAPI* PcuCtxPopCurrent)(CUcontext*);
typedef CUresult(CUAPI* PcuMemcpy2DAsync)(const CUDA_MEMCPY2D*, CUstream);
typedef CUresult(CUAPI* PcuStreamCreate)(CUstream*, unsigned);
typedef CUresult(CUAPI* PcuStreamDestroy)(CUstream);
typedef CUresult(CUAPI* PcuRegister)(CUgraphicsResource*, ID3D11Resource*, unsigned);
typedef CUresult(CUAPI* PcuUnregister)(CUgraphicsResource);
typedef CUresult(CUAPI* PcuMap)(unsigned, CUgraphicsResource*, CUstream);
typedef CUresult(CUAPI* PcuStreamQuery)(CUstream);
typedef CUresult(CUAPI* PcuGetArray)(CUarray*, CUgraphicsResource, unsigned, unsigned);
typedef NV_OF_STATUS(NVOFAPI* PCreateOF)(uint32_t, NV_OF_CUDA_API_FUNCTION_LIST*);
}  // namespace

struct NvFlow::Impl {
    HMODULE cu = nullptr, of = nullptr;
    PcuInit cuInit;
    PcuD3D11GetDevice cuD3D11GetDevice;
    PcuDeviceGet cuDeviceGet;
    PcuCtxCreate cuCtxCreate;
    PcuCtxDestroy cuCtxDestroy;
    PcuCtxPushCurrent cuCtxPush;
    PcuCtxPopCurrent cuCtxPop;
    PcuMemcpy2DAsync cuMemcpy2DAsync;
    PcuStreamCreate cuStreamCreate;
    PcuStreamDestroy cuStreamDestroy;
    PcuRegister cuRegister;
    PcuUnregister cuUnregister;
    PcuMap cuMap, cuUnmap;
    PcuGetArray cuGetArray;
    PcuStreamQuery cuStreamQuery;
    CUgraphicsResource mapped[4] = {};
    bool isMapped = false;
    bool queryErrLogged = false;
    double startMs = 0, doneMs = -1;  // outputs still owned by CUDA (see Release)
    NV_OF_CUDA_API_FUNCTION_LIST fl{};
    CUcontext ctx = nullptr;
    CUstream stream = nullptr;  // all work is queued here; the CPU doesn't wait for it
    struct Dir {
        NvOFHandle h = nullptr;
        NvOFGPUBufferHandle in = nullptr, ref = nullptr, out = nullptr;
    };
    // One engine per direction so each keeps its own history of vectors as temporal hints;
    // one pair of engines per quality level, all sharing the context and registrations.
    struct Level {
        Dir fwd, bwd;
    };
    std::map<int, Level> levels;  // by perf (5 = best, 10 = medium, 20 = fast)
    std::map<ID3D11Resource*, CUgraphicsResource> reg;
};

#define NVF_LOAD(lib, var, type, name)           \
    var = (type)GetProcAddress(lib, name);       \
    if (!var) {                                  \
        error_ = std::string("missing ") + name; \
        return false;                            \
    }

bool NvFlow::Has(int perf) const { return ok_ && p_ && p_->levels.count(perf) != 0; }

bool NvFlow::Init(IDXGIAdapter* adapter, int width, int height, std::initializer_list<int> perfs) {
    Shutdown();
    error_.clear();
    p_ = new Impl();
    Impl& P = *p_;
    P.cu = LoadLibraryA("nvcuda.dll");
    P.of = LoadLibraryA("nvofapi64.dll");
    if (!P.cu || !P.of) {
        error_ = "NVIDIA Optical Flow not available (driver libraries missing)";
        return false;
    }
    NVF_LOAD(P.cu, P.cuInit, PcuInit, "cuInit");
    NVF_LOAD(P.cu, P.cuD3D11GetDevice, PcuD3D11GetDevice, "cuD3D11GetDevice");
    NVF_LOAD(P.cu, P.cuDeviceGet, PcuDeviceGet, "cuDeviceGet");
    NVF_LOAD(P.cu, P.cuCtxCreate, PcuCtxCreate, "cuCtxCreate_v2");
    NVF_LOAD(P.cu, P.cuCtxDestroy, PcuCtxDestroy, "cuCtxDestroy_v2");
    NVF_LOAD(P.cu, P.cuCtxPush, PcuCtxPushCurrent, "cuCtxPushCurrent_v2");
    NVF_LOAD(P.cu, P.cuCtxPop, PcuCtxPopCurrent, "cuCtxPopCurrent_v2");
    NVF_LOAD(P.cu, P.cuMemcpy2DAsync, PcuMemcpy2DAsync, "cuMemcpy2DAsync_v2");
    NVF_LOAD(P.cu, P.cuStreamCreate, PcuStreamCreate, "cuStreamCreate");
    NVF_LOAD(P.cu, P.cuStreamDestroy, PcuStreamDestroy, "cuStreamDestroy_v2");
    NVF_LOAD(P.cu, P.cuRegister, PcuRegister, "cuGraphicsD3D11RegisterResource");
    NVF_LOAD(P.cu, P.cuUnregister, PcuUnregister, "cuGraphicsUnregisterResource");
    NVF_LOAD(P.cu, P.cuMap, PcuMap, "cuGraphicsMapResources");
    NVF_LOAD(P.cu, P.cuUnmap, PcuMap, "cuGraphicsUnmapResources");
    NVF_LOAD(P.cu, P.cuGetArray, PcuGetArray, "cuGraphicsSubResourceGetMappedArray");
    NVF_LOAD(P.cu, P.cuStreamQuery, PcuStreamQuery, "cuStreamQuery");
    auto create = (PCreateOF)GetProcAddress(P.of, "NvOFAPICreateInstanceCuda");
    if (!create) {
        error_ = "missing NvOFAPICreateInstanceCuda";
        return false;
    }
    CUdevice dev = 0;
    CUresult r0 = P.cuInit(0);
    CUresult r1 = r0 ? r0 : P.cuD3D11GetDevice(&dev, adapter);
    if (r1 && !r0) {
        // Matching by adapter can fail in some sessions; with a single NVIDIA GPU, device 0 is it.
        Log("nvflow: cuD3D11GetDevice failed (%d), using CUDA device 0", r1);
        if (P.cuDeviceGet(&dev, 0) == 0) r1 = 0;
    }
    CUresult r2 = r1 ? r1 : P.cuCtxCreate(&P.ctx, 0, dev);
    if (r0 || r1 || r2) {
        char b[96];
        snprintf(b, sizeof(b), "CUDA context failed (init %d, device %d, context %d)", r0, r1, r2);
        error_ = b;
        return false;
    }
    if (P.cuStreamCreate(&P.stream, 1 /* non-blocking */)) {
        error_ = "CUDA stream failed";
        return false;
    }
    if (create(NV_OF_API_VERSION, &P.fl)) {
        error_ = "Optical Flow API unavailable";
        return false;
    }
    for (int perf : perfs) {
        Impl::Level& lv = P.levels[perf];
        for (Impl::Dir* d : {&lv.fwd, &lv.bwd}) {
            if (P.fl.nvCreateOpticalFlowCuda(P.ctx, &d->h)) {
                error_ = "this GPU has no Optical Flow engine";
                return false;
            }
            NV_OF_INIT_PARAMS ip{};
            ip.width = (uint32_t)width;
            ip.height = (uint32_t)height;
            ip.outGridSize = NV_OF_OUTPUT_VECTOR_GRID_SIZE_4;
            ip.hintGridSize = NV_OF_HINT_VECTOR_GRID_SIZE_UNDEFINED;
            ip.mode = NV_OF_MODE_OPTICALFLOW;
            ip.perfLevel = (NV_OF_PERF_LEVEL)perf;
            if (P.fl.nvOFInit(d->h, &ip)) {
                error_ = "Optical Flow init failed";
                return false;
            }
            P.fl.nvOFSetIOCudaStreams(d->h, P.stream, P.stream);
            NV_OF_BUFFER_DESCRIPTOR in{(uint32_t)width, (uint32_t)height, NV_OF_BUFFER_USAGE_INPUT, NV_OF_BUFFER_FORMAT_GRAYSCALE8};
            NV_OF_BUFFER_DESCRIPTOR out{(uint32_t)width / 4, (uint32_t)height / 4, NV_OF_BUFFER_USAGE_OUTPUT, NV_OF_BUFFER_FORMAT_SHORT2};
            if (P.fl.nvOFCreateGPUBufferCuda(d->h, &in, NV_OF_CUDA_BUFFER_TYPE_CUDEVICEPTR, &d->in) ||
                P.fl.nvOFCreateGPUBufferCuda(d->h, &in, NV_OF_CUDA_BUFFER_TYPE_CUDEVICEPTR, &d->ref) ||
                P.fl.nvOFCreateGPUBufferCuda(d->h, &out, NV_OF_CUDA_BUFFER_TYPE_CUDEVICEPTR, &d->out)) {
                error_ = "Optical Flow buffers failed";
                return false;
            }
        }
    }
    w_ = width;
    h_ = height;
    ok_ = true;
    Log("nvflow: NVIDIA Optical Flow ready (%dx%d, %zu quality levels)", width, height, P.levels.size());
    return true;
}

double NvFlow::LastDoneMs() const { return p_ ? p_->doneMs : -1; }

bool NvFlow::Busy() const {
    if (!p_ || !p_->isMapped) return false;
    p_->cuCtxPush(p_->ctx);
    const CUresult q = p_->cuStreamQuery(p_->stream);
    CUcontext dummy;
    p_->cuCtxPop(&dummy);
    if (q != 0 && q != 600 && !p_->queryErrLogged) {  // 600 = CUDA_ERROR_NOT_READY
        p_->queryErrLogged = true;
        Log("nvflow: cuStreamQuery returned %d", (int)q);
    }
    if (q == 0 && p_->doneMs < 0) p_->doneMs = NowMs() - p_->startMs;
    return q == 600;
}

void NvFlow::Release() {
    if (!p_ || !p_->isMapped) return;
    Impl& P = *p_;
    P.cuCtxPush(P.ctx);
    P.cuUnmap(4, P.mapped, P.stream);  // later D3D work waits (on the GPU) for the stream
    CUcontext dummy;
    P.cuCtxPop(&dummy);
    P.isMapped = false;
}

void NvFlow::Shutdown() {
    if (!p_) return;
    Release();
    Impl& P = *p_;
    if (P.ctx) {
        P.cuCtxPush(P.ctx);
        for (auto& r : P.reg) P.cuUnregister(r.second);
        for (auto& lv : P.levels)
            for (Impl::Dir* d : {&lv.second.fwd, &lv.second.bwd}) {
                if (d->in) P.fl.nvOFDestroyGPUBufferCuda(d->in);
                if (d->ref) P.fl.nvOFDestroyGPUBufferCuda(d->ref);
                if (d->out) P.fl.nvOFDestroyGPUBufferCuda(d->out);
                if (d->h) P.fl.nvOFDestroy(d->h);
            }
        if (P.stream) P.cuStreamDestroy(P.stream);
        CUcontext dummy;
        P.cuCtxPop(&dummy);
        P.cuCtxDestroy(P.ctx);
    }
    delete p_;
    p_ = nullptr;
    ok_ = false;
}

bool NvFlow::Compute(ID3D11DeviceContext* ctx, ID3D11Texture2D* lumaA, ID3D11Texture2D* lumaB, ID3D11Texture2D* outFwd,
                     ID3D11Texture2D* outBwd, bool resetHints, int perf) {
    if (!ok_ || !p_->levels.count(perf)) return false;
    Impl& P = *p_;
    Impl::Level& L = P.levels[perf];
    P.cuCtxPush(P.ctx);
    char why[128] = "";
    auto regOf = [&](ID3D11Texture2D* t) -> CUgraphicsResource {
        auto it = P.reg.find(t);
        if (it != P.reg.end()) return it->second;
        CUgraphicsResource r = nullptr;
        if (CUresult e = P.cuRegister(&r, t, 0)) {
            snprintf(why, sizeof(why), "register texture: %d", e);
            return nullptr;
        }
        P.reg[t] = r;
        return r;
    };
    if (P.isMapped) {
        CUcontext d0;
        P.cuCtxPop(&d0);
        Release();
        P.cuCtxPush(P.ctx);
    }
    CUgraphicsResource res[4] = {regOf(lumaA), regOf(lumaB), regOf(outFwd), regOf(outBwd)};
    bool ok = res[0] && res[1] && res[2] && res[3];
    ctx->Flush();  // D3D work that produced the luma must be submitted before CUDA reads it
    if (ok)
        if (CUresult e = P.cuMap(4, res, P.stream)) {
            snprintf(why, sizeof(why), "map: %d", e);
            ok = false;
        }
    if (ok) {
        CUarray arr[4] = {};
        for (int i = 0; i < 4; i++)
            if (CUresult e = P.cuGetArray(&arr[i], res[i], 0, 0)) {
                snprintf(why, sizeof(why), "get array %d: %d", i, e);
                ok = false;
            }
        auto toBuf = [&](CUarray src, NvOFGPUBufferHandle dst) {
            NV_OF_CUDA_BUFFER_STRIDE_INFO si{};
            P.fl.nvOFGPUBufferGetStrideInfo(dst, &si);
            CUDA_MEMCPY2D c{};
            c.srcMemoryType = CU_MEMORYTYPE_ARRAY;
            c.srcArray = src;
            c.dstMemoryType = CU_MEMORYTYPE_DEVICE;
            c.dstDevice = P.fl.nvOFGPUBufferGetCUdeviceptr(dst);
            c.dstPitch = si.strideInfo[0].strideXInBytes;
            c.WidthInBytes = (size_t)w_;
            c.Height = (size_t)h_;
            CUresult e = P.cuMemcpy2DAsync(&c, P.stream);
            if (e) snprintf(why, sizeof(why), "copy luma in: %d", e);
            return e == 0;
        };
        auto fromBuf = [&](NvOFGPUBufferHandle src, CUarray dst) {
            NV_OF_CUDA_BUFFER_STRIDE_INFO si{};
            P.fl.nvOFGPUBufferGetStrideInfo(src, &si);
            CUDA_MEMCPY2D c{};
            c.srcMemoryType = CU_MEMORYTYPE_DEVICE;
            c.srcDevice = P.fl.nvOFGPUBufferGetCUdeviceptr(src);
            c.srcPitch = si.strideInfo[0].strideXInBytes;
            c.dstMemoryType = CU_MEMORYTYPE_ARRAY;
            c.dstArray = dst;
            c.WidthInBytes = (size_t)(w_ / 4) * 4;
            c.Height = (size_t)(h_ / 4);
            CUresult e = P.cuMemcpy2DAsync(&c, P.stream);
            if (e) snprintf(why, sizeof(why), "copy flow out: %d", e);
            return e == 0;
        };
        auto run = [&](Impl::Dir& d, CUarray in, CUarray ref, CUarray out) {
            if (!toBuf(in, d.in) || !toBuf(ref, d.ref)) return false;
            NV_OF_EXECUTE_INPUT_PARAMS ei{};
            ei.inputFrame = d.in;
            ei.referenceFrame = d.ref;
            ei.disableTemporalHints = resetHints ? NV_OF_TRUE : NV_OF_FALSE;
            NV_OF_EXECUTE_OUTPUT_PARAMS eo{};
            eo.outputBuffer = d.out;
            if (NV_OF_STATUS e = P.fl.nvOFExecute(d.h, &ei, &eo)) {
                snprintf(why, sizeof(why), "execute: %d", (int)e);
                return false;
            }
            return fromBuf(d.out, out);
        };
        if (ok) ok = run(L.fwd, arr[0], arr[1], arr[2]) && run(L.bwd, arr[1], arr[0], arr[3]);
        // Stay mapped: unmapping now would make ALL later D3D11 work (the next presents included)
        // wait for this stream. Release() unmaps once Busy() is false, or when the outputs are needed.
        if (ok) {
            for (int i = 0; i < 4; i++) P.mapped[i] = res[i];
            P.isMapped = true;
            P.startMs = NowMs();
            P.doneMs = -1;
        } else {
            P.cuUnmap(4, res, P.stream);
        }
    }
    CUcontext dummy;
    P.cuCtxPop(&dummy);
    if (!ok) error_ = std::string("Optical Flow compute failed: ") + why;
    return ok;
}
