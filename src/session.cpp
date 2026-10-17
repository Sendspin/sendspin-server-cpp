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

#include "session.h"

#include "clock.h"
#include "crypto/crypto.h"
#include "playback_policy.h"
#include "sched/chunk.h"
#include "source_decoder.h"

#include <algorithm>
#include <array>
#include <span>
#include <string_view>
#include <utility>
#include <variant>

namespace sendspin::server {

namespace {

std::string_view as_text(std::span<const uint8_t> bytes) {
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

std::span<const uint8_t> as_bytes(std::string_view text) {
    return {reinterpret_cast<const uint8_t*>(text.data()), text.size()};
}

/// JSON and source audio are the message IDs the server implements: before the initial
/// server/activate a client sends only client/hello and client/goodbye, and after it other IDs
/// are ignored
bool is_buffered(uint8_t message_id) {
    return message_id == MESSAGE_ID_JSON || message_id == MESSAGE_ID_SOURCE_AUDIO;
}

}  // namespace

Session::Session(const Identity& identity, const SessionSettings& settings, SessionTrace* trace,
                 CredentialSource* credentials)
    : identity_(identity),
      settings_(settings),
      trace_(trace),
      reassembler_(settings.max_reassembled_bytes, &is_buffered),
      queues_(settings.queue_caps),
      credentials_(credentials) {}

Session::~Session() {
    this->wipe_scratch();
}

void Session::start(int64_t now_us) {
    this->deadline_us_ = now_us + this->settings_.step_timeout_us;
}

std::optional<int64_t> Session::deadline_us() const {
    switch (this->phase_) {
        case SessionPhase::AWAIT_CLIENT_INIT:
        case SessionPhase::AWAIT_NOISE_MSG2:
        case SessionPhase::AWAIT_CLIENT_HELLO:
            return this->deadline_us_;
        case SessionPhase::ACTIVATING:
        case SessionPhase::ACTIVE:
            if (this->rekey_phase_ == sched::Phase::AWAIT_MSG2) {
                return this->deadline_us_;
            }
            break;
        case SessionPhase::READY_FOR_ACTIVATION:
        case SessionPhase::CLOSED:
            break;
    }
    return std::nullopt;
}

void Session::on_deadline(int64_t now_us) {
    const std::optional<int64_t> deadline = this->deadline_us();
    if (deadline && now_us >= *deadline) {
        this->finish(SessionEnd::TIMED_OUT);
    }
}

void Session::on_message(MessageKind kind, std::span<const uint8_t> payload, int64_t rx_us) {
    // A binary message is traced once decrypted
    if (this->trace_ != nullptr && this->phase_ != SessionPhase::CLOSED &&
        kind == MessageKind::TEXT) {
        this->trace_->on_inbound(kind, payload, rx_us);
    }
    switch (this->phase_) {
        case SessionPhase::AWAIT_CLIENT_INIT:
            this->on_client_init(kind, payload, rx_us);
            break;
        case SessionPhase::AWAIT_NOISE_MSG2:
            this->on_message_2(kind, payload, rx_us);
            break;
        case SessionPhase::AWAIT_CLIENT_HELLO:
        case SessionPhase::READY_FOR_ACTIVATION:
        case SessionPhase::ACTIVATING:
        case SessionPhase::ACTIVE:
            this->on_transport_message(kind, payload, rx_us);
            break;
        case SessionPhase::CLOSED:
            break;
    }
}

// messaging.md "Communication": client/init, then server/init immediately followed by Noise
// message 1
void Session::on_client_init(MessageKind kind, std::span<const uint8_t> payload, int64_t rx_us) {
    std::variant<ClientInit, InitError> result =
        kind == MessageKind::TEXT ? validate_client_init(as_text(payload), rx_us)
                                  : std::variant<ClientInit, InitError>(InitError::MALFORMED);
    if (const InitError* error = std::get_if<InitError>(&result)) {
        // connection.md "Failure Handling": server/error with the matching reason, then close
        this->init_error_ = *error;
        if (this->queue_text(build_server_error(*error))) {
            this->finish(SessionEnd::INIT_REJECTED);
        }
        return;
    }
    ClientInit& init = std::get<ClientInit>(result);

    // connection.md "Prologue": the exact client/init bytes, then the exact server/init bytes
    const std::string server_init = build_server_init(this->identity_.peer_id());
    std::vector<uint8_t> prologue(payload.begin(), payload.end());
    prologue.insert(prologue.end(), server_init.begin(), server_init.end());

    // connection.md "Pre-Shared Key": the Sentinel when no other PSK applies
    const HandshakePsk psk =
        this->credentials_ != nullptr ? this->credentials_->handshake_psk(init) : HandshakePsk{};
    this->initiator_ =
        NoiseInitiator::create(init.suite, this->identity_, init.client_key, prologue, psk.psk);
    this->referenced_ = psk.category;
    const std::string payload_1 = build_message_1_payload(psk_id_for(psk.psk), psk.category);
    std::optional<std::vector<uint8_t>> message_1;
    if (this->initiator_) {
        message_1 = this->initiator_->write_message_1(as_bytes(payload_1));
    }
    if (!message_1) {
        this->finish(SessionEnd::HANDSHAKE_FAILED);
        return;
    }
    this->client_init_ = std::move(init);
    if (!this->queue_text(server_init) || !this->queue_text(build_noise_handshake(*message_1))) {
        return;
    }
    this->phase_ = SessionPhase::AWAIT_NOISE_MSG2;
    this->deadline_us_ = rx_us + this->settings_.step_timeout_us;
}

// messaging.md "noise/handshake"; every failure here is silent (connection.md "Failure Handling")
void Session::on_message_2(MessageKind kind, std::span<const uint8_t> payload, int64_t rx_us) {
    std::optional<std::vector<uint8_t>> message_2;
    if (kind == MessageKind::TEXT) {
        message_2 = parse_noise_handshake(as_text(payload));
    }
    if (!message_2 || !this->initiator_) {
        this->finish(SessionEnd::HANDSHAKE_FAILED);
        return;
    }
    std::optional<std::vector<uint8_t>> payload_2 = this->initiator_->read_message_2(*message_2);
    PskMatch match{this->referenced_, std::nullopt};
    // connection.md "Sentinel Fallback": a client that could not use the referenced PSK completed
    // message 2 under the Sentinel
    if (!payload_2 && this->referenced_ != PskCategory::SENTINEL) {
        payload_2 = this->initiator_->replay_message_2(SENTINEL_PSK, *message_2);
        match = {PskCategory::SENTINEL, this->referenced_};
    }
    if (!payload_2 || !is_message_2_payload(*payload_2)) {
        this->finish(SessionEnd::HANDSHAKE_FAILED);
        return;
    }
    this->psk_match_ = match;
    this->next_psk_match_ = match;
    this->ciphers_ = this->initiator_->split();
    this->initiator_.reset();
    if (!this->ciphers_) {
        this->finish(SessionEnd::HANDSHAKE_FAILED);
        return;
    }
    // messaging.md "Communication": server/hello is the first message in transport mode
    const std::optional<std::string> hello = build_server_hello(this->settings_.hello);
    if (!hello) {
        this->finish(SessionEnd::ENCODE_FAILED);
        return;
    }
    if (!this->queue_json(*hello)) {
        return;
    }
    this->phase_ = SessionPhase::AWAIT_CLIENT_HELLO;
    this->deadline_us_ = rx_us + this->settings_.step_timeout_us;
}

void Session::on_transport_message(MessageKind kind, std::span<const uint8_t> payload,
                                   int64_t rx_us) {
    // connection.md "Failure Handling": cleartext after the switch to transport mode
    if (kind == MessageKind::TEXT) {
        this->finish(SessionEnd::CLEARTEXT_IN_TRANSPORT);
        return;
    }
    // Emptied first so a growth copies nothing it would overwrite
    this->scratch_.clear();
    secure_reserve(this->scratch_, payload.size(), this->scratch_written_);
    this->scratch_.assign(payload.begin(), payload.end());
    this->scratch_written_ = std::max(this->scratch_written_, this->scratch_.size());
    if (!this->ciphers_ || !this->ciphers_->decrypt(this->scratch_)) {
        this->finish(SessionEnd::DECRYPT_FAILED);
        return;
    }
    // A fragment is traced once its message is whole
    const bool fragment = !this->scratch_.empty() && this->scratch_[0] == MESSAGE_ID_FRAGMENT;
    if (!fragment) {
        this->trace_inbound(this->scratch_, rx_us);
    }
    switch (this->reassembler_.accept(this->scratch_)) {
        case Reassembler::Outcome::PENDING:
            return;
        case Reassembler::Outcome::MESSAGE:
            if (fragment) {
                this->trace_inbound(this->reassembler_.message(), rx_us);
            }
            this->on_application_message(this->reassembler_.message(), rx_us);
            return;
        case Reassembler::Outcome::DISCARDED:
            // messaging.md "Communication": an unimplemented ID is ignored once activated; before
            // that a client sends only JSON
            if (this->phase_ != SessionPhase::ACTIVE) {
                this->finish(SessionEnd::PROTOCOL_ERROR);
            }
            return;
        case Reassembler::Outcome::MALFORMED:
            this->finish(SessionEnd::MALFORMED_FRAGMENTS);
            return;
        case Reassembler::Outcome::TOO_LARGE:
            this->finish(SessionEnd::MESSAGE_TOO_LARGE);
            return;
    }
}

void Session::wipe_scratch() {
    secure_zero_storage(this->scratch_, this->scratch_written_);
    this->scratch_written_ = 0;
}

void Session::trace_inbound(std::span<const uint8_t> plaintext, int64_t rx_us) {
    if (this->trace_ == nullptr) {
        return;
    }
    std::optional<std::vector<uint8_t>> redacted = redact_pairing_secrets(plaintext);
    this->trace_->on_inbound(MessageKind::BINARY, redacted ? *redacted : plaintext, rx_us);
}

// messaging.md "Communication": until the initial server/activate the client sends client/hello
// once and may send client/goodbye, and the ignore rules do not apply
void Session::on_application_message(std::span<const uint8_t> message, int64_t rx_us) {
    if (message[0] != MESSAGE_ID_JSON) {
        // messaging.md "Communication": binary messages whose ID is not implemented are ignored
        if (this->phase_ != SessionPhase::ACTIVE) {
            this->finish(SessionEnd::PROTOCOL_ERROR);
        } else if (message[0] == MESSAGE_ID_SOURCE_AUDIO) {
            this->on_source_chunk(message, rx_us);
        }
        return;
    }
    ClientMessage parsed = parse_client_message(as_text(message.subspan(1)), rx_us);
    // A pairing message may carry a secret, which no buffer keeps once parsed, whether or not it
    // passed validation; so may a message whose type could not be read
    const MalformedMessage* malformed = std::get_if<MalformedMessage>(&parsed);
    if (std::holds_alternative<ClientPairing>(parsed) ||
        (malformed != nullptr && (malformed->type.empty() || is_pairing_type(malformed->type)))) {
        this->wipe_scratch();
        this->reassembler_.wipe();
    }
    // connection.md "Re-handshake": message 2 answers the server's message 1, and travels under
    // the keys before it
    const OtherMessage* other = std::get_if<OtherMessage>(&parsed);
    if (other != nullptr && other->type == "noise/handshake" &&
        this->rekey_phase_ == sched::Phase::AWAIT_MSG2) {
        this->on_rekey_message_2(as_text(message.subspan(1)));
        return;
    }
    if (this->phase_ == SessionPhase::ACTIVE) {
        this->on_active_message(parsed);
        return;
    }
    if (ClientGoodbye* goodbye = std::get_if<ClientGoodbye>(&parsed)) {
        // messaging.md "client/goodbye": the server initiates the disconnect
        this->goodbye_ = *goodbye;
        this->finish(SessionEnd::GOODBYE);
        return;
    }
    ClientHello* hello = std::get_if<ClientHello>(&parsed);
    if (hello == nullptr || this->phase_ != SessionPhase::AWAIT_CLIENT_HELLO) {
        this->finish(SessionEnd::PROTOCOL_ERROR);
        return;
    }
    this->client_hello_ = std::move(*hello);
    if (this->client_hello_->player_v1_support) {
        this->player_.set_support(*this->client_hello_->player_v1_support);
    }
    if (this->client_hello_->visualizer_v1_support) {
        this->visualizer_.set_support(*this->client_hello_->visualizer_v1_support);
    }
    this->phase_ = SessionPhase::READY_FOR_ACTIVATION;
}

// connection.md "Re-handshake"; every failure here is silent (connection.md "Failure Handling"),
// and a miss fails it: the Sentinel Fallback applies to the initial handshake only
void Session::on_rekey_message_2(std::string_view json) {
    const std::optional<std::vector<uint8_t>> message_2 = parse_noise_handshake(json);
    std::optional<TransportCiphers> ciphers;
    if (message_2 && this->initiator_) {
        const std::optional<std::vector<uint8_t>> payload_2 =
            this->initiator_->read_message_2(*message_2);
        if (payload_2 && is_message_2_payload(*payload_2)) {
            ciphers = this->initiator_->split();
        }
    }
    this->initiator_.reset();
    if (!ciphers) {
        this->finish(SessionEnd::HANDSHAKE_FAILED);
        return;
    }
    // messaging.md "noise/handshake": each side's next binary message uses the new keys
    this->ciphers_ = std::move(ciphers);
    this->psk_match_ = {this->referenced_, std::nullopt};
    // pairing.md "Pairing index": the count restarts at every Noise handshake
    this->pairing_written_ = 0;
    this->rekeyed_ = true;
    this->rekey_phase_ = sched::Phase::AWAIT_ACTIVATE;
}

// messaging.md "Communication": the ignore rules apply from here on
void Session::on_active_message(const ClientMessage& message) {
    if (const ClientTime* time = std::get_if<ClientTime>(&message)) {
        if (!this->queues_.push_time(*time)) {
            this->finish(SessionEnd::QUEUE_OVERFLOW);
        }
        return;
    }
    if (const ClientGoodbye* goodbye = std::get_if<ClientGoodbye>(&message)) {
        this->goodbye_ = *goodbye;
        this->finish(SessionEnd::GOODBYE);
        return;
    }
    if (const ClientState* state = std::get_if<ClientState>(&message)) {
        this->on_client_state(*state);
        return;
    }
    if (const ClientCommand* command = std::get_if<ClientCommand>(&message)) {
        this->on_client_command(*command);
        return;
    }
    if (std::holds_alternative<ClientLeave>(message)) {
        this->leave_ = true;
        return;
    }
    if (const ClientStreamStart* start = std::get_if<ClientStreamStart>(&message)) {
        this->on_stream_start(*start);
        return;
    }
    if (std::holds_alternative<ClientStreamEnd>(message)) {
        this->on_stream_end();
        return;
    }
    if (const ClientPairing* pairing = std::get_if<ClientPairing>(&message)) {
        this->pairing_messages_.push_back(*pairing);
        return;
    }
    const OtherMessage* other = std::get_if<OtherMessage>(&message);
    // A malformed envelope, a recognized message that fails validation, and a second
    // client/hello are protocol errors
    if (other == nullptr || rule_for_unhandled(other->type) == UnhandledRule::CLOSE) {
        this->finish(SessionEnd::PROTOCOL_ERROR);
    }
}

void Session::on_client_state(const ClientState& state) {
    // messaging.md "server/activate": an active role's object is validated, an inactive one's
    // ignored
    // roles/artwork/v1.md "client/state artwork object": more than 4 channels is a protocol
    // error; an artwork object invalid otherwise is ignored, the channels declared before kept.
    // roles/visualizer/v1.md "client/state visualizer object": 'spectrum' without a spectrum
    // object is a protocol error.
    if ((state.player_malformed && this->player_.role_written()) ||
        (state.visualizer_malformed && this->visualizer_.role_written()) ||
        (state.artwork_too_many_channels && this->artwork_.role_written()) ||
        (state.source_malformed && this->source_role_.role_written())) {
        this->finish(SessionEnd::PROTOCOL_ERROR);
        return;
    }
    // A report of `available: true` that is the first or follows `false` is a join for the
    // client's stream roles
    const bool turned_available = state.available && !this->available_;
    this->available_ = state.available;
    this->artwork_.on_client_state(state);
    this->source_role_.on_client_state(state);
    // Queued by queue_state_marker(), behind what the owner queues in reaction (a group/update
    // before the stream/end it leads to). The most recent kind-setting event wins, and every event
    // here is a JOIN.
    const std::optional<sched::TriggerKind> player = this->player_.on_client_state(state);
    const std::optional<sched::TriggerKind> visualizer = this->visualizer_.on_client_state(state);
    this->state_marker_kind_ =
        turned_available ? sched::TriggerKind::JOIN : (player ? player : visualizer);
    this->state_changed_ = true;
}

void Session::on_stream_start(const ClientStreamStart& start) {
    const bool supported = source_input_format(start.format).has_value();
    if (this->source_role_.on_stream_start(start, this->settings_.hello.source_codecs, supported) ==
        SourceStream::Verdict::PROTOCOL_ERROR) {
        this->finish(SessionEnd::PROTOCOL_ERROR);
        return;
    }
    if (!supported) {
        this->queue_marker(std::nullopt);
        return;
    }
    this->push_source_start();
}

void Session::push_source_start() {
    if (this->source_feed_ == nullptr) {
        return;
    }
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access): only called with a stream open
    const ClientStreamStart& start = *this->source_role_.stream();
    SourceItem item;
    item.kind = SourceItem::Kind::START;
    item.connection = this->source_connection_;
    item.stream = this->source_role_.stream_number();
    item.format = start.format;
    if (!this->source_feed_->push(item, start.codec_header)) {
        this->source_role_.fail();
        this->queue_marker(std::nullopt);
    }
}

void Session::on_stream_end() {
    if (!this->source_role_.stream()) {
        return;
    }
    if (this->source_feed_ != nullptr) {
        SourceItem item;
        item.kind = SourceItem::Kind::END;
        item.connection = this->source_connection_;
        item.stream = this->source_role_.stream_number();
        // A refused end leaves the run to the next stream's gap or the input's release
        (void)this->source_feed_->push(item, {});
    }
    this->source_role_.on_stream_end();
    this->queue_marker(std::nullopt);
}

void Session::on_source_chunk(std::span<const uint8_t> message, int64_t rx_us) {
    const std::optional<SourceChunk> chunk = parse_source_chunk(message);
    const SourceStream::Verdict verdict = this->source_role_.on_chunk();
    if (!chunk || verdict == SourceStream::Verdict::PROTOCOL_ERROR) {
        this->finish(SessionEnd::PROTOCOL_ERROR);
        return;
    }
    if (verdict == SourceStream::Verdict::DISCARD || this->source_feed_ == nullptr) {
        return;
    }
    // A chunk the feed can never hold fails the stream; one refused for room is a gap
    if (chunk->audio.size() > this->source_feed_->capacity()) {
        this->source_role_.fail();
        this->queue_marker(std::nullopt);
        return;
    }
    SourceItem item;
    item.kind = SourceItem::Kind::CHUNK;
    item.connection = this->source_connection_;
    item.stream = this->source_role_.stream_number();
    item.capture_us = chunk->capture_us;
    item.rx_us = rx_us;
    (void)this->source_feed_->push(item, chunk->audio);
}

void Session::set_source_route(SourceFeed* feed, uint64_t connection) {
    const bool moved = feed != this->source_feed_;
    this->source_feed_ = feed;
    this->source_connection_ = connection;
    if (moved && feed != nullptr && this->source_role_.stream() && !this->source_role_.failed()) {
        this->source_role_.on_rerouted();
        this->push_source_start();
    }
    if (this->source_role_.set_routed(feed != nullptr)) {
        this->queue_marker(std::nullopt);
    }
}

void Session::fail_source(uint64_t stream) {
    if (this->source_role_.stream() && this->source_role_.stream_number() == stream) {
        this->source_role_.fail();
        this->queue_marker(std::nullopt);
    }
}

void Session::on_client_command(const ClientCommand& command) {
    // messaging.md "server/activate": an active role's object is validated, an inactive one's
    // ignored (controller/v1.md: only valid while the controller role is active)
    if (!this->role_active(CONTROLLER_ROLE)) {
        return;
    }
    if (command.controller_malformed) {
        this->finish(SessionEnd::PROTOCOL_ERROR);
        return;
    }
    if (command.controller) {
        this->commands_.push_back(*command.controller);
    }
}

const HandshakeHash& Session::handshake_hash() const {
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access): set from AWAIT_CLIENT_HELLO on
    return this->ciphers_->handshake_hash();
}

