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

/// @file test_server_source.cpp
/// @brief The source role end to end (spec roles/source/v1.md): sources activated only with
/// their approval, a routed source's audio streamed into a group and played by its players, PCM
/// and FLAC bit for bit and Opus decoded, a stall's backlog dropped and a long gap starting a new
/// run, and the protocol errors that close a source's connection. The sources are in-process
/// sendspin-cpp clients and the raw test client (for FLAC, which the sendspin-cpp source does not
/// send, timestamps of the test's choosing and misbehavior); the players are sendspin-cpp clients.

#include "clock.h"
#include "crypto/keys.h"
#include "fragmentation.h"
#include "frame_encoder.h"
#include "opus_reader.h"
#include "peer.h"
#include "player_sink.h"
#include "raw_client.h"
#include "test_ports.h"
#include "test_recorder.h"
#include <ArduinoJson.h>
#include <gtest/gtest.h>

#include <sendspin/client.h>
#include <sendspin/player_role.h>
#include <sendspin/server/server.h>
#include <sendspin/source_role.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using sendspin::server::ApprovalScope;
using sendspin::server::GroupInput;
using sendspin::server::RouteSourceResult;
using sendspin::server::SendspinServer;
using sendspin::server::ServerConfig;
using sendspin::server::StartResult;
using sendspin::server::StreamCodec;
using sendspin::server::StreamFormat;
using sendspin::server::TraceDirection;
using sendspin::server::TracedMessage;
using sendspin::server::test::AlwaysReady;
using sendspin::server::test::Bytes;
using sendspin::server::test::MemoryProvider;
using sendspin::server::test::RawClient;
using sendspin::server::test::Recorder;
using sendspin::server::test::Signal;
using sendspin::server::test::Sink;
using Codec = sendspin::SendspinCodecFormat;

constexpr uint32_t RATE = 48000;
/// A 20 ms chunk's frames
constexpr uint32_t CHUNK = RATE / 50;

// ==============================================================================
// What the server sent each client
// ==============================================================================

/// From the message trace: each player's PCM frames, decoded to the signal's positions, its
/// player streams' starts and ends, and the active_roles of each client's last server/activate
class SourceTrace {
public:
    explicit SourceTrace(const Signal& signal) : signal_(signal) {}

    /// On the dispatch thread
    void on_message(const TracedMessage& message) {
        if (message.direction != TraceDirection::OUTBOUND || !message.binary ||
            message.plaintext.empty()) {
            return;
        }
        const std::lock_guard<std::mutex> lock(this->mutex);
        Client& client = this->clients[message.client_id];
        constexpr size_t CHUNK_HEADER = 13;
        if (message.plaintext[0] == 4 && message.plaintext.size() > CHUNK_HEADER) {
            const size_t frame_bytes = client.frame_bytes == 0 ? 1 : client.frame_bytes;
            for (size_t at = CHUNK_HEADER; at + frame_bytes <= message.plaintext.size();
                 at += frame_bytes) {
                const uint8_t* frame = &message.plaintext[at];
                client.samples.push_back(static_cast<int16_t>(frame[0] | (frame[1] << 8)));
                if (const std::optional<uint32_t> position = this->signal_.get(frame)) {
                    client.positions.push_back(*position);
                } else {
                    ++client.altered;
                }
            }
            return;
        }
        if (message.plaintext[0] != 0) {
            return;
        }
        JsonDocument doc;
        if (deserializeJson(doc, message.plaintext.data() + 1, message.plaintext.size() - 1) !=
            DeserializationError::Ok) {
            return;
        }
        if (doc["type"] == "stream/start" && !doc["payload"]["player"].isNull()) {
            ++client.starts;
            client.frame_bytes = doc["payload"]["player"]["channels"].as<size_t>() *
                                 doc["payload"]["player"]["bit_depth"].as<size_t>() / 8;
        }
        if (doc["type"] == "stream/end") {
            ++client.ends;
        }
        if (doc["type"] == "server/activate" && doc["payload"]["active_roles"].is<JsonArray>()) {
            client.roles.clear();
            for (const JsonVariantConst role :
                 doc["payload"]["active_roles"].as<JsonArrayConst>()) {
                client.roles.push_back(role.as<std::string>());
            }
        }
    }

