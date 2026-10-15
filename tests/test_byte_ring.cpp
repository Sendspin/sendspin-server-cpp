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

/// @file test_byte_ring.cpp
/// @brief ByteRing: where each record is placed, at the tail, at the start past the array's end
/// and in the gap up to the head once wrapped; the refusals for bytes and for records; the reset
/// when it empties or is cleared; and a commit after pops emptied the ring

#include "byte_ring.h"
#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace {

using sendspin::server::ByteRing;

enum class Op : uint8_t {
    PUSH,    ///< try_push() of size bytes
    POP,     ///< pop()
    CLEAR,   ///< clear()
    PLACE,   ///< place() of size bytes, filled in and kept for the next COMMIT
    COMMIT,  ///< commit() of the last PLACE
};

struct Step {
    Op op;
    size_t size{0};
};

// Each row runs its steps on a ring of 8 bytes and expects where the last push or place went
// (8, the capacity, when refused) and the sizes it then holds, oldest first, each record's bytes
// all its own tag
TEST(ByteRing, PlacesEachRecordAtTheTailOrPastTheEndAndResetsWhenEmpty) {
    constexpr size_t BYTES = 8;
    struct Row {
        const char* name;
        size_t records;
        std::vector<Step> steps;
        size_t expected_offset;
        std::vector<size_t> expected_sizes;
    };
    const Row rows[] = {
        {"Control: at the tail", 4, {{Op::PUSH, 3}, {Op::PUSH, 3}}, 3, {3, 3}},
        {"fills the storage to its end", 4, {{Op::PUSH, 3}, {Op::PUSH, 5}}, 3, {3, 5}},
        {"refused at the tail and at the start",
         4,
         {{Op::PUSH, 3}, {Op::PUSH, 3}, {Op::PUSH, 3}},
         BYTES,
         {3, 3}},
        {"wraps to the start once the head has moved",
         4,
         {{Op::PUSH, 3}, {Op::PUSH, 3}, {Op::POP}, {Op::PUSH, 3}},
         0,
         {3, 3}},
        {"wrapped: fills the gap up to the head",
         4,
         {{Op::PUSH, 3}, {Op::PUSH, 4}, {Op::POP}, {Op::PUSH, 2}, {Op::PUSH, 1}},
         2,
         {4, 2, 1}},
        {"wrapped: refused past the head",
         4,
         {{Op::PUSH, 3}, {Op::PUSH, 4}, {Op::POP}, {Op::PUSH, 2}, {Op::PUSH, 2}},
         BYTES,
         {4, 2}},
        {"popping past the wrap unwraps: only the tail's end is free",
         4,
         {{Op::PUSH, 3}, {Op::PUSH, 3}, {Op::POP}, {Op::PUSH, 3}, {Op::POP}, {Op::PUSH, 6}},
         BYTES,
         {3}},
        {"refused at the records' limit with bytes to spare",
         2,
         {{Op::PUSH, 1}, {Op::PUSH, 1}, {Op::PUSH, 1}},
         BYTES,
         {1, 1}},
        {"an empty record takes a byte", 4, {{Op::PUSH, 0}, {Op::PUSH, 0}}, 1, {0, 0}},
        {"emptied, it places at its start",
         4,
         {{Op::PUSH, 3}, {Op::PUSH, 3}, {Op::POP}, {Op::POP}, {Op::PUSH, 8}},
         0,
         {8}},
        {"cleared, it places at its start",
         4,
         {{Op::PUSH, 3}, {Op::PUSH, 3}, {Op::CLEAR}, {Op::PUSH, 8}},
         0,
         {8}},
        {"cleared while wrapped, it unwraps",
         4,
         {{Op::PUSH, 3},
          {Op::PUSH, 3},
          {Op::POP},
          {Op::PUSH, 3},
          {Op::CLEAR},
          {Op::PUSH, 3},
          {Op::PUSH, 3},
          {Op::PUSH, 3}},
         BYTES,
         {3, 3}},
        {"refused larger than the storage", 4, {{Op::PUSH, 9}}, BYTES, {}},
        {"a commit after pops emptied the ring keeps its place as the head",
         4,
         {{Op::PUSH, 3}, {Op::PLACE, 3}, {Op::POP}, {Op::COMMIT}, {Op::PUSH, 3}},
         0,
         {3, 3}},
        {"so the start up to that head is free, and no more",
         4,
         {{Op::PUSH, 3}, {Op::PLACE, 3}, {Op::POP}, {Op::COMMIT}, {Op::PUSH, 4}},
         BYTES,
         {3}},
    };
    for (const Row& row : rows) {
        ByteRing<int> ring(BYTES, row.records);
        int tag = 0;
        size_t offset = BYTES;
        size_t placed_size = 0;
        int placed_tag = 0;
        for (const Step& step : row.steps) {
            switch (step.op) {
                case Op::PUSH: {
                    ++tag;
                    const std::vector<uint8_t> bytes(step.size, static_cast<uint8_t>(tag));
                    offset = ring.place(step.size);
                    const bool pushed = ring.try_push(std::span(bytes), int{tag});
                    EXPECT_EQ(pushed, offset != BYTES) << row.name;
                    break;
                }
                case Op::POP:
                    ring.pop();
                    break;
                case Op::CLEAR:
                    ring.clear();
                    break;
                case Op::PLACE:
                    placed_tag = ++tag;
                    placed_size = step.size;
                    offset = ring.place(step.size);
                    ASSERT_NE(offset, BYTES) << row.name;
                    std::fill_n(ring.data(offset), step.size, static_cast<uint8_t>(placed_tag));
                    break;
                case Op::COMMIT:
                    ring.commit(offset, placed_size, int{placed_tag});
                    break;
            }
        }
        EXPECT_EQ(offset, row.expected_offset) << row.name;
        std::vector<size_t> sizes;
        while (!ring.empty()) {
            const std::span<const uint8_t> bytes = ring.front_bytes();
            sizes.push_back(bytes.size());
            for (const uint8_t byte : bytes) {
                EXPECT_EQ(byte, ring.front()) << row.name;
            }
            ring.pop();
        }
        EXPECT_EQ(sizes, row.expected_sizes) << row.name;
    }
}

}  // namespace
