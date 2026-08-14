add_executable(treeminer_hashapi_selftest_tests
    HashApiSelfTestTests.cpp
    ../../../src/hashapi/HashApiSelfTest.cpp
)
target_include_directories(treeminer_hashapi_selftest_tests PRIVATE ../../../src)
target_compile_features(treeminer_hashapi_selftest_tests PRIVATE cxx_std_17)
add_test(NAME hashapi.cpu_cuda_selftest COMMAND treeminer_hashapi_selftest_tests)

add_executable(treeminer_hashapi_tuning_share_tests
    HashApiTuningShareTests.cpp
    ../../../src/hashapi/HashApiTuning.cpp
)
target_include_directories(treeminer_hashapi_tuning_share_tests PRIVATE ../../../src)
target_compile_features(treeminer_hashapi_tuning_share_tests PRIVATE cxx_std_17)
add_test(NAME hashapi.tuning_stream_share COMMAND treeminer_hashapi_tuning_share_tests)

add_executable(treeminer_gpu_memory_planner_tests
    GpuMemoryPlannerTests.cpp
    ../../../src/GpuMemoryPlanner.cpp
    ../../../src/hashapi/HashApiTuning.cpp
)
target_include_directories(treeminer_gpu_memory_planner_tests PRIVATE ../../../src)
target_compile_features(treeminer_gpu_memory_planner_tests PRIVATE cxx_std_17)
add_test(NAME hashapi.gpu_memory_planner COMMAND treeminer_gpu_memory_planner_tests)
