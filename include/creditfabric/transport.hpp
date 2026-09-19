// Credit Fabric - real framed byte-stream transport (loopback TCP).
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Credit Fabric tests its distributed claims over real OS processes and real
// framed transport. The transport is deliberately minimal: loopback TCP, one
// frame at a time, hard bounds on every read, and an explicit shutdown that
// unblocks a pending accept without leaving a socket half-closed.

#ifndef CREDITFABRIC_TRANSPORT_HPP
#define CREDITFABRIC_TRANSPORT_HPP

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include "creditfabric/status.hpp"
#include "creditfabric/wire.hpp"

namespace creditfabric {

class Socket {
 public:
  Socket() = default;
  ~Socket();

  Socket(const Socket&) = delete;
  Socket& operator=(const Socket&) = delete;
  Socket(Socket&& other) noexcept;
  Socket& operator=(Socket&& other) noexcept;

  [[nodiscard]] bool valid() const noexcept { return handle_ != kInvalid; }

  /// Writes every byte. When \p stop is supplied the call also makes progress
  /// checking it, so a peer that stops reading cannot block a shutdown forever.
  [[nodiscard]] Status send_all(const void* data, std::size_t count, const std::atomic<bool>* stop = nullptr);
  [[nodiscard]] Status recv_all(void* data, std::size_t count, bool& closed,
                                const std::atomic<bool>* stop = nullptr);

  [[nodiscard]] Status send_frame(MessageType type, const std::vector<std::uint8_t>& payload,
                                  const std::atomic<bool>* stop = nullptr);
  [[nodiscard]] Status recv_frame(std::size_t max_payload, MessageType& type, std::vector<std::uint8_t>& payload,
                                  bool& closed, const std::atomic<bool>* stop = nullptr);

  [[nodiscard]] Status close();

  /// Half-closes the send direction so a peer sees a clean end of stream.
  [[nodiscard]] Status shutdown_send();

  /// How long a single wait for readiness blocks before the caller's stop flag
  /// is consulted again. Shutdown latency is bounded by this interval.
  static constexpr std::uint64_t kPollIntervalMillis = 50;

  void set_nodelay(bool enabled);
  [[nodiscard]] std::string peer_description() const;

  /// Connects to a loopback endpoint. Blocking, with no internal watchdog.
  [[nodiscard]] static Status connect_to(const std::string& host, std::uint16_t port, Socket& out);

  static constexpr std::uintptr_t kInvalid = static_cast<std::uintptr_t>(~static_cast<std::uintptr_t>(0));

 private:
  explicit Socket(std::uintptr_t handle) noexcept : handle_(handle) {}
  friend class Listener;
  mutable std::atomic<std::uintptr_t> handle_{kInvalid};
};

class Listener {
 public:
  Listener() = default;
  ~Listener();

  Listener(const Listener&) = delete;
  Listener& operator=(const Listener&) = delete;

  /// Binds a loopback listener. Port 0 selects an ephemeral port.
  [[nodiscard]] Status bind_loopback(const std::string& host, std::uint16_t port);

  [[nodiscard]] Status accept(Socket& out, bool& closed);

  /// Rejects further connections and unblocks a pending accept within one poll
  /// interval. Safe to call from another thread and safe to call more than once.
  void stop_accepting() noexcept;

  [[nodiscard]] Status close();

  [[nodiscard]] std::uint16_t port() const noexcept { return port_; }
  [[nodiscard]] bool valid() const noexcept { return handle_ != Socket::kInvalid; }

 private:
  std::uintptr_t handle_{Socket::kInvalid};
  std::uint16_t port_{0};
  std::atomic<bool> stopping_{false};
};

}  // namespace creditfabric

#endif  // CREDITFABRIC_TRANSPORT_HPP
