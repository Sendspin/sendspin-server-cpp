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

/// @file frame_payload.h
/// @brief FramePayload: a frame's bytes, held in a slot of its pipeline instance's PayloadSlots

#pragma once

#include "shared.h"

#include <cstddef>
#include <cstdint>
#include <span>

namespace sendspin::server {

class PayloadSlots;

/// @brief A frame's bytes in one slot of its instance's PayloadSlots (payload_slots.h). Move-only;
/// destroying it returns the slot, on whatever thread that happens. Empty when default-made.
class FramePayload {
public:
    FramePayload() noexcept = default;
    ~FramePayload();
    FramePayload(FramePayload&& other) noexcept;
    FramePayload& operator=(FramePayload&& other) noexcept;
    FramePayload(const FramePayload&) = delete;
    FramePayload& operator=(const FramePayload&) = delete;

    std::span<const uint8_t> bytes() const {
        return {this->data_, this->size_};
    }

    size_t size() const {
        return this->size_;
    }

private:
    friend class PayloadSlots;

    FramePayload(SharedPtr<PayloadSlots> slots, const uint8_t* data, size_t size,
                 uint32_t slot) noexcept;

    /// Returns the slot, leaving this empty
    void reset() noexcept;

    SharedPtr<PayloadSlots> slots_;
    const uint8_t* data_{nullptr};
    size_t size_{0};
    uint32_t slot_{0};
};

}  // namespace sendspin::server
