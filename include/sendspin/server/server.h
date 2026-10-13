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

/// @file server.h
/// @brief SendspinServer, the library's entry point, and the listener and persistence provider
/// the host supplies

#pragma once

#include "sendspin/server/config.h"
#include "sendspin/server/persistence_keys.h"
#include "sendspin/server/types.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace sendspin::server {

/// @brief Key-value blob storage the host provides; the keys and sizes are in
/// persistence_keys.h. Called on the dispatch thread (SendspinServer).
class PersistenceProvider {
public:
    virtual ~PersistenceProvider() = default;

    /// @return The bytes stored under key, or nullopt when there are none
    virtual std::optional<std::vector<uint8_t>> load_blob(const std::string& /*key*/) {
        return std::nullopt;
    }

    /// @return true when the write was accepted: stored, or queued until the next commit()
    virtual bool save_blob(const std::string& /*key*/, const uint8_t* /*data*/, size_t /*len*/) {
        return false;
    }

    /// @brief Makes every accepted write durable. A provider that writes through in save_blob()
    /// keeps the default.
    /// @return true when the accepted writes are durable
    virtual bool commit() {
        return true;
    }
};

/// @brief The host's view of the server. Called on the dispatch thread (SendspinServer), in the
/// order the events happened.
class ServerListener {
public:
    virtual ~ServerListener() = default;

    /// @brief Browsing found a client instance the server does not know in this run: client_id
    /// is empty. The server dials it, unless that client is connected already.
    virtual void on_client_seen(const ClientInfo& /*client*/) {}

    /// @brief The client took the connection: its first client/state followed the initial
    /// server/activate, or that activation carried no roles, so it need not send one (spec
    /// messaging.md "client/state"). A client that refuses the activation is reported with
    /// on_error() instead. A second connection of a connected client is not reported.
    virtual void on_client_connected(const ClientInfo& /*client*/) {}

    /// @brief A client reported by on_client_connected() went away
    virtual void on_client_disconnected(const ClientInfo& /*client*/,
                                        const ClientDisconnect& /*disconnect*/) {}

    /// @brief A connected client enables unpaired access but is not approved, so it is held with
    /// no activities or roles until SendspinServer::approve() (spec pairing.md "Unpaired
    /// Access"). Present it as unapproved, distinct from approved clients.
    virtual void on_approval_needed(const ClientInfo& /*client*/) {}

    /// @brief A connected client does not enable unpaired access, so it is held with no
    /// activities or roles: it plays only once paired (SendspinServer::begin_pairing()), or once
    /// the user enables unpaired access on the device (spec pairing.md "Unpaired Access")
    virtual void on_pairing_required(const ClientInfo& /*client*/) {}

    /// @brief The client holds the pairing back until its operator acts on it, such as a button
    /// press that opens its pairing window (spec pairing.md "client/pair-pending"). message is the
    /// device's own text, at most 200 characters, possibly empty: show it as plain text attributed
    /// to the device, never interpreting markup or links in it.
    virtual void on_pairing_pending(const ClientInfo& /*client*/, std::string_view /*message*/) {}

    /// @brief The pairing needs the operator's secret in format: a token for
    /// PairMethod::PAIRING_PSK (SendspinServer::submit_pairing_token()), the code the client shows,
    /// speaks or has printed otherwise (SendspinServer::submit_pairing_code()). A dynamic code is
    /// asked for again in each round, after a wrong one, and a code of the wrong shape is asked
    /// for again.
    virtual void on_pairing_code_needed(const ClientInfo& /*client*/, PairMethod /*method*/,
                                        CodeFormat /*format*/) {}

    /// @brief A pairing begin_pairing() or submit_pairing_token() started ended; once per pairing
    virtual void on_pairing_result(const ClientInfo& /*client*/, PairingResult /*result*/) {}

    /// @brief A connected client could not use the long-term PSK the server holds for it (it lost
    /// its pairing record, say) and completed the handshake under the Sentinel instead (spec
    /// connection.md "Sentinel Fallback"). It is held with no activities or roles until it is
    /// paired again, which replaces the record; the server never removes the record for this.
    /// ClientInfo::credential_mismatch is set.
    virtual void on_credential_mismatch(const ClientInfo& /*client*/) {}

