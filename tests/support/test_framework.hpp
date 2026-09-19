// Credit Fabric - minimal self-contained test framework.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Deliberately tiny: no external dependency, no watchdog, no timeout. A test
// that hangs is a defect to diagnose, not something to hide behind a timer.

#ifndef CREDITFABRIC_TESTS_TEST_FRAMEWORK_HPP
#define CREDITFABRIC_TESTS_TEST_FRAMEWORK_HPP

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace cftest {

using TestFn = void (*)();

struct TestCase {
  const char* name;
  const char* file;
  int line;
  TestFn fn;
};

inline std::vector<TestCase>& registry() {
  static std::vector<TestCase> cases;
  return cases;
}

inline long long& check_count() {
  static long long count = 0;
  return count;
}

inline long long& failure_count() {
  static long long count = 0;
  return count;
}

inline const char*& current_test() {
  static const char* name = "";
  return name;
}

inline bool& abort_current_test() {
  static bool abort = false;
  return abort;
}

struct Registrar {
  Registrar(const char* name, const char* file, int line, TestFn fn) {
    registry().push_back(TestCase{name, file, line, fn});
  }
};

inline void report_failure(const char* file, int line, const std::string& message) {
  ++failure_count();
  std::fprintf(stderr, "FAIL %s\n  at %s:%d\n  %s\n", current_test(), file, line, message.c_str());
}

inline int run_all(int argc, char** argv) {
  const char* filter = argc > 1 ? argv[1] : nullptr;
  int ran = 0;
  for (const TestCase& test : registry()) {
    if (filter != nullptr && std::strstr(test.name, filter) == nullptr) continue;
    current_test() = test.name;
    abort_current_test() = false;
    const long long before = failure_count();
    std::printf("[ RUN  ] %s\n", test.name);
    std::fflush(stdout);
    test.fn();
    ++ran;
    if (failure_count() == before) {
      std::printf("[  OK  ] %s\n", test.name);
    } else {
      std::printf("[ FAIL ] %s\n", test.name);
    }
    std::fflush(stdout);
  }
  std::printf("\n%d test(s) run, %lld check(s), %lld failure(s)\n", ran, check_count(), failure_count());
  std::fflush(stdout);
  return failure_count() == 0 ? 0 : 1;
}

}  // namespace cftest

/// Every test translation unit ends with CF_TEST_MAIN();.
#define CF_TEST_MAIN()                                       \
  int main(int argc, char** argv) { return ::cftest::run_all(argc, argv); }

#define CF_TEST(name)                                                                     \
  static void name();                                                                     \
  static const ::cftest::Registrar cf_registrar_##name(#name, __FILE__, __LINE__, &name); \
  static void name()

#define CF_CHECK_IMPL(expr, text)                                          \
  do {                                                                     \
    ++::cftest::check_count();                                             \
    if (!(expr)) {                                                         \
      ::cftest::report_failure(__FILE__, __LINE__, std::string(text));     \
    }                                                                      \
  } while (false)

#define CHECK(expr) CF_CHECK_IMPL(expr, std::string("expected: ") + #expr)

#define REQUIRE(expr)                                                      \
  do {                                                                     \
    ++::cftest::check_count();                                             \
    if (!(expr)) {                                                         \
      ::cftest::report_failure(__FILE__, __LINE__, std::string("required: ") + #expr); \
      return;                                                              \
    }                                                                      \
  } while (false)

#define CHECK_EQ(actual, expected)                                     \
  do {                                                                 \
    ++::cftest::check_count();                                         \
    if (!((actual) == (expected))) {                                   \
      ::cftest::report_failure(__FILE__, __LINE__,                     \
                               std::string(#actual " == " #expected)); \
    }                                                                  \
  } while (false)

#define CHECK_NE(actual, expected)                                     \
  do {                                                                 \
    ++::cftest::check_count();                                         \
    if (!((actual) != (expected))) {                                   \
      ::cftest::report_failure(__FILE__, __LINE__,                     \
                               std::string(#actual " != " #expected)); \
    }                                                                  \
  } while (false)

#endif  // CREDITFABRIC_TESTS_TEST_FRAMEWORK_HPP
