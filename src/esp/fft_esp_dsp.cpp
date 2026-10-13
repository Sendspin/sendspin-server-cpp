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

/// @file fft_esp_dsp.cpp
/// @brief RealFft on esp-dsp, the ESP-IDF implementation of fft.h: a size-N real transform as an
/// N/2-point complex FFT of the even and odd samples, then the split that separates their spectra.
/// The complex FFT is radix-4 (dsps_fft4r_fc32) where N/2 is a power of four up to 1024 (N = 512
/// and 2048, the window at 44.1 and 48 kHz), and radix-2 (dsps_fft2r_fc32) otherwise

#include "fft.h"
#include "memory.h"
#include <dsps_fft2r.h>
#include <dsps_fft4r.h>

#include <bit>
#include <cmath>
#include <mutex>
#include <numbers>

#if CONFIG_DSP_MAX_FFT_SIZE < 4096
#error "RealFft's 8192-point transform needs CONFIG_DSP_MAX_FFT_SIZE of at least 4096"
#endif

namespace sendspin::server {

namespace {

/// The largest complex size the radix-4 path serves: its global table costs 16 bytes a point,
/// 16 KB here
constexpr size_t RADIX4_MAX_POINTS = 1024;

/// esp-dsp's twiddle tables are global, built once: radix-2's for the largest complex size,
/// which serves every smaller one, and radix-4's for RADIX4_MAX_POINTS. Another component may
/// have built either first, perhaps smaller, so each serves only the sizes its table covers
struct Tables {
    size_t radix2_points{0};
    size_t radix4_points{0};
};

const Tables& tables() {
    static Tables built;
    static std::once_flag once;
    std::call_once(once, [] {
        // The project's full CONFIG_DSP_MAX_FFT_SIZE, not just the 4096 points create() needs:
        // esp-dsp never grows a table built smaller, so a smaller one would fail another
        // component that raised the setting for a larger transform
        dsps_fft2r_init_fc32(nullptr, CONFIG_DSP_MAX_FFT_SIZE);
        dsps_fft4r_init_fc32(nullptr, RADIX4_MAX_POINTS);
        if (dsps_fft2r_initialized != 0) {
            built.radix2_points = static_cast<size_t>(dsps_fft_w_table_size);
        }
        if (dsps_fft4r_initialized != 0) {
            // dsps_fft4r_init_fc32() keeps two table entries a point
            built.radix4_points = static_cast<size_t>(dsps_fft4r_w_table_size) / 2;
        }
    });
    return built;
}

/// The largest transform whose buffers prefer internal RAM: 12 KB at 2048 points; at 8192 four
/// analyses would take about 192 KB of it for a 1-3% gain
constexpr size_t INTERNAL_MAX_SIZE = 2048;

/// 16-byte aligned floats for a size-point transform, in internal RAM for one up to
/// INTERNAL_MAX_SIZE while it has room (PSRAM is slower), else in PSRAM
float* alloc_floats(size_t count, size_t size) {
    return static_cast<float*>(placed_malloc(count * sizeof(float), 16,
                                             size <= INTERNAL_MAX_SIZE
                                                 ? MemoryLocation::PREFER_INTERNAL
                                                 : MemoryLocation::PREFER_EXTERNAL));
}

}  // namespace

struct RealFft::Backend {
    /// N floats: the real input, then the N/2 complex points transformed in place; 16-byte
    /// aligned for esp-dsp's vector paths
    float* buffer{nullptr};
    /// (cos, sin) of 2 pi k / N for k = 1 .. N/4, the split's twiddles
    float* twiddles{nullptr};
    /// The complex FFT is radix-4, else radix-2
    bool radix4{false};

    Backend() = default;
    Backend(const Backend&) = delete;
    Backend& operator=(const Backend&) = delete;

    ~Backend() {
        placed_free(this->twiddles);
        placed_free(this->buffer);
    }
};

std::unique_ptr<RealFft> RealFft::create(size_t size) {
    if (size < MIN_SIZE || size > MAX_SIZE || (size & (size - 1)) != 0 ||
        tables().radix2_points < size / 2) {
        return nullptr;
    }
    auto backend = std::make_unique<Backend>();
    const size_t points = size / 2;
    backend->radix4 = (std::countr_zero(points) % 2) == 0 && points <= tables().radix4_points;
    backend->buffer = alloc_floats(size, size);
    backend->twiddles = alloc_floats(size / 2, size);
    if (backend->buffer == nullptr || backend->twiddles == nullptr) {
        return nullptr;
    }
    for (size_t k = 1; k <= size / 4; ++k) {
        const double angle =
            2.0 * std::numbers::pi * static_cast<double>(k) / static_cast<double>(size);
        backend->twiddles[2 * (k - 1)] = static_cast<float>(std::cos(angle));
        backend->twiddles[(2 * (k - 1)) + 1] = static_cast<float>(std::sin(angle));
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
    float* data = this->backend_->buffer;
    const float* twiddles = this->backend_->twiddles;
    const size_t half = this->size_ / 2;
    // z[m] = x[2m] + i x[2m + 1], already laid out as interleaved complex: Z = FFT(z)
    if (this->backend_->radix4) {
        dsps_fft4r_fc32(data, static_cast<int>(half));
        dsps_bit_rev4r_fc32(data, static_cast<int>(half));
    } else {
        dsps_fft2r_fc32(data, static_cast<int>(half));
        dsps_bit_rev2r_fc32(data, static_cast<int>(half));
    }
    // X[0] and X[N/2] are real: (Re Z[0] + Im Z[0], Re Z[0] - Im Z[0]), kept as pffft's layout
    const float z0_re = data[0];
    const float z0_im = data[1];
    data[0] = z0_re + z0_im;
    data[1] = z0_re - z0_im;
    // For k = 1 .. N/4, with A = Z[k], B = Z[N/2 - k], E = (A + conj B) / 2,
    // O = (A - conj B) / 2i and W = e^(-2 pi i k / N): X[k] = E + W O and
    // X[N/2 - k] = conj(E - W O). Each pair is read before either is written
    for (size_t k = 1; k <= half / 2; ++k) {
        float* a = data + (2 * k);
        float* b = data + (2 * (half - k));
        const float e_re = 0.5F * (a[0] + b[0]);
        const float e_im = 0.5F * (a[1] - b[1]);
        const float o_re = 0.5F * (a[1] + b[1]);
        const float o_im = -0.5F * (a[0] - b[0]);
        const float c = twiddles[2 * (k - 1)];
        const float s = twiddles[(2 * (k - 1)) + 1];
        const float t_re = (c * o_re) + (s * o_im);
        const float t_im = (c * o_im) - (s * o_re);
        a[0] = e_re + t_re;
        a[1] = e_im + t_im;
        b[0] = e_re - t_re;
        b[1] = t_im - e_im;
    }
    // Written in place in k order: data[k] holds a value of bin k / 2, read before
    const float dc = data[0] * data[0];
    const float nyquist = data[1] * data[1];
    for (size_t k = 1; k < half; ++k) {
        const float re = data[2 * k];
        const float im = data[(2 * k) + 1];
        data[k] = (re * re) + (im * im);
    }
    data[0] = dc;
    data[half] = nyquist;
    return {data, half + 1};
}

}  // namespace sendspin::server