    /// @brief A connected client's client/hello lists a role version or pairing method this
    /// server does not implement, identifier, so the client speaks a newer revision of the spec
    /// and the server may need updating (spec README "Detecting Outdated Servers", pairing.md
    /// "client/hello pair-method descriptor"). Once per identifier per connection, for at most the
    /// first 8 distinct identifiers.
    virtual void on_newer_client_detected(const ClientInfo& /*client*/,
                                          std::string_view /*identifier*/) {}

    /// @brief A controller in group sent a command the group offers (SendspinServer::
    /// set_controller_caps()); the host carries it out. The server never acts on the group's
    /// input for a controller.
    virtual void on_transport_command(GroupId /*group*/, TransportCommand /*command*/,
                                      const CommandArgs& /*args*/) {}

    virtual void on_error(const ServerError& /*error*/) {}

    /// @brief A group's playback state, members or name changed, or create_group() created it. A
    /// move of a client that is not connected into the group, which keeps its place there for its
    /// rejoin grace, is reported too, though GroupInfo lists connected members only. A server stop
    /// that waits for the host to acknowledge it with GroupInput::stop() is reported here as
    /// PlaybackState::STOPPED.
    virtual void on_group_changed(const GroupInfo& /*group*/) {}

    /// @brief The input, refused by a write() that came up short, admits at least one cell (20 ms)
    /// of samples again. Once per write() that came up short; not while draining or stop-latched.
    /// One already posted may arrive after the GroupInput is released.
    virtual void on_writable(GroupId /*group*/, InputId /*input*/) {}

    /// @brief One per GroupInput::drain() call, with the id that call returned and how the drain
    /// was resolved
    virtual void on_drained(GroupId /*group*/, InputId /*input*/, DrainId /*drain*/,
                            DrainOutcome /*outcome*/) {}

    /// @brief A connected client's source can stream (spec roles/source/v1.md): its source role is
    /// active, which needs pairing or ApprovalScope::PLAYBACK_AND_SOURCE, its client/state source
    /// object arrived and it reports itself available. Once per activation that adds the role, and
    /// again after the client was unavailable. A source SendspinServer::route_source() routed is
    /// asked to stream at once.
    virtual void on_source_ready(const ClientInfo& /*client*/) {}

    /// @brief A routed source opened its input stream into group's input, changed the open
    /// stream's format in place, or its open stream moved into group with a new route; format is
    /// what its client-stream/start announced (bit_depth 0 for Opus). A format the server does not
    /// decode is reported by on_error() (ErrorKind::SOURCE_FAILED) alone; one only the decoder or
    /// the input finds it cannot take is reported here first.
    virtual void on_source_started(const ClientInfo& /*client*/, GroupId /*group*/,
                                   const StreamFormat& /*format*/) {}

    /// @brief The stream on_source_started() reported stopped playing into group: the source
    /// ended it, its connection closed, or the route ended
    virtual void on_source_stopped(const ClientInfo& /*client*/, GroupId /*group*/) {}

    /// @brief A source with line sensing reported its signal (spec roles/source/v1.md
    /// "client/state source object"), a hint a host may route it by
    virtual void on_source_signal(const ClientInfo& /*client*/, SourceSignal /*signal*/) {}
};

class GroupInput;

/// @brief What SendspinServer::open_input() returns
struct OpenedInput {
    OpenInputResult result{OpenInputResult::NOT_RUNNING};
    /// Set with OpenInputResult::OK only
    std::shared_ptr<GroupInput> input;
};

/// @brief What SendspinServer::route_source() returns
struct RoutedSource {
    RouteSourceResult result{RouteSourceResult::NOT_RUNNING};
    /// Set with RouteSourceResult::OK only: the group's input the source plays into
    std::shared_ptr<GroupInput> input;
};

