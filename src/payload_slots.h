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

/// @file payload_slots.h
/// @brief PayloadSlots: one pipeline instance's frame payloads, in fixed slots placed when the
/// instance is built and returned from any thread

#pragma once

#include "frame_payload.h"
#include "memory.h"
#include "ring.h"
#include "shared.h"

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <span>

namespace sendspin::server {

/// @brief Slots of a format's largest frame, one per frame alive at once, placed once, in PSRAM
/// where there is one. The instance (its one producer) reserves a slot, writes the frame
/// into it without the lock and commits it as a FramePayload; destroying the payload returns the
/// slot, in any order and from any thread. The mutex is a leaf, held only to take or return a slot.
class PayloadSlots : public EnableSharedFromThis<PayloadSlots> {
public:
    /// @return null when neither memory has room
    static SharedPtr<PayloadSlots> create(size_t slot_bytes, size_t slots);

    /// @brief Use create()
    PayloadSlots(PlacedArray<uint8_t> bytes, Ring<uint32_t> free, size_t slot_bytes,
                 size_t slots) noexcept;

    /// @brief Producer only. A slot's room for one frame, the same slot until it is committed;
    /// empty when every slot is taken, which sets starved()
    std::span<uint8_t> reserve();

    /// @brief Producer only. The first size bytes of the slot reserve() gave, which it must have
    /// given since the last commit
    FramePayload commit(size_t size);

    /// @brief Producer only: the last reserve() found no slot
    bool starved() const {
        return this->starved_;
    }

    size_t slot_bytes() const {
        return this->slot_bytes_;
    }

    size_t slots() const {
        return this->slots_;
    }

private:
    friend class FramePayload;

    static constexpr uint32_t NONE = UINT32_MAX;

    /// Any thread
    void release(uint32_t slot) noexcept;

    std::mutex mutex_;
    PlacedArray<uint8_t> bytes_;
    /// The slots not taken; its limit is every slot, so a return never grows it
    Ring<uint32_t> free_;
    const size_t slot_bytes_;
    const size_t slots_;
    /// Producer only: the slot reserve() gave, until it is committed
    uint32_t reserved_{NONE};
    bool starved_{false};
};

}  // namespace sendspin::server
