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

/// @file discovery.h
/// @brief The mDNS service discovery the server advertises and browses through (spec
/// connection.md "Establishing a Connection"): the built-in provider, or one the host supplies
/// in ServerConfig::discovery

#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace sendspin::server {

/// @brief The `_sendspin-server._tcp` service to advertise
struct AdvertisedServer {
    /// The service instance name, at most 63 bytes (RFC 6763 section 4.1.1)
    std::string instance;
    uint16_t port{0};
    /// TXT `path`
    std::string path;
    /// TXT `name`; omitted when empty
    std::string name;
};

/// @brief What a provider is started for
struct DiscoveryRequest {
    /// Set to advertise the server
    std::optional<AdvertisedServer> advertise;
    /// Browse `_sendspin._tcp` and resolve every instance found
    bool browse{false};
};

/// @brief A resolved `_sendspin._tcp` instance
struct DiscoveredClient {
    /// The service instance name, which identifies the client while it advertises
    std::string instance;
    /// Literal IPv4 or IPv6 addresses, an IPv6 link-local one with its `%` zone, to try in order
    std::vector<std::string> addresses;
    /// The SRV target host name, for the WebSocket upgrade's Host header; empty for none
    std::string host;
    uint16_t port{0};
    /// TXT `path`, which a client must advertise; one without it is not reported
    std::string path;
    /// TXT `name`, a hint the client/hello `name` overrides
    std::optional<std::string> name;
};

/// @brief Receives a provider's browse results. Any thread; the server posts each to its core
/// thread, so a call returns at once.
class DiscoverySink {
public:
    virtual ~DiscoverySink() = default;

    /// @brief An instance resolved, or resolved again with changed values
    virtual void on_client_found(const DiscoveredClient& client) = 0;

    /// @brief An instance reported found stopped advertising
    virtual void on_client_lost(const std::string& instance) = 0;

    /// @brief The system's mDNS service cannot be reached; report every instance found as lost
    /// first. Reported to the host as ErrorKind::DISCOVERY_UNAVAILABLE.
    virtual void on_unavailable() = 0;

    /// @brief The system's mDNS service can be reached again after on_unavailable(); logged
    virtual void on_available() = 0;
};

/// @brief Advertises and browses for one run of the server. start() is called from
/// SendspinServer::start(), and stop() once after a successful start(), from the server's core
/// thread as it stops; a provider may be started again after stop() for a later run.
class DiscoveryProvider {
public:
    virtual ~DiscoveryProvider() = default;

    /// @brief Starts what request asks for, reporting browse results to sink until stop()
    /// returns
    /// @return false when it cannot, which fails SendspinServer::start() with
    /// StartResult::DISCOVERY_FAILED for a provider from ServerConfig::discovery
    virtual bool start(const DiscoveryRequest& request, DiscoverySink& sink) = 0;

    /// @brief Withdraws the advertisement and stops browsing; once it returns, the sink is not
    /// called again
    virtual void stop() = 0;
};

/// @brief The built-in provider: dns_sd on macOS, avahi-client on Linux, each with a thread of its
/// own. It starts without the system's mDNS service, reporting on_unavailable(), and keeps
/// trying until it can reach it. nullptr when the library is built without discovery.
std::shared_ptr<DiscoveryProvider> platform_discovery();

}  // namespace sendspin::server
