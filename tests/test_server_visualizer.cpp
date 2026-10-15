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

/// @file test_server_visualizer.cpp
/// @brief The visualizer role on the wire, with raw clients: the stream/start it echoes, the
/// messages on the audio timeline, the stream's lifecycle with the group's playback, its order
/// among player chunks, its ledger and a configuration change in place; and end to end, with an
/// in-process sendspin-cpp client that plays and visualizes.

#include "clock.h"
#include "peer.h"
#include "player_sink.h"
#include "raw_client.h"
#include "sched/constants.h"
#include "test_ports.h"
#include "test_recorder.h"
#include <ArduinoJson.h>
#include <gtest/gtest.h>

#include <sendspin/client.h>
#include <sendspin/player_role.h>
#include <sendspin/server/server.h>
#include <sendspin/visualizer_role.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <numbers>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace {

using sendspin::server::DisconnectReason;
using sendspin::server::DrainOutcome;
using sendspin::server::GroupInput;
using sendspin::server::InputFormat;
using sendspin::server::InputOptions;
using sendspin::server::InputState;
using sendspin::server::SampleFormat;
using sendspin::server::SendspinServer;
using sendspin::server::ServerConfig;
using sendspin::server::StartResult;
using sendspin::server::TraceDirection;
using sendspin::server::TracedMessage;
using sendspin::server::WriteResult;
using sendspin::server::test::Bytes;
using sendspin::server::test::MemoryProvider;
using sendspin::server::test::RawClient;
using sendspin::server::test::Recorder;
using sendspin::server::test::run_until;

constexpr uint32_t RATE = 48000;
constexpr int64_t CELL_US = 20000;
constexpr size_t CHUNK_HEADER = 13;
constexpr uint8_t AUDIO_CHUNK = 4;
/// A tone at the center of an FFT bin of the 2048-sample window at RATE
constexpr double TONE_HZ = 44.0 * RATE / 2048;
constexpr const char* PCM16 =
    R"([{"codec":"pcm","channels":2,"sample_rate":48000,"bit_depth":16}])";
/// A spectrum of 8 bins from 0 to 4 kHz, as the client/state carries it and stream/start echoes it
constexpr const char* SPECTRUM =
    R"("spectrum":{"n_disp_bins":8,"scale":"lin","f_min":0,"f_max":4000})";
constexpr uint32_t BINS = 8;

/// A client/hello listing the visualizer with buffer_capacity, and the player when player is set
std::string hello_json(const std::string& name, uint32_t capacity, bool player) {
    std::string roles = R"("visualizer@v1")";
    std::string support =
        R"("visualizer@v1_support":{"buffer_capacity":)" + std::to_string(capacity) + "}";
    if (player) {
        roles = R"("player@v1",)" + roles;
        support += R"(,"player@v1_support":{"supported_formats":)" + std::string(PCM16) +
                   R"(,"buffer_capacity":2000000})";
    }
    return R"({"type":"client/hello","payload":{"name":")" + name + R"(","supported_roles":[)" +
           roles + "]," + support +
           R"(,"supported_pair_methods":{"pairing_psk":{}},"unpaired_access":{"enabled":true}}})";
}

/// A client/state with a visualizer object, and a player object for a player
std::string state_json(bool available, const std::string& visualizer, bool player) {
    std::string json = R"({"type":"client/state","payload":{"available":)" +
                       std::string(available ? "true" : "false") + R"(,"visualizer":)" + visualizer;
    if (player) {
        // A lead far above a trigger's, so a start written late still finds the first frame
        json += R"(,"player":)" + sendspin::server::test::player_state_json(150, 700, 0);
    }
    return json + "}}";
}

/// One message a client received, in order
struct Entry {
    /// The JSON type, "chunk", or "visualizer" for a visualization message
    std::string type;
    std::string json;
    int64_t ts{0};
    /// A visualization message's type byte and whole size
    uint8_t id{0};
    size_t bytes{0};
    Bytes data;
};

/// A raw client with the visualizer role, and the player when asked, that the server dials
class RawVisualizer {
public:
    /// @param state sends the client/state now; otherwise the test sends it
    RawVisualizer(SendspinServer& server, const std::string& name, uint32_t capacity,
                  const std::string& visualizer, bool player = false, bool available = true,
                  bool state = true)
        : player_(player) {
        this->id = this->client.noise.identity().peer_id();
        EXPECT_TRUE(server.connect_url(this->client.url()));
        this->client.activate(hello_json(name, capacity, player));
        if (state) {
            this->send_state(available, visualizer);
        }
    }

    void send_state(bool available, const std::string& visualizer) {
        this->client.send_json(state_json(available, visualizer, this->player_));
    }

    /// Reads and logs messages until done holds for the last one
    /// @return false when the connection closed first
    template <typename Done>
    bool read_until(Done done) {
        for (;;) {
            const std::optional<Bytes> plaintext = this->client.receive_plaintext();
            if (!plaintext) {
                return false;
            }
            Entry entry;
            const uint8_t id = plaintext->empty() ? 0 : plaintext->front();
            if (id == AUDIO_CHUNK || (id >= 16 && id <= 23)) {
                entry.type = id == AUDIO_CHUNK ? "chunk" : "visualizer";
                entry.id = id;
                entry.bytes = plaintext->size();
                for (size_t i = 1; i < 9 && i < plaintext->size(); ++i) {
                    entry.ts = (entry.ts << 8) | (*plaintext)[i];
                }
                const size_t header = id == AUDIO_CHUNK ? CHUNK_HEADER : 9;
                if (plaintext->size() > header) {
                    entry.data.assign(plaintext->begin() + static_cast<std::ptrdiff_t>(header),
                                      plaintext->end());
                }
            } else if (id == 0) {
                entry.json.assign(plaintext->begin() + 1, plaintext->end());
                JsonDocument doc;
                EXPECT_EQ(deserializeJson(doc, entry.json), DeserializationError::Ok);
                entry.type = doc["type"].as<std::string>();
            }
            this->log.push_back(entry);
            if (done(this->log.back())) {
                return true;
            }
        }
    }

    bool read_until_type(const std::string& type) {
        return this->read_until([&](const Entry& entry) { return entry.type == type; });
    }

    /// Reads until a stream message of type naming role
    bool read_until_stream(const std::string& type, const std::string& role) {
        return this->read_until([&](const Entry& entry) { return names(entry, type, role); });
    }

