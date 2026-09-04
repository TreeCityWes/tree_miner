cmake_minimum_required(VERSION 3.18)

option(XENBLOCKS_BUILD_MINER "Build the full CUDA miner" ON)
option(TREEMINER_BUILD_TESTS "Build TreeMiner unit tests (journal + submit)" ON)
option(XENBLOCKS_BUILD_HASHAPI_CLI "Build the standalone Hash API CLI" OFF)
option(XENBLOCKS_HASHAPI_STUB_BACKEND "Build hashapi-cli with a deterministic stub backend for CLI smoke tests" OFF)
option(XENBLOCKS_BUILD_HASH_DIAGNOSTICS "Build small hash diagnostic self-test tools" ON)
option(XENBLOCKS_STATIC_LIBSTDCXX "Statically link libstdc++/libgcc for a portable binary; turn OFF when the CUDA host compiler is older than the toolchain that built the vcpkg deps" ON)
option(TREEMINER_BUILD_HASH_FFI "Build libtreeminer_hash (C ABI for the Rust host). Not linked into xenblocksMiner." OFF)

# GPU compute backend. CUDA drives NVIDIA cards through nvcc; HIP drives AMD cards
# through ROCm's hipcc. The miner sources are shared — src/gpu/GpuRuntime.h maps the
# CUDA runtime calls onto HIP when TREEMINER_GPU_HIP is defined.
set(TREEMINER_GPU_BACKEND "CUDA" CACHE STRING "GPU backend for the miner: CUDA (NVIDIA) or HIP (AMD ROCm)")
set_property(CACHE TREEMINER_GPU_BACKEND PROPERTY STRINGS CUDA HIP)
string(TOUPPER "${TREEMINER_GPU_BACKEND}" TREEMINER_GPU_BACKEND_UPPER)
if(TREEMINER_GPU_BACKEND_UPPER STREQUAL "ROCM")
    set(TREEMINER_GPU_BACKEND_UPPER "HIP")
endif()
if(NOT TREEMINER_GPU_BACKEND_UPPER MATCHES "^(CUDA|HIP)$")
    message(FATAL_ERROR "TREEMINER_GPU_BACKEND must be CUDA or HIP (got '${TREEMINER_GPU_BACKEND}')")
endif()
if(TREEMINER_GPU_BACKEND_UPPER STREQUAL "HIP")
    set(TREEMINER_GPU_HIP ON)
    if(CMAKE_VERSION VERSION_LESS 3.21)
        message(FATAL_ERROR "The ROCm/HIP backend needs CMake 3.21 or newer (HIP language support); found ${CMAKE_VERSION}")
    endif()
else()
    set(TREEMINER_GPU_HIP OFF)
endif()

if(NOT TREEMINER_GPU_HIP)
# --- NVIDIA CUDA ---------------------------------------------------------------
# Remember whether the caller selected an architecture. CMake may initialize
# CMAKE_CUDA_ARCHITECTURES while enabling CUDA, so this must be captured first.
if(DEFINED CMAKE_CUDA_ARCHITECTURES)
    set(XENBLOCKS_CUDA_ARCHITECTURES_EXPLICIT ON)
else()
    set(XENBLOCKS_CUDA_ARCHITECTURES_EXPLICIT OFF)
endif()

