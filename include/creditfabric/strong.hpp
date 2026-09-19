// Credit Fabric - strong identities and binding digests.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Every governed entity in Credit Fabric carries a distinct, non-interchangeable
// identity type. Raw integers never cross the public API boundary: an AccountId
// cannot be passed where a ResourceId is expected, and a generation cannot be
// silently substituted for an epoch.

#ifndef CREDITFABRIC_STRONG_HPP
#define CREDITFABRIC_STRONG_HPP

#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace creditfabric {

/// Tag types. A tag exists only to keep identity domains separate at compile time.
struct DomainTag;
struct AccountTag;
struct ResourceTag;
struct ProducerTag;
struct ConsumerTag;
struct GrantTag;
struct PolicyTag;
struct PublisherTag;
struct BootTag;
struct IncarnationTag;
struct EpochTag;
struct GenerationTag;
struct SequenceTag;

/// Strongly typed scalar identity.
///
/// The default-constructed value is the zero / unset identity. Zero is never a
/// valid issued identity in Credit Fabric; it is reserved for "UNKNOWN".
template <class Tag, class Rep = std::uint64_t>
class StrongId {
 public:
  using rep_type = Rep;
  using tag_type = Tag;

  constexpr StrongId() noexcept = default;
  constexpr explicit StrongId(Rep value) noexcept : value_(value) {}

  [[nodiscard]] constexpr Rep value() const noexcept { return value_; }
  [[nodiscard]] constexpr bool is_unset() const noexcept { return value_ == Rep{}; }
  [[nodiscard]] constexpr bool is_set() const noexcept { return value_ != Rep{}; }

  friend constexpr bool operator==(StrongId, StrongId) noexcept = default;
  friend constexpr auto operator<=>(StrongId, StrongId) noexcept = default;

 private:
  Rep value_{};
};

using DomainId = StrongId<DomainTag>;
using AccountId = StrongId<AccountTag>;
using ResourceId = StrongId<ResourceTag>;
using ProducerId = StrongId<ProducerTag>;
using ConsumerId = StrongId<ConsumerTag>;
using GrantId = StrongId<GrantTag>;
using PolicyId = StrongId<PolicyTag>;
using PublisherId = StrongId<PublisherTag>;
using BootId = StrongId<BootTag>;
using IncarnationId = StrongId<IncarnationTag>;
using EpochId = StrongId<EpochTag>;
using Generation = StrongId<GenerationTag>;
using Sequence = StrongId<SequenceTag>;

/// 128-bit binding digest.
///
/// This is a *binding and corruption-detection* digest, not a cryptographic
/// message authentication code. It is keyed with a per-process secret so that a
/// remote party cannot pre-compute reconciliation proofs, but Credit Fabric does
/// not claim adversarial cryptographic strength for it.
struct Digest128 {
  std::uint64_t lo{0};
  std::uint64_t hi{0};

  [[nodiscard]] constexpr bool is_zero() const noexcept { return lo == 0 && hi == 0; }
  [[nodiscard]] std::string to_hex() const;
  [[nodiscard]] static bool parse(std::string_view text, Digest128& out) noexcept;

  friend constexpr bool operator==(Digest128, Digest128) noexcept = default;
  friend constexpr auto operator<=>(Digest128, Digest128) noexcept = default;
};

[[nodiscard]] constexpr std::uint64_t mix64(std::uint64_t x) noexcept {
  x += 0x9E3779B97F4A7C15ull;
  x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
  x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
  return x ^ (x >> 31);
}

[[nodiscard]] constexpr std::uint64_t avalanch64(std::uint64_t x) noexcept {
  x ^= x >> 33;
  x *= 0xFF51AFD7ED558CCDull;
  x ^= x >> 33;
  x *= 0xC4CEB9FE1A85EC53ull;
  x ^= x >> 33;
  return x;
}

/// Incremental keyed 128-bit binding digest.
class DigestBuilder {
 public:
  constexpr DigestBuilder() noexcept = default;
  constexpr explicit DigestBuilder(Digest128 key) noexcept
      : lo_(key.lo ^ 0xCBF29CE484222325ull), hi_(key.hi ^ 0x9E3779B97F4A7C15ull) {}

  constexpr DigestBuilder& u64(std::uint64_t v) noexcept {
    lo_ = avalanch64(lo_ ^ mix64(v + 0x9E3779B97F4A7C15ull));
    hi_ = avalanch64(hi_ + mix64(v ^ 0xD6E8FEB86659FD93ull));
    ++length_;
    return *this;
  }

