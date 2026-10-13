// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Kevin Ahrendt

/// @file memory_utils.h
/// @brief Aligned and unaligned memory allocation helpers for ESP-IDF and host builds
///
/// MemoryUtils centralizes allocation with a PSRAM-preferred or internal-only
/// strategy, using heap_caps_* on ESP-IDF and std::malloc / std::aligned_alloc
/// on host platforms

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdlib>

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#include "sdkconfig.h"
#endif

namespace micro_resampler {

// Memory allocation strategies
enum class AllocStrategy : uint8_t {
    PREFER_PSRAM,     // Try PSRAM first, fall back to internal
    PREFER_INTERNAL,  // Try internal first, fall back to PSRAM
    INTERNAL_ONLY,    // Only use internal memory
    DEFAULT           // Platform default behavior
};

// Centralized memory allocation helpers
class MemoryUtils {
public:
    // Allocate aligned memory with fallback strategy
    template <typename T>
    static T* allocate_aligned(size_t count, size_t alignment = 16,
                               AllocStrategy strategy = AllocStrategy::PREFER_PSRAM) {
        size_t size = count * sizeof(T);
        void* ptr = nullptr;

#ifdef ESP_PLATFORM
        if (strategy == AllocStrategy::PREFER_PSRAM) {
#ifdef CONFIG_RESAMPLER_USE_PSRAM
            // Try PSRAM first
            ptr = heap_caps_aligned_alloc(alignment, size, MALLOC_CAP_SPIRAM);
#endif
            // Fall back to internal if PSRAM failed or not configured
            if (!ptr) {
                ptr =
                    heap_caps_aligned_alloc(alignment, size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
            }
        } else if (strategy == AllocStrategy::PREFER_INTERNAL) {
            // Try internal first (for small, hot data structures)
            ptr = heap_caps_aligned_alloc(alignment, size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
#ifdef CONFIG_RESAMPLER_USE_PSRAM
            if (!ptr) {
                ptr = heap_caps_aligned_alloc(alignment, size, MALLOC_CAP_SPIRAM);
            }
#endif
        } else {
            // Internal only
            ptr = heap_caps_aligned_alloc(alignment, size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        }
#else
        // Non-ESP platform
        (void)strategy;  // Suppress unused parameter warning
        // std::aligned_alloc requires size to be a multiple of alignment
        size_t aligned_size = (size + alignment - 1) & ~(alignment - 1);
        ptr = std::aligned_alloc(alignment, aligned_size);
#endif
        return static_cast<T*>(ptr);
    }

    // Allocate unaligned memory with fallback strategy
    template <typename T>
    static T* allocate(size_t count, AllocStrategy strategy = AllocStrategy::PREFER_PSRAM) {
        size_t size = count * sizeof(T);
        void* ptr = nullptr;

#ifdef ESP_PLATFORM
        if (strategy == AllocStrategy::PREFER_PSRAM) {
#ifdef CONFIG_RESAMPLER_USE_PSRAM
            // Try PSRAM first, fall back to internal automatically
            ptr = heap_caps_malloc_prefer(size, 2, MALLOC_CAP_SPIRAM,
                                          MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
#else
            // PSRAM not configured, use internal only
            ptr = heap_caps_malloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
#endif
        } else if (strategy == AllocStrategy::PREFER_INTERNAL) {
#ifdef CONFIG_RESAMPLER_USE_PSRAM
            // Try internal first (for small, hot data structures)
            ptr = heap_caps_malloc_prefer(size, 2, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT,
                                          MALLOC_CAP_SPIRAM);
#else
            ptr = heap_caps_malloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
#endif
        } else {
            // Internal only
            ptr = heap_caps_malloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        }
#else
        // Non-ESP platform
        (void)strategy;  // Suppress unused parameter warning
        ptr = std::malloc(size);
#endif
        return static_cast<T*>(ptr);
    }

    /// @brief Free memory allocated by allocate_aligned() or allocate()
    /// A no-op if ptr is null. Handles both aligned and unaligned allocations, using
    /// heap_caps_free() on ESP-IDF (which tracks capability flags per pointer) or
    /// std::free() on host builds
    /// @param ptr Pointer returned by allocate_aligned() or allocate(), or nullptr
    static void free(void* ptr) {
        if (!ptr) {
            return;
        }

#ifdef ESP_PLATFORM
        heap_caps_free(ptr);
#else
        std::free(ptr);
#endif
    }
};

}  // namespace micro_resampler
