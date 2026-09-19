// Credit Fabric - credit authority engine.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// The engine is the single authority for every credit decision. It is
// deliberately single-locked: exactly one mutex guards all authoritative state,
// it is never held across a callback, a socket operation or a thread join, and
// the journal is called underneath it and never calls back. See README
// "Concurrency and lock discipline".

#ifndef CREDITFABRIC_ENGINE_HPP
#define CREDITFABRIC_ENGINE_HPP

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "creditfabric/authority.hpp"
#include "creditfabric/ledger.hpp"
#include "creditfabric/policy.hpp"
#include "creditfabric/strong.hpp"

namespace creditfabric {

class Journal;

struct DurableState;
struct DurableAttempt;
struct DurableAccount;

/// Operation kinds. Values are durable and must never be renumbered.
enum class OpKind : std::uint8_t {
  Describe = 0,        ///< read-only
  Open = 1,            ///< create an account (control)
  Issue = 2,           ///< available -> outstanding
  Consume = 3,         ///< spend outstanding credit
  Return = 4,          ///< outstanding -> available
  Protect = 5,         ///< available -> protected
  Unprotect = 6,       ///< protected -> available
  Exhaust = 7,         ///< latch hard exhaustion (control)
  ClearExhaustion = 8, ///< release the latch (control)
  SetCapacity = 9,     ///< change capacity (control)
  AdvanceEpoch = 10,   ///< new authority era; quarantines outstanding credit (control)
  Fence = 11,          ///< permanently invalidate an incarnation (control)
  Rotate = 12,         ///< alias of AdvanceEpoch with an explicit new epoch value (control)
  Retire = 13,         ///< quarantine outstanding credit without advancing (control)
  Revalidate = 14,     ///< resolve quarantined credit (control)
  Reconcile = 15,      ///< prove incarnation liveness for the current era
  RetireIncarnation = 16, ///< close an incarnation without fencing it (control)
};

inline constexpr std::uint8_t kOpKindCount = 17u;

[[nodiscard]] const char* to_string(OpKind op) noexcept;
[[nodiscard]] bool is_valid_op(std::uint8_t raw) noexcept;

/// How an operation is authorized.
enum class OpClass : std::uint8_t {
  Read = 0,     ///< no mutation, no journal record
  Data = 1,     ///< requires a live, reconciled incarnation
  Control = 2,  ///< requires a live, reconciled operator incarnation
};

[[nodiscard]] OpClass classify(OpKind op) noexcept;

/// How quarantined credit is resolved by a Revalidate attempt.
enum class RevalidationDecision : std::uint8_t {
  None = 0,
  ToReturned = 1,  ///< proved never spent: the credits go back to the pool
  ToConsumed = 2,  ///< proved spent: the credits keep occupying capacity
};

[[nodiscard]] const char* to_string(RevalidationDecision decision) noexcept;

/// One authoritative request. Fixed shape, bounded note, no unbounded fields.
struct CreditRequest {
  AttemptId attempt{};
  OpKind op{OpKind::Describe};
  std::uint64_t sequence{0};  ///< per-incarnation decision index; 1-based, strictly sequential
  AuthorityVector authority{};
  Incarnation presenter{};  ///< must hash to authority.incarnation; never trusted unchecked
  ResourceId resource{};
  ProducerId producer{};
  ConsumerId consumer{};
  GrantId grant{};
  IncarnationId target{};  ///< Fence / RetireIncarnation subject
  std::uint64_t count{0};
  std::uint64_t new_capacity{0};
  Digest128 proof{};             ///< reconciliation proof (Reconcile only)
  Digest128 expected_state{};    ///< evidence binding; zero means "not supplied"
  AccountConfig config{};        ///< Open only
  RevalidationDecision decision{RevalidationDecision::None};
  Reason fence_cause{Reason::UnknownState};
  std::string note{};            ///< bounded provenance note

  [[nodiscard]] Digest128 digest() const noexcept;
};

/// Result of one attempt, as returned to the caller and recorded durably.
///
/// For OpKind::Describe, \c sequence carries the caller's committed sequence
/// watermark for this account (0 when the incarnation is not yet known). A
/// client uses that value to place its next attempt exactly at watermark + 1.
struct CreditOutcome {
  Status status{};
  OpKind op{OpKind::Describe};
  std::uint64_t sequence{0};
  bool applied{false};    ///< this call mutated authoritative state
  bool duplicate{false};  ///< recognized as an already-decided attempt
  bool decided{false};    ///< the attempt was admitted to the ledger and consumed its sequence
  AuthorityVector authority{};
  CreditView view{};
  Digest128 state_digest{};
  std::size_t ambiguous_pending{0};

