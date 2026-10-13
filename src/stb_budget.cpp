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

#include "stb_budget.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace sendspin::server {

namespace {

/// Each block starts with its size, padded so the bytes after it keep malloc's alignment
constexpr size_t HEADER_BYTES = alignof(std::max_align_t);

struct Budget {
    size_t limit{std::numeric_limits<size_t>::max()};
    size_t used{0};
};

thread_local Budget budget;

/// Whether growing the thread's allocations by more bytes stays within its budget
bool fits(size_t more) {
    return more <= budget.limit && budget.used <= budget.limit - more;
}

/// Whether a block of size bytes and its header fit a size_t
bool representable(size_t size) {
    return size <= std::numeric_limits<size_t>::max() - HEADER_BYTES;
}

unsigned char* header_of(void* block) {
    return static_cast<unsigned char*>(block) - HEADER_BYTES;
}

size_t size_of(const unsigned char* header) {
    size_t size = 0;
    std::memcpy(&size, header, sizeof(size));
    return size;
}

void release(size_t size) {
    // A block freed on another thread, or after its budget ended, is not counted there
    budget.used -= std::min(size, budget.used);
}

}  // namespace

StbBudget::StbBudget(size_t bytes) : previous_limit_(budget.limit), previous_used_(budget.used) {
    budget = Budget{bytes, 0};
}

StbBudget::~StbBudget() {
    budget = Budget{this->previous_limit_, this->previous_used_};
}

void* stb_budget_malloc(size_t size) {
    if (!representable(size) || !fits(size)) {
        return nullptr;
    }
    auto* header = static_cast<unsigned char*>(std::malloc(HEADER_BYTES + size));
    if (header == nullptr) {
        return nullptr;
    }
    std::memcpy(header, &size, sizeof(size));
    budget.used += size;
    return header + HEADER_BYTES;
}

void* stb_budget_realloc(void* block, size_t size) {
    if (block == nullptr) {
        return stb_budget_malloc(size);
    }
    unsigned char* header = header_of(block);
    const size_t old_size = size_of(header);
    if (!representable(size) || (size > old_size && !fits(size - old_size))) {
        return nullptr;
    }
    auto* grown = static_cast<unsigned char*>(std::realloc(header, HEADER_BYTES + size));
    if (grown == nullptr) {
        return nullptr;
    }
    std::memcpy(grown, &size, sizeof(size));
    release(old_size);
    budget.used += size;
    return grown + HEADER_BYTES;
}

void stb_budget_free(void* block) {
    if (block == nullptr) {
        return;
    }
    unsigned char* header = header_of(block);
    release(size_of(header));
    std::free(header);
}

}  // namespace sendspin::server
