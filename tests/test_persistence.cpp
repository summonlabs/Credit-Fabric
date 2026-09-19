// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Durability: committed state is rebuilt from the journal, a torn or damaged
// tail never becomes authority, and durable bytes never restore liveness.

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>

#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

#include "support/fixture.hpp"
#include "support/scratch.hpp"
#include "support/test_framework.hpp"

#include "creditfabric/file_journal.hpp"
#include "creditfabric/platform.hpp"

using namespace creditfabric;

namespace {

std::string scratch_prefix(const char* name) { return cftest::scratch_path(name); }

void wipe(const std::string& prefix) { cftest::wipe_scratch(prefix); }

struct FileEngine {
  FileJournalOptions options{};
  std::unique_ptr<CreditEngine> engine{};
  Incarnation me{};

  FileEngine(const std::string& prefix, std::uint64_t boot) : options{}, me(Incarnation::make(PublisherId{7}, BootId{boot})) {
    options.prefix = prefix;
    engine = std::make_unique<CreditEngine>(EngineLimits{}, std::make_unique<FileJournal>(options), me);
  }

  [[nodiscard]] Status open() { return engine->recover(); }
};

CreditRequest make_open(const Incarnation& me, const ReconciliationChallenge& challenge, DomainId domain,
                        AccountId account, std::uint64_t capacity) {
  CreditRequest request{};
  request.op = OpKind::Open;
  request.config.domain = domain;
  request.config.account = account;
  request.config.resource = ResourceId{3};
  request.config.capacity = capacity;
  request.config.max_capacity = capacity;
  request.config.rules.max_issue_per_attempt = capacity;
  request.presenter = me;
  request.authority.domain = domain;
  request.authority.account = account;
  request.authority.epoch = EpochId{1};
  request.authority.generation = Generation{1};
  request.authority.incarnation = me.id;
  request.sequence = 1;
  request.proof = challenge.prove(me, request.authority);
  return request;
}

CreditRequest make_data(const CreditEngine& engine, const Incarnation& me, const Explanation& explanation,
                        OpKind op, std::uint64_t count, std::uint64_t sequence) {
  CreditRequest request{};
  request.op = op;
  request.count = count;
  request.presenter = me;
  request.authority.domain = explanation.authority.domain;
  request.authority.account = explanation.authority.account;
  request.authority.epoch = explanation.authority.epoch;
  request.authority.generation = explanation.authority.generation;
  request.authority.incarnation = me.id;
  request.sequence = sequence;
  (void)engine;
  return request;
}

}  // namespace

CF_TEST(a_durable_ledger_survives_a_process_restart) {
  const std::string prefix = scratch_prefix("restart");
  wipe(prefix);
  const DomainId domain{0xD1};
  const AccountId account{0xA1};

  {
    FileEngine first(prefix, 0x11);
    REQUIRE(first.open().ok());
    AttemptIdGenerator attempts{1};
    CreditRequest open = make_open(first.me, first.engine->challenge(), domain, account, 1000);
    open.attempt = attempts.next();
    REQUIRE(first.engine->apply(open).ok());

    const Explanation explanation = first.engine->explain(account);
    CreditRequest issue = make_data(*first.engine, first.me, explanation, OpKind::Issue, 400, 2);
    issue.attempt = attempts.next();
    REQUIRE(first.engine->apply(issue).ok());

    CreditRequest consume = make_data(*first.engine, first.me, explanation, OpKind::Consume, 100, 3);
    consume.attempt = attempts.next();
    REQUIRE(first.engine->apply(consume).ok());
  }

  // A brand new process, with a new authority boot, opens the same journal.
  {
    FileEngine second(prefix, 0x11);
    REQUIRE(second.open().ok());
    const Explanation explanation = second.engine->explain(account);
    REQUIRE(explanation.found);
    CHECK_EQ(explanation.view.capacity, 1000ull);
    CHECK_EQ(explanation.view.issued, 400ull);
    CHECK_EQ(explanation.view.consumed, 100ull);
    CHECK_EQ(explanation.view.available, 600ull);
    CHECK_EQ(explanation.view.in_flight, 400ull);
    CHECK(explanation.view.closed);

    // Liveness is NOT restored: the incarnation is UNKNOWN again.
    bool saw_unknown = false;
    for (const IncarnationSummary& summary : explanation.incarnations) {
      if (summary.id == second.me.id) {
        saw_unknown = true;
        CHECK_EQ(summary.state, AuthorityState::Unknown);
        CHECK_EQ(summary.last_sequence, 3ull);
      }
    }
    CHECK(saw_unknown);

    // ... and the durable incarnation cannot act until it revalidates.
    CreditRequest reuse = make_data(*second.engine, second.me, explanation, OpKind::Issue, 10, 4);
    reuse.attempt = AttemptIdGenerator{9}.next();
    const CreditOutcome refused = second.engine->apply(reuse);
    CHECK(!refused.ok());
    CHECK_EQ(refused.status.reason, Reason::ReconciliationRequired);

    // Revalidation against the new boot's challenge restores authority.
    CreditRequest revalidate{};
    revalidate.op = OpKind::Reconcile;
    revalidate.presenter = second.me;
    revalidate.authority.domain = explanation.authority.domain;
    revalidate.authority.account = explanation.authority.account;
    revalidate.authority.epoch = explanation.authority.epoch;
    revalidate.authority.generation = explanation.authority.generation;
    revalidate.authority.incarnation = second.me.id;
    revalidate.sequence = 4;
    revalidate.attempt = AttemptIdGenerator{10}.next();
    revalidate.proof = second.engine->challenge().prove(second.me, revalidate.authority);
    REQUIRE(second.engine->apply(revalidate).ok());

    CreditRequest again = make_data(*second.engine, second.me, explanation, OpKind::Issue, 10, 5);
    again.attempt = AttemptIdGenerator{11}.next();
    CHECK(second.engine->apply(again).ok());
    CHECK_EQ(second.engine->explain(account).view.issued, 410ull);
  }
  wipe(prefix);
}