    struct Client {
        size_t frame_bytes{0};
        std::vector<uint32_t> positions;
        /// The left channel of every frame, as 16-bit samples
        std::vector<int16_t> samples;
        size_t altered{0};
        size_t starts{0};
        size_t ends{0};
        std::vector<std::string> roles;
    };

    std::mutex mutex;
    std::map<std::string, Client> clients;

private:
    const Signal& signal_;
};

// ==============================================================================
// sendspin-cpp peers: players, sources, and both
// ==============================================================================

/// A sendspin-cpp client the server dials, with a player role, a source role, or both; its source
/// captures in real time from a thread of its own while the client runs
class Peer : public sendspin::SourceRoleListener {
public:
    /// @param bits the player's and the PCM source's depth; an Opus source captures 16 bits
    Peer(bool player, std::optional<Codec> source, uint32_t bits)
        : port_(sendspin::server::test::free_port()),
          client_(sendspin::server::test::peer_config("Peer " + std::to_string(this->port_),
                                                      this->port_)) {
        if (player) {
            sendspin::PlayerRoleConfig config;
            config.audio_formats.push_back({Codec::PCM, 2, RATE, static_cast<uint8_t>(bits)});
            sendspin::PlayerRole& role = this->client_.add_player(std::move(config));
            this->sink_ = std::make_unique<Sink>(role, bits / 8 * 2, RATE);
            role.set_listener(this->sink_.get());
        }
        if (source) {
            sendspin::SourceRoleConfig config;
            config.codec = *source;
            config.sample_rate = RATE;
            config.channels = 2;
            config.bit_depth = static_cast<uint8_t>(*source == Codec::OPUS ? 16 : bits);
            this->source_ = &this->client_.add_source(config);
            this->source_->set_listener(this);
        }
        this->client_.set_network_provider(&this->network_);
        this->client_.set_unpaired_access_enabled(true);
        EXPECT_TRUE(this->client_.start());
    }

    ~Peer() override {
        this->stop_capture();
        this->client_.stop();
    }

    Peer(const Peer&) = delete;
    Peer& operator=(const Peer&) = delete;

    void on_streaming_started() override {
        ++this->starts;
    }

    void on_streaming_stopped() override {
        ++this->stops;
    }

    /// Writes 10 ms blocks, put() filling each frame of a position, as a capture device delivers
    /// them; a block the source refuses is a gap
    void start_capture(size_t frame_bytes, std::function<void(uint8_t*, uint32_t)> put) {
        this->capture_ = std::thread([this, frame_bytes, put = std::move(put)] {
            constexpr uint32_t BLOCK = RATE / 100;
            Bytes block(BLOCK * frame_bytes);
            uint32_t position = 0;
            auto next = std::chrono::steady_clock::now();
            while (!this->stopping_.load()) {
                next += std::chrono::milliseconds(10);
                std::this_thread::sleep_until(next);
                for (uint32_t i = 0; i < BLOCK; ++i) {
                    put(&block[i * frame_bytes], position + i);
                }
                (void)this->source_->write_audio(block.data(), block.size(), 0);
                position += BLOCK;
            }
        });
    }

    void stop_capture() {
        this->stopping_.store(true);
        if (this->capture_.joinable()) {
            this->capture_.join();
        }
    }

    std::string url() const {
        return sendspin::server::test::peer_url(this->port_);
    }

    sendspin::SendspinClient& client() {
        return this->client_;
    }

    const Sink& sink() const {
        return *this->sink_;
    }

    // Main loop only
    int starts{0};
    int stops{0};

private:
    uint16_t port_;
    AlwaysReady network_;
    sendspin::SendspinClient client_;
    std::unique_ptr<Sink> sink_;
    sendspin::SourceRole* source_{nullptr};
    std::thread capture_;
    std::atomic<bool> stopping_{false};
};

/// The server, its trace and its sendspin-cpp peers
class Rig {
public:
    explicit Rig(const Signal& signal, bool opus = false) : trace(signal) {
        ServerConfig config = sendspin::server::test::local_config();
        config.name = "Sources";
        // Playback alone, which the source role needs more than
        config.approve_all_clients = true;
        config.enable_opus = opus;
        config.trace = [this](const TracedMessage& message) { this->trace.on_message(message); };
        EXPECT_EQ(this->server.start(config, &this->provider, &this->recorder), StartResult::OK);
    }

