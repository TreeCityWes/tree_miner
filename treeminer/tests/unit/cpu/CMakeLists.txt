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

# vcpkg exports argon2 as unofficial-argon2; distro/nix installs expose it through
# pkg-config (both are located by the root CMakeLists).
if(unofficial-argon2_FOUND)
    target_link_libraries(treeminer_cpu_worker_tests PRIVATE unofficial::argon2::libargon2)
elseif(ARGON2_FOUND)
    target_include_directories(treeminer_cpu_worker_tests PRIVATE ${ARGON2_INCLUDE_DIRS})
    target_link_directories(treeminer_cpu_worker_tests PRIVATE ${ARGON2_LIBRARY_DIRS})
    target_link_libraries(treeminer_cpu_worker_tests PRIVATE ${ARGON2_LIBRARIES})
else()
    message(FATAL_ERROR "libargon2 not found; cannot build treeminer_cpu_worker_tests")
endif()

add_test(NAME cpu.sidecar_worker COMMAND treeminer_cpu_worker_tests)
