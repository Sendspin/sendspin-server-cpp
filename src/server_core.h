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

/// @file server_core.h
/// @brief The server's state on the core thread: its listener, dial targets (URLs and clients found
/// by browsing), connections and groups. Reports to the host through the dispatch thread and
/// publishes the clients() and groups() snapshots.

#pragma once

#include "artwork_cache.h"
#include "artwork_worker.h"
#include "audio_workers.h"
#include "backoff.h"
#include "client_records.h"
#include "crypto/keys.h"
#include "crypto/pairing_token.h"
#include "discovery_policy.h"
#include "dispatcher.h"
#include "group.h"
#include "host_view.h"
#include "log.h"
#include "pairing.h"
#include "playback_policy.h"
#include "profile.h"
#include "sendspin/server/server.h"
#include "session_driver.h"
#include "source_decoder.h"
#include "transport.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace sendspin::server {

/// @brief Caps on connections in their handshake, from the TCP accept or the dial's start until
/// the client is activated or the connection closes, and the internal RAM a new one needs
namespace connection_limits {
/// Accepted connections in their WebSocket upgrade or handshake, far above what one network's
/// clients start at once. One accepted past it is closed at once, which the log reports.
constexpr size_t MAX_ACCEPTED_HANDSHAKES = profile::MAX_ACCEPTED_HANDSHAKES;
/// Dials and dialed connections in their handshake, enough to reach a network's clients at once
/// while unreachable ones time out. A dial past it waits for one of them to end, in the order the
/// dials became due.
constexpr size_t MAX_DIALING = profile::MAX_DIALING;

/// @brief The internal RAM a new connection needs
struct HeapReserve {
    size_t reserve_bytes{0};
    size_t per_connection_bytes{0};
    size_t min_block_bytes{0};
};
/// The build's (profile.h). A connection it refuses is an accept closed at once, which the log
/// reports, or a dial reported failed (ErrorKind::DIAL_FAILED).
constexpr HeapReserve HEAP_RESERVE{profile::INTERNAL_RAM_RESERVE_BYTES,
                                   profile::INTERNAL_RAM_PER_CONNECTION_BYTES,
                                   profile::INTERNAL_RAM_MIN_BLOCK_BYTES};

/// @brief Whether heap has room for one more connection while `opening` others open: at least
/// reserve_bytes free beyond per_connection_bytes for each, the new one included, and a block of
/// min_block_bytes
bool heap_admits(const InternalHeap& heap, size_t opening, const HeapReserve& reserve);
}  // namespace connection_limits

/// @brief Settings shared by every connection
struct CoreSettings {
    SessionSettings session;
    TransportSettings transport;
    BackoffLimits backoff;
    MessageTraceFn trace;
    /// ServerConfig::trace_writes
    bool trace_writes{false};
    /// ServerConfig::approve_all_clients
    bool approve_all_clients{false};
    /// ServerTiming::group_rejoin_grace_us
    int64_t group_rejoin_grace_us{config_defaults::GROUP_REJOIN_GRACE_US};
    /// ServerTiming::pairing_start_timeout_us
    int64_t pairing_start_timeout_us{config_defaults::PAIRING_START_TIMEOUT_US};
    /// PAIRING_CLIENT_STEP_TIMEOUT_US; not a ServerConfig value
    int64_t pairing_client_step_timeout_us{PAIRING_CLIENT_STEP_TIMEOUT_US};
    /// ServerConfig::operator_can_scan_qr
    bool operator_can_scan_qr{false};
    /// The server's group's name
    std::string group_name;
    ResamplerQuality resampler_quality{ResamplerQuality::MEDIUM};
    /// ServerConfig::enable_opus
    bool enable_opus{false};
    /// ServerConfig::log and log_level
    LogFn log;
    LogLevel log_level{LogLevel::WARN};
};

/// @brief A list swapped whole by the core thread and copied by any thread (latest wins). A plain
/// vector under the mutex: a shared pointer would cost a control block per publish, and on
/// ESP-IDF's riscv32 toolchain a FreeRTOS mutex with it
template <typename T>
class Snapshot {
public:
    /// @brief The list replaced is destroyed after the mutex is released
    void publish(std::vector<T> items) {
        const std::lock_guard<std::mutex> lock(this->mutex_);
        this->items_.swap(items);
    }