CipherSuite Session::suite() const {
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access): set from AWAIT_NOISE_MSG2 on
    return this->client_init_->suite;
}

bool Session::role_active(std::string_view role) const {
    return std::find(this->active_roles_.begin(), this->active_roles_.end(), role) !=
           this->active_roles_.end();
}

bool Session::role_ahead(std::string_view role) const {
    const std::vector<std::string>& roles =
        this->queued_.empty() ? this->active_roles_ : this->queued_.front().active_roles;
    return std::find(roles.begin(), roles.end(), role) != roles.end();
}

bool Session::role_queued(std::string_view role) const {
    const std::vector<std::string>& roles = this->next_roles();
    return std::find(roles.begin(), roles.end(), role) != roles.end();
}

void Session::queue_state_marker() {
    this->queue_marker(std::exchange(this->state_marker_kind_, std::nullopt));
}

std::optional<OutboundPlaintext> Session::prepare_activation(const ServerActivate& activation,
                                                             sched::RoleSet& roles) {
    const std::optional<std::string> json = build_server_activate(activation);
    if (!json) {
        this->finish(SessionEnd::ENCODE_FAILED);
        return std::nullopt;
    }
    // messaging.md "server/activate": active_roles persists across activations that omit it
    std::vector<std::string> active_roles =
        this->queued_.empty() ? this->active_roles_ : this->queued_.back().active_roles;
    if (activation.active_roles) {
        active_roles = *activation.active_roles;
    }
    roles = {};
    if (std::find(active_roles.begin(), active_roles.end(), PLAYER_ROLE) != active_roles.end()) {
        roles = roles.with(sched::StreamRole::PLAYER);
    }
    if (std::find(active_roles.begin(), active_roles.end(), VISUALIZER_ROLE) !=
        active_roles.end()) {
        roles = roles.with(sched::StreamRole::VISUALIZER);
    }
    OutboundPlaintext message = json_plaintext(*json);
    this->queued_.push({std::move(active_roles), activation.pairing.has_value()});
    return message;
}

