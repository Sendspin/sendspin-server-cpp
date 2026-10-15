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

/// @file test_server_pairing.cpp
/// @brief Pairing end to end through the public API against in-process sendspin-cpp clients:
/// every method, the reconnect under the long-term PSK, a wrong code and a wrong token, the round
/// limit, cancelling, unpairing, pairing a paired client again, and pairing beside playback.

#include "client_records.h"
#include "crypto/base64url.h"
#include "crypto/keys.h"
#include "crypto/pairing_token.h"
#include "pairing.h"
#include "peer.h"
#include "player_sink.h"
#include "test_ports.h"
#include "test_recorder.h"
#include "trust_fixtures.h"
#include <ArduinoJson.h>
#include <gtest/gtest.h>
#include <noise/protocol.h>

#include <sendspin/client.h>
#include <sendspin/player_role.h>
#include <sendspin/server/server.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace {

using sendspin::server::ClientInfo;
using sendspin::server::ClientRecord;
using sendspin::server::CodeFormat;
using sendspin::server::Identity;
using sendspin::server::PairingOptions;
using sendspin::server::PairingResult;
using sendspin::server::PairMethod;
using sendspin::server::Psk;
using sendspin::server::SendspinServer;
using sendspin::server::ServerConfig;
using sendspin::server::StartResult;
using sendspin::server::TraceDirection;
using sendspin::server::TracedMessage;
using sendspin::server::test::ClientStore;
using sendspin::server::test::MemoryProvider;
using sendspin::server::test::psk_of;
using sendspin::server::test::Recorder;
using sendspin::server::test::stored_record;
namespace persistence_keys = sendspin::server::persistence_keys;

using Bytes = std::vector<uint8_t>;

constexpr uint32_t RATE = 48000;

/// What a sendspin-cpp client reports of its pairing, on its main loop (the test thread)
class PairingListener : public sendspin::SendspinClientListener {
public:
    void on_trust_changed(sendspin::ConnectionTrust trust) override {
        this->trust = trust;
    }

    void on_pairing_started(const std::string& /*server_id*/) override {
        ++this->started;
    }

    void on_pairing_succeeded(const std::string& /*server_id*/) override {
        ++this->succeeded;
    }

    void on_pairing_failed(const std::string& /*server_id*/,
                           sendspin::SendspinPairAbortReason reason) override {
        this->failures.push_back(reason);
    }

    void on_display_pairing_code(const std::string& code,
                                 sendspin::SendspinPairingCodeFormat /*format*/) override {
        this->code = code;
        ++this->displays;
    }

    void on_clear_pairing_code() override {
        this->code.reset();
    }

    void on_open_pairing_window() override {
        ++this->windows;
    }

    sendspin::ConnectionTrust trust{sendspin::ConnectionTrust::NONE};
    int started{0};
    int succeeded{0};
    int displays{0};
    int windows{0};
    std::optional<std::string> code;
    std::vector<sendspin::SendspinPairAbortReason> failures;
};

/// A sendspin-cpp client listening on a loopback port for the server to dial, with what it offers
/// for pairing; a player with a real-time audio output when asked. Its main loop runs on the test
/// thread through run_until().
class Peer {
public:
    struct Options {
        bool unpaired_access{false};
        std::optional<Psk> pairing_psk;
        std::optional<std::string> static_code;
        std::vector<sendspin::SendspinPairingCodeFormat> formats{
            sendspin::SendspinPairingCodeFormat::DIGITS};
        bool player{false};
    };

