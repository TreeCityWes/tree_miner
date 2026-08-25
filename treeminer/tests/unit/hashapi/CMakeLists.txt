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

add_executable(treeminer_indexed_ref_table_tests
    IndexedRefTableTests.cpp
    ../../../src/hashapi/IndexedRefTable.cpp
    ../../../src/hashapi/HashApiValidation.cpp
)
target_include_directories(treeminer_indexed_ref_table_tests PRIVATE ../../../src)
target_compile_features(treeminer_indexed_ref_table_tests PRIVATE cxx_std_17)
add_test(NAME hashapi.indexed_ref_table COMMAND treeminer_indexed_ref_table_tests)

add_executable(treeminer_first_block_vector_tests
    FirstBlockVectorsTests.cpp
    ../../../src/hashapi/FirstBlockVectors.cpp
    ../../../src/argon2params.cpp
    ../../../src/blake2b.cpp
)
target_include_directories(treeminer_first_block_vector_tests PRIVATE ../../../src)
target_compile_features(treeminer_first_block_vector_tests PRIVATE cxx_std_17)
target_compile_definitions(treeminer_first_block_vector_tests PRIVATE
    TREEMINER_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/goldens"
)
add_test(NAME hashapi.first_block_vectors COMMAND treeminer_first_block_vector_tests)

add_executable(treeminer_device_finalize_tests
    DeviceFinalizeTests.cpp
    ../../../src/hashapi/DeviceFinalize.cpp
    ../../../src/hashapi/HashApiEncoding.cpp
    ../../../src/hashapi/HashApiMatching.cpp
    ../../../src/argon2params.cpp
    ../../../src/blake2b.cpp
)
target_include_directories(treeminer_device_finalize_tests PRIVATE ../../../src)
target_compile_features(treeminer_device_finalize_tests PRIVATE cxx_std_17)
add_test(NAME hashapi.device_finalize COMMAND treeminer_device_finalize_tests)
