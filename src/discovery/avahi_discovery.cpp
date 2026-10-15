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

/// @file avahi_discovery.cpp
/// @brief platform_discovery() on Linux: avahi-client, whose threaded poll is the discovery thread.
/// Avahi calls back on that thread, or within avahi_client_new() in start() before it runs; stop()
/// frees everything after it ends. The client never fails (AVAHI_CLIENT_NO_FAIL): it waits for
/// avahi-daemon, and its state callback advertises and browses while the daemon runs. The
/// avahi-compat dns_sd layer is not used, since it cannot resolve addresses.

#include "discovery_policy.h"
#include "sendspin/server/discovery.h"
#include <avahi-client/client.h>
#include <avahi-client/lookup.h>
#include <avahi-client/publish.h>
#include <avahi-common/alternative.h>
#include <avahi-common/error.h>
#include <avahi-common/malloc.h>
#include <avahi-common/thread-watch.h>
#include <avahi-common/timeval.h>
#include <net/if.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace sendspin::server {

namespace {

constexpr const char* SERVER_SERVICE = "_sendspin-server._tcp";
constexpr const char* CLIENT_SERVICE = "_sendspin._tcp";
/// The wait before resolving an instance again after its resolver failed
constexpr unsigned RESOLVE_RETRY_MS = 5000;
/// The wait before advertising again after the advertisement failed
constexpr unsigned PUBLISH_RETRY_MS = 5000;

/// Avahi defines no enumerator for the empty flag sets
constexpr AvahiLookupFlags NO_LOOKUP_FLAGS{};
constexpr AvahiPublishFlags NO_PUBLISH_FLAGS{};

/// The TXT value of key, if present
std::optional<std::string> txt_value(AvahiStringList* txt, const char* key) {
    AvahiStringList* entry = avahi_string_list_find(txt, key);
    if (entry == nullptr) {
        return std::nullopt;
    }
    char* name = nullptr;
    char* value = nullptr;
    size_t size = 0;
    if (avahi_string_list_get_pair(entry, &name, &value, &size) != 0) {
        return std::nullopt;
    }
    std::string result = value != nullptr ? std::string(value, size) : std::string();
    avahi_free(name);
    avahi_free(value);
    return result;
}

/// address as text, an IPv6 link-local one with its interface as zone
std::string address_text(const AvahiAddress& address, AvahiIfIndex interface) {
    std::array<char, AVAHI_ADDRESS_STR_MAX> text{};
    avahi_address_snprint(text.data(), text.size(), &address);
    std::string result(text.data());
    constexpr uint8_t LINK_LOCAL_FIRST = 0xfe;
    constexpr uint8_t LINK_LOCAL_SECOND_MASK = 0xc0;
    constexpr uint8_t LINK_LOCAL_SECOND = 0x80;
    std::array<char, IF_NAMESIZE> name{};
    if (address.proto == AVAHI_PROTO_INET6 && address.data.ipv6.address[0] == LINK_LOCAL_FIRST &&
        (address.data.ipv6.address[1] & LINK_LOCAL_SECOND_MASK) == LINK_LOCAL_SECOND &&
        interface > 0 && if_indextoname(static_cast<unsigned>(interface), name.data()) != nullptr) {
        result += "%";
        result += name.data();
    }
    return result;
}

class AvahiDiscovery final : public DiscoveryProvider {
public:
    ~AvahiDiscovery() override {
        this->shut_down();
    }

    bool start(const DiscoveryRequest& request, DiscoverySink& sink) override {
        this->sink_ = &sink;
        this->request_ = request;
        if (request.advertise) {
            this->instance_name_ = request.advertise->instance;
        }
        this->poll_ = avahi_threaded_poll_new();
        if (this->poll_ == nullptr) {
            return false;
        }
        int error = 0;
        // Calls on_client_state() within, before it returns the client
        this->client_ = avahi_client_new(avahi_threaded_poll_get(this->poll_), AVAHI_CLIENT_NO_FAIL,
                                         &AvahiDiscovery::on_client_state, this, &error);
        if (this->client_ == nullptr || avahi_threaded_poll_start(this->poll_) < 0) {
            this->release();
            return false;
        }
        this->running_ = true;
        return true;
    }

    void stop() override {
        this->shut_down();
    }

private:
    /// One resolver, for one interface and protocol the instance is advertised on
    struct Resolution {
        AvahiDiscovery* owner{nullptr};
        std::string instance;
        std::string type;
        std::string domain;
        AvahiIfIndex interface{AVAHI_IF_UNSPEC};
        AvahiProtocol protocol{AVAHI_PROTO_UNSPEC};
        AvahiServiceResolver* resolver{nullptr};
        /// Pending while the resolver waits to be made again after a failure
        AvahiTimeout* retry{nullptr};
        std::optional<std::string> address;
        std::string host;
        uint16_t port{0};
        std::string path;
        std::optional<std::string> txt_name;
    };

