add_executable(treeminer_hashapi_selftest_tests
    HashApiSelfTestTests.cpp
    ../../../src/hashapi/HashApiSelfTest.cpp
)
target_include_directories(treeminer_hashapi_selftest_tests PRIVATE ../../../src)
target_compile_features(treeminer_hashapi_selftest_tests PRIVATE cxx_std_17)
add_test(NAME hashapi.cpu_cuda_selftest COMMAND treeminer_hashapi_selftest_tests)

add_executable(treeminer_oneshot_launch_tests
    OneshotLaunchTests.cpp
    ../../../src/hashapi/HashApiValidation.cpp
)
target_include_directories(treeminer_oneshot_launch_tests PRIVATE ../../../src)
target_compile_features(treeminer_oneshot_launch_tests PRIVATE cxx_std_17)
add_test(NAME hashapi.oneshot_launch COMMAND treeminer_oneshot_launch_tests)
