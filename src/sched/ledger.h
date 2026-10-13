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

/// @file ledger.h
/// @brief A session's byte accounting of one stream role: a value type whose methods take `now`

#pragma once

#include "ring.h"
#include "sched/constants.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <ranges>

namespace sendspin::server::sched {

/// @brief The entries a ledger has room for before it first grows: one per chunk of `n_target`
/// cells over the staging ring's span (the write-ahead and the lead the profile allows), and one
/// more
constexpr size_t LEDGER_ENTRIES =
    static_cast<size_t>(profile::STAGING_RING_US / (TARGET_CHUNK_CELLS * CELL_US)) + 1;

/// @brief The spec's buffer accounting (spec player/v1.md "Player Buffer Accounting",
/// visualizer/v1.md). A player entry counts while `key - output_delay > now`, a visualizer entry
/// while `key > now`; the output delay is applied when evaluated, so a delay update re-times every
/// entry since the last reset, including ones that had stopped counting. Entries are kept until
/// `key <= now`, past which no output delay can make them count again.
///
/// It is never evaluated while a write is in flight, which keeps a chunk whose transmission is
/// unfinished counted in full.
class Ledger {
public:
    /// @param capacity the role's `buffer_capacity`
    /// @param player false for a visualizer, which ignores the output delay
    Ledger(int64_t capacity, bool player)
        : entries_(LEDGER_ENTRIES, Ring<Entry>::UNLIMITED, MemoryLocation::PREFER_EXTERNAL),
          capacity_(capacity),
          player_(player) {}

    /// @brief On a stream/clear or stream/end for the role: the count becomes 0
    void reset();

    /// @brief The latest `output_delay_ms` from client/state, in microseconds; player only
    void set_output_delay_us(int64_t output_delay_us);

    /// @brief Counts a message sent: a player chunk's `ts + duration` and `13 + payload`, a
    /// visualizer message's `ts` and size. Keys never decrease within a reset.
    void add(int64_t key_us, int64_t bytes);

    int64_t count(int64_t now_us) const;

    /// @brief Whether `bytes` more may start now: `count(now) + bytes <= capacity`
    bool admits(int64_t bytes, int64_t now_us) const;

    /// @brief When `bytes` more fit: now_us when they do already, else the completion of the entry
    /// whose end leaves room; NEVER when bytes exceed the capacity
    int64_t room_time(int64_t bytes, int64_t now_us) const;

    /// @brief The last entry's completion time, for the drain watch; NEVER when empty
    int64_t last_completion() const;

    /// @brief Drops the entries with `key <= now`
    void prune(int64_t now_us);

    int64_t capacity() const {
        return this->capacity_;
    }

    int64_t output_delay_us() const {
        return this->output_delay_us_;
    }

    size_t entries() const {
        return this->entries_.size();
    }

private:
    struct Entry {
        int64_t key;
        /// The bytes added since the reset before this entry
        int64_t total_before;
    };

    /// The completion time of an entry
    int64_t completion(const Entry& entry) const;

    /// The index of the first entry that counts at now_us
    size_t first_counting(int64_t now_us) const;

    /// The index of the first entry holds() is true of, or the size; holds() is true of every
    /// entry after it
    template <class Holds>
    size_t first_index(Holds holds) const {
        const auto indices = std::views::iota(size_t{0}, this->entries_.size());
        return static_cast<size_t>(
            std::ranges::partition_point(indices,
                                         [&](size_t i) { return !holds(this->entries_[i]); }) -
            indices.begin());
    }

    /// The bytes of the entries from index on
    int64_t bytes_from(size_t index) const;

    /// Bounded by the send path's horizon: an entry is pruned once its key passes, and keys are at
    /// most the group lead and the write-ahead ahead
    Ring<Entry> entries_;
    int64_t total_{0};
    int64_t output_delay_us_{0};
    int64_t capacity_;
    bool player_;
};

}  // namespace sendspin::server::sched
