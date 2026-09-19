// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "creditfabric/record.hpp"

#include "creditfabric/bytes.hpp"
#include "creditfabric/wire.hpp"

namespace creditfabric {
namespace {

/// Hard ceiling on any decoded collection. A corrupt length prefix can waste at
/// most this much memory, and every decode additionally requires that the
/// claimed count actually fits in the remaining bytes.
constexpr std::uint64_t kMaxCollection = 1u << 20;

bool within_budget(ByteReader& reader, std::uint64_t count, std::size_t minimum_bytes_per_item) {
  if (count > kMaxCollection) return false;
  return static_cast<std::uint64_t>(reader.remaining()) >= count * minimum_bytes_per_item;
}

void encode_totals(ByteWriter& writer, const CreditTotals& totals) {
  writer.u64(totals.issued);
  writer.u64(totals.consumed);
  writer.u64(totals.returned);
  writer.u64(totals.stale);
  writer.u64(totals.spent);
  writer.u64(totals.protected_credits);
}

Status decode_totals(ByteReader& reader, CreditTotals& out) {
  if (!reader.u64(out.issued) || !reader.u64(out.consumed) || !reader.u64(out.returned) || !reader.u64(out.stale) ||
      !reader.u64(out.spent) || !reader.u64(out.protected_credits)) {
    return Status::refused(Reason::TruncatedInput, reader.position());
  }
  return Status::success();
}

void encode_incarnation(ByteWriter& writer, const IncarnationRecord& record) {
  writer.u64(record.id.value());
  writer.u64(record.publisher.value());
  writer.u64(record.boot.value());
  writer.u8(static_cast<std::uint8_t>(record.state));
  writer.u64(record.reconciled_epoch.value());
  writer.u64(record.reconciled_generation.value());
  writer.digest(record.reconciliation_proof);
  writer.u64(record.producer.value());
  writer.u64(record.consumer.value());
  writer.u64(record.grant.value());
  writer.u64(record.last_sequence);
  writer.u64(record.committed_ops);
  writer.u64(record.refused_ops);
  writer.u64(record.fenced_at_sequence);
}

Status decode_incarnation(ByteReader& reader, IncarnationRecord& out) {
  std::uint8_t state = 0;
  std::uint64_t id = 0;
  std::uint64_t publisher = 0;
  std::uint64_t boot = 0;
  std::uint64_t epoch = 0;
  std::uint64_t generation = 0;
  std::uint64_t producer = 0;
  std::uint64_t consumer = 0;
  std::uint64_t grant = 0;
  if (!reader.u64(id) || !reader.u64(publisher) || !reader.u64(boot) || !reader.u8(state) || !reader.u64(epoch) ||
      !reader.u64(generation) || !reader.digest(out.reconciliation_proof) || !reader.u64(producer) ||
      !reader.u64(consumer) || !reader.u64(grant) || !reader.u64(out.last_sequence) ||
      !reader.u64(out.committed_ops) || !reader.u64(out.refused_ops) || !reader.u64(out.fenced_at_sequence)) {
    return Status::refused(Reason::TruncatedInput, reader.position());
  }
  if (state > static_cast<std::uint8_t>(AuthorityState::Retired)) {
    return Status::refused(Reason::MalformedMessage, state);
  }
  out.id = IncarnationId{id};
  out.publisher = PublisherId{publisher};
  out.boot = BootId{boot};
  out.state = static_cast<AuthorityState>(state);
  out.reconciled_epoch = EpochId{epoch};
  out.reconciled_generation = Generation{generation};
  out.producer = ProducerId{producer};
  out.consumer = ConsumerId{consumer};
  out.grant = GrantId{grant};
  return Status::success();
}

void encode_fence(ByteWriter& writer, const FenceRecord& fence) {
  writer.u64(fence.incarnation.value());
  writer.u64(fence.epoch.value());
  writer.u64(fence.generation.value());
  writer.u16(static_cast<std::uint16_t>(fence.cause));
  writer.u64(fence.sequence);
  writer.u64(fence.fenced_credit);
}

Status decode_fence(ByteReader& reader, FenceRecord& out) {
  std::uint64_t incarnation = 0;
  std::uint64_t epoch = 0;
  std::uint64_t generation = 0;
  std::uint16_t cause = 0;
  if (!reader.u64(incarnation) || !reader.u64(epoch) || !reader.u64(generation) || !reader.u16(cause) ||
      !reader.u64(out.sequence) || !reader.u64(out.fenced_credit)) {
    return Status::refused(Reason::TruncatedInput, reader.position());
  }
  if (!is_valid_reason(cause)) return Status::refused(Reason::MalformedMessage, cause);
  out.incarnation = IncarnationId{incarnation};
  out.epoch = EpochId{epoch};
  out.generation = Generation{generation};
  out.cause = static_cast<Reason>(cause);
  return Status::success();
}

void encode_ambiguous(ByteWriter& writer, const AmbiguousNote& note) {
  writer.digest(note.attempt.value);
  writer.u8(static_cast<std::uint8_t>(note.op));
  writer.u64(note.sequence);
  writer.u64(note.incarnation.value());
  writer.u64(note.journal_sequence);
}

Status decode_ambiguous(ByteReader& reader, AmbiguousNote& out) {
  std::uint8_t op = 0;
  std::uint64_t incarnation = 0;
  if (!reader.digest(out.attempt.value) || !reader.u8(op) || !reader.u64(out.sequence) || !reader.u64(incarnation) ||
      !reader.u64(out.journal_sequence)) {
    return Status::refused(Reason::TruncatedInput, reader.position());
  }
  if (!is_valid_op(op)) return Status::refused(Reason::MalformedMessage, op);
  out.op = static_cast<OpKind>(op);
  out.incarnation = IncarnationId{incarnation};
  return Status::success();
}

}  // namespace

bool is_admitted_stage_refusal(Reason reason) noexcept {
  switch (reason) {
    case Reason::InsufficientAvailable:
    case Reason::InsufficientOutstanding:
    case Reason::InsufficientProtected:
    case Reason::CapacityExceeded:
    case Reason::CapacityShrinkViolation:
    case Reason::CapacityOutOfRange:
    case Reason::CountZero:
    case Reason::CountOverflow:
    case Reason::PolicyRefused:
    case Reason::ExhaustedHard:
    case Reason::NotExhausted:
    case Reason::NothingToRetire:
    case Reason::InvalidState:
    case Reason::GrantMismatch:
    case Reason::ProducerMismatch:
    case Reason::ConsumerMismatch:
    case Reason::ResourceMismatch:
    case Reason::DomainMismatch:
    case Reason::AccountExists:
    case Reason::AccountUnknown:
    case Reason::ProfileNotSupported:
    case Reason::DuplicateAttempt:
      return true;
    default:
      return false;
  }
}

Status encode_durable_attempt(const DurableAttempt& attempt, std::vector<std::uint8_t>& out) {
  out.clear();
  ByteWriter writer(out);
  encode_request_body(writer, attempt.request);
  encode_outcome_body(writer, attempt.outcome);
  writer.boolean(attempt.has_account);
  writer.boolean(attempt.created_account);
  if (attempt.has_account) {
    const DurableAccount& account = attempt.account;
    encode_account_config(writer, account.config);
    writer.u64(account.epoch.value());
    writer.u64(account.generation.value());
    writer.u64(account.capacity);
    encode_totals(writer, account.totals);
    writer.boolean(account.hard_exhausted);
    writer.u16(static_cast<std::uint16_t>(account.exhaustion_cause));
    writer.digest(account.state_digest);
    writer.u64(account.incarnations.size());
    for (const IncarnationRecord& record : account.incarnations) encode_incarnation(writer, record);
    writer.u64(account.fences.size());
    for (const FenceRecord& fence : account.fences) encode_fence(writer, fence);
    writer.u64(account.ambiguous.size());
    for (const AmbiguousNote& note : account.ambiguous) encode_ambiguous(writer, note);
    writer.u64(account.committed_attempts);
    writer.u64(account.refused_attempts);
    writer.u64(account.attempt_window.size());
    for (const auto& entry : account.attempt_window) {
      writer.u64(entry.first);
      writer.u64(entry.second);
    }
  }
  writer.u64(attempt.engine_applied_attempts);
  writer.u64(attempt.engine_refused_attempts);
  return Status::success();
}

Status decode_durable_attempt(const std::uint8_t* data, std::size_t size, DurableAttempt& out) {
  ByteReader reader(data, size);
  Status status = decode_request_body(reader, out.request, 4096u);
  if (!status.ok()) return status;
  status = decode_outcome_body(reader, out.outcome);
  if (!status.ok()) return status;
  if (!reader.boolean(out.has_account) || !reader.boolean(out.created_account)) {
    return Status::refused(Reason::TruncatedInput, reader.position());
  }
  if (out.created_account && !out.has_account) return Status::refused(Reason::MalformedMessage, 11);
  if (out.created_account && !out.outcome.applied) return Status::refused(Reason::MalformedMessage, 12);
  if (out.has_account && !out.created_account && !out.outcome.applied && out.outcome.decided) {
    // A decided refusal legitimately carries the unchanged account state.
  }
  if (out.has_account) {
    DurableAccount& account = out.account;
    status = decode_account_config(reader, account.config);
    if (!status.ok()) return status;
    std::uint64_t epoch = 0;
    std::uint64_t generation = 0;
    std::uint16_t cause = 0;
    if (!reader.u64(epoch) || !reader.u64(generation) || !reader.u64(account.capacity)) {
      return Status::refused(Reason::TruncatedInput, reader.position());
    }
    account.epoch = EpochId{epoch};
    account.generation = Generation{generation};
    status = decode_totals(reader, account.totals);
    if (!status.ok()) return status;
    if (!reader.boolean(account.hard_exhausted) || !reader.u16(cause)) {
      return Status::refused(Reason::TruncatedInput, reader.position());
    }
    if (!is_valid_reason(cause)) return Status::refused(Reason::MalformedMessage, cause);
    account.exhaustion_cause = static_cast<Reason>(cause);
    if (!reader.digest(account.state_digest)) return Status::refused(Reason::TruncatedInput, reader.position());

    std::uint64_t count = 0;
    if (!reader.u64(count) || !within_budget(reader, count, 112)) {
      return Status::refused(Reason::MalformedMessage, 13);
    }
    account.incarnations.resize(static_cast<std::size_t>(count));
    for (std::uint64_t i = 0; i < count; ++i) {
      status = decode_incarnation(reader, account.incarnations[static_cast<std::size_t>(i)]);
      if (!status.ok()) return status;
    }
    if (!reader.u64(count) || !within_budget(reader, count, 40)) {
      return Status::refused(Reason::MalformedMessage, 14);
    }
    account.fences.resize(static_cast<std::size_t>(count));
    for (std::uint64_t i = 0; i < count; ++i) {
      status = decode_fence(reader, account.fences[static_cast<std::size_t>(i)]);
      if (!status.ok()) return status;
    }
    if (!reader.u64(count) || !within_budget(reader, count, 40)) {
      return Status::refused(Reason::MalformedMessage, 15);
    }
    account.ambiguous.resize(static_cast<std::size_t>(count));
    for (std::uint64_t i = 0; i < count; ++i) {
      status = decode_ambiguous(reader, account.ambiguous[static_cast<std::size_t>(i)]);
      if (!status.ok()) return status;
    }
    if (!reader.u64(account.committed_attempts) || !reader.u64(account.refused_attempts)) {
      return Status::refused(Reason::TruncatedInput, reader.position());
    }
    if (!reader.u64(count) || !within_budget(reader, count, 16)) {
      return Status::refused(Reason::MalformedMessage, 16);
    }
    account.attempt_window.resize(static_cast<std::size_t>(count));
    for (std::uint64_t i = 0; i < count; ++i) {
      std::uint64_t incarnation = 0;
      std::uint64_t sequence = 0;
      if (!reader.u64(incarnation) || !reader.u64(sequence)) {
        return Status::refused(Reason::TruncatedInput, reader.position());
      }
      account.attempt_window[static_cast<std::size_t>(i)] = {incarnation, sequence};
    }
  }
  if (!reader.u64(out.engine_applied_attempts) || !reader.u64(out.engine_refused_attempts)) {
    return Status::refused(Reason::TruncatedInput, reader.position());
  }
  if (!reader.done()) return Status::refused(Reason::MalformedMessage, reader.position());
  return Status::success();
}

Status encode_durable_state(const DurableState& state, std::vector<std::uint8_t>& out) {
  out.clear();
  ByteWriter writer(out);
  writer.u64(state.journal_sequence);
  writer.digest(state.chain);
  writer.u64(state.snapshots);
  writer.digest(state.state_digest);
  writer.u64(state.accounts.size());
  for (const DurableAccount& account : state.accounts) {
    std::vector<std::uint8_t> encoded;
    ByteWriter nested(encoded);
    encode_account_config(nested, account.config);
    nested.u64(account.epoch.value());
    nested.u64(account.generation.value());
    nested.u64(account.capacity);
    encode_totals(nested, account.totals);
    nested.boolean(account.hard_exhausted);
    nested.u16(static_cast<std::uint16_t>(account.exhaustion_cause));
    nested.digest(account.state_digest);
    nested.u64(account.incarnations.size());
    for (const IncarnationRecord& record : account.incarnations) encode_incarnation(nested, record);
    nested.u64(account.fences.size());
    for (const FenceRecord& fence : account.fences) encode_fence(nested, fence);
    nested.u64(account.ambiguous.size());
    for (const AmbiguousNote& note : account.ambiguous) encode_ambiguous(nested, note);
    nested.u64(account.committed_attempts);
    nested.u64(account.refused_attempts);
    nested.u64(account.attempt_window.size());
    for (const auto& entry : account.attempt_window) {
      nested.u64(entry.first);
      nested.u64(entry.second);
    }
    writer.u32(static_cast<std::uint32_t>(encoded.size()));
    writer.raw(encoded.data(), encoded.size());
  }
  return Status::success();
}

Status decode_durable_state(const std::uint8_t* data, std::size_t size, DurableState& out) {
  ByteReader reader(data, size);
  if (!reader.u64(out.journal_sequence) || !reader.digest(out.chain) || !reader.u64(out.snapshots) ||
      !reader.digest(out.state_digest)) {
    return Status::refused(Reason::TruncatedInput, reader.position());
  }
  std::uint64_t account_count = 0;
  if (!reader.u64(account_count) || !within_budget(reader, account_count, 4)) {
    return Status::refused(Reason::MalformedMessage, 17);
  }
  out.accounts.clear();
  out.accounts.reserve(static_cast<std::size_t>(account_count));
  for (std::uint64_t i = 0; i < account_count; ++i) {
    std::uint32_t length = 0;
    if (!reader.u32(length) || length > reader.remaining()) {
      return Status::refused(Reason::TruncatedInput, reader.position());
    }
    const std::uint8_t* start = data + reader.position();
    if (!reader.skip(length)) return Status::refused(Reason::TruncatedInput, reader.position());
    ByteReader nested(start, length);
    DurableAccount account{};
    Status status = decode_account_config(nested, account.config);
    if (!status.ok()) return status;
    std::uint64_t epoch = 0;
    std::uint64_t generation = 0;
    std::uint16_t cause = 0;
    if (!nested.u64(epoch) || !nested.u64(generation) || !nested.u64(account.capacity)) {
      return Status::refused(Reason::TruncatedInput, nested.position());
    }
    account.epoch = EpochId{epoch};
    account.generation = Generation{generation};
    status = decode_totals(nested, account.totals);
    if (!status.ok()) return status;
    if (!nested.boolean(account.hard_exhausted) || !nested.u16(cause)) {
      return Status::refused(Reason::TruncatedInput, nested.position());
    }
    if (!is_valid_reason(cause)) return Status::refused(Reason::MalformedMessage, cause);
    account.exhaustion_cause = static_cast<Reason>(cause);
    if (!nested.digest(account.state_digest)) return Status::refused(Reason::TruncatedInput, nested.position());
    std::uint64_t count = 0;
    if (!nested.u64(count) || !within_budget(nested, count, 112)) {
      return Status::refused(Reason::MalformedMessage, 18);
    }
    account.incarnations.resize(static_cast<std::size_t>(count));
    for (std::uint64_t k = 0; k < count; ++k) {
      status = decode_incarnation(nested, account.incarnations[static_cast<std::size_t>(k)]);
      if (!status.ok()) return status;
    }
    if (!nested.u64(count) || !within_budget(nested, count, 40)) {
      return Status::refused(Reason::MalformedMessage, 19);
    }
    account.fences.resize(static_cast<std::size_t>(count));
    for (std::uint64_t k = 0; k < count; ++k) {
      status = decode_fence(nested, account.fences[static_cast<std::size_t>(k)]);
      if (!status.ok()) return status;
    }
    if (!nested.u64(count) || !within_budget(nested, count, 40)) {
      return Status::refused(Reason::MalformedMessage, 20);
    }
    account.ambiguous.resize(static_cast<std::size_t>(count));
    for (std::uint64_t k = 0; k < count; ++k) {
      status = decode_ambiguous(nested, account.ambiguous[static_cast<std::size_t>(k)]);
      if (!status.ok()) return status;
    }
    if (!nested.u64(account.committed_attempts) || !nested.u64(account.refused_attempts)) {
      return Status::refused(Reason::TruncatedInput, nested.position());
    }
    if (!nested.u64(count) || !within_budget(nested, count, 16)) {
      return Status::refused(Reason::MalformedMessage, 21);
    }
    account.attempt_window.resize(static_cast<std::size_t>(count));
    for (std::uint64_t k = 0; k < count; ++k) {
      std::uint64_t incarnation = 0;
      std::uint64_t sequence = 0;
      if (!nested.u64(incarnation) || !nested.u64(sequence)) {
        return Status::refused(Reason::TruncatedInput, nested.position());
      }
      account.attempt_window[static_cast<std::size_t>(k)] = {incarnation, sequence};
    }
    if (!nested.done()) return Status::refused(Reason::MalformedMessage, 22);
    out.accounts.push_back(std::move(account));
  }
  if (!reader.done()) return Status::refused(Reason::MalformedMessage, reader.position());
  return Status::success();
}

}  // namespace creditfabric
