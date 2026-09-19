// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// The engine is the single authority for credit decisions. Every attempt walks
// the same path:
//
//   validate -> bind authority -> resolve incarnation -> place in sequence
//            -> plan -> verify invariants -> journal intent -> commit
//            -> journal commit -> publish to the attempt window
//
// Nothing is acknowledged before its commit record is durable, and a journal
// failure poisons the engine rather than leaving memory ahead of storage.

#include "creditfabric/engine.hpp"

#include <algorithm>
#include <deque>
#include <string>
#include <utility>

#include "creditfabric/journal.hpp"
#include "creditfabric/record.hpp"
#include "creditfabric/wire.hpp"

namespace creditfabric {

const char* to_string(OpKind op) noexcept {
  switch (op) {
    case OpKind::Describe:
      return "Describe";
    case OpKind::Open:
      return "Open";
    case OpKind::Issue:
      return "Issue";
    case OpKind::Consume:
      return "Consume";
    case OpKind::Return:
      return "Return";
    case OpKind::Protect:
      return "Protect";
    case OpKind::Unprotect:
      return "Unprotect";
    case OpKind::Exhaust:
      return "Exhaust";
    case OpKind::ClearExhaustion:
      return "ClearExhaustion";
    case OpKind::SetCapacity:
      return "SetCapacity";
    case OpKind::AdvanceEpoch:
      return "AdvanceEpoch";
    case OpKind::Fence:
      return "Fence";
    case OpKind::Rotate:
      return "Rotate";
    case OpKind::Retire:
      return "Retire";
    case OpKind::Revalidate:
      return "Revalidate";
    case OpKind::Reconcile:
      return "Reconcile";
    case OpKind::RetireIncarnation:
      return "RetireIncarnation";
  }
  return "Unknown";
}

bool is_valid_op(std::uint8_t raw) noexcept { return raw < kOpKindCount; }

OpClass classify(OpKind op) noexcept {
  switch (op) {
    case OpKind::Describe:
      return OpClass::Read;
    case OpKind::Issue:
    case OpKind::Consume:
    case OpKind::Return:
    case OpKind::Protect:
    case OpKind::Unprotect:
    case OpKind::Reconcile:
      return OpClass::Data;
    default:
      return OpClass::Control;
  }
}

const char* to_string(RevalidationDecision decision) noexcept {
  switch (decision) {
    case RevalidationDecision::None:
      return "None";
    case RevalidationDecision::ToReturned:
      return "ToReturned";
    case RevalidationDecision::ToConsumed:
      return "ToConsumed";
  }
  return "Unknown";
}

namespace {

Digest128 config_digest(const AccountConfig& config) {
  return DigestBuilder{Digest128{0x3C9A17E5B2D48F60ull, 0x608FD4B2E517A93Cull}}
      .u64(config.domain.value())
      .u64(config.account.value())
      .u64(config.resource.value())
      .u64(config.policy.value())
      .u64(config.capacity)
      .u64(config.max_capacity)
      .u64(static_cast<std::uint64_t>(config.profile))
      .u64(config.rules.min_issue)
      .u64(config.rules.max_issue_per_attempt)
      .boolean(config.rules.allow_protect)
      .boolean(config.rules.allow_return)
      .boolean(config.rules.allow_capacity_growth)
      .finish();
}

bool op_requires_count(OpKind op) noexcept {
  switch (op) {
    case OpKind::Issue:
    case OpKind::Consume:
    case OpKind::Return:
    case OpKind::Protect:
    case OpKind::Unprotect:
    case OpKind::Retire:
    case OpKind::Revalidate:
      return true;
    default:
      return false;
  }
}

bool op_is_ledger(OpKind op) noexcept {
  switch (op) {
    case OpKind::Issue:
    case OpKind::Consume:
    case OpKind::Return:
    case OpKind::Protect:
    case OpKind::Unprotect:
      return true;
    default:
      return false;
  }
}

Digest128 outcome_digest(const CreditOutcome& outcome) {
  return DigestBuilder{Digest128{0x1122334455667788ull, 0x8877665544332211ull}}
      .u64(static_cast<std::uint64_t>(outcome.status.reason))
      .u64(outcome.status.detail)
      .u64(static_cast<std::uint64_t>(outcome.op))
      .u64(outcome.sequence)
      .boolean(outcome.applied)
      .boolean(outcome.decided)
      .digest(outcome.state_digest)
      .finish();
}

}  // namespace

Digest128 CreditRequest::digest() const noexcept {
  return DigestBuilder{Digest128{0xFEEDFACE12345678ull, 0x87654321CEFAEDEFull}}
      .digest(attempt.value)
      .u64(static_cast<std::uint64_t>(op))
      .u64(sequence)
      .digest(authority.digest())
      .u64(presenter.publisher.value())
      .u64(presenter.boot.value())
      .u64(presenter.id.value())
      .u64(resource.value())
      .u64(producer.value())
      .u64(consumer.value())
      .u64(grant.value())
      .u64(target.value())
      .u64(count)
      .u64(new_capacity)
      .digest(proof)
      .digest(expected_state)
      .digest(config_digest(config))
      .u64(static_cast<std::uint64_t>(decision))
      .u64(static_cast<std::uint64_t>(fence_cause))
      .text(note)
      .finish();
}

std::string CreditOutcome::describe() const {
  std::string text = to_string(op);
  text += " seq=";
  text += std::to_string(sequence);
  text += " -> ";
  text += status.describe();
  if (duplicate) {
    text += " [duplicate]";
  } else if (applied) {
    text += " [applied]";
  }
  if (decided) text += " [decided]";
  return text;
}

struct CreditEngine::AttemptWindow {
  struct Key {
    std::uint64_t incarnation{0};
    std::uint64_t sequence{0};
    friend bool operator==(const Key& lhs, const Key& rhs) noexcept {
      return lhs.incarnation == rhs.incarnation && lhs.sequence == rhs.sequence;
    }
  };

  struct KeyHash {
    std::size_t operator()(const Key& key) const noexcept {
      return static_cast<std::size_t>(mix64(key.incarnation * 0x9E3779B97F4A7C15ull ^ key.sequence));
    }
  };

  struct Entry {
    Key key{};
    AttemptId attempt{};
    Digest128 request{};
    Status status{};
    bool applied{false};
    CreditView view{};
  };

  explicit AttemptWindow(std::size_t limit) : limit_(limit == 0 ? 1 : limit) {}

  [[nodiscard]] const Entry* find(const Key& key) const {
    const auto it = by_key_.find(key);
    return it == by_key_.end() ? nullptr : &it->second;
  }

  [[nodiscard]] const Entry* find_by_attempt(const AttemptId& id) const {
    const auto it = by_attempt_.find(id);
    if (it == by_attempt_.end()) return nullptr;
    return find(it->second);
  }

  void insert(const Entry& entry) {
    const auto previous = by_key_.find(entry.key);
    if (previous != by_key_.end()) by_attempt_.erase(previous->second.attempt);
    by_key_[entry.key] = entry;
    by_attempt_[entry.attempt] = entry.key;
    order_.push_back(entry.key);
    while (order_.size() > limit_) {
      const Key oldest = order_.front();
      order_.pop_front();
      const auto it = by_key_.find(oldest);
      if (it != by_key_.end()) {
        by_attempt_.erase(it->second.attempt);
        by_key_.erase(it);
      }
    }
  }

  void clear() {
    order_.clear();
    by_key_.clear();
    by_attempt_.clear();
  }

  /// Identity pairs in insertion order, used to carry replay protection across
  /// a snapshot rotation.
  [[nodiscard]] std::vector<std::pair<std::uint64_t, std::uint64_t>> identities() const {
    std::vector<std::pair<std::uint64_t, std::uint64_t>> out;
    out.reserve(order_.size());
    for (const Key& key : order_) out.emplace_back(key.incarnation, key.sequence);
    return out;
  }

  [[nodiscard]] std::size_t size() const noexcept { return by_key_.size(); }

 private:
  std::deque<Key> order_{};
  std::unordered_map<Key, Entry, KeyHash> by_key_{};
  std::unordered_map<AttemptId, Key> by_attempt_{};
  std::size_t limit_{1};
};

struct CreditEngine::AccountState {
  explicit AccountState(std::size_t window_limit) : window(window_limit) {}

