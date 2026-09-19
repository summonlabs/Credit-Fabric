// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// In-process session used by cfctl: it binds an incarnation to a credit engine
// exactly the way a remote worker binds to a coordinator, including explicit
// reconciliation after every process start.

#ifndef CREDITFABRIC_APPS_LOCAL_SESSION_HPP
#define CREDITFABRIC_APPS_LOCAL_SESSION_HPP

#include <cstdint>
#include <string>

#include "creditfabric/creditfabric.hpp"

namespace cfapp {

/// Operator identity that survives process restarts. The boot identity is a
/// pure function of the deployment prefix, so a restarted tool reconnects as
/// the same incarnation and must reconcile again.
[[nodiscard]] inline creditfabric::Incarnation durable_operator(const std::string& deployment) {
  const creditfabric::Digest128 digest =
      creditfabric::DigestBuilder{creditfabric::Digest128{0x0CF0BEEFCAFE0001ull, 0x0CF0BEEFCAFE0002ull}}
          .text(deployment)
          .finish();
  return creditfabric::Incarnation::make(creditfabric::PublisherId{0x0CF0BEEFCAFEull},
                                         creditfabric::BootId{digest.lo ^ digest.hi});
}

class LocalSession {
 public:
  LocalSession(creditfabric::CreditEngine& engine, creditfabric::Incarnation me, creditfabric::DomainId domain,
               creditfabric::AccountId account)
      : engine_(engine), me_(me), domain_(domain), account_(account) {}

  [[nodiscard]] creditfabric::CreditOutcome open(const creditfabric::AccountConfig& config) {
    creditfabric::CreditRequest request{};
    request.op = creditfabric::OpKind::Open;
    request.config = config;
    creditfabric::AuthorityVector authority{};
    authority.domain = config.domain;
    authority.account = config.account;
    authority.epoch = creditfabric::EpochId{1};
    authority.generation = creditfabric::Generation{1};
    authority.incarnation = me_.id;
    request.authority = authority;
    request.presenter = me_;
    request.attempt = attempts_.next();
    request.sequence = 1;
    request.proof = engine_.challenge().prove(me_, authority);
    const creditfabric::CreditOutcome outcome = engine_.apply(request);
    if (outcome.decided) sequence_ = request.sequence + 1;
    if (outcome.ok()) {
      authority_ = authority;
      reconciled_ = true;
    }
    return outcome;
  }

  /// Establishes the authority vector and reconciles this incarnation.
  [[nodiscard]] creditfabric::Status reconcile() {
    const creditfabric::Explanation explanation = engine_.explain(account_);
    if (!explanation.found) return creditfabric::Status::refused(creditfabric::Reason::AccountUnknown, 1);
    creditfabric::AuthorityVector authority{};
    authority.domain = domain_;
    authority.account = account_;
    authority.epoch = explanation.authority.epoch;
    authority.generation = explanation.authority.generation;
    authority.incarnation = me_.id;
    authority_ = authority;

    // Discover the durable sequence watermark for this incarnation. A restarted
    // process has no memory of its previous decisions, so it must ask.
    {
      creditfabric::CreditRequest probe{};
      probe.op = creditfabric::OpKind::Describe;
      probe.sequence = 1;
      probe.presenter = me_;
      probe.authority = authority_;
      const creditfabric::CreditOutcome described = engine_.apply(probe);
      if (!described.ok()) return described.status;
      sequence_ = described.sequence + 1;
    }

    for (int retry = 0; retry < 8; ++retry) {
      creditfabric::CreditRequest request{};
      request.op = creditfabric::OpKind::Reconcile;
      request.presenter = me_;
      request.attempt = attempts_.next();
      request.sequence = sequence_;
      request.authority = authority_;
      request.proof = engine_.challenge().prove(me_, authority_);
      const creditfabric::CreditOutcome outcome = engine_.apply(request);
      if (outcome.ok()) {
        sequence_ = request.sequence + 1;
        reconciled_ = true;
        return creditfabric::Status::success();
      }
      if (outcome.status.reason == creditfabric::Reason::SequenceGap) {
        sequence_ = outcome.status.detail;
        continue;
      }
      if (outcome.status.reason == creditfabric::Reason::StaleReplay) {
        sequence_ = outcome.status.detail + 1;
        continue;
      }
      return outcome.status;
    }
    return creditfabric::Status::refused(creditfabric::Reason::BudgetExceeded, 1);
  }

