// Credit Fabric - shared test fixture.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#ifndef CREDITFABRIC_TESTS_FIXTURE_HPP
#define CREDITFABRIC_TESTS_FIXTURE_HPP

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "creditfabric/creditfabric.hpp"

namespace cftest {

/// Non-owning Journal adapter so that a test keeps direct access to the
/// in-memory journal (to simulate torn writes and media damage) while the
/// engine owns the Journal interface.
class BorrowedJournal final : public creditfabric::Journal {
 public:
  explicit BorrowedJournal(creditfabric::InMemoryJournal& inner) : inner_(&inner) {}

  [[nodiscard]] creditfabric::Status open() override { return inner_->open(); }
  [[nodiscard]] creditfabric::Status append(std::uint64_t sequence, creditfabric::JournalRecordKind kind,
                                            creditfabric::Digest128 chain,
                                            const std::vector<std::uint8_t>& payload) override {
    return inner_->append(sequence, kind, chain, payload);
  }
  [[nodiscard]] creditfabric::Status begin_replay() override { return inner_->begin_replay(); }
  [[nodiscard]] creditfabric::Status next_record(creditfabric::JournalRecord& out) override {
    return inner_->next_record(out);
  }
  [[nodiscard]] creditfabric::Status end_replay() override { return inner_->end_replay(); }
  [[nodiscard]] creditfabric::Status rotate(std::uint64_t snapshot_sequence) override {
    return inner_->rotate(snapshot_sequence);
  }
  [[nodiscard]] creditfabric::Status write_snapshot(std::uint64_t sequence, creditfabric::Digest128 chain,
                                                    const std::vector<std::uint8_t>& payload) override {
    return inner_->write_snapshot(sequence, chain, payload);
  }
  [[nodiscard]] creditfabric::Status read_snapshot(std::uint64_t& sequence, creditfabric::Digest128& chain,
                                                   std::vector<std::uint8_t>& payload, bool& present) override {
    return inner_->read_snapshot(sequence, chain, payload, present);
  }
  [[nodiscard]] std::uint64_t bytes() const override { return inner_->bytes(); }
  [[nodiscard]] std::uint64_t records() const override { return inner_->records(); }
  [[nodiscard]] bool torn_tail() const override { return inner_->torn_tail(); }
  [[nodiscard]] std::string describe() const override { return inner_->describe(); }
  [[nodiscard]] creditfabric::Status close() override { return inner_->close(); }

 private:
  creditfabric::InMemoryJournal* inner_;
};

/// One in-process authority plus the identity and helpers a test needs.
struct Fixture {
  creditfabric::EngineLimits limits{};
  creditfabric::InMemoryJournal journal{};
  std::unique_ptr<creditfabric::CreditEngine> engine{};
  creditfabric::Incarnation me{};
  creditfabric::DomainId domain{0xD0A1ull};
  creditfabric::AccountId account{0xACCEull};
  creditfabric::ResourceId resource{0x2E50ull};
  creditfabric::AttemptIdGenerator attempts{0x5EEDull};
  std::uint64_t sequence{1};

  explicit Fixture(std::uint64_t operator_boot = 0x0B007ull) : me(creditfabric::Incarnation::make(
                                                        creditfabric::PublisherId{0x0DEAull}, creditfabric::BootId{operator_boot})) {
    engine = std::make_unique<creditfabric::CreditEngine>(limits,
                                                          std::make_unique<BorrowedJournal>(journal), me);
  }

  [[nodiscard]] creditfabric::Status recover() { return engine->recover(); }

  /// Creates an account with the fixture's domain/account defaults.
  [[nodiscard]] creditfabric::CreditOutcome open(std::uint64_t capacity, std::uint64_t max_capacity = 0,
                                                 creditfabric::ProfileKind profile = creditfabric::ProfileKind::Abstract,
                                                 std::uint64_t max_issue = 0) {
    creditfabric::AccountConfig config{};
    config.domain = domain;
    config.account = account;
    config.resource = resource;
    config.policy = creditfabric::PolicyId{1};
    config.capacity = capacity;
    config.max_capacity = max_capacity == 0 ? capacity : max_capacity;
    config.profile = profile;
    config.rules.max_issue_per_attempt = max_issue == 0 ? capacity : max_issue;
    creditfabric::CreditRequest request{};
    request.op = creditfabric::OpKind::Open;
    request.config = config;
    creditfabric::AuthorityVector authority{};
    authority.domain = domain;
    authority.account = account;
    authority.epoch = creditfabric::EpochId{1};
    authority.generation = creditfabric::Generation{1};
    authority.incarnation = me.id;
    request.authority = authority;
    request.presenter = me;
    request.attempt = attempts.next();
    request.sequence = 1;
    request.proof = engine->challenge().prove(me, authority);
    const creditfabric::CreditOutcome outcome = engine->apply(request);
    if (outcome.decided) sequence = 2;
    return outcome;
  }

