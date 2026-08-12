cmake_minimum_required(VERSION 3.16)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(treeminer_mining_common_tests LANGUAGES CXX)
    enable_testing()
endif()

find_package(Threads REQUIRED)
add_executable(treeminer_mining_identity_tests
    MiningIdentityTests.cpp
    ../../../src/MiningCommon.cpp
)
target_include_directories(treeminer_mining_identity_tests PRIVATE ../../../src)
target_compile_features(treeminer_mining_identity_tests PRIVATE cxx_std_17)
target_link_libraries(treeminer_mining_identity_tests PRIVATE Threads::Threads)
add_test(NAME treeminer_mining_identity_tests COMMAND treeminer_mining_identity_tests)
