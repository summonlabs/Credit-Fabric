// Credit Fabric - credit accounting core.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Accounting model
// ----------------
// A credit account owns a fixed capacity C. Every unit of that capacity is, at
// any instant, in exactly one bucket:
//
//   available   : still in the pool, issue-able right now
//   protected   : reserved in the pool, withheld from issue
//   in_flight   : issued - returned (handed out and not yet handed back)
//
//   invariant (closure)   C == available + protected + in_flight
//
// Credits inside in_flight are partitioned by lifecycle:
//
//   outstanding : live, spendable permission
//   spent       : spent by a holder and not yet returned
//   stale       : quarantined after an authority advance; never spendable again
//
//   invariant (partition) in_flight == outstanding + spent + stale
//
// Consuming a credit moves it from outstanding to spent; it stays inside
// in_flight, so it still occupies capacity. A return hands credit back to the
// pool and moves in_flight down. That is the recycling cycle of credit-based
// flow control, and it is why "return cannot exceed issued/in-flight credit"
// is a checkable arithmetic property rather than a convention.
//
// Every externally influenced count is combined with checked arithmetic. A
// counter set that cannot be reconciled is reported as not closed and is never
// silently presented as healthy.

#ifndef CREDITFABRIC_LEDGER_HPP
#define CREDITFABRIC_LEDGER_HPP

#include <cstdint>
#include <string>

#include "creditfabric/checked.hpp"
#include "creditfabric/status.hpp"
#include "creditfabric/strong.hpp"

namespace creditfabric {

/// Committed counter state of one credit account.
struct CreditTotals {
  std::uint64_t issued{0};             ///< cumulative credits issued
  std::uint64_t consumed{0};           ///< cumulative spend events (telemetry; never gates authority)
  std::uint64_t returned{0};           ///< cumulative credits handed back
  std::uint64_t stale{0};              ///< quarantined in-flight credits
  std::uint64_t spent{0};              ///< spent and not yet returned
  std::uint64_t protected_credits{0};  ///< currently reserved in the pool

  friend constexpr bool operator==(const CreditTotals&, const CreditTotals&) noexcept = default;
};

/// Derived, explainable view of an account at one instant.
struct CreditView {
  std::uint64_t capacity{0};           ///< total configured capacity
  std::uint64_t issued{0};             ///< cumulative credits issued
  std::uint64_t consumed{0};           ///< cumulative spend events
  std::uint64_t returned{0};           ///< cumulative credits returned
  std::uint64_t stale{0};              ///< cumulative credits quarantined as stale
  std::uint64_t spent{0};              ///< spent and not yet returned
  std::uint64_t protected_credits{0};  ///< currently reserved/protected
  std::uint64_t in_flight{0};          ///< issued - returned
  std::uint64_t outstanding{0};        ///< live, spendable credit
  std::uint64_t quarantined{0};        ///< == stale; named for explanations
  std::uint64_t available{0};          ///< issue-able right now
  bool closed{false};                  ///< true when both invariants hold exactly

  [[nodiscard]] constexpr bool exhausted() const noexcept { return available == 0; }
  [[nodiscard]] Digest128 digest() const noexcept;
  [[nodiscard]] std::string to_string() const;
};

/// Derives the view. Never throws, never wraps: if a stored counter set cannot
/// be reconciled, \c closed is false and the numeric fields are saturated so
/// that no caller can mistake a broken account for a healthy one.
[[nodiscard]] CreditView compute_view(std::uint64_t capacity, const CreditTotals& totals) noexcept;

/// Verifies both closure invariants with checked arithmetic.
[[nodiscard]] Status check_invariants(std::uint64_t capacity, const CreditTotals& totals) noexcept;

/// Stages one authoritative mutation.
///
/// The plan holds the candidate counters. Every mutator validates the candidate
/// against the invariants before publishing it, so a rejected mutation leaves
/// the plan exactly as it was. "Plan -> verify -> commit" in one object.
class LedgerPlan {
 public:
  LedgerPlan(std::uint64_t capacity, const CreditTotals& current) noexcept;

  [[nodiscard]] std::uint64_t capacity() const noexcept { return capacity_; }
  [[nodiscard]] const CreditTotals& totals() const noexcept { return totals_; }
  [[nodiscard]] CreditView view() const noexcept { return compute_view(capacity_, totals_); }
  [[nodiscard]] Status status() const noexcept { return status_; }
  [[nodiscard]] bool valid() const noexcept { return status_.ok(); }

  /// Moves credits from available to outstanding.
  bool issue(std::uint64_t count, std::uint64_t min_issue, std::uint64_t max_issue) noexcept;
  /// Spends outstanding credits. They stay in flight until returned.
  bool consume(std::uint64_t count) noexcept;
  /// Hands in-flight credits back to the pool (spent credits first).
  bool give_back(std::uint64_t count) noexcept;
  /// Reserves available credits so they cannot be issued.
  bool protect(std::uint64_t count, bool allowed) noexcept;
  /// Releases reserved credits back to the pool.
  bool unprotect(std::uint64_t count) noexcept;
  /// Quarantines outstanding credits after an authority advance.
  bool retire_stale(std::uint64_t count) noexcept;
  /// Quarantines every in-flight credit: the authority-advance transition.
  bool quarantine_in_flight() noexcept;
  /// Revalidates quarantined credits as handed back (returns them to the pool).
  bool revalidate_to_returned(std::uint64_t count) noexcept;
  /// Revalidates quarantined credits as spent (they keep occupying capacity).
  bool revalidate_to_consumed(std::uint64_t count) noexcept;
  /// Changes capacity subject to the obligation floor protected + in_flight.
  bool set_capacity(std::uint64_t new_capacity, std::uint64_t max_capacity, bool allow_growth) noexcept;

  [[nodiscard]] std::uint64_t obligation_floor() const noexcept;

  /// Explicitly poisons the plan (used when a caller detects an unrelated fault).
  void refuse(Status status) noexcept { status_ = status; }

 private:
  bool commit(const CreditTotals& candidate) noexcept;

  std::uint64_t capacity_{0};
  CreditTotals totals_{};
  Status status_{};
};

}  // namespace creditfabric

#endif  // CREDITFABRIC_LEDGER_HPP
