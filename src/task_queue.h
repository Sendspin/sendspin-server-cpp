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

/// @file task_queue.h
/// @brief TaskQueue: a FIFO of closures of any size, each moved into a block of placed memory
/// behind a small header, so queueing one takes no allocation of its own; TaskRef, a closure on its
/// way into one, and HeapTask, a closure held alone

#pragma once

#include "memory.h"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <new>
#include <type_traits>
#include <utility>

namespace sendspin::server {

/// @brief How a closure is run, moved and destroyed, without its type
struct TaskOps {
    void (*run)(void* task);
    /// Move-constructs the closure at to from the one at from, which its owner still destroys
    void (*move_to)(void* from, void* to);
    void (*destroy)(void* task) noexcept;
};

/// @brief The alignment of every closure a TaskQueue or HeapTask holds
constexpr size_t TASK_ALIGNMENT = alignof(uint64_t);

/// @brief bytes rounded up to TASK_ALIGNMENT
constexpr size_t task_bytes(size_t bytes) {
    return (bytes + TASK_ALIGNMENT - 1) & ~(TASK_ALIGNMENT - 1);
}

template <class F>
inline constexpr TaskOps TASK_OPS{
    [](void* task) { std::invoke(*static_cast<F*>(task)); },
    [](void* from, void* to) { new (to) F(std::move(*static_cast<F*>(from))); },
    [](void* task) noexcept { static_cast<F*>(task)->~F(); },
};

/// @brief A closure its caller holds, for the time of the one call that moves it into a queue
class TaskRef {
public:
    template <class F>
    explicit TaskRef(F& task) : task_(&task), ops_(&TASK_OPS<F>), size_(sizeof(F)) {
        static_assert(std::is_invocable_v<F&>, "a task is called with no arguments");
        static_assert(std::is_move_constructible_v<F>, "a task is moved into its queue");
        static_assert(alignof(F) <= TASK_ALIGNMENT, "a queue aligns its tasks to TASK_ALIGNMENT");
    }

    void* task() const {
        return this->task_;
    }

    const TaskOps* ops() const {
        return this->ops_;
    }

    size_t size() const {
        return this->size_;
    }

private:
    void* task_;
    const TaskOps* ops_;
    size_t size_;
};

/// @brief One closure in a heap block of its own, for a queue that takes only owning handlers (the
/// host's Asio loop). Move-only.
class HeapTask {
public:
    explicit HeapTask(TaskRef task)
        : task_(::operator new(task.size(), std::align_val_t{TASK_ALIGNMENT})), ops_(task.ops()) {
        try {
            this->ops_->move_to(task.task(), this->task_);
        } catch (...) {
            ::operator delete(this->task_, std::align_val_t{TASK_ALIGNMENT});
            throw;
        }
    }

    ~HeapTask() {
        if (this->task_ != nullptr) {
            this->ops_->destroy(this->task_);
            ::operator delete(this->task_, std::align_val_t{TASK_ALIGNMENT});
        }
    }

    HeapTask(HeapTask&& other) noexcept
        : task_(std::exchange(other.task_, nullptr)), ops_(other.ops_) {}
    HeapTask(const HeapTask&) = delete;
    HeapTask& operator=(const HeapTask&) = delete;
    HeapTask& operator=(HeapTask&&) = delete;

    void operator()() const {
        this->ops_->run(this->task_);
    }

private:
    void* task_;
    const TaskOps* ops_;
};

/// @brief A FIFO of closures, each moved into a block behind a header naming its operations, its
/// size and an order its owner gives it; a closure stays where it was moved in until it is run or
/// destroyed, so a closure need not move without throwing. A push that finds no room in the newest
/// block chains one of at least twice the bytes queued, and a block before the newest is freed
/// once its closures are gone. Made for an owner that takes the whole queue at once (swap()) and
/// runs it out: an empty queue starts its newest block over, so steady traffic reuses one block,
/// while a queue that never empties allocates a block each time its newest fills. One thread at a
/// time: an owner shared by threads locks around it.
class TaskQueue {
public:
    /// @brief No block: the first push allocates one. The other side of a swap.
    TaskQueue() noexcept = default;

    /// @brief Allocates a block of bytes in location; throws std::bad_alloc when neither memory has
    /// them
    TaskQueue(size_t bytes, MemoryLocation location) : initial_bytes_(bytes), location_(location) {
        this->head_ = this->allocate(bytes);
        this->tail_ = this->head_;
    }

    ~TaskQueue() {
        this->clear();
        this->keep_newest();
        placed_free(this->tail_);
    }

    TaskQueue(const TaskQueue&) = delete;
    TaskQueue& operator=(const TaskQueue&) = delete;
    TaskQueue(TaskQueue&&) = delete;
    TaskQueue& operator=(TaskQueue&&) = delete;