void Session::activate(const ServerActivate& activation) {
    if (this->phase_ == SessionPhase::CLOSED) {
        return;
    }
    sched::RoleSet roles;
    std::optional<OutboundPlaintext> message = this->prepare_activation(activation, roles);
    if (!message) {
        return;
    }
    // Every activation is queued behind a marker
    if (!this->queues_.push_marker() ||
        !this->queues_.push_activation(std::move(*message), roles)) {
        this->finish(SessionEnd::QUEUE_OVERFLOW);
        return;
    }
    if (activation.pairing) {
        ++this->pairing_queued_;
    }
    if (this->phase_ == SessionPhase::READY_FOR_ACTIVATION) {
        this->phase_ = SessionPhase::ACTIVATING;
    }
}

void Session::rekey(const HandshakePsk& psk, const ServerActivate& activation) {
    if (this->phase_ == SessionPhase::CLOSED) {
        return;
    }
    sched::RoleSet roles;
    std::optional<OutboundPlaintext> message = this->prepare_activation(activation, roles);
    if (!message) {
        return;
    }
    // The marker ahead ends what the frozen activation removes before message 1
    if (!this->queues_.push_marker() || !this->queues_.push_rekey(std::move(*message), roles)) {
        this->finish(SessionEnd::QUEUE_OVERFLOW);
        return;
    }
    this->rekeys_.push(psk);
    // A re-handshake is a match, so the fallback no longer applies
    this->next_psk_match_ = {psk.category, std::nullopt};
    // pairing.md "Pairing index": the frozen activation is the first of the new handshake
    this->pairing_queued_ = activation.pairing ? 1 : 0;
}