/// @brief One group's audio input: the host writes PCM into it and controls playback through it.
///
/// Threading: every method may be called from any thread (on ESP-IDF any task, a pthread or not),
/// including from a listener call, and returns without waiting, except wait_writable(). Each call
/// updates the input's state at once, in call order, so time_of(), latency_us(), state() and
/// pause() reflect every earlier call.
///
/// Releasing the input (destroying the last reference) and then opening another on the same group
/// at once is supported: the released input no longer holds the group.
///
/// A run begins with the first write() after the input opens, a stop() or a completed drain,
/// and ends with pause(), stop() or a completed drain; resume() begins the next run. Sample
/// positions count from 0 at a run's first write() and after flush(), and continue across
/// pause(), resume() and set_format().
class GroupInput {
public:
    /// @brief Releases the input: a pending drain resolves as DrainOutcome::STOPPED and the
    /// group's playback stops
    ~GroupInput();
    GroupInput(const GroupInput&) = delete;
    GroupInput& operator=(const GroupInput&) = delete;

    InputId id() const;
    GroupId group() const;

    /// @brief Copies up to frames frames of PCM in the input's format. Never blocks or allocates;
    /// real-time safe. Takes as many as the input admits, up to about the group's lead plus its
    /// write-ahead past the time now playing; state says why it took fewer. A new input admits
    /// nothing (InputState::FULL) until the server has attached it to its group. A live input's
    /// writer drops what write() refuses and never resends what a stall held back. A live input
    /// may also take fewer with InputState::OPEN, cutting audio to bring a raised latency back
    /// down (InputKind::LIVE); its writer drops the rest the same way.
    WriteResult write(const void* pcm, size_t frames);

    /// @brief The input's state now: what write() would report for a cell (20 ms) of samples.
    /// Shows a stop by the server (InputState::STOP_LATCHED) or a closed input without a write.
    InputState state() const;

    /// @brief Waits until write() would take at least one cell (20 ms) of samples, for at most
    /// timeout_us (0 checks without waiting). A flush(), drain(), pause(), resume(), stop() or
    /// set_format() ends the wait: one another thread makes while it waits, or one made since the
    /// last write() or wait_writable() returned, so a writer that waits after a short write misses
    /// none. The server closing the input or stopping its playback ends it too.
    /// Not for a live input's writer, which writes as its samples arrive.
    /// @return state() as the wait ends: InputState::OPEN when writable, FULL at the timeout or
    /// when another call ended the wait, and at once DRAINING, STOP_LATCHED or CLOSED
    InputState wait_writable(int64_t timeout_us);

    /// @brief For a seek or a track jump: discards what was written and not yet played, players
    /// clear their buffers, and the next write() continues the run from position 0 with a fresh
    /// lead. Cancels a pending drain (DrainOutcome::FLUSHED).
    void flush();

    /// @brief Ends playback once what was written has played out; write() takes nothing until
    /// on_drained()
    /// @return the id on_drained() reports this call's outcome with
    DrainId drain();

    /// @brief Ends the run, keeping what was written from the paused position on. A live input
    /// (InputKind::LIVE) keeps taking every write while paused and keeps only the newest samples.
    /// @return the paused position: the sample playing now, the same on every player
    uint64_t pause();

    /// @brief After pause(): begins a new run from the paused position with a fresh lead; a live
    /// input's from the next sample written, dropping what was written while paused
    void resume();

    /// @brief Ends the run with no position to resume from and discards what was written. Also
    /// acknowledges a stop by the server (InputState::STOP_LATCHED).
    void stop();

    /// @brief Changes the format of what write() takes from the next sample on. During a run
    /// (playing, paused, or after a flush()), it continues the run gaplessly on the same timeline:
    /// each player keeps its stream and format, the server resampling and mixing the new input
    /// to it; otherwise it is the next run's format. A format change for gapless tracks, say.
    /// It may allocate a larger buffer, so it is not for a real-time thread.
    /// @return false for an invalid format, after drain() until on_drained(), or while what was
    /// written and not yet played spans 32 formats. That count includes the history kept behind
    /// the position playing, and does not shrink while paused; a host retries once more has
    /// played, or calls drain() and sets the format for the next run.
    bool set_format(const InputFormat& format);

