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

/// @file fft_pffft.cpp
/// @brief RealFft on pffft, the host's implementation of fft.h

#include "fft.h"

#include <pffft.h>

namespace sendspin::server {

struct RealFft::Backend {
    PFFFT_Setup* setup{nullptr};
    /// pffft's SIMD paths need 16-byte aligned buffers, which pffft_aligned_malloc() gives; the
    /// transform runs in place in buffer (pffft.h: input and output may alias)
    float* buffer{nullptr};
    float* work{nullptr};

    Backend() = default;
    Backend(const Backend&) = delete;
    Backend& operator=(const Backend&) = delete;

    ~Backend() {
        pffft_aligned_free(this->work);
        pffft_aligned_free(this->buffer);
        if (this->setup != nullptr) {
            pffft_destroy_setup(this->setup);
        }
    }
};

// pffft keeps no global state: each setup holds its own twiddles, so create() is safe on any
// thread
std::unique_ptr<RealFft> RealFft::create(size_t size) {
    if (size < MIN_SIZE || size > MAX_SIZE || (size & (size - 1)) != 0) {
        return nullptr;
    }
    auto backend = std::make_unique<Backend>();
    backend->setup = pffft_new_setup(static_cast<int>(size), PFFFT_REAL);
    const size_t bytes = size * sizeof(float);
    backend->buffer = static_cast<float*>(pffft_aligned_malloc(bytes));
    backend->work = static_cast<float*>(pffft_aligned_malloc(bytes));
    if (backend->setup == nullptr || backend->buffer == nullptr || backend->work == nullptr) {
        return nullptr;
    }
    return std::unique_ptr<RealFft>(new RealFft(size, std::move(backend)));
}

RealFft::RealFft(size_t size, std::unique_ptr<Backend> backend)
    : size_(size), backend_(std::move(backend)) {}

RealFft::~RealFft() = default;

std::span<float> RealFft::input() {
    return {this->backend_->buffer, this->size_};
}

std::span<float> RealFft::power() {
    Backend& backend = *this->backend_;
    float* out = backend.buffer;
    pffft_transform_ordered(backend.setup, out, out, backend.work, PFFFT_FORWARD);
    // The ordered real layout: X[0] and X[N/2] (both real) first, then X[k] as (re, im) at 2k.
    // Written in place in k order: out[k] holds a value of bin k / 2, read before
    const size_t half = this->size_ / 2;
    const float dc = out[0] * out[0];
    const float nyquist = out[1] * out[1];
    for (size_t k = 1; k < half; ++k) {
        const float re = out[2 * k];
        const float im = out[(2 * k) + 1];
        out[k] = (re * re) + (im * im);
    }
    out[0] = dc;
    out[half] = nyquist;
    return {out, half + 1};
}

}  // namespace sendspin::server