    using ResolutionKey = std::pair<AvahiIfIndex, AvahiProtocol>;

    struct Instance {
        std::map<ResolutionKey, std::unique_ptr<Resolution>> resolutions;
        bool reported{false};
    };

    void shut_down() {
        if (this->running_) {
            avahi_threaded_poll_stop(this->poll_);
            this->running_ = false;
        }
        this->release();
    }

    static void on_client_state(AvahiClient* client, AvahiClientState state, void* context) {
        auto* self = static_cast<AvahiDiscovery*>(context);
        switch (state) {
            case AVAHI_CLIENT_S_RUNNING:
                if (std::exchange(self->unavailable_, false)) {
                    self->sink_->on_available();
                }
                self->begin(client);
                break;
            case AVAHI_CLIENT_S_REGISTERING:
            case AVAHI_CLIENT_S_COLLISION:
                // The host name is being registered or is in use: the service waits for it
                if (self->group_ != nullptr) {
                    avahi_entry_group_reset(self->group_);
                }
                break;
            case AVAHI_CLIENT_CONNECTING:
            case AVAHI_CLIENT_FAILURE:
                // avahi-daemon is gone; with AVAHI_CLIENT_NO_FAIL the client reconnects to it
                self->end_session();
                self->unavailable_ = true;
                self->sink_->on_unavailable();
                break;
        }
    }

    /// Advertises and browses with client, which runs
    void begin(AvahiClient* client) {
        this->client_ = client;
        if (this->request_.advertise) {
            this->advertise();
        }
        if (this->request_.browse && this->browser_ == nullptr) {
            this->browser_ = avahi_service_browser_new(client, AVAHI_IF_UNSPEC, AVAHI_PROTO_UNSPEC,
                                                       CLIENT_SERVICE, nullptr, NO_LOOKUP_FLAGS,
                                                       &AvahiDiscovery::on_browsed, this);
        }
    }

    /// Frees what the daemon's session held, reporting each client found as lost
    void end_session() {
        for (auto& [name, instance] : this->instances_) {
            for (auto& [key, resolution] : instance.resolutions) {
                this->free_resolution(*resolution);
            }
            if (instance.reported) {
                this->sink_->on_client_lost(name);
            }
        }
        this->instances_.clear();
        if (this->browser_ != nullptr) {
            avahi_service_browser_free(this->browser_);
            this->browser_ = nullptr;
        }
        this->cancel_publish_retry();
        if (this->group_ != nullptr) {
            avahi_entry_group_free(this->group_);
            this->group_ = nullptr;
        }
    }

    /// Publishes the advertisement unless its entry group holds it; a group that cannot be made is
    /// tried again later
    void advertise() {
        if (this->group_ == nullptr) {
            this->group_ =
                avahi_entry_group_new(this->client_, &AvahiDiscovery::on_group_state, this);
        }
        if (this->group_ == nullptr) {
            this->publish_later();
        } else if (avahi_entry_group_is_empty(this->group_) != 0) {
            this->publish();
        }
    }

    /// Adds the advertised service to the entry group and commits it, under the next alternative
    /// name while the name is in use; one that cannot be added or committed is tried again later
    void publish() {
        if (!this->request_.advertise) {
            return;
        }
        const AdvertisedServer& server = *this->request_.advertise;
        AvahiStringList* txt = avahi_string_list_add_pair(nullptr, "path", server.path.c_str());
        if (!server.name.empty()) {
            txt = avahi_string_list_add_pair(txt, "name", server.name.c_str());
        }
        for (;;) {
            const int added = avahi_entry_group_add_service_strlst(
                this->group_, AVAHI_IF_UNSPEC, AVAHI_PROTO_UNSPEC, NO_PUBLISH_FLAGS,
                this->instance_name_.c_str(), SERVER_SERVICE, nullptr, nullptr, server.port, txt);
            if (added == AVAHI_ERR_COLLISION) {
                this->rename();
                continue;
            }
            if (added != 0 || avahi_entry_group_commit(this->group_) < 0) {
                this->publish_later();
            }
            break;
        }
        avahi_string_list_free(txt);
    }

    /// Empties the entry group and advertises again after PUBLISH_RETRY_MS, or when the client
    /// runs again
    void publish_later() {
        if (this->group_ != nullptr) {
            avahi_entry_group_reset(this->group_);
        }
        if (this->publish_retry_ != nullptr) {
            return;
        }
        const AvahiPoll* poll = avahi_threaded_poll_get(this->poll_);
        timeval at{};
        avahi_elapse_time(&at, PUBLISH_RETRY_MS, 0);
        this->publish_retry_ =
            poll->timeout_new(poll, &at, &AvahiDiscovery::on_publish_retry, this);
    }

