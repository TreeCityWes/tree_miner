# Unit tests for the submission layer. Pure std — no CUDA, no vcpkg, no network.
#
# Build standalone (no miner toolchain needed):
#   cmake -S tests/unit/submit -B build-submit-tests
#   cmake --build build-submit-tests
#   ctest --test-dir build-submit-tests --output-on-failure
#
# Or from the root build via: enable_testing(); add_subdirectory(tests/unit/submit)

cmake_minimum_required(VERSION 3.18)

if(NOT DEFINED PROJECT_NAME)
    project(treeminer_submit_tests LANGUAGES CXX)
    set(CMAKE_CXX_STANDARD 17)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    enable_testing()
endif()

if(NOT TARGET treeminer_submit)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../../src/submit
                     ${CMAKE_CURRENT_BINARY_DIR}/treeminer_submit)
endif()

foreach(suite response_classifier circuit_breaker drain_scheduler submission_manager margin_policy)
    add_executable(test_${suite} test_${suite}.cpp)
    target_link_libraries(test_${suite} PRIVATE treeminer_submit)
    target_include_directories(test_${suite} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    add_test(NAME submit.${suite} COMMAND test_${suite})
endforeach()
