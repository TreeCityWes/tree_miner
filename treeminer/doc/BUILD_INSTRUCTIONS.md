# Build Instructions

This document provides instructions for building the project on both Linux and Windows systems using vcpkg.

## Prerequisites

- CMake installed on your system (3.21+ for the AMD/ROCm backend).
- A GPU toolchain for the vendor you are building for:
  - NVIDIA: CUDA Toolkit (default backend).
  - AMD: ROCm with HIP (`hipcc`), selected with `-DTREEMINER_GPU_BACKEND=HIP`.

## Building on Linux

The default build detects the CUDA architecture of the GPU visible during CMake
configuration. For example, RTX 3060 cards resolve to `sm_86`, while RTX 50-series
cards resolve to their Blackwell architecture. Use a fresh build directory when
moving the source to a different GPU generation; CMake caches the detected value.

Execute the following commands in your terminal:

```bash
sudo apt install build-essential tar curl zip unzip git cmake ninja-build
git clone https://github.com/TreeCityWes/tree_miner.git
git clone https://github.com/microsoft/vcpkg.git "$HOME/vcpkg"
"$HOME/vcpkg/bootstrap-vcpkg.sh" -disableMetrics
cd tree_miner/treeminer
cmake -S . -B build-native -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE="$HOME/vcpkg/scripts/buildsystems/vcpkg.cmake" \
  -DVCPKG_OVERLAY_TRIPLETS="$PWD/custom-triplets" \
  -DVCPKG_TARGET_TRIPLET=x64-linux-static
cmake --build build-native --parallel "$(nproc)"
ctest --test-dir build-native --output-on-failure
```

The configure output reports the concrete detected target. On a machine with
one or more RTX 3060 cards it should read:

```text
-- TreeMiner CUDA architectures: 86
```

The generated CUDA compile command will likewise contain `compute_86` /
`sm_86`. Mixed-generation machines include each unique detected capability.

Override detection only when building for a GPU that is not installed locally:

```bash
cmake -S . -B build-sm86 -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE="$HOME/vcpkg/scripts/buildsystems/vcpkg.cmake" \
  -DVCPKG_OVERLAY_TRIPLETS="$PWD/custom-triplets" \
  -DVCPKG_TARGET_TRIPLET=x64-linux-static \
  -DCMAKE_CUDA_ARCHITECTURES=86
```

For repeatable Hash API/CUDA benchmark runs, prefer a fresh Release CUDA preset instead of reusing an old local build directory:

```bash
cmake --preset cuda-release-vcpkg-modern
cmake --build --preset cuda-release-vcpkg-modern
```

The `cuda-release-vcpkg-modern` preset uses CUDA Toolkit 12.8 and builds a fat binary with native code for `50;52;60;61;70;75;80;86;87;89;90;120`. This covers supported NVIDIA generations from Maxwell through Blackwell, including RTX 20 (`sm_75`), RTX 30 (`sm_86`), RTX 40 series (`sm_89`), and RTX 50 series (`sm_120`). Older targets use CMake's `-real` form so they carry native SASS only; the final `120` target also carries PTX for forward compatibility. A fat binary is larger and takes longer to compile, but each listed GPU runs native code at full mining performance.

RTX 50 series support requires CUDA Toolkit 12.8 or newer. Older toolkits cannot generate `sm_120` code. The NVIDIA display driver must also be new enough for the installed toolkit.

If you want a smaller binary and faster compilation for one CUDA architecture, use the matching preset:

```bash
cmake --preset cuda-release-vcpkg-sm86
cmake --build --preset cuda-release-vcpkg-sm86
```

For RTX 40 or RTX 50 series respectively:

```bash
cmake --preset cuda-release-vcpkg-sm89
cmake --build --preset cuda-release-vcpkg-sm89

cmake --preset cuda-release-vcpkg-sm120
cmake --build --preset cuda-release-vcpkg-sm120
```

Or override the architecture explicitly:

```bash
cmake -S . -B build --preset ninja-multi-vcpkg -DCMAKE_CUDA_ARCHITECTURES=86
cmake --build build --preset ninja-vcpkg-release
```

## Building for AMD GPUs (ROCm)

The miner has one kernel source that compiles either with `nvcc` (NVIDIA, the default) or
with ROCm's HIP compiler (AMD). Nothing about the NVIDIA build changes — the AMD backend is
opt-in through `-DTREEMINER_GPU_BACKEND=HIP` (`ROCm` is accepted as a synonym).