if(XENBLOCKS_BUILD_MINER)
    # Select the compute capabilities of GPUs visible during configuration.
    # Explicit -DCMAKE_CUDA_ARCHITECTURES values still win for cross-builds.
    if(NOT DEFINED CMAKE_CUDA_ARCHITECTURES)
        set(TREEMINER_SMI_RESULT 1)
        find_program(TREEMINER_NVIDIA_SMI nvidia-smi)
        if(TREEMINER_NVIDIA_SMI)
            execute_process(
                COMMAND ${TREEMINER_NVIDIA_SMI} --query-gpu=compute_cap --format=csv,noheader,nounits
                RESULT_VARIABLE TREEMINER_SMI_RESULT
                OUTPUT_VARIABLE TREEMINER_COMPUTE_CAPS
                OUTPUT_STRIP_TRAILING_WHITESPACE
                ERROR_QUIET)
        endif()
        if(TREEMINER_SMI_RESULT EQUAL 0 AND NOT TREEMINER_COMPUTE_CAPS STREQUAL "")
            string(REPLACE "\r" "" TREEMINER_COMPUTE_CAPS "${TREEMINER_COMPUTE_CAPS}")
            string(REPLACE "\n" ";" TREEMINER_COMPUTE_CAPS "${TREEMINER_COMPUTE_CAPS}")
            set(TREEMINER_DETECTED_ARCHITECTURES "")
            foreach(TREEMINER_CAP IN LISTS TREEMINER_COMPUTE_CAPS)
                string(STRIP "${TREEMINER_CAP}" TREEMINER_CAP)
                if(TREEMINER_CAP MATCHES "^([0-9]+)\\.([0-9]+)$")
                    list(APPEND TREEMINER_DETECTED_ARCHITECTURES "${CMAKE_MATCH_1}${CMAKE_MATCH_2}")
                endif()
            endforeach()
            list(REMOVE_DUPLICATES TREEMINER_DETECTED_ARCHITECTURES)
            if(TREEMINER_DETECTED_ARCHITECTURES)
                set(CMAKE_CUDA_ARCHITECTURES "${TREEMINER_DETECTED_ARCHITECTURES}"
                    CACHE STRING "CUDA architectures detected from nvidia-smi")
            endif()
        endif()
        if(NOT DEFINED CMAKE_CUDA_ARCHITECTURES)
            message(WARNING "No NVIDIA compute capability detected; pass -DCMAKE_CUDA_ARCHITECTURES explicitly")
        endif()
    endif()
endif()

if(XENBLOCKS_BUILD_MINER)
    project(xenblocksMiner LANGUAGES CUDA CXX VERSION 1.4.1)
else()
    project(xenblocksMiner LANGUAGES CXX VERSION 1.4.1)
endif()

else() # TREEMINER_GPU_HIP
# --- AMD ROCm / HIP ------------------------------------------------------------
# Select the GPU architectures to emit. Explicit -DCMAKE_HIP_ARCHITECTURES wins for
# cross-builds; otherwise ask the local ROCm install what is installed; otherwise emit
# a fat binary covering the consumer and datacenter parts ROCm currently supports.
if(XENBLOCKS_BUILD_MINER AND NOT DEFINED CMAKE_HIP_ARCHITECTURES)
    set(TREEMINER_GFX_RESULT 1)
    find_program(TREEMINER_AMDGPU_ARCH NAMES amdgpu-arch amdgpu_arch
                 HINTS ENV ROCM_PATH /opt/rocm PATH_SUFFIXES bin llvm/bin)
    if(TREEMINER_AMDGPU_ARCH)
        execute_process(
            COMMAND ${TREEMINER_AMDGPU_ARCH}
            RESULT_VARIABLE TREEMINER_GFX_RESULT
            OUTPUT_VARIABLE TREEMINER_GFX_OUTPUT
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET)
    endif()
    if(NOT TREEMINER_GFX_RESULT EQUAL 0)
        find_program(TREEMINER_ROCMINFO rocminfo HINTS ENV ROCM_PATH /opt/rocm PATH_SUFFIXES bin)
        if(TREEMINER_ROCMINFO)
            execute_process(
                COMMAND ${TREEMINER_ROCMINFO}
                RESULT_VARIABLE TREEMINER_GFX_RESULT
                OUTPUT_VARIABLE TREEMINER_GFX_OUTPUT
                OUTPUT_STRIP_TRAILING_WHITESPACE
                ERROR_QUIET)
        endif()
    endif()
    if(TREEMINER_GFX_RESULT EQUAL 0 AND NOT TREEMINER_GFX_OUTPUT STREQUAL "")
        # Both tools print one gfx target per line (rocminfo also prints the CPU agents,
        # which carry no gfx name), so a plain match over the output covers either source.
        string(REGEX MATCHALL "gfx[0-9a-f]+(:[-+a-z0-9]+)*" TREEMINER_DETECTED_GFX "${TREEMINER_GFX_OUTPUT}")
        if(TREEMINER_DETECTED_GFX)
            list(REMOVE_DUPLICATES TREEMINER_DETECTED_GFX)
            set(CMAKE_HIP_ARCHITECTURES "${TREEMINER_DETECTED_GFX}"
                CACHE STRING "HIP architectures detected from the local ROCm install")
        endif()
    endif()
    if(NOT DEFINED CMAKE_HIP_ARCHITECTURES)
        # No GPU visible at configure time (CI, container builds): fat binary.
        set(CMAKE_HIP_ARCHITECTURES
            gfx900 gfx906 gfx908 gfx90a gfx942
            gfx1010 gfx1030 gfx1031 gfx1100 gfx1101 gfx1102 gfx1200 gfx1201
            CACHE STRING "HIP architectures (fat-binary default)")
        message(STATUS "No AMD GPU detected at configure time; building a fat HIP binary")
    endif()
