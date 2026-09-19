// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "creditfabric/transport.hpp"

#include <atomic>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "creditfabric/crc32c.hpp"
#include "creditfabric/platform.hpp"
#include "creditfabric/version.hpp"

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace creditfabric {
namespace {

#if defined(_WIN32)
using native_socket = SOCKET;
constexpr native_socket kInvalidNative = INVALID_SOCKET;
#else
using native_socket = int;
constexpr native_socket kInvalidNative = -1;
#endif

native_socket to_native(std::uintptr_t handle) noexcept { return static_cast<native_socket>(handle); }
std::uintptr_t from_native(native_socket handle) noexcept { return static_cast<std::uintptr_t>(handle); }

bool would_block() noexcept {
#if defined(_WIN32)
  const int error = ::WSAGetLastError();
  return error == WSAEWOULDBLOCK || error == WSAEINTR;
#else
  return errno == EWOULDBLOCK || errno == EAGAIN || errno == EINTR;
#endif
}

void close_native(native_socket handle) noexcept {
#if defined(_WIN32)
  ::closesocket(handle);
#else
  ::close(handle);
#endif
}

/// Waits until the socket is ready in the requested direction. A bounded wait is
/// what makes shutdown total: between waits the caller's stop flag is consulted,
/// so no peer can hold a worker (or the process) hostage by staying quiet.
/// Returns 1 when ready, 0 when the wait expired, -1 when the socket failed.
int wait_ready(native_socket handle, bool for_write) {
  for (;;) {
    fd_set readable;
    fd_set writable;
    FD_ZERO(&readable);
    FD_ZERO(&writable);
    if (for_write) {
      FD_SET(handle, &writable);
    } else {
      FD_SET(handle, &readable);
    }
    timeval wait{};
    wait.tv_sec = 0;
    wait.tv_usec = static_cast<long>(Socket::kPollIntervalMillis) * 1000;
#if defined(_WIN32)
    const int ready = ::select(0, for_write ? nullptr : &readable, for_write ? &writable : nullptr, nullptr, &wait);
#else
    const int ready = ::select(static_cast<int>(handle) + 1, for_write ? nullptr : &readable,
                               for_write ? &writable : nullptr, nullptr, &wait);
#endif
    if (ready > 0) return 1;
    if (ready == 0) return 0;
    if (would_block()) continue;
    return -1;
  }
}

/// Reads exactly count bytes. Returns Ok and sets closed when the peer closed
/// cleanly before any byte of the read arrived. When the caller supplies a stop
/// flag, a set flag ends the read with ShutdownInProgress and no bytes are
/// reported as read.
Status read_exact(native_socket handle, void* destination, std::size_t count, bool& closed,
                  const std::atomic<bool>* stop) {
  auto* bytes = static_cast<unsigned char*>(destination);
  std::size_t offset = 0;
  while (offset < count) {
    if (stop != nullptr && stop->load(std::memory_order_acquire)) {
      return Status::refused(Reason::ShutdownInProgress, offset);
    }
    const int ready = wait_ready(handle, false);
    if (ready == 0) continue;
    if (ready < 0) return Status::refused(Reason::TransportError, 1);
#if defined(_WIN32)
    const int received = ::recv(handle, reinterpret_cast<char*>(bytes + offset), static_cast<int>(count - offset), 0);
#else
    const ssize_t received = ::recv(handle, bytes + offset, count - offset, 0);
#endif
    if (received == 0) {
      closed = offset == 0;
      return closed ? Status::success() : Status::refused(Reason::TruncatedInput, offset);
    }
    if (received < 0) {
      if (would_block()) continue;
      return Status::refused(Reason::TransportError, 1);
    }
    offset += static_cast<std::size_t>(received);
  }
  return Status::success();
}

Status write_all(native_socket handle, const void* source, std::size_t count, const std::atomic<bool>* stop) {
  const auto* bytes = static_cast<const unsigned char*>(source);
  std::size_t offset = 0;
  while (offset < count) {
    if (stop != nullptr && stop->load(std::memory_order_acquire)) {
      return Status::refused(Reason::ShutdownInProgress, offset);
    }
    const int ready = wait_ready(handle, true);
    if (ready == 0) continue;
    if (ready < 0) return Status::refused(Reason::TransportError, 2);
#if defined(_WIN32)
    const int sent = ::send(handle, reinterpret_cast<const char*>(bytes + offset), static_cast<int>(count - offset), 0);
#else
    const ssize_t sent = ::send(handle, bytes + offset, count - offset, 0);
#endif
    if (sent <= 0) {
      if (sent < 0 && would_block()) continue;
      return Status::refused(Reason::TransportError, 2);
    }
    offset += static_cast<std::size_t>(sent);
  }
  return Status::success();
}

}  // namespace

Socket::~Socket() { (void)close(); }

Socket::Socket(Socket&& other) noexcept : handle_(other.handle_.load(std::memory_order_acquire)) {
  other.handle_.store(kInvalid, std::memory_order_release);
}

Socket& Socket::operator=(Socket&& other) noexcept {
  if (this != &other) {
    (void)close();
    handle_.store(other.handle_.load(std::memory_order_acquire), std::memory_order_release);
    other.handle_.store(kInvalid, std::memory_order_release);
  }
  return *this;
}

Status Socket::close() {
  const std::uintptr_t handle = handle_.exchange(kInvalid, std::memory_order_acq_rel);
  if (handle == kInvalid) return Status::success();
  close_native(to_native(handle));
  return Status::success();
}

Status Socket::shutdown_send() {
  const std::uintptr_t handle = handle_.load(std::memory_order_acquire);
  if (handle == kInvalid) return Status::success();
#if defined(_WIN32)
  ::shutdown(to_native(handle), SD_SEND);
#else
  ::shutdown(to_native(handle), SHUT_WR);
#endif
  return Status::success();
}

void Socket::set_nodelay(bool enabled) {
  if (handle_ == kInvalid) return;
  const int value = enabled ? 1 : 0;
  (void)::setsockopt(to_native(handle_), IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&value),
                     static_cast<int>(sizeof(value)));
}

