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

/// @file messages.h
/// @brief JSON messages of the connection handshake, the hello exchange, activation, time sync,
/// state, commands and the stream lifecycle: builders for what the server sends, and parsers
/// that validate what a client sends into typed structs carrying the receive stamp. Pure
/// functions.

#pragma once

#include "artwork.h"
#include "colors.h"
#include "crypto/cpace.h"
#include "crypto/keys.h"
#include "crypto/pairing_code.h"
#include "crypto/psk_wrap.h"
#include "noise_types.h"
#include "sendspin/server/types.h"
#include "visualizer.h"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace sendspin::server {

/// @brief The core message format version of client/init and server/init
constexpr int64_t PROTOCOL_VERSION = 1;

/// @brief The longest string value the JSON builders can encode (ArduinoJson's string length
/// limit). A builder that takes a variable string returns nullopt for a longer one; the others
/// throw std::bad_alloc if the JSON cannot be built.
constexpr size_t MAX_JSON_STRING_BYTES = 65535;

// ==============================================================================
// Cleartext handshake
// ==============================================================================

/// @brief A validated client/init
struct ClientInit {
    std::string client_id;
    PublicKey client_key{};
    CipherSuite suite{CipherSuite::CHACHAPOLY_SHA256};
    int64_t rx_us{0};
};

/// @brief The server/error reasons, one per init failure (spec connection.md "Failure Handling")
enum class InitError : uint8_t {
    UNSUPPORTED_VERSION,
    UNSUPPORTED_SUITE,
    MALFORMED,
};

std::string_view to_wire(InitError error);

/// @brief Validates client/init in the spec's order: the envelope, then an integer `version`
/// other than 1 (unsupported_version whatever else the payload holds), then `suite`, then
/// `client_id`. Any other shape failure, invalid JSON included, is malformed.
std::variant<ClientInit, InitError> validate_client_init(std::string_view text, int64_t rx_us);

std::string build_server_init(std::string_view server_id);

std::string build_server_error(InitError error);

/// @brief A noise/handshake message carrying one Noise handshake message: the server's message
/// 1, whose fixed size encodes far within MAX_JSON_STRING_BYTES
std::string build_noise_handshake(std::span<const uint8_t> noise_message);

/// @brief The Noise handshake message a noise/handshake carries
/// @return nullopt for any other type, or a missing, empty or non-base64url `data`
std::optional<std::vector<uint8_t>> parse_noise_handshake(std::string_view text);

/// @brief The Noise message 1 payload: {"psk_id": ..., "psk_category": ...}
std::string build_message_1_payload(std::string_view psk_id, PskCategory category);

/// @brief Whether a Noise message 2 payload is the literal two bytes `{}`
bool is_message_2_payload(std::span<const uint8_t> payload);

// ==============================================================================
// server/hello
// ==============================================================================

enum class Codec : uint8_t { OPUS, FLAC, PCM };

std::string_view to_wire(Codec codec);

struct ServerHello {
    std::string name;
    /// BCP 47 tags in descending preference; omitted from the message when empty
    std::vector<std::string> languages;
    /// The codecs the source role accepts in client-stream/start, which source@v1_support lists
    /// (roles/source/v1.md "server/hello source@v1 support object"); empty leaves it out
    std::vector<Codec> source_codecs;
};

/// @return nullopt when a value passes MAX_JSON_STRING_BYTES
std::optional<std::string> build_server_hello(const ServerHello& hello);

// ==============================================================================
// client/hello
// ==============================================================================

/// @brief One `supported_formats` entry (roles/player/v1.md "client/hello player@v1 support
/// object")
struct AudioFormat {
    Codec codec{Codec::PCM};
    uint32_t channels{0};
    uint32_t sample_rate{0};
    uint32_t bit_depth{0};

    bool operator==(const AudioFormat&) const = default;
};

struct PlayerSupport {
    /// In the client's priority order; entries with an unrecognized codec are left out
    std::vector<AudioFormat> supported_formats;
    uint32_t buffer_capacity{0};
};

