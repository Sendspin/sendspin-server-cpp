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

#include "server_core.h"

#include "clock.h"
#include "crypto/crypto.h"
#include "memory.h"
#include "messages.h"
#include "playback_policy.h"
#include "sched/constants.h"
#include "server_core_client.h"

#include <algorithm>
#include <utility>
#include <variant>

namespace sendspin::server {

namespace {

/// The most distinct identifiers one connection reports with on_newer_client_detected(), which
/// bounds the events a client/hello listing many can cause
constexpr size_t MAX_NEWER_IDENTIFIER_EVENTS = 8;

}  // namespace

bool connection_limits::heap_admits(const InternalHeap& heap, size_t opening,
                                    const HeapReserve& reserve) {
    return heap.free_bytes >=
               reserve.reserve_bytes + reserve.per_connection_bytes * (opening + 1) &&
           heap.largest_block_bytes >= reserve.min_block_bytes;
}

// ==============================================================================
// ServerCore
// ==============================================================================

ServerCore::ServerCore(EventLoop& loop, AudioWorkers& workers, Dispatcher& dispatcher,
                       ServerListener* listener, const Identity& identity,
                       const CoreSettings& settings, ClientSnapshot& snapshot,
                       GroupSnapshot& groups, InputRegistry& inputs,
                       std::atomic<uint64_t>& group_ids, PersistenceProvider* provider,
                       ClientRecords records)
    : loop_(loop),
      workers_(workers),
      dispatcher_(dispatcher),
      listener_(listener),
      identity_(identity),
      settings_(settings),
      snapshot_(snapshot),
      groups_snapshot_(groups),
      inputs_(inputs),
      group_ids_(group_ids),
      log_(dispatcher, settings.log, settings.log_level),
      artwork_worker_([this, &loop](ArtworkResult result) {
          // On the worker's thread. A handler posted after the loop ended is never run, so this is
          // alive in one that runs.
          loop.post([this, done = std::make_shared<ArtworkResult>(std::move(result))] {
              if (!this->stopping_) {
                  this->on_artwork_result(std::move(*done));
              }
          });
      }),
      artwork_cache_([this](ArtworkJob job) { return this->artwork_worker_.post(std::move(job)); }),
      provider_(provider),
      records_(std::move(records)) {
    this->default_group_ = this->new_group(settings.group_name).id();
    this->publish_groups();
}

ServerCore::~ServerCore() = default;

bool ServerCore::listen(uint16_t port) {
    this->listener_socket_ = std::make_unique<Listener>(
        this->loop_, this->settings_.transport,
        [this](Connection connection) { this->on_accepted(std::move(connection)); },
        [this] { return this->admit_accept(); });
    // Dual-stack where the system has IPv6, else IPv4 alone; a port in use fails either way
    if (this->listener_socket_->listen("::", port) ||
        (this->listener_socket_->family_unavailable() &&
         this->listener_socket_->listen("0.0.0.0", port))) {
        this->log_memory("start");
        return true;
    }
    this->listener_socket_.reset();
    return false;
}

void ServerCore::log_memory(const char* phase) const {
    if constexpr (profile::MEMORY_STATS) {
        const MemoryStats stats = memory_stats();
        SS_LOGI(this->log_,
                "memory %s: internal %zu free, %zu largest block, %zu least free, %zu allocated "
                "in %zu blocks, %zu free blocks; PSRAM %zu free",
                phase, stats.internal_free_bytes, stats.internal_largest_block_bytes,
                stats.internal_minimum_free_bytes, stats.internal_allocated_bytes,
                stats.internal_allocated_blocks, stats.internal_free_blocks,
                stats.external_free_bytes);
        SS_LOGI(this->log_, "stacks %s (bytes never used): %s", phase, task_stacks().c_str());
    }
}

uint16_t ServerCore::listen_port() const {
    return this->listener_socket_ ? this->listener_socket_->port() : 0;
}

void ServerCore::connect_url(const std::string& url, const DialTarget& target, bool retry) {
    if (this->stopping_) {
        return;
    }
    this->prune_targets(now_us());
    auto [it, inserted] = this->targets_.try_emplace(url);
    Target& entry = it->second;
    if (inserted) {
        entry.key = url;
        entry.url = url;
        entry.dial_target = target;
    }
    entry.operator_dial = true;
    entry.host_stopped = false;
    entry.retrying = entry.retrying || retry;
    this->schedule_dial(entry);
}

void ServerCore::cancel_connect_url(const std::string& url) {
    const auto it = this->targets_.find(url);
    if (this->stopping_ || it == this->targets_.end() || it->second.discovery) {
        return;
    }
    // A dialed connection the client has not taken yet is part of the dial: abandoned with it
    Client* client = it->second.client;
    const bool abandon = client != nullptr && !client->admitted && !client->closed;
    this->stop_dialing(it->second);
    if (abandon) {
        client->driver->session().close_by_host();
        // Its close may erase the target
        client->driver->pump();
    }
    this->erase_if_idle(url);
    this->publish_snapshot();
    this->start_waiting_dials();
}

void ServerCore::disconnect(const std::string& client_id) {
    if (this->stopping_) {
        return;
    }
    // Not dialed again on the server's own, through whichever target reached it
    for (auto& [key, target] : this->targets_) {
        if (target.known && target.known->client_id == client_id) {
            this->stop_dialing(target);
        }
    }
    // Every connection of the client, also one still in its handshake once its client/init named
    // it
    std::vector<Client*> closing;
    for (const auto& [id, client] : this->clients_) {
        const std::optional<ClientInit>& init = client->driver->session().client_init();
        if (!client->closed && init && init->client_id == client_id) {
            closing.push_back(client.get());
        }
    }
    // All end first, so that none is reported in the place of another as it closes
    for (Client* client : closing) {
        client->driver->session().close_by_host();
    }
    for (Client* client : closing) {
        client->driver->pump();
    }
    this->publish_snapshot();
    this->start_waiting_dials();
}

void ServerCore::connect(const std::string& client_id) {
    if (this->stopping_ || this->connected(client_id)) {
        return;
    }
    Target* target = this->find_known(client_id);
    if (target == nullptr) {
        return;
    }
    // Operator actions always dial
    target->operator_dial = true;
    target->host_stopped = false;
    if (target->discovery) {
        target->discovery->suppressed = false;
        this->publish_snapshot();
    }
    this->schedule_dial(*target);
}

void ServerCore::on_client_found(const DiscoveredClient& found) {
    std::optional<DiscoveredClient> service = validated(found);
    if (this->stopping_ || !service) {
        return;
    }
    const std::string key = std::string(DISCOVERED_KEY) + service->instance;
    auto it = this->targets_.find(key);
    const bool seen = it == this->targets_.end();
    if (seen) {
        if (this->discovered_count_ >= discovery_limits::MAX_CLIENTS) {
            return;
        }
        ++this->discovered_count_;
        it = this->targets_.try_emplace(key).first;
        it->second.key = key;
    }
    Target& target = it->second;
    // Suppression clears when the instance disappears and reappears
    const bool reappeared = target.discovery && !target.discovery->present;
    const bool suppressed = target.discovery && target.discovery->suppressed && !reappeared;
    target.url = discovered_url(*service);
    std::string host = service->host;
    if (host.ends_with('.')) {
        host.pop_back();
    }
    target.dial_target = {host.empty() ? service->addresses.front() : host, service->port,
                          service->path, service->addresses};
    target.discovery = DiscoveryEntry{std::move(*service), true, suppressed};
    this->publish_snapshot();
    if (seen) {
        ClientInfo info;
        const DiscoveredClient& entry = target.discovery->service;
        info.name = entry.name.value_or(entry.instance);
        info.url = target.url;
        info.address = discovered_address(entry);
        info.connected = false;
        info.discovered = true;
        this->post_event([info](ServerListener& listener) { listener.on_client_seen(info); });
    }
    this->dial_if_due(target);
}

void ServerCore::on_client_lost(const std::string& instance) {
    const auto it = this->targets_.find(std::string(DISCOVERED_KEY) + instance);
    if (this->stopping_ || it == this->targets_.end()) {
        return;
    }
    std::optional<DiscoveryEntry>& entry = it->second.discovery;
    if (!entry) {
        return;
    }
    entry->present = false;
    // Cancels a dial or a wait for one; a connected client's target goes when it closes
    const std::string key = it->first;
    this->erase_if_idle(key);
    this->publish_snapshot();
    this->start_waiting_dials();
}

void ServerCore::on_discovery_unavailable() {
    SS_LOGW(this->log_, "mDNS is unavailable: clients reach the server only by URL or address");
    this->post_error({ErrorKind::DISCOVERY_UNAVAILABLE, {}, {}, std::nullopt, std::nullopt});
}

void ServerCore::on_discovery_available() {
    SS_LOGI(this->log_, "mDNS is available again");
}

void ServerCore::schedule_dial(Target& target) {
    if (target.client != nullptr || target.dial || target.waiting) {
        return;
    }
    const int64_t at_us = next_dial_at_us(target);
    if (at_us > now_us()) {
        this->arm_retry(target, at_us);
        return;
    }
    if (target.retry_pending) {
        target.retry->cancel();
    }
    this->start_dial(target);
}

int64_t ServerCore::next_dial_at_us(const Target& target) {
    // messaging.md "client/goodbye": backoff on repeated handshake failures. The operator's dial
    // waits out only that, a dial on the server's own the reconnect wait too.
    return target.operator_dial ? target.backoff.next_dial_us()
                                : target.backoff.next_reconnect_us();
}

void ServerCore::arm_retry(Target& target, int64_t at_us) {
    const int64_t now = now_us();
    SS_LOGI(this->log_, "next dial to %s in %lld ms (%u failed in a row)",
            url_for_log(target.url).c_str(),
            static_cast<long long>(std::max<int64_t>(at_us - now, 0) / sched::US_PER_MS),
            static_cast<unsigned>(target.backoff.failures()));
    if (!target.retry) {
        target.retry = std::make_unique<Timer>(this->loop_);
    }
    target.retry_pending = true;
    target.retry->start(at_us, [this, entry = &target] {
        entry->retry_pending = false;
        // What held at the request may not hold after the wait
        if (entry->operator_dial || dials_on_its_own(this->dial_state(*entry))) {
            this->start_dial(*entry);
        }
    });
}

void ServerCore::stop_dialing(Target& target) {
    target.operator_dial = false;
    target.retrying = false;
    target.reconnecting = false;
    target.host_stopped = true;
    // A pairing waiting for this target's dial has no connection to come
    if (std::exchange(target.pairing_dial, false) && target.known) {
        const auto pairing = this->pairings_.find(target.known->client_id);
        if (pairing != this->pairings_.end() && pairing->second.connection == 0) {
            this->end_pairing(target.known->client_id, PairingResult::CONNECTION_LOST, false);
        }
    }
    if (target.discovery) {
        target.discovery->suppressed = true;
    }
    if (target.retry_pending) {
        target.retry->cancel();
        target.retry_pending = false;
    }
    target.waiting.reset();
    target.dial.reset();
}

DialState ServerCore::dial_state(const Target& target) const {
    DialState state;
    state.discovered = target.discovery.has_value();
    state.present = state.discovered && target.discovery->present;
    state.suppressed = state.discovered && target.discovery->suppressed;
    state.reconnecting = target.reconnecting;
    state.retrying = target.retrying;
    state.busy = target.client != nullptr || target.dial || target.retry_pending || target.waiting;
    state.connected_elsewhere = target.known && this->connected(target.known->client_id);
    return state;
}

void ServerCore::dial_if_due(Target& target) {
    if (!this->stopping_ && dials_on_its_own(this->dial_state(target))) {
        this->schedule_dial(target);
    }
}

void ServerCore::start_dial(Target& target) {
    target.retry_pending = false;
    if (this->dialing() >= connection_limits::MAX_DIALING) {
        if (!target.waiting) {
            target.waiting = this->next_waiting_++;
            SS_LOGI(this->log_, "dial to %s waits: %zu dials are in progress",
                    url_for_log(target.url).c_str(), connection_limits::MAX_DIALING);
        }
        return;
    }
    target.waiting.reset();
    // A new connection: the host's earlier stop was for the one before
    target.host_stopped = false;
    if (!this->room_for_connection()) {
        this->dial_failed(target, "internal RAM is low");
        // From the timer even when the backoff has passed, so a refusal never dials from here
        if (!this->stopping_ && dials_on_its_own(this->dial_state(target))) {
            this->arm_retry(target, next_dial_at_us(target));
        }
        return;
    }
    SS_LOGI(this->log_, "dialing %s%s", url_for_log(target.url).c_str(),
            target.operator_dial ? " for playback" : "");
    target.dial.emplace(dial(this->loop_, target.dial_target, this->settings_.transport,
                             [this, target = &target](DialResult result) {
                                 this->on_dialed(*target, std::move(result));
                             }));
}

void ServerCore::start_waiting_dials() {
    while (!this->stopping_ && this->dialing() < connection_limits::MAX_DIALING) {
        Target* next = nullptr;
        for (auto& [key, target] : this->targets_) {
            if (target.waiting && (next == nullptr || target.waiting < next->waiting)) {
                next = &target;
            }
        }
        if (next == nullptr) {
            return;
        }
        next->waiting.reset();
        // What held when the dial became due may not hold after the wait
        if (next->operator_dial || dials_on_its_own(this->dial_state(*next))) {
            this->start_dial(*next);
        }
    }
}

size_t ServerCore::dialing() const {
    const auto dials =
        std::count_if(this->targets_.begin(), this->targets_.end(),
                      [](const auto& entry) { return entry.second.dial.has_value(); });
    return static_cast<size_t>(dials) + this->in_handshake(true);
}

size_t ServerCore::opening() const {
    const size_t upgrades =
        this->listener_socket_ ? this->listener_socket_->upgrades_in_progress() : 0;
    return this->dialing() + upgrades + this->in_handshake(false);
}

bool ServerCore::room_for_connection() const {
    const std::optional<InternalHeap> heap = internal_heap();
    return !heap ||
           connection_limits::heap_admits(*heap, this->opening(), connection_limits::HEAP_RESERVE);
}

size_t ServerCore::in_handshake(bool dialed) const {
    return static_cast<size_t>(
        std::count_if(this->clients_.begin(), this->clients_.end(), [dialed](const auto& entry) {
            const Client& client = *entry.second;
            return !client.closed && !client.active && (client.target != nullptr) == dialed;
        }));
}

void ServerCore::on_dialed(Target& target, DialResult result) {
    target.dial.reset();
    if (Connection* connection = std::get_if<Connection>(&result)) {
        Client& client = this->add_client(std::move(*connection), &target, target.url);
        client.pairing_dial = std::exchange(target.pairing_dial, false);
        target.client = &client;
        client.driver->start();
        return;
    }
    this->dial_failed(target, close_description(std::get<CloseReason>(result)));
    this->dial_if_due(target);
    this->start_waiting_dials();
}

void ServerCore::dial_failed(Target& target, std::string_view why) {
    target.backoff.record_failure(now_us(), this->settings_.backoff);
    // A later dial on the server's own is idle, unless it retries the operator's
    if (!target.retrying) {
        target.operator_dial = false;
    }
    if (std::exchange(target.pairing_dial, false) && target.known &&
        this->pairings_.contains(target.known->client_id)) {
        this->end_pairing(target.known->client_id, PairingResult::CONNECTION_LOST, false);
    }
    SS_LOGW(this->log_, "dial to %s failed: %s", url_for_log(target.url).c_str(),
            std::string(why).c_str());
    this->post_event(
        [error = ServerError{ErrorKind::DIAL_FAILED, target.url, {}, std::nullopt, std::nullopt}](
            ServerListener& listener) { listener.on_error(error); });
}

bool ServerCore::admit_accept() {
    const size_t handshaking =
        this->listener_socket_->upgrades_in_progress() + this->in_handshake(false);
    const bool capped = handshaking >= connection_limits::MAX_ACCEPTED_HANDSHAKES;
    if (!capped && this->room_for_connection()) {
        if (this->refused_accepts_ > 0) {
            SS_LOGI(this->log_, "accepting connections again, after refusing %zu",
                    this->refused_accepts_);
            this->refused_accepts_ = 0;
        }
        return true;
    }
    // Once per run of refusals, so a flood of connections does not flood the log
    if (this->refused_accepts_++ == 0) {
        if (capped) {
            SS_LOGW(this->log_, "refusing connections: %zu are in their handshake", handshaking);
        } else {
            SS_LOGW(this->log_, "refusing connections: internal RAM is low");
        }
    }
    return false;
}

void ServerCore::on_accepted(Connection connection) {
    if (this->stopping_) {
        return;
    }
    this->add_client(std::move(connection), nullptr, {}).driver->start();
}

ServerCore::Client& ServerCore::add_client(Connection connection, Target* target, std::string url) {
    const uint64_t id = this->next_connection_id_++;
    auto client = std::make_unique<Client>(*this, target, std::move(url), id);
    client->driver = std::make_unique<SessionDriver>(
        this->loop_, std::move(connection), this->identity_, this->settings_.session, *client,
        this->settings_.trace ? client.get() : nullptr, client.get());
    Client& added = *client;
    this->clients_.emplace(id, std::move(client));
    return added;
}

void ServerCore::on_ready(Client& client) {
    this->log_memory("ready");
    const Session& session = client.driver->session();
    // NOLINTBEGIN(bugprone-unchecked-optional-access): both are set from READY_FOR_ACTIVATION on,
    // which on_ready() follows
    const ClientHello& hello = *session.client_hello();
    client.info = ClientInfo{};
    client.info.client_id = session.client_init()->client_id;
    client.info.name = hello.name;
    client.info.url = client.url;
    client.info.unpaired_access = hello.unpaired_access_enabled;
    // NOLINTEND(bugprone-unchecked-optional-access)
    client.info.approval = this->approval_of(client.info.client_id);
    client.info.paired = this->long_term_psk(client.info.client_id) != nullptr;
    client.info.pairing_offers = pairing_offers(hello.pair_methods);
    client.info.roles = implemented_roles(hello);
    client.info.credential_mismatch =
        session.psk_match().fallback_from == std::optional(PskCategory::LONG_TERM);
    client.info.address = client.driver->remote_endpoint();
    client.info.accepted = client.target == nullptr;
    client.info.discovered = client.target != nullptr && client.target->discovery.has_value();
    client.active = true;
    if (client.target != nullptr) {
        client.target->retrying = false;
    }
    client.activated_us = now_us();
    DialIntent intent = DialIntent::IDLE;
    if (Target* target = client.target) {
        target->backoff.record_success();
        intent = target->operator_dial ? DialIntent::PLAYBACK : DialIntent::IDLE;
        target->operator_dial = false;
    }
    // A dial for a pairing that ended meanwhile is an idle one
    if (client.pairing_dial) {
        const auto waiting = this->pairings_.find(client.info.client_id);
        intent = waiting != this->pairings_.end() && waiting->second.connection == 0
                     ? DialIntent::PAIRING
                     : DialIntent::IDLE;
    }
    // Every client is in one group: a player back within its grace in the group it left, a client
    // that may play in the server's, any other alone
    const bool playable = this->decide(client, false).playable;
    // connection.md "Multiple servers (server-initiated)": a client that may play arbitrates its
    // first activation and holds its client/state until it took the connection, so it counts as
    // connected from that client/state (on_client_state()); one held with no roles at once
    if (!playable) {
        this->admit(client);
    }
    Group* group = nullptr;
    const auto held = this->held_.find(client.info.client_id);
    if (held != this->held_.end() && playable) {
        group = this->find_group(held->second.group);
    }
    if (held != this->held_.end()) {
        this->release_held(client.info.client_id);
    }
    // A dial back into a group that plays is for playback
    if (group != nullptr && group->playing() && intent == DialIntent::IDLE) {
        intent = DialIntent::PLAYBACK;
    }
    if (group == nullptr) {
        group = playable ? this->groups_.at(this->default_group_).get()
                         : &this->new_group(client.info.name);
    }
    const ConnectionOrigin origin =
        client.target != nullptr ? ConnectionOrigin::DIALED : ConnectionOrigin::ACCEPTED;
    // messaging.md "server/activate": `active_roles` is required on the first activation
    const ActivationDecision decision =
        this->decide(client, first_activation_playback(origin, intent, group->playing()));
    this->post_notice(client, decision.notice);
    // README "Detecting Outdated Servers"
    std::vector<std::string_view> reported;
    for (const std::string& identifier : hello.unrecognized_identifiers) {
        if (reported.size() == MAX_NEWER_IDENTIFIER_EVENTS) {
            break;
        }
        if (std::find(reported.begin(), reported.end(), identifier) != reported.end()) {
            continue;
        }
        reported.emplace_back(identifier);
        this->post_event([info = client.info, newer = identifier](ServerListener& listener) {
            listener.on_newer_client_detected(info, newer);
        });
    }
    this->refresh_record(client);
    client.playable = decision.playable;
    client.playback = decision.activation.playback;
    // The operator's pairing of the client, on this connection when its PSK is the one the method
    // needs: the first activation is the pairing one
    const auto pairing = this->pairings_.find(client.info.client_id);
    Pairing* waiting = nullptr;
    if (pairing != this->pairings_.end() && pairing->second.connection == 0) {
        waiting = &pairing->second;
        waiting->info = client.info;
        // connection.md "Sentinel Fallback": the client could not use the token's pairing PSK
        if (waiting->method == PairMethod::PAIRING_PSK &&
            session.psk_match().fallback_from == std::optional(PskCategory::PAIRING)) {
            waiting = nullptr;
            this->end_pairing(client.info.client_id, PairingResult::TOKEN_MISMATCH, false);
        }
    }
    const bool pairs_now = waiting != nullptr &&
                           session.psk_match().category == attempt_psk(waiting->method) &&
                           (waiting->method != PairMethod::PAIRING_PSK ||
                            this->pairing_psks_.contains(client.info.client_id));
    client.group = group;
    if (const auto format = this->format_overrides_.find(client.info.client_id);
        format != this->format_overrides_.end()) {
        client.driver->session().set_format_override(format->second);
    }
    if (pairs_now) {
        this->start_attempt(client, *waiting, decision.activation);
    } else {
        client.driver->activate(decision.activation);
    }
    // The later activation rule applies from here, except that a dial for playback keeps `playback`
    // until its group's playback next starts or stops
    if (origin == ConnectionOrigin::ACCEPTED || intent == DialIntent::IDLE) {
        this->refresh_playback(client, group->playing());
    }
    group->add_member(client);
    // The PSK differs: the attempt starts behind a re-handshake to it
    if (waiting != nullptr && !pairs_now && this->pairings_.contains(client.info.client_id) &&
        (waiting->method != PairMethod::PAIRING_PSK ||
         this->pairing_psks_.contains(client.info.client_id))) {
        this->start_attempt(client, *waiting);
    }
    if (client.target != nullptr) {
        this->start_waiting_dials();
    }
}

void ServerCore::admit(Client& client) {
    client.admitted = true;
    // The target dials this client from now on (connected_elsewhere(), connect())
    if (client.target != nullptr) {
        client.target->known = client.info;
    }
    // A second connection of a client already reported is not reported again
    if (this->reported(client.info.client_id)) {
        return;
    }
    client.reported = true;
    SS_LOGI(this->log_, "%s connected", this->describe(client).c_str());
    // Published first, so that a listener sees the client in clients()
    this->publish_snapshot();
    this->post_event(
        [info = client.info](ServerListener& listener) { listener.on_client_connected(info); });
}

bool ServerCore::reported(const std::string& client_id) const {
    return std::any_of(this->clients_.begin(), this->clients_.end(), [&](const auto& entry) {
        return entry.second->reported && entry.second->info.client_id == client_id;
    });
}

// The session no longer runs on a Sentinel fallback
void ServerCore::on_rekeyed(Client& client) {
    const auto pairing = this->pairings_.find(client.info.client_id);
    if (pairing != this->pairings_.end() && pairing->second.connection == client.id) {
        pairing->second.awaiting_rekey = false;
    }
    if (std::exchange(client.info.credential_mismatch, false)) {
        this->publish_snapshot();
    }
}

void ServerCore::on_client_state(Client& client) {
    if (!client.admitted) {
        this->admit(client);
    }
    if (client.group == nullptr) {
        return;
    }
    const bool available = client.driver->session().player().available();
    // messaging.md "Server behavior when a client becomes unavailable": the first client/state is
    // an initial value, not a transition
    const bool turned_unavailable = client.available.value_or(false) && !available;
    client.available = available;
    if (turned_unavailable) {
        this->leave_group(client);
    }
    if (client.group != nullptr) {
        client.group->on_client_state(client);
    }
}

void ServerCore::on_client_leave(Client& client) {
    this->leave_group(client);
    // After the move, which clears it
    client.driver->session().set_artwork_left(true);
}

void ServerCore::leave_group(Client& client) {
    // Held: the pumps below can clear client.group
    Group& group = *client.group;
    if (group.member_count() > 1) {
        const Group& solo = this->move_to_solo_group(client);
        client.previous_group = group.id();
        client.solo_group = solo.id();
    } else {
        group.server_stop();
    }
}

void ServerCore::on_controller_request(Client& client, const ControllerRequest& request) {
    if (client.group == nullptr) {
        return;
    }
    Group& group = *client.group;
    switch (judge_command(request, group.controller_state())) {
        case CommandVerdict::IGNORE:
            return;
        case CommandVerdict::PROTOCOL_ERROR:
            client.driver->session().reject();
            client.pump();
            return;
        case CommandVerdict::ACCEPT:
            break;
    }
    // NOLINTBEGIN(bugprone-unchecked-optional-access): judge_command() checks the command and
    // its parameter
    switch (*request.command) {
        case ControllerCommand::VOLUME:
            group.apply_volume(*request.volume);
            return;
        case ControllerCommand::MUTE:
            group.apply_mute(*request.mute);
            return;
        case ControllerCommand::SWITCH:
            // messaging.md "Server behavior when a client becomes unavailable": it rejoins a
            // group only by a switch once available again
            if (client.available.value_or(true)) {
                this->switch_group(client);
            }
            return;
        default:
            break;
    }
    CommandArgs args;
    args.client_id = client.info.client_id;
    if (*request.command == ControllerCommand::SEEK) {
        args.position_ms = static_cast<uint32_t>(*request.position_ms);
    } else if (*request.command == ControllerCommand::SEEK_RELATIVE) {
        args.offset_ms = request.offset_ms;
        // controller/v1.md: the result clamped to the seekable range, where the position is
        // known: up to the host's seek_max_ms, else the track's length when known
        const MetadataState now = group.metadata_now();
        if (now.metadata.progress) {
            std::optional<uint32_t> end = group.controller_caps().seek_max_ms;
            if (!end && now.metadata.progress->track_duration_ms != 0) {
                end = now.metadata.progress->track_duration_ms;
            }
            args.position_ms = clamp_relative_seek(now.metadata.progress->track_progress_ms,
                                                   *request.offset_ms, end);
        }
    }
    const std::optional<TransportCommand> command = transport_command(*request.command);
    // NOLINTEND(bugprone-unchecked-optional-access)
    if (command) {
        this->post_event([id = group.id(), command = *command,
                          args = std::move(args)](ServerListener& listener) {
            listener.on_transport_command(id, command, args);
        });
    }
}

void ServerCore::switch_group(Client& client) {
    std::vector<SwitchGroup> groups;
    groups.reserve(this->groups_.size());
    for (const auto& [id, group] : this->groups_) {
        groups.push_back({id, group->member_count(), group->playing()});
    }
    const GroupId current = client.group->id();
    const std::optional<GroupId> previous =
        current == client.solo_group ? client.previous_group : std::nullopt;
    const SwitchTarget target =
        switch_target(groups, current, previous, client.driver->session().role_active(PLAYER_ROLE));
    if (target.group) {
        client.previous_group.reset();
        this->move_member(client, *this->groups_.at(*target.group));
    } else if (target.new_solo) {
        client.previous_group.reset();
        this->move_to_solo_group(client);
    }
}

std::optional<ApprovalScope> ServerCore::approval_of(const std::string& client_id) const {
    if (const ClientRecord* record = this->records_.find(client_id)) {
        return record->approval;
    }
    return this->settings_.approve_all_clients ? std::optional(ApprovalScope::PLAYBACK)
                                               : std::nullopt;
}

const Psk* ServerCore::long_term_psk(const std::string& client_id) const {
    const auto unsaved = this->unsaved_records_.find(client_id);
    const ClientRecord* record = unsaved != this->unsaved_records_.end()
                                     ? &unsaved->second.before
                                     : this->records_.find(client_id);
    return record != nullptr && record->long_term_psk ? &*record->long_term_psk : nullptr;
}

HandshakePsk ServerCore::handshake_psk(const std::string& client_id, bool pairing_dial) const {
    const auto pairing = this->pairing_psks_.find(client_id);
    const Psk* pairing_psk = pairing != this->pairing_psks_.end() ? &pairing->second.psk : nullptr;
    const auto operator_pairing = this->pairings_.find(client_id);
    if (pairing_dial && operator_pairing != this->pairings_.end()) {
        if (attempt_psk(operator_pairing->second.method) == PskCategory::SENTINEL) {
            return {};
        }
        if (pairing_psk != nullptr) {
            return {PskCategory::PAIRING, *pairing_psk};
        }
    }
    return select_handshake_psk(this->long_term_psk(client_id), pairing_psk);
}

ActivationDecision ServerCore::decide(const Client& client, const PskMatch& psk,
                                      bool playback) const {
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access): set from READY_FOR_ACTIVATION on
    const ClientHello& hello = *client.driver->session().client_hello();
    return decide_activation(psk, this->long_term_psk(client.info.client_id) != nullptr, hello,
                             client.info.approval, playback);
}

