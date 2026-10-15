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

/// @file server_core_pairing.cpp
/// @brief ServerCore's pairing: the operator's requests, the attempts on their connections, the
/// long-term PSK's save and re-handshake, and unpairing (spec pairing.md)

#include "clock.h"
#include "crypto/crypto.h"
#include "messages.h"
#include "server_core.h"
#include "server_core_client.h"

#include <utility>

namespace sendspin::server {

namespace {

std::string_view result_text(PairingResult result) {
    switch (result) {
        case PairingResult::PAIRED:
            return "paired";
        case PairingResult::CANCELLED:
            return "cancelled";
        case PairingResult::CODE_MISMATCH:
            return "the code did not match";
        case PairingResult::TOKEN_MISMATCH:
            return "the client could not use the token's pairing PSK";
        case PairingResult::METHOD_NOT_SUPPORTED:
            return "the method is not supported";
        case PairingResult::CLIENT_TIMED_OUT:
            return "the client's attempt timed out";
        case PairingResult::CLIENT_CANCELLED:
            return "cancelled on the client";
        case PairingResult::CONCURRENT_ATTEMPT:
            return "the client is busy with another server";
        case PairingResult::TIMED_OUT:
            return "the client did not start the attempt or answer in time";
        case PairingResult::PERSISTENCE_FAILED:
            return "the long-term PSK could not be saved";
        case PairingResult::PROTOCOL_ERROR:
            return "protocol error";
        case PairingResult::CONNECTION_LOST:
            return "the connection was lost";
        case PairingResult::SERVER_STOPPED:
            break;
    }
    return "the server stopped";
}

}  // namespace

ServerCore::Pairing::~Pairing() {
    secure_zero(this->code.data(), this->code.size());
}

// ==============================================================================
// Operator requests
// ==============================================================================

void ServerCore::begin_pairing(const std::string& client_id, const PairingOptions& options) {
    if (this->stopping_) {
        return;
    }
    // An operator restart needs a new pairing activation. One whose long-term PSK is being saved
    // completes.
    if (this->pairings_.contains(client_id)) {
        this->cancel_pairing(client_id);
        if (this->pairings_.contains(client_id)) {
            return;
        }
    }
    Pairing pairing;
    pairing.method = options.method;
    pairing.requested_format = options.format;
    Client* client = this->pairing_connection(client_id);
    Target* target = client == nullptr ? this->find_known(client_id) : nullptr;
    if (client != nullptr) {
        pairing.info = client->info;
    } else if (target != nullptr) {
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access): find_known() requires it
        pairing.info = *target->known;
    } else {
        ClientInfo info;
        info.client_id = client_id;
        this->post_pairing_result(info, PairingResult::CONNECTION_LOST);
        return;
    }
    SS_LOGI(this->log_, "pairing with %s",
            client != nullptr ? this->describe(*client).c_str() : "a client not connected");
    const ClientInfo info = pairing.info;
    Pairing& entry = this->pairings_.insert_or_assign(client_id, std::move(pairing)).first->second;
    this->publish_snapshot();
    const bool token = this->pairing_psks_.contains(client_id);
    // The operator enters what the method needs: the token before anything is sent, a static
    // code at any time
    if ((options.method == PairMethod::PAIRING_PSK && !token) ||
        options.method == PairMethod::STATIC_PAIRING_CODE) {
        this->post_code_needed(info, options.method, std::nullopt);
    }
    if (options.method == PairMethod::PAIRING_PSK && !token) {
        return;
    }
    if (client != nullptr) {
        this->start_attempt(*client, entry);
        return;
    }
    this->dial_for_pairing(client_id, entry);
}

bool ServerCore::dial_for_pairing(const std::string& client_id, Pairing& pairing) {
    Target* target = this->find_known(client_id);
    if (target == nullptr) {
        return false;
    }
    // A dial for pairing; operator actions always dial
    target->pairing_dial = true;
    target->operator_dial = true;
    if (target->discovery) {
        target->discovery->suppressed = false;
    }
    pairing.dialed = true;
    pairing.connection = 0;
    // A dial that never completes, or waits out a backoff, does not hold the pairing forever
    this->arm_pairing_timer(client_id, 0, std::nullopt, this->settings_.pairing_start_timeout_us);
    this->schedule_dial(*target);
    return true;
}

