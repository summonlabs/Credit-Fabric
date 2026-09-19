// Credit Fabric - umbrella header.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Credit Fabric owns credit identity, issuance, accounting, lifecycle,
// exhaustion, return and authority for relationships where credit semantics are
// explicitly supported. It does not own packet scheduling, generic rate
// limiting, buffer allocation, congestion synthesis, backpressure propagation,
// transport design, or vendor physical credit mechanisms.

#ifndef CREDITFABRIC_CREDITFABRIC_HPP
#define CREDITFABRIC_CREDITFABRIC_HPP

#include "creditfabric/authority.hpp"
#include "creditfabric/bench.hpp"
#include "creditfabric/bytes.hpp"
#include "creditfabric/checked.hpp"
#include "creditfabric/coordinator.hpp"
#include "creditfabric/crc32c.hpp"
#include "creditfabric/engine.hpp"
#include "creditfabric/file_journal.hpp"
#include "creditfabric/journal.hpp"
#include "creditfabric/ledger.hpp"
#include "creditfabric/platform.hpp"
#include "creditfabric/policy.hpp"
#include "creditfabric/record.hpp"
#include "creditfabric/status.hpp"
#include "creditfabric/strong.hpp"
#include "creditfabric/transport.hpp"
#include "creditfabric/version.hpp"
#include "creditfabric/wire.hpp"

#endif  // CREDITFABRIC_CREDITFABRIC_HPP