    /// Whether entry is a stream message of type for role: a stream/start carrying its object, or
    /// a stream/clear or stream/end listing it
    static bool names(const Entry& entry, const std::string& type, const std::string& role) {
        if (entry.type != type) {
            return false;
        }
        JsonDocument doc;
        deserializeJson(doc, entry.json);
        if (type == "stream/start") {
            return !doc["payload"][role].isNull();
        }
        for (const JsonVariantConst listed : doc["payload"]["roles"].as<JsonArrayConst>()) {
            if (listed.as<std::string>() == role) {
                return true;
            }
        }
        return false;
    }

    std::vector<const Entry*> of_type(const std::string& type) const {
        std::vector<const Entry*> out;
        for (const Entry& entry : this->log) {
            if (entry.type == type) {
                out.push_back(&entry);
            }
        }
        return out;
    }

    RawClient client;
    std::string id;
    std::vector<Entry> log;

private:
    bool player_;
};

/// A visualizer object asking for types at rate, with SPECTRUM
std::string request(const std::string& types, uint32_t rate) {
    return R"({"types":[)" + types + R"(],"rate_max":)" + std::to_string(rate) + "," + SPECTRUM +
           "}";
}

InputOptions s16_input() {
    InputOptions options;
    options.format = InputFormat{RATE, 2, SampleFormat::S16};
    return options;
}

/// Writes count frames of a TONE_HZ sine at amplitude from position on, silence before position
/// silent_until, waiting for room; stops early when the input refuses for any reason but FULL
InputState feed_tone(GroupInput& input, uint64_t position, uint64_t count, double amplitude,
                     uint64_t silent_until = 0) {
    constexpr uint64_t BLOCK = 960;
    Bytes block(BLOCK * 4);
    uint64_t written = 0;
    InputState state = InputState::OPEN;
    while (written < count) {
        const uint64_t n = std::min(BLOCK, count - written);
        for (uint64_t i = 0; i < n; ++i) {
            const uint64_t at = position + written + i;
            const double t = static_cast<double>(at) / RATE;
            const double level = at < silent_until ? 0.0 : amplitude;
            const auto sample = static_cast<int16_t>(
                std::lround(level * 32767.0 * std::sin(2.0 * std::numbers::pi * TONE_HZ * t)));
            const auto bits = static_cast<uint16_t>(sample);
            for (size_t channel = 0; channel < 2; ++channel) {
                block[(i * 4) + (channel * 2)] = static_cast<uint8_t>(bits);
                block[(i * 4) + (channel * 2) + 1] = static_cast<uint8_t>(bits >> 8);
            }
        }
        const WriteResult result = input.write(block.data(), n);
        written += result.frames;
        state = result.state;
        if (result.frames < n) {
            if (result.state != InputState::FULL) {
                return state;
            }
            input.wait_writable(std::numeric_limits<int64_t>::max());
        }
    }
    return state;
}

/// The server's outbound visualization messages to one client, with their stamps, the stream/clear
/// and stream/end messages naming the visualizer, and its audio chunks, from the message trace
struct VisualizerTrace {
    struct Sent {
        bool reset{false};
        /// A stream/start naming the visualizer
        bool start{false};
        int64_t stamp_us{0};
        int64_t ts{0};
        size_t bytes{0};
        /// A visualization message's type byte, or AUDIO_CHUNK
        uint8_t id{0};
        Bytes data;
    };

    void on_message(const TracedMessage& message) {
        if (message.direction != TraceDirection::OUTBOUND || !message.binary ||
            message.plaintext.empty()) {
            return;
        }
        const uint8_t id = message.plaintext.front();
        Sent sent;
        sent.stamp_us = message.stamp_us;
        if ((id >= 16 && id <= 23) || id == AUDIO_CHUNK) {
            for (size_t i = 1; i < 9; ++i) {
                sent.ts = (sent.ts << 8) | message.plaintext[i];
            }
            sent.bytes = message.plaintext.size();
            sent.id = id;
            const size_t header = id == AUDIO_CHUNK ? CHUNK_HEADER : 9;
            sent.data.assign(message.plaintext.begin() + static_cast<std::ptrdiff_t>(header),
                             message.plaintext.end());
        } else if (id == 0) {
            const std::string json(message.plaintext.begin() + 1, message.plaintext.end());
            const bool named = json.find("visualizer") != std::string::npos;
            sent.reset = named && (json.find("stream/clear") != std::string::npos ||
                                   json.find("stream/end") != std::string::npos);
            sent.start = named && json.find("stream/start") != std::string::npos;
            if (!sent.reset && !sent.start) {
                return;
            }
        } else {
            return;
        }
        const std::lock_guard<std::mutex> lock(this->mutex);
        this->sent[message.client_id].push_back(sent);
    }

    std::mutex mutex;
    std::map<std::string, std::vector<Sent>> sent;
};

/// A started server that approves every client and traces its visualization messages
class VisualizerServer {
public:
    VisualizerServer() {
        ServerConfig config = sendspin::server::test::local_config();
        config.name = "Visualizer";
        config.approve_all_clients = true;
        config.trace = [this](const TracedMessage& message) { this->trace.on_message(message); };
        EXPECT_EQ(this->server.start(config, &this->provider, &this->recorder), StartResult::OK);
    }

    /// Opens the default group's input once each client took its connection
    std::shared_ptr<GroupInput> open(std::initializer_list<const RawVisualizer*> clients) {
        for (const RawVisualizer* client : clients) {
            this->recorder.wait_for("connected " + client->id);
        }
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access): set while the server runs
        return this->server.open_input(*this->server.default_group(), s16_input()).input;
    }

    VisualizerTrace trace;
    MemoryProvider provider;
    Recorder recorder;
    SendspinServer server;
};

/// The messages of one periodic type a buffered stream sent, as their stamps show: each step after
/// the one before, or later only where the last instant missed had passed by the next one's stamp,
/// as the pump skips a message already late and an analysis behind real time the cells that ended
/// meanwhile (GroupAudioTest.RestartsALateAnalysisAtTheCellPlayingNow)
void expect_on_grid(const std::vector<VisualizerTrace::Sent>& messages, int64_t step) {
    for (size_t i = 1; i < messages.size(); ++i) {
        const VisualizerTrace::Sent& message = messages[i];
        const int64_t due = messages[i - 1].ts + step;
        EXPECT_GE(message.ts, due) << i;
        EXPECT_EQ((message.ts - due) % step, 0) << i;
        if (message.ts > due) {
            EXPECT_LE(message.ts - step, message.stamp_us) << "an instant skipped before its time";
        }
    }
}