void Session::queue_marker(std::optional<sched::TriggerKind> kind) {
    if (this->phase_ == SessionPhase::CLOSED) {
        return;
    }
    this->set_pending_kind(kind);
    if (!this->queues_.push_marker()) {
        this->finish(SessionEnd::QUEUE_OVERFLOW);
    }
}

void Session::set_pending_kind(std::optional<sched::TriggerKind> kind) {
    // The most recent kind-setting event wins
    if (kind) {
        this->player_.set_pending_kind(*kind);
        this->visualizer_.set_pending_kind(*kind);
    }
}

void Session::queue_json_message(const std::string& json) {
    if (this->phase_ != SessionPhase::CLOSED) {
        this->queue_json(json);
    }
}

bool Session::has_message() {
    const int64_t now = now_us();
    this->prepare(now);
    return sched::select_next(this->snapshot(now)) != sched::Choice::NONE;
}

Session::Poll Session::poll() {
    const int64_t now = now_us();
    this->prepare(now);
    const sched::PumpSnapshot snapshot = this->snapshot(now);
    Poll poll;
    poll.ready = sched::select_next(snapshot) != sched::Choice::NONE;
    if (this->phase_ != SessionPhase::ACTIVE) {
        return poll;
    }
    if (poll.ready) {
        poll.wake_us = now;
        return poll;
    }
    const int64_t wake = sched::next_wake(snapshot);
    if (wake != sched::NEVER) {
        poll.wake_us = wake;
    }
    return poll;
}

