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

/// @file test_soak_wire_checker.cpp
/// @brief The soak's wire checker (tests/soak_wire_checker.h) over synthetic traces, so that each
/// of its checks is shown to fire where the soak relies on it

#include "soak_wire_checker.h"
#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

namespace {

using sendspin::server::TraceDirection;
using sendspin::server::TracedMessage;
using sendspin::server::test::WireChecker;

constexpr int64_t MS = 1000;
/// A 20 ms PCM chunk at 48 kHz, stereo, 16 bits: its header and 960 frames
constexpr uint32_t CHUNK_BYTES = 13 + (960 * 4);
constexpr uint32_t ROOMY = 100 * CHUNK_BYTES;

TracedMessage json(const std::string& client, TraceDirection direction, int64_t stamp_us,
                   const std::string& text) {
    TracedMessage message;
    message.client_id = client;
    message.direction = direction;
    message.binary = true;
    message.stamp_us = stamp_us;
    message.plaintext.push_back(0);
    message.plaintext.insert(message.plaintext.end(), text.begin(), text.end());
    return message;
}

/// A chunk of 20 ms whose samples are all fill
TracedMessage chunk(const std::string& client, int64_t ts_us, int64_t stamp_us, uint8_t fill = 1) {
    TracedMessage message;
    message.client_id = client;
    message.direction = TraceDirection::OUTBOUND;
    message.binary = true;
    message.stamp_us = stamp_us;
    message.plaintext.assign(CHUNK_BYTES, fill);
    message.plaintext[0] = 4;
    for (size_t i = 0; i < 8; ++i) {
        message.plaintext[1 + i] = static_cast<uint8_t>(ts_us >> (8 * (7 - i)));
    }
    return message;
}

TracedMessage state(const std::string& client, int64_t stamp_us, uint32_t output_delay_ms) {
    return json(client, TraceDirection::INBOUND, stamp_us,
                R"({"type":"client/state","payload":{"player":{"min_buffer_ms":100,)"
                R"("output_delay_ms":)" +
                    std::to_string(output_delay_ms) + "}}}");
}

/// A client's session up to its stream's start at 10 ms, in group 1, with no output delay
std::vector<TracedMessage> session(const std::string& client, uint32_t capacity) {
    return {
        json(client, TraceDirection::INBOUND, 0,
             R"({"type":"client/hello","payload":{"player@v1_support":{"buffer_capacity":)" +
                 std::to_string(capacity) + "}}}"),
        state(client, 1, 0),
        json(client, TraceDirection::OUTBOUND, 2,
             R"({"type":"group/update","payload":{"group_id":"1"}})"),
        json(client, TraceDirection::OUTBOUND, 10 * MS,
             R"({"type":"stream/start","payload":{"player":{"codec":"pcm","sample_rate":48000,)"
             R"("channels":2,"bit_depth":16}}})"),
    };
}

std::vector<TracedMessage> join(std::vector<TracedMessage> first,
                                const std::vector<TracedMessage>& then) {
    first.insert(first.end(), then.begin(), then.end());
    return first;
}

// Each check the soak relies on fires on a trace that breaks it, and stays quiet on its control
TEST(SoakWireChecker, FlagsEachBrokenRule) {
    struct Row {
        const char* name;
        std::vector<TracedMessage> trace;
        /// Part of the first anomaly, or empty for none
        std::string anomaly;
        size_t late_starts{0};
    };
    const std::vector<Row> rows = {
        {"Control: a contiguous stream, on time",
         join(session("a", ROOMY), {chunk("a", 1000 * MS, 10 * MS), chunk("a", 1020 * MS, 20 * MS),
                                    chunk("a", 1040 * MS, 30 * MS)}),
         ""},
        {"a chunk overlapping the one before",
         join(session("a", ROOMY),
              {chunk("a", 1000 * MS, 10 * MS), chunk("a", 1010 * MS, 20 * MS)}),
         "overlapping"},
        {"a gap where no frame was late",
         join(session("a", ROOMY),
              {chunk("a", 1000 * MS, 10 * MS), chunk("a", 1060 * MS, 20 * MS)}),
         "a gap of 40000 us"},
        {"Control: a gap where the frames skipped were late",
         join(session("a", ROOMY),
              {chunk("a", 1000 * MS, 10 * MS), chunk("a", 1100 * MS, 1030 * MS)}),
         ""},
        {"a chunk sent late",
         join(session("a", ROOMY),
              {chunk("a", 1000 * MS, 10 * MS), chunk("a", 1020 * MS, 1020 * MS)}),
         "sent late"},
        {"more than buffer_capacity",
         join(session("a", 2 * CHUNK_BYTES),
              {chunk("a", 1000 * MS, 10 * MS), chunk("a", 1020 * MS, 20 * MS),
               chunk("a", 1040 * MS, 30 * MS)}),
         "over capacity"},
        {"Control: chunks past their completion leave the count",
         join(session("a", 2 * CHUNK_BYTES),
              {chunk("a", 1000 * MS, 10 * MS), chunk("a", 1020 * MS, 20 * MS),
               chunk("a", 1100 * MS, 1045 * MS)}),
         ""},
        {"Control: an output delay leaves out a chunk completing before now",
         join(session("a", 2 * CHUNK_BYTES),
              {chunk("a", 1000 * MS, 10 * MS), state("a", 700 * MS, 300),
               chunk("a", 1100 * MS, 750 * MS), chunk("a", 1120 * MS, 760 * MS)}),
         ""},
        {"a lower output delay counts that chunk again (spec: recalculates every chunk)",
         join(session("a", 2 * CHUNK_BYTES),
              {chunk("a", 1000 * MS, 10 * MS), state("a", 700 * MS, 300),
               chunk("a", 1100 * MS, 750 * MS), state("a", 755 * MS, 0),
               chunk("a", 1120 * MS, 760 * MS)}),
         "over capacity"},
        {"a first chunk under the player's floor",
         join(session("a", ROOMY), {chunk("a", 60 * MS, 15 * MS)}), "under the floor"},
        {"Control: two players of one format get the same cell",
         join(join(session("a", ROOMY), session("b", ROOMY)),
              {chunk("a", 1000 * MS, 10 * MS, 1), chunk("b", 1000 * MS, 11 * MS, 1)}),
         ""},
        {"two players of one format get different audio for one cell",
         join(join(session("a", ROOMY), session("b", ROOMY)),
              {chunk("a", 1000 * MS, 10 * MS, 1), chunk("b", 1000 * MS, 11 * MS, 2)}),
         "different audio"},
        {"a stream with no first chunk", session("a", ROOMY), "", 1},
    };
    for (const Row& row : rows) {
        SCOPED_TRACE(row.name);
        WireChecker checker;
        for (const TracedMessage& message : row.trace) {
            checker.on_message(message);
        }
        checker.finish(10 * WireChecker::SECOND_US);
        const std::vector<std::string> anomalies = checker.anomalies();
        if (row.anomaly.empty()) {
            EXPECT_TRUE(anomalies.empty()) << anomalies.front();
        } else {
            ASSERT_FALSE(anomalies.empty());
            EXPECT_NE(anomalies.front().find(row.anomaly), std::string::npos) << anomalies.front();
        }
        EXPECT_EQ(checker.stats()["a"].late_starts, row.late_starts);
    }
}

/// A loudness message: 11 bytes
TracedMessage loudness(const std::string& client, int64_t ts_us, int64_t stamp_us) {
    TracedMessage message;
    message.client_id = client;
    message.direction = TraceDirection::OUTBOUND;
    message.binary = true;
    message.stamp_us = stamp_us;
    message.plaintext.assign(11, 0);
    message.plaintext[0] = 16;
    for (size_t i = 0; i < 8; ++i) {
        message.plaintext[1 + i] = static_cast<uint8_t>(ts_us >> (8 * (7 - i)));
    }
    return message;
}

/// A visualizer client's session up to its stream's start at 10 ms
std::vector<TracedMessage> visualizer_session(const std::string& client, uint32_t capacity) {
    return {
        json(client, TraceDirection::INBOUND, 0,
             R"({"type":"client/hello","payload":{"visualizer@v1_support":{"buffer_capacity":)" +
                 std::to_string(capacity) + "}}}"),
        json(client, TraceDirection::OUTBOUND, 10 * MS,
             R"({"type":"stream/start","payload":{"visualizer":{"types":["loudness"],)"
             R"("rate_max":50}}})"),
    };
}

TracedMessage stream_message(const std::string& client, int64_t stamp_us, const char* type,
                             const char* role) {
    return json(
        client, TraceDirection::OUTBOUND, stamp_us,
        std::string(R"({"type":")") + type + R"(","payload":{"roles":[")" + role + R"("]}})");
}

// spec roles/visualizer/v1.md: each visualizer check the soak relies on fires on a trace that
// breaks it, and stays quiet on its control
TEST(SoakWireChecker, FlagsEachBrokenVisualizerRule) {
    struct Row {
        const char* name;
        std::vector<TracedMessage> trace;
        /// Part of the first anomaly, or empty for none
        std::string anomaly;
    };
    const std::vector<Row> rows = {
        {"Control: messages ahead of their times, in order, within the capacity",
         join(visualizer_session("a", 22),
              {loudness("a", 100 * MS, 20 * MS), loudness("a", 120 * MS, 30 * MS),
               loudness("a", 140 * MS, 105 * MS)}),
         ""},
        {"a message outside a stream", {loudness("a", 100 * MS, 20 * MS)}, "outside a stream"},
        {"a message after the stream's end",
         join(visualizer_session("a", 100),
              {stream_message("a", 15 * MS, "stream/end", "visualizer"),
               loudness("a", 100 * MS, 20 * MS)}),
         "outside a stream"},
        {"Control: a player's end leaves the visualizer's stream",
         join(visualizer_session("a", 100), {stream_message("a", 15 * MS, "stream/end", "player"),
                                             loudness("a", 100 * MS, 20 * MS)}),
         ""},
        {"a message sent late",
         join(visualizer_session("a", 100), {loudness("a", 100 * MS, 100 * MS)}), "sent late"},
        {"a timestamp that decreased",
         join(visualizer_session("a", 100),
              {loudness("a", 120 * MS, 20 * MS), loudness("a", 100 * MS, 30 * MS)}),
         "decreased"},
        {"Control: a clear lets timestamps decrease",
         join(visualizer_session("a", 100),
              {loudness("a", 120 * MS, 20 * MS),
               stream_message("a", 25 * MS, "stream/clear", "visualizer"),
               loudness("a", 100 * MS, 30 * MS)}),
         ""},
        {"more than buffer_capacity",
         join(visualizer_session("a", 32),
              {loudness("a", 100 * MS, 20 * MS), loudness("a", 120 * MS, 30 * MS),
               loudness("a", 140 * MS, 40 * MS)}),
         "visualizer over capacity"},
        {"Control: a clear resets the count",
         join(visualizer_session("a", 22),
              {loudness("a", 100 * MS, 20 * MS), loudness("a", 120 * MS, 30 * MS),
               stream_message("a", 35 * MS, "stream/clear", "visualizer"),
               loudness("a", 140 * MS, 40 * MS)}),
         ""},
    };
    for (const Row& row : rows) {
        SCOPED_TRACE(row.name);
        WireChecker checker;
        for (const TracedMessage& message : row.trace) {
            checker.on_message(message);
        }
        const std::vector<std::string> anomalies = checker.anomalies();
        if (row.anomaly.empty()) {
            EXPECT_TRUE(anomalies.empty()) << anomalies.front();
        } else {
            ASSERT_FALSE(anomalies.empty());
            EXPECT_NE(anomalies.front().find(row.anomaly), std::string::npos) << anomalies.front();
        }
    }
}

}  // namespace
