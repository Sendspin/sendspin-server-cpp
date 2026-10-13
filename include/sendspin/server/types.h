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

/// @file types.h
/// @brief Values the server and the host exchange: start results, clients, approvals,
/// disconnects, errors, groups, controllers, metadata and group inputs

#pragma once

#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace sendspin::server {

/// @brief The outcome of SendspinServer::start()
enum class StartResult : uint8_t {
    OK,
    /// The server is running, or another start() is in progress
    ALREADY_RUNNING,
    /// No provider, or the server key could not be loaded, or created and saved
    PERSISTENCE_FAILED,
    /// A ServerConfig value is out of range: past a config_limits limit, a timing that is not
    /// positive, a backoff maximum below its minimum, or a resampler quality that is not one of
    /// ResamplerQuality's, or a path that is not one
    INVALID_CONFIG,
    /// ServerConfig::listen_port could not be bound
    BIND_FAILED,
    /// ServerConfig::advertise or browse is set and the provider in ServerConfig::discovery could
    /// not start. The built-in provider never fails the start: see
    /// ErrorKind::DISCOVERY_UNAVAILABLE.
    DISCOVERY_FAILED,
};

/// @brief What an operator's approval of a client without a pairing record covers (spec
/// pairing.md "Unpaired Access")
enum class ApprovalScope : uint8_t {
    /// The playback activity and every role the server implements but the source role
    PLAYBACK,
    /// PLAYBACK and the source role, which approval implied by other use does not cover (spec
    /// roles/source/v1.md "Unpaired access")
    PLAYBACK_AND_SOURCE,
};

/// @brief The pairing methods (spec pairing.md "Methods")
enum class PairMethod : uint8_t {
    /// The client's pairing PSK, from its pairing token (SendspinServer::submit_pairing_token())
    PAIRING_PSK,
    /// The client's fixed 8-digit pairing code
    STATIC_PAIRING_CODE,
    /// A code the client derives for the attempt and shows or speaks, 6 digits or a QR code
    DYNAMIC_PAIRING_CODE,
};

/// @brief The form of the secret the operator enters to pair
enum class CodeFormat : uint8_t {
    /// A version-0 pairing token, "SP:0..." (PairMethod::PAIRING_PSK):
    /// SendspinServer::submit_pairing_token()
    TOKEN,
    /// Decimal digits, 8 for a static pairing code and 6 for a dynamic one; spaces and hyphens
    /// are ignored: SendspinServer::submit_pairing_code()
    DIGITS,
    /// A dynamic pairing code scanned from the client's QR code, a version-1 pairing token
    /// "SP:1...": SendspinServer::submit_pairing_code()
    QR_CODE,
};

/// @brief The pairing methods a client offers in its latest client/hello (spec pairing.md
/// "client/hello pair-method descriptor")
struct PairingOffers {
    bool pairing_psk{false};
    bool static_pairing_code{false};
    /// The dynamic pairing code in the `digits` format
    bool dynamic_digits{false};
    /// The dynamic pairing code in the `qr_code` format
    bool dynamic_qr_code{false};

    bool operator==(const PairingOffers&) const = default;
};

/// @brief What SendspinServer::begin_pairing() starts
struct PairingOptions {
    PairMethod method{PairMethod::DYNAMIC_PAIRING_CODE};
    /// PairMethod::DYNAMIC_PAIRING_CODE only: CodeFormat::DIGITS or QR_CODE, the format the client
    /// is asked to emit. nullopt takes QR_CODE when the client offers it and
    /// ServerConfig::operator_can_scan_qr is set, else DIGITS.
    std::optional<CodeFormat> format;
};