  [[nodiscard]] bool ok() const noexcept { return status.ok(); }
  [[nodiscard]] std::string describe() const;
};

/// Bounded audit rows carried in an explanation.
struct IncarnationSummary {
  IncarnationId id{};
  PublisherId publisher{};
  BootId boot{};
  AuthorityState state{AuthorityState::Unknown};
  EpochId reconciled_epoch{};
  Generation reconciled_generation{};
  ProducerId producer{};
  ConsumerId consumer{};
  GrantId grant{};
  std::uint64_t last_sequence{0};
  std::uint64_t committed_ops{0};
  std::uint64_t refused_ops{0};
};

struct AmbiguousNote {
  AttemptId attempt{};
  OpKind op{OpKind::Describe};
  std::uint64_t sequence{0};
  IncarnationId incarnation{};
  std::uint64_t journal_sequence{0};
};

struct RefusalNote {
  AttemptId attempt{};
  OpKind op{OpKind::Describe};
  std::uint64_t sequence{0};
  IncarnationId incarnation{};
  Reason reason{Reason::Ok};
  std::uint64_t detail{0};
  EpochId epoch{};
  Generation generation{};
};

/// Complete, bounded, explainable state of one account.
struct Explanation {
  bool found{false};
  AccountConfig config{};
  AuthorityVector authority{};
  CreditView view{};
  bool hard_exhausted{false};
  Reason exhaustion_cause{Reason::Ok};
  IncarnationId operator_incarnation{};
  std::uint64_t committed_attempts{0};
  std::uint64_t refused_attempts{0};
  std::uint64_t ambiguous_total{0};
  std::uint64_t fences_total{0};
  std::uint64_t incarnations_total{0};
  std::uint64_t refusals_total{0};
  std::vector<IncarnationSummary> incarnations{};
  std::vector<FenceRecord> fences{};
  std::vector<AmbiguousNote> ambiguous{};
  std::vector<RefusalNote> refusals{};
  Digest128 state_digest{};
  std::string note{};
};

[[nodiscard]] std::string render(const Explanation& explanation);

/// Durable engine statistics, useful for closure reporting.
struct EngineStats {
  std::uint64_t attempts_applied{0};
  std::uint64_t attempts_refused{0};
  std::uint64_t duplicates_absorbed{0};
  std::uint64_t journal_records{0};
  std::uint64_t journal_bytes{0};
  std::uint64_t snapshots{0};
  std::uint64_t replayed_records{0};
  std::uint64_t replayed_ambiguous{0};
};

/// Crash-safe, deterministic credit authority.
class CreditEngine {
 public:
  CreditEngine(EngineLimits limits, std::unique_ptr<Journal> journal, Incarnation operator_incarnation);
  ~CreditEngine();

  CreditEngine(const CreditEngine&) = delete;
  CreditEngine& operator=(const CreditEngine&) = delete;

  /// Replays durable state. Never restores incarnation liveness.
  [[nodiscard]] Status recover();

  /// Evaluates and, when permitted, commits one attempt.
  [[nodiscard]] CreditOutcome apply(const CreditRequest& request);

  /// Read-only projection. Never mutates and never journals.
  [[nodiscard]] Explanation explain(AccountId account) const;
  [[nodiscard]] std::vector<AccountId> accounts() const;

  [[nodiscard]] const ReconciliationChallenge& challenge() const noexcept { return challenge_; }
  [[nodiscard]] const Incarnation& operator_incarnation() const noexcept { return operator_incarnation_; }
  [[nodiscard]] EngineStats stats() const;
  [[nodiscard]] const EngineLimits& limits() const noexcept { return limits_; }

  /// Force a snapshot of durable state. Returns the resulting journal size.
  [[nodiscard]] Status snapshot();

  // Implementation-detail types. They are only forward declared here so that
  // the durable record codec can be written in its own translation unit.
  struct AccountState;
  struct AttemptWindow;

 private:
  [[nodiscard]] CreditOutcome apply_locked(const CreditRequest& request);
  [[nodiscard]] CreditOutcome create_account_locked(const CreditRequest& request, CreditOutcome outcome);
  [[nodiscard]] CreditOutcome decide_data_locked(AccountState& account, const CreditRequest& request,
                                                 CreditOutcome outcome);
  [[nodiscard]] CreditOutcome decide_control_locked(AccountState& account, const CreditRequest& request,
                                                    CreditOutcome outcome);
  [[nodiscard]] CreditOutcome finish_decided(AccountState& account, const CreditRequest& request,
                                             CreditOutcome outcome);
  [[nodiscard]] Status ledger_apply(LedgerPlan& plan, AccountState& account, const CreditRequest& request);
  [[nodiscard]] Status apply_replay_transition(AccountState& account, const CreditRequest& request);

  [[nodiscard]] Status restore_durable_state(const DurableState& state);
  [[nodiscard]] Status replay_attempt(const DurableAttempt& attempt);
  void mark_ambiguous(const CreditRequest& request, std::uint64_t journal_sequence);
  void resolve_ambiguous(AccountState& account, IncarnationId incarnation, std::uint64_t sequence);
  [[nodiscard]] DurableAccount build_durable_account(const AccountState& account, bool whole) const;
  [[nodiscard]] Status persist_locked(const CreditRequest& request, const CreditOutcome& outcome,
                                      const AccountState* account, bool created_account);
  [[nodiscard]] Status snapshot_locked();
  void record_refusal_locked(AccountState& account, const CreditRequest& request, const CreditOutcome& outcome);
  void publish_window_locked(AccountState& account, const CreditRequest& request, const CreditOutcome& outcome);

  mutable std::mutex mutex_;  ///< leaf lock; never held across IO or callbacks
  EngineLimits limits_;
  std::unique_ptr<Journal> journal_;
  Incarnation operator_incarnation_;
  ReconciliationChallenge challenge_;
  std::unordered_map<std::uint64_t, std::unique_ptr<AccountState>> accounts_;
  std::uint64_t next_journal_sequence_{0};
  Digest128 chain_{};
  std::uint64_t journal_bytes_{0};
  std::uint64_t journal_records_{0};
  std::uint64_t snapshots_{0};
  EngineStats stats_{};
  bool recovered_{false};
  bool journal_failed_{false};  ///< latched: a journal failure poisons the engine until restart
};

}  // namespace creditfabric

#endif  // CREDITFABRIC_ENGINE_HPP
