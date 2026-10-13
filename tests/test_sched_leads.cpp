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

/// @file test_sched_leads.cpp
/// @brief Leads, pipeline latency, the group lead, the startup bound and the buffered write-ahead

#include "sched/constants.h"
#include "sched/leads.h"
#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

namespace {

using sendspin::server::sched::CapacityRate;
using sendspin::server::sched::InputKind;
using sendspin::server::sched::LeadInput;
using sendspin::server::sched::PipelineShape;
using sendspin::server::sched::PlayerLead;
using sendspin::server::sched::PlayerTiming;
using sendspin::server::sched::SchedTiming;
using sendspin::server::sched::TriggerKind;
namespace sched = sendspin::server::sched;

constexpr int64_t MS = 1000;
constexpr int64_t CLAMP = 10000 * MS;

constexpr PlayerLead LEAD_A{300 * MS, 100 * MS};
constexpr PlayerLead LEAD_B{550 * MS, 250 * MS};

TEST(SchedLeads, PlayerLead) {
    struct Row {
        const char* name;
        PlayerTiming timing;
        InputKind kind;
        int64_t lead;
        int64_t floor;
    };
    const Row rows[] = {
        {"Control: buffered 100/300/0", {100, 300, 0}, InputKind::BUFFERED, 300 * MS, 100 * MS},
        {"buffered rl below mb", {300, 100, 0}, InputKind::BUFFERED, 300 * MS, 300 * MS},
        {"live ignores rl", {100, 300, 0}, InputKind::LIVE, 100 * MS, 100 * MS},
        {"od added, buffered", {100, 300, 50}, InputKind::BUFFERED, 350 * MS, 150 * MS},
        {"od added, live", {100, 300, 50}, InputKind::LIVE, 150 * MS, 150 * MS},
        {"mb clamps at T_clamp", {20000, 0, 0}, InputKind::BUFFERED, CLAMP, CLAMP},
        {"rl clamps at T_clamp", {100, 50000, 0}, InputKind::BUFFERED, CLAMP, 100 * MS},
        {"player B", {200, 500, 50}, InputKind::BUFFERED, 550 * MS, 250 * MS},
    };
    for (const Row& row : rows) {
        SCOPED_TRACE(row.name);
        const PlayerLead lead = sched::player_lead_us(row.timing, row.kind, CLAMP);
        EXPECT_EQ(lead.lead_us, row.lead);
        EXPECT_EQ(lead.floor_us, row.floor);
    }
}

TEST(SchedLeads, PipelineLatency) {
    struct Row {
        const char* name;
        PipelineShape shape;
        InputKind kind;
        int64_t jitter;
        int64_t latency;
    };
    const Row rows[] = {
        {"buffered is 0", {1, 104, 44100}, InputKind::BUFFERED, 20 * MS, 0},
        {"Control: live, no resampler", {1, 0, 48000}, InputKind::LIVE, 10 * MS, 30 * MS},
        // ceil(53 * 10^6 / 44,100) = 1,202 (the MEDIUM filter)
        {"live with a resampler", {1, 104, 44100}, InputKind::LIVE, 20 * MS, 41202},
        {"live two-cell frame", {2, 0, 11025}, InputKind::LIVE, 20 * MS, 60 * MS},
        // An Opus encoder's 2.5 ms lookahead
        {"live with an encoder delay", {1, 0, 48000, 2500}, InputKind::LIVE, 20 * MS, 42500},
        {"buffered with an encoder delay is 0",
         {1, 0, 48000, 2500},
         InputKind::BUFFERED,
         20 * MS,
         0},
    };
    for (const Row& row : rows) {
        SCOPED_TRACE(row.name);
        EXPECT_EQ(sched::pipeline_latency_us(row.shape, row.kind, row.jitter), row.latency);
    }
}

TEST(SchedLeads, GroupLead) {
    const SchedTiming timing;
    // D live, lead 60, L_p 30
    const LeadInput d{{60 * MS, 60 * MS}, 30 * MS};
    // E live, lead 190 (min_buffer 150, od 40)
    const LeadInput e{{190 * MS, 190 * MS}, 30 * MS};
    struct Row {
        const char* name;
        std::vector<LeadInput> members;
        int64_t lead;
    };
    const Row rows[] = {
        {"Control: A and B", {{LEAD_A, 0}, {LEAD_B, 0}}, 560 * MS},
        {"no eligible member gives G_default", {}, 250 * MS},
        {"J_min floor inside the latency term", {d}, 140 * MS},
        {"live with E", {d, e}, 230 * MS},
        {"the higher lead + L_p wins, not the higher lead",
         {{{300 * MS, 0}, 0}, {{200 * MS, 0}, 150 * MS}},
         360 * MS},
        {"joiner C leaves G at 560",
         {{LEAD_A, 0}, {LEAD_B, 0}, {{150 * MS, 150 * MS}, 0}},
         560 * MS},
    };
    for (const Row& row : rows) {
        SCOPED_TRACE(row.name);
        EXPECT_EQ(sched::group_lead_us(row.members, timing), row.lead);
    }
}

TEST(SchedLeads, StartupMinTs) {
    const int64_t j_min = sched::timing_defaults::JOIN_MIN_US;
    struct Row {
        const char* name;
        int64_t stamp;
        PlayerLead lead;
        TriggerKind kind;
        int64_t min_ts;
    };
    const Row rows[] = {
        // A join, lead 150
        {"Control: join", 5000100, {150 * MS, 150 * MS}, TriggerKind::JOIN, 5150100},
        // A's group trigger uses its floor 100, not its lead 300
        {"group uses the floor", 1050, LEAD_A, TriggerKind::GROUP, 101050},
        {"join uses the lead", 1050, LEAD_A, TriggerKind::JOIN, 301050},
        // B's group trigger, floor 250 above J_min
        {"floor above J_min", 1060, LEAD_B, TriggerKind::GROUP, 251060},
        {"floor below J_min", 0, {60 * MS, 60 * MS}, TriggerKind::GROUP, 100 * MS},
        {"lead below J_min", 0, {60 * MS, 60 * MS}, TriggerKind::JOIN, 100 * MS},
        // A's and B's clears
        {"A clear", 2000250, LEAD_A, TriggerKind::GROUP, 2100250},
        {"B clear", 2000120, LEAD_B, TriggerKind::GROUP, 2250120},
    };
    for (const Row& row : rows) {
        SCOPED_TRACE(row.name);
        EXPECT_EQ(sched::startup_min_ts(row.stamp, row.lead, row.kind, j_min), row.min_ts);
    }
}

TEST(SchedLeads, WriteAhead) {
    constexpr int64_t w_min = sched::timing_defaults::WRITE_AHEAD_MIN_US;
    constexpr int64_t w_max = sched::timing_defaults::WRITE_AHEAD_MAX_US;
    struct Row {
        const char* name;
        std::vector<CapacityRate> members;
        int64_t w;
    };
    // 1.5 s and 20 s at 100,000 B/s lie inside and above the clamp in either profile
    static_assert(w_min < 1500 * MS && 1500 * MS < w_max && w_max < 20000 * MS);
    const Row rows[] = {
        // 0.566 s and 0.320 s, clamped up to 1 s
        {"Control: A and B", {{100000, 176563}, {32000, 100130}}, 1000 * MS},
        {"inside the clamp", {{150000, 100000}, {32000, 100130}}, 1500 * MS},
        {"above W_max", {{2000000, 100000}}, w_max},
        {"no players", {}, 1000 * MS},
    };
    for (const Row& row : rows) {
        SCOPED_TRACE(row.name);
        EXPECT_EQ(sched::write_ahead_us(row.members, w_min, w_max), row.w);
    }
}

}  // namespace
