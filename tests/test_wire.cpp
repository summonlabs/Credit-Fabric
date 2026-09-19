// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Wire codec: every field round-trips, and every malformed shape is refused by
// name before a single payload byte is trusted.

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "support/fixture.hpp"
#include "support/test_framework.hpp"

#include "creditfabric/crc32c.hpp"
#include "creditfabric/wire.hpp"

using namespace creditfabric;

namespace {

CreditRequest sample_request() {
  CreditRequest request{};
  request.attempt = AttemptIdGenerator{0xABCDull}.next();
  request.op = OpKind::Issue;
  request.sequence = 42;
  request.authority.domain = DomainId{1};
  request.authority.account = AccountId{2};
  request.authority.epoch = EpochId{3};
  request.authority.generation = Generation{4};
  request.authority.incarnation = IncarnationId{5};
  request.presenter = Incarnation::make(PublisherId{6}, BootId{7});
  request.authority.incarnation = request.presenter.id;
  request.resource = ResourceId{8};
  request.producer = ProducerId{9};
  request.consumer = ConsumerId{10};
  request.grant = GrantId{11};
  request.target = IncarnationId{12};
  request.count = 13;
  request.new_capacity = 14;
  request.proof = Digest128{15, 16};
  request.expected_state = Digest128{17, 18};
  request.decision = RevalidationDecision::ToConsumed;
  request.fence_cause = Reason::Cancelled;
  request.note = "bounded provenance note";
  return request;
}

std::vector<std::uint8_t> frame_of(MessageType type, const std::vector<std::uint8_t>& payload) {
  std::vector<std::uint8_t> frame;
  (void)encode_frame(type, payload, frame);
  return frame;
}

}  // namespace

CF_TEST(frames_round_trip_and_validate_their_checksum) {
  const std::vector<std::uint8_t> payload{1, 2, 3, 4, 5};
  const std::vector<std::uint8_t> frame = frame_of(MessageType::Request, payload);
  CHECK_EQ(frame.size(), payload.size() + kFrameOverheadBytes);

  FrameView view{};
  REQUIRE(decode_frame(frame.data(), frame.size(), 1024, view).ok());
  CHECK(view.header.type == MessageType::Request);
  CHECK_EQ(view.payload_length, payload.size());
  CHECK_EQ(view.consumed, frame.size());
  CHECK(std::equal(payload.begin(), payload.end(), view.payload));

  // Any single-byte flip is detected.
  for (std::size_t i = 0; i < frame.size(); ++i) {
    std::vector<std::uint8_t> damaged = frame;
    damaged[i] = static_cast<std::uint8_t>(damaged[i] ^ 0x40u);
    FrameView damaged_view{};
    CHECK(!decode_frame(damaged.data(), damaged.size(), 1024, damaged_view).ok());
  }
}

CF_TEST(malformed_frames_are_refused_by_name) {
  const std::vector<std::uint8_t> payload{9, 9, 9};
  const std::vector<std::uint8_t> good = frame_of(MessageType::Response, payload);
  FrameView view{};

  // Truncated below the frame overhead.
  CHECK_EQ(decode_frame(good.data(), kFrameOverheadBytes - 1, 1024, view).reason, Reason::TruncatedInput);

  // Truncated payload.
  CHECK_EQ(decode_frame(good.data(), good.size() - 1, 1024, view).reason, Reason::TruncatedInput);

  // Wrong magic.
  std::vector<std::uint8_t> bad_magic = good;
  bad_magic[0] = static_cast<std::uint8_t>(bad_magic[0] ^ 0xFFu);
  CHECK_EQ(decode_frame(bad_magic.data(), bad_magic.size(), 1024, view).reason, Reason::MalformedMessage);

  // Unsupported format version.
  std::vector<std::uint8_t> bad_version = good;
  bad_version[4] = 99;
  CHECK_EQ(decode_frame(bad_version.data(), bad_version.size(), 1024, view).reason, Reason::UnsupportedVersion);

  // Reserved flags must be zero.
  std::vector<std::uint8_t> bad_flags = good;
  bad_flags[6] = 1;
  CHECK_EQ(decode_frame(bad_flags.data(), bad_flags.size(), 1024, view).reason, Reason::MalformedMessage);

  // Unknown message type.
  std::vector<std::uint8_t> bad_type = good;
  bad_type[5] = 200;
  CHECK_EQ(decode_frame(bad_type.data(), bad_type.size(), 1024, view).reason, Reason::MalformedMessage);

  // A declared length above the receiver's bound is refused without allocating.
  std::vector<std::uint8_t> huge = good;
  huge[8] = 0xFF;
  huge[9] = 0xFF;
  huge[10] = 0xFF;
  huge[11] = 0x7F;
  CHECK_EQ(decode_frame(huge.data(), huge.size(), 1024, view).reason, Reason::OversizedFrame);
}