    /// Adds a peer, which the server dials, and runs every peer until it is connected
    Peer& add(bool player, std::optional<Codec> source, uint32_t bits = 24) {
        this->peers.push_back(std::make_unique<Peer>(player, source, bits));
        const std::string url = this->peers.back()->url();
        EXPECT_TRUE(this->server.connect_url(url));
        this->run_until([&] {
            const std::optional<std::string> id = this->find_client_id(url);
            return id && this->recorder.contains("connected " + *id);
        });
        return *this->peers.back();
    }

    std::optional<std::string> find_client_id(const std::string& url) const {
        for (const sendspin::server::ClientInfo& client : this->server.clients()) {
            if (client.url == url) {
                return client.client_id;
            }
        }
        return std::nullopt;
    }

    std::string client_id(const Peer& peer) const {
        return this->find_client_id(peer.url()).value_or(std::string{});
    }

    /// Runs every peer's main loop on this thread until done() holds, with no timeout
    template <typename Predicate>
    void run_until(Predicate done) {
        sendspin::server::test::run_all_until(this->peers, done);
    }

    /// A copy of what the trace holds for client_id
    SourceTrace::Client traced(const std::string& client_id) {
        const std::lock_guard<std::mutex> lock(this->trace.mutex);
        return this->trace.clients[client_id];
    }

    sendspin::server::GroupId group() const {
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access): set while the server runs
        return *this->server.default_group();
    }

    MemoryProvider provider;
    Recorder recorder;
    SourceTrace trace;
    SendspinServer server;
    std::vector<std::unique_ptr<Peer>> peers;
};

/// Positions in a row, each one more than the one before, from the start of each stretch: the
/// frames the server streamed come from the source in order, none altered or repeated
void expect_rising(const std::vector<uint32_t>& positions) {
    for (size_t i = 1; i < positions.size(); ++i) {
        ASSERT_GT(positions[i], positions[i - 1]) << "at " << i;
    }
}

// ==============================================================================
// sendspin-cpp sources
// ==============================================================================

// spec roles/source/v1.md "Unpaired access": approval for playback does not cover the source role,
// which an unpaired client gets only with its own approval. Routed, the source is asked to start
// and streams 24-bit PCM, which the group's players (one of them the source device itself, which
// plays only what its group streams it) are streamed bit for bit, in order; releasing the route's
// input stops it. Takes about 3 s: two seconds of audio captured in real time.
TEST(ServerSources, StreamPcmBitForBitToTheGroupsPlayers) {
    const Signal signal(24);
    Rig s(signal);
    Peer& player = s.add(true, std::nullopt);
    Peer& source = s.add(true, Codec::PCM);
    const std::string source_id = s.client_id(source);
    const std::string player_id = s.client_id(player);
    EXPECT_EQ(s.traced(source_id).roles, std::vector<std::string>{"player@v1"});
    EXPECT_FALSE(s.recorder.contains("source ready " + source_id));

    ASSERT_TRUE(s.server.approve(source_id, ApprovalScope::PLAYBACK_AND_SOURCE));
    s.run_until([&] { return s.recorder.contains("source ready " + source_id); });
    EXPECT_EQ(s.traced(source_id).roles, (std::vector<std::string>{"player@v1", "source@v1"}));

    sendspin::server::RoutedSource routed = s.server.route_source(source_id, s.group());
    ASSERT_EQ(routed.result, RouteSourceResult::OK);
    source.start_capture(signal.frame_bytes(),
                         [&](uint8_t* out, uint32_t position) { signal.put(out, position); });
    constexpr size_t HEARD = 2 * RATE;
    s.run_until([&] {
        return s.traced(player_id).positions.size() >= HEARD &&
               s.traced(source_id).positions.size() >= HEARD;
    });
    EXPECT_TRUE(s.recorder.contains(Recorder::source_started(
        source_id, s.group(), StreamFormat{StreamCodec::PCM, RATE, 2, 24})));

    routed.input.reset();
    s.run_until([&] {
        return source.stops > 0 && s.recorder.contains("source stopped " + source_id + " " +
                                                       std::to_string(s.group().value));
    });
    source.stop_capture();
    s.run_until([&] { return player.sink().played_out(); });
    for (const std::string& id : {player_id, source_id}) {
        SCOPED_TRACE(id);
        const SourceTrace::Client client = s.traced(id);
        EXPECT_EQ(client.altered, 0U);
        expect_rising(client.positions);
    }
    // The player plays what it was streamed, but for its own sync corrections; how much of it is
    // real time, which a player that fell behind its clock drops at the stream's end
    size_t played = 0;
    const Bytes output = player.sink().played();
    for (size_t at = 0; at + signal.frame_bytes() <= output.size(); at += signal.frame_bytes()) {
        played += signal.get(&output[at]) ? 1 : 0;
    }
    EXPECT_GT(played, 0U);
}

