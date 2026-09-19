// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "creditfabric/ledger.hpp"

namespace creditfabric {
namespace {

/// Recomputes every derived field with checked arithmetic. A stored counter set
/// that cannot be reconciled leaves the view marked as not closed; it is never
/// silently presented as healthy.
void fill_derived(CreditView& view) noexcept {
  view.closed = false;
  if (view.returned > view.issued) return;
  view.in_flight = view.issued - view.returned;

  std::uint64_t reserved = 0;
  if (add_overflow(view.spent, view.stale, reserved)) return;
  if (reserved > view.in_flight) return;
  view.outstanding = view.in_flight - reserved;
  view.quarantined = view.stale;

  std::uint64_t claimed = 0;
  if (add_overflow(view.protected_credits, view.in_flight, claimed)) return;
  if (claimed > view.capacity) return;
  view.available = view.capacity - claimed;
  view.closed = true;
}

}  // namespace

CreditView compute_view(std::uint64_t capacity, const CreditTotals& totals) noexcept {
  CreditView view{};
  view.capacity = capacity;
  view.issued = totals.issued;
  view.consumed = totals.consumed;
  view.returned = totals.returned;
  view.stale = totals.stale;
  view.spent = totals.spent;
  view.protected_credits = totals.protected_credits;
  fill_derived(view);
  if (!view.closed) {
    // Saturate so that no caller can mistake a broken account for a healthy one.
    view.in_flight = 0;
    view.outstanding = 0;
    view.quarantined = view.stale;
    view.available = 0;
  }
  return view;
}

Status check_invariants(std::uint64_t capacity, const CreditTotals& totals) noexcept {
  if (totals.returned > totals.issued) return Status::refused(Reason::InvalidState, 4);
  const std::uint64_t in_flight = totals.issued - totals.returned;
  std::uint64_t reserved = 0;
  if (add_overflow(totals.spent, totals.stale, reserved)) return Status::refused(Reason::CountOverflow, 3);
  if (reserved > in_flight) return Status::refused(Reason::InvalidState, 3);
  std::uint64_t claimed = 0;
  if (add_overflow(totals.protected_credits, in_flight, claimed)) return Status::refused(Reason::CountOverflow, 5);
  if (claimed > capacity) return Status::refused(Reason::InvalidState, 6);
  return Status::success();
}

Digest128 CreditView::digest() const noexcept {
  return DigestBuilder{Digest128{0x1F2E3D4C5B6A7988ull, 0x88796A5B4C3D2E1Full}}
      .u64(capacity)
      .u64(issued)
      .u64(consumed)
      .u64(returned)
      .u64(stale)
      .u64(spent)
      .u64(protected_credits)
      .u64(in_flight)
      .u64(outstanding)
      .u64(available)
      .boolean(closed)
      .finish();
}

std::string CreditView::to_string() const {
  std::string text = "capacity=";
  text += std::to_string(capacity);
  text += " available=";
  text += std::to_string(available);
  text += " issued=";
  text += std::to_string(issued);
  text += " in_flight=";
  text += std::to_string(in_flight);
  text += " spendable=";
  text += std::to_string(outstanding);
  text += " spent=";
  text += std::to_string(spent);
  text += " consumed=";
  text += std::to_string(consumed);
  text += " returned=";
  text += std::to_string(returned);
  text += " protected=";
  text += std::to_string(protected_credits);
  text += " quarantined=";
  text += std::to_string(quarantined);
  text += " closed=";
  text += closed ? "yes" : "no";
  return text;
}

LedgerPlan::LedgerPlan(std::uint64_t capacity, const CreditTotals& current) noexcept
    : capacity_(capacity), totals_(current), status_(Status::success()) {
  status_ = check_invariants(capacity_, totals_);
}

std::uint64_t LedgerPlan::obligation_floor() const noexcept {
  const std::uint64_t in_flight = (totals_.returned <= totals_.issued) ? (totals_.issued - totals_.returned) : 0;
  return saturating_add(totals_.protected_credits, in_flight);
}

bool LedgerPlan::commit(const CreditTotals& candidate) noexcept {
  if (!status_.ok()) return false;
  const Status verdict = check_invariants(capacity_, candidate);
  if (!verdict.ok()) {
    status_ = verdict;
    return false;
  }
  totals_ = candidate;
  return true;
}

bool LedgerPlan::issue(std::uint64_t count, std::uint64_t min_issue, std::uint64_t max_issue) noexcept {
  if (!status_.ok()) return false;
  if (count == 0) {
    status_ = Status::refused(Reason::CountZero);
    return false;
  }
  if (count < min_issue || count > max_issue) {
    status_ = Status::refused(Reason::PolicyRefused, count);
    return false;
  }
  const CreditView current = view();
  if (current.available < count) {
    status_ = Status::refused(Reason::InsufficientAvailable, current.available);
    return false;
  }
  CreditTotals candidate = totals_;
  if (add_overflow(candidate.issued, count, candidate.issued)) {
    status_ = Status::refused(Reason::CountOverflow, candidate.issued);
    return false;
  }
  return commit(candidate);
}

bool LedgerPlan::consume(std::uint64_t count) noexcept {
  if (!status_.ok()) return false;
  if (count == 0) {
    status_ = Status::refused(Reason::CountZero);
    return false;
  }
  const CreditView current = view();
  if (current.outstanding < count) {
    status_ = Status::refused(Reason::InsufficientOutstanding, current.outstanding);
    return false;
  }
  CreditTotals candidate = totals_;
  if (add_overflow(candidate.spent, count, candidate.spent)) {
    status_ = Status::refused(Reason::CountOverflow, candidate.spent);
    return false;
  }
  if (add_overflow(candidate.consumed, count, candidate.consumed)) {
    status_ = Status::refused(Reason::CountOverflow, candidate.consumed);
    return false;
  }
  return commit(candidate);
}

bool LedgerPlan::give_back(std::uint64_t count) noexcept {
  if (!status_.ok()) return false;
  if (count == 0) {
    status_ = Status::refused(Reason::CountZero);
    return false;
  }
  const CreditView current = view();
  // Return cannot exceed issued/in-flight credit, and quarantined credit is not
  // returnable: it must be revalidated first.
  const std::uint64_t returnable = current.in_flight - current.stale;
  if (current.in_flight < count || returnable < count) {
    status_ = Status::refused(Reason::InsufficientOutstanding, returnable);
    return false;
  }
  CreditTotals candidate = totals_;
  const std::uint64_t from_spent = (candidate.spent < count) ? candidate.spent : count;
  candidate.spent -= from_spent;
  if (add_overflow(candidate.returned, count, candidate.returned)) {
    status_ = Status::refused(Reason::CountOverflow, candidate.returned);
    return false;
  }
  return commit(candidate);
}

bool LedgerPlan::protect(std::uint64_t count, bool allowed) noexcept {
  if (!status_.ok()) return false;
  if (!allowed) {
    status_ = Status::refused(Reason::PolicyRefused, 0);
    return false;
  }
  if (count == 0) {
    status_ = Status::refused(Reason::CountZero);
    return false;
  }
  const CreditView current = view();
  if (current.available < count) {
    status_ = Status::refused(Reason::InsufficientAvailable, current.available);
    return false;
  }
  CreditTotals candidate = totals_;
  if (add_overflow(candidate.protected_credits, count, candidate.protected_credits)) {
    status_ = Status::refused(Reason::CountOverflow, candidate.protected_credits);
    return false;
  }
  return commit(candidate);
}

bool LedgerPlan::unprotect(std::uint64_t count) noexcept {
  if (!status_.ok()) return false;
  if (count == 0) {
    status_ = Status::refused(Reason::CountZero);
    return false;
  }
  if (totals_.protected_credits < count) {
    status_ = Status::refused(Reason::InsufficientProtected, totals_.protected_credits);
    return false;
  }
  CreditTotals candidate = totals_;
  candidate.protected_credits -= count;
  return commit(candidate);
}

bool LedgerPlan::retire_stale(std::uint64_t count) noexcept {
  if (!status_.ok()) return false;
  if (count == 0) {
    status_ = Status::refused(Reason::CountZero);
    return false;
  }
  const CreditView current = view();
  if (current.outstanding < count) {
    status_ = Status::refused(Reason::InsufficientOutstanding, current.outstanding);
    return false;
  }
  CreditTotals candidate = totals_;
  if (add_overflow(candidate.stale, count, candidate.stale)) {
    status_ = Status::refused(Reason::CountOverflow, candidate.stale);
    return false;
  }
  return commit(candidate);
}

bool LedgerPlan::quarantine_in_flight() noexcept {
  if (!status_.ok()) return false;
  const CreditView current = view();
  const std::uint64_t amount = current.in_flight - current.stale;
  if (amount == 0) return true;
  CreditTotals candidate = totals_;
  candidate.spent = 0;
  if (add_overflow(candidate.stale, amount, candidate.stale)) {
    status_ = Status::refused(Reason::CountOverflow, candidate.stale);
    return false;
  }
  return commit(candidate);
}

bool LedgerPlan::revalidate_to_returned(std::uint64_t count) noexcept {
  if (!status_.ok()) return false;
  if (count == 0) {
    status_ = Status::refused(Reason::CountZero);
    return false;
  }
  if (totals_.stale < count) {
    status_ = Status::refused(Reason::NothingToRetire, totals_.stale);
    return false;
  }
  CreditTotals candidate = totals_;
  candidate.stale -= count;
  if (add_overflow(candidate.returned, count, candidate.returned)) {
    status_ = Status::refused(Reason::CountOverflow, candidate.returned);
    return false;
  }
  return commit(candidate);
}

bool LedgerPlan::revalidate_to_consumed(std::uint64_t count) noexcept {
  if (!status_.ok()) return false;
  if (count == 0) {
    status_ = Status::refused(Reason::CountZero);
    return false;
  }
  if (totals_.stale < count) {
    status_ = Status::refused(Reason::NothingToRetire, totals_.stale);
    return false;
  }
  CreditTotals candidate = totals_;
  candidate.stale -= count;
  if (add_overflow(candidate.spent, count, candidate.spent)) {
    status_ = Status::refused(Reason::CountOverflow, candidate.spent);
    return false;
  }
  if (add_overflow(candidate.consumed, count, candidate.consumed)) {
    status_ = Status::refused(Reason::CountOverflow, candidate.consumed);
    return false;
  }
  return commit(candidate);
}

bool LedgerPlan::set_capacity(std::uint64_t new_capacity, std::uint64_t max_capacity, bool allow_growth) noexcept {
  if (!status_.ok()) return false;
  if (new_capacity == 0) {
    status_ = Status::refused(Reason::CapacityOutOfRange, 0);
    return false;
  }
  if (new_capacity > max_capacity) {
    status_ = Status::refused(Reason::CapacityExceeded, max_capacity);
    return false;
  }
  if (new_capacity > capacity_ && !allow_growth) {
    status_ = Status::refused(Reason::PolicyRefused, capacity_);
    return false;
  }
  const std::uint64_t floor = obligation_floor();
  if (new_capacity < floor) {
    status_ = Status::refused(Reason::CapacityShrinkViolation, floor);
    return false;
  }
  capacity_ = new_capacity;
  return true;
}

}  // namespace creditfabric
