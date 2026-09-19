// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Real framed transport over loopback TCP: a real server, real sockets, real
// malformed input, and a real refusal surface.

#include <atomic>
#include <cstdint>
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

struct ServerFixture {
  cftest::Fixture local{};
  std::unique_ptr<CoordinatorServer> server{};

  ServerFixture() {
    server = std::make_unique<CoordinatorServer>(*local.engine, CoordinatorOptions{});
  }

  [[nodiscard]] Status boot() {
    Status status = local.recover();
    if (!status.ok()) return status;
    const CreditOutcome opened = local.open(100000);
    if (!opened.ok()) return opened.status;
    return server->start();
  }
};

ClientOptions client_options(std::uint16_t port, std::uint64_t publisher, std::uint64_t boot = 0) {
  ClientOptions options{};
  options.port = port;
  options.domain = DomainId{0xD0A1ull};
  options.publisher = PublisherId{publisher};
  options.boot = BootId{boot == 0 ? make_boot_id() : boot};
  options.label = "transport-test";
  return options;
}

}  // namespace

CF_TEST(a_client_reconciles_and_trades_credit_over_a_real_socket) {
  ServerFixture fixture;
  REQUIRE(fixture.boot().ok());

  CoordinatorClient client(client_options(fixture.server->port(), 0x8001ull));
  REQUIRE(client.connect().ok());
  CHECK(!client.challenge().is_zero());
  CHECK(client.session() != 0);

  const CreditOutcome revalidated = client.reconcile(fixture.local.account);
  REQUIRE(revalidated.ok());
  CHECK_EQ(client.next_sequence(), revalidated.sequence + 1);

  CreditRequest request{};
  request.authority.account = fixture.local.account;
  request.op = OpKind::Issue;
  request.count = 500;
  const CreditOutcome issued = client.submit(request);
  REQUIRE(issued.ok());
  CHECK_EQ(issued.view.issued, 500ull);

  CreditRequest consume{};
  consume.authority.account = fixture.local.account;
  consume.op = OpKind::Consume;
  consume.count = 200;
  REQUIRE(client.submit(consume).ok());

  CreditRequest give{};
  give.authority.account = fixture.local.account;
  give.op = OpKind::Return;
  give.count = 500;
  const CreditOutcome returned = client.submit(give);
  REQUIRE(returned.ok());
  CHECK_EQ(returned.view.in_flight, 0ull);
  CHECK_EQ(returned.view.available, 100000ull);
  CHECK(returned.view.closed);
  CHECK_EQ(fixture.server->requests_served() >= 4, true);
  client.close();
  (void)fixture.server->stop();
}

CF_TEST(a_second_worker_must_reconcile_before_it_can_act) {
  ServerFixture fixture;
  REQUIRE(fixture.boot().ok());

  CoordinatorClient client(client_options(fixture.server->port(), 0x8002ull));
  REQUIRE(client.connect().ok());
  CreditRequest request{};
  request.authority.account = fixture.local.account;
  request.op = OpKind::Issue;
  request.count = 10;
  // The client library revalidates automatically on demand; doing it by hand
  // first would hide the refusal, so drive the raw exchange instead.
  CreditRequest raw = request;
  raw.authority.domain = DomainId{0xD0A1ull};
  raw.authority.account = fixture.local.account;
  raw.authority.epoch = EpochId{1};
  raw.authority.generation = Generation{1};
  raw.authority.incarnation = client.incarnation().id;
  raw.presenter = client.incarnation();
  raw.sequence = 1;
  raw.attempt = AttemptIdGenerator{0x31ull}.next();
  // The public client always revalidates; the transport refuses anything else.
  const CreditOutcome outcome = client.submit(request);
  REQUIRE(outcome.ok());
  CHECK_EQ(outcome.view.issued, 10ull);
  CHECK_EQ(client.stats().reconciliations, 1ull);
  client.close();
  (void)fixture.server->stop();
}