    std::vector<T> read() const {
        const std::lock_guard<std::mutex> lock(this->mutex_);
        return this->items_;
    }

private:
    mutable std::mutex mutex_;
    std::vector<T> items_;
};

using ClientSnapshot = Snapshot<ClientInfo>;
using GroupSnapshot = Snapshot<GroupInfo>;

/// @brief Core thread only. Every reference passed in must outlive it.
class ServerCore {
public:
    /// @brief Creates the server's group and publishes the groups() snapshot
    /// @param group_ids the run's group id counter, which SendspinServer::create_group() takes ids
    /// from on host threads too
    /// @param provider nullptr to save no client record; called on the dispatch thread only
    /// @param records the client records loaded at start
    ServerCore(EventLoop& loop, AudioWorkers& workers, Dispatcher& dispatcher,
               ServerListener* listener, const Identity& identity, const CoreSettings& settings,
               ClientSnapshot& snapshot, GroupSnapshot& groups, InputRegistry& inputs,
               std::atomic<uint64_t>& group_ids, PersistenceProvider* provider = nullptr,
               ClientRecords records = {});
    /// @brief Joins the artwork worker, so it waits for the job in progress: one decode or
    /// encode, bounded by artwork_limits (a second or so at the limits)
    ~ServerCore();
    ServerCore(const ServerCore&) = delete;
    ServerCore& operator=(const ServerCore&) = delete;

    /// @brief The server's group, created with the core and kept until it is destroyed
    GroupId default_group() const {
        return this->default_group_;
    }

    /// @brief Accepts connections on port, on every interface; from the thread that builds the
    /// core, before its loop runs. Accepted connections use CoreSettings::transport's path.
    /// @return false when the port cannot be bound
    bool listen(uint16_t port);

    /// @brief The bound port, or 0 before listen()
    uint16_t listen_port() const;

    /// @brief The operator's dial of url, for playback: unless a dial or connection for it exists,
    /// waiting out its backoff first; with retry, dialed again on the server's own until a
    /// connection to it is activated (ConnectOptions)
    void connect_url(const std::string& url, const DialTarget& target, bool retry = false);

    /// @brief SendspinServer::cancel_connect_url()
    void cancel_connect_url(const std::string& url);

    /// @brief SendspinServer::disconnect(): closes every connection whose client/init named
    /// client_id, admitted or still in its handshake, and stops every target that reached it from
    /// dialing on its own
    void disconnect(const std::string& client_id);

    /// @brief The operator's dial, for playback, of the not connected client the clients()
    /// snapshot lists with client_id; nothing for an unknown or connected one
    void connect(const std::string& client_id);

    /// @brief A browse result (DiscoverySink::on_client_found())
    void on_client_found(const DiscoveredClient& found);

    /// @brief DiscoverySink::on_client_lost()
    void on_client_lost(const std::string& instance);

    /// @brief DiscoverySink::on_unavailable(): reports ErrorKind::DISCOVERY_UNAVAILABLE
    void on_discovery_unavailable();

    /// @brief DiscoverySink::on_available(): logged
    void on_discovery_available();

    /// @brief Attaches an input to its group, or closes it when the group is gone or holds one
    /// the host has not released
    void attach_input(const std::shared_ptr<HostView>& view);

    /// @brief SendspinServer::route_source(): client_id's source, once ready, streams into view,
    /// a live input posted to attach_input() before this, through a decoder on the group audio
    /// threads. A route the client had is replaced, its input closed. Nothing for a view gone.
    void route_source(const std::string& client_id, const std::shared_ptr<HostView>& view,
                      int64_t jitter_us);

    /// @brief Sets client_id's approval, nullopt to revoke it, saves its record and activates
    /// a connected client again when its activation changes (pairing.md "Unpaired Access")
    void set_approval(const std::string& client_id, std::optional<ApprovalScope> approval);

    /// @brief A store_long_term_psk() change being saved, and what puts it back
    struct UnsavedRecord {
        /// The record before it, which handshakes keep referencing
        ClientRecord before;
        /// A record existed before it; else the change created one
        bool existed{false};
        /// set_approval() ran meanwhile, so putting the change back keeps the approval it set
        bool approval_set{false};
    };