    static void on_publish_retry(AvahiTimeout* /*timeout*/, void* context) {
        auto* self = static_cast<AvahiDiscovery*>(context);
        self->cancel_publish_retry();
        // A client that is not running advertises in begin() once it runs again
        if (avahi_client_get_state(self->client_) == AVAHI_CLIENT_S_RUNNING) {
            self->advertise();
        }
    }

    void cancel_publish_retry() {
        if (this->publish_retry_ != nullptr) {
            const AvahiPoll* poll = avahi_threaded_poll_get(this->poll_);
            poll->timeout_free(this->publish_retry_);
            this->publish_retry_ = nullptr;
        }
    }

    /// The next alternative instance name, with the entry group emptied for it
    void rename() {
        char* alternative = avahi_alternative_service_name(this->instance_name_.c_str());
        this->instance_name_ = alternative;
        avahi_free(alternative);
        avahi_entry_group_reset(this->group_);
    }

    static void on_group_state(AvahiEntryGroup* /*group*/, AvahiEntryGroupState state,
                               void* context) {
        auto* self = static_cast<AvahiDiscovery*>(context);
        if (state == AVAHI_ENTRY_GROUP_COLLISION) {
            // Another service holds the name: advertise under the next alternative
            self->rename();
            self->publish();
        } else if (state == AVAHI_ENTRY_GROUP_FAILURE) {
            self->publish_later();
        }
    }

    static void on_browsed(AvahiServiceBrowser* /*browser*/, AvahiIfIndex interface,
                           AvahiProtocol protocol, AvahiBrowserEvent event, const char* name,
                           const char* type, const char* domain, AvahiLookupResultFlags /*flags*/,
                           void* context) {
        auto* self = static_cast<AvahiDiscovery*>(context);
        if (event == AVAHI_BROWSER_NEW) {
            self->add({interface, protocol}, name, type, domain);
        } else if (event == AVAHI_BROWSER_REMOVE) {
            self->remove({interface, protocol}, name);
        }
    }

    void add(ResolutionKey key, const char* name, const char* type, const char* domain) {
        if (this->instances_.size() >= discovery_limits::MAX_CLIENTS &&
            this->instances_.count(name) == 0) {
            return;
        }
        Instance& instance = this->instances_[name];
        std::unique_ptr<Resolution>& slot = instance.resolutions[key];
        if (slot) {
            return;
        }
        slot = std::make_unique<Resolution>();
        slot->owner = this;
        slot->instance = name;
        slot->type = type;
        slot->domain = domain;
        slot->interface = key.first;
        slot->protocol = key.second;
        this->resolve(*slot);
    }

    /// Starts resolution's resolver, kept while the instance is advertised there so a change is
    /// reported again; one that cannot be made is tried again later
    void resolve(Resolution& resolution) {
        resolution.resolver = avahi_service_resolver_new(
            this->client_, resolution.interface, resolution.protocol, resolution.instance.c_str(),
            resolution.type.c_str(), resolution.domain.c_str(), AVAHI_PROTO_UNSPEC, NO_LOOKUP_FLAGS,
            &AvahiDiscovery::on_resolved, &resolution);
        if (resolution.resolver == nullptr) {
            this->retry_later(resolution);
        }
    }

    void retry_later(Resolution& resolution) {
        const AvahiPoll* poll = avahi_threaded_poll_get(this->poll_);
        timeval at{};
        avahi_elapse_time(&at, RESOLVE_RETRY_MS, 0);
        resolution.retry = poll->timeout_new(poll, &at, &AvahiDiscovery::on_retry, &resolution);
    }

    static void on_retry(AvahiTimeout* /*timeout*/, void* context) {
        auto* resolution = static_cast<Resolution*>(context);
        const AvahiPoll* poll = avahi_threaded_poll_get(resolution->owner->poll_);
        poll->timeout_free(resolution->retry);
        resolution->retry = nullptr;
        resolution->owner->resolve(*resolution);
    }

    void remove(ResolutionKey key, const char* name) {
        const auto it = this->instances_.find(name);
        if (it == this->instances_.end()) {
            return;
        }
        Instance& instance = it->second;
        const auto resolution = instance.resolutions.find(key);
        if (resolution != instance.resolutions.end()) {
            this->free_resolution(*resolution->second);
            instance.resolutions.erase(resolution);
        }
        if (!instance.resolutions.empty()) {
            this->report(name, instance);
            return;
        }
        const bool reported = instance.reported;
        const std::string lost = name;
        this->instances_.erase(it);
        if (reported) {
            this->sink_->on_client_lost(lost);
        }
    }

