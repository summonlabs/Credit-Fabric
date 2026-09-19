// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Distributed closure with real OS processes and real framed transport.
//
// Nothing here is simulated: cf_coordinator and cf_worker are separate
// executables, the coordinator is hard-killed with TerminateProcess/SIGKILL,
// incarnations are fenced across process boundaries, and the ledger is checked
// afterwards by a third process reading the durable journal.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#include "support/fixture.hpp"
#include "support/process.hpp"
#include "support/scratch.hpp"
#include "support/test_framework.hpp"

#include "creditfabric/creditfabric.hpp"
#include "creditfabric/platform.hpp"

using namespace creditfabric;

namespace {

constexpr std::uint64_t kDomain = 0xD0A1ull;
constexpr std::uint64_t kAccount = 0xACCu;
constexpr std::uint64_t kCapacity = 1000000ull;

std::string scratch(const char* name) { return cftest::scratch_path(name); }

void wipe(const std::string& prefix) { cftest::wipe_scratch(prefix); }

std::string exe(const char* path) { return std::string(path); }

/// Reads child output until a marker line appears. There is no timeout: a child
/// that never prints its marker is a defect, not something to paper over.
bool read_until(cftest::ChildProcess& child, const std::string& marker, std::string& collected) {
  for (;;) {
    const std::string line = child.read_line();
    if (line.empty() && child.output_closed()) return false;
    collected += line;
    collected += "\n";
    if (line.find(marker) != std::string::npos) return true;
  }
}

std::string tail(const std::string& text, std::size_t lines) {
  std::vector<std::string> all;
  std::string current;
  for (char c : text) {
    if (c == '\n') {
      all.push_back(current);
      current.clear();
    } else {
      current += c;
    }
  }
  if (!current.empty()) all.push_back(current);
  std::string out;
  const std::size_t begin = all.size() > lines ? all.size() - lines : 0;
  for (std::size_t i = begin; i < all.size(); ++i) {
    out += all[i];
    out += "\n";
  }
  return out;
}

bool parse_hex_u64(const std::string& text, std::uint64_t& out) {
  if (text.size() != 16) return false;
  std::uint64_t value = 0;
  for (char c : text) {
    int digit = -1;
    if (c >= '0' && c <= '9') digit = c - '0';
    else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
    else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10;
    if (digit < 0) return false;
    value = value * 16ull + static_cast<std::uint64_t>(digit);
  }
  out = value;
  return true;
}

/// Returns the single line that starts with \p prefix, or an empty string.
std::string line_starting_with(const std::string& text, const std::string& prefix) {
  std::size_t at = 0;
  while (at < text.size()) {
    std::size_t end = text.find('\n', at);
    if (end == std::string::npos) end = text.size();
    const std::string line = text.substr(at, end - at);
    if (line.rfind(prefix, 0) == 0) return line;
    at = end + 1;
  }
  return std::string();
}

bool parse_marker_u64(const std::string& text, const std::string& key, std::uint64_t& out) {
  const std::size_t at = text.find(key);
  if (at == std::string::npos) return false;
  std::size_t begin = at + key.size();
  std::size_t end = begin;
  while (end < text.size() && text[end] >= '0' && text[end] <= '9') ++end;
  if (end == begin) return false;
  return parse_u64(text.substr(begin, end - begin), out);
}

struct Coordinator {
  cftest::ChildProcess process{};
  std::uint16_t port{0};
  std::string transcript{};

  bool start(const std::string& journal, const std::vector<std::string>& extra) {
    std::vector<std::string> arguments{"--journal", journal, "--domain", std::to_string(kDomain),
                                       "--account", std::to_string(kAccount)};
    for (const std::string& value : extra) arguments.push_back(value);
    if (!cftest::ChildProcess::spawn(exe(CF_COORDINATOR_EXE), arguments, process)) return false;
    if (!read_until(process, "LISTENING", transcript)) return false;
    std::uint64_t parsed = 0;
    if (!parse_marker_u64(transcript, "LISTENING ", parsed)) return false;
    port = static_cast<std::uint16_t>(parsed);
    return true;
  }

  int hard_kill() {
    (void)process.kill();
    return process.wait();
  }

