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

/// @file session_driver.h
/// @brief Runs a Session over a Connection: feeds it inbound messages and its deadlines, writes
/// its outbound messages one at a time under the transport's write invariant, and closes the
/// connection once the session ends and its last message is written. Core thread only.

#pragma once

#include "memory.h"
#include "session.h"
#include "transport.h"

#include <optional>
#include <string>

namespace sendspin::server {

class SessionDriver;

/// @brief Receives a driver's events on the core thread. A call must not destroy the driver.
class SessionObserver {
public:
    virtual ~SessionObserver() = default;

    /// @brief The session reached SessionPhase::READY_FOR_ACTIVATION; activate() may be called
    virtual void on_ready(SessionDriver& driver) = 0;

    /// @brief The connection is closed: the session ended (Session::end()) or the transport
    /// failed (transport_close()). The last event.
    virtual void on_closed(SessionDriver& driver) = 0;

    /// @brief The session took in a client/state
    virtual void on_client_state(SessionDriver& /*driver*/) {}

    /// @brief The session took in a client/command controller object for the active role
    virtual void on_controller_request(SessionDriver& /*driver*/,
                                       const ControllerRequest& /*request*/) {}

    /// @brief The session took in a client/leave
    virtual void on_client_leave(SessionDriver& /*driver*/) {}

    /// @brief A re-handshake completed: Session::psk_match() is the PSK it switched to
    virtual void on_rekeyed(SessionDriver& /*driver*/) {}

    /// @brief The session took in a pairing message
    virtual void on_pairing_message(SessionDriver& /*driver*/, const ClientPairing& /*message*/) {}

    /// @brief The session's source role changed (Session::take_source_events()), reported as the
    /// driver next pumps. A call must not pump this driver.
    virtual void on_source_event(SessionDriver& /*driver*/, const SourceEvent& /*event*/) {}
};

/// In PSRAM where there is one, with its Session
class SessionDriver : private ConnectionHandler, public ExternalNew {
public:
    /// @param identity, settings, observer must outlive the driver
    /// @param trace nullptr, or a sink that outlives the driver; told of each completed write too
    /// @param credentials as Session's
    SessionDriver(EventLoop& loop, Connection connection, const Identity& identity,
                  const SessionSettings& settings, SessionObserver& observer,
                  SessionTrace* trace = nullptr, CredentialSource* credentials = nullptr);
    ~SessionDriver() override = default;
    SessionDriver(const SessionDriver&) = delete;
    SessionDriver& operator=(const SessionDriver&) = delete;

    /// @brief Starts reading and the session's first timeout. Called once.
    void start();

    /// @brief Session::activate(), then writes the activation. Only after on_ready().
    void activate(const ServerActivate& activation);

    const Session& session() const {
        return this->session_;
    }

    /// @brief For changes from outside the driver's own events: queue through it, then pump()
    Session& session() {
        return this->session_;
    }

    /// @brief Acts on the session's state: asks for room when a message is ready, closes an ended
    /// session, and arms the timer for the next deadline
    void pump();

    /// @brief Why the transport closed the connection, when it did so before the session ended
    std::optional<CloseReason> transport_close() const {
        return this->transport_close_;
    }

    bool is_closed() const {
        return this->closed_;
    }

    /// @brief Connection::remote_endpoint()
    const std::string& remote_endpoint() const {
        return this->connection_.remote_endpoint();
    }

private:
    void on_message(MessageKind kind, std::span<const uint8_t> payload, int64_t rx_us) override;
    void on_writable() override;
    void on_write_done() override;
    void on_closed(CloseReason reason) override;

    void on_timer();

    Connection connection_;
    Timer timer_;
    Session session_;
    SessionObserver& observer_;
    SessionTrace* trace_;
    std::optional<CloseReason> transport_close_;
    std::optional<int64_t> armed_deadline_us_;
    bool waiting_{false};
    bool writing_{false};
    bool ready_reported_{false};
    bool closed_{false};
};

}  // namespace sendspin::server