/// @brief How a pairing ended (ServerListener::on_pairing_result())
enum class PairingResult : uint8_t {
    /// The server saved the client's long-term PSK and acknowledged it; later connections are
    /// paired sessions
    PAIRED,
    /// SendspinServer::cancel_pairing(), or a later begin_pairing() for the client
    CANCELLED,
    /// The code entered did not match the client's (spec pairing.md "pair/abort"
    /// `pairing_code_mismatch`, from either side)
    CODE_MISMATCH,
    /// The client could not use the pairing PSK of the token entered: the token is another
    /// device's, or the client's pairing PSK changed
    TOKEN_MISMATCH,
    /// The client does not offer the method or format, or refused it (`method_not_supported`)
    METHOD_NOT_SUPPORTED,
    /// The client's attempt timed out (`attempt_timeout`)
    CLIENT_TIMED_OUT,
    /// The client's operator cancelled (`user_cancelled`)
    CLIENT_CANCELLED,
    /// The client runs another pairing or will not pair beside its connection to another server
    /// (`concurrent_attempt`)
    CONCURRENT_ATTEMPT,
    /// The client did not start the attempt within ServerTiming::pairing_start_timeout_us, or
    /// went silent mid-attempt for longer than its own attempt timeout allows
    TIMED_OUT,
    /// The long-term PSK could not be saved through the PersistenceProvider; nothing changed
    PERSISTENCE_FAILED,
    /// The client broke the pairing protocol; its connection was closed
    PROTOCOL_ERROR,
    /// The connection ended, or could not be made, before the pairing completed
    CONNECTION_LOST,
    /// SendspinServer::stop()
    SERVER_STOPPED,
};

/// @brief A client, connected or one the server can dial. A client not connected keeps the
/// values of its last connection.
struct ClientInfo {
    /// The client's base64url public key (spec connection.md "Identities"); empty for one found
    /// by browsing that the server has not connected to
    std::string client_id;
    /// The name from its client/hello, or its TXT `name` before the first connection
    std::string name;
    /// The URL the server dials; empty for a client that connected to the server
    std::string url;
    /// The operator's approval of the client, which lets it play without pairing; nullopt while
    /// it is not approved (SendspinServer::approve())
    std::optional<ApprovalScope> approval;
    /// Whether its client/hello enables unpaired access. Without it the client plays only once
    /// paired, approved or not (ServerListener::on_pairing_required()).
    bool unpaired_access{false};
    /// The server holds a long-term PSK for the client (spec pairing.md "Pairing Records"), so
    /// its connections are paired sessions and it plays without approval
    bool paired{false};
    /// The pairing methods its client/hello offers; present pairing as available when any is
    /// (spec pairing.md "Unpaired Access")
    PairingOffers pairing_offers;
    /// A pairing SendspinServer::begin_pairing() started is in progress: set in the ClientInfo of
    /// on_pairing_pending() and on_pairing_code_needed(), clear in on_pairing_result()
    bool pairing{false};
    /// Its connection completed under the Sentinel because it could not use its long-term PSK
    /// (ServerListener::on_credential_mismatch()), so it is held until paired again; cleared once
    /// a re-handshake of the connection completes
    bool credential_mismatch{false};
    /// The peer's address and port, as "192.0.2.1:8928" or "[2001:db8::1]:8928"
    std::string address;
    /// False for a client SendspinServer::clients() lists while the server can dial it: one it
    /// reached by connect_url(), or found by browsing and still advertising
    bool connected{true};
    /// The client connected to the server (spec connection.md "Client Initiated Connections")
    bool accepted{false};
    /// The server found the client by browsing mDNS
    bool discovered{false};
    /// Not connected, and the server dials it again on its own. False after a client/goodbye that
    /// asks the server not to reconnect (spec messaging.md "client/goodbye"), until
    /// SendspinServer::connect(), or for a client found by browsing, until it advertises again.
    bool reconnecting{false};
    /// The role versions its client/hello lists that the server implements, in the client's
    /// order: the roles it may be activated with ("player@v1", "visualizer@v1", ...); empty before
    /// its first connection
    std::vector<std::string> roles;
};

/// @brief Why a connected client went away
enum class DisconnectReason : uint8_t {
    /// The client sent client/goodbye
    GOODBYE,
    /// The connection dropped, timed out, or stopped accepting writes
    CONNECTION_LOST,
    /// The server closed it: a protocol error by the client, a send queue overflow or a cipher
    /// failure
    CLOSED_BY_SERVER,
    /// SendspinServer::stop()
    SERVER_STOPPED,
    /// SendspinServer::disconnect()
    DISCONNECTED_BY_HOST,
};

