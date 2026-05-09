cmake_minimum_required(VERSION 3.14)
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
project(TerminalFinance)

# Use C++20 for modern features (Concepts, Ranges, etc.)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# FetchContent: Downloads the library source code at build time
include(FetchContent)

FetchContent_Declare(ftxui
  GIT_REPOSITORY https://github.com/ArthurSonzogni/ftxui
  GIT_TAG v5.0.0
)

FetchContent_Declare(nlohmann_json
  URL https://github.com/nlohmann/json/releases/download/v3.11.2/json.tar.xz
)

FetchContent_Declare(googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG v1.14.0
)

set(gtest_force_static_RUNTIME TRUE)

if(MSVC)
  set(CMAKE_MSVC_RUNTIME_LIBRARY_DBG "MultiThreadedDebug")
  set(CMAKE_MSVC_RUNTIME_LIBRARY_REL "MultiThreaded")
endif()

FetchContent_MakeAvailable(ftxui)
FetchContent_MakeAvailable(nlohmann_json)
FetchContent_MakeAvailable(googletest)

# --------------------------------------------------------------------------
# Platform-specific dependencies
# --------------------------------------------------------------------------

# libcurl: used on macOS (replaces winhttp for future http-client work).
# On Windows, winhttp is used directly by PlaidService; libcurl is declared
# in vcpkg.json for future IHttpClient work (0.B).
if(APPLE)
  find_package(CURL REQUIRED)
endif()

# --------------------------------------------------------------------------
# Platform-specific source files for the secret store
# --------------------------------------------------------------------------
if(WIN32)
  set(SECRET_STORE_SOURCES
    src/services/secret_store/DpapiSecretStore.cpp
  )
elseif(APPLE)
  set(SECRET_STORE_SOURCES
    src/services/secret_store/KeychainSecretStore.cpp
  )
else()
  # Linux / other POSIX: no secret store implementation in Phase 0.
  # Add a stub or libsecret implementation in a future phase.
  set(SECRET_STORE_SOURCES "")
  message(WARNING "TerminalFinance: No ISecretStore implementation for this platform. "
                  "SecretStore will be unavailable at runtime.")
endif()

# --------------------------------------------------------------------------
# Common source files (platform-independent)
# --------------------------------------------------------------------------
set(COMMON_SOURCES
  src/models/DataStore.cpp
  src/services/PlaidService.cpp
  src/services/StorageService.cpp
  src/services/DiscoveryService.cpp
  src/services/ConsolidationService.cpp
  src/utils/SyntheticGenerator.cpp
  src/utils/Validator.cpp
  src/utils/ConfigManager.cpp
  ${SECRET_STORE_SOURCES}
)

# NOTE: SecurityService.cpp is intentionally excluded from COMMON_SOURCES.
# Its v0.1 content has been moved into DpapiSecretStore.cpp / KeychainSecretStore.cpp.
# The file still exists in the tree as a documented stub.

enable_testing()

# --------------------------------------------------------------------------
# Main executable
# --------------------------------------------------------------------------
add_executable(TerminalFinance src/main.cpp ${COMMON_SOURCES})

target_link_libraries(TerminalFinance PRIVATE
    ftxui::screen
    ftxui::dom
    ftxui::component
    nlohmann_json::nlohmann_json
)

if(WIN32)
  target_link_libraries(TerminalFinance PRIVATE crypt32 winhttp advapi32)
elseif(APPLE)
  target_link_libraries(TerminalFinance PRIVATE
    "-framework Security"
    "-framework CoreFoundation"
    CURL::libcurl
  )
endif()

# --------------------------------------------------------------------------
# DataStoreTests
# --------------------------------------------------------------------------
add_executable(DataStoreTests tests/test_datastore.cpp ${COMMON_SOURCES})

if(MSVC)
  target_compile_options(DataStoreTests PRIVATE /MT)
endif()

target_link_libraries(DataStoreTests PRIVATE
    gtest
    gtest_main
    nlohmann_json::nlohmann_json
)

if(WIN32)
  target_link_libraries(DataStoreTests PRIVATE crypt32 winhttp advapi32)
elseif(APPLE)
  target_link_libraries(DataStoreTests PRIVATE
    "-framework Security"
    "-framework CoreFoundation"
    CURL::libcurl
  )
endif()

add_test(NAME DataStoreTests COMMAND DataStoreTests)