endif()

if(XENBLOCKS_BUILD_MINER)
    project(xenblocksMiner LANGUAGES HIP CXX VERSION 1.4.1)
    set(CMAKE_HIP_STANDARD 17)
    set(CMAKE_HIP_STANDARD_REQUIRED ON)
    message(STATUS "TreeMiner HIP architectures: ${CMAKE_HIP_ARCHITECTURES}")
else()
    project(xenblocksMiner LANGUAGES CXX VERSION 1.4.1)
endif()
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

set(CMAKE_BUILD_TYPE Release)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED True)

if(XENBLOCKS_BUILD_MINER AND NOT TREEMINER_GPU_HIP)
    set(CMAKE_CUDA_STANDARD 17)
    set(CMAKE_CUDA_STANDARD_REQUIRED ON)
    # Produce native code for every architecture supported by the selected toolkit —
    # but only when nothing chose one already. Precedence: explicit
    # -DCMAKE_CUDA_ARCHITECTURES > nvidia-smi detection above (local dev builds) >
    # these toolkit-tier fat-binary defaults (release/CI builds on GPU-less hosts).
    if(NOT XENBLOCKS_CUDA_ARCHITECTURES_EXPLICIT AND NOT TREEMINER_DETECTED_ARCHITECTURES)
        if(CMAKE_CUDA_COMPILER_VERSION VERSION_GREATER_EQUAL 13.0)
            set(_xenblocks_cuda_architectures 75-real 80-real 86-real 87-real 89-real 90-real 120)
        elseif(CMAKE_CUDA_COMPILER_VERSION VERSION_GREATER_EQUAL 12.8)
            set(_xenblocks_cuda_architectures 50-real 52-real 60-real 61-real 70-real 75-real 80-real 86-real 87-real 89-real 90-real 120)
        elseif(CMAKE_CUDA_COMPILER_VERSION VERSION_GREATER_EQUAL 11.8)
            set(_xenblocks_cuda_architectures 50-real 52-real 60-real 61-real 70-real 75-real 80-real 86-real 87-real 89-real 90)
        else()
            set(_xenblocks_cuda_architectures 50-real 52-real 60-real 61-real 70-real 75-real 80-real 86)
        endif()
        set(CMAKE_CUDA_ARCHITECTURES "${_xenblocks_cuda_architectures}" CACHE STRING "CUDA architectures" FORCE)
    endif()

    if(CMAKE_CUDA_COMPILER_VERSION VERSION_GREATER_EQUAL 12.8)
        set(_treeminer_cuda_max_arch 120)
    elseif(CMAKE_CUDA_COMPILER_VERSION VERSION_GREATER_EQUAL 11.8)
        set(_treeminer_cuda_max_arch 90)
    elseif(CMAKE_CUDA_COMPILER_VERSION VERSION_GREATER_EQUAL 11.0)
        set(_treeminer_cuda_max_arch 86)
    else()
        set(_treeminer_cuda_max_arch 75)
    endif()

    set(_xenblocks_adjusted_architectures "")
    foreach(_xenblocks_cuda_arch IN LISTS CMAKE_CUDA_ARCHITECTURES)
        set(_treeminer_arch_num 0)
        if(_xenblocks_cuda_arch MATCHES "^([0-9]+)")
            set(_treeminer_arch_num "${CMAKE_MATCH_1}")
        endif()
        if(_treeminer_arch_num GREATER _treeminer_cuda_max_arch)
            if(XENBLOCKS_CUDA_ARCHITECTURES_EXPLICIT)
                message(FATAL_ERROR
                    "sm_${_treeminer_arch_num} requires a newer CUDA Toolkit "
                    "(this nvcc is ${CMAKE_CUDA_COMPILER_VERSION}, max native sm_${_treeminer_cuda_max_arch}).")
            endif()
            message(WARNING
                "nvidia-smi reported sm_${_treeminer_arch_num} but toolkit ${CMAKE_CUDA_COMPILER_VERSION} "
                "cannot emit that SASS; falling back to sm_${_treeminer_cuda_max_arch} PTX")
            list(APPEND _xenblocks_adjusted_architectures "${_treeminer_cuda_max_arch}")
        else()
            list(APPEND _xenblocks_adjusted_architectures "${_xenblocks_cuda_arch}")
        endif()
    endforeach()
    if(_xenblocks_adjusted_architectures)
        list(REMOVE_DUPLICATES _xenblocks_adjusted_architectures)
        set(CMAKE_CUDA_ARCHITECTURES "${_xenblocks_adjusted_architectures}"
            CACHE STRING "CUDA architectures" FORCE)
    endif()
    message(STATUS "TreeMiner CUDA architectures (toolkit ${CMAKE_CUDA_COMPILER_VERSION}): ${CMAKE_CUDA_ARCHITECTURES}")