void ServerCore::arm_pairing_timer(const std::string& client_id, uint64_t connection,
                                   std::optional<PairingStep> step, int64_t timeout_us) {
    const auto it = this->pairings_.find(client_id);
    if (it == this->pairings_.end()) {
        return;
    }
    Pairing& pairing = it->second;
    pairing.start_timer = std::make_unique<Timer>(this->loop_);
    pairing.start_timer->start(
        now_us() + timeout_us, [this, client_id = client_id, connection, step] {
            const auto found = this->pairings_.find(client_id);
            if (found == this->pairings_.end() || found->second.connection != connection) {
                return;
            }
            const Pairing& current = found->second;
            const std::optional<PairingStep> now =
                current.attempt ? std::optional(current.attempt->step()) : std::nullopt;
            if (now == step) {
                this->end_pairing(client_id, PairingResult::TIMED_OUT, step.has_value());
            }
        });
}

void ServerCore::submit_pairing_token(const std::string& client_id, const PairingPskToken& token) {
    if (this->stopping_) {
        return;
    }
    const auto it = this->pairings_.find(client_id);
    const bool waiting = it != this->pairings_.end() &&
                         it->second.method == PairMethod::PAIRING_PSK && !it->second.attempt &&
                         !this->pairing_psks_.contains(client_id);
    if (!waiting && it != this->pairings_.end()) {
        this->cancel_pairing(client_id);
        if (this->pairings_.contains(client_id)) {
            return;
        }
    }
    this->pairing_psks_.insert_or_assign(client_id,
                                         HandshakePsk{PskCategory::PAIRING, token.pairing_psk});
    if (!waiting) {
        this->begin_pairing(client_id, PairingOptions{PairMethod::PAIRING_PSK, std::nullopt});
        return;
    }
    Pairing& pairing = this->pairings_.at(client_id);
    if (Client* client = this->pairing_connection(client_id)) {
        this->start_attempt(*client, pairing);
        return;
    }
    if (!this->dial_for_pairing(client_id, pairing)) {
        this->end_pairing(client_id, PairingResult::CONNECTION_LOST, false);
    }
}

void ServerCore::submit_pairing_code(const std::string& client_id, const std::string& code) {
    const auto it = this->pairings_.find(client_id);
    if (this->stopping_ || it == this->pairings_.end() ||
        it->second.method == PairMethod::PAIRING_PSK) {
        return;
    }
    Pairing& pairing = it->second;
    const std::optional<CodeFormat> format =
        pairing.attempt ? pairing.attempt->format() : pairing.requested_format;
    std::optional<std::vector<uint8_t>> prs = code_prs(code, pairing.method, format);
    if (!prs) {
        // A typo: ask again
        this->post_code_needed(pairing.info, pairing.method, format);
        return;
    }
    if (!pairing.attempt) {
        // A static code is the device's, so it may come first; a dynamic one cannot
        if (pairing.method == PairMethod::STATIC_PAIRING_CODE) {
            secure_zero(pairing.code.data(), pairing.code.size());
            pairing.code = std::move(*prs);
        } else {
            secure_zero(prs->data(), prs->size());
        }
        return;
    }
    pairing.attempt->set_code(std::move(*prs));
    const auto client = this->clients_.find(pairing.connection);
    if (client != this->clients_.end() && client->second->active && !client->second->closed) {
        this->start_round(*client->second, pairing);
    }
}

void ServerCore::cancel_pairing(const std::string& client_id) {
    const auto it = this->pairings_.find(client_id);
    if (this->stopping_ || it == this->pairings_.end()) {
        return;
    }
    const Pairing& pairing = it->second;
    // The record is being saved: the pairing completes
    if (pairing.attempt && pairing.attempt->step() == PairingStep::PERSISTING) {
        return;
    }
    // pairing.md "Entering and leaving pairing": pair/abort `user_cancelled` first, so the
    // client can say why the attempt ended
    if (pairing.attempt) {
        const auto client = this->clients_.find(pairing.connection);
        if (client != this->clients_.end() && client->second->active) {
            client->second->driver->session().queue_json_message(
                build_pair_abort(PairAbortReason::USER_CANCELLED));
        }
    }
    this->end_pairing(client_id, PairingResult::CANCELLED, true);
}

