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

/// @file stb_budget.h
/// @brief The allocator stb_image allocates through (STBI_MALLOC, STBI_REALLOC, STBI_FREE in
/// stb_implementation.cpp), with a per-thread byte budget: a decode whose data inflates past
/// what its header declares fails instead of growing without bound.

#pragma once

#include <cstddef>

namespace sendspin::server {

/// @brief While alive, the calling thread's stb_image allocations fail once the bytes they hold
/// would exceed bytes; the budget before it returns when it ends. Without one, there is no limit.
class StbBudget {
public:
    explicit StbBudget(size_t bytes);
    ~StbBudget();
    StbBudget(const StbBudget&) = delete;
    StbBudget& operator=(const StbBudget&) = delete;

private:
    size_t previous_limit_;
    size_t previous_used_;
};

/// @brief stb_image's allocator, counted against the calling thread's budget; nullptr past it
void* stb_budget_malloc(size_t size);
/// @brief As stb_budget_malloc(), for a block from it, or nullptr for a new one
void* stb_budget_realloc(void* block, size_t size);
/// @brief Frees a block from stb_budget_malloc() or stb_budget_realloc(), on any thread
void stb_budget_free(void* block);

}  // namespace sendspin::server