ActivationDecision ServerCore::decide(const Client& client, bool playback) const {
    return this->decide(client, client.driver->session().next_psk_match(), playback);
}

void ServerCore::post_notice(const Client& client, ActivationNotice notice) {
    switch (notice) {
        case ActivationNotice::APPROVAL_NEEDED:
            this->post_event([info = client.info](ServerListener& listener) {
                listener.on_approval_needed(info);
            });
            break;
        case ActivationNotice::PAIRING_REQUIRED:
            this->post_event([info = client.info](ServerListener& listener) {
                listener.on_pairing_required(info);
            });
            break;
        case ActivationNotice::CREDENTIAL_MISMATCH:
            SS_LOGI(this->log_, "%s could not use its long-term PSK and is held until paired again",
                    this->describe(client).c_str());
            this->post_event([info = client.info](ServerListener& listener) {
                listener.on_credential_mismatch(info);
            });
            break;
        case ActivationNotice::PAIRING_PSK_MISMATCH:
            SS_LOGI(this->log_, "%s could not use the pairing PSK it was referenced under",
                    this->describe(client).c_str());
            break;
        case ActivationNotice::NONE:
            break;
    }
}

ServerCore::RecordBlob::~RecordBlob() {
    secure_zero(this->bytes.data(), this->bytes.size());
}

