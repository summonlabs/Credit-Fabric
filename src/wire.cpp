// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "creditfabric/wire.hpp"

#include <cstring>

#include "creditfabric/crc32c.hpp"

namespace creditfabric {
namespace {

constexpr std::size_t kMaxLabelBytes = 64u;

void write_header(std::uint8_t* destination, MessageType type, std::uint16_t flags, std::uint32_t payload_length) {
  const std::uint32_t magic = kWireMagic;
  std::memcpy(destination, &magic, 4);
  destination[4] = static_cast<std::uint8_t>(kFormatVersion);
  destination[5] = static_cast<std::uint8_t>(type);
  destination[6] = static_cast<std::uint8_t>(flags & 0xFFu);
  destination[7] = static_cast<std::uint8_t>((flags >> 8) & 0xFFu);
  std::memcpy(destination + 8, &payload_length, 4);
}

std::uint32_t read_u32(const std::uint8_t* source) {
  std::uint32_t value = 0;
  std::memcpy(&value, source, 4);
  return value;
}

}  // namespace

const char* to_string(MessageType type) noexcept {
  switch (type) {
    case MessageType::Hello:
      return "Hello";
    case MessageType::HelloAck:
      return "HelloAck";
    case MessageType::Request:
      return "Request";
    case MessageType::Response:
      return "Response";
    case MessageType::Bye:
      return "Bye";
  }
  return "Unknown";
}

Status encode_frame(MessageType type, const std::vector<std::uint8_t>& payload, std::vector<std::uint8_t>& out) {
  if (payload.size() > 0xFFFFFFFFull) return Status::refused(Reason::OversizedFrame, payload.size());
  out.clear();
  out.resize(kFrameHeaderBytes);
  write_header(out.data(), type, 0, static_cast<std::uint32_t>(payload.size()));
  out.insert(out.end(), payload.begin(), payload.end());
  const std::uint32_t crc = crc32c(out.data(), out.size());
  const auto* crc_bytes = reinterpret_cast<const std::uint8_t*>(&crc);
  out.insert(out.end(), crc_bytes, crc_bytes + 4);
  return Status::success();
}

Status decode_frame(const std::uint8_t* data, std::size_t size, std::size_t max_payload, FrameView& out) {
  if (size < kFrameOverheadBytes) return Status::refused(Reason::TruncatedInput, size);
  if (read_u32(data) != kWireMagic) return Status::refused(Reason::MalformedMessage, 1);
  if (data[4] != static_cast<std::uint8_t>(kFormatVersion)) {
    return Status::refused(Reason::UnsupportedVersion, data[4]);
  }
  const auto type = static_cast<MessageType>(data[5]);
  if (type != MessageType::Hello && type != MessageType::HelloAck && type != MessageType::Request &&
      type != MessageType::Response && type != MessageType::Bye) {
    return Status::refused(Reason::MalformedMessage, 2);
  }
  const std::uint16_t flags = static_cast<std::uint16_t>(data[6]) | static_cast<std::uint16_t>(data[7] << 8);
  if (flags != 0) return Status::refused(Reason::MalformedMessage, 3);
  const std::uint32_t length = read_u32(data + 8);
  if (length > max_payload) return Status::refused(Reason::OversizedFrame, length);
  const std::size_t total = kFrameOverheadBytes + static_cast<std::size_t>(length);
  if (size < total) return Status::refused(Reason::TruncatedInput, size);

  const std::uint32_t expected = read_u32(data + kFrameHeaderBytes + length);
  const std::uint32_t actual = crc32c(data, kFrameHeaderBytes + static_cast<std::size_t>(length));
  if (expected != actual) return Status::refused(Reason::ChecksumMismatch, expected ^ actual);

  out.header.version = data[4];
  out.header.type = type;
  out.header.flags = flags;
  out.header.payload_length = length;
  out.payload = data + kFrameHeaderBytes;
  out.payload_length = length;
  out.consumed = total;
  return Status::success();
}

// ---------------------------------------------------------------------------
// Field codecs
// ---------------------------------------------------------------------------

void encode_authority(ByteWriter& writer, const AuthorityVector& authority) {
  writer.u64(authority.domain.value());
  writer.u64(authority.account.value());
  writer.u64(authority.epoch.value());
  writer.u64(authority.generation.value());
  writer.u64(authority.incarnation.value());
}

Status decode_authority(ByteReader& reader, AuthorityVector& out) {
  std::uint64_t domain = 0;
  std::uint64_t account = 0;
  std::uint64_t epoch = 0;
  std::uint64_t generation = 0;
  std::uint64_t incarnation = 0;
  if (!reader.u64(domain) || !reader.u64(account) || !reader.u64(epoch) || !reader.u64(generation) ||
      !reader.u64(incarnation)) {
    return Status::refused(Reason::TruncatedInput, reader.position());
  }
  out.domain = DomainId{domain};
  out.account = AccountId{account};
  out.epoch = EpochId{epoch};
  out.generation = Generation{generation};
  out.incarnation = IncarnationId{incarnation};
  return Status::success();
}

void encode_account_config(ByteWriter& writer, const AccountConfig& config) {
  writer.u64(config.domain.value());
  writer.u64(config.account.value());
  writer.u64(config.resource.value());
  writer.u64(config.policy.value());
  writer.u64(config.capacity);
  writer.u64(config.max_capacity);
  writer.u8(static_cast<std::uint8_t>(config.profile));
  writer.u64(config.rules.min_issue);
  writer.u64(config.rules.max_issue_per_attempt);
  writer.boolean(config.rules.allow_protect);
  writer.boolean(config.rules.allow_return);
  writer.boolean(config.rules.allow_capacity_growth);
}

Status decode_account_config(ByteReader& reader, AccountConfig& out) {
  std::uint64_t domain = 0;
  std::uint64_t account = 0;
  std::uint64_t resource = 0;
  std::uint64_t policy = 0;
  std::uint8_t profile = 0;
  bool allow_protect = false;
  bool allow_return = false;
  bool allow_growth = false;
  if (!reader.u64(domain) || !reader.u64(account) || !reader.u64(resource) || !reader.u64(policy) ||
      !reader.u64(out.capacity) || !reader.u64(out.max_capacity) || !reader.u8(profile) ||
      !reader.u64(out.rules.min_issue) || !reader.u64(out.rules.max_issue_per_attempt) ||
      !reader.boolean(allow_protect) || !reader.boolean(allow_return) || !reader.boolean(allow_growth)) {
    return Status::refused(Reason::TruncatedInput, reader.position());
  }
  if (profile > static_cast<std::uint8_t>(ProfileKind::PhysicalValidated)) {
    return Status::refused(Reason::MalformedMessage, static_cast<std::uint64_t>(profile));
  }
  out.domain = DomainId{domain};
  out.account = AccountId{account};
  out.resource = ResourceId{resource};
  out.policy = PolicyId{policy};
  out.profile = static_cast<ProfileKind>(profile);
  out.rules.allow_protect = allow_protect;
  out.rules.allow_return = allow_return;
  out.rules.allow_capacity_growth = allow_growth;
  return Status::success();
}

void encode_view(ByteWriter& writer, const CreditView& view) {
  writer.u64(view.capacity);
  writer.u64(view.issued);
  writer.u64(view.consumed);
  writer.u64(view.returned);
  writer.u64(view.stale);
  writer.u64(view.spent);
  writer.u64(view.protected_credits);
  writer.u64(view.in_flight);
  writer.u64(view.outstanding);
  writer.u64(view.available);
  writer.boolean(view.closed);
}

Status decode_view(ByteReader& reader, CreditView& out) {
  if (!reader.u64(out.capacity) || !reader.u64(out.issued) || !reader.u64(out.consumed) || !reader.u64(out.returned) ||
      !reader.u64(out.stale) || !reader.u64(out.spent) || !reader.u64(out.protected_credits) ||
      !reader.u64(out.in_flight) || !reader.u64(out.outstanding) || !reader.u64(out.available) ||
      !reader.boolean(out.closed)) {
    return Status::refused(Reason::TruncatedInput, reader.position());
  }
  out.quarantined = out.stale;
  return Status::success();
}

void encode_request_body(ByteWriter& writer, const CreditRequest& request) {
  writer.digest(request.attempt.value);
  writer.u8(static_cast<std::uint8_t>(request.op));
  writer.u64(request.sequence);
  encode_authority(writer, request.authority);
  writer.u64(request.presenter.publisher.value());
  writer.u64(request.presenter.boot.value());
  writer.u64(request.presenter.id.value());
  writer.u64(request.resource.value());
  writer.u64(request.producer.value());
  writer.u64(request.consumer.value());
  writer.u64(request.grant.value());
  writer.u64(request.target.value());
  writer.u64(request.count);
  writer.u64(request.new_capacity);
  writer.digest(request.proof);
  writer.digest(request.expected_state);
  encode_account_config(writer, request.config);
  writer.u8(static_cast<std::uint8_t>(request.decision));
  writer.u16(static_cast<std::uint16_t>(request.fence_cause));
  (void)writer.text(request.note, 4096u);
}

Status decode_request_body(ByteReader& reader, CreditRequest& out, std::size_t max_note_bytes) {
  std::uint8_t op = 0;
  std::uint8_t decision = 0;
  std::uint16_t fence_cause = 0;
  if (!reader.digest(out.attempt.value) || !reader.u8(op) || !reader.u64(out.sequence)) {
    return Status::refused(Reason::TruncatedInput, reader.position());
  }
  if (!is_valid_op(op)) return Status::refused(Reason::MalformedMessage, op);
  out.op = static_cast<OpKind>(op);

  Status status = decode_authority(reader, out.authority);
  if (!status.ok()) return status;

  std::uint64_t publisher = 0;
  std::uint64_t boot = 0;
  std::uint64_t incarnation = 0;
  if (!reader.u64(publisher) || !reader.u64(boot) || !reader.u64(incarnation)) {
    return Status::refused(Reason::TruncatedInput, reader.position());
  }
  out.presenter.publisher = PublisherId{publisher};
  out.presenter.boot = BootId{boot};
  out.presenter.id = IncarnationId{incarnation};

  std::uint64_t resource = 0;
  std::uint64_t producer = 0;
  std::uint64_t consumer = 0;
  std::uint64_t grant = 0;
  std::uint64_t target = 0;
  if (!reader.u64(resource) || !reader.u64(producer) || !reader.u64(consumer) || !reader.u64(grant) ||
      !reader.u64(target) || !reader.u64(out.count) || !reader.u64(out.new_capacity)) {
    return Status::refused(Reason::TruncatedInput, reader.position());
  }
  out.resource = ResourceId{resource};
  out.producer = ProducerId{producer};
  out.consumer = ConsumerId{consumer};
  out.grant = GrantId{grant};
  out.target = IncarnationId{target};

  if (!reader.digest(out.proof) || !reader.digest(out.expected_state)) {
    return Status::refused(Reason::TruncatedInput, reader.position());
  }
  status = decode_account_config(reader, out.config);
  if (!status.ok()) return status;
  if (!reader.u8(decision)) return Status::refused(Reason::TruncatedInput, reader.position());
  if (decision > static_cast<std::uint8_t>(RevalidationDecision::ToConsumed)) {
    return Status::refused(Reason::MalformedMessage, decision);
  }
  out.decision = static_cast<RevalidationDecision>(decision);
  if (!reader.u16(fence_cause)) return Status::refused(Reason::TruncatedInput, reader.position());
  if (!is_valid_reason(fence_cause)) return Status::refused(Reason::MalformedMessage, fence_cause);
  out.fence_cause = static_cast<Reason>(fence_cause);
  if (!reader.text(out.note, max_note_bytes)) return Status::refused(Reason::MalformedMessage, reader.position());
  return Status::success();
}

void encode_outcome_body(ByteWriter& writer, const CreditOutcome& outcome) {
  writer.u16(static_cast<std::uint16_t>(outcome.status.reason));
  writer.u64(outcome.status.detail);
  writer.u8(static_cast<std::uint8_t>(outcome.op));
  writer.u64(outcome.sequence);
  writer.boolean(outcome.applied);
  writer.boolean(outcome.duplicate);
  writer.boolean(outcome.decided);
  encode_authority(writer, outcome.authority);
  encode_view(writer, outcome.view);
  writer.digest(outcome.state_digest);
  writer.u64(static_cast<std::uint64_t>(outcome.ambiguous_pending));
}

Status decode_outcome_body(ByteReader& reader, CreditOutcome& out) {
  std::uint16_t reason = 0;
  std::uint8_t op = 0;
  if (!reader.u16(reason) || !reader.u64(out.status.detail)) {
    return Status::refused(Reason::TruncatedInput, reader.position());
  }
  if (!is_valid_reason(reason)) return Status::refused(Reason::MalformedMessage, reason);
  out.status.reason = static_cast<Reason>(reason);
  if (!reader.u8(op)) return Status::refused(Reason::TruncatedInput, reader.position());
  if (!is_valid_op(op)) return Status::refused(Reason::MalformedMessage, op);
  out.op = static_cast<OpKind>(op);
  if (!reader.u64(out.sequence) || !reader.boolean(out.applied) || !reader.boolean(out.duplicate) ||
      !reader.boolean(out.decided)) {
    return Status::refused(Reason::TruncatedInput, reader.position());
  }
  Status status = decode_authority(reader, out.authority);
  if (!status.ok()) return status;
  status = decode_view(reader, out.view);
  if (!status.ok()) return status;
  if (!reader.digest(out.state_digest)) return Status::refused(Reason::TruncatedInput, reader.position());
  std::uint64_t ambiguous = 0;
  if (!reader.u64(ambiguous)) return Status::refused(Reason::TruncatedInput, reader.position());
  out.ambiguous_pending = static_cast<std::size_t>(ambiguous);
  return Status::success();
}

// ---------------------------------------------------------------------------
// Messages
// ---------------------------------------------------------------------------

Status encode_hello(const HelloMessage& message, std::vector<std::uint8_t>& out) {
  out.clear();
  ByteWriter writer(out);
  writer.u64(message.publisher.value());
  writer.u64(message.boot.value());
  if (!writer.text(message.label, kMaxLabelBytes)) return Status::refused(Reason::OversizedFrame, message.label.size());
  return Status::success();
}

Status decode_hello(const std::uint8_t* data, std::size_t size, HelloMessage& out) {
  ByteReader reader(data, size);
  std::uint64_t publisher = 0;
  std::uint64_t boot = 0;
  if (!reader.u64(publisher) || !reader.u64(boot) || !reader.text(out.label, kMaxLabelBytes)) {
    return Status::refused(Reason::TruncatedInput, reader.position());
  }
  if (!reader.done()) return Status::refused(Reason::MalformedMessage, reader.position());
  out.publisher = PublisherId{publisher};
  out.boot = BootId{boot};
  if (out.publisher.is_unset() || out.boot.is_unset()) return Status::refused(Reason::MalformedMessage, 7);
  return Status::success();
}

Status encode_hello_ack(const HelloAckMessage& message, std::vector<std::uint8_t>& out) {
  out.clear();
  ByteWriter writer(out);
  writer.u32(message.session);
  writer.digest(message.challenge);
  writer.u32(message.max_payload);
  writer.u32(message.format_version);
  if (!writer.text(message.server_label, kMaxLabelBytes)) {
    return Status::refused(Reason::OversizedFrame, message.server_label.size());
  }
  return Status::success();
}

Status decode_hello_ack(const std::uint8_t* data, std::size_t size, HelloAckMessage& out) {
  ByteReader reader(data, size);
  if (!reader.u32(out.session) || !reader.digest(out.challenge) || !reader.u32(out.max_payload) ||
      !reader.u32(out.format_version) || !reader.text(out.server_label, kMaxLabelBytes)) {
    return Status::refused(Reason::TruncatedInput, reader.position());
  }
  if (!reader.done()) return Status::refused(Reason::MalformedMessage, reader.position());
  if (out.challenge.is_zero()) return Status::refused(Reason::MalformedMessage, 8);
  return Status::success();
}

Status encode_request_message(const CreditRequest& request, std::vector<std::uint8_t>& out) {
  out.clear();
  ByteWriter writer(out);
  encode_request_body(writer, request);
  return Status::success();
}

Status decode_request_message(const std::uint8_t* data, std::size_t size, std::size_t max_note_bytes,
                              CreditRequest& out) {
  ByteReader reader(data, size);
  const Status status = decode_request_body(reader, out, max_note_bytes);
  if (!status.ok()) return status;
  if (!reader.done()) return Status::refused(Reason::MalformedMessage, reader.position());
  return Status::success();
}

Status encode_response_message(const CreditOutcome& outcome, std::vector<std::uint8_t>& out) {
  out.clear();
  ByteWriter writer(out);
  encode_outcome_body(writer, outcome);
  return Status::success();
}

Status decode_response_message(const std::uint8_t* data, std::size_t size, CreditOutcome& out) {
  ByteReader reader(data, size);
  const Status status = decode_outcome_body(reader, out);
  if (!status.ok()) return status;
  if (!reader.done()) return Status::refused(Reason::MalformedMessage, reader.position());
  return Status::success();
}

}  // namespace creditfabric
