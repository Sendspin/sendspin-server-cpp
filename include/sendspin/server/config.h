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

/// @file config.h
/// @brief ServerConfig, the settings SendspinServer::start() takes

#pragma once

#include "sendspin/server/discovery.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace sendspin::server {

namespace config_defaults {
constexpr int64_t HANDSHAKE_STEP_TIMEOUT_US = 30000000;
constexpr int64_t WRITE_STALL_US = 15000000;
constexpr size_t MAX_MESSAGE_BYTES = 16384;
constexpr int64_t HANDSHAKE_BACKOFF_MIN_US = 1000000;
constexpr int64_t HANDSHAKE_BACKOFF_MAX_US = 60000000;
constexpr int64_t GROUP_REJOIN_GRACE_US = 180000000;
/// The window recommended for a pairing window gesture (spec pairing.md "Pairing Window")
constexpr int64_t PAIRING_START_TIMEOUT_US = 300000000;
/// spec connection.md "Client Initiated Connections": the recommended port and path
constexpr uint16_t LISTEN_PORT = 8927;
constexpr const char* PATH = "/sendspin";
}  // namespace config_defaults

/// @brief Limits on ServerConfig values; SendspinServer::start() refuses a configuration past
/// them with StartResult::INVALID_CONFIG
namespace config_limits {
/// The server's name, and a group's (SendspinServer::create_group(), rename_group())
constexpr size_t MAX_NAME_BYTES = 1024;
/// BCP 47 tags in practice stay within 35 characters (RFC 5646 section 4.4.1)
constexpr size_t MAX_LANGUAGE_TAG_BYTES = 64;
constexpr size_t MAX_LANGUAGES = 32;
/// The longest ServerTiming value: 24 hours
constexpr int64_t MAX_TIMING_US = int64_t{24} * 60 * 60 * 1000000;
/// A TXT record string holds at most 255 bytes (RFC 6763 section 6.1), `path=` included
constexpr size_t MAX_PATH_BYTES = 250;
}  // namespace config_limits

/// @brief Timing constants, each positive and at most config_limits::MAX_TIMING_US
struct ServerTiming {
    /// Limit for receiving each handshake message: client/init, Noise message 2, client/hello
    /// (spec connection.md "Failure Handling")
    int64_t handshake_step_timeout_us{config_defaults::HANDSHAKE_STEP_TIMEOUT_US};
    /// A connection whose wait for socket room or whose write stays outstanding this long is
    /// closed
    int64_t write_stall_us{config_defaults::WRITE_STALL_US};
    /// The wait before dialing a URL again after a failed dial or handshake: this long after the
    /// first failure, doubling with each further one up to handshake_backoff_max_us (spec
    /// messaging.md "client/goodbye")
    int64_t handshake_backoff_min_us{config_defaults::HANDSHAKE_BACKOFF_MIN_US};
    /// At least handshake_backoff_min_us
    int64_t handshake_backoff_max_us{config_defaults::HANDSHAKE_BACKOFF_MAX_US};
    /// How long a client activated for playback that went away without a client/goodbye, or
    /// with reason `restart`, keeps its place in its group: reconnecting within it, it rejoins
    /// the group, and the group is kept for it
    int64_t group_rejoin_grace_us{config_defaults::GROUP_REJOIN_GRACE_US};
    /// How long a pairing's activation waits for the client to start the attempt with
    /// client/pair-init, through a client/pair-pending (spec pairing.md "Entering and leaving
    /// pairing"); then the pairing ends with PairingResult::TIMED_OUT
    int64_t pairing_start_timeout_us{config_defaults::PAIRING_START_TIMEOUT_US};
};

/// @brief The resampler's filter, from the narrowest passband and shortest filter to the widest
/// and longest; each step costs more CPU per sample and more lookahead
enum class ResamplerQuality : uint8_t { VERY_LOW, LOW, MEDIUM, HIGH, VERY_HIGH };

enum class TraceDirection : uint8_t { INBOUND, OUTBOUND };

/// @brief One WebSocket message as the message trace sees it
struct TracedMessage {
    /// Unique per connection while the server runs
    uint64_t connection_id{0};
    /// Empty until the client's client/init was accepted
    std::string client_id;
    TraceDirection direction{TraceDirection::INBOUND};
    /// False for a cleartext handshake message (JSON text); true for a binary message, whose
    /// plaintext starts with its message ID. Outbound, each fragment of a fragmented message is
    /// traced on its own; inbound, the message is traced once whole.
    bool binary{false};
    /// Inbound: the receive stamp. Outbound: the stamp taken immediately before encryption, which
    /// a server/time carries as `server_transmitted`. On SendspinServer::now_us()'s clock.
    int64_t stamp_us{0};
    /// Inbound: after decryption. Outbound: before encryption. The pairing secrets a client sends
    /// (`long_term_psk`, `wrapped_psk`, `wrapped_nonce_B`) read "<redacted>".
    std::vector<uint8_t> plaintext;
    /// Outbound audio chunk: the bytes of encoded audio it carries, counted from the frames packed
    /// into it rather than from the message's size; 0 for any other message
    size_t audio_bytes{0};
    /// Set only with ServerConfig::trace_writes: the entry reports that the write of the
    /// connection's last traced outbound message completed (handed whole to the kernel). Its
    /// direction is OUTBOUND, stamp_us is now_us() as the write completed and plaintext is empty.
    bool written{false};
};

