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

/// @file dns_sd_discovery.cpp
/// @brief platform_discovery() on macOS: the system's dns_sd API over one shared connection to
/// mDNSResponder, served by a thread of its own, which connects, and connects again after a delay
/// whenever it cannot reach mDNSResponder. Every DNSServiceRef is used on that thread only, and
/// stop() releases what is left after it ends. A failed poll() is reported as unavailable and
/// waited out like an unreachable mDNSResponder; a failed wake pipe ends the thread, reported as
/// unavailable.

#include "discovery_policy.h"
#include "sendspin/server/discovery.h"
#include <arpa/inet.h>
#include <dns_sd.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace sendspin::server {

namespace {

constexpr const char* SERVER_SERVICE = "_sendspin-server._tcp";
constexpr const char* CLIENT_SERVICE = "_sendspin._tcp";
/// The wait before connecting to mDNSResponder again
constexpr int RECONNECT_DELAY_MS = 5000;
/// How often a wait after a failed poll() checks for stop()
constexpr int STOP_CHECK_MS = 100;

/// The TXT value of key, if present
std::optional<std::string> txt_value(uint16_t length, const unsigned char* txt, const char* key) {
    uint8_t value_length = 0;
    const void* value = TXTRecordGetValuePtr(length, txt, key, &value_length);
    if (value == nullptr) {
        return std::nullopt;
    }
    return std::string(static_cast<const char*>(value), value_length);
}

/// address as text, an IPv6 link-local one with its interface as zone; nullopt for another
/// family
std::optional<std::string> address_text(const sockaddr* address) {
    std::array<char, INET6_ADDRSTRLEN> text{};
    if (address->sa_family == AF_INET) {
        const auto* v4 = reinterpret_cast<const sockaddr_in*>(address);
        if (inet_ntop(AF_INET, &v4->sin_addr, text.data(), text.size()) == nullptr) {
            return std::nullopt;
        }
        return std::string(text.data());
    }
    if (address->sa_family == AF_INET6) {
        const auto* v6 = reinterpret_cast<const sockaddr_in6*>(address);
        if (inet_ntop(AF_INET6, &v6->sin6_addr, text.data(), text.size()) == nullptr) {
            return std::nullopt;
        }
        std::string result(text.data());
        std::array<char, IF_NAMESIZE> name{};
        if (IN6_IS_ADDR_LINKLOCAL(&v6->sin6_addr) && v6->sin6_scope_id != 0 &&
            if_indextoname(v6->sin6_scope_id, name.data()) != nullptr) {
            result += "%";
            result += name.data();
        }
        return result;
    }
    return std::nullopt;
}

class DnsSdDiscovery final : public DiscoveryProvider {
public:
    ~DnsSdDiscovery() override {
        this->shut_down();
    }

    bool start(const DiscoveryRequest& request, DiscoverySink& sink) override {
        this->sink_ = &sink;
        this->request_ = request;
        this->stopping_ = false;
        if (pipe(this->wake_.data()) != 0) {
            this->release();
            return false;
        }
        this->thread_ = std::thread([this] { this->run(); });
        return true;
    }

    void stop() override {
        this->shut_down();
    }

private:
    void shut_down() {
        if (this->thread_.joinable()) {
            this->stopping_ = true;
            const char byte = 0;
            // The thread ends on any byte, or on the pipe's end
            while (write(this->wake_[1], &byte, 1) < 0 && errno == EINTR) {}
            this->thread_.join();
        }
        this->release();
    }

    /// One browsed instance, resolved over any interface while it is advertised on at least one
    struct Instance {
        DnsSdDiscovery* owner{nullptr};
        std::string name;
        std::string type;
        std::string domain;
        std::set<uint32_t> interfaces;
        DNSServiceRef resolve{nullptr};
        DNSServiceRef lookup{nullptr};
        std::string host;
        uint16_t port{0};
        std::string path;
        std::optional<std::string> txt_name;
        std::vector<std::string> addresses;
        bool reported{false};
    };

    bool advertise(const AdvertisedServer& server) {
        TXTRecordRef txt;
        TXTRecordCreate(&txt, 0, nullptr);
        bool built = TXTRecordSetValue(&txt, "path", static_cast<uint8_t>(server.path.size()),
                                       server.path.data()) == kDNSServiceErr_NoError;
        if (built && !server.name.empty()) {
            built = TXTRecordSetValue(&txt, "name", static_cast<uint8_t>(server.name.size()),
                                      server.name.data()) == kDNSServiceErr_NoError;
        }
        DNSServiceRef ref = this->connection_;
        // A name in use is renamed by mDNSResponder (no kDNSServiceFlagsNoAutoRename)
        const bool registered =
            built && DNSServiceRegister(
                         &ref, kDNSServiceFlagsShareConnection, kDNSServiceInterfaceIndexAny,
                         server.instance.c_str(), SERVER_SERVICE, nullptr, nullptr,
                         htons(server.port), TXTRecordGetLength(&txt), TXTRecordGetBytesPtr(&txt),
                         &DnsSdDiscovery::on_registered, this) == kDNSServiceErr_NoError;
        TXTRecordDeallocate(&txt);
        return registered;
    }