endif()

if(WIN32)
    add_definitions(-D_CRT_SECURE_NO_WARNINGS)
endif()

if(XENBLOCKS_BUILD_MINER)
    if(TREEMINER_GPU_HIP)
        find_package(hip REQUIRED)
        # ROCm SMI is what NVML is on the NVIDIA side: optional, only used for the
        # power/utilization gauges. Without it the miner runs and reports no power.
        find_path(TREEMINER_ROCM_SMI_INCLUDE_DIR rocm_smi/rocm_smi.h
                  HINTS ENV ROCM_PATH /opt/rocm PATH_SUFFIXES include rocm_smi/include)
        find_library(TREEMINER_ROCM_SMI_LIBRARY NAMES rocm_smi64 rocm_smi
                     HINTS ENV ROCM_PATH /opt/rocm PATH_SUFFIXES lib lib64 rocm_smi/lib)
        if(TREEMINER_ROCM_SMI_INCLUDE_DIR AND TREEMINER_ROCM_SMI_LIBRARY)
            message(STATUS "TreeMiner GPU telemetry: ROCm SMI (${TREEMINER_ROCM_SMI_LIBRARY})")
        else()
            message(WARNING "ROCm SMI not found; GPU power/utilization reporting is disabled")
        endif()
    else()
        find_package(CUDAToolkit REQUIRED)
    endif()
    find_package(cpr CONFIG REQUIRED)
    if(NOT cpr_FOUND)
        message(FATAL_ERROR "libcpr not found. Please install libcpr.")
    endif()

    # vcpkg ships a cryptopp CMake config; most distro/nix packages only ship pkg-config.
    find_package(cryptopp CONFIG QUIET)
    if(cryptopp_FOUND)
        set(XENBLOCKS_CRYPTOPP_TARGETS cryptopp::cryptopp)
    else()
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(CRYPTOPP REQUIRED IMPORTED_TARGET libcryptopp)
        set(XENBLOCKS_CRYPTOPP_TARGETS PkgConfig::CRYPTOPP)
    endif()

    find_package(OpenSSL REQUIRED)
    if(NOT OPENSSL_FOUND)
        message(FATAL_ERROR "OpenSSL not found. Please install OpenSSL.")
    endif()

    find_package(Boost REQUIRED COMPONENTS program_options)
    if(NOT Boost_FOUND)
        message(FATAL_ERROR "Boost.Program_options not found. Please install Boost.Program_options.")
    endif()

    # Older vcpkg ports export unofficial-secp256k1; current ones ship upstream's
    # libsecp256k1 config. Accept either.
    find_package(unofficial-secp256k1 CONFIG QUIET)
    if(unofficial-secp256k1_FOUND)
        set(XENBLOCKS_SECP256K1_TARGETS unofficial::secp256k1 unofficial::secp256k1_precomputed)
    else()
        find_package(libsecp256k1 CONFIG QUIET)
        if(libsecp256k1_FOUND)
            set(XENBLOCKS_SECP256K1_TARGETS libsecp256k1::secp256k1)
        else()
            # Distro/nix builds expose it through pkg-config only. The recovery module
            # (secp256k1_recovery.h) must be enabled in that build — it is what signs the
            # miner's identity payloads.
            find_package(PkgConfig REQUIRED)
            pkg_check_modules(SECP256K1 REQUIRED IMPORTED_TARGET libsecp256k1)
            set(XENBLOCKS_SECP256K1_TARGETS PkgConfig::SECP256K1)
        endif()
    endif()

    find_package(Crow CONFIG REQUIRED)
    if(NOT Crow_FOUND)
        message(FATAL_ERROR "Libcrow not found. Please install Libcrow.")
    endif()

    find_package(PahoMqttCpp CONFIG REQUIRED)
    if(NOT PahoMqttCpp_FOUND)
        message(FATAL_ERROR "PahoMqttCpp not found. Please install PahoMqttCpp.")
    endif()
    if(TARGET PahoMqttCpp::paho-mqttpp3-static)
        set(XENBLOCKS_PAHO_MQTT_CPP_TARGET PahoMqttCpp::paho-mqttpp3-static)
    elseif(TARGET PahoMqttCpp::paho-mqttpp3-shared)
        set(XENBLOCKS_PAHO_MQTT_CPP_TARGET PahoMqttCpp::paho-mqttpp3-shared)
        set(XENBLOCKS_PAHO_MQTT_CPP_SHARED ON)
    elseif(TARGET PahoMqttCpp::paho-mqttpp3)
        set(XENBLOCKS_PAHO_MQTT_CPP_TARGET PahoMqttCpp::paho-mqttpp3)
    else()
        message(FATAL_ERROR "PahoMqttCpp target not found after find_package(PahoMqttCpp).")
    endif()
