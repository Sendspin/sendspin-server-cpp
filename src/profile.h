// Copyright 2026 Sendspin Contributors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

/// @file profile.h
/// @brief The build's resource profile, the one place a build setting changes the library's limits
/// (conventions.md "Platform differences stay in their files"). SENDSPIN_SERVER_EMBEDDED_PROFILE,
/// which cmake/esp_idf.cmake sets and the host's CMake option of the same name sets for a test run,
/// selects the embedded profile: one group of up to about eight players on an ESP32-class device
/// with PSRAM. Every other source reads the values below and carries no conditional of its own.

#pragma once

#include "memory.h"

#include <cstddef>
#include <cstdint>

namespace sendspin::server::profile {

#if defined(SENDSPIN_SERVER_EMBEDDED_PROFILE) && SENDSPIN_SERVER_EMBEDDED_PROFILE
constexpr bool EMBEDDED = true;
#else
constexpr bool EMBEDDED = false;
#endif

/// @brief The most input a buffered input is written ahead of its group's lead
/// (sched::timing_defaults::WRITE_AHEAD_MAX_US)
constexpr int64_t WRITE_AHEAD_MAX_US = EMBEDDED ? 2000000 : 10000000;
/// @brief The staging ring's room past the write-ahead: the group lead and the history the
/// pipelines read behind the playhead
constexpr int64_t STAGING_LEAD_ALLOWANCE_US = EMBEDDED ? 1000000 : 2000000;
/// @brief The staging ring holds this much input, so admission never passes it
constexpr int64_t STAGING_RING_US = WRITE_AHEAD_MAX_US + STAGING_LEAD_ALLOWANCE_US;
static_assert(STAGING_RING_US > WRITE_AHEAD_MAX_US, "admission must stay within the staging ring");
/// @brief ...or this many bytes, whichever is less (in PSRAM on the embedded profile)
constexpr size_t STAGING_RING_MAX_BYTES = (EMBEDDED ? size_t{2} : size_t{64}) * 1024 * 1024;

/// @brief Accepted connections in their WebSocket upgrade or handshake
/// (connection_limits::MAX_ACCEPTED_HANDSHAKES); lwIP's socket table is small on the embedded
/// profile
constexpr size_t MAX_ACCEPTED_HANDSHAKES = EMBEDDED ? 8 : 64;
/// @brief Dials and dialed connections in their handshake (connection_limits::MAX_DIALING)
constexpr size_t MAX_DIALING = EMBEDDED ? 4 : 32;
/// @brief Activated accepted connections (connection_limits::MAX_ACCEPTED_CLIENTS): the embedded
/// profile's players
constexpr size_t MAX_ACCEPTED_CLIENTS = EMBEDDED ? 8 : 64;
/// @brief Activated accepted connections that may not play
/// (connection_limits::MAX_ACCEPTED_UNAPPROVED)
constexpr size_t MAX_ACCEPTED_UNAPPROVED = EMBEDDED ? 4 : 16;
static_assert(MAX_ACCEPTED_UNAPPROVED < MAX_ACCEPTED_CLIENTS,
              "unapproved clients must leave room for clients that play");

/// @brief Internal RAM a new connection leaves free (connection_limits::heap_admits()); none on
/// the host profile, and a host reports no internal heap whatever the profile
constexpr size_t INTERNAL_RAM_RESERVE_BYTES = EMBEDDED ? size_t{64} * 1024 : 0;
/// @brief Internal RAM counted for each connection opening, the new one included: a dial's measured
/// cost with room for a client without task_caps
constexpr size_t INTERNAL_RAM_PER_CONNECTION_BYTES = EMBEDDED ? size_t{10} * 1024 : 0;
/// @brief The largest free internal block a new connection needs: a dial's receive stack where the
/// client takes no task_caps
constexpr size_t INTERNAL_RAM_MIN_BLOCK_BYTES = EMBEDDED ? size_t{4} * 1024 : 0;

/// @brief The inbound ring's payload storage, shared by a run's connections (InboundRing): at least
/// one largest message (MAX_MESSAGE_BYTES), which then fits once the ring is empty; the host holds
/// four
constexpr size_t INBOUND_RING_BYTES = (EMBEDDED ? size_t{1} : size_t{4}) * 64 * 1024;
/// @brief The most received messages the inbound ring holds: a burst of time requests from each of
/// eight players with their other messages, and on the host many more connections
constexpr size_t INBOUND_RING_MESSAGES = EMBEDDED ? 128 : 1024;

/// @brief Default cap on a reassembled inbound message (SessionSettings::max_reassembled_bytes); on
/// the embedded profile, room for a source's chunk of 150 ms of 96 kHz stereo 32-bit PCM
constexpr size_t DEFAULT_MAX_REASSEMBLED_BYTES = (EMBEDDED ? size_t{128} : size_t{1024}) * 1024;

/// @brief A routed source's feed to its decoder (SourceFeed): two of the largest chunks the server
/// takes, default-sized reassembled messages
constexpr size_t SOURCE_FEED_BYTES = 2 * DEFAULT_MAX_REASSEMBLED_BYTES;
/// @brief The most items a source's feed holds: about a second of 20 ms chunks on the embedded
/// profile, five on the host
constexpr size_t SOURCE_FEED_ITEMS = EMBEDDED ? 64 : 256;

/// @brief FLAC with linear prediction (LPC); the embedded profile uses the fixed predictors only,
/// bounding the encode cost on one core
constexpr bool FLAC_LPC = !EMBEDDED;

/// @brief FLAC's largest Rice partition order: none on the embedded profile, whose 20 ms blocks
/// gain little from partitions for the time the search costs
constexpr uint8_t FLAC_RICE_PARTITION_ORDER = EMBEDDED ? 0 : 6;

/// @brief The resampler's arithmetic: on the embedded profile Q15 coefficients over 16-bit samples
/// in and out, which micro-resampler runs on a target's integer SIMD; FLOAT32 coefficients over
/// floats on a host
constexpr bool RESAMPLER_Q15 = EMBEDDED;

/// @brief How a routed source's clock is corrected against the server's (LiveSourceSink), and a
/// live input played fast to lower its latency (LiveSlew): with micro-resampler's ASRC on a host;
/// on the embedded profile by dropping or repeating single frames, which needs no filter and
/// stands in for the ASRC until its cost on the ESP32-P4 is measured.
constexpr bool SOURCE_ASRC = !EMBEDDED;

/// @brief Group audio threads: the embedded profile runs one, on the second core; 0 is one per
/// hardware thread (AudioWorkers::default_threads())
constexpr size_t AUDIO_WORKERS = EMBEDDED ? 1 : 0;

#if defined(SENDSPIN_SERVER_MEMORY_STATS) && SENDSPIN_SERVER_MEMORY_STATS
/// @brief The heaps and the tasks' stacks are logged at the server's start (on the thread calling
/// start()), and on the core thread at each connection's start and end and a group's playback;
/// CONFIG_SENDSPIN_SERVER_MEMORY_STATS on ESP-IDF, the CMake option of the same name on a host
constexpr bool MEMORY_STATS = true;
#else
constexpr bool MEMORY_STATS = false;
#endif

/// @brief Where each library task's stack lives on ESP-IDF (esp/thread_setup_esp.cpp): PSRAM, as
/// the build allows it, for a task that never writes flash; a host ignores these
constexpr MemoryLocation CORE_STACK = MemoryLocation::PREFER_EXTERNAL;
constexpr MemoryLocation AUDIO_WORKER_STACK = MemoryLocation::PREFER_EXTERNAL;
constexpr MemoryLocation ARTWORK_STACK = MemoryLocation::PREFER_EXTERNAL;
/// @brief Each connection's writer task (ss_ws_tx)
constexpr MemoryLocation LINK_WRITER_STACK = MemoryLocation::PREFER_EXTERNAL;
/// @brief esp_http_server's task, which receives for every accepted connection
constexpr MemoryLocation LISTENER_STACK = MemoryLocation::PREFER_EXTERNAL;
/// @brief Each dialed connection's reader task (ss_ws_rx)
constexpr MemoryLocation DIAL_RECEIVE_STACK = MemoryLocation::PREFER_EXTERNAL;
/// @brief Internal RAM: the PersistenceProvider's calls run on the dispatch thread and may write
/// flash, which disables the cache PSRAM is read through
constexpr MemoryLocation DISPATCH_STACK = MemoryLocation::PREFER_INTERNAL;

}  // namespace sendspin::server::profile