    bool browse() {
        DNSServiceRef ref = this->connection_;
        return DNSServiceBrowse(&ref, kDNSServiceFlagsShareConnection, kDNSServiceInterfaceIndexAny,
                                CLIENT_SERVICE, nullptr, &DnsSdDiscovery::on_browsed,
                                this) == kDNSServiceErr_NoError;
    }

    /// Connects to mDNSResponder and starts the request's operations; false, with nothing left,
    /// when it cannot be reached
    bool connect() {
        if (DNSServiceCreateConnection(&this->connection_) != kDNSServiceErr_NoError) {
            this->connection_ = nullptr;
            return false;
        }
        if (DNSServiceRefSockFD(this->connection_) < 0 ||
            (this->request_.advertise && !this->advertise(*this->request_.advertise)) ||
            (this->request_.browse && !this->browse())) {
            this->disconnect();
            return false;
        }
        return true;
    }

    /// Releases the connection and every operation on it, reporting each client found as lost
    void disconnect() {
        if (this->connection_ != nullptr) {
            DNSServiceRefDeallocate(this->connection_);
            this->connection_ = nullptr;
        }
        for (const auto& [name, instance] : this->instances_) {
            if (instance->reported) {
                this->sink_->on_client_lost(name);
            }
        }
        this->instances_.clear();
    }

    void run() {
        bool reported_unavailable = false;
        for (;;) {
            if (this->connection_ == nullptr && !this->connect() && !reported_unavailable) {
                reported_unavailable = true;
                this->sink_->on_unavailable();
            }
            std::array<pollfd, 2> fds{};
            fds[0] = {this->wake_[0], POLLIN, 0};
            nfds_t count = 1;
            if (this->connection_ != nullptr) {
                if (std::exchange(reported_unavailable, false)) {
                    this->sink_->on_available();
                }
                fds[1] = {DNSServiceRefSockFD(this->connection_), POLLIN, 0};
                count = 2;
            }
            const int ready =
                poll(fds.data(), count, this->connection_ != nullptr ? -1 : RECONNECT_DELAY_MS);
            if (ready < 0 && errno == EINTR) {
                continue;
            }
            if (ready < 0 || (fds[0].revents & (POLLERR | POLLNVAL)) != 0) {
                this->disconnect();
                if (!std::exchange(reported_unavailable, true)) {
                    this->sink_->on_unavailable();
                }
                // stop() cannot wake a thread whose wake pipe failed
                if (ready >= 0 || !this->wait_out_failure()) {
                    return;
                }
                continue;
            }
            // stop() asked the thread to end
            if (fds[0].revents != 0) {
                return;
            }
            // The socket is invalid or mDNSResponder went away (kDNSServiceErr_ServiceNotRunning):
            // connect again
            if (count == 2 && fds[1].revents != 0 &&
                ((fds[1].revents & POLLNVAL) != 0 ||
                 DNSServiceProcessResult(this->connection_) != kDNSServiceErr_NoError)) {
                this->disconnect();
                reported_unavailable = true;
                this->sink_->on_unavailable();
            }
        }
    }

    /// Waits RECONNECT_DELAY_MS without poll(); false when stop() asks the thread to end meanwhile
    bool wait_out_failure() const {
        for (int waited = 0; waited < RECONNECT_DELAY_MS; waited += STOP_CHECK_MS) {
            if (this->stopping_) {
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(STOP_CHECK_MS));
        }
        return !this->stopping_;
    }

    /// Frees every operation: deallocating the shared connection releases the operations on it
    void release() {
        if (this->connection_ != nullptr) {
            DNSServiceRefDeallocate(this->connection_);
            this->connection_ = nullptr;
        }
        this->instances_.clear();
        for (int& fd : this->wake_) {
            if (fd >= 0) {
                close(fd);
                fd = -1;
            }
        }
    }

    static void DNSSD_API on_registered(DNSServiceRef /*ref*/, DNSServiceFlags /*flags*/,
                                        DNSServiceErrorType /*error*/, const char* /*name*/,
                                        const char* /*type*/, const char* /*domain*/,
                                        void* /*context*/) {}

    static void DNSSD_API on_browsed(DNSServiceRef /*ref*/, DNSServiceFlags flags,
                                     uint32_t interface, DNSServiceErrorType error,
                                     const char* name, const char* type, const char* domain,
                                     void* context) {
        auto* self = static_cast<DnsSdDiscovery*>(context);
        if (error != kDNSServiceErr_NoError) {
            return;
        }
        if ((flags & kDNSServiceFlagsAdd) != 0) {
            self->add(interface, name, type, domain);
        } else {
            self->remove(interface, name);
        }
    }