std::string ServerCore::record_owner(const std::string& client_id) const {
    // The client_id is never logged: the client's name when connected
    if (!this->log_.enabled(LogLevel::ERROR)) {
        return {};
    }
    for (const auto& [id, client] : this->clients_) {
        if (client->active && client->info.client_id == client_id) {
            return this->describe(*client);
        }
    }
    return "a client not connected";
}

void ServerCore::save_record(const std::string& client_id, std::function<void(bool)> done) {
    const ClientRecord* record = this->records_.find(client_id);
    const std::optional<size_t> slot = this->records_.slot_of(client_id);
    auto blob = std::make_shared<RecordBlob>();
    const bool encoded = record != nullptr && encode_client_record(*record, blob->bytes);
    if (!slot || !encoded) {
        SS_LOGE(this->log_, "the record of %s cannot be saved%s",
                this->record_owner(client_id).c_str(), slot ? "" : ": the client records are full");
        this->post_error(
            {ErrorKind::PERSISTENCE_FAILED, {}, client_id, std::nullopt, std::nullopt});
        if (done) {
            done(false);
        }
        return;
    }
    this->write_record(client_id, *slot, std::move(blob), std::move(done));
}

void ServerCore::erase_record(const std::string& client_id, size_t slot) {
    // persistence_keys.h: an all-zero blob is a free slot
    this->write_record(client_id, slot, std::make_shared<RecordBlob>(), nullptr);
}