  [[nodiscard]] creditfabric::AuthorityVector authority_of(creditfabric::Incarnation who) const {
    const creditfabric::Explanation explanation = engine->explain(account);
    creditfabric::AuthorityVector authority{};
    authority.domain = domain;
    authority.account = account;
    authority.epoch = explanation.authority.epoch;
    authority.generation = explanation.authority.generation;
    authority.incarnation = who.id;
    return authority;
  }

  /// Registers and reconciles an incarnation, returning its first free sequence.
  [[nodiscard]] creditfabric::CreditOutcome reconcile(creditfabric::Incarnation who, std::uint64_t& next_sequence) {
    creditfabric::AuthorityVector authority = authority_of(who);
    const creditfabric::CreditOutcome described = describe(who);
    std::uint64_t placed = described.ok() ? described.sequence + 1 : 1;
    creditfabric::CreditRequest request{};
    request.op = creditfabric::OpKind::Reconcile;
    request.presenter = who;
    request.authority = authority;
    request.attempt = attempts.next();
    request.sequence = placed;
    request.proof = engine->challenge().prove(who, authority);
    const creditfabric::CreditOutcome outcome = engine->apply(request);
    if (outcome.decided) next_sequence = placed + 1;
    return outcome;
  }

  [[nodiscard]] creditfabric::CreditOutcome describe(creditfabric::Incarnation who) {
    creditfabric::CreditRequest request{};
    request.op = creditfabric::OpKind::Describe;
    request.presenter = who;
    request.authority.domain = domain;
    request.authority.account = account;
    request.authority.incarnation = who.id;
    request.sequence = 1;
    request.attempt = attempts.next();
    return engine->apply(request);
  }

  [[nodiscard]] creditfabric::CreditOutcome call_raw(creditfabric::OpKind op, std::uint64_t count = 0,
                                                     std::uint64_t new_capacity = 0,
                                                     creditfabric::IncarnationId target = {},
                                                     creditfabric::Reason cause = creditfabric::Reason::PolicyRefused,
                                                     creditfabric::RevalidationDecision decision =
                                                         creditfabric::RevalidationDecision::None) {
    creditfabric::CreditRequest request{};
    request.op = op;
    request.count = count;
    request.new_capacity = new_capacity;
    request.target = target;
    request.fence_cause = cause;
    request.decision = decision;
    request.presenter = me;
    request.authority = authority_of(me);
    request.attempt = attempts.next();
    request.sequence = sequence;
    const creditfabric::CreditOutcome outcome = engine->apply(request);
    if (outcome.decided) sequence = request.sequence + 1;
    return outcome;
  }

  /// Operator call with the same automatic revalidation a real client performs:
  /// a stale or unreconciled authority is refreshed once and retried.
  [[nodiscard]] creditfabric::CreditOutcome call(creditfabric::OpKind op, std::uint64_t count = 0,
                                                 std::uint64_t new_capacity = 0,
                                                 creditfabric::IncarnationId target = {},
                                                 creditfabric::Reason cause = creditfabric::Reason::PolicyRefused,
                                                 creditfabric::RevalidationDecision decision =
                                                     creditfabric::RevalidationDecision::None) {
    const creditfabric::CreditOutcome first = call_raw(op, count, new_capacity, target, cause, decision);
    if (first.ok()) return first;
    const creditfabric::Reason reason = first.status.reason;
    if (reason != creditfabric::Reason::ReconciliationRequired &&
        reason != creditfabric::Reason::StaleAuthority &&
        reason != creditfabric::Reason::UnknownIncarnation) {
      return first;
    }
    std::uint64_t placed = 0;
    if (!reconcile(me, placed).ok()) return first;
    sequence = placed;  // reconciliation consumed a slot
    return call_raw(op, count, new_capacity, target, cause, decision);
  }

  [[nodiscard]] creditfabric::CreditView view() const {
    return engine->explain(account).view;
  }

  [[nodiscard]] creditfabric::Explanation explain() const { return engine->explain(account); }
};

/// Deterministic pseudo-random source for property and adversarial tests.
class Rng {
 public:
  explicit Rng(std::uint64_t seed) : state_(seed | 1ull) {}

  [[nodiscard]] std::uint64_t next() noexcept {
    state_ ^= state_ << 13;
    state_ ^= state_ >> 7;
    state_ ^= state_ << 17;
    return state_;
  }

  [[nodiscard]] std::uint64_t bounded(std::uint64_t bound) noexcept {
    return bound == 0 ? 0 : next() % bound;
  }

 private:
  std::uint64_t state_;
};

}  // namespace cftest

#endif  // CREDITFABRIC_TESTS_FIXTURE_HPP
