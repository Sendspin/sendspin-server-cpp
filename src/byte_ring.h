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

/// @file byte_ring.h
/// @brief ByteRing: a FIFO of variable-length byte records, each with a Meta, over one byte array
/// and one Ring of records, both taken when it is made

#pragma once

#include "memory.h"
#include "ring.h"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <utility>

namespace sendspin::server {

/// @brief A FIFO of records, each a Meta and a run of bytes, the bytes contiguous in one array
/// used as a circular buffer: a record that does not fit before the array's end goes at its start.
/// Both arrays are allocated when the ring is made, in PSRAM where there is one, and never
/// again. A record is placed and then committed: the bytes at the place are the producer's alone
/// until the commit, so an owner may copy them in outside its lock. No locking of its own: an
/// owner shared by threads locks around every call but data() and capacity(), which read only
/// what the constructor set.
template <class Meta>
class ByteRing {
public:
    /// @param bytes the byte storage; a record of up to this size fits once the ring is empty
    /// @param records the most records held at once
    ByteRing(size_t bytes, size_t records)
        : capacity_(bytes),
          bytes_(make_placed_array<uint8_t>(bytes, MemoryLocation::PREFER_EXTERNAL)),
          records_(records, records, MemoryLocation::PREFER_EXTERNAL) {}

    ByteRing(const ByteRing&) = delete;
    ByteRing& operator=(const ByteRing&) = delete;

    /// @brief Where a record of size bytes would go
    /// @return capacity() when it does not fit, or the records are at their limit
    size_t place(size_t size) const {
        const size_t need = stored_size(size);
        if (this->records_.size() == this->records_.limit()) {
            return this->capacity_;
        }
        if (this->records_.empty()) {
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

    /// @brief Whether a record of size bytes would fit now
    bool fits(size_t size) const {
        return this->place(size) != this->capacity_;
    }

    /// @brief The storage at offset, a place() not yet committed, for the producer to copy into
    uint8_t* data(size_t offset) {
        return this->bytes_.get() + offset;
    }

    /// @brief Records size bytes at offset, which place(size) returned with nothing but pops since
    void commit(size_t offset, size_t size, Meta&& meta) {
        assert(offset < this->capacity_ && this->records_.size() < this->records_.limit());
        if (this->records_.empty()) {
            this->byte_head_ = offset;
            this->wrapped_ = false;
        } else if (offset < this->byte_tail_) {
            this->wrapped_ = true;
        }
        this->byte_tail_ = offset + stored_size(size);
        const bool pushed = this->records_.try_push(Record{std::move(meta), offset, size});
        assert(pushed);
        static_cast<void>(pushed);
    }

    /// @brief Places, copies and commits bytes under meta in one step
    /// @return false, leaving meta as it was, when they do not fit
    [[nodiscard]] bool try_push(std::span<const uint8_t> bytes, Meta&& meta) {
        const size_t offset = this->place(bytes.size());
        if (offset == this->capacity_) {
            return false;
        }
        if (!bytes.empty()) {
            std::memcpy(this->data(offset), bytes.data(), bytes.size());
        }
        this->commit(offset, bytes.size(), std::move(meta));
        return true;
    }

    /// @brief The oldest record's Meta, valid until pop(). Not empty.
    Meta& front() {
        return this->records_.front().meta;
    }

    const Meta& front() const {
        return this->records_.front().meta;
    }

    /// @brief The oldest record's bytes, valid until pop() whatever is placed and committed
    /// meanwhile. Not empty.
    std::span<const uint8_t> front_bytes() const {
        const Record& record = this->records_.front();
        return std::span<const uint8_t>(this->bytes_.get() + record.offset, record.size);
    }

    /// @brief Destroys the oldest record and frees its bytes. Not empty.
    void pop() {
        this->records_.pop_front();
        if (this->records_.empty()) {
            return;
        }
        const size_t next = this->records_.front().offset;
        if (next < this->byte_head_) {
            this->wrapped_ = false;
        }
        this->byte_head_ = next;
    }

    /// @brief Destroys every record; keeps the arrays
    void clear() {
        this->records_.clear();
    }

    size_t size() const {
        return this->records_.size();
    }

    bool empty() const {
        return this->records_.empty();
    }

    /// @brief The largest record's bytes the ring holds
    size_t capacity() const {
        return this->capacity_;
    }

private:
    struct Record {
        Meta meta;
        size_t offset{0};
        size_t size{0};
    };

    // An empty record takes one byte, so the records' offsets rise strictly within each stretch of
    // the storage and an offset below the oldest one marks the wrap
    static size_t stored_size(size_t size) {
        return std::max<size_t>(size, 1);
    }

    const size_t capacity_;
    /// Allocated uninitialized: no byte is read before a commit covers it
    PlacedArray<uint8_t> bytes_;
    /// Fixed at its limit, so a record stays where it is until popped
    Ring<Record> records_;
    /// The oldest record's offset and the next free byte; wrapped_ when the records run past the
    /// end of the storage and on from its start. Unread while empty: the next commit sets them
    size_t byte_head_{0};
    size_t byte_tail_{0};
    bool wrapped_{false};
};

}  // namespace sendspin::server