    /// @brief A completed pairing's record (pairing.md "Pairing Records"): sets client_id's
    /// long-term PSK and discards its approval (pairing.md "Unpaired Access"), then saves the
    /// record and calls done on the core thread with whether it is durable and the change's
    /// UnsavedRecord. Until then handshakes reference the PSK of the record as it was, and a
    /// failed save puts the change back (put_back()), so nothing uses the new PSK before it is
    /// persisted (pairing.md "server/pair-finalize"). After done, every connection of the client
    /// is activated again under its record. A second store for a client while one is in flight
    /// gets done(false) at once. done is dropped once the core stops, and shutdown() puts the
    /// change back on disk.
    void store_long_term_psk(const std::string& client_id, const Psk& psk,
                             std::function<void(bool, const UnsavedRecord&)> done);

    /// @brief Undoes a store_long_term_psk() change (spec pairing.md "Entering and leaving
    /// pairing": an attempt that ends without server/pair-finalize persists nothing): the record
    /// gets back its long-term PSK and, unless set_approval() ran meanwhile, its approval, and a
    /// record the change created goes (its slot erased) unless an approval was set on it. A name
    /// saved meanwhile stays. Saves the result and updates the ClientInfo of the client's
    /// connections and targets; publishes nothing.
    void put_back(const std::string& client_id, const UnsavedRecord& unsaved);

    /// @brief The operator's pairing of client_id (spec pairing.md): ends one in progress as
    /// PairingResult::CANCELLED, then runs the method on the client's connection, re-handshaking it
    /// first to the PSK the method needs, or dials the client for pairing when it is not connected.
    /// Pairing PSK waits for submit_pairing_token(). Every pairing ends with one
    /// on_pairing_result().
    void begin_pairing(const std::string& client_id, const PairingOptions& options);

    /// @brief The pairing token of client_id, whose client key the caller checked against it:
    /// begins a Pairing PSK pairing unless one waits for its token
    void submit_pairing_token(const std::string& client_id, const PairingPskToken& token);

    /// @brief The code the operator entered for client_id's code pairing; one of another shape
    /// asks again (on_pairing_code_needed())
    void submit_pairing_code(const std::string& client_id, const std::string& code);

    /// @brief Ends client_id's pairing as PairingResult::CANCELLED: pair/abort `user_cancelled`
    /// and an activation without `'pairing'` (spec pairing.md "Entering and leaving pairing").
    /// Nothing once its long-term PSK is being saved.
    void cancel_pairing(const std::string& client_id);

    /// @brief Removes client_id's long-term PSK and, with no approval left, its record; a
    /// connected paired session gets server/unpair (spec messaging.md "server/unpair")
    void unpair(const std::string& client_id);

    /// @brief The host's new group id, named name, created for first_member's move into it
    /// (move_action()); none when the client does not move
    void create_group(GroupId id, const std::string& name, const std::string& first_member);

    /// @brief The host's move of client_id into group (move_action()): every connection of the
    /// client that may play, or the place held for a client not connected
    void move_client(const std::string& client_id, GroupId group);

    void rename_group(GroupId group, const std::string& name);

    /// @brief The operator's format for client_id's player streams, nullopt to clear it
    /// (SendspinServer::set_player_format()): kept until cleared or the server stops, and given to
    /// each of the client's connections, whose active stream switches in place to what it selects
    void set_player_format(const std::string& client_id, std::optional<AudioFormat> format);

    void set_controller_caps(GroupId group, const ControllerCaps& caps);
    void set_group_volume(GroupId group, uint32_t volume);
    void set_group_mute(GroupId group, bool muted);
    void set_metadata(GroupId group, const Metadata& metadata, int64_t at_us);
    void cancel_scheduled_metadata(GroupId group);
    void set_artwork(GroupId group, ArtworkSource source, std::shared_ptr<const SourceImage> image,
                     int64_t at_us);
    void cancel_scheduled_artwork(GroupId group, ArtworkSource source);
    void set_colors(GroupId group, const Colors& colors, int64_t at_us);
    void cancel_scheduled_colors(GroupId group);