/// @brief The `reason` of a client/goodbye (spec messaging.md "client/goodbye")
enum class GoodbyeReason : uint8_t {
    ANOTHER_SERVER,
    SHUTDOWN,
    RESTART,
    USER_REQUEST,
    UNAUTHORIZED,
    PAIRING_REQUIRED,
    CONCURRENT_ATTEMPT,
    UNPAIRED,
};

/// @brief How SendspinServer::connect_url() dials
struct ConnectOptions {
    /// Keep dialing the URL on the server's own until a connection to it is activated, each dial
    /// after a failed dial or handshake waiting out the backoff that failure set (spec
    /// messaging.md "client/goodbye": backoff on repeated handshake failures). A
    /// client/goodbye that stops reconnecting (spec messaging.md "client/goodbye") and
    /// SendspinServer::cancel_connect_url() end it.
    bool retry_until_connected{false};
};

/// @brief How a connected client went away
struct ClientDisconnect {
    DisconnectReason reason{DisconnectReason::CONNECTION_LOST};
    /// With DisconnectReason::GOODBYE: the client/goodbye's reason, or nullopt for one this
    /// server does not recognize
    std::optional<GoodbyeReason> goodbye_reason;
};

enum class ErrorKind : uint8_t {
    /// A dial could not connect or complete the WebSocket upgrade, or was not made because the
    /// device was short of the internal RAM a connection needs
    DIAL_FAILED,
    /// The server advertises or browses, and mDNS is unavailable: the library is built without
    /// discovery, or the system's mDNS service (mDNSResponder, avahi-daemon) cannot be reached.
    /// The built-in provider keeps trying, and reports again each time the service goes away;
    /// clients found before are reported lost.
    DISCOVERY_UNAVAILABLE,
    /// A player listed no format the server can stream from its group's input, so it gets no
    /// stream (spec player/v1.md "client/hello player@v1 support object"); client_id names it.
    /// The server streams PCM (16, 24 or 32-bit) and FLAC (16 or 24-bit) at any rate that is a
    /// multiple of 10 or 25 Hz (so a frame of whole samples spans at most seven 20 ms cells), such
    /// as 11,025, 44,100 or 48,000 Hz, resampled from the input's rate, with the input's channel
    /// count, or to mono from any channel count, or from a mono input to any count. The input's
    /// format is the one last passed to set_format(), even while earlier input in another format
    /// still plays.
    NO_PLAYABLE_FORMAT,
    /// The client sent client/goodbye before it was connected (ServerListener::
    /// on_client_connected()), such as `concurrent_attempt` when it keeps another server's
    /// connection; goodbye_reason says why
    GOODBYE_BEFORE_ACTIVATION,
    /// A connection ended before the client was connected, other than by a client/goodbye or
    /// by the host (disconnect(), cancel_connect_url())
    HANDSHAKE_FAILED,
    /// The pipeline producing format for a group failed (an encoder or the resampler refused
    /// its input), so the format is not streamed in that group until its playback next stops;
    /// client_id names a player that streamed it, whose stream ends, or wanted it. With no
    /// format, a visualizer analysis failed (its FFT could not be built): client_id names a
    /// visualizer whose configuration it computed, which gets no visualizer stream until then
    PIPELINE_FAILED,
    /// The client record of client_id could not be saved or committed through the
    /// PersistenceProvider, or the store holds persistence_keys::MAX_CLIENT_RECORDS records
    /// already. A changed approval, name or identifiers apply until the server stops; a long-term
    /// PSK is never used unsaved, and its record is put back as it was.
    PERSISTENCE_FAILED,
    /// A source's input stream could not be taken (SendspinServer::route_source()): its format is
    /// one the server does not decode, its FLAC header one the decoder cannot read, the group's
    /// input refused it, its buffers could not be allocated, or a chunk was larger than the server
    /// holds. client_id names the source, format its stream's format. The server asks the source to
    /// stop, and does not ask it to stream again until it is routed again, reports itself available
    /// again after an unavailable report, gets its role again, or connects again.
    SOURCE_FAILED,
};

enum class StreamCodec : uint8_t { PCM, FLAC, OPUS };

/// @brief A player stream's format (spec player/v1.md "client/hello player@v1 support object"),
/// or a source's input stream's (spec roles/source/v1.md "client-stream/start")
struct StreamFormat {
    StreamCodec codec{StreamCodec::PCM};
    uint32_t sample_rate{0};
    uint8_t channels{0};
    uint8_t bit_depth{0};

