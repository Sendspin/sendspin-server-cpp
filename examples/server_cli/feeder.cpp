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

#include "feeder.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace sendspin::server::examples {

namespace {

/// Frames per read and write()
constexpr uint64_t BLOCK_FRAMES = 4096;
constexpr int64_t US_PER_SECOND = 1000000;
/// The files placed since the last seek that are kept, so a loop of short files does not grow
/// them without bound; status() locates what plays within the last few of them
constexpr size_t MAX_PLACED = 1024;

}  // namespace

Feeder::Feeder(std::shared_ptr<GroupInput> input, std::shared_ptr<AudioSource> file,
               std::function<void(const Report&)> on_event, bool loop)
    : input_(std::move(input)), on_event_(std::move(on_event)), loop_(loop) {
    const AudioInfo info = file->info();
    this->files_.push_back({std::move(file), info});
    this->placed_.push_back({0, {0, 0}});
    this->thread_ = std::thread([this] { this->run(); });
}

Feeder::~Feeder() {
    {
        const std::lock_guard<std::mutex> lock(this->mutex_);
        this->stopping_ = true;
        // Also ends the thread's wait_writable()
        this->input_->stop();
    }
    this->wake_.notify_all();
    this->thread_.join();
}

InputId Feeder::input_id() const {
    return this->input_->id();
}

AudioInfo Feeder::info(size_t file) const {
    const std::lock_guard<std::mutex> lock(this->mutex_);
    return this->files_[std::min(file, this->files_.size() - 1)].info;
}

size_t Feeder::writing() const {
    const std::lock_guard<std::mutex> lock(this->mutex_);
    return this->placed_.back().spot.file;
}

bool Feeder::play(std::shared_ptr<AudioSource> file) {
    const AudioInfo info = file->info();
    {
        const std::lock_guard<std::mutex> lock(this->mutex_);
        if (this->phase_ == Phase::CLOSED) {
            return false;
        }
        // Under the lock the thread writes with, so no write of the old file follows the flush;
        // the flush also ends the thread's wait_writable()
        this->input_->flush();
        if (this->paused_) {
            this->input_->resume();
            this->paused_ = false;
        }
        if (!this->input_->set_format(info.format)) {
            // The flush dropped what the old file had written, so it is not written on
            this->phase_ = Phase::STOPPED;
            return false;
        }
        this->drain_.reset();
        this->files_.clear();
        this->files_.push_back({std::move(file), info});
        this->placed_.assign(1, {0, {0, 0}});
        this->written_ = 0;
        this->pass_written_ = 0;
        this->phase_ = Phase::FEEDING;
        ++this->generation_;
    }
    this->wake_.notify_all();
    return true;
}

bool Feeder::queue(std::shared_ptr<AudioSource> file) {
    const AudioInfo info = file->info();
    std::optional<Report> begun;
    {
        const std::lock_guard<std::mutex> lock(this->mutex_);
        if (this->phase_ == Phase::CLOSED || this->phase_ == Phase::STOPPED) {
            return false;
        }
        this->files_.push_back({std::move(file), info});
        if (this->phase_ == Phase::FINISHED) {
            // The drain played: the file starts the next run, from position 0
            begun = this->begin_next(0, this->files_.size() - 1);
            if (!begun) {
                this->files_.pop_back();
                return false;
            }
            this->phase_ = Phase::FEEDING;
        }
    }
    this->wake_.notify_all();
    if (begun) {
        this->report(*begun);
    }
    return true;
}

bool Feeder::seek(uint64_t frame) {
    {
        const std::lock_guard<std::mutex> lock(this->mutex_);
        if (this->phase_ == Phase::CLOSED) {
            return false;
        }
        // Under the lock the thread writes with, so no write of the old position follows the flush;
        // the flush also ends the thread's wait_writable()
        this->input_->flush();
        this->drain_.reset();
        this->phase_ = Phase::FEEDING;
        const size_t file = this->placed_.back().spot.file;
        const Spot spot{file, std::min(frame, this->files_[file].info.frames)};
        this->placed_.assign(1, {0, spot});
        this->written_ = 0;
        this->paused_at_ = spot;
        ++this->generation_;
    }
    this->wake_.notify_all();
    return true;
}

void Feeder::set_loop(bool loop) {
    const std::lock_guard<std::mutex> lock(this->mutex_);
    this->loop_ = loop;
}