// roles/source/v1.md "client/hello source@v1 support object": a server MAY accept Opus, which it
// lists when ServerConfig::enable_opus is set; the source then streams it, and the server decodes
// it for its players. Takes about 2 s: a second of audio captured in real time.
TEST(ServerSources, DecodeAnOpusSourceForThePlayers) {
    const Signal signal(16);
    Rig s(signal, true);
    Peer& player = s.add(true, std::nullopt, 16);
    Peer& source = s.add(false, Codec::OPUS);
    const std::string source_id = s.client_id(source);
    const std::string player_id = s.client_id(player);
    ASSERT_TRUE(s.server.approve(source_id, ApprovalScope::PLAYBACK_AND_SOURCE));
    s.run_until([&] { return s.recorder.contains("source ready " + source_id); });
    const sendspin::server::RoutedSource routed = s.server.route_source(source_id, s.group());
    ASSERT_EQ(routed.result, RouteSourceResult::OK);
    source.start_capture(4, [](uint8_t* out, uint32_t position) {
        for (uint32_t channel = 0; channel < 2; ++channel) {
            const int16_t sample = sendspin::server::test::tone(position, channel, RATE);
            out[2 * channel] = static_cast<uint8_t>(sample);
            out[(2 * channel) + 1] = static_cast<uint8_t>(static_cast<uint16_t>(sample) >> 8);
        }
    });
    s.run_until([&] { return s.traced(player_id).samples.size() >= RATE; });
    source.stop_capture();
    EXPECT_TRUE(s.recorder.contains(Recorder::source_started(
        source_id, s.group(), StreamFormat{StreamCodec::OPUS, RATE, 2, 0})));
    // The tone, at a third of full scale, decoded: its RMS is about 7,000 past the stream's start
    const std::vector<int16_t> samples = s.traced(player_id).samples;
    double sum = 0;
    for (size_t i = samples.size() / 2; i < samples.size(); ++i) {
        sum += static_cast<double>(samples[i]) * samples[i];
    }
    EXPECT_GT(std::sqrt(sum / static_cast<double>(samples.size() - (samples.size() / 2))), 3000.0);
}

// ==============================================================================
// The raw test client as a source
// ==============================================================================

constexpr const char* AVAILABLE =
    R"({"type":"client/state","payload":{"available":true,"source":{}}})";

/// The raw client, approved for its source and dialed, through its activation and its
/// client/state
void connect_raw_source(Rig& s, RawClient& raw) {
    ASSERT_TRUE(
        s.server.approve(raw.noise.identity().peer_id(), ApprovalScope::PLAYBACK_AND_SOURCE));
    ASSERT_TRUE(s.server.connect_url(raw.url()));
    ASSERT_NO_FATAL_FAILURE(raw.activate(sendspin::server::test::source_hello_json("Turntable")));
    raw.send_json(AVAILABLE);
}

/// A source audio message, encrypted: the ID, the big-endian capture time and the audio
void send_chunk(RawClient& raw, int64_t capture_us, const Bytes& audio) {
    Bytes plaintext{12};
    for (int shift = 56; shift >= 0; shift -= 8) {
        plaintext.push_back(static_cast<uint8_t>(static_cast<uint64_t>(capture_us) >> shift));
    }
    plaintext.insert(plaintext.end(), audio.begin(), audio.end());
    raw.send_binary(raw.noise.encrypt(plaintext));
}