CF_TEST(a_torn_tail_is_discarded_and_never_becomes_authority) {
  const std::string prefix = scratch_prefix("torn");
  wipe(prefix);
  const DomainId domain{0xD2};
  const AccountId account{0xA2};

  {
    FileEngine first(prefix, 0x33);
    REQUIRE(first.open().ok());
    AttemptIdGenerator attempts{2};
    CreditRequest open = make_open(first.me, first.engine->challenge(), domain, account, 500);
    open.attempt = attempts.next();
    REQUIRE(first.engine->apply(open).ok());
    for (std::uint64_t i = 0; i < 5; ++i) {
      const Explanation explanation = first.engine->explain(account);
      CreditRequest issue = make_data(*first.engine, first.me, explanation, OpKind::Issue, 10, 2 + i);
      issue.attempt = attempts.next();
      REQUIRE(first.engine->apply(issue).ok());
    }
  }

  std::uint64_t size = 0;
  REQUIRE(file_size(prefix + ".jrnl", size).ok());
  // Truncate the file mid-record: the last committed attempt disappears and the
  // ledger must rebuild from what is actually durable.
  {
    std::FILE* stream = std::fopen((prefix + ".jrnl").c_str(), "r+b");
    REQUIRE(stream != nullptr);
    const long truncated = static_cast<long>(size - 12);
    CHECK(std::fseek(stream, truncated, SEEK_SET) == 0);
#if defined(_WIN32)
    CHECK(::_chsize_s(::_fileno(stream), truncated) == 0);
#else
    CHECK(::ftruncate(::fileno(stream), truncated) == 0);
#endif
    std::fclose(stream);
  }

  {
    FileEngine second(prefix, 0x33);
    const Status recovered = second.open();
    // A torn tail is either repaired or reported; it is never silently applied.
    CHECK(recovered.ok() || recovered.reason == Reason::JournalTorn);
    if (recovered.ok()) {
      const Explanation explanation = second.engine->explain(account);
      REQUIRE(explanation.found);
      CHECK(explanation.view.closed);
      CHECK(explanation.view.issued >= 10ull);
      CHECK(explanation.view.issued <= 50ull);
      CHECK_EQ(explanation.view.issued % 10ull, 0ull);
    }
  }
  wipe(prefix);
}