endif()

find_package(unofficial-argon2 CONFIG QUIET)
if(NOT unofficial-argon2_FOUND)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(ARGON2 QUIET libargon2)
    endif()
endif()

find_package(nlohmann_json CONFIG QUIET)

if(XENBLOCKS_BUILD_MINER)
    # vcpkg exports argon2 as unofficial-argon2; distro/nix installs expose it through
    # pkg-config as libargon2 (found above). Either satisfies the miner.
    if(unofficial-argon2_FOUND)
        set(TREEMINER_ARGON2_TARGETS unofficial::argon2::libargon2)
    elseif(ARGON2_FOUND)
        set(TREEMINER_ARGON2_TARGETS ${ARGON2_LIBRARIES})
    else()
        message(FATAL_ERROR "libargon2 not found. Please install argon2.")
    endif()
    if(NOT nlohmann_json_FOUND)
        message(FATAL_ERROR "nlohmann_json library not found. Please install nlohmann_json.")
    endif()
    if(TREEMINER_GPU_HIP)
        list(APPEND LINK_LIBRARIES hip::host)
        if(TREEMINER_ROCM_SMI_LIBRARY)
            list(APPEND LINK_LIBRARIES ${TREEMINER_ROCM_SMI_LIBRARY})
        endif()
    else()
        list(APPEND LINK_LIBRARIES CUDA::nvml)
    endif()
    list(APPEND LINK_LIBRARIES ${TREEMINER_ARGON2_TARGETS} ${XENBLOCKS_CRYPTOPP_TARGETS} cpr::cpr nlohmann_json::nlohmann_json OpenSSL::SSL OpenSSL::Crypto Boost::program_options ${XENBLOCKS_SECP256K1_TARGETS} Crow::Crow ${XENBLOCKS_PAHO_MQTT_CPP_TARGET})
endif()

# TreeMiner components: durable journal + submission stack (contract in src/treeminer/).
if(XENBLOCKS_BUILD_MINER)
    set(TREEMINER_SUBMIT_WITH_HTTP ON CACHE BOOL "" FORCE)
    add_subdirectory(src/journal)
    add_subdirectory(src/submit)