    /// @brief When the sample at sample_pos plays, on SendspinServer::now_us()'s clock.
    /// Provisional, moving with the time now, for a position not yet anchored to the timeline: in
    /// a run's or a flush's first write() still to come, or after a pause, from the paused
    /// position on (a live input's from the next position to be written). Positions not yet sent
    /// move forward by the gap a late write() leaves, or, for a live input, by a joiner raising
    /// the group's lead. A position the server no longer holds may read later than it plays, by
    /// the gaps after it.
    int64_t time_of(uint64_t sample_pos) const;

    /// @brief time_of() the next position to be written, less the time now; it moves with
    /// time_of()
    int64_t latency_us() const;

private:
    friend class SendspinServer;
    class Impl;

    /// @brief SendspinServer::open_input() makes every input
    explicit GroupInput(std::unique_ptr<Impl> impl);

    std::unique_ptr<Impl> impl_;
};

/// @brief A Sendspin server.
///
/// Threading: every method may be called from any thread (on ESP-IDF any task, a pthread or not),
/// including from a listener call, except start() and the destructor, which must not be called
/// from a listener. Methods other than start() and the destructor return without waiting for the
/// server's threads. The listener, the persistence provider, the message trace and the log sink
/// are called on one dispatch thread, one call at a time and in order, so the server's own threads
/// never wait on the host.
class SendspinServer {
public:
    SendspinServer();
    /// @brief Stops the server and waits for its threads, including a listener call in progress,
    /// which may still call the server's methods and gets a stopped server's answers
    ~SendspinServer();
    SendspinServer(const SendspinServer&) = delete;
    SendspinServer& operator=(const SendspinServer&) = delete;

    /// @brief Checks config, loads the server key through provider, creating and saving one on
    /// first start, and starts the server's threads. Waits for the load. A stopped server may be
    /// started again.
    /// @param provider valid until the server is destroyed or started again
    /// @param listener nullptr for none, or one valid until the server is destroyed or started
    /// again
    StartResult start(const ServerConfig& config, PersistenceProvider* provider,
                      ServerListener* listener);

    /// @brief Closes every connection and stops the server's threads. Each connected client is
    /// reported with DisconnectReason::SERVER_STOPPED, after the events that came before the stop.
    void stop();

    /// @brief The clock of every timestamp the server sends, in microseconds: monotonic, and
    /// counting through system sleep
    int64_t now_us() const;

    /// @brief The server_id clients see (spec connection.md "Identities"); empty while stopped
    std::string server_id() const;

    /// @brief The port clients connect to (ServerConfig::listen_port); 0 while stopped
    uint16_t listen_port() const;

    /// @brief The connected clients, and those not connected that the server can dial
    /// (ClientInfo::connected), as of the last change
    std::vector<ClientInfo> clients() const;

    /// @brief Dials a client at a `ws://host[:port][/path]` URL, for playback (spec connection.md
    /// "Multiple servers (server-initiated)": its first activation declares `playback` when the
    /// client may play, so it may take the client from another server). A dial already in progress
    /// or connected for the same URL is kept; after repeated failures toward the URL the dial waits
    /// out a backoff. A client reached this way that drops, or says goodbye with reason `restart`
    /// or `concurrent_attempt`, is dialed again on its own (spec messaging.md "client/goodbye").
    /// A failed dial or handshake ends the request unless options ask to retry until connected
    /// (ConnectOptions), which a later call without the option leaves in place. Failures go to
    /// ServerListener::on_error().
    /// @return false while stopped or for a URL that does not parse
    bool connect_url(std::string_view url, const ConnectOptions& options = {});

    /// @brief Stops dialing a URL connect_url() asked for: a dial in progress or waiting, and a
    /// connection to it the client has not taken yet, are abandoned, and the server does not dial
    /// it on its own, also once a connection open through it ends, until connect_url() or
    /// connect(). A client connected through it stays connected (disconnect() closes it).
    /// @return false while stopped or for a URL that does not parse
    bool cancel_connect_url(std::string_view url);

