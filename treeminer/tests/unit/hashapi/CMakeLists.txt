add_executable(treeminer_hashapi_selftest_tests
    HashApiSelfTestTests.cpp
    ../../../src/hashapi/HashApiSelfTest.cpp
)
target_include_directories(treeminer_hashapi_selftest_tests PRIVATE ../../../src)
target_compile_features(treeminer_hashapi_selftest_tests PRIVATE cxx_std_17)
add_test(NAME hashapi.cpu_cuda_selftest COMMAND treeminer_hashapi_selftest_tests)
