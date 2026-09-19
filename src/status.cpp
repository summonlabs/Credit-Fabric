// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "creditfabric/status.hpp"

namespace creditfabric {

const char* to_string(Reason reason) noexcept {
  switch (reason) {
#define CREDITFABRIC_REASON_CASE(name, value) \
  case Reason::name:                          \
    return #name;
    CREDITFABRIC_REASON_LIST(CREDITFABRIC_REASON_CASE)
#undef CREDITFABRIC_REASON_CASE
  }
  return "ReasonOutOfRange";
}

bool is_valid_reason(std::uint16_t raw) noexcept { return raw < kReasonCount; }

ReasonClass classify(Reason reason) noexcept {
  switch (reason) {
    case Reason::Ok:
      return ReasonClass::Success;
    case Reason::ReconciliationRequired:
    case Reason::StaleAuthority:
    case Reason::StaleEvidence:
    case Reason::FencedIncarnation:
    case Reason::UnknownIncarnation:
    case Reason::IncarnationNotReconciled:
    case Reason::UnknownState:
    case Reason::AccountUnknown:
    case Reason::AccountExists:
    case Reason::AccountClosed:
    case Reason::DomainMismatch:
    case Reason::ResourceMismatch:
    case Reason::ProducerMismatch:
    case Reason::ConsumerMismatch:
    case Reason::GrantMismatch:
      return ReasonClass::Authority;
    case Reason::StaleReplay:
    case Reason::SequenceGap:
    case Reason::AttemptConflict:
    case Reason::DuplicateAttempt:
    case Reason::AttemptUnknown:
      return ReasonClass::Replay;
    case Reason::InsufficientAvailable:
    case Reason::InsufficientOutstanding:
    case Reason::InsufficientProtected:
    case Reason::CapacityExceeded:
    case Reason::CapacityShrinkViolation:
    case Reason::CapacityOutOfRange:
    case Reason::CountZero:
    case Reason::CountOverflow:
    case Reason::ExhaustedHard:
    case Reason::NotExhausted:
    case Reason::NothingToRetire:
    case Reason::InvalidState:
    case Reason::PolicyRefused:
    case Reason::ProfileNotSupported:
      return ReasonClass::Accounting;
    case Reason::MalformedMessage:
    case Reason::UnsupportedVersion:
    case Reason::OversizedFrame:
    case Reason::ChecksumMismatch:
    case Reason::TruncatedInput:
    case Reason::Unsupported:
      return ReasonClass::Input;
    case Reason::JournalCorrupt:
    case Reason::JournalTorn:
    case Reason::JournalVersionUnsupported:
    case Reason::JournalDigestMismatch:
    case Reason::IoError:
      return ReasonClass::Durability;
    case Reason::TransportError:
    case Reason::ConnectionLimitReached:
    case Reason::ShutdownInProgress:
    case Reason::Cancelled:
    case Reason::BudgetExceeded:
    case Reason::InternalError:
      return ReasonClass::Lifecycle;
  }
  return ReasonClass::Lifecycle;
}

std::string Status::describe() const {
  if (reason == Reason::Ok) return "ok";
  std::string text = to_string(reason);
  if (detail != 0) {
    text += " (detail=";
    text += std::to_string(detail);
    text += ")";
  }
  return text;
}

}  // namespace creditfabric