endif()
if(TREEMINER_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests/unit/mining_common)
    add_subdirectory(tests/unit/cpu)
    add_subdirectory(tests/unit/hashapi)
    add_subdirectory(tests/unit/journal)
    add_subdirectory(tests/unit/submit)
    add_subdirectory(tests/unit/platform)
    # Chaos gates (PLAN §6): needs both the journal and the submit library, so it is only
    # buildable where the journal target exists.
    if(TARGET treeminer_journal)
        add_subdirectory(tests/chaos)
    endif()
endif()

set(SOURCE_FILES
    src/main.cpp
    src/treeminer/Types.cpp
    src/ProcessMonitor.cpp
    src/DifficultyManager.cpp
    src/StatReporter.cpp
    src/TerminalUi.cpp
    src/LocalServer.cpp
    src/BlockSubmitter.cpp
    src/AppConfig.cpp
    src/argon2params.cpp
    src/blake2b.cpp
    src/ConfigManager.cpp
    src/CudaDevice.cpp
    src/CudaBackend.cpp
    src/gpu/GpuTelemetry.cpp
    src/CpuMiningWorker.cpp
    src/kernelrunner.cu
    src/MineUnit.cpp
    src/MiningCommon.cpp
    src/Logger.cpp
    src/MerkleTree.cpp
    src/SHA256Hasher.cpp
    src/EthereumAddressValidator.cpp
    src/Argon2idHasher.cpp
    src/HttpClient.cpp
    src/HttpResponse.cpp
    src/PowSubmitter.cpp
    src/EthereumSignatureValidator.cpp
    src/MachineIDGetter.cpp
    src/MiningCoordinator.cpp
    src/MqttClient.cpp
    src/WorkerReporter.cpp
    src/LeaseManager.cpp
    src/PlatformManager.cpp
    src/platform/CommandEnvelope.cpp
    src/hashapi/CpuHashBackend.cpp
    src/hashapi/CudaHashBackend.cpp
    src/hashapi/HashApiCli.cpp
    src/hashapi/HashApiEncoding.cpp
    src/hashapi/HashApiJson.cpp
    src/hashapi/HashApiMatching.cpp
    src/hashapi/HashApiSelfTest.cpp
    src/hashapi/HashApiTuning.cpp
    src/hashapi/HashApiValidation.cpp
    src/hashapi/IndexedRefTable.cpp
    src/hashapi/WarpsPerBlockGolden.cpp
)
set(HASHAPI_SOURCE_FILES
    src/hashapi/HashApiCli.cpp
    src/hashapi/HashApiEncoding.cpp
    src/hashapi/HashApiJson.cpp
    src/hashapi/HashApiMatching.cpp
    src/hashapi/HashApiTuning.cpp
    src/hashapi/HashApiValidation.cpp
)

if(XENBLOCKS_HASHAPI_STUB_BACKEND)
    list(APPEND HASHAPI_SOURCE_FILES src/hashapi/StubHashBackend.cpp)
else()
    list(APPEND HASHAPI_SOURCE_FILES
        src/Argon2idHasher.cpp
        src/hashapi/CpuHashBackend.cpp
    )
endif()

