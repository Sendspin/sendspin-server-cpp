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

#include "payload_slots.h"

#include <cassert>
#include <new>
#include <utility>

namespace sendspin::server {

// ==============================================================================
// FramePayload
// ==============================================================================

FramePayload::FramePayload(SharedPtr<PayloadSlots> slots, const uint8_t* data, size_t size,
                           uint32_t slot) noexcept
    : slots_(std::move(slots)), data_(data), size_(size), slot_(slot) {}

FramePayload::~FramePayload() {
    this->reset();
}

FramePayload::FramePayload(FramePayload&& other) noexcept
    : slots_(std::move(other.slots_)),
      data_(std::exchange(other.data_, nullptr)),
      size_(std::exchange(other.size_, 0)),
      slot_(other.slot_) {}

FramePayload& FramePayload::operator=(FramePayload&& other) noexcept {
    if (this != &other) {
        this->reset();
        this->slots_ = std::move(other.slots_);
        this->data_ = std::exchange(other.data_, nullptr);
        this->size_ = std::exchange(other.size_, 0);
        this->slot_ = other.slot_;
    }
    return *this;
}

void FramePayload::reset() noexcept {
    if (this->slots_) {
        this->slots_->release(this->slot_);
        this->slots_.reset();
    }
    this->data_ = nullptr;
    this->size_ = 0;
}

// ==============================================================================
// PayloadSlots
// ==============================================================================

SharedPtr<PayloadSlots> PayloadSlots::create(size_t slot_bytes, size_t slots) {
    if (slot_bytes == 0 || slots == 0 || slots >= NONE || slot_bytes > SIZE_MAX / slots) {
        return nullptr;
    }
    try {
        PlacedArray<uint8_t> bytes =
            make_placed_array<uint8_t>(slot_bytes * slots, MemoryLocation::PREFER_EXTERNAL);
        Ring<uint32_t> free(slots, slots, MemoryLocation::PREFER_EXTERNAL);
        for (uint32_t slot = 0; slot < slots; ++slot) {
            static_cast<void>(free.try_push(slot));
        }
        return make_shared_external<PayloadSlots>(std::move(bytes), std::move(free), slot_bytes,
                                                  slots);
    } catch (const std::bad_alloc&) {
        return nullptr;
    }
}

PayloadSlots::PayloadSlots(PlacedArray<uint8_t> bytes, Ring<uint32_t> free, size_t slot_bytes,
                           size_t slots) noexcept
    : bytes_(std::move(bytes)), free_(std::move(free)), slot_bytes_(slot_bytes), slots_(slots) {}

std::span<uint8_t> PayloadSlots::reserve() {
    if (this->reserved_ == NONE) {
        const std::lock_guard<std::mutex> lock(this->mutex_);
        if (!this->free_.empty()) {
            this->reserved_ = this->free_.front();
            this->free_.pop_front();
        }
    }
    this->starved_ = this->reserved_ == NONE;
    if (this->starved_) {
        return {};
    }
    return {this->bytes_.get() + (size_t{this->reserved_} * this->slot_bytes_), this->slot_bytes_};
}

FramePayload PayloadSlots::commit(size_t size) {
    assert(this->reserved_ != NONE && size <= this->slot_bytes_);
    const uint32_t slot = std::exchange(this->reserved_, NONE);
    return {this->shared_from_this(), this->bytes_.get() + (size_t{slot} * this->slot_bytes_), size,
            slot};
}

void PayloadSlots::release(uint32_t slot) noexcept {
    const std::lock_guard<std::mutex> lock(this->mutex_);
    // Holds every slot, so a return always fits
    const bool returned = this->free_.try_push(slot);
    assert(returned);
    static_cast<void>(returned);
}

}  // namespace sendspin::server
