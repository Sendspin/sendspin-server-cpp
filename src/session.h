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

/// @file session.h
/// @brief The protocol state of one connection, from client/init through activation, time sync,
/// the player, visualizer and artwork streams and re-handshakes. Performs no I/O: it consumes
/// inbound WebSocket messages and deadlines and yields outbound WebSocket messages, so the owner
/// decides when they are written (SessionDriver binds it to a Connection). Core thread only.

#pragma once

#include "artwork_stream.h"
#include "crypto/keys.h"
#include "fragmentation.h"
#include "memory.h"
#include "message_kind.h"
#include "messages.h"
#include "noise.h"
#include "player_stream.h"
#include "profile.h"
#include "ring.h"
#include "sched/types.h"
#include "send_queues.h"
#include "session_end.h"
#include "source_feed.h"
#include "source_stream.h"
#include "trust_policy.h"
#include "visualizer_stream.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace sendspin::server {

/// @brief The spec's suggested limit for receiving each next handshake message (connection.md
/// "Failure Handling")
constexpr int64_t DEFAULT_STEP_TIMEOUT_US = 30000000;

/// @brief Default cap on a reassembled inbound message
constexpr size_t DEFAULT_MAX_REASSEMBLED_BYTES = profile::DEFAULT_MAX_REASSEMBLED_BYTES;

/// @brief Settings shared by every session; must outlive them
struct SessionSettings {
    /// The `server/hello` content
    ServerHello hello;
    /// Limit for each of AWAIT_CLIENT_INIT, AWAIT_NOISE_MSG2 and AWAIT_CLIENT_HELLO
    int64_t step_timeout_us{DEFAULT_STEP_TIMEOUT_US};
    /// Cap on a reassembled inbound message, its ID byte excluded
    size_t max_reassembled_bytes{DEFAULT_MAX_REASSEMBLED_BYTES};
    /// Plaintext size of each fragment of an outbound message that needs fragmenting, from
    /// MIN_FRAGMENT_PLAINTEXT to MAX_TRANSPORT_PLAINTEXT
    size_t max_fragment_plaintext{MAX_TRANSPORT_PLAINTEXT};
    SendQueueCaps queue_caps;
};

enum class SessionPhase : uint8_t {
    AWAIT_CLIENT_INIT,
    AWAIT_NOISE_MSG2,
    AWAIT_CLIENT_HELLO,
    /// client/hello is validated; activate() queues the initial server/activate
    READY_FOR_ACTIVATION,
    /// The initial server/activate is queued and waits to be taken
    ACTIVATING,
    /// The initial server/activate is taken: the ignore rules apply, client/time is answered and
    /// client/state taken in
    ACTIVE,
    /// Ended; end() says why. A queued server/error goes out before the connection closes.
    CLOSED,
};

/// @brief One outbound WebSocket message, encrypted when BINARY
struct OutboundMessage {
    MessageKind kind{MessageKind::TEXT};
    std::vector<uint8_t> bytes;
    /// A server/time: the last message of its write, so no message is built between its stamp and
    /// its write
    bool time_reply{false};
};

/// @brief Receives the plaintext of every WebSocket message a session takes in or yields
class SessionTrace {
public:
    virtual ~SessionTrace() = default;

    /// @brief An inbound message: a text message as received, a binary one after decryption (so
    /// one received before transport mode, a protocol error, is not traced)
    virtual void on_inbound(MessageKind kind, std::span<const uint8_t> plaintext,
                            int64_t rx_us) = 0;

    /// @brief An outbound message before encryption, with the stamp taken immediately before it
    /// @param audio_bytes an audio chunk's encoded audio, counted from the frames packed into it;
    /// 0 for any other message
    virtual void on_outbound(MessageKind kind, std::span<const uint8_t> plaintext, int64_t stamp_us,
                             size_t audio_bytes) = 0;

