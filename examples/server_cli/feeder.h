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

/// @file feeder.h
/// @brief Feeder, a thread that plays audio files through a GroupInput

#pragma once

#include "audio_source.h"

#include <sendspin/server/server.h>

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

namespace sendspin::server::examples {

/// @brief Writes audio files into a GroupInput on its own thread, one after another on the same run
/// (each file of another format starts a new input segment with GroupInput::set_format()), waiting
/// while the input is full, and drains the input after the last; looping, it goes on from the
/// first file instead, on the same run. Files are read a block at a time as they are written.
/// Seeking flushes the input and continues the file being written from the new position; play()
/// jumps to another file the same way. Every method may be called from any thread.
class Feeder {
public:
    enum class Phase : uint8_t {
        /// Writing a file
        FEEDING,
        /// The last file is written; a drain is pending
        DRAINING,
        /// The drain played
        FINISHED,
        /// The server stopped the group's playback, and the feeder acknowledged it
        STOPPED,
        /// The input is closed
        CLOSED,
    };

    /// @brief What the feeder reports as it happens: the next file's writing begun, the end of the
    /// last file reached and a drain issued, then the drain played; or a stop by the server, or
    /// the input closed
    enum class Event : uint8_t { NEXT_FILE, END_OF_FILE, PLAYED, STOPPED_BY_SERVER, CLOSED };

    struct Report {
        Event event{Event::END_OF_FILE};
        /// With NEXT_FILE: the file begun, its index among those given since the last play() (0
        /// again when a loop starts over), and when its first frame plays, on the server's clock
        size_t file{0};
        int64_t at_us{0};
    };

    /// @brief A file and a frame of it
    struct Spot {
        size_t file{0};
        uint64_t frame{0};
    };

    struct Status {
        Phase phase{Phase::FEEDING};
        bool paused{false};
        /// The file and frame playing now; estimated from latency_us while playing
        Spot playing;
        /// The file being written, the frame its next write starts at and its length
        size_t writing{0};
        uint64_t written_to{0};
        uint64_t total{0};
        int64_t latency_us{0};
        InputState input_state{InputState::OPEN};
    };

    /// @param file read only on the feeder's thread from now on
    /// @param on_event called on the feeder's thread or from on_drained(), with no lock held; it
    /// must not destroy the feeder or call it
    /// @param loop set_loop()
    Feeder(std::shared_ptr<GroupInput> input, std::shared_ptr<AudioSource> file,
           std::function<void(const Report&)> on_event, bool loop);
    /// @brief Stops the input, then joins the thread and releases the input. Waits for a read in
    /// progress: a FLAC seek without a seek table decodes up to the whole file.
    ~Feeder();
    Feeder(const Feeder&) = delete;
    Feeder& operator=(const Feeder&) = delete;

    InputId input_id() const;

    /// @brief The format and length of the file given index-th since the last play()
    AudioInfo info(size_t file) const;

    /// @brief The file being written
    size_t writing() const;

    /// @brief Jumps to file, dropping the files queued: flushes the input (a track jump, spec
    /// messaging.md "stream/clear"), takes the file's format and writes it from its start; resumes
    /// a paused input and restarts a finished or stopped one
    /// @return false once the input is closed, or for a format the input refuses, which leaves the
    /// feeder stopped
    bool play(std::shared_ptr<AudioSource> file);

    /// @brief Adds file after the last one given: written on the same run when it comes before
    /// the drain, else as the next run once the drain played
    /// @return false once the input is closed or the server stopped its playback
    bool queue(std::shared_ptr<AudioSource> file);

    /// @brief Whether the end of the last file goes on from the start of the first, as the next
    /// file would (a gapless loop on the same run), rather than draining; applies from the next end
    /// of a file
    void set_loop(bool loop);

    /// @brief Flushes the input and continues writing the file being written from frame;
    /// restarts a finished or stopped one
    /// @return false once the input is closed
    bool seek(uint64_t frame);

    /// @return the file and frame paused at, or nullopt when nothing is playing
    std::optional<Spot> pause();

    /// @return false when not paused
    bool resume();

    /// @brief Takes the ServerListener::on_drained() of the input
    void on_drained(DrainId drain, DrainOutcome outcome);

    /// @brief When frame of the file being written plays, as GroupInput::time_of() gives it for
    /// the frame's input position; frame at or after where that file's writing began or last
    /// seeked to
    int64_t time_of(uint64_t frame) const;

    Status status() const;

private:
    struct File {
        std::shared_ptr<AudioSource> source;
        /// Its frames end where the file could not be read on
        AudioInfo info;
    };

    /// Where a file's writing began since the last seek: its frame at input position
    struct Placed {
        uint64_t position{0};
        Spot spot;
    };

    void run();
    void report(const Report& report);
    /// The file and frame at an input position since the last seek
    Spot locate(uint64_t position) const;
    /// The file after file: the next one given, else the first when looping and the pass that
    /// ends wrote a frame
    std::optional<size_t> following(size_t file) const;
    /// Begins file next at input position head, 0 starting a new run: a new segment when its
    /// format differs from the file written before
    /// @return nullopt when the input refuses the segment
    std::optional<Report> begin_next(uint64_t head, size_t next);

    std::shared_ptr<GroupInput> input_;
    std::function<void(const Report&)> on_event_;

    mutable std::mutex mutex_;
    std::condition_variable wake_;
    /// Every file given since the last play()
    std::vector<File> files_;
    bool loop_;
    Phase phase_{Phase::FEEDING};
    bool paused_{false};
    bool stopping_{false};
    /// The files written since the last seek, in order, the last MAX_PLACED of them; the last is
    /// the file being written
    std::deque<Placed> placed_;
    /// Frames of the file being written since placed_.back()
    uint64_t written_{0};
    /// Frames written since the loop last started over at the first file, or since play(); a
    /// pass that wrote none is not looped, so files with no frame end the run
    uint64_t pass_written_{0};
    Spot paused_at_;
    /// The drain issued at the end of the last file and not yet answered; a seek forgets it
    std::optional<DrainId> drain_;
    /// Counts play() and seek(), so a block read outside the lock for a place they replaced is not
    /// written
    uint64_t generation_{0};
    std::thread thread_;
};

}  // namespace sendspin::server::examples
