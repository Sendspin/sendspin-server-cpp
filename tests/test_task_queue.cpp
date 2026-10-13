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

/// @file test_task_queue.cpp
/// @brief TaskQueue: closures of mixed sizes run in push order, within one block and across the
/// blocks a burst chains (past empty ones a throwing push left), the block reused once the queue
/// empties, a push whose move throws queueing nothing, each closure aligned, a swap moving no
/// closure, each closure destroyed once however it leaves; HeapTask

#include "task_queue.h"
#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using sendspin::server::HeapTask;
using sendspin::server::MemoryLocation;
using sendspin::server::TaskQueue;
using sendspin::server::TaskRef;

/// Pushes a closure appending value to ran, with PADDING bytes of captures
template <size_t PADDING>
void push_recorder(TaskQueue& queue, std::vector<int>& ran, int value, uint64_t order = 0) {
    auto task = [&ran, value, padding = std::array<uint8_t, PADDING>{}] {
        static_cast<void>(padding);
        ran.push_back(value);
    };
    queue.push(TaskRef(task), order);
}

void run_all(TaskQueue& queue) {
    while (!queue.empty()) {
        queue.run_front();
    }
}

// Each round queues closures of three sizes and runs them out, as the queue's owners do: the
// block they fit in is started over each time, never grown
TEST(TaskQueue, RunsInPushOrderReusingItsBlockOnceEmpty) {
    TaskQueue queue(512, MemoryLocation::PREFER_INTERNAL);
    std::vector<int> ran;
    std::vector<int> expected;
    int next = 0;
    for (int round = 0; round < 50; ++round) {
        push_recorder<8>(queue, ran, next++);
        push_recorder<40>(queue, ran, next++);
        push_recorder<24>(queue, ran, next++);
        run_all(queue);
    }
    for (int i = 0; i < next; ++i) {
        expected.push_back(i);
    }
    EXPECT_EQ(ran, expected);
    EXPECT_EQ(queue.bytes(), 512U) << "the block had room for every round";
}

// A burst chains blocks; the closures run in order across them, one larger than the block
// included, and once the queue empties only the newest block is kept
TEST(TaskQueue, ChainsBlocksForABurstKeepingTheOrder) {
    TaskQueue queue(128, MemoryLocation::PREFER_EXTERNAL);
    std::vector<int> ran;
    for (int i = 0; i < 8; ++i) {
        push_recorder<16>(queue, ran, i);
    }
    queue.run_front();
    push_recorder<512>(queue, ran, 8);
    push_recorder<16>(queue, ran, 9);
    EXPECT_GT(queue.bytes(), 128U + 512U) << "blocks chained";
    run_all(queue);
    EXPECT_EQ(ran, (std::vector<int>{0, 1, 2, 3, 4, 5, 6, 7, 8, 9}));
    const size_t newest = queue.bytes();
    EXPECT_GE(newest, 512U) << "the newest block, which held the large closure, is kept";
    // Control: the block kept takes the next round without growing
    push_recorder<16>(queue, ran, 10);
    run_all(queue);
    EXPECT_EQ(queue.bytes(), newest);
}

TEST(TaskQueue, KeepsTheOrderEachPushGave) {
    TaskQueue queue(64, MemoryLocation::PREFER_EXTERNAL);
    std::vector<int> ran;
    push_recorder<0>(queue, ran, 0, 41);
    push_recorder<0>(queue, ran, 1, 42);
    EXPECT_EQ(queue.front_order(), 41U);
    queue.run_front();
    EXPECT_EQ(queue.front_order(), 42U);
}

// A closure run may queue another behind the ones queued, which runs in its turn
TEST(TaskQueue, TakesAPushFromAClosureItRuns) {
    TaskQueue queue(64, MemoryLocation::PREFER_EXTERNAL);
    std::vector<int> ran;
    auto first = [&queue, &ran] {
        ran.push_back(0);
        for (int i = 2; i < 6; ++i) {
            push_recorder<16>(queue, ran, i);
        }
    };
    queue.push(TaskRef(first));
    push_recorder<16>(queue, ran, 1);
    run_all(queue);
    EXPECT_EQ(ran, (std::vector<int>{0, 1, 2, 3, 4, 5}));
}

/// A capture whose move throws when told to
struct ThrowingMove {
    bool* fail;
    explicit ThrowingMove(bool* fail_in) : fail(fail_in) {}
    ThrowingMove(ThrowingMove&& other) : fail(other.fail) {
        if (*this->fail) {
            throw std::runtime_error("move");
        }
    }
    ThrowingMove(const ThrowingMove&) = delete;
    ThrowingMove& operator=(const ThrowingMove&) = delete;
    ThrowingMove& operator=(ThrowingMove&&) = delete;
    ~ThrowingMove() = default;
};

TEST(TaskQueue, QueuesNothingWhenAClosuresMoveThrows) {
    TaskQueue queue(64, MemoryLocation::PREFER_EXTERNAL);
    std::vector<int> ran;
    bool fail = false;
    int runs = 0;
    auto task = [capture = ThrowingMove(&fail), &runs] {
        static_cast<void>(capture);
        ++runs;
    };
    push_recorder<16>(queue, ran, 0);
    fail = true;
    EXPECT_THROW(queue.push(TaskRef(task)), std::runtime_error);
    EXPECT_EQ(queue.size(), 1U);
    // Control: the same closure is queued once its move succeeds
    fail = false;
    queue.push(TaskRef(task));
    run_all(queue);
    EXPECT_EQ(ran, (std::vector<int>{0}));
    EXPECT_EQ(runs, 1);
}