    /// @brief The write of the last outbound message completed (SessionDriver)
    virtual void on_written(int64_t done_us) = 0;
};

/// @brief Chooses the PSK a session's handshake references
class CredentialSource {
public:
    virtual ~CredentialSource() = default;

    /// @brief Called once, when the client/init is validated
    virtual HandshakePsk handshake_psk(const ClientInit& init) = 0;
};

class Session {
public:
    /// @param identity the server's static key pair; must outlive the session
    /// @param settings must outlive the session
    /// @param trace nullptr, or a sink that outlives the session
    /// @param credentials nullptr to reference the Sentinel always, or a source that outlives the
    /// session
    Session(const Identity& identity, const SessionSettings& settings,
            SessionTrace* trace = nullptr, CredentialSource* credentials = nullptr);

    /// @brief Starts the AWAIT_CLIENT_INIT timeout once the transport is connected. Called once,
    /// before anything else.
    void start(int64_t now_us);

    /// @brief One complete inbound WebSocket message
    /// @param rx_us its receive stamp, taken by the transport when its last byte was in
    void on_message(MessageKind kind, std::span<const uint8_t> payload, int64_t rx_us);

    /// @brief Ends the session with TIMED_OUT if now_us is at or past deadline_us()
    void on_deadline(int64_t now_us);

    /// @brief When the current handshake step, or a re-handshake's wait for message 2, times out;
    /// nullopt outside them
    std::optional<int64_t> deadline_us() const;

    /// @brief Queues a server/activate, behind a marker: the initial one in READY_FOR_ACTIVATION,
    /// and later ones from ACTIVATING on
    void activate(const ServerActivate& activation);

    /// @brief Queues a re-handshake to psk (connection.md "Re-handshake"), behind a marker that
    /// evaluates against activation, the server/activate frozen for after it. When the rekey
    /// reaches the head of the control FIFO, Noise message 1 goes out under the current keys, with
    /// `h` as the prologue; from then nothing is written until message 2 has been read, under psk
    /// only (a miss fails it), and then the frozen activation goes first under the new keys.
    /// Old-key messages received meanwhile are taken in as usual. Only from ACTIVATING on.
    void rekey(const HandshakePsk& psk, const ServerActivate& activation);

    /// @brief The group the player and visualizer streams follow; nullptr for none. Must outlive
    /// its use.
    void set_stream_source(StreamSource* source) {
        this->source_ = source;
    }

    /// @brief The group the artwork stream shows; nullptr for none. Must outlive its use.
    void set_artwork_view(ArtworkView* view) {
        this->artwork_view_ = view;
    }

    /// @brief ArtworkStream::set_left()
    void set_artwork_left(bool left) {
        this->artwork_.set_left(left);
    }

    /// @brief Queues a desired-state marker, with the kind of the event that changed the desired
    /// state when it can need a trigger. Only from ACTIVATING on.
    void queue_marker(std::optional<sched::TriggerKind> kind);

    /// @brief Queues a JSON control message. Only from ACTIVATING on.
    void queue_json_message(const std::string& json);

    /// @brief Whether take_message() has a message now; runs the cursor's step first
    bool has_message();

    /// @brief From one cursor step and snapshot, which the driver's pump takes once per event:
    /// ready as has_message() gives it, and wake_us, the time a message is ready, now when one is,
    /// else when one becomes ready by the passage of time alone, nullopt for none and before the
    /// session is ACTIVE
    struct Poll {
        bool ready{false};
        std::optional<int64_t> wake_us;
    };
    Poll poll();

    /// @brief The next outbound message in select_next() order, built and encrypted by this call.
    /// Its stamp, which a server/time, stream/start or stream/clear carries as
    /// `server_transmitted` and a chunk as `send_ahead`, is taken here, immediately before
    /// encryption (messaging.md "Transmit timestamps"). A message larger than one Noise transport
    /// message goes out as consecutive fragments, one per call, with nothing in between. nullopt
    /// when none is ready, or when encryption failed and ended the session.
    std::optional<OutboundMessage> take_message();