void Session::prepare(int64_t now_us) {
    if (this->phase_ != SessionPhase::ACTIVE) {
        return;
    }
    const bool player_watch =
        this->player_.prepare(this->source_, this->queues_.markers_queued(), now_us);
    const bool visualizer_watch =
        this->visualizer_.prepare(this->source_, this->queues_.markers_queued(), now_us);
    if (player_watch || visualizer_watch) {
        // The drain watch's marker
        this->queue_marker(std::nullopt);
    }
    this->artwork_step_ =
        this->artwork_.next_step(this->artwork_view_, this->role_ahead(ARTWORK_ROLE), now_us);
    this->artwork_.note_ready(this->artwork_step_.kind != ArtworkStep::Kind::NONE, now_us);
}

sched::PumpSnapshot Session::snapshot(int64_t now_us) const {
    sched::PumpSnapshot snapshot = this->queues_.snapshot();
    snapshot.phase = this->rekey_phase_;
    if (this->phase_ == SessionPhase::ACTIVE) {
        snapshot.player =
            this->player_.candidate(this->source_, this->settings_.max_fragment_plaintext, now_us);
        snapshot.visualizer = this->visualizer_.candidate(this->source_, now_us);
        snapshot.drain_watch_us =
            std::min(this->player_.drain_watch_us(this->source_, this->queues_.markers_queued()),
                     this->visualizer_.drain_watch_us(this->queues_.markers_queued()));
        snapshot.artwork_ready = this->artwork_step_.kind != ArtworkStep::Kind::NONE;
        snapshot.artwork_aged = snapshot.artwork_ready &&
                                this->artwork_.aged(sched::timing_defaults::ARTWORK_AGE_US, now_us);
        snapshot.artwork_wake_us = this->artwork_step_.wake_us;
    }
    return snapshot;
}

