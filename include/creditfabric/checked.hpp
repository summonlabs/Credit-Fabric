// Credit Fabric - checked integer arithmetic.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Every externally influenced size, capacity, rate and counter passes through
// these primitives. Silent wraparound is never acceptable in an accounting
// authority: an overflow or underflow is a refusal, not a new number.

#ifndef CREDITFABRIC_CHECKED_HPP
#define CREDITFABRIC_CHECKED_HPP

#include <cstdint>
#include <limits>

namespace creditfabric {

[[nodiscard]] constexpr bool add_overflow(std::uint64_t a, std::uint64_t b, std::uint64_t& out) noexcept {
  out = a + b;
  return out < a;
}

[[nodiscard]] constexpr bool sub_overflow(std::uint64_t a, std::uint64_t b, std::uint64_t& out) noexcept {
  if (b > a) {
    out = 0;
    return true;
  }
  out = a - b;
  return false;
}

[[nodiscard]] constexpr bool mul_overflow(std::uint64_t a, std::uint64_t b, std::uint64_t& out) noexcept {
  if (a == 0 || b == 0) {
    out = 0;
    return false;
  }
  if (a > (std::numeric_limits<std::uint64_t>::max)() / b) {
    out = 0;
    return true;
  }
  out = a * b;
  return false;
}

/// Staged arithmetic that latches the first failure and never wraps.
class CheckedAccumulator {
 public:
  constexpr CheckedAccumulator() noexcept = default;
  constexpr explicit CheckedAccumulator(std::uint64_t initial) noexcept : value_(initial) {}

  [[nodiscard]] constexpr std::uint64_t value() const noexcept { return value_; }
  [[nodiscard]] constexpr bool ok() const noexcept { return ok_; }
  [[nodiscard]] constexpr bool overflowed() const noexcept { return !ok_ && !underflowed_; }
  [[nodiscard]] constexpr bool underflowed() const noexcept { return !ok_ && underflowed_; }

  constexpr bool add(std::uint64_t v) noexcept {
    if (!ok_) return false;
    std::uint64_t next = 0;
    if (add_overflow(value_, v, next)) {
      ok_ = false;
      return false;
    }
    value_ = next;
    return true;
  }

  constexpr bool sub(std::uint64_t v) noexcept {
    if (!ok_) return false;
    std::uint64_t next = 0;
    if (sub_overflow(value_, v, next)) {
      ok_ = false;
      underflowed_ = true;
      return false;
    }
    value_ = next;
    return true;
  }

  constexpr bool mul(std::uint64_t v) noexcept {
    if (!ok_) return false;
    std::uint64_t next = 0;
    if (mul_overflow(value_, v, next)) {
      ok_ = false;
      return false;
    }
    value_ = next;
    return true;
  }

  constexpr void reset(std::uint64_t value) noexcept {
    value_ = value;
    ok_ = true;
    underflowed_ = false;
  }

 private:
  std::uint64_t value_{0};
  bool ok_{true};
  bool underflowed_{false};
};

/// Saturating sum used only for non-authoritative telemetry aggregation.
[[nodiscard]] constexpr std::uint64_t saturating_add(std::uint64_t a, std::uint64_t b) noexcept {
  const std::uint64_t max = (std::numeric_limits<std::uint64_t>::max)();
  return (b > max - a) ? max : a + b;
}

}  // namespace creditfabric

#endif  // CREDITFABRIC_CHECKED_HPP
