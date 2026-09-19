// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// cf_coordinator - a real OS process that owns the only CreditEngine instance
// and serves it over loopback framed transport.
//
// It prints "LISTENING <port>" once bound, then serves until it is asked to
// stop (--max-requests) or is hard-killed. A hard kill is a supported outcome:
// the journal is the authority, and the next boot revalidates everything.

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>

#include "args.hpp"
#include "local_session.hpp"

using namespace creditfabric;

int main(int argc, char** argv) {
  cfapp::Args args(argc, argv);
  const std::string journal = args.get("--journal", "");
  if (journal.empty()) return cfapp::fail("--journal PREFIX is required");

  bool ok = true;
  const std::uint64_t port = args.number("--port", 0, ok);
  const std::uint64_t domain_value = args.number("--domain", 0xD0A1ull, ok);
  const std::uint64_t account_value = args.number("--account", 0xACCu, ok);
  const std::uint64_t resource_value = args.number("--resource", 0x2E50ull, ok);
  const std::uint64_t capacity = args.number("--capacity", 4096, ok);
  const std::uint64_t max_capacity = args.number("--max-capacity", capacity, ok);
  const std::uint64_t max_requests = args.number("--max-requests", 0, ok);
  const std::uint64_t window = args.number("--limit-window", 4096, ok);
  if (!ok) return cfapp::fail("a numeric option was not a valid unsigned integer");

  const DomainId domain{domain_value};
  const AccountId account{account_value};
  const Incarnation me = cfapp::durable_operator(journal);

  EngineLimits limits{};
  limits.max_attempt_window = static_cast<std::size_t>(window);

  FileJournalOptions journal_options{};
  journal_options.prefix = journal;
  auto engine = std::make_unique<CreditEngine>(limits, std::make_unique<FileJournal>(journal_options), me);
  const Status recovered = engine->recover();
  if (!recovered.ok()) {
    std::fprintf(stderr, "recovery failed: %s\n", recovered.describe().c_str());
    return 3;
  }

  if (args.has("--open")) {
    cfapp::LocalSession session(*engine, me, domain, account);
    AccountConfig config{};
    config.domain = domain;
    config.account = account;
    config.resource = ResourceId{resource_value};
    config.policy = PolicyId{1};
    config.capacity = capacity;
    config.max_capacity = max_capacity;
    config.profile = ProfileKind::Synthetic;
    config.rules.max_issue_per_attempt = capacity;
    config.rules.allow_protect = true;
    config.rules.allow_return = true;
    const CreditOutcome outcome = session.open(config);
    if (!outcome.ok()) {
      std::fprintf(stderr, "open refused: %s\n", outcome.status.describe().c_str());
      return 4;
    }
    std::printf("OPENED account=%llu capacity=%llu\n", static_cast<unsigned long long>(account.value()),
                static_cast<unsigned long long>(capacity));
    std::fflush(stdout);
  }

  CoordinatorOptions options{};
  options.port = static_cast<std::uint16_t>(port);
  options.limits = limits;
  options.server_label = args.get("--label", "creditfabric-coordinator");
  options.max_requests_per_connection = 0;

  CoordinatorServer server(*engine, options);
  const Status started = server.start();
  if (!started.ok()) {
    std::fprintf(stderr, "listen failed: %s\n", started.describe().c_str());
    return 5;
  }
  std::printf("LISTENING %u\n", static_cast<unsigned>(server.port()));
  std::fflush(stdout);

  if (max_requests != 0) {
    while (server.requests_served() < max_requests) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
      if (!server.running()) break;
    }
  } else {
    for (;;) {
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
  }

  (void)server.stop();
  std::printf("STOPPED served=%llu\n", static_cast<unsigned long long>(server.requests_served()));
  std::fflush(stdout);
  return 0;
}