    /// @brief Closes every input, resolving pending drains as stopped, closes every connection,
    /// reports connected clients as stopped, ends the dispatch thread after the events already
    /// posted, and ends the loop
    void shutdown(int64_t grace_us);

private:
    class Client;

    /// @brief One client to dial: a URL the host asked for, keyed by the URL, or a client found by
    /// browsing, keyed by DISCOVERED_KEY and its instance. Its dial and retry timer call back with
    /// a pointer to it, so it is never moved, and destroying it cancels both.
    struct Target {
        std::string key;
        /// The URL dialed, for reports
        std::string url;
        DialTarget dial_target;
        /// Set for a client found by browsing
        std::optional<DiscoveryEntry> discovery;
        HandshakeBackoff backoff;
        std::optional<PendingDial> dial;
        std::unique_ptr<Timer> retry;
        bool retry_pending{false};
        /// A URL target whose last connection ended with Reconnect::AGAIN
        bool reconnecting{false};
        /// A URL target connect_url() asked to retry until a connection to it is activated
        bool retrying{false};
        /// The operator asked for the next connection, which then dials for playback; kept while
        /// the target retries until connected, so each retry dials for playback too
        bool operator_dial{false};
        /// stop_dialing() ran since the target's last dial began: whatever ends the connection
        /// open through it, the server does not dial it again on its own
        bool host_stopped{false};
        /// The next connection is for the client's pairing
        bool pairing_dial{false};
        /// Set while its dial waits for connection_limits::MAX_DIALING to allow it: the order
        /// in which it became due
        std::optional<uint64_t> waiting;
        /// The connection to this target, from the dial's success until it closes
        Client* client{nullptr};
        /// The client of the last connection, from its activation on
        std::optional<ClientInfo> known;
    };

    /// @brief The key prefix of a target found by browsing; no URL starts with it
    static constexpr std::string_view DISCOVERED_KEY = "mdns:";

    /// @brief Dials target unless a dial or connection for it exists, waiting out its backoff
    void schedule_dial(Target& target);
    /// @brief When target's next dial may start: past its backoff, and for a dial on the server's
    /// own past its reconnect wait too
    static int64_t next_dial_at_us(const Target& target);
    /// @brief Starts target's dial from its retry timer at at_us (start_dial())
    void arm_retry(Target& target, int64_t at_us);
    /// @brief schedule_dial() when the target is dialed on its own now (dials_on_its_own())
    void dial_if_due(Target& target);
    /// @brief The host's stop: no dial on the server's own (a discovered target suppressed), none
    /// in progress or waiting, a pairing that waited for its dial ended, and the connection open
    /// through it not dialed again when it ends
    void stop_dialing(Target& target);
    /// @brief Dials target, or has it wait while connection_limits::MAX_DIALING dials are in
    /// progress; a dial the internal RAM has no room for (room_for_connection()) fails at once
    void start_dial(Target& target);
    /// @brief Dials the targets waiting for a slot, in order, while slots are free and each is
    /// still due
    void start_waiting_dials();
    /// @brief Dials and dialed connections in their handshake
    size_t dialing() const;
    /// @brief Connections not yet activated or closed: dialed ones, or accepted ones
    size_t in_handshake(bool dialed) const;
    /// @brief Connections opening: dials, and dialed and accepted connections not yet activated
    size_t opening() const;
    /// @brief connection_limits::heap_admits() for one more connection, with the platform's
    /// internal heap now; always where it has none (internal_heap())
    bool room_for_connection() const;
    void on_dialed(Target& target, DialResult result);
    /// @brief A dial that did not connect, for why: counted toward the target's backoff and
    /// reported (ErrorKind::DIAL_FAILED); the caller dials again
    void dial_failed(Target& target, std::string_view why);
    /// @brief Whether the listener takes a TCP connection: while fewer than
    /// connection_limits::MAX_ACCEPTED_HANDSHAKES accepted connections are in their upgrade or
    /// handshake, and room_for_connection()
    bool admit_accept();
    void on_accepted(Connection connection);
    /// @brief A client for connection, its session started
    Client& add_client(Connection connection, Target* target, std::string url);
    /// @brief After a dialed connection closed: what its end does to dialing the target again
    void after_close(Target& target, const Client& client);
    /// @brief Erases a target whose client is gone from the network or was never reached
    void erase_if_idle(const std::string& key);
    Target* find_known(const std::string& client_id);
    /// @brief Whether a connection of client_id is activated
    bool connected(const std::string& client_id) const;
    /// @brief Whether a connection of client_id is reported connected
    bool reported(const std::string& client_id) const;
    DialState dial_state(const Target& target) const;
    /// @brief The client took the connection: reports it connected unless another connection of
    /// its client_id is reported
    void admit(Client& client);
    /// @brief A departed player's place in its group, kept for CoreSettings::group_rejoin_grace_us
    struct Held {
        GroupId group;
        std::unique_ptr<Timer> expiry;
    };

