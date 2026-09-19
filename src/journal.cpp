// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "creditfabric/journal.hpp"

namespace creditfabric {

Status InMemoryJournal::open() {
  cursor_ = 0;
  replaying_ = false;
  torn_ = false;
  return Status::success();
}

Status InMemoryJournal::append(std::uint64_t sequence, JournalRecordKind kind, Digest128 chain,
                              const std::vector<std::uint8_t>& payload) {
  if (replaying_) return Status::refused(Reason::InvalidState, 1);
  const std::uint64_t growth = static_cast<std::uint64_t>(payload.size()) + 8u;
  if (bytes_ + growth > max_bytes_) return Status::refused(Reason::BudgetExceeded, bytes_);
  Entry entry{};
  entry.sequence = sequence;
  entry.kind = kind;
  entry.chain = chain;
  entry.payload = payload;
  entries_.push_back(std::move(entry));
  bytes_ += growth;
  return Status::success();
}

Status InMemoryJournal::begin_replay() {
  cursor_ = 0;
  replaying_ = true;
  torn_ = false;
  return Status::success();
}

Status InMemoryJournal::next_record(JournalRecord& out) {
  out = JournalRecord{};
  while (cursor_ < entries_.size()) {
    const Entry& entry = entries_[cursor_];
    if (entry.corrupted) {
      torn_ = true;
      return Status::success();
    }
    out.valid = true;
    out.sequence = entry.sequence;
    out.kind = entry.kind;
    out.chain = entry.chain;
    out.payload = entry.payload;
    out.offset = cursor_;
    ++cursor_;
    return Status::success();
  }
  return Status::success();
}

Status InMemoryJournal::end_replay() {
  replaying_ = false;
  return Status::success();
}

Status InMemoryJournal::rotate(std::uint64_t snapshot_sequence) {
  std::vector<Entry> kept;
  kept.reserve(entries_.size());
  std::uint64_t bytes = 0;
  for (Entry& entry : entries_) {
    if (entry.sequence <= snapshot_sequence) continue;
    bytes += static_cast<std::uint64_t>(entry.payload.size()) + 8u;
    kept.push_back(std::move(entry));
  }
  entries_ = std::move(kept);
  bytes_ = bytes;
  cursor_ = 0;
  return Status::success();
}

Status InMemoryJournal::write_snapshot(std::uint64_t sequence, Digest128 chain,
                                        const std::vector<std::uint8_t>& payload) {
  if (payload.size() > max_bytes_) return Status::refused(Reason::BudgetExceeded, payload.size());
  has_snapshot_ = true;
  snapshot_sequence_ = sequence;
  snapshot_chain_ = chain;
  snapshot_payload_ = payload;
  return Status::success();
}

Status InMemoryJournal::read_snapshot(std::uint64_t& sequence, Digest128& chain, std::vector<std::uint8_t>& payload,
                                      bool& present) {
  present = has_snapshot_;
  if (!has_snapshot_) return Status::success();
  sequence = snapshot_sequence_;
  chain = snapshot_chain_;
  payload = snapshot_payload_;
  return Status::success();
}

Status InMemoryJournal::close() {
  replaying_ = false;
  return Status::success();
}

void InMemoryJournal::simulate_torn_tail() {
  if (entries_.empty()) return;
  entries_.back().corrupted = true;
}

void InMemoryJournal::simulate_corruption(std::size_t index) {
  if (index >= entries_.size()) return;
  entries_[index].corrupted = true;
}

}  // namespace creditfabric