void ServerCore::write_record(const std::string& client_id, size_t slot,
                              std::shared_ptr<RecordBlob> blob, std::function<void(bool)> done) {
    if (this->provider_ == nullptr) {
        if (done) {
            done(true);
        }
        return;
    }
    // The loop, not this: the core may be destroyed before the dispatcher finishes the save, while
    // the loop outlives the dispatcher thread (Running joins it first)
    this->dispatcher_.post(
        [this, &loop = this->loop_, provider = this->provider_, listener = this->listener_,
         key = persistence_keys::client_record_key(slot), blob = std::move(blob),
         error =
             ServerError{ErrorKind::PERSISTENCE_FAILED, {}, client_id, std::nullopt, std::nullopt},
         log = this->log_, whom = this->record_owner(client_id), done = std::move(done)] {
            const bool saved = provider->save_blob(key, blob->bytes.data(), blob->bytes.size()) &&
                               provider->commit();
            if (!saved) {
                SS_LOGE(log, "saving the record of %s failed", whom.c_str());
                if (listener != nullptr) {
                    listener->on_error(error);
                }
            }
            if (done) {
                // A handler posted after the loop ended is never run, so this is alive in one
                // that runs; one run during the shutdown grace finds the core stopped
                loop.post([this, done, saved] {
                    if (!this->stopping_) {
                        done(saved);
                    }
                });
            }
        });
}