std::optional<Feeder::Spot> Feeder::pause() {
    const std::lock_guard<std::mutex> lock(this->mutex_);
    if (this->paused_ || (this->placed_.size() == 1 && this->written_ == 0) ||
        (this->phase_ != Phase::FEEDING && this->phase_ != Phase::DRAINING)) {
        return std::nullopt;
    }
    this->paused_ = true;
    this->paused_at_ = this->locate(this->input_->pause());
    return this->paused_at_;
}

bool Feeder::resume() {
    const std::lock_guard<std::mutex> lock(this->mutex_);
    if (!this->paused_) {
        return false;
    }
    this->paused_ = false;
    this->input_->resume();
    return true;
}

int64_t Feeder::time_of(uint64_t frame) const {
    const std::lock_guard<std::mutex> lock(this->mutex_);
    const Placed& placed = this->placed_.back();
    return this->input_->time_of(placed.position + frame - std::min(frame, placed.spot.frame));
}

void Feeder::on_drained(DrainId drain, DrainOutcome outcome) {
    std::optional<Report> report;
    {
        const std::lock_guard<std::mutex> lock(this->mutex_);
        // A drain a seek replaced: FLUSHED, or PLAYED decided just before the seek
        if (this->drain_ != drain || this->stopping_) {
            return;
        }
        this->drain_.reset();
        const size_t file = this->placed_.back().spot.file;
        // A file queued once the drain was issued, or the loop, starts the next run
        const std::optional<size_t> next = this->following(file);
        if (outcome == DrainOutcome::PLAYED && next) {
            report = this->begin_next(0, *next);
            if (!report && *next > file) {
                this->files_.resize(file + 1);
            }
        }
        if (report) {
            this->paused_ = false;
            this->phase_ = Phase::FEEDING;
        } else if (outcome == DrainOutcome::PLAYED) {
            this->phase_ = Phase::FINISHED;
            this->paused_ = false;
            report = Report{Event::PLAYED};
        } else if (outcome == DrainOutcome::STOPPED &&
                   this->input_->state() == InputState::CLOSED) {
            this->phase_ = Phase::CLOSED;
            report = Report{Event::CLOSED};
        } else if (outcome == DrainOutcome::STOPPED) {
            // Otherwise only a stop by the server discards this feeder's drain; acknowledge it
            this->input_->stop();
            this->phase_ = Phase::STOPPED;
            this->paused_ = false;
            report = Report{Event::STOPPED_BY_SERVER};
        }
    }
    this->wake_.notify_all();
    if (report) {
        this->report(*report);
    }
}

Feeder::Status Feeder::status() const {
    const std::lock_guard<std::mutex> lock(this->mutex_);
    Status status;
    status.phase = this->phase_;
    status.paused = this->paused_;
    const Placed& placed = this->placed_.back();
    status.writing = placed.spot.file;
    status.written_to = placed.spot.frame + this->written_;
    status.total = this->files_[placed.spot.file].info.frames;
    status.latency_us = this->input_->latency_us();
    status.input_state = this->input_->state();
    if (this->paused_) {
        status.playing = this->paused_at_;
    } else if (this->phase_ == Phase::FINISHED) {
        status.playing = {placed.spot.file, status.total};
    } else if (this->phase_ == Phase::FEEDING || this->phase_ == Phase::DRAINING) {
        // What is written and not yet played spans latency_us(), at about the writing file's rate
        const uint64_t head = placed.position + this->written_;
        const auto pending = static_cast<uint64_t>(
            std::max<int64_t>(status.latency_us, 0) *
            this->files_[placed.spot.file].info.format.sample_rate / US_PER_SECOND);
        status.playing = this->locate(head - std::min(pending, head));
    } else {
        status.playing = placed.spot;
    }
    return status;
}

Feeder::Spot Feeder::locate(uint64_t position) const {
    const auto later =
        std::upper_bound(this->placed_.begin(), this->placed_.end(), position,
                         [](uint64_t p, const Placed& placed) { return p < placed.position; });
    const Placed& placed = later == this->placed_.begin() ? this->placed_.front() : *(later - 1);
    const uint64_t frames = this->files_[placed.spot.file].info.frames;
    return {placed.spot.file,
            std::min(frames, placed.spot.frame + (position - std::min(position, placed.position)))};
}

