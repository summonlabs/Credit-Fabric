// Credit Fabric - bounded, little-endian byte codec.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Every decode path is length-checked and every encode path is length-bounded.
// A decoder that runs out of bytes fails; it never reads past the buffer and
// never allocates based on an unvalidated length.

#ifndef CREDITFABRIC_BYTES_HPP
#define CREDITFABRIC_BYTES_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "creditfabric/strong.hpp"

namespace creditfabric {

class ByteWriter {
 public:
  explicit ByteWriter(std::vector<std::uint8_t>& out) noexcept : out_(&out) {}

  void u8(std::uint8_t value) { out_->push_back(value); }
  void boolean(bool value) { out_->push_back(value ? 1u : 0u); }

  void u16(std::uint16_t value) {
    for (int i = 0; i < 2; ++i) out_->push_back(static_cast<std::uint8_t>((value >> (8 * i)) & 0xFFu));
  }

  void u32(std::uint32_t value) {
    for (int i = 0; i < 4; ++i) out_->push_back(static_cast<std::uint8_t>((value >> (8 * i)) & 0xFFu));
  }

  void u64(std::uint64_t value) {
    for (int i = 0; i < 8; ++i) out_->push_back(static_cast<std::uint8_t>((value >> (8 * i)) & 0xFFu));
  }

  void digest(const Digest128& value) {
    u64(value.lo);
    u64(value.hi);
  }

  void raw(const void* data, std::size_t count) {
    const auto* p = static_cast<const std::uint8_t*>(data);
    out_->insert(out_->end(), p, p + count);
  }

  /// Writes a length-prefixed string, refusing anything longer than max_bytes.
  bool text(std::string_view value, std::size_t max_bytes) {
    if (value.size() > max_bytes) return false;
    u32(static_cast<std::uint32_t>(value.size()));
    raw(value.data(), value.size());
    return true;
  }

  [[nodiscard]] std::size_t size() const noexcept { return out_->size(); }

 private:
  std::vector<std::uint8_t>* out_;
};

class ByteReader {
 public:
  ByteReader(const std::uint8_t* data, std::size_t size) noexcept : data_(data), size_(size) {}

  [[nodiscard]] std::size_t position() const noexcept { return position_; }
  [[nodiscard]] std::size_t remaining() const noexcept { return size_ - position_; }
  [[nodiscard]] bool done() const noexcept { return position_ == size_; }
  [[nodiscard]] bool failed() const noexcept { return failed_; }

  bool u8(std::uint8_t& out) {
    if (!ensure(1)) return false;
    out = data_[position_++];
    return true;
  }

  bool boolean(bool& out) {
    std::uint8_t raw_value = 0;
    if (!u8(raw_value)) return false;
    if (raw_value > 1u) {
      failed_ = true;
      return false;
    }
    out = raw_value == 1u;
    return true;
  }

  bool u16(std::uint16_t& out) {
    if (!ensure(2)) return false;
    out = static_cast<std::uint16_t>(data_[position_]) |
          static_cast<std::uint16_t>(static_cast<std::uint16_t>(data_[position_ + 1]) << 8);
    position_ += 2;
    return true;
  }

  bool u32(std::uint32_t& out) {
    if (!ensure(4)) return false;
    std::uint32_t value = 0;
    for (int i = 0; i < 4; ++i) {
      value |= static_cast<std::uint32_t>(data_[position_ + static_cast<std::size_t>(i)]) << (8 * i);
    }
    position_ += 4;
    out = value;
    return true;
  }

  bool u64(std::uint64_t& out) {
    if (!ensure(8)) return false;
    std::uint64_t value = 0;
    for (int i = 0; i < 8; ++i) {
      value |= static_cast<std::uint64_t>(data_[position_ + static_cast<std::size_t>(i)]) << (8 * i);
    }
    position_ += 8;
    out = value;
    return true;
  }

  bool digest(Digest128& out) { return u64(out.lo) && u64(out.hi); }

  bool raw(void* destination, std::size_t count) {
    if (!ensure(count)) return false;
    auto* target = static_cast<std::uint8_t*>(destination);
    for (std::size_t i = 0; i < count; ++i) target[i] = data_[position_ + i];
    position_ += count;
    return true;
  }

  bool skip(std::size_t count) {
    if (!ensure(count)) return false;
    position_ += count;
    return true;
  }

  /// Reads a length-prefixed string, refusing anything longer than max_bytes.
  bool text(std::string& out, std::size_t max_bytes) {
    std::uint32_t length = 0;
    if (!u32(length)) return false;
    if (length > max_bytes || length > remaining()) {
      failed_ = true;
      return false;
    }
    out.assign(reinterpret_cast<const char*>(data_ + position_), length);
    position_ += length;
    return true;
  }

 private:
  bool ensure(std::size_t count) {
    if (count > size_ - position_) {
      failed_ = true;
      return false;
    }
    return true;
  }

  const std::uint8_t* data_;
  std::size_t size_;
  std::size_t position_{0};
  bool failed_{false};
};

}  // namespace creditfabric

#endif  // CREDITFABRIC_BYTES_HPP