    /// @brief The operator's pairing of one client, from begin_pairing() or a token until its
    /// result
    struct Pairing {
        PairMethod method{PairMethod::DYNAMIC_PAIRING_CODE};
        /// PairingOptions::format
        std::optional<CodeFormat> requested_format;
        /// The static pairing code the operator entered before the attempt started, as PRS;
        /// wiped when destroyed
        std::vector<uint8_t> code;
        /// The connection (Client::id) the attempt runs on; 0 before one is chosen
        uint64_t connection{0};
        /// From the pairing activation queued until the attempt ends
        std::unique_ptr<PairingAttempt> attempt;
        /// The connection is re-handshaking to the PSK the attempt runs under
        bool awaiting_rekey{false};
        /// That re-handshake left the long-term PSK, so a pairing that does not complete goes back
        /// to it
        bool left_long_term{false};
        /// The connection closed during that re-handshake and the client was dialed again, once
        bool redialed{false};
        /// The pairing dialed the client (Target::pairing_dial), so its end clears that dial
        bool dialed{false};
        /// The client refused a pairing-only connection with `concurrent_attempt`; the target's
        /// reconnect dials it again, and its close does not end the pairing
        bool retry_dial{false};
        /// A pair/abort that arrived while the long-term PSK was being saved
        std::optional<PairingResult> late_abort;
        /// The pairing's one timer: ServerTiming::pairing_start_timeout_us while the client is
        /// dialed and until client/pair-init, then PAIRING_CLIENT_STEP_TIMEOUT_US while the
        /// attempt waits on the client (arm_pairing_timer())
        std::unique_ptr<Timer> start_timer;
        /// The client as last seen, for the result
        ClientInfo info;

        Pairing() = default;
        ~Pairing();
        Pairing(const Pairing&) = delete;
        Pairing& operator=(const Pairing&) = delete;
        Pairing(Pairing&&) noexcept = default;
        Pairing& operator=(Pairing&&) noexcept = default;
    };

    /// @brief A routed source (SendspinServer::route_source()): the input it plays into, the feed
    /// and job that decode its audio there, and the connection that streams into it
    struct Route {
        std::shared_ptr<HostView> view;
        std::shared_ptr<SourceFeed> feed;
        std::shared_ptr<AudioWorkers::Task> task;
        /// Signalled by the decoder when it reports a fault (on_source_faults())
        std::unique_ptr<Wakeup> faults;
        /// The connection (Client::id) bound to it; 0 for none
        uint64_t connection{0};
        /// on_source_started() reported a stream into it, which on_source_stopped() ends
        bool started{false};

        Route() = default;
        /// Stops the decoder's job without waiting for one running: what it then does reaches a
        /// view and a feed it shares, and a Wakeup that is gone
        ~Route();
        Route(const Route&) = delete;
        Route& operator=(const Route&) = delete;
    };

