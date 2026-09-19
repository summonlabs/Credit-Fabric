// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "creditfabric/strong.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <random>

#if defined(_WIN32)
#include <process.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace creditfabric {
namespace {

constexpr char kHexDigits[] = "0123456789abcdef";

int hex_value(char c) noexcept {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

std::uint64_t entropy_from_system() noexcept {
  std::random_device device;
  std::uint64_t value = (static_cast<std::uint64_t>(device()) << 32) ^ static_cast<std::uint64_t>(device());
  const auto now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
  value ^= static_cast<std::uint64_t>(now);
  value ^= static_cast<std::uint64_t>(current_process_id()) * 0x9E3779B97F4A7C15ull;
  return avalanch64(value);
}

}  // namespace

std::uint32_t current_process_id() noexcept {
#if defined(_WIN32)
  return static_cast<std::uint32_t>(::GetCurrentProcessId());
#else
  return static_cast<std::uint32_t>(::getpid());
#endif
}

std::uint64_t process_entropy_seed() noexcept { return entropy_from_system(); }

std::uint64_t random_u64() noexcept {
  static std::atomic<std::uint64_t> state{0};
  std::uint64_t current = state.load(std::memory_order_relaxed);
  if (current == 0) {
    current = entropy_from_system();
    state.store(current, std::memory_order_relaxed);
  }
  for (;;) {
    const std::uint64_t next = mix64(current);
    if (state.compare_exchange_weak(current, next, std::memory_order_relaxed)) {
      return avalanch64(next ^ current);
    }
  }
}

std::string Digest128::to_hex() const {
  std::string out;
  out.resize(32);
  for (int i = 0; i < 16; ++i) {
    const std::uint64_t word = (i < 8) ? lo : hi;
    const int shift = (i % 8) * 8;
    const auto byte = static_cast<std::uint8_t>((word >> shift) & 0xFFu);
    out[static_cast<std::size_t>(i) * 2] = kHexDigits[byte >> 4];
    out[static_cast<std::size_t>(i) * 2 + 1] = kHexDigits[byte & 0x0Fu];
  }
  return out;
}

bool Digest128::parse(std::string_view text, Digest128& out) noexcept {
  if (text.size() != 32) return false;
  Digest128 value{};
  for (int i = 0; i < 16; ++i) {
    const int high = hex_value(text[static_cast<std::size_t>(i) * 2]);
    const int low = hex_value(text[static_cast<std::size_t>(i) * 2 + 1]);
    if (high < 0 || low < 0) return false;
    const auto byte = static_cast<std::uint8_t>((high << 4) | low);
    if (i < 8) {
      value.lo |= static_cast<std::uint64_t>(byte) << ((i % 8) * 8);
    } else {
      value.hi |= static_cast<std::uint64_t>(byte) << ((i % 8) * 8);
    }
  }
  out = value;
  return true;
}

AttemptIdGenerator::AttemptIdGenerator() noexcept
    : stream_seed_(process_entropy_seed()), counter_(0) {}

AttemptIdGenerator::AttemptIdGenerator(std::uint64_t stream_seed) noexcept
    : stream_seed_(stream_seed == 0 ? process_entropy_seed() : stream_seed), counter_(0) {}

AttemptId AttemptIdGenerator::next() noexcept {
  ++counter_;
  const std::uint64_t a = mix64(stream_seed_ ^ (counter_ * 0x9E3779B97F4A7C15ull));
  const std::uint64_t b = avalanch64(a + counter_ + stream_seed_);
  AttemptId id{};
  id.value.lo = a;
  id.value.hi = b;
  if (id.value.is_zero()) id.value.lo = 1;  // zero means "unset"; never issue it
  return id;
}

std::string to_hex(std::uint64_t value) {
  std::string out;
  out.resize(16);
  for (int i = 0; i < 16; ++i) {
    const auto nibble = static_cast<std::uint8_t>((value >> ((15 - i) * 4)) & 0xFu);
    out[static_cast<std::size_t>(i)] = kHexDigits[nibble];
  }
  return out;
}

bool parse_u64(std::string_view text, std::uint64_t& out) noexcept {
  if (text.empty() || text.size() > 20) return false;
  std::uint64_t value = 0;
  for (char c : text) {
    if (c < '0' || c > '9') return false;
    const std::uint64_t digit = static_cast<std::uint64_t>(c - '0');
    if (value > (0xFFFFFFFFFFFFFFFFull - digit) / 10ull) return false;
    value = value * 10ull + digit;
  }
  out = value;
  return true;
}

}  // namespace creditfabric
