# Credit Fabric test suite.
#
# Deliberately no CTest TIMEOUT property anywhere: a hanging test is a defect to
# diagnose, not something to hide behind watchdog termination.

set(CREDITFABRIC_TESTS
  test_checked
  test_ledger
  test_engine
  test_authority
  test_idempotency
  test_persistence
  test_wire
  test_explain
  test_concurrency
  test_property
  test_bench
  test_transport
  test_multiprocess
  test_hardening)

add_library(cf_test_support STATIC
  support/process.cpp)
target_include_directories(cf_test_support PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(cf_test_support PUBLIC
  CF_TEST_SCRATCH="${CMAKE_CURRENT_BINARY_DIR}/scratch/")
target_link_libraries(cf_test_support PUBLIC CreditFabric::creditfabric)
set_target_properties(cf_test_support PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
if(MSVC)
  target_compile_definitions(cf_test_support PUBLIC _CRT_SECURE_NO_WARNINGS)
  target_compile_options(cf_test_support PRIVATE /W4 /utf-8)
else()
  target_compile_options(cf_test_support PRIVATE -Wall -Wextra)
endif()

foreach(test_name IN LISTS CREDITFABRIC_TESTS)
  if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${test_name}.cpp")
    continue()
  endif()
  add_executable(${test_name} ${test_name}.cpp)
  target_link_libraries(${test_name} PRIVATE cf_test_support)
  set_target_properties(${test_name} PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
  if(MSVC)
    target_compile_definitions(${test_name} PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_options(${test_name} PRIVATE /W4 /utf-8)
    if(CREDITFABRIC_WARNINGS_AS_ERRORS)
      target_compile_options(${test_name} PRIVATE /WX)
    endif()
  else()
    target_compile_options(${test_name} PRIVATE -Wall -Wextra -Wpedantic)
    if(CREDITFABRIC_WARNINGS_AS_ERRORS)
      target_compile_options(${test_name} PRIVATE -Werror)
    endif()
  endif()
  add_test(NAME ${test_name} COMMAND ${test_name})
endforeach()

# Real multiprocess closure: the test drives the real coordinator and worker
# binaries as separate OS processes.
if(TARGET cf_coordinator AND TARGET cf_worker AND TARGET cfctl)
  if(TARGET test_multiprocess)
    target_compile_definitions(test_multiprocess PRIVATE
      CF_COORDINATOR_EXE="$<TARGET_FILE:cf_coordinator>"
      CF_WORKER_EXE="$<TARGET_FILE:cf_worker>"
      CF_CTL_EXE="$<TARGET_FILE:cfctl>")
  endif()
endif()