Status Socket::send_all(const void* data, std::size_t count, const std::atomic<bool>* stop) {
  if (handle_ == kInvalid) return Status::refused(Reason::InvalidState, 1);
  return write_all(to_native(handle_), data, count, stop);
}

Status Socket::recv_all(void* data, std::size_t count, bool& closed, const std::atomic<bool>* stop) {
  closed = false;
  if (handle_ == kInvalid) return Status::refused(Reason::InvalidState, 2);
  return read_exact(to_native(handle_), data, count, closed, stop);
}

Status Socket::send_frame(MessageType type, const std::vector<std::uint8_t>& payload,
                          const std::atomic<bool>* stop) {
  std::vector<std::uint8_t> frame;
  const Status encoded = encode_frame(type, payload, frame);
  if (!encoded.ok()) return encoded;
  return send_all(frame.data(), frame.size(), stop);
}

Status Socket::recv_frame(std::size_t max_payload, MessageType& type, std::vector<std::uint8_t>& payload,
                          bool& closed, const std::atomic<bool>* stop) {
  closed = false;
  payload.clear();
  if (handle_ == kInvalid) return Status::refused(Reason::InvalidState, 3);

  std::vector<std::uint8_t> frame(kFrameHeaderBytes);
  bool peer_closed = false;
  Status status = read_exact(to_native(handle_), frame.data(), kFrameHeaderBytes, peer_closed, stop);
  if (!status.ok()) return status;
  if (peer_closed) {
    closed = true;
    return Status::success();
  }

  const std::uint32_t magic = static_cast<std::uint32_t>(frame[0]) | (static_cast<std::uint32_t>(frame[1]) << 8) |
                              (static_cast<std::uint32_t>(frame[2]) << 16) |
                              (static_cast<std::uint32_t>(frame[3]) << 24);
  if (magic != kWireMagic) return Status::refused(Reason::MalformedMessage, 1);
  if (frame[4] != static_cast<std::uint8_t>(kFormatVersion)) {
    return Status::refused(Reason::UnsupportedVersion, frame[4]);
  }
  const std::uint16_t flags = static_cast<std::uint16_t>(frame[6]) | static_cast<std::uint16_t>(frame[7] << 8);
  if (flags != 0) return Status::refused(Reason::MalformedMessage, 2);
  const std::uint32_t length = static_cast<std::uint32_t>(frame[8]) |
                               (static_cast<std::uint32_t>(frame[9]) << 8) |
                               (static_cast<std::uint32_t>(frame[10]) << 16) |
                               (static_cast<std::uint32_t>(frame[11]) << 24);
  if (length > max_payload) return Status::refused(Reason::OversizedFrame, length);

  frame.resize(kFrameHeaderBytes + static_cast<std::size_t>(length) + kFrameTrailerBytes);
  status = read_exact(to_native(handle_), frame.data() + kFrameHeaderBytes,
                      static_cast<std::size_t>(length) + kFrameTrailerBytes, peer_closed, stop);
  if (!status.ok()) return status;
  if (peer_closed) {
    closed = true;
    return Status::success();
  }

  FrameView view{};
  status = decode_frame(frame.data(), frame.size(), max_payload, view);
  if (!status.ok()) return status;
  type = view.header.type;
  payload.assign(view.payload, view.payload + view.payload_length);
  return Status::success();
}

