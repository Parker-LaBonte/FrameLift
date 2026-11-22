// Minimal stand-in for NVIDIA's cuda.h: only the handle types nvOpticalFlowCuda.h needs, plus the
// few driver-API declarations FrameLift loads dynamically from nvcuda.dll (no CUDA toolkit needed).
#pragma once
#include <stddef.h>
typedef int CUresult;
typedef int CUdevice;
typedef struct CUctx_st* CUcontext;
typedef struct CUstream_st* CUstream;
typedef struct CUarray_st* CUarray;
typedef unsigned long long CUdeviceptr;
typedef enum { CU_MEMORYTYPE_HOST = 1, CU_MEMORYTYPE_DEVICE = 2, CU_MEMORYTYPE_ARRAY = 3 } CUmemorytype;
typedef struct CUDA_MEMCPY2D_st {
    size_t srcXInBytes, srcY;
    CUmemorytype srcMemoryType;
    const void* srcHost;
    CUdeviceptr srcDevice;
    CUarray srcArray;
    size_t srcPitch;
    size_t dstXInBytes, dstY;
    CUmemorytype dstMemoryType;
    void* dstHost;
    CUdeviceptr dstDevice;
    CUarray dstArray;
    size_t dstPitch;
    size_t WidthInBytes, Height;
} CUDA_MEMCPY2D;
