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

/// @file ring.h
/// @brief Ring: a FIFO of fixed-size records over one array in placed memory, taken when it is made

#pragma once

#include "memory.h"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <new>
#include <type_traits>
#include <utility>

namespace sendspin::server {

/// @brief A FIFO of T in one array used as a circular buffer, in the memory its owner names. The
/// array is allocated when the ring is made; a push that finds it full doubles it, up to the
/// ring's limit, and moves the elements over in order, so a ring whose limit is its capacity never
/// allocates again. Growing invalidates references to the elements; popping invalidates only the
/// element popped. One thread at a time: an owner shared by threads locks around it.
template <class T>
class Ring {
    static_assert(std::is_nothrow_move_constructible_v<T>, "a ring moves its elements as it grows");

public:
    /// @brief The limit of a ring that its producers bound, as its declaration says
    static constexpr size_t UNLIMITED = SIZE_MAX / sizeof(T);

    /// @brief No array and a limit of 0: refuses every push. The other side of a swap.
    Ring() noexcept = default;

    /// @brief Allocates capacity elements' room in location; throws std::bad_alloc when neither
    /// memory has it
    /// @param limit the most elements it grows to: capacity for a ring that never allocates again
    Ring(size_t capacity, size_t limit, MemoryLocation location)
        : limit_(limit), location_(location) {
        assert(capacity <= limit);
        this->reallocate(capacity);
    }

    ~Ring() {
        this->clear();
        placed_free(this->slots_);
    }

    Ring(const Ring&) = delete;
    Ring& operator=(const Ring&) = delete;

    Ring(Ring&& other) noexcept {
        this->swap(other);
    }

    Ring& operator=(Ring&& other) noexcept {
        Ring moved(std::move(other));
        this->swap(moved);
        return *this;
    }

    /// @brief Appends a T made from value, growing the array when it is full and below the limit;
    /// value is not one of the ring's elements
    /// @return false, leaving value as it was, when it is full at its limit
    template <class U = T>
    [[nodiscard]] bool try_push(U&& value) {
        if (this->size_ == this->capacity_) {
            if (this->capacity_ >= this->limit_) {
                return false;
            }
            this->reallocate(this->capacity_ == 0                 ? 1
                             : this->capacity_ > this->limit_ / 2 ? this->limit_
                                                                  : 2 * this->capacity_);
        }
        new (this->slot(this->size_)) T(std::forward<U>(value));
        ++this->size_;
        return true;
    }

    /// @brief try_push() into an UNLIMITED ring, which takes every push it has memory for
    template <class U = T>
    void push(U&& value) {
        assert(this->limit_ == UNLIMITED);
        const bool pushed = this->try_push(std::forward<U>(value));
        assert(pushed);
        static_cast<void>(pushed);
    }

    /// @brief Destroys the oldest element. Not empty.
    void pop_front() {
        assert(this->size_ > 0);
        this->slot(0)->~T();
        this->head_ = this->head_ + 1 == this->capacity_ ? 0 : this->head_ + 1;
        --this->size_;
    }

    /// @brief Destroys every element; keeps the array
    void clear() {
        while (this->size_ > 0) {
            this->pop_front();
        }
        this->head_ = 0;
    }

    /// @brief The element index places from the oldest
    T& operator[](size_t index) {
        assert(index < this->size_);
        return *this->slot(index);
    }

    const T& operator[](size_t index) const {
        assert(index < this->size_);
        return *this->slot(index);
    }

    T& front() {
        return (*this)[0];
    }

    const T& front() const {
        return (*this)[0];
    }

    T& back() {
        return (*this)[this->size_ - 1];
    }

    const T& back() const {
        return (*this)[this->size_ - 1];
    }

    size_t size() const {
        return this->size_;
    }

    bool empty() const {
        return this->size_ == 0;
    }

    /// @brief The elements the array holds now
    size_t capacity() const {
        return this->capacity_;
    }

    size_t limit() const {
        return this->limit_;
    }

    /// @brief Exchanges the two rings' arrays, elements, limits and memory, allocating nothing
    void swap(Ring& other) noexcept {
        std::swap(this->slots_, other.slots_);
        std::swap(this->capacity_, other.capacity_);
        std::swap(this->limit_, other.limit_);
        std::swap(this->head_, other.head_);
        std::swap(this->size_, other.size_);
        std::swap(this->location_, other.location_);
    }

private:
    T* slot(size_t index) const {
        const size_t at = this->head_ + index;
        return this->slots_ + (at < this->capacity_ ? at : at - this->capacity_);
    }

    /// Moves the elements, oldest first, into an array of capacity elements
    void reallocate(size_t capacity) {
        T* slots = nullptr;
        if (capacity > 0) {
            slots = static_cast<T*>(
                placed_malloc(capacity * sizeof(T), new_alignment<T>(), this->location_));
            if (slots == nullptr) {
                throw std::bad_alloc();
            }
        }
        for (size_t i = 0; i < this->size_; ++i) {
            T* from = this->slot(i);
            new (slots + i) T(std::move(*from));
            from->~T();
        }
        placed_free(this->slots_);
        this->slots_ = slots;
        this->capacity_ = capacity;
        this->head_ = 0;
    }

    T* slots_{nullptr};
    size_t capacity_{0};
    size_t limit_{0};
    /// The oldest element's slot
    size_t head_{0};
    size_t size_{0};
    MemoryLocation location_{MemoryLocation::PREFER_EXTERNAL};
};

}  // namespace sendspin::server