std::optional<OutboundPlaintext> Session::take_control(int64_t now_us, int64_t& stamp_us) {
    if (this->queues_.snapshot().fragment_in_progress) {
        OutboundPlaintext fragment =
            this->queues_.take_control(this->settings_.max_fragment_plaintext);
        stamp_us = ::sendspin::server::now_us();
        return fragment;
    }
    switch (this->queues_.head_kind()) {
        case ControlKind::MARKER:
            return this->take_marker(now_us, stamp_us);
        case ControlKind::ACTIVATION: {
            const sched::RoleSet roles = this->queues_.head_roles();
            OutboundPlaintext activation =
                this->queues_.take_control(this->settings_.max_fragment_plaintext);
            stamp_us = ::sendspin::server::now_us();
            this->on_activation_taken(roles);
            return activation;
        }
        case ControlKind::REKEY:
            return this->take_rekey(stamp_us);
        case ControlKind::MESSAGE:
            break;
    }
    OutboundPlaintext message = this->queues_.take_control(this->settings_.max_fragment_plaintext);
    stamp_us = ::sendspin::server::now_us();
    return message;
}

std::optional<OutboundPlaintext> Session::take_marker(int64_t now_us, int64_t& stamp_us) {
    const sched::RoleSet ahead = this->queues_.roles_ahead(this->roles_written_);
    const sched::Want player = this->player_.want(this->source_, ahead, now_us);
    const sched::Want visualizer = this->visualizer_.want(this->source_, ahead, now_us);
    const sched::MarkerEmission emission =
        sched::marker_step({this->player_.sent(), this->visualizer_.sent()}, {player, visualizer},
                           {this->player_.preconditions(), this->visualizer_.preconditions()});
    if (emission.action == sched::StreamAction::NONE) {
        // The artwork stream's end is owed here too
        if (const std::optional<ArtworkStep> end =
                this->artwork_.end_step(this->role_ahead(ARTWORK_ROLE))) {
            stamp_us = ::sendspin::server::now_us();
            return this->artwork_.commit(*end, stamp_us, this->settings_.max_fragment_plaintext);
        }
        // roles/source/v1.md "Source command semantics": built here, its preconditions checked
        // as it is written
        if (const std::optional<SourceCommand> command =
                this->source_role_.take_command(this->role_ahead(SOURCE_ROLE))) {
            stamp_us = ::sendspin::server::now_us();
            return json_plaintext(build_source_command(*command));
        }
        // The pending trigger kind is cleared when a marker pops
        this->queues_.pop_marker();
        this->player_.clear_pending_kind();
        this->visualizer_.clear_pending_kind();
        return std::nullopt;
    }
    // A marker stays at the head until it has nothing left to emit; its one message lists every
    // role whose first matching row has its action
    const bool to_player = emission.roles.contains(sched::StreamRole::PLAYER);
    const bool to_visualizer = emission.roles.contains(sched::StreamRole::VISUALIZER);
    std::array<std::string_view, sched::STREAM_ROLE_COUNT> names{};
    size_t named = 0;
    if (to_player) {
        names[named++] = PLAYER_STREAM_ROLE;
    }
    if (to_visualizer) {
        names[named++] = VISUALIZER_STREAM_ROLE;
    }
    const std::span<const std::string_view> roles(names.data(), named);
    stamp_us = ::sendspin::server::now_us();
    std::string json;
    switch (emission.action) {
        case sched::StreamAction::END:
            json = build_stream_end(roles);
            break;
        case sched::StreamAction::CLEAR:
            json = build_stream_clear(stamp_us, roles);
            break;
        case sched::StreamAction::START: {
            StreamStart start;
            if (to_player) {
                start = player_start(player.format.audio);
            }
            if (to_visualizer) {
                start.visualizer = this->visualizer_.config_for(visualizer);
            }
            json = build_stream_start(stamp_us, start);
            break;
        }
        case sched::StreamAction::NONE:
            break;
    }
    if (to_player) {
        this->player_.commit_marker(this->source_, emission.action, player, stamp_us);
    }
    if (to_visualizer) {
        this->visualizer_.commit_marker(this->source_, emission.action, visualizer, stamp_us);
    }
    return json_plaintext(json);
}