void ServerCore::refresh_record(const Client& client) {
    const ClientRecord* record = this->records_.find(client.info.client_id);
    if (record == nullptr) {
        return;
    }
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access): set from READY_FOR_ACTIVATION on
    const ClientHello& hello = *client.driver->session().client_hello();
    ClientRecord updated = *record;
    set_details(updated, hello.name, hello.unrecognized_identifiers);
    if (updated == *record) {
        return;
    }
    this->records_.set(updated);
    this->save_record(client.info.client_id);
}

void ServerCore::set_player_format(const std::string& client_id,
                                   std::optional<AudioFormat> format) {
    if (this->stopping_) {
        return;
    }
    if (format) {
        this->format_overrides_.insert_or_assign(client_id, *format);
    } else {
        this->format_overrides_.erase(client_id);
    }
    // The pump plans the switch, and the group builds the target then
    for (const auto& [id, client] : this->clients_) {
        if (client->active && client->info.client_id == client_id) {
            client->driver->session().set_format_override(format);
            client->pump();
        }
    }
}

void ServerCore::set_approval(const std::string& client_id, std::optional<ApprovalScope> approval) {
    if (this->stopping_) {
        return;
    }
    std::vector<Client*> connected;
    for (const auto& [id, client] : this->clients_) {
        if (client->active && client->info.client_id == client_id) {
            connected.push_back(client.get());
        }
    }
    const ClientRecord* existing = this->records_.find(client_id);
    ClientRecord record = existing != nullptr ? *existing : ClientRecord{client_id, std::nullopt};
    record.approval = approval;
    if (existing == nullptr && !connected.empty()) {
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access): set once the client is active
        const ClientHello& hello = *connected.front()->driver->session().client_hello();
        set_details(record, hello.name, hello.unrecognized_identifiers);
    }
    this->records_.set(record);
    const auto unsaved = this->unsaved_records_.find(client_id);
    if (unsaved != this->unsaved_records_.end()) {
        unsaved->second.approval_set = true;
    }
    // pairing.md "Unpaired Access": approval SHOULD persist
    this->save_record(client_id);
    for (Client* client : connected) {
        client->info.approval = approval_of(client_id);
    }
    if (!connected.empty()) {
        this->publish_snapshot();
    }
    for (Client* client : connected) {
        this->reactivate(*client);
    }
}

void ServerCore::store_long_term_psk(const std::string& client_id, const Psk& psk,
                                     std::function<void(bool, const UnsavedRecord&)> done) {
    if (this->stopping_) {
        return;
    }
    const ClientRecord* existing = this->records_.find(client_id);
    // One change in flight per client, so the record handshakes reference stays the durable one
    if (this->unsaved_records_.contains(client_id)) {
        done(false,
             UnsavedRecord{existing != nullptr ? *existing : ClientRecord{client_id, std::nullopt},
                           existing != nullptr, false});
        return;
    }
    ClientRecord before = existing != nullptr ? *existing : ClientRecord{client_id, std::nullopt};
    ClientRecord record = before;
    record.long_term_psk = psk;
    record.approval.reset();
    for (const auto& [id, client] : this->clients_) {
        if (existing == nullptr && client->active && client->info.client_id == client_id) {
            // NOLINTNEXTLINE(bugprone-unchecked-optional-access): set once the client is active
            const ClientHello& hello = *client->driver->session().client_hello();
            set_details(record, hello.name, hello.unrecognized_identifiers);
            break;
        }
    }
    this->unsaved_records_.emplace(client_id,
                                   UnsavedRecord{std::move(before), existing != nullptr, false});
    this->records_.set(record);
    this->save_record(client_id, [this, id = client_id, done = std::move(done)](bool saved) {
        const auto found = this->unsaved_records_.find(id);
        const UnsavedRecord unsaved = std::move(found->second);
        this->unsaved_records_.erase(found);
        if (saved) {
            const bool paired = this->long_term_psk(id) != nullptr;
            for (const auto& [key, client] : this->clients_) {
                if (client->active && client->info.client_id == id) {
                    client->info.paired = paired;
                    client->info.approval = this->approval_of(id);
                }
            }
        } else {
            this->put_back(id, unsaved);
        }
        // done may put the change back too, so the snapshot waits for it
        done(saved, unsaved);
        std::vector<Client*> connected;
        for (const auto& [key, client] : this->clients_) {
            if (client->active && client->info.client_id == id) {
                connected.push_back(client.get());
            }
        }
        if (!connected.empty()) {
            this->publish_snapshot();
        }
        // A paired client's unpaired sessions play no more (messaging.md "server/activate"),
        // unless done re-handshook them
        for (Client* client : connected) {
            this->reactivate(*client);
        }
    });
}

void ServerCore::put_back(const std::string& client_id, const UnsavedRecord& unsaved) {
    const ClientRecord* current = this->records_.find(client_id);
    if (!unsaved.existed && !unsaved.approval_set) {
        if (const std::optional<size_t> slot = this->records_.remove(client_id)) {
            this->erase_record(client_id, *slot);
        }
    } else {
        // A name saved meanwhile stays
        ClientRecord restored = current != nullptr ? *current : unsaved.before;
        restored.long_term_psk = unsaved.before.long_term_psk;
        if (!unsaved.approval_set) {
            restored.approval = unsaved.before.approval;
        }
        this->records_.set(restored);
        this->save_record(client_id);
    }
    const bool paired = this->long_term_psk(client_id) != nullptr;
    for (const auto& [id, client] : this->clients_) {
        if (client->active && client->info.client_id == client_id) {
            client->info.paired = paired;
            client->info.approval = this->approval_of(client_id);
        }
    }
    for (auto& [key, target] : this->targets_) {
        if (target.known && target.known->client_id == client_id) {
            target.known->paired = paired;
        }
    }
}

void ServerCore::rekey(const std::string& client_id, const HandshakePsk& psk) {
    for (const auto& [id, client] : this->clients_) {
        if (client->active && client->info.client_id == client_id) {
            this->reactivate(*client, psk);
        }
    }
}