    Peer(ClientStore& store, const Options& options)
        : port_(sendspin::server::test::free_port()), client_(make_config(port_, options)) {
        if (options.player) {
            sendspin::PlayerRoleConfig player;
            player.audio_formats.assign(1, sendspin::AudioSupportedFormatObject{
                                               sendspin::SendspinCodecFormat::PCM, 2, RATE, 16});
            player.min_buffer_ms = 150;
            sendspin::PlayerRole& role = this->client_.add_player(player);
            this->sink_ = std::make_unique<sendspin::server::test::Sink>(role, 4, RATE);
            role.set_listener(this->sink_.get());
        }
        this->client_.set_network_provider(&this->network_);
        this->client_.set_persistence_provider(&store);
        this->client_.set_listener(&this->listener_);
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

    PairingListener& listener() {
        return this->listener_;
    }

    const sendspin::server::test::Sink& sink() const {
        return *this->sink_;
    }

    /// Runs the client's main loop until done() holds, with no timeout
    template <typename Predicate>
    void run_until(Predicate done) {
        sendspin::server::test::run_until(this->client_, done);
    }

    /// Runs the client's main loop until done() holds or about bound passed, so a test can report
    /// what it saw before the suite watchdog would end it
    /// @return whether done() holds
    template <typename Predicate>
    bool run_until_within(Predicate done, std::chrono::milliseconds bound) {
        const auto end = std::chrono::steady_clock::now() + bound;
        while (!done() && std::chrono::steady_clock::now() < end) {
            this->client_.loop();
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return done();
    }

private:
    static sendspin::SendspinClientConfig make_config(uint16_t port, const Options& options) {
        sendspin::SendspinClientConfig config =
            sendspin::server::test::peer_config("pairing peer", port);
        if (options.pairing_psk) {
            config.pairing_psk = sendspin::SendspinPsk(*options.pairing_psk);
        }
        config.static_pairing_code = options.static_code;
        if (!options.static_code) {
            // Assigned by count: GCC 13 -Warray-bounds misreads an initializer-list assignment
            config.pairing_code_out_channels.assign(1,
                                                    sendspin::SendspinPairingCodeChannel::DISPLAY);
            config.pairing_code_formats = options.formats;
        }
        return config;
    }

    uint16_t port_;
    sendspin::server::test::AlwaysReady network_;
    PairingListener listener_;
    sendspin::SendspinClient client_;
    std::unique_ptr<sendspin::server::test::Sink> sink_;
};

/// The plaintext of every traced message, with its direction and client, and the log lines
class Capture {
public:
    struct Message {
        TraceDirection direction;
        std::string client_id;
        std::string text;
    };

    void trace(const TracedMessage& message) {
        const std::lock_guard<std::mutex> lock(this->mutex_);
        const size_t offset = std::min<size_t>(message.binary ? 1 : 0, message.plaintext.size());
        this->messages_.push_back(
            {message.direction, message.client_id,
             std::string(message.plaintext.begin() + static_cast<std::ptrdiff_t>(offset),
                         message.plaintext.end())});
    }

    void log(std::string_view line) {
        const std::lock_guard<std::mutex> lock(this->mutex_);
        this->lines_.emplace_back(line);
    }

    /// The JSON of each traced message of type in direction, in order; of client_id's
    /// connections only when it is not empty
    std::vector<std::string> of_type(TraceDirection direction, const std::string& type,
                                     const std::string& client_id = {}) const {
        const std::lock_guard<std::mutex> lock(this->mutex_);
        std::vector<std::string> found;
        for (const Message& message : this->messages_) {
            if (message.direction == direction && is_type(message.text, type) &&
                (client_id.empty() || message.client_id == client_id)) {
                found.push_back(message.text);
            }
        }
        return found;
    }

    /// Every message of client_id's connections, in order
    std::vector<Message> of_client(const std::string& client_id) const {
        const std::lock_guard<std::mutex> lock(this->mutex_);
        std::vector<Message> found;
        for (const Message& message : this->messages_) {
            if (message.client_id == client_id) {
                found.push_back(message);
            }
        }
        return found;
    }

    /// Every traced plaintext and log line
    std::vector<std::string> everything() const {
        const std::lock_guard<std::mutex> lock(this->mutex_);
        std::vector<std::string> all = this->lines_;
        for (const Message& message : this->messages_) {
            all.push_back(message.text);
        }
        return all;
    }

    static bool is_type(const std::string& text, const std::string& type) {
        return text.find(R"("type":")" + type + '"') != std::string::npos;
    }

private:
    mutable std::mutex mutex_;
    std::vector<Message> messages_;
    std::vector<std::string> lines_;
};

/// A server whose key is known before it starts, so a client can hold a record for it
struct KnownServer {
    KnownServer() : identity(*Identity::generate()) {
        this->provider.blobs[persistence_keys::SERVER_KEY] =
            Bytes(this->identity.private_bytes.begin(), this->identity.private_bytes.end());
    }

    ServerConfig config() {
        ServerConfig config = sendspin::server::test::local_config();
        config.name = "Pairing server";
        config.trace = [this](const TracedMessage& message) {
            this->capture.trace(message);
            this->recorder.trace(message);
        };
        config.log = [this](sendspin::server::LogLevel /*level*/, std::string_view line) {
            this->capture.log(line);
        };
        config.log_level = sendspin::server::LogLevel::INFO;
        return config;
    }

    StartResult start(const ServerConfig& config) {
        return this->server.start(config, &this->provider, &this->recorder);
    }

    Identity identity;
    MemoryProvider provider;
    Recorder recorder;
    Capture capture;
    SendspinServer server;
};

/// Whether client_id's pairings reported count results
bool has_result(const KnownServer& known, const std::string& client_id, size_t count = 1) {
    const std::string prefix = "pairing result " + client_id + " ";
    const std::vector<Recorder::Entry> entries = known.recorder.entries();
    return static_cast<size_t>(
               std::count_if(entries.begin(), entries.end(), [&](const Recorder::Entry& entry) {
                   return entry.what.starts_with(prefix);
               })) >= count;
}

/// The result of client_id's count-th pairing, which must be its last, with every pairing event
/// delivered on the dispatch thread (the thread the provider was loaded on)
PairingResult result_of(const KnownServer& known, const std::string& client_id, size_t count = 1) {
    const std::string prefix = "pairing result " + client_id + " ";
    std::vector<std::string> results;
    for (const Recorder::Entry& entry : known.recorder.entries()) {
        if (entry.what.starts_with("pairing ") || entry.what.starts_with("code needed ")) {
            EXPECT_EQ(entry.thread, known.provider.thread) << entry.what;
        }
        if (entry.what.starts_with(prefix)) {
            results.push_back(entry.what);
        }
    }
    EXPECT_EQ(results.size(), count);
    return results.empty()
               ? PairingResult::SERVER_STOPPED
               : static_cast<PairingResult>(std::stoi(results.back().substr(prefix.size())));
}

/// Dials peer and runs it until the server reports it connected
void connect(KnownServer& known, Peer& peer) {
    ASSERT_TRUE(known.server.connect_url(peer.url()));
    const std::string id = peer.client().client_id();
    peer.run_until([&] { return known.recorder.contains("connected " + id); });
}

/// Fails for each secret that appears in a traced message or a log line, as it is or in
/// base64url, the encoding messages carry it in (pairing.md "Messages")
void expect_no_secret(const Capture& capture, const std::vector<Bytes>& secrets) {
    const std::vector<std::string> all = capture.everything();
    for (const Bytes& secret : secrets) {
        const std::string raw(secret.begin(), secret.end());
        const std::string encoded = sendspin::server::b64url_encode(secret.data(), secret.size());
        for (const std::string& text : all) {
            EXPECT_EQ(text.find(raw), std::string::npos) << text;
            EXPECT_EQ(text.find(encoded), std::string::npos) << text;
        }
    }
}

/// Runs the main loops of every peer until done() holds, with no timeout
template <typename Predicate>
void run_all(std::initializer_list<Peer*> peers, Predicate done) {
    sendspin::server::test::run_all_until(peers, done);
}

/// Writes the test signal into an input from a thread of its own until destroyed
class SignalFeeder {
public:
    explicit SignalFeeder(std::shared_ptr<sendspin::server::GroupInput> input)
        : input_(std::move(input)), thread_([this] { this->run(); }) {}

    ~SignalFeeder() {
        this->stop_ = true;
        this->thread_.join();
        this->input_->stop();
    }

    SignalFeeder(const SignalFeeder&) = delete;
    SignalFeeder& operator=(const SignalFeeder&) = delete;

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
                    this->input_->write(&block[written * signal.frame_bytes()], BLOCK - written)
                        .frames);
                if (written < BLOCK) {
                    this->input_->wait_writable(WAIT_US);
                }
            }
            position += BLOCK;
        }
    }

    std::shared_ptr<sendspin::server::GroupInput> input_;
    std::atomic<bool> stop_{false};
    std::thread thread_;
};

