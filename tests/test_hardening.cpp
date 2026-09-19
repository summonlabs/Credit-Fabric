// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Defects found by the post-first-green hardening pass, each pinned by a test:
//   - shutdown could wait forever on an idle client socket;
//   - a zero structural limit could become a division by zero or an erase on an
//     empty container;
//   - a sequence watermark at the top of its range could wrap into a live slot;
//   - a huge externally supplied count had to be refused, not wrapped.

#include <atomic>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "support/fixture.hpp"
#include "support/test_framework.hpp"

#include "creditfabric/coordinator.hpp"
#include "creditfabric/platform.hpp"

using namespace creditfabric;

namespace {

constexpr std::uint64_t kMax = (std::numeric_limits<std::uint64_t>::max)();

/// Connects and completes the handshake, then goes quiet while keeping the
/// socket open.
Socket idle_handshaken_client(std::uint16_t port) {
  Socket socket;
  const Status connected = Socket::connect_to("127.0.0.1", port, socket);
  if (!connected.ok()) return socket;
  HelloMessage hello{};
  hello.publisher = PublisherId{0xDEADull};
  hello.boot = BootId{0xBEEFull};
  hello.label = "idle";
  std::vector<std::uint8_t> payload;
  const Status encoded = encode_hello(hello, payload);
  if (!encoded.ok()) return Socket{};
  const Status sent = socket.send_frame(MessageType::Hello, payload);
  if (!sent.ok()) return Socket{};
  MessageType type = MessageType::Bye;
  std::vector<std::uint8_t> ack;
  bool closed = false;
  const Status received = socket.recv_frame(1u << 16, type, payload, closed);
  if (!received.ok()) return Socket{};
  (void)ack;
  return socket;
}

}  // namespace

CF_TEST(shutdown_is_not_held_hostage_by_an_idle_client) {
  cftest::Fixture fixture;
  REQUIRE(fixture.recover().ok());
  REQUIRE(fixture.open(1000).ok());
  CoordinatorServer server(*fixture.engine, CoordinatorOptions{});
  REQUIRE(server.start().ok());

  Socket idle = idle_handshaken_client(server.port());
  REQUIRE(idle.valid());
  // The client is now silent. Shutdown must still complete.
  CHECK(server.stop().ok());
  CHECK(!server.running());
  (void)idle.close();
}

CF_TEST(shutdown_is_not_held_hostage_by_a_silent_connection) {
  cftest::Fixture fixture;
  REQUIRE(fixture.recover().ok());
  REQUIRE(fixture.open(1000).ok());
  CoordinatorServer server(*fixture.engine, CoordinatorOptions{});
  REQUIRE(server.start().ok());

  Socket silent;
  REQUIRE(Socket::connect_to("127.0.0.1", server.port(), silent).ok());
  CHECK(server.stop().ok());
  (void)silent.close();
}

CF_TEST(shutdown_during_live_traffic_completes) {
  cftest::Fixture fixture;
  REQUIRE(fixture.recover().ok());
  REQUIRE(fixture.open(100000).ok());
  CoordinatorServer server(*fixture.engine, CoordinatorOptions{});
  REQUIRE(server.start().ok());

  std::atomic<bool> stop{false};
  std::thread traffic([&server, &stop]() {
    std::uint64_t publisher = 1;
    while (!stop.load(std::memory_order_acquire)) {
      CoordinatorClient client([&server, publisher]() {
        ClientOptions options{};
        options.port = server.port();
        options.domain = DomainId{0xD0A1ull};
        options.publisher = PublisherId{0xC000ull + publisher};
        options.boot = BootId{make_boot_id()};
        return options;
      }());
      ++publisher;
      if (!client.connect().ok()) continue;
      CreditRequest request{};
      request.authority.account = AccountId{0xACCEull};
      request.op = OpKind::Issue;
      request.count = 1;
      (void)client.submit(request);
      client.close();
    }
  });

  // Let some traffic flow, then stop while it is still running.
  for (int i = 0; i < 200; ++i) std::this_thread::yield();
  CHECK(server.stop().ok());
  stop.store(true, std::memory_order_release);
  traffic.join();
  CHECK(fixture.view().closed);
}

CF_TEST(zero_structural_limits_are_clamped_not_divided_by) {
  cftest::Fixture fixture;
  fixture.limits.max_accounts = 0;
  fixture.limits.max_incarnations_per_account = 0;
  fixture.limits.max_fences_per_account = 0;
  fixture.limits.max_attempt_window = 0;
  fixture.limits.max_ambiguous_attempts = 0;
  fixture.limits.max_refusal_log = 0;
  fixture.limits.max_note_bytes = 0;
  fixture.limits.max_explanation_incarnations = 0;
  fixture.limits.max_explanation_fences = 0;
  fixture.limits.max_explanation_refusals = 0;
  fixture.limits.journal_records_per_snapshot = 0;
  fixture.engine = std::make_unique<CreditEngine>(fixture.limits,
                                                  std::make_unique<cftest::BorrowedJournal>(fixture.journal),
                                                  fixture.me);
  REQUIRE(fixture.recover().ok());
  // max_accounts is clamped to exactly one, which is enough for this account.
  REQUIRE(fixture.open(1000).ok());
  REQUIRE(fixture.call(OpKind::Issue, 10).ok());
  // A decided refusal exercises the bounded refusal ring. Returning more than
  // the in-flight credit is refused rather than wrapped.
  CHECK(!fixture.call(OpKind::Return, 11).ok());
  const Explanation explanation = fixture.explain();
  CHECK(explanation.found);
  CHECK(explanation.view.closed);
}