void ServerCore::reactivate(Client& client, const std::optional<HandshakePsk>& rekey,
                            const std::optional<PairingParams>& pairing, bool force) {
    const PskMatch psk =
        rekey ? PskMatch{rekey->category, std::nullopt} : client.driver->session().next_psk_match();
    const ActivationDecision probe = this->decide(client, psk, false);
    const bool playable = probe.playable;
    if (client.group == nullptr) {
        return;
    }
    // An approval's scope changes the source role alone (roles/source/v1.md "Unpaired access")
    const bool roles_changed =
        playable && client.playable &&
        probe.activation.active_roles != std::optional(client.driver->session().next_roles());
    if (!rekey && !pairing && !force) {
        if (playable == client.playable && !roles_changed) {
            return;
        }
        // An activation would end the attempt; the change waits for its end
        if (this->pairing_running(client)) {
            return;
        }
    }
    // connection.md "Multiple servers": only the first activation, and one that drops 'pairing', is
    // arbitrated; this escalation or de-escalation is not. A removed role's stream/end goes first.
    Group& group = *client.group;
    Group& server_group = *this->groups_.at(this->default_group_);
    const bool changed = playable != client.playable;
    const bool to_server = changed && playable && &group != &server_group;
    const bool to_solo =
        changed && !playable && (&group == &server_group || group.member_count() > 1);
    const bool playing = to_server ? server_group.playing() : !to_solo && group.playing();
    ActivationDecision decision = this->decide(client, psk, later_activation_playback(playing));
    decision.activation.pairing = pairing;
    client.playable = decision.playable;
    client.playback = decision.activation.playback;
    client.pairing_now = pairing.has_value();
    client.declared_pairing = client.declared_pairing || pairing.has_value();
    // messaging.md "server/activate": roles that stay persist when omitted
    if (!changed && !roles_changed) {
        decision.activation.active_roles.reset();
    }
    if (rekey) {
        client.driver->session().rekey(*rekey, decision.activation);
    } else {
        client.driver->session().activate(decision.activation);
    }
    if (changed) {
        this->post_notice(client, decision.notice);
    }
    if (to_server) {
        this->move_member(client, server_group);
    } else if (to_solo) {
        this->move_to_solo_group(client);
    } else {
        if (changed || roles_changed) {
            // messaging.md "server/state": a role added gets its state promptly
            group.send_role_state(client);
        }
        client.pump();
    }
}

void ServerCore::refresh_playback(Client& client, bool group_playing) {
    const bool playback = later_activation_playback(group_playing);
    // An activation would end a pairing attempt; the attempt's end decides it
    if (!client.playable || client.playback == playback || this->pairing_running(client)) {
        return;
    }
    client.playback = playback;
    client.pairing_now = false;
    client.driver->session().activate(ServerActivate{playback, std::nullopt, std::nullopt});
}

void ServerCore::on_playback_changed(Group& group) {
    for (const auto& [id, client] : this->clients_) {
        if (client->active && client->group == &group) {
            this->refresh_playback(*client, group.playing());
        }
    }
    if (!group.playing()) {
        return;
    }
    this->log_memory("playing");
    // Playback on a group with a member not connected dials it, for playback
    for (const auto& [client_id, held] : this->held_) {
        if (held.group != group.id()) {
            continue;
        }
        if (Target* target = this->find_known(client_id)) {
            this->dial_if_due(*target);
        }
    }
}

Group& ServerCore::move_to_solo_group(Client& client) {
    Group& solo = this->new_group(client.info.name);
    this->move_member(client, solo);
    return solo;
}

void ServerCore::move_member(Client& client, Group& to) {
    Group& from = *client.group;
    from.remove_member(client);
    client.group = &to;
    // Joining a group restarts an artwork stream a client/leave ended
    client.driver->session().set_artwork_left(false);
    to.add_member(client);
    // A move changes the group whose playback the activities follow. Queued after the join, so the
    // group/update and the end of a stream go first (messaging.md "Server behavior when a client
    // becomes unavailable").
    this->refresh_playback(client, to.playing());
    client.pump();
    this->erase_if_unused(from);
}

void ServerCore::hold(const std::string& client_id, GroupId group) {
    Held& held = this->held_[client_id];
    held.group = group;
    if (!held.expiry) {
        held.expiry = std::make_unique<Timer>(this->loop_);
    }
    held.expiry->start(now_us() + this->settings_.group_rejoin_grace_us,
                       [this, id = std::string(client_id)] { this->release_held(id); });
}

void ServerCore::release_held(const std::string& client_id) {
    const auto it = this->held_.find(client_id);
    if (it == this->held_.end()) {
        return;
    }
    const GroupId group = it->second.group;
    // The expiry may be calling this, which a Timer allows
    this->held_.erase(it);
    if (Group* held = this->find_group(group)) {
        this->erase_if_unused(*held);
    }
}

Group* ServerCore::find_group(GroupId id) {
    const auto it = this->groups_.find(id);
    return it == this->groups_.end() ? nullptr : it->second.get();
}

void ServerCore::set_controller_caps(GroupId group, const ControllerCaps& caps) {
    if (Group* found = this->find_group(group)) {
        found->set_controller_caps(caps);
    }
}

void ServerCore::set_group_volume(GroupId group, uint32_t volume) {
    if (Group* found = this->find_group(group)) {
        found->apply_volume(volume);
    }
}

void ServerCore::set_group_mute(GroupId group, bool muted) {
    if (Group* found = this->find_group(group)) {
        found->apply_mute(muted);
    }
}

void ServerCore::set_metadata(GroupId group, const Metadata& metadata, int64_t at_us) {
    if (Group* found = this->find_group(group)) {
        found->set_metadata(metadata, at_us);
    }
}

void ServerCore::cancel_scheduled_metadata(GroupId group) {
    if (Group* found = this->find_group(group)) {
        found->cancel_scheduled_metadata();
    }
}

void ServerCore::set_artwork(GroupId group, ArtworkSource source,
                             std::shared_ptr<const SourceImage> image, int64_t at_us) {
    if (Group* found = this->find_group(group)) {
        found->set_artwork(source, std::move(image), at_us);
    }
}

void ServerCore::cancel_scheduled_artwork(GroupId group, ArtworkSource source) {
    if (Group* found = this->find_group(group)) {
        found->cancel_scheduled_artwork(source);
    }
}

void ServerCore::set_colors(GroupId group, const Colors& colors, int64_t at_us) {
    if (Group* found = this->find_group(group)) {
        found->set_colors(colors, at_us);
    }
}

void ServerCore::cancel_scheduled_colors(GroupId group) {
    if (Group* found = this->find_group(group)) {
        found->cancel_scheduled_colors();
    }
}

void ServerCore::on_artwork_result(ArtworkResult result) {
    if (result.job.variant && !result.encoded) {
        SS_LOGI(this->log_, "an artwork image did not decode or encode; channels show none");
    }
    this->artwork_cache_.on_result(std::move(result));
    // A pump that closes a connection erases no group here: that runs from a later handler
    for (const auto& [id, group] : this->groups_) {
        group->on_artwork_ready();
    }
}

void ServerCore::create_group(GroupId id, const std::string& name,
                              const std::string& first_member) {
    // Created for the first move into it, so a client that does not move leaves no empty group
    Group* group = nullptr;
    this->place(first_member, std::nullopt, [&]() -> Group& {
        if (group == nullptr) {
            group = &this->new_group(name, id);
        }
        return *group;
    });
}

void ServerCore::move_client(const std::string& client_id, GroupId group) {
    // Gone since the host checked groups(): nothing moves
    Group* to = this->find_group(group);
    if (to != nullptr) {
        this->place(client_id, group, [to]() -> Group& { return *to; });
    }
}