struct SourceSupport {
    bool line_sense{false};
};

/// @brief A validated client/state source object (roles/source/v1.md "client/state source
/// object")
struct SourceState {
    /// Absent when not reported, or a value this server does not recognize
    std::optional<SourceSignal> signal;

    bool operator==(const SourceState&) const = default;
};

struct VisualizerSupport {
    uint32_t buffer_capacity{0};
};

struct DeviceInfo {
    std::optional<std::string> product_name;
    std::optional<std::string> manufacturer;
    std::optional<std::string> software_version;
    std::optional<std::string> mac_address;
};

/// @brief Bits of PairMethodDescriptor::locations
namespace pair_location {
constexpr uint8_t DEVICE = 0x01;
constexpr uint8_t LEAFLET = 0x02;
constexpr uint8_t OPERATOR = 0x04;
}  // namespace pair_location

/// @brief Bits of DynamicPairingCode::out_channels and formats
namespace pair_code {
constexpr uint8_t DISPLAY = 0x01;
constexpr uint8_t SPEAKER = 0x02;
constexpr uint8_t DIGITS = 0x01;
constexpr uint8_t QR_CODE = 0x02;
}  // namespace pair_code

struct PairMethodDescriptor {
    uint8_t locations{0};
};

struct DynamicPairingCode {
    uint8_t out_channels{0};
    uint8_t formats{0};
};

/// @brief The recognized `supported_pair_methods` entries (pairing.md "client/hello pair-method
/// descriptor"); unrecognized values inside them are dropped
struct PairMethods {
    std::optional<PairMethodDescriptor> pairing_psk;
    std::optional<PairMethodDescriptor> static_pairing_code;
    std::optional<DynamicPairingCode> dynamic_pairing_code;
};

/// @brief A validated client/hello
struct ClientHello {
    std::string name;
    DeviceInfo device_info;
    /// As listed, in the client's priority order
    std::vector<std::string> supported_roles;
    /// Recorded only when the role is listed; absent when missing or invalid, so the role cannot
    /// be activated (messaging.md "client/hello")
    std::optional<PlayerSupport> player_v1_support;
    std::optional<SourceSupport> source_v1_support;
    std::optional<VisualizerSupport> visualizer_v1_support;
    PairMethods pair_methods;
    bool unpaired_access_enabled{false};
    /// Role versions (other than application-specific ones) and pair-method identifiers this
    /// server does not recognize: a sign the client speaks a newer revision (README "Detecting
    /// Outdated Servers", pairing.md "client/hello pair-method descriptor")
    std::vector<std::string> unrecognized_identifiers;
    int64_t rx_us{0};
};

// ==============================================================================
// server/activate
// ==============================================================================

/// @brief A pairing activation's `pairing` object (messaging.md "server/activate")
struct PairingParams {
    PairMethod method{PairMethod::DYNAMIC_PAIRING_CODE};
    /// With DYNAMIC_PAIRING_CODE: DIGITS or QR_CODE; absent otherwise
    std::optional<CodeFormat> format;

    bool operator==(const PairingParams&) const = default;
};

/// @brief A server/activate (messaging.md "server/activate")
struct ServerActivate {
    /// `activities` holds `'playback'` when set
    bool playback{false};
    /// Omitted from the message when nullopt, which keeps the client's persisted roles; required
    /// on the first activation
    std::optional<std::vector<std::string>> active_roles;
    /// Set for a pairing activation: `activities` holds `'pairing'` and the message carries it
    std::optional<PairingParams> pairing;
};

/// @return nullopt when a role passes MAX_JSON_STRING_BYTES
std::optional<std::string> build_server_activate(const ServerActivate& activate);

// ==============================================================================
// Inbound application messages
// ==============================================================================

struct ClientGoodbye {
    /// nullopt for a reason this server does not recognize
    std::optional<GoodbyeReason> reason;
    int64_t rx_us{0};
};

