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
# v1.1, integration-lead-owned); each test binary compiles it in rather than defining
# the symbols itself. Direct .obj definitions take precedence at link, so this stays
# safe even if a future root-level library also carries Types.cpp.
add_executable(treeminer_journal_tests
    FindJournalTests.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../src/treeminer/Types.cpp
)
target_link_libraries(treeminer_journal_tests
    PRIVATE treeminer_journal ${TREEMINER_SQLITE3_TARGET})

# One CTest case per test function (the binary takes test names as arguments).
set(TREEMINER_JOURNAL_TEST_CASES
    append_durable_and_idempotent
    invalid_payload_stored
    fetch_eligible_backoff_and_ordering
    record_attempt_round_trip
    unpark_for_difficulty_boundary
    unpark_xuni_budget_exhaustion
    recover_on_startup_counts
    fetch_eligible_of_kind
    fetch_awaiting_confirmation
    get_by_id_hit_and_miss
    difficulty_seen_round_trip
    reopen_persistence
)
foreach(case IN LISTS TREEMINER_JOURNAL_TEST_CASES)
    add_test(NAME journal.${case} COMMAND treeminer_journal_tests ${case})
endforeach()

# ---------------------------------------------------------------------------------------
# treeminer_fallback_sink_tests — the last-resort JSONL sink that catches finds when
# FindJournal::append() throws, plus its startup drain back into the journal.
#
# A separate executable rather than extra sources on the one above: the two files each
# carry their own copy of the inline test framework and their own main(), so linking them
# together would collide. Same Types.cpp rule applies (single defining TU for
# treeminer::to_string); the import tests use the real FindJournal on a temp database, so
# this target links sqlite3 directly as well.
# ---------------------------------------------------------------------------------------
add_executable(treeminer_fallback_sink_tests
    FallbackSinkTests.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../src/treeminer/Types.cpp
)
target_link_libraries(treeminer_fallback_sink_tests
    PRIVATE treeminer_journal ${TREEMINER_SQLITE3_TARGET})

set(TREEMINER_FALLBACK_SINK_TEST_CASES
    append_and_import_round_trip
    import_is_idempotent_by_key
    malformed_lines_never_block_recovery
    missing_file_is_clean_noop
    append_survives_weird_content
)
foreach(case IN LISTS TREEMINER_FALLBACK_SINK_TEST_CASES)
    add_test(NAME journal.fallback_sink.${case}
             COMMAND treeminer_fallback_sink_tests ${case})
endforeach()
