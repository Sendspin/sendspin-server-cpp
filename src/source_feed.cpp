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

#include "source_feed.h"

#include <cstring>
#include <utility>

namespace sendspin::server {

SourceFeed::SourceFeed(size_t bytes, size_t items) : items_(bytes, items) {
    this->faults_.reserve(MAX_FAULTS);
}

bool SourceFeed::push(const SourceItem& item, std::span<const uint8_t> bytes) {
    size_t offset = 0;
    {
        const std::lock_guard<std::mutex> lock(this->mutex_);
        offset = this->items_.place(bytes.size());
        if (offset == this->items_.capacity()) {
            return false;
        }
    }
    // The bytes past the tail are core's alone until the item is recorded, and the decoder only
    // frees room meanwhile, so the copy needs no lock
    if (!bytes.empty()) {
        std::memcpy(this->items_.data(offset), bytes.data(), bytes.size());
    }
    {
        const std::lock_guard<std::mutex> lock(this->mutex_);
        this->items_.commit(offset, bytes.size(), SourceItem(item));
    }
    if (const std::shared_ptr<AudioWorkers::Task> task = this->task_.lock()) {
        task->wake();
    }
    return true;
}

bool SourceFeed::front(SourceItem& item, std::span<const uint8_t>& bytes) const {
    const std::lock_guard<std::mutex> lock(this->mutex_);
    if (this->items_.empty()) {
        return false;
    }
    item = this->items_.front();
    bytes = this->items_.front_bytes();
    return true;
}

void SourceFeed::pop() {
    const std::lock_guard<std::mutex> lock(this->mutex_);
    if (!this->items_.empty()) {
        this->items_.pop();
    }
}

bool SourceFeed::empty() const {
    const std::lock_guard<std::mutex> lock(this->mutex_);
    return this->items_.empty();
}

void SourceFeed::report(const SourceFault& fault) {
    const std::lock_guard<std::mutex> lock(this->mutex_);
    if (this->faults_.size() < MAX_FAULTS) {
        this->faults_.push_back(fault);
    }
}

std::vector<SourceFault> SourceFeed::take_faults() {
    std::vector<SourceFault> faults;
    faults.reserve(MAX_FAULTS);
    const std::lock_guard<std::mutex> lock(this->mutex_);
    std::swap(faults, this->faults_);
    return faults;
}

}  // namespace sendspin::server
