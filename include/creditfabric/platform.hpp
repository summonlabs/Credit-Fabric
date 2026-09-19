// Credit Fabric - small platform surface (durable flush, atomic replace, PIDs).
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#ifndef CREDITFABRIC_PLATFORM_HPP
#define CREDITFABRIC_PLATFORM_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "creditfabric/status.hpp"

namespace creditfabric {

/// Flushes a stdio stream to the storage device (not just to the OS cache).
[[nodiscard]] Status durable_flush(void* file_handle);

/// Atomically replaces \p destination with \p source.
[[nodiscard]] Status atomic_replace_file(const std::string& source, const std::string& destination);

[[nodiscard]] bool file_exists(const std::string& path);
[[nodiscard]] bool directory_exists(const std::string& path);
[[nodiscard]] Status remove_file(const std::string& path);
[[nodiscard]] Status file_size(const std::string& path, std::uint64_t& out);
[[nodiscard]] Status ensure_parent_directory(const std::string& path);

/// Monotonic millisecond clock. Used for reporting only: no authoritative
/// decision in Credit Fabric is derived from wall-clock time.
[[nodiscard]] std::uint64_t monotonic_millis() noexcept;

/// Boot identity for this process: random, never derived from time alone.
[[nodiscard]] std::uint64_t make_boot_id() noexcept;

[[nodiscard]] Status initialize_sockets();
void shutdown_sockets();

}  // namespace creditfabric

#endif  // CREDITFABRIC_PLATFORM_HPP
