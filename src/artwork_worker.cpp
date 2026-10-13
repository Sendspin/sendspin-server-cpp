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

#include "artwork_worker.h"

#include "colors.h"
#include "thread_setup.h"

#include <utility>

namespace sendspin::server {

ArtworkResult run_artwork_job(const ArtworkJob& job, const DecodedImage* decoded) {
    ArtworkResult result{job, std::nullopt, {}};
    if (decoded == nullptr) {
        return result;
    }
    if (job.variant) {
        result.encoded =
            render_image(*decoded, job.variant->format, job.variant->width, job.variant->height);
    } else {
        const std::vector<RgbColor> candidates =
            quantize_palette(decoded->rgba(), PALETTE_COLORS, PALETTE_QUALITY);
        result.colors = palette_colors(candidates);
    }
    return result;
}

ArtworkWorker::ArtworkWorker(Deliver deliver)
    : deliver_(std::move(deliver)),
      thread_(start_thread(ThreadRole::ARTWORK, [this] { this->run(); })) {}

ArtworkWorker::~ArtworkWorker() {
    this->stop();
}

bool ArtworkWorker::post(ArtworkJob job) {
    {
        const std::lock_guard<std::mutex> lock(this->mutex_);
        if (this->stopping_ || !this->jobs_.try_push(std::move(job))) {
            return false;
        }
    }
    this->wake_.notify_one();
    return true;
}

void ArtworkWorker::discard() {
    Ring<ArtworkJob> discarded;
    {
        const std::lock_guard<std::mutex> lock(this->mutex_);
        this->stopping_ = true;
        discarded.swap(this->jobs_);
    }
    this->wake_.notify_one();
}

void ArtworkWorker::stop() {
    this->discard();
    if (this->thread_.joinable()) {
        this->thread_.join();
    }
}

void ArtworkWorker::run() {
    // The last image decoded, kept while jobs for it follow one another, so its encodings for
    // several channels and its palette decode it once; released when the queue empties
    std::optional<std::array<uint8_t, 32>> decoded_hash;
    std::optional<DecodedImage> decoded;
    for (;;) {
        ArtworkJob job;
        {
            std::unique_lock<std::mutex> lock(this->mutex_);
            if (this->jobs_.empty()) {
                lock.unlock();
                decoded.reset();
                decoded_hash.reset();
                lock.lock();
            }
            this->wake_.wait(lock, [this] { return this->stopping_ || !this->jobs_.empty(); });
            if (this->stopping_) {
                return;
            }
            job = std::move(this->jobs_.front());
            this->jobs_.pop_front();
        }
        if (decoded_hash != job.image->hash) {
            // The image before goes first, so one decoded image is held at a time
            decoded.reset();
            decoded = decode_image(job.image->bytes);
            decoded_hash = job.image->hash;
        }
        this->deliver_(run_artwork_job(job, decoded ? &*decoded : nullptr));
    }
}

}  // namespace sendspin::server