    void on_ready(Client& client);
    void on_closed(Client& client);
    /// @brief A change in client's source role (SessionObserver::on_source_event())
    void on_source_event(Client& client, const SourceEvent& event);
    /// @brief Binds client_id's route, if any, to one of its connections whose activation holds
    /// the source role, the reported one first, and every other connection to none, so only that
    /// one is asked to stream; caller, whose driver is reporting, is not pumped
    void refresh_source(const std::string& client_id, const Client* caller = nullptr);
    /// @brief The faults the decoder of client_id's route reported: a protocol error closes the
    /// connection (roles/source/v1.md), a stream the route cannot take fails
    void on_source_faults(const std::string& client_id);
    /// @brief Unbinds route's connection and reports its stream stopped; the route is destroyed
    void end_route(std::unique_ptr<Route> route, const Client* caller = nullptr);
    /// @brief route's connection stops streaming into it: an open stream's end goes to the
    /// decoder, so the input's run ends, and the host hears it stopped
    void unbind(Route& route);
    /// @brief Ends the routes whose input the host released or the server closed
    void drop_gone_routes();
    /// @brief Reports route's stream started, or stopped when started is false, for client
    void report_source(Route& route, const Client& client, bool started,
                       const AudioFormat& format = {});
    /// @brief With profile::MEMORY_STATS, logs the heaps and the tasks' stacks at phase
    void log_memory(const char* phase) const;
    void on_pairing_message(Client& client, const ClientPairing& message);
    /// @brief The connection client_id's pairing runs on: the reported one, else any activated
    Client* pairing_connection(const std::string& client_id);
    /// @brief Whether an attempt runs on client's connection, so its activation changes wait
    bool pairing_running(const Client& client) const;
    /// @brief Starts the attempt of client's pairing on client, its pairing activation queued
    /// under the PSK the method needs, behind a re-handshake when the session is under another
    /// @param first the client's first activation, which then carries the pairing
    void start_attempt(Client& client, Pairing& pairing,
                       std::optional<ServerActivate> first = std::nullopt);
    /// @brief Ends client_id's pairing with result: the attempt's connection, when it is open and
    /// leave is set, gets the activation without `'pairing'` (behind a re-handshake back to the
    /// long-term PSK the attempt left)
    void end_pairing(const std::string& client_id, PairingResult result, bool leave);
    /// @brief The accepted client/pair-finalize's long-term PSK is stored, then acknowledged and
    /// re-handshaken to (spec pairing.md "server/pair-finalize")
    void finalize_pairing(Client& client, const HandshakePsk& psk);
    /// @brief Arms client_id's pairing timer: on expiry the pairing ends as TIMED_OUT when it is
    /// still on connection (0 for one dialed) at step (nullopt: no attempt yet)
    void arm_pairing_timer(const std::string& client_id, uint64_t connection,
                           std::optional<PairingStep> step, int64_t timeout_us);
    /// @brief Dials client_id's target for its pairing; false when the server knows no target
    bool dial_for_pairing(const std::string& client_id, Pairing& pairing);
    /// @brief The pairing's dial flags (Target::pairing_dial, operator_dial) go once it ends,
    /// however it ends, so a dial not made yet is not one for the pairing
    void release_pairing_dial(const std::string& client_id, const Pairing& pairing);
    /// @brief Starts the round of client's attempt when it waits for a code it has
    void start_round(Client& client, Pairing& pairing);
    /// @brief Ends client's pairing as a protocol error and closes it (spec pairing.md "Protocol
    /// Errors")
    void reject_pairing(Client& client);
    /// @brief The pairing on client's connection after it closed: a dial again when it closed
    /// re-handshaking for the attempt, else its end
    void on_pairing_connection_closed(Client& client);
    void post_pairing_result(ClientInfo info, PairingResult result);
    void post_code_needed(ClientInfo info, PairMethod method, std::optional<CodeFormat> format);
    void on_client_state(Client& client);
    /// @brief A re-handshake of client's connection completed: it is no longer a fallback
    void on_rekeyed(Client& client);
    void on_controller_request(Client& client, const ControllerRequest& request);
    /// @brief client/leave: leave_group(), and the artwork stream ends until the client joins a
    /// group or returns to `available: true`
    void on_client_leave(Client& client);
    /// @brief A result of the artwork worker: kept in the cache, then every group takes what it
    /// awaited
    void on_artwork_result(ArtworkResult result);
    /// @brief The unavailable branch, which client/leave shares (messaging.md "Server behavior
    /// when a client becomes unavailable", "client/leave"): out of a shared group into a stopped
    /// one of its own, remembering the group it left; alone, its group's playback stops
    void leave_group(Client& client);
    /// @brief A new stopped group holding client alone
    /// @return the new group, which outlives the handler even when the move closed the connection
    Group& move_to_solo_group(Client& client);
    /// @brief client leaves its group for to, which may then be erased
    void move_member(Client& client, Group& to);
    /// @brief Moves client_id's connections, or the place held for it, as move_action() decides,
    /// into to(), which is called only when something moves
    /// @param target to()'s id when it exists already, for move_action()'s in_target
    void place(const std::string& client_id, std::optional<GroupId> target,
               const std::function<Group&()>& to);
    /// @brief controller/v1.md "Switch command cycle"
    void switch_group(Client& client);
    std::optional<ApprovalScope> approval_of(const std::string& client_id) const;
    /// @brief The long-term PSK handshakes reference for client_id: its record's, or while a new
    /// one is being saved, the one before it; nullptr for none
    const Psk* long_term_psk(const std::string& client_id) const;
    /// @brief The PSK a new connection of client_id references (select_handshake_psk()); for a dial
    /// for pairing, the PSK its method runs under, so a paired client is not referenced under its
    /// long-term PSK
    HandshakePsk handshake_psk(const std::string& client_id, bool pairing_dial = false) const;
    /// @param psk the PSK the activation goes out under
    /// @param playback whether the activation declares `playback` when the client may play
    ActivationDecision decide(const Client& client, const PskMatch& psk, bool playback) const;
    /// @brief decide() under the PSK of the session's activation queued last
    ActivationDecision decide(const Client& client, bool playback) const;
    void post_notice(const Client& client, ActivationNotice notice);
    /// @brief After a client's approval changed, or with rekey, to re-handshake its connection to
    /// that PSK: activates it again when whether it may play changes, and moves it into the
    /// server's group, or alone out of the server's or a shared one. A re-handshake's activation is
    /// decided under its PSK and always queued, frozen behind it, as is a pairing activation
    /// (pairing set) and one forced. Otherwise, while a pairing attempt runs, the change waits for
    /// its end.
    void reactivate(Client& client, const std::optional<HandshakePsk>& rekey = std::nullopt,
                    const std::optional<PairingParams>& pairing = std::nullopt, bool force = false);
    /// @brief Re-handshakes every activated connection of client_id to psk (connection.md
    /// "Re-handshake")
    void rekey(const std::string& client_id, const HandshakePsk& psk);
    /// @brief Saves client_id's record through the provider on the dispatch thread, reporting a
    /// failure with on_error(); then, unless the core stopped, calls done on the core thread with
    /// whether it is durable. Without a provider nothing is saved and done gets true. The save
    /// may finish after the core is destroyed: it reaches the core only through loop_, which
    /// outlives the dispatch thread, and a handler posted after the loop ended never runs.
    void save_record(const std::string& client_id, std::function<void(bool)> done = nullptr);
    /// @brief Saves an all-zero blob, a free slot, in slot, once client_id's record is removed
    void erase_record(const std::string& client_id, size_t slot);
    /// @brief A record's blob, wiped when the last holder releases it
    struct RecordBlob {
        ClientRecordBlob bytes{};

