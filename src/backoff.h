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

/// @file backoff.h
/// @brief Exponential backoff on repeated handshake failures toward one dial target
/// (messaging.md "client/goodbye"), and how a connection's end counts toward it and is reported.
/// The session's end is recorded before the transport can report a close, so a set end decides:
/// a client that closes right after its client/goodbye still ended with the goodbye.

#pragma once

#include "sendspin/server/config.h"
#include "sendspin/server/types.h"
#include "session.h"
#include "transport.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace sendspin::server {

/// @brief The range of the wait before a dial after failures; min_us positive, max_us at least
/// min_us
struct BackoffLimits {
    int64_t min_us{config_defaults::HANDSHAKE_BACKOFF_MIN_US};
    int64_t max_us{config_defaults::HANDSHAKE_BACKOFF_MAX_US};
};

/// @brief The wait before the next dial after `failures` consecutive failures: none after none,
/// then limits.min_us, doubling up to limits.max_us
int64_t handshake_backoff_us(uint32_t failures, const BackoffLimits& limits);

/// @brief Whether a connection that ended before activation counts as a failed handshake toward
/// its dial target: every end does except a client/goodbye, the client's own decision, and the
/// host's disconnect()
/// @param end the session's end; nullopt when the transport closed with the session still open
bool is_handshake_failure(std::optional<SessionEnd> end);

/// @brief The on_error() report of a connection that ended before activation:
/// GOODBYE_BEFORE_ACTIVATION with the reason when the session ended with a client/goodbye,
/// HANDSHAKE_FAILED otherwise
ServerError error_before_activation(const std::string& url, const Session& session);

/// @brief Why a connected client went away: from its session's end, or when the transport closed
/// with the session still open (end nullopt), from the transport's reason
DisconnectReason disconnect_reason(std::optional<SessionEnd> end,
                                   std::optional<CloseReason> transport);

/// @brief Why a connection ended, for the log: the session's end, with the client/goodbye's or the
/// server/error's reason, or when the transport closed with the session still open (end
/// nullopt), the transport's reason
std::string end_description(const Session& session, std::optional<CloseReason> transport);

/// @brief Why a dial failed or a connection's transport closed, for the log
std::string_view close_description(CloseReason reason);

/// @brief One dial target's failure count and the earliest time of its next dial
class HandshakeBackoff {
public:
    void record_failure(int64_t now_us, const BackoffLimits& limits) {
        ++this->failures_;
        this->next_dial_us_ = now_us + handshake_backoff_us(this->failures_, limits);
    }

    void record_success() {
        this->failures_ = 0;
        this->next_dial_us_ = 0;
    }

    /// @brief A connection that ended so that its target is dialed again on its own: that dial
    /// waits as after as many failures as such ends in a row. A connection that lasted
    /// limits.max_us or longer starts the count over, so one that keeps ending at once backs off.
    /// @param connected_us when the connection was activated
    void record_reconnect(int64_t now_us, int64_t connected_us, const BackoffLimits& limits) {
        if (now_us - connected_us >= limits.max_us) {
            this->reconnects_ = 0;
        }
        ++this->reconnects_;
        this->reconnect_at_us_ = now_us + handshake_backoff_us(this->reconnects_, limits);
    }

    /// @brief A dial requested earlier waits until then
    int64_t next_dial_us() const {
        return this->next_dial_us_;
    }

    /// @brief A dial the server makes on its own waits until then: past the failures' wait and
    /// the reconnects'
    int64_t next_reconnect_us() const {
        return this->next_dial_us_ > this->reconnect_at_us_ ? this->next_dial_us_
                                                            : this->reconnect_at_us_;
    }

    uint32_t failures() const {
        return this->failures_;
    }

private:
    uint32_t failures_{0};
    uint32_t reconnects_{0};
    int64_t next_dial_us_{0};
    int64_t reconnect_at_us_{0};
};

}  // namespace sendspin::server