/// The visualization messages between a stream/start for the visualizer and the next stream/end
/// for it, from the first start
std::vector<const Entry*> first_stream(const RawVisualizer& client) {
    std::vector<const Entry*> out;
    bool active = false;
    for (const Entry& entry : client.log) {
        if (RawVisualizer::names(entry, "stream/start", "visualizer")) {
            active = true;
        } else if (RawVisualizer::names(entry, "stream/end", "visualizer")) {
            if (active) {
                break;
            }
        } else if (entry.type == "visualizer") {
            EXPECT_TRUE(active) << "a visualization message outside a stream";
            out.push_back(&entry);
        }
    }
    return out;
}

uint16_t u16_at(const Bytes& data, size_t at) {
    return static_cast<uint16_t>((data[at] << 8) | data[at + 1]);
}

// ==============================================================================
// The stream and its messages
// ==============================================================================

// spec roles/visualizer/v1.md "stream/start visualizer object" and "Visualization Data (Binary)":
// one stream/start starts the player and the visualizer, echoing the requested types the server
// streams (no `beat`), the rate and the spectrum. Each message is `[type][ts][data]` of its type's
// exact size, in non-decreasing ts, the periodic ones on the rate's grid from the timeline's cell
// 0, so on the chunks' 20 ms grid every 40 ms at 25 per second; the tone shows in f_peak and the
// spectrum's bin, and a drain ends both streams. An instant is missing only where it was late
// (expect_on_grid()).
TEST(ServerVisualizer, StreamsTheEchoedConfigurationOnTheAudioTimeline) {
    VisualizerServer s;
    RawVisualizer client(s.server, "Both", 1000000,
                         request(R"("loudness","beat","f_peak","spectrum","peak")", 25), true);
    const std::shared_ptr<GroupInput> input = s.open({&client});
    ASSERT_TRUE(input);
    std::thread host([&] {
        feed_tone(*input, 0, RATE, 0.5);
        input->drain();
    });
    ASSERT_TRUE(client.read_until_stream("stream/end", "visualizer"));
    if (!RawVisualizer::names(client.log.back(), "stream/end", "player")) {
        ASSERT_TRUE(client.read_until_stream("stream/end", "player"));
    }
    host.join();
    s.recorder.wait_for(Recorder::drained(input->id(), DrainOutcome::PLAYED));

    const std::vector<const Entry*> starts = client.of_type("stream/start");
    ASSERT_EQ(starts.size(), 1U);
    JsonDocument doc;
    ASSERT_EQ(deserializeJson(doc, starts[0]->json), DeserializationError::Ok);
    EXPECT_FALSE(doc["payload"]["player"].isNull());
    std::string visualizer;
    serializeJson(doc["payload"]["visualizer"], visualizer);
    EXPECT_EQ(visualizer, R"({"types":["loudness","f_peak","spectrum","peak"],"rate_max":25,)"
                          R"("spectrum":{"n_disp_bins":8,"scale":"lin","f_min":0,"f_max":4000}})");

    const std::vector<const Entry*> chunks = client.of_type("chunk");
    const std::vector<const Entry*> messages = first_stream(client);
    ASSERT_FALSE(chunks.empty());
    ASSERT_FALSE(messages.empty());
    int64_t last = std::numeric_limits<int64_t>::min();
    std::vector<int64_t> loudness_ts;
    for (const Entry* message : messages) {
        EXPECT_GE(message->ts, last);
        last = message->ts;
        const std::map<uint8_t, size_t> sizes = {
            {16, 11}, {18, 13}, {19, 9 + (2 * BINS)}, {20, 10}};
        ASSERT_TRUE(sizes.contains(message->id)) << int{message->id};
        EXPECT_EQ(message->bytes, sizes.at(message->id));
        // A peak at a cell's start, and every periodic instant at 25 per second on a cell's too
        EXPECT_EQ((message->ts - chunks[0]->ts) % CELL_US, 0) << int{message->id};
        if (message->id == 16) {
            loudness_ts.push_back(message->ts);
        }
    }
    // At most a second of audio at 25 per second, every instant 40 ms after the one before
    EXPECT_LE(loudness_ts.size(), 26U);
    ASSERT_FALSE(loudness_ts.empty());
    std::vector<VisualizerTrace::Sent> loudness;
    {
        const std::lock_guard<std::mutex> lock(s.trace.mutex);
        for (const VisualizerTrace::Sent& sent : s.trace.sent[client.id]) {
            if (sent.reset && !loudness.empty()) {
                break;
            }
            if (sent.id == 16) {
                loudness.push_back(sent);
            }
        }
    }
    ASSERT_EQ(loudness.size(), loudness_ts.size());
    expect_on_grid(loudness, 2 * CELL_US);
    EXPECT_EQ((loudness_ts.front() - chunks[0]->ts) % CELL_US, 0);
    // The tone, half scale, past the first window of the stream: about -6 dB
    const auto later = std::find_if(messages.begin(), messages.end(), [&](const Entry* m) {
        return m->id == 18 && m->ts >= loudness_ts.front() + (4 * CELL_US);
    });
    ASSERT_NE(later, messages.end());
    EXPECT_EQ(u16_at((*later)->data, 0), static_cast<uint16_t>(std::lround(TONE_HZ)));
    EXPECT_NEAR(u16_at((*later)->data, 2), 58960, 200);
    const auto spectrum =
        std::find_if(later, messages.end(), [](const Entry* m) { return m->id == 19; });
    ASSERT_NE(spectrum, messages.end());
    std::vector<uint16_t> bins;
    for (uint32_t i = 0; i < BINS; ++i) {
        bins.push_back(u16_at((*spectrum)->data, 2 * size_t{i}));
    }
    EXPECT_EQ(std::max_element(bins.begin(), bins.end()) - bins.begin(), 2);
}

// ==============================================================================
// The stream's lifecycle
// ==============================================================================

/// The roles a stream message names, as "type role role"
std::string stream_event(const Entry& entry) {
    JsonDocument doc;
    deserializeJson(doc, entry.json);
    std::string event = entry.type;
    if (entry.type == "stream/start") {
        for (const char* role : {"player", "visualizer"}) {
            if (!doc["payload"][role].isNull()) {
                event += std::string(" ") + role;
            }
        }
        return event;
    }
    for (const JsonVariantConst role : doc["payload"]["roles"].as<JsonArrayConst>()) {
        event += " " + role.as<std::string>();
    }
    return event;
}

/// The stream messages of client's log from index on
std::vector<std::string> stream_events(const RawVisualizer& client, size_t from = 0) {
    std::vector<std::string> events;
    for (size_t i = from; i < client.log.size(); ++i) {
        if (client.log[i].type.starts_with("stream/")) {
            events.push_back(stream_event(client.log[i]));
        }
    }
    return events;
}

