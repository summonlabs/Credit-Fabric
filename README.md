# Credit Fabric

Open-source, vendor-neutral C++20 runtime for generation-bound, credit-based
flow-control authority: issuance, consumption, return, exhaustion, recovery and
stale-credit fencing, for relationships where credit semantics are explicitly
supported.

Credit Fabric answers one question, and answers it exactly:

> For each governed producer/consumer/resource relationship, how many credits are
> authoritative **now**, what has been issued, consumed, returned or exhausted,
> and when must credits be withheld, fenced, revalidated or retired as stale?

Version: **1.0.0**. Licence: Apache License 2.0. No telemetry is transmitted,
collected or implied by anything in this repository.

## Contents

- [What Credit Fabric is](#what-credit-fabric-is)
- [What Credit Fabric is not](#what-credit-fabric-is-not)
- [Provenance labels](#provenance-labels-real--synthetic--unsupported)
- [Building](#building)
- [Installing and consuming](#installing-and-consuming)
- [Command line tools](#command-line-tools)
- [The accounting model](#the-accounting-model)
- [The authority model](#the-authority-model)
- [Attempt identity and replay protection](#attempt-identity-and-replay-protection)
- [Durability](#durability)
- [Transport and multiprocess](#transport-and-multiprocess)
- [Concurrency and lock discipline](#concurrency-and-lock-discipline)
- [Structural bounds](#structural-bounds)
- [Tests](#tests)
- [Benchmark](#benchmark)
- [Verified closure](#verified-closure)
- [Remaining limitations](#remaining-limitations)
- [License](#license)

## What Credit Fabric is

A single authority that owns credit state for a governed relationship and that
makes every authoritative decision **deterministic and explainable**:

- **Strong identities.** Domains, accounts, resources, producers, consumers,
  grants, policies, epochs, generations, incarnations, attempts and provenance
  are distinct types. A generation cannot be passed where an epoch is expected.
- **Exact accounting.** Capacity partitions into `available`, `protected` and
  `in_flight`; in-flight credit partitions into `outstanding` (spendable),
  `spent` (spent, not yet returned) and `stale` (quarantined). Both
  partitions close exactly, and every mutation is checked arithmetic.
- **Authority vectors.** Every decision is bound to the exact domain, account,
  epoch, generation and incarnation that justified it. Advancing any component
  invalidates every decision taken under the previous value.
- **Explicit reconciliation.** A restarted authority never restores liveness from
  durable bytes. Incarnations are `UNKNOWN` after every boot until they prove
  participation in *this* boot's challenge.
- **Idempotent attempts.** An admitted attempt is decided exactly once, ever.
  Replays return the recorded outcome; contradictions are refused.
- **Permanent fences.** Fencing an incarnation invalidates its authority forever,
  across restarts and across epoch advances.
- **Crash-safe durability.** A CRC-protected, digest-chained journal with atomic
  snapshot rotation. Nothing is acknowledged before its commit record is
  durable; a journal failure poisons the engine rather than acknowledging.
- **Real multiprocess closure.** The distributed claims are proved with real
  executables, real loopback TCP framing and real `TerminateProcess`/`SIGKILL`.

Source layout: 17 public headers (`include/creditfabric`), 16 implementation
units (`src`), four command line tools (`apps`), 14 test binaries
(`tests`) and one independent downstream consumer (`examples/consumer`).

## What Credit Fabric is not

Credit Fabric does **not** own, implement or claim any of the following, and no
part of this repository models them:

- packet scheduling or queue discipline;
- generic rate limiting;
- buffer allocation or memory management;
- congestion synthesis or congestion-control algorithms;
- generic backpressure propagation outside a governed credit relationship;
- transport-protocol design (the loopback framing here exists only to carry
  credit requests between the authority and its callers);
- vendor-specific physical credit mechanisms, unless represented through an
  explicit backend profile — and this build supports **no validated physical
  profile at all**.

The only profile values that a caller may open an account with are `ABSTRACT`
(mechanism-neutral) and `SYNTHETIC` (generated workload). `PHYSICAL-UNVALIDATED`
is accepted as a label and carries no validation claim.
`PHYSICAL-VALIDATED` is **refused** (`ProfileNotSupported`) because this build
has no hardware evidence to justify it.

## Provenance labels (REAL / SYNTHETIC / UNSUPPORTED)

| Surface | Label | Evidence |
|---|---|---|
| Credit accounting, authority, epochs, generations, fences, reconciliation | **REAL** | Implemented in this repository, exercised by unit, property, adversarial, concurrency and persistence tests |
| Durable journal, snapshot rotation, crash recovery | **REAL** | Real files, real torn tails, real corruption, real process kills |
| Multiprocess coordinator/worker over framed loopback TCP | **REAL** | Real executables, real sockets, real hard kills (`tests/test_multiprocess.cpp`) |
| Accounting closure, no double spend, stale-credit refusal, fencing permanence | **REAL (proved)** | Model-checked property suite plus explicit invariant tests |
| Benchmark throughput numbers | **SYNTHETIC** | Generated ledger population, no hardware involved; every report is labelled `SYNTHETIC` |
| Physical network, NIC, switch, RDMA, NVLink, optical, DPU, fabric credit mechanisms | **UNSUPPORTED** | Not modelled, not tested, not claimed |
| AddressSanitizer / ThreadSanitizer coverage | **UNSUPPORTED in this environment** | MSVC `/fsanitize=address` compiles, but the Clang ASan runtime (`clang_rt.asan_dynamic_runtime_thunk-x86_64.lib`) is not installed with this Visual Studio configuration, so no ASan binary could be linked. No sanitizer run was performed and none is claimed |
| Cryptographic authentication of peers | **UNSUPPORTED** | The transport is unauthenticated loopback; the reconciliation proof is a freshness/round-trip proof, not a MAC |

## Building

Requirements: a C++20 compiler (MSVC 19.3x, GCC 11+, or Clang 14+), CMake 3.20+
and a build tool (Ninja or Make). No third-party dependencies.

```sh
cmake -S . -B build/release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/release
ctest --test-dir build/release
```

Options:

| Option | Default | Meaning |
|---|---|---|
| `CREDITFABRIC_BUILD_TESTS` | `ON` | Build the 14 test binaries and register them with CTest |
| `CREDITFABRIC_BUILD_APPS` | `ON` | Build `cfctl`, `cf_coordinator`, `cf_worker`, `cf_bench` |
| `CREDITFABRIC_WARNINGS_AS_ERRORS` | `ON` | `/WX` on MSVC, `-Werror` elsewhere |
| `CREDITFABRIC_ENABLE_ASAN` | `OFF` | `/fsanitize=address` or `-fsanitize=address` |

Release and Debug both build clean under MSVC `/W4 /WX` (with `/permissive-`,
`/utf-8`, `/Zc:__cplusplus`, `/Zc:preprocessor`). No CTest test sets a timeout:
a hanging test is a defect to diagnose, not something to hide behind a watchdog.

There are no test timeouts anywhere in this repository, by design.

## Installing and consuming

```sh
cmake --install build/release --prefix /your/prefix
```

The install exports `CreditFabric::creditfabric` plus
`CreditFabricConfig.cmake` / `CreditFabricConfigVersion.cmake`
(`SameMajorVersion` compatibility).

An independent consumer lives in `examples/consumer`. It is deliberately **not**
part of this build; it is configured on its own against the installed prefix:

```sh
cmake -S examples/consumer -B build/consumer -G Ninja \
      -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=/your/prefix
cmake --build build/consumer
build/consumer/creditfabric_consumer
```

On Windows, pass the prefix through the `CMAKE_PREFIX_PATH` environment variable
if the path contains spaces.

```cpp
#include <creditfabric/creditfabric.hpp>

creditfabric::CreditEngine engine(limits, std::make_unique<creditfabric::FileJournal>(options), my_incarnation);
engine.recover();
creditfabric::CreditOutcome outcome = engine.apply(request);
creditfabric::Explanation explanation = engine.explain(account);
```

## Command line tools

`cfctl` is the operator tool and has two modes:

```sh
# Offline: cfctl owns the durable journal directly.
cfctl --journal ./ledger open --resource 30 --capacity 1000
cfctl --journal ./ledger issue --count 400
cfctl --journal ./ledger explain

# Remote: cfctl speaks to the coordinator that owns the authority.
cfctl --journal ./ledger --port 7000 fence --target <incarnation-hex>
```

An offline `cfctl` must never run against a journal that a live coordinator
owns: exactly one engine may hold a journal at a time. The remote mode exists
precisely so that operator actions go through the owning authority.

Commands: `open`, `describe`, `explain`, `issue`, `consume`, `return`,
`protect`, `unprotect`, `reconcile`, `capacity`, `epoch`, `fence`,
`retire`, `revalidate`, `exhaust`, `clear-exhaustion`, `snapshot`,
`bench`.

`cf_coordinator` owns one engine and serves it over loopback framing:

```sh
cf_coordinator --journal ./ledger --port 7000 --open --capacity 1000000
# prints: OPENED account=... capacity=...
#         LISTENING 7000
```

`cf_worker` holds no authority of its own. It reconciles, then runs balanced
issue/consume/return rounds and refuses to continue if anything is refused:

```sh
cf_worker --port 7000 --publisher 5001 --boot 9001 --issue 64 --rounds 4
```

`cf_bench` runs the synthetic ledger benchmark (see below).

## The accounting model

A credit account owns a capacity $C$. Every unit of that capacity is, at any
instant, in exactly one bucket:

```
available   : in the pool, issue-able right now
protected   : reserved in the pool, withheld from issue
in_flight   : issued - returned
```

**Closure invariant:** `C == available + protected + in_flight`.

Credits inside `in_flight` carry a lifecycle tag:

```
outstanding : live, spendable permission
spent       : spent by a holder and not yet returned
stale       : quarantined after an authority advance; never spendable again
```

**Partition invariant:** `in_flight == outstanding + spent + stale`.

Transitions, all with checked arithmetic:

| Operation | Effect |
|---|---|
| `issue(k)` | `issued += k`; requires `k <= available` and the policy window |
| `consume(k)` | `spent += k`, `consumed += k`; requires `k <= outstanding` |
| `return(k)` | `returned += k`; requires `k <= in_flight - stale` (spent credit is handed back first) |
| `protect(k)` / `unprotect(k)` | moves credit between `available` and `protected` |
| `advance epoch` / `rotate` | quarantines **all** in-flight credit into `stale`; nothing is silently returned |
| `retire(k)` | quarantines `k` outstanding credits without advancing the era |
| `revalidate(k, ToReturned)` | `stale -= k`, `returned += k` — proved never spent, so it re-enters the pool |
| `revalidate(k, ToConsumed)` | `stale -= k`, `spent += k` — proved spent, so it keeps occupying capacity |
| `set capacity(N)` | requires `N >= protected + in_flight`, otherwise `CapacityShrinkViolation` with the exact floor |

Consequences worth stating plainly:

- **No credit is spent twice.** `consume` requires `k <= outstanding`; a spent
  credit is not outstanding again until it is returned to the pool, and only a
  fresh `issue` can make it spendable.
- **Return cannot exceed issued/in-flight credit.** `return` requires
  `k <= in_flight - stale`, and quarantined credit is not returnable at all.
- **The pool recycles.** Consumed credit occupies capacity until returned, which
  is what makes credit-based flow control sustainable rather than a budget that
  drains.

## The authority model

```
AuthorityVector = { domain, account, epoch, generation, incarnation }
```

A request is admitted only when its authority vector matches the account's
current vector *exactly*. Anything else is refused by name:

| Situation | Refusal |
|---|---|
| epoch or generation superseded | `StaleAuthority` (detail carries the current generation) |
| incarnation fenced | `FencedIncarnation` |
| incarnation never seen | `UnknownIncarnation` |
| incarnation not reconciled for this era | `ReconciliationRequired` |
| control operation from a non-operator | `NotOperator` |
| bound producer/consumer/grant contradicted | `ProducerMismatch` / `ConsumerMismatch` / `GrantMismatch` |
| optimistic evidence no longer current | `StaleEvidence` |

**Reconciliation.** Each authority boot generates a fresh 128-bit nonce. A
caller proves participation by presenting `digest(nonce, incarnation, authority
vector)`. The proof binds the whole authority vector, so an epoch advance
invalidates every previously captured proof. Recovery deliberately resets every
reconciled incarnation to `UNKNOWN`: durable bytes never restore liveness.

**Epochs and generations.** `advance epoch` increments both epoch and
generation and quarantines all in-flight credit. `rotate --to N` sets an explicit
strictly-greater epoch. Every previously live incarnation becomes `UNKNOWN` and
must revalidate against the new era.

**Fences are permanent.** A fence appends an immutable record naming the
incarnation, the era it was fenced in, the cause and the live credit it
invalidated. No operation removes a fence: not reconciliation, not an epoch
advance, not a restart.

**Provenance binding.** The first `issue` from an incarnation fixes which
`producer`/`consumer`/`grant` pair it speaks for. A later attempt claiming a
different pair is refused rather than silently accepted.

## Attempt identity and replay protection

An attempt carries a 128-bit `AttemptId` and a per-incarnation `sequence`
index. The pair `(incarnation, sequence)` is the decision identity:

1. If the pair is already decided **and the request digest matches**, the
   recorded outcome is returned verbatim: `duplicate = true`, `applied = false`,
   and no state changes. Duplicates are absorbed, not re-decided.
2. If the pair is already decided **and the request digest differs**, the attempt
   is a contradiction: `AttemptConflict`.
3. If the sequence is below the watermark, it is `StaleReplay` — refused, never
   re-applied, even after the bounded window has evicted the record.
4. If the sequence is above the watermark, it is `SequenceGap` and the refusal
   reports the exact expected slot.
5. A watermark at the top of its range is `BudgetExceeded`; the index never
   wraps into a live slot.

Pre-admission refusals (stale authority, unknown incarnation, fenced
incarnation, gaps, malformed shape) carry **no durable identity** and consume no
sequence: the caller repairs its authority and retries the same slot. Refusals
that were decided against the ledger *do* consume their slot and are journaled,
so they replay verbatim.

The dedup window is bounded (`max_attempt_window`, default 8192 records per
account). Eviction weakens *recall*, never *safety*: an evicted attempt is
refused as stale replay rather than re-decided.

## Durability

The journal has exactly two record kinds — a decided attempt and a snapshot —
plus an intent record:

```
<intent>  : written before the mutation so an unfinished attempt stays visible
<attempt> : request + outcome + post-state digest and counters
<snapshot>: whole-state replacement, also written to a separately
            digest-protected file and installed by atomic rename
```

Properties:

- **Framed and self-checking.** Every record is length-prefixed and CRC-32C
  protected. A record whose length or CRC does not validate ends the replay:
  everything before it stands, everything from it onward is reported as torn and
  truncated before the next append resumes.
- **Digest-chained.** Every record is chained from its request digest, its
  outcome digest and the previous chain value. Reordering, removal or rewriting
  is detected as `JournalDigestMismatch`.
- **Deterministically re-derived.** Replay re-runs the same state transitions
  from the recorded requests and compares the recomputed account digest against
  the recorded one. Recovery either reproduces the exact state or reports
  corruption; it never guesses.
- **Ambiguous outcomes are first-class.** An intent without a matching commit is
  reported as an ambiguous attempt in every explanation. It is resolved exactly
  when a decided attempt with the same identity commits — not by assumption.
- **Durable growth is bounded** by record count and byte budget, with atomic
  snapshot rotation.
- **No acknowledgement before durability.** A journal failure rolls back the
  in-memory mutation exactly and latches the engine, which then refuses
  everything rather than diverging from storage.

## Transport and multiprocess

Frames over loopback TCP:

```
magic u32 | version u8 | type u8 | flags u16 | length u32 | payload | crc32c u32
```

A receiver validates magic, version, reserved flags, the length bound and the
CRC before it looks at a single payload byte. Any failure closes the connection
with a named refusal; the server stays up and keeps serving other clients.

The coordinator owns the only `CreditEngine`. Workers hold no authority: they
receive the boot nonce in the handshake, reconcile, and then act. A request that
arrives after shutdown began is dropped **without an answer** — an
unacknowledged attempt stays undecided, which is the honest outcome.

## Concurrency and lock discipline

The whole runtime uses exactly two mutexes, and they are never nested:

| Lock | Guards | Held across |
|---|---|---|
| `CreditEngine::mutex_` | all authoritative state and journal access | nothing that can re-enter: no callbacks, no socket operations, no thread joins |
| `CoordinatorServer::registry_mutex_` | the worker-thread registry | nothing: never across a socket operation, an engine call, or a join |

Explicit audit results:

- **No read-lock to write-lock re-entry on the same lock.** No `std::shared_mutex`
  or recursive mutex exists anywhere in the codebase.
- **No write lock held across callbacks.** The engine has no callback surface;
  `Journal` is a pure interface whose implementations never call back.
- **No mutex re-entry through callbacks.** Nothing registered with the engine can
  run under its lock.
- **No event emission under an internal lock.** Explanation is built and returned
  by value; rendering happens after the lock is released.
- **No worker shutdown while holding locks a worker needs.** `stop()` signals,
  closes the listener, joins the accept thread, then drains the worker list by
  swapping it under the registry lock and joining **outside** the lock.
- **No join while holding state the joined thread needs.** Connection workers
  never touch the registry lock on any path that the joiner holds.
- **No cross-thread socket surgery.** Shutdown is poll-based: every socket wait
  has a bounded 50 ms timeout and re-checks the server's stop flag, so no peer can
  hold a worker — or the process — hostage by staying quiet. (An earlier revision
  used `shutdown(SHUT_RDWR)` from the stopping thread; that is not a reliable way
  to unblock a pending blocking `recv` on Windows, and a hardening test caught
  it. It was replaced, not worked around.)
- **No reversed lock ordering on cancellation paths.** With a strict
  `registry_mutex_ > engine mutex` ordering that is never exercised because the
  two are never held together, and no nested acquisition anywhere, inversion is
  structurally impossible.
- **No progress callback re-entering mutable state.**

Cancellation and shutdown semantics: cancelled or undecided work never reports
success and never mutates authoritative state. Work that did cross its commit
boundary is durable and replays; work that did not is either refused or left
undecided, and the caller learns which from the absence of a response.

## Structural bounds

Every bound exists so that a hostile or broken peer can never make the authority
allocate, log or remember an unbounded amount. All limits are clamped to a
non-zero minimum, so a zero can never become a division by zero or an erase on an
empty container.

| Bound | Default |
|---|---|
| Accounts per engine | 64 |
| Incarnations per account | 256 |
| Fences per account | 1024 |
| Attempt window per account | 8192 |
| Ambiguous attempts per account | 256 |
| Refusal log per account | 256 |
| Explanation rows (incarnations / fences / refusals) | 64 / 64 / 32 |
| Provenance note | 128 bytes |
| Journal record | 1 MiB |
| Journal before rotation | 64 MiB |
| Frame payload | 64 KiB |
| Concurrent connections | 64 |

## Tests

Fourteen test binaries, all green in Release and Debug. No timeouts.

| Binary | What it establishes |
|---|---|
| `test_checked` | checked arithmetic, wraparound, underflow, identity typing, digest determinism |
| `test_ledger` | the accounting core: closure after every transition, recycling, refusal reasons, huge counts |
| `test_engine` | open/issue/consume/return/protect/exhaust/capacity, provenance binding, refusal shape |
| `test_authority` | stale authority, unknown incarnation, fence permanence, epoch advance, revalidation, operator gating |
| `test_idempotency` | duplicate absorption, contradictions, stale replay, sequence gaps, window eviction, pre-admission semantics |
| `test_persistence` | restart, torn tail, corrupted record, snapshot rotation, foreign file, journal-failure rollback |
| `test_wire` | frame round trips, all single-byte corruptions, malformed shapes, random-byte fuzzing, codec round trips |
| `test_explain` | bounded explanation, exact refusal reasons, digest stability, unknown-account honesty |
| `test_concurrency` | real threads: no lost update, no double spend, one mutation per duplicated attempt, no torn reads |
| `test_property` | seeded randomized sequences against an independent reference model, invariant checked after every operation |
| `test_bench` | the benchmark harness itself: closure, determinism per seed, idempotent replays, no physical claims |
| `test_transport` | real sockets: handshake, damaged frames, oversized frames, invalid payloads, concurrency, connection limit |
| `test_multiprocess` | real processes: worker runs, coordinator hard kill, restart, watermark survival, incarnation fencing |
| `test_hardening` | defects found by the hardening pass, each pinned |

## Static analysis

| Tool | Scope | Result |
|---|---|---|
| MSVC `/analyze` (with `/EHsc`, external-header warnings suppressed) | library, apps, tests | **0 first-party findings** |
| `clang-tidy` 20 (`bugprone-*`, `performance-*`, `portability-*`, `clang-analyzer-*`) | `src/*.cpp` with `--header-filter=creditfabric` | 6 advisories, all `bugprone-easily-swappable-parameters` on small internal helpers whose call sites pass named locals (`crc32c(data, count, seed)`, `decode_frame(data, size, max_payload, out)`). Accepted as style advisories; no defect behind any of them |
| MSVC `/W4 /WX /permissive-` | library, apps, tests, Release and Debug | clean |
| AddressSanitizer | — | **not run**: see limitations |

The first `/analyze` run reported one genuine first-party finding
(`C28020` on the CRC-32C table, where a `std::array` index could not be proved
in range by the analyser). The table was rewritten as a plain array with an
explicitly bounded index, and the finding is gone.

Run one binary directly to see per-test output:

```sh
build/release/tests/test_property
build/release/tests/test_property executor_never_loses   # substring filter
```

## Benchmark

`cf_bench` measures **completed synthetic ledger decisions per second** against
the in-process authority. It is explicitly **not** a physical flow-control
measurement: there is no NIC, no switch, no fabric and no hardware credit
mechanism anywhere in this harness, and every report is labelled `SYNTHETIC`.

```sh
cf_bench --operations 200000 --capacity 4194304
```

The harness issues, consumes and returns credit across a synthetic population of
producers and consumers, replays a fraction of attempts verbatim to measure
idempotency, drains every wallet at the end, and then re-verifies that the
account closes exactly at capacity with zero in-flight credit. A run that does
not close is reported as `closure=OPEN` and exits non-zero.

Measured on the development machine (x64, MSVC 19.44, Release, single process,
`--operations 200000 --capacity 4194304`), three consecutive runs:

| Run | Completed operations | Refusals | Duplicates absorbed | Completed ops/s | Closure |
|---|---|---|---|---|---|
| 1 | 200000 | 0 | 2046 | 204456 | closed |
| 2 | 200000 | 0 | 2046 | 196353 | closed |
| 3 | 200000 | 0 | 2046 | 199759 | closed |

All three end at exactly `available = capacity`, `in_flight = 0`,
`issued = returned = 218365722`, `consumed = 127729`.

The same workload against a durable file journal, where every attempt writes an
intent and a commit record and waits for both to reach the storage device
(`--operations 20000 --durable`), completes at **~590 completed decisions per
second**. That number is dominated by the durability requirement, and it is
reported separately rather than blended into the in-memory figure.

Both figures are **SYNTHETIC**. Neither is a physical flow-control measurement.

## Verified closure

Every claim below was executed against the committed sources.

| Closure | Result |
|---|---|
| Release build, strict warnings | clean under MSVC `/W4 /WX /permissive- /utf-8 /Zc:__cplusplus` |
| Debug build, strict warnings | clean; full test suite green |
| Full test suite | 14/14 binaries pass in Release and Debug |
| No double spend | `consume` requires spendable credit; property suite checks an independent model after every operation |
| Exact closure | `C == available + protected + in_flight` and `in_flight == outstanding + spent + stale` re-verified after every operation and at the end of the benchmark |
| Stale-credit refusal | epoch advance quarantines in-flight credit; the previous era's authority vector and proof are both refused |
| Restart reconciliation | recovery rebuilds state and resets liveness to `UNKNOWN`; a durable incarnation is refused until it revalidates against the new boot's nonce |
| Incarnation fencing | a hard-killed worker is fenced by a third process and its identical publisher/boot is refused when it returns |
| Property / adversarial closure | seeded model-based suite plus malformed-frame, corruption, torn-tail and huge-count adversarial tests |
| Install / `find_package` | installed to a prefix and consumed by an independent project via `find_package(CreditFabric 1.0 CONFIG REQUIRED)` |
| Fresh clone | built and tested from a clean clone of the committed sources |
| Remote / tag | `main` and annotated tag `v1.0.0` pushed and verified against the same commit |

## Remaining limitations

These are real and are not worked around:

1. **No sanitizer run.** MSVC `/fsanitize=address` compiles but cannot link in
   this environment; the Clang ASan runtime is not installed with this Visual
   Studio configuration, and no Clang compiler is present. No ASan, TSan or MSan
   coverage is claimed.
2. **Unauthenticated transport.** Anything that can reach the loopback port can
   claim an incarnation identity. Reconciliation proves freshness and
   participation, not identity. Do not expose the coordinator beyond loopback
   without adding authentication in front of it.
3. **No validated physical profile.** Physical credit mechanisms are out of scope
   and no hardware validation of any kind was performed.
4. **One engine per journal.** There is no file locking. Two engines must not hold
   the same journal; the second writer's view will diverge. This is documented,
   not enforced by the OS.
5. **The reconciliation proof is not cryptographic.** It is a keyed 128-bit
   binding digest used for staleness binding and corruption detection, not a MAC.
6. **Bounded replay recall.** After a snapshot rotation or window eviction, an
   attempt older than the retained window is refused as stale replay rather than
   answered from the recorded outcome. Safety is preserved; verbatim recall for
   very old attempts is not.
7. **Single-process authority.** The accounting model assumes one authoritative
   engine per account. Multi-writer replication is not implemented.
8. **No real-time guarantees.** No decision is derived from wall-clock time, and
   none is promised in bounded wall-clock time.

## License

Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
