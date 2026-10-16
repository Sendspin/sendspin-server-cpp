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

/// @file freed_blocks.h
/// @brief FreedBlockScan, which counts the heap blocks the test thread frees while it is alive that
/// still hold a secret, and AllocationCount, which counts the blocks a thread allocates while it
/// counts, through the sanitizer's free and malloc hooks; only in a build with ASan or TSan

#pragma once

#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define SENDSPIN_TEST_FREE_HOOKS 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
#define SENDSPIN_TEST_FREE_HOOKS 1
#endif
#endif

#include <pthread.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string_view>

#ifdef SENDSPIN_TEST_FREE_HOOKS
#if __has_include(<sanitizer/allocator_interface.h>)
#include <sanitizer/allocator_interface.h>
#else
// GCC's runtimes export these but install no header declaring them
extern "C" {
int __sanitizer_install_malloc_and_free_hooks(void (*malloc_hook)(const volatile void*, size_t),
                                              void (*free_hook)(const volatile void*));
size_t __sanitizer_get_allocated_size(const volatile void* pointer);
}
#endif
#endif

namespace freed_blocks {

/// The secret the live scan looks for; nullptr while none is alive
inline std::atomic<const std::string_view*> needle{nullptr};
/// The thread whose frees the live scan counts
inline std::atomic<pthread_t> scanning_thread{};
inline std::atomic<int> holding{0};
/// The thread whose allocations are counted while counting is set
inline std::atomic<pthread_t> counting_thread{};
inline std::atomic<bool> counting{false};
inline std::atomic<int> allocated{0};

#ifdef SENDSPIN_TEST_FREE_HOOKS
// Runs before the allocator takes the block back, so its bytes are still there. A container's
// spare capacity may be poisoned, so the block is read only in this uninstrumented function.
// Another thread's frees are not counted, and only the scanning thread ends the scan, so the
// secret it views outlives every read of it.
__attribute__((no_sanitize_address, no_sanitize_thread)) inline void on_free(
    const volatile void* pointer) {
    if (pointer == nullptr || pthread_equal(pthread_self(), scanning_thread.load()) == 0) {
        return;
    }
    const std::string_view* secret = needle.load();
    if (secret == nullptr) {
        return;
    }
    const void* block = const_cast<const void*>(pointer);
    const size_t size = __sanitizer_get_allocated_size(block);
    const auto* data = static_cast<const uint8_t*>(block);
    for (size_t start = 0; start + secret->size() <= size; ++start) {
        size_t matched = 0;
        while (matched < secret->size() &&
               data[start + matched] == static_cast<uint8_t>((*secret)[matched])) {
            ++matched;
        }
        if (matched == secret->size()) {
            holding.fetch_add(1);
            return;
        }
    }
}

inline void on_malloc(const volatile void* /*pointer*/, size_t /*size*/) {
    if (counting.load() && pthread_equal(pthread_self(), counting_thread.load()) != 0) {
        allocated.fetch_add(1);
    }
}
#endif

}  // namespace freed_blocks

namespace freed_blocks {

/// @brief Whether the build has the hooks; a test that needs them skips itself otherwise
inline bool hooks_available() {
#ifdef SENDSPIN_TEST_FREE_HOOKS
    // The sanitizer takes a few hook pairs and never removes one: installed once per process
    static const bool installed =
        __sanitizer_install_malloc_and_free_hooks(&on_malloc, &on_free) != 0;
    return installed;
#else
    return false;
#endif
}

}  // namespace freed_blocks

/// Counts the blocks the calling thread frees while it is alive that hold `secret`. One at a time.
class FreedBlockScan {
public:
    /// @brief Whether the build has the hook; a test that needs it skips itself otherwise
    static bool available() {
        return freed_blocks::hooks_available();
    }

    explicit FreedBlockScan(std::string_view secret) : secret_(secret) {
        freed_blocks::holding.store(0);
        freed_blocks::scanning_thread.store(pthread_self());
        freed_blocks::needle.store(&this->secret_);
    }
    ~FreedBlockScan() {
        freed_blocks::needle.store(nullptr);
    }
    FreedBlockScan(const FreedBlockScan&) = delete;
    FreedBlockScan& operator=(const FreedBlockScan&) = delete;

    /// @brief The blocks freed holding the secret so far
    int holding() const {
        return freed_blocks::holding.load();
    }

private:
    std::string_view secret_;
};

/// Counts the blocks the calling thread allocates between start() and stop(). One at a time.
class AllocationCount {
public:
    /// @brief Whether the build has the hook; a test that needs it skips itself otherwise
    static bool available() {
        return freed_blocks::hooks_available();
    }

    AllocationCount() {
        freed_blocks::allocated.store(0);
        freed_blocks::counting_thread.store(pthread_self());
    }
    ~AllocationCount() {
        this->stop();
    }
    AllocationCount(const AllocationCount&) = delete;
    AllocationCount& operator=(const AllocationCount&) = delete;

    void start() {
        freed_blocks::counting.store(true);
    }

    void stop() {
        freed_blocks::counting.store(false);
    }

    /// @brief The blocks allocated while counting so far
    int blocks() const {
        return freed_blocks::allocated.load();
    }
};
