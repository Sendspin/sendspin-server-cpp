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

/// @file test_fragmentation.cpp
/// @brief Inbound reassembly with every malformed sequence of spec messaging.md "Fragmentation",
/// and outbound splitting.

#include "fragmentation.h"
#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace {

using sendspin::server::MAX_TRANSPORT_PLAINTEXT;
using sendspin::server::Reassembler;
using Outcome = sendspin::server::Reassembler::Outcome;
using Bytes = std::vector<uint8_t>;

constexpr uint8_t JSON = 0;
constexpr uint8_t FRAG = 1;
constexpr uint8_t AUDIO = 4;
constexpr uint8_t FIRST = 0x02;
constexpr uint8_t LAST = 0x01;
constexpr uint8_t MIDDLE = 0x00;
constexpr size_t CAP = 8;

bool buffers_json_only(uint8_t message_id) {
    return message_id == JSON;
}

struct Step {
    Bytes plaintext;
    Outcome outcome;
};

struct Row {
    const char* name;
    std::vector<Step> steps;
    /// The message the last step dispatches, when it is MESSAGE
    Bytes message;
};

TEST(Reassembler, FollowsTheFragmentSequenceRules) {
    const Row rows[] = {
        // Controls:
        {"unfragmented", {{{JSON, 'a', 'b'}, Outcome::MESSAGE}}, {JSON, 'a', 'b'}},
        {"three fragments",
         {{{FRAG, FIRST, JSON, 'a'}, Outcome::PENDING},
          {{FRAG, MIDDLE, 'b'}, Outcome::PENDING},
          {{FRAG, LAST, 'c'}, Outcome::MESSAGE}},
         {JSON, 'a', 'b', 'c'}},
        {"one fragment both first and last",
         {{{FRAG, FIRST | LAST, JSON, 'a'}, Outcome::MESSAGE}},
         {JSON, 'a'}},
        {"a new sequence after the last fragment",
         {{{FRAG, FIRST | LAST, JSON, 'a'}, Outcome::MESSAGE},
          {{FRAG, FIRST, JSON, 'b'}, Outcome::PENDING},
          {{FRAG, LAST, 'c'}, Outcome::MESSAGE}},
         {JSON, 'b', 'c'}},
        {"exactly the cap",
         {{{FRAG, FIRST, JSON, '1', '2', '3', '4'}, Outcome::PENDING},
          {{FRAG, LAST, '5', '6', '7', '8'}, Outcome::MESSAGE}},
         {JSON, '1', '2', '3', '4', '5', '6', '7', '8'}},
        {"an unbuffered type is tracked and discarded",
         {{{FRAG, FIRST, AUDIO, 'a'}, Outcome::PENDING}, {{FRAG, LAST, 'b'}, Outcome::DISCARDED}},
         {}},
        // The malformed sequences the spec lists:
        {"first fragment while one is in flight",
         {{{FRAG, FIRST, JSON, 'a'}, Outcome::PENDING},
          {{FRAG, FIRST, JSON, 'b'}, Outcome::MALFORMED}},
         {}},
        {"non-first fragment with none in flight", {{{FRAG, LAST, 'a'}, Outcome::MALFORMED}}, {}},
        {"non-fragment while one is in flight",
         {{{FRAG, FIRST, JSON, 'a'}, Outcome::PENDING}, {{JSON, 'b'}, Outcome::MALFORMED}},
         {}},
        {"reserved flag bit 2", {{{FRAG, FIRST | 0x04, JSON, 'a'}, Outcome::MALFORMED}}, {}},
        {"reserved flag bit 7", {{{FRAG, FIRST | LAST | 0x80, JSON, 'a'}, Outcome::MALFORMED}}, {}},
        {"orig_type 1", {{{FRAG, FIRST | LAST, FRAG, 'a'}, Outcome::MALFORMED}}, {}},
        {"the rules hold while discarding",
         {{{FRAG, FIRST, AUDIO, 'a'}, Outcome::PENDING}, {{JSON, 'b'}, Outcome::MALFORMED}},
         {}},
        // Fragments with no place in the sequence, and a plaintext without an ID:
        {"fragment without flags", {{{FRAG}, Outcome::MALFORMED}}, {}},
        {"first fragment without orig_type", {{{FRAG, FIRST}, Outcome::MALFORMED}}, {}},
        {"empty plaintext", {{{}, Outcome::MALFORMED}}, {}},
        // The cap:
        {"one byte over the cap",
         {{{FRAG, FIRST, JSON, '1', '2', '3', '4'}, Outcome::PENDING},
          {{FRAG, LAST, '5', '6', '7', '8', '9'}, Outcome::TOO_LARGE}},
         {}},
    };
    for (const Row& row : rows) {
        SCOPED_TRACE(row.name);
        Reassembler reassembler(CAP, &buffers_json_only);
        for (const Step& step : row.steps) {
            ASSERT_EQ(reassembler.accept(step.plaintext), step.outcome);
        }
        if (row.steps.back().outcome == Outcome::MESSAGE) {
            EXPECT_EQ(Bytes(reassembler.message().begin(), reassembler.message().end()),
                      row.message);
        }
        // No fragmented message is left in flight: an unfragmented one is taken whole
        EXPECT_EQ(reassembler.accept(Bytes{JSON, 'z'}), Outcome::MESSAGE);
    }
}

