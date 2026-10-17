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

// TEMPORARY DEBUG (branch debug/time-reply-latency). Not for merging.

#include "debug_time_reply.h"

#include "clock.h"
#include <time.h>

#include <algorithm>
#include <array>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <vector>

namespace sendspin::server {

namespace {

constexpr size_t SEGMENTS = 16;
constexpr std::array<const char*, SEGMENTS> NAMES = {
    "stamp->built     (json)",
    "built->encrypted (noise)",
    "encrypted->batched (copy)",
    "batched->posted  (on_writable rest)",
    "posted->net_start (thread hop)",
    "net_start->issue (frames ahead)",
    "issue->issued    (send syscall)",
    "issued->done     (completion)",
    "TOTAL stamp->issued",
    "d: stamp->sched  (kernel qdisc)",
    "d: stamp->sw tx  (driver)",
    "d: stamp->hw tx  (NIC)",
    "r: rx sw->app rx (kernel->net thread)",
    "r: rx hw->app rx (NIC->net thread)",
    "offset err (r-d)/2, sw, x10",
    "offset err (r-d)/2, hw, x10",
};
constexpr int64_t NONE = INT64_MIN;

struct Stats {
    std::mutex mutex;
    std::array<std::vector<int64_t>, SEGMENTS> samples;
    size_t count{0};
    size_t with_ahead{0};
    size_t rx_buffered{0};
    size_t key_mismatch{0};
    int max_unsent{-1};
};

Stats& stats() {
    static Stats s;
    return s;
}

size_t env_size(const char* name, size_t fallback) {
    const char* v = std::getenv(name);
    return v != nullptr ? static_cast<size_t>(std::strtoul(v, nullptr, 10)) : fallback;
}

int64_t pct(std::vector<int64_t> v, double p) {
    std::sort(v.begin(), v.end());
    return v[std::min(v.size() - 1, static_cast<size_t>(p * static_cast<double>(v.size())))];
}

int64_t since(int64_t from, int64_t to) {
    return (from != 0 && to != 0) ? to - from : NONE;
}

}  // namespace

int64_t debug_realtime_to_now_us(int64_t realtime_ns) {
    timespec real{};
    const int64_t before = now_us();
    clock_gettime(CLOCK_REALTIME, &real);
    const int64_t after = now_us();
    const int64_t real_ns = static_cast<int64_t>(real.tv_sec) * 1000000000 + real.tv_nsec;
    // realtime - now at one instant, the midpoint of the two now_us() reads
    const int64_t offset_ns = real_ns - (before + after) * 500;
    return (realtime_ns - offset_ns) / 1000;
}

int64_t debug_best_tx_us(const TimeReplyTrace& t) {
    static const bool protocol_hw = env_size("SENDSPIN_DEBUG_PROTOCOL_HW", 0) != 0;
    if (protocol_hw && t.hw_tx_us != 0) {
        return t.hw_tx_us;
    }
    if (t.sw_tx_us != 0) {
        return t.sw_tx_us;
    }
    if (t.sched_us != 0) {
        return t.sched_us;
    }
    return t.issued_us;
}

void debug_time_reply_record(const TimeReplyTrace& t) {
    const int64_t d_sw = since(t.stamp_us, t.sw_tx_us);
    const int64_t d_hw = since(t.stamp_us, t.hw_tx_us);
    const int64_t r_sw = since(t.rx_sw_us, t.rx_app_us);
    const int64_t r_hw = since(t.rx_hw_us, t.rx_app_us);
    const std::array<int64_t, SEGMENTS> d = {
        t.built_us - t.stamp_us,
        t.encrypted_us - t.built_us,
        t.batched_us - t.encrypted_us,
        t.posted_us - t.batched_us,
        t.net_start_us - t.posted_us,
        t.issue_us - t.net_start_us,
        t.issued_us - t.issue_us,
        t.done_us - t.issued_us,
        t.issued_us - t.stamp_us,
        since(t.stamp_us, t.sched_us),
        d_sw,
        d_hw,
        r_sw,
        r_hw,
        (d_sw != NONE && r_sw != NONE) ? (r_sw - d_sw) * 5 : NONE,
        (d_hw != NONE && r_hw != NONE) ? (r_hw - d_hw) * 5 : NONE,
    };
    static const size_t every = env_size("SENDSPIN_DEBUG_TIME_EVERY", 200);
    static const bool each = env_size("SENDSPIN_DEBUG_TIME_EACH", 0) != 0;
    Stats& s = stats();
    std::lock_guard lock(s.mutex);
    if (each) {
        std::fprintf(stderr, "[time-reply]");
        for (size_t i = 0; i < SEGMENTS; ++i) {
            if (d[i] != NONE) {
                std::fprintf(stderr, " s%zu=%lld", i, (long long)d[i]);
            }
        }
        std::fprintf(stderr, " frames_ahead=%zu rx_buffered=%d key_mismatch=%d\n", t.frame_index,
                     t.rx_buffered ? 1 : 0, t.key_mismatch ? 1 : 0);
    }
    for (size_t i = 0; i < SEGMENTS; ++i) {
        if (d[i] != NONE) {
            s.samples[i].push_back(d[i]);
        }
    }
    ++s.count;
    s.with_ahead += t.frame_index > 0 ? 1 : 0;
    s.rx_buffered += t.rx_buffered ? 1 : 0;
    s.key_mismatch += t.key_mismatch ? 1 : 0;
    s.max_unsent = std::max(s.max_unsent, t.unsent_bytes);
    if (s.count < every) {
        return;
    }
    std::fprintf(stderr,
                 "[time-reply] %zu replies, %zu behind other frames, %zu requests buffered, %zu "
                 "key mismatches, max unsent %d B (us; offset err in 0.1 us)\n",
                 s.count, s.with_ahead, s.rx_buffered, s.key_mismatch, s.max_unsent);
    std::fprintf(stderr, "  %-38s %6s %7s %7s %7s %7s %7s\n", "segment", "n", "min", "p50", "p90",
                 "p99", "max");
    for (size_t i = 0; i < SEGMENTS; ++i) {
        const auto& v = s.samples[i];
        if (v.empty()) {
            std::fprintf(stderr, "  %-38s %6s\n", NAMES[i], "-");
            continue;
        }
        std::fprintf(stderr, "  %-38s %6zu %7lld %7lld %7lld %7lld %7lld\n", NAMES[i], v.size(),
                     (long long)pct(v, 0.0), (long long)pct(v, 0.5), (long long)pct(v, 0.9),
                     (long long)pct(v, 0.99), (long long)*std::max_element(v.begin(), v.end()));
        s.samples[i].clear();
    }
    s.count = s.with_ahead = s.rx_buffered = s.key_mismatch = 0;
    s.max_unsent = -1;
}

}  // namespace sendspin::server
