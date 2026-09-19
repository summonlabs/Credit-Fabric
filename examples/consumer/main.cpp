// Independent downstream consumer of the installed CreditFabric package.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// It uses only the public headers and the exported CMake target: if this builds
// and runs against an installed prefix, the package is genuinely consumable.

#include <cstdio>
#include <memory>
#include <string>

#include <creditfabric/creditfabric.hpp>

namespace {

int fail(const char* message) {
  std::fprintf(stderr, "consumer: %s\n", message);
  return 1;
}

}  // namespace

int main() {
  std::printf("creditfabric version %s (%s)\n", creditfabric::version_string().c_str(),
              creditfabric::build_profile());

  const creditfabric::DomainId domain{0xC0DEull};
  const creditfabric::AccountId account{0xFEEDull};
  const creditfabric::Incarnation me =
      creditfabric::Incarnation::make(creditfabric::PublisherId{0x1000ull}, creditfabric::BootId{0x2000ull});

  creditfabric::EngineLimits limits{};
  auto engine = std::make_unique<creditfabric::CreditEngine>(
      limits, std::make_unique<creditfabric::InMemoryJournal>(), me);
  if (!engine->recover().ok()) return fail("recovery refused");

  creditfabric::AccountConfig config{};
  config.domain = domain;
  config.account = account;
  config.resource = creditfabric::ResourceId{1};
  config.policy = creditfabric::PolicyId{1};
  config.capacity = 1000;
  config.max_capacity = 1000;
  config.profile = creditfabric::ProfileKind::Synthetic;
  config.rules.max_issue_per_attempt = 1000;

  creditfabric::AuthorityVector authority{};
  authority.domain = domain;
  authority.account = account;
  authority.epoch = creditfabric::EpochId{1};
  authority.generation = creditfabric::Generation{1};
  authority.incarnation = me.id;

  creditfabric::AttemptIdGenerator attempts{0x1234ull};
  std::uint64_t sequence = 1;
  const auto submit = [&](creditfabric::OpKind op, std::uint64_t count) {
    creditfabric::CreditRequest request{};
    request.op = op;
    request.count = count;
    request.presenter = me;
    request.authority = authority;
    request.attempt = attempts.next();
    request.sequence = sequence;
    const creditfabric::CreditOutcome outcome = engine->apply(request);
    if (outcome.decided) sequence = request.sequence + 1;
    return outcome;
  };

  creditfabric::CreditRequest open{};
  open.op = creditfabric::OpKind::Open;
  open.config = config;
  open.presenter = me;
  open.authority = authority;
  open.attempt = attempts.next();
  open.sequence = sequence;
  open.proof = engine->challenge().prove(me, authority);
  const creditfabric::CreditOutcome opened = engine->apply(open);
  if (!opened.ok()) return fail("open refused");
  sequence = 2;

  if (!submit(creditfabric::OpKind::Issue, 400).ok()) return fail("issue refused");
  if (!submit(creditfabric::OpKind::Consume, 150).ok()) return fail("consume refused");
  if (!submit(creditfabric::OpKind::Return, 400).ok()) return fail("return refused");

  const creditfabric::Explanation explanation = engine->explain(account);
  std::printf("%s", creditfabric::render(explanation).c_str());
  if (!explanation.view.closed) return fail("accounting does not close");
  if (explanation.view.in_flight != 0) return fail("credit is still in flight after the round trip");
  if (explanation.view.available != explanation.view.capacity) return fail("capacity was not recycled");
  if (explanation.view.consumed != 150) return fail("consumed total is wrong");

  std::printf("consumer: OK\n");
  return 0;
}