// A visualizer with no player follows its group's playback. It starts with the group's audio, ends
// on a pause and starts again on the resume, clears on a flush, ends when the client turns
// unavailable (spec messaging.md "Server behavior when a client becomes unavailable": as the only
// member, its group stops), and starts again as a joiner when the host moves it into a group that
// plays, ending when it moves out
TEST(ServerVisualizer, FollowsTheGroupsPlayback) {
    VisualizerServer s;
    const std::string loudness = R"({"types":["loudness"],"rate_max":50})";
    RawVisualizer client(s.server, "Alone", 100000, loudness);
    const std::shared_ptr<GroupInput> input = s.open({&client});
    ASSERT_TRUE(input);
    // About 8 s staged ahead at most, the write-ahead admits a second past the lead: each step
    // below finds audio playing
    std::atomic<bool> stop{false};
    std::thread host([&] {
        uint64_t position = 0;
        while (!stop.load()) {
            if (feed_tone(*input, position, 960, 0.5) != InputState::OPEN &&
                input->state() != InputState::OPEN) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                continue;
            }
            position += 960;
        }
    });
    ASSERT_TRUE(client.read_until_type("visualizer"));
    size_t mark = client.log.size();
    input->pause();
    ASSERT_TRUE(client.read_until_stream("stream/end", "visualizer"));
    input->resume();
    ASSERT_TRUE(client.read_until_stream("stream/start", "visualizer"));
    ASSERT_TRUE(client.read_until_type("visualizer"));
    input->flush();
    ASSERT_TRUE(client.read_until_stream("stream/clear", "visualizer"));
    ASSERT_TRUE(client.read_until_type("visualizer"));
    EXPECT_EQ(stream_events(client, mark),
              (std::vector<std::string>{"stream/end visualizer", "stream/start visualizer",
                                        "stream/clear visualizer"}));

    mark = client.log.size();
    client.send_state(false, loudness);
    ASSERT_TRUE(client.read_until_stream("stream/end", "visualizer"));
    // The stop latches the input until the host acknowledges it
    stop = true;
    host.join();
    input->stop();
    client.send_state(true, loudness);
    // Back available, it joins nothing on its own (messaging.md "External Source Handling")
    client.client.send_json(sendspin::server::test::client_time_json(1));
    ASSERT_TRUE(client.read_until_type("server/time"));
    EXPECT_EQ(stream_events(client, mark), (std::vector<std::string>{"stream/end visualizer"}));

    // A playing group: the host moves the client in, then out again
    RawVisualizer other(s.server, "Other", 100000, loudness);
    s.recorder.wait_for("connected " + other.id);
    const std::optional<sendspin::server::GroupId> created =
        s.server.create_group("Playing", other.id);
    ASSERT_TRUE(created);
    const sendspin::server::GroupId playing = *created;
    // open_input() takes the group once on_group_changed() reports it
    s.recorder.wait_for("group " + std::to_string(playing.value) + " stopped");
    const std::shared_ptr<GroupInput> second = s.server.open_input(playing, s16_input()).input;
    ASSERT_TRUE(second);
    std::thread host2([&] { feed_tone(*second, 0, 4 * RATE, 0.5); });
    ASSERT_TRUE(other.read_until_type("visualizer"));
    mark = client.log.size();
    ASSERT_TRUE(s.server.move_client(client.id, playing));
    ASSERT_TRUE(client.read_until_type("visualizer"));
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access): set while the server runs
    ASSERT_TRUE(s.server.move_client(client.id, *s.server.default_group()));
    ASSERT_TRUE(client.read_until_stream("stream/end", "visualizer"));
    EXPECT_EQ(stream_events(client, mark),
              (std::vector<std::string>{"stream/start visualizer", "stream/end visualizer"}));
    second->stop();
    host2.join();
}

/// The server's messages to client, from the trace
std::vector<VisualizerTrace::Sent> sent_to(VisualizerServer& s, const std::string& client) {
    const std::lock_guard<std::mutex> lock(s.trace.mutex);
    return s.trace.sent[client];
}

// A visualizer alone in a group on a live input is rebased across a stall in the input's writes as
// players are: the timeline moves past the stall, so the first message timed after the writes
// resume is at least the restored lead, `G + J_in` with `G` at G_default, past them, where without
// the rebase the messages the resumed writes make would be late and skipped and the next ones
// barely ahead. The messages are in order.
TEST(ServerVisualizer, StaysOnTimeAcrossALiveStallWithNoPlayer) {
    VisualizerServer s;
    RawVisualizer client(s.server, "Live", 100000, R"({"types":["loudness"],"rate_max":50})");
    s.recorder.wait_for("connected " + client.id);
    InputOptions options = s16_input();
    options.kind = sendspin::server::InputKind::LIVE;
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access): set while the server runs
    const std::shared_ptr<GroupInput> input =
        s.server.open_input(*s.server.default_group(), options).input;
    ASSERT_TRUE(input);
    std::atomic<int64_t> resumed{0};
    std::thread host([&] {
        // Written in real time once its first 200 ms are in
        feed_tone(*input, 0, RATE / 2, 0.5);
        std::this_thread::sleep_for(std::chrono::milliseconds(600));
        resumed = sendspin::server::now_us();
        feed_tone(*input, RATE / 2, RATE, 0.5);
        input->drain();
    });
    ASSERT_TRUE(client.read_until_stream("stream/end", "visualizer"));
    host.join();
    std::optional<int64_t> first_after;
    int64_t last = 0;
    for (const VisualizerTrace::Sent& sent : sent_to(s, client.id)) {
        if (sent.reset || sent.id != 16) {
            continue;
        }
        EXPECT_GE(sent.ts, last);
        last = sent.ts;
        if (!first_after && sent.ts >= resumed.load()) {
            first_after = sent.ts;
        }
    }
    // So the first message timed after the resume is the resumed writes' first: the writes before
    // the stall ended 600 ms before the resume, and no lead reaches that far
    ASSERT_TRUE(first_after) << "no message after the stall";
    EXPECT_GE(*first_after - resumed.load(),
              sendspin::server::sched::timing_defaults::GROUP_DEFAULT_US);
}