void ServerCore::place(const std::string& client_id, std::optional<GroupId> target,
                       const std::function<Group&()>& to) {
    MoveFacts facts;
    const auto held = this->held_.find(client_id);
    facts.held = held != this->held_.end();
    facts.held_in_target = facts.held && target == held->second.group;
    std::vector<Client*> connections;
    for (const auto& [id, client] : this->clients_) {
        if (client->active && client->group != nullptr && client->info.client_id == client_id) {
            connections.push_back(client.get());
        }
    }
    facts.connected = !connections.empty();
    if (move_action(facts) == MoveAction::MOVE_HELD) {
        Group& group = to();
        const GroupId from = held->second.group;
        held->second.group = group.id();
        this->report_group(group);
        if (Group* left = this->find_group(from)) {
            this->erase_if_unused(*left);
        }
        // Playback on a group with a member not connected dials it, as on_playback_changed() does
        if (group.playing()) {
            if (Target* target = this->find_known(client_id)) {
                this->dial_if_due(*target);
            }
        }
    }
    // Each connection of the client is judged on its own
    for (Client* client : connections) {
        // An earlier move's pump can close it
        if (client->group == nullptr) {
            continue;
        }
        facts.playable = client->playable;
        facts.in_target = target == client->group->id();
        if (move_action(facts) == MoveAction::MOVE) {
            this->move_member(*client, to());
        }
    }
}

void ServerCore::rename_group(GroupId group, const std::string& name) {
    if (Group* found = this->find_group(group)) {
        found->rename(name);
    }
}

Group& ServerCore::new_group(std::string name, std::optional<GroupId> id_in) {
    const GroupId id = id_in.value_or(GroupId{this->group_ids_.fetch_add(1)});
    GroupHooks hooks;
    hooks.changed = [this, id, was_playing = std::make_shared<bool>(false)] {
        const auto it = this->groups_.find(id);
        if (it == this->groups_.end()) {
            this->publish_groups();
            return;
        }
        this->report_group(*it->second);
        // The group queues markers for its members and pumps them after reporting the change
        if (it->second->playing() != *was_playing) {
            *was_playing = it->second->playing();
            this->on_playback_changed(*it->second);
        }
    };
    hooks.error = [this](const ServerError& error) { this->post_error(error); };
    // A group whose last member left while it held an input goes once the input does
    hooks.detached = [this, id] {
        this->drop_gone_routes();
        if (Group* group = this->find_group(id)) {
            this->erase_if_unused(*group);
        }
    };
    hooks.info = [this](const std::string& line) { SS_LOGI(this->log_, "%s", line.c_str()); };
    auto group = std::make_unique<Group>(this->loop_, this->workers_, id, std::move(name),
                                         this->next_instance_id_, this->settings_.resampler_quality,
                                         this->settings_.enable_opus, std::move(hooks),
                                         this->artwork_cache_);
    Group& created = *group;
    this->groups_.emplace(id, std::move(group));
    return created;
}

void ServerCore::report_group(const Group& group) {
    this->publish_groups();
    this->post_event(
        [info = group.info()](ServerListener& listener) { listener.on_group_changed(info); });
}

void ServerCore::post_error(ServerError error) {
    this->post_event(
        [error = std::move(error)](ServerListener& listener) { listener.on_error(error); });
}

void ServerCore::erase_if_unused(Group& group) {
    if (group.id() == this->default_group_) {
        return;
    }
    // From a later handler: the group may be on the stack, pumping the member that closed
    this->loop_.post([this, id = group.id()] {
        const auto it = this->groups_.find(id);
        const bool held = std::any_of(this->held_.begin(), this->held_.end(),
                                      [id](const auto& entry) { return entry.second.group == id; });
        if (this->stopping_ || it == this->groups_.end() || it->second->member_count() > 0 ||
            it->second->has_input() || held) {
            return;
        }
        this->groups_.erase(it);
        this->publish_groups();
    });
}

void ServerCore::publish_groups() {
    std::vector<GroupInfo> groups;
    groups.reserve(this->groups_.size());
    for (const auto& [id, group] : this->groups_) {
        groups.push_back(group->info());
    }
    this->groups_snapshot_.publish(std::move(groups));
}

void ServerCore::attach_input(const std::shared_ptr<HostView>& view) {
    this->inputs_.remove(*view);
    const auto it = this->groups_.find(view->group());
    // A view the host released before its attachment ran holds nothing
    if (this->stopping_ || view->gone() || it == this->groups_.end() ||
        !it->second->attach_input(view)) {
        view->close();
    }
}

void ServerCore::on_closed(Client& client) {
    this->log_memory("closed");
    const SessionDriver& driver = *client.driver;
    const bool was_active = client.active;
    const bool was_reported = client.reported;
    client.closed = true;
    // The arguments are evaluated only when the level is logged
    if (client.admitted) {
        SS_LOGI(this->log_, "%s disconnected: %s", this->describe(client).c_str(),
                end_description(driver.session(), driver.transport_close()).c_str());
    } else if (is_handshake_failure(driver.session().end())) {
        // A dial's failures are bounded by its backoff; any peer can repeat an accepted one
        SS_LOG_AT(this->log_, client.target != nullptr ? LogLevel::WARN : LogLevel::INFO,
                  "handshake with %s failed: %s", this->describe(client).c_str(),
                  end_description(driver.session(), driver.transport_close()).c_str());
    } else {
        SS_LOGI(this->log_, "%s left before activation: %s", this->describe(client).c_str(),
                end_description(driver.session(), driver.transport_close()).c_str());
    }
    if (client.target != nullptr) {
        client.target->client = nullptr;
        this->after_close(*client.target, client);
    }
    this->on_pairing_connection_closed(client);
    if (const auto route = this->routes_.find(client.info.client_id);
        route != this->routes_.end() && route->second->connection == client.id) {
        this->unbind(*route->second);
        this->refresh_source(client.info.client_id, &client);
    }
    client.active = false;
    client.reported = false;
    if (was_reported) {
        this->publish_snapshot();
        ClientDisconnect disconnect;
        disconnect.reason = disconnect_reason(driver.session().end(), driver.transport_close());
        const std::optional<ClientGoodbye>& goodbye = driver.session().goodbye();
        if (disconnect.reason == DisconnectReason::GOODBYE && goodbye) {
            disconnect.goodbye_reason = goodbye->reason;
        }
        this->post_event([info = client.info, disconnect](ServerListener& listener) {
            listener.on_client_disconnected(info, disconnect);
        });
        // Another connection of the client that took it is reported in its place
        for (const auto& [id, other] : this->clients_) {
            if (other->admitted && other->active && !other->driver->session().end() &&
                other->info.client_id == client.info.client_id) {
                this->admit(*other);
                break;
            }
        }
    } else if (!client.admitted && driver.session().end() != SessionEnd::CLOSED_BY_HOST) {
        // Ended before the client took it: a failed handshake, or a refusal such as
        // `concurrent_attempt`; the host's own close (disconnect(), cancel_connect_url()) is no
        // error
        this->post_event([error = error_before_activation(client.url, driver.session())](
                             ServerListener& listener) { listener.on_error(error); });
    }
    if (client.group != nullptr) {
        Group& group = *client.group;
        client.group = nullptr;
        group.remove_member(client);
        // A player reconnecting within the grace rejoins its group; one that said goodbye for any
        // reason but a restart is not expected back (messaging.md "client/goodbye")
        const std::optional<ClientGoodbye>& goodbye = driver.session().goodbye();
        const bool expected_back =
            driver.session().end() != SessionEnd::CLOSED_BY_HOST &&
            (!goodbye || goodbye->reason == std::optional(GoodbyeReason::RESTART));
        if (was_reported && client.playable && expected_back &&
            !this->connected(client.info.client_id)) {
            this->hold(client.info.client_id, group.id());
        }
        this->erase_if_unused(group);
    }
    if (client.target != nullptr) {
        const std::string key = client.target->key;
        this->dial_if_due(*client.target);
        this->erase_if_idle(key);
    }
    // A target held back while its client was connected through this connection
    if (was_active) {
        for (auto& [key, target] : this->targets_) {
            if (&target != client.target && target.known &&
                target.known->client_id == client.info.client_id) {
                this->dial_if_due(target);
            }
        }
    }
    if (client.target != nullptr) {
        this->start_waiting_dials();
    }
    // The driver is calling this: it is destroyed from a later handler
    this->loop_.post([this, id = client.id] { this->clients_.erase(id); });
}