    /// @brief Moves task in behind every closure queued, chaining a block when the newest has no
    /// room. When the closure's move throws, nothing is queued.
    void push(TaskRef task, uint64_t order = 0) {
        if (this->count_ == 0) {
            this->keep_newest();
        }
        const size_t stride = task_bytes(HEADER_BYTES + task.size());
        if (this->tail_ == nullptr || this->tail_->bytes - this->tail_->used < stride) {
            const size_t bytes =
                std::max({this->tail_ == nullptr ? this->initial_bytes_ : this->tail_->bytes,
                          2 * this->queued_bytes_, stride});
            Block* block = this->allocate(bytes);
            if (this->tail_ == nullptr) {
                this->head_ = block;
            } else if (this->count_ == 0) {
                placed_free(this->tail_);
                this->head_ = block;
            } else {
                this->tail_->next = block;
            }
            this->tail_ = block;
        }
        uint8_t* at = data(this->tail_) + this->tail_->used;
        task.ops()->move_to(task.task(), at + HEADER_BYTES);
        new (at) Header{task.ops(), stride, order};
        this->tail_->used += stride;
        this->queued_bytes_ += stride;
        ++this->count_;
    }

    /// @brief The order the oldest closure was pushed with. Not empty.
    uint64_t front_order() const {
        return this->front()->order;
    }

    /// @brief Runs the oldest closure, then destroys it. Not empty.
    void run_front() {
        Header* header = this->front();
        header->ops->run(reinterpret_cast<uint8_t*>(header) + HEADER_BYTES);
        this->pop_front();
    }

    /// @brief Destroys the oldest closure without running it. Not empty.
    void pop_front() {
        Header* header = this->front();
        header->ops->destroy(reinterpret_cast<uint8_t*>(header) + HEADER_BYTES);
        this->read_ += header->stride;
        this->queued_bytes_ -= header->stride;
        --this->count_;
        if (this->count_ == 0) {
            this->keep_newest();
            return;
        }
        // The next closure is in a later block; one a throwing push chained may be empty
        while (this->read_ == this->head_->used) {
            Block* next = this->head_->next;
            placed_free(this->head_);
            this->head_ = next;
            this->read_ = 0;
        }
    }

    /// @brief Destroys every closure without running it
    void clear() {
        while (this->count_ > 0) {
            this->pop_front();
        }
    }

    bool empty() const {
        return this->count_ == 0;
    }

    size_t size() const {
        return this->count_;
    }

    /// @brief The bytes of its blocks
    size_t bytes() const {
        size_t bytes = 0;
        for (const Block* block = this->head_; block != nullptr; block = block->next) {
            bytes += block->bytes;
        }
        return bytes;
    }

    /// @brief Exchanges the two queues' blocks and closures, allocating nothing
    void swap(TaskQueue& other) noexcept {
        std::swap(this->head_, other.head_);
        std::swap(this->tail_, other.tail_);
        std::swap(this->read_, other.read_);
        std::swap(this->queued_bytes_, other.queued_bytes_);
        std::swap(this->count_, other.count_);
        std::swap(this->initial_bytes_, other.initial_bytes_);
        std::swap(this->location_, other.location_);
    }

private:
    /// A block's own header; its closures follow at BLOCK_BYTES
    struct Block {
        Block* next;
        size_t bytes;
        size_t used;
    };

    struct Header {
        const TaskOps* ops;
        size_t stride;
        uint64_t order;
    };

    static constexpr size_t BLOCK_BYTES = task_bytes(sizeof(Block));
    static constexpr size_t HEADER_BYTES = task_bytes(sizeof(Header));

    static uint8_t* data(Block* block) {
        return reinterpret_cast<uint8_t*>(block) + BLOCK_BYTES;
    }

    Block* allocate(size_t bytes) const {
        void* memory = placed_malloc(BLOCK_BYTES + bytes, TASK_ALIGNMENT, this->location_);
        if (memory == nullptr) {
            throw std::bad_alloc();
        }
        return new (memory) Block{nullptr, bytes, 0};
    }

    Header* front() const {
        assert(this->count_ > 0);
        return reinterpret_cast<Header*>(data(this->head_) + this->read_);
    }

    /// Empty: frees every block but the newest and starts it over
    void keep_newest() {
        while (this->head_ != this->tail_) {
            Block* next = this->head_->next;
            placed_free(this->head_);
            this->head_ = next;
        }
        this->read_ = 0;
        if (this->tail_ != nullptr) {
            this->tail_->used = 0;
        }
    }

    /// The oldest block, holding the oldest closure, and the newest, which pushes go into
    Block* head_{nullptr};
    Block* tail_{nullptr};
    /// The oldest closure's offset in head_
    size_t read_{0};
    size_t queued_bytes_{0};
    size_t count_{0};
    size_t initial_bytes_{0};
    MemoryLocation location_{MemoryLocation::PREFER_EXTERNAL};
};

}  // namespace sendspin::server
