// Credit Fabric - crash-safe file journal with snapshot rotation.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Layout (two files under one prefix):
//   <prefix>.jrnl : append-only, CRC-32C protected record log
//   <prefix>.snap : single digest-protected snapshot, replaced atomically
//
// Recovery never guesses. A record whose length or CRC does not validate ends
// the replay: everything before it is authoritative, everything from it onward
// is reported as torn and (by default) truncated on the next append so durable
// growth cannot resume on top of damage. Only a Snapshot record may legally
// appear inside the journal log; a second one means the rotation was
// interrupted and is handled by sequence comparison.

#ifndef CREDITFABRIC_FILE_JOURNAL_HPP
#define CREDITFABRIC_FILE_JOURNAL_HPP

#include <cstdint>
#include <string>

#include "creditfabric/journal.hpp"

namespace creditfabric {

struct FileJournalOptions {
  std::string prefix{"creditfabric"};
  std::size_t max_record_bytes{1u << 20};
  std::uint64_t max_bytes{64ull << 20};
  std::size_t records_per_snapshot{4096};
  bool truncate_torn_tail{true};
};

class FileJournal final : public Journal {
 public:
  explicit FileJournal(FileJournalOptions options);
  ~FileJournal() override;

  FileJournal(const FileJournal&) = delete;
  FileJournal& operator=(const FileJournal&) = delete;

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
  [[nodiscard]] std::uint64_t records() const override { return records_; }
  [[nodiscard]] bool torn_tail() const override { return torn_; }
  [[nodiscard]] std::string describe() const override;
  [[nodiscard]] Status close() override;

  [[nodiscard]] const std::string& journal_path() const noexcept { return journal_path_; }
  [[nodiscard]] const std::string& snapshot_path() const noexcept { return snapshot_path_; }
  [[nodiscard]] std::uint64_t truncated_bytes() const noexcept { return truncated_bytes_; }

 private:
  [[nodiscard]] Status open_append_stream();
  [[nodiscard]] Status rewrite_journal_header();

  FileJournalOptions options_;
  std::string journal_path_;
  std::string snapshot_path_;
  void* stream_{nullptr};
  std::uint64_t write_offset_{0};
  std::uint64_t scan_offset_{0};
  std::uint64_t bytes_{0};
  std::uint64_t records_{0};
  std::uint64_t truncated_bytes_{0};
  bool replaying_{false};
  bool torn_{false};
  bool open_{false};
};

}  // namespace creditfabric

#endif  // CREDITFABRIC_FILE_JOURNAL_HPP