/// Opens an input on the server's group and feeds it the signal
std::unique_ptr<SignalFeeder> feed(SendspinServer& server) {
    const std::optional<sendspin::server::GroupId> group = server.default_group();
    EXPECT_TRUE(group);
    sendspin::server::InputOptions options;
    options.format = {RATE, 2, sendspin::server::SampleFormat::S16};
    sendspin::server::OpenedInput opened = server.open_input(*group, options);
    EXPECT_EQ(opened.result, sendspin::server::OpenInputResult::OK);
    return std::make_unique<SignalFeeder>(std::move(opened.input));
}

/// The code a client shows with its last digit changed
std::string wrong_code(const std::string& code) {
    std::string wrong = code;
    wrong.back() = wrong.back() == '0' ? '1' : '0';
    return wrong;
}

// pairing.md "Dynamic Pairing Code Flow": the operator types the code the client shows. The record
// is saved before server/pair-finalize, the session is re-handshaken to the new long-term PSK, and
// a later connection is a paired session under it (connection.md "Pre-Shared Key").
TEST(ServerPairing, PairsWithADynamicCodeAndReconnectsUnderTheLongTermPsk) {
    ASSERT_EQ(noise_init_framework(), NOISE_ERROR_NONE);
    KnownServer known;
    ClientStore store;
    Peer peer(store, {});
    const std::string id = peer.client().client_id();
    ASSERT_EQ(known.start(known.config()), StartResult::OK);
    ASSERT_NO_FATAL_FAILURE(connect(known, peer));

    ASSERT_TRUE(known.server.begin_pairing(
        id, PairingOptions{PairMethod::DYNAMIC_PAIRING_CODE, CodeFormat::DIGITS}));
    const std::string needed =
        Recorder::code_needed(id, PairMethod::DYNAMIC_PAIRING_CODE, CodeFormat::DIGITS);
    peer.run_until([&] { return peer.listener().code && known.recorder.contains(needed); });
    const std::string code = *peer.listener().code;
    ASSERT_TRUE(known.server.submit_pairing_code(id, code));
    peer.run_until([&] {
        return has_result(known, id) && peer.listener().trust == sendspin::ConnectionTrust::USER;
    });
    EXPECT_EQ(result_of(known, id), PairingResult::PAIRED);
    EXPECT_EQ(peer.listener().succeeded, 1);
    const std::optional<ClientRecord> record = stored_record(known.provider, 0);
    ASSERT_TRUE(record);
    ASSERT_TRUE(record->long_term_psk);
    // The code and the long-term PSK, which travelled wrapped, are in no trace or log line
    ASSERT_FALSE(
        known.capture.of_type(TraceDirection::INBOUND, "client/pair-finalize", id).empty());
    expect_no_secret(known.capture,
                     {Bytes(code.begin(), code.end()),
                      Bytes(record->long_term_psk->begin(), record->long_term_psk->end())});

    known.server.stop();
    peer.run_until([&] { return !peer.client().is_connected(); });
    ASSERT_EQ(known.start(known.config()), StartResult::OK);
    ASSERT_TRUE(known.server.connect_url(peer.url()));
    peer.listener().trust = sendspin::ConnectionTrust::NONE;
    peer.run_until([&] {
        return known.recorder.count("connected " + id) == 2 &&
               peer.listener().trust == sendspin::ConnectionTrust::USER;
    });
    const std::vector<ClientInfo> clients = known.server.clients();
    ASSERT_EQ(clients.size(), 1U);
    EXPECT_TRUE(clients[0].paired);
}

