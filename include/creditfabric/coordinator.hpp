// Credit Fabric - coordinator service and its client.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Distributed claims in Credit Fabric are proved with real OS processes over
// real framed transport. The coordinator owns the only CreditEngine instance;
// workers (producers and consumers) hold no authority of their own and must
// reconcile against the coordinator's per-boot challenge before any of their
// attempts can be admitted.

#ifndef CREDITFABRIC_COORDINATOR_HPP
#define CREDITFABRIC_COORDINATOR_HPP

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "creditfabric/engine.hpp"
#include "creditfabric/transport.hpp"

namespace creditfabric {

struct CoordinatorOptions {
  std::string host{"127.0.0.1"};
  std::uint16_t port{0};
  EngineLimits limits{};
  std::string server_label{"creditfabric-coordinator"};
  std::uint64_t max_requests_per_connection{0};  ///< 0 = unlimited
  std::size_t max_connections{64};
};

/// Serves one CreditEngine over loopback framed transport.
///
/// Threading: one accept thread plus one thread per connection. The connection
/// registry mutex is only ever held for set insertion/removal, never across a
/// socket operation, never across an engine call, and never across join().
class CoordinatorServer {
 public:
  CoordinatorServer(CreditEngine& engine, CoordinatorOptions options);
  ~CoordinatorServer();

  CoordinatorServer(const CoordinatorServer&) = delete;
  CoordinatorServer& operator=(const CoordinatorServer&) = delete;

  [[nodiscard]] Status start();
  [[nodiscard]] Status stop();
  [[nodiscard]] bool running() const noexcept { return running_.load(std::memory_order_acquire); }
  [[nodiscard]] std::uint16_t port() const noexcept { return port_; }

  [[nodiscard]] std::uint64_t connections_accepted() const noexcept { return accepted_.load(std::memory_order_relaxed); }
  [[nodiscard]] std::uint64_t requests_served() const noexcept { return served_.load(std::memory_order_relaxed); }
  [[nodiscard]] std::uint64_t malformed_frames() const noexcept { return malformed_.load(std::memory_order_relaxed); }
  [[nodiscard]] std::size_t active_connections() const;

 private:
  void accept_loop();
  void serve_connection(Socket socket, std::uint32_t session);
  void register_connection(std::thread worker);

  CreditEngine& engine_;
  CoordinatorOptions options_;
  Listener listener_;
  mutable std::mutex registry_mutex_;  ///< guards workers_ only; leaf lock
  std::vector<std::thread> workers_;
  std::atomic<bool> running_{false};
  std::atomic<bool> stopping_{false};
  std::atomic<std::uint64_t> accepted_{0};
  std::atomic<std::uint64_t> served_{0};
  std::atomic<std::uint64_t> malformed_{0};
  std::atomic<std::uint32_t> next_session_{1};
  std::uint16_t port_{0};
  std::thread accept_thread_;
};

struct ClientOptions {
  std::string host{"127.0.0.1"};
  std::uint16_t port{0};
  DomainId domain{};
  PublisherId publisher{};
  BootId boot{};
  std::string label{"creditfabric-worker"};
  std::size_t max_payload{1u << 16};
};

struct ClientStats {
  std::uint64_t requests_sent{0};
  std::uint64_t reconciliations{0};
  std::uint64_t refusals{0};
  std::uint64_t duplicates{0};
  std::uint64_t retries{0};
  std::uint64_t reconnects{0};
};

/// Worker-side client. Owns no authority: it holds a challenge nonce, a
/// sequence counter and the last authority vector the coordinator confirmed.
class CoordinatorClient {
 public:
  explicit CoordinatorClient(ClientOptions options);
  ~CoordinatorClient();

  CoordinatorClient(const CoordinatorClient&) = delete;
  CoordinatorClient& operator=(const CoordinatorClient&) = delete;

  [[nodiscard]] Status connect();
  void close();

  /// Read-only projection for the given account. Never journaled.
  [[nodiscard]] CreditOutcome describe(AccountId account);

  /// Performs an explicit reconciliation attempt for the given account.
  [[nodiscard]] CreditOutcome reconcile(AccountId account);

  /// Sends an attempt, filling in attempt id, sequence, presenter and the
  /// authority vector. Reconciles once automatically when the coordinator
  /// demands revalidation.
  [[nodiscard]] CreditOutcome submit(CreditRequest request);

  [[nodiscard]] const Digest128& challenge() const noexcept { return challenge_; }
  [[nodiscard]] std::uint32_t session() const noexcept { return session_; }
  [[nodiscard]] std::uint64_t next_sequence() const noexcept { return next_sequence_; }
  [[nodiscard]] const AuthorityVector& authority() const noexcept { return authority_; }
  [[nodiscard]] const ClientStats& stats() const noexcept { return stats_; }
  [[nodiscard]] const Incarnation& incarnation() const noexcept { return incarnation_; }
  [[nodiscard]] bool connected() const noexcept { return socket_.valid() && connected_; }

 private:
  [[nodiscard]] CreditOutcome exchange(const CreditRequest& request);
  [[nodiscard]] Status handshake();

  ClientOptions options_;
  Socket socket_;
  Incarnation incarnation_{};
  Digest128 challenge_{};
  std::uint32_t session_{0};
  std::uint64_t next_sequence_{1};
  AuthorityVector authority_{};
  AttemptIdGenerator attempts_{};
  ClientStats stats_{};
  bool connected_{false};
  bool placed_{false};  ///< the sequence watermark for this session has been discovered
};

/// Convenience: wait for a listening coordinator without using a watchdog.
/// Returns Ok as soon as a connection succeeds, or the last transport error.
[[nodiscard]] Status wait_for_listener(const std::string& host, std::uint16_t port, unsigned max_attempts);

}  // namespace creditfabric

#endif  // CREDITFABRIC_COORDINATOR_HPP
