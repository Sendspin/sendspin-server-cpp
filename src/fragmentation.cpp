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

#include "fragmentation.h"

#include "crypto/crypto.h"

#include <algorithm>

namespace sendspin::server {

namespace {

/// [1][flags] on every fragment, then [orig_type] on the first
constexpr size_t CONTINUATION_HEADER_SIZE = 2;
constexpr size_t FIRST_HEADER_SIZE = CONTINUATION_HEADER_SIZE + 1;

}  // namespace

Reassembler::Reassembler(size_t max_message_bytes, bool (*is_buffered)(uint8_t message_id))
    : max_message_bytes_(max_message_bytes), is_buffered_(is_buffered) {}

Reassembler::~Reassembler() {
    this->wipe();
}

Reassembler::Outcome Reassembler::fail(Outcome outcome) {
    this->in_progress_ = false;
    this->discarding_ = false;
    // The message it held may carry a secret
    this->wipe();
    this->buffer_.clear();
    return outcome;
}

Reassembler::Outcome Reassembler::accept(std::span<const uint8_t> plaintext) {
    this->message_ = {};
    if (plaintext.empty()) {
        return this->fail(Outcome::MALFORMED);
    }
    if (plaintext[0] != MESSAGE_ID_FRAGMENT) {
        if (this->in_progress_) {
            return this->fail(Outcome::MALFORMED);
        }
        this->message_ = plaintext;
        return Outcome::MESSAGE;
    }
    // A fragment without its flags byte, or a first fragment without its orig_type, has no place
    // in the sequence and is treated like the listed malformed sequences
    if (plaintext.size() < CONTINUATION_HEADER_SIZE) {
        return this->fail(Outcome::MALFORMED);
    }
    const uint8_t flags = plaintext[1];
    if ((flags & FRAGMENT_FLAGS_RESERVED) != 0) {
        return this->fail(Outcome::MALFORMED);
    }

    std::span<const uint8_t> data;
    if ((flags & FRAGMENT_FLAG_FIRST) != 0) {
        if (this->in_progress_ || plaintext.size() < FIRST_HEADER_SIZE) {
            return this->fail(Outcome::MALFORMED);
        }
        const uint8_t orig_type = plaintext[CONTINUATION_HEADER_SIZE];
        if (orig_type == MESSAGE_ID_FRAGMENT) {
            return this->fail(Outcome::MALFORMED);
        }
        this->in_progress_ = true;
        this->discarding_ = !this->is_buffered_(orig_type);
        this->buffer_.clear();
        if (!this->discarding_) {
            this->buffer_.push_back(orig_type);
        }
        data = plaintext.subspan(FIRST_HEADER_SIZE);
    } else {
        if (!this->in_progress_) {
            return this->fail(Outcome::MALFORMED);
        }
        data = plaintext.subspan(CONTINUATION_HEADER_SIZE);
    }

    if (!this->discarding_) {
        // The buffer holds the ID byte as well
        if (this->buffer_.size() - 1 + data.size() > this->max_message_bytes_) {
            return this->fail(Outcome::TOO_LARGE);
        }
        const size_t needed = this->buffer_.size() + data.size();
        if (needed > this->buffer_.capacity()) {
            // At least doubled, as insert() would, within the cap
            const size_t doubled =
                std::min(2 * this->buffer_.capacity(), this->max_message_bytes_ + 1);
            secure_reserve(this->buffer_, std::max(needed, doubled), this->written_);
        }
        this->buffer_.insert(this->buffer_.end(), data.begin(), data.end());
        this->written_ = std::max(this->written_, this->buffer_.size());
    }

    if ((flags & FRAGMENT_FLAG_LAST) == 0) {
        return Outcome::PENDING;
    }
    this->in_progress_ = false;
    if (this->discarding_) {
        this->discarding_ = false;
        return Outcome::DISCARDED;
    }
    this->message_ = this->buffer_;
    return Outcome::MESSAGE;
}

size_t write_fragment(uint8_t orig_type, std::span<const uint8_t> payload, size_t offset,
                      size_t max_plaintext, std::vector<uint8_t>& out) {
    const bool first = offset == 0;
    const size_t header = first ? FIRST_HEADER_SIZE : CONTINUATION_HEADER_SIZE;
    const size_t size = std::min(payload.size() - offset, max_plaintext - header);
    const size_t next = offset + size;
    uint8_t flags = 0;
    if (first) {
        flags |= FRAGMENT_FLAG_FIRST;
    }
    if (next == payload.size()) {
        flags |= FRAGMENT_FLAG_LAST;
    }
    out.clear();
    reserve_plaintext(out, header + size);
    out.push_back(MESSAGE_ID_FRAGMENT);
    out.push_back(flags);
    if (first) {
        out.push_back(orig_type);
    }
    out.insert(out.end(), payload.begin() + static_cast<ptrdiff_t>(offset),
               payload.begin() + static_cast<ptrdiff_t>(next));
    return next;
}

void Reassembler::wipe() {
    secure_zero_storage(this->buffer_, this->written_);
    this->written_ = 0;
}

}  // namespace sendspin::server