// pairing.md "QR-code emission": with an operator interface that scans, the activation asks for the
// `qr_code` format and the operator scans the version-1 token the client shows (messaging.md
// "server/activate")
TEST(ServerPairing, PairsWithAScannedDynamicCode) {
    ASSERT_EQ(noise_init_framework(), NOISE_ERROR_NONE);
    KnownServer known;
    ClientStore store;
    Peer peer(store, {false,
                      std::nullopt,
                      std::nullopt,
                      {sendspin::SendspinPairingCodeFormat::DIGITS,
                       sendspin::SendspinPairingCodeFormat::QR_CODE},
                      false});
    const std::string id = peer.client().client_id();
    ServerConfig config = known.config();
    config.operator_can_scan_qr = true;
    ASSERT_EQ(known.start(config), StartResult::OK);
    ASSERT_NO_FATAL_FAILURE(connect(known, peer));

    ASSERT_TRUE(known.server.begin_pairing(
        id, PairingOptions{PairMethod::DYNAMIC_PAIRING_CODE, std::nullopt}));
    const std::string needed =
        Recorder::code_needed(id, PairMethod::DYNAMIC_PAIRING_CODE, CodeFormat::QR_CODE);
    peer.run_until([&] { return peer.listener().code && known.recorder.contains(needed); });
    EXPECT_TRUE(peer.listener().code->starts_with("SP:1")) << *peer.listener().code;
    ASSERT_TRUE(known.server.submit_pairing_code(id, *peer.listener().code));
    peer.run_until([&] {
        return has_result(known, id) && peer.listener().trust == sendspin::ConnectionTrust::USER;
    });
    EXPECT_EQ(result_of(known, id), PairingResult::PAIRED);
    const std::vector<std::string> activations =
        known.capture.of_type(TraceDirection::OUTBOUND, "server/activate", id);
    EXPECT_TRUE(std::any_of(activations.begin(), activations.end(), [](const std::string& text) {
        return text.find(R"("format":"qr_code")") != std::string::npos;
    }));
}

// pairing.md "Static Pairing Code Flow": the client holds the attempt back with
// client/pair-pending until its pairing window opens, which the operator sees; a code entered
// before then (with its separator) waits for client/pair-init, so server/pair-auth follows it
// ("Signal to the server")
TEST(ServerPairing, PairsWithAStaticCodeOnceTheWindowOpens) {
    ASSERT_EQ(noise_init_framework(), NOISE_ERROR_NONE);
    KnownServer known;
    ClientStore store;
    Peer peer(store, {false, std::nullopt, "24681357", {}, false});
    const std::string id = peer.client().client_id();
    ASSERT_EQ(known.start(known.config()), StartResult::OK);
    ASSERT_NO_FATAL_FAILURE(connect(known, peer));

    ASSERT_TRUE(known.server.begin_pairing(
        id, PairingOptions{PairMethod::STATIC_PAIRING_CODE, std::nullopt}));
    peer.run_until([&] {
        return peer.listener().windows == 1 &&
               known.recorder.contains(
                   Recorder::code_needed(id, PairMethod::STATIC_PAIRING_CODE, CodeFormat::DIGITS));
    });
    ASSERT_TRUE(known.server.submit_pairing_code(id, "2468-1357"));
    peer.run_until([&] {
        for (const auto& entry : known.recorder.entries()) {
            if (entry.what.starts_with("pairing pending " + id)) {
                return true;
            }
        }
        return false;
    });
    peer.client().confirm_pairing_window();
    peer.run_until([&] {
        return has_result(known, id) && peer.listener().trust == sendspin::ConnectionTrust::USER;
    });
    EXPECT_EQ(result_of(known, id), PairingResult::PAIRED);
    // server/pair-auth after client/pair-init (pairing.md "client/pair-init")
    const std::vector<Capture::Message> messages = known.capture.of_client(id);
    const auto position = [&](TraceDirection direction, const std::string& type) {
        return std::find_if(messages.begin(), messages.end(), [&](const Capture::Message& message) {
            return message.direction == direction && Capture::is_type(message.text, type);
        });
    };
    const auto init = position(TraceDirection::INBOUND, "client/pair-init");
    const auto auth = position(TraceDirection::OUTBOUND, "server/pair-auth");
    ASSERT_NE(init, messages.end());
    ASSERT_NE(auth, messages.end());
    EXPECT_LT(init - messages.begin(), auth - messages.begin());
}

// pairing.md "Pairing PSK Flow": the token the client shows starts the pairing; a session under the
// Sentinel is re-handshaken to the pairing PSK first, and the client's long-term PSK arrives in
// client/pair-finalize. A token that does not decode, or whose client key is another client's, is
// refused ("Pairing Token").
TEST(ServerPairing, PairsWithTheClientsPairingToken) {
    ASSERT_EQ(noise_init_framework(), NOISE_ERROR_NONE);
    KnownServer known;
    ClientStore store;
    Peer peer(store, {false, psk_of(0x40), std::nullopt, {}, false});
    const std::string id = peer.client().client_id();
    ASSERT_EQ(known.start(known.config()), StartResult::OK);
    ASSERT_NO_FATAL_FAILURE(connect(known, peer));
    const std::optional<std::string> token = peer.client().pairing_token();
    ASSERT_TRUE(token);

    const Identity other = *Identity::generate();
    EXPECT_FALSE(known.server.submit_pairing_token(
        id, sendspin::server::format_pairing_token(other.public_bytes, psk_of(0x40))));
    EXPECT_FALSE(known.server.submit_pairing_token(id, "SP:0NOTATOKEN"));
    ASSERT_TRUE(known.server.submit_pairing_token(id, *token));
    peer.run_until([&] {
        return has_result(known, id) && peer.listener().trust == sendspin::ConnectionTrust::USER;
    });
    EXPECT_EQ(result_of(known, id), PairingResult::PAIRED);
    const std::vector<std::string> activations =
        known.capture.of_type(TraceDirection::OUTBOUND, "server/activate", id);
    EXPECT_TRUE(std::any_of(activations.begin(), activations.end(), [](const std::string& text) {
        return text.find(R"("method":"pairing_psk")") != std::string::npos;
    }));
    const std::optional<ClientRecord> record = stored_record(known.provider, 0);
    ASSERT_TRUE(record);
    ASSERT_TRUE(record->long_term_psk);
    // The pairing PSK, and the long-term PSK client/pair-finalize carries as it is, are redacted
    // from the trace and in no log line
    const std::vector<std::string> finalize =
        known.capture.of_type(TraceDirection::INBOUND, "client/pair-finalize", id);
    ASSERT_EQ(finalize.size(), 1U);
    EXPECT_NE(finalize[0].find(R"("long_term_psk":"<redacted>")"), std::string::npos)
        << finalize[0];
    const Psk pairing_psk = psk_of(0x40);
    expect_no_secret(known.capture,
                     {Bytes(pairing_psk.begin(), pairing_psk.end()),
                      Bytes(record->long_term_psk->begin(), record->long_term_psk->end()),
                      Bytes(token->begin(), token->end())});
}

