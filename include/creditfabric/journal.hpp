// Credit Fabric - journal interface and in-memory implementation.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Journal implementations are NOT thread-safe. CreditEngine serializes every
// call under its own mutex, so a journal never needs a lock of its own and can
// therefore never participate in a lock cycle.

#ifndef CREDITFABRIC_JOURNAL_HPP
#define CREDITFABRIC_JOURNAL_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "creditfabric/status.hpp"
#include "creditfabric/strong.hpp"
#include "creditfabric/version.hpp"

namespace creditfabric {

enum class JournalRecordKind : std::uint8_t {
  Unknown = 0,
  Attempt = 1,
  Snapshot = 2,
  Intent = 3,  ///< written before a mutation so an unfinished attempt stays visible
};

struct JournalRecord {
  bool valid{false};
  std::uint64_t sequence{0};
  JournalRecordKind kind{JournalRecordKind::Unknown};
  Digest128 chain{};
  std::vector<std::uint8_t> payload{};
  std::uint64_t offset{0};
};

class Journal {
 public:
  virtual ~Journal() = default;

  /// Prepares the journal for replay followed by append.
  [[nodiscard]] virtual Status open() = 0;

  /// Durably appends one record. Must not return Ok until the bytes are
  /// durable, because the engine only acknowledges after this returns.
  [[nodiscard]] virtual Status append(std::uint64_t sequence, JournalRecordKind kind, Digest128 chain,
                                      const std::vector<std::uint8_t>& payload) = 0;

  /// Begins a forward replay. Must be called before any append.
  [[nodiscard]] virtual Status begin_replay() = 0;

  /// Reads the next record. Returns Ok with out.valid == false at end of stream.
  [[nodiscard]] virtual Status next_record(JournalRecord& out) = 0;

  [[nodiscard]] virtual Status end_replay() = 0;

  /// Discards every record at or below snapshot_sequence after the snapshot
  /// containing them has been made durable.
  [[nodiscard]] virtual Status rotate(std::uint64_t snapshot_sequence) = 0;

  /// Makes a whole-state snapshot durable. A snapshot is written to a separate
  /// file and atomically replaced, so a crash during snapshotting leaves the
  /// previous snapshot intact and replayable.
  [[nodiscard]] virtual Status write_snapshot(std::uint64_t sequence, Digest128 chain,
                                              const std::vector<std::uint8_t>& payload) = 0;

  /// Loads the newest durable snapshot. \c present is false when none exists.
  [[nodiscard]] virtual Status read_snapshot(std::uint64_t& sequence, Digest128& chain,
                                             std::vector<std::uint8_t>& payload, bool& present) = 0;

  [[nodiscard]] virtual std::uint64_t bytes() const = 0;
  [[nodiscard]] virtual std::uint64_t records() const = 0;
  [[nodiscard]] virtual bool torn_tail() const = 0;
  [[nodiscard]] virtual std::string describe() const = 0;
  [[nodiscard]] virtual Status close() = 0;
};

/// Bounded in-memory journal used by unit, property and adversarial tests.
class InMemoryJournal final : public Journal {
 public:
  explicit InMemoryJournal(std::size_t max_bytes = (64u << 20)) : max_bytes_(max_bytes) {}

  [[nodiscard]] Status open() override;
  [[nodiscard]] Status append(std::uint64_t sequence, JournalRecordKind kind, Digest128 chain,
                              const std::vector<std::uint8_t>& payload) override;
  [[nodiscard]] Status begin_replay() override;
  [[nodiscard]] Status next_record(JournalRecord& out) override;
  [[nodiscard]] Status end_replay() override;
  [[nodiscard]] Status rotate(std::uint64_t snapshot_sequence) override;
  [[nodiscard]] Status write_snapshot(std::uint64_t sequence, Digest128 chain,
                                      const std::vector<std::uint8_t>& payload) override;
  [[nodiscard]] Status read_snapshot(std::uint64_t& sequence, Digest128& chain,
                                     std::vector<std::uint8_t>& payload, bool& present) override;
  [[nodiscard]] std::uint64_t bytes() const override { return bytes_; }
  [[nodiscard]] std::uint64_t records() const override { return static_cast<std::uint64_t>(entries_.size()); }
  [[nodiscard]] bool torn_tail() const override { return torn_; }
  [[nodiscard]] std::string describe() const override { return "in-memory"; }
  [[nodiscard]] Status close() override;

  /// Test hooks: simulate a torn write and simulated media damage.
  void simulate_torn_tail();
  void simulate_corruption(std::size_t index);
  void set_max_bytes(std::size_t max_bytes) { max_bytes_ = max_bytes; }

  struct Entry {
    std::uint64_t sequence{0};
    JournalRecordKind kind{JournalRecordKind::Attempt};
    Digest128 chain{};
    std::vector<std::uint8_t> payload{};
    bool corrupted{false};
  };

  [[nodiscard]] const std::vector<Entry>& entries() const { return entries_; }

 private:
  std::vector<Entry> entries_;
  std::size_t cursor_{0};
  std::size_t max_bytes_{(64u << 20)};
  std::uint64_t bytes_{0};
  bool replaying_{false};
  bool torn_{false};
  bool has_snapshot_{false};
  std::uint64_t snapshot_sequence_{0};
  Digest128 snapshot_chain_{};
  std::vector<std::uint8_t> snapshot_payload_{};
};

}  // namespace creditfabric

#endif  // CREDITFABRIC_JOURNAL_HPP