/// @brief The reason as client/goodbye spells it
std::string_view to_wire(GoodbyeReason reason);

/// @brief A validated client/time (messaging.md "client/time")
struct ClientTime {
    int64_t client_transmitted{0};
    int64_t rx_us{0};
};

/// @brief Bits of PlayerState::supported_commands
namespace player_command {
constexpr uint8_t VOLUME = 0x01;
constexpr uint8_t MUTE = 0x02;
constexpr uint8_t SET_OUTPUT_DELAY = 0x04;
}  // namespace player_command

/// @brief The largest `output_delay_ms` (roles/player/v1.md "client/state player object")
constexpr uint32_t MAX_OUTPUT_DELAY_MS = 5000;

/// @brief A validated client/state player object (roles/player/v1.md "client/state player
/// object")
struct PlayerState {
    std::optional<uint32_t> volume;
    std::optional<bool> muted;
    uint32_t output_delay_ms{0};
    uint32_t required_lead_time_ms{0};
    uint32_t min_buffer_ms{0};
    /// player_command bits; unrecognized commands are left out
    uint8_t supported_commands{0};
    /// The preferred format; absent when not set or its codec is not recognized
    std::optional<AudioFormat> format;

    bool operator==(const PlayerState&) const = default;
};

/// @brief A validated client/state (messaging.md "client/state")
struct ClientState {
    bool available{false};
    /// The player object, when present and valid
    std::optional<PlayerState> player;
    /// A player object was present and invalid: a protocol error while the role is active, and
    /// ignored otherwise (messaging.md "server/activate": inactive-role objects are ignored)
    bool player_malformed{false};
    /// The artwork object's channels, from 1 to MAX_ARTWORK_CHANNELS, when present and valid
    /// (roles/artwork/v1.md "client/state artwork object"); an invalid one is ignored
    std::optional<std::vector<ArtworkChannel>> artwork;
    /// The artwork object lists more than MAX_ARTWORK_CHANNELS channels: a protocol error while
    /// the role is active, and ignored otherwise
    bool artwork_too_many_channels{false};
    /// The visualizer object, when present and valid (roles/visualizer/v1.md "client/state
    /// visualizer object")
    std::optional<VisualizerRequest> visualizer;
    /// A visualizer object was present and invalid, `types` listing 'spectrum' without a
    /// `spectrum` object included: a protocol error while the role is active, and ignored
    /// otherwise
    bool visualizer_malformed{false};
    /// The source object, when present and valid
    std::optional<SourceState> source;
    /// A source object was present and invalid: a protocol error while the role is active, and
    /// ignored otherwise
    bool source_malformed{false};
    int64_t rx_us{0};
};

// ==============================================================================
// Source role (roles/source/v1.md)
// ==============================================================================

/// @brief The binary message ID of source audio (roles/source/v1.md "Source Audio Chunks
/// (Binary)")
constexpr uint8_t MESSAGE_ID_SOURCE_AUDIO = 12;
/// @brief A source audio message's ID and timestamp
constexpr size_t SOURCE_CHUNK_HEADER_BYTES = 9;

/// @brief A validated client-stream/start (roles/source/v1.md "client-stream/start")
struct ClientStreamStart {
    /// `bit_depth` is 0 for Opus, which ignores it
    AudioFormat format;
    /// The decoded `codec_header`: for FLAC, `fLaC` and a STREAMINFO block matching the format;
    /// empty for PCM and Opus, which have none
    std::vector<uint8_t> codec_header;
    int64_t rx_us{0};
};

/// @brief A client-stream/end
struct ClientStreamEnd {
    int64_t rx_us{0};
};

/// @brief A source audio message
struct SourceChunk {
    /// The server-clock time its first sample was captured
    int64_t capture_us{0};
    std::span<const uint8_t> audio;
};

