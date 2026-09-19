// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "creditfabric/platform.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>

#include "creditfabric/strong.hpp"

#if defined(_WIN32)
// winsock2.h must precede windows.h: windows.h pulls in the deprecated winsock.h.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <direct.h>
#include <io.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace creditfabric {

Status durable_flush(void* file_handle) {
  auto* stream = static_cast<std::FILE*>(file_handle);
  if (stream == nullptr) return Status::refused(Reason::InvalidState);
  if (std::fflush(stream) != 0) return Status::refused(Reason::IoError);
#if defined(_WIN32)
  const int descriptor = ::_fileno(stream);
  if (descriptor < 0) return Status::refused(Reason::IoError);
  if (::_commit(descriptor) != 0) return Status::refused(Reason::IoError);
#else
  const int descriptor = ::fileno(stream);
  if (descriptor < 0) return Status::refused(Reason::IoError);
  if (::fsync(descriptor) != 0) return Status::refused(Reason::IoError);
#endif
  return Status::success();
}

Status atomic_replace_file(const std::string& source, const std::string& destination) {
#if defined(_WIN32)
  if (::MoveFileExA(source.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0) {
    return Status::refused(Reason::IoError, static_cast<std::uint64_t>(::GetLastError()));
  }
  return Status::success();
#else
  if (::rename(source.c_str(), destination.c_str()) != 0) return Status::refused(Reason::IoError);
  // Durably record the rename in the containing directory when the platform allows it.
  const std::size_t slash = destination.find_last_of('/');
  const std::string directory = (slash == std::string::npos) ? std::string(".") : destination.substr(0, slash);
  const int dir_fd = ::open(directory.c_str(), O_RDONLY);
  if (dir_fd >= 0) {
    (void)::fsync(dir_fd);
    (void)::close(dir_fd);
  }
  return Status::success();
#endif
}

bool file_exists(const std::string& path) {
  std::FILE* stream = std::fopen(path.c_str(), "rb");
  if (stream == nullptr) return false;
  std::fclose(stream);
  return true;
}

Status remove_file(const std::string& path) {
  if (!file_exists(path)) return Status::success();
  if (std::remove(path.c_str()) != 0) return Status::refused(Reason::IoError);
  return Status::success();
}

Status file_size(const std::string& path, std::uint64_t& out) {
  std::FILE* stream = std::fopen(path.c_str(), "rb");
  if (stream == nullptr) return Status::refused(Reason::IoError);
  if (std::fseek(stream, 0, SEEK_END) != 0) {
    std::fclose(stream);
    return Status::refused(Reason::IoError);
  }
  const long size = std::ftell(stream);
  std::fclose(stream);
  if (size < 0) return Status::refused(Reason::IoError);
  out = static_cast<std::uint64_t>(size);
  return Status::success();
}

bool directory_exists(const std::string& path) {
#if defined(_WIN32)
  const DWORD attributes = ::GetFileAttributesA(path.c_str());
  return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
#else
  struct stat info {};
  if (::stat(path.c_str(), &info) != 0) return false;
  return S_ISDIR(info.st_mode);
#endif
}

Status ensure_parent_directory(const std::string& path) {
  const std::size_t slash = path.find_last_of("/\\");
  if (slash == std::string::npos || slash == 0) return Status::success();
  const std::string directory = path.substr(0, slash);
  if (directory_exists(directory)) return Status::success();
#if defined(_WIN32)
  const int result = ::_mkdir(directory.c_str());
#else
  const int result = ::mkdir(directory.c_str(), 0755);
#endif
  if (result != 0 && !directory_exists(directory)) return Status::refused(Reason::IoError);
  return Status::success();
}

std::uint64_t monotonic_millis() noexcept {
  const auto now = std::chrono::steady_clock::now().time_since_epoch();
  return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
}

std::uint64_t make_boot_id() noexcept { return mix64(random_u64() ^ monotonic_millis()); }

Status initialize_sockets() {
#if defined(_WIN32)
  static std::atomic<bool> initialized{false};
  if (initialized.load(std::memory_order_acquire)) return Status::success();
  WSADATA data{};
  if (::WSAStartup(MAKEWORD(2, 2), &data) != 0) return Status::refused(Reason::TransportError);
  initialized.store(true, std::memory_order_release);
#endif
  return Status::success();
}

void shutdown_sockets() {
#if defined(_WIN32)
  ::WSACleanup();
#endif
}

}  // namespace creditfabric
