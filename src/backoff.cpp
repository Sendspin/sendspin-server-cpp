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

#include "backoff.h"

#include "messages.h"

namespace sendspin::server {

int64_t handshake_backoff_us(uint32_t failures, const BackoffLimits& limits) {
    if (failures == 0) {
        return 0;
    }
    int64_t wait = limits.min_us;
    for (uint32_t i = 1; i < failures && wait < limits.max_us; ++i) {
        wait *= 2;
    }
    return wait < limits.max_us ? wait : limits.max_us;
}

bool is_handshake_failure(std::optional<SessionEnd> end) {
    return end != SessionEnd::GOODBYE && end != SessionEnd::CLOSED_BY_HOST;
}

ServerError error_before_activation(const std::string& url, const Session& session) {
    const std::optional<ClientInit>& init = session.client_init();
    ServerError error{ErrorKind::HANDSHAKE_FAILED, url, init ? init->client_id : std::string{},
                      std::nullopt, std::nullopt};
    const std::optional<ClientGoodbye>& goodbye = session.goodbye();
    if (goodbye && !is_handshake_failure(session.end())) {
        error.kind = ErrorKind::GOODBYE_BEFORE_ACTIVATION;
        error.goodbye_reason = goodbye->reason;
    }
    return error;
}

DisconnectReason disconnect_reason(std::optional<SessionEnd> end,
                                   std::optional<CloseReason> transport) {
    if (!end) {
        // A message over the size limit, or broken WebSocket framing, is the client's error
        return transport == CloseReason::PROTOCOL_VIOLATION ? DisconnectReason::CLOSED_BY_SERVER
                                                            : DisconnectReason::CONNECTION_LOST;
    }
    if (*end == SessionEnd::GOODBYE) {
        return DisconnectReason::GOODBYE;
    }
    return *end == SessionEnd::CLOSED_BY_HOST ? DisconnectReason::DISCONNECTED_BY_HOST
                                              : DisconnectReason::CLOSED_BY_SERVER;
}

std::string_view close_description(CloseReason reason) {
    switch (reason) {
        case CloseReason::PEER_CLOSED:
            return "the peer closed the connection";
        case CloseReason::TIMED_OUT:
            return "the connection timed out";
        case CloseReason::WRITE_STALLED:
            return "writes stalled";
        case CloseReason::PROTOCOL_VIOLATION:
            return "a WebSocket protocol violation or an oversized message";
        case CloseReason::FAILED:
            break;
    }
    return "the connection failed";
}

std::string end_description(const Session& session, std::optional<CloseReason> transport) {
    const std::optional<SessionEnd> end = session.end();
    if (!end) {
        return std::string(transport ? close_description(*transport) : "closed");
    }
    switch (*end) {
        case SessionEnd::INIT_REJECTED: {
            const std::optional<InitError> error = session.init_error();
            return "client/init rejected" +
                   (error ? " (" + std::string(to_wire(*error)) + ")" : std::string());
        }
        case SessionEnd::TIMED_OUT:
            return "a handshake step timed out";
        case SessionEnd::HANDSHAKE_FAILED:
            return "the Noise handshake failed";
        case SessionEnd::DECRYPT_FAILED:
            return "a message did not decrypt";
        case SessionEnd::CLEARTEXT_IN_TRANSPORT:
            return "a cleartext message after the handshake";
        case SessionEnd::MALFORMED_FRAGMENTS:
            return "malformed fragments";
        case SessionEnd::MESSAGE_TOO_LARGE:
            return "a fragmented message too large";
        case SessionEnd::PROTOCOL_ERROR:
            return "a protocol error";
        case SessionEnd::ENCRYPT_FAILED:
            return "a message did not encrypt";
        case SessionEnd::QUEUE_OVERFLOW:
            return "a send queue overflowed";
        case SessionEnd::ENCODE_FAILED:
            return "a message could not be encoded";
        case SessionEnd::CLOSED_BY_HOST:
            return "the host disconnected it";
        case SessionEnd::GOODBYE:
            break;
    }
    const std::optional<ClientGoodbye>& goodbye = session.goodbye();
    return "client/goodbye (" +
           (goodbye && goodbye->reason ? std::string(to_wire(*goodbye->reason))
                                       : std::string("unrecognized reason")) +
           ")";
}

}  // namespace sendspin::server
