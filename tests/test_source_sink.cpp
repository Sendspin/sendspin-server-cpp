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

/// @file test_source_sink.cpp
/// @brief judge_source_chunk(): what a routed source's capture times do on the live input, the
/// backlog of a stall dropped (roles/source/v1.md "Source Audio Chunks (Binary)") and a long gap a
/// new run

#include "source_sink.h"
#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

namespace {

using sendspin::server::judge_source_chunk;
using sendspin::server::SOURCE_RESTART_GAP_US;
using sendspin::server::SourceChunkAction;
using sendspin::server::SourceTiming;

constexpr int64_t CHUNK_US = 20000;
constexpr int64_t JITTER_US = 60000;
/// The path from capture end to receipt when nothing holds the audio back
constexpr int64_t PATH_US = 3000;

/// One chunk: captured at capture_us, received late_us after its path's usual time
struct Chunk {
    int64_t capture_us;
    int64_t late_us;
    SourceChunkAction action;
};

void expect_actions(const std::vector<Chunk>& chunks) {
    SourceTiming timing;
    for (size_t i = 0; i < chunks.size(); ++i) {
        SCOPED_TRACE(i);
        const Chunk& chunk = chunks[i];
        const int64_t end = chunk.capture_us + CHUNK_US;
        EXPECT_EQ(judge_source_chunk(timing, chunk.capture_us, end, end + PATH_US + chunk.late_us,
                                     JITTER_US),
                  chunk.action);
    }
}

TEST(SourceChunkJudgement, WritesAudioArrivingWithinTheJitter) {
    expect_actions({
        {0, 0, SourceChunkAction::WRITE},
        {20000, JITTER_US, SourceChunkAction::WRITE},
        {40000, 5000, SourceChunkAction::WRITE},
        // Capture times that overlap or step back are written, not cut points
        {50000, 0, SourceChunkAction::WRITE},
    });
}

// A stall's backlog arrives later than the jitter allows and is dropped, up to the point where
// live capture resumes on time; the gap it leaves is shorter than a restart's
TEST(SourceChunkJudgement, DropsAStallsBacklog) {
    expect_actions({
        {0, 0, SourceChunkAction::WRITE},
        {20000, 300000, SourceChunkAction::DROP},
        {40000, 280000, SourceChunkAction::DROP},
        {60000, JITTER_US + 1, SourceChunkAction::DROP},
        {80000, 0, SourceChunkAction::WRITE},
    });
}

// A gap of SOURCE_RESTART_GAP_US or more in the capture times, as after a backlog the source
// dropped itself, starts a new run; one just short of it does not
TEST(SourceChunkJudgement, RestartsTheRunAfterALongGap) {
    expect_actions({
        {0, 0, SourceChunkAction::WRITE},
        {CHUNK_US + SOURCE_RESTART_GAP_US - 1, 0, SourceChunkAction::WRITE},
        {2 * CHUNK_US + 2 * SOURCE_RESTART_GAP_US - 1, 0, SourceChunkAction::RESTART},
    });
}

// The spec's "Source timestamps ... may show discontinuities": every chunk late from some point
// on, for longer than a restart's gap, is a jump in the source's time mapping, not a backlog. The
// baseline restarts there and the audio plays again, in a new run.
TEST(SourceChunkJudgement, RebasesAfterStaleAudioOutlastsARestartsGap) {
    std::vector<Chunk> chunks{{0, 0, SourceChunkAction::WRITE}};
    const int64_t jump = 500000;
    int64_t capture = CHUNK_US;
    for (int64_t stale = 0; stale + CHUNK_US <= SOURCE_RESTART_GAP_US; stale += CHUNK_US) {
        chunks.push_back({capture, jump, SourceChunkAction::DROP});
        capture += CHUNK_US;
    }
    chunks.push_back({capture, jump, SourceChunkAction::RESTART});
    chunks.push_back({capture + CHUNK_US, jump, SourceChunkAction::WRITE});
    expect_actions(chunks);
}

}  // namespace
