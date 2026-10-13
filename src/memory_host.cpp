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

/// @file memory_host.cpp
/// @brief Placed allocation on a host: one heap, every location the same

#include "memory.h"

#include <cstdlib>

namespace sendspin::server {

void* placed_malloc(size_t bytes, size_t alignment, MemoryLocation /*location*/) noexcept {
    if (alignment <= alignof(std::max_align_t)) {
        return std::malloc(bytes == 0 ? 1 : bytes);
    }
    // aligned_alloc() takes a multiple of the alignment
    const size_t rounded = ((bytes + alignment - 1) / alignment) * alignment;
    return std::aligned_alloc(alignment, rounded == 0 ? alignment : rounded);
}

void placed_free(void* memory) noexcept {
    std::free(memory);
}

MemoryStats memory_stats() noexcept {
    return {};
}

std::string task_stacks() {
    return {};
}

}  // namespace sendspin::server
