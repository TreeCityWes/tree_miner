#pragma once

// Single include point for the GPU runtime API used by the miner.
//
// TreeMiner keeps one kernel/host source tree and compiles it against either the
// CUDA runtime (NVIDIA) or the HIP runtime (AMD ROCm). HIP mirrors the CUDA
// runtime API name-for-name, so the ROCm build maps the small set of `cuda*`
// symbols this codebase uses onto their `hip*` equivalents instead of forking
// every call site. Only the symbols actually used here are mapped — add a line
// when a new one is needed rather than pulling in a blanket compatibility layer.
//
// Selected by CMake: -DTREEMINER_GPU_HIP=1 for the ROCm build, nothing for CUDA.

#if defined(TREEMINER_GPU_HIP)

#include <hip/hip_runtime.h>

#define TREEMINER_GPU_BACKEND_NAME "ROCm"

// Error handling
using cudaError_t = hipError_t;
#define cudaSuccess hipSuccess
#define cudaGetErrorString hipGetErrorString
#define cudaGetLastError hipGetLastError

// Device query / selection
using cudaDeviceProp = hipDeviceProp_t;
#define cudaGetDeviceProperties hipGetDeviceProperties
#define cudaGetDeviceCount hipGetDeviceCount
#define cudaGetDevice hipGetDevice
#define cudaSetDevice hipSetDevice
#define cudaMemGetInfo hipMemGetInfo

// Memory
#define cudaMalloc hipMalloc
#define cudaFree hipFree
#define cudaMemcpy hipMemcpy
#define cudaMemcpy2DAsync hipMemcpy2DAsync
#define cudaMemcpyHostToDevice hipMemcpyHostToDevice
#define cudaMemcpyDeviceToHost hipMemcpyDeviceToHost

// Streams
using cudaStream_t = hipStream_t;
#define cudaStreamCreate hipStreamCreate
#define cudaStreamDestroy hipStreamDestroy
#define cudaStreamSynchronize hipStreamSynchronize

// Events
using cudaEvent_t = hipEvent_t;
#define cudaEventCreate hipEventCreate
#define cudaEventDestroy hipEventDestroy
#define cudaEventRecord hipEventRecord
#define cudaEventElapsedTime hipEventElapsedTime

#else

#include <cuda_runtime.h>

#define TREEMINER_GPU_BACKEND_NAME "CUDA"

#endif
