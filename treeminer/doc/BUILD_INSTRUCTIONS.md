# Build Instructions

This document provides instructions for building the project on both Linux and Windows systems using vcpkg.

## Prerequisites

- CMake installed on your system.
- CUDA Toolkit installed on your system

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

The `cuda-release-vcpkg-modern` preset builds for a public modern CUDA architecture set: `75;80;86;89;90`. If you want to build only for one CUDA architecture, use the matching preset when available:

```bash
cmake --preset cuda-release-vcpkg-sm86
cmake --build --preset cuda-release-vcpkg-sm86
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
