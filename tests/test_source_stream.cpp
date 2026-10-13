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

/// @file test_source_stream.cpp
/// @brief SourceStream: when the server asks a source to start and stop (roles/source/v1.md
/// "Source command semantics"), and what each of the client's input stream messages does
/// ("Default streaming behavior", "Source Audio Chunks (Binary)")

#include "source_stream.h"
#include <gtest/gtest.h>

#include <array>
#include <optional>
#include <vector>

namespace {

using sendspin::server::AudioFormat;
using sendspin::server::ClientState;
using sendspin::server::ClientStreamStart;
using sendspin::server::Codec;
using sendspin::server::SourceCommand;
using sendspin::server::SourceEvent;
using sendspin::server::SourceSignal;
using sendspin::server::SourceState;
using sendspin::server::SourceStream;
using Verdict = sendspin::server::SourceStream::Verdict;

constexpr std::array<Codec, 2> ACCEPTED{Codec::PCM, Codec::FLAC};

ClientState state(bool available, bool source_object = true) {
    ClientState out;
    out.available = available;
    if (source_object) {
        out.source = SourceState{};
    }
    return out;
}

ClientStreamStart pcm_start(uint32_t rate = 48000) {
    return ClientStreamStart{AudioFormat{Codec::PCM, 2, rate, 16}, {}, 0};
}

/// A source with the role written, its client/state in, available and routed: ready to be started
SourceStream ready_source() {
    SourceStream source;
    source.on_activation_written(true);
    source.on_client_state(state(true));
    source.set_routed(true);
    return source;
}

std::vector<SourceEvent::Kind> kinds(SourceStream& source) {
    std::vector<SourceEvent::Kind> out;
    for (const SourceEvent& event : source.take_events()) {
        out.push_back(event.kind);
    }
    return out;
}

// roles/source/v1.md "Source command semantics": the server MUST NOT send start unless the role is
// active (in the activation written and the one ahead), it received the source object in the
// client/state that activation asks for, and the latest client/state reports available; the
// server sends start only for a routed source, and once
TEST(SourceStreamCommands, StartOnlyWhenEveryPreconditionHolds) {
    struct Row {
        const char* name;
        bool role;
        bool state_object;
        bool available;
        bool routed;
        bool role_ahead;
        bool start;
    };
    const Row rows[] = {
        {"Control: everything holds", true, true, true, true, true, true},
        {"the role not active", false, true, true, true, true, false},
        {"the activation ahead removes it", true, true, true, true, false, false},
        {"no source object since the activation", true, false, true, true, true, false},
        {"unavailable", true, true, false, true, true, false},
        {"not routed", true, true, true, false, true, false},
    };
    for (const Row& row : rows) {
        SCOPED_TRACE(row.name);
        SourceStream source;
        source.on_activation_written(row.role);
        source.on_client_state(state(row.available, row.state_object));
        source.set_routed(row.routed);
        const std::optional<SourceCommand> command = source.take_command(row.role_ahead);
        EXPECT_EQ(command, row.start ? std::optional(SourceCommand::START) : std::nullopt);
        // Never twice
        EXPECT_EQ(source.take_command(row.role_ahead), std::nullopt);
    }
}

// messaging.md "client/state": a source object that came before the activation that adds the
// role does not count; the one after it does
TEST(SourceStreamCommands, WaitForTheSourceObjectAfterTheActivationAddingTheRole) {
    SourceStream source;
    source.on_client_state(state(true));
    source.set_routed(true);
    source.on_activation_written(true);
    EXPECT_EQ(source.take_command(true), std::nullopt);
    source.on_client_state(state(true));
    EXPECT_EQ(source.take_command(true), SourceCommand::START);
}

// roles/source/v1.md "Source command semantics": unrouting sends stop; unavailability and role
// removal are implicit stops, so no stop goes out for them, and a start goes out again only after
// the client is available or its role active again
TEST(SourceStreamCommands, StopExplicitlyOnlyWhileTheClientNeedsIt) {
    SourceStream unrouted = ready_source();
    ASSERT_EQ(unrouted.take_command(true), SourceCommand::START);
    unrouted.set_routed(false);
    EXPECT_EQ(unrouted.take_command(true), SourceCommand::STOP);
    EXPECT_EQ(unrouted.take_command(true), std::nullopt);
    unrouted.set_routed(true);
    EXPECT_EQ(unrouted.take_command(true), SourceCommand::START);

    SourceStream unavailable = ready_source();
    ASSERT_EQ(unavailable.take_command(true), SourceCommand::START);
    unavailable.on_client_state(state(false));
    EXPECT_EQ(unavailable.take_command(true), std::nullopt);
    unavailable.on_client_state(state(true));
    EXPECT_EQ(unavailable.take_command(true), SourceCommand::START);

    SourceStream removed = ready_source();
    ASSERT_EQ(removed.take_command(true), SourceCommand::START);
    EXPECT_EQ(removed.take_command(false), std::nullopt);
    removed.on_activation_written(false);
    removed.on_activation_written(true);
    EXPECT_EQ(removed.take_command(true), std::nullopt) << "waits for the source object";
    removed.on_client_state(state(true));
    EXPECT_EQ(removed.take_command(true), SourceCommand::START);
}

// roles/source/v1.md "Default streaming behavior": a client-stream/start that opens a stream with
// no start sent is a protocol error, as is a codec server/hello does not list; each start
// authorizes one opening, also when the stop sent after it reached the client later; on an open
// stream it replaces the format in place
TEST(SourceStreamInput, OpenOnlyAnswersToAStart) {
    SourceStream unsolicited = ready_source();
    EXPECT_EQ(unsolicited.on_stream_start(pcm_start(), ACCEPTED, true), Verdict::PROTOCOL_ERROR);

    SourceStream unlisted = ready_source();
    ASSERT_EQ(unlisted.take_command(true), SourceCommand::START);
    EXPECT_EQ(
        unlisted.on_stream_start({AudioFormat{Codec::OPUS, 2, 48000, 0}, {}, 0}, ACCEPTED, true),
        Verdict::PROTOCOL_ERROR);

    SourceStream source = ready_source();
    ASSERT_EQ(source.take_command(true), SourceCommand::START);
    source.set_routed(false);
    ASSERT_EQ(source.take_command(true), SourceCommand::STOP);
    // Control: the start's answer, in flight past the stop, opens the stream
    EXPECT_EQ(source.on_stream_start(pcm_start(), ACCEPTED, true), Verdict::TAKE);
    EXPECT_EQ(source.stream_number(), 1U);
    EXPECT_EQ(source.on_stream_start(pcm_start(44100), ACCEPTED, true), Verdict::TAKE)
        << "in place";
    EXPECT_EQ(source.stream_number(), 1U);
    ASSERT_TRUE(source.stream());
    EXPECT_EQ(source.stream()->format.sample_rate, 44100U);
    source.on_stream_end();
    EXPECT_FALSE(source.stream());
    // The start was answered once
    EXPECT_EQ(source.on_stream_start(pcm_start(), ACCEPTED, true), Verdict::PROTOCOL_ERROR);
    EXPECT_EQ(kinds(source), (std::vector{SourceEvent::Kind::READY, SourceEvent::Kind::OPENED,
                                          SourceEvent::Kind::OPENED, SourceEvent::Kind::CLOSED}));
}

// roles/source/v1.md "Source Audio Chunks (Binary)": a chunk with no stream open, or while the
// latest client/state reports unavailable, is a protocol error; after a stop or the end of the
// route the server may discard it, and so it does for a stream it cannot take
TEST(SourceStreamInput, TakeChunksOnlyOfAnOpenStream) {
    SourceStream source = ready_source();
    EXPECT_EQ(source.on_chunk(), Verdict::PROTOCOL_ERROR) << "no stream";
    ASSERT_EQ(source.take_command(true), SourceCommand::START);
    ASSERT_EQ(source.on_stream_start(pcm_start(), ACCEPTED, true), Verdict::TAKE);
    EXPECT_EQ(source.on_chunk(), Verdict::TAKE);
    source.set_routed(false);
    EXPECT_EQ(source.on_chunk(), Verdict::DISCARD);
    source.set_routed(true);
    source.fail();
    EXPECT_EQ(source.on_chunk(), Verdict::DISCARD);
    source.on_client_state(state(false));
    EXPECT_EQ(source.on_chunk(), Verdict::PROTOCOL_ERROR);
}

// A failed stream is stopped and not started again until something changes; a stream the source
// ended on its own after the start that opened it likewise, while one ended in answer to a stop
// (a start sent since still unanswered) is not
TEST(SourceStreamCommands, HoldBackAStartAfterAFailureOrTheSourcesOwnEnd) {
    SourceStream failed = ready_source();
    ASSERT_EQ(failed.take_command(true), SourceCommand::START);
    ASSERT_EQ(failed.on_stream_start(pcm_start(), ACCEPTED, true), Verdict::TAKE);
    failed.fail();
    EXPECT_EQ(failed.take_command(true), SourceCommand::STOP);
    failed.on_stream_end();
    EXPECT_EQ(failed.take_command(true), std::nullopt);
    failed.set_routed(false);
    failed.set_routed(true);
    EXPECT_EQ(failed.take_command(true), SourceCommand::START) << "routed again";

    SourceStream ended = ready_source();
    ASSERT_EQ(ended.take_command(true), SourceCommand::START);
    ASSERT_EQ(ended.on_stream_start(pcm_start(), ACCEPTED, true), Verdict::TAKE);
    ended.on_stream_end();
    EXPECT_EQ(ended.take_command(true), SourceCommand::STOP);
    EXPECT_EQ(ended.take_command(true), std::nullopt);
    ended.on_client_state(state(false));
    ended.on_client_state(state(true));
    EXPECT_EQ(ended.take_command(true), SourceCommand::START) << "available again";

    // Control: stop, then start again before the stop's end arrives
    SourceStream restarted = ready_source();
    ASSERT_EQ(restarted.take_command(true), SourceCommand::START);
    ASSERT_EQ(restarted.on_stream_start(pcm_start(), ACCEPTED, true), Verdict::TAKE);
    restarted.set_routed(false);
    ASSERT_EQ(restarted.take_command(true), SourceCommand::STOP);
    restarted.set_routed(true);
    ASSERT_EQ(restarted.take_command(true), SourceCommand::START);
    restarted.on_stream_end();
    EXPECT_EQ(restarted.take_command(true), std::nullopt);
    EXPECT_EQ(restarted.on_stream_start(pcm_start(), ACCEPTED, true), Verdict::TAKE);
}

// A stream the server cannot take is reported as failed and never as opened; an open stream moved
// to another route is reported as opened there, unless it failed or ended
TEST(SourceStreamEvents, ReportAStreamOpenedWhereverItGoes) {
    SourceStream refused = ready_source();
    ASSERT_EQ(refused.take_command(true), SourceCommand::START);
    (void)refused.take_events();
    ASSERT_EQ(refused.on_stream_start(pcm_start(), ACCEPTED, false), Verdict::TAKE);
    EXPECT_TRUE(refused.failed());
    EXPECT_EQ(kinds(refused), std::vector{SourceEvent::Kind::FAILED});
    refused.on_rerouted();
    EXPECT_TRUE(kinds(refused).empty());

    SourceStream moved = ready_source();
    ASSERT_EQ(moved.take_command(true), SourceCommand::START);
    ASSERT_EQ(moved.on_stream_start(pcm_start(), ACCEPTED, true), Verdict::TAKE);
    EXPECT_EQ(kinds(moved), (std::vector{SourceEvent::Kind::READY, SourceEvent::Kind::OPENED}))
        << "Control";
    moved.on_rerouted();
    EXPECT_EQ(kinds(moved), std::vector{SourceEvent::Kind::OPENED});
    moved.on_stream_end();
    moved.on_rerouted();
    EXPECT_EQ(kinds(moved), std::vector{SourceEvent::Kind::CLOSED});
}

// What the owner hears: readiness as it changes, and the line sensing a source reports
TEST(SourceStreamEvents, ReportReadinessAndSignal) {
    SourceStream source;
    source.on_activation_written(true);
    ClientState present = state(true);
    present.source->signal = SourceSignal::PRESENT;
    source.on_client_state(present);
    source.on_client_state(present);
    source.on_client_state(state(false));
    const std::vector<SourceEvent> events = source.take_events();
    ASSERT_EQ(events.size(), 4U);
    EXPECT_EQ(events[0].kind, SourceEvent::Kind::SIGNAL);
    EXPECT_EQ(events[0].signal, SourceSignal::PRESENT);
    EXPECT_EQ(events[1].kind, SourceEvent::Kind::READY);
    EXPECT_EQ(events[2].kind, SourceEvent::Kind::SIGNAL);
    EXPECT_EQ(events[2].signal, std::nullopt);
    EXPECT_EQ(events[3].kind, SourceEvent::Kind::UNREADY);
}

}  // namespace
