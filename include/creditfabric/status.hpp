// Credit Fabric - refusal reasons and status values.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Every authoritative refusal names an exact reason. "Refused" without a reason
// is not an acceptable answer from a credit authority.

#ifndef CREDITFABRIC_STATUS_HPP
#define CREDITFABRIC_STATUS_HPP

#include <cstdint>
#include <string>

namespace creditfabric {

/// Refusal reasons. Stable numeric values are part of the durable journal and
/// wire contract: never renumber an existing entry.
#define CREDITFABRIC_REASON_LIST(X)                     \
  X(Ok, 0)                                              \
  X(UnknownState, 1)                                    \
  X(ReconciliationRequired, 2)                          \
  X(StaleAuthority, 3)                                  \
  X(StaleEvidence, 4)                                   \
  X(FencedIncarnation, 5)                               \
  X(UnknownIncarnation, 6)                              \
  X(IncarnationNotReconciled, 7)                        \
  X(StaleReplay, 8)                                     \
  X(SequenceGap, 9)                                     \
  X(AttemptConflict, 10)                                \
  X(DuplicateAttempt, 11)                               \
  X(AttemptUnknown, 12)                                 \
  X(AccountUnknown, 13)                                 \
  X(AccountExists, 14)                                  \
  X(AccountClosed, 15)                                  \
  X(DomainMismatch, 16)                                 \
  X(ResourceMismatch, 17)                               \
  X(ProducerMismatch, 18)                               \
  X(ConsumerMismatch, 19)                               \
  X(GrantMismatch, 20)                                  \
  X(PolicyRefused, 21)                                  \
  X(ProfileNotSupported, 22)                            \
  X(InsufficientAvailable, 23)                          \
  X(InsufficientOutstanding, 24)                        \
  X(InsufficientProtected, 25)                          \
  X(CapacityExceeded, 26)                               \
  X(CapacityShrinkViolation, 27)                        \
  X(CapacityOutOfRange, 28)                             \
  X(CountZero, 29)                                      \
  X(CountOverflow, 30)                                  \
  X(ExhaustedHard, 31)                                  \
  X(NotExhausted, 32)                                   \
  X(NothingToRetire, 33)                                \
  X(InvalidState, 34)                                   \
  X(MalformedMessage, 35)                               \
  X(UnsupportedVersion, 36)                             \
  X(OversizedFrame, 37)                                 \
  X(ChecksumMismatch, 38)                               \
  X(TruncatedInput, 39)                                 \
  X(JournalCorrupt, 40)                                 \
  X(JournalTorn, 41)                                    \
  X(JournalVersionUnsupported, 42)                      \
  X(JournalDigestMismatch, 43)                          \
  X(IoError, 44)                                        \
  X(TransportError, 45)                                 \
  X(ConnectionLimitReached, 46)                         \
  X(ShutdownInProgress, 47)                             \
  X(Cancelled, 48)                                      \
  X(BudgetExceeded, 49)                                 \
  X(Unsupported, 50)                                    \
  X(NotOperator, 51)                                    \
  X(InternalError, 52)

enum class Reason : std::uint8_t {
#define CREDITFABRIC_REASON_ENUM(name, value) name = (value),
  CREDITFABRIC_REASON_LIST(CREDITFABRIC_REASON_ENUM)
#undef CREDITFABRIC_REASON_ENUM
};

inline constexpr std::uint16_t kReasonCount = 53u;

[[nodiscard]] const char* to_string(Reason reason) noexcept;
[[nodiscard]] bool is_valid_reason(std::uint16_t raw) noexcept;

/// Classifies a reason for reporting. Nothing here upgrades a refusal into
/// authority; the classification is purely descriptive.
enum class ReasonClass : std::uint8_t {
  Success = 0,
  Authority = 1,   // stale / unknown / fenced / unreconciled authority
  Replay = 2,      // duplicate, conflicting or out-of-order attempt
  Accounting = 3,  // credit conservation or capacity refusal
  Input = 4,       // malformed, truncated, oversized, unsupported input
  Durability = 5,  // journal integrity, IO
  Lifecycle = 6,   // shutdown, cancellation, budget, internal
};

[[nodiscard]] ReasonClass classify(Reason reason) noexcept;

/// Status: a reason code plus an optional bounded non-authoritative detail word.
///
/// The detail word never carries authority; it exists so an operator can tie a
/// refusal to a specific sequence number, byte offset or offending field.
struct Status {
  Reason reason{Reason::Ok};
  std::uint64_t detail{0};

  [[nodiscard]] constexpr bool ok() const noexcept { return reason == Reason::Ok; }
  [[nodiscard]] std::string describe() const;

  [[nodiscard]] static constexpr Status success() noexcept { return Status{}; }
  [[nodiscard]] static constexpr Status refused(Reason reason, std::uint64_t detail = 0) noexcept {
    return Status{reason, detail};
  }

  friend constexpr bool operator==(Status lhs, Status rhs) noexcept {
    return lhs.reason == rhs.reason && lhs.detail == rhs.detail;
  }
};

}  // namespace creditfabric

#endif  // CREDITFABRIC_STATUS_HPP