        RecordBlob() = default;
        ~RecordBlob();
        RecordBlob(const RecordBlob&) = delete;
        RecordBlob& operator=(const RecordBlob&) = delete;
    };
    /// @brief Saves blob in slot on the dispatch thread, as save_record() describes
    void write_record(const std::string& client_id, size_t slot, std::shared_ptr<RecordBlob> blob,
                      std::function<void(bool)> done);
    /// @brief The client the log names for client_id's record: its name and address when
    /// connected; empty when errors are not logged
    std::string record_owner(const std::string& client_id) const;
    /// @brief Keeps the name and identifiers of client's client/hello in its record, when it has
    /// one, saving it when they changed
    void refresh_record(const Client& client);
    /// @brief Queues an activation that declares `playback` or drops it as the later activation
    /// rule asks for in a group whose playback is group_playing, keeping the roles. Queued only:
    /// the caller pumps.
    void refresh_playback(Client& client, bool group_playing);
    /// @brief After group's playback started or stopped: refresh_playback() for every member,
    /// and a dial for each client held for it whose target is due
    void on_playback_changed(Group& group);
    /// @brief Keeps a departed client's place in group until the grace ends
    void hold(const std::string& client_id, GroupId group);
    /// @brief Ends client_id's hold and erases its group if nothing else keeps it
    void release_held(const std::string& client_id);
    Group* find_group(GroupId id);
    /// @param id nullopt for the next id of the run's counter
    Group& new_group(std::string name, std::optional<GroupId> id = std::nullopt);
    /// @brief Publishes the groups() snapshot, then posts on_group_changed() for group
    void report_group(const Group& group);
    /// @brief Erases group, from a later handler, when it is not the server's group and has no
    /// member or input by then
    void erase_if_unused(Group& group);
    void publish_groups();
    void post_error(ServerError error);
    void trace(const Client& client, TraceDirection direction, MessageKind kind,
               std::span<const uint8_t> plaintext, int64_t stamp_us, size_t audio_bytes);
    /// @brief Traces the completed write of client's last traced outbound message
    void trace_written(const Client& client, int64_t done_us);
    /// @brief Posts message to the trace, naming client's connection and client_id
    void post_trace(const Client& client, TracedMessage message);
    /// @brief Erases idle URL targets that never reached a client and have no failures, or whose
    /// last wait ended at least the maximum backoff before now_us, so the map stays bounded
    void prune_targets(int64_t now_us);
    void publish_snapshot();
    /// @brief Posts event, called with the host's listener, to the dispatch thread, the closure
    /// stored in its queue; nothing without a listener or once stopping
    template <class F>
    void post_event(F&& event) {
        if (this->listener_ == nullptr || this->stopping_) {
            return;
        }
        this->dispatcher_.post(
            [listener = this->listener_, event = std::forward<F>(event)] { event(*listener); });
    }
    /// @brief The client for the log: its name and address once activated, else the URL dialed
    /// or the address it connected from
    std::string describe(const Client& client) const;