// A visualizer that joins a playing group starts the group lead past its stream/start, as a joining
// player starts its own lead past its trigger. The player here asks a 700 ms required lead, so `G`
// is 710 ms with `M_pump`: the first message is at or past it.
TEST(ServerVisualizer, StartsAJoinerTheGroupLeadPastItsStart) {
    constexpr int64_t G = 710000;
    VisualizerServer s;
    const std::string loudness = R"({"types":["loudness"],"rate_max":50})";
    RawVisualizer both(s.server, "Both", 1000000, loudness, true);
    const std::shared_ptr<GroupInput> input = s.open({&both});
    ASSERT_TRUE(input);
    std::thread host([&] { feed_tone(*input, 0, 3 * RATE, 0.5); });
    ASSERT_TRUE(both.read_until_type("visualizer"));
    RawVisualizer joiner(s.server, "Joiner", 100000, loudness);
    ASSERT_TRUE(joiner.read_until_type("visualizer"));
    input->stop();
    host.join();
    // The trace runs on the dispatch thread, which reports the joiner's disconnect at the stop
    // after every trace call before it
    s.server.stop();
    s.recorder.wait_for("disconnected " + joiner.id + " " +
                        std::to_string(static_cast<int>(DisconnectReason::SERVER_STOPPED)));
    std::optional<int64_t> started;
    std::optional<VisualizerTrace::Sent> first;
    for (const VisualizerTrace::Sent& sent : sent_to(s, joiner.id)) {
        if (sent.start && !started) {
            started = sent.stamp_us;
        } else if (started && sent.id == 16) {
            first = sent;
            break;
        }
    }
    ASSERT_TRUE(started) << "no stream/start traced";
    ASSERT_TRUE(first) << "no message traced after the start";
    // The first cell at or past it, unless that cell had passed when the message was sent
    EXPECT_GE(first->ts - *started, G);
    if (first->stamp_us < *started + G) {
        EXPECT_LT(first->ts - *started, G + CELL_US);
    }
}

// A group's drain completes once no stream is active, a visualizer's included, so a visualizer
// alone in the group gets its stream/end only past its last message's time. Its messages are
// timed after its stream's start, one a cell, missing only where late (expect_on_grid()).
TEST(ServerVisualizer, EndsTheStreamAfterItsLastMessageBeforeTheDrainCompletes) {
    VisualizerServer s;
    RawVisualizer client(s.server, "Drained", 100000, R"({"types":["loudness"],"rate_max":50})");
    const std::shared_ptr<GroupInput> input = s.open({&client});
    ASSERT_TRUE(input);
    std::thread host([&] {
        feed_tone(*input, 0, RATE / 2, 0.5);
        input->drain();
    });
    ASSERT_TRUE(client.read_until_stream("stream/end", "visualizer"));
    host.join();
    s.recorder.wait_for(Recorder::drained(input->id(), DrainOutcome::PLAYED));
    std::vector<VisualizerTrace::Sent> messages;
    std::optional<int64_t> started;
    std::optional<int64_t> ended;
    for (const VisualizerTrace::Sent& sent : sent_to(s, client.id)) {
        if (sent.start && !started) {
            started = sent.stamp_us;
        } else if (sent.reset) {
            ended = sent.stamp_us;
            break;
        } else if (sent.id == 16) {
            messages.push_back(sent);
        }
    }
    ASSERT_TRUE(started && ended);
    // The half second's 25 cells at most
    ASSERT_FALSE(messages.empty());
    EXPECT_LE(messages.size(), 25U);
    EXPECT_GT(messages.front().ts, *started);
    expect_on_grid(messages, CELL_US);
    EXPECT_GE(*ended, messages.back().ts);
}

// spec messaging.md "stream/start": the stream starts on the client/state that the activation
// waits for, also when that state comes after the markers of the activation and of the join were
// taken, and is not the client's first or a turn to available, so only the visualizer role's
// first object since the activation gives the trigger its kind. An unapproved client is held with
// no roles in a group of its own (pairing.md "Unpaired Access"), its client/state's visualizer
// object ignored; its approval activates the visualizer and moves it into the server's group,
// which plays
TEST(ServerVisualizer, StartsOnItsRolesStateThatComesLate) {
    MemoryProvider provider;
    Recorder recorder;
    SendspinServer server;
    ServerConfig config = sendspin::server::test::local_config();
    config.name = "Approving";
    ASSERT_EQ(server.start(config, &provider, &recorder), StartResult::OK);
    const std::string loudness = R"({"types":["loudness"],"rate_max":50})";
    RawVisualizer client(server, "Late", 100000, loudness);
    recorder.wait_for("approval needed " + client.id);
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access): set while the server runs
    const std::shared_ptr<GroupInput> input =
        server.open_input(*server.default_group(), s16_input()).input;
    ASSERT_TRUE(input);
    std::thread host([&] {
        feed_tone(*input, 0, RATE, 0.5);
        input->drain();
    });
    recorder.wait_for("group " + std::to_string(input->group().value) + " playing");
    ASSERT_TRUE(server.approve(client.id, sendspin::server::ApprovalScope::PLAYBACK));
    // The activation and the move are written, and their markers taken
    ASSERT_TRUE(client.read_until_type("group/update"));
    client.client.send_json(sendspin::server::test::client_time_json(1));
    ASSERT_TRUE(client.read_until_type("server/time"));
    EXPECT_TRUE(stream_events(client).empty());
    client.send_state(true, loudness);
    recorder.wait_for(Recorder::drained(input->id(), DrainOutcome::PLAYED));
    host.join();
    client.client.send_json(sendspin::server::test::client_time_json(2));
    ASSERT_TRUE(client.read_until_type("server/time"));
    EXPECT_EQ(stream_events(client),
              (std::vector<std::string>{"stream/start visualizer", "stream/end visualizer"}));
}

// MAX_GROUP_ANALYSES: a group runs four analyses at most; a fifth configuration fails as an
// analysis that failed would, its client reported with PIPELINE_FAILED and given no stream, while
// the four stream
TEST(ServerVisualizer, CapsAGroupsAnalyses) {
    VisualizerServer s;
    std::vector<std::unique_ptr<RawVisualizer>> clients;
    for (uint32_t rate = 46; rate <= 50; ++rate) {
        clients.push_back(std::make_unique<RawVisualizer>(
            s.server, "Rate " + std::to_string(rate), 100000,
            R"({"types":["loudness"],"rate_max":)" + std::to_string(rate) + "}"));
    }
    for (const auto& client : clients) {
        s.recorder.wait_for("connected " + client->id);
    }
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access): set while the server runs
    const std::shared_ptr<GroupInput> input =
        s.server.open_input(*s.server.default_group(), s16_input()).input;
    ASSERT_TRUE(input);
    std::thread host([&] {
        feed_tone(*input, 0, RATE / 2, 0.5);
        input->drain();
    });
    s.recorder.wait_for(Recorder::drained(input->id(), DrainOutcome::PLAYED));
    host.join();
    const std::string failed =
        "error " + std::to_string(static_cast<int>(sendspin::server::ErrorKind::PIPELINE_FAILED)) +
        " ";
    size_t streamed = 0;
    size_t reported = 0;
    for (const auto& client : clients) {
        client->client.send_json(sendspin::server::test::client_time_json(1));
        ASSERT_TRUE(client->read_until_type("server/time"));
        const bool started = !stream_events(*client).empty();
        const bool error = s.recorder.count(failed + client->client.url()) > 0;
        EXPECT_NE(started, error) << client->id;
        streamed += started ? 1 : 0;
        reported += error ? 1 : 0;
    }
    EXPECT_EQ(streamed, 4U);
    EXPECT_EQ(reported, 1U);
}