    /// @brief Closes every connection of a connected client, those still in their handshake whose
    /// client/init named it included (no on_error() for them), which is reported once with
    /// DisconnectReason::DISCONNECTED_BY_HOST, and stops the server dialing it on its own as a
    /// client/goodbye that stops reconnecting would (spec messaging.md "client/goodbye"), until
    /// connect() or connect_url(), or for a client found by browsing until its instance is lost
    /// and found again. A client that connected to the server may connect again.
    /// @return false while stopped, or for a client_id clients() does not list as connected
    bool disconnect(std::string_view client_id);

    /// @brief Dials a client clients() lists as not connected, for playback, as connect_url()
    /// does, also when a client/goodbye asked the server not to reconnect. Does nothing for a
    /// connected client.
    /// @return false while stopped, or for a client_id clients() does not list
    bool connect(std::string_view client_id);

    /// @brief The groups as of the last change. While running there is always the server's group
    /// (default_group()).
    std::vector<GroupInfo> groups() const;

    /// @brief The server's group, named after ServerConfig::name until renamed, which every client
    /// approved for playback joins and which lasts as long as the run; the group to play to.
    /// nullopt while stopped.
    std::optional<GroupId> default_group() const;

    /// @brief Opens an input to a group, one at a time per group. If the group went away by the
    /// time the server attaches the input, the input is closed (InputState::CLOSED).
    OpenedInput open_input(GroupId group, const InputOptions& options);

    /// @brief Routes client_id's source into group (spec roles/source/v1.md): opens a live input
    /// to the group (InputKind::LIVE, with options' write jitter), held as open_input()'s is,
    /// and, whenever the source can stream (ServerListener::on_source_ready()), at once or once it
    /// connects, asks it to start and writes its decoded audio into that input, setting the input's
    /// format to its stream's. The host must not call write() or set_format() on the input; it
    /// controls the group's playback through it as through any input, and releasing it ends the
    /// route: the server asks the source to stop. A second call for the same client into another
    /// group replaces the route, the earlier input closing (InputState::CLOSED) and a stream open
    /// moving with it; into the group its input holds, it returns GROUP_BUSY until that input is
    /// released.
    ///
    /// The source's audio plays a fixed latency after its capture: the group's lead past its
    /// arrival when its run starts, held there by correcting the source's clock against the
    /// server's once the two measurably drift, and raised or lowered as for any live input. Its
    /// capture times also drop a chunk that arrives later than the write jitter allows, the
    /// backlog of a stall, and end the input's run at a gap of a second or more, and at the end
    /// of the source's stream, so its next audio starts a new run. A device that is a player too
    /// plays only what its group streams it, its own input included (spec roles/source/v1.md).
    /// @param client_id need not be connected
    RoutedSource route_source(std::string_view client_id, GroupId group,
                              const SourceRouteOptions& options = {});

    /// @brief Approves the client for playback without pairing (spec pairing.md "Unpaired
    /// Access") and saves the approval in its client record. A connected client that enables
    /// unpaired access is activated for playback at once and joins the server's group. Its
    /// source role needs ApprovalScope::PLAYBACK_AND_SOURCE (spec roles/source/v1.md "Unpaired
    /// access").
    /// @param client_id need not be connected
    /// @return false while stopped, or for a client_id that is not one
    bool approve(std::string_view client_id, ApprovalScope scope);

    /// @brief Revokes the client's approval and saves that in its client record, so
    /// ServerConfig::approve_all_clients no longer approves it. A connected client's streams
    /// end, it is activated with no activities or roles, and it leaves a group it shares.
    /// @return false while stopped, or for a client_id that is not one
    bool revoke_approval(std::string_view client_id);

