// Credit Fabric - real OS child processes for multiprocess closure tests.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Distributed claims are proved with real processes, not threads pretending to
// be processes. This helper spawns a real executable, captures its stdout
// through a pipe, and can hard-kill it so that crash recovery is genuinely
// exercised.

#ifndef CREDITFABRIC_TESTS_PROCESS_HPP
#define CREDITFABRIC_TESTS_PROCESS_HPP

#include <string>
#include <vector>

namespace cftest {

class ChildProcess {
 public:
  ChildProcess() = default;
  ~ChildProcess();

  ChildProcess(const ChildProcess&) = delete;
  ChildProcess& operator=(const ChildProcess&) = delete;
  ChildProcess(ChildProcess&& other) noexcept;
  ChildProcess& operator=(ChildProcess&& other) noexcept;

  /// Spawns \p executable with \p arguments, capturing stdout and stderr.
  static bool spawn(const std::string& executable, const std::vector<std::string>& arguments, ChildProcess& out);

  [[nodiscard]] bool running();
  /// Hard kill (SIGKILL equivalent). No cooperative shutdown happens.
  bool kill();
  /// Waits for exit and returns the exit code, or -1 when unavailable.
  int wait();

  /// Blocks until one line of child output is available (no timeout: a hang is
  /// a defect, not something to paper over).
  [[nodiscard]] std::string read_line();
  /// Reads whatever output is available without blocking; empty when none.
  [[nodiscard]] std::string drain();
  [[nodiscard]] bool output_closed() const noexcept { return output_closed_; }
  [[nodiscard]] int process_id() const noexcept { return pid_; }

 private:
  void* process_{nullptr};
  void* output_read_{nullptr};
  int pid_{0};
  bool exited_{false};
  int exit_code_{-1};
  bool output_closed_{false};
  std::string buffer_{};
};

}  // namespace cftest

#endif  // CREDITFABRIC_TESTS_PROCESS_HPP
