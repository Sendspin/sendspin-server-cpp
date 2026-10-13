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

/// @file test_ring.cpp
/// @brief Ring: FIFO order across the array's end and across growth, the limit refusing a push
/// and a fixed ring keeping its array, each element destroyed once, a swap, and a refused push
/// leaving its value

#include "ring.h"
#include <gtest/gtest.h>

#include <cstddef>
#include <memory>
#include <vector>

namespace {

using sendspin::server::MemoryLocation;
using sendspin::server::Ring;

std::vector<int> contents(const Ring<int>& ring) {
    std::vector<int> values;
    for (size_t i = 0; i < ring.size(); ++i) {
        values.push_back(ring[i]);
    }
    return values;
}

// Each row pushes and pops in turn (a negative step pops that many) and expects the contents, the
// capacity and whether its last push was taken
TEST(Ring, KeepsFifoOrderAcrossItsEndAndItsGrowthUpToItsLimit) {
    struct Row {
        const char* name;
        size_t capacity;
        size_t limit;
        std::vector<int> steps;
        std::vector<int> expected;
        size_t expected_capacity;
        bool last_push_taken;
    };
    const Row rows[] = {
        {"Control: within the capacity", 4, 4, {3}, {1, 2, 3}, 4, true},
        {"wraps past the array's end", 4, 4, {3, -2, 3}, {3, 4, 5, 6}, 4, true},
        {"a fixed ring refuses when full", 4, 4, {3, -2, 3, 1}, {3, 4, 5, 6}, 4, false},
        {"grows, doubling, up to the limit", 2, 5, {5}, {1, 2, 3, 4, 5}, 5, true},
        {"refuses at the limit", 2, 5, {6}, {1, 2, 3, 4, 5}, 5, false},
        {"grows while wrapped", 3, Ring<int>::UNLIMITED, {3, -1, 2}, {2, 3, 4, 5}, 6, true},
        {"grows from no capacity", 0, 3, {3}, {1, 2, 3}, 3, true},
    };
    for (const Row& row : rows) {
        Ring<int> ring(row.capacity, row.limit, MemoryLocation::PREFER_EXTERNAL);
        int next = 1;
        bool taken = true;
        for (const int step : row.steps) {
            for (int i = 0; i < step; ++i) {
                taken = ring.try_push(next);
                next += taken ? 1 : 0;
            }
            for (int i = 0; i < -step; ++i) {
                ring.pop_front();
            }
        }
        EXPECT_EQ(contents(ring), row.expected) << row.name;
        EXPECT_EQ(ring.capacity(), row.expected_capacity) << row.name;
        EXPECT_EQ(taken, row.last_push_taken) << row.name;
        EXPECT_EQ(ring.front(), row.expected.front()) << row.name;
        EXPECT_EQ(ring.back(), row.expected.back()) << row.name;
    }
}

// A ring whose limit is its capacity keeps the array it was made with: an element stays where it
// was pushed however often the ring wraps
TEST(Ring, AFixedRingKeepsItsArray) {
    Ring<int> ring(3, 3, MemoryLocation::PREFER_EXTERNAL);
    ASSERT_TRUE(ring.try_push(0));
    const int* first = &ring.front();
    std::vector<const int*> seen{first};
    for (int i = 1; i < 10; ++i) {
        ASSERT_TRUE(ring.try_push(i));
        seen.push_back(&ring.back());
        ring.pop_front();
    }
    for (const int* slot : seen) {
        EXPECT_GE(slot, first);
        EXPECT_LT(slot, first + 3);
    }
}

/// Counts the live objects of its kind, to show each is destroyed once
class Counted {
public:
    explicit Counted(int& live) : live_(&live) {
        ++*this->live_;
    }
    Counted(Counted&& other) noexcept : live_(other.live_) {
        ++*this->live_;
    }
    Counted(const Counted&) = delete;
    Counted& operator=(const Counted&) = delete;
    Counted& operator=(Counted&&) = delete;
    ~Counted() {
        --*this->live_;
    }

private:
    int* live_;
};

TEST(Ring, DestroysEachElementOnceWhetherPoppedMovedClearedOrLeft) {
    int live = 0;
    {
        Ring<Counted> ring(1, Ring<Counted>::UNLIMITED, MemoryLocation::PREFER_EXTERNAL);
        for (int i = 0; i < 5; ++i) {
            ring.push(Counted(live));
        }
        EXPECT_EQ(live, 5) << "growth moves the elements and destroys the originals";
        ring.pop_front();
        EXPECT_EQ(live, 4);
        ring.clear();
        EXPECT_EQ(live, 0);
        ring.push(Counted(live));
        ring.push(Counted(live));
        const Counted* first = &ring.front();
        Ring<Counted> moved(std::move(ring));
        EXPECT_EQ(live, 2);
        EXPECT_EQ(&moved.front(), first) << "a move takes the array, leaving the elements in place";
    }
    EXPECT_EQ(live, 0) << "the ring destroys what it still holds";
}

TEST(Ring, SwapExchangesElementsAndLimitsAndARefusalKeepsTheValue) {
    Ring<std::unique_ptr<int>> ring(2, 2, MemoryLocation::PREFER_EXTERNAL);
    ASSERT_TRUE(ring.try_push(std::make_unique<int>(7)));
    Ring<std::unique_ptr<int>> other;
    auto refused = std::make_unique<int>(1);
    EXPECT_FALSE(other.try_push(std::move(refused))) << "a ring made empty takes nothing";
    EXPECT_NE(refused, nullptr) << "a refused push leaves the value as it was";
    other.swap(ring);
    ASSERT_EQ(other.size(), 1U);
    EXPECT_EQ(*other.front(), 7);
    EXPECT_EQ(other.limit(), 2U);
    EXPECT_TRUE(ring.empty());
    EXPECT_FALSE(ring.try_push(std::make_unique<int>(1)));
}

}  // namespace