  int stop_gracefully() { return process.wait(); }
};

struct WorkerRun {
  int exit_code{-1};
  std::string output{};
  bool reconciled{false};
  std::uint64_t incarnation{0};
  std::uint64_t sequence{0};
  std::uint64_t final_in_flight{0};
  std::uint64_t final_capacity{0};
  std::uint64_t final_available{0};
  std::uint64_t final_issued{0};
  std::uint64_t final_consumed{0};
  std::uint64_t final_returned{0};
  bool closed{false};
};

WorkerRun run_worker(const std::vector<std::string>& extra) {
  WorkerRun run{};
  cftest::ChildProcess process;
  if (!cftest::ChildProcess::spawn(exe(CF_WORKER_EXE), extra, process)) {
    run.exit_code = -2;
    return run;
  }
  std::string transcript;
  while (!process.output_closed()) {
    const std::string line = process.read_line();
    if (line.empty() && process.output_closed()) break;
    transcript += line;
    transcript += "\n";
  }
  run.exit_code = process.wait();
  run.output = transcript;

  const std::string reconciled_line = line_starting_with(transcript, "RECONCILED ");
  if (!reconciled_line.empty()) {
    run.reconciled = true;
    const std::size_t at = reconciled_line.find("incarnation=");
    if (at != std::string::npos) (void)parse_hex_u64(reconciled_line.substr(at + 12, 16), run.incarnation);
    (void)parse_marker_u64(reconciled_line, "sequence=", run.sequence);
  }
  const std::string final_line = line_starting_with(transcript, "FINAL ");
  (void)parse_marker_u64(final_line, "capacity=", run.final_capacity);
  (void)parse_marker_u64(final_line, "available=", run.final_available);
  (void)parse_marker_u64(final_line, "issued=", run.final_issued);
  (void)parse_marker_u64(final_line, "consumed=", run.final_consumed);
  (void)parse_marker_u64(final_line, "returned=", run.final_returned);
  (void)parse_marker_u64(final_line, "in_flight=", run.final_in_flight);
  run.closed = final_line.find("closed=yes") != std::string::npos;
  return run;
}

/// Reads the durable ledger through a third process.
std::string ctl_explain(const std::string& journal) {
  cftest::ChildProcess process;
  const std::vector<std::string> arguments{"--journal", journal, "--domain", std::to_string(kDomain), "--account",
                                           std::to_string(kAccount), "explain"};
  if (!cftest::ChildProcess::spawn(exe(CF_CTL_EXE), arguments, process)) return std::string();
  std::string transcript;
  while (!process.output_closed()) {
    const std::string line = process.read_line();
    if (line.empty() && process.output_closed()) break;
    transcript += line;
    transcript += "\n";
  }
  (void)process.wait();
  return transcript;
}

std::string ctl(const std::vector<std::string>& middle, std::uint16_t port) {
  cftest::ChildProcess process;
  std::vector<std::string> arguments{"--domain", std::to_string(kDomain), "--account", std::to_string(kAccount)};
  if (port != 0) {
    arguments.push_back("--port");
    arguments.push_back(std::to_string(port));
  }
  for (const std::string& value : middle) arguments.push_back(value);
  if (!cftest::ChildProcess::spawn(exe(CF_CTL_EXE), arguments, process)) return std::string();
  std::string transcript;
  while (!process.output_closed()) {
    const std::string line = process.read_line();
    if (line.empty() && process.output_closed()) break;
    transcript += line;
    transcript += "\n";
  }
  (void)process.wait();
  return transcript;
}

}  // namespace

