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

// EventLoop, Timer and Wakeup on FreeRTOS: one binary semaphore wakes the core task, an
// esp_timer gives it at the next timer's due time

#include "clock.h"
#include "esp_loop.h"
#include "memory.h"
#include "transport.h"

#include <algorithm>
#include <utility>

namespace sendspin::server {

namespace {

constexpr int64_t US_PER_TICK = static_cast<int64_t>(portTICK_PERIOD_MS) * 1000;

/// Ticks to wait for at least us microseconds, and at least one
TickType_t ticks_for(int64_t us) {
    return static_cast<TickType_t>(std::max<int64_t>((us + US_PER_TICK - 1) / US_PER_TICK, 1));
}

}  // namespace

// ==============================================================================
// Wakeup
// ==============================================================================

class Wakeup::Impl {
public:
    Impl(WeakPtr<LoopCore> loop, std::function<void()> on_wake)
        : loop_(std::move(loop)), on_wake_(std::move(on_wake)) {}

    static void signal(const SharedPtr<Impl>& self) {
        if (self->pending_.exchange(true, std::memory_order_acq_rel)) {
            return;
        }
        // A loop that is gone leaves pending_ set, so later signals stop here
        if (const SharedPtr<LoopCore> loop = self->loop_.lock()) {
            loop->enqueue(self);
        }
    }

    /// Core task only. A run already queued finds cancelled_ and does nothing; pending_ stays set,
    /// so later signals queue nothing.
    void cancel() {
        this->pending_.store(true, std::memory_order_release);
        this->cancelled_ = true;
        if (!this->running_) {
            this->on_wake_ = nullptr;
        }
    }

    /// Core task only: the run a signal queued
    void run() {
        if (this->cancelled_) {
            return;
        }
        this->pending_.store(false, std::memory_order_release);
        this->running_ = true;
        this->on_wake_();
        this->running_ = false;
        // Cancelled by its own run
        if (this->cancelled_) {
            this->on_wake_ = nullptr;
        }
    }