/// @brief The furthest from 0 a chunk's capture time may be: about 146,000 years of server clock,
/// so the arithmetic on it and the chunk's duration never overflows
constexpr int64_t MAX_SOURCE_TIMESTAMP_US = int64_t{1} << 62;

/// @brief The chunk in a source audio message, ID first; its audio may be empty (roles/source/v1.md
/// "Codec framing": PCM is any whole number of frames)
/// @return nullopt for one shorter than its header, or with a capture time past
/// MAX_SOURCE_TIMESTAMP_US
std::optional<SourceChunk> parse_source_chunk(std::span<const uint8_t> message);

// ==============================================================================
// client/command and client/leave
// ==============================================================================

/// @brief The controller commands (spec controller/v1.md "client/command controller object")
enum class ControllerCommand : uint8_t {
    PLAY,
    PAUSE,
    STOP,
    NEXT,
    PREVIOUS,
    VOLUME,
    MUTE,
    REPEAT_OFF,
    REPEAT_ONE,
    REPEAT_ALL,
    SHUFFLE,
    UNSHUFFLE,
    SWITCH,
    SEEK,
    SEEK_RELATIVE,
};

constexpr size_t CONTROLLER_COMMAND_COUNT = 15;

std::string_view to_wire(ControllerCommand command);

/// @brief A set of controller commands, one bit per ControllerCommand value
using ControllerCommandSet = uint16_t;

constexpr ControllerCommandSet command_bit(ControllerCommand command) {
    return static_cast<ControllerCommandSet>(1U << static_cast<unsigned>(command));
}

/// @brief A validated client/command controller object
struct ControllerRequest {
    /// nullopt for a command this server does not recognize, which no supported_commands lists
    std::optional<ControllerCommand> command;
    /// With VOLUME: 0 to 100
    std::optional<uint32_t> volume;
    /// With MUTE
    std::optional<bool> mute;
    /// With SEEK, unchecked against the seekable range
    std::optional<int64_t> position_ms;
    /// With SEEK_RELATIVE
    std::optional<int64_t> offset_ms;
    /// A recognized command whose parameter is missing or invalid: a protocol error when the
    /// command is offered, and ignored otherwise
    bool malformed{false};
};

/// @brief A validated client/command (messaging.md "client/command")
struct ClientCommand {
    /// The controller object, when present and an object with a `command` string
    std::optional<ControllerRequest> controller;
    /// A controller object was present and not that: a protocol error while the role is active,
    /// and ignored otherwise (messaging.md "server/activate")
    bool controller_malformed{false};
    int64_t rx_us{0};
};

/// @brief A client/leave (messaging.md "client/leave")
struct ClientLeave {
    int64_t rx_us{0};
};

/// @brief A valid envelope whose type has no parser here
struct OtherMessage {
    std::string type;
    int64_t rx_us{0};
};

/// @brief Not a JSON object with a `type` string and a `payload` object, or a client/hello,
/// client/goodbye, client/time or client-stream/start whose payload fails validation
struct MalformedMessage {
    /// The `type`, when there is one
    std::string type;
};

// ==============================================================================
// Pairing (spec pairing.md "Messages")
// ==============================================================================

/// @brief The pair/abort reasons (pairing.md "Client <-> Server: pair/abort")
enum class PairAbortReason : uint8_t {
    ATTEMPT_TIMEOUT,
    CONCURRENT_ATTEMPT,
    METHOD_NOT_SUPPORTED,
    PAIRING_CODE_MISMATCH,
    USER_CANCELLED,
};

/// @brief The pairing messages a client sends
enum class PairingMessageKind : uint8_t {
    PENDING,   ///< client/pair-pending
    INIT,      ///< client/pair-init
    AUTH,      ///< client/pair-auth
    RETRY,     ///< client/pair-retry
    CONFIRM,   ///< client/pair-confirm
    FINALIZE,  ///< client/pair-finalize
    ABORT,     ///< pair/abort
};

/// @brief The most characters of a client/pair-pending `message` kept (pairing.md
/// "client/pair-pending")
constexpr size_t MAX_PAIR_PENDING_CHARS = 200;