// MAX_GROUP_ANALYSES: with four configurations streaming, a client that changes to a fifth holds
// it past the cap, so the change fails: its stream ends and it is reported with PIPELINE_FAILED,
// while the other three go on streaming
TEST(ServerVisualizer, EndsTheStreamOfAChangeToAConfigurationPastTheCap) {
    VisualizerServer s;
    std::vector<std::unique_ptr<RawVisualizer>> clients;
    const auto loudness = [](uint32_t rate) {
        return R"({"types":["loudness"],"rate_max":)" + std::to_string(rate) + "}";
    };
    for (uint32_t rate = 47; rate <= 50; ++rate) {
        clients.push_back(std::make_unique<RawVisualizer>(s.server, "Rate " + std::to_string(rate),
                                                          100000, loudness(rate)));
    }
    for (const auto& client : clients) {
        s.recorder.wait_for("connected " + client->id);
    }
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access): set while the server runs
    const std::shared_ptr<GroupInput> input =
        s.server.open_input(*s.server.default_group(), s16_input()).input;
    ASSERT_TRUE(input);
    std::thread host([&] {
        feed_tone(*input, 0, 6 * RATE, 0.5);
        input->drain();
    });
    for (const auto& client : clients) {
        ASSERT_TRUE(client->read_until_type("visualizer"));
    }
    RawVisualizer& changer = *clients.back();
    changer.send_state(true, loudness(46));
    ASSERT_TRUE(changer.read_until_stream("stream/end", "visualizer"));
    s.recorder.wait_for_error(changer.client.url());
    EXPECT_EQ(s.recorder.count(
                  "error " +
                  std::to_string(static_cast<int>(sendspin::server::ErrorKind::PIPELINE_FAILED)) +
                  " " + changer.client.url()),
              1U);
    // The others still stream, seconds before the drain ends them
    for (size_t i = 0; i + 1 < clients.size(); ++i) {
        const size_t mark = clients[i]->log.size();
        ASSERT_TRUE(clients[i]->read_until([](const Entry& entry) {
            return entry.type == "visualizer" || entry.type.starts_with("stream/");
        }));
        EXPECT_EQ(clients[i]->log.back().type, "visualizer") << i;
        EXPECT_TRUE(stream_events(*clients[i], mark).empty()) << i;
    }
    input->stop();
    host.join();
}

// ==============================================================================
// Order, accounting and configuration
// ==============================================================================

// The player and visualizer cursors share the connection by earliest deadline, a chunk's being its
// first frame's ts (no output delay here) and a message's its ts. With room in both ledgers, a
// visualization message never follows a chunk whose deadline is later than its own, while the
// player buffers far ahead, so the messages fill the gaps between chunks rather than wait behind
// them
TEST(ServerVisualizer, InterleavesMessagesWithChunksByDeadline) {
    VisualizerServer s;
    RawVisualizer client(s.server, "Both", 1000000, request(R"("loudness","spectrum")", 50), true);
    const std::shared_ptr<GroupInput> input = s.open({&client});
    ASSERT_TRUE(input);
    std::thread host([&] {
        feed_tone(*input, 0, 2 * RATE, 0.5);
        input->drain();
    });
    ASSERT_TRUE(client.read_until_stream("stream/end", "visualizer"));
    host.join();
    int64_t latest_chunk = std::numeric_limits<int64_t>::min();
    size_t interleaved = 0;
    bool chunk_seen = false;
    for (const Entry& entry : client.log) {
        if (entry.type == "chunk") {
            latest_chunk = std::max(latest_chunk, entry.ts);
            chunk_seen = true;
        } else if (entry.type == "visualizer") {
            EXPECT_GE(entry.ts, latest_chunk) << "a message after a later chunk";
            interleaved += chunk_seen ? 1 : 0;
        }
    }
    // The player's lead puts chunks seconds ahead of the messages, which still go between them;
    // how many is when the pump ran, which skips a message already late
    EXPECT_GT(interleaved, 0U);
}

// spec roles/visualizer/v1.md "client/hello visualizer@v1 support object": no message is sent
// when its size plus that of the messages sent with timestamps still ahead of the server's clock
// would pass buffer_capacity; a stream/clear or stream/end resets the count. Checked against the
// stamp each message was sent with, over a stream whose capacity binds: about three messages. That
// it binds shows in the last message: with every message produced at once, only the capacity holds
// it back past the first one's time
TEST(ServerVisualizer, KeepsMessagesWithinTheBufferCapacity) {
    VisualizerServer s;
    constexpr uint32_t CAPACITY = 40;
    RawVisualizer client(s.server, "Small", CAPACITY, R"({"types":["loudness"],"rate_max":50})");
    const std::shared_ptr<GroupInput> input = s.open({&client});
    ASSERT_TRUE(input);
    std::thread host([&] {
        feed_tone(*input, 0, RATE, 0.5);
        input->drain();
    });
    ASSERT_TRUE(client.read_until_stream("stream/end", "visualizer"));
    host.join();
    std::vector<VisualizerTrace::Sent> sent;
    {
        const std::lock_guard<std::mutex> lock(s.trace.mutex);
        sent = s.trace.sent[client.id];
    }
    std::optional<int64_t> first_ts;
    int64_t last_stamp = 0;
    size_t messages = 0;
    for (size_t i = 0; i < sent.size(); ++i) {
        if (sent[i].reset || sent[i].start || sent[i].id == AUDIO_CHUNK) {
            continue;
        }
        ++messages;
        first_ts = first_ts.value_or(sent[i].ts);
        last_stamp = sent[i].stamp_us;
        int64_t counted = 0;
        for (size_t j = i; j-- > 0 && !sent[j].reset;) {
            if (sent[j].id != AUDIO_CHUNK && sent[j].ts > sent[i].stamp_us) {
                counted += static_cast<int64_t>(sent[j].bytes);
            }
        }
        EXPECT_LE(counted + static_cast<int64_t>(sent[i].bytes), CAPACITY) << i;
    }
    // A fourth message is past the capacity; how many of the 50 are sent is when the pump ran
    EXPECT_GE(messages, 4U);
    ASSERT_TRUE(first_ts);
    EXPECT_GE(last_stamp, *first_ts) << "the capacity never bound";
}