    /// @brief Whether a client/state was taken in since the last call; the owner then reacts and
    /// calls queue_state_marker()
    bool take_state_changed() {
        return std::exchange(this->state_changed_, false);
    }

    /// @brief Queues the marker the last client/state calls for, with its trigger kind
    void queue_state_marker();

    /// @brief The controller requests taken in since the last call, from client/command while
    /// the controller role is active
    std::vector<ControllerRequest> take_commands() {
        return std::exchange(this->commands_, {});
    }

    /// @brief Ends the session for a protocol error the owner found in a message it took in
    void reject() {
        this->finish(SessionEnd::PROTOCOL_ERROR);
    }

    /// @brief Whether a client/leave was taken in since the last call
    bool take_leave() {
        return std::exchange(this->leave_, false);
    }

    /// @brief Whether a re-handshake completed since the last call, so psk_match() changed
    bool take_rekeyed() {
        return std::exchange(this->rekeyed_, false);
    }

    /// @brief The pairing messages taken in since the last call (pairing.md "Messages"), parsed
    /// and validated; whether one fits the attempt is the owner's to judge
    std::vector<ClientPairing> take_pairing_messages() {
        return std::exchange(this->pairing_messages_, {});
    }

    /// @brief The pairing activations taken for writing since the last Noise handshake: the
    /// `pairing_index` the client counts (pairing.md "Pairing index")
    uint32_t pairing_index() const {
        return this->pairing_written_;
    }

    /// @brief The pairing_index the pairing activation queued last will have once written
    uint32_t queued_pairing_index() const {
        return this->pairing_queued_;
    }

    /// @brief Whether a re-handshake is queued or not yet complete: from rekey() until its frozen
    /// activation is taken for writing
    bool rekey_pending() const {
        return !this->rekeys_.empty() || this->rekey_phase_ != sched::Phase::NORMAL;
    }

    /// @brief The handshake hash `h` of the keys in use, from AWAIT_CLIENT_HELLO on
    const HandshakeHash& handshake_hash() const;

    /// @brief The client/init's suite, from AWAIT_NOISE_MSG2 on
    CipherSuite suite() const;

    /// @brief Whether role is in the active_roles of the activation last taken for writing
    bool role_active(std::string_view role) const;

    /// @brief Whether role is in the active_roles of the activation last queued, so a message
    /// queued now for it reaches the client while it is active
    bool role_queued(std::string_view role) const;

    /// @brief The active_roles of the activation last queued
    const std::vector<std::string>& next_roles() const {
        return this->queued_.empty() ? this->active_roles_ : this->queued_.back().active_roles;
    }

    const PlayerStream& player() const {
        return this->player_;
    }

    const VisualizerStream& visualizer() const {
        return this->visualizer_;
    }

    const ArtworkStream& artwork() const {
        return this->artwork_;
    }

    const SourceStream& source() const {
        return this->source_role_;
    }

    /// @brief Where the source role's input stream goes: the route's feed, its items named by
    /// connection; nullptr while the source is not routed, so the server stops it
    /// (roles/source/v1.md "Source command semantics"). An open stream starts again in a new feed.
    /// Must outlive its use. Only from ACTIVATING on.
    void set_source_route(SourceFeed* feed, uint64_t connection);

    /// @brief The changes to the source role since the last call
    std::vector<SourceEvent> take_source_events() {
        return this->source_role_.take_events();
    }

    /// @brief The route cannot take stream, the stream SourceStream::stream_number() names: it
    /// fails (SourceStream::fail()) when it is still open
    void fail_source(uint64_t stream);

    /// @brief The operator's format for the player stream (PlayerStream::set_format_override())
    void set_format_override(std::optional<AudioFormat> format) {
        this->player_.set_format_override(format);
    }