void sleep_until_us(int64_t at_us) {
    while (sendspin::server::now_us() < at_us) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

// roles/source/v1.md "Codec framing": a source's FLAC frames, after the codec_header its
// client-stream/start carries, reach the players bit for bit. "Source Audio Chunks (Binary)": after
// a stall, a backlog sent at once is dropped beyond the input's write jitter rather than burst (it
// would raise the live input's latency), and a gap of a second or more in the capture times starts
// a new run; the stream's end ends the run. Takes about 5 s: the chunks go out in real time, since
// the server reads the stall and the gap from now_us().
TEST(ServerSources, StreamFlacAndRideOutAStallAndAGap) {
    const Signal signal(24);
    Rig s(signal);
    Peer& player = s.add(true, std::nullopt);
    const std::string player_id = s.client_id(player);
    RawClient raw;
    const std::string source_id = raw.noise.identity().peer_id();
    // Routed before it connects: it is asked to start once ready
    const sendspin::server::RoutedSource routed = s.server.route_source(source_id, s.group());
    ASSERT_EQ(routed.result, RouteSourceResult::OK);

    // Chunks 50 to 74 wait out a 500 ms stall and go at once; 100 to 164 the source dropped
    constexpr int STALL_FIRST = 50;
    constexpr int STALL_END = 75;
    constexpr int GAP_FIRST = 100;
    constexpr int GAP_END = 165;
    constexpr int LAST = 215;
    std::atomic<bool> done{false};
    std::atomic<bool> finish{false};
    std::thread source([&] {
        ASSERT_NO_FATAL_FAILURE(connect_raw_source(s, raw));
        EXPECT_NE(raw.receive_until("server/command").find("start"), std::string::npos);
        const sendspin::server::sched::AudioFormat format{sendspin::server::sched::Codec::FLAC,
                                                          RATE, 2, 24};
        raw.send_json(R"({"type":"client-stream/start","payload":{"source":{"codec":"flac",)"
                      R"("channels":2,"sample_rate":48000,"bit_depth":24,"codec_header":")" +
                      *sendspin::server::codec_header(format) + R"("}}})");
        const std::unique_ptr<sendspin::server::FrameEncoder> encoder =
            sendspin::server::FrameEncoder::create(format);
        const int64_t base = sendspin::server::now_us() + 20000;
        Bytes pcm(CHUNK * signal.frame_bytes());
        const auto send = [&](int k) {
            for (uint32_t i = 0; i < CHUNK; ++i) {
                signal.put(&pcm[i * signal.frame_bytes()], (k * CHUNK) + i);
            }
            Bytes frame;
            ASSERT_TRUE(encoder->encode(pcm, sendspin::server::SampleFormat::S24, frame));
            send_chunk(raw, base + (int64_t{k} * 20000), frame);
        };
        const auto live = [&](int from, int to) {
            for (int k = from; k < to; ++k) {
                sleep_until_us(base + (int64_t{k + 1} * 20000));
                send(k);
            }
        };
        live(0, STALL_FIRST);
        sleep_until_us(base + (int64_t{STALL_END} * 20000));
        for (int k = STALL_FIRST; k < STALL_END; ++k) {
            send(k);
        }
        live(STALL_END, GAP_FIRST);
        live(GAP_END, LAST);
        raw.send_json(R"({"type":"client-stream/end","payload":{}})");
        done.store(true);
        // The connection stays open until the players heard the end, so the end ends the run
        while (!finish.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        raw.close();
    });
    s.run_until([&] {
        const SourceTrace::Client client = s.traced(player_id);
        return done.load() && client.starts >= 2 && client.ends == client.starts &&
               s.recorder.contains("source stopped " + source_id + " " +
                                   std::to_string(s.group().value));
    });
    finish.store(true);
    source.join();

    const SourceTrace::Client client = s.traced(player_id);
    EXPECT_EQ(client.altered, 0U);
    expect_rising(client.positions);
    const auto heard = [&](int first, int end) {
        return std::count_if(client.positions.begin(), client.positions.end(),
                             [&](uint32_t p) { return p >= first * CHUNK && p < end * CHUNK; });
    };
    EXPECT_GT(heard(0, STALL_FIRST), 0);
    // A chunk sent more than the 60 ms jitter after it was captured is backlog
    EXPECT_EQ(heard(STALL_FIRST, STALL_END - 4), 0) << "the backlog played";
    EXPECT_GT(heard(STALL_END, GAP_FIRST), 0);
    EXPECT_GT(heard(GAP_END, LAST), 0);
    EXPECT_TRUE(s.recorder.contains(Recorder::source_started(
        source_id, s.group(), StreamFormat{StreamCodec::FLAC, RATE, 2, 24})));
}