  AccountConfig config{};
  DomainId domain{};
  AccountId account{};
  EpochId epoch{1};
  Generation generation{1};
  std::uint64_t capacity{0};
  CreditTotals totals{};
  bool hard_exhausted{false};
  Reason exhaustion_cause{Reason::Ok};
  AttemptWindow window;
  std::unordered_map<std::uint64_t, IncarnationRecord> incarnations{};
  std::vector<FenceRecord> fences{};
  std::vector<AmbiguousNote> ambiguous{};
  std::vector<RefusalNote> refusals{};
  std::size_t refusal_cursor{0};
  std::uint64_t committed_attempts{0};
  std::uint64_t refused_attempts{0};

  [[nodiscard]] CreditView view() const noexcept { return compute_view(capacity, totals); }

  [[nodiscard]] IncarnationRecord* find_incarnation(IncarnationId id) {
    const auto it = incarnations.find(id.value());
    return it == incarnations.end() ? nullptr : &it->second;
  }

  [[nodiscard]] bool is_fenced(IncarnationId id) const {
    return std::any_of(fences.begin(), fences.end(),
                       [id](const FenceRecord& fence) { return fence.incarnation == id; });
  }
};

namespace {

Digest128 account_digest(const CreditEngine::AccountState& account) {
  DigestBuilder builder{Digest128{0x51ED270B5A3D8F11ull, 0x118F3D5A0B27ED51ull}};
  builder.digest(config_digest(account.config));
  builder.u64(account.epoch.value());
  builder.u64(account.generation.value());
  builder.u64(account.capacity);
  builder.u64(account.totals.issued);
  builder.u64(account.totals.consumed);
  builder.u64(account.totals.returned);
  builder.u64(account.totals.stale);
  builder.u64(account.totals.protected_credits);
  builder.boolean(account.hard_exhausted);
  builder.u64(static_cast<std::uint64_t>(account.exhaustion_cause));
  builder.u64(account.committed_attempts);
  builder.u64(account.refused_attempts);
  builder.u64(account.fences.size());
  for (const FenceRecord& fence : account.fences) {
    builder.u64(fence.incarnation.value());
    builder.u64(fence.epoch.value());
    builder.u64(fence.generation.value());
    builder.u64(static_cast<std::uint64_t>(fence.cause));
    builder.u64(fence.sequence);
    builder.u64(fence.fenced_credit);
  }
  std::vector<std::uint64_t> ids;
  ids.reserve(account.incarnations.size());
  for (const auto& entry : account.incarnations) ids.push_back(entry.first);
  std::sort(ids.begin(), ids.end());
  builder.u64(ids.size());
  for (std::uint64_t id : ids) {
    const IncarnationRecord& record = account.incarnations.at(id);
    builder.u64(record.id.value());
    builder.u64(record.publisher.value());
    builder.u64(record.boot.value());
    builder.u64(static_cast<std::uint64_t>(record.state));
    builder.u64(record.reconciled_epoch.value());
    builder.u64(record.reconciled_generation.value());
    builder.u64(record.producer.value());
    builder.u64(record.consumer.value());
    builder.u64(record.grant.value());
    builder.u64(record.last_sequence);
    builder.u64(record.committed_ops);
    builder.u64(record.refused_ops);
  }
  return builder.finish();
}

/// Everything a single admitted attempt can change about an account, captured
/// in constant space so that a durability failure can be undone exactly.
struct AccountBackup {
  CreditTotals totals{};
  std::uint64_t capacity{0};
  bool hard_exhausted{false};
  Reason exhaustion_cause{Reason::Ok};
  EpochId epoch{};
  Generation generation{};
  std::uint64_t committed_attempts{0};
  std::uint64_t refused_attempts{0};
  std::size_t fence_count{0};
  bool caller_existed{false};
  IncarnationRecord caller{};
  bool target_existed{false};
  IncarnationRecord target{};
};

AccountBackup capture_backup(const CreditEngine::AccountState& account, IncarnationId caller, IncarnationId subject) {
  AccountBackup backup{};
  backup.totals = account.totals;
  backup.capacity = account.capacity;
  backup.hard_exhausted = account.hard_exhausted;
  backup.exhaustion_cause = account.exhaustion_cause;
  backup.epoch = account.epoch;
  backup.generation = account.generation;
  backup.committed_attempts = account.committed_attempts;
  backup.refused_attempts = account.refused_attempts;
  backup.fence_count = account.fences.size();
  const auto caller_entry = account.incarnations.find(caller.value());
  if (caller_entry != account.incarnations.end()) {
    backup.caller_existed = true;
    backup.caller = caller_entry->second;
  }
  if (subject.is_set() && !(subject == caller)) {
    const auto target_entry = account.incarnations.find(subject.value());
    if (target_entry != account.incarnations.end()) {
      backup.target_existed = true;
      backup.target = target_entry->second;
    }
  } else {
    backup.target_existed = backup.caller_existed;
    backup.target = backup.caller;
  }
  return backup;
}

void restore_backup(CreditEngine::AccountState& account, const AccountBackup& backup, IncarnationId caller,
                    IncarnationId subject) {
  account.totals = backup.totals;
  account.capacity = backup.capacity;
  account.hard_exhausted = backup.hard_exhausted;
  account.exhaustion_cause = backup.exhaustion_cause;
  account.epoch = backup.epoch;
  account.generation = backup.generation;
  account.committed_attempts = backup.committed_attempts;
  account.refused_attempts = backup.refused_attempts;
  account.fences.resize(backup.fence_count);
  if (backup.caller_existed) {
    account.incarnations[caller.value()] = backup.caller;
  } else {
    account.incarnations.erase(caller.value());
  }
  if (subject.is_set() && !(subject == caller)) {
    if (backup.target_existed) {
      account.incarnations[subject.value()] = backup.target;
    } else {
      account.incarnations.erase(subject.value());
    }
  }
}

CreditOutcome refused(CreditOutcome outcome, Reason reason, std::uint64_t detail) {
  outcome.status = Status::refused(reason, detail);
  outcome.applied = false;
  return outcome;
}

CreditOutcome decided_refusal(CreditOutcome outcome, Reason reason, std::uint64_t detail = 0) {
  outcome.status = Status::refused(reason, detail);
  outcome.applied = false;
  outcome.decided = true;
  return outcome;
}

}  // namespace

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

CreditEngine::CreditEngine(EngineLimits limits, std::unique_ptr<Journal> journal, Incarnation operator_incarnation)
    : limits_(limits), journal_(std::move(journal)), operator_incarnation_(operator_incarnation), challenge_() {
  // Structural bounds are clamped rather than trusted: a zero would otherwise
  // become a division by zero, an erase on an empty container, or a silently
  // unbounded collection.
  if (limits_.max_accounts == 0) limits_.max_accounts = 1;
  if (limits_.max_attempt_window == 0) limits_.max_attempt_window = 1;
  if (limits_.max_note_bytes == 0) limits_.max_note_bytes = 1;
  if (limits_.max_incarnations_per_account == 0) limits_.max_incarnations_per_account = 1;
  if (limits_.max_fences_per_account == 0) limits_.max_fences_per_account = 1;
  if (limits_.max_ambiguous_attempts == 0) limits_.max_ambiguous_attempts = 1;
  if (limits_.max_refusal_log == 0) limits_.max_refusal_log = 1;
  if (limits_.max_journal_record_bytes == 0) limits_.max_journal_record_bytes = 1024;
  if (limits_.max_frame_payload_bytes == 0) limits_.max_frame_payload_bytes = 1024;
  if (limits_.journal_records_per_snapshot == 0) limits_.journal_records_per_snapshot = 1024;
}

CreditEngine::~CreditEngine() = default;

// ---------------------------------------------------------------------------
// Recovery
// ---------------------------------------------------------------------------

Status CreditEngine::restore_durable_state(const DurableState& state) {
  accounts_.clear();
  for (const DurableAccount& durable : state.accounts) {
    auto account = std::make_unique<AccountState>(limits_.max_attempt_window);
    account->config = durable.config;
    account->domain = durable.config.domain;
    account->account = durable.config.account;
    account->epoch = durable.epoch;
    account->generation = durable.generation;
    account->capacity = durable.capacity;
    account->totals = durable.totals;
    account->hard_exhausted = durable.hard_exhausted;
    account->exhaustion_cause = durable.exhaustion_cause;
    account->committed_attempts = durable.committed_attempts;
    account->refused_attempts = durable.refused_attempts;
    account->fences = durable.fences;
    account->ambiguous = durable.ambiguous;
    for (const IncarnationRecord& record : durable.incarnations) {
      account->incarnations[record.id.value()] = record;
    }
    for (const auto& entry : durable.attempt_window) {
      AttemptWindow::Entry window_entry{};
      window_entry.key = AttemptWindow::Key{entry.first, entry.second};
      window_entry.status = Status::refused(Reason::AttemptUnknown);
      account->window.insert(window_entry);
    }
    const Status invariants = check_invariants(account->capacity, account->totals);
    if (!invariants.ok()) return Status::refused(Reason::JournalCorrupt, 120);
    if (!(account_digest(*account) == durable.state_digest)) {
      return Status::refused(Reason::JournalDigestMismatch, 121);
    }
    accounts_[account->account.value()] = std::move(account);
  }
  return Status::success();
}

Status CreditEngine::recover() {
  std::lock_guard<std::mutex> guard(mutex_);
  if (journal_ == nullptr) return Status::refused(Reason::InvalidState, 1);
  Status status = journal_->open();
  if (!status.ok()) return status;

  std::uint64_t snapshot_sequence = 0;
  Digest128 snapshot_chain{};
  std::vector<std::uint8_t> snapshot_payload;
  bool has_snapshot = false;
  status = journal_->read_snapshot(snapshot_sequence, snapshot_chain, snapshot_payload, has_snapshot);
  if (!status.ok()) return status;

  accounts_.clear();
  next_journal_sequence_ = 1;
  chain_ = Digest128{};
  journal_records_ = 0;
  snapshots_ = 0;

  if (has_snapshot) {
    DurableState restored{};
    status = decode_durable_state(snapshot_payload.data(), snapshot_payload.size(), restored);
    if (!status.ok()) return Status::refused(Reason::JournalCorrupt, 100);
    if (restored.journal_sequence != snapshot_sequence) return Status::refused(Reason::JournalDigestMismatch, 101);
    status = restore_durable_state(restored);
    if (!status.ok()) return status;
    chain_ = snapshot_chain;
    next_journal_sequence_ = snapshot_sequence + 1;
    snapshots_ = restored.snapshots;
  }

  status = journal_->begin_replay();
  if (!status.ok()) return status;

  for (;;) {
    JournalRecord record{};
    status = journal_->next_record(record);
    if (!status.ok()) return status;
    if (!record.valid) break;
    if (has_snapshot && record.sequence <= snapshot_sequence) continue;

    if (record.kind == JournalRecordKind::Intent) {
      CreditRequest pending{};
      const Status decoded =
          decode_request_message(record.payload.data(), record.payload.size(), limits_.max_note_bytes, pending);
      if (!decoded.ok()) return Status::refused(Reason::JournalCorrupt, 102);
      mark_ambiguous(pending, record.sequence);
      continue;
    }

    if (record.kind == JournalRecordKind::Snapshot) {
      DurableState restored{};
      const Status decoded = decode_durable_state(record.payload.data(), record.payload.size(), restored);
      if (!decoded.ok()) return Status::refused(Reason::JournalCorrupt, 103);
      if (restored.journal_sequence != record.sequence) return Status::refused(Reason::JournalDigestMismatch, 104);
      const Status restored_status = restore_durable_state(restored);
      if (!restored_status.ok()) return restored_status;
      snapshots_ = restored.snapshots;
      chain_ = record.chain;
      next_journal_sequence_ = record.sequence + 1;
      ++stats_.replayed_records;
      continue;
    }

    if (record.kind != JournalRecordKind::Attempt) return Status::refused(Reason::JournalCorrupt, 105);

    DurableAttempt attempt{};
    const Status decoded = decode_durable_attempt(record.payload.data(), record.payload.size(), attempt);
    if (!decoded.ok()) return Status::refused(Reason::JournalCorrupt, 106);

    const Digest128 expected_chain = DigestBuilder{chain_}
                                         .u64(record.sequence)
                                         .digest(attempt.request.digest())
                                         .digest(outcome_digest(attempt.outcome))
                                         .finish();
    if (!(expected_chain == record.chain)) return Status::refused(Reason::JournalDigestMismatch, record.sequence);

    const Status applied = replay_attempt(attempt);
    if (!applied.ok()) return applied;

    chain_ = record.chain;
    next_journal_sequence_ = record.sequence + 1;
    ++journal_records_;
    ++stats_.replayed_records;
  }

  const Status ended = journal_->end_replay();
  if (!ended.ok()) return ended;

  // Durable bytes never restore liveness: every reconciled incarnation becomes
  // UNKNOWN again and must revalidate against this boot's challenge.
  for (auto& entry : accounts_) {
    for (auto& incarnation : entry.second->incarnations) {
      if (incarnation.second.state == AuthorityState::Reconciled) {
        incarnation.second.state = AuthorityState::Unknown;
      }
    }
  }

  stats_.journal_records = journal_records_;
  stats_.journal_bytes = journal_->bytes();
  stats_.snapshots = snapshots_;
  recovered_ = true;
  return Status::success();
}

void CreditEngine::mark_ambiguous(const CreditRequest& request, std::uint64_t journal_sequence) {
  const auto it = accounts_.find(request.authority.account.value());
  if (it == accounts_.end()) return;
  AccountState& account = *it->second;
  for (const AmbiguousNote& note : account.ambiguous) {
    if (note.incarnation == request.authority.incarnation && note.sequence == request.sequence) return;
  }
  if (account.ambiguous.size() >= limits_.max_ambiguous_attempts) {
    account.ambiguous.erase(account.ambiguous.begin());
  }
  AmbiguousNote note{};
  note.attempt = request.attempt;
  note.op = request.op;
  note.sequence = request.sequence;
  note.incarnation = request.authority.incarnation;
  note.journal_sequence = journal_sequence;
  account.ambiguous.push_back(note);
  ++stats_.replayed_ambiguous;
}

void CreditEngine::resolve_ambiguous(AccountState& account, IncarnationId incarnation, std::uint64_t sequence) {
  account.ambiguous.erase(std::remove_if(account.ambiguous.begin(), account.ambiguous.end(),
                                         [incarnation, sequence](const AmbiguousNote& note) {
                                           return note.incarnation == incarnation && note.sequence == sequence;
                                         }),
                          account.ambiguous.end());
}

Status CreditEngine::replay_attempt(const DurableAttempt& attempt) {
  const CreditRequest& request = attempt.request;
  AccountState* account = nullptr;

  bool fresh_account = false;
  if (attempt.created_account) {
    fresh_account = true;
    const std::uint64_t key = request.authority.account.value();
    if (accounts_.find(key) != accounts_.end()) return Status::refused(Reason::JournalCorrupt, 130);
    auto fresh = std::make_unique<AccountState>(limits_.max_attempt_window);
    fresh->config = attempt.account.config;
    fresh->domain = attempt.account.config.domain;
    fresh->account = attempt.account.config.account;
    fresh->epoch = attempt.account.epoch;
    fresh->generation = attempt.account.generation;
    fresh->capacity = attempt.account.capacity;
    fresh->totals = attempt.account.totals;
    fresh->hard_exhausted = attempt.account.hard_exhausted;
    fresh->exhaustion_cause = attempt.account.exhaustion_cause;
    account = fresh.get();
    accounts_[key] = std::move(fresh);
    IncarnationRecord record{};
    record.id = request.authority.incarnation;
    record.publisher = request.presenter.publisher;
    record.boot = request.presenter.boot;
    record.state = AuthorityState::Reconciled;
    record.reconciled_epoch = account->epoch;
    record.reconciled_generation = account->generation;
    record.reconciliation_proof = request.proof;
    record.committed_ops = 1;
    account->incarnations[record.id.value()] = record;
    account->committed_attempts = 1;
  } else {
    const auto it = accounts_.find(request.authority.account.value());
    if (it == accounts_.end()) return Status::refused(Reason::JournalCorrupt, 131);
    account = it->second.get();
    if (attempt.outcome.applied) {
      const Status transition = apply_replay_transition(*account, request);
      if (!transition.ok()) return transition;
    }
  }

  // A freshly created account already carries the opener's counters, so the
  // per-attempt bookkeeping below applies only to subsequent decisions.
  if (!fresh_account) {
    if (attempt.outcome.applied) {
      if (request.op == OpKind::Reconcile) {
        IncarnationRecord* record = account->find_incarnation(request.authority.incarnation);
        if (record == nullptr) {
          IncarnationRecord fresh{};
          fresh.id = request.authority.incarnation;
          fresh.publisher = request.presenter.publisher;
          fresh.boot = request.presenter.boot;
          account->incarnations[fresh.id.value()] = fresh;
          record = account->find_incarnation(request.authority.incarnation);
          if (record == nullptr) return Status::refused(Reason::JournalCorrupt, 132);
        }
        record->state = AuthorityState::Reconciled;
        record->reconciled_epoch = account->epoch;
        record->reconciled_generation = account->generation;
        record->reconciliation_proof = request.proof;
      } else if (request.op == OpKind::Fence) {
        IncarnationRecord* record = account->find_incarnation(request.target);
        if (record == nullptr) {
          IncarnationRecord fresh{};
          fresh.id = request.target;
          account->incarnations[fresh.id.value()] = fresh;
          record = account->find_incarnation(request.target);
          if (record == nullptr) return Status::refused(Reason::JournalCorrupt, 133);
        }
        FenceRecord fence{};
        fence.incarnation = request.target;
        fence.epoch = account->epoch;
        fence.generation = account->generation;
        fence.cause = request.fence_cause;
        fence.sequence = request.sequence;
        const CreditView before = account->view();
        fence.fenced_credit = before.in_flight - before.stale;
        record->state = AuthorityState::Fenced;
        record->fenced_at_sequence = request.sequence;
        account->fences.push_back(fence);
      } else if (request.op == OpKind::RetireIncarnation) {
        IncarnationRecord* record = account->find_incarnation(request.target);
        if (record == nullptr) return Status::refused(Reason::JournalCorrupt, 134);
        record->state = AuthorityState::Retired;
      } else if (request.op == OpKind::Issue) {
        IncarnationRecord* record = account->find_incarnation(request.authority.incarnation);
        if (record == nullptr) return Status::refused(Reason::JournalCorrupt, 141);
        if (request.producer.is_set()) record->producer = request.producer;
        if (request.consumer.is_set()) record->consumer = request.consumer;
        if (request.grant.is_set()) record->grant = request.grant;
      }
      ++account->committed_attempts;
      IncarnationRecord* caller = account->find_incarnation(request.authority.incarnation);
      if (caller != nullptr) ++caller->committed_ops;
    } else if (attempt.outcome.decided) {
      ++account->refused_attempts;
      IncarnationRecord* caller = account->find_incarnation(request.authority.incarnation);
      if (caller != nullptr) ++caller->refused_ops;
    }
  }

  if (!(account_digest(*account) == attempt.account.state_digest)) {
    return Status::refused(Reason::JournalDigestMismatch, 135);
  }
  if (account->capacity != attempt.account.capacity) return Status::refused(Reason::JournalDigestMismatch, 136);
  if (!(account->totals == attempt.account.totals)) return Status::refused(Reason::JournalDigestMismatch, 137);
  if (account->hard_exhausted != attempt.account.hard_exhausted) {
    return Status::refused(Reason::JournalDigestMismatch, 138);
  }
  if (account->exhaustion_cause != attempt.account.exhaustion_cause) {
    return Status::refused(Reason::JournalDigestMismatch, 139);
  }

  if (attempt.outcome.decided) {
    CreditOutcome recorded = attempt.outcome;
    recorded.authority.domain = account->domain;
    recorded.authority.account = account->account;
    recorded.authority.epoch = account->epoch;
    recorded.authority.generation = account->generation;
    recorded.authority.incarnation = request.authority.incarnation;
    recorded.view = account->view();
    publish_window_locked(*account, request, recorded);
  }

  stats_.attempts_applied = attempt.engine_applied_attempts;
  stats_.attempts_refused = attempt.engine_refused_attempts;
  return Status::success();
}

Status CreditEngine::apply_replay_transition(AccountState& account, const CreditRequest& request) {
  switch (request.op) {
    case OpKind::Open:
    case OpKind::Reconcile:
    case OpKind::Fence:
    case OpKind::RetireIncarnation:
    case OpKind::Describe:
      return Status::success();
    case OpKind::Issue:
    case OpKind::Consume:
    case OpKind::Return:
    case OpKind::Protect:
    case OpKind::Unprotect: {
      LedgerPlan plan(account.capacity, account.totals);
      const Status status = ledger_apply(plan, account, request);
      if (!status.ok()) return Status::refused(Reason::JournalDigestMismatch, 140);
      account.totals = plan.totals();
      return Status::success();
    }
    case OpKind::Exhaust:
      account.hard_exhausted = true;
      account.exhaustion_cause = request.fence_cause;
      return Status::success();
    case OpKind::ClearExhaustion:
      account.hard_exhausted = false;
      account.exhaustion_cause = Reason::Ok;
      return Status::success();
    case OpKind::SetCapacity: {
      LedgerPlan plan(account.capacity, account.totals);
      if (!plan.set_capacity(request.new_capacity, account.config.max_capacity,
                             account.config.rules.allow_capacity_growth)) {
        return Status::refused(Reason::JournalDigestMismatch, 141);
      }
      account.capacity = plan.capacity();
      return Status::success();
    }
    case OpKind::AdvanceEpoch:
    case OpKind::Rotate: {
      const std::uint64_t next_epoch = (request.op == OpKind::Rotate) ? request.count : (account.epoch.value() + 1);
      LedgerPlan plan(account.capacity, account.totals);
      if (!plan.quarantine_in_flight()) {
        return Status::refused(Reason::JournalDigestMismatch, 142);
      }
      account.totals = plan.totals();
      account.epoch = EpochId{next_epoch};
      account.generation = Generation{account.generation.value() + 1};
      for (auto& entry : account.incarnations) {
        if (entry.second.state == AuthorityState::Reconciled) entry.second.state = AuthorityState::Unknown;
      }
      return Status::success();
    }
    case OpKind::Retire: {
      LedgerPlan plan(account.capacity, account.totals);
      if (!plan.retire_stale(request.count)) return Status::refused(Reason::JournalDigestMismatch, 143);
      account.totals = plan.totals();
      return Status::success();
    }
    case OpKind::Revalidate: {
      LedgerPlan plan(account.capacity, account.totals);
      const bool ok = request.decision == RevalidationDecision::ToReturned
                          ? plan.revalidate_to_returned(request.count)
                          : plan.revalidate_to_consumed(request.count);
      if (!ok) return Status::refused(Reason::JournalDigestMismatch, 144);
      account.totals = plan.totals();
      return Status::success();
    }
  }
  return Status::refused(Reason::JournalCorrupt, 145);
}

// ---------------------------------------------------------------------------
// Live decisions
// ---------------------------------------------------------------------------

CreditOutcome CreditEngine::apply(const CreditRequest& request) {
  std::lock_guard<std::mutex> guard(mutex_);
  CreditOutcome outcome{};
  outcome.op = request.op;
  outcome.sequence = request.sequence;
  outcome.authority = request.authority;
  if (!recovered_) return refused(outcome, Reason::ReconciliationRequired, 1);
  if (journal_failed_) return refused(outcome, Reason::IoError, 2);
  return apply_locked(request);
}

CreditOutcome CreditEngine::apply_locked(const CreditRequest& request) {
  CreditOutcome outcome{};
  outcome.op = request.op;
  outcome.sequence = request.sequence;
  outcome.authority = request.authority;

  // --- structural validation: no identity, no sequence, no durable trace ---
  if (!request.presenter.is_set() || request.presenter.id != request.authority.incarnation ||
      !(make_incarnation_id(request.presenter.publisher, request.presenter.boot) == request.presenter.id)) {
    return refused(outcome, Reason::MalformedMessage, 2);
  }
  if (!request.authority.domain.is_set() || !request.authority.account.is_set()) {
    return refused(outcome, Reason::MalformedMessage, 3);
  }
  if (request.sequence == 0) return refused(outcome, Reason::MalformedMessage, 4);
  if (request.note.size() > limits_.max_note_bytes) {
    return refused(outcome, Reason::OversizedFrame, request.note.size());
  }
  if (op_requires_count(request.op) && request.count == 0) return refused(outcome, Reason::CountZero, 1);

  if (request.op == OpKind::Describe) {
    const auto it = accounts_.find(request.authority.account.value());
    if (it == accounts_.end()) return refused(outcome, Reason::AccountUnknown, 1);
    const AccountState& account = *it->second;
    if (!(request.authority.domain == account.domain)) return refused(outcome, Reason::DomainMismatch, 1);
    outcome.authority.epoch = account.epoch;
    outcome.authority.generation = account.generation;
    outcome.view = account.view();
    outcome.state_digest = account_digest(account);
    outcome.ambiguous_pending = account.ambiguous.size();
    // Describe reports the caller's own committed sequence watermark so that a
    // restarted client can place its next attempt at watermark + 1 instead of
    // guessing and provoking a replay refusal.
    outcome.sequence = 0;
    const auto caller = account.incarnations.find(request.authority.incarnation.value());
    if (caller != account.incarnations.end()) outcome.sequence = caller->second.last_sequence;
    outcome.status = Status::success();
    return outcome;
  }

  if (request.op == OpKind::Open) return create_account_locked(request, outcome);

  const auto it = accounts_.find(request.authority.account.value());
  if (it == accounts_.end()) return refused(outcome, Reason::AccountUnknown, 2);
  AccountState& account = *it->second;

  // --- authority binding ---
  if (!(request.authority.domain == account.domain)) return refused(outcome, Reason::DomainMismatch, 2);
  if (!(request.authority.epoch == account.epoch) || !(request.authority.generation == account.generation)) {
    return refused(outcome, Reason::StaleAuthority, account.generation.value());
  }

  IncarnationRecord* incarnation = account.find_incarnation(request.authority.incarnation);
  if (request.op == OpKind::Reconcile) {
    if (account.is_fenced(request.authority.incarnation)) {
      return refused(outcome, Reason::FencedIncarnation, request.authority.incarnation.value());
    }
    if (incarnation != nullptr && incarnation->state == AuthorityState::Retired) {
      return refused(outcome, Reason::IncarnationNotReconciled, request.authority.incarnation.value());
    }
    if (incarnation == nullptr && account.incarnations.size() >= limits_.max_incarnations_per_account) {
      return refused(outcome, Reason::BudgetExceeded, account.incarnations.size());
    }
  } else {
    if (incarnation == nullptr) return refused(outcome, Reason::UnknownIncarnation, 1);
    if (incarnation->state == AuthorityState::Fenced) {
      return refused(outcome, Reason::FencedIncarnation, request.authority.incarnation.value());
    }
    if (incarnation->state == AuthorityState::Retired) {
      return refused(outcome, Reason::IncarnationNotReconciled, request.authority.incarnation.value());
    }
    if (!incarnation->is_live(account.epoch, account.generation)) {
      return refused(outcome, Reason::ReconciliationRequired, 3);
    }
  }

  const OpClass op_class = classify(request.op);
  if (op_class == OpClass::Control && !(request.authority.incarnation == operator_incarnation_.id)) {
    return refused(outcome, Reason::NotOperator, request.authority.incarnation.value());
  }

  // --- attempt identity and sequence placement ---
  const AttemptWindow::Key key{request.authority.incarnation.value(), request.sequence};
  const Digest128 request_digest = request.digest();
  if (const AttemptWindow::Entry* existing = account.window.find(key)) {
    if (existing->request == request_digest) {
      outcome.status = existing->status;
      outcome.applied = false;
      outcome.duplicate = true;
      outcome.decided = true;
      outcome.view = account.view();
      outcome.state_digest = account_digest(account);
      outcome.authority.epoch = account.epoch;
      outcome.authority.generation = account.generation;
      ++stats_.duplicates_absorbed;
      return outcome;
    }
    return decided_refusal(outcome, Reason::AttemptConflict, request.sequence);
  }
  if (account.window.find_by_attempt(request.attempt) != nullptr) {
    return decided_refusal(outcome, Reason::AttemptConflict, request.sequence);
  }

  const std::uint64_t watermark = incarnation == nullptr ? 0 : incarnation->last_sequence;
  if (request.sequence <= watermark) return refused(outcome, Reason::StaleReplay, watermark);
  std::uint64_t expected_sequence = 0;
  if (add_overflow(watermark, 1, expected_sequence)) {
    // The incarnation has exhausted its decision index space; no further
    // attempt from it can be placed, and wraparound is never accepted.
    return refused(outcome, Reason::BudgetExceeded, watermark);
  }
  if (request.sequence > expected_sequence) return refused(outcome, Reason::SequenceGap, expected_sequence);

  outcome.authority.epoch = account.epoch;
  outcome.authority.generation = account.generation;
  if (op_class == OpClass::Control) return decide_control_locked(account, request, outcome);
  return decide_data_locked(account, request, outcome);
}

CreditOutcome CreditEngine::create_account_locked(const CreditRequest& request, CreditOutcome outcome) {
  if (accounts_.find(request.authority.account.value()) != accounts_.end()) {
    return refused(outcome, Reason::AccountExists, request.authority.account.value());
  }
  const AccountConfig& config = request.config;
  if (!(config.domain == request.authority.domain) || !(config.account == request.authority.account)) {
    return refused(outcome, Reason::DomainMismatch, 3);
  }
  if (!config.resource.is_set()) return refused(outcome, Reason::ResourceMismatch, 1);
  if (request.authority.epoch.value() != 1 || request.authority.generation.value() != 1) {
    return refused(outcome, Reason::StaleAuthority, 1);
  }
  if (config.capacity == 0 || config.max_capacity < config.capacity) {
    return refused(outcome, Reason::CapacityOutOfRange, config.capacity);
  }
  if (config.profile == ProfileKind::PhysicalValidated) {
    return refused(outcome, Reason::ProfileNotSupported, static_cast<std::uint64_t>(config.profile));
  }
  if (config.rules.min_issue == 0 || config.rules.max_issue_per_attempt < config.rules.min_issue) {
    return refused(outcome, Reason::PolicyRefused, 1);
  }
  if (accounts_.size() >= limits_.max_accounts) {
    return refused(outcome, Reason::BudgetExceeded, accounts_.size());
  }
  if (request.sequence != 1) return refused(outcome, Reason::SequenceGap, 1);
  if (!challenge_.verify(request.presenter, request.authority, request.proof)) {
    return refused(outcome, Reason::ReconciliationRequired, 4);
  }

  auto account = std::make_unique<AccountState>(limits_.max_attempt_window);
  account->config = config;
  account->domain = config.domain;
  account->account = config.account;
  account->epoch = request.authority.epoch;
  account->generation = request.authority.generation;
  account->capacity = config.capacity;
  account->totals = CreditTotals{};
  account->hard_exhausted = false;
  account->exhaustion_cause = Reason::Ok;

  IncarnationRecord record{};
  record.id = request.authority.incarnation;
  record.publisher = request.presenter.publisher;
  record.boot = request.presenter.boot;
  record.state = AuthorityState::Reconciled;
  record.reconciled_epoch = account->epoch;
  record.reconciled_generation = account->generation;
  record.reconciliation_proof = request.proof;
  record.committed_ops = 1;
  account->incarnations[record.id.value()] = record;
  account->committed_attempts = 1;

  AccountState* raw = account.get();
  accounts_[account->account.value()] = std::move(account);

  outcome.applied = true;
  outcome.decided = true;
  outcome.status = Status::success();
  outcome.view = raw->view();
  outcome.state_digest = account_digest(*raw);

  const Status persisted = persist_locked(request, outcome, raw, true);
  if (!persisted.ok()) {
    accounts_.erase(raw->account.value());
    journal_failed_ = true;
    CreditOutcome failure = outcome;
    failure.applied = false;
    failure.decided = false;
    failure.status = persisted;
    return failure;
  }
  publish_window_locked(*raw, request, outcome);
  ++stats_.attempts_applied;
  return outcome;
}

Status CreditEngine::ledger_apply(LedgerPlan& plan, AccountState& account, const CreditRequest& request) {
  switch (request.op) {
    case OpKind::Issue:
      return plan.issue(request.count, account.config.rules.min_issue,
                        account.config.rules.max_issue_per_attempt)
                 ? Status::success()
                 : plan.status();
    case OpKind::Consume:
      return plan.consume(request.count) ? Status::success() : plan.status();
    case OpKind::Return:
      if (!account.config.rules.allow_return) return Status::refused(Reason::PolicyRefused, 2);
      return plan.give_back(request.count) ? Status::success() : plan.status();
    case OpKind::Protect:
      return plan.protect(request.count, account.config.rules.allow_protect) ? Status::success() : plan.status();
    case OpKind::Unprotect:
      return plan.unprotect(request.count) ? Status::success() : plan.status();
    default:
      return Status::refused(Reason::InvalidState, 10);
  }
}

CreditOutcome CreditEngine::decide_data_locked(AccountState& account, const CreditRequest& request,
                                               CreditOutcome outcome) {
  if (request.op == OpKind::Reconcile) {
    if (!challenge_.verify(request.presenter, request.authority, request.proof)) {
      return refused(outcome, Reason::ReconciliationRequired, 5);
    }
    const AccountBackup backup = capture_backup(account, request.authority.incarnation, IncarnationId{});
    IncarnationRecord* record = account.find_incarnation(request.authority.incarnation);
    if (record == nullptr) {
      IncarnationRecord fresh{};
      fresh.id = request.authority.incarnation;
      fresh.publisher = request.presenter.publisher;
      fresh.boot = request.presenter.boot;
      account.incarnations[fresh.id.value()] = fresh;
      record = account.find_incarnation(request.authority.incarnation);
      if (record == nullptr) return refused(outcome, Reason::InternalError, 1);
    }
    record->state = AuthorityState::Reconciled;
    record->reconciled_epoch = account.epoch;
    record->reconciled_generation = account.generation;
    record->reconciliation_proof = request.proof;
    ++record->committed_ops;
    ++account.committed_attempts;
    outcome.applied = true;
    outcome.decided = true;
    outcome.status = Status::success();
    outcome.view = account.view();
    outcome.state_digest = account_digest(account);

    const Status persisted = persist_locked(request, outcome, &account, false);
    if (!persisted.ok()) {
      restore_backup(account, backup, request.authority.incarnation, IncarnationId{});
      journal_failed_ = true;
      CreditOutcome failure = outcome;
      failure.applied = false;
      failure.decided = false;
      failure.status = persisted;
      return failure;
    }
    publish_window_locked(account, request, outcome);
    ++stats_.attempts_applied;
    return outcome;
  }

  if (!op_is_ledger(request.op)) {
    return refused(outcome, Reason::Unsupported, static_cast<std::uint64_t>(request.op));
  }

  if (account.hard_exhausted && request.op == OpKind::Issue) {
    return finish_decided(
        account, request,
        decided_refusal(outcome, Reason::ExhaustedHard, static_cast<std::uint64_t>(account.exhaustion_cause)));
  }

  const AccountBackup backup = capture_backup(account, request.authority.incarnation, IncarnationId{});

  if (request.op == OpKind::Issue) {
    IncarnationRecord* binding = account.find_incarnation(request.authority.incarnation);
    if (binding == nullptr) return refused(outcome, Reason::UnknownIncarnation, 3);
    if (request.producer.is_set()) {
      if (binding->producer.is_set() && !(binding->producer == request.producer)) {
        return finish_decided(account, request,
                              decided_refusal(outcome, Reason::ProducerMismatch, binding->producer.value()));
      }
      binding->producer = request.producer;
    }
    if (request.consumer.is_set()) {
      if (binding->consumer.is_set() && !(binding->consumer == request.consumer)) {
        return finish_decided(account, request,
                              decided_refusal(outcome, Reason::ConsumerMismatch, binding->consumer.value()));
      }
      binding->consumer = request.consumer;
    }
    if (request.grant.is_set()) {
      if (binding->grant.is_set() && !(binding->grant == request.grant)) {
        return finish_decided(account, request,
                              decided_refusal(outcome, Reason::GrantMismatch, binding->grant.value()));
      }
      binding->grant = request.grant;
    }
  }
  LedgerPlan plan(account.capacity, account.totals);
  const Status verdict = ledger_apply(plan, account, request);
  if (!verdict.ok()) {
    return finish_decided(account, request, decided_refusal(outcome, verdict.reason, verdict.detail));
  }
  account.totals = plan.totals();
  ++account.committed_attempts;
  if (IncarnationRecord* record = account.find_incarnation(request.authority.incarnation)) ++record->committed_ops;
  outcome.applied = true;
  outcome.decided = true;
  outcome.status = Status::success();
  outcome.view = account.view();
  outcome.state_digest = account_digest(account);

  const Status persisted = persist_locked(request, outcome, &account, false);
  if (!persisted.ok()) {
    restore_backup(account, backup, request.authority.incarnation, IncarnationId{});
    journal_failed_ = true;
    CreditOutcome failure = outcome;
    failure.applied = false;
    failure.decided = false;
    failure.status = persisted;
    return failure;
  }
  publish_window_locked(account, request, outcome);
  ++stats_.attempts_applied;
  return outcome;
}

CreditOutcome CreditEngine::finish_decided(AccountState& account, const CreditRequest& request,
                                           CreditOutcome outcome) {
  const AccountBackup backup = capture_backup(account, request.authority.incarnation, IncarnationId{});
  ++account.refused_attempts;
  if (IncarnationRecord* record = account.find_incarnation(request.authority.incarnation)) ++record->refused_ops;
  outcome.view = account.view();
  outcome.state_digest = account_digest(account);
  const Status persisted = persist_locked(request, outcome, &account, false);
  if (!persisted.ok()) {
    restore_backup(account, backup, request.authority.incarnation, IncarnationId{});
    journal_failed_ = true;
    CreditOutcome failure = outcome;
    failure.applied = false;
    failure.decided = false;
    failure.status = persisted;
    return failure;
  }
  publish_window_locked(account, request, outcome);
  record_refusal_locked(account, request, outcome);
  ++stats_.attempts_refused;
  return outcome;
}

CreditOutcome CreditEngine::decide_control_locked(AccountState& account, const CreditRequest& request,
                                                  CreditOutcome outcome) {
  if (!request.expected_state.is_zero() && !(request.expected_state == account_digest(account))) {
    return refused(outcome, Reason::StaleEvidence, 1);
  }

  const AccountBackup backup = capture_backup(account, request.authority.incarnation, request.target);

  switch (request.op) {
    case OpKind::Exhaust: {
      if (account.hard_exhausted) {
        return finish_decided(account, request, decided_refusal(outcome, Reason::ExhaustedHard, 1));
      }
      if (request.fence_cause == Reason::Ok) return refused(outcome, Reason::PolicyRefused, 3);
      account.hard_exhausted = true;
      account.exhaustion_cause = request.fence_cause;
      break;
    }
    case OpKind::ClearExhaustion: {
      if (!account.hard_exhausted) {
        return finish_decided(account, request, decided_refusal(outcome, Reason::NotExhausted, 1));
      }
      account.hard_exhausted = false;
      account.exhaustion_cause = Reason::Ok;
      break;
    }
    case OpKind::SetCapacity: {
      LedgerPlan plan(account.capacity, account.totals);
      if (!plan.set_capacity(request.new_capacity, account.config.max_capacity,
                             account.config.rules.allow_capacity_growth)) {
        return finish_decided(account, request, decided_refusal(outcome, plan.status().reason, plan.status().detail));
      }
      account.capacity = plan.capacity();
      break;
    }
    case OpKind::AdvanceEpoch:
    case OpKind::Rotate: {
      std::uint64_t next_epoch = 0;
      if (request.op == OpKind::Rotate) {
        next_epoch = request.count;
        if (next_epoch <= account.epoch.value()) return refused(outcome, Reason::InvalidState, account.epoch.value());
      } else if (add_overflow(account.epoch.value(), 1, next_epoch)) {
        return refused(outcome, Reason::CountOverflow, account.epoch.value());
      }
      std::uint64_t next_generation = 0;
      if (add_overflow(account.generation.value(), 1, next_generation)) {
        return refused(outcome, Reason::CountOverflow, account.generation.value());
      }
      LedgerPlan plan(account.capacity, account.totals);
      if (!plan.quarantine_in_flight()) {
        return refused(outcome, plan.status().reason, plan.status().detail);
      }
      account.totals = plan.totals();
      account.epoch = EpochId{next_epoch};
      account.generation = Generation{next_generation};
      for (auto& entry : account.incarnations) {
        if (entry.second.state == AuthorityState::Reconciled) entry.second.state = AuthorityState::Unknown;
      }
      break;
    }
    case OpKind::Retire: {
      LedgerPlan plan(account.capacity, account.totals);
      if (!plan.retire_stale(request.count)) {
        return finish_decided(account, request, decided_refusal(outcome, plan.status().reason, plan.status().detail));
      }
      account.totals = plan.totals();
      break;
    }
    case OpKind::Revalidate: {
      if (request.decision == RevalidationDecision::None) return refused(outcome, Reason::PolicyRefused, 4);
      LedgerPlan plan(account.capacity, account.totals);
      const bool ok = request.decision == RevalidationDecision::ToReturned
                          ? plan.revalidate_to_returned(request.count)
                          : plan.revalidate_to_consumed(request.count);
      if (!ok) {
        return finish_decided(account, request, decided_refusal(outcome, plan.status().reason, plan.status().detail));
      }
      account.totals = plan.totals();
      break;
    }
    case OpKind::Fence: {
      if (!request.target.is_set()) return refused(outcome, Reason::MalformedMessage, 5);
      if (request.fence_cause == Reason::Ok) return refused(outcome, Reason::PolicyRefused, 5);
      if (account.is_fenced(request.target)) {
        return finish_decided(account, request, decided_refusal(outcome, Reason::InvalidState, 1));
      }
      IncarnationRecord* record = account.find_incarnation(request.target);
      if (record == nullptr) {
        if (account.incarnations.size() >= limits_.max_incarnations_per_account) {
          return refused(outcome, Reason::BudgetExceeded, account.incarnations.size());
        }
        IncarnationRecord fresh{};
        fresh.id = request.target;
        account.incarnations[fresh.id.value()] = fresh;
        record = account.find_incarnation(request.target);
        if (record == nullptr) return refused(outcome, Reason::InternalError, 2);
      }
      if (account.fences.size() >= limits_.max_fences_per_account) {
        return refused(outcome, Reason::BudgetExceeded, account.fences.size());
      }
      FenceRecord fence{};
      fence.incarnation = request.target;
      fence.epoch = account.epoch;
      fence.generation = account.generation;
      fence.cause = request.fence_cause;
      fence.sequence = request.sequence;
      const CreditView before = account.view();
      fence.fenced_credit = before.in_flight - before.stale;
      record->state = AuthorityState::Fenced;
      record->fenced_at_sequence = request.sequence;
      account.fences.push_back(fence);
      break;
    }
    case OpKind::RetireIncarnation: {
      if (!request.target.is_set()) return refused(outcome, Reason::MalformedMessage, 6);
      IncarnationRecord* record = account.find_incarnation(request.target);
      if (record == nullptr) return refused(outcome, Reason::UnknownIncarnation, 2);
      if (record->state == AuthorityState::Fenced) {
        return finish_decided(account, request, decided_refusal(outcome, Reason::FencedIncarnation, 1));
      }
      record->state = AuthorityState::Retired;
      break;
    }
    default:
      return refused(outcome, Reason::Unsupported, static_cast<std::uint64_t>(request.op));
  }

  ++account.committed_attempts;
  if (IncarnationRecord* record = account.find_incarnation(request.authority.incarnation)) ++record->committed_ops;
  outcome.applied = true;
  outcome.decided = true;
  outcome.status = Status::success();
  outcome.view = account.view();
  outcome.state_digest = account_digest(account);

  const Status persisted = persist_locked(request, outcome, &account, false);
  if (!persisted.ok()) {
    restore_backup(account, backup, request.authority.incarnation, request.target);
    journal_failed_ = true;
    CreditOutcome failure = outcome;
    failure.applied = false;
    failure.decided = false;
    failure.status = persisted;
    return failure;
  }
  publish_window_locked(account, request, outcome);
  ++stats_.attempts_applied;
  return outcome;
}

// ---------------------------------------------------------------------------
// Durability
// ---------------------------------------------------------------------------

DurableAccount CreditEngine::build_durable_account(const AccountState& account, bool whole) const {
  DurableAccount durable{};
  durable.config = account.config;
  durable.epoch = account.epoch;
  durable.generation = account.generation;
  durable.capacity = account.capacity;
  durable.totals = account.totals;
  durable.hard_exhausted = account.hard_exhausted;
  durable.exhaustion_cause = account.exhaustion_cause;
  durable.state_digest = account_digest(account);
  durable.committed_attempts = account.committed_attempts;
  durable.refused_attempts = account.refused_attempts;
  if (!whole) return durable;

  durable.incarnations.reserve(account.incarnations.size());
  for (const auto& entry : account.incarnations) durable.incarnations.push_back(entry.second);
  durable.fences = account.fences;
  durable.ambiguous = account.ambiguous;
  durable.attempt_window = account.window.identities();
  return durable;
}

Status CreditEngine::persist_locked(const CreditRequest& request, const CreditOutcome& outcome,
                                    const AccountState* account, bool created_account) {
  std::vector<std::uint8_t> intent_payload;
  const Status encoded_intent = encode_request_message(request, intent_payload);
  if (!encoded_intent.ok()) return encoded_intent;

  const std::uint64_t intent_sequence = next_journal_sequence_;
  const Digest128 intent_chain = DigestBuilder{chain_}.u64(intent_sequence).digest(request.digest()).finish();
  Status status = journal_->append(intent_sequence, JournalRecordKind::Intent, intent_chain, intent_payload);
  if (!status.ok()) return status;
  ++next_journal_sequence_;

  DurableAttempt durable{};
  durable.request = request;
  durable.outcome = outcome;
  durable.has_account = account != nullptr;
  durable.created_account = created_account;
  if (account != nullptr) durable.account = build_durable_account(*account, false);
  durable.engine_applied_attempts = stats_.attempts_applied;
  durable.engine_refused_attempts = stats_.attempts_refused;

  std::vector<std::uint8_t> commit_payload;
  const Status encoded_commit = encode_durable_attempt(durable, commit_payload);
  if (!encoded_commit.ok()) return encoded_commit;

  const std::uint64_t commit_sequence = next_journal_sequence_;
  const Digest128 commit_chain = DigestBuilder{chain_}
                                     .u64(commit_sequence)
                                     .digest(request.digest())
                                     .digest(outcome_digest(outcome))
                                     .finish();
  status = journal_->append(commit_sequence, JournalRecordKind::Attempt, commit_chain, commit_payload);
  if (!status.ok()) return status;

  chain_ = commit_chain;
  ++next_journal_sequence_;
  journal_records_ += 2;
  stats_.journal_records = journal_records_;
  stats_.journal_bytes = journal_->bytes();

  if (journal_records_ >= limits_.journal_records_per_snapshot ||
      journal_->bytes() >= limits_.max_journal_bytes) {
    return snapshot_locked();
  }
  return Status::success();
}

Status CreditEngine::snapshot_locked() {
  if (journal_ == nullptr) return Status::refused(Reason::InvalidState, 2);
  const std::uint64_t sequence = next_journal_sequence_;
  DurableState state{};
  state.journal_sequence = sequence;
  state.chain = chain_;
  state.snapshots = snapshots_ + 1;
  state.accounts.reserve(accounts_.size());
  for (const auto& entry : accounts_) state.accounts.push_back(build_durable_account(*entry.second, true));
  std::sort(state.accounts.begin(), state.accounts.end(), [](const DurableAccount& lhs, const DurableAccount& rhs) {
    return lhs.config.account.value() < rhs.config.account.value();
  });
  DigestBuilder aggregate{Digest128{0x2211AA55BB66CC77ull, 0x77CC66BB55AA1122ull}};
  for (const DurableAccount& account : state.accounts) aggregate.digest(account.state_digest);
  state.state_digest = aggregate.finish();

  std::vector<std::uint8_t> payload;
  const Status encoded = encode_durable_state(state, payload);
  if (!encoded.ok()) return encoded;

  Status status = journal_->append(sequence, JournalRecordKind::Snapshot, chain_, payload);
  if (!status.ok()) return status;
  ++next_journal_sequence_;
  ++journal_records_;

  status = journal_->write_snapshot(sequence, chain_, payload);
  if (!status.ok()) return status;
  status = journal_->rotate(sequence);
  if (!status.ok()) return status;

  journal_records_ = 0;
  ++snapshots_;
  stats_.journal_records = 0;
  stats_.journal_bytes = journal_->bytes();
  stats_.snapshots = snapshots_;
  return Status::success();
}

Status CreditEngine::snapshot() {
  std::lock_guard<std::mutex> guard(mutex_);
  if (!recovered_) return Status::refused(Reason::ReconciliationRequired, 2);
  if (journal_failed_) return Status::refused(Reason::IoError, 3);
  return snapshot_locked();
}

// ---------------------------------------------------------------------------
// Window, refusals, explanation
// ---------------------------------------------------------------------------

void CreditEngine::publish_window_locked(AccountState& account, const CreditRequest& request,
                                         const CreditOutcome& outcome) {
  AttemptWindow::Entry entry{};
  entry.key = AttemptWindow::Key{request.authority.incarnation.value(), request.sequence};
  entry.attempt = request.attempt;
  entry.request = request.digest();
  entry.status = outcome.status;
  entry.applied = outcome.applied;
  entry.view = outcome.view;
  account.window.insert(entry);
  if (IncarnationRecord* record = account.find_incarnation(request.authority.incarnation)) {
    record->last_sequence = request.sequence;
  }
  resolve_ambiguous(account, request.authority.incarnation, request.sequence);
}

void CreditEngine::record_refusal_locked(AccountState& account, const CreditRequest& request,
                                         const CreditOutcome& outcome) {
  RefusalNote note{};
  note.attempt = request.attempt;
  note.op = request.op;
  note.sequence = request.sequence;
  note.incarnation = request.authority.incarnation;
  note.reason = outcome.status.reason;
  note.detail = outcome.status.detail;
  note.epoch = account.epoch;
  note.generation = account.generation;
  if (account.refusals.size() < limits_.max_refusal_log) {
    account.refusals.push_back(note);
    return;
  }
  account.refusals[account.refusal_cursor % limits_.max_refusal_log] = note;
  account.refusal_cursor = (account.refusal_cursor + 1) % limits_.max_refusal_log;
}

std::vector<AccountId> CreditEngine::accounts() const {
  std::lock_guard<std::mutex> guard(mutex_);
  std::vector<AccountId> ids;
  ids.reserve(accounts_.size());
  for (const auto& entry : accounts_) ids.push_back(entry.second->account);
  std::sort(ids.begin(), ids.end());
  return ids;
}

EngineStats CreditEngine::stats() const {
  std::lock_guard<std::mutex> guard(mutex_);
  EngineStats copy = stats_;
  copy.journal_records = journal_records_;
  copy.snapshots = snapshots_;
  if (journal_ != nullptr) copy.journal_bytes = journal_->bytes();
  return copy;
}

Explanation CreditEngine::explain(AccountId account_id) const {
  std::lock_guard<std::mutex> guard(mutex_);
  Explanation explanation{};
  explanation.operator_incarnation = operator_incarnation_.id;
  const auto it = accounts_.find(account_id.value());
  if (it == accounts_.end()) {
    explanation.note = "account unknown";
    return explanation;
  }
  const AccountState& account = *it->second;
  explanation.found = true;
  explanation.config = account.config;
  explanation.view = account.view();
  explanation.hard_exhausted = account.hard_exhausted;
  explanation.exhaustion_cause = account.exhaustion_cause;
  explanation.state_digest = account_digest(account);
  explanation.authority.domain = account.domain;
  explanation.authority.account = account.account;
  explanation.authority.epoch = account.epoch;
  explanation.authority.generation = account.generation;
  explanation.committed_attempts = account.committed_attempts;
  explanation.refused_attempts = account.refused_attempts;
  explanation.ambiguous_total = account.ambiguous.size();
  explanation.fences_total = account.fences.size();
  explanation.incarnations_total = account.incarnations.size();
  explanation.refusals_total = account.refusals.size();

  std::vector<std::uint64_t> ids;
  ids.reserve(account.incarnations.size());
  for (const auto& entry : account.incarnations) ids.push_back(entry.first);
  std::sort(ids.begin(), ids.end());
  for (std::uint64_t id : ids) {
    if (explanation.incarnations.size() >= limits_.max_explanation_incarnations) break;
    const IncarnationRecord& record = account.incarnations.at(id);
    IncarnationSummary summary{};
    summary.id = record.id;
    summary.publisher = record.publisher;
    summary.boot = record.boot;
    summary.state = record.state;
    summary.reconciled_epoch = record.reconciled_epoch;
    summary.reconciled_generation = record.reconciled_generation;
    summary.producer = record.producer;
    summary.consumer = record.consumer;
    summary.grant = record.grant;
    summary.last_sequence = record.last_sequence;
    summary.committed_ops = record.committed_ops;
    summary.refused_ops = record.refused_ops;
    explanation.incarnations.push_back(summary);
  }
  for (std::size_t i = 0; i < account.fences.size() && explanation.fences.size() < limits_.max_explanation_fences; ++i) {
    explanation.fences.push_back(account.fences[i]);
  }
  for (std::size_t i = 0;
       i < account.ambiguous.size() && explanation.ambiguous.size() < limits_.max_ambiguous_attempts; ++i) {
    explanation.ambiguous.push_back(account.ambiguous[i]);
  }
  for (std::size_t i = 0;
       i < account.refusals.size() && explanation.refusals.size() < limits_.max_explanation_refusals; ++i) {
    explanation.refusals.push_back(account.refusals[i]);
  }
  return explanation;
}

std::string render(const Explanation& explanation) {
  std::string text;
  text += "account=";
  text += to_hex(explanation.config.account.value());
  if (!explanation.found) {
    text += " UNKNOWN";
    text += "\n";
    return text;
  }
  text += " domain=";
  text += to_hex(explanation.config.domain.value());
  text += " resource=";
  text += to_hex(explanation.config.resource.value());
  text += " profile=";
  text += to_string(explanation.config.profile);
  text += "\n";
  text += "authority: epoch=";
  text += std::to_string(explanation.authority.epoch.value());
  text += " generation=";
  text += std::to_string(explanation.authority.generation.value());
  text += " operator=";
  text += to_hex(explanation.operator_incarnation.value());
  text += "\n";
  text += "view: ";
  text += explanation.view.to_string();
  text += "\n";
  text += "exhaustion: ";
  text += explanation.hard_exhausted ? "HARD" : "open";
  text += " cause=";
  text += to_string(explanation.exhaustion_cause);
  text += explanation.view.exhausted() ? " available=0" : " available>0";
  text += "\n";
  text += "state_digest=";
  text += explanation.state_digest.to_hex();
  text += " committed=";
  text += std::to_string(explanation.committed_attempts);
  text += " refused=";
  text += std::to_string(explanation.refused_attempts);
  text += "\n";
  text += "incarnations=";
  text += std::to_string(explanation.incarnations_total);
  text += " fences=";
  text += std::to_string(explanation.fences_total);
  text += " ambiguous=";
  text += std::to_string(explanation.ambiguous_total);
  text += "\n";
  for (const IncarnationSummary& summary : explanation.incarnations) {
    text += "  incarnation ";
    text += to_hex(summary.id.value());
    text += " publisher=";
    text += to_hex(summary.publisher.value());
    text += " boot=";
    text += to_hex(summary.boot.value());
    text += " state=";
    text += to_string(summary.state);
    text += " era=";
    text += std::to_string(summary.reconciled_epoch.value());
    text += "/";
    text += std::to_string(summary.reconciled_generation.value());
    text += " producer=";
    text += to_hex(summary.producer.value());
    text += " consumer=";
    text += to_hex(summary.consumer.value());
    text += " last_seq=";
    text += std::to_string(summary.last_sequence);
    text += "\n";
  }
  for (const FenceRecord& fence : explanation.fences) {
    text += "  fence ";
    text += to_hex(fence.incarnation.value());
    text += " at epoch=";
    text += std::to_string(fence.epoch.value());
    text += " cause=";
    text += to_string(fence.cause);
    text += " fenced_credit=";
    text += std::to_string(fence.fenced_credit);
    text += "\n";
  }
  for (const AmbiguousNote& note : explanation.ambiguous) {
    text += "  ambiguous ";
    text += to_string(note.op);
    text += " seq=";
    text += std::to_string(note.sequence);
    text += " incarnation=";
    text += to_hex(note.incarnation.value());
    text += "\n";
  }
  for (const RefusalNote& note : explanation.refusals) {
    text += "  refusal ";
    text += to_string(note.op);
    text += " seq=";
    text += std::to_string(note.sequence);
    text += " reason=";
    text += to_string(note.reason);
    text += "\n";
  }
  return text;
}

}  // namespace creditfabric