    SessionPhase phase() const {
        return this->phase_;
    }

    std::optional<SessionEnd> end() const {
        return this->end_;
    }

    /// @brief Ends the session for the host (SendspinServer::disconnect()); what is queued is
    /// dropped. Does nothing once the session ended.
    void close_by_host();

    /// @brief Ends a session in READY_FOR_ACTIVATION that the server has no room for
    /// (connection_limits), with no activation
    void refuse();

    /// @brief The PSK the session's keys come from, from AWAIT_CLIENT_HELLO on
    const PskMatch& psk_match() const {
        return this->psk_match_;
    }

    /// @brief The PSK of the activation queued last: psk_match(), or that of the last re-handshake
    /// queued, which the activations behind it go out under
    const PskMatch& next_psk_match() const {
        return this->next_psk_match_;
    }

    /// @brief The validated client/init, from AWAIT_NOISE_MSG2 on
    const std::optional<ClientInit>& client_init() const {
        return this->client_init_;
    }

    /// @brief The validated client/hello, from READY_FOR_ACTIVATION on
    const std::optional<ClientHello>& client_hello() const {
        return this->client_hello_;
    }

    /// @brief With SessionEnd::INIT_REJECTED: the reason sent in server/error
    std::optional<InitError> init_error() const {
        return this->init_error_;
    }

    /// @brief With SessionEnd::GOODBYE: the message received
    const std::optional<ClientGoodbye>& goodbye() const {
        return this->goodbye_;
    }

    /// @brief This session's SessionOutcome
    SessionOutcome outcome() const {
        return {this->end_, this->client_init_ ? this->client_init_->client_id : std::string{},
                this->init_error_, this->goodbye_};
    }

private:
    /// An activation queued and not yet taken for writing
    struct QueuedActivation {
        std::vector<std::string> active_roles;
        bool pairing{false};
    };

    void on_client_init(MessageKind kind, std::span<const uint8_t> payload, int64_t rx_us);
    void on_message_2(MessageKind kind, std::span<const uint8_t> payload, int64_t rx_us);
    void on_transport_message(MessageKind kind, std::span<const uint8_t> payload, int64_t rx_us);
    void on_application_message(std::span<const uint8_t> message, int64_t rx_us);
    /// @brief A re-handshake's Noise message 2, in the JSON message parsed
    void on_rekey_message_2(std::string_view json);
    void on_active_message(const ClientMessage& message);

    void on_client_state(const ClientState& state);
    void on_client_command(const ClientCommand& command);
    /// @brief roles/source/v1.md "client-stream/start", "client-stream/end" and "Source Audio
    /// Chunks (Binary)"
    void on_stream_start(const ClientStreamStart& start);
    void on_stream_end();
    void on_source_chunk(std::span<const uint8_t> message, int64_t rx_us);
    /// @brief Hands the open stream's start to the route; fails the stream when it has no room
    void push_source_start();

    /// @brief Queues a control message
    /// @return false when it overflowed a cap, which ended the session
    bool queue_text(const std::string& text);
    bool queue_json(const std::string& json);
    bool queue(OutboundPlaintext message);
    /// @brief Traces an inbound plaintext, its pairing secrets replaced
    void trace_inbound(std::span<const uint8_t> plaintext, int64_t rx_us);
    /// @brief An activation's message and stream roles, its active_roles appended to the queued
    /// ones; nullopt when it cannot be encoded, which ended the session
    std::optional<OutboundPlaintext> prepare_activation(const ServerActivate& activation,
                                                        sched::RoleSet& roles);