std::string Socket::peer_description() const { return handle_ == kInvalid ? "closed" : "loopback"; }

Status Socket::connect_to(const std::string& host, std::uint16_t port, Socket& out) {
  Status status = initialize_sockets();
  if (!status.ok()) return status;
  native_socket handle = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (handle == kInvalidNative) return Status::refused(Reason::TransportError, 20);

  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(port);
  if (::inet_pton(AF_INET, host.c_str(), &address.sin_addr) != 1) {
    close_native(handle);
    return Status::refused(Reason::TransportError, 21);
  }
  if (::connect(handle, reinterpret_cast<sockaddr*>(&address), static_cast<int>(sizeof(address))) != 0) {
    close_native(handle);
    return Status::refused(Reason::TransportError, 22);
  }
  out = Socket(from_native(handle));
  return Status::success();
}

Listener::~Listener() { (void)close(); }

Status Listener::bind_loopback(const std::string& host, std::uint16_t port) {
  Status status = initialize_sockets();
  if (!status.ok()) return status;
  if (handle_ != Socket::kInvalid) return Status::refused(Reason::InvalidState, 4);

  native_socket handle = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (handle == kInvalidNative) return Status::refused(Reason::TransportError, 10);

  int reuse = 1;
  (void)::setsockopt(handle, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse),
                     static_cast<int>(sizeof(reuse)));

  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(port);
  if (::inet_pton(AF_INET, host.c_str(), &address.sin_addr) != 1) {
    close_native(handle);
    return Status::refused(Reason::TransportError, 11);
  }
  if (::bind(handle, reinterpret_cast<sockaddr*>(&address), static_cast<int>(sizeof(address))) != 0) {
    close_native(handle);
    return Status::refused(Reason::TransportError, 12);
  }
  if (::listen(handle, 64) != 0) {
    close_native(handle);
    return Status::refused(Reason::TransportError, 13);
  }

  sockaddr_in bound{};
#if defined(_WIN32)
  int length = static_cast<int>(sizeof(bound));
#else
  socklen_t length = static_cast<socklen_t>(sizeof(bound));
#endif
  if (::getsockname(handle, reinterpret_cast<sockaddr*>(&bound), &length) != 0) {
    close_native(handle);
    return Status::refused(Reason::TransportError, 14);
  }

  handle_ = from_native(handle);
  port_ = ntohs(bound.sin_port);
  return Status::success();
}

Status Listener::accept(Socket& out, bool& closed) {
  closed = false;
  if (handle_ == Socket::kInvalid) return Status::refused(Reason::InvalidState, 5);
  const native_socket handle = to_native(handle_);

  for (;;) {
    if (stopping_.load(std::memory_order_acquire)) {
      closed = true;
      return Status::success();
    }
    fd_set readable;
    FD_ZERO(&readable);
    FD_SET(handle, &readable);
    timeval wait{};
    wait.tv_sec = 0;
    wait.tv_usec = static_cast<long>(Socket::kPollIntervalMillis) * 1000;
    const int ready = ::select(static_cast<int>(handle) + 1, &readable, nullptr, nullptr, &wait);
    if (ready < 0) {
      if (would_block()) continue;
      return Status::refused(Reason::TransportError, 15);
    }
    if (ready == 0) continue;

    sockaddr_in peer{};
#if defined(_WIN32)
    int length = static_cast<int>(sizeof(peer));
#else
    socklen_t length = static_cast<socklen_t>(sizeof(peer));
#endif
    const native_socket accepted = ::accept(handle, reinterpret_cast<sockaddr*>(&peer), &length);
    if (accepted == kInvalidNative) {
      if (would_block()) continue;
      return Status::refused(Reason::TransportError, 16);
    }
    out = Socket(from_native(accepted));
    return Status::success();
  }
}

void Listener::stop_accepting() noexcept { stopping_.store(true, std::memory_order_release); }

Status Listener::close() {
  if (handle_ == Socket::kInvalid) return Status::success();
  close_native(to_native(handle_));
  handle_ = Socket::kInvalid;
  return Status::success();
}

}  // namespace creditfabric
