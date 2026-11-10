cmake_minimum_required(VERSION 3.28)
project(qoderz VERSION 0.1.0 LANGUAGES CXX)

include(GNUInstallDirs)
include(CTest)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 14)
  message(FATAL_ERROR "qoderz requires GCC 14 or newer (found ${CMAKE_CXX_COMPILER_VERSION})")
endif()

find_package(Boost 1.83 REQUIRED CONFIG)
find_package(OpenSSL REQUIRED)
find_package(nlohmann_json 3.11 REQUIRED)
find_package(CLI11 2.3 REQUIRED)

file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/generated/common")
configure_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/src/common/version.hpp.in"
  "${CMAKE_BINARY_DIR}/generated/common/version.hpp"
  @ONLY
)

add_library(qoderz_core STATIC src/common/log.cpp)
add_library(qoderz::core ALIAS qoderz_core)

target_include_directories(qoderz_core
  PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_BINARY_DIR}/generated"
)

target_link_libraries(qoderz_core PUBLIC nlohmann_json::nlohmann_json)
target_compile_features(qoderz_core PUBLIC cxx_std_23)
target_compile_options(qoderz_core PRIVATE -Wall -Wextra -Wpedantic -Wshadow)

add_executable(qoderz src/main.cpp)
target_link_libraries(qoderz
  PRIVATE
    qoderz_core
    CLI11::CLI11
    Boost::headers
    OpenSSL::SSL
    OpenSSL::Crypto
)
target_compile_options(qoderz PRIVATE -Wall -Wextra -Wpedantic -Wshadow)

install(TARGETS qoderz RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")

if(BUILD_TESTING)
  find_package(Catch2 3 REQUIRED)
  add_subdirectory(tests)
endif()