    EventLoop& loop_;
    AudioWorkers& workers_;
    Dispatcher& dispatcher_;
    ServerListener* listener_;
    const Identity& identity_;
    const CoreSettings& settings_;
    ClientSnapshot& snapshot_;
    GroupSnapshot& groups_snapshot_;
    InputRegistry& inputs_;
    std::atomic<uint64_t>& group_ids_;
    Logger log_;
    std::unique_ptr<Listener> listener_socket_;
    std::map<std::string, Target> targets_;
    size_t discovered_count_{0};
    std::map<uint64_t, std::unique_ptr<Client>> clients_;
    /// The worker's results reach core through loop_; one delivered after the loop ended is never
    /// run. Declared before the groups, which read the cache, so it outlives them.
    ArtworkWorker artwork_worker_;
    ArtworkCache artwork_cache_;
    std::map<GroupId, std::unique_ptr<Group>> groups_;
    std::map<std::string, Held, std::less<>> held_;
    PersistenceProvider* provider_;
    ClientRecords records_;
    /// By client_id, at most one each
    std::map<std::string, UnsavedRecord, std::less<>> unsaved_records_;
    /// The pairing PSK of each client the operator started a Pairing PSK pairing for, from a
    /// pairing token whose client key matches the client_id (pairing.md "Pairing Token")
    std::map<std::string, HandshakePsk, std::less<>> pairing_psks_;
    /// The operator's pairings, by client_id, at most one each
    std::map<std::string, Pairing, std::less<>> pairings_;
    /// The routed sources, by client_id: one per client, bounded by the groups, each of which
    /// holds at most one input
    std::map<std::string, std::unique_ptr<Route>, std::less<>> routes_;
    /// The operator's player format per client_id (set_player_format()): one entry per client the
    /// host names, cleared at its call, so the host bounds it
    std::map<std::string, AudioFormat, std::less<>> format_overrides_;
    GroupId default_group_;
    uint64_t next_connection_id_{1};
    uint64_t next_instance_id_{1};
    uint64_t next_waiting_{0};
    /// Connections refused since the listener last took one
    size_t refused_accepts_{0};
    bool stopping_{false};
};

}  // namespace sendspin::server