if(XENBLOCKS_BUILD_MINER)
    if(TREEMINER_GPU_HIP)
        # One kernel source, two compilers: hipcc/clang accepts the .cu file as HIP once
        # the language is stated explicitly (CMake will not infer HIP from the extension).
        set_source_files_properties(src/kernelrunner.cu PROPERTIES LANGUAGE HIP)
    endif()
    if(WIN32)
        add_executable(${PROJECT_NAME} ${SOURCE_FILES} res/version.rc)
    else()
        # Static libstdc++/libgcc make the release binary portable across hosts. But when the
        # CUDA host compiler (nvcc's -ccbin) is OLDER than the toolchain that built the vcpkg
        # dependencies — e.g. CUDA 11.5 needs g++-10 as host, while vcpkg deps compile with the
        # system g++-11 — the older static libstdc++ lacks symbols the newer deps reference
        # (std::__throw_bad_array_new_length, a GCC-11 addition), and the link fails. Disable
        # this on the split-compiler lane (-DXENBLOCKS_STATIC_LIBSTDCXX=OFF) to link the shared
        # system libstdc++ instead.
        if(XENBLOCKS_STATIC_LIBSTDCXX)
            set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -static-libgcc -static-libstdc++")
        endif()
        add_executable(${PROJECT_NAME} ${SOURCE_FILES})
    endif()

    if(WIN32)
        target_link_libraries(${PROJECT_NAME} PRIVATE ${LINK_LIBRARIES} Iphlpapi)
    elseif(UNIX AND NOT APPLE)
        target_link_libraries(${PROJECT_NAME} PRIVATE ${LINK_LIBRARIES})
    endif()
    target_link_libraries(${PROJECT_NAME} PRIVATE treeminer_journal treeminer_submit)

    target_compile_options(${PROJECT_NAME} PRIVATE
        $<$<COMPILE_LANGUAGE:CUDA>:-Wno-deprecated-gpu-targets>
    )
    target_compile_definitions(${PROJECT_NAME} PRIVATE XENBLOCKS_BUILD_MINER=1)
    if(NOT unofficial-argon2_FOUND AND ARGON2_FOUND)
        target_include_directories(${PROJECT_NAME} PRIVATE ${ARGON2_INCLUDE_DIRS})
        target_link_directories(${PROJECT_NAME} PRIVATE ${ARGON2_LIBRARY_DIRS})
    endif()
    if(TREEMINER_GPU_HIP)
        # Deliberately hip::host and not hip::device: the latter pushes --offload-arch onto
        # every source in the target, including the C++ ones the host compiler builds.
        # CMake's HIP language already emits the offload flags for the HIP source from
        # CMAKE_HIP_ARCHITECTURES.
        # Defined for every language in the target so the shim, the kernel and the host
        # code all agree on which runtime they are compiled against.
        target_compile_definitions(${PROJECT_NAME} PRIVATE TREEMINER_GPU_HIP=1)
        if(TREEMINER_ROCM_SMI_INCLUDE_DIR AND TREEMINER_ROCM_SMI_LIBRARY)
            target_include_directories(${PROJECT_NAME} PRIVATE ${TREEMINER_ROCM_SMI_INCLUDE_DIR})
            target_compile_definitions(${PROJECT_NAME} PRIVATE TREEMINER_HAVE_ROCM_SMI=1)
        endif()
    endif()
    if(WIN32 AND XENBLOCKS_PAHO_MQTT_CPP_SHARED)
        target_compile_definitions(${PROJECT_NAME} PRIVATE PAHO_MQTTPP_IMPORTS)
    endif()
endif()

# Extra shared library for the Rust host (`treeminer-hash` crate `--features cuda`).
# NOT linked into xenblocksMiner — the live GPU process is unchanged.
# CUDA-only: the FFI crate links CUDA::cudart and is not hipified.
if(TREEMINER_BUILD_HASH_FFI)
    if(TREEMINER_GPU_HIP)
        message(FATAL_ERROR "TREEMINER_BUILD_HASH_FFI=ON is CUDA-only; the HIP backend does not build libtreeminer_hash.")
    endif()
    if(NOT XENBLOCKS_BUILD_MINER)
        message(FATAL_ERROR "TREEMINER_BUILD_HASH_FFI=ON requires XENBLOCKS_BUILD_MINER=ON (CUDA + argon2).")
    endif()
    find_package(Threads REQUIRED)
    add_library(treeminer_hash SHARED
        src/hashapi/treeminer_hash.cpp
        src/hashapi/CpuHashBackend.cpp
        src/hashapi/CudaHashBackend.cpp
        src/hashapi/HashApiValidation.cpp
        src/hashapi/HashApiMatching.cpp
        src/hashapi/HashApiEncoding.cpp
        src/Argon2idHasher.cpp
        src/argon2params.cpp
        src/blake2b.cpp
        src/CudaBackend.cpp
        src/CudaDevice.cpp
        src/kernelrunner.cu
    )
    target_include_directories(treeminer_hash PRIVATE src)
    target_compile_features(treeminer_hash PRIVATE cxx_std_17)
    target_compile_definitions(treeminer_hash PRIVATE XENBLOCKS_BUILD_MINER=1)
    target_compile_options(treeminer_hash PRIVATE
        $<$<COMPILE_LANGUAGE:CUDA>:-Wno-deprecated-gpu-targets>
    )
    set_target_properties(treeminer_hash PROPERTIES
        POSITION_INDEPENDENT_CODE ON
        CUDA_RESOLVE_DEVICE_SYMBOLS ON
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
        ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
        BUILD_RPATH "${CUDAToolkit_LIBRARY_DIR}"
        INSTALL_RPATH "${CUDAToolkit_LIBRARY_DIR}"
    )
    target_link_libraries(treeminer_hash PRIVATE
        CUDA::cudart
        unofficial::argon2::libargon2
        Threads::Threads
    )
    file(GENERATE
        OUTPUT "${CMAKE_BINARY_DIR}/treeminer-hash-cuda.env"
        CONTENT "TREEMINER_HASH_LIB_DIR=$<TARGET_FILE_DIR:treeminer_hash>\nLD_LIBRARY_PATH=$<TARGET_FILE_DIR:treeminer_hash>:\$LD_LIBRARY_PATH\n"
    )