// A client that could not use the token's pairing PSK. It refuses the re-handshake to it
// (connection.md "Re-handshake": a miss fails), so the server dials it again referencing the
// pairing PSK in the initial handshake, where it falls back to the Sentinel (connection.md
// "Sentinel Fallback"): the host hears TOKEN_MISMATCH and nothing is saved.
TEST(ServerPairing, ReportsATokenTheClientCannotUse) {
    ASSERT_EQ(noise_init_framework(), NOISE_ERROR_NONE);
    KnownServer known;
    ClientStore store;
    Peer peer(store, {false, psk_of(0x40), std::nullopt, {}, false});
    const std::string id = peer.client().client_id();
    ASSERT_EQ(known.start(known.config()), StartResult::OK);
    ASSERT_NO_FATAL_FAILURE(connect(known, peer));
    const std::optional<sendspin::server::PublicKey> key =
        sendspin::server::public_key_from_peer_id(id);
    ASSERT_TRUE(key);
    ASSERT_TRUE(known.server.submit_pairing_token(
        id, sendspin::server::format_pairing_token(*key, psk_of(0x41))));
    peer.run_until([&] { return has_result(known, id); });
    EXPECT_EQ(peer.listener().started, 0);
    EXPECT_EQ(result_of(known, id), PairingResult::TOKEN_MISMATCH);
    EXPECT_FALSE(stored_record(known.provider, 0));
    EXPECT_NE(peer.listener().trust, sendspin::ConnectionTrust::USER);
}

// pairing.md "Rounds": a wrong code fails the client's check of server_kc, it asks for another
// round with client/pair-retry, and the operator is asked again; the code is unchanged, and the
// right one pairs
TEST(ServerPairing, RunsAnotherRoundAfterAWrongDynamicCode) {
    ASSERT_EQ(noise_init_framework(), NOISE_ERROR_NONE);
    KnownServer known;
    ClientStore store;
    Peer peer(store, {});
    const std::string id = peer.client().client_id();
    ASSERT_EQ(known.start(known.config()), StartResult::OK);
    ASSERT_NO_FATAL_FAILURE(connect(known, peer));
    ASSERT_TRUE(known.server.begin_pairing(
        id, PairingOptions{PairMethod::DYNAMIC_PAIRING_CODE, std::nullopt}));
    const std::string needed =
        Recorder::code_needed(id, PairMethod::DYNAMIC_PAIRING_CODE, CodeFormat::DIGITS);
    peer.run_until([&] { return peer.listener().code && known.recorder.contains(needed); });
    const std::string code = *peer.listener().code;
    ASSERT_TRUE(known.server.submit_pairing_code(id, wrong_code(code)));
    peer.run_until([&] { return known.recorder.count(needed) == 2; });
    EXPECT_EQ(known.capture.of_type(TraceDirection::INBOUND, "client/pair-retry", id).size(), 1U);
    ASSERT_TRUE(known.server.submit_pairing_code(id, code.substr(0, 3) + " " + code.substr(3)));
    peer.run_until([&] { return has_result(known, id); });
    EXPECT_EQ(peer.listener().displays, 1);
    EXPECT_EQ(result_of(known, id), PairingResult::PAIRED);
}

// pairing.md "Static Pairing Code Flow": one round; a wrong code fails the client's check and it
// aborts with `pairing_code_mismatch`. The connection stays, and nothing is saved.
TEST(ServerPairing, EndsAStaticPairingWithAWrongCode) {
    ASSERT_EQ(noise_init_framework(), NOISE_ERROR_NONE);
    KnownServer known;
    ClientStore store;
    Peer peer(store, {false, std::nullopt, "24681357", {}, false});
    const std::string id = peer.client().client_id();
    ASSERT_EQ(known.start(known.config()), StartResult::OK);
    ASSERT_NO_FATAL_FAILURE(connect(known, peer));
    ASSERT_TRUE(known.server.begin_pairing(
        id, PairingOptions{PairMethod::STATIC_PAIRING_CODE, std::nullopt}));
    ASSERT_TRUE(known.server.submit_pairing_code(id, "24681358"));
    peer.run_until([&] { return peer.listener().windows == 1; });
    peer.client().confirm_pairing_window();
    peer.run_until([&] { return has_result(known, id); });
    const std::vector<std::string> aborts =
        known.capture.of_type(TraceDirection::INBOUND, "pair/abort", id);
    EXPECT_EQ(result_of(known, id), PairingResult::CODE_MISMATCH);
    ASSERT_EQ(aborts.size(), 1U);
    EXPECT_NE(aborts[0].find(R"("reason":"pairing_code_mismatch")"), std::string::npos)
        << aborts[0];
    EXPECT_TRUE(peer.client().is_connected());
    EXPECT_FALSE(stored_record(known.provider, 0));
}

