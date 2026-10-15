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

/// @file trust_harness.h
/// @brief What the trust tests run: a sendspin-cpp player the server dials, a ServerCore on its own
/// core thread, an input fed the test signal, a raw client player's readings, and providers whose
/// saves fail or wait as scripted

#pragma once

#include "dispatcher.h"
#include "host_view.h"
#include "peer.h"
#include "player_sink.h"
#include "raw_client.h"
#include "server_core.h"
#include "test_ports.h"
#include "test_recorder.h"
#include "transport.h"
#include "wire_messages.h"
#include <gtest/gtest.h>

#include <sendspin/client.h>
#include <sendspin/metadata_role.h>
#include <sendspin/player_role.h>
#include <sendspin/server/server.h>

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace sendspin::server::test {

/// The rate the players play at
inline constexpr uint32_t PLAYER_RATE = 48000;

/// The trust a sendspin-cpp client reports for its connection: USER for a long-term PSK, NONE for
/// the Sentinel or a pairing PSK
class TrustListener : public sendspin::SendspinClientListener {
public:
    void on_trust_changed(sendspin::ConnectionTrust trust) override {
        this->trust = trust;
        ++this->reports;
    }

    std::atomic<sendspin::ConnectionTrust> trust{sendspin::ConnectionTrust::NONE};
    std::atomic<int> reports{0};
};

/// A sendspin-cpp player listening on a loopback port for the server to dial, with a real-time
/// audio output. Its main loop runs on the test thread through run_until().
class Peer {
public:
    struct Options {
        bool unpaired_access{true};
        std::optional<Psk> pairing_psk;
    };

    explicit Peer(ClientStore& store, const Options& options)
        : port_(sendspin::server::test::free_port()), client_(make_config(port_, options)) {
        sendspin::PlayerRoleConfig player;
        // One format, assigned by count: GCC 13 -Warray-bounds misreads an initializer-list
        // assignment
        player.audio_formats.assign(1, sendspin::AudioSupportedFormatObject{
                                           sendspin::SendspinCodecFormat::PCM, 2, PLAYER_RATE, 16});
        player.min_buffer_ms = 150;
        sendspin::PlayerRole& role = this->client_.add_player(player);
        this->sink_ = std::make_unique<sendspin::server::test::Sink>(role, 4, PLAYER_RATE);
        role.set_listener(this->sink_.get());
        this->client_.add_metadata();
        this->client_.set_network_provider(&this->network_);
        this->client_.set_persistence_provider(&store);
        this->client_.set_listener(&this->trust_);
        this->client_.set_unpaired_access_enabled(options.unpaired_access);
        EXPECT_TRUE(this->client_.start());
    }

    ~Peer() {
        this->client_.stop();
    }

    Peer(const Peer&) = delete;
    Peer& operator=(const Peer&) = delete;

    std::string url() const {
        return sendspin::server::test::peer_url(this->port_);
    }

    sendspin::SendspinClient& client() {
        return this->client_;
    }

    const sendspin::server::test::Sink& sink() const {
        return *this->sink_;
    }

    const TrustListener& trust() const {
        return this->trust_;
    }

    /// Runs the client's main loop until done() holds, with no timeout
    template <typename Predicate>
    void run_until(Predicate done) {
        sendspin::server::test::run_until(this->client_, done);
    }

private:
    static sendspin::SendspinClientConfig make_config(uint16_t port, const Options& options) {
        sendspin::SendspinClientConfig config =
            sendspin::server::test::peer_config("sendspin-cpp peer", port);
        if (options.pairing_psk) {
            config.pairing_psk = sendspin::SendspinPsk(*options.pairing_psk);
        }
        return config;
    }

    uint16_t port_;
    sendspin::server::test::AlwaysReady network_;
    TrustListener trust_;
    sendspin::SendspinClient client_;
    std::unique_ptr<sendspin::server::test::Sink> sink_;
};

/// Collects the plaintext of the server/activate messages and the log lines a server produces
class Capture {
public:
    void trace(const TracedMessage& message) {
        const std::string text(message.plaintext.begin(), message.plaintext.end());
        if (message.direction == TraceDirection::OUTBOUND &&
            text.find(R"("type":"server/activate")") != std::string::npos) {
            const std::lock_guard<std::mutex> lock(this->mutex_);
            this->activations_.push_back(text.substr(1));
        }
    }

    void log(std::string_view line) {
        const std::lock_guard<std::mutex> lock(this->mutex_);
        this->lines_.emplace_back(line);
    }

    std::vector<std::string> activations() const {
        const std::lock_guard<std::mutex> lock(this->mutex_);
        return this->activations_;
    }

    std::vector<std::string> lines() const {
        const std::lock_guard<std::mutex> lock(this->mutex_);
        return this->lines_;
    }

private:
    mutable std::mutex mutex_;
    std::vector<std::string> activations_;
    std::vector<std::string> lines_;
};

/// A ServerCore with every client approved, running on its own thread until stopped, which then
/// destroys it as a run's core thread does
class Core {
public:
    explicit Core(sendspin::server::ClientRecords records = {},
                  sendspin::server::PersistenceProvider* provider = nullptr)
        : dispatcher(std::make_shared<sendspin::server::Dispatcher>()),
          identity(*Identity::generate()) {
        this->settings.approve_all_clients = true;
        // The capture first, so that an activation the recorder lists is captured
        this->settings.trace = [this](const TracedMessage& message) {
            this->capture.trace(message);
            this->recorder.trace(message);
        };
        this->core = std::make_unique<sendspin::server::ServerCore>(
            this->loop, this->workers, *this->dispatcher, &this->recorder, this->identity,
            this->settings, this->snapshot, this->groups, this->inputs, this->group_ids, provider,
            std::move(records));
        this->thread = std::thread([this] {
            this->loop.run();
            this->core.reset();
        });
    }

    ~Core() {
        this->stop();
        this->dispatcher->finish();
        this->dispatcher->join();
    }

    /// Shuts the core down and waits until its thread destroyed it; the dispatcher runs on
    void stop() {
        if (this->thread.joinable()) {
            this->loop.post([this] { this->core->shutdown(0); });
            this->thread.join();
        }
    }

    Core(const Core&) = delete;
    Core& operator=(const Core&) = delete;

    /// Runs fn on the core thread and returns its result
    template <typename Fn>
    auto on_core(Fn fn) {
        std::promise<decltype(fn())> result;
        this->loop.post([&] {
            if constexpr (std::is_void_v<decltype(fn())>) {
                fn();
                result.set_value();
            } else {
                result.set_value(fn());
            }
        });
        return result.get_future().get();
    }

    void dial(const std::string& url) {
        this->loop.post(
            [this, url] { this->core->connect_url(url, *sendspin::server::parse_ws_url(url)); });
    }

    /// Opens an input on the server's group
    std::shared_ptr<sendspin::server::HostView> open_input(uint32_t rate) {
        auto view = std::make_shared<sendspin::server::HostView>(
            this->core->default_group(), sendspin::server::InputId{this->next_input++},
            sendspin::server::InputOptions{
                sendspin::server::InputFormat{rate, 2, sendspin::server::SampleFormat::S16}},
            sendspin::server::GroupParams{250000, 0, 0}, this->dispatcher, &this->recorder);
        EXPECT_TRUE(this->inputs.add(view));
        this->loop.post([this, view] { this->core->attach_input(view); });
        return view;
    }

    sendspin::server::EventLoop loop;
    sendspin::server::AudioWorkers workers{1};
    std::shared_ptr<sendspin::server::Dispatcher> dispatcher;
    Identity identity;
    sendspin::server::CoreSettings settings;
    sendspin::server::ClientSnapshot snapshot;
    sendspin::server::GroupSnapshot groups;
    sendspin::server::InputRegistry inputs;
    std::atomic<uint64_t> group_ids{1};
    Recorder recorder;
    Capture capture;
    std::unique_ptr<sendspin::server::ServerCore> core;
    std::thread thread;
    uint64_t next_input{1};
};

/// Writes the test signal into an input until stopped, from a thread of its own
class Feeder {
public:
    explicit Feeder(std::shared_ptr<sendspin::server::HostView> view)
        : view_(std::move(view)), thread_([this] { this->run(); }) {}

    ~Feeder() {
        this->stop_ = true;
        this->thread_.join();
    }

    Feeder(const Feeder&) = delete;
    Feeder& operator=(const Feeder&) = delete;

private:
    void run() {
        constexpr uint32_t BLOCK = 960;
        constexpr int64_t WAIT_US = 20000;
        const sendspin::server::test::Signal signal(16);
        Bytes block(size_t{BLOCK} * signal.frame_bytes());
        uint32_t position = 0;
        while (!this->stop_) {
            for (uint32_t i = 0; i < BLOCK; ++i) {
                signal.put(&block[i * signal.frame_bytes()], position + i);
            }
            uint32_t written = 0;
            while (written < BLOCK && !this->stop_) {
                written += static_cast<uint32_t>(
                    this->view_->write(&block[written * signal.frame_bytes()], BLOCK - written)
                        .frames);
                if (written < BLOCK) {
                    this->view_->wait_writable(WAIT_US);
                }
            }
            position += BLOCK;
        }
    }

    std::shared_ptr<sendspin::server::HostView> view_;
    std::atomic<bool> stop_{false};
    std::thread thread_;
};

inline std::string player_hello(const std::string& name) {
    return sendspin::server::test::player_hello_json(
        name, R"([{"codec":"pcm","sample_rate":48000,"channels":2,"bit_depth":16}])", 400000);
}

/// What a raw client player reads: chunks by timestamp and duration, JSON messages by text
struct Reading {
    std::optional<int64_t> ts;
    int64_t duration_us{0};
    std::string json;
};

/// The next message the raw client reads, as a Reading; json empty and ts unset once closed
inline Reading read_one(RawClient& client) {
    Reading reading;
    const std::optional<Bytes> plaintext = client.receive_plaintext();
    if (!plaintext || plaintext->empty()) {
        return reading;
    }
    if (plaintext->front() == wire::JSON_ID) {
        reading.json.assign(plaintext->begin() + 1, plaintext->end());
        return reading;
    }
    if (plaintext->front() == wire::AUDIO_CHUNK_ID &&
        plaintext->size() > wire::AUDIO_CHUNK_HEADER) {
        reading.ts = wire::timestamp_at(*plaintext);
        const auto frames =
            static_cast<int64_t>((plaintext->size() - wire::AUDIO_CHUNK_HEADER) / 4);
        reading.duration_us = frames * 1000000 / PLAYER_RATE;
    }
    return reading;
}

inline bool is_type(const std::string& json, const std::string& type) {
    return json.find(R"("type":")" + type + '"') != std::string::npos;
}

/// A provider whose first saves succeed or fail as scripted; every later save succeeds
class ScriptedProvider : public MemoryProvider {
public:
    explicit ScriptedProvider(std::vector<bool> accepted = {}) : accepted_(std::move(accepted)) {}

    bool save_blob(const std::string& key, const uint8_t* data, size_t len) override {
        const auto index = static_cast<size_t>(this->saves);
        if (index < this->accepted_.size() && !this->accepted_[index]) {
            ++this->saves;
            return false;
        }
        return MemoryProvider::save_blob(key, data, len);
    }

private:
    std::vector<bool> accepted_;
};

/// A provider whose saves wait for release(), then succeed or fail as scripted, and fail when told
/// to
class HeldProvider : public ScriptedProvider {
public:
    using ScriptedProvider::ScriptedProvider;

    bool save_blob(const std::string& key, const uint8_t* data, size_t len) override {
        std::unique_lock<std::mutex> lock(this->mutex_);
        this->changed_.wait(lock, [this] { return this->released_ > 0; });
        --this->released_;
        lock.unlock();
        return ScriptedProvider::save_blob(key, data, len);
    }

    /// Lets count saves through
    void release(int count) {
        {
            const std::lock_guard<std::mutex> lock(this->mutex_);
            this->released_ += count;
        }
        this->changed_.notify_all();
    }

private:
    std::mutex mutex_;
    std::condition_variable changed_;
    int released_{0};
};

}  // namespace sendspin::server::test
