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

/// @file inbound_hold.h
/// @brief InboundHold: one connection's receive side in front of an InboundRing, for a transport
/// whose receive task cannot simply stop reading one connection: the message it assembles, held
/// when the ring refuses it or core has not started the connection, pushed by whichever task finds
/// room, and the connection's close, raised after its last push. Platform-free; the ESP-IDF
/// transport (src/esp/ws_link.*) is its glue.

#pragma once

#include "memory.h"
#include "transport.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <span>
#include <vector>

namespace sendspin::server {

/// @brief A connection's assembled message and its way into the inbound ring, under one mutex that
/// is a leaf (never held across a push, a park or a raise).
///
/// The receive task assembles a message in the buffer begin_message() gives, then offer()s it.
/// The ring refusing it, or core not having started the connection, leaves it held: the receive
/// task then receives nothing more for the connection (it pauses it, or waits) until the held
/// message is released, pushed or dropped, which resume() does from any task. Each held message
/// has a generation, and a pause belongs to one: only the release of its own message ends it, so
/// a resume that finds nothing held ends none. The buffer is the receive task's whenever nothing
/// is held; a pusher reads it only while the message is held and it is the one pushing.
class InboundHold {
public:
    /// @brief What became of a message offered or resumed
    enum class Result : uint8_t {
        PUSHED,   ///< Released into the ring
        HELD,     ///< Still held: nothing more may be received for the connection
        DROPPED,  ///< Released without a push: the connection is closing
        NOTHING,  ///< No message held, or another task is pushing it and will try again
    };

    struct Outcome {
        Result result;
        /// The held message's generation; 0 for NOTHING
        uint64_t generation;
        /// For PUSHED and DROPPED: the pause of this message to end, or nullptr
        void* unpause;
    };

    /// @brief Pushes a message into the ring; false when the ring refuses it
    using PushFn =
        std::function<bool(MessageKind kind, std::span<const uint8_t> payload, int64_t rx_us)>;

    /// @param push into the ring, for the connection's endpoint
    /// @param park registers the connection to be resume()d once the ring takes pushes again;
    /// called at most once between two resume()s
    /// @param raise_closed raises CLOSED in the ring, after the connection's last push
    /// @param kept_bytes the buffer capacity kept between messages; a larger one is freed
    InboundHold(PushFn push, std::function<void()> park,
                std::function<void(CloseReason reason)> raise_closed, size_t kept_bytes);
    InboundHold(const InboundHold&) = delete;
    InboundHold& operator=(const InboundHold&) = delete;

    // ---- Receive task ----

    /// @brief The buffer to assemble the next message in, emptied (and freed when it grew past
    /// kept_bytes)
    /// @return nullptr while a message is held or being pushed: the receive task received
    /// although it was to stop, and the connection ends
    ExternalBytes* begin_message();

    /// @brief The buffer of the message being assembled
    ExternalBytes& message() {
        return this->message_;
    }

    /// @brief Holds the assembled message, its first size bytes, and offers it to the ring;
    /// DROPPED while another is held or being pushed
    Outcome offer(MessageKind kind, size_t size, int64_t rx_us);

    /// @brief After offer() or resume() gave HELD for generation: records the pause token that
    /// keeps the connection from being received, for the release of that message to return
    /// @return false when that message is released already: the caller ends the pause itself
    bool pause(uint64_t generation, void* token);

    /// @brief Whether the message of generation was released
    bool released(uint64_t generation);

    // ---- Any task ----

    /// @brief Core bound the connection's endpoint: a close reported before is raised now
    void bind();

    /// @brief Core started the connection: a held message may be pushed (the caller resumes)
    void start();

    /// @brief The connection is closing: a held message, and every later one, is dropped (the
    /// caller resumes)
    void drop();

    /// @brief Pushes the held message, or drops it after drop(); NOTHING when none is held or
    /// another task is pushing it (that task tries once more before it holds it)
    Outcome resume();

    /// @brief The transport's close; the first report wins. Raised once bound and once no
    /// message is held or being pushed.
    void report_closed(CloseReason reason);

    /// @brief The pause recorded, for a connection whose transport ends; nullptr when none
    void* take_pause();

private:
    /// Under the mutex, with a message held and no task pushing it: pushes or drops it, retrying
    /// while a resume came during a refused push
    Outcome release(std::unique_lock<std::mutex>& lock);
    /// Under the mutex: the close to raise now, marked raised
    std::optional<CloseReason> close_to_raise();

    const PushFn push_;
    const std::function<void()> park_;
    const std::function<void(CloseReason reason)> raise_closed_;
    const size_t kept_bytes_;

    /// In PSRAM where there is one: up to MAX_MESSAGE_BYTES per connection
    ExternalBytes message_;

    std::mutex mutex_;
    /// The held message: its size, kind, stamp and generation
    bool held_{false};
    size_t size_{0};
    MessageKind kind_{MessageKind::BINARY};
    int64_t rx_us_{0};
    uint64_t generation_{0};
    /// The last generation released
    uint64_t released_{0};
    /// A task is pushing the held message, outside the mutex
    bool pushing_{false};
    /// A resume came while it was: the pusher tries once more
    bool resume_requested_{false};
    /// park() was called and no resume() has come since
    bool parked_{false};
    bool bound_{false};
    bool started_{false};
    bool dropping_{false};
    void* pause_{nullptr};
    uint64_t pause_generation_{0};
    std::optional<CloseReason> close_reason_;
    bool close_raised_{false};
};

}  // namespace sendspin::server
