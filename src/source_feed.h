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

/// @file source_feed.h
/// @brief SourceFeed: how core hands a routed source's input stream (its starts, chunks and ends)
/// to the source's decoder on a group audio thread, and how the decoder reports back what it
/// could not take

#pragma once

#include "audio_workers.h"
#include "byte_ring.h"
#include "messages.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <vector>

namespace sendspin::server {

/// @brief One step of a source's input stream, as core received it
struct SourceItem {
    enum class Kind : uint8_t {
        /// A stream opened, or its format replaced in place; the bytes are its codec_header
        START,
        /// A chunk; the bytes are its encoded audio
        CHUNK,
        /// The stream ended
        END,
    };

    Kind kind{Kind::START};
    /// The connection (ServerCore's id for it) and its stream (SourceStream::stream_number())
    uint64_t connection{0};
    uint64_t stream{0};
    /// START
    AudioFormat format;
    /// CHUNK: the server-clock time its first sample was captured, and its receive stamp
    int64_t capture_us{0};
    int64_t rx_us{0};
};

/// @brief What the decoder could not take
struct SourceFault {
    enum class Kind : uint8_t {
        /// A chunk that is not whole codec units of the stream's format, or longer than
        /// MAX_SOURCE_CHUNK_US: the client broke roles/source/v1.md "client-stream/start" or
        /// "Source Audio Chunks (Binary)"
        PROTOCOL,
        /// The stream is one the server cannot take: its decoder or its group's input refused it
        UNSUPPORTED,
    };

    Kind kind{Kind::PROTOCOL};
    uint64_t connection{0};
    uint64_t stream{0};
};

/// @brief A bounded FIFO of a source's items, copied in by core and read in place by the
/// decoder, under one mutex that is a leaf; and the faults the decoder reports, which core takes.
/// A push that finds no room is refused, and a refused chunk is a gap in the source's audio. Core,
/// the one producer, copies past the tail outside the mutex and records the item under it, so the
/// oldest item's bytes stay as they are while the decoder reads them outside the mutex.
class SourceFeed {
public:
    /// @brief The faults held at once; more before core takes them are dropped, one of each
    /// stream being all core acts on
    static constexpr size_t MAX_FAULTS = 4;

    /// @param bytes the item storage; an item of up to this size fits once the feed is empty
    /// @param items the most items held at once
    SourceFeed(size_t bytes, size_t items);
    SourceFeed(const SourceFeed&) = delete;
    SourceFeed& operator=(const SourceFeed&) = delete;

    /// @brief The decoder's task, woken by each push. Core thread, once, before the first push.
    void attach(std::weak_ptr<AudioWorkers::Task> task) {
        this->task_ = std::move(task);
    }

    /// @brief Copies an item for the decoder and wakes its task. Core thread only; never
    /// allocates.
    /// @return false when it has no room
    bool push(const SourceItem& item, std::span<const uint8_t> bytes);

    /// @brief The oldest item and its bytes, valid until pop(). Decoder only.
    /// @return false when there is none
    bool front(SourceItem& item, std::span<const uint8_t>& bytes) const;

    /// @brief Removes the oldest item. Decoder only.
    void pop();

    /// @brief Whether an item waits
    bool empty() const;

    /// @brief The largest item's bytes the feed holds
    size_t capacity() const {
        return this->items_.capacity();
    }

    /// @brief Keeps a fault for core. Decoder only; the decoder then signals core.
    void report(const SourceFault& fault);

    /// @brief The faults reported since the last call. Core thread.
    std::vector<SourceFault> take_faults();

private:
    std::weak_ptr<AudioWorkers::Task> task_;

    mutable std::mutex mutex_;
    ByteRing<SourceItem> items_;
    std::vector<SourceFault> faults_;
};

}  // namespace sendspin::server
