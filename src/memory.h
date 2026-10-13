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

/// @file memory.h
/// @brief Placed allocation: memory that names where it prefers to live (memory_host.cpp,
/// esp/memory_esp.cpp, chosen by CMake). On ESP-IDF that is internal RAM or PSRAM, each falling
/// back to the other, whatever the project's malloc() policy; a host has one heap and ignores the
/// preference.

#pragma once

#include "shared.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace sendspin::server {

/// @brief Where an allocation prefers to live; the other memory is the fallback
enum class MemoryLocation : uint8_t {
    /// PSRAM: buffers and state touched at message or frame rate, and task stacks that never run
    /// with the flash cache disabled
    PREFER_EXTERNAL,
    /// Internal RAM: what DMA, an ISR or a flash write reads, and hot loops measured faster there
    PREFER_INTERNAL,
};

/// @brief bytes aligned to alignment (a power of two), in location or else the other memory
/// @return null when neither has room
void* placed_malloc(size_t bytes, size_t alignment, MemoryLocation location) noexcept;
/// @brief Frees what placed_malloc() returned; null is ignored
void placed_free(void* memory) noexcept;

/// @brief The heaps' state, for the memory statistics
struct MemoryStats {
    size_t internal_free_bytes{0};
    size_t internal_largest_block_bytes{0};
    /// The least internal RAM free since boot
    size_t internal_minimum_free_bytes{0};
    /// What the internal heap holds: the bytes and blocks allocated, and the free blocks
    size_t internal_allocated_bytes{0};
    size_t internal_allocated_blocks{0};
    size_t internal_free_blocks{0};
    size_t external_free_bytes{0};
};

/// @brief Any thread: the heaps now; all zero on a host
MemoryStats memory_stats() noexcept;

/// @brief Any thread: each task's name and the bytes of its stack it has never used, as
/// "name=bytes" separated by spaces; empty on a host, or on ESP-IDF without
/// CONFIG_FREERTOS_USE_TRACE_FACILITY
std::string task_stacks();

/// @brief The alignment to ask placed_malloc() for T with, as operator new aligns it: the heap's
/// own, unless T is over-aligned
template <class T>
constexpr size_t new_alignment() {
    return alignof(T) > __STDCPP_DEFAULT_NEW_ALIGNMENT__ ? alignof(T) : 1;
}

/// @brief An allocator for standard containers whose storage prefers Location, aligned and
/// throwing std::bad_alloc as std::allocator does
template <class T, MemoryLocation Location>
class PlacedAllocator {
public:
    using value_type = T;

    PlacedAllocator() noexcept = default;
    template <class U>
    explicit PlacedAllocator(const PlacedAllocator<U, Location>& /*other*/) noexcept {}

    T* allocate(size_t count) {
        if (count > SIZE_MAX / sizeof(T)) {
            throw std::bad_array_new_length();
        }
        void* memory = placed_malloc(count * sizeof(T), new_alignment<T>(), Location);
        if (memory == nullptr) {
            throw std::bad_alloc();
        }
        return static_cast<T*>(memory);
    }

    void deallocate(T* memory, size_t /*count*/) noexcept {
        placed_free(memory);
    }

    template <class U>
    struct rebind {
        using other = PlacedAllocator<U, Location>;
    };

    friend bool operator==(const PlacedAllocator& /*a*/, const PlacedAllocator& /*b*/) noexcept {
        return true;
    }
};

template <class T>
using ExternalAllocator = PlacedAllocator<T, MemoryLocation::PREFER_EXTERNAL>;

/// @brief A byte buffer in PSRAM where there is one: message payloads, frames and their copies
using ExternalBytes = std::vector<uint8_t, ExternalAllocator<uint8_t>>;

/// @brief Frees a PlacedArray's storage
struct PlacedDeleter {
    void operator()(void* memory) const noexcept {
        placed_free(memory);
    }
};

/// @brief An array of trivially constructible T, left uninitialized, in Location's memory
template <class T>
using PlacedArray = std::unique_ptr<T[], PlacedDeleter>;

/// @brief count elements of T, uninitialized, preferring location
/// @return null when count is 0; throws std::bad_alloc when neither memory has room
template <class T>
PlacedArray<T> make_placed_array(size_t count, MemoryLocation location) {
    static_assert(
        std::is_trivially_default_constructible_v<T> && std::is_trivially_destructible_v<T>,
        "a PlacedArray's elements are neither constructed nor destroyed");
    if (count == 0) {
        return nullptr;
    }
    if (count > SIZE_MAX / sizeof(T)) {
        throw std::bad_array_new_length();
    }
    void* memory = placed_malloc(count * sizeof(T), new_alignment<T>(), location);
    if (memory == nullptr) {
        throw std::bad_alloc();
    }
    return PlacedArray<T>(static_cast<T*>(memory));
}

/// @brief make_shared_ptr() with the object and its control block in PSRAM where there is one
template <class T, class... Args>
SharedPtr<T> make_shared_external(Args&&... args) {
    return allocate_shared_ptr<T>(ExternalAllocator<T>(), std::forward<Args>(args)...);
}

/// @brief A base that puts its derived class's objects, made with new, in PSRAM where there is
/// one, aligned as operator new aligns them (an over-aligned class through the align_val_t form)
struct ExternalNew {
    static void* operator new(size_t bytes) {
        void* memory = placed_malloc(bytes, 1, MemoryLocation::PREFER_EXTERNAL);
        if (memory == nullptr) {
            throw std::bad_alloc();
        }
        return memory;
    }

    static void* operator new(size_t bytes, std::align_val_t alignment) {
        void* memory =
            placed_malloc(bytes, static_cast<size_t>(alignment), MemoryLocation::PREFER_EXTERNAL);
        if (memory == nullptr) {
            throw std::bad_alloc();
        }
        return memory;
    }

    static void operator delete(void* memory) noexcept {
        placed_free(memory);
    }

    static void operator delete(void* memory, std::align_val_t /*alignment*/) noexcept {
        placed_free(memory);
    }
};

}  // namespace sendspin::server
