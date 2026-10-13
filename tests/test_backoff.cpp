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

/// @file test_backoff.cpp
/// @brief The handshake backoff schedule (messaging.md "client/goodbye")

#include "backoff.h"
#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <optional>

namespace {

using sendspin::server::BackoffLimits;
using sendspin::server::HandshakeBackoff;
using sendspin::server::config_defaults::HANDSHAKE_BACKOFF_MAX_US;
using sendspin::server::config_defaults::HANDSHAKE_BACKOFF_MIN_US;

TEST(HandshakeBackoff, DoublesFromTheMinimumUpToTheMaximum) {
    struct Row {
        BackoffLimits limits;
        uint32_t failures;
        int64_t wait_us;
    };
    const BackoffLimits defaults;
    const BackoffLimits short_limits{100, 250};
    const Row rows[] = {
        {defaults, 0, 0},
        {defaults, 1, HANDSHAKE_BACKOFF_MIN_US},
        {defaults, 2, 2 * HANDSHAKE_BACKOFF_MIN_US},
        {defaults, 3, 4 * HANDSHAKE_BACKOFF_MIN_US},
        {defaults, 6, 32 * HANDSHAKE_BACKOFF_MIN_US},
        {defaults, 7, HANDSHAKE_BACKOFF_MAX_US},
        {defaults, std::numeric_limits<uint32_t>::max(), HANDSHAKE_BACKOFF_MAX_US},
        {short_limits, 1, 100},
        {short_limits, 2, 200},
        {short_limits, 3, 250},
    };
    for (const Row& row : rows) {
        SCOPED_TRACE(testing::Message() << row.limits.min_us << " " << row.failures);
        EXPECT_EQ(sendspin::server::handshake_backoff_us(row.failures, row.limits), row.wait_us);
    }
}

TEST(HandshakeBackoff, WaitsFromTheLastFailureAndClearsOnSuccess) {
    HandshakeBackoff backoff;
    EXPECT_EQ(backoff.next_dial_us(), 0);
    backoff.record_failure(1000, BackoffLimits{});
    EXPECT_EQ(backoff.next_dial_us(), 1000 + HANDSHAKE_BACKOFF_MIN_US);
    backoff.record_failure(5000, BackoffLimits{});
    EXPECT_EQ(backoff.failures(), 2U);
    EXPECT_EQ(backoff.next_dial_us(), 5000 + (2 * HANDSHAKE_BACKOFF_MIN_US));
    backoff.record_success();
    EXPECT_EQ(backoff.failures(), 0U);
    EXPECT_EQ(backoff.next_dial_us(), 0);
    backoff.record_failure(9000, BackoffLimits{});
    EXPECT_EQ(backoff.next_dial_us(), 9000 + HANDSHAKE_BACKOFF_MIN_US);
}

// A connection that ends so that its target is dialed again waits as after one failure more than
// the ends in a row before it, a dial on the server's own only, and a connection that lasted the
// maximum wait starts the count over; a later success leaves the wait
TEST(HandshakeBackoff, WaitsLongerAfterEachReconnectInARow) {
    const BackoffLimits limits{100, 250};
    HandshakeBackoff backoff;
    EXPECT_EQ(backoff.next_reconnect_us(), 0);
    backoff.record_reconnect(1000, 990, limits);
    EXPECT_EQ(backoff.next_reconnect_us(), 1100);
    // A requested dial does not wait for it
    EXPECT_EQ(backoff.next_dial_us(), 0);
    EXPECT_EQ(backoff.failures(), 0U);
    backoff.record_success();
    backoff.record_reconnect(2000, 1990, limits);
    EXPECT_EQ(backoff.next_reconnect_us(), 2200);
    backoff.record_reconnect(3000, 2990, limits);
    EXPECT_EQ(backoff.next_reconnect_us(), 3250);
    // Control: a connection that lasted the maximum wait
    backoff.record_reconnect(4000, 4000 - 250, limits);
    EXPECT_EQ(backoff.next_reconnect_us(), 4100);
    // The later of the failures' wait and the reconnects'
    backoff.record_failure(4050, limits);
    EXPECT_EQ(backoff.next_dial_us(), 4150);
    EXPECT_EQ(backoff.next_reconnect_us(), 4150);
}

// messaging.md "client/goodbye": backoff on repeated handshake failures; a goodbye is the
// client's decision, and it stays one when the client closes right after sending it, since the
// transport's close is only consulted with the session still open (nullopt)
TEST(HandshakeBackoff, CountsEveryEndBeforeActivationButAGoodbye) {
    using sendspin::server::SessionEnd;
    struct Row {
        const char* name;
        std::optional<SessionEnd> end;
        bool failure;
    };
    const Row rows[] = {
        {"Control: a goodbye", SessionEnd::GOODBYE, false},
        {"the host's disconnect", SessionEnd::CLOSED_BY_HOST, false},
        {"init rejected", SessionEnd::INIT_REJECTED, true},
        {"timed out", SessionEnd::TIMED_OUT, true},
        {"handshake failed", SessionEnd::HANDSHAKE_FAILED, true},
        {"protocol error", SessionEnd::PROTOCOL_ERROR, true},
        {"the transport closed first", std::nullopt, true},
    };
    for (const Row& row : rows) {
        SCOPED_TRACE(row.name);
        EXPECT_EQ(sendspin::server::is_handshake_failure(row.end), row.failure);
    }
}

// A set session end decides over the transport's close that may follow it; with none, a client's
// WebSocket violation is the server's close
TEST(DisconnectReason, FollowsTheSessionEndThenTheTransport) {
    using sendspin::server::CloseReason;
    using sendspin::server::DisconnectReason;
    using sendspin::server::SessionEnd;
    struct Row {
        const char* name;
        std::optional<SessionEnd> end;
        std::optional<CloseReason> transport;
        DisconnectReason reason;
    };
    const Row rows[] = {
        {"a goodbye, the peer closing after it", SessionEnd::GOODBYE, CloseReason::PEER_CLOSED,
         DisconnectReason::GOODBYE},
        {"a protocol error, the peer closing after it", SessionEnd::PROTOCOL_ERROR,
         CloseReason::PEER_CLOSED, DisconnectReason::CLOSED_BY_SERVER},
        {"a queue overflow", SessionEnd::QUEUE_OVERFLOW, std::nullopt,
         DisconnectReason::CLOSED_BY_SERVER},
        {"the host's disconnect", SessionEnd::CLOSED_BY_HOST, std::nullopt,
         DisconnectReason::DISCONNECTED_BY_HOST},
        {"Control: the peer closed first", std::nullopt, CloseReason::PEER_CLOSED,
         DisconnectReason::CONNECTION_LOST},
        {"a write stall", std::nullopt, CloseReason::WRITE_STALLED,
         DisconnectReason::CONNECTION_LOST},
        {"a transport failure", std::nullopt, CloseReason::FAILED,
         DisconnectReason::CONNECTION_LOST},
        {"a WebSocket violation", std::nullopt, CloseReason::PROTOCOL_VIOLATION,
         DisconnectReason::CLOSED_BY_SERVER},
    };
    for (const Row& row : rows) {
        SCOPED_TRACE(row.name);
        EXPECT_EQ(sendspin::server::disconnect_reason(row.end, row.transport), row.reason);
    }
}

}  // namespace