// pairing.md "Entering and leaving pairing": the operator's cancel sends pair/abort
// `user_cancelled`, then an activation without 'pairing'; the client stays connected, and a new
// pairing activation pairs it
TEST(ServerPairing, CancelsAPairingAndPairsAgainLater) {
    ASSERT_EQ(noise_init_framework(), NOISE_ERROR_NONE);
    KnownServer known;
    ClientStore store;
    Peer peer(store, {});
    const std::string id = peer.client().client_id();
    ASSERT_EQ(known.start(known.config()), StartResult::OK);
    ASSERT_NO_FATAL_FAILURE(connect(known, peer));
    const PairingOptions options{PairMethod::DYNAMIC_PAIRING_CODE, std::nullopt};
    ASSERT_TRUE(known.server.begin_pairing(id, options));
    peer.run_until([&] { return peer.listener().code.has_value(); });
    ASSERT_TRUE(known.server.cancel_pairing(id));
    // The activation follows the pair/abort the client acts on, so its trace may come later
    auto activated_without_pairing = [&] {
        const std::vector<std::string> activations =
            known.capture.of_type(TraceDirection::OUTBOUND, "server/activate", id);
        return !activations.empty() && activations.back().find("pairing") == std::string::npos;
    };
    // Diagnostic only: the bound, well under the suite's 55 s watchdog, lets a hang print the trace
    const bool ended = peer.run_until_within(
        [&] {
            return has_result(known, id) && !peer.listener().failures.empty() &&
                   !peer.listener().code && activated_without_pairing();
        },
        std::chrono::seconds(20));
    if (!ended) {
        std::string trace;
        for (const Capture::Message& message : known.capture.of_client(id)) {
            trace += (message.direction == TraceDirection::INBOUND ? "in  " : "out ") +
                     message.text + "\n";
        }
        FAIL() << "the cancel did not end with an activation without 'pairing'; trace:\n" << trace;
    }
    EXPECT_EQ(result_of(known, id), PairingResult::CANCELLED);
    EXPECT_EQ(peer.listener().failures[0], sendspin::SendspinPairAbortReason::USER_CANCELLED);
    EXPECT_TRUE(peer.client().is_connected());

    ASSERT_TRUE(known.server.begin_pairing(id, options));
    peer.run_until([&] { return peer.listener().code.has_value(); });
    ASSERT_TRUE(known.server.submit_pairing_code(id, *peer.listener().code));
    peer.run_until([&] { return has_result(known, id, 2); });
    EXPECT_EQ(result_of(known, id, 2), PairingResult::PAIRED);
}

// pairing.md "Rounds": after 20 rounds without a verified server_kc the client aborts and holds
// later attempts back with client/pair-pending, which the operator sees, until its operator acts
TEST(ServerPairing, StopsAtTheClientsRoundLimit) {
    ASSERT_EQ(noise_init_framework(), NOISE_ERROR_NONE);
    KnownServer known;
    ClientStore store;
    Peer peer(store, {});
    const std::string id = peer.client().client_id();
    ASSERT_EQ(known.start(known.config()), StartResult::OK);
    ASSERT_NO_FATAL_FAILURE(connect(known, peer));
    const PairingOptions options{PairMethod::DYNAMIC_PAIRING_CODE, std::nullopt};
    ASSERT_TRUE(known.server.begin_pairing(id, options));
    const std::string needed =
        Recorder::code_needed(id, PairMethod::DYNAMIC_PAIRING_CODE, CodeFormat::DIGITS);
    peer.run_until([&] { return peer.listener().code.has_value(); });
    const std::string code = *peer.listener().code;
    for (size_t round = 1; round <= sendspin::server::MAX_PAIRING_ROUNDS; ++round) {
        peer.run_until([&] { return known.recorder.count(needed) == round; });
        ASSERT_TRUE(known.server.submit_pairing_code(id, wrong_code(code)));
    }
    peer.run_until([&] { return has_result(known, id); });
    EXPECT_EQ(known.capture.of_type(TraceDirection::INBOUND, "client/pair-retry", id).size(),
              sendspin::server::MAX_PAIRING_ROUNDS - 1);
    EXPECT_EQ(result_of(known, id), PairingResult::CODE_MISMATCH);

    ASSERT_TRUE(known.server.begin_pairing(id, options));
    peer.run_until([&] {
        for (const auto& entry : known.recorder.entries()) {
            if (entry.what.starts_with("pairing pending " + id)) {
                return true;
            }
        }
        return false;
    });
    EXPECT_EQ(known.recorder.count(needed), sendspin::server::MAX_PAIRING_ROUNDS);
}

// messaging.md "server/unpair": the server removes its record and the paired session gets
// server/unpair; the client removes its own and leaves with `unpaired`, and the server does not
// dial it again (messaging.md "client/goodbye")
TEST(ServerPairing, UnpairsAPairedClient) {
    ASSERT_EQ(noise_init_framework(), NOISE_ERROR_NONE);
    KnownServer known;
    ClientStore store;
    Peer peer(store, {false, psk_of(0x40), std::nullopt, {}, false});
    const std::string id = peer.client().client_id();
    ASSERT_EQ(known.start(known.config()), StartResult::OK);
    ASSERT_NO_FATAL_FAILURE(connect(known, peer));
    ASSERT_TRUE(known.server.submit_pairing_token(id, *peer.client().pairing_token()));
    peer.run_until([&] {
        return has_result(known, id) && peer.listener().trust == sendspin::ConnectionTrust::USER;
    });
    EXPECT_EQ(result_of(known, id), PairingResult::PAIRED);

    ASSERT_TRUE(known.server.unpair(id));
    const std::vector<sendspin::server::ClientDisconnect> disconnects =
        known.recorder.wait_for_disconnects(1);
    EXPECT_EQ(disconnects[0].reason, sendspin::server::DisconnectReason::GOODBYE);
    EXPECT_EQ(disconnects[0].goodbye_reason, sendspin::server::GoodbyeReason::UNPAIRED);
    // persistence_keys.h: a removed record leaves an all-zero blob
    const Bytes& blob = known.provider.blobs[persistence_keys::client_record_key(0)];
    EXPECT_TRUE(std::all_of(blob.begin(), blob.end(), [](uint8_t byte) { return byte == 0; }));
    const std::vector<ClientInfo> clients = known.server.clients();
    ASSERT_EQ(clients.size(), 1U);
    EXPECT_FALSE(clients[0].connected);
    EXPECT_FALSE(clients[0].paired);
    EXPECT_FALSE(clients[0].reconnecting);
}