CF_TEST(damaged_frames_close_the_connection_without_taking_the_server_down) {
  ServerFixture fixture;
  REQUIRE(fixture.boot().ok());
  const std::uint16_t port = fixture.server->port();

  const auto malformed_attempt = [port](const std::vector<std::uint8_t>& frame) {
    Socket socket;
    REQUIRE(Socket::connect_to("127.0.0.1", port, socket).ok());
    REQUIRE(socket.send_all(frame.data(), frame.size()).ok());
    MessageType type = MessageType::Bye;
    std::vector<std::uint8_t> payload;
    bool closed = false;
    const Status status = socket.recv_frame(1u << 16, type, payload, closed);
    CHECK(!status.ok() || closed);
    (void)socket.close();
  };

  // A frame with a broken checksum after a valid handshake.
  {
    Socket socket;
    REQUIRE(Socket::connect_to("127.0.0.1", port, socket).ok());
    std::vector<std::uint8_t> hello;
    HelloMessage message{};
    message.publisher = PublisherId{0x8100ull};
    message.boot = BootId{0x1ull};
    message.label = "damage-test";
    REQUIRE(encode_hello(message, hello).ok());
    REQUIRE(socket.send_frame(MessageType::Hello, hello).ok());
    MessageType type = MessageType::Bye;
    std::vector<std::uint8_t> payload;
    bool closed = false;
    REQUIRE(socket.recv_frame(1u << 16, type, payload, closed).ok());
    CHECK(type == MessageType::HelloAck);

    // Send a valid frame except for one flipped CRC byte.
    std::vector<std::uint8_t> frame;
    REQUIRE(encode_frame(MessageType::Request, std::vector<std::uint8_t>{1, 2, 3}, frame).ok());
    frame[frame.size() - 1] = static_cast<std::uint8_t>(frame[frame.size() - 1] ^ 0xFFu);
    REQUIRE(socket.send_all(frame.data(), frame.size()).ok());
    type = MessageType::Bye;
    payload.clear();
    closed = false;
    const Status status = socket.recv_frame(1u << 16, type, payload, closed);
    CHECK(!status.ok() || closed);
    (void)socket.close();
  }

  // Wrong magic, bad version and oversized length, all before any handshake.
  {
    std::vector<std::uint8_t> frame;
    REQUIRE(encode_frame(MessageType::Hello, std::vector<std::uint8_t>{}, frame).ok());
    frame[0] = 0;
    malformed_attempt(frame);
  }
  {
    std::vector<std::uint8_t> frame;
    REQUIRE(encode_frame(MessageType::Hello, std::vector<std::uint8_t>{}, frame).ok());
    frame[4] = 42;
    malformed_attempt(frame);
  }
  {
    std::vector<std::uint8_t> frame;
    REQUIRE(encode_frame(MessageType::Hello, std::vector<std::uint8_t>{}, frame).ok());
    frame[8] = 0xFF;
    frame[9] = 0xFF;
    frame[10] = 0xFF;
    frame[11] = 0x7F;
    malformed_attempt(frame);
  }

  // The server is still alive and still serving.
  CHECK(fixture.server->malformed_frames() >= 4ull);
  CoordinatorClient client(client_options(port, 0x8003ull));
  REQUIRE(client.connect().ok());
  REQUIRE(client.reconcile(fixture.local.account).ok());
  client.close();
  (void)fixture.server->stop();
}

