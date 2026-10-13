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

/// @file test_metadata.cpp
/// @brief A group's metadata timeline as tables (spec metadata/v1.md): progress carried forward,
/// and what each step sends, scheduled updates, cancellation, pause and resume included.

#include "metadata.h"
#include <gtest/gtest.h>

#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace {

using sendspin::server::Metadata;
using sendspin::server::METADATA_SCHEDULE_AHEAD_US;
using sendspin::server::MetadataProgress;
using sendspin::server::MetadataSends;
using sendspin::server::MetadataState;
using sendspin::server::MetadataTimeline;

constexpr int64_t S = 1000000;

Metadata titled(const std::string& title, std::optional<MetadataProgress> progress = {}) {
    Metadata metadata;
    metadata.title = title;
    metadata.progress = progress;
    return metadata;
}

// metadata/v1.md "Calculating current track position": the position at the timestamp plus the
// elapsed time at the playback speed, within the track's length
TEST(MetadataAt, CarriesTheProgressForward) {
    struct Row {
        const char* name;
        MetadataProgress progress;
        int64_t at_us;
        uint32_t expected_ms;
    };
    const Row rows[] = {
        {"Control: normal speed", {1000, 0, 1000}, 2500000, 3500},
        {"half speed", {1000, 0, 500}, 2500000, 2250},
        {"paused", {1000, 0, 0}, 2500000, 1000},
        {"within the track's length", {1000, 3000, 1000}, 2500000, 3000},
        {"an earlier time leaves it", {1000, 0, 1000}, -5, 1000},
    };
    for (const Row& row : rows) {
        SCOPED_TRACE(row.name);
        const MetadataState moved =
            sendspin::server::metadata_at(MetadataState{0, titled("A", row.progress)}, row.at_us);
        EXPECT_EQ(moved.timestamp_us, row.at_us);
        EXPECT_EQ(moved.metadata.progress->track_progress_ms, row.expected_ms);
        EXPECT_EQ(moved.metadata.title, "A");
    }
}

std::vector<std::string> titles(const MetadataSends& sends) {
    std::vector<std::string> out;
    for (const MetadataState& state : sends.states) {
        out.push_back(state.metadata.title.value_or("-") + "@" +
                      std::to_string(state.timestamp_us / S));
    }
    return out;
}

// metadata/v1.md "Scheduled metadata updates" and "Server rules for scheduled metadata": one
// current state and at most one scheduled update; an update is sent at most 20 s early; a new one
// replaces it; one already sent is cancelled by resending the current state; a past or present
// state discards it. A pause sends the current state at speed 0, and the resume restores the
// speed from the run's first sample.
TEST(MetadataTimeline, SendsEachStepsStatesInOrder) {
    MetadataTimeline timeline;
    // Nothing set: an empty state at the time asked
    EXPECT_EQ(timeline.current(7 * S).timestamp_us, 7 * S);
    EXPECT_FALSE(timeline.current(7 * S).metadata.title);

    struct Step {
        const char* name;
        std::function<MetadataSends(MetadataTimeline&)> step;
        std::vector<std::string> sends;
        std::optional<int64_t> next_due;
    };
    const MetadataProgress playing{10000, 200000, 1000};
    const Step steps[] = {
        {"Control: a present state is sent at once",
         [&](MetadataTimeline& t) { return t.set(titled("A", playing), 100 * S, 100 * S); },
         {"A@100"},
         std::nullopt},
        {"one 30 s ahead is held",
         [&](MetadataTimeline& t) { return t.set(titled("B"), 130 * S, 100 * S); },
         {},
         110 * S},
        {"before its window nothing is due",
         [&](MetadataTimeline& t) { return t.advance(109 * S); },
         {},
         110 * S},
        {"20 s ahead it is sent",
         [&](MetadataTimeline& t) { return t.advance(110 * S); },
         {"B@130"},
         130 * S},
        {"a replacement held cancels the sent one with the current state",
         [&](MetadataTimeline& t) { return t.set(titled("C"), 150 * S, 111 * S); },
         {"A@100"},
         130 * S},
        {"Control: cancelling one not sent sends nothing",
         [&](MetadataTimeline& t) { return t.cancel(112 * S); },
         {},
         std::nullopt},
        {"one within 20 s is sent at once",
         [&](MetadataTimeline& t) { return t.set(titled("D"), 120 * S, 113 * S); },
         {"D@120"},
         120 * S},
        {"cancelling a sent one resends the current state",
         [&](MetadataTimeline& t) { return t.cancel(114 * S); },
         {"A@100"},
         std::nullopt},
        {"a pause: the current state at speed 0",
         [&](MetadataTimeline& t) { return t.stop(115 * S, true); },
         {"A@115"},
         std::nullopt},
        {"the resume restores the speed from the first sample",
         [&](MetadataTimeline& t) { return t.resume(118 * S, 116 * S); },
         {"A@118"},
         118 * S},
        {"its time comes: applied, nothing more to send",
         [&](MetadataTimeline& t) { return t.advance(118 * S); },
         {},
         std::nullopt},
        {"a stop: speed 0, and no resume restores it",
         [&](MetadataTimeline& t) {
             MetadataSends sends = t.stop(119 * S, false);
             const MetadataSends resumed = t.resume(121 * S, 120 * S);
             sends.states.insert(sends.states.end(), resumed.states.begin(), resumed.states.end());
             return sends;
         },
         {"A@119"},
         std::nullopt},
    };
    for (const Step& step : steps) {
        SCOPED_TRACE(step.name);
        EXPECT_EQ(titles(step.step(timeline)), step.sends);
        EXPECT_EQ(timeline.next_due_us(), step.next_due);
    }
}

