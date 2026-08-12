add_executable(treeminer_cpu_worker_tests
    CpuMiningWorkerTests.cpp
    ../../../src/CpuMiningWorker.cpp
    ../../../src/Argon2idHasher.cpp
    ../../../src/hashapi/CpuHashBackend.cpp
    ../../../src/hashapi/HashApiMatching.cpp
    ../../../src/hashapi/HashApiValidation.cpp
)

target_include_directories(treeminer_cpu_worker_tests PRIVATE
    ../../../src
)

target_compile_features(treeminer_cpu_worker_tests PRIVATE cxx_std_17)
target_link_libraries(treeminer_cpu_worker_tests PRIVATE
    unofficial::argon2::libargon2
)

add_test(NAME cpu.sidecar_worker COMMAND treeminer_cpu_worker_tests)
