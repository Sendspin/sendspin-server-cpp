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

/// @file player_sink.h
/// @brief For tests with in-process sendspin-cpp players: the test signal, whose frames carry
/// their position, and an audio output that plays in real time

#pragma once

#include <sendspin/client.h>
#include <sendspin/player_role.h>
#include <sendspin/server/types.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

namespace sendspin::server::test {

using Bytes = std::vector<uint8_t>;

// ==============================================================================
// The test signal: stereo, the left sample a frame's position and the right a check value of it,
// so a frame the player altered (an interpolated or blended correction) does not decode. No
// frame is silence, position 0 included.
// ==============================================================================

class Signal {
public:
    /// @param bits 16 (positions below 65,536) or 24 (below 2^23)
    explicit Signal(uint32_t bits) : bits_(bits) {}

    uint32_t bits() const {
        return this->bits_;
    }

    size_t frame_bytes() const {
        return size_t{this->bits_} / 8 * 2;
    }

    SampleFormat sample_format() const {
        return this->bits_ == 16 ? SampleFormat::S16 : SampleFormat::S24;
    }

    void put(uint8_t* out, uint32_t position) const {
        put_sample(out, position);
        put_sample(out + (this->bits_ / 8), this->check_of(position));
    }

    /// @return nullopt for a frame that is not one of the signal's
    std::optional<uint32_t> get(const uint8_t* in) const {
        const uint32_t position = this->get_sample(in);
        if (this->get_sample(in + (this->bits_ / 8)) != this->check_of(position)) {
            return std::nullopt;
        }
        return position;
    }

private:
    uint32_t mask() const {
        // 24 bits: 23, so a position is never negative
        return this->bits_ == 16 ? 0xFFFFU : 0x7FFFFFU;
    }

    uint32_t check_of(uint32_t position) const {
        return (((position * 2654435761U) >> 8) ^ 0x5A5A5AU) & this->mask();
    }

    void put_sample(uint8_t* out, uint32_t value) const {
        for (uint32_t i = 0; i < this->bits_ / 8; ++i) {
            out[i] = static_cast<uint8_t>(value >> (8 * i));
        }
    }

    uint32_t get_sample(const uint8_t* in) const {
        uint32_t value = 0;
        for (uint32_t i = 0; i < this->bits_ / 8; ++i) {
            value |= uint32_t{in[i]} << (8 * i);
        }
        return value;
    }

    uint32_t bits_;
};

// ==============================================================================
// An audio output that plays in real time
// ==============================================================================

inline int64_t client_clock_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

/// A player's audio output: a 100 ms buffer that its own thread empties at the stream's rate,
/// recording every frame it plays and reporting it to the player as a sound card's callback would
class Sink : public sendspin::PlayerRoleListener {
public:
    Sink(sendspin::PlayerRole& role, size_t frame_bytes, uint32_t rate)
        : role_(role),
          frame_bytes_(frame_bytes),
          rate_(rate),
          buffer_bytes_(rate / 10 * frame_bytes),
          thread_([this] { this->run(); }) {}

    ~Sink() override {
        {
            const std::lock_guard<std::mutex> lock(this->mutex_);
            this->stopping_ = true;
        }
        this->changed_.notify_all();
        this->thread_.join();
    }

    Sink(const Sink&) = delete;
    Sink& operator=(const Sink&) = delete;

    size_t on_audio_write(uint8_t* data, size_t length, uint32_t timeout_ms) override {
        std::unique_lock<std::mutex> lock(this->mutex_);
        this->changed_.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&] {
            return this->stopping_ ||
                   this->buffer_.size() + this->frame_bytes_ <= this->buffer_bytes_;
        });
        size_t room = this->buffer_bytes_ - std::min(this->buffer_bytes_, this->buffer_.size());
        room = std::min(room, length);
        room -= room % this->frame_bytes_;
        this->buffer_.insert(this->buffer_.end(), data, data + room);
        return room;
    }

    void on_stream_start() override {
        const std::lock_guard<std::mutex> lock(this->mutex_);
        this->streaming_ = true;
    }

    void on_stream_end() override {
        const std::lock_guard<std::mutex> lock(this->mutex_);
        this->streaming_ = false;
    }

    /// Whether no stream is open and every frame written has played. on_stream_end() fires once
    /// the player has stopped writing its stream.
    bool played_out() const {
        const std::lock_guard<std::mutex> lock(this->mutex_);
        return !this->streaming_ && this->buffer_.empty();
    }

    /// Every frame played so far
    Bytes played() const {
        const std::lock_guard<std::mutex> lock(this->mutex_);
        return this->played_;
    }

    /// Frames played that are not silence
    size_t audible() const {
        const std::lock_guard<std::mutex> lock(this->mutex_);
        return this->audible_;
    }

private:
    void run() {
        constexpr int64_t TICK_US = 5000;
        int64_t last = client_clock_us();
        int64_t owed_us = 0;
        std::unique_lock<std::mutex> lock(this->mutex_);
        while (!this->stopping_) {
            this->changed_.wait_for(lock, std::chrono::microseconds(TICK_US));
            const int64_t now = client_clock_us();
            owed_us += now - last;
            last = now;
            const auto due = static_cast<size_t>(owed_us * this->rate_ / 1000000);
            owed_us -= static_cast<int64_t>(due) * 1000000 / this->rate_;
            const size_t frames = std::min(due, this->buffer_.size() / this->frame_bytes_);
            if (frames == 0) {
                continue;
            }
            const auto end =
                this->buffer_.begin() + static_cast<std::ptrdiff_t>(frames * this->frame_bytes_);
            for (auto frame = this->buffer_.begin(); frame != end;
                 frame += static_cast<std::ptrdiff_t>(this->frame_bytes_)) {
                const auto next = frame + static_cast<std::ptrdiff_t>(this->frame_bytes_);
                if (std::any_of(frame, next, [](uint8_t byte) { return byte != 0; })) {
                    ++this->audible_;
                }
            }
            this->played_.insert(this->played_.end(), this->buffer_.begin(), end);
            this->buffer_.erase(this->buffer_.begin(), end);
            this->changed_.notify_all();
            lock.unlock();
            this->role_.notify_audio_played(static_cast<uint32_t>(frames), now);
            lock.lock();
        }
    }

    sendspin::PlayerRole& role_;
    size_t frame_bytes_;
    int64_t rate_;
    size_t buffer_bytes_;
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    Bytes buffer_;
    Bytes played_;
    size_t audible_{0};
    bool streaming_{false};
    bool stopping_{false};
    std::thread thread_;
};

class AlwaysReady : public sendspin::SendspinNetworkProvider {
public:
    bool is_network_ready() override {
        return true;
    }
};

}  // namespace sendspin::server::test
