// Credit Fabric - absolute scratch paths for durable test artefacts.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Durable tests write real journal and snapshot files. The root is supplied by
// the build as an absolute path so that a test behaves identically no matter
// which directory the runner started in.

#ifndef CREDITFABRIC_TESTS_SCRATCH_HPP
#define CREDITFABRIC_TESTS_SCRATCH_HPP

#include <string>

#include "creditfabric/platform.hpp"

#ifndef CF_TEST_SCRATCH
#define CF_TEST_SCRATCH "test-scratch/"
#endif

namespace cftest {

inline std::string scratch_path(const char* name) {
  std::string prefix = CF_TEST_SCRATCH;
  prefix += name;
  return prefix;
}

inline void wipe_scratch(const std::string& prefix) {
  (void)creditfabric::remove_file(prefix + ".jrnl");
  (void)creditfabric::remove_file(prefix + ".snap");
  (void)creditfabric::remove_file(prefix + ".snap.tmp");
}

}  // namespace cftest

#endif  // CREDITFABRIC_TESTS_SCRATCH_HPP
