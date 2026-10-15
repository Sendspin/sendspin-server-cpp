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
#include <iterator>
#include <limits>
#include <utility>

namespace sendspin::server {

InboundRing::InboundRing(size_t bytes, size_t messages, std::function<void(uint64_t place)> wake,
                         std::function<void()> on_room)
    : wake_(std::move(wake)), on_room_(std::move(on_room)), messages_(bytes, messages) {
    this->raised_.reserve(messages);
    this->taking_.reserve(messages);
}

bool InboundRing::push(const SharedPtr<InboundEndpoint>& to, MessageKind kind,
                       std::span<const uint8_t> payload, int64_t rx_us) {
    uint64_t place = 0;
    {
        const std::lock_guard<std::mutex> lock(this->mutex_);
        const uint64_t next = this->places_ + 1;
        if (this->refused_ || !this->messages_.try_push(payload, {to, rx_us, next, kind})) {
            this->refused_ = true;
            this->refused_bytes_ = std::max(this->refused_bytes_, payload.size());
            return false;
        }
        place = this->places_ = next;
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
        const Message* message = nullptr;
        std::span<const uint8_t> payload;
        {
            const std::lock_guard<std::mutex> lock(this->mutex_);
            const uint64_t message_place = !this->messages_.empty()
                                               ? this->messages_.front().place
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
            } else if (!this->messages_.empty() && message_place <= through) {
                message = &this->messages_.front();
                payload = this->messages_.front_bytes();
                to = message->to;
            } else {
                break;
            }
        }
        if (message == nullptr) {
            this->deliver_taken();
            continue;
        }
        // Producers write only past the tail, so the oldest message stays as it is while delivered
        to->deliver_message(message->kind, payload, message->rx_us);
        SharedPtr<InboundEndpoint> released;
        {
            const std::lock_guard<std::mutex> lock(this->mutex_);
            released = std::move(this->messages_.front().to);
            this->messages_.pop();
            if (this->refused_ && this->messages_.fits(this->refused_bytes_)) {
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
        while (!this->messages_.empty()) {
            released.push_back(std::move(this->messages_.front().to));
            this->messages_.pop();
        }
        this->refused_ = false;
        this->refused_bytes_ = 0;
    }
    // The endpoints go outside the mutex
    released.clear();
}

}  // namespace sendspin::server