    /// LoopCore's queue: the next signalled Wakeup, and this one held while queued
    Impl* next_{nullptr};
    SharedPtr<Impl> queued_;

private:
    WeakPtr<LoopCore> loop_;
    std::function<void()> on_wake_;
    std::atomic<bool> pending_{false};
    bool cancelled_{false};
    bool running_{false};
};

Wakeup::Wakeup(EventLoop& loop, std::function<void()> on_wake)
    : impl_(make_shared_external<Impl>(loop.impl().core(), std::move(on_wake))) {}

Wakeup::~Wakeup() {
    this->impl_->cancel();
}

void Wakeup::signal() {
    Impl::signal(this->impl_);
}

void Wakeup::signal(const Handle& handle) {
    if (handle) {
        Impl::signal(handle);
    }
}

// ==============================================================================
// LoopCore
// ==============================================================================

LoopCore::LoopCore()
    : ready_(xSemaphoreCreateBinary()),
      inbound_(
          profile::INBOUND_RING_BYTES, profile::INBOUND_RING_MESSAGES,
          [this](uint64_t place) { this->queue_drain(place); }, [this] { this->resume_parked(); }) {
}

LoopCore::~LoopCore() {
    // close() emptied the queue on the task that destroyed the loop
    vSemaphoreDelete(this->ready_);
}

void LoopCore::post_task(TaskRef task) {
    {
        const std::lock_guard<std::mutex> lock(this->mutex_);
        if (this->closed_) {
            // Destroyed by the task that posted it
            return;
        }
        this->posted_.push(task, ++this->order_);
    }
    this->notify();
}

void LoopCore::queue_drain(uint64_t place) {
    {
        const std::lock_guard<std::mutex> lock(this->mutex_);
        if (this->closed_) {
            return;
        }
        const uint64_t order = ++this->order_;
        if (!this->drains_.try_push(Drain{order, place})) {
            // Past the slots, a drain is a handler like any other, in the same order
            auto drain = [this, place] { this->inbound_.drain(place); };
            this->posted_.push(TaskRef(drain), order);
        }
    }
    this->notify();
}

void LoopCore::park(WeakPtr<InboundProducer> producer) {
    const std::lock_guard<std::mutex> lock(this->park_mutex_);
    // A producer whose earlier park outlived its message (another resume released it) keeps that
    // place
    for (const WeakPtr<InboundProducer>& parked : this->parked_) {
        if (!parked.owner_before(producer) && !producer.owner_before(parked)) {
            return;
        }
    }
    this->parked_.push_back(std::move(producer));
}

void LoopCore::resume_parked() {
    std::vector<WeakPtr<InboundProducer>> parked;
    {
        const std::lock_guard<std::mutex> lock(this->park_mutex_);
        parked.swap(this->parked_);
    }
    // A producer refused again parks again, behind those still to resume, so the order holds
    for (const WeakPtr<InboundProducer>& weak : parked) {
        if (const SharedPtr<InboundProducer> producer = weak.lock()) {
            producer->resume_inbound();
        }
    }
}

void LoopCore::enqueue(const SharedPtr<Wakeup::Impl>& wakeup) {
    {
        const std::lock_guard<std::mutex> lock(this->mutex_);
        if (this->closed_) {
            return;
        }
        wakeup->queued_ = wakeup;
        if (this->wake_tail_ == nullptr) {
            this->wake_head_ = wakeup.get();
        } else {
            this->wake_tail_->next_ = wakeup.get();
        }
        this->wake_tail_ = wakeup.get();
    }
    this->notify();
}

void LoopCore::close() {
    TaskQueue posted;
    Wakeup::Impl* wakeups = nullptr;
    {
        const std::lock_guard<std::mutex> lock(this->mutex_);
        this->closed_ = true;
        posted.swap(this->posted_);
        this->drains_.clear();
        wakeups = this->wake_head_;
        this->wake_head_ = nullptr;
        this->wake_tail_ = nullptr;
    }
    {
        const std::lock_guard<std::mutex> lock(this->park_mutex_);
        this->parked_.clear();
    }
    // The messages and events no drain will deliver; a later push is held until the ring goes
    // with the LoopCore, and its endpoint does not reach its connection, which is gone
    this->inbound_.clear();
    // Queued Wakeups hold themselves; dropping the hold may destroy them, on this task
    while (wakeups != nullptr) {
        Wakeup::Impl* next = wakeups->next_;
        wakeups->next_ = nullptr;
        const SharedPtr<Wakeup::Impl> released = std::move(wakeups->queued_);
        wakeups = next;
    }
}

void LoopCore::take(TaskQueue& posted, std::vector<Drain>& drains, Wakeup::Impl*& wakeups) {
    const std::lock_guard<std::mutex> lock(this->mutex_);
    posted.swap(this->posted_);
    drains.clear();
    for (size_t i = 0; i < this->drains_.size(); ++i) {
        drains.push_back(this->drains_[i]);
    }
    this->drains_.clear();
    wakeups = this->wake_head_;
    this->wake_head_ = nullptr;
    this->wake_tail_ = nullptr;
}

// ==============================================================================
// EventLoop
// ==============================================================================

EventLoop::Impl::Impl() {
    this->drains_.reserve(LoopCore::DRAIN_SLOTS);
    esp_timer_create_args_t args{};
    args.callback = [](void* arg) { static_cast<LoopCore*>(arg)->notify(); };
    args.arg = this->core_.get();
    args.dispatch_method = ESP_TIMER_TASK;
    args.name = "ss_loop";
    esp_timer_create(&args, &this->wake_timer_);
}

EventLoop::Impl::~Impl() {
    if (this->wake_timer_ != nullptr) {
        esp_timer_stop(this->wake_timer_);
        esp_timer_delete(this->wake_timer_);
        // esp_timer_delete() does not wait for a callback already running, which uses core_.
        // esp_timer's task runs callbacks one at a time, so once a callback started now has run,
        // so has that one.
        SemaphoreHandle_t done = xSemaphoreCreateBinary();
        esp_timer_create_args_t args{};
        args.callback = [](void* arg) { xSemaphoreGive(static_cast<SemaphoreHandle_t>(arg)); };
        args.arg = done;
        args.dispatch_method = ESP_TIMER_TASK;
        args.name = "ss_loop_end";
        esp_timer_handle_t barrier = nullptr;
        if (done != nullptr && esp_timer_create(&args, &barrier) == ESP_OK) {
            if (esp_timer_start_once(barrier, 0) == ESP_OK) {
                xSemaphoreTake(done, portMAX_DELAY);
            }
            esp_timer_delete(barrier);
        }
        if (done != nullptr) {
            vSemaphoreDelete(done);
        }
    }
    // What is still queued is destroyed on this task, and nothing is queued later, so a transport
    // task holding the last reference to core_ destroys nothing but its semaphore and the ring
    this->core_->close();
}

void EventLoop::Impl::run() {
    while (!this->stopping_) {
        if (!this->run_ready()) {
            this->arm_wake_timer();
            this->core_->wait(portMAX_DELAY);
        }
    }
    // The grace: what is ready runs, and then the loop waits for the transport tasks to end
    const int64_t deadline_us = now_us() + this->grace_us_;
    while (true) {
        if (this->run_ready()) {
            continue;
        }
        const int64_t left_us = deadline_us - now_us();
        if (this->core_->tasks() == 0 || left_us <= 0) {
            break;
        }
        this->arm_wake_timer();
        this->core_->wait(ticks_for(left_us));
    }
    esp_timer_stop(this->wake_timer_);
}

void EventLoop::Impl::shutdown(int64_t grace_us) {
    this->stopping_ = true;
    this->grace_us_ = grace_us;
}

bool EventLoop::Impl::run_ready() {
    bool ran = false;
    const size_t due = this->timers_.size();
    const int64_t now = now_us();
    // At most the timers armed now, so a timer restarted at once waits for the next pass
    for (size_t i = 0; i < due && !this->timers_.empty() && this->timers_[0]->at_us_ <= now; ++i) {
        Timer::Impl* timer = this->timers_[0];
        this->disarm(*timer);
        // The call may destroy the timer or start it again
        std::function<void()> on_fire = std::move(timer->on_fire_);
        timer->on_fire_ = nullptr;
        on_fire();
        ran = true;
    }

    Wakeup::Impl* wakeups = nullptr;
    this->core_->take(this->batch_, this->drains_, wakeups);
    // Posts and drains in the order they came: a message's drain runs after what was posted before
    // it was pushed
    auto drain = this->drains_.begin();
    while (!this->batch_.empty() || drain != this->drains_.end()) {
        if (drain == this->drains_.end() ||
            (!this->batch_.empty() && this->batch_.front_order() < drain->order)) {
            this->batch_.run_front();
        } else {
            this->core_->inbound().drain(drain->place);
            ++drain;
        }
        ran = true;
    }
    while (wakeups != nullptr) {
        Wakeup::Impl* next = wakeups->next_;
        wakeups->next_ = nullptr;
        // Its signal cannot queue it again before run() clears pending_
        const SharedPtr<Wakeup::Impl> wakeup = std::move(wakeups->queued_);
        wakeup->run();
        wakeups = next;
        ran = true;
    }
    return ran;
}

void EventLoop::Impl::arm_wake_timer() {
    const int64_t next_us = this->timers_.empty() ? 0 : this->timers_[0]->at_us_;
    if (next_us == this->wake_at_us_ && (next_us == 0 || esp_timer_is_active(this->wake_timer_))) {
        return;
    }
    esp_timer_stop(this->wake_timer_);
    this->wake_at_us_ = next_us;
    if (next_us == 0) {
        return;
    }
    const int64_t delay_us = next_us - now_us();
    if (delay_us <= 0) {
        this->core_->notify();
        return;
    }
    esp_timer_start_once(this->wake_timer_, static_cast<uint64_t>(delay_us));
}

void EventLoop::Impl::arm(Timer::Impl& timer) {
    if (timer.index_ == Timer::Impl::NOT_ARMED) {
        timer.index_ = this->timers_.size();
        this->timers_.push_back(&timer);
        this->sift_up(timer.index_);
        return;
    }
    this->sift_up(timer.index_);
    this->sift_down(timer.index_);
}

void EventLoop::Impl::disarm(Timer::Impl& timer) {
    const size_t index = timer.index_;
    if (index == Timer::Impl::NOT_ARMED) {
        return;
    }
    const size_t last = this->timers_.size() - 1;
    if (index != last) {
        this->swap_entries(index, last);
    }
    this->timers_.pop_back();
    timer.index_ = Timer::Impl::NOT_ARMED;
    if (index != last) {
        this->sift_up(index);
        this->sift_down(index);
    }
}

void EventLoop::Impl::swap_entries(size_t a, size_t b) {
    std::swap(this->timers_[a], this->timers_[b]);
    this->timers_[a]->index_ = a;
    this->timers_[b]->index_ = b;
}

void EventLoop::Impl::sift_up(size_t index) {
    while (index > 0) {
        const size_t parent = (index - 1) / 2;
        if (this->timers_[parent]->at_us_ <= this->timers_[index]->at_us_) {
            return;
        }
        this->swap_entries(parent, index);
        index = parent;
    }
}

void EventLoop::Impl::sift_down(size_t index) {
    const size_t size = this->timers_.size();
    while (true) {
        const size_t left = (2 * index) + 1;
        const size_t right = left + 1;
        size_t least = index;
        if (left < size && this->timers_[left]->at_us_ < this->timers_[least]->at_us_) {
            least = left;
        }
        if (right < size && this->timers_[right]->at_us_ < this->timers_[least]->at_us_) {
            least = right;
        }
        if (least == index) {
            return;
        }
        this->swap_entries(index, least);
        index = least;
    }
}

EventLoop::EventLoop() : impl_(std::make_unique<Impl>()) {}

EventLoop::~EventLoop() = default;

void EventLoop::run() {
    this->impl_->run();
}

void EventLoop::post_task(TaskRef task) {
    this->impl_->core()->post_task(task);
}

void EventLoop::shutdown(int64_t grace_us) {
    this->impl_->shutdown(grace_us);
}

// ==============================================================================
// Timer
// ==============================================================================

Timer::Timer(EventLoop& loop) : impl_(make_shared_external<Impl>(loop.impl())) {}

Timer::~Timer() {
    this->cancel();
}

void Timer::start(int64_t at_us, std::function<void()> on_fire) {
    this->impl_->on_fire_ = std::move(on_fire);
    this->impl_->at_us_ = at_us;
    this->impl_->loop_.arm(*this->impl_);
}

void Timer::cancel() {
    this->impl_->loop_.disarm(*this->impl_);
    this->impl_->on_fire_ = nullptr;
}

}  // namespace sendspin::server
