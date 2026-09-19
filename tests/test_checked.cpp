// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include <cstdint>
#include <limits>

#include "support/test_framework.hpp"

#include "creditfabric/checked.hpp"
#include "creditfabric/strong.hpp"

using namespace creditfabric;

namespace {

constexpr std::uint64_t kMax = (std::numeric_limits<std::uint64_t>::max)();

}  // namespace

CF_TEST(checked_add_detects_wraparound) {
  std::uint64_t out = 0;
  CHECK(!add_overflow(1ull, 2ull, out));
  CHECK_EQ(out, 3ull);

  CHECK(!add_overflow(kMax - 1ull, 1ull, out));
  CHECK_EQ(out, kMax);

  CHECK(add_overflow(kMax, 1ull, out));
  CHECK(add_overflow(kMax, kMax, out));
  CHECK(add_overflow(1ull << 63, 1ull << 63, out));
}

CF_TEST(checked_sub_detects_underflow) {
  std::uint64_t out = 0;
  CHECK(!sub_overflow(5ull, 3ull, out));
  CHECK_EQ(out, 2ull);
  CHECK(!sub_overflow(3ull, 3ull, out));
  CHECK_EQ(out, 0ull);
  CHECK(sub_overflow(3ull, 4ull, out));
  CHECK(sub_overflow(0ull, 1ull, out));
  CHECK(sub_overflow(0ull, kMax, out));
}

CF_TEST(checked_mul_detects_overflow) {
  std::uint64_t out = 0;
  CHECK(!mul_overflow(0ull, kMax, out));
  CHECK_EQ(out, 0ull);
  CHECK(!mul_overflow(kMax, 0ull, out));
  CHECK_EQ(out, 0ull);
  CHECK(!mul_overflow(1ull << 32, 1ull << 31, out));
  CHECK_EQ(out, 1ull << 63);
  CHECK(mul_overflow(1ull << 32, 1ull << 32, out));
  CHECK(mul_overflow(kMax, 2ull, out));
}

CF_TEST(checked_accumulator_latches_first_failure) {
  CheckedAccumulator accumulator{10};
  CHECK(accumulator.add(5));
  CHECK_EQ(accumulator.value(), 15ull);
  CHECK(!accumulator.sub(20));
  CHECK(accumulator.underflowed());
  CHECK(!accumulator.ok());
  // Once poisoned, every later operation fails and the value never moves.
  CHECK(!accumulator.add(1));
  CHECK_EQ(accumulator.value(), 15ull);

  CheckedAccumulator second{0};
  CHECK(!second.sub(1));
  CHECK(second.underflowed());
  CHECK(second.mul(2) == false);
  second.reset(7);
  CHECK(second.ok());
  CHECK_EQ(second.value(), 7ull);

  CheckedAccumulator third{kMax};
  CHECK(third.add(0));
  CHECK(!third.add(1));
  CHECK(third.overflowed());
  CHECK(!third.underflowed());
}

CF_TEST(saturating_add_never_wraps) {
  CHECK_EQ(saturating_add(1ull, 2ull), 3ull);
  CHECK_EQ(saturating_add(kMax, 1ull), kMax);
  CHECK_EQ(saturating_add(kMax, kMax), kMax);
}

CF_TEST(strong_ids_keep_identity_domains_separate) {
  const DomainId domain{1};
  const AccountId account{1};
  // Same numeric value, different types: the following would not compile.
  CHECK_EQ(domain.value(), account.value());
  CHECK(domain.is_set());
  CHECK(!DomainId{}.is_set());
  CHECK(DomainId{} == DomainId{});
  CHECK(DomainId{2} > DomainId{1});
}

CF_TEST(digest_builder_is_deterministic_and_order_sensitive) {
  const Digest128 first =
      DigestBuilder{}.u64(1).u64(2).text("abc").finish();
  const Digest128 second =
      DigestBuilder{}.u64(1).u64(2).text("abc").finish();
  const Digest128 reordered =
      DigestBuilder{}.u64(2).u64(1).text("abc").finish();
  CHECK(first == second);
  CHECK_NE(first, reordered);
  CHECK_EQ(first.to_hex().size(), 32ull);
  Digest128 parsed{};
  CHECK(Digest128::parse(first.to_hex(), parsed));
  CHECK(parsed == first);
  CHECK(!Digest128::parse("not-hex", parsed));
  CHECK(!Digest128::parse("", parsed));
}

CF_TEST(attempt_ids_are_unique_and_never_zero) {
  AttemptIdGenerator generator{0x1234ull};
  AttemptId previous = generator.next();
  CHECK(!previous.is_unset());
  for (int i = 0; i < 10000; ++i) {
    const AttemptId current = generator.next();
    CHECK(!current.is_unset());
    CHECK_NE(current, previous);
    previous = current;
  }
}

CF_TEST_MAIN()
