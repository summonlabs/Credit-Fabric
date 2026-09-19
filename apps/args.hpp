// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Minimal argument reader shared by the command line tools. Every numeric
// argument is parsed with overflow checking and rejected when malformed.

#ifndef CREDITFABRIC_APPS_ARGS_HPP
#define CREDITFABRIC_APPS_ARGS_HPP

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "creditfabric/strong.hpp"

namespace cfapp {

class Args {
 public:
  Args(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) values_.emplace_back(argv[i]);
  }

  [[nodiscard]] const std::vector<std::string>& values() const { return values_; }

  [[nodiscard]] bool has(const std::string& flag) const {
    for (const std::string& value : values_) {
      if (value == flag) return true;
    }
    return false;
  }

  [[nodiscard]] std::string get(const std::string& flag, const std::string& fallback) const {
    for (std::size_t i = 0; i + 1 < values_.size(); ++i) {
      if (values_[i] == flag) return values_[i + 1];
    }
    return fallback;
  }

  [[nodiscard]] std::uint64_t number(const std::string& flag, std::uint64_t fallback, bool& ok) const {
    const std::string text = get(flag, "");
    if (text.empty()) return fallback;
    std::uint64_t value = 0;
    ok = creditfabric::parse_u64(text, value);
    return ok ? value : fallback;
  }

  [[nodiscard]] bool flag(const std::string& name, bool fallback) const {
    if (has(name)) return true;
    if (has("--no-" + name.substr(2))) return false;
    return fallback;
  }

 private:
  std::vector<std::string> values_{};
};

inline int fail(const char* message) {
  std::fprintf(stderr, "error: %s\n", message);
  return 2;
}

}  // namespace cfapp

#endif  // CREDITFABRIC_APPS_ARGS_HPP