CF_TEST(an_exhausted_sequence_index_is_refused_rather_than_wrapped) {
  cftest::Fixture fixture;
  REQUIRE(fixture.recover().ok());
  REQUIRE(fixture.open(1000).ok());

  CreditRequest request{};
  request.op = OpKind::Issue;
  request.count = 1;
  request.presenter = fixture.me;
  request.authority = fixture.authority_of(fixture.me);
  request.attempt = fixture.attempts.next();
  request.sequence = kMax;
  // The watermark is 1, so an attempt at the top of the range is a gap, never a
  // successful placement.
  const CreditOutcome outcome = fixture.engine->apply(request);
  CHECK(!outcome.ok());
  CHECK_EQ(outcome.status.reason, Reason::SequenceGap);
  CHECK(!outcome.decided);
  CHECK_EQ(fixture.view().issued, 0ull);
}

CF_TEST(a_huge_count_is_refused_without_wrapping) {
  cftest::Fixture fixture;
  REQUIRE(fixture.recover().ok());
  REQUIRE(fixture.open(1000, 1000, ProfileKind::Abstract, kMax).ok());

  const CreditOutcome outcome = fixture.call(OpKind::Issue, kMax);
  CHECK(!outcome.ok());
  CHECK_EQ(outcome.status.reason, Reason::InsufficientAvailable);
  CHECK_EQ(outcome.status.detail, 1000ull);
  CHECK_EQ(fixture.view().issued, 0ull);
  CHECK(fixture.view().closed);

  const CreditOutcome consume = fixture.call(OpKind::Consume, kMax);
  CHECK(!consume.ok());
  CHECK_EQ(consume.status.reason, Reason::InsufficientOutstanding);

  const CreditOutcome shrink = fixture.call(OpKind::SetCapacity, 0, kMax);
  CHECK(!shrink.ok());
  CHECK_EQ(shrink.status.reason, Reason::CapacityExceeded);
  CHECK_EQ(fixture.view().capacity, 1000ull);
}

CF_TEST(a_note_beyond_the_configured_bound_is_refused_before_it_is_stored) {
  cftest::Fixture fixture;
  REQUIRE(fixture.recover().ok());
  REQUIRE(fixture.open(1000).ok());
  CreditRequest request{};
  request.op = OpKind::Issue;
  request.count = 1;
  request.presenter = fixture.me;
  request.authority = fixture.authority_of(fixture.me);
  request.attempt = fixture.attempts.next();
  request.sequence = fixture.sequence;
  request.note = std::string(fixture.limits.max_note_bytes + 1, 'n');
  const CreditOutcome outcome = fixture.engine->apply(request);
  CHECK(!outcome.ok());
  CHECK_EQ(outcome.status.reason, Reason::OversizedFrame);
  CHECK(!outcome.decided);
}

CF_TEST(a_record_beyond_the_journal_budget_is_refused) {
  InMemoryJournal journal{1024};
  REQUIRE(journal.open().ok());
  const std::vector<std::uint8_t> payload(2048, 0x5A);
  const Status status = journal.append(1, JournalRecordKind::Attempt, Digest128{1, 2}, payload);
  CHECK(!status.ok());
  CHECK_EQ(status.reason, Reason::BudgetExceeded);
  CHECK_EQ(journal.records(), 0ull);
}

CF_TEST(the_connection_limit_is_never_zero) {
  cftest::Fixture fixture;
  REQUIRE(fixture.recover().ok());
  REQUIRE(fixture.open(1000).ok());
  CoordinatorOptions options{};
  options.max_connections = 0;
  CoordinatorServer server(*fixture.engine, options);
  REQUIRE(server.start().ok());

  CoordinatorClient client([&server]() {
    ClientOptions client_options{};
    client_options.port = server.port();
    client_options.domain = DomainId{0xD0A1ull};
    client_options.publisher = PublisherId{0xD00Dull};
    client_options.boot = BootId{make_boot_id()};
    return client_options;
  }());
  CHECK(client.connect().ok());
  client.close();
  CHECK(server.stop().ok());
}

CF_TEST(a_malformed_frame_never_reaches_the_authority) {
  cftest::Fixture fixture;
  REQUIRE(fixture.recover().ok());
  REQUIRE(fixture.open(1000).ok());
  CoordinatorServer server(*fixture.engine, CoordinatorOptions{});
  REQUIRE(server.start().ok());

  const std::uint64_t applied_before = fixture.engine->stats().attempts_applied;
  for (int trial = 0; trial < 8; ++trial) {
    Socket socket;
    REQUIRE(Socket::connect_to("127.0.0.1", server.port(), socket).ok());
    std::vector<std::uint8_t> garbage(24);
    for (std::size_t i = 0; i < garbage.size(); ++i) {
      garbage[i] = static_cast<std::uint8_t>((trial + 1) * (i + 7));
    }
    (void)socket.send_all(garbage.data(), garbage.size());
    (void)socket.close();
  }
  CHECK_EQ(fixture.engine->stats().attempts_applied, applied_before);
  CHECK_EQ(fixture.view().issued, 0ull);
  CHECK(server.stop().ok());
}

CF_TEST_MAIN()
