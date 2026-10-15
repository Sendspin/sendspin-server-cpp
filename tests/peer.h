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

/// @file peer.h
/// @brief For tests with in-process sendspin-cpp clients the server dials: their network
/// provider and blob store, their config and URL, their main loops run on the test thread, and
/// the client_id the server gives the one at a URL

#pragma once

#include "crypto/keys.h"
#include <gtest/gtest.h>

#include <sendspin/client.h>
#include <sendspin/persistence_codec.h>
#include <sendspin/server/server.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace sendspin::server::test {

/// A sendspin-cpp client's network provider that is always ready
class AlwaysReady : public sendspin::SendspinNetworkProvider {
public:
    bool is_network_ready() override {
        return true;
    }
};

/// A sendspin-cpp client's blob store
class ClientStore : public sendspin::SendspinPersistenceProvider {
public:
    std::optional<std::vector<uint8_t>> load_blob(const std::string& key) override {
        const std::lock_guard<std::mutex> lock(this->mutex_);
        const auto it = this->blobs_.find(key);
        return it == this->blobs_.end() ? std::nullopt : std::optional(it->second);
    }

    bool save_blob(const std::string& key, const uint8_t* data, size_t len) override {
        const std::lock_guard<std::mutex> lock(this->mutex_);
        this->blobs_[key] = std::vector<uint8_t>(data, data + len);
        return true;
    }

    /// A pairing record for server_id with psk, as a client that paired holds it
    void hold_record(const std::string& server_id, const Psk& psk) {
        sendspin::SendspinPairingRecord record;
        record.psk = psk;
        record.server_id = server_id;
        const auto blob = sendspin::encode_pairing_record(record);
        ASSERT_TRUE(blob);
        this->save_blob(sendspin::persistence_keys::record_slot_key(0), blob->data(), blob->size());
    }

private:
    std::mutex mutex_;
    std::map<std::string, std::vector<uint8_t>> blobs_;
};

/// The config of a sendspin-cpp client named name, listening on port for the server to dial
inline sendspin::SendspinClientConfig peer_config(std::string name, uint16_t port) {
    sendspin::SendspinClientConfig config;
    config.name = std::move(name);
    config.server_port = port;
    // Bursts often enough that several complete within a test
    config.time_burst_interval_ms = 50;
    return config;
}

/// The URL the server dials a sendspin-cpp client listening on port at
inline std::string peer_url(uint16_t port) {
    return "ws://127.0.0.1:" + std::to_string(port) + "/sendspin";
}

/// Runs step() on this thread, sleeping 2 ms after each run, until done() holds, with no timeout;
/// step() runs the main loops of a test's sendspin-cpp clients
template <typename Step, typename Predicate>
void poll_until(Step step, Predicate done) {
    while (!done()) {
        step();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

/// Runs client's main loop on this thread until done() holds, with no timeout
template <typename Predicate>
void run_until(sendspin::SendspinClient& client, Predicate done) {
    poll_until([&] { client.loop(); }, done);
}

/// Runs the main loop of each peer's client() on this thread until done() holds, with no timeout;
/// peers holds pointers to the test's peer objects
template <typename Peers, typename Predicate>
void run_all_until(const Peers& peers, Predicate done) {
    poll_until(
        [&] {
            for (const auto& peer : peers) {
                peer->client().loop();
            }
        },
        done);
}

/// The client_id of server's client at url, or empty
inline std::string client_at(const SendspinServer& server, const std::string& url) {
    for (const ClientInfo& client : server.clients()) {
        if (client.url == url) {
            return client.client_id;
        }
    }
    return {};
}

}  // namespace sendspin::server::test
