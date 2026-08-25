# ---------------------------------------------------------------------------------------
# treeminer_journal — SQLite-backed durable FindJournal (PLAN.md §3.1/§10, SOL-PLAN.md §4).
#
# ROOT CMakeLists.txt WIRING (for the integration lead — do not wire from here):
#
#   add_subdirectory(src/journal)             # provides static lib target: treeminer_journal
#   # unit tests (needs enable_testing()/include(CTest) at the root first):
#   add_subdirectory(tests/unit/journal)
#
# Consumers just link the target:
#   target_link_libraries(<consumer> PRIVATE treeminer_journal)
# and include headers as "journal/FindJournal.h" / "treeminer/IFindJournal.h".
#
# NOTE (contract v1.1): this library REFERENCES treeminer::to_string(FindStatus/FindKind)
# but deliberately does not define them — src/treeminer/Types.cpp is the single defining
# TU (integration-lead-owned) and must be compiled into each final binary that links
# treeminer_journal (the miner links it once; test binaries add it to their sources).
#
# Dependency: vcpkg port `sqlite3` (already listed in vcpkg.json).
# ---------------------------------------------------------------------------------------

# vcpkg exports SQLite as unofficial-sqlite3; distro/nix installs ship the upstream
# library that CMake's own FindSQLite3 module locates. Accept either.
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

add_library(treeminer_journal STATIC
    FindJournal.cpp
    FallbackSink.cpp
)

# Headers are included relative to src/ ("journal/...", "treeminer/...").
target_include_directories(treeminer_journal PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)

target_compile_features(treeminer_journal PUBLIC cxx_std_17)

target_link_libraries(treeminer_journal PUBLIC ${TREEMINER_SQLITE3_TARGET})