// messaging.md "server/unpair": the server removes its pairing record; a paired session gets
// server/unpair, which the client answers by removing its own and leaving with `unpaired`
void ServerCore::unpair(const std::string& client_id) {
    if (this->stopping_) {
        return;
    }
    this->cancel_pairing(client_id);
    const ClientRecord* record = this->records_.find(client_id);
    if (record == nullptr || !record->long_term_psk) {
        return;
    }
    // One change in flight per client: the record handshakes reference stays the durable one
    if (this->unsaved_records_.contains(client_id)) {
        SS_LOGW(this->log_, "%s is not unpaired: its long-term PSK is being saved",
                this->record_owner(client_id).c_str());
        return;
    }
    ClientRecord updated = *record;
    updated.long_term_psk.reset();
    if (updated.approval) {
        this->records_.set(updated);
        this->save_record(client_id);
    } else if (const std::optional<size_t> slot = this->records_.remove(client_id)) {
        this->erase_record(client_id, *slot);
    }
    std::vector<Client*> connected;
    this->for_each_connection(client_id, [&connected](Client& client) {
        client.info.paired = false;
        connected.push_back(&client);
    });
    this->for_each_known(client_id, [](Target& target) { target.known->paired = false; });
    this->publish_snapshot();
    for (Client* client : connected) {
        Session& session = client->driver->session();
        if (session.next_psk_match().category == PskCategory::LONG_TERM) {
            session.queue_json_message(build_server_unpair());
            client->pump();
        } else {
            // An unpaired session of a client no longer paired may play (connection.md "Sentinel
            // Fallback")
            this->reactivate(*client);
        }
    }
}

// ==============================================================================
// The attempt
// ==============================================================================

ServerCore::Client* ServerCore::pairing_connection(const std::string& client_id) {
    Client* found = nullptr;
    this->for_each_connection(client_id, [&found](Client& client) {
        if (!client.closed && (found == nullptr || client.reported)) {
            found = &client;
        }
    });
    return found;
}

bool ServerCore::pairing_running(const Client& client) const {
    const auto it = this->pairings_.find(client.info.client_id);
    return it != this->pairings_.end() && it->second.connection == client.id &&
           it->second.attempt != nullptr;
}

void ServerCore::start_attempt(Client& client, Pairing& pairing,
                               std::optional<ServerActivate> first) {
    const std::string client_id = client.info.client_id;
    std::optional<CodeFormat> format;
    if (pairing.method == PairMethod::DYNAMIC_PAIRING_CODE) {
        format = dynamic_code_format(client.info.pairing_offers, pairing.requested_format,
                                     this->settings_.operator_can_scan_qr);
    }
    // messaging.md "server/activate": the method and format are drawn from the client's offer
    if (!offers_method(client.info.pairing_offers, pairing.method) ||
        (pairing.method == PairMethod::DYNAMIC_PAIRING_CODE && !format)) {
        if (first) {
            client.driver->activate(*first);
        }
        this->end_pairing(client_id, PairingResult::METHOD_NOT_SUPPORTED, false);
        return;
    }
    Session& session = client.driver->session();
    const PskCategory needed = attempt_psk(pairing.method);
    const bool rekey = !first && session.next_psk_match().category != needed;
    pairing.connection = client.id;
    pairing.info = client.info;
    pairing.retry_dial = false;
    // pairing.md "Pairing index": the activation's index once written; a re-handshake restarts
    // the count
    const uint32_t index = rekey ? 1 : session.queued_pairing_index() + 1;
    pairing.attempt =
        std::make_unique<PairingAttempt>(pairing.method, format, index, client.declared_pairing);
    if (!pairing.code.empty()) {
        pairing.attempt->set_code(std::move(pairing.code));
        pairing.code.clear();
    }
    const PairingParams params{pairing.method, format};
    // pairing.md "Entering and leaving pairing": the server's own wait for client/pair-init.
    // Armed first: an activation can close the connection at once.
    this->arm_pairing_timer(client_id, client.id, PairingStep::AWAIT_INIT,
                            this->settings_.pairing_start_timeout_us);
    if (first) {
        // A paired client dialed for pairing goes back to its long-term PSK when the pairing does
        // not complete, unless it lost its record
        pairing.left_long_term =
            this->long_term_psk(client_id) != nullptr &&
            session.psk_match().fallback_from != std::optional(PskCategory::LONG_TERM);
        first->pairing = params;
        client.pairing_now = true;
        client.declared_pairing = true;
        client.driver->activate(*first);
    } else if (rekey) {
        // The attempt runs under its method's PSK, the pairing PSK or the Sentinel
        pairing.left_long_term = session.next_psk_match().category == PskCategory::LONG_TERM;
        pairing.awaiting_rekey = true;
        const HandshakePsk psk =
            needed == PskCategory::PAIRING ? this->pairing_psks_.at(client_id) : HandshakePsk{};
        this->reactivate(client, psk, params);
    } else {
        this->reactivate(client, std::nullopt, params);
    }
    client.pump();
}

