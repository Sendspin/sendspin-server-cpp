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

#include <algorithm>
#include <cstring>
#include <utility>

namespace sendspin::server {

namespace {

// An empty item takes one byte, so the items' offsets rise strictly within each stretch of the
// storage and an offset below the oldest one marks the wrap (as InboundRing's)
size_t stored_size(size_t size) {
    return std::max<size_t>(size, 1);
}

}  // namespace

SourceFeed::SourceFeed(size_t bytes, size_t items)
    : capacity_(bytes),
      bytes_(make_placed_array<uint8_t>(bytes, MemoryLocation::PREFER_EXTERNAL)),
      records_(items) {
    this->faults_.reserve(MAX_FAULTS);
}

size_t SourceFeed::place(size_t size) const {
    const size_t need = stored_size(size);
    if (this->count_ == this->records_.size()) {
        return this->capacity_;
    }
    if (this->count_ == 0) {
        return need <= this->capacity_ ? 0 : this->capacity_;
    }
    if (this->wrapped_) {
        return this->byte_head_ - this->byte_tail_ >= need ? this->byte_tail_ : this->capacity_;
    }
    if (this->capacity_ - this->byte_tail_ >= need) {
        return this->byte_tail_;
    }
    return need <= this->byte_head_ ? 0 : this->capacity_;
}

bool SourceFeed::push(const SourceItem& item, std::span<const uint8_t> bytes) {
    size_t offset = 0;
    {
        const std::lock_guard<std::mutex> lock(this->mutex_);
        offset = this->place(bytes.size());
        if (offset == this->capacity_) {
            return false;
        }
    }
    // The bytes past the tail are core's alone until the item is recorded, and the decoder only
    // frees room meanwhile, so the copy needs no lock
    if (!bytes.empty()) {
        std::memcpy(this->bytes_.get() + offset, bytes.data(), bytes.size());
    }
    {
        const std::lock_guard<std::mutex> lock(this->mutex_);
        if (this->count_ == 0) {
            this->byte_head_ = offset;
            this->wrapped_ = false;
        } else if (offset < this->byte_tail_) {
            this->wrapped_ = true;
        }
        this->byte_tail_ = offset + stored_size(bytes.size());
        this->records_[(this->head_ + this->count_) % this->records_.size()] = {item, offset,
                                                                                bytes.size()};
        ++this->count_;
    }
    if (const std::shared_ptr<AudioWorkers::Task> task = this->task_.lock()) {
        task->wake();
    }
    return true;
}

bool SourceFeed::front(SourceItem& item, std::span<const uint8_t>& bytes) const {
    const std::lock_guard<std::mutex> lock(this->mutex_);
    if (this->count_ == 0) {
        return false;
    }
    const Record& record = this->records_[this->head_];
    item = record.item;
    bytes = std::span<const uint8_t>(this->bytes_.get() + record.offset, record.size);
    return true;
}

void SourceFeed::pop() {
    const std::lock_guard<std::mutex> lock(this->mutex_);
    if (this->count_ == 0) {
        return;
    }
    this->head_ = (this->head_ + 1) % this->records_.size();
    --this->count_;
    if (this->count_ == 0) {
        this->byte_head_ = 0;
        this->byte_tail_ = 0;
        this->wrapped_ = false;
        return;
    }
    const size_t next = this->records_[this->head_].offset;
    if (next < this->byte_head_) {
        this->wrapped_ = false;
    }
    this->byte_head_ = next;
}

bool SourceFeed::empty() const {
    const std::lock_guard<std::mutex> lock(this->mutex_);
    return this->count_ == 0;
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
