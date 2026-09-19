// Credit Fabric - bounded framed wire codec.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Frame layout (little-endian):
//   magic   u32  0x31524643 ("CFR1")
//   version u8   kFormatVersion
//   type    u8
//   flags   u16  reserved, must be zero
//   length  u32  payload length, bounded by the receiver's limit
//   payload bytes
//   crc     u32  CRC-32C over magic..payload
//
// A receiver validates magic, version, reserved flags, length bound and CRC
// before it looks at a single payload byte. Anything else closes the
// connection with a named refusal.

#ifndef CREDITFABRIC_WIRE_HPP
#define CREDITFABRIC_WIRE_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "creditfabric/bytes.hpp"
#include "creditfabric/engine.hpp"
#include "creditfabric/policy.hpp"
#include "creditfabric/status.hpp"
#include "creditfabric/version.hpp"

namespace creditfabric {

inline constexpr std::uint32_t kWireMagic = 0x31524643u;
inline constexpr std::size_t kFrameHeaderBytes = 12u;
inline constexpr std::size_t kFrameTrailerBytes = 4u;
inline constexpr std::size_t kFrameOverheadBytes = kFrameHeaderBytes + kFrameTrailerBytes;

enum class MessageType : std::uint8_t {
  Hello = 1,
  HelloAck = 2,
  Request = 3,
  Response = 4,
  Bye = 5,
};

[[nodiscard]] const char* to_string(MessageType type) noexcept;

struct FrameHeader {
  std::uint8_t version{0};
  MessageType type{MessageType::Hello};
  std::uint16_t flags{0};
  std::uint32_t payload_length{0};
};

struct FrameView {
  FrameHeader header{};
  const std::uint8_t* payload{nullptr};
  std::size_t payload_length{0};
  std::size_t consumed{0};
};

[[nodiscard]] Status encode_frame(MessageType type, const std::vector<std::uint8_t>& payload,
                                  std::vector<std::uint8_t>& out);
[[nodiscard]] Status decode_frame(const std::uint8_t* data, std::size_t size, std::size_t max_payload,
                                  FrameView& out);

// ---------------------------------------------------------------------------
// Shared field codecs. Used by both the wire and the durable record codec so
// that a request means exactly one thing in both places.
// ---------------------------------------------------------------------------

void encode_authority(ByteWriter& writer, const AuthorityVector& authority);
[[nodiscard]] Status decode_authority(ByteReader& reader, AuthorityVector& out);

void encode_account_config(ByteWriter& writer, const AccountConfig& config);
[[nodiscard]] Status decode_account_config(ByteReader& reader, AccountConfig& out);

void encode_view(ByteWriter& writer, const CreditView& view);
[[nodiscard]] Status decode_view(ByteReader& reader, CreditView& out);

void encode_request_body(ByteWriter& writer, const CreditRequest& request);
[[nodiscard]] Status decode_request_body(ByteReader& reader, CreditRequest& out, std::size_t max_note_bytes);

void encode_outcome_body(ByteWriter& writer, const CreditOutcome& outcome);
[[nodiscard]] Status decode_outcome_body(ByteReader& reader, CreditOutcome& out);

// ---------------------------------------------------------------------------
// Messages
// ---------------------------------------------------------------------------

struct HelloMessage {
  PublisherId publisher{};
  BootId boot{};
  std::string label{};  ///< bounded, advisory only
};

struct HelloAckMessage {
  std::uint32_t session{0};
  Digest128 challenge{};
  std::string server_label{};
  std::uint32_t max_payload{0};
  std::uint32_t format_version{0};
};

struct RequestMessage {
  CreditRequest request{};
};

struct ResponseMessage {
  CreditOutcome outcome{};
};

[[nodiscard]] Status encode_hello(const HelloMessage& message, std::vector<std::uint8_t>& out);
[[nodiscard]] Status decode_hello(const std::uint8_t* data, std::size_t size, HelloMessage& out);

[[nodiscard]] Status encode_hello_ack(const HelloAckMessage& message, std::vector<std::uint8_t>& out);
[[nodiscard]] Status decode_hello_ack(const std::uint8_t* data, std::size_t size, HelloAckMessage& out);

[[nodiscard]] Status encode_request_message(const CreditRequest& request, std::vector<std::uint8_t>& out);
[[nodiscard]] Status decode_request_message(const std::uint8_t* data, std::size_t size,
                                            std::size_t max_note_bytes, CreditRequest& out);

[[nodiscard]] Status encode_response_message(const CreditOutcome& outcome, std::vector<std::uint8_t>& out);
[[nodiscard]] Status decode_response_message(const std::uint8_t* data, std::size_t size, CreditOutcome& out);

}  // namespace creditfabric

#endif  // CREDITFABRIC_WIRE_HPP