    /// @brief Pairs the client (spec pairing.md): ServerListener::on_pairing_code_needed() asks
    /// for what the operator enters, and on_pairing_result() reports the end. A connected client
    /// pairs on its connection, which keeps playing where the method allows (a pairing PSK or
    /// Sentinel session the operator approved); one not connected is dialed for pairing. Pairing
    /// a paired client ends its playback until the pairing ends. A pairing in progress for the
    /// client ends as PairingResult::CANCELLED first; one whose long-term PSK is being saved
    /// completes and this call does nothing. Pairing PSK waits for submit_pairing_token().
    /// @return false while stopped, for a client clients() does not list, for an options.format
    /// with a method other than PairMethod::DYNAMIC_PAIRING_CODE or other than DIGITS or QR_CODE,
    /// for QR_CODE unless ServerConfig::operator_can_scan_qr, or for a connected client whose
    /// client/hello does not offer the method or a usable format (ClientInfo::pairing_offers)
    bool begin_pairing(std::string_view client_id, const PairingOptions& options);

    /// @brief The client's pairing token, "SP:0...", from its device, leaflet or QR code (spec
    /// pairing.md "Pairing Token"): pairs it with PairMethod::PAIRING_PSK, beginning that
    /// pairing unless one waits for this token
    /// @return false while stopped, for a client clients() does not list, or a token that does not
    /// decode or whose client key is not the client_id's
    bool submit_pairing_token(std::string_view client_id, std::string_view token);

    /// @brief The code the operator entered for the client's code pairing, after
    /// ServerListener::on_pairing_code_needed(): digits (spaces and hyphens ignored) or the
    /// scanned "SP:1..." token. A static code may be entered before it is asked for.
    /// @return false while stopped, or for a client_id that is not one
    bool submit_pairing_code(std::string_view client_id, std::string_view code);

    /// @brief Ends the client's pairing as PairingResult::CANCELLED (spec pairing.md "Entering and
    /// leaving pairing"); nothing once its long-term PSK is being saved, or with no pairing
    /// @return false while stopped, or for a client_id that is not one
    bool cancel_pairing(std::string_view client_id);

    /// @brief Unpairs the client (spec messaging.md "server/unpair"): removes its long-term PSK
    /// from its client record, and the record with it unless it holds an approval. A connected
    /// paired session gets server/unpair, and the client removes its own record and leaves with
    /// GoodbyeReason::UNPAIRED; an offline client's record is removed here alone. A pairing in
    /// progress is cancelled first; while a pairing's long-term PSK is being saved, unpairing does
    /// nothing (logged): call it again once on_pairing_result() reports.
    /// @return false while stopped, or for a client_id that is not one
    bool unpair(std::string_view client_id);

    /// @brief Creates a stopped group named name and moves first_member into it, as move_client()
    /// does: the group then has its own input (open_input()), controller caps and metadata.
    /// groups() lists it, and open_input() takes it, once on_group_changed() reports it. No group
    /// is created when first_member does not move: a client not connected that holds no place,
    /// or one held with no roles (an unapproved client, say).
    /// @return the new group's id; nullopt while stopped, for a name longer than
    /// config_limits::MAX_NAME_BYTES or a first_member that is not a client_id
    std::optional<GroupId> create_group(std::string_view name, std::string_view first_member);

    /// @brief Renames the group; its members get the name in a group/update
    /// @return false while stopped, for a group not in groups() or a name longer than
    /// config_limits::MAX_NAME_BYTES
    bool rename_group(GroupId group, std::string_view name);

    /// @brief Moves the client into group, which it is told with a group/update: its streams in its
    /// old group end, and in a group that plays they start again as a late join, on that group's
    /// timeline; it gets the group's controller and metadata state. A client held with no roles
    /// (not approved, say) stays in a group of its own and is not moved; one not connected that
    /// keeps its place in a group for its rejoin grace (ServerTiming::group_rejoin_grace_us)
    /// rejoins this group instead. A group other than the server's goes once it has no member, no
    /// input and no client keeping its place.
    /// @return false while stopped, for a group not in groups() or a client_id that is not one
    bool move_client(std::string_view client_id, GroupId group);

