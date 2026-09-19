// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "creditfabric/file_journal.hpp"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "creditfabric/crc32c.hpp"
#include "creditfabric/platform.hpp"

#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

namespace creditfabric {
namespace {

constexpr std::uint32_t kJournalMagic = 0x314A4643u;   // "CFJ1" in file order
constexpr std::uint32_t kSnapshotMagic = 0x31534643u;  // "CFS1" in file order
constexpr std::size_t kHeaderBytes = 12u;
constexpr std::size_t kRecordOverhead = 8u;   // length + crc
constexpr std::size_t kEnvelopeBytes = 25u;   // sequence + kind + chain
constexpr std::uint32_t kSnapshotPayloadLimit = 64u << 20;

void put_u32(std::uint8_t* destination, std::uint32_t value) { std::memcpy(destination, &value, 4); }
void put_u64(std::uint8_t* destination, std::uint64_t value) { std::memcpy(destination, &value, 8); }

std::uint32_t get_u32(const std::uint8_t* source) {
  std::uint32_t value = 0;
  std::memcpy(&value, source, 4);
  return value;
}

std::uint64_t get_u64(const std::uint8_t* source) {
  std::uint64_t value = 0;
  std::memcpy(&value, source, 8);
  return value;
}

void encode_envelope(std::vector<std::uint8_t>& out, std::uint64_t sequence, JournalRecordKind kind,
                     Digest128 chain, const std::vector<std::uint8_t>& payload) {
  out.resize(kEnvelopeBytes);
  put_u64(out.data(), sequence);
  out[8] = static_cast<std::uint8_t>(kind);
  put_u64(out.data() + 9, chain.lo);
  put_u64(out.data() + 17, chain.hi);
  out.insert(out.end(), payload.begin(), payload.end());
}

Status truncate_stream(std::FILE* stream, std::uint64_t size) {
  if (std::fflush(stream) != 0) return Status::refused(Reason::IoError, 1);
#if defined(_WIN32)
  const int descriptor = ::_fileno(stream);
  if (descriptor < 0) return Status::refused(Reason::IoError, 2);
  if (::_chsize_s(descriptor, static_cast<long long>(size)) != 0) return Status::refused(Reason::IoError, 3);
#else
  const int descriptor = ::fileno(stream);
  if (descriptor < 0) return Status::refused(Reason::IoError, 2);
  if (::ftruncate(descriptor, static_cast<off_t>(size)) != 0) return Status::refused(Reason::IoError, 3);
#endif
  return Status::success();
}

}  // namespace

FileJournal::FileJournal(FileJournalOptions options) : options_(std::move(options)) {
  journal_path_ = options_.prefix + ".jrnl";
  snapshot_path_ = options_.prefix + ".snap";
}

FileJournal::~FileJournal() { (void)close(); }

Status FileJournal::open() {
  Status status = ensure_parent_directory(journal_path_);
  if (!status.ok()) return status;

  const bool existed = file_exists(journal_path_);
  std::FILE* stream = std::fopen(journal_path_.c_str(), existed ? "r+b" : "w+b");
  if (stream == nullptr) return Status::refused(Reason::IoError, 10);

  std::uint8_t header[kHeaderBytes] = {};
  if (!existed) {
    put_u32(header, kJournalMagic);
    header[4] = static_cast<std::uint8_t>(kFormatVersion);
    put_u32(header + 8, crc32c(header, 8));
    if (std::fwrite(header, 1, sizeof(header), stream) != sizeof(header)) {
      std::fclose(stream);
      return Status::refused(Reason::IoError, 11);
    }
    status = durable_flush(stream);
    if (!status.ok()) {
      std::fclose(stream);
      return status;
    }
  } else {
    if (std::fread(header, 1, sizeof(header), stream) != sizeof(header)) {
      std::fclose(stream);
      return Status::refused(Reason::JournalCorrupt, 12);
    }
    if (get_u32(header) != kJournalMagic) {
      std::fclose(stream);
      return Status::refused(Reason::JournalCorrupt, 13);
    }
    if (header[4] != static_cast<std::uint8_t>(kFormatVersion)) {
      std::fclose(stream);
      return Status::refused(Reason::JournalVersionUnsupported, header[4]);
    }
    if (get_u32(header + 8) != crc32c(header, 8)) {
      std::fclose(stream);
      return Status::refused(Reason::JournalCorrupt, 14);
    }
  }

  stream_ = stream;
  open_ = true;
  replaying_ = false;
  torn_ = false;
  truncated_bytes_ = 0;
  if (std::fseek(stream, 0, SEEK_END) != 0) return Status::refused(Reason::IoError, 15);
  const long size = std::ftell(stream);
  if (size < 0) return Status::refused(Reason::IoError, 16);
  write_offset_ = static_cast<std::uint64_t>(size);
  scan_offset_ = kHeaderBytes;
  bytes_ = write_offset_;
  records_ = 0;
  return Status::success();
}

Status FileJournal::open_append_stream() { return Status::success(); }

Status FileJournal::rewrite_journal_header() {
  auto* stream = static_cast<std::FILE*>(stream_);
  if (stream == nullptr) return Status::refused(Reason::InvalidState, 20);
  std::uint8_t header[kHeaderBytes] = {};
  put_u32(header, kJournalMagic);
  header[4] = static_cast<std::uint8_t>(kFormatVersion);
  put_u32(header + 8, crc32c(header, 8));
  if (std::fseek(stream, 0, SEEK_SET) != 0) return Status::refused(Reason::IoError, 21);
  if (std::fwrite(header, 1, sizeof(header), stream) != sizeof(header)) {
    return Status::refused(Reason::IoError, 22);
  }
  return durable_flush(stream);
}

Status FileJournal::begin_replay() {
  if (!open_) return Status::refused(Reason::InvalidState, 30);
  if (replaying_) return Status::refused(Reason::InvalidState, 31);
  scan_offset_ = kHeaderBytes;
  records_ = 0;
  torn_ = false;
  replaying_ = true;
  return Status::success();
}

Status FileJournal::next_record(JournalRecord& out) {
  out = JournalRecord{};
  if (!open_) return Status::refused(Reason::InvalidState, 32);
  auto* stream = static_cast<std::FILE*>(stream_);
  if (stream == nullptr) return Status::refused(Reason::InvalidState, 33);
  if (scan_offset_ + kRecordOverhead > write_offset_) {
    if (scan_offset_ != write_offset_) {
      torn_ = true;
      truncated_bytes_ = write_offset_ - scan_offset_;
    }
    return Status::success();
  }

  std::uint8_t prefix[kRecordOverhead] = {};
  if (std::fseek(stream, static_cast<long>(scan_offset_), SEEK_SET) != 0) {
    return Status::refused(Reason::IoError, 34);
  }
  const std::size_t prefix_read = std::fread(prefix, 1, sizeof(prefix), stream);
  if (prefix_read < sizeof(prefix)) {
    torn_ = true;
    truncated_bytes_ = write_offset_ - scan_offset_;
    return Status::success();
  }

  const std::uint32_t length = get_u32(prefix);
  const std::uint32_t expected_crc = get_u32(prefix + 4);
  if (length > options_.max_record_bytes || length < kEnvelopeBytes ||
      scan_offset_ + kRecordOverhead + length > write_offset_) {
    torn_ = true;
    truncated_bytes_ = write_offset_ - scan_offset_;
    return Status::success();
  }

  std::vector<std::uint8_t> envelope(length);
  if (std::fread(envelope.data(), 1, length, stream) != length) {
    torn_ = true;
    truncated_bytes_ = write_offset_ - scan_offset_;
    return Status::success();
  }
  if (crc32c(envelope.data(), envelope.size()) != expected_crc) {
    torn_ = true;
    truncated_bytes_ = write_offset_ - scan_offset_;
    return Status::success();
  }

  const auto kind = static_cast<JournalRecordKind>(envelope[8]);
  if (kind != JournalRecordKind::Attempt && kind != JournalRecordKind::Snapshot && kind != JournalRecordKind::Intent) {
    torn_ = true;
    truncated_bytes_ = write_offset_ - scan_offset_;
    return Status::success();
  }

  out.valid = true;
  out.offset = scan_offset_;
  out.sequence = get_u64(envelope.data());
  out.kind = kind;
  out.chain = Digest128{get_u64(envelope.data() + 9), get_u64(envelope.data() + 17)};
  out.payload.assign(envelope.begin() + static_cast<std::ptrdiff_t>(kEnvelopeBytes), envelope.end());
  scan_offset_ += kRecordOverhead + length;
  ++records_;
  return Status::success();
}

Status FileJournal::end_replay() {
  if (!open_) return Status::refused(Reason::InvalidState, 40);
  if (!replaying_) return Status::refused(Reason::InvalidState, 42);
  replaying_ = false;
  auto* stream = static_cast<std::FILE*>(stream_);
  if (stream == nullptr) return Status::refused(Reason::InvalidState, 41);
  if (torn_) {
    if (!options_.truncate_torn_tail) return Status::refused(Reason::JournalTorn, truncated_bytes_);
    const Status status = truncate_stream(stream, scan_offset_);
    if (!status.ok()) return status;
    write_offset_ = scan_offset_;
    bytes_ = write_offset_;
  }
  return Status::success();
}

Status FileJournal::append(std::uint64_t sequence, JournalRecordKind kind, Digest128 chain,
                           const std::vector<std::uint8_t>& payload) {
  if (!open_) return Status::refused(Reason::InvalidState, 50);
  if (replaying_) return Status::refused(Reason::InvalidState, 51);
  if (torn_) return Status::refused(Reason::JournalTorn, truncated_bytes_);
  if (payload.size() + kEnvelopeBytes > options_.max_record_bytes) {
    return Status::refused(Reason::OversizedFrame, payload.size());
  }
  if (bytes_ + payload.size() + kEnvelopeBytes + kRecordOverhead > options_.max_bytes) {
    return Status::refused(Reason::BudgetExceeded, bytes_);
  }
  auto* stream = static_cast<std::FILE*>(stream_);
  if (stream == nullptr) return Status::refused(Reason::InvalidState, 52);
  if (std::fseek(stream, static_cast<long>(write_offset_), SEEK_SET) != 0) {
    return Status::refused(Reason::IoError, 53);
  }
  std::vector<std::uint8_t> envelope;
  encode_envelope(envelope, sequence, kind, chain, payload);
  std::uint8_t prefix[kRecordOverhead] = {};
  put_u32(prefix, static_cast<std::uint32_t>(envelope.size()));
  put_u32(prefix + 4, crc32c(envelope.data(), envelope.size()));
  if (std::fwrite(prefix, 1, sizeof(prefix), stream) != sizeof(prefix)) {
    return Status::refused(Reason::IoError, 54);
  }
  if (std::fwrite(envelope.data(), 1, envelope.size(), stream) != envelope.size()) {
    return Status::refused(Reason::IoError, 55);
  }
  const Status status = durable_flush(stream);
  if (!status.ok()) return status;
  write_offset_ += kRecordOverhead + envelope.size();
  bytes_ = write_offset_;
  ++records_;
  return Status::success();
}

Status FileJournal::rotate(std::uint64_t snapshot_sequence) {
  (void)snapshot_sequence;
  if (!open_) return Status::refused(Reason::InvalidState, 60);
  auto* stream = static_cast<std::FILE*>(stream_);
  if (stream == nullptr) return Status::refused(Reason::InvalidState, 61);
  Status status = rewrite_journal_header();
  if (!status.ok()) return status;
  status = truncate_stream(stream, kHeaderBytes);
  if (!status.ok()) return status;
  status = durable_flush(stream);
  if (!status.ok()) return status;
  write_offset_ = kHeaderBytes;
  scan_offset_ = kHeaderBytes;
  bytes_ = kHeaderBytes;
  records_ = 0;
  torn_ = false;
  truncated_bytes_ = 0;
  return Status::success();
}

Status FileJournal::write_snapshot(std::uint64_t sequence, Digest128 chain,
                                   const std::vector<std::uint8_t>& payload) {
  if (payload.size() > kSnapshotPayloadLimit) return Status::refused(Reason::OversizedFrame, payload.size());
  const std::string temporary = snapshot_path_ + ".tmp";
  std::FILE* stream = std::fopen(temporary.c_str(), "wb");
  if (stream == nullptr) return Status::refused(Reason::IoError, 70);

  std::uint8_t header[kHeaderBytes + 16] = {};
  put_u32(header, kSnapshotMagic);
  header[4] = static_cast<std::uint8_t>(kFormatVersion);
  put_u32(header + 8, crc32c(header, 8));
  put_u64(header + 12, sequence);
  put_u64(header + 20, chain.lo);
  std::uint8_t trailer[16] = {};
  put_u64(trailer, chain.hi);
  put_u32(trailer + 8, static_cast<std::uint32_t>(payload.size()));
  put_u32(trailer + 12, crc32c(payload.data(), payload.size()));

  bool ok = std::fwrite(header, 1, sizeof(header), stream) == sizeof(header);
  ok = ok && (payload.empty() || std::fwrite(payload.data(), 1, payload.size(), stream) == payload.size());
  ok = ok && std::fwrite(trailer, 1, sizeof(trailer), stream) == sizeof(trailer);
  if (!ok) {
    std::fclose(stream);
    (void)remove_file(temporary);
    return Status::refused(Reason::IoError, 71);
  }
  const Status flushed = durable_flush(stream);
  std::fclose(stream);
  if (!flushed.ok()) {
    (void)remove_file(temporary);
    return flushed;
  }
  const Status replaced = atomic_replace_file(temporary, snapshot_path_);
  if (!replaced.ok()) {
    (void)remove_file(temporary);
    return replaced;
  }
  return Status::success();
}

Status FileJournal::read_snapshot(std::uint64_t& sequence, Digest128& chain, std::vector<std::uint8_t>& payload,
                                  bool& present) {
  present = false;
  if (!file_exists(snapshot_path_)) return Status::success();
  std::FILE* stream = std::fopen(snapshot_path_.c_str(), "rb");
  if (stream == nullptr) return Status::refused(Reason::IoError, 80);
  std::uint8_t header[kHeaderBytes + 16] = {};
  if (std::fread(header, 1, sizeof(header), stream) != sizeof(header)) {
    std::fclose(stream);
    return Status::refused(Reason::JournalCorrupt, 81);
  }
  if (get_u32(header) != kSnapshotMagic) {
    std::fclose(stream);
    return Status::refused(Reason::JournalCorrupt, 82);
  }
  if (header[4] != static_cast<std::uint8_t>(kFormatVersion)) {
    std::fclose(stream);
    return Status::refused(Reason::JournalVersionUnsupported, header[4]);
  }
  if (get_u32(header + 8) != crc32c(header, 8)) {
    std::fclose(stream);
    return Status::refused(Reason::JournalCorrupt, 83);
  }
  sequence = get_u64(header + 12);
  chain.lo = get_u64(header + 20);

  // The CRC block lives at the end of the file, after the payload.
  std::uint8_t trailer[16] = {};
  if (std::fseek(stream, -static_cast<long>(sizeof(trailer)), SEEK_END) != 0) {
    std::fclose(stream);
    return Status::refused(Reason::JournalCorrupt, 84);
  }
  if (std::fread(trailer, 1, sizeof(trailer), stream) != sizeof(trailer)) {
    std::fclose(stream);
    return Status::refused(Reason::JournalCorrupt, 84);
  }
  chain.hi = get_u64(trailer);
  const std::uint32_t length = get_u32(trailer + 8);
  const std::uint32_t expected_crc = get_u32(trailer + 12);
  if (length > kSnapshotPayloadLimit) {
    std::fclose(stream);
    return Status::refused(Reason::JournalCorrupt, 85);
  }
  payload.resize(length);
  if (std::fseek(stream, static_cast<long>(sizeof(header)), SEEK_SET) != 0) {
    std::fclose(stream);
    return Status::refused(Reason::JournalCorrupt, 86);
  }
  if (length > 0 && std::fread(payload.data(), 1, length, stream) != length) {
    std::fclose(stream);
    return Status::refused(Reason::JournalCorrupt, 86);
  }
  std::fclose(stream);
  if (crc32c(payload.data(), payload.size()) != expected_crc) {
    return Status::refused(Reason::JournalCorrupt, 87);
  }
  present = true;
  return Status::success();
}

std::string FileJournal::describe() const {
  std::string text = journal_path_;
  text += " (bytes=";
  text += std::to_string(bytes_);
  text += " records=";
  text += std::to_string(records_);
  text += torn_ ? " torn=yes" : " torn=no";
  text += ")";
  return text;
}

Status FileJournal::close() {
  if (stream_ != nullptr) {
    auto* stream = static_cast<std::FILE*>(stream_);
    (void)durable_flush(stream);
    std::fclose(stream);
    stream_ = nullptr;
  }
  open_ = false;
  replaying_ = false;
  return Status::success();
}

}  // namespace creditfabric
