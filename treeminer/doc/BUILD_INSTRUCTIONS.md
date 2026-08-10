# Build Instructions

This document provides instructions for building the project on both Linux and Windows systems using vcpkg.

## Prerequisites

- CMake installed on your system.
- CUDA Toolkit installed on your system

## Building on Linux

Execute the following commands in your terminal:

```bash
sudo apt install build-essential tar curl zip unzip git cmake ninja-build
git clone https://github.com/woodysoil/XenblocksMiner.git
cd XenblocksMiner
git submodule init
git submodule update
./vcpkg/bootstrap-vcpkg.sh
./vcpkg/vcpkg install
cmake -S . -B build --preset ninja-multi-vcpkg
cmake --build build --preset ninja-vcpkg-release
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