std::optional<size_t> Feeder::following(size_t file) const {
    if (file + 1 < this->files_.size()) {
        return file + 1;
    }
    // Starting over after a pass that wrote nothing would write nothing again, forever
    return this->loop_ && this->pass_written_ > 0 ? std::optional<size_t>(0) : std::nullopt;
}

std::optional<Feeder::Report> Feeder::begin_next(uint64_t head, size_t next) {
    const AudioInfo& info = this->files_[next].info;
    // Positions continue on the same run; a file of another format is a new input segment
    if (!(info.format == this->files_[this->placed_.back().spot.file].info.format) &&
        !this->input_->set_format(info.format)) {
        return std::nullopt;
    }
    if (next <= this->placed_.back().spot.file) {
        this->pass_written_ = 0;
    }
    if (head == 0) {
        this->placed_.assign(1, {0, {next, 0}});
    } else {
        if (this->placed_.size() == MAX_PLACED) {
            this->placed_.pop_front();
        }
        this->placed_.push_back({head, {next, 0}});
    }
    this->written_ = 0;
    return Report{Event::NEXT_FILE, next, this->input_->time_of(head)};
}

void Feeder::run() {
    // The block read last: block_frames frames of block_source from block_from. Only this thread
    // reads the files, outside the lock.
    std::vector<uint8_t> block;
    std::shared_ptr<AudioSource> block_source;
    uint64_t block_from = 0;
    uint64_t block_frames = 0;
    std::unique_lock<std::mutex> lock(this->mutex_);
    while (!this->stopping_) {
        if (this->phase_ != Phase::FEEDING) {
            this->wake_.wait(lock);
            continue;
        }
        const Placed& placed = this->placed_.back();
        File& file = this->files_[placed.spot.file];
        const uint64_t next = placed.spot.frame + this->written_;
        if (next >= file.info.frames) {
            // The next file on the same run, unless the input takes no further segment now: then
            // after the drain, as the next run
            const std::optional<size_t> following = this->following(placed.spot.file);
            if (following) {
                const std::optional<Report> report =
                    this->begin_next(placed.position + this->written_, *following);
                if (report) {
                    lock.unlock();
                    this->report(*report);
                    lock.lock();
                    continue;
                }
            }
            this->phase_ = Phase::DRAINING;
            this->drain_ = this->input_->drain();
            lock.unlock();
            this->report({Event::END_OF_FILE});
            lock.lock();
            continue;
        }
        const size_t frame_size = frame_bytes(file.info.format);
        if (block_source != file.source || next < block_from || next >= block_from + block_frames) {
            block_source = file.source;
            block_from = next;
            const uint64_t want = std::min(BLOCK_FRAMES, file.info.frames - next);
            block.resize(want * frame_size);
            const uint64_t generation = this->generation_;
            lock.unlock();
            block_frames = block_source->read(next, block.data(), want);
            lock.lock();
            if (block_frames == 0 && generation == this->generation_) {
                // The file cannot be read on: it ends here
                this->files_[this->placed_.back().spot.file].info.frames = next;
            }
            // A play() or seek() meanwhile leaves the block unused; a queue() may have moved file
            continue;
        }
        const uint64_t frames = block_from + block_frames - next;
        const WriteResult result =
            this->input_->write(block.data() + (next - block_from) * frame_size, frames);
        this->written_ += result.frames;
        this->pass_written_ += result.frames;
        if (result.frames == frames) {
            continue;
        }
        if (result.state == InputState::OPEN || result.state == InputState::FULL) {
            lock.unlock();
            this->input_->wait_writable(std::numeric_limits<int64_t>::max());
            lock.lock();
        } else if (result.state == InputState::STOP_LATCHED) {
            this->input_->stop();
            this->phase_ = Phase::STOPPED;
            this->paused_ = false;
            lock.unlock();
            this->report({Event::STOPPED_BY_SERVER});
            lock.lock();
        } else if (result.state == InputState::CLOSED) {
            this->phase_ = Phase::CLOSED;
            lock.unlock();
            this->report({Event::CLOSED});
            lock.lock();
        } else {
            // DRAINING: only this feeder drains, and it stops writing first
            this->phase_ = Phase::DRAINING;
        }
    }
}

void Feeder::report(const Report& report) {
    if (this->on_event_) {
        this->on_event_(report);
    }
}

}  // namespace sendspin::server::examples
