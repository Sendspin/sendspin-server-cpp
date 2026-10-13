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

#include "inbound_ring.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <limits>
#include <utility>

namespace sendspin::server {

namespace {

// An empty message takes one byte, so the messages' offsets rise strictly within each stretch of
// the storage and an offset below the oldest one marks the wrap
size_t stored_size(size_t size) {
    return std::max<size_t>(size, 1);
}

}  // namespace

InboundRing::InboundRing(size_t bytes, size_t messages, std::function<void(uint64_t place)> wake,
                         std::function<void()> on_room)
    : capacity_(bytes),
      wake_(std::move(wake)),
      on_room_(std::move(on_room)),
      bytes_(make_placed_array<uint8_t>(bytes, MemoryLocation::PREFER_EXTERNAL)),
      records_(messages) {
    this->raised_.reserve(messages);
    this->taking_.reserve(messages);
}

size_t InboundRing::place(size_t size) const {
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

bool InboundRing::push(const SharedPtr<InboundEndpoint>& to, MessageKind kind,
                       std::span<const uint8_t> payload, int64_t rx_us) {
    uint64_t place = 0;
    {
        const std::lock_guard<std::mutex> lock(this->mutex_);
        const size_t offset = this->refused_ ? this->capacity_ : this->place(payload.size());
        if (offset == this->capacity_) {
            this->refused_ = true;
            this->refused_bytes_ = std::max(this->refused_bytes_, payload.size());
            return false;
        }
        if (this->count_ == 0) {
            this->byte_head_ = 0;
            this->wrapped_ = false;
        } else if (offset < this->byte_tail_) {
            this->wrapped_ = true;
        }
        if (!payload.empty()) {
            std::memcpy(this->bytes_.get() + offset, payload.data(), payload.size());
        }
        this->byte_tail_ = offset + stored_size(payload.size());
        place = ++this->places_;
        Record& record = this->records_[(this->head_ + this->count_) % this->records_.size()];
        record.to = to;
        record.offset = offset;
        record.size = payload.size();
        record.rx_us = rx_us;
        record.place = place;
        record.kind = kind;
        ++this->count_;
    }
    this->wake_(place);
    return true;
}

void InboundRing::raise(const SharedPtr<InboundEndpoint>& to, InboundEndpoint::Event event,
                        CloseReason reason) {
    uint64_t place = 0;
    {
        const std::lock_guard<std::mutex> lock(this->mutex_);
        place = ++this->places_;
        this->raised_.push_back({to, place, event, reason});
    }
    this->wake_(place);
}

void InboundRing::deliver_taken() {
    for (const Raised& raised : this->taking_) {
        switch (raised.event) {
            case InboundEndpoint::Event::WRITABLE:
                raised.to->deliver_writable();
                break;
            case InboundEndpoint::Event::WRITE_DONE:
                raised.to->deliver_write_done();
                break;
            case InboundEndpoint::Event::CLOSED:
                raised.to->deliver_closed(raised.reason);
                break;
        }
    }
    this->taking_.clear();
}

void InboundRing::drain(uint64_t through) {
    bool room = false;
    for (;;) {
        SharedPtr<InboundEndpoint> to;
        const Record* record = nullptr;
        {
            const std::lock_guard<std::mutex> lock(this->mutex_);
            const uint64_t message_place = this->count_ != 0 ? this->records_[this->head_].place
                                                             : std::numeric_limits<uint64_t>::max();
            size_t events = 0;
            while (events < this->raised_.size() && this->raised_[events].place <= through &&
                   this->raised_[events].place < message_place) {
                ++events;
            }
            if (events > 0) {
                const auto taken = this->raised_.begin() + static_cast<std::ptrdiff_t>(events);
                std::move(this->raised_.begin(), taken, std::back_inserter(this->taking_));
                this->raised_.erase(this->raised_.begin(), taken);
            } else if (this->count_ != 0 && message_place <= through) {
                record = &this->records_[this->head_];
                to = record->to;
            } else {
                break;
            }
        }
        if (record == nullptr) {
            this->deliver_taken();
            continue;
        }
        // Producers write only past the tail, so the oldest message stays as it is while delivered
        to->deliver_message(record->kind,
                            std::span(this->bytes_.get() + record->offset, record->size),
                            record->rx_us);
        SharedPtr<InboundEndpoint> released;
        {
            const std::lock_guard<std::mutex> lock(this->mutex_);
            released = std::move(this->records_[this->head_].to);
            this->head_ = (this->head_ + 1) % this->records_.size();
            --this->count_;
            if (this->count_ == 0) {
                this->byte_head_ = 0;
                this->byte_tail_ = 0;
                this->wrapped_ = false;
            } else {
                const size_t next = this->records_[this->head_].offset;
                if (next < this->byte_head_) {
                    this->wrapped_ = false;
                }
                this->byte_head_ = next;
            }
            if (this->refused_ && this->place(this->refused_bytes_) != this->capacity_) {
                this->refused_ = false;
                this->refused_bytes_ = 0;
                room = true;
            }
        }
    }
    if (room) {
        this->on_room_();
    }
}

void InboundRing::clear() {
    std::vector<SharedPtr<InboundEndpoint>> released;
    {
        const std::lock_guard<std::mutex> lock(this->mutex_);
        for (Raised& raised : this->raised_) {
            released.push_back(std::move(raised.to));
        }
        this->raised_.clear();
        for (Record& record : this->records_) {
            if (record.to) {
                released.push_back(std::move(record.to));
            }
        }
        this->head_ = 0;
        this->count_ = 0;
        this->byte_head_ = 0;
        this->byte_tail_ = 0;
        this->wrapped_ = false;
        this->refused_ = false;
        this->refused_bytes_ = 0;
    }
    // The endpoints go outside the mutex
    released.clear();
}

}  // namespace sendspin::server