/// @brief A validated pairing message from the client. A field not listed for its kind is ignored.
struct ClientPairing {
    PairingMessageKind kind{PairingMessageKind::PENDING};
    /// PENDING and INIT
    uint32_t pairing_index{0};
    /// PENDING: the operator text, cut to MAX_PAIR_PENDING_CHARS UTF-8 characters
    std::optional<std::string> message;
    /// INIT: commit_B, present in the Dynamic Pairing Code Flow only
    std::optional<std::array<uint8_t, PAIRING_COMMIT_SIZE>> commit_b;
    /// AUTH: `Yb`
    std::array<uint8_t, CPACE_SHARE_SIZE> pake_msg_2{};
    /// CONFIRM: `Tb`, and wrapped_nonce_B in the Dynamic Pairing Code Flow only
    std::array<uint8_t, CPACE_TAG_SIZE> client_kc{};
    std::optional<WrappedValue> wrapped_nonce_b;
    /// FINALIZE: exactly one of the two. The PSK wipes itself.
    std::optional<HandshakePsk> long_term_psk;
    std::optional<WrappedValue> wrapped_psk;
    /// ABORT: nullopt for a reason this server does not recognize
    std::optional<PairAbortReason> abort_reason;
    int64_t rx_us{0};
};

using ClientMessage = std::variant<MalformedMessage, ClientHello, ClientGoodbye, ClientTime,
                                   ClientState, ClientCommand, ClientLeave, ClientPairing,
                                   ClientStreamStart, ClientStreamEnd, OtherMessage>;

/// @brief Parses and validates one JSON message body from the client. A client/state whose
/// client-level fields fail validation is a MalformedMessage; a client/command's role objects
/// are validated on their own. A pairing message with a missing or malformed field, a value of
/// the wrong length included, is a MalformedMessage (pairing.md "Protocol Errors"). A
/// client-stream/start is malformed with a codec other than `opus`, `flac` or `pcm`, a channel
/// count or rate that is not a positive integer, a `bit_depth` that is not one but for Opus, a
/// `codec_header` that is not standard Base64 with its padding, one for PCM or Opus, or a FLAC
/// one that is missing or whose STREAMINFO disagrees with the format (roles/source/v1.md
/// "client-stream/start").
ClientMessage parse_client_message(std::string_view json, int64_t rx_us);

/// @brief What an active session does with a valid message it has no handler for
enum class UnhandledRule : uint8_t {
    IGNORE,  ///< An unrecognized type, or a client message this server does not act on
    CLOSE,   ///< A recognized type the client may not send here; a protocol error
};

/// @brief The rule for a valid envelope of the given type after the initial server/activate,
/// other than client/time, client/goodbye and client/state. messaging.md "Communication":
/// unrecognized types are ignored, while the direction and sequencing rules of recognized ones
/// still apply.
UnhandledRule rule_for_unhandled(std::string_view type);

// ==============================================================================
// server/time
// ==============================================================================

/// @brief The reply to request (messaging.md "server/time"): `server_received` is the request's
/// receive stamp, `server_transmitted` the stamp taken immediately before encryption
/// (messaging.md "Transmit timestamps")
std::string build_server_time(const ClientTime& request, int64_t transmitted_us);

// ==============================================================================
// group/update and stream lifecycle
// ==============================================================================

/// @brief A group/update; every field is required (messaging.md "group/update")
struct GroupUpdate {
    bool playing{false};
    std::string group_id;
    std::string group_name;
};

/// @return nullopt when a value passes MAX_JSON_STRING_BYTES
std::optional<std::string> build_group_update(const GroupUpdate& update);

// ==============================================================================
// server/state and server/command
// ==============================================================================

/// @brief A server/state controller object (spec controller/v1.md "server/state controller
/// object")
struct ControllerState {
    ControllerCommandSet supported_commands{0};
    uint32_t volume{MAX_VOLUME};
    bool muted{false};
    RepeatMode repeat{RepeatMode::OFF};
    bool shuffle{false};
    /// Set exactly when SEEK is in supported_commands
    std::optional<uint32_t> seek_max_ms;