    /// @brief The cursor's step before every selection, which may queue a drain-watch marker
    void prepare(int64_t now_us);
    sched::PumpSnapshot snapshot(int64_t now_us) const;
    /// @brief The control FIFO's head as one message: a marker's emission, an activation, a
    /// re-handshake's message 1 or a message. nullopt for a marker with nothing left, which is
    /// popped.
    std::optional<OutboundPlaintext> take_control(int64_t now_us, int64_t& stamp_us);
    /// @brief The rekey at the head: its message 1, which starts the freeze
    std::optional<OutboundPlaintext> take_rekey(int64_t& stamp_us);
    /// @brief Writing an activation: the roles it holds become the written ones, and a marker
    /// follows
    void on_activation_taken(sched::RoleSet roles);
    /// @brief A marker at the head: its next stream message over the player and visualizer roles,
    /// nullopt when it has none left
    std::optional<OutboundPlaintext> take_marker(int64_t now_us, int64_t& stamp_us);
    /// @brief Records a kind-setting event's kind for both stream roles
    void set_pending_kind(std::optional<sched::TriggerKind> kind);
    /// @brief Ends the session; a silent end drops every queued message
    void finish(SessionEnd end);
    /// @brief Whether role is in the activation a marker at the head evaluates against: the first
    /// queued, else the one last written
    bool role_ahead(std::string_view role) const;

    const Identity& identity_;
    const SessionSettings& settings_;
    SessionTrace* trace_;
    Reassembler reassembler_;
    SendQueues queues_;
    CredentialSource* credentials_;
    std::optional<NoiseInitiator> initiator_;
    std::optional<TransportCiphers> ciphers_;
    /// The category of the PSK message 1 referenced
    PskCategory referenced_{PskCategory::SENTINEL};
    PskMatch psk_match_;
    PskMatch next_psk_match_;
    /// The PSK of each rekey queued, oldest first: one per rekey in the control FIFO, whose entry
    /// cap bounds them; room for one at first
    Ring<HandshakePsk> rekeys_{1, Ring<HandshakePsk>::UNLIMITED, MemoryLocation::PREFER_EXTERNAL};
    /// The send path's re-handshake phase
    sched::Phase rekey_phase_{sched::Phase::NORMAL};
    /// From a rekey's message 1 until its activation is taken: the frozen activation
    std::optional<OutboundPlaintext> rekey_activation_;
    sched::RoleSet rekey_roles_;
    std::optional<ClientInit> client_init_;
    std::optional<ClientHello> client_hello_;
    std::optional<ClientGoodbye> goodbye_;
    std::optional<InitError> init_error_;
    std::optional<SessionEnd> end_;
    std::vector<uint8_t> scratch_;
    int64_t deadline_us_{0};
    PlayerStream player_;
    VisualizerStream visualizer_;
    StreamSource* source_{nullptr};
    ArtworkStream artwork_;
    ArtworkView* artwork_view_{nullptr};
    SourceStream source_role_;
    SourceFeed* source_feed_{nullptr};
    uint64_t source_connection_{0};
    /// The artwork queue's step, as the last prepare() found it
    ArtworkStep artwork_step_;
    /// The stream roles of the activation last written
    sched::RoleSet roles_written_;
    /// The active_roles of the activation last written
    std::vector<std::string> active_roles_;
    /// Each activation queued behind it, oldest first: one per activation or rekey in the control
    /// FIFO, whose entry cap bounds them, and a rekey's frozen activation in its handshake; room
    /// for two at first
    Ring<QueuedActivation> queued_{2, Ring<QueuedActivation>::UNLIMITED,
                                   MemoryLocation::PREFER_EXTERNAL};
    /// Pairing activations written since the last Noise handshake, and the count the last one
    /// queued will have
    uint32_t pairing_written_{0};
    uint32_t pairing_queued_{0};
    std::vector<ClientPairing> pairing_messages_;
    std::vector<ControllerRequest> commands_;
    bool leave_{false};
    bool rekeyed_{false};
    bool state_changed_{false};
    std::optional<sched::TriggerKind> state_marker_kind_;
    SessionPhase phase_{SessionPhase::AWAIT_CLIENT_INIT};
};

}  // namespace sendspin::server
