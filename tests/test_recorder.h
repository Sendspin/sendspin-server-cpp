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

/// @file test_recorder.h
/// @brief A persistence provider and a recording listener for tests that run SendspinServer

#pragma once

#include <ArduinoJson.h>
#include <gtest/gtest.h>

#include <sendspin/server/server.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace sendspin::server::test {

/// A ServerConfig that neither advertises nor browses, listening on a port the system picks, so
/// tests stay off the network and run side by side
inline ServerConfig local_config() {
    ServerConfig config;
    config.listen_port = 0;
    config.advertise = false;
    config.browse = false;
    return config;
}

/// An in-memory PersistenceProvider that can refuse writes
class MemoryProvider : public PersistenceProvider {
public:
    std::optional<std::vector<uint8_t>> load_blob(const std::string& key) override {
        this->thread = std::this_thread::get_id();
        const auto it = this->blobs.find(key);
        if (it == this->blobs.end()) {
            return std::nullopt;
        }
        return it->second;
    }

    bool save_blob(const std::string& key, const uint8_t* data, size_t len) override {
        ++this->saves;
        if (this->refuse_save) {
            return false;
        }
        this->blobs[key] = std::vector<uint8_t>(data, data + len);
        return true;
    }

    bool commit() override {
        ++this->commits;
        return !this->refuse_commit;
    }

    // Written on the dispatch thread; read after start() returns, which waits for the load
    std::map<std::string, std::vector<uint8_t>> blobs;
    int saves{0};
    int commits{0};
    bool refuse_save{false};
    bool refuse_commit{false};
    std::thread::id thread;
};

/// Records listener calls and traced messages, in delivery order, as one log
class Recorder : public ServerListener {
public:
    struct Entry {
        std::string what;
        std::thread::id thread;
    };

    void on_client_seen(const ClientInfo& client) override {
        this->add("seen " + client.name);
    }

    void on_client_connected(const ClientInfo& client) override {
        this->add("connected " + client.client_id);
        if (this->stop_on_connect != nullptr) {
            this->stop_on_connect->stop();
        }
    }

    void on_client_disconnected(const ClientInfo& client,
                                const ClientDisconnect& disconnect) override {
        // Widens the window in which a destructor that does not wait would return first
        std::this_thread::sleep_for(this->disconnect_delay);
        // Deliberately unsynchronized: the destructor test reads it after the server's
        // destructor, which must have joined this thread
        ++this->unsynchronized_disconnects;
        {
            const std::lock_guard<std::mutex> lock(this->mutex_);
            this->disconnects_.push_back(disconnect);
        }
        this->add("disconnected " + client.client_id + " " +
                  std::to_string(static_cast<int>(disconnect.reason)));
    }

    void on_approval_needed(const ClientInfo& client) override {
        this->add("approval needed " + client.client_id);
    }

    void on_pairing_required(const ClientInfo& client) override {
        this->add("pairing required " + client.client_id);
    }

    void on_credential_mismatch(const ClientInfo& client) override {
        this->add("mismatch " + client.client_id);
    }

    void on_pairing_pending(const ClientInfo& client, std::string_view message) override {
        this->add("pairing pending " + client.client_id + " " + std::string(message));
    }

    void on_pairing_code_needed(const ClientInfo& client, PairMethod method,
                                CodeFormat format) override {
        this->add(code_needed(client.client_id, method, format));
    }

    void on_pairing_result(const ClientInfo& client, PairingResult result) override {
        this->add(pairing_result(client.client_id, result));
    }

    static std::string code_needed(const std::string& client_id, PairMethod method,
                                   CodeFormat format) {
        return "code needed " + client_id + " " + std::to_string(static_cast<int>(method)) + " " +
               std::to_string(static_cast<int>(format));
    }

    static std::string pairing_result(const std::string& client_id, PairingResult result) {
        return "pairing result " + client_id + " " + std::to_string(static_cast<int>(result));
    }

