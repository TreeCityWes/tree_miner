# ---------------------------------------------------------------------------------------
# Chaos tests — PLAN.md §6 acceptance gates, driven against the REAL FindJournal and the
# REAL SubmissionManager with only the transport scripted. No GPU, no network, no sleeping:
# the manager's clocks are injected, so a multi-hour outage runs in milliseconds.
#
# ROOT WIRING (already done in the root CMakeLists.txt):
#   add_subdirectory(src/journal)
#   add_subdirectory(src/submit)
#   enable_testing()
#   add_subdirectory(tests/chaos)
#
# Standalone, for fast iteration:
#   cmake -S tests/chaos -B build-chaos \
#         -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake
#   cmake --build build-chaos && ctest --test-dir build-chaos --output-on-failure
# ---------------------------------------------------------------------------------------

if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    cmake_minimum_required(VERSION 3.18)
    project(treeminer_chaos_tests LANGUAGES CXX)
    set(CMAKE_CXX_STANDARD 17)
    set(CMAKE_CXX_STANDARD_REQUIRED True)
    enable_testing()
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../src/journal
                     ${CMAKE_BINARY_DIR}/treeminer_journal)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../src/submit
                     ${CMAKE_BINARY_DIR}/treeminer_submit)
endif()

# Imported targets are directory-scoped; the journal library links it too, but this
# directory needs its own find_package for the transitive sqlite3 dependency.
# vcpkg (unofficial-sqlite3) or a system/nix SQLite via CMake's FindSQLite3.
find_package(unofficial-sqlite3 CONFIG QUIET)
if(unofficial-sqlite3_FOUND)
    set(TREEMINER_SQLITE3_TARGET unofficial::sqlite3::sqlite3)
else()
    find_package(SQLite3 REQUIRED)
    if(TARGET SQLite3::SQLite3)
        set(TREEMINER_SQLITE3_TARGET SQLite3::SQLite3)
    else()
        set(TREEMINER_SQLITE3_TARGET SQLite::SQLite3)
    endif()
endif()

# src/treeminer/Types.cpp is the single defining TU for treeminer::to_string (contract
# v1.1); each test binary compiles it in rather than defining the symbols itself.
add_executable(treeminer_chaos_tests
    test_difficulty_outage.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/treeminer/Types.cpp
)
target_link_libraries(treeminer_chaos_tests
    PRIVATE treeminer_journal treeminer_submit ${TREEMINER_SQLITE3_TARGET})
# Reuse the shared plain-assert framework from the submit unit tests.
target_include_directories(treeminer_chaos_tests
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../unit/submit)

add_test(NAME chaos.difficulty_outage COMMAND treeminer_chaos_tests)
# Each case writes its own scratch database in the working directory.
set_tests_properties(chaos.difficulty_outage PROPERTIES
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    TIMEOUT 60)