void Session::on_activation_taken(sched::RoleSet roles) {
    // messaging.md "Communication": the ignore rules apply once the activation is sent
    if (this->phase_ == SessionPhase::ACTIVATING) {
        this->phase_ = SessionPhase::ACTIVE;
    }
    this->roles_written_ = roles;
    this->active_roles_ = std::move(this->queued_.front().active_roles);
    if (this->queued_.front().pairing) {
        ++this->pairing_written_;
    }
    this->queued_.pop_front();
    this->artwork_.on_activation_written(this->role_active(ARTWORK_ROLE));
    this->source_role_.on_activation_written(this->role_active(SOURCE_ROLE));
    const std::optional<sched::TriggerKind> player =
        this->player_.on_activation_written(roles.contains(sched::StreamRole::PLAYER));
    const std::optional<sched::TriggerKind> visualizer =
        this->visualizer_.on_activation_written(roles.contains(sched::StreamRole::VISUALIZER));
    // A marker follows
    this->queue_marker(player ? player : visualizer);
}

// connection.md "Re-handshake": message 1 travels as an encrypted noise/handshake under the
// current keys, with the prior handshake's `h` as the prologue; client/init and server/init are
// not sent again
std::optional<OutboundPlaintext> Session::take_rekey(int64_t& stamp_us) {
    this->rekey_roles_ = this->queues_.head_roles();
    this->rekey_activation_ = this->queues_.take_rekey();
    const HandshakePsk psk = std::move(this->rekeys_.front());
    this->rekeys_.pop_front();
    // NOLINTBEGIN(bugprone-unchecked-optional-access): the client/init is set from
    // AWAIT_NOISE_MSG2 on, and the ciphers from AWAIT_CLIENT_HELLO on, before any rekey is queued
    const ClientInit& init = *this->client_init_;
    const HandshakeHash& h = this->ciphers_->handshake_hash();
    // NOLINTEND(bugprone-unchecked-optional-access)
    this->initiator_ =
        NoiseInitiator::create(init.suite, this->identity_, init.client_key, h, psk.psk);
    this->referenced_ = psk.category;
    const std::string payload_1 = build_message_1_payload(psk_id_for(psk.psk), psk.category);
    std::optional<std::vector<uint8_t>> message_1;
    if (this->initiator_) {
        message_1 = this->initiator_->write_message_1(as_bytes(payload_1));
    }
    if (!message_1) {
        this->finish(SessionEnd::HANDSHAKE_FAILED);
        return std::nullopt;
    }
    OutboundPlaintext message = json_plaintext(build_noise_handshake(*message_1));
    stamp_us = ::sendspin::server::now_us();
    // Nothing is written until message 2 is read
    this->rekey_phase_ = sched::Phase::AWAIT_MSG2;
    this->deadline_us_ = stamp_us + this->settings_.step_timeout_us;
    return message;
}

