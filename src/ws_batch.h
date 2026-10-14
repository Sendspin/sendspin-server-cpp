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

/// @file ws_batch.h
/// @brief WsBatch: the messages of one write (Connection's write invariant) as WebSocket frames
/// back to back, ready for one send: unmasked from an accepted connection, masked from a dialed
/// one. Platform-free; the ESP-IDF transport (src/esp/) writes from it.

#pragma once

#include "memory.h"
#include "transport.h"
#include "ws_frame.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace sendspin::server {

/// @brief One write's frames, in PSRAM where there is one. Written by core while no write is in
/// flight, read by the writer while one is.
class WsBatch {
public:
    void clear() {
        this->bytes_.clear();
        this->frames_ = 0;
    }

    /// @brief Appends payload as one final text or binary frame, masked with mask when set (in
    /// place over the copy, by aligned words)
    void append(MessageKind kind, std::span<const uint8_t> payload,
                const std::optional<WsMaskKey>& mask = std::nullopt);

    /// @brief Every frame, headers included, as one send writes them
    std::span<const uint8_t> bytes() const {
        return this->bytes_;
    }

    size_t frames() const {
        return this->frames_;
    }

private:
    ExternalBytes bytes_;
    size_t frames_{0};
};

}  // namespace sendspin::server
