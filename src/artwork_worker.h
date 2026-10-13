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

/// @file artwork_worker.h
/// @brief The artwork worker: a thread of its own that decodes the host's images, encodes them at
/// each channel's format and size and derives their palettes, so core never runs an encoder. Core
/// posts jobs and gets each result back through a callback.

#pragma once

#include "artwork.h"
#include "image_codec.h"
#include "memory.h"
#include "ring.h"
#include "sendspin/server/types.h"

#include <array>
#include <compare>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

namespace sendspin::server {

/// @brief One encoding of a host's image: the server's cache key
struct ArtworkVariant {
    std::array<uint8_t, 32> hash{};
    ImageFormat format{ImageFormat::JPEG};
    uint32_t width{0};
    uint32_t height{0};

    friend auto operator<=>(const ArtworkVariant&, const ArtworkVariant&) = default;
};

/// @brief What the worker is asked for an image
struct ArtworkJob {
    std::shared_ptr<const SourceImage> image;
    /// The encoding asked for; nullopt for the image's palette
    std::optional<ArtworkVariant> variant;
};

struct ArtworkResult {
    ArtworkJob job;
    /// With a variant: the encoded image; nullopt when the image did not decode or encode
    std::optional<std::vector<uint8_t>> encoded;
    /// Without one: the image's colors (palette_colors()), empty when it did not decode
    Colors colors;
};

/// @brief Runs job on decoded, the job's image decoded, or nullptr when it did not decode
ArtworkResult run_artwork_job(const ArtworkJob& job, const DecodedImage* decoded);

/// @brief The palette quantize_palette() takes from an image for palette_colors(), and every
/// how many pixels it samples
constexpr size_t PALETTE_COLORS = 5;
constexpr size_t PALETTE_QUALITY = 10;

class ArtworkWorker {
public:
    /// @brief The jobs queued at most. Each is one image's encoding for one channel
    /// configuration or its palette; a job past it is refused, and core asks again when a result
    /// comes back.
    static constexpr size_t MAX_QUEUED_JOBS = 16;

    /// @brief Called on the worker's thread with each result, in the order the jobs were posted
    using Deliver = std::function<void(ArtworkResult)>;

    /// @brief Starts the thread
    explicit ArtworkWorker(Deliver deliver);
    /// @brief stop()
    ~ArtworkWorker();
    ArtworkWorker(const ArtworkWorker&) = delete;
    ArtworkWorker& operator=(const ArtworkWorker&) = delete;

    /// @brief Queues job; any thread
    /// @return false when MAX_QUEUED_JOBS are queued or the worker was stopped
    bool post(ArtworkJob job);

    /// @brief Discards the jobs queued and refuses later ones; the job running finishes and is
    /// delivered. Never waits, so core may call it.
    void discard();

    /// @brief discard(), then waits for the thread to exit, which waits for the job in progress
    /// to finish; never from the worker's thread
    void stop();

private:
    void run();

    Deliver deliver_;
    std::mutex mutex_;
    std::condition_variable wake_;
    Ring<ArtworkJob> jobs_{MAX_QUEUED_JOBS, MAX_QUEUED_JOBS, MemoryLocation::PREFER_EXTERNAL};
    bool stopping_{false};
    std::thread thread_;
};

}  // namespace sendspin::server