void ServerCore::start_round(Client& client, Pairing& pairing) {
    PairingAttempt& attempt = *pairing.attempt;
    if (attempt.step() != PairingStep::AWAIT_CODE || !attempt.has_code()) {
        return;
    }
    const std::optional<std::string> auth = attempt.start_round();
    if (!auth) {
        SS_LOGE(this->log_, "pairing with %s failed: CPace could not start",
                this->describe(client).c_str());
        this->end_pairing(client.info.client_id, PairingResult::CANCELLED, true);
        return;
    }
    client.driver->session().queue_json_message(*auth);
    this->arm_pairing_timer(client.info.client_id, client.id, PairingStep::AWAIT_AUTH,
                            this->settings_.pairing_client_step_timeout_us);
    client.pump();
}

void ServerCore::on_pairing_message(Client& client, const ClientPairing& message) {
    if (!client.active) {
        return;
    }
    const std::string client_id = client.info.client_id;
    Session& session = client.driver->session();
    const auto it = this->pairings_.find(client_id);
    Pairing* pairing =
        it != this->pairings_.end() && it->second.connection == client.id && it->second.attempt
            ? &it->second
            : nullptr;
    PairingContext ctx;
    ctx.written_index = session.pairing_index();
    ctx.declared_before = client.declared_pairing;
    if (pairing != nullptr) {
        ctx = pairing->attempt->context(session.pairing_index(), session.rekey_pending());
    }
    switch (judge_pairing_message(ctx, message)) {
        case PairingVerdict::DISCARD:
            return;
        case PairingVerdict::PROTOCOL_ERROR:
            this->reject_pairing(client);
            return;
        case PairingVerdict::ACCEPT:
            break;
    }
    // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage): ACCEPT needs an attempt
    PairingAttempt& attempt = *pairing->attempt;
    switch (message.kind) {
        case PairingMessageKind::PENDING: {
            attempt.note_pending();
            ClientInfo info = client.info;
            info.pairing = true;
            this->post_event(
                [info = std::move(info), text = message.message.value_or(std::string{})](
                    ServerListener& listener) { listener.on_pairing_pending(info, text); });
            break;
        }
        case PairingMessageKind::INIT: {
            // The operator's step has no timeout: the client's attempt timeout bounds it
            pairing->start_timer.reset();
            const std::optional<std::string> init =
                attempt.on_init(message, session.handshake_hash());
            if (init) {
                session.queue_json_message(*init);
                this->post_code_needed(client.info, attempt.method(), attempt.format());
            } else {
                this->start_round(client, *pairing);
            }
            break;
        }
        case PairingMessageKind::AUTH: {
            const std::optional<std::string> confirm = attempt.on_auth(message);
            if (!confirm) {
                this->reject_pairing(client);
                return;
            }
            session.queue_json_message(*confirm);
            this->arm_pairing_timer(client_id, client.id, PairingStep::AWAIT_CONFIRM,
                                    this->settings_.pairing_client_step_timeout_us);
            break;
        }
        case PairingMessageKind::RETRY:
            pairing->start_timer.reset();
            session.queue_json_message(attempt.on_retry());
            this->post_code_needed(client.info, attempt.method(), attempt.format());
            break;
        case PairingMessageKind::CONFIRM:
            switch (attempt.on_confirm(message, session.suite())) {
                case PairingAttempt::Confirmation::VERIFIED:
                    this->arm_pairing_timer(client_id, client.id, PairingStep::AWAIT_FINALIZE,
                                            this->settings_.pairing_client_step_timeout_us);
                    break;
                case PairingAttempt::Confirmation::CODE_MISMATCH:
                    // pairing.md "Server verification": the attempt ends; another needs a new
                    // pairing activation
                    session.queue_json_message(
                        build_pair_abort(PairAbortReason::PAIRING_CODE_MISMATCH));
                    this->end_pairing(client_id, PairingResult::CODE_MISMATCH, true);
                    return;
                case PairingAttempt::Confirmation::PROTOCOL_ERROR:
                    this->reject_pairing(client);
                    return;
            }
            break;
        case PairingMessageKind::FINALIZE: {
            const std::optional<HandshakePsk> psk = attempt.on_finalize(message, session.suite());
            if (!psk) {
                this->reject_pairing(client);
                return;
            }
            this->finalize_pairing(client, *psk);
            return;
        }
        case PairingMessageKind::ABORT: {
            const PairingResult result = result_of_abort(message.abort_reason);
            if (attempt.step() == PairingStep::PERSISTING) {
                pairing->late_abort = result;
                return;
            }
            // pairing.md "pair/abort": with `concurrent_attempt` the client closes the connection.
            // A pairing-only connection it refused is dialed again after the reconnect wait, within
            // the pairing's timeout.
            if (message.abort_reason == PairAbortReason::CONCURRENT_ATTEMPT &&
                client.pairing_dial && client.target != nullptr) {
                pairing->attempt.reset();
                pairing->retry_dial = true;
                pairing->connection = 0;
                pairing->left_long_term = false;
                client.target->pairing_dial = true;
                this->arm_pairing_timer(client_id, 0, std::nullopt,
                                        this->settings_.pairing_start_timeout_us);
                return;
            }
            this->end_pairing(client_id, result,
                              message.abort_reason != PairAbortReason::CONCURRENT_ATTEMPT);
            return;
        }
    }
    client.pump();
}

