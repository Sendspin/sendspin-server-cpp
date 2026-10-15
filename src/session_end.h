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

/// @file session_end.h
/// @brief SessionEnd, why a session ended, and SessionOutcome, what its end is reported by

#pragma once

#include "messages.h"

#include <cstdint>
#include <optional>
#include <string>

namespace sendspin::server {

/// @brief Why a session ended. Only INIT_REJECTED leaves a message to send. Every end but it and
/// GOODBYE is a silent failure (connection.md "Failure Handling").
enum class SessionEnd : uint8_t {
    INIT_REJECTED,  ///< server/error queued; Session::init_error() says which
    /// A handshake step, or a re-handshake's wait for message 2, outlived
    /// SessionSettings::step_timeout_us
    TIMED_OUT,
    /// The Noise state could not be created or write message 1, or message 2 was missing,
    /// malformed, unauthenticated or not `{}`, in the handshake or a re-handshake
    HANDSHAKE_FAILED,
    DECRYPT_FAILED,          ///< An AEAD failure in transport mode
    CLEARTEXT_IN_TRANSPORT,  ///< A text message after the switch to transport mode
    MALFORMED_FRAGMENTS,     ///< A malformed sequence (messaging.md "Fragmentation")
    /// A fragmented message outgrew SessionSettings::max_reassembled_bytes
    MESSAGE_TOO_LARGE,
    PROTOCOL_ERROR,  ///< A malformed message, or one the phase does not allow
    ENCRYPT_FAILED,  ///< The send cipher refused a message
    QUEUE_OVERFLOW,  ///< A send queue passed its cap
    ENCODE_FAILED,   ///< A message to send held a value over MAX_JSON_STRING_BYTES
    GOODBYE,         ///< client/goodbye received; Session::goodbye() holds it
    CLOSED_BY_HOST,  ///< Session::close_by_host()
    REFUSED,         ///< Session::refuse()
};

/// @brief What a session's end is reported by (backoff.h), as a value: the end, the client_id and
/// the message that ended it
struct SessionOutcome {
    /// nullopt while the session is open
    std::optional<SessionEnd> end;
    /// The validated client/init's client_id; empty before one
    std::string client_id;
    /// With SessionEnd::INIT_REJECTED: the reason sent in server/error
    std::optional<InitError> init_error;
    /// With SessionEnd::GOODBYE: the message received
    std::optional<ClientGoodbye> goodbye;
};

}  // namespace sendspin::server
