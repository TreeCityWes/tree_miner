# treeminer_submit — the durable submission layer (classifier, breaker, scheduler, manager).
# Everything except HttpTransport.cpp is pure std + src/treeminer headers: no CUDA, no vcpkg.
#
# ---- Root wiring (integration lead) — add to the root CMakeLists.txt: ----
#   add_subdirectory(src/submit)                                   # after project()
#   target_link_libraries(xenblocksMiner PRIVATE treeminer_submit) # miner target
#   set(TREEMINER_SUBMIT_WITH_HTTP ON CACHE BOOL "" FORCE)         # enable the cpr adapter
#   # HttpTransport.cpp only *declares* against src/HttpClient.h; the definitions come from
#   # src/HttpClient.cpp already compiled into the miner target, so no extra sources needed.
#   # Unit tests:
#   enable_testing()
#   add_subdirectory(tests/unit/submit)
# --------------------------------------------------------------------------

option(TREEMINER_SUBMIT_WITH_HTTP
       "Compile the HttpTransport adapter over src/HttpClient (requires cpr + nlohmann_json)"
       OFF)

add_library(treeminer_submit STATIC
    ResponseClassifier.cpp
    CircuitBreaker.cpp
    DrainScheduler.cpp
    SubmissionManager.cpp
    MarginPolicy.cpp
)

target_compile_features(treeminer_submit PUBLIC cxx_std_17)
# Consumers include "submit/..." and "treeminer/..." relative to src/.
target_include_directories(treeminer_submit PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)

if(NOT WIN32)
    find_package(Threads REQUIRED)
    target_link_libraries(treeminer_submit PUBLIC Threads::Threads)
endif()

if(TREEMINER_SUBMIT_WITH_HTTP)
    # The adapter needs nlohmann/json.hpp (pulled in by src/HttpClient.h). cpr symbols are
    # resolved at the final miner link, where src/HttpClient.cpp is compiled.
    find_package(nlohmann_json CONFIG REQUIRED)
    target_sources(treeminer_submit PRIVATE HttpTransport.cpp)
    target_link_libraries(treeminer_submit PRIVATE nlohmann_json::nlohmann_json)
endif()