TEST(Fragments, SplitALargeMessageIntoFragmentsOfTheGivenSizeThatReassemble) {
    struct Row {
        size_t max_plaintext;
        size_t fragments;
    };
    // Twice the transport maximum: two full fragments and a short third at the transport maximum;
    // at 16 KiB, 16,381 bytes of data in the first fragment and 16,382 in each later one
    const Row rows[] = {
        {MAX_TRANSPORT_PLAINTEXT, 3},
        {16384, 8},
    };
    Bytes payload(2 * MAX_TRANSPORT_PLAINTEXT, 0);
    for (size_t i = 0; i < payload.size(); ++i) {
        payload[i] = static_cast<uint8_t>(i * 7);
    }
    for (const Row& row : rows) {
        SCOPED_TRACE(row.max_plaintext);
        Reassembler reassembler(payload.size(), &buffers_json_only);
        std::vector<Bytes> fragments;
        size_t offset = 0;
        while (offset < payload.size()) {
            Bytes fragment;
            offset = sendspin::server::write_fragment(JSON, payload, offset, row.max_plaintext,
                                                      fragment);
            fragments.push_back(fragment);
        }
        ASSERT_EQ(fragments.size(), row.fragments);
        for (size_t i = 0; i + 1 < fragments.size(); ++i) {
            EXPECT_EQ(fragments[i].size(), row.max_plaintext);
            EXPECT_EQ(fragments[i][1], i == 0 ? FIRST : MIDDLE);
        }
        EXPECT_LE(fragments.back().size(), row.max_plaintext);
        EXPECT_EQ(fragments.back()[1], LAST);

        for (size_t i = 0; i < fragments.size(); ++i) {
            ASSERT_EQ(reassembler.accept(fragments[i]),
                      i + 1 < fragments.size() ? Outcome::PENDING : Outcome::MESSAGE);
        }
        Bytes expected{JSON};
        expected.insert(expected.end(), payload.begin(), payload.end());
        EXPECT_EQ(Bytes(reassembler.message().begin(), reassembler.message().end()), expected);
    }
}

TEST(Fragments, AreNeededOnlyAboveOneTransportMessage) {
    EXPECT_FALSE(sendspin::server::needs_fragments(MAX_TRANSPORT_PLAINTEXT));
    EXPECT_TRUE(sendspin::server::needs_fragments(MAX_TRANSPORT_PLAINTEXT + 1));
}

}  // namespace