    void add(uint32_t interface, const char* name, const char* type, const char* domain) {
        if (this->instances_.size() >= discovery_limits::MAX_CLIENTS &&
            this->instances_.count(name) == 0) {
            return;
        }
        std::unique_ptr<Instance>& slot = this->instances_[name];
        if (!slot) {
            slot = std::make_unique<Instance>();
            slot->owner = this;
            slot->name = name;
            slot->type = type;
            slot->domain = domain;
        }
        Instance& instance = *slot;
        instance.interfaces.insert(interface);
        if (instance.resolve != nullptr) {
            return;
        }
        // Kept while the instance is advertised, so a changed TXT or port is reported again
        instance.resolve = this->connection_;
        if (DNSServiceResolve(&instance.resolve, kDNSServiceFlagsShareConnection,
                              kDNSServiceInterfaceIndexAny, name, type, domain,
                              &DnsSdDiscovery::on_resolved, &instance) != kDNSServiceErr_NoError) {
            instance.resolve = nullptr;
        }
    }

    void remove(uint32_t interface, const char* name) {
        const auto it = this->instances_.find(name);
        if (it == this->instances_.end()) {
            return;
        }
        Instance& instance = *it->second;
        instance.interfaces.erase(interface);
        if (!instance.interfaces.empty()) {
            return;
        }
        const bool reported = instance.reported;
        const std::string lost = instance.name;
        stop_operation(instance.resolve);
        stop_operation(instance.lookup);
        this->instances_.erase(it);
        if (reported) {
            this->sink_->on_client_lost(lost);
        }
    }

    static void stop_operation(DNSServiceRef& ref) {
        if (ref != nullptr) {
            DNSServiceRefDeallocate(ref);
            ref = nullptr;
        }
    }

    static void DNSSD_API on_resolved(DNSServiceRef /*ref*/, DNSServiceFlags /*flags*/,
                                      uint32_t /*interface*/, DNSServiceErrorType error,
                                      const char* /*full_name*/, const char* host, uint16_t port,
                                      uint16_t txt_length, const unsigned char* txt,
                                      void* context) {
        auto* instance = static_cast<Instance*>(context);
        if (error != kDNSServiceErr_NoError) {
            return;
        }
        // spec connection.md "Server Initiated Connections": `path` is required, `name` optional
        instance->port = ntohs(port);
        instance->path = txt_value(txt_length, txt, "path").value_or("");
        instance->txt_name = txt_value(txt_length, txt, "name");
        if (instance->lookup != nullptr && instance->host == host) {
            instance->owner->report(*instance);
            return;
        }
        stop_operation(instance->lookup);
        instance->host = host;
        instance->addresses.clear();
        instance->lookup = instance->owner->connection_;
        if (DNSServiceGetAddrInfo(
                &instance->lookup, kDNSServiceFlagsShareConnection, kDNSServiceInterfaceIndexAny,
                kDNSServiceProtocol_IPv4 | kDNSServiceProtocol_IPv6, host,
                &DnsSdDiscovery::on_address, instance) != kDNSServiceErr_NoError) {
            instance->lookup = nullptr;
        }
    }

    static void DNSSD_API on_address(DNSServiceRef /*ref*/, DNSServiceFlags flags,
                                     uint32_t /*interface*/, DNSServiceErrorType error,
                                     const char* /*host*/, const sockaddr* address,
                                     uint32_t /*ttl*/, void* context) {
        auto* instance = static_cast<Instance*>(context);
        if (error != kDNSServiceErr_NoError || address == nullptr) {
            return;
        }
        const std::optional<std::string> text = address_text(address);
        if (text) {
            std::vector<std::string>& addresses = instance->addresses;
            const auto it = std::find(addresses.begin(), addresses.end(), *text);
            if ((flags & kDNSServiceFlagsAdd) != 0 && it == addresses.end()) {
                addresses.push_back(*text);
            } else if ((flags & kDNSServiceFlagsAdd) == 0 && it != addresses.end()) {
                addresses.erase(it);
            }
        }
        if ((flags & kDNSServiceFlagsMoreComing) == 0) {
            instance->owner->report(*instance);
        }
    }

    /// Reports instance once it has a path and an address: IPv4 first, then IPv6, link-local
    /// last
    void report(Instance& instance) {
        if (instance.path.empty() || instance.port == 0 || instance.addresses.empty()) {
            return;
        }
        DiscoveredClient client;
        client.instance = instance.name;
        client.addresses = instance.addresses;
        const auto rank = [](const std::string& address) {
            if (address.find(':') == std::string::npos) {
                return 0;
            }
            return address.find('%') == std::string::npos ? 1 : 2;
        };
        std::stable_sort(
            client.addresses.begin(), client.addresses.end(),
            [&](const std::string& a, const std::string& b) { return rank(a) < rank(b); });
        client.host = instance.host;
        client.port = instance.port;
        client.path = instance.path;
        client.name = instance.txt_name;
        instance.reported = true;
        this->sink_->on_client_found(client);
    }

    DiscoverySink* sink_{nullptr};
    DiscoveryRequest request_;
    DNSServiceRef connection_{nullptr};
    std::array<int, 2> wake_{-1, -1};
    /// Set by stop() before it writes to the wake pipe
    std::atomic<bool> stopping_{false};
    std::map<std::string, std::unique_ptr<Instance>> instances_;
    std::thread thread_;
};

}  // namespace

std::shared_ptr<DiscoveryProvider> platform_discovery() {
    return std::make_shared<DnsSdDiscovery>();
}

}  // namespace sendspin::server