// spec roles/visualizer/v1.md "stream/start visualizer object": types the server cannot stream are
// omitted from the echo (`beat`, and `spectrum` past the bin cap), and a client asking for none of
// the rest still gets its stream, with no types
TEST(ServerVisualizer, OmitsTheTypesItCannotStream) {
    VisualizerServer s;
    RawVisualizer capped(
        s.server, "Capped", 100000,
        R"({"types":["beat","spectrum","loudness"],"rate_max":200,"spectrum":{"n_disp_bins":257,)"
        R"("scale":"mel","f_min":20,"f_max":20000}})");
    RawVisualizer beats(s.server, "Beats", 100000, R"({"types":["beat"],"rate_max":30})");
    const std::shared_ptr<GroupInput> input = s.open({&capped, &beats});
    ASSERT_TRUE(input);
    std::thread host([&] {
        feed_tone(*input, 0, RATE / 2, 0.5);
        input->drain();
    });
    for (RawVisualizer* client : {&capped, &beats}) {
        ASSERT_TRUE(client->read_until_stream("stream/end", "visualizer"));
        const std::vector<const Entry*> starts = client->of_type("stream/start");
        ASSERT_EQ(starts.size(), 1U);
        JsonDocument doc;
        ASSERT_EQ(deserializeJson(doc, starts[0]->json), DeserializationError::Ok);
        std::string visualizer;
        serializeJson(doc["payload"]["visualizer"], visualizer);
        EXPECT_EQ(visualizer, client == &capped ? R"({"types":["loudness"],"rate_max":50})"
                                                : R"({"types":[],"rate_max":30})");
        for (const Entry* message : first_stream(*client)) {
            EXPECT_EQ(message->id, 16) << client->id;
        }
    }
    EXPECT_FALSE(first_stream(capped).empty());
    EXPECT_TRUE(first_stream(beats).empty());
    host.join();
}

// spec roles/visualizer/v1.md "client/state visualizer object": a visualizer object listing
// 'spectrum' without a spectrum object is a protocol error, and the server closes the connection
// silently (connection.md "Failure Handling"); the same object from a client without the role is
// ignored (messaging.md "server/activate")
TEST(ServerVisualizer, ClosesOnASpectrumRequestWithoutItsConfiguration) {
    VisualizerServer s;
    const std::string missing = R"({"types":["spectrum"],"rate_max":30})";
    RawClient plain;
    EXPECT_TRUE(s.server.connect_url(plain.url()));
    plain.activate();
    plain.send_json(state_json(true, missing, false));
    // Control: still answered
    plain.send_json(sendspin::server::test::client_time_json(1));
    EXPECT_FALSE(plain.receive_until("server/time").empty());

    RawClient visualizer;
    EXPECT_TRUE(s.server.connect_url(visualizer.url()));
    visualizer.activate(hello_json("Broken", 1000, false));
    visualizer.send_json(state_json(true, missing, false));
    visualizer.expect_silent_close();
}

// spec roles/visualizer/v1.md "client/state visualizer object": a changed configuration while the
// stream is active is announced with a stream/start for the visualizer alone, in place, with no
// stream/end; its messages then follow it, of the new types, never earlier than the last before it
TEST(ServerVisualizer, ReconfiguresAnActiveStreamInPlace) {
    VisualizerServer s;
    RawVisualizer client(s.server, "Changes", 100000, R"({"types":["loudness"],"rate_max":50})");
    const std::shared_ptr<GroupInput> input = s.open({&client});
    ASSERT_TRUE(input);
    std::thread host([&] {
        feed_tone(*input, 0, 3 * RATE, 0.5);
        input->drain();
    });
    ASSERT_TRUE(client.read_until([](const Entry& entry) { return entry.id == 16; }));
    const size_t before = client.log.size();
    client.send_state(true, R"({"types":["f_peak"],"rate_max":10})");
    ASSERT_TRUE(client.read_until([](const Entry& entry) { return entry.id == 18; }));
    ASSERT_TRUE(client.read_until([](const Entry& entry) { return entry.id == 18; }));
    EXPECT_EQ(stream_events(client, before), (std::vector<std::string>{"stream/start visualizer"}));
    int64_t last_old = 0;
    std::vector<int64_t> new_ts;
    bool announced = false;
    for (size_t i = 0; i < client.log.size(); ++i) {
        const Entry& entry = client.log[i];
        if (i >= before && entry.type == "stream/start") {
            announced = true;
            JsonDocument doc;
            deserializeJson(doc, entry.json);
            EXPECT_EQ(doc["payload"]["visualizer"]["rate_max"], 10);
        } else if (entry.type == "visualizer") {
            EXPECT_EQ(entry.id, announced ? 18 : 16);
            if (announced) {
                new_ts.push_back(entry.ts);
            } else {
                last_old = entry.ts;
            }
        }
    }
    ASSERT_EQ(new_ts.size(), 2U);
    EXPECT_GT(new_ts[0], last_old);
    // 10 per second
    EXPECT_EQ(new_ts[1] - new_ts[0], 100000);
    input->stop();
    host.join();
}

// ==============================================================================
// End to end with sendspin-cpp
// ==============================================================================

/// What a sendspin-cpp visualizer reported
class VisualizerLog : public sendspin::VisualizerRoleListener {
public:
    void on_loudness(int64_t client_timestamp, uint16_t loudness) override {
        const std::lock_guard<std::mutex> lock(this->mutex);
        this->loudness.emplace_back(client_timestamp, loudness);
    }

    void on_f_peak(int64_t /*client_timestamp*/, uint16_t frequency_hz,
                   uint16_t /*amplitude*/) override {
        const std::lock_guard<std::mutex> lock(this->mutex);
        this->f_peaks.push_back(frequency_hz);
    }

    void on_spectrum(int64_t /*client_timestamp*/, const std::vector<uint16_t>& bins) override {
        const std::lock_guard<std::mutex> lock(this->mutex);
        this->spectrum_sizes.push_back(bins.size());
    }

    void on_visualizer_stream_start(const sendspin::ServerVisualizerStreamObject& stream) override {
        const std::lock_guard<std::mutex> lock(this->mutex);
        this->starts.push_back(stream);
    }

