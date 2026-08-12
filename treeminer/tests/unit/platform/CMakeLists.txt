# Unit tests for platform command authentication (security review finding 2). Needs
# nlohmann-json headers and OpenSSL::Crypto (both already vcpkg dependencies of the miner).
cmake_minimum_required(VERSION 3.18)

find_package(OpenSSL REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

add_executable(test_command_envelope
    test_command_envelope.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../src/platform/CommandEnvelope.cpp)
target_include_directories(test_command_envelope PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../submit          # test_framework.h
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../src)
target_link_libraries(test_command_envelope PRIVATE
    OpenSSL::Crypto nlohmann_json::nlohmann_json)
add_test(NAME platform.command_envelope COMMAND test_command_envelope)
