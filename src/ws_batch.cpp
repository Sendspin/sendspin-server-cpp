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

#include "ws_batch.h"

#include <array>

namespace sendspin::server {

void WsBatch::append(MessageKind kind, std::span<const uint8_t> payload,
                     const std::optional<WsMaskKey>& mask) {
    std::array<uint8_t, WS_FRAME_HEADER_MAX> header{};
    const size_t header_bytes =
        ws_frame_header(kind == MessageKind::TEXT ? ws_opcode::TEXT : ws_opcode::BINARY,
                        payload.size(), header, mask);
    this->bytes_.insert(this->bytes_.end(), header.begin(), header.begin() + header_bytes);
    const size_t at = this->bytes_.size();
    this->bytes_.insert(this->bytes_.end(), payload.begin(), payload.end());
    if (mask) {
        uint8_t* copied = this->bytes_.data() + at;
        ws_mask_copy(copied, copied, payload.size(), *mask);
    }
    ++this->frames_;
}

}  // namespace sendspin::server