CF_TEST(a_corrupt_record_stops_replay_instead_of_guessing) {
  const std::string prefix = scratch_prefix("corrupt");
  wipe(prefix);
  const DomainId domain{0xD3};
  const AccountId account{0xA3};

  {
    FileEngine first(prefix, 0x55);
    REQUIRE(first.open().ok());
    AttemptIdGenerator attempts{3};
    CreditRequest open = make_open(first.me, first.engine->challenge(), domain, account, 500);
    open.attempt = attempts.next();
    REQUIRE(first.engine->apply(open).ok());
    for (std::uint64_t i = 0; i < 4; ++i) {
      const Explanation explanation = first.engine->explain(account);
      CreditRequest issue = make_data(*first.engine, first.me, explanation, OpKind::Issue, 10, 2 + i);
      issue.attempt = attempts.next();
      REQUIRE(first.engine->apply(issue).ok());
    }
  }

  std::uint64_t size = 0;
  REQUIRE(file_size(prefix + ".jrnl", size).ok());
  {
    std::FILE* stream = std::fopen((prefix + ".jrnl").c_str(), "r+b");
    REQUIRE(stream != nullptr);
    // Flip a byte inside the final record's payload.
    const long offset = static_cast<long>(size - 20);
    CHECK(std::fseek(stream, offset, SEEK_SET) == 0);
    const int byte = std::fgetc(stream);
    REQUIRE(byte != EOF);
    CHECK(std::fseek(stream, offset, SEEK_SET) == 0);
    CHECK(std::fputc(byte ^ 0x5A, stream) != EOF);
    std::fflush(stream);
    std::fclose(stream);
  }

  {
    FileEngine second(prefix, 0x55);
    const Status recovered = second.open();
    CHECK(recovered.ok() || recovered.reason == Reason::JournalTorn);
    if (recovered.ok()) {
      const Explanation explanation = second.engine->explain(account);
      REQUIRE(explanation.found);
      CHECK(explanation.view.closed);
      CHECK(explanation.view.issued <= 50ull);
    } else {
      CHECK_EQ(recovered.reason, Reason::JournalTorn);
    }
  }
  wipe(prefix);
}

CF_TEST(a_snapshot_rotates_the_journal_without_losing_accounting) {
  const std::string prefix = scratch_prefix("snapshot");
  wipe(prefix);
  const DomainId domain{0xD4};
  const AccountId account{0xA4};

  {
    FileEngine first(prefix, 0x77);
    REQUIRE(first.open().ok());
    AttemptIdGenerator attempts{4};
    CreditRequest open = make_open(first.me, first.engine->challenge(), domain, account, 100000);
    open.attempt = attempts.next();
    REQUIRE(first.engine->apply(open).ok());
    for (std::uint64_t i = 0; i < 60; ++i) {
      const Explanation explanation = first.engine->explain(account);
      CreditRequest issue = make_data(*first.engine, first.me, explanation, OpKind::Issue, 100, 2 + i);
      issue.attempt = attempts.next();
      REQUIRE(first.engine->apply(issue).ok());
    }
    REQUIRE(first.engine->snapshot().ok());
    CHECK(file_exists(prefix + ".snap"));
    std::uint64_t journal_bytes = 0;
    REQUIRE(file_size(prefix + ".jrnl", journal_bytes).ok());
    CHECK(journal_bytes < 64ull);  // only the header survives the rotation
  }

  {
    FileEngine second(prefix, 0x77);
    REQUIRE(second.open().ok());
    const Explanation explanation = second.engine->explain(account);
    REQUIRE(explanation.found);
    CHECK_EQ(explanation.view.issued, 6000ull);
    CHECK_EQ(explanation.view.available, 94000ull);
    CHECK(explanation.view.closed);
  }
  wipe(prefix);
}

CF_TEST(recovery_is_refused_when_the_journal_header_is_not_ours) {
  const std::string prefix = scratch_prefix("foreign");
  wipe(prefix);
  {
    std::FILE* stream = std::fopen((prefix + ".jrnl").c_str(), "wb");
    REQUIRE(stream != nullptr);
    const char garbage[] = "this is not a credit fabric journal at all";
    CHECK(std::fwrite(garbage, 1, sizeof(garbage) - 1, stream) == sizeof(garbage) - 1);
    std::fclose(stream);
  }
  FileEngine engine(prefix, 0x99);
  const Status recovered = engine.open();
  CHECK(!recovered.ok());
  CHECK_EQ(recovered.reason, Reason::JournalCorrupt);
  wipe(prefix);
}

CF_TEST(a_journal_failure_poisons_the_engine_and_is_never_acknowledged) {
  cftest::Fixture fixture;
  REQUIRE(fixture.recover().ok());
  REQUIRE(fixture.open(1000).ok());
  // Force the in-memory journal to refuse every later append.
  fixture.journal.set_max_bytes(fixture.journal.bytes());
  const CreditOutcome outcome = fixture.call(OpKind::Issue, 10);
  CHECK(!outcome.ok());
  CHECK_EQ(outcome.status.reason, Reason::BudgetExceeded);
  CHECK(!outcome.applied);
  CHECK(!outcome.decided);
  CHECK_EQ(fixture.view().issued, 0ull);

  // The engine refuses everything afterwards rather than diverging from storage.
  const CreditOutcome later = fixture.call(OpKind::Issue, 10);
  CHECK(!later.ok());
  CHECK_EQ(later.status.reason, Reason::IoError);
}

CF_TEST_MAIN()