/// @brief Called for every message on every connection, on the dispatch thread (SendspinServer)
using MessageTraceFn = std::function<void(const TracedMessage& message)>;

/// @brief How much the server logs; each level includes the ones before it
enum class LogLevel : uint8_t {
    OFF,
    /// A failure the host should act on: a server key not loaded or saved, a stored client
    /// record that is not valid, a client record not saved
    ERROR,
    /// A failure the server recovers from: a dial or a dialed connection's handshake, mDNS
    /// unavailable, connections refused while too many are in their handshake
    WARN,
    /// Each connection and its end, a failed handshake of a connection the server accepted, each
    /// dial and the wait before one, mDNS available again, each pairing and its end
    INFO,
};

/// @brief Receives one log line, with no trailing newline. Called on the dispatch thread
/// (SendspinServer), in order with the listener's calls. A line never holds a key, a PSK or a
/// client_id.
using LogFn = std::function<void(LogLevel level, std::string_view line)>;

struct ServerConfig {
    /// The server/hello `name`; at most config_limits::MAX_NAME_BYTES
    std::string name;
    /// BCP 47 tags in descending preference; omitted from server/hello when empty. At most
    /// config_limits::MAX_LANGUAGES tags of at most config_limits::MAX_LANGUAGE_TAG_BYTES each.
    std::vector<std::string> languages;
    /// Plaintext size of each fragment of an outbound message that needs fragmenting (spec
    /// messaging.md "Fragmentation") and of each artwork part message; taken as 1,024 when
    /// smaller and 65,519 when larger
    size_t max_message_bytes{config_defaults::MAX_MESSAGE_BYTES};
    ServerTiming timing;
    /// The filter of the resampler that converts a group input to a player's sample rate
    ResamplerQuality resampler_quality{ResamplerQuality::MEDIUM};
    /// Streams Opus to a player that lists it (spec player/v1.md: servers MAY support `opus`), at
    /// 8, 12, 16, 24 or 48 kHz, mono or stereo. Off by default: Opus is covered by patents (spec
    /// player/v1.md "client/hello player@v1 support object"), which the host decides about.
    bool enable_opus{false};
    /// Approves every client with no client record for playback, as
    /// SendspinServer::approve() with ApprovalScope::PLAYBACK would, without saving a record:
    /// the operator's approval in advance (spec pairing.md "Unpaired Access"). A client's record,
    /// which approve() and revoke_approval() write, decides over it.
    bool approve_all_clients{false};
    /// The host's operator interface can scan a QR code, so a dynamic pairing code may use the
    /// `qr_code` format (spec messaging.md "server/activate")
    bool operator_can_scan_qr{false};
    /// Opt-in plaintext trace of every message; empty for none, which costs nothing
    MessageTraceFn trace;
    /// With trace: also an entry each time an outbound message's write completes
    /// (TracedMessage::written), in order with the messages, so a host can tell what was on the
    /// wire before a message it received. Off by default.
    bool trace_writes{false};
    /// Where log lines go; empty for standard error
    LogFn log;
    /// The most detailed level logged: WARN by default, so a host's standard error stays quiet
    /// while the server works as intended
    LogLevel log_level{LogLevel::WARN};
    /// The TCP port clients connect to, on every interface; 0 for a port the system picks
    /// (SendspinServer::listen_port())
    uint16_t listen_port{config_defaults::LISTEN_PORT};
    /// The WebSocket path a client that connects must request, and the advertised TXT `path`:
    /// '/' and then printable ASCII other than '?' and '#', at most config_limits::MAX_PATH_BYTES
    std::string path{config_defaults::PATH};
    /// Advertise `_sendspin-server._tcp`, so that clients find the server and connect to it (spec
    /// connection.md "Client Initiated Connections")
    bool advertise{true};
    /// Browse `_sendspin._tcp` and dial the clients found (spec connection.md "Server Initiated
    /// Connections")
    bool browse{true};
    /// What advertises and browses; nullptr for platform_discovery(), whose unavailability does
    /// not stop the server (ErrorKind::DISCOVERY_UNAVAILABLE). Unused when neither advertise nor
    /// browse is set.
    std::shared_ptr<DiscoveryProvider> discovery;
};

}  // namespace sendspin::server
