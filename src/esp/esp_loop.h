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

/// @file esp_loop.h
/// @brief ESP-IDF's EventLoop::Impl: the core task's queue of posted handlers, inbound drains and
/// signalled Wakeups, with the inbound ring the transport's tasks hand messages and events to core
/// through (LoopCore, which other tasks reach through a reference of their own), its timers, and
/// the count of transport tasks still tearing down that shutdown's grace waits for

#pragma once

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "inbound_ring.h"
#include "memory.h"
#include "profile.h"
#include "shared.h"
#include "task_queue.h"
#include "transport.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <type_traits>
#include <utility>
#include <vector>

namespace sendspin::server {

/// @brief A connection's receive side, which holds a message the inbound ring refused, or one that
/// came before core started the connection, and receives nothing more until it is pushed
class InboundProducer {
public:
    virtual ~InboundProducer() = default;

    /// @brief Core task: the ring takes pushes again, or core started the connection; pushes the
    /// message held, or has the receive task push it, and receives again
    virtual void resume_inbound() = 0;
};

/// @brief What other tasks reach of the loop: posting, signalling, the inbound ring and the
/// transport tasks' count. Every method but the destructor is safe from any task.
class LoopCore {
public:
    /// The inbound drains queued without allocating: one per message the ring holds, and as many
    /// again for the events raised; past them a drain is posted as a handler
    static constexpr size_t DRAIN_SLOTS = 2 * profile::INBOUND_RING_MESSAGES;

    /// The bytes of posted handlers each of the loop's two handler queues holds before it grows:
    /// what a pass of the loop takes, host calls, the group FIFOs' drains and artwork results
    static constexpr size_t POSTED_BYTES = 1024;

    /// @brief An inbound drain: InboundRing::drain(place), in the order of every post and drain
    struct Drain {
        uint64_t order;
        uint64_t place;
    };

    LoopCore();
    ~LoopCore();
    LoopCore(const LoopCore&) = delete;
    LoopCore& operator=(const LoopCore&) = delete;

    /// @brief Queues fn, a closure called with no arguments, in the order of every post and drain
    template <class F>
    void post(F&& fn) {
        std::decay_t<F> held(std::forward<F>(fn));
        this->post_task(TaskRef(held));
    }

    void post_task(TaskRef task);

    /// @brief The ring every connection of the loop hands its messages and events to core through
    InboundRing& inbound() {
        return this->inbound_;
    }

    /// @brief Any task: producer holds a refused message; it is resumed, in the order producers
    /// parked, once the ring takes pushes again. A producer resumed with nothing held does nothing.
    void park(WeakPtr<InboundProducer> producer);

    /// @brief Queues a signalled Wakeup, which holds itself until it runs; never allocates
    void enqueue(const SharedPtr<Wakeup::Impl>& wakeup);

    /// @brief A transport task started; the loop's shutdown grace waits for it to end
    void task_started() {
        this->tasks_.fetch_add(1, std::memory_order_relaxed);
    }

    /// @brief A transport task has done everything it does
    void task_ended() {
        this->tasks_.fetch_sub(1, std::memory_order_acq_rel);
        this->notify();
    }

    int tasks() const {
        return this->tasks_.load(std::memory_order_acquire);
    }

    /// @brief Wakes the loop's task
    void notify() {
        xSemaphoreGive(this->ready_);
    }

    /// @brief Waits until notified or until timeout ticks pass
    void wait(TickType_t timeout) {
        xSemaphoreTake(this->ready_, timeout);
    }

    /// @brief Takes everything posted, drained and signalled so far, each handler with its order;
    /// swaps posted, which is empty, for the handlers queued, and drains, reserved to DRAIN_SLOTS,
    /// does not allocate
    void take(TaskQueue& posted, std::vector<Drain>& drains, Wakeup::Impl*& wakeups);

    /// @brief The task destroying the loop, once its run has ended: destroys what is queued and
    /// refuses what is posted or signalled later
    void close();

private:
    /// The ring's wake: queues drain(place) without allocating while DRAIN_SLOTS has room
    void queue_drain(uint64_t place);
    /// The ring's on_room, on the core task: resumes the producers parked
    void resume_parked();

    std::mutex mutex_;
    /// Guarded by mutex_: the order the next post or drain takes
    uint64_t order_{0};
    /// Guarded by mutex_: the handlers posted, each with its order
    TaskQueue posted_{POSTED_BYTES, MemoryLocation::PREFER_EXTERNAL};
    /// Guarded by mutex_: a circular queue of DRAIN_SLOTS
    std::vector<Drain> drains_;
    size_t drain_head_{0};
    size_t drain_count_{0};
    Wakeup::Impl* wake_head_{nullptr};
    Wakeup::Impl* wake_tail_{nullptr};
    /// Guarded by mutex_
    bool closed_{false};
    SemaphoreHandle_t ready_;
    std::atomic<int> tasks_{0};
    std::mutex park_mutex_;
    /// Guarded by park_mutex_
    std::vector<WeakPtr<InboundProducer>> parked_;
    InboundRing inbound_;
};

/// @brief The core task's loop. run(), the timers and the Wakeups' runs are on that task.
class EventLoop::Impl {
public:
    Impl();
    /// Not on esp_timer's task, nor on a task an esp_timer callback waits for: it waits for a
    /// callback of its own on esp_timer's task
    ~Impl();
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    const SharedPtr<LoopCore>& core() {
        return this->core_;
    }

    void run();

    void shutdown(int64_t grace_us);

    /// @brief Timer::Impl's heap: armed, moved or removed, keyed by its due time
    void arm(Timer::Impl& timer);
    void disarm(Timer::Impl& timer);

private:
    /// Runs what is due and what was posted or signalled; false when there was nothing
    bool run_ready();
    /// Sets the esp_timer that ends the wait at the next timer's due time
    void arm_wake_timer();

    void sift_up(size_t index);
    void sift_down(size_t index);
    void swap_entries(size_t a, size_t b);

    SharedPtr<LoopCore> core_{make_shared_ptr<LoopCore>()};
    /// Binary min-heap by due time; each timer keeps its own index into it
    std::vector<Timer::Impl*> timers_;
    esp_timer_handle_t wake_timer_{nullptr};
    /// The due time the wake timer is set for, or 0 when it is not
    int64_t wake_at_us_{0};
    /// The handlers a pass takes, run in order with drains_
    TaskQueue batch_{LoopCore::POSTED_BYTES, MemoryLocation::PREFER_EXTERNAL};
    std::vector<LoopCore::Drain> drains_;
    bool stopping_{false};
    int64_t grace_us_{0};
};

/// @brief One-shot timer state on the loop's heap
class Timer::Impl {
public:
    explicit Impl(EventLoop::Impl& loop) : loop_(loop) {}

    EventLoop::Impl& loop_;
    std::function<void()> on_fire_;
    int64_t at_us_{0};
    /// Position in the loop's heap, or NOT_ARMED
    size_t index_{NOT_ARMED};

    static constexpr size_t NOT_ARMED = static_cast<size_t>(-1);
};

}  // namespace sendspin::server
