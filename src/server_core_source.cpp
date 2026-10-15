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

/// @file server_core_source.cpp
/// @brief ServerCore's routed sources (roles/source/v1.md): each route's decoder, which connection
/// streams into it, and what the host hears of it

#include "server_core.h"
#include "server_core_client.h"

#include <algorithm>
#include <utility>

namespace sendspin::server {

namespace {

/// A source's format as the host sees it, as its client-stream/start announced it
StreamFormat source_format(const AudioFormat& format) {
    StreamFormat stream;
    switch (format.codec) {
        case Codec::PCM:
            stream.codec = StreamCodec::PCM;
            break;
        case Codec::FLAC:
            stream.codec = StreamCodec::FLAC;
            break;
        case Codec::OPUS:
            stream.codec = StreamCodec::OPUS;
            break;
    }
    constexpr uint32_t BYTE_MAX = 255;
    stream.sample_rate = format.sample_rate;
    stream.channels = static_cast<uint8_t>(std::min(format.channels, BYTE_MAX));
    stream.bit_depth = static_cast<uint8_t>(std::min(format.bit_depth, BYTE_MAX));
    return stream;
}

}  // namespace

ServerCore::Route::~Route() {
    if (this->task) {
        this->task->stop();
    }
}

void ServerCore::route_source(const std::string& client_id, const std::shared_ptr<HostView>& view,
                              int64_t jitter_us) {
    if (this->stopping_ || view->gone()) {
        return;
    }
    if (auto old = this->routes_.find(client_id); old != this->routes_.end()) {
        std::unique_ptr<Route> replaced = std::move(old->second);
        this->routes_.erase(old);
        // Its input closes as a release would end it; the detach this runs finds no route of it
        Group* group = this->find_group(replaced->view->group());
        if (group == nullptr || !group->close_input_of(*replaced->view)) {
            replaced->view->close();
        }
        this->end_route(std::move(replaced));
    }
    auto route = std::make_unique<Route>();
    route->view = view;
    route->feed =
        std::make_shared<SourceFeed>(profile::SOURCE_FEED_BYTES, profile::SOURCE_FEED_ITEMS);
    route->faults = std::make_unique<Wakeup>(
        this->loop_, [this, client_id] { this->on_source_faults(client_id); });
    auto decoder = std::make_shared<SourceDecoder>(
        route->feed,
        std::make_unique<LiveSourceSink>(view, jitter_us, this->settings_.resampler_quality),
        route->faults->handle());
    route->task = this->workers_.add([decoder] { return decoder->run(); });
    route->feed->attach(route->task);
    this->routes_.emplace(client_id, std::move(route));
    this->refresh_source(client_id);
}

void ServerCore::refresh_source(const std::string& client_id, const Client* caller) {
    const auto found = this->routes_.find(client_id);
    Route* route = found != this->routes_.end() ? found->second.get() : nullptr;
    Client* chosen = nullptr;
    this->for_each_connection(client_id, [&chosen](Client& client) {
        if (!client.closed && client.driver->session().source().role_written() &&
            (chosen == nullptr || client.reported)) {
            chosen = &client;
        }
    });
    if (route != nullptr && route->connection != (chosen != nullptr ? chosen->id : 0)) {
        this->unbind(*route);
        route->connection = chosen != nullptr ? chosen->id : 0;
    }
    this->for_each_connection(client_id, [route, chosen, caller](Client& client) {
        if (client.closed) {
            return;
        }
        SourceFeed* feed = route != nullptr && &client == chosen ? route->feed.get() : nullptr;
        client.driver->session().set_source_route(feed, client.id);
        if (&client != caller) {
            client.pump();
        }
    });
}

void ServerCore::on_source_event(Client& client, const SourceEvent& event) {
    const auto found = this->routes_.find(client.info.client_id);
    Route* route = found != this->routes_.end() ? found->second.get() : nullptr;
    const bool bound = route != nullptr && route->connection == client.id;
    switch (event.kind) {
        case SourceEvent::Kind::READY:
            this->post_event(
                [info = client.info](ServerListener& listener) { listener.on_source_ready(info); });
            this->refresh_source(client.info.client_id, &client);
            break;
        case SourceEvent::Kind::UNREADY:
            this->refresh_source(client.info.client_id, &client);
            break;
        case SourceEvent::Kind::OPENED:
            if (bound) {
                SS_LOGI(this->log_, "%s streams its source", this->describe(client).c_str());
                this->report_source(*route, client, true, event.format);
            }
            break;
        case SourceEvent::Kind::CLOSED:
            if (bound && route->started) {
                this->report_source(*route, client, false);
            }
            break;
        case SourceEvent::Kind::FAILED: {
            SS_LOGI(this->log_, "%s streams a source the server cannot take",
                    this->describe(client).c_str());
            ServerError error;
            error.kind = ErrorKind::SOURCE_FAILED;
            error.url = client.url;
            error.client_id = client.info.client_id;
            error.format = source_format(event.format);
            this->post_error(std::move(error));
            break;
        }
        case SourceEvent::Kind::SIGNAL:
            if (event.signal) {
                this->post_event(
                    [info = client.info, signal = *event.signal](ServerListener& listener) {
                        listener.on_source_signal(info, signal);
                    });
            }
            break;
    }
}

void ServerCore::report_source(Route& route, const Client& client, bool started,
                               const AudioFormat& format) {
    route.started = started;
    const GroupId group = route.view->group();
    if (started) {
        this->post_event(
            [info = client.info, group, stream = source_format(format)](ServerListener& listener) {
                listener.on_source_started(info, group, stream);
            });
    } else {
        this->post_event([info = client.info, group](ServerListener& listener) {
            listener.on_source_stopped(info, group);
        });
    }
}

void ServerCore::on_source_faults(const std::string& client_id) {
    const auto found = this->routes_.find(client_id);
    if (found == this->routes_.end()) {
        return;
    }
    for (const SourceFault& fault : found->second->feed->take_faults()) {
        const auto it = this->clients_.find(fault.connection);
        if (it == this->clients_.end() || it->second->closed) {
            continue;
        }
        Client& client = *it->second;
        Session& session = client.driver->session();
        if (!session.source().stream() || session.source().stream_number() != fault.stream) {
            continue;
        }
        if (fault.kind == SourceFault::Kind::PROTOCOL) {
            SS_LOGI(this->log_, "%s sent source audio its format does not frame",
                    this->describe(client).c_str());
            session.reject();
        } else {
            session.fail_source(fault.stream);
        }
        client.pump();
    }
}

void ServerCore::unbind(Route& route) {
    const auto bound = this->clients_.find(route.connection);
    route.connection = 0;
    if (bound == this->clients_.end()) {
        return;
    }
    const Client& client = *bound->second;
    // Its stream goes nowhere from here: the input's run ends with it
    const SourceStream& source = client.driver->session().source();
    if (source.stream()) {
        SourceItem item;
        item.kind = SourceItem::Kind::END;
        item.connection = client.id;
        item.stream = source.stream_number();
        (void)route.feed->push(item, {});
    }
    if (route.started) {
        this->report_source(route, client, false);
    }
}

void ServerCore::end_route(std::unique_ptr<Route> route, const Client* caller) {
    const auto bound = this->clients_.find(route->connection);
    if (bound == this->clients_.end()) {
        return;
    }
    Client& client = *bound->second;
    if (route->started) {
        this->report_source(*route, client, false);
    }
    if (!client.closed) {
        client.driver->session().set_source_route(nullptr, client.id);
        if (&client != caller) {
            client.pump();
        }
    }
}

void ServerCore::drop_gone_routes() {
    for (auto it = this->routes_.begin(); it != this->routes_.end();) {
        if (!it->second->view->gone()) {
            ++it;
            continue;
        }
        std::unique_ptr<Route> route = std::move(it->second);
        it = this->routes_.erase(it);
        this->end_route(std::move(route));
    }
}

}  // namespace sendspin::server
