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

#include "sched/ledger.h"

#include "sched/constants.h"

namespace sendspin::server::sched {

void Ledger::reset() {
    this->entries_.clear();
    this->total_ = 0;
}

void Ledger::set_output_delay_us(int64_t output_delay_us) {
    if (this->player_) {
        this->output_delay_us_ = output_delay_us;
    }
}

void Ledger::add(int64_t key_us, int64_t bytes) {
    this->entries_.push({key_us, this->total_});
    this->total_ += bytes;
}

int64_t Ledger::count(int64_t now_us) const {
    return this->bytes_from(this->first_counting(now_us));
}

bool Ledger::admits(int64_t bytes, int64_t now_us) const {
    return this->count(now_us) + bytes <= this->capacity_;
}

int64_t Ledger::room_time(int64_t bytes, int64_t now_us) const {
    if (bytes > this->capacity_) {
        return NEVER;
    }
    if (this->admits(bytes, now_us)) {
        return now_us;
    }
    // Entries complete in order, so room appears when every entry before the first suffix that
    // fits has completed
    const size_t suffix = this->first_index([&](const Entry& entry) {
        return this->total_ - entry.total_before + bytes <= this->capacity_;
    });
    return this->completion(this->entries_[suffix - 1]);
}

int64_t Ledger::last_completion() const {
    return this->entries_.empty() ? NEVER : this->completion(this->entries_.back());
}

void Ledger::prune(int64_t now_us) {
    while (!this->entries_.empty() && this->entries_.front().key <= now_us) {
        this->entries_.pop_front();
    }
}

int64_t Ledger::completion(const Entry& entry) const {
    return entry.key - this->output_delay_us_;
}

size_t Ledger::first_counting(int64_t now_us) const {
    return this->first_index([&](const Entry& entry) { return this->completion(entry) > now_us; });
}

int64_t Ledger::bytes_from(size_t index) const {
    return index == this->entries_.size() ? 0 : this->total_ - this->entries_[index].total_before;
}

}  // namespace sendspin::server::sched