// pairing.md "server/pair-finalize": sent only once the record is persisted, then the
// re-handshake to the new long-term PSK
void ServerCore::finalize_pairing(Client& client, const HandshakePsk& psk) {
    const std::string client_id = client.info.client_id;
    this->store_long_term_psk(
        client_id, psk.psk,
        [this, client_id = client_id, connection = client.id, psk = psk](
            bool saved, const UnsavedRecord& unsaved) {
            const auto it = this->pairings_.find(client_id);
            if (it == this->pairings_.end() || it->second.connection != connection) {
                return;
            }
            const auto found = this->clients_.find(connection);
            Client* client =
                found != this->clients_.end() && found->second->active && !found->second->closed
                    ? found->second.get()
                    : nullptr;
            if (client == nullptr || !saved || it->second.late_abort) {
                // pairing.md "Entering and leaving pairing": an attempt that ends without
                // server/pair-finalize persists nothing and discards the PSK; a failed save
                // already put the record back
                if (saved) {
                    this->put_back(client_id, unsaved);
                }
                const PairingResult result = client == nullptr ? PairingResult::CONNECTION_LOST
                                             : !saved          ? PairingResult::PERSISTENCE_FAILED
                                                               : *it->second.late_abort;
                // An activation in place of server/pair-finalize, behind a re-handshake back to
                // the long-term PSK the attempt left
                this->end_pairing(client_id, result, client != nullptr);
                return;
            }
            client->driver->session().queue_json_message(build_server_pair_finalize());
            this->release_pairing_dial(client_id, it->second);
            this->pairings_.erase(it);
            this->pairing_psks_.erase(client_id);
            SS_LOGI(this->log_, "%s is paired", this->describe(*client).c_str());
            // pairing.md "Pairing": the server re-handshakes to the new long-term PSK, then sends
            // server/activate
            this->rekey(client_id, psk);
            this->publish_snapshot();
            this->post_pairing_result(client->info, PairingResult::PAIRED);
            client->pump();
        });
}

void ServerCore::release_pairing_dial(const std::string& client_id, const Pairing& pairing) {
    // A dial for this pairing not made yet is not one for pairing any more
    if (!pairing.dialed) {
        return;
    }
    if (Target* target = this->find_known(client_id)) {
        target->pairing_dial = false;
        target->operator_dial = false;
    }
}

void ServerCore::reject_pairing(Client& client) {
    // Only the attempt's own connection ends it: another connection of the client may break the
    // protocol without touching the attempt
    const auto it = this->pairings_.find(client.info.client_id);
    if (it != this->pairings_.end() && it->second.connection == client.id) {
        this->end_pairing(client.info.client_id, PairingResult::PROTOCOL_ERROR, false);
    }
    client.driver->session().reject();
    client.pump();
}