Requirements: ROCm 5.7 or newer with the HIP runtime and, optionally, `rocm-smi` for the
power/utilization gauges on the dashboard. The GPU must be one ROCm supports.

```bash
cd tree_miner/treeminer
cmake -S . -B build-rocm -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DTREEMINER_GPU_BACKEND=HIP \
  -DCMAKE_TOOLCHAIN_FILE="$HOME/vcpkg/scripts/buildsystems/vcpkg.cmake" \
  -DVCPKG_OVERLAY_TRIPLETS="$PWD/custom-triplets" \
  -DVCPKG_TARGET_TRIPLET=x64-linux-static
cmake --build build-rocm --parallel "$(nproc)"
```

The gfx target is detected from the installed ROCm tools (`amdgpu-arch`, falling back to
`rocminfo`) and reported at configure time:

```text
-- TreeMiner HIP architectures: gfx1030
```

With no AMD GPU visible at configure time — CI, containers — the build falls back to a fat
binary covering `gfx900 gfx906 gfx908 gfx90a gfx942 gfx1010 gfx1030 gfx1031 gfx1100 gfx1101
gfx1102 gfx1200 gfx1201`. Override either case with `-DCMAKE_HIP_ARCHITECTURES=gfx1100`.

Two presets wrap this:

```bash
cmake --preset rocm-release-linux-native   # detect the local card
cmake --preset rocm-release-linux-fat      # every supported gfx target
```

### What differs from the NVIDIA build

- **Warp width.** An Argon2 lane is 32 threads on both vendors. gfx9/CDNA wavefronts are 64
  lanes wide, so every shuffle in the kernel is pinned to a 32-lane width explicitly
  (`TM_SHFL` / `TM_SHFL_XOR` in `src/kernelrunner.cu`).
- **The Argon2 G function.** NVIDIA uses a hand-written PTX block; PTX cannot be assembled by
  the AMD compiler, so the ROCm build uses the equivalent C++ form (`g1()`).
- **First blocks stay on the CPU.** `kGpuFirstBlocksEnabled` is `false` on ROCm until the GPU
  first-blocks kernel is confirmed against the CPU reference on real AMD hardware. The
  startup self-test still probes the GPU path and prints whether it matched, which is the
  signal for flipping it on.
- **Telemetry.** Power and utilization come from ROCm SMI instead of NVML. If the ROCm SMI
  library is absent the miner still runs and those gauges simply report unavailable.

Both backends run the same startup Argon2 CPU/GPU self-test and refuse to mine on a device
whose digests do not match the CPU reference, so a bad toolchain fails closed at launch
rather than submitting invalid blocks.

## Building on Windows

Execute the following commands in Command Prompt or PowerShell:

```bash
.\vcpkg.exe install argon2:x64-windows-static
.\vcpkg.exe install cryptopp:x64-windows-static
.\vcpkg.exe install cpr:x64-windows-static
.\vcpkg.exe install nlohmann-json:x64-windows-static
.\vcpkg.exe install openssl:x64-windows-static
.\vcpkg.exe install boost-program-options:x64-windows-static
.\vcpkg.exe install secp256k1:x64-windows-static
.\vcpkg.exe install crow:x64-windows-static

cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=path/to/vcpkg/scripts/buildsystems/vcpkg.cmake -DVCPKG_TARGET_TRIPLET=x64-windows-static -DCMAKE_PREFIX_PATH=path/to/vcpkg/installed/x64-windows-static -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded
cmake --build build --config Release
```

For repeatable Hash API/CUDA benchmark runs on Windows, use a fresh Release CUDA preset and keep benchmark output under an ignored local artifact directory:

```bash
cmake --preset cuda-release-vcpkg-modern
cmake --build --preset cuda-release-vcpkg-modern
python scripts/hash_api_benchmark.py --binary <miner-binary> --backend cuda --device 0 --preset warm-short --seconds 2 --warmup 1 --repeat 3 --no-xuni --output .benchmarks/warm-short.json
```

Use the architecture-specific presets for targeted builds:

```bash
cmake --preset cuda-release-vcpkg-sm86
cmake --build --preset cuda-release-vcpkg-sm86
```

Do not compare benchmark results from a Debug build or from a stale build directory that was configured for an unrelated `CMAKE_CUDA_ARCHITECTURES` value.

## Clean

```bash
cmake --build build --target clean
```

## Clean build system

```bash
rm -rf build
```