// Re-pairing a paired client: its playing session under the long-term PSK is re-handshaken to the
// Sentinel, which ends its stream before Noise message 1 since a paired client's unpaired session
// plays nothing (connection.md "Sentinel Fallback"); the new long-term PSK replaces the old, and
// the client plays again under it
TEST(ServerPairing, PairsAPairedClientAgainAndPlaysOn) {
    ASSERT_EQ(noise_init_framework(), NOISE_ERROR_NONE);
    KnownServer known;
    ClientStore store;
    const Psk old_psk = psk_of(0x60);
    store.hold_record(known.identity.peer_id(), old_psk);
    Peer peer(
        store,
        {false, std::nullopt, std::nullopt, {sendspin::SendspinPairingCodeFormat::DIGITS}, true});
    const std::string id = peer.client().client_id();
    ClientRecord record{id, std::nullopt};
    record.long_term_psk = old_psk;
    sendspin::server::ClientRecordBlob blob{};
    ASSERT_TRUE(sendspin::server::encode_client_record(record, blob));
    known.provider.blobs[persistence_keys::client_record_key(0)] = Bytes(blob.begin(), blob.end());
    ASSERT_EQ(known.start(known.config()), StartResult::OK);
    ASSERT_NO_FATAL_FAILURE(connect(known, peer));
    const std::unique_ptr<SignalFeeder> feeder = feed(known.server);
    peer.run_until([&] { return peer.sink().audible() >= RATE / 10; });

    ASSERT_TRUE(known.server.begin_pairing(
        id, PairingOptions{PairMethod::DYNAMIC_PAIRING_CODE, std::nullopt}));
    peer.run_until([&] { return peer.listener().code.has_value(); });
    EXPECT_EQ(known.capture.of_type(TraceDirection::OUTBOUND, "stream/end", id).size(), 1U);
    ASSERT_TRUE(known.server.submit_pairing_code(id, *peer.listener().code));
    peer.run_until([&] {
        return has_result(known, id) && peer.listener().trust == sendspin::ConnectionTrust::USER;
    });
    EXPECT_EQ(result_of(known, id), PairingResult::PAIRED);
    const size_t audible = peer.sink().audible();
    peer.run_until([&] { return peer.sink().audible() >= audible + RATE / 10; });
    EXPECT_EQ(known.capture.of_type(TraceDirection::OUTBOUND, "stream/start", id).size(), 2U);
    const std::optional<ClientRecord> stored = stored_record(known.provider, 0);
    ASSERT_TRUE(stored);
    ASSERT_TRUE(stored->long_term_psk);
    EXPECT_NE(*stored->long_term_psk, old_psk);
}

// pairing.md "Entering and leaving pairing": pairing runs alongside playback. An approved player
// under the Sentinel pairs on its playing connection with no stream message, and so does the
// other player of its group; after the re-handshake to its long-term PSK it plays on.
TEST(ServerPairing, PairsAlongsidePlayback) {
    ASSERT_EQ(noise_init_framework(), NOISE_ERROR_NONE);
    KnownServer known;
    ClientStore store_a;
    ClientStore store_b;
    Peer a(store_a,
           {true, std::nullopt, std::nullopt, {sendspin::SendspinPairingCodeFormat::DIGITS}, true});
    Peer b(store_b,
           {true, std::nullopt, std::nullopt, {sendspin::SendspinPairingCodeFormat::DIGITS}, true});
    const std::string id = a.client().client_id();
    ServerConfig config = known.config();
    config.approve_all_clients = true;
    ASSERT_EQ(known.start(config), StartResult::OK);
    ASSERT_TRUE(known.server.connect_url(a.url()));
    ASSERT_TRUE(known.server.connect_url(b.url()));
    const std::unique_ptr<SignalFeeder> feeder = feed(known.server);
    run_all({&a, &b},
            [&] { return a.sink().audible() >= RATE / 10 && b.sink().audible() >= RATE / 10; });

    ASSERT_TRUE(known.server.begin_pairing(
        id, PairingOptions{PairMethod::DYNAMIC_PAIRING_CODE, std::nullopt}));
    run_all({&a, &b}, [&] { return a.listener().code.has_value(); });
    const size_t audible_b = b.sink().audible();
    ASSERT_TRUE(known.server.submit_pairing_code(id, *a.listener().code));
    run_all({&a, &b}, [&] {
        return has_result(known, id) && a.listener().trust == sendspin::ConnectionTrust::USER;
    });
    EXPECT_EQ(result_of(known, id), PairingResult::PAIRED);
    const size_t audible_a = a.sink().audible();
    run_all({&a, &b}, [&] {
        return a.sink().audible() >= audible_a + RATE / 10 &&
               b.sink().audible() >= audible_b + RATE / 5;
    });
    for (const std::string& client_id : {id, b.client().client_id()}) {
        SCOPED_TRACE(client_id);
        EXPECT_EQ(known.capture.of_type(TraceDirection::OUTBOUND, "stream/start", client_id).size(),
                  1U);
        EXPECT_TRUE(
            known.capture.of_type(TraceDirection::OUTBOUND, "stream/end", client_id).empty());
        EXPECT_TRUE(
            known.capture.of_type(TraceDirection::OUTBOUND, "stream/clear", client_id).empty());
    }
    const std::vector<std::string> activations =
        known.capture.of_type(TraceDirection::OUTBOUND, "server/activate", id);
    EXPECT_TRUE(std::any_of(activations.begin(), activations.end(), [](const std::string& text) {
        return text.find(R"("activities":["playback","pairing"])") != std::string::npos;
    }));
}

