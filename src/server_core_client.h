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

/// @file server_core_client.h
/// @brief ServerCore::Client, one connection as the core thread keeps it; shared by
/// server_core.cpp and server_core_pairing.cpp

#pragma once

#include "memory.h"
#include "server_core.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>

namespace sendspin::server {

// ==============================================================================
// Client
// ==============================================================================

/// One connection, from the dial's success or the accept until it closes; in PSRAM where there is
/// one
class ServerCore::Client : public SessionObserver,
                           public SessionTrace,
                           public CredentialSource,
                           public GroupMember,
                           public ExternalNew {
public:
    Client(ServerCore& core, Target* target, std::string url, uint64_t id)
        : core_(core), target(target), url(std::move(url)), id(id) {}

    void on_ready(SessionDriver& /*driver*/) override {
        this->core_.on_ready(*this);
    }

    void on_closed(SessionDriver& /*driver*/) override {
        this->core_.on_closed(*this);
    }

    void on_client_state(SessionDriver& /*driver*/) override {
        this->core_.on_client_state(*this);
    }

    void on_controller_request(SessionDriver& /*driver*/,
                               const ControllerRequest& request) override {
        this->core_.on_controller_request(*this, request);
    }

    void on_client_leave(SessionDriver& /*driver*/) override {
        if (this->group != nullptr) {
            this->core_.on_client_leave(*this);
        }
    }

    void on_rekeyed(SessionDriver& /*driver*/) override {
        this->core_.on_rekeyed(*this);
    }

    void on_pairing_message(SessionDriver& /*driver*/, const ClientPairing& message) override {
        this->core_.on_pairing_message(*this, message);
    }

    void on_source_event(SessionDriver& /*driver*/, const SourceEvent& event) override {
        this->core_.on_source_event(*this, event);
    }

    Session& session() override {
        return this->driver->session();
    }

    void pump() override {
        this->driver->pump();
    }

    const ClientInfo& client_info() const override {
        return this->info;
    }

    void on_inbound(MessageKind kind, std::span<const uint8_t> plaintext, int64_t rx_us) override {
        this->core_.trace(*this, TraceDirection::INBOUND, kind, plaintext, rx_us, 0);
    }

    void on_outbound(MessageKind kind, std::span<const uint8_t> plaintext, int64_t stamp_us,
                     size_t audio_bytes) override {
        this->core_.trace(*this, TraceDirection::OUTBOUND, kind, plaintext, stamp_us, audio_bytes);
    }

    void on_written(int64_t done_us) override {
        if (this->core_.settings_.trace_writes) {
            this->core_.trace_written(*this, done_us);
        }
    }

    HandshakePsk handshake_psk(const ClientInit& init) override {
        return this->core_.handshake_psk(init.client_id, this->pairing_dial);
    }

private:
    ServerCore& core_;

public:
    /// The target dialed; nullptr for a client that connected to the server
    Target* const target;
    const std::string url;
    const uint64_t id;
    std::unique_ptr<SessionDriver> driver;
    /// Set when the client is activated
    ClientInfo info;
    bool active{false};
    /// on_closed() ran: the connection no longer counts as in its handshake
    bool closed{false};
    /// The client took the connection: its first client/state arrived, or its first activation
    /// carried no roles, after which it need not send one (messaging.md "client/state")
    bool admitted{false};
    /// Reported with on_client_connected() and listed in clients(): admitted, and the only
    /// connection of its client_id so reported
    bool reported{false};
    /// When the client was activated
    int64_t activated_us{0};
    /// The client may play: its activations carry every implemented role
    bool playable{false};
    /// The client's group, from its activation until it closes
    Group* group{nullptr};
    /// The latest client/state's `available`; nullopt before the first
    std::optional<bool> available;
    /// The activation last queued declares `'playback'`
    bool playback{false};
    /// The server dialed this connection for the client's pairing
    bool pairing_dial{false};
    /// An activation queued on this connection declared `'pairing'`
    bool declared_pairing{false};
    /// The activation last queued declares `'pairing'`
    bool pairing_now{false};
    /// The group the client left on turning unavailable or leaving, and the group it was moved
    /// to; previous_group counts only while the client is still in solo_group (controller/v1.md
    /// "Switch command cycle")
    std::optional<GroupId> previous_group;
    GroupId solo_group;
};

}  // namespace sendspin::server
