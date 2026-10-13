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

#include "inbound_hold.h"

#include <utility>

namespace sendspin::server {

InboundHold::InboundHold(PushFn push, std::function<void()> park,
                         std::function<void(CloseReason reason)> raise_closed, size_t kept_bytes)
    : push_(std::move(push)),
      park_(std::move(park)),
      raise_closed_(std::move(raise_closed)),
      kept_bytes_(kept_bytes) {}

ExternalBytes* InboundHold::begin_message() {
    {
        const std::lock_guard<std::mutex> lock(this->mutex_);
        // A pusher may be reading the buffer
        if (this->held_ || this->pushing_) {
            return nullptr;
        }
    }
    // Nothing is held, so no task reads the buffer until the next offer()
    if (this->message_.capacity() > this->kept_bytes_) {
        ExternalBytes().swap(this->message_);
    } else {
        this->message_.clear();
    }
    return &this->message_;
}

InboundHold::Outcome InboundHold::offer(MessageKind kind, size_t size, int64_t rx_us) {
    std::unique_lock<std::mutex> lock(this->mutex_);
    if (this->held_ || this->pushing_) {
        return {Result::DROPPED, 0, nullptr};
    }
    this->held_ = true;
    this->size_ = size;
    this->kind_ = kind;
    this->rx_us_ = rx_us;
    ++this->generation_;
    return this->release(lock);
}

bool InboundHold::pause(uint64_t generation, void* token) {
    const std::lock_guard<std::mutex> lock(this->mutex_);
    if (this->released_ >= generation) {
        return false;
    }
    this->pause_ = token;
    this->pause_generation_ = generation;
    return true;
}

bool InboundHold::released(uint64_t generation) {
    const std::lock_guard<std::mutex> lock(this->mutex_);
    return this->released_ >= generation;
}

void InboundHold::bind() {
    std::optional<CloseReason> close;
    {
        const std::lock_guard<std::mutex> lock(this->mutex_);
        this->bound_ = true;
        close = this->close_to_raise();
    }
    if (close) {
        this->raise_closed_(*close);
    }
}

void InboundHold::start() {
    const std::lock_guard<std::mutex> lock(this->mutex_);
    this->started_ = true;
}

void InboundHold::drop() {
    const std::lock_guard<std::mutex> lock(this->mutex_);
    this->dropping_ = true;
}

InboundHold::Outcome InboundHold::resume() {
    std::unique_lock<std::mutex> lock(this->mutex_);
    this->parked_ = false;
    if (!this->held_) {
        return {Result::NOTHING, 0, nullptr};
    }
    if (this->pushing_) {
        this->resume_requested_ = true;
        return {Result::NOTHING, 0, nullptr};
    }
    return this->release(lock);
}

InboundHold::Outcome InboundHold::release(std::unique_lock<std::mutex>& lock) {
    Result result = Result::HELD;
    for (;;) {
        if (this->dropping_) {
            result = Result::DROPPED;
            break;
        }
        if (!this->started_) {
            return {Result::HELD, this->generation_, nullptr};
        }
        this->pushing_ = true;
        this->resume_requested_ = false;
        const MessageKind kind = this->kind_;
        const std::span<const uint8_t> payload(this->message_.data(), this->size_);
        const int64_t rx_us = this->rx_us_;
        lock.unlock();
        bool pushed = this->push_(kind, payload, rx_us);
        if (!pushed) {
            // Parked first, then offered once more: room made before the park would wake nobody.
            // One park until the resume it brings: parked_ is cleared by that resume.
            lock.lock();
            const bool park = !std::exchange(this->parked_, true);
            lock.unlock();
            if (park) {
                this->park_();
            }
            pushed = this->push_(kind, payload, rx_us);
        }
        lock.lock();
        this->pushing_ = false;
        if (pushed) {
            result = Result::PUSHED;
            break;
        }
        // A resume, or a drop, that came while the push was refused: its room may already be
        // spent on nobody, so offer once more
        if (!std::exchange(this->resume_requested_, false)) {
            return {Result::HELD, this->generation_, nullptr};
        }
    }
    this->held_ = false;
    this->released_ = this->generation_;
    void* unpause = nullptr;
    if (this->pause_ != nullptr && this->pause_generation_ == this->generation_) {
        unpause = std::exchange(this->pause_, nullptr);
    }
    const std::optional<CloseReason> close = this->close_to_raise();
    const uint64_t generation = this->generation_;
    lock.unlock();
    if (close) {
        this->raise_closed_(*close);
    }
    lock.lock();
    return {result, generation, unpause};
}

void InboundHold::report_closed(CloseReason reason) {
    std::optional<CloseReason> close;
    {
        const std::lock_guard<std::mutex> lock(this->mutex_);
        if (this->close_reason_) {
            return;
        }
        this->close_reason_ = reason;
        close = this->close_to_raise();
    }
    if (close) {
        this->raise_closed_(*close);
    }
}

void* InboundHold::take_pause() {
    const std::lock_guard<std::mutex> lock(this->mutex_);
    return std::exchange(this->pause_, nullptr);
}

std::optional<CloseReason> InboundHold::close_to_raise() {
    // CLOSED comes after the connection's last push: a message stays held while it is pushed, so
    // this waits for a push in flight too
    if (!this->close_reason_ || this->close_raised_ || !this->bound_ || this->held_) {
        return std::nullopt;
    }
    this->close_raised_ = true;
    return this->close_reason_;
}

}  // namespace sendspin::server
