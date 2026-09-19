// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "creditfabric/bench.hpp"

#include <algorithm>
#include <chrono>
#include <memory>
#include <string>
#include <vector>

#include "creditfabric/file_journal.hpp"
#include "creditfabric/journal.hpp"
#include "creditfabric/platform.hpp"

namespace creditfabric {
namespace {

struct Wallet {
  Incarnation incarnation{};
  std::uint64_t sequence{1};
  std::uint64_t credits{0};   ///< spendable credit currently granted to this wallet
  std::uint64_t in_flight{0}; ///< issued minus returned for this wallet
  AttemptId last_attempt{};
  std::uint64_t last_sequence{0};
  OpKind last_op{OpKind::Issue};
  std::uint64_t last_count{0};
};

/// Submits one attempt for a wallet. When duplicate is set the exact previous
/// attempt identity is replayed, so idempotency is measured rather than assumed.
CreditOutcome submit(CreditEngine& engine, Wallet& wallet, OpKind op, std::uint64_t count,
                     const AuthorityVector& authority, AttemptIdGenerator& attempts, bool duplicate) {
  CreditRequest request{};
  request.presenter = wallet.incarnation;
  request.authority = authority;
  if (duplicate && wallet.last_sequence != 0) {
    request.op = wallet.last_op;
    request.count = wallet.last_count;
    request.attempt = wallet.last_attempt;
    request.sequence = wallet.last_sequence;
  } else {
    request.op = op;
    request.count = count;
    request.attempt = attempts.next();
    request.sequence = wallet.sequence;
    wallet.last_attempt = request.attempt;
    wallet.last_sequence = request.sequence;
    wallet.last_op = op;
    wallet.last_count = count;
  }
  const CreditOutcome outcome = engine.apply(request);
  if (outcome.decided && !outcome.duplicate) wallet.sequence = request.sequence + 1;
  return outcome;
}

}  // namespace

void tally_refusal(BenchResult& result, Reason reason) {
  const std::string name = to_string(reason);
  for (std::pair<std::string, std::uint64_t>& entry : result.refusal_reasons) {
    if (entry.first == name) {
      ++entry.second;
      return;
    }
  }
  if (result.refusal_reasons.size() >= 16) return;
  result.refusal_reasons.emplace_back(name, 1);
}

BenchResult run_synthetic_ledger_benchmark(const BenchConfig& config) {
  BenchResult result{};
  result.scenario = config.scenario;
  result.provenance = "SYNTHETIC";

  EngineLimits limits{};
  limits.max_attempt_window = 4096;

  std::unique_ptr<Journal> journal;
  if (config.durable) {
    FileJournalOptions options{};
    options.prefix = config.journal_prefix.empty() ? std::string("creditfabric-bench") : config.journal_prefix;
    options.max_bytes = 4ull << 30;
    options.records_per_snapshot = 1u << 20;
    journal = std::make_unique<FileJournal>(options);
  } else {
    journal = std::make_unique<InMemoryJournal>(4ull << 30);
  }

  const Incarnation operator_incarnation = Incarnation::make(PublisherId{0xB00Bull}, BootId{make_boot_id()});
  CreditEngine engine(limits, std::move(journal), operator_incarnation);
  const Status recovered = engine.recover();
  if (!recovered.ok()) {
    result.scenario += " (recovery failed: ";
    result.scenario += to_string(recovered.reason);
    result.scenario += ")";
    return result;
  }

  const std::size_t worker_count = std::max<std::size_t>(1, config.producers + config.consumers);
  // Grant size per issue, chosen so that the synthetic population can never
  // approach the account capacity and the benchmark measures ledger decisions
  // rather than exhaustion behaviour.
  const std::uint64_t divisor = static_cast<std::uint64_t>(worker_count) * 4ull;
  const std::uint64_t grant = std::max<std::uint64_t>(1ull, config.capacity / divisor);

  const DomainId domain{0x1D0A1Dull};
  const AccountId account{0xACCu};
  const ResourceId resource{0x2E50ull};
  AccountConfig account_config{};
  account_config.domain = domain;
  account_config.account = account;
  account_config.resource = resource;
  account_config.policy = PolicyId{1};
  account_config.capacity = config.capacity;
  account_config.max_capacity = config.capacity;
  account_config.profile = ProfileKind::Synthetic;
  account_config.rules.min_issue = 1;
  account_config.rules.max_issue_per_attempt = grant;

  {
    AuthorityVector authority{};
    authority.domain = domain;
    authority.account = account;
    authority.epoch = EpochId{1};
    authority.generation = Generation{1};
    authority.incarnation = operator_incarnation.id;
    CreditRequest request{};
    request.op = OpKind::Open;
    request.attempt = AttemptIdGenerator{0x0F1CEull}.next();
    request.sequence = 1;
    request.presenter = operator_incarnation;
    request.authority = authority;
    request.config = account_config;
    request.proof = engine.challenge().prove(operator_incarnation, authority);
    const CreditOutcome outcome = engine.apply(request);
    if (!outcome.ok()) {
      result.scenario += " (open refused: ";
      result.scenario += to_string(outcome.status.reason);
      result.scenario += ")";
      return result;
    }
  }

  AuthorityVector authority{};
  authority.domain = domain;
  authority.account = account;
  authority.epoch = EpochId{1};
  authority.generation = Generation{1};
  authority.incarnation = operator_incarnation.id;

  AttemptIdGenerator attempts{0xBEEFCAFEull};
  std::vector<Wallet> wallets;
  for (std::size_t i = 0; i < worker_count; ++i) {
    Wallet wallet{};
    wallet.incarnation = Incarnation::make(PublisherId{0x1000ull + i}, BootId{make_boot_id()});
    AuthorityVector worker_authority = authority;
    worker_authority.incarnation = wallet.incarnation.id;
    CreditRequest request{};
    request.op = OpKind::Reconcile;
    request.attempt = attempts.next();
    request.sequence = 1;
    request.presenter = wallet.incarnation;
    request.authority = worker_authority;
    request.proof = engine.challenge().prove(wallet.incarnation, worker_authority);
    const CreditOutcome outcome = engine.apply(request);
    if (!outcome.ok()) {
      result.scenario += " (reconcile refused: ";
      result.scenario += to_string(outcome.status.reason);
      result.scenario += ")";
      return result;
    }
    wallet.sequence = 2;
    wallets.push_back(wallet);
  }

  std::uint64_t state = config.seed | 1ull;
  const auto next_random = [&state]() noexcept {
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    return state;
  };

  const auto started = std::chrono::steady_clock::now();
  for (std::uint64_t operation = 0; operation < config.operations; ++operation) {
    Wallet& wallet = wallets[static_cast<std::size_t>(next_random() % wallets.size())];
    AuthorityVector worker_authority = authority;
    worker_authority.incarnation = wallet.incarnation.id;
    const std::uint64_t draw = next_random();

    if (wallet.last_sequence != 0 && (draw % 97ull) == 0ull) {
      ++result.operations_attempted;
      const CreditOutcome replay = submit(engine, wallet, wallet.last_op, wallet.last_count, worker_authority, attempts,
                                          true);
      if (replay.duplicate && replay.status.ok()) {
        ++result.duplicates_absorbed;
        ++result.operations_completed;
      } else if (replay.status.ok()) {
        ++result.operations_completed;
      } else {
        ++result.refusals;
        tally_refusal(result, replay.status.reason);
      }
      continue;
    }

    if (wallet.credits == 0) {
      const std::uint64_t count = 1 + (next_random() % grant);
      ++result.operations_attempted;
      const CreditOutcome outcome = submit(engine, wallet, OpKind::Issue, count, worker_authority, attempts, false);
      if (!outcome.ok()) {
        ++result.refusals;
        tally_refusal(result, outcome.status.reason);
        continue;
      }
      wallet.credits += count;
      wallet.in_flight += count;
      result.issued += count;
      ++result.operations_completed;
      continue;
    }

    if ((draw % 3ull) == 0ull) {
      const std::uint64_t amount = 1 + (next_random() % wallet.credits);
      ++result.operations_attempted;
      const CreditOutcome outcome = submit(engine, wallet, OpKind::Return, amount, worker_authority, attempts, false);
      if (!outcome.ok()) {
        ++result.refusals;
        tally_refusal(result, outcome.status.reason);
        continue;
      }
      wallet.credits -= amount;
      wallet.in_flight -= amount;
      result.returned += amount;
      ++result.operations_completed;
      continue;
    }

    ++result.operations_attempted;
    const CreditOutcome outcome = submit(engine, wallet, OpKind::Consume, 1, worker_authority, attempts, false);
    if (!outcome.ok()) {
      ++result.refusals;
      continue;
    }
    --wallet.credits;
    ++result.consumed;
    ++result.operations_completed;
  }

  // Drain: every wallet hands back all of its in-flight credit so that the
  // account must close exactly at capacity with zero outstanding credit.
  for (Wallet& wallet : wallets) {
    if (wallet.in_flight == 0) continue;
    AuthorityVector worker_authority = authority;
    worker_authority.incarnation = wallet.incarnation.id;
    const std::uint64_t credits = wallet.in_flight;
    const CreditOutcome outcome = submit(engine, wallet, OpKind::Return, credits, worker_authority, attempts, false);
    if (outcome.ok() && !outcome.duplicate) {
      result.returned += credits;
      wallet.in_flight = 0;
      wallet.credits = 0;
    }
  }

  const auto finished = std::chrono::steady_clock::now();
  const std::chrono::duration<double> elapsed = finished - started;
  result.seconds = elapsed.count();
  result.ops_per_second =
      result.seconds > 0.0 ? static_cast<double>(result.operations_completed) / result.seconds : 0.0;

  const Explanation explanation = engine.explain(account);
  result.final_view = explanation.view;
  result.closure_ok = explanation.view.closed && explanation.view.outstanding == 0 &&
                      explanation.view.available == explanation.view.capacity &&
                      explanation.view.protected_credits == 0 && explanation.view.stale == 0;
  const EngineStats stats = engine.stats();
  std::string evidence = "journal_records=";
  evidence += std::to_string(stats.journal_records);
  evidence += " applied=";
  evidence += std::to_string(stats.attempts_applied);
  evidence += " refused=";
  evidence += std::to_string(stats.attempts_refused);
  evidence += " duplicates=";
  evidence += std::to_string(stats.duplicates_absorbed);
  evidence += " snapshots=";
  evidence += std::to_string(stats.snapshots);
  result.journal = evidence;
  std::sort(result.refusal_reasons.begin(), result.refusal_reasons.end(),
            [](const std::pair<std::string, std::uint64_t>& lhs,
               const std::pair<std::string, std::uint64_t>& rhs) { return lhs.second > rhs.second; });
  return result;
}

std::string render(const BenchResult& result) {
  std::string text;
  text += "scenario=";
  text += result.scenario;
  text += " provenance=";
  text += result.provenance;
  text += "\n";
  text += "  operations_completed=";
  text += std::to_string(result.operations_completed);
  text += " attempted=";
  text += std::to_string(result.operations_attempted);
  text += " refusals=";
  text += std::to_string(result.refusals);
  text += " duplicates=";
  text += std::to_string(result.duplicates_absorbed);
  text += "\n";
  text += "  issued=";
  text += std::to_string(result.issued);
  text += " consumed=";
  text += std::to_string(result.consumed);
  text += " returned=";
  text += std::to_string(result.returned);
  text += "\n";
  text += "  seconds=";
  text += std::to_string(result.seconds);
  text += " completed_ops_per_second=";
  text += std::to_string(result.ops_per_second);
  text += "\n";
  text += "  final: ";
  text += result.final_view.to_string();
  text += " closure=";
  text += result.closure_ok ? "closed" : "OPEN";
  text += "\n";
  text += "  refusal_reasons=";
  if (result.refusal_reasons.empty()) {
    text += "none";
  } else {
    for (std::size_t i = 0; i < result.refusal_reasons.size(); ++i) {
      if (i != 0) text += ",";
      text += result.refusal_reasons[i].first;
      text += ":";
      text += std::to_string(result.refusal_reasons[i].second);
    }
  }
  text += "\n";
  text += "  evidence=";
  text += result.journal;
  text += "\n";
  return text;
}

}  // namespace creditfabric
