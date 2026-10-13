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

/// @file test_timing_stats.cpp
/// @brief server_cli's time-reply and audio-chunk figures over traced messages built in memory

#include "timing_stats.h"
#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

namespace {

using sendspin::server::TraceDirection;
using sendspin::server::TracedMessage;
using sendspin::server::examples::TimingStats;

constexpr uint64_t CONNECTION = 7;
constexpr uint64_t OTHER_CONNECTION = 8;
constexpr int64_t RECEIVED_US = 1'000'000;
constexpr int64_t CHUNK_PAYLOAD_BYTES = 100;
constexpr int64_t SECOND_US = 1'000'000;

TracedMessage json_message(uint64_t connection, TraceDirection direction, const std::string& json,
                           int64_t stamp_us = 0) {
    TracedMessage message;
    message.connection_id = connection;
    message.client_id = "client-" + std::to_string(connection);
    message.direction = direction;
    message.binary = true;
    message.stamp_us = stamp_us;
    message.plaintext.push_back(0);
    message.plaintext.insert(message.plaintext.end(), json.begin(), json.end());
    return message;
}

TracedMessage server_time(uint64_t connection, int64_t hold_us) {
    const int64_t transmitted = RECEIVED_US + hold_us;
    return json_message(connection, TraceDirection::OUTBOUND,
                        R"({"type":"server/time","payload":{"client_transmitted":5,)"
                        R"("server_received":)" +
                            std::to_string(RECEIVED_US) + R"(,"server_transmitted":)" +
                            std::to_string(transmitted) + "}}",
                        transmitted);
}

TracedMessage chunk(uint64_t connection, uint32_t send_ahead_us, int64_t stamp_us) {
    TracedMessage message;
    message.connection_id = connection;
    message.direction = TraceDirection::OUTBOUND;
    message.binary = true;
    message.stamp_us = stamp_us;
    message.plaintext = {4, 0, 0, 0, 0, 0, 0, 0, 0};
    for (int shift = 24; shift >= 0; shift -= 8) {
        message.plaintext.push_back(static_cast<uint8_t>(send_ahead_us >> shift));
    }
    message.plaintext.resize(message.plaintext.size() + CHUNK_PAYLOAD_BYTES);
    return message;
}

TEST(TimingStats, HoldPercentilesOverServerTimeReplies) {
    TimingStats stats;
    // Holds 1..100 us in reverse order: nearest-rank p50 is 50 and p99 is 99
    constexpr int64_t REPLIES = 100;
    for (int64_t hold = REPLIES; hold >= 1; --hold) {
        stats.add(server_time(CONNECTION, hold));
    }
    const auto report = stats.report(RECEIVED_US + SECOND_US);
    ASSERT_EQ(report.size(), 1U);
    EXPECT_EQ(report[0].connection_id, CONNECTION);
    EXPECT_EQ(report[0].client_id, "client-7");
    EXPECT_EQ(report[0].hold_us.count, 100U);
    EXPECT_EQ(report[0].hold_us.p50, 50);
    EXPECT_EQ(report[0].hold_us.p99, 99);
    EXPECT_EQ(report[0].hold_us.min, 1);
    EXPECT_EQ(report[0].hold_us.max, 100);
    EXPECT_EQ(report[0].chunks, 0U);
}

TEST(TimingStats, NearestRankOverTenReplies) {
    TimingStats stats;
    // Holds 1..10 us: p50 is the 5th value and p99 rounds up to the 10th
    constexpr int64_t REPLIES = 10;
    for (int64_t hold = 1; hold <= REPLIES; ++hold) {
        stats.add(server_time(CONNECTION, hold));
    }
    const auto report = stats.report(RECEIVED_US + SECOND_US);
    ASSERT_EQ(report.size(), 1U);
    EXPECT_EQ(report[0].hold_us.p50, 5);
    EXPECT_EQ(report[0].hold_us.p99, REPLIES);
}

TEST(TimingStats, CountsAudioChunksAndTheirSendAhead) {
    TimingStats stats;
    constexpr uint32_t AHEAD_SHORT = 250'000;
    // Above 2^24, so every byte of the big-endian field counts
    constexpr uint32_t AHEAD_LONG = 0x01020304;
    stats.add(chunk(CONNECTION, AHEAD_LONG, SECOND_US));
    stats.add(chunk(CONNECTION, AHEAD_SHORT, SECOND_US));
    stats.add(chunk(CONNECTION, AHEAD_LONG, SECOND_US));
    const auto report = stats.report(3 * SECOND_US);
    ASSERT_EQ(report.size(), 1U);
    EXPECT_EQ(report[0].chunks, 3U);
    EXPECT_EQ(report[0].chunk_bytes, 3U * (13 + CHUNK_PAYLOAD_BYTES));
    EXPECT_EQ(report[0].send_ahead_us.min, AHEAD_SHORT);
    EXPECT_EQ(report[0].send_ahead_us.p50, AHEAD_LONG);
    // From the connection's first message
    EXPECT_EQ(report[0].window_us, 2 * SECOND_US);
    EXPECT_EQ(report[0].hold_us.count, 0U);
}

TEST(TimingStats, IgnoresInboundAndOtherMessages) {
    TimingStats stats;
    // A server/time as a client would never send it, inbound
    TracedMessage inbound = server_time(CONNECTION, 1);
    inbound.direction = TraceDirection::INBOUND;
    stats.add(inbound);
    TracedMessage cleartext = server_time(CONNECTION, 1);
    cleartext.binary = false;
    stats.add(cleartext);
    EXPECT_TRUE(stats.report(SECOND_US).empty());
    stats.add(json_message(CONNECTION, TraceDirection::OUTBOUND,
                           R"({"type":"group/update","payload":{"server_received":1}})"));
    const auto report = stats.report(SECOND_US);
    ASSERT_EQ(report.size(), 1U);
    EXPECT_EQ(report[0].hold_us.count, 0U);
    EXPECT_EQ(report[0].chunks, 0U);
}

TEST(TimingStats, ResetForgetsFiguresAndStartsTheWindow) {
    TimingStats stats;
    stats.add(server_time(CONNECTION, 1));
    stats.add(chunk(OTHER_CONNECTION, 1, 0));
    stats.reset(5 * SECOND_US);
    EXPECT_TRUE(stats.report(6 * SECOND_US).empty());
    // A message stamped before the reset (taken just before it) counts from the reset
    stats.add(chunk(OTHER_CONNECTION, 1, 4 * SECOND_US));
    const auto report = stats.report(7 * SECOND_US);
    ASSERT_EQ(report.size(), 1U);
    EXPECT_EQ(report[0].connection_id, OTHER_CONNECTION);
    EXPECT_EQ(report[0].chunks, 1U);
    EXPECT_EQ(report[0].window_us, 2 * SECOND_US);
}

}  // namespace