CF_TEST(request_and_outcome_round_trip_exactly) {
  const CreditRequest original = sample_request();
  std::vector<std::uint8_t> payload;
  REQUIRE(encode_request_message(original, payload).ok());

  CreditRequest decoded{};
  REQUIRE(decode_request_message(payload.data(), payload.size(), 128, decoded).ok());
  CHECK(decoded.attempt == original.attempt);
  CHECK(decoded.op == original.op);
  CHECK_EQ(decoded.sequence, original.sequence);
  CHECK(decoded.authority == original.authority);
  CHECK(decoded.presenter == original.presenter);
  CHECK(decoded.resource == original.resource);
  CHECK(decoded.producer == original.producer);
  CHECK(decoded.consumer == original.consumer);
  CHECK(decoded.grant == original.grant);
  CHECK(decoded.target == original.target);
  CHECK_EQ(decoded.count, original.count);
  CHECK_EQ(decoded.new_capacity, original.new_capacity);
  CHECK(decoded.proof == original.proof);
  CHECK(decoded.expected_state == original.expected_state);
  CHECK(decoded.decision == original.decision);
  CHECK(decoded.fence_cause == original.fence_cause);
  CHECK_EQ(decoded.note, original.note);
  CHECK(decoded.digest() == original.digest());
}

CF_TEST(a_truncated_request_body_is_refused) {
  const CreditRequest original = sample_request();
  std::vector<std::uint8_t> payload;
  REQUIRE(encode_request_message(original, payload).ok());
  for (std::size_t cut = 0; cut < payload.size(); ++cut) {
    CreditRequest decoded{};
    const Status status = decode_request_message(payload.data(), cut, 128, decoded);
    CHECK(!status.ok());
  }
  // Trailing garbage is a structural error, not something to ignore.
  std::vector<std::uint8_t> padded = payload;
  padded.push_back(0);
  CreditRequest decoded{};
  CHECK(!decode_request_message(padded.data(), padded.size(), 128, decoded).ok());
}

CF_TEST(an_over_long_note_is_refused) {
  CreditRequest request = sample_request();
  request.note = std::string(200, 'x');
  std::vector<std::uint8_t> payload;
  REQUIRE(encode_request_message(request, payload).ok());
  CreditRequest decoded{};
  CHECK(!decode_request_message(payload.data(), payload.size(), 64, decoded).ok());
  CHECK(decode_request_message(payload.data(), payload.size(), 256, decoded).ok());
}