    bool operator==(const ControllerState&) const = default;
};

/// @brief A server/state metadata object (spec metadata/v1.md "server/state metadata object")
struct MetadataState {
    /// When it takes effect, on the server's clock
    int64_t timestamp_us{0};
    Metadata metadata;

    bool operator==(const MetadataState&) const = default;
};

/// @brief A server/state; each object only for an active role (messaging.md "server/state")
struct ServerState {
    std::optional<ControllerState> controller;
    std::optional<MetadataState> metadata;
    std::optional<ColorState> color;
};

/// @return nullopt when a value passes MAX_JSON_STRING_BYTES
std::optional<std::string> build_server_state(const ServerState& state);

/// @brief A server/command player volume command (roles/player/v1.md "server/command player
/// object"); volume at most 100
std::string build_player_volume_command(uint32_t volume);

/// @brief A server/command player mute command
std::string build_player_mute_command(bool mute);

/// @brief What a server/command source object asks (roles/source/v1.md "server/command source
/// object")
enum class SourceCommand : uint8_t { START, STOP };

/// @brief A server/command source command
std::string build_source_command(SourceCommand command);

/// @brief The player and visualizer objects of a stream/start, each for a stream it starts or
/// reconfigures
struct StreamStart {
    /// The player's format (roles/player/v1.md "stream/start player object")
    std::optional<AudioFormat> player;
    /// Standard Base64, for a player codec that needs one (FLAC)
    std::optional<std::string> codec_header;
    /// roles/visualizer/v1.md "stream/start visualizer object"
    std::optional<VisualizerConfig> visualizer;
};

/// @brief A stream/start for the player and visualizer roles, at least one
/// @param transmitted_us the stamp taken immediately before encryption
std::string build_stream_start(int64_t transmitted_us, const StreamStart& start);

/// @brief A stream/start for the artwork role (roles/artwork/v1.md "stream/start artwork object")
/// @param channels the channels as listed, a `none` channel with no format or size
std::string build_stream_start_artwork(int64_t transmitted_us,
                                       std::span<const ArtworkChannel> channels);

/// @brief A stream/clear for the listed roles, at least one, each with an active stream
std::string build_stream_clear(int64_t transmitted_us, std::span<const std::string_view> roles);

/// @brief A stream/end for the listed roles, at least one, each with an active stream
std::string build_stream_end(std::span<const std::string_view> roles);

// ==============================================================================
// Pairing messages the server sends
// ==============================================================================

/// @brief A server/pair-init: nonce_A in the first round only (pairing.md "server/pair-init")
std::string build_server_pair_init(
    const std::optional<std::array<uint8_t, PAIRING_NONCE_SIZE>>& nonce_a);

/// @brief A server/pair-auth carrying `Ya`
std::string build_server_pair_auth(const std::array<uint8_t, CPACE_SHARE_SIZE>& pake_msg_1);

/// @brief A server/pair-confirm carrying `Ta`
std::string build_server_pair_confirm(const std::array<uint8_t, CPACE_TAG_SIZE>& server_kc);

std::string build_server_pair_finalize();

std::string build_pair_abort(PairAbortReason reason);

/// @brief A server/unpair (messaging.md "server/unpair")
std::string build_server_unpair();

/// @brief The plaintext of an inbound message for the message trace, with every pairing secret the
/// client sent replaced: a `long_term_psk`, `wrapped_psk` or `wrapped_nonce_B` value in the
/// payload, or the whole JSON body when it names one and does not parse. nullopt when there is
/// nothing to replace, so the plaintext is traced as it is.
/// @param plaintext a binary message, ID first
std::optional<std::vector<uint8_t>> redact_pairing_secrets(std::span<const uint8_t> plaintext);

}  // namespace sendspin::server