  constexpr DigestBuilder& u32(std::uint32_t v) noexcept {
    return u64(static_cast<std::uint64_t>(v) | 0x100000000ull);
  }

  constexpr DigestBuilder& boolean(bool v) noexcept { return u64(v ? 0x9E3779B97F4A7C15ull : 0x1ull); }

  constexpr DigestBuilder& digest(const Digest128& value) noexcept { return u64(value.lo).u64(value.hi); }

  constexpr DigestBuilder& bytes(const void* data, std::size_t count) noexcept {
    const auto* p = static_cast<const unsigned char*>(data);
    for (std::size_t i = 0; i < count; ++i) {
      lo_ = avalanch64(lo_ ^ (static_cast<std::uint64_t>(p[i]) << ((i % 8u) * 8u)));
      hi_ = avalanch64(hi_ + static_cast<std::uint64_t>(p[i]) + mix64(i + 1u));
    }
    length_ += count;
    return *this;
  }

  constexpr DigestBuilder& text(std::string_view sv) noexcept { return bytes(sv.data(), sv.size()); }

  [[nodiscard]] constexpr Digest128 finish() const noexcept {
    return Digest128{avalanch64(lo_ ^ mix64(length_ + 0x2545F4914F6CDD1Dull)),
                     avalanch64(hi_ ^ mix64(length_ + 0x14057B7EF767814Full))};
  }

 private:
  std::uint64_t lo_{0xCBF29CE484222325ull};
  std::uint64_t hi_{0x9E3779B97F4A7C15ull};
  std::uint64_t length_{0};
};

/// Strongly typed 128-bit attempt identity.
struct AttemptId {
  Digest128 value{};

  [[nodiscard]] constexpr bool is_unset() const noexcept { return value.is_zero(); }
  [[nodiscard]] std::string to_hex() const { return value.to_hex(); }

  friend constexpr bool operator==(AttemptId, AttemptId) noexcept = default;
  friend constexpr auto operator<=>(AttemptId, AttemptId) noexcept = default;
};

/// Process-unique incarnation identity: a publisher identity bound to a boot.
[[nodiscard]] constexpr IncarnationId make_incarnation_id(PublisherId publisher, BootId boot) noexcept {
  const Digest128 d = DigestBuilder{Digest128{0x1234567890ABCDEFull, 0xFEDCBA0987654321ull}}
                          .u64(publisher.value())
                          .u64(boot.value())
                          .finish();
  return IncarnationId{d.lo ^ d.hi};
}

/// Monotonic attempt-id source. Seeded from OS entropy plus process identity so
/// that two processes started in the same millisecond cannot collide.
class AttemptIdGenerator {
 public:
  AttemptIdGenerator() noexcept;
  explicit AttemptIdGenerator(std::uint64_t stream_seed) noexcept;

  [[nodiscard]] AttemptId next() noexcept;
  [[nodiscard]] std::uint64_t stream_seed() const noexcept { return stream_seed_; }

 private:
  std::uint64_t stream_seed_{0};
  std::uint64_t counter_{0};
};

[[nodiscard]] std::uint64_t process_entropy_seed() noexcept;
[[nodiscard]] std::uint64_t random_u64() noexcept;
[[nodiscard]] std::uint32_t current_process_id() noexcept;

[[nodiscard]] std::string to_hex(std::uint64_t value);
[[nodiscard]] bool parse_u64(std::string_view text, std::uint64_t& out) noexcept;

}  // namespace creditfabric

namespace std {

template <class Tag, class Rep>
struct hash<creditfabric::StrongId<Tag, Rep>> {
  [[nodiscard]] size_t operator()(const creditfabric::StrongId<Tag, Rep>& id) const noexcept {
    return static_cast<size_t>(creditfabric::mix64(static_cast<std::uint64_t>(id.value()) ^
                                                   static_cast<std::uint64_t>(sizeof(Tag)) * 0x9E3779B97F4A7C15ull));
  }
};

template <>
struct hash<creditfabric::Digest128> {
  [[nodiscard]] size_t operator()(const creditfabric::Digest128& d) const noexcept {
    return static_cast<size_t>(creditfabric::mix64(d.lo ^ creditfabric::mix64(d.hi)));
  }
};

template <>
struct hash<creditfabric::AttemptId> {
  [[nodiscard]] size_t operator()(const creditfabric::AttemptId& a) const noexcept {
    return static_cast<size_t>(creditfabric::mix64(a.value.lo ^ creditfabric::mix64(a.value.hi)));
  }
};

}  // namespace std

#endif  // CREDITFABRIC_STRONG_HPP