    void free_resolution(Resolution& resolution) {
        if (resolution.resolver != nullptr) {
            avahi_service_resolver_free(resolution.resolver);
            resolution.resolver = nullptr;
        }
        if (resolution.retry != nullptr) {
            const AvahiPoll* poll = avahi_threaded_poll_get(this->poll_);
            poll->timeout_free(resolution.retry);
            resolution.retry = nullptr;
        }
    }

    static void on_resolved(AvahiServiceResolver* /*resolver*/, AvahiIfIndex interface,
                            AvahiProtocol /*protocol*/, AvahiResolverEvent event,
                            const char* /*name*/, const char* /*type*/, const char* /*domain*/,
                            const char* host, const AvahiAddress* address, uint16_t port,
                            AvahiStringList* txt, AvahiLookupResultFlags /*flags*/, void* context) {
        auto* resolution = static_cast<Resolution*>(context);
        AvahiDiscovery& self = *resolution->owner;
        if (event != AVAHI_RESOLVER_FOUND || address == nullptr) {
            // The address may be stale: drop it and resolve again later
            resolution->address.reset();
            self.free_resolution(*resolution);
            self.retry_later(*resolution);
        } else {
            // spec connection.md "Server Initiated Connections": `path` is required, `name`
            // optional
            resolution->address = address_text(*address, interface);
            resolution->host = host != nullptr ? host : "";
            resolution->port = port;
            resolution->path = txt_value(txt, "path").value_or("");
            resolution->txt_name = txt_value(txt, "name");
        }
        const auto it = self.instances_.find(resolution->instance);
        if (it != self.instances_.end()) {
            self.report(resolution->instance, it->second);
        }
    }

    /// Reports instance from its latest resolution with a path, with every address resolved: IPv4
    /// first, then IPv6, link-local last
    void report(const std::string& name, Instance& instance) {
        const Resolution* latest = nullptr;
        DiscoveredClient client;
        client.instance = name;
        for (const auto& [key, resolution] : instance.resolutions) {
            const std::optional<std::string>& address = resolution->address;
            if (!address || resolution->path.empty()) {
                continue;
            }
            latest = resolution.get();
            if (std::find(client.addresses.begin(), client.addresses.end(), *address) ==
                client.addresses.end()) {
                client.addresses.push_back(*address);
            }
        }
        if (latest == nullptr) {
            return;
        }
        const auto rank = [](const std::string& address) {
            if (address.find(':') == std::string::npos) {
                return 0;
            }
            return address.find('%') == std::string::npos ? 1 : 2;
        };
        std::stable_sort(
            client.addresses.begin(), client.addresses.end(),
            [&](const std::string& a, const std::string& b) { return rank(a) < rank(b); });
        client.host = latest->host;
        client.port = latest->port;
        client.path = latest->path;
        client.name = latest->txt_name;
        instance.reported = true;
        this->sink_->on_client_found(client);
    }

    /// Frees the resolvers, the browser and the entry group, then the client and the poll
    void release() {
        for (auto& [name, instance] : this->instances_) {
            for (auto& [key, resolution] : instance.resolutions) {
                this->free_resolution(*resolution);
            }
        }
        this->instances_.clear();
        if (this->browser_ != nullptr) {
            avahi_service_browser_free(this->browser_);
            this->browser_ = nullptr;
        }
        this->cancel_publish_retry();
        if (this->group_ != nullptr) {
            avahi_entry_group_free(this->group_);
            this->group_ = nullptr;
        }
        if (this->client_ != nullptr) {
            avahi_client_free(this->client_);
            this->client_ = nullptr;
        }
        if (this->poll_ != nullptr) {
            avahi_threaded_poll_free(this->poll_);
            this->poll_ = nullptr;
        }
    }

    DiscoverySink* sink_{nullptr};
    DiscoveryRequest request_;
    AvahiThreadedPoll* poll_{nullptr};
    /// Set once avahi_client_new() returns; its callbacks before then get the client as argument
    AvahiClient* client_{nullptr};
    AvahiEntryGroup* group_{nullptr};
    /// Pending while the advertisement waits to be published again after a failure
    AvahiTimeout* publish_retry_{nullptr};
    AvahiServiceBrowser* browser_{nullptr};
    std::string instance_name_;
    std::map<std::string, Instance> instances_;
    bool running_{false};
    /// on_unavailable() was reported last, on the poll thread or within avahi_client_new()
    bool unavailable_{false};
};

}  // namespace

std::shared_ptr<DiscoveryProvider> platform_discovery() {
    return std::make_shared<AvahiDiscovery>();
}

}  // namespace sendspin::server
