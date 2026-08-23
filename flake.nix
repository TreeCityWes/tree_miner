{
  description = "TreeMiner — outage-proof XenBlocks miner (NVIDIA CUDA / AMD ROCm)";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs = { self, nixpkgs, flake-utils }:
    flake-utils.lib.eachDefaultSystem (system:
      let
        pkgs = import nixpkgs { inherit system; };
        rocm = pkgs.rocmPackages;

        # Everything the miner links against, independent of GPU vendor. The upstream
        # build uses vcpkg for these; CMake also accepts the system copies (SQLite via
        # FindSQLite3, argon2 via pkg-config).
        commonDeps = with pkgs; [
          libargon2
          cryptopp
          cpr
          nlohmann_json
          openssl
          boost
          secp256k1
          crow
          paho-mqtt-c
          paho-mqtt-cpp
          sqlite
          asio
        ];

        rocmDeps = [
          rocm.clr           # HIP runtime + hipcc/amdclang++ + rocminfo
          rocm.rocm-smi      # optional: power/utilization gauges
          rocm.rocm-device-libs
        ];

        buildTools = with pkgs; [ cmake ninja pkg-config gcc python3 ];
      in
      {
        devShells.default = pkgs.mkShell {
          name = "treeminer-rocm";
          packages = buildTools ++ commonDeps ++ rocmDeps;

          # hipcc needs to find its device bitcode; on NixOS these do not live under
          # /opt/rocm, so point the toolchain at the store paths explicitly.
          ROCM_PATH = "${rocm.clr}";
          HIP_PATH = "${rocm.clr}";
          HIP_DEVICE_LIB_PATH = "${rocm.rocm-device-libs}/amdgcn/bitcode";

          shellHook = ''
            echo "TreeMiner ROCm dev shell — hipcc $(hipcc --version 2>/dev/null | head -1)"
            echo "configure:  cmake -S treeminer -B build -G Ninja \\"
            echo "              -DTREEMINER_GPU_BACKEND=HIP \\"
            echo "              -DCMAKE_HIP_COMPILER=$ROCM_PATH/bin/amdclang++"
            echo "build:      cmake --build build -j"
          '';
        };

        # `nix build .#treeminer-rocm` — the miner built for AMD GPUs. Pass a gfx target
        # with --override-input or edit hipArch below; gfx1100 covers RDNA3 (RX 7900).
        packages.treeminer-rocm = pkgs.stdenv.mkDerivation {
          pname = "treeminer-rocm";
          version = "1.0";
          src = ./treeminer;
          nativeBuildInputs = buildTools;
          buildInputs = commonDeps ++ rocmDeps;
          cmakeFlags = [
            "-DTREEMINER_GPU_BACKEND=HIP"
            "-DCMAKE_HIP_COMPILER=${rocm.clr}/bin/amdclang++"
            "-DCMAKE_HIP_ARCHITECTURES=gfx1100"
            "-DTREEMINER_BUILD_TESTS=OFF"
          ];
          HIP_DEVICE_LIB_PATH = "${rocm.rocm-device-libs}/amdgcn/bitcode";
          installPhase = ''
            mkdir -p $out/bin
            cp bin/xenblocksMiner $out/bin/
          '';
        };

        packages.default = self.packages.${system}.treeminer-rocm;
      });
}