// A routed source's open stream is reported where it plays: routed into another group, as stopped
// in the first and started in the second, the first input closed; a second route into the group it
// plays in is busy; released and routed again, it is reported started again
TEST(ServerSources, ReportTheOpenStreamWhereverItIsRouted) {
    const Signal signal(16);
    Rig s(signal);
    Peer& player = s.add(true, std::nullopt, 16);
    RawClient raw;
    const std::string id = raw.noise.identity().peer_id();
    const sendspin::server::RoutedSource first = s.server.route_source(id, s.group());
    ASSERT_EQ(first.result, RouteSourceResult::OK);
    ASSERT_NO_FATAL_FAILURE(connect_raw_source(s, raw));
    EXPECT_NE(raw.receive_until("server/command").find("start"), std::string::npos);
    raw.send_json(
        R"({"type":"client-stream/start","payload":{"source":{"codec":"pcm","channels":2,)"
        R"("sample_rate":48000,"bit_depth":16}}})");
    const StreamFormat format{StreamCodec::PCM, RATE, 2, 16};
    const auto started = [&](sendspin::server::GroupId group) {
        return Recorder::source_started(id, group, format);
    };
    const auto stopped = [&](sendspin::server::GroupId group) {
        return "source stopped " + id + " " + std::to_string(group.value);
    };
    s.run_until([&] { return s.recorder.contains(started(s.group())); });
    EXPECT_EQ(s.server.route_source(id, s.group()).result, RouteSourceResult::GROUP_BUSY);

    const std::optional<sendspin::server::GroupId> second =
        s.server.create_group("Second", s.client_id(player));
    ASSERT_TRUE(second);
    s.run_until(
        [&] { return s.recorder.contains("group " + std::to_string(second->value) + " stopped"); });
    sendspin::server::RoutedSource moved = s.server.route_source(id, *second);
    ASSERT_EQ(moved.result, RouteSourceResult::OK);
    s.run_until([&] {
        return s.recorder.contains(stopped(s.group())) && s.recorder.contains(started(*second));
    });
    EXPECT_EQ(first.input->state(), sendspin::server::InputState::CLOSED);

    moved.input.reset();
    s.run_until([&] { return s.recorder.contains(stopped(*second)); });
    const sendspin::server::RoutedSource again = s.server.route_source(id, s.group());
    ASSERT_EQ(again.result, RouteSourceResult::OK);
    s.run_until([&] { return s.recorder.count(started(s.group())) == 2; });
    raw.close();
}

// roles/source/v1.md "Unpaired access": an approval for playback activates the client without its
// source role; one with it adds the role, and revoking it removes every role
TEST(ServerSources, ActivateTheSourceRoleOnlyWithItsApproval) {
    const Signal signal(16);
    Rig s(signal);
    RawClient raw;
    const std::string id = raw.noise.identity().peer_id();
    ASSERT_TRUE(s.server.connect_url(raw.url()));
    ASSERT_NO_FATAL_FAILURE(raw.handshake());
    raw.send_json(sendspin::server::test::source_hello_json("Turntable"));
    EXPECT_NE(raw.receive_until("server/activate").find(R"("active_roles":[])"), std::string::npos);
    ASSERT_TRUE(s.server.approve(id, ApprovalScope::PLAYBACK_AND_SOURCE));
    EXPECT_NE(raw.receive_until("server/activate").find(R"("active_roles":["source@v1"])"),
              std::string::npos);
    ASSERT_TRUE(s.server.revoke_approval(id));
    EXPECT_NE(raw.receive_until("server/activate").find(R"("active_roles":[])"), std::string::npos);
    raw.close();
}

