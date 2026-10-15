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

/// @file test_source_feed.cpp
/// @brief SourceFeed, the bounded FIFO a routed source's stream items cross from core to its
/// decoder.

#include "source_feed.h"
#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace {

using sendspin::server::SourceFeed;
using sendspin::server::SourceItem;
using Bytes = std::vector<uint8_t>;

// ==============================================================================
// The feed
// ==============================================================================

// A bounded FIFO: items come out in order with their bytes, across the storage's wrap, and a push
// past the items or the bytes it holds is refused
TEST(SourceFeed, HoldsItemsInOrderWithinItsBounds) {
    SourceFeed feed(10, 3);
    const auto push = [&](uint64_t stream, size_t size) {
        SourceItem item;
        item.kind = SourceItem::Kind::CHUNK;
        item.stream = stream;
        return feed.push(item, Bytes(size, static_cast<uint8_t>(stream)));
    };
    EXPECT_TRUE(push(1, 4));
    EXPECT_TRUE(push(2, 4));
    EXPECT_FALSE(push(3, 4)) << "past the bytes";
    EXPECT_TRUE(push(3, 2));
    EXPECT_FALSE(push(4, 1)) << "past the items";
    for (uint64_t expected = 1; expected <= 2; ++expected) {
        SourceItem item;
        std::span<const uint8_t> bytes;
        ASSERT_TRUE(feed.front(item, bytes));
        EXPECT_EQ(item.stream, expected);
        EXPECT_TRUE(
            std::all_of(bytes.begin(), bytes.end(), [&](uint8_t b) { return b == expected; }));
        feed.pop();
    }
    // Wraps to the start of the storage, behind the item left
    EXPECT_TRUE(push(5, 6));
    SourceItem item;
    std::span<const uint8_t> bytes;
    ASSERT_TRUE(feed.front(item, bytes));
    EXPECT_EQ(item.stream, 3U);
    feed.pop();
    ASSERT_TRUE(feed.front(item, bytes));
    EXPECT_EQ(item.stream, 5U);
    EXPECT_EQ(bytes.size(), 6U);
    feed.pop();
    EXPECT_TRUE(feed.empty());
    EXPECT_TRUE(push(6, 10)) << "an empty feed holds its whole storage";
}

}  // namespace
