// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "creditfabric/coordinator.hpp"

#include <algorithm>
#include <cstdio>
#include <chrono>
#include <thread>
#include <utility>
#include <vector>

#include "creditfabric/platform.hpp"

namespace creditfabric {
namespace {

CreditOutcome refusal_outcome(Reason reason, std::uint64_t detail) {
  CreditOutcome outcome{};
  outcome.status = Status::refused(reason, detail);
  return outcome;
}

}  // namespace

// ---------------------------------------------------------------------------
// CoordinatorServer
// ---------------------------------------------------------------------------

CoordinatorServer::CoordinatorServer(CreditEngine& engine, CoordinatorOptions options)
    : engine_(engine), options_(std::move(options)) {
  if (options_.max_connections == 0) options_.max_connections = 1;
  if (options_.limits.max_frame_payload_bytes == 0) options_.limits.max_frame_payload_bytes = 1024;
  if (options_.limits.max_note_bytes == 0) options_.limits.max_note_bytes = 1;
}

CoordinatorServer::~CoordinatorServer() { (void)stop(); }

Status CoordinatorServer::start() {
  if (running_.load(std::memory_order_acquire)) return Status::success();
  const Status bound = listener_.bind_loopback(options_.host, options_.port);
  if (!bound.ok()) return bound;
  port_ = listener_.port();
  stopping_.store(false, std::memory_order_release);
  running_.store(true, std::memory_order_release);
  accept_thread_ = std::thread(&CoordinatorServer::accept_loop, this);
  return Status::success();
}

Status CoordinatorServer::stop() {
  if (!running_.load(std::memory_order_acquire) && !accept_thread_.joinable()) {
    std::lock_guard<std::mutex> guard(registry_mutex_);
    workers_.clear();
    return Status::success();
  }
  stopping_.store(true, std::memory_order_release);
  listener_.stop_accepting();
  if (accept_thread_.joinable()) accept_thread_.join();
  (void)listener_.close();

  // Connection workers poll the same stop flag between socket waits, so a peer
  // that holds a connection open and idle cannot keep shutdown waiting.

  // Drain the worker list without holding the registry lock across a join.
  std::vector<std::thread> workers;
  {
    std::lock_guard<std::mutex> guard(registry_mutex_);
    workers.swap(workers_);
  }
  for (std::thread& worker : workers) {
    if (worker.joinable()) worker.join();
  }
  running_.store(false, std::memory_order_release);
  return Status::success();
}

std::size_t CoordinatorServer::active_connections() const {
  std::lock_guard<std::mutex> guard(registry_mutex_);
  return workers_.size();
}

void CoordinatorServer::register_connection(std::thread worker) {
  // The list stays bounded: a connection is only accepted while the list is
  // shorter than the configured limit, and every worker is joined by stop().
  // The registry lock is a leaf: it is never held across a socket operation, an
  // engine call, or a join.
  std::lock_guard<std::mutex> guard(registry_mutex_);
  workers_.push_back(std::move(worker));
}

void CoordinatorServer::accept_loop() {
  while (!stopping_.load(std::memory_order_acquire)) {
    Socket socket;
    bool closed = false;
    const Status status = listener_.accept(socket, closed);
    if (!status.ok() || closed) break;
    if (active_connections() >= options_.max_connections) {
      const std::vector<std::uint8_t> empty;
      (void)socket.send_frame(MessageType::Bye, empty);
      (void)socket.close();
      continue;
    }
    socket.set_nodelay(true);
    const std::uint32_t session = next_session_.fetch_add(1, std::memory_order_relaxed);
    accepted_.fetch_add(1, std::memory_order_relaxed);
    std::thread worker(&CoordinatorServer::serve_connection, this, std::move(socket), session);
    register_connection(std::move(worker));
  }
}

void CoordinatorServer::serve_connection(Socket socket, std::uint32_t session) {
  // Every socket operation on this connection polls the server's stop flag, so
  // shutdown never depends on the peer closing first.
  const std::atomic<bool>* stop = &stopping_;
  const std::size_t max_payload = options_.limits.max_frame_payload_bytes;
  MessageType type = MessageType::Bye;
  std::vector<std::uint8_t> payload;
  bool closed = false;

  Status status = socket.recv_frame(max_payload, type, payload, closed, stop);
  if (!status.ok() || closed || type != MessageType::Hello) {
    malformed_.fetch_add(1, std::memory_order_relaxed);
    (void)socket.close();
    return;
  }
  HelloMessage hello{};
  status = decode_hello(payload.data(), payload.size(), hello);
  if (!status.ok()) {
    malformed_.fetch_add(1, std::memory_order_relaxed);
    (void)socket.close();
    return;
  }

  HelloAckMessage ack{};
  ack.session = session;
  ack.challenge = engine_.challenge().nonce();
  ack.server_label = options_.server_label;
  ack.max_payload = static_cast<std::uint32_t>(max_payload);
  ack.format_version = kFormatVersion;
  std::vector<std::uint8_t> ack_payload;
  status = encode_hello_ack(ack, ack_payload);
  if (status.ok()) status = socket.send_frame(MessageType::HelloAck, ack_payload, stop);
  if (!status.ok()) {
    (void)socket.close();
    return;
  }

  std::uint64_t served_here = 0;
  while (!stopping_.load(std::memory_order_acquire)) {
    type = MessageType::Bye;
    payload.clear();
    closed = false;
    status = socket.recv_frame(max_payload, type, payload, closed, stop);
    if (closed) break;
    if (!status.ok()) {
      malformed_.fetch_add(1, std::memory_order_relaxed);
      break;  // a damaged frame desynchronizes the stream: close, do not guess
    }
    if (type == MessageType::Bye) break;

    if (type != MessageType::Request) {
      malformed_.fetch_add(1, std::memory_order_relaxed);
      std::vector<std::uint8_t> response;
      const Status encoded =
          encode_response_message(refusal_outcome(Reason::MalformedMessage, 20), response);
      if (encoded.ok()) (void)socket.send_frame(MessageType::Response, response, stop);
      break;
    }

    CreditRequest request{};
    status = decode_request_message(payload.data(), payload.size(), options_.limits.max_note_bytes, request);
    if (!status.ok()) {
      // A structurally invalid request is answered with its exact refusal and
      // then the connection is closed: the peer is not trusted to be in sync.
      malformed_.fetch_add(1, std::memory_order_relaxed);
      std::vector<std::uint8_t> response;
      const Status encoded = encode_response_message(refusal_outcome(status.reason, status.detail), response);
      if (encoded.ok()) (void)socket.send_frame(MessageType::Response, response, stop);
      break;
    }

    const CreditOutcome outcome = engine_.apply(request);
    served_.fetch_add(1, std::memory_order_relaxed);
    ++served_here;
    std::vector<std::uint8_t> response;
    const Status encoded = encode_response_message(outcome, response);
    if (!encoded.ok()) break;
    if (!socket.send_frame(MessageType::Response, response, stop).ok()) break;
    if (options_.max_requests_per_connection != 0 && served_here >= options_.max_requests_per_connection) break;
  }

  (void)socket.shutdown_send();
  (void)socket.close();
}

// ---------------------------------------------------------------------------
// CoordinatorClient
// ---------------------------------------------------------------------------

CoordinatorClient::CoordinatorClient(ClientOptions options)
    : options_(std::move(options)),
      incarnation_(Incarnation::make(options_.publisher, options_.boot)) {}

CoordinatorClient::~CoordinatorClient() { close(); }

Status CoordinatorClient::handshake() {
  HelloMessage hello{};
  hello.publisher = options_.publisher;
  hello.boot = options_.boot;
  hello.label = options_.label;
  if (hello.label.size() > 64) hello.label.resize(64);

  std::vector<std::uint8_t> payload;
  Status status = encode_hello(hello, payload);
  if (!status.ok()) return status;
  status = socket_.send_frame(MessageType::Hello, payload);
  if (!status.ok()) return status;

  MessageType type = MessageType::Bye;
  payload.clear();
  bool closed = false;
  status = socket_.recv_frame(options_.max_payload, type, payload, closed);
  if (!status.ok()) return status;
  if (closed || type != MessageType::HelloAck) return Status::refused(Reason::TransportError, 20);

  HelloAckMessage ack{};
  status = decode_hello_ack(payload.data(), payload.size(), ack);
  if (!status.ok()) return status;
  challenge_ = ack.challenge;
  session_ = ack.session;
  connected_ = true;
  placed_ = false;
  return Status::success();
}

Status CoordinatorClient::connect() {
  close();
  const Status ready = initialize_sockets();
  if (!ready.ok()) return ready;
  const Status opened = Socket::connect_to(options_.host, options_.port, socket_);
  if (!opened.ok()) return opened;
  socket_.set_nodelay(true);
  const Status status = handshake();
  if (!status.ok()) close();
  return status;
}

void CoordinatorClient::close() {
  if (socket_.valid()) {
    const std::vector<std::uint8_t> empty;
    (void)socket_.send_frame(MessageType::Bye, empty);
    (void)socket_.close();
  }
  connected_ = false;
}

CreditOutcome CoordinatorClient::exchange(const CreditRequest& request) {
  CreditOutcome outcome{};
  outcome.op = request.op;
  outcome.sequence = request.sequence;
  outcome.authority = request.authority;
  if (!connected_) {
    outcome.status = Status::refused(Reason::TransportError, 21);
    return outcome;
  }
  std::vector<std::uint8_t> payload;
  Status status = encode_request_message(request, payload);
  if (!status.ok()) {
    outcome.status = status;
    return outcome;
  }
  status = socket_.send_frame(MessageType::Request, payload);
  if (!status.ok()) {
    connected_ = false;
    outcome.status = status;
    return outcome;
  }
  ++stats_.requests_sent;

  MessageType type = MessageType::Bye;
  payload.clear();
  bool closed = false;
  status = socket_.recv_frame(options_.max_payload, type, payload, closed);
  if (!status.ok() || closed || type != MessageType::Response) {
    connected_ = false;
    outcome.status = status.ok() ? Status::refused(Reason::TransportError, 22) : status;
    return outcome;
  }
  status = decode_response_message(payload.data(), payload.size(), outcome);
  if (!status.ok()) {
    connected_ = false;
    outcome.status = status;
    return outcome;
  }
  if (!outcome.status.ok()) ++stats_.refusals;
  if (outcome.duplicate) ++stats_.duplicates;
  if (outcome.decided) next_sequence_ = request.sequence + 1;
  if (outcome.authority.account.is_set()) {
    authority_.domain = outcome.authority.domain;
    authority_.account = outcome.authority.account;
    if (outcome.authority.epoch.is_set()) authority_.epoch = outcome.authority.epoch;
    if (outcome.authority.generation.is_set()) authority_.generation = outcome.authority.generation;
    authority_.incarnation = incarnation_.id;
  }
  return outcome;
}

CreditOutcome CoordinatorClient::describe(AccountId account) {
  CreditRequest request{};
  request.op = OpKind::Describe;
  request.sequence = next_sequence_;
  request.attempt = attempts_.next();
  request.presenter = incarnation_;
  request.authority.domain = options_.domain;
  request.authority.account = account;
  request.authority.incarnation = incarnation_.id;
  return exchange(request);
}

CreditOutcome CoordinatorClient::reconcile(AccountId account) {
  const CreditOutcome description = describe(account);
  if (!description.ok()) return description;
  // The description carries this incarnation's durable sequence watermark.
  next_sequence_ = description.sequence + 1;

  AuthorityVector authority{};
  authority.domain = options_.domain;
  authority.account = account;
  authority.epoch = description.authority.epoch;
  authority.generation = description.authority.generation;
  authority.incarnation = incarnation_.id;

  CreditRequest request{};
  request.op = OpKind::Reconcile;
  request.sequence = next_sequence_;
  request.attempt = attempts_.next();
  request.presenter = incarnation_;
  request.authority = authority;
  request.proof = ReconciliationChallenge::compute(challenge_, incarnation_, authority);
  CreditOutcome outcome = exchange(request);
  if (outcome.ok()) {
    authority_ = authority;
    placed_ = true;
    ++stats_.reconciliations;
  }
  return outcome;
}

CreditOutcome CoordinatorClient::submit(CreditRequest request) {
  request.presenter = incarnation_;
  request.attempt = attempts_.next();

  if (request.op == OpKind::Open) {
    AuthorityVector authority{};
    authority.domain = request.config.domain;
    authority.account = request.config.account;
    authority.epoch = EpochId{1};
    authority.generation = Generation{1};
    authority.incarnation = incarnation_.id;
    request.authority = authority;
    request.sequence = 1;
    request.proof = ReconciliationChallenge::compute(challenge_, incarnation_, authority);
    return exchange(request);
  }

  if (request.op == OpKind::Describe) {
    request.sequence = next_sequence_;
    request.authority.domain = options_.domain;
    request.authority.incarnation = incarnation_.id;
    return exchange(request);
  }

  // One description serves both purposes: it reports the account's current
  // authority vector and this incarnation's durable sequence watermark, so a
  // freshly started client places its first attempt exactly at watermark + 1.
  if (!placed_ || !authority_.account.is_set() || !(authority_.account == request.authority.account)) {
    const CreditOutcome description = describe(request.authority.account);
    if (!description.ok()) return description;
    authority_ = description.authority;
    authority_.domain = options_.domain;
    authority_.incarnation = incarnation_.id;
    if (description.sequence + 1 > next_sequence_) next_sequence_ = description.sequence + 1;
    placed_ = true;
  }

  request.sequence = next_sequence_;
  request.authority = authority_;
  CreditOutcome outcome = exchange(request);

  if (outcome.status.reason == Reason::ReconciliationRequired ||
      outcome.status.reason == Reason::StaleAuthority ||
      outcome.status.reason == Reason::UnknownIncarnation) {
    const CreditOutcome revalidated = reconcile(request.authority.account);
    if (!revalidated.ok()) return revalidated;
    request.sequence = next_sequence_;
    request.authority = authority_;
    request.attempt = attempts_.next();
    ++stats_.retries;
    outcome = exchange(request);
  }
  return outcome;
}

Status wait_for_listener(const std::string& host, std::uint16_t port, unsigned max_attempts) {
  const Status ready = initialize_sockets();
  if (!ready.ok()) return ready;
  Status last = Status::refused(Reason::TransportError, 30);
  for (unsigned attempt = 0; attempt < max_attempts; ++attempt) {
    Socket socket;
    last = Socket::connect_to(host, port, socket);
    if (last.ok()) {
      (void)socket.close();
      return Status::success();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return last;
}

}  // namespace creditfabric
