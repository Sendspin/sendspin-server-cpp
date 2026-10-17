// Copyright 2026 Kevin Ahrendt
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

// TEMPORARY DEBUG (branch debug/time-reply-latency): times one server/time from its stamp to the
// kernel's acceptance of its bytes. Not for merging.

#pragma once

#include <cstddef>
#include <cstdint>

namespace sendspin::server {

/// @brief Instants (now_us()) along one server/time's way out
struct TimeReplyTrace {
    int64_t stamp_us{0};      // server_transmitted taken (core)
    int64_t built_us{0};      // JSON built (core)
    int64_t encrypted_us{0};  // Noise encryption done (core)
    int64_t batched_us{0};    // copied into the connection's write batch (core)
    int64_t posted_us{0};     // write posted to the network thread (core)
    int64_t net_start_us{0};  // network thread runs the write (network)
    int64_t issue_us{0};      // async_write of the time reply's frame begins (network)
    int64_t issued_us{0};     // that async_write initiation returned, its send() attempted
    int64_t done_us{0};       // its completion handler runs (network)
    size_t frame_index{0};    // frames ahead of it in the same write
    size_t bytes_before{0};   // their bytes
    int unsent_bytes{-1};     // SO_NWRITE at completion (macOS), else -1
    // Linux SO_TIMESTAMPING, converted to now_us(); 0 when not reported
    int64_t sched_us{0};       // SCM_TSTAMP_SCHED: entered the qdisc
    int64_t sw_tx_us{0};       // SCM_TSTAMP_SND, software: the driver took it
    int64_t hw_tx_us{0};       // SCM_TSTAMP_SND, hardware: the NIC sent it (PHC as system time)
    int64_t request_rx_us{0};  // the client/time's server_received, as the session saw it
    int64_t rx_app_us{0};      // network thread's stamp at the read completion
    int64_t rx_sw_us{0};       // kernel software receive stamp of the read's last segment
    int64_t rx_hw_us{0};       // NIC receive stamp
    bool rx_buffered{false};   // the request came out of Beast's buffer with no read of its own
    bool key_mismatch{false};  // a send stamp's byte key was not the reply's last byte
};

/// @brief A CLOCK_REALTIME instant (a kernel stamp) on now_us()'s clock
int64_t debug_realtime_to_now_us(int64_t realtime_ns);

/// @brief The best send time known: hardware, else software, else sched, else issued_us
int64_t debug_best_tx_us(const TimeReplyTrace& trace);

/// @brief Accumulates a trace and prints a summary to stderr every SENDSPIN_DEBUG_TIME_EVERY
/// replies (default 200); SENDSPIN_DEBUG_TIME_EACH=1 also prints each one
void debug_time_reply_record(const TimeReplyTrace& trace);

}  // namespace sendspin::server
