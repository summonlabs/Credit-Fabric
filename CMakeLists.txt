cmake_minimum_required(VERSION 3.20)

project(CreditFabric
  VERSION 1.0.0
  DESCRIPTION "Vendor-neutral C++20 runtime for credit-based flow-control authority"
  LANGUAGES CXX)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
include(CTest)

option(CREDITFABRIC_BUILD_TESTS "Build the Credit Fabric test suite" ON)
option(CREDITFABRIC_BUILD_APPS "Build the Credit Fabric command line tools" ON)
option(CREDITFABRIC_WARNINGS_AS_ERRORS "Treat first-party compiler warnings as errors" ON)
option(CREDITFABRIC_ENABLE_ASAN "Build with AddressSanitizer when the toolchain supports it" OFF)

set(CREDITFABRIC_SOURCES
  src/authority.cpp
  src/bench.cpp
  src/coordinator.cpp
  src/crc32c.cpp
  src/engine.cpp
  src/file_journal.cpp
  src/journal.cpp
  src/ledger.cpp
  src/platform.cpp
  src/policy.cpp
  src/record.cpp
  src/status.cpp
  src/strong.cpp
  src/transport.cpp
  src/version.cpp
  src/wire.cpp)

add_library(creditfabric ${CREDITFABRIC_SOURCES})
add_library(CreditFabric::creditfabric ALIAS creditfabric)
set_target_properties(creditfabric PROPERTIES
  CXX_STANDARD 20
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF
  POSITION_INDEPENDENT_CODE ON)

target_compile_features(creditfabric PUBLIC cxx_std_20)
target_include_directories(creditfabric PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

find_package(Threads REQUIRED)
target_link_libraries(creditfabric PUBLIC Threads::Threads)
if(WIN32)
  target_link_libraries(creditfabric PUBLIC ws2_32)
endif()

if(MSVC)
  target_compile_definitions(creditfabric PUBLIC _CRT_SECURE_NO_WARNINGS)
  target_compile_options(creditfabric PUBLIC /utf-8 /permissive- /Zc:__cplusplus /Zc:preprocessor)
  target_compile_options(creditfabric PRIVATE /W4)
  if(CREDITFABRIC_WARNINGS_AS_ERRORS)
    target_compile_options(creditfabric PRIVATE /WX)
  endif()
  if(CREDITFABRIC_ENABLE_ASAN)
    target_compile_options(creditfabric PUBLIC /fsanitize=address)
    target_link_options(creditfabric PUBLIC /fsanitize=address)
  endif()
else()
  target_compile_options(creditfabric PRIVATE -Wall -Wextra -Wpedantic -Wshadow -Wconversion)
  if(CREDITFABRIC_WARNINGS_AS_ERRORS)
    target_compile_options(creditfabric PRIVATE -Werror)
  endif()
  if(CREDITFABRIC_ENABLE_ASAN)
    target_compile_options(creditfabric PUBLIC -fsanitize=address -fno-omit-frame-pointer)
    target_link_options(creditfabric PUBLIC -fsanitize=address -fno-omit-frame-pointer)
  endif()
endif()

# ---------------------------------------------------------------------------
# Install / export
# ---------------------------------------------------------------------------

install(TARGETS creditfabric
  EXPORT CreditFabricTargets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(EXPORT CreditFabricTargets
  FILE CreditFabricTargets.cmake
  NAMESPACE CreditFabric::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/CreditFabric)

configure_package_config_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/cmake/CreditFabricConfig.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/CreditFabricConfig.cmake
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/CreditFabric)

write_basic_package_version_file(
  ${CMAKE_CURRENT_BINARY_DIR}/CreditFabricConfigVersion.cmake
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion)

install(FILES
  ${CMAKE_CURRENT_BINARY_DIR}/CreditFabricConfig.cmake
  ${CMAKE_CURRENT_BINARY_DIR}/CreditFabricConfigVersion.cmake
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/CreditFabric)

# ---------------------------------------------------------------------------
# Applications
# ---------------------------------------------------------------------------

if(CREDITFABRIC_BUILD_APPS)
  foreach(app cfctl cf_coordinator cf_worker cf_bench)
    add_executable(${app} apps/${app}.cpp)
    target_link_libraries(${app} PRIVATE CreditFabric::creditfabric)
    set_target_properties(${app} PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
    if(MSVC)
      target_compile_options(${app} PRIVATE /W4)
      if(CREDITFABRIC_WARNINGS_AS_ERRORS)
        target_compile_options(${app} PRIVATE /WX)
      endif()
    else()
      target_compile_options(${app} PRIVATE -Wall -Wextra -Wpedantic)
      if(CREDITFABRIC_WARNINGS_AS_ERRORS)
        target_compile_options(${app} PRIVATE -Werror)
      endif()
    endif()
    install(TARGETS ${app} RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
  endforeach()
endif()

if(CREDITFABRIC_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