    /// @brief Sets the format the client's player streams take in place of the `format` its
    /// client/state prefers (spec player/v1.md "client/hello player@v1 support object": a server
    /// MAY apply an operator-configured format), nullopt to clear it; kept until cleared or the
    /// server stops, across the client's connections. While the client does not list the format, or
    /// the server cannot stream it to the client from its group's input, it is ignored and the
    /// client's own preference applies. A stream already playing switches to the format selected in
    /// place, at a frame boundary, continuing its timeline, and clearing the format switches it
    /// back the same way; otherwise it applies at the client's next stream.
    /// @return false while stopped, or for a client_id that is not one; true does not mean the
    /// client lists the format
    bool set_player_format(std::string_view client_id, std::optional<StreamFormat> format);

    /// @brief Sets what the group's controllers are offered and shown; a group starts with no
    /// transport commands, repeat off and shuffle off. Call it again as the state changes, such
    /// as after carrying out a REPEAT_ONE.
    /// @return false while stopped or for a group not in groups()
    bool set_controller_caps(GroupId group, const ControllerCaps& caps);

    /// @brief Sets the group's volume as a controller's `volume` command does: each player that
    /// supports volume gets one volume command, with relative levels kept (spec controller/v1.md
    /// "Setting group volume")
    /// @return false while stopped, for a group not in groups() or a volume past 100
    bool set_group_volume(GroupId group, uint32_t volume);

    /// @brief Mutes or unmutes each player in the group that supports mute
    /// @return false while stopped or for a group not in groups()
    bool set_group_mute(GroupId group, bool muted);

    /// @brief Sets what the group plays, for its members' metadata role, taking effect at at_us
    /// on now_us()'s clock. A time to come schedules it: it replaces an update scheduled before
    /// and is sent at most 20 s ahead of at_us. A time now or past replaces the current metadata
    /// and cancels a scheduled update. Report a seek, a track change or a speed change this
    /// way, and a play after a stop or a completed drain. The server reports a pause, which
    /// drops a scheduled update, a resume, a stop and the end of a drain, from the metadata set
    /// last. Set while paused, metadata goes out at speed 0, and the resume restores its speed.
    /// @return false while stopped, for a group not in groups(), a string longer than 65,535
    /// bytes or a playback_speed past MAX_PLAYBACK_SPEED
    bool set_metadata(GroupId group, const Metadata& metadata, int64_t at_us);

    /// @brief Cancels the group's scheduled metadata update, if any (spec metadata/v1.md
    /// "Server rules for scheduled metadata")
    /// @return false while stopped or for a group not in groups()
    bool cancel_scheduled_metadata(GroupId group);

    /// @brief Sets the group's image for source from at_us on now_us()'s clock, scheduled when
    /// to come, as set_metadata() does (spec roles/artwork/v1.md "Server rules for scheduled
    /// artwork"). The image is copied and hashed on the calling thread, which takes time growing
    /// with its size, so call it from neither a real-time thread nor a listener callback, which
    /// holds the dispatch thread.
    /// @param image a JPEG or PNG file's bytes; empty for no image, which clears the channels
    /// @return false while stopped, for a group not in groups(), or an image that is not a JPEG or
    /// PNG within artwork_limits
    bool set_artwork(GroupId group, ArtworkSource source, std::span<const uint8_t> image,
                     int64_t at_us);

    /// @brief Cancels the group's scheduled image for source, if any; for ALBUM, its scheduled
    /// colors too (spec roles/artwork/v1.md "Server rules for scheduled artwork")
    /// @return false while stopped or for a group not in groups()
    bool cancel_scheduled_artwork(GroupId group, ArtworkSource source);

    /// @brief Sets the group's colors for its members' color role from at_us, as
    /// set_metadata() sets metadata: a time to come schedules them, sent at most 20 s ahead. A
    /// background or on-color without the contrast spec roles/color/v1.md "server/state color
    /// object" requires (4.5:1 with its text color and its pair) is moved toward black or white
    /// until it has it; primary and accent are sent as given.
    /// @return false while stopped or for a group not in groups()
    bool set_colors(GroupId group, const Colors& colors, int64_t at_us);

    /// @brief Cancels the group's scheduled colors, if any (spec roles/color/v1.md "Server rules
    /// for scheduled colors")
    /// @return false while stopped or for a group not in groups()
    bool cancel_scheduled_colors(GroupId group);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace sendspin::server
