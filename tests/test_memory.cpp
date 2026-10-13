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

/// @file test_memory.cpp
/// @brief Placed allocation on the host: the alignment a caller asks for, past malloc()'s own and
/// with a size that is not a multiple of it; an empty placed array; a shared object made in placed
/// memory; an over-aligned object made with new through ExternalNew

#include "memory.h"
#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace {

using sendspin::server::ExternalNew;
using sendspin::server::make_placed_array;
using sendspin::server::make_shared_external;
using sendspin::server::MemoryLocation;
using sendspin::server::placed_free;
using sendspin::server::placed_malloc;

TEST(PlacedMalloc, ReturnsTheAlignmentAskedFor) {
    struct Row {
        const char* name;
        size_t bytes;
        size_t alignment;
    };
    const Row rows[] = {
        {"Control: malloc's own alignment", 100, alignof(std::max_align_t)},
        {"past malloc's, size a multiple", 128, 64},
        {"past malloc's, size not a multiple", 100, 64},
        {"past malloc's, zero bytes", 0, 128},
    };
    for (const Row& row : rows) {
        for (const MemoryLocation location :
             {MemoryLocation::PREFER_EXTERNAL, MemoryLocation::PREFER_INTERNAL}) {
            void* memory = placed_malloc(row.bytes, row.alignment, location);
            ASSERT_NE(memory, nullptr) << row.name;
            EXPECT_EQ(reinterpret_cast<uintptr_t>(memory) % row.alignment, 0U) << row.name;
            placed_free(memory);
        }
    }
}

TEST(PlacedArray, IsNullForNoElementsAndHoldsTheOthers) {
    // HostView's staging ring is allocated with no bytes for an input with no ring
    EXPECT_EQ(make_placed_array<uint8_t>(0, MemoryLocation::PREFER_EXTERNAL), nullptr);
    // Control: a non-empty array holds its elements
    const auto bytes = make_placed_array<uint8_t>(3, MemoryLocation::PREFER_EXTERNAL);
    ASSERT_NE(bytes, nullptr);
    bytes[2] = 7;
    EXPECT_EQ(bytes[2], 7);
}

class Shared : public std::enable_shared_from_this<Shared> {
public:
    explicit Shared(bool& destroyed) : destroyed_(destroyed) {}
    ~Shared() {
        this->destroyed_ = true;
    }
    Shared(const Shared&) = delete;
    Shared& operator=(const Shared&) = delete;

private:
    bool& destroyed_;
};

TEST(MakeSharedExternal, MakesASharedObjectThatKnowsItselfAndIsDestroyedWithItsLastOwner) {
    bool destroyed = false;
    std::weak_ptr<Shared> weak;
    {
        const std::shared_ptr<Shared> shared = make_shared_external<Shared>(destroyed);
        weak = shared;
        EXPECT_EQ(shared->shared_from_this(), shared);
        EXPECT_EQ(shared.use_count(), 1);
        EXPECT_FALSE(destroyed);
    }
    EXPECT_TRUE(destroyed);
    EXPECT_TRUE(weak.expired());
}

struct alignas(64) OverAligned : public ExternalNew {
    uint8_t bytes[64];
};

TEST(ExternalNew, AlignsAnOverAlignedClass) {
    // Several, so a 16-byte aligned block that happens to be 64-byte aligned does not pass
    std::vector<std::unique_ptr<OverAligned>> objects;
    for (int i = 0; i < 8; ++i) {
        objects.push_back(std::make_unique<OverAligned>());
        EXPECT_EQ(reinterpret_cast<uintptr_t>(objects.back().get()) % alignof(OverAligned), 0U)
            << "object " << i;
    }
}

}  // namespace
