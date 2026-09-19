// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// cfctl - operator command line for Credit Fabric.
//
// Two modes, both real:
//   --journal PREFIX          offline: cfctl owns the durable journal directly
//   --journal PREFIX --port P remote: cfctl speaks to the running coordinator
//
// An offline invocation must not run against a journal that a live coordinator
// owns; exactly one engine may hold a journal at a time. Every invocation is a
// fresh process, so the durable operator incarnation must revalidate against
// the current authority before any of its attempts can be admitted.

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>

#include "args.hpp"
#include "local_session.hpp"

#include "creditfabric/creditfabric.hpp"

using namespace creditfabric;

namespace {

void print_outcome(const CreditOutcome& outcome) {
  std::printf("%s\n", outcome.describe().c_str());
  std::printf("  view: %s\n", outcome.view.to_string().c_str());
}

int usage() {
  std::fprintf(stderr,
               "usage: cfctl --journal PREFIX [--port P] [--domain D] [--account A] <command> [options]\n"
               "  open        --resource R --capacity C [--max-capacity M] [--profile NAME] [--max-issue N] [--allow-growth]\n"
               "  explain | describe\n"
               "  issue       --count N [--producer P]\n"
               "  consume     --count N\n"
               "  return      --count N\n"
               "  protect     --count N\n"
               "  unprotect   --count N\n"
               "  reconcile\n"
               "  capacity    --value C\n"
               "  epoch       [--to N]\n"
               "  fence       --target HEX\n"
               "  retire      --count N\n"
               "  revalidate  --count N --as returned|consumed\n"
               "  exhaust     --reason NAME\n"
               "  clear-exhaustion\n"
               "  snapshot\n"
               "  bench       [--operations N] [--capacity C]\n");
  return 2;
}

bool parse_hex_id(const std::string& text, std::uint64_t& out) {
  if (text.empty() || text.size() > 16) return false;
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

ProfileKind parse_profile(const std::string& name) {
  if (name == "SYNTHETIC") return ProfileKind::Synthetic;
  if (name == "PHYSICAL-UNVALIDATED") return ProfileKind::PhysicalUnvalidated;
  return ProfileKind::Abstract;
}

Reason parse_reason(const std::string& name) {
  for (std::uint16_t raw = 0; raw < kReasonCount; ++raw) {
    const auto reason = static_cast<Reason>(raw);
    if (name == to_string(reason)) return reason;
  }
  return Reason::PolicyRefused;
}

struct Options {
  std::string journal{};
  std::string command{};
  std::uint64_t domain{0xD0A1ull};
  std::uint64_t account{0xACCu};
  std::uint64_t resource{0x2E50ull};
  std::uint64_t capacity{1024};
  std::uint64_t max_capacity{0};
  std::uint64_t max_issue{0};
  std::uint64_t count{0};
  std::uint64_t value{0};
  std::uint64_t window{4096};
  std::uint64_t port{0};
  std::uint64_t producer{0};
  std::uint64_t target{0};
  bool allow_growth{false};
  ProfileKind profile{ProfileKind::Abstract};
  Reason reason{Reason::PolicyRefused};
  RevalidationDecision decision{RevalidationDecision::None};
  std::string host{"127.0.0.1"};
};

bool parse_options(const cfapp::Args& args, Options& out) {
  bool ok = true;
  out.journal = args.get("--journal", "");
  out.host = args.get("--host", "127.0.0.1");
  out.domain = args.number("--domain", out.domain, ok);
  out.account = args.number("--account", out.account, ok);
  out.resource = args.number("--resource", out.resource, ok);
  out.capacity = args.number("--capacity", out.capacity, ok);
  out.max_capacity = args.number("--max-capacity", out.capacity, ok);
  out.max_issue = args.number("--max-issue", out.capacity, ok);
  out.count = args.number("--count", 0, ok);
  if (args.has("--to")) out.count = args.number("--to", out.count, ok);
  out.value = args.number("--value", 0, ok);
  out.window = args.number("--limit-window", 4096, ok);
  out.port = args.number("--port", 0, ok);
  out.producer = args.number("--producer", 0, ok);
  if (!ok) return false;
  out.allow_growth = args.has("--allow-growth");
  out.profile = parse_profile(args.get("--profile", "ABSTRACT"));
  out.reason = parse_reason(args.get("--reason", "PolicyRefused"));
  if (args.get("--as", "returned") == "consumed") out.decision = RevalidationDecision::ToConsumed;
  if (args.has("--as")) out.decision = (args.get("--as", "returned") == "consumed") ? RevalidationDecision::ToConsumed
                                                                                   : RevalidationDecision::ToReturned;
  const std::string target_text = args.get("--target", "");
  if (!target_text.empty() && !parse_hex_id(target_text, out.target)) return false;

  for (std::size_t i = 0; i < args.values().size(); ++i) {
    const std::string& value = args.values()[i];
    if (value.rfind("--", 0) == 0) {
      ++i;
      continue;
    }
    out.command = value;
    break;
  }
  return true;
}

/// Builds the request a command corresponds to. Returns false for commands that
/// are not single attempts.
bool build_request(const Options& options, CreditRequest& request, bool& is_describe, bool& is_snapshot) {
  is_describe = false;
  is_snapshot = false;
  request = CreditRequest{};
  request.count = options.count;
  request.new_capacity = options.value;
  request.resource = ResourceId{options.resource};
  request.producer = ProducerId{options.producer};
  request.fence_cause = options.reason;
  request.decision = options.decision;
  request.target = IncarnationId{options.target};

  if (options.command == "issue") {
    request.op = OpKind::Issue;
  } else if (options.command == "consume") {
    request.op = OpKind::Consume;
  } else if (options.command == "return") {
    request.op = OpKind::Return;
  } else if (options.command == "protect") {
    request.op = OpKind::Protect;
  } else if (options.command == "unprotect") {
    request.op = OpKind::Unprotect;
  } else if (options.command == "capacity") {
    request.op = OpKind::SetCapacity;
  } else if (options.command == "epoch") {
    if (options.count != 0) {
      request.op = OpKind::Rotate;
      request.count = options.count;
    } else {
      request.op = OpKind::AdvanceEpoch;
    }
  } else if (options.command == "fence") {
    request.op = OpKind::Fence;
  } else if (options.command == "retire") {
    request.op = OpKind::Retire;
  } else if (options.command == "revalidate") {
    request.op = OpKind::Revalidate;
  } else if (options.command == "exhaust") {
    request.op = OpKind::Exhaust;
  } else if (options.command == "clear-exhaustion") {
    request.op = OpKind::ClearExhaustion;
  } else if (options.command == "reconcile") {
    request.op = OpKind::Reconcile;
  } else if (options.command == "describe" || options.command == "explain") {
    request.op = OpKind::Describe;
    is_describe = true;
  } else if (options.command == "snapshot") {
    is_snapshot = true;
    return false;
  } else {
    return false;
  }
  return true;
}

int run_offline(const Options& options, const Incarnation& me) {
  EngineLimits limits{};
  limits.max_attempt_window = static_cast<std::size_t>(options.window);
  FileJournalOptions journal_options{};
  journal_options.prefix = options.journal;
  auto engine = std::make_unique<CreditEngine>(limits, std::make_unique<FileJournal>(journal_options), me);
  const Status recovered = engine->recover();
  if (!recovered.ok()) {
    std::fprintf(stderr, "recovery failed: %s\n", recovered.describe().c_str());
    return 3;
  }

  const DomainId domain{options.domain};
  const AccountId account{options.account};
  cfapp::LocalSession session(*engine, me, domain, account);

  if (options.command == "bench") {
    BenchConfig config{};
    config.operations = options.count == 0 ? 20000 : options.count;
    config.capacity = options.capacity;
    const BenchResult result = run_synthetic_ledger_benchmark(config);
    std::printf("%s", render(result).c_str());
    return result.closure_ok ? 0 : 4;
  }

  if (options.command == "open") {
    AccountConfig config{};
    config.domain = domain;
    config.account = account;
    config.resource = ResourceId{options.resource};
    config.policy = PolicyId{1};
    config.capacity = options.capacity;
    config.max_capacity = options.max_capacity;
    config.profile = options.profile;
    config.rules.max_issue_per_attempt = options.max_issue;
    config.rules.allow_capacity_growth = options.allow_growth;
    const CreditOutcome outcome = session.open(config);
    print_outcome(outcome);
    return outcome.ok() ? 0 : 1;
  }

  if (options.command == "snapshot") {
    const Status status = engine->snapshot();
    if (!status.ok()) {
      std::fprintf(stderr, "snapshot failed: %s\n", status.describe().c_str());
      return 1;
    }
    std::printf("snapshot durable\n");
    std::printf("%s", render(session.explain()).c_str());
    return 0;
  }

  if (options.command == "explain" || options.command == "describe") {
    const Explanation explanation = session.explain();
    std::printf("%s", render(explanation).c_str());
    return explanation.found ? 0 : 1;
  }

  if (options.command == "reconcile") {
    const Status status = session.reconcile();
    if (!status.ok()) {
      std::fprintf(stderr, "reconcile refused: %s\n", status.describe().c_str());
      return 1;
    }
    std::printf("reconciled incarnation %s at sequence %llu\n", to_hex(me.id.value()).c_str(),
                static_cast<unsigned long long>(session.next_sequence()));
    std::printf("%s", render(session.explain()).c_str());
    return 0;
  }

  CreditRequest request{};
  bool is_describe = false;
  bool is_snapshot = false;
  if (!build_request(options, request, is_describe, is_snapshot)) return usage();
  const CreditOutcome outcome = session.call(request);
  print_outcome(outcome);
  std::printf("%s", render(session.explain()).c_str());
  return outcome.ok() ? 0 : 1;
}

int run_remote(const Options& options, const Incarnation& me) {
  ClientOptions client_options{};
  client_options.host = options.host;
  client_options.port = static_cast<std::uint16_t>(options.port);
  client_options.domain = DomainId{options.domain};
  client_options.publisher = me.publisher;
  client_options.boot = me.boot;
  client_options.label = "cfctl";

  CoordinatorClient client(client_options);
  const Status connected = client.connect();
  if (!connected.ok()) {
    std::fprintf(stderr, "connect failed: %s\n", connected.describe().c_str());
    return 3;
  }
  const AccountId account{options.account};

  if (options.command == "bench") {
    std::fprintf(stderr, "bench runs against a local engine only\n");
    return 2;
  }
  if (options.command == "snapshot") {
    std::fprintf(stderr, "snapshot is an operator action on the owning process\n");
    return 2;
  }

  if (options.command == "explain" || options.command == "describe") {
    const CreditOutcome described = client.describe(account);
    print_outcome(described);
    std::printf("authority: epoch=%llu generation=%llu\n",
                static_cast<unsigned long long>(described.authority.epoch.value()),
                static_cast<unsigned long long>(described.authority.generation.value()));
    client.close();
    return described.ok() ? 0 : 1;
  }

  if (options.command == "open") {
    CreditRequest request{};
    request.op = OpKind::Open;
    request.config.domain = DomainId{options.domain};
    request.config.account = account;
    request.config.resource = ResourceId{options.resource};
    request.config.policy = PolicyId{1};
    request.config.capacity = options.capacity;
    request.config.max_capacity = options.max_capacity;
    request.config.profile = options.profile;
    request.config.rules.max_issue_per_attempt = options.max_issue;
    request.config.rules.allow_capacity_growth = options.allow_growth;
    const CreditOutcome outcome = client.submit(request);
    print_outcome(outcome);
    client.close();
    return outcome.ok() ? 0 : 1;
  }

  if (options.command == "reconcile") {
    const CreditOutcome outcome = client.reconcile(account);
    print_outcome(outcome);
    std::printf("reconciled incarnation %s at sequence %llu\n", to_hex(me.id.value()).c_str(),
                static_cast<unsigned long long>(client.next_sequence()));
    client.close();
    return outcome.ok() ? 0 : 1;
  }

  CreditRequest request{};
  bool is_describe = false;
  bool is_snapshot = false;
  if (!build_request(options, request, is_describe, is_snapshot)) return usage();
  request.authority.account = account;
  const CreditOutcome outcome = client.submit(request);
  print_outcome(outcome);
  client.close();
  return outcome.ok() ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
  cfapp::Args args(argc, argv);
  Options options{};
  if (!parse_options(args, options)) {
    std::fprintf(stderr, "error: an option was not a valid value\n");
    return 2;
  }
  if (options.journal.empty() || options.command.empty()) return usage();

  const Incarnation me = cfapp::durable_operator(options.journal);
  if (options.port != 0) return run_remote(options, me);
  return run_offline(options, me);
}
