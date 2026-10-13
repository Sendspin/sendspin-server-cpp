// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Kevin Ahrendt

///////////////////////////////////////////////////////////////////////////////
/// @file cycle_profile.h
/// @brief Compile-time gated cycle profiler for the process_block hot loop
///
/// Accumulates per-section CCOUNT deltas so the fixed per-output-sample
/// overhead can be attributed (phase tracking vs. dot-product kernel vs.
/// conversion bookkeeping). Compiled out entirely unless the build defines
/// RESAMPLER_PROFILE_CYCLES for BOTH the library and the app; the benchmark
/// example's esp32s3-profile PlatformIO environment does this via CMake.
///
/// ESP-only: probes read the CPU cycle counter directly (Xtensa CCOUNT, or
/// the RISC-V machine performance counter via esp_cpu_get_cycle_count()).
/// Each probe pair costs a handful of cycles, so every section reading is
/// inflated by roughly one probe (~5-10 cycles); the benchmark prints a
/// calibration estimate alongside the results.
///////////////////////////////////////////////////////////////////////////////

#ifndef CYCLE_PROFILE_H
#define CYCLE_PROFILE_H

#include <cstdint>

#if defined(RESAMPLER_PROFILE_CYCLES) && \
    (defined(__XTENSA__) || (defined(__riscv) && defined(ESP_PLATFORM)))
#define MR_PROFILE_ENABLED 1
#else
#define MR_PROFILE_ENABLED 0
#endif

#if MR_PROFILE_ENABLED && defined(__riscv)
#include "esp_cpu.h"
#endif

namespace micro_resampler {
namespace profile {

// Section accumulators and event counts for one profiling window. All fields
// are 32-bit: a 5-second run at 240 MHz stays well under wrap for any one
// section as long as the caller resets between configurations.
struct CycleCounters {
    // Section accumulators (CPU cycles)
    uint32_t phase_calc = 0;   // calculate_phase, every loop pass
    uint32_t consume = 0;      // consume-branch bookkeeping (update_after_consume)
    uint32_t prep = 0;         // wrap check + lazy conversion + pointer math
    uint32_t kernel = 0;       // dotprod::compute() proper (dispatch + asm kernel)
    uint32_t maxtap = 0;       // apply_max_taps portion of the kernel section
    uint32_t frame = 0;        // interpolate_frame_impl total (incl. kernel section)
    uint32_t post = 0;         // update_after_output + loop bookkeeping
    uint32_t block_fixup = 0;  // end-of-block conversion + history memmove + finalize
    uint32_t total = 0;        // whole process_block_inner

    // Event counts
    uint32_t iterations = 0;     // loop passes (emit passes + consume passes)
    uint32_t consume_iters = 0;  // passes that took the consume branch
    uint32_t outputs = 0;        // output frames generated
    uint32_t kernel_calls = 0;   // dotprod kernel invocations
    uint32_t blocks = 0;         // process_block calls

    void reset() {
        *this = CycleCounters{};
    }
};

#if MR_PROFILE_ENABLED

// Single global instance: the profiler is a measurement tool, not a
// per-resampler feature, and the hot loop must not chase extra pointers
extern CycleCounters g_cycles;

// Read the CPU cycle counter
inline uint32_t ccount() {
#if defined(__XTENSA__)
    uint32_t cycles;
    asm volatile("rsr.ccount %0" : "=a"(cycles));
    return cycles;
#else
    return esp_cpu_get_cycle_count();
#endif
}

// Charge the cycles elapsed since `t` to `field`, then restart `t`
inline void mark(uint32_t& t, uint32_t& field) {
    uint32_t now = ccount();
    field += now - t;
    t = now;
}

#endif  // MR_PROFILE_ENABLED

}  // namespace profile
}  // namespace micro_resampler

// Statement wrapper: expands to nothing when profiling is compiled out, so
// probe sites in the hot loop carry zero cost in normal builds
#if MR_PROFILE_ENABLED
#define MR_PROF(...) __VA_ARGS__
#else
#define MR_PROF(...)
#endif

#endif  // CYCLE_PROFILE_H