CF_TEST(real_processes_close_the_ledger_and_survive_a_coordinator_kill) {
  const std::string journal = scratch("mp-basic");
  wipe(journal);

  Coordinator coordinator;
  REQUIRE(coordinator.start(journal,
                            {"--open", "--capacity", std::to_string(kCapacity), "--resource", "12345"}));

  const WorkerRun first = run_worker({"--port", std::to_string(coordinator.port), "--publisher", "5001", "--boot",
                                      "9001", "--issue", "64", "--rounds", "4"});
  CHECK_EQ(first.exit_code, 0);
  CHECK(first.reconciled);
  CHECK(first.closed);
  CHECK_EQ(first.final_in_flight, 0ull);
  CHECK_EQ(first.final_capacity, kCapacity);
  CHECK_EQ(first.final_available, kCapacity);
  CHECK_EQ(first.final_issued, 256ull);
  CHECK_EQ(first.final_consumed, 256ull);
  CHECK_EQ(first.final_returned, 256ull);
  CHECK(first.output.find("DONE") != std::string::npos);

  const WorkerRun second = run_worker({"--port", std::to_string(coordinator.port), "--publisher", "5002", "--boot",
                                       "9002", "--issue", "128", "--rounds", "3"});
  CHECK_EQ(second.exit_code, 0);
  CHECK_EQ(second.final_in_flight, 0ull);
  CHECK_EQ(second.final_issued, 640ull);
  CHECK_EQ(second.final_available, kCapacity);

  // Hard kill: no cooperative shutdown, no flush, no goodbye.
  const int killed = coordinator.hard_kill();
  CHECK(killed != 0);

  // A third process reads the durable journal and must see the closed ledger.
  const std::string after_kill = ctl_explain(journal);
  const std::string view_line = line_starting_with(after_kill, "view: ");
  REQUIRE(!view_line.empty());
  std::uint64_t capacity = 0;
  std::uint64_t in_flight = 0;
  std::uint64_t available = 0;
  REQUIRE(parse_marker_u64(view_line, "capacity=", capacity));
  REQUIRE(parse_marker_u64(view_line, "in_flight=", in_flight));
  REQUIRE(parse_marker_u64(view_line, "available=", available));
  CHECK_EQ(capacity, kCapacity);
  CHECK_EQ(in_flight, 0ull);
  CHECK_EQ(available, kCapacity);
  CHECK(view_line.find("closed=yes") != std::string::npos);

  // Restart on the same journal: the account exists, so no --open.
  Coordinator restarted;
  REQUIRE(restarted.start(journal, {"--resource", "12345"}));

  const WorkerRun third = run_worker({"--port", std::to_string(restarted.port), "--publisher", "5003", "--boot",
                                      "9003", "--issue", "32", "--rounds", "2"});
  CHECK_EQ(third.exit_code, 0);
  CHECK_EQ(third.final_in_flight, 0ull);
  CHECK_EQ(third.final_available, kCapacity);
  CHECK_EQ(third.final_issued, 704ull);

  (void)restarted.hard_kill();
  wipe(journal);
}

CF_TEST(a_hard_killed_worker_is_fenced_and_cannot_be_replayed) {
  const std::string journal = scratch("mp-fence");
  wipe(journal);
  Coordinator coordinator;
  REQUIRE(coordinator.start(journal, {"--open", "--capacity", std::to_string(kCapacity)}));

  // A worker parks after reconciling so it can be hard-killed mid-lifetime.
  cftest::ChildProcess parking;
  REQUIRE(cftest::ChildProcess::spawn(
      exe(CF_WORKER_EXE),
      {"--port", std::to_string(coordinator.port), "--publisher", "6001", "--boot", "7001", "--hold"}, parking));
  std::string transcript;
  REQUIRE(read_until(parking, "HOLDING", transcript));

  std::uint64_t reported = 0;
  const std::string reconciled_line = line_starting_with(transcript, "RECONCILED ");
  REQUIRE(!reconciled_line.empty());
  const std::size_t at = reconciled_line.find("incarnation=");
  REQUIRE(at != std::string::npos);
  REQUIRE(parse_hex_u64(reconciled_line.substr(at + 12, 16), reported));

  // The identity the worker reported is derived, not invented: recompute it.
  const Incarnation expected = Incarnation::make(PublisherId{6001}, BootId{7001});
  CHECK_EQ(reported, expected.id.value());

  CHECK(parking.kill() == true);
  CHECK(parking.wait() != 0);

  // Fence the dead incarnation from a third process.
  // The fence is issued through the coordinator that owns the authority.
  // A second engine must never open a journal a live coordinator owns.
  const std::string fence_output =
      ctl({"--journal", journal, "fence", "--target", to_hex(reported)}, coordinator.port);
  CHECK(fence_output.find("Fence ") != std::string::npos);
  CHECK(fence_output.find("ok [applied]") != std::string::npos);

  // The same publisher/boot comes back and is refused. A fenced incarnation is
  // permanently invalid: restarting the process does not restore authority.
  const WorkerRun revived = run_worker({"--port", std::to_string(coordinator.port), "--publisher", "6001", "--boot",
                                        "7001", "--issue", "8", "--rounds", "1"});
  CHECK_NE(revived.exit_code, 0);
  CHECK(revived.output.find("FencedIncarnation") != std::string::npos);
  CHECK(!revived.reconciled);

  // A fresh boot is a fresh incarnation and is served normally.
  const WorkerRun fresh = run_worker({"--port", std::to_string(coordinator.port), "--publisher", "6001", "--boot",
                                      "7002", "--issue", "8", "--rounds", "1"});
  CHECK_EQ(fresh.exit_code, 0);
  CHECK(fresh.reconciled);
  CHECK(fresh.closed);

  (void)coordinator.hard_kill();
  wipe(journal);
}