std::optional<OutboundMessage> Session::take_message() {
    OutboundPlaintext message;
    int64_t stamp_us = 0;
    size_t audio_bytes = 0;
    bool time_reply = false;
    TimeReplyTrace debug_trace;  // TEMPORARY DEBUG
    for (bool taken = false; !taken;) {
        const int64_t now = now_us();
        this->prepare(now);
        const sched::PumpSnapshot snapshot = this->snapshot(now);
        switch (sched::select_next(snapshot)) {
            case sched::Choice::TIME: {
                const ClientTime request = this->queues_.take_time();
                std::optional<PreviousTimeReply> previous;
                if (request.interleaved && this->last_time_reply_ &&
                    this->last_time_reply_->sent_us != 0) {
                    previous = PreviousTimeReply{this->last_time_reply_->client_transmitted,
                                                 this->last_time_reply_->sent_us};
                }
                stamp_us = ::sendspin::server::now_us();
                message = json_plaintext(build_server_time(request, stamp_us, previous));
                if (request.interleaved) {
                    this->last_time_reply_ = LastTimeReply{request.client_transmitted, stamp_us, 0};
                } else {
                    this->last_time_reply_.reset();
                }
                debug_trace.stamp_us = stamp_us;
                debug_trace.request_rx_us = request.rx_us;
                debug_trace.built_us = ::sendspin::server::now_us();
                time_reply = true;
                taken = true;
                break;
            }
            case sched::Choice::FRAGMENT:
            case sched::Choice::CONTROL: {
                std::optional<OutboundPlaintext> control = this->take_control(now, stamp_us);
                if (control) {
                    message = std::move(*control);
                    taken = true;
                }
                break;
            }
            case sched::Choice::PLAYER: {
                if (this->player_.announce_due(this->source_)) {
                    stamp_us = ::sendspin::server::now_us();
                    std::optional<OutboundPlaintext> announce =
                        this->player_.commit_announce(this->source_, stamp_us);
                    if (!announce) {
                        return std::nullopt;
                    }
                    message = std::move(*announce);
                    taken = true;
                    break;
                }
                PackedChunk chunk = this->player_.take_chunk(
                    this->source_, this->settings_.max_fragment_plaintext, now);
                if (chunk.plaintext.bytes.empty()) {
                    return std::nullopt;
                }
                message = std::move(chunk.plaintext);
                audio_bytes = chunk.audio_bytes;
                // messaging.md "Transmit timestamps": send_ahead from the stamp taken last
                stamp_us = ::sendspin::server::now_us();
                sched::write_chunk_header(std::span<uint8_t, sched::CHUNK_HEADER_BYTES>(
                                              message.bytes.data(), sched::CHUNK_HEADER_BYTES),
                                          chunk.ts_us,
                                          sched::send_ahead_field(chunk.ts_us, stamp_us));
                taken = true;
                break;
            }
            case sched::Choice::REKEY_ACTIVATE: {
                // The frozen activation goes first under the new keys
                // NOLINTNEXTLINE(bugprone-unchecked-optional-access): set in AWAIT_ACTIVATE
                message = std::move(*this->rekey_activation_);
                this->rekey_activation_.reset();
                this->rekey_phase_ = sched::Phase::NORMAL;
                stamp_us = ::sendspin::server::now_us();
                this->on_activation_taken(this->rekey_roles_);
                taken = true;
                break;
            }
            case sched::Choice::ARTWORK:
                stamp_us = ::sendspin::server::now_us();
                message = this->artwork_.commit(this->artwork_step_, stamp_us,
                                                this->settings_.max_fragment_plaintext);
                this->artwork_.on_written(snapshot.artwork_aged, stamp_us);
                taken = true;
                break;
            case sched::Choice::VISUALIZER: {
                if (this->visualizer_.announce_due(this->source_)) {
                    stamp_us = ::sendspin::server::now_us();
                    std::optional<OutboundPlaintext> announce =
                        this->visualizer_.commit_announce(this->source_, stamp_us);
                    if (!announce) {
                        return std::nullopt;
                    }
                    message = std::move(*announce);
                    taken = true;
                    break;
                }
                message = this->visualizer_.take_message(this->source_);
                if (message.bytes.empty()) {
                    return std::nullopt;
                }
                stamp_us = ::sendspin::server::now_us();
                taken = true;
                break;
            }
            case sched::Choice::NONE:
                return std::nullopt;
        }
    }
    // Handed to the trace after encryption, so that nothing slower than a copy goes between the
    // stamp and the encryption
    std::vector<uint8_t> traced;
    if (this->trace_ != nullptr) {
        traced = message.bytes;
    }
    if (message.kind == MessageKind::BINARY &&
        (!this->ciphers_ || !this->ciphers_->encrypt(message.bytes))) {
        // The peer can authenticate nothing sent after a skipped nonce
        this->finish(SessionEnd::ENCRYPT_FAILED);
        return std::nullopt;
    }
    debug_trace.encrypted_us = ::sendspin::server::now_us();
    if (this->trace_ != nullptr) {
        this->trace_->on_outbound(message.kind, traced, stamp_us, audio_bytes);
    }
    return OutboundMessage{message.kind, std::move(message.bytes), time_reply, debug_trace};
}

bool Session::queue_text(const std::string& text) {
    return this->queue({MessageKind::TEXT, {text.begin(), text.end()}});
}

bool Session::queue_json(const std::string& json) {
    return this->queue(json_plaintext(json));
}

bool Session::queue(OutboundPlaintext message) {
    if (!this->queues_.push_control(std::move(message))) {
        this->finish(SessionEnd::QUEUE_OVERFLOW);
        return false;
    }
    return true;
}

void Session::close_by_host() {
    if (!this->end_) {
        this->finish(SessionEnd::CLOSED_BY_HOST);
    }
}

void Session::refuse() {
    this->finish(SessionEnd::REFUSED);
}

void Session::finish(SessionEnd end) {
    if (end != SessionEnd::INIT_REJECTED) {
        this->queues_.clear();
    }
    this->end_ = end;
    this->phase_ = SessionPhase::CLOSED;
    // A pairing message's plaintext may be left: a fragment pending, or a message AES-GCM decrypted
    // in place before its tag check failed
    this->wipe_scratch();
    this->reassembler_.wipe();
    this->initiator_.reset();
    this->rekeys_.clear();
    // A re-handshake that completed has no frozen activation left to write
    this->rekey_activation_.reset();
    this->rekey_phase_ = sched::Phase::NORMAL;
}

}  // namespace sendspin::server
