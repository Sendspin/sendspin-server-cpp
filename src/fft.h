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

/// @file fft.h
/// @brief The visualizer's real FFT: a seam over the platform's FFT library, single precision.
/// The host build implements it with pffft (fft_pffft.cpp); another platform supplies its own
/// source for this header (ESP-IDF, esp-dsp in esp/fft_esp_dsp.cpp), chosen by CMake.

#pragma once

#include <cstddef>
#include <memory>
#include <span>

namespace sendspin::server {

/// @brief A forward real FFT of one size, transformed in place. Every buffer it uses is allocated
/// by create(), so a transform allocates nothing. An instance is used by one thread at a time;
/// create() may run on several threads at once, so a backend with global tables initializes them
/// exactly once (std::call_once).
class RealFft {
public:
    /// The sizes every implementation supports: the powers of two from MIN_SIZE to MAX_SIZE
    static constexpr size_t MIN_SIZE = 256;
    static constexpr size_t MAX_SIZE = 8192;

    /// @return nullptr for a size outside the supported ones, or when allocation fails
    static std::unique_ptr<RealFft> create(size_t size);

    ~RealFft();
    RealFft(const RealFft&) = delete;
    RealFft& operator=(const RealFft&) = delete;

    size_t size() const {
        return this->size_;
    }

    /// @brief The transform's input, size() samples, which the caller fills (windowed) before
    /// each power()
    std::span<float> input();

    /// @brief Transforms input(), overwriting it, into the power `|X[k]|^2` for `k = 0 .. size / 2`
    /// of its DFT, unnormalized: a constant 1 gives size()^2 at k = 0, and a cosine of amplitude 1
    /// on bin k gives (size() / 2)^2 there.
    /// @return size() / 2 + 1 values in the FFT's own storage, which the caller may scale in place;
    /// valid until the next input() is filled, and never past the RealFft's destruction
    std::span<float> power();

private:
    struct Backend;

    RealFft(size_t size, std::unique_ptr<Backend> backend);

    size_t size_;
    std::unique_ptr<Backend> backend_;
};

}  // namespace sendspin::server
