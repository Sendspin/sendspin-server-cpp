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

#include "live_feeder.h"

#include "wav_reader.h"
#include <poll.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <utility>
#include <vector>

namespace sendspin::server::examples {

namespace {

/// The most bytes one read takes
constexpr size_t READ_BYTES = size_t{64} * 1024;

}  // namespace

LiveFeeder::LiveFeeder(int fd, const InputFormat& format,
                       std::function<void(GroupId, Event)> on_event)
    : fd_(fd), format_(format), frame_bytes_(frame_bytes(format)), on_event_(std::move(on_event)) {
    if (::pipe(this->wake_) != 0) {
        throw std::runtime_error(std::string("pipe: ") + std::strerror(errno));
    }
    this->thread_ = std::thread([this] { this->run(); });
}

LiveFeeder::~LiveFeeder() {
    const char byte = 0;
    while (::write(this->wake_[1], &byte, 1) < 0 && errno == EINTR) {}
    this->thread_.join();
    std::shared_ptr<GroupInput> input;
    {
        const std::lock_guard<std::mutex> lock(this->mutex_);
        input = std::move(this->input_);
    }
    if (input) {
        input->stop();
    }
    ::close(this->wake_[0]);
    ::close(this->wake_[1]);
}

bool LiveFeeder::attach(std::shared_ptr<GroupInput> input) {
    std::shared_ptr<GroupInput> old;
    {
        const std::lock_guard<std::mutex> lock(this->mutex_);
        if (this->ended_) {
            return false;
        }
        old = std::exchange(this->input_, std::move(input));
        this->paused_ = false;
        this->stopped_ = false;
        this->drain_.reset();
        this->written_ = 0;
        this->dropped_ = 0;
    }
    if (old) {
        old->stop();
    }
    return true;
}

bool LiveFeeder::detach(GroupId group) {
    std::shared_ptr<GroupInput> old;
    bool ended = false;
    {
        const std::lock_guard<std::mutex> lock(this->mutex_);
        if (!this->input_ || this->input_->group() != group) {
            return false;
        }
        old = std::move(this->input_);
        // Its drain's outcome no longer reaches the feeder, so the detach is the final event
        ended = this->ended_ && this->drain_.has_value();
        this->drain_.reset();
    }
    old->stop();
    this->report(ended ? std::optional<Event>(Event::ENDED) : std::nullopt, group);
    return true;
}

std::optional<uint64_t> LiveFeeder::pause(GroupId group) {
    const std::lock_guard<std::mutex> lock(this->mutex_);
    if (!this->input_ || this->input_->group() != group || this->paused_ || this->stopped_) {
        return std::nullopt;
    }
    this->paused_ = true;
    // The input takes every write while paused and resumes at the newest
    return this->input_->pause();
}

bool LiveFeeder::resume(GroupId group) {
    const std::lock_guard<std::mutex> lock(this->mutex_);
    if (!this->input_ || this->input_->group() != group) {
        return false;
    }
    if (this->stopped_) {
        // The next write begins a new run
        this->stopped_ = false;
        return true;
    }
    if (!this->paused_) {
        return false;
    }
    this->paused_ = false;
    this->input_->resume();
    return true;
}

void LiveFeeder::on_drained(InputId input, DrainId drain, DrainOutcome outcome) {
    std::optional<Event> event;
    GroupId group;
    {
        const std::lock_guard<std::mutex> lock(this->mutex_);
        if (!this->input_ || this->input_->id() != input || this->drain_ != drain) {
            return;
        }
        this->drain_.reset();
        group = this->input_->group();
        if (outcome == DrainOutcome::PLAYED) {
            event = Event::PLAYED;
        } else if (this->input_->state() == InputState::CLOSED) {
            event = Event::CLOSED;
        } else if (outcome == DrainOutcome::STOPPED) {
            // Only a stop by the server discards the feeder's drain; acknowledge it
            this->input_->stop();
            this->stopped_ = true;
            this->paused_ = false;
            event = Event::STOPPED_BY_SERVER;
        }
    }
    this->report(event, group);
}

std::optional<LiveFeeder::Status> LiveFeeder::status(GroupId group) const {
    const std::lock_guard<std::mutex> lock(this->mutex_);
    if (!this->input_ || this->input_->group() != group) {
        return std::nullopt;
    }
    Status status;
    status.paused = this->paused_;
    status.stopped = this->stopped_;
    status.ended = this->ended_;
    status.written = this->written_;
    status.dropped = this->dropped_;
    status.latency_us = this->input_->latency_us();
    status.input_state = this->input_->state();
    return status;
}

void LiveFeeder::run() {
    std::vector<uint8_t> buffer(READ_BYTES + this->frame_bytes_);
    size_t held = 0;
    for (;;) {
        pollfd fds[2] = {{this->fd_, POLLIN, 0}, {this->wake_[0], POLLIN, 0}};
        const int ready = ::poll(fds, 2, -1);
        if (ready < 0 && errno == EINTR) {
            continue;
        }
        if (ready >= 0 && fds[1].revents != 0) {
            return;
        }
        // A failed poll ends the data as a failed read does
        ssize_t got = -1;
        if (ready >= 0) {
            got = ::read(this->fd_, buffer.data() + held, READ_BYTES);
            if (got < 0 && (errno == EINTR || errno == EAGAIN)) {
                continue;
            }
        }
        std::optional<Event> event;
        GroupId group;
        if (got <= 0) {
            {
                const std::lock_guard<std::mutex> lock(this->mutex_);
                group = this->input_ ? this->input_->group() : GroupId{};
                event = this->end_locked();
            }
            this->report(event, group);
            return;
        }
        held += static_cast<size_t>(got);
        const size_t frames = held / this->frame_bytes_;
        std::shared_ptr<GroupInput> released;
        {
            const std::lock_guard<std::mutex> lock(this->mutex_);
            group = this->input_ ? this->input_->group() : GroupId{};
            event = this->write_locked(buffer.data(), frames, released);
        }
        released.reset();
        this->report(event, group);
        // A partial frame waits for the rest of it
        const size_t used = frames * this->frame_bytes_;
        std::memmove(buffer.data(), buffer.data() + used, held - used);
        held -= used;
    }
}

std::optional<LiveFeeder::Event> LiveFeeder::write_locked(const uint8_t* pcm, size_t frames,
                                                          std::shared_ptr<GroupInput>& released) {
    if (!this->input_ || this->stopped_ || frames == 0) {
        return std::nullopt;
    }
    const WriteResult result = this->input_->write(pcm, frames);
    // Short with OPEN, the input played its latency down: the rest plays within what it staged
    const size_t taken = result.state == InputState::OPEN ? frames : result.frames;
    this->written_ += taken;
    this->dropped_ += frames - taken;
    if (result.state == InputState::STOP_LATCHED) {
        // Acknowledged; the next run waits for resume()
        this->input_->stop();
        this->stopped_ = true;
        this->paused_ = false;
        return Event::STOPPED_BY_SERVER;
    }
    if (result.state == InputState::CLOSED) {
        // Released: what is read from now on is dropped
        released = std::move(this->input_);
        return Event::CLOSED;
    }
    return std::nullopt;
}

LiveFeeder::Event LiveFeeder::end_locked() {
    this->ended_ = true;
    if (!this->input_ || this->stopped_) {
        return Event::ENDED;
    }
    this->drain_ = this->input_->drain();
    return Event::END_OF_INPUT;
}

void LiveFeeder::report(const std::optional<Event>& event, GroupId group) {
    if (event && this->on_event_) {
        this->on_event_(group, *event);
    }
}

}  // namespace sendspin::server::examples