    bool operator==(const StreamFormat&) const = default;
};

/// @brief A failure reported through ServerListener::on_error()
struct ServerError {
    ErrorKind kind{ErrorKind::DIAL_FAILED};
    /// The URL the server dialed
    std::string url;
    /// Empty until the client's client/init was accepted
    std::string client_id;
    /// With GOODBYE_BEFORE_ACTIVATION: the reason, or nullopt for one this server does not
    /// recognize
    std::optional<GoodbyeReason> goodbye_reason;
    /// With PIPELINE_FAILED: the format whose pipeline failed; nullopt for a visualizer analysis
    std::optional<StreamFormat> format;
};

// ==============================================================================
// Groups and group inputs
// ==============================================================================

/// @brief A group of clients that play together; unique while the server runs
struct GroupId {
    uint64_t value{0};

    friend constexpr auto operator<=>(const GroupId&, const GroupId&) = default;
};

/// @brief A GroupInput; unique while the server runs
struct InputId {
    uint64_t value{0};

    friend constexpr auto operator<=>(const InputId&, const InputId&) = default;
};

/// @brief A GroupInput::drain() call; unique while the server runs
struct DrainId {
    uint64_t value{0};

    friend constexpr auto operator<=>(const DrainId&, const DrainId&) = default;
};

/// @brief A group's `playback_state` (spec messaging.md "group/update")
enum class PlaybackState : uint8_t { PLAYING, STOPPED };

// ==============================================================================
// Controllers and metadata
// ==============================================================================

/// @brief A controller command the host carries out (spec controller/v1.md "client/command
/// controller object"). The server carries out `volume`, `mute` and `switch` itself.
enum class TransportCommand : uint8_t {
    PLAY,
    PAUSE,
    STOP,
    NEXT,
    PREVIOUS,
    REPEAT_OFF,
    REPEAT_ONE,
    REPEAT_ALL,
    SHUFFLE,
    UNSHUFFLE,
    SEEK,
    SEEK_RELATIVE,
};

enum class RepeatMode : uint8_t { OFF, ONE, ALL };

/// @brief What the host offers a group's controllers, and the state they show (spec
/// controller/v1.md "server/state controller object")
struct ControllerCaps {
    /// The commands the host carries out; each reaches it through
    /// ServerListener::on_transport_command()
    std::vector<TransportCommand> commands;
    RepeatMode repeat{RepeatMode::OFF};
    bool shuffle{false};
    /// The last position a seek may target, such as the current track's length. SEEK is offered
    /// only with it, so leave it unset for a live input.
    std::optional<uint32_t> seek_max_ms;
};

/// @brief The parameters of a TransportCommand
struct CommandArgs {
    /// The controller's client_id
    std::string client_id;
    /// SEEK: the position, from 0 to ControllerCaps::seek_max_ms. SEEK_RELATIVE: the position
    /// offset_ms leads to, clamped to the seekable range, when the group's metadata reports a
    /// position (SendspinServer::set_metadata()); otherwise unset, and the host clamps.
    std::optional<uint32_t> position_ms;
    /// SEEK_RELATIVE: the offset from the current position, as the controller sent it
    std::optional<int64_t> offset_ms;
};

/// @brief MetadataProgress::playback_speed at normal speed
constexpr uint32_t NORMAL_PLAYBACK_SPEED = 1000;
/// @brief The largest MetadataProgress::playback_speed SendspinServer::set_metadata() takes:
/// 100 times normal speed
constexpr uint32_t MAX_PLAYBACK_SPEED = 100 * NORMAL_PLAYBACK_SPEED;

/// @brief Playback progress (spec metadata/v1.md "server/state metadata object")
struct MetadataProgress {
    /// The position in the track when the metadata takes effect
    uint32_t track_progress_ms{0};
    /// 0 for an unknown or unlimited length
    uint32_t track_duration_ms{0};
    /// The speed times NORMAL_PLAYBACK_SPEED, at most MAX_PLAYBACK_SPEED; 0 is paused
    uint32_t playback_speed{NORMAL_PLAYBACK_SPEED};