    void on_newer_client_detected(const ClientInfo& client, std::string_view identifier) override {
        this->add("newer " + client.client_id + " " + std::string(identifier));
    }

    void on_transport_command(GroupId group, TransportCommand command,
                              const CommandArgs& args) override {
        {
            const std::lock_guard<std::mutex> lock(this->mutex_);
            this->commands_.emplace_back(command, args);
        }
        this->add("command " + std::to_string(group.value) + " " +
                  std::to_string(static_cast<int>(command)) + " " + args.client_id);
    }

    /// Every on_client_disconnected(), in delivery order
    std::vector<ClientDisconnect> disconnects() const {
        const std::lock_guard<std::mutex> lock(this->mutex_);
        return this->disconnects_;
    }

    /// Waits, with no timeout, for count calls of on_client_disconnected()
    std::vector<ClientDisconnect> wait_for_disconnects(size_t count) {
        std::unique_lock<std::mutex> lock(this->mutex_);
        this->changed_.wait(lock, [&] { return this->disconnects_.size() >= count; });
        return this->disconnects_;
    }

    /// Every on_transport_command(), in delivery order
    std::vector<std::pair<TransportCommand, CommandArgs>> commands() const {
        const std::lock_guard<std::mutex> lock(this->mutex_);
        return this->commands_;
    }

    void on_error(const ServerError& error) override {
        {
            const std::lock_guard<std::mutex> lock(this->mutex_);
            this->errors_.push_back(error);
        }
        this->add("error " + std::to_string(static_cast<int>(error.kind)) + " " + error.url);
    }

    void on_group_changed(const GroupInfo& group) override {
        this->add("group " + std::to_string(group.id.value) + " " +
                  (group.playback_state == PlaybackState::PLAYING ? "playing" : "stopped"));
    }

    void on_writable(GroupId group, InputId input) override {
        this->add("writable " + std::to_string(group.value) + " " + std::to_string(input.value));
    }

    void on_drained(GroupId /*group*/, InputId input, DrainId drain,
                    DrainOutcome outcome) override {
        {
            const std::lock_guard<std::mutex> lock(this->mutex_);
            this->drains_.emplace_back(drain, outcome);
        }
        this->add(drained(input, outcome));
    }

    void on_source_ready(const ClientInfo& client) override {
        this->add("source ready " + client.client_id);
    }

    void on_source_started(const ClientInfo& client, GroupId group,
                           const StreamFormat& format) override {
        this->add(source_started(client.client_id, group, format));
    }

    void on_source_stopped(const ClientInfo& client, GroupId group) override {
        this->add("source stopped " + client.client_id + " " + std::to_string(group.value));
    }

    void on_source_signal(const ClientInfo& client, SourceSignal signal) override {
        this->add("source signal " + client.client_id + " " +
                  std::to_string(static_cast<int>(signal)));
    }

    static std::string source_started(const std::string& client_id, GroupId group,
                                      const StreamFormat& format) {
        return "source started " + client_id + " " + std::to_string(group.value) + " " +
               std::to_string(static_cast<int>(format.codec)) + " " +
               std::to_string(format.sample_rate) + " " + std::to_string(format.channels) + " " +
               std::to_string(format.bit_depth);
    }

    /// Every on_drained() with its drain id, in delivery order
    std::vector<std::pair<DrainId, DrainOutcome>> drains() const {
        const std::lock_guard<std::mutex> lock(this->mutex_);
        return this->drains_;
    }

    static std::string drained(InputId input, DrainOutcome outcome) {
        return "drained " + std::to_string(input.value) + " " +
               std::to_string(static_cast<int>(outcome));
    }