// metadata/v1.md: the paused state keeps the position reached, and the resumed one starts there
// at the speed the pause took away
TEST(MetadataTimeline, PausesAtThePositionReachedAndResumesFromIt) {
    MetadataTimeline timeline;
    timeline.set(titled("A", MetadataProgress{10000, 200000, 1500}), 0, 0);
    const MetadataSends paused = timeline.stop(4 * S, true);
    ASSERT_EQ(paused.states.size(), 1U);
    EXPECT_EQ(paused.states[0].metadata.progress, (MetadataProgress{16000, 200000, 0}));

    const MetadataSends resumed = timeline.resume(10 * S, 9 * S);
    ASSERT_EQ(resumed.states.size(), 1U);
    EXPECT_EQ(resumed.states[0].timestamp_us, 10 * S);
    EXPECT_EQ(resumed.states[0].metadata.progress, (MetadataProgress{16000, 200000, 1500}));
    // Until then the members hold the paused state, then the resumed one as scheduled
    EXPECT_EQ(timeline.current(9 * S).metadata.progress->playback_speed, 0U);
    ASSERT_TRUE(timeline.sent_pending());
    EXPECT_EQ(timeline.sent_pending()->timestamp_us, 10 * S);

    // Control: with no position there is no speed to restore
    MetadataTimeline untimed;
    untimed.set(titled("B"), 0, 0);
    EXPECT_EQ(titles(untimed.stop(S, true)), std::vector<std::string>{"B@1"});
    EXPECT_TRUE(untimed.resume(3 * S, 2 * S).states.empty());
    static_assert(METADATA_SCHEDULE_AHEAD_US == 20 * S);
}

// metadata/v1.md: a new state on play, pause, resume and seek. While paused, metadata the host
// sets (a seek, a new track) goes out at speed 0 and the resume restores its speed; a host that
// reports the pause at speed 0 before the run's end reaches core still gets its speed back.
// Control: after a stop, what the host sets goes out as set.
TEST(MetadataTimeline, KeepsAPauseThroughWhatTheHostSets) {
    MetadataTimeline timeline;
    timeline.set(titled("A", MetadataProgress{10000, 200000, 1500}), 0, 0);
    // The host's own report of the pause, before the run's end
    timeline.set(titled("A", MetadataProgress{12000, 200000, 0}), 2 * S, 2 * S);
    const MetadataSends paused = timeline.stop(3 * S, true);
    ASSERT_EQ(paused.states.size(), 1U);
    EXPECT_EQ(paused.states[0].metadata.progress, (MetadataProgress{12000, 200000, 0}));
    const MetadataSends back = timeline.resume(4 * S, 3 * S);
    ASSERT_EQ(back.states.size(), 1U);
    EXPECT_EQ(back.states[0].metadata.progress, (MetadataProgress{12000, 200000, 1500}));
    timeline.advance(4 * S);

    // A seek while paused, which the host reports at its playing speed
    timeline.stop(4 * S, true);
    const MetadataSends seeked =
        timeline.set(titled("A", MetadataProgress{50000, 200000, 1000}), 4 * S, 4 * S);
    ASSERT_EQ(seeked.states.size(), 1U);
    EXPECT_EQ(seeked.states[0].metadata.progress, (MetadataProgress{50000, 200000, 0}));
    const MetadataSends resumed = timeline.resume(6 * S, 5 * S);
    ASSERT_EQ(resumed.states.size(), 1U);
    EXPECT_EQ(resumed.states[0].timestamp_us, 6 * S);
    EXPECT_EQ(resumed.states[0].metadata.progress, (MetadataProgress{50000, 200000, 1000}));

    timeline.advance(6 * S);
    timeline.stop(7 * S, false);
    const MetadataSends after_stop =
        timeline.set(titled("B", MetadataProgress{0, 100000, 1000}), 8 * S, 8 * S);
    ASSERT_EQ(after_stop.states.size(), 1U);
    EXPECT_EQ(after_stop.states[0].metadata.progress->playback_speed, 1000U);
}

// The position carried forward cannot overflow, however long ago the timestamp
TEST(MetadataAt, SaturatesAtTheFieldsRange) {
    const MetadataState state{0, titled("A", MetadataProgress{1000, 0, 100000})};
    const MetadataState moved =
        sendspin::server::metadata_at(state, std::numeric_limits<int64_t>::max());
    EXPECT_EQ(moved.metadata.progress->track_progress_ms, std::numeric_limits<uint32_t>::max());
}

}  // namespace