    bool operator==(const MetadataProgress&) const = default;
};

/// @brief What a group plays, for its members' metadata role (spec metadata/v1.md
/// "server/state metadata object"); every field is optional
struct Metadata {
    std::optional<std::string> title;
    std::optional<std::string> artist;
    std::optional<std::string> album_artist;
    std::optional<std::string> album;
    std::optional<std::string> artwork_url;
    std::optional<uint32_t> year;
    /// 1 for the first track
    std::optional<uint32_t> track;
    /// Set whenever there is a position to report
    std::optional<MetadataProgress> progress;

    bool operator==(const Metadata&) const = default;
};

struct GroupInfo {
    GroupId id;
    /// The `group_name` its members see
    std::string name;
    PlaybackState playback_state{PlaybackState::STOPPED};
    /// The `client_id` of every member
    std::vector<std::string> client_ids;
};

// ==============================================================================
// Artwork and colors
// ==============================================================================

/// @brief What an artwork channel shows (spec roles/artwork/v1.md "client/state artwork object")
enum class ArtworkSource : uint8_t { ALBUM, ARTIST };

/// The images SendspinServer::set_artwork() takes; with them, the memory an image's decode and
/// encodings hold is bounded
namespace artwork_limits {
/// The largest image SendspinServer::set_artwork() takes, in bytes
constexpr size_t MAX_IMAGE_BYTES = size_t{32} * 1024 * 1024;
/// The most pixels an image SendspinServer::set_artwork() takes may have: about 4,900 by 4,900
constexpr uint64_t MAX_IMAGE_PIXELS = uint64_t{24} * 1000 * 1000;
/// The most scans a JPEG SendspinServer::set_artwork() takes may hold; a progressive JPEG holds
/// about 10, and each is a pass over the whole image when decoding
constexpr size_t MAX_JPEG_SCANS = 100;
/// The widest and tallest channel the server delivers images to; a larger channel is cleared
constexpr uint32_t MAX_CHANNEL_SIDE = 4096;
}  // namespace artwork_limits

struct RgbColor {
    uint8_t r{0};
    uint8_t g{0};
    uint8_t b{0};

    bool operator==(const RgbColor&) const = default;
};

/// @brief A group's colors for its members' color role (spec roles/color/v1.md "server/state color
/// object"); a field left unset is not sent
struct Colors {
    /// A background for dark mode, for white text and on_dark
    std::optional<RgbColor> background_dark;
    /// A background for light mode, for black text and on_light
    std::optional<RgbColor> background_light;
    /// The dominant color, sent as given
    std::optional<RgbColor> primary;
    /// A secondary color, sent as given
    std::optional<RgbColor> accent;
    /// A light color for dark backgrounds, also usable as a background for black text
    std::optional<RgbColor> on_dark;
    /// A dark color for light backgrounds, also usable as a background for white text
    std::optional<RgbColor> on_light;

    bool operator==(const Colors&) const = default;
};

/// @brief How the host's PCM is laid out: interleaved by channel, little-endian
enum class SampleFormat : uint8_t {
    /// Signed 16-bit integers
    S16,
    /// Signed 24-bit integers packed in 3 bytes
    S24,
    /// 32-bit floats with full scale at +-1.0; values past it are clipped
    F32,
};

/// @brief The PCM a GroupInput takes
struct InputFormat {
    /// At most 384,000 Hz
    uint32_t sample_rate{0};
    /// From 1 to 32
    uint8_t channels{0};
    SampleFormat sample_format{SampleFormat::S16};

    bool operator==(const InputFormat&) const = default;
};

/// @brief Where a group input's audio comes from
enum class InputKind : uint8_t {
    /// Written ahead of time from a file or a decoder: the server takes it as far ahead as its
    /// players can hold, and a resume continues from the paused position
    BUFFERED,
    /// Produced in real time, such as a capture device: write() takes it up to 200 ms past the
    /// group's lead ahead of the time now, players get the least lead they allow, writes are taken
    /// while paused, and a resume continues from the newest sample. Its writer drops what write()
    /// refuses instead of waiting (GroupInput::write()). A silence keeps the run, and the next
    /// write moves the timeline forward by it in one gap; a host whose source can go silent for
    /// longer than a gap should call GroupInput::stop() and start a new run with its next samples.
    /// A latency raised by a joiner who has left, a stall or a source running fast comes back
    /// down once two seconds of writes all landed at least 40 ms later than a new run would place
    /// them: the input cuts the least of those excesses from the writes that follow, an audible
    /// skip (GroupInput::write()).
    LIVE,
};