    /// For ServerConfig::trace
    void trace(const TracedMessage& message) {
        std::string type = "binary";
        if (!message.plaintext.empty() && (!message.binary || message.plaintext.front() == 0)) {
            const size_t offset = message.binary ? 1 : 0;
            JsonDocument doc;
            if (deserializeJson(doc, message.plaintext.data() + offset,
                                message.plaintext.size() - offset) == DeserializationError::Ok) {
                type = doc["type"].as<std::string>();
                if (type == "server/time") {
                    EXPECT_EQ(doc["payload"]["server_transmitted"].as<int64_t>(), message.stamp_us);
                }
            }
        }
        this->add(std::string(message.direction == TraceDirection::INBOUND ? "in " : "out ") +
                  type);
    }

    bool contains(const std::string& what) const {
        const std::lock_guard<std::mutex> lock(this->mutex_);
        return std::any_of(this->entries_.begin(), this->entries_.end(),
                           [&](const Entry& entry) { return entry.what == what; });
    }

    size_t count(const std::string& what) const {
        const std::lock_guard<std::mutex> lock(this->mutex_);
        return static_cast<size_t>(
            std::count_if(this->entries_.begin(), this->entries_.end(),
                          [&](const Entry& entry) { return entry.what == what; }));
    }

    /// The on_error() reports of kind recorded so far, whatever their URL
    size_t count_errors(ErrorKind kind) const {
        const std::string prefix = "error " + std::to_string(static_cast<int>(kind)) + " ";
        const std::lock_guard<std::mutex> lock(this->mutex_);
        return static_cast<size_t>(
            std::count_if(this->entries_.begin(), this->entries_.end(),
                          [&](const Entry& entry) { return entry.what.starts_with(prefix); }));
    }

    std::vector<Entry> entries() const {
        const std::lock_guard<std::mutex> lock(this->mutex_);
        return this->entries_;
    }

    /// Waits with no timeout; a regression hangs into the suite watchdog
    void wait_for(const std::string& what) {
        std::unique_lock<std::mutex> lock(this->mutex_);
        this->changed_.wait(lock, [&] {
            return std::any_of(this->entries_.begin(), this->entries_.end(),
                               [&](const Entry& entry) { return entry.what == what; });
        });
    }

    /// Waits with no timeout for an entry that match accepts
    void wait_for_match(const std::function<bool(const std::string&)>& match) {
        std::unique_lock<std::mutex> lock(this->mutex_);
        this->changed_.wait(lock, [&] {
            return std::any_of(this->entries_.begin(), this->entries_.end(),
                               [&](const Entry& entry) { return match(entry.what); });
        });
    }

    /// Waits, with no timeout, until what was recorded count times
    void wait_for_count(const std::string& what, size_t count) {
        std::unique_lock<std::mutex> lock(this->mutex_);
        this->changed_.wait(lock, [&] {
            return static_cast<size_t>(std::count_if(
                       this->entries_.begin(), this->entries_.end(),
                       [&](const Entry& entry) { return entry.what == what; })) >= count;
        });
    }

    /// The first error reported for url, waiting with no timeout
    ServerError wait_for_error(const std::string& url) {
        std::unique_lock<std::mutex> lock(this->mutex_);
        std::vector<ServerError>::const_iterator found;
        this->changed_.wait(lock, [&] {
            found = std::find_if(this->errors_.begin(), this->errors_.end(),
                                 [&](const ServerError& error) { return error.url == url; });
            return found != this->errors_.end();
        });
        return *found;
    }

    SendspinServer* stop_on_connect{nullptr};
    std::chrono::milliseconds disconnect_delay{0};
    int unsynchronized_disconnects{0};

private:
    void add(std::string what) {
        {
            const std::lock_guard<std::mutex> lock(this->mutex_);
            this->entries_.push_back({std::move(what), std::this_thread::get_id()});
        }
        this->changed_.notify_all();
    }

    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::vector<Entry> entries_;
    std::vector<ServerError> errors_;
    std::vector<std::pair<DrainId, DrainOutcome>> drains_;
    std::vector<ClientDisconnect> disconnects_;
    std::vector<std::pair<TransportCommand, CommandArgs>> commands_;
};

}  // namespace sendspin::server::test