// A route's least latency holds its source's audio back at least that long after its capture,
// however little the group's players need (about 300 ms here): the first sample of a chunk
// captured now plays a second or more from now. A least latency out of range refuses the route.
TEST(ServerSources, PlayARoutedSourceNoSoonerThanItsLeastLatency) {
    const Signal signal(16);
    Rig s(signal);
    struct Row {
        const char* name;
        int64_t min_latency_us;
        RouteSourceResult result;
    };
    const Row rows[] = {
        {"Below 0", -1, RouteSourceResult::INVALID_LATENCY},
        {"Past the largest", sendspin::server::MAX_SOURCE_MIN_LATENCY_US + 1,
         RouteSourceResult::INVALID_LATENCY},
        {"Control: the largest", sendspin::server::MAX_SOURCE_MIN_LATENCY_US,
         RouteSourceResult::OK},
    };
    for (const Row& row : rows) {
        SCOPED_TRACE(row.name);
        RawClient raw;
        const std::string id = raw.noise.identity().peer_id();
        sendspin::server::SourceRouteOptions options;
        options.min_latency_us = row.min_latency_us;
        const sendspin::server::RoutedSource routed = s.server.route_source(id, s.group(), options);
        ASSERT_EQ(routed.result, row.result);
        if (row.result != RouteSourceResult::OK) {
            EXPECT_EQ(routed.input, nullptr);
            continue;
        }
        ASSERT_NO_FATAL_FAILURE(connect_raw_source(s, raw));
        EXPECT_NE(raw.receive_until("server/command").find("start"), std::string::npos);
        raw.send_json(
            R"({"type":"client-stream/start","payload":{"source":{"codec":"pcm","channels":2,)"
            R"("sample_rate":48000,"bit_depth":16}}})");
        const int64_t captured = sendspin::server::now_us();
        send_chunk(raw, captured, Bytes(size_t{CHUNK} * 4, 1));
        s.recorder.wait_for("group " + std::to_string(s.group().value) + " playing");
        EXPECT_GE(routed.input->time_of(0) - captured, row.min_latency_us);
        raw.close();
    }
}

// roles/source/v1.md "Default streaming behavior", "Codec framing" and "Source Audio Chunks
// (Binary)": the server closes the connection of a source that opens a stream it was not asked
// for, sends a chunk longer than 150 ms, or a chunk its codec does not frame; a chunk of exactly
// 150 ms plays
TEST(ServerSources, CloseASourceThatBreaksTheStreamRules) {
    const Signal signal(16);
    Rig s(signal);
    const std::string pcm =
        R"({"type":"client-stream/start","payload":{"source":{"codec":"pcm","channels":2,)"
        R"("sample_rate":48000,"bit_depth":16}}})";
    const sendspin::server::sched::AudioFormat flac{sendspin::server::sched::Codec::FLAC, RATE, 2,
                                                    16};
    const std::string flac_start =
        R"({"type":"client-stream/start","payload":{"source":{"codec":"flac","channels":2,)"
        R"("sample_rate":48000,"bit_depth":16,"codec_header":")" +
        *sendspin::server::codec_header(flac) + R"("}}})";
    struct Row {
        const char* name;
        bool routed;
        std::string start;
        Bytes chunk;
        bool closes;
    };
    const Row rows[] = {
        {"Control: a chunk of 150 ms", true, pcm, Bytes(size_t{7200} * 4, 1), false},
        {"an unsolicited start", false, pcm, {}, true},
        {"a chunk past 150 ms", true, pcm, Bytes(size_t{7201} * 4, 1), true},
        {"a chunk that is not FLAC", true, flac_start, Bytes(64, 0x55), true},
    };
    for (const Row& row : rows) {
        SCOPED_TRACE(row.name);
        RawClient raw;
        const std::string id = raw.noise.identity().peer_id();
        std::shared_ptr<GroupInput> input;
        if (row.routed) {
            input = s.server.route_source(id, s.group()).input;
            ASSERT_TRUE(input);
        }
        ASSERT_NO_FATAL_FAILURE(connect_raw_source(s, raw));
        if (row.routed) {
            EXPECT_NE(raw.receive_until("server/command").find("start"), std::string::npos);
        }
        raw.send_json(row.start);
        if (!row.chunk.empty()) {
            send_chunk(raw, sendspin::server::now_us(), row.chunk);
        }
        const std::string disconnected =
            "disconnected " + id + " " +
            std::to_string(static_cast<int>(sendspin::server::DisconnectReason::CLOSED_BY_SERVER));
        if (row.closes) {
            raw.read_until_closed();
            s.recorder.wait_for(disconnected);
            continue;
        }
        // The chunk reached the input, which started the group's run, and the source is
        // connected still: it answers a time request
        s.recorder.wait_for("group " + std::to_string(s.group().value) + " playing");
        raw.send_json(sendspin::server::test::client_time_json(1));
        EXPECT_FALSE(raw.receive_until("server/time").empty());
        EXPECT_FALSE(s.recorder.contains(disconnected));
        raw.close();
    }
}

}  // namespace