/// @brief The write jitter a live input declares when the host gives none
constexpr int64_t DEFAULT_WRITE_JITTER_US = 20000;
/// @brief The largest write jitter a live input may declare
constexpr int64_t MAX_WRITE_JITTER_US = 1000000;

struct InputOptions {
    InputFormat format;
    InputKind kind{InputKind::BUFFERED};
    /// A live input's write jitter: how late a write may come, from 0 to MAX_WRITE_JITTER_US. The
    /// lead its players get grows by it; a write later than it moves the timeline forward, which
    /// players hear as a gap.
    int64_t write_jitter_us{DEFAULT_WRITE_JITTER_US};
};

/// @brief Why GroupInput::write() took what it took, and what GroupInput::state() and
/// GroupInput::wait_writable() report
enum class InputState : uint8_t {
    /// The input admits at least one cell (20 ms) of samples
    OPEN,
    /// The input holds as much as it admits; wait for GroupInput::wait_writable() or
    /// ServerListener::on_writable()
    FULL,
    /// A drain() is pending until its ServerListener::on_drained()
    DRAINING,
    /// The server stopped the group's playback; the host acknowledges with GroupInput::stop()
    STOP_LATCHED,
    /// The server stopped, or the input could not be attached to its group
    CLOSED,
};

/// @brief The outcome of SendspinServer::open_input()
enum class OpenInputResult : uint8_t {
    OK,
    /// The server is not running, or is stopping
    NOT_RUNNING,
    /// The group is not in SendspinServer::groups()
    UNKNOWN_GROUP,
    /// The format is out of range (InputFormat)
    INVALID_FORMAT,
    /// A live input's write_jitter_us is out of range (InputOptions)
    INVALID_JITTER,
    /// The group holds an input that is neither released nor closed
    GROUP_BUSY,
};

struct WriteResult {
    /// Frames taken from the start of the block
    size_t frames{0};
    InputState state{InputState::OPEN};
};

/// @brief The outcome every GroupInput::drain() call reports exactly once
enum class DrainOutcome : uint8_t {
    /// The drain completed: every player's stream ended after the audio written before it
    /// played out, or there was nothing to play
    PLAYED,
    /// A GroupInput::flush() cancelled it
    FLUSHED,
    /// A stop discarded it: GroupInput::stop(), a stop by the server, releasing the input or
    /// stopping the server, or a drain() while a server stop waited for acknowledgement
    STOPPED,
};

// ==============================================================================
// Sources
// ==============================================================================

/// @brief A source's line sensing (spec roles/source/v1.md "client/state source object")
enum class SourceSignal : uint8_t { PRESENT, ABSENT };

/// @brief The write jitter a routed source's input declares when the host gives none
constexpr int64_t DEFAULT_SOURCE_WRITE_JITTER_US = 60000;

/// @brief How SendspinServer::route_source() feeds the group
struct SourceRouteOptions {
    /// The input's write jitter (InputOptions::write_jitter_us), from 0 to MAX_WRITE_JITTER_US:
    /// how late a chunk of the source's audio may arrive and still play, which the group's lead
    /// grows by. A chunk later than it is dropped as the backlog of a stall.
    int64_t write_jitter_us{DEFAULT_SOURCE_WRITE_JITTER_US};
};

/// @brief The outcome of SendspinServer::route_source()
enum class RouteSourceResult : uint8_t {
    OK,
    /// The server is not running, or is stopping
    NOT_RUNNING,
    /// The client_id is not one
    INVALID_CLIENT,
    /// The group is not in SendspinServer::groups()
    UNKNOWN_GROUP,
    /// SourceRouteOptions::write_jitter_us is out of range
    INVALID_JITTER,
    /// The group holds an input that is neither released nor closed
    GROUP_BUSY,
};

}  // namespace sendspin::server
