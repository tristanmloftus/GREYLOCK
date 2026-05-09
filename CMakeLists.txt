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

set(CMAKE_MSVC_RUNTIME_LIBRARY_DBG "MultiThreadedDebug")
set(CMAKE_MSVC_RUNTIME_LIBRARY_REL "MultiThreaded")

FetchContent_MakeAvailable(ftxui)
FetchContent_MakeAvailable(nlohmann_json)
FetchContent_MakeAvailable(googletest)

enable_testing()

# Tell CMake to create an executable from our source files
add_executable(TerminalFinance src/main.cpp src/models/DataStore.cpp src/services/PlaidService.cpp src/services/StorageService.cpp src/services/DiscoveryService.cpp src/services/SecurityService.cpp src/services/ConsolidationService.cpp src/utils/SyntheticGenerator.cpp src/utils/Validator.cpp src/utils/ConfigManager.cpp)

# Add 'ftxui::component' to the list
target_link_libraries(TerminalFinance PRIVATE
    ftxui::screen
    ftxui::dom
    ftxui::component
)

# Link the ftxui and nlohmann_json libraries to our executable
target_link_libraries(TerminalFinance PRIVATE ftxui::screen ftxui::dom nlohmann_json::nlohmann_json crypt32 winhttp advapi32)

# Tests - use static runtime to match GoogleTest
add_executable(DataStoreTests tests/test_datastore.cpp src/models/DataStore.cpp src/services/StorageService.cpp src/services/DiscoveryService.cpp src/utils/SyntheticGenerator.cpp src/utils/Validator.cpp src/utils/ConfigManager.cpp src/services/SecurityService.cpp src/services/ConsolidationService.cpp src/services/PlaidService.cpp)
target_compile_options(DataStoreTests PRIVATE /MT)
target_link_libraries(DataStoreTests PRIVATE gtest gtest_main nlohmann_json::nlohmann_json crypt32 winhttp advapi32)
add_test(NAME DataStoreTests COMMAND DataStoreTests)