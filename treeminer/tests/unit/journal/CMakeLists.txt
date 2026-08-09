# ---------------------------------------------------------------------------------------
# treeminer_journal_tests — standalone CTest executable for the FindJournal (no GPU,
# no network; plain-assert framework inline in FindJournalTests.cpp).
#
# ROOT CMakeLists.txt WIRING (for the integration lead — do not wire from here):
#
#   enable_testing()                          # or include(CTest), once, at the root
#   add_subdirectory(src/journal)             # must come first (provides treeminer_journal)
#   add_subdirectory(tests/unit/journal)
#
# This directory also configures standalone for fast iteration:
#   cmake -S tests/unit/journal -B build-journal-tests \
#         -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake
#   cmake --build build-journal-tests && ctest --test-dir build-journal-tests
# ---------------------------------------------------------------------------------------

if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    cmake_minimum_required(VERSION 3.18)
    project(treeminer_journal_tests LANGUAGES CXX)
    set(CMAKE_CXX_STANDARD 17)
    set(CMAKE_CXX_STANDARD_REQUIRED True)
    enable_testing()
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../../src/journal
                     ${CMAKE_BINARY_DIR}/treeminer_journal)
endif()

# Idempotent if src/journal already found it; needed here because the tests also open
# the database directly (raw persistence checks) and imported targets are
# directory-scoped.
find_package(unofficial-sqlite3 CONFIG REQUIRED)

add_executable(treeminer_journal_tests FindJournalTests.cpp)
target_link_libraries(treeminer_journal_tests
    PRIVATE treeminer_journal unofficial::sqlite3::sqlite3)

# One CTest case per test function (the binary takes test names as arguments).
set(TREEMINER_JOURNAL_TEST_CASES
    append_durable_and_idempotent
    invalid_payload_stored
    fetch_eligible_backoff_and_ordering
    record_attempt_round_trip
    unpark_for_difficulty_boundary
    unpark_xuni_budget_exhaustion
    recover_on_startup_counts
    difficulty_seen_round_trip
    reopen_persistence
)
foreach(case IN LISTS TREEMINER_JOURNAL_TEST_CASES)
    add_test(NAME journal.${case} COMMAND treeminer_journal_tests ${case})
endforeach()