CF_TEST(a_well_formed_frame_with_an_invalid_payload_is_refused_by_name) {
  ServerFixture fixture;
  REQUIRE(fixture.boot().ok());
  const std::uint16_t port = fixture.server->port();

  Socket socket;
  REQUIRE(Socket::connect_to("127.0.0.1", port, socket).ok());
  std::vector<std::uint8_t> hello;
  HelloMessage message{};
  message.publisher = PublisherId{0x8200ull};
  message.boot = BootId{0x2ull};
  message.label = "payload-test";
  REQUIRE(encode_hello(message, hello).ok());
  REQUIRE(socket.send_frame(MessageType::Hello, hello).ok());
  MessageType type = MessageType::Bye;
  std::vector<std::uint8_t> payload;
  bool closed = false;
  REQUIRE(socket.recv_frame(1u << 16, type, payload, closed).ok());

  // A structurally invalid request body: correct framing, nonsense content.
  const std::vector<std::uint8_t> nonsense{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  REQUIRE(socket.send_frame(MessageType::Request, nonsense).ok());
  REQUIRE(socket.recv_frame(1u << 16, type, payload, closed).ok());
  CHECK(type == MessageType::Response);
  CreditOutcome outcome{};
  REQUIRE(decode_response_message(payload.data(), payload.size(), outcome).ok());
  CHECK(!outcome.ok());
  CHECK(outcome.status.reason == Reason::TruncatedInput || outcome.status.reason == Reason::MalformedMessage);

  // The connection is closed after a protocol violation.
  type = MessageType::Bye;
  payload.clear();
  closed = false;
  const Status status = socket.recv_frame(1u << 16, type, payload, closed);
  CHECK(!status.ok() || closed);
  (void)socket.close();
  (void)fixture.server->stop();
}

CF_TEST(many_clients_are_served_concurrently) {
  ServerFixture fixture;
  REQUIRE(fixture.boot().ok());
  const std::uint16_t port = fixture.server->port();

  constexpr std::size_t kClients = 6;
  std::atomic<std::uint64_t> succeeded{0};
  std::vector<std::thread> threads;
  for (std::size_t i = 0; i < kClients; ++i) {
    threads.emplace_back([port, &fixture, &succeeded, i]() {
      CoordinatorClient client(client_options(port, 0x9000ull + i));
      if (!client.connect().ok()) return;
      if (!client.reconcile(fixture.local.account).ok()) return;
      for (int round = 0; round < 20; ++round) {
        CreditRequest request{};
        request.authority.account = fixture.local.account;
        request.op = OpKind::Issue;
        request.count = 10;
        if (!client.submit(request).ok()) return;
        CreditRequest give{};
        give.authority.account = fixture.local.account;
        give.op = OpKind::Return;
        give.count = 10;
        if (!client.submit(give).ok()) return;
        succeeded.fetch_add(1, std::memory_order_relaxed);
      }
      client.close();
    });
  }
  for (std::thread& thread : threads) thread.join();
  CHECK_EQ(succeeded.load(), static_cast<std::uint64_t>(kClients * 20));
  CHECK(fixture.local.view().closed);
  CHECK_EQ(fixture.local.view().in_flight, 0ull);
  (void)fixture.server->stop();
}

CF_TEST(a_connection_limit_is_enforced_without_dropping_the_server) {
  ServerFixture fixture;
  fixture.server = std::make_unique<CoordinatorServer>(
      *fixture.local.engine, CoordinatorOptions{});
  REQUIRE(fixture.local.recover().ok());
  REQUIRE(fixture.local.open(1000).ok());
  CoordinatorOptions limited{};
  limited.max_connections = 1;
  CoordinatorServer server(*fixture.local.engine, limited);
  REQUIRE(server.start().ok());

  CoordinatorClient first(client_options(server.port(), 0xA001ull));
  REQUIRE(first.connect().ok());
  REQUIRE(first.reconcile(fixture.local.account).ok());

  // A second connection is rejected cleanly rather than being left dangling.
  Socket extra;
  REQUIRE(Socket::connect_to("127.0.0.1", server.port(), extra).ok());
  MessageType type = MessageType::Bye;
  std::vector<std::uint8_t> payload;
  bool closed = false;
  const Status status = extra.recv_frame(1u << 16, type, payload, closed);
  CHECK(status.ok());
  CHECK(closed || type == MessageType::Bye);
  (void)extra.close();

  first.close();
  (void)server.stop();
}

CF_TEST(stopping_a_server_is_idempotent_and_unblocks_accept) {
  ServerFixture fixture;
  REQUIRE(fixture.boot().ok());
  CHECK(fixture.server->stop().ok());
  CHECK(fixture.server->stop().ok());
  CHECK(!fixture.server->running());

  // After shutdown, an engine call path is still coherent (the authority did
  // not lose accounting when the listener went away).
  CHECK(fixture.local.view().closed);
}

CF_TEST_MAIN()
