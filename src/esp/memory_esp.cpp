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

/// @file memory_esp.cpp
/// @brief The ESP-IDF build's memory: placed allocation through heap_caps, with the location's
/// memory first and the other as the fallback, so a placement holds whether or not the project
/// lets malloc() reach PSRAM (CONFIG_SPIRAM_USE_MALLOC), and a device without PSRAM gets internal
/// RAM for everything; the memory statistics; internal_heap(); and the wraps of
/// pthread_mutex_init() and pthread_cond_init() that cmake/esp_idf.cmake links into the whole
/// application (--wrap).
///
/// The toolchain's libstdc++ initializes every std::mutex, shared_ptr control block and
/// std::condition_variable with these and ignores the result. ESP-IDF's leave the object as its
/// allocation left it when they fail (the mutex's FreeRTOS semaphore is internal RAM), and a later
/// lock or destroy faults on it. A mutex that failed is left at the static initializer, which its
/// first lock creates again (libstdc++ aborts when that fails too) and a destroy skips. A condition
/// variable that failed aborts at once: libstdc++ ignores a wait's error, so one whose lazy
/// creation failed too would return from every wait at once and spin its caller's predicate loop.

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "memory.h"
#include "profile.h"
#include "transport.h"
#include <pthread.h>

#include <cstdlib>
#include <vector>

extern "C" {
int __real_pthread_mutex_init(pthread_mutex_t* mutex, const pthread_mutexattr_t* attr);
int __real_pthread_cond_init(pthread_cond_t* cond, const pthread_condattr_t* attr);
int __wrap_pthread_mutex_init(pthread_mutex_t* mutex, const pthread_mutexattr_t* attr);
int __wrap_pthread_cond_init(pthread_cond_t* cond, const pthread_condattr_t* attr);
}

namespace sendspin::server {

namespace {

constexpr uint32_t INTERNAL_CAPS = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
constexpr uint32_t EXTERNAL_CAPS = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
/// heap_caps_malloc()'s own alignment; a larger one goes through heap_caps_aligned_alloc()
constexpr size_t HEAP_ALIGNMENT = 4;

void* allocate(size_t bytes, size_t alignment, uint32_t caps) {
    return alignment <= HEAP_ALIGNMENT ? heap_caps_malloc(bytes, caps)
                                       : heap_caps_aligned_alloc(alignment, bytes, caps);
}

}  // namespace

void* placed_malloc(size_t bytes, size_t alignment, MemoryLocation location) noexcept {
    const bool external = location == MemoryLocation::PREFER_EXTERNAL;
    const size_t size = bytes == 0 ? 1 : bytes;
    void* memory = allocate(size, alignment, external ? EXTERNAL_CAPS : INTERNAL_CAPS);
    if (memory == nullptr) {
        memory = allocate(size, alignment, external ? INTERNAL_CAPS : EXTERNAL_CAPS);
    }
    return memory;
}

void placed_free(void* memory) noexcept {
    heap_caps_free(memory);
}

MemoryStats memory_stats() noexcept {
    multi_heap_info_t internal{};
    heap_caps_get_info(&internal, MALLOC_CAP_INTERNAL);
    return MemoryStats{
        .internal_free_bytes = internal.total_free_bytes,
        .internal_largest_block_bytes = internal.largest_free_block,
        .internal_minimum_free_bytes = internal.minimum_free_bytes,
        .internal_allocated_bytes = internal.total_allocated_bytes,
        .internal_allocated_blocks = internal.allocated_blocks,
        .internal_free_blocks = internal.free_blocks,
        .external_free_bytes = heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
    };
}

std::string task_stacks() {
    std::string out;
#if configUSE_TRACE_FACILITY
    // Room for tasks started between the count and the snapshot
    constexpr UBaseType_t SPARE_TASKS = 4;
    std::vector<TaskStatus_t> tasks(uxTaskGetNumberOfTasks() + SPARE_TASKS);
    const UBaseType_t count = uxTaskGetSystemState(tasks.data(), tasks.size(), nullptr);
    for (UBaseType_t i = 0; i < count; ++i) {
        if (!out.empty()) {
            out += ' ';
        }
        out += tasks[i].pcTaskName;
        out += '=';
        // StackType_t is a byte on ESP-IDF: the high-water mark is in bytes
        out += std::to_string(tasks[i].usStackHighWaterMark);
    }
#endif
    return out;
}

std::optional<InternalHeap> internal_heap() {
    InternalHeap heap;
    heap.free_bytes = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    // The walk runs with interrupts off: skipped below the reserve, which refuses a connection
    // whatever the largest block (connection_limits::heap_admits())
    if (heap.free_bytes >= profile::INTERNAL_RAM_RESERVE_BYTES) {
        heap.largest_block_bytes = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    }
    return heap;
}

}  // namespace sendspin::server

int __wrap_pthread_mutex_init(pthread_mutex_t* mutex, const pthread_mutexattr_t* attr) {
    const int result = __real_pthread_mutex_init(mutex, attr);
    if (result != 0 && mutex != nullptr) {
        // The static initializer is a normal mutex; another type has none, and 0 makes every later
        // call on it fail with EINVAL
        const bool normal =
            attr == nullptr || (attr->is_initialized && attr->type == PTHREAD_MUTEX_NORMAL);
        *mutex = normal ? PTHREAD_MUTEX_INITIALIZER : 0;
    }
    return result;
}

int __wrap_pthread_cond_init(pthread_cond_t* cond, const pthread_condattr_t* attr) {
    const int result = __real_pthread_cond_init(cond, attr);
    if (result != 0 && cond != nullptr) {
        // Early logging: no allocation and no lock
        ESP_EARLY_LOGE("sendspin_server", "pthread_cond_init() failed (%d): out of memory", result);
        std::abort();
    }
    return result;
}