// A swap exchanges the blocks: a closure that can no longer move without throwing is not moved
TEST(TaskQueue, SwapLeavesTheClosuresWhereTheyAre) {
    TaskQueue queue(64, MemoryLocation::PREFER_EXTERNAL);
    bool fail = false;
    int runs = 0;
    auto task = [capture = ThrowingMove(&fail), &runs] {
        static_cast<void>(capture);
        ++runs;
    };
    queue.push(TaskRef(task));
    fail = true;
    TaskQueue other;
    other.swap(queue);
    other.run_front();
    EXPECT_EQ(runs, 1);
    EXPECT_TRUE(queue.empty());
}

/// What AlignsAClosurePastAnOddSizedOne's closures write, so they capture nothing else
int32_t narrow_written = 0;
uint64_t wide_written = 0;

// A closure goes at TASK_ALIGNMENT past one whose size is not a multiple of it; UBSan checks the
// accesses to the second's header and captures
TEST(TaskQueue, AlignsAClosurePastAnOddSizedOne) {
    narrow_written = 0;
    wide_written = 0;
    TaskQueue queue(256, MemoryLocation::PREFER_EXTERNAL);
    auto narrow = [n = int32_t{7}] { narrow_written = n; };
    static_assert(sizeof(narrow) % sendspin::server::TASK_ALIGNMENT != 0);
    auto wide = [value = uint64_t{42}] { wide_written = value; };
    queue.push(TaskRef(narrow), 1);
    queue.push(TaskRef(wide), 2);
    queue.run_front();
    EXPECT_EQ(queue.front_order(), 2U);
    queue.run_front();
    EXPECT_EQ(narrow_written, 7);
    EXPECT_EQ(wide_written, 42U);
}

/// Pushes a closure of PADDING bytes whose move throws, which chains a block when the newest has
/// no room for it and then queues nothing
template <size_t PADDING>
void push_failing(TaskQueue& queue, bool* fail) {
    auto task = [capture = ThrowingMove(fail), padding = std::array<uint8_t, PADDING>{}] {
        static_cast<void>(capture);
        static_cast<void>(padding);
    };
    EXPECT_THROW(queue.push(TaskRef(task)), std::runtime_error);
}

// Pushes whose moves threw leave empty blocks chained between two closures: the next closure is
// found past all of them
TEST(TaskQueue, FindsTheNextClosurePastSeveralEmptyBlocks) {
    TaskQueue queue(64, MemoryLocation::PREFER_EXTERNAL);
    std::vector<int> ran;
    bool fail = true;
    push_recorder<8>(queue, ran, 0);
    const size_t one_block = queue.bytes();
    push_failing<40>(queue, &fail);
    const size_t two_blocks = queue.bytes();
    push_failing<120>(queue, &fail);
    EXPECT_GT(queue.bytes(), two_blocks) << "each failed push chained a block";
    EXPECT_GT(two_blocks, one_block);
    push_recorder<200>(queue, ran, 1);
    ASSERT_EQ(queue.size(), 2U);
    run_all(queue);
    EXPECT_EQ(ran, (std::vector<int>{0, 1}));
}

/// Counts the live copies of a capture, to show each closure is destroyed once
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

void push_counted(TaskQueue& queue, int& live, int& runs) {
    auto task = [counted = Counted(live), &runs] {
        static_cast<void>(counted);
        ++runs;
    };
    queue.push(TaskRef(task));
}

TEST(TaskQueue, DestroysEachClosureOnceWhetherRunPoppedClearedOrLeft) {
    int live = 0;
    int runs = 0;
    {
        TaskQueue queue(64, MemoryLocation::PREFER_EXTERNAL);
        for (int i = 0; i < 8; ++i) {
            push_counted(queue, live, runs);
        }
        EXPECT_EQ(live, 8) << "the queue holds one of each, the callers' copies destroyed";
        queue.run_front();
        EXPECT_EQ(runs, 1);
        EXPECT_EQ(live, 7) << "a closure run is destroyed";
        queue.pop_front();
        EXPECT_EQ(runs, 1) << "a closure popped is not run";
        EXPECT_EQ(live, 6);
        queue.clear();
        EXPECT_EQ(live, 0);
        push_counted(queue, live, runs);
        push_counted(queue, live, runs);
        TaskQueue other;
        other.swap(queue);
        EXPECT_EQ(live, 2);
        EXPECT_TRUE(queue.empty());
    }
    EXPECT_EQ(live, 0) << "the queue destroys what it still holds";
    EXPECT_EQ(runs, 1);
}

TEST(HeapTask, RunsTheClosureAndDestroysItOnce) {
    int live = 0;
    int runs = 0;
    {
        auto task = [counted = Counted(live), &runs, text = std::string(100, 'x')] {
            static_cast<void>(counted);
            runs += text.size() == 100 ? 1 : 0;
        };
        HeapTask held(TaskRef{task});
        HeapTask moved(std::move(held));
        moved();
        EXPECT_EQ(runs, 1);
        EXPECT_EQ(live, 2) << "the caller's closure and the one held";
    }
    EXPECT_EQ(live, 0);
}

}  // namespace