CF_TEST(outcomes_round_trip_exactly) {
  CreditOutcome outcome{};
  outcome.status = Status::refused(Reason::InsufficientAvailable, 12345);
  outcome.op = OpKind::Return;
  outcome.sequence = 77;
  outcome.applied = false;
  outcome.duplicate = true;
  outcome.decided = true;
  outcome.authority.domain = DomainId{1};
  outcome.authority.account = AccountId{2};
  outcome.authority.epoch = EpochId{3};
  outcome.authority.generation = Generation{4};
  outcome.authority.incarnation = IncarnationId{5};
  outcome.view.capacity = 1000;
  outcome.view.issued = 500;
  outcome.view.consumed = 100;
  outcome.view.returned = 50;
  outcome.view.stale = 25;
  outcome.view.spent = 75;
  outcome.view.protected_credits = 10;
  outcome.view.in_flight = 450;
  outcome.view.outstanding = 350;
  outcome.view.available = 540;
  outcome.view.quarantined = 25;
  outcome.view.closed = true;
  outcome.state_digest = Digest128{0xAAull, 0xBBull};
  outcome.ambiguous_pending = 3;

  std::vector<std::uint8_t> payload;
  REQUIRE(encode_response_message(outcome, payload).ok());
  CreditOutcome decoded{};
  REQUIRE(decode_response_message(payload.data(), payload.size(), decoded).ok());
  CHECK(decoded.status == outcome.status);
  CHECK(decoded.op == outcome.op);
  CHECK_EQ(decoded.sequence, outcome.sequence);
  CHECK_EQ(decoded.applied, outcome.applied);
  CHECK_EQ(decoded.duplicate, outcome.duplicate);
  CHECK_EQ(decoded.decided, outcome.decided);
  CHECK(decoded.authority == outcome.authority);
  CHECK(decoded.state_digest == outcome.state_digest);
  CHECK_EQ(decoded.ambiguous_pending, outcome.ambiguous_pending);
  CHECK_EQ(decoded.view.capacity, outcome.view.capacity);
  CHECK_EQ(decoded.view.in_flight, outcome.view.in_flight);
  CHECK_EQ(decoded.view.spent, outcome.view.spent);
}

CF_TEST(an_out_of_range_reason_code_is_refused) {
  std::vector<std::uint8_t> payload;
  ByteWriter writer(payload);
  writer.u16(60000);  // not a valid reason
  writer.u64(0);
  writer.u8(static_cast<std::uint8_t>(OpKind::Issue));
  writer.u64(1);
  writer.boolean(false);
  writer.boolean(false);
  writer.boolean(false);
  CreditOutcome decoded{};
  CHECK(!decode_response_message(payload.data(), payload.size(), decoded).ok());
}

CF_TEST(hello_messages_round_trip_and_reject_empty_identities) {
  HelloMessage hello{};
  hello.publisher = PublisherId{0x11ull};
  hello.boot = BootId{0x22ull};
  hello.label = "unit-test-worker";
  std::vector<std::uint8_t> payload;
  REQUIRE(encode_hello(hello, payload).ok());
  HelloMessage decoded{};
  REQUIRE(decode_hello(payload.data(), payload.size(), decoded).ok());
  CHECK(decoded.publisher == hello.publisher);
  CHECK(decoded.boot == hello.boot);
  CHECK_EQ(decoded.label, hello.label);

  HelloMessage unset{};
  unset.publisher = PublisherId{0};
  unset.boot = BootId{0};
  std::vector<std::uint8_t> unset_payload;
  REQUIRE(encode_hello(unset, unset_payload).ok());
  CHECK(!decode_hello(unset_payload.data(), unset_payload.size(), decoded).ok());

  HelloAckMessage ack{};
  ack.session = 3;
  ack.challenge = Digest128{4, 5};
  ack.max_payload = 65536;
  ack.format_version = kFormatVersion;
  ack.server_label = "coordinator";
  std::vector<std::uint8_t> ack_payload;
  REQUIRE(encode_hello_ack(ack, ack_payload).ok());
  HelloAckMessage decoded_ack{};
  REQUIRE(decode_hello_ack(ack_payload.data(), ack_payload.size(), decoded_ack).ok());
  CHECK_EQ(decoded_ack.session, 3ull);
  CHECK(decoded_ack.challenge == ack.challenge);
}

CF_TEST(random_bytes_never_decode_into_something_valid) {
  cftest::Rng rng{0x1DEAull};
  for (int trial = 0; trial < 20000; ++trial) {
    const std::size_t length = 1 + rng.bounded(64);
    std::vector<std::uint8_t> bytes(length);
    for (std::size_t i = 0; i < length; ++i) bytes[i] = static_cast<std::uint8_t>(rng.bounded(256));
    FrameView view{};
    const Status status = decode_frame(bytes.data(), bytes.size(), 4096, view);
    // Random bytes may only be accepted if they are a genuinely valid frame,
    // which random noise cannot be.
    CHECK(!status.ok());
  }
}

CF_TEST_MAIN()