/// The server's group's playback state, from groups()
sendspin::server::PlaybackState server_group_state(SendspinServer& server) {
    const std::optional<sendspin::server::GroupId> group = server.default_group();
    for (const sendspin::server::GroupInfo& info : server.groups()) {
        if (group && info.id == *group) {
            return info.playback_state;
        }
    }
    return sendspin::server::PlaybackState::STOPPED;
}

/// The client's approval, from clients()
std::optional<sendspin::server::ApprovalScope> approval_of(SendspinServer& server,
                                                           const std::string& client_id) {
    for (const ClientInfo& client : server.clients()) {
        if (client.client_id == client_id) {
            return client.approval;
        }
    }
    return std::nullopt;
}

/// The outbound server/activate after the count-th outbound pair/abort to client_id, checking
/// that none was sent between the pairing activation before that abort and the abort
std::string activation_after_abort(const Capture& capture, const std::string& client_id,
                                   size_t count) {
    const std::vector<Capture::Message> messages = capture.of_client(client_id);
    size_t aborts = 0;
    size_t abort_at = messages.size();
    for (size_t i = 0; i < messages.size(); ++i) {
        if (messages[i].direction == TraceDirection::OUTBOUND &&
            Capture::is_type(messages[i].text, "pair/abort") && ++aborts == count) {
            abort_at = i;
            break;
        }
    }
    if (abort_at == messages.size()) {
        return {};
    }
    size_t pairing_at = abort_at;
    while (pairing_at > 0 && !(Capture::is_type(messages[pairing_at].text, "server/activate") &&
                               messages[pairing_at].text.find("pairing") != std::string::npos)) {
        --pairing_at;
    }
    for (size_t i = pairing_at + 1; i < abort_at; ++i) {
        EXPECT_FALSE(messages[i].direction == TraceDirection::OUTBOUND &&
                     Capture::is_type(messages[i].text, "server/activate"))
            << messages[i].text;
    }
    for (size_t i = abort_at + 1; i < messages.size(); ++i) {
        if (messages[i].direction == TraceDirection::OUTBOUND &&
            Capture::is_type(messages[i].text, "server/activate")) {
            return messages[i].text;
        }
    }
    return {};
}

// While an attempt runs, the client's other activation changes wait. The group's playback stopping
// and the client's approval revoked, then the playback starting and the client approved, send no
// server/activate until the attempt ends; the activation that ends it carries the state they left.
TEST(ServerPairing, DefersActivationChangesUntilTheAttemptEnds) {
    ASSERT_EQ(noise_init_framework(), NOISE_ERROR_NONE);
    KnownServer known;
    ClientStore store;
    Peer peer(
        store,
        {true, std::nullopt, std::nullopt, {sendspin::SendspinPairingCodeFormat::DIGITS}, true});
    const std::string id = peer.client().client_id();
    ASSERT_EQ(known.start(known.config()), StartResult::OK);
    ASSERT_NO_FATAL_FAILURE(connect(known, peer));
    ASSERT_TRUE(known.server.approve(id, sendspin::server::ApprovalScope::PLAYBACK));
    std::unique_ptr<SignalFeeder> feeder = feed(known.server);
    peer.run_until([&] { return peer.sink().audible() >= RATE / 10; });
    const PairingOptions options{PairMethod::DYNAMIC_PAIRING_CODE, std::nullopt};

    // Stopped and revoked mid-attempt
    ASSERT_TRUE(known.server.begin_pairing(id, options));
    peer.run_until([&] { return peer.listener().code.has_value(); });
    feeder.reset();
    peer.run_until([&] {
        return server_group_state(known.server) == sendspin::server::PlaybackState::STOPPED;
    });
    ASSERT_TRUE(known.server.revoke_approval(id));
    peer.run_until([&] { return !approval_of(known.server, id); });
    ASSERT_TRUE(known.server.cancel_pairing(id));
    std::string ending;
    peer.run_until([&] {
        ending = activation_after_abort(known.capture, id, 1);
        return has_result(known, id) && !ending.empty();
    });
    EXPECT_EQ(result_of(known, id), PairingResult::CANCELLED);
    EXPECT_EQ(ending,
              R"({"type":"server/activate","payload":{"activities":[],"active_roles":[]}})");

    // Playback starts and the client is approved again mid-attempt
    peer.run_until([&] { return !peer.listener().code.has_value(); });
    ASSERT_TRUE(known.server.begin_pairing(id, options));
    peer.run_until([&] { return peer.listener().code.has_value(); });
    feeder = feed(known.server);
    peer.run_until([&] {
        return server_group_state(known.server) == sendspin::server::PlaybackState::PLAYING;
    });
    ASSERT_TRUE(known.server.approve(id, sendspin::server::ApprovalScope::PLAYBACK));
    peer.run_until([&] { return approval_of(known.server, id).has_value(); });
    ASSERT_TRUE(known.server.cancel_pairing(id));
    peer.run_until([&] {
        ending = activation_after_abort(known.capture, id, 2);
        return has_result(known, id, 2) && !ending.empty();
    });
    EXPECT_EQ(result_of(known, id, 2), PairingResult::CANCELLED);
    EXPECT_NE(ending.find(R"("activities":["playback"],"active_roles":["player@v1")"),
              std::string::npos)
        << ending;
}

}  // namespace