  [[nodiscard]] creditfabric::CreditOutcome call(creditfabric::OpKind op, std::uint64_t count = 0,
                                                 std::uint64_t new_capacity = 0,
                                                 creditfabric::RevalidationDecision decision =
                                                     creditfabric::RevalidationDecision::None,
                                                 creditfabric::IncarnationId target = {},
                                                 creditfabric::Reason cause = creditfabric::Reason::PolicyRefused,
                                                 creditfabric::Digest128 expected_state = {}) {
    creditfabric::CreditRequest request{};
    request.op = op;
    request.count = count;
    request.new_capacity = new_capacity;
    request.decision = decision;
    request.target = target;
    request.fence_cause = cause;
    request.expected_state = expected_state;
    return call(request);
  }

  [[nodiscard]] creditfabric::CreditOutcome call(creditfabric::CreditRequest request) {
    if (!reconciled_) {
      const creditfabric::Status status = reconcile();
      if (!status.ok()) {
        creditfabric::CreditOutcome outcome{};
        outcome.status = status;
        return outcome;
      }
    }
    request.presenter = me_;
    for (int retry = 0; retry < 8; ++retry) {
      request.attempt = attempts_.next();
      request.sequence = sequence_;
      request.authority = authority_;
      const creditfabric::CreditOutcome outcome = engine_.apply(request);
      if (outcome.decided) {
        sequence_ = request.sequence + 1;
        return outcome;
      }
      if (outcome.status.reason == creditfabric::Reason::SequenceGap) {
        sequence_ = outcome.status.detail;
        continue;
      }
      if (outcome.status.reason == creditfabric::Reason::StaleReplay) {
        sequence_ = outcome.status.detail + 1;
        continue;
      }
      if (outcome.status.reason == creditfabric::Reason::ReconciliationRequired ||
          outcome.status.reason == creditfabric::Reason::UnknownIncarnation ||
          outcome.status.reason == creditfabric::Reason::StaleAuthority) {
        const creditfabric::Status status = reconcile();
        if (!status.ok()) {
          creditfabric::CreditOutcome failure = outcome;
          failure.status = status;
          return failure;
        }
        continue;
      }
      return outcome;
    }
    creditfabric::CreditOutcome exhausted{};
    exhausted.status = creditfabric::Status::refused(creditfabric::Reason::BudgetExceeded, 2);
    return exhausted;
  }

  [[nodiscard]] std::uint64_t next_sequence() const noexcept { return sequence_; }
  [[nodiscard]] const creditfabric::AuthorityVector& authority() const noexcept { return authority_; }
  [[nodiscard]] creditfabric::Incarnation incarnation() const noexcept { return me_; }
  [[nodiscard]] creditfabric::Explanation explain() const { return engine_.explain(account_); }
  [[nodiscard]] creditfabric::CreditEngine& engine() noexcept { return engine_; }

 private:
  creditfabric::CreditEngine& engine_;
  creditfabric::Incarnation me_{};
  creditfabric::DomainId domain_{};
  creditfabric::AccountId account_{};
  creditfabric::AuthorityVector authority_{};
  creditfabric::AttemptIdGenerator attempts_{};
  std::uint64_t sequence_{1};
  bool reconciled_{false};
};

}  // namespace cfapp

#endif  // CREDITFABRIC_APPS_LOCAL_SESSION_HPP
