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

#include "session_driver.h"

#include "clock.h"

#include <utility>

namespace sendspin::server {

SessionDriver::SessionDriver(EventLoop& loop, Connection connection, const Identity& identity,
                             const SessionSettings& settings, SessionObserver& observer,
                             SessionTrace* trace, CredentialSource* credentials)
    : connection_(std::move(connection)),
      timer_(loop),
      session_(identity, settings, trace, credentials),
      observer_(observer),
      trace_(trace) {}

void SessionDriver::start() {
    this->connection_.start(*this);
    this->session_.start(now_us());
    this->pump();
}

void SessionDriver::activate(const ServerActivate& activation) {
    this->session_.activate(activation);
    this->pump();
}

void SessionDriver::on_message(MessageKind kind, std::span<const uint8_t> payload, int64_t rx_us) {
    this->session_.on_message(kind, payload, rx_us);
    if (this->session_.take_state_changed()) {
        this->observer_.on_client_state(*this);
        this->session_.queue_state_marker();
    }
    for (const ControllerRequest& request : this->session_.take_commands()) {
        this->observer_.on_controller_request(*this, request);
    }
    if (this->session_.take_leave()) {
        this->observer_.on_client_leave(*this);
        // Behind what the observer queues for the leave, so its group/update goes first
        // (messaging.md "Server behavior when a client becomes unavailable")
        this->session_.queue_marker(std::nullopt);
    }
    if (this->session_.take_rekeyed()) {
        this->observer_.on_rekeyed(*this);
    }
    for (const ClientPairing& message : this->session_.take_pairing_messages()) {
        this->observer_.on_pairing_message(*this, message);
    }
    this->pump();
}

void SessionDriver::on_writable() {
    this->waiting_ = false;
    // Every message ready now, in select_next() order, while the write takes them
    while (this->connection_.can_write()) {
        std::optional<OutboundMessage> message = this->session_.take_message();
        if (!message) {
            break;
        }
        if (this->connection_.write(message->kind, message->bytes)) {
            this->writing_ = true;
        }
        if (message->time_reply) {
            message->debug_trace.batched_us = now_us();
            this->connection_.debug_time_reply(message->debug_trace);
            break;
        }
    }
    this->pump();
}

void SessionDriver::on_write_done() {
    this->writing_ = false;
    if (this->trace_ != nullptr) {
        this->trace_->on_written(now_us());
    }
    this->pump();
}

void SessionDriver::on_closed(CloseReason reason) {
    this->transport_close_ = reason;
    this->closed_ = true;
    this->timer_.cancel();
    this->observer_.on_closed(*this);
}

void SessionDriver::on_timer() {
    this->armed_deadline_us_.reset();
    this->session_.on_deadline(now_us());
    this->pump();
}

void SessionDriver::pump() {
    if (this->closed_) {
        return;
    }
    // First, so that what the observer queues for them is pumped below
    for (const SourceEvent& event : this->session_.take_source_events()) {
        this->observer_.on_source_event(*this, event);
    }
    // One evaluation for both questions the pump asks: the session's state changes only through
    // what this pump does after it, waiting for room or arming the timer
    std::optional<int64_t> wake;
    if (!this->waiting_ && !this->writing_) {
        const Session::Poll poll = this->session_.poll();
        wake = poll.wake_us;
        if (poll.ready) {
            this->waiting_ = this->connection_.wait_writable();
        }
    }

    const SessionPhase phase = this->session_.phase();
    if (phase == SessionPhase::CLOSED) {
        // A silent end has nothing queued and closes even while a wait for room is outstanding
        if (this->writing_ || this->session_.has_message()) {
            return;
        }
        this->closed_ = true;
        this->timer_.cancel();
        this->connection_.close(close_code::NORMAL);
        this->observer_.on_closed(*this);
        return;
    }

    // Armed whenever the deadline changes; a call that finds the deadline ahead arms it again.
    // While a wait or a write is outstanding only the handshake deadline applies: everything else
    // is evaluated again when it completes. Neither started since the poll above when neither is
    // outstanding here, so its wake applies.
    std::optional<int64_t> deadline = this->session_.deadline_us();
    if (!this->waiting_ && !this->writing_) {
        if (wake && (!deadline || *wake < *deadline)) {
            deadline = wake;
        }
    }
    if (deadline != this->armed_deadline_us_) {
        this->armed_deadline_us_ = deadline;
        if (deadline) {
            this->timer_.start(*deadline, [this] { this->on_timer(); });
        } else {
            this->timer_.cancel();
        }
    }

    if (phase == SessionPhase::READY_FOR_ACTIVATION && !this->ready_reported_) {
        this->ready_reported_ = true;
        this->observer_.on_ready(*this);
    }
}

}  // namespace sendspin::server