endif()

if(XENBLOCKS_BUILD_HASHAPI_CLI)
    add_executable(hashapi-cli
        src/hashapi/main.cpp
        ${HASHAPI_SOURCE_FILES}
    )
    target_include_directories(hashapi-cli PRIVATE src)
    if(XENBLOCKS_HASHAPI_STUB_BACKEND)
        target_compile_definitions(hashapi-cli PRIVATE XENBLOCKS_HASHAPI_STUB_BACKEND=1)
    else()
        if(unofficial-argon2_FOUND)
            target_link_libraries(hashapi-cli PRIVATE unofficial::argon2::libargon2)
        elseif(ARGON2_FOUND)
            target_include_directories(hashapi-cli PRIVATE ${ARGON2_INCLUDE_DIRS})
            target_link_libraries(hashapi-cli PRIVATE ${ARGON2_LIBRARIES})
        else()
            message(FATAL_ERROR "hashapi-cli requires libargon2 unless XENBLOCKS_HASHAPI_STUB_BACKEND=ON")
        endif()
    endif()
    if(nlohmann_json_FOUND)
        target_link_libraries(hashapi-cli PRIVATE nlohmann_json::nlohmann_json)
    endif()
endif()

if(XENBLOCKS_BUILD_HASH_DIAGNOSTICS)
    add_executable(blake2b-copy-selftest
        tests/cpp/blake2b_copy_selftest.cpp
        src/blake2b.cpp
    )
    target_include_directories(blake2b-copy-selftest PRIVATE src)

    add_executable(argon2-finalize-benchmark
        tests/cpp/argon2_finalize_benchmark.cpp
        src/argon2params.cpp
        src/blake2b.cpp
    )
    target_include_directories(argon2-finalize-benchmark PRIVATE src)
endif()

if(XENBLOCKS_BUILD_MINER AND TREEMINER_BUILD_TESTS)
    add_executable(treeminer_warps_golden
        tests/cpp/warps_per_block_golden.cpp
        src/hashapi/WarpsPerBlockGolden.cpp
        src/hashapi/IndexedRefTable.cpp
        src/argon2params.cpp
        src/blake2b.cpp
        src/kernelrunner.cu
    )
    target_include_directories(treeminer_warps_golden PRIVATE src)
    target_compile_features(treeminer_warps_golden PRIVATE cxx_std_17)
    target_compile_definitions(treeminer_warps_golden PRIVATE XENBLOCKS_BUILD_MINER=1)
    target_compile_options(treeminer_warps_golden PRIVATE
        $<$<COMPILE_LANGUAGE:CUDA>:-Wno-deprecated-gpu-targets>
    )
    if(TREEMINER_GPU_HIP)
        set_source_files_properties(src/kernelrunner.cu PROPERTIES LANGUAGE HIP)
        target_compile_definitions(treeminer_warps_golden PRIVATE TREEMINER_GPU_HIP=1)
        target_link_libraries(treeminer_warps_golden PRIVATE hip::host)
    else()
        target_link_libraries(treeminer_warps_golden PRIVATE CUDA::cudart)
    endif()
    add_test(NAME hashapi.warps_per_block_golden COMMAND treeminer_warps_golden)
endif()