void ServerCore::after_close(Target& target, const Client& client) {
    const Session& session = client.driver->session();
    const int64_t now = now_us();
    if (!client.admitted && is_handshake_failure(session.end())) {
        target.backoff.record_failure(now, this->settings_.backoff);
    }
    if (client.admitted) {
        target.known = client.info;
    }
    if (!client.active && !target.retrying) {
        // A later dial on the server's own is idle, unless it retries the operator's
        target.operator_dial = false;
    }
    ConnectionEnd end;
    end.activated = client.active;
    const std::optional<ClientGoodbye>& goodbye = session.goodbye();
    end.goodbye = goodbye.has_value();
    end.reason = goodbye ? goodbye->reason : std::nullopt;
    end.activities = client.playback      ? DeclaredActivities::PLAYBACK
                     : client.pairing_now ? DeclaredActivities::PAIRING_ONLY
                                          : DeclaredActivities::NONE;
    end.stopped_by_host = session.end() == SessionEnd::CLOSED_BY_HOST || target.host_stopped;
    switch (reconnect_after(end)) {
        case Reconnect::AGAIN:
            target.backoff.record_reconnect(now, client.activated_us, this->settings_.backoff);
            target.reconnecting = true;
            break;
        case Reconnect::UNCHANGED:
            break;
        case Reconnect::STOP:
            target.reconnecting = false;
            target.retrying = false;
            target.operator_dial = false;
            if (target.discovery) {
                target.discovery->suppressed = true;
            }
            break;
    }
}

void ServerCore::trace(const Client& client, TraceDirection direction, MessageKind kind,
                       std::span<const uint8_t> plaintext, int64_t stamp_us, size_t audio_bytes) {
    TracedMessage message;
    message.direction = direction;
    message.binary = kind == MessageKind::BINARY;
    message.stamp_us = stamp_us;
    message.plaintext.assign(plaintext.begin(), plaintext.end());
    message.audio_bytes = audio_bytes;
    this->post_trace(client, std::move(message));
}

void ServerCore::trace_written(const Client& client, int64_t done_us) {
    TracedMessage message;
    message.direction = TraceDirection::OUTBOUND;
    message.stamp_us = done_us;
    message.written = true;
    this->post_trace(client, std::move(message));
}

void ServerCore::post_trace(const Client& client, TracedMessage message) {
    message.connection_id = client.id;
    const std::optional<ClientInit>& init = client.driver->session().client_init();
    if (init) {
        message.client_id = init->client_id;
    }
    this->dispatcher_.post(
        [trace = &this->settings_.trace, message = std::move(message)] { (*trace)(message); });
}

void ServerCore::erase_if_idle(const std::string& key) {
    const auto it = this->targets_.find(key);
    if (it == this->targets_.end()) {
        return;
    }
    const Target& target = it->second;
    if (target.client != nullptr) {
        return;
    }
    if (target.discovery) {
        if (!target.discovery->present) {
            this->targets_.erase(it);
            --this->discovered_count_;
        }
        return;
    }
    // A target with failures keeps its backoff, and one that reached a client stays dialable
    if (!target.dial && !target.retry_pending && !target.waiting &&
        target.backoff.failures() == 0 && !target.known) {
        this->targets_.erase(it);
    }
}

void ServerCore::prune_targets(int64_t now_us) {
    // A target with failures is kept until a full maximum wait has passed since its last wait
    // ended, so that a host retrying just after each wait still escalates the backoff
    const int64_t max_us = this->settings_.backoff.max_us;
    std::erase_if(this->targets_, [now_us, max_us](const auto& entry) {
        const Target& target = entry.second;
        const HandshakeBackoff& backoff = target.backoff;
        return !target.discovery && !target.known && target.client == nullptr && !target.dial &&
               !target.retry_pending && !target.waiting &&
               (backoff.failures() == 0 || backoff.next_dial_us() + max_us <= now_us);
    });
}

ServerCore::Target* ServerCore::find_known(const std::string& client_id) {
    Target* found = nullptr;
    for (auto& [key, target] : this->targets_) {
        if (!target.known || target.known->client_id != client_id) {
            continue;
        }
        // One still advertised first: its address is the current one
        if (found == nullptr || (target.discovery && target.discovery->present)) {
            found = &target;
        }
    }
    return found;
}

bool ServerCore::connected(const std::string& client_id) const {
    return std::any_of(this->clients_.begin(), this->clients_.end(), [&](const auto& entry) {
        return entry.second->active && entry.second->info.client_id == client_id;
    });
}

void ServerCore::publish_snapshot() {
    std::vector<ClientInfo> clients;
    for (const auto& [id, client] : this->clients_) {
        if (client->reported) {
            clients.push_back(client->info);
            clients.back().pairing = this->pairings_.contains(client->info.client_id);
        }
    }
    const auto listed = [&clients](const std::string& client_id) {
        return std::any_of(clients.begin(), clients.end(),
                           [&](const ClientInfo& info) { return info.client_id == client_id; });
    };
    for (const auto& [key, target] : this->targets_) {
        const bool present = target.discovery && target.discovery->present;
        if (target.client != nullptr || (target.discovery && !present)) {
            continue;
        }
        ClientInfo info;
        if (target.known) {
            if (listed(target.known->client_id)) {
                continue;
            }
            info = *target.known;
        } else if (present) {
            const DiscoveredClient& service = target.discovery->service;
            info.name = service.name.value_or(service.instance);
            info.address = discovered_address(service);
        } else {
            continue;
        }
        info.url = target.url;
        info.connected = false;
        info.pairing = this->pairings_.contains(info.client_id);
        info.discovered = target.discovery.has_value();
        info.reconnecting = target.discovery ? !target.discovery->suppressed : target.reconnecting;
        clients.push_back(std::move(info));
    }
    this->snapshot_.publish(std::move(clients));
}

std::string ServerCore::describe(const Client& client) const {
    if (client.active) {
        return "\"" + printable(client.info.name) + "\" at " + client.info.address;
    }
    return client.target != nullptr ? url_for_log(client.url) : client.driver->remote_endpoint();
}

void ServerCore::shutdown(int64_t grace_us) {
    this->snapshot_.publish({});
    // A started source stops before its client is reported, as when its connection closes
    for (const auto& [client_id, route] : this->routes_) {
        const auto bound = this->clients_.find(route->connection);
        if (route->started && bound != this->clients_.end()) {
            this->report_source(*route, *bound->second, false);
        }
    }
    for (const auto& [id, client] : this->clients_) {
        if (client->reported) {
            this->post_event([info = client->info](ServerListener& listener) {
                listener.on_client_disconnected(info, {DisconnectReason::SERVER_STOPPED, {}});
            });
        }
    }
    for (const auto& [client_id, pairing] : this->pairings_) {
        this->post_pairing_result(pairing.info, PairingResult::SERVER_STOPPED);
    }
    // post_event() posts nothing after these reports, whatever closing the inputs below would
    // report
    this->stopping_ = true;
    // Pending drains resolve as stopped before the dispatch thread finishes, an input whose
    // attachment is still to run included
    this->inputs_.close_all();
    for (const auto& [id, group] : this->groups_) {
        group->close_input();
    }
    // The decoders' jobs stop; one running finishes on its thread into a closed input
    this->routes_.clear();
    // The job running finishes on the worker's thread, which the destructor joins once the loop
    // ended; its result is never run
    this->artwork_worker_.discard();
    // A long-term PSK still being saved was never finalized: its save's outcome is dropped, so
    // the change is put back on disk now, behind that save in the dispatcher's order
    for (const auto& [client_id, unsaved] : this->unsaved_records_) {
        this->put_back(client_id, unsaved);
    }
    this->unsaved_records_.clear();
    if (this->listener_socket_) {
        this->listener_socket_->close();
    }
    // Cancels dials and retries, and closes connections; none reports an event after this
    this->pairings_.clear();
    this->targets_.clear();
    this->clients_.clear();
    this->held_.clear();
    this->groups_.clear();
    this->groups_snapshot_.publish({});
    this->dispatcher_.finish();
    this->loop_.shutdown(grace_us);
}

}  // namespace sendspin::server