void ServerCore::end_pairing(const std::string& client_id, PairingResult result, bool leave) {
    const auto it = this->pairings_.find(client_id);
    if (it == this->pairings_.end()) {
        return;
    }
    Pairing pairing = std::move(it->second);
    this->pairings_.erase(it);
    this->pairing_psks_.erase(client_id);
    this->release_pairing_dial(client_id, pairing);
    ClientInfo info = pairing.info;
    const auto found =
        pairing.connection != 0 ? this->clients_.find(pairing.connection) : this->clients_.end();
    if (found != this->clients_.end() && found->second->active && !found->second->closed) {
        Client& client = *found->second;
        info = client.info;
        if (leave && pairing.attempt) {
            // Back to the long-term PSK the attempt left, else an activation without `'pairing'`,
            // which a client keeping another server's playback arbitrates (connection.md "Multiple
            // servers (server-initiated)")
            const Psk* long_term = this->long_term_psk(client_id);
            if (pairing.left_long_term && long_term != nullptr) {
                this->reactivate(client, HandshakePsk{PskCategory::LONG_TERM, *long_term});
            } else {
                this->reactivate(client, std::nullopt, std::nullopt, true);
            }
            client.pump();
        }
    }
    if (result != PairingResult::PAIRED) {
        SS_LOGI(this->log_, "pairing with \"%s\" ended: %s", printable(info.name).c_str(),
                std::string(result_text(result)).c_str());
    }
    this->publish_snapshot();
    this->post_pairing_result(info, result);
}

void ServerCore::on_pairing_connection_closed(Client& client) {
    std::string client_id = client.info.client_id;
    if (client_id.empty() && client.pairing_dial && client.target != nullptr &&
        client.target->known) {
        client_id = client.target->known->client_id;
    }
    const auto it = this->pairings_.find(client_id);
    if (client_id.empty() || it == this->pairings_.end()) {
        return;
    }
    Pairing& pairing = it->second;
    const bool ours = pairing.connection == client.id;
    if (!ours && (pairing.connection != 0 || !client.pairing_dial)) {
        return;
    }
    // The save reports its outcome
    if (ours && pairing.attempt && pairing.attempt->step() == PairingStep::PERSISTING) {
        return;
    }
    // A pairing-only connection refused with `concurrent_attempt` is dialed again after the
    // reconnect wait
    if (pairing.retry_dial && client.target != nullptr) {
        Target& target = *client.target;
        target.backoff.record_reconnect(now_us(), client.activated_us, this->settings_.backoff);
        target.reconnecting = true;
        target.pairing_dial = true;
        if (target.discovery) {
            target.discovery->suppressed = false;
        }
        this->dial_if_due(target);
        return;
    }
    // A client that refuses the re-handshake to the attempt's PSK is dialed again, once,
    // referencing that PSK in the initial handshake
    if (ours && pairing.awaiting_rekey && !pairing.redialed && client.target != nullptr) {
        SS_LOGI(this->log_, "%s closed the re-handshake for pairing; dialing it again",
                this->describe(client).c_str());
        pairing.redialed = true;
        pairing.awaiting_rekey = false;
        pairing.left_long_term = false;
        pairing.attempt.reset();
        this->dial_for_pairing(client_id, pairing);
        return;
    }
    const std::optional<SessionEnd> end = client.driver->session().end();
    this->end_pairing(client_id,
                      end == std::optional(SessionEnd::PROTOCOL_ERROR)
                          ? PairingResult::PROTOCOL_ERROR
                          : PairingResult::CONNECTION_LOST,
                      false);
}

void ServerCore::post_pairing_result(ClientInfo info, PairingResult result) {
    // As clients() shows the client once the pairing ended
    info.pairing = false;
    this->post_event([info = std::move(info), result](ServerListener& listener) {
        listener.on_pairing_result(info, result);
    });
}

void ServerCore::post_code_needed(ClientInfo info, PairMethod method,
                                  std::optional<CodeFormat> format) {
    info.pairing = true;
    this->post_event([info = std::move(info), method,
                      format = code_format_for(method, format)](ServerListener& listener) {
        listener.on_pairing_code_needed(info, method, format);
    });
}

}  // namespace sendspin::server