    void on_visualizer_stream_end() override {
        const std::lock_guard<std::mutex> lock(this->mutex);
        ++this->ends;
    }

    std::mutex mutex;
    std::vector<std::pair<int64_t, uint16_t>> loudness;
    std::vector<uint16_t> f_peaks;
    std::vector<size_t> spectrum_sizes;
    std::vector<sendspin::ServerVisualizerStreamObject> starts;
    size_t ends{0};
};

// A sendspin-cpp client that plays and visualizes takes the visualizer stream the server echoes and
// delivers its frames in order, of the echoed shape; the server sends its messages in order; on the
// server's timeline the loudness is quiet before the tone's first sample and loud from a window
// past it
TEST(ServerVisualizerPeer, DeliversFramesInTimeWithTheAudio) {
    VisualizerServer s;
    const uint16_t port = sendspin::server::test::free_port();
    sendspin::SendspinClient client(sendspin::server::test::peer_config("Visualizer peer", port));
    sendspin::PlayerRoleConfig player_config;
    // One format, assigned by count: GCC 13 -Warray-bounds misreads an initializer-list assignment
    player_config.audio_formats.assign(
        1, sendspin::AudioSupportedFormatObject{sendspin::SendspinCodecFormat::PCM, 2, RATE, 16});
    sendspin::PlayerRole& player = client.add_player(player_config);
    sendspin::server::test::Sink sink(player, 4, RATE);
    player.set_listener(&sink);
    sendspin::VisualizerRoleConfig visualizer_config;
    visualizer_config.support.buffer_capacity = 16384;
    visualizer_config.stream.types = {
        sendspin::VisualizerDataType::LOUDNESS, sendspin::VisualizerDataType::F_PEAK,
        sendspin::VisualizerDataType::SPECTRUM, sendspin::VisualizerDataType::BEAT};
    visualizer_config.stream.rate_max = 30;
    visualizer_config.stream.spectrum =
        sendspin::VisualizerSpectrumConfig{.n_disp_bins = 16,
                                           .scale = sendspin::VisualizerSpectrumScale::MEL,
                                           .f_min = 40,
                                           .f_max = 16000};
    VisualizerLog log;
    client.add_visualizer(visualizer_config).set_listener(&log);
    sendspin::server::test::AlwaysReady network;
    client.set_network_provider(&network);
    client.set_unpaired_access_enabled(true);
    ASSERT_TRUE(client.start());
    const std::string url = sendspin::server::test::peer_url(port);
    ASSERT_TRUE(s.server.connect_url(url));
    std::string id;
    run_until(client, [&] {
        for (const sendspin::server::ClientInfo& info : s.server.clients()) {
            if (info.url == url && s.recorder.contains("connected " + info.client_id)) {
                id = info.client_id;
                EXPECT_EQ(info.roles, (std::vector<std::string>{"player@v1", "visualizer@v1"}));
                return true;
            }
        }
        return false;
    });
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access): set while the server runs
    const std::shared_ptr<GroupInput> input =
        s.server.open_input(*s.server.default_group(), s16_input()).input;
    ASSERT_TRUE(input);
    constexpr uint64_t SILENT = RATE / 2;
    std::thread host([&] {
        feed_tone(*input, 0, 2 * RATE, 0.5, SILENT);
        input->drain();
    });
    run_until(client, [&] {
        const std::lock_guard<std::mutex> lock(log.mutex);
        return log.ends > 0;
    });
    host.join();
    client.stop();

    const std::lock_guard<std::mutex> lock(log.mutex);
    ASSERT_EQ(log.starts.size(), 1U);
    const sendspin::ServerVisualizerStreamObject& start = log.starts[0];
    EXPECT_EQ(start.types,
              (std::vector<sendspin::VisualizerDataType>{sendspin::VisualizerDataType::LOUDNESS,
                                                         sendspin::VisualizerDataType::F_PEAK,
                                                         sendspin::VisualizerDataType::SPECTRUM}));
    EXPECT_EQ(start.rate_max, 30);
    ASSERT_TRUE(start.spectrum);
    EXPECT_EQ(start.spectrum->n_disp_bins, 16);

    std::vector<VisualizerTrace::Sent> sent;
    {
        const std::lock_guard<std::mutex> trace_lock(s.trace.mutex);
        sent = s.trace.sent[id];
    }
    int64_t last_ts = 0;
    for (const VisualizerTrace::Sent& message : sent) {
        if (message.reset || message.start || message.id == AUDIO_CHUNK) {
            continue;
        }
        EXPECT_GE(message.ts, last_ts);
        last_ts = message.ts;
    }
    ASSERT_FALSE(log.loudness.empty());
    for (size_t i = 1; i < log.loudness.size(); ++i) {
        EXPECT_GT(log.loudness[i].first, log.loudness[i - 1].first);
    }
    EXPECT_FALSE(log.spectrum_sizes.empty());
    for (const size_t size : log.spectrum_sizes) {
        EXPECT_EQ(size, 16U);
    }
    ASSERT_FALSE(log.f_peaks.empty());
    EXPECT_EQ(log.f_peaks.back(), static_cast<uint16_t>(std::lround(TONE_HZ)));

    // The tone's first sample, from the chunk holding it
    std::optional<int64_t> onset;
    for (const VisualizerTrace::Sent& chunk : sent) {
        if (chunk.id != AUDIO_CHUNK || onset) {
            continue;
        }
        for (size_t frame = 0; frame + 4 <= chunk.data.size(); frame += 4) {
            if (chunk.data[frame] != 0 || chunk.data[frame + 1] != 0) {
                onset = chunk.ts + static_cast<int64_t>((frame / 4) * 1000000 / RATE);
                break;
            }
        }
    }
    ASSERT_TRUE(onset);
    // The window ends at its instant, so it holds none of the tone before its first sample and
    // nothing but the tone a window past it; the messages in between may be either
    constexpr int64_t WINDOW_US = 2048 * 1000000 / RATE;
    size_t loud_after = 0;
    for (const VisualizerTrace::Sent& message : sent) {
        if (message.id != 16) {
            continue;
        }
        const bool loud = u16_at(message.data, 0) > 30000;
        if (message.ts < *onset) {
            EXPECT_FALSE(loud) << message.ts - *onset;
        } else if (message.ts >= *onset + WINDOW_US) {
            EXPECT_TRUE(loud) << message.ts - *onset;
            ++loud_after;
        }
    }
    EXPECT_GT(loud_after, 0U);
}

}  // namespace