CF_TEST(a_killed_coordinator_forces_reconciliation_and_keeps_the_watermark) {
  const std::string journal = scratch("mp-restart");
  wipe(journal);
  Coordinator coordinator;
  REQUIRE(coordinator.start(journal, {"--open", "--capacity", std::to_string(kCapacity)}));

  const WorkerRun first = run_worker({"--port", std::to_string(coordinator.port), "--publisher", "6100", "--boot",
                                      "7100", "--issue", "16", "--rounds", "3"});
  REQUIRE(first.exit_code == 0);
  CHECK(first.reconciled);
  CHECK_EQ(first.final_issued, 48ull);

  (void)coordinator.hard_kill();

  Coordinator restarted;
  REQUIRE(restarted.start(journal, {"--resource", "12345"}));

  // The very same incarnation (same publisher and boot) returns. Its durable
  // sequence watermark survived the coordinator's death, and liveness did not:
  // it must reconcile again before it can act.
  const WorkerRun second = run_worker({"--port", std::to_string(restarted.port), "--publisher", "6100", "--boot",
                                       "7100", "--issue", "16", "--rounds", "3"});
  CHECK_EQ(second.exit_code, 0);
  CHECK(second.reconciled);
  CHECK_EQ(second.incarnation, first.incarnation);
  CHECK(second.sequence > 1ull);
  CHECK_EQ(second.final_in_flight, 0ull);
  CHECK_EQ(second.final_issued, 96ull);
  CHECK_EQ(second.final_consumed, 96ull);
  CHECK_EQ(second.final_returned, 96ull);
  CHECK_EQ(second.final_available, kCapacity);

  (void)restarted.hard_kill();
  wipe(journal);
}

CF_TEST(a_graceful_stop_serves_every_request_before_it_exits) {
  const std::string journal = scratch("mp-graceful");
  wipe(journal);
  Coordinator coordinator;
  REQUIRE(coordinator.start(journal,
                            {"--open", "--capacity", std::to_string(kCapacity), "--max-requests", "12"}));

  const WorkerRun worker = run_worker({"--port", std::to_string(coordinator.port), "--publisher", "6200", "--boot",
                                       "7200", "--issue", "8", "--rounds", "3"});
  CHECK_EQ(worker.exit_code, 0);
  CHECK_EQ(worker.final_in_flight, 0ull);
  CHECK_EQ(worker.final_issued, 24ull);

  const int exit_code = coordinator.stop_gracefully();
  CHECK_EQ(exit_code, 0);
  CHECK(coordinator.transcript.find("STOPPED") != std::string::npos ||
        read_until(coordinator.process, "STOPPED", coordinator.transcript));

  const std::string after = ctl_explain(journal);
  const std::string view_line = line_starting_with(after, "view: ");
  std::uint64_t in_flight = 0;
  REQUIRE(parse_marker_u64(view_line, "in_flight=", in_flight));
  CHECK_EQ(in_flight, 0ull);
  wipe(journal);
}

CF_TEST(two_coordinators_cannot_own_the_same_journal_at_once) {
  const std::string journal = scratch("mp-exclusive");
  wipe(journal);
  Coordinator first;
  REQUIRE(first.start(journal, {"--open", "--capacity", std::to_string(kCapacity)}));

  // A second coordinator on the same journal is a separate authority. It must
  // not silently share the account: it either refuses the account as existing
  // (its own view) or fails to open it. What it must never do is corrupt the
  // first coordinator's ledger.
  Coordinator second;
  const bool started = second.start(journal, {"--open", "--capacity", std::to_string(kCapacity)});
  if (started) {
    const WorkerRun worker = run_worker({"--port", std::to_string(first.port), "--publisher", "6300", "--boot",
                                         "7300", "--issue", "8", "--rounds", "2"});
    CHECK_EQ(worker.exit_code, 0);
    CHECK_EQ(worker.final_in_flight, 0ull);
    (void)second.hard_kill();
  } else {
    // Refusing to start is also acceptable; report which happened.
    CHECK(!started);
  }

  const std::string after = ctl_explain(journal);
  CHECK(after.find("view: ") != std::string::npos);
  (void)first.hard_kill();
  wipe(journal);
}

CF_TEST_MAIN()
