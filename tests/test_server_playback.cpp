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

/// @file test_server_playback.cpp
/// @brief Playback end to end through the public API: a host writes PCM into a GroupInput and raw
/// test clients, approved players, log every message the server sends them. Every timing check uses
/// the timeline and the stamps the messages carry, never elapsed time, and holds however late a
/// send is.

#include "flac_reader.h"
#include "raw_client.h"
#include "test_recorder.h"
#include <ArduinoJson.h>
#include <gtest/gtest.h>

#include <sendspin/server/server.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using sendspin::server::DrainId;
using sendspin::server::DrainOutcome;
using sendspin::server::GroupId;
using sendspin::server::GroupInfo;
using sendspin::server::GroupInput;
using sendspin::server::InputFormat;
using sendspin::server::InputId;
using sendspin::server::InputOptions;
using sendspin::server::InputState;
using sendspin::server::OpenedInput;
using sendspin::server::OpenInputResult;
using sendspin::server::SampleFormat;
using sendspin::server::SendspinServer;
using sendspin::server::ServerConfig;
using sendspin::server::StartResult;
using sendspin::server::WriteResult;
using sendspin::server::test::Bytes;
using sendspin::server::test::client_state_json;
using sendspin::server::test::client_time_json;
using sendspin::server::test::DecodedFlacFrame;
using sendspin::server::test::FlacReader;
using sendspin::server::test::MemoryProvider;
using sendspin::server::test::player_hello_json;
using sendspin::server::test::player_state_json;
using sendspin::server::test::RawClient;
using sendspin::server::test::Recorder;

constexpr uint32_t RATE = 48000;
constexpr uint64_t FRAMES_PER_CELL = RATE / 50;
constexpr int64_t CELL_US = 20000;
constexpr int64_t J_MIN_US = 100000;
constexpr size_t HEADER = 13;
constexpr uint8_t AUDIO_CHUNK = 4;
constexpr const char* PCM16 =
    R"([{"codec":"pcm","sample_rate":48000,"channels":2,"bit_depth":16}])";

// ==============================================================================
// The test signal: every frame carries its position and an epoch tag, so content shows which
// audio a chunk holds
// ==============================================================================

struct Sample {
    uint64_t position{0};
    uint8_t tag{0};

    bool operator==(const Sample&) const = default;
};

/// S16 stereo: left the position's low 16 bits, right its next 8 bits under the tag
void write_frame(uint8_t* out, uint64_t position, uint8_t tag) {
    const auto low = static_cast<uint16_t>(position);
    const auto high = static_cast<uint16_t>((uint32_t{tag} << 8) | ((position >> 16) & 0xFF));
    out[0] = static_cast<uint8_t>(low);
    out[1] = static_cast<uint8_t>(low >> 8);
    out[2] = static_cast<uint8_t>(high);
    out[3] = static_cast<uint8_t>(high >> 8);
}

Sample read_frame(const uint8_t* in) {
    const uint32_t low = in[0] | (uint32_t{in[1]} << 8);
    const uint32_t high = in[2] | (uint32_t{in[3]} << 8);
    return {low | (uint64_t{high & 0xFF} << 16), static_cast<uint8_t>(high >> 8)};
}

InputOptions s16_input() {
    InputOptions options;
    options.format = InputFormat{RATE, 2, SampleFormat::S16};
    return options;
}

/// Writes count frames of the signal from position on, waiting for room; stops early when the
/// input refuses for any reason but FULL
/// @return the last state write() reported
InputState feed(GroupInput& input, uint64_t position, uint64_t count, uint8_t tag) {
    constexpr uint64_t BLOCK = 1024;
    Bytes block(BLOCK * 4);
    uint64_t written = 0;
    InputState state = InputState::OPEN;
    while (written < count) {
        const uint64_t n = std::min(BLOCK, count - written);
        for (uint64_t i = 0; i < n; ++i) {
            write_frame(&block[i * 4], position + written + i, tag);
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

// ==============================================================================
// A raw player and its log
// ==============================================================================

struct Chunk {
    int64_t ts{0};
    uint32_t send_ahead{0};
    Bytes payload;
};

/// One message the player received, in order
struct Entry {
    /// The JSON type, or "chunk"
    std::string type;
    std::string json;
    Chunk chunk;

    int64_t stamp() const {
        JsonDocument doc;
        deserializeJson(doc, this->json);
        return doc["payload"]["server_transmitted"].as<int64_t>();
    }
};

class RawPlayer {
public:
    /// Dials it and completes its activation; the player then reports its state
    /// @param before_dial called with its client_id before the server dials it
    RawPlayer(SendspinServer& server, const std::string& name, uint32_t capacity,
              const std::string& state, const std::string& formats = PCM16, bool available = true,
              const std::function<void(const std::string&)>& before_dial = {}) {
        this->id = this->client.noise.identity().peer_id();
        if (before_dial) {
            before_dial(this->id);
        }
        EXPECT_TRUE(server.connect_url(this->client.url()));
        this->client.activate(player_hello_json(name, formats, capacity));
        this->client.send_json(client_state_json(available, state));
    }

    /// Reads and logs messages until done holds for the last one
    /// @return false when the connection closed first
    bool read_until(const std::function<bool(const Entry&)>& done) {
        for (;;) {
            const std::optional<Bytes> plaintext = this->client.receive_plaintext();
            if (!plaintext) {
                return false;
            }
            Entry entry;
            if (!plaintext->empty() && plaintext->front() == AUDIO_CHUNK) {
                EXPECT_GE(plaintext->size(), HEADER);
                entry.type = "chunk";
                for (size_t i = 1; i < 9; ++i) {
                    entry.chunk.ts = (entry.chunk.ts << 8) | (*plaintext)[i];
                }
                for (size_t i = 9; i < HEADER; ++i) {
                    entry.chunk.send_ahead = (entry.chunk.send_ahead << 8) | (*plaintext)[i];
                }
                entry.chunk.payload.assign(plaintext->begin() + HEADER, plaintext->end());
            } else if (!plaintext->empty()) {
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

    std::vector<const Entry*> chunks() const {
        std::vector<const Entry*> out;
        for (const Entry& entry : this->log) {
            if (entry.type == "chunk") {
                out.push_back(&entry);
            }
        }
        return out;
    }

    RawClient client;
    std::string id;
    std::vector<Entry> log;
};

/// A player's timing: the group lead follows its required lead, far above the minimum buffer a
/// group trigger owes it, so a trigger written late still finds the epoch's first frame
constexpr uint32_t MIN_BUFFER_MS = 150;
constexpr uint32_t REQUIRED_LEAD_MS = 700;

std::string lead_state(uint32_t output_delay_ms = 0) {
    return player_state_json(MIN_BUFFER_MS, REQUIRED_LEAD_MS, output_delay_ms);
}

/// Chunks only within an active stream, a stream/clear or stream/end only for an active one, a
/// start only for an inactive one, and after a clear no chunk of the audio before it (the test
/// raises the tag at every flush)
bool ordered(const std::vector<Entry>& log) {
    bool active = false;
    uint8_t last_tag = 0;
    uint8_t least_tag = 0;
    for (const Entry& entry : log) {
        if (entry.type == "stream/start") {
            if (active) {
                ADD_FAILURE() << "a start for an active stream";
                return false;
            }
            active = true;
        } else if (entry.type == "stream/clear" || entry.type == "stream/end") {
            if (!active) {
                ADD_FAILURE() << entry.type << " with no active stream";
                return false;
            }
            active = entry.type == "stream/clear";
            if (active) {
                least_tag = static_cast<uint8_t>(last_tag + 1);
            }
        } else if (entry.type == "chunk") {
            const uint8_t tag = read_frame(entry.chunk.payload.data()).tag;
            if (!active || tag < least_tag) {
                ADD_FAILURE() << (active ? "audio from before the clear"
                                         : "a chunk with no stream");
                return false;
            }
            last_tag = tag;
        }
    }
    return true;
}

/// A started server that approves every client
class PlaybackServer {
public:
    PlaybackServer() {
        ServerConfig config = sendspin::server::test::local_config();
        config.name = "Playback";
        config.approve_all_clients = true;
        EXPECT_EQ(this->server.start(config, &this->provider, &this->recorder), StartResult::OK);
    }

    std::shared_ptr<GroupInput> open() {
        const std::vector<sendspin::server::GroupInfo> groups = this->server.groups();
        EXPECT_EQ(groups.size(), 1U);
        return this->server.open_input(groups.at(0).id, s16_input()).input;
    }

    /// open(), once each player's own client/state is in and the group lead takes in
    /// REQUIRED_LEAD_MS: a first write before a player's client/state anchors the epoch without its
    /// lead, and its start is skipped. A client is connected at its first client/state. Waits with
    /// no timeout, so a regression hangs into the suite watchdog.
    std::shared_ptr<GroupInput> open_led(std::initializer_list<const RawPlayer*> players) {
        for (const RawPlayer* player : players) {
            this->recorder.wait_for("connected " + player->id);
        }
        std::shared_ptr<GroupInput> input = this->open();
        while (input && input->latency_us() < int64_t{REQUIRED_LEAD_MS} * 1000) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return input;
    }

    MemoryProvider provider;
    Recorder recorder;
    SendspinServer server;
};

/// Duration of a PCM16 stereo payload at RATE
int64_t duration_us(const Bytes& payload) {
    const auto frames = static_cast<int64_t>(payload.size() / 4);
    return (frames * 1000000 + RATE - 1) / RATE;
}

/// The samples skipped between the chunk before, which ended at next_ts, and chunk: a frame already
/// late when the pump ran is skipped with its audio, as on a loaded machine, so a gap is allowed
/// only where its last frame had passed by the chunk's stamp (`ts - send_ahead`). The stream's end
/// may be skipped the same way with no later chunk to show it, so no test here requires it sent;
/// that a stream runs through its last frame is checked with the clock passed in
/// (PlayerStreamSwitch).
/// @param output_delay_us the player's output delay, which makes a frame late that much sooner
uint64_t skipped_late(const Chunk& chunk, int64_t next_ts, int64_t output_delay_us = 0) {
    if (chunk.ts == next_ts) {
        return 0;
    }
    EXPECT_GT(chunk.ts, next_ts) << "the timeline ran back";
    EXPECT_LE(chunk.ts - CELL_US - output_delay_us, chunk.ts - int64_t{chunk.send_ahead})
        << "audio skipped before its time";
    return static_cast<uint64_t>((chunk.ts - next_ts) * RATE / 1000000);
}

// ==============================================================================
// Startup lead, the wire format, the ledger and the end of a drain
// ==============================================================================

// spec player/v1.md "Audio Chunks", "Player Buffer Accounting", "Server Audio Send Constraints"
TEST(ServerPlayback, StreamsADrainedClipWithinTheLedgerFromAStartupLead) {
    PlaybackServer s;
    // About half a second of 16-bit stereo: the ledger binds, since the host is admitted a second
    // ahead, and a chunk sent as an earlier one completes is still hundreds of milliseconds from
    // being late, a margin a loaded machine keeps
    constexpr uint32_t CAPACITY = 100000;
    RawPlayer player(s.server, "Raw player", CAPACITY, lead_state());
    const std::shared_ptr<GroupInput> input = s.open_led({&player});
    ASSERT_TRUE(input);

    // Two seconds of audio, then a drain, from the host's own thread
    constexpr uint64_t TOTAL = 2 * RATE + 300;
    std::thread host([&] {
        EXPECT_EQ(feed(*input, 0, TOTAL, 1), InputState::OPEN);
        input->drain();
    });
    ASSERT_TRUE(player.read_until_type("stream/end"));
    host.join();
    // spec player/v1.md "Player Buffer Accounting": the end follows the last chunk's completion,
    // which a time request sent now must not precede
    player.client.send_json(client_time_json(1));
    ASSERT_TRUE(player.read_until_type("server/time"));
    const std::string drained = Recorder::drained(input->id(), DrainOutcome::PLAYED);
    s.recorder.wait_for(drained);
    // The run has ended on the server's side when the host hears the drain played
    const std::vector<Recorder::Entry> events = s.recorder.entries();
    const std::string group = "group " + std::to_string(input->group().value);
    const auto playing = std::find_if(
        events.begin(), events.end(),
        [&](const Recorder::Entry& entry) { return entry.what == group + " playing"; });
    const auto stopped = std::find_if(playing, events.end(), [&](const Recorder::Entry& entry) {
        return entry.what == group + " stopped";
    });
    const auto played =
        std::find_if(events.begin(), events.end(),
                     [&](const Recorder::Entry& entry) { return entry.what == drained; });
    EXPECT_LT(stopped, played);

    const std::vector<const Entry*> chunks = player.chunks();
    ASSERT_FALSE(chunks.empty());
    const auto start = std::find_if(player.log.begin(), player.log.end(),
                                    [](const Entry& e) { return e.type == "stream/start"; });
    ASSERT_NE(start, player.log.end());
    JsonDocument doc;
    ASSERT_EQ(deserializeJson(doc, start->json), DeserializationError::Ok);
    EXPECT_EQ(doc["payload"]["player"]["codec"], "pcm");
    EXPECT_EQ(doc["payload"]["player"]["sample_rate"], RATE);
    EXPECT_EQ(doc["payload"]["player"]["channels"], 2);
    EXPECT_EQ(doc["payload"]["player"]["bit_depth"], 16);
    EXPECT_TRUE(doc["payload"]["player"]["codec_header"].isNull());
    const int64_t start_stamp = start->stamp();

    // The first chunk is at least max(min_buffer + output_delay, J_min) after the trigger
    EXPECT_GE(chunks.front()->chunk.ts - start_stamp,
              std::max<int64_t>(MIN_BUFFER_MS * 1000, J_MIN_US));
    EXPECT_TRUE(ordered(player.log));

    uint64_t expected_position = 0;
    int64_t next_ts = chunks.front()->chunk.ts;
    int64_t most_counted = 0;
    for (size_t i = 0; i < chunks.size(); ++i) {
        SCOPED_TRACE(i);
        const Chunk& chunk = chunks[i]->chunk;
        // Whole frames, at most 150 ms, consecutive on the timeline
        ASSERT_EQ(chunk.payload.size() % 4, 0U);
        EXPECT_LE(duration_us(chunk.payload), 150000);
        expected_position += skipped_late(chunk, next_ts);
        next_ts = chunk.ts + duration_us(chunk.payload);
        // The content is the host's audio, bit for bit and in order
        for (size_t f = 0; f < chunk.payload.size() / 4; ++f) {
            ASSERT_EQ(read_frame(&chunk.payload[f * 4]), (Sample{expected_position, 1}));
            ++expected_position;
        }
        // A lead, never the field's saturated maximum; how far ahead is when the pump ran
        EXPECT_LT(chunk.send_ahead, 0xFFFFFFFFU);
        // The capacity replay: at the chunk's transmit time, it and the earlier chunks still
        // counting fit the capacity (output delay 0)
        const int64_t tau = chunk.ts - chunk.send_ahead;
        EXPECT_GE(tau, start_stamp);
        int64_t counted = HEADER + static_cast<int64_t>(chunk.payload.size());
        for (size_t j = 0; j < i; ++j) {
            const Chunk& earlier = chunks[j]->chunk;
            if (earlier.ts + duration_us(earlier.payload) > tau) {
                counted += HEADER + static_cast<int64_t>(earlier.payload.size());
            }
        }
        EXPECT_LE(counted, CAPACITY);
        most_counted = std::max(most_counted, counted);
    }
    // The ledger, not the host, held the stream back
    EXPECT_GT(most_counted, CAPACITY / 2);

    // The end came after the stream's last sample played, and nothing followed it but the reply
    JsonDocument time;
    ASSERT_EQ(deserializeJson(time, player.log.back().json), DeserializationError::Ok);
    EXPECT_GE(time["payload"]["server_received"].as<int64_t>(), next_ts);
    const auto end = std::find_if(player.log.begin(), player.log.end(),
                                  [](const Entry& e) { return e.type == "stream/end"; });
    EXPECT_TRUE(
        std::none_of(end, player.log.end(), [](const Entry& e) { return e.type == "chunk"; }));
}

// A buffered host that stalls past the group lead loses no audio: the timeline shifts once at the
// frame after the stall, restoring the lead, so the audio after it is not skipped as late
TEST(ServerPlayback, ShiftsTheTimelineAfterAStallInsteadOfDroppingAudio) {
    PlaybackServer s;
    RawPlayer player(s.server, "Raw player", 200000, lead_state());
    const std::shared_ptr<GroupInput> input = s.open_led({&player});
    ASSERT_TRUE(input);
    constexpr uint64_t HALF = RATE / 2;
    std::thread host([&] {
        feed(*input, 0, HALF, 1);
        // Longer than the group lead, so the next frame would be late
        std::this_thread::sleep_for(std::chrono::milliseconds(1500));
        feed(*input, HALF, HALF, 1);
        input->drain();
    });
    ASSERT_TRUE(player.read_until_type("stream/end"));
    host.join();
    s.recorder.wait_for(Recorder::drained(input->id(), DrainOutcome::PLAYED));
    EXPECT_TRUE(ordered(player.log));

    const std::vector<const Entry*> chunks = player.chunks();
    ASSERT_FALSE(chunks.empty());
    uint64_t expected_position = 0;
    int64_t next_ts = chunks.front()->chunk.ts;
    size_t jumps = 0;
    for (const Entry* entry : chunks) {
        const Chunk& chunk = entry->chunk;
        if (chunk.ts != next_ts && read_frame(chunk.payload.data()).position != expected_position) {
            expected_position += skipped_late(chunk, next_ts);
        } else if (chunk.ts != next_ts) {
            // The one shift, forward, at the frame after the stall
            EXPECT_GT(chunk.ts, next_ts);
            EXPECT_EQ(read_frame(chunk.payload.data()).position % FRAMES_PER_CELL, 0U);
            ++jumps;
        }
        next_ts = chunk.ts + duration_us(chunk.payload);
        for (size_t f = 0; f < chunk.payload.size() / 4; ++f) {
            ASSERT_EQ(read_frame(&chunk.payload[f * 4]), (Sample{expected_position, 1}));
            ++expected_position;
        }
    }
    EXPECT_EQ(jumps, 1U);
}

// When the group's only player leaves mid-run, no frame is produced and none is next; the host
// keeps writing past several write-aheads and its drain plays
TEST(ServerPlayback, KeepsTakingInputAfterTheLastPlayerLeaves) {
    PlaybackServer s;
    std::optional<RawPlayer> player(std::in_place, s.server, "Raw player", 200000, lead_state());
    const std::shared_ptr<GroupInput> input = s.open_led({&*player});
    ASSERT_TRUE(input);
    constexpr uint64_t FIRST = RATE / 2;
    feed(*input, 0, FIRST, 1);
    ASSERT_TRUE(player->read_until_type("chunk"));
    player->client.send_json(sendspin::server::test::client_goodbye_json("shutdown"));
    EXPECT_FALSE(player->read_until([](const Entry&) { return false; }));
    player.reset();
    // Three seconds more: each write-ahead is a second, so admission must keep moving
    EXPECT_EQ(feed(*input, FIRST, 3 * RATE, 1), InputState::OPEN);
    input->drain();
    s.recorder.wait_for(Recorder::drained(input->id(), DrainOutcome::PLAYED));
}

// ==============================================================================
// Timing updates (spec player/v1.md "Timing parameters")
// ==============================================================================

/// Sends a client/state, then a time request, and reads until the reply, which the server writes
/// only after taking the state in
/// @return the indices in the log of the first message read after sending and of the reply
std::pair<size_t, size_t> update_state(RawPlayer& player, const std::string& state) {
    const size_t sent = player.log.size();
    player.client.send_json(client_state_json(true, state));
    player.client.send_json(client_time_json(1));
    EXPECT_TRUE(player.read_until_type("server/time"));
    return {sent, player.log.size() - 1};
}

// spec player/v1.md "Player Buffer Accounting": an output delay change mid-stream recounts the
// chunks sent: a higher delay completes them sooner, so the stream runs further ahead, past what
// the capacity would hold with no delay, and a lower one counts earlier chunks again, holding the
// stream back. Through both the samples are sent in order, on one timeline, within the capacity
// under the delay in force. The stream lasts four seconds, played in real time, so that each delay
// holds the stream for a while.
TEST(ServerPlayback, RecountsTheLedgerWhenTheOutputDelayChanges) {
    PlaybackServer s;
    // About half a second, so the ledger holds the stream back (as in the capacity replay above)
    constexpr uint32_t CAPACITY = 100000;
    constexpr int64_t DELAY_US = 200000;
    RawPlayer player(s.server, "Raw player", CAPACITY, lead_state());
    const std::shared_ptr<GroupInput> input = s.open_led({&player});
    ASSERT_TRUE(input);
    constexpr uint64_t TOTAL = 4 * RATE;
    std::thread host([&] {
        EXPECT_EQ(feed(*input, 0, TOTAL, 1), InputState::OPEN);
        input->drain();
    });
    const auto read_chunks = [&](size_t count) {
        const size_t target = player.chunks().size() + count;
        ASSERT_TRUE(
            player.read_until([&](const Entry&) { return player.chunks().size() >= target; }));
    };
    read_chunks(20);
    const auto [raised_from, raised] = update_state(player, lead_state(DELAY_US / 1000));
    read_chunks(20);
    const auto [lowered_from, lowered] = update_state(player, lead_state());
    ASSERT_TRUE(player.read_until_type("stream/end"));
    host.join();
    s.recorder.wait_for(Recorder::drained(input->id(), DrainOutcome::PLAYED));

    std::optional<uint64_t> expected_position;
    int64_t next_ts = 0;
    // The most a chunk sent under the higher delay counts with no delay
    int64_t raised_undelayed = 0;
    std::vector<const Chunk*> sent;
    for (size_t i = 0; i < player.log.size(); ++i) {
        if (player.log[i].type != "chunk") {
            continue;
        }
        SCOPED_TRACE(i);
        const Chunk& chunk = player.log[i].chunk;
        // The delay the server counted with: known outside the stretch between sending a state
        // and the reply that shows it taken in, where the higher one counts fewer chunks
        const int64_t delay = i >= raised_from && i < lowered ? DELAY_US : 0;
        // A skip may come while the higher delay is still in force, however long after the lower
        // one is sent
        if (next_ts != 0) {
            *expected_position += skipped_late(chunk, next_ts, i >= raised_from ? DELAY_US : 0);
        }
        next_ts = chunk.ts + duration_us(chunk.payload);
        // The stream starts on a cell; a pump run late on a loaded machine may skip a late one
        if (!expected_position) {
            expected_position = read_frame(chunk.payload.data()).position;
            ASSERT_EQ(*expected_position % FRAMES_PER_CELL, 0U);
        }
        for (size_t f = 0; f < chunk.payload.size() / 4; ++f) {
            ASSERT_EQ(read_frame(&chunk.payload[f * 4]), (Sample{*expected_position, 1}));
            ++*expected_position;
        }
        const int64_t tau = chunk.ts - chunk.send_ahead;
        int64_t counted = HEADER + static_cast<int64_t>(chunk.payload.size());
        int64_t undelayed = counted;
        for (const Chunk* earlier : sent) {
            const int64_t end = earlier->ts + duration_us(earlier->payload);
            const int64_t bytes = HEADER + static_cast<int64_t>(earlier->payload.size());
            counted += end - delay > tau ? bytes : 0;
            undelayed += end > tau ? bytes : 0;
        }
        EXPECT_LE(counted, CAPACITY);
        if (i > raised && i < lowered_from) {
            raised_undelayed = std::max(raised_undelayed, undelayed);
        }
        sent.push_back(&chunk);
    }
    // The higher delay let the stream run further ahead than the capacity holds with none: the
    // ledger fills to it at each send, however late the pump ran, and the chunks the delay
    // completes early count again without it
    EXPECT_GT(raised_undelayed, CAPACITY);
}

// spec player/v1.md "client/state player object" ("Format preference"), "stream/start player
// object", "Player Buffer Accounting": a player that prefers another format it lists mid-stream
// gets one in-place stream/start in it, with no stream/end or stream/clear, and its chunks go on on
// one timeline: 16-bit up to the switch and 24-bit after it, each sample once and in order, each
// chunk where the one before it ended. The in-place start leaves the ledger as it was, so every
// chunk fits the capacity counted with the chunks before the switch (a reset would let the server
// send a capacity's worth more at once). Four seconds of input, played in real time, so the switch
// lands mid-stream with the ledger full.
TEST(ServerPlayback, SwitchesAStreamInPlaceWhenThePlayerPrefersAnotherFormat) {
    PlaybackServer s;
    constexpr uint32_t CAPACITY = 100000;
    RawPlayer player(s.server, "Raw player", CAPACITY, lead_state(),
                     R"([{"codec":"pcm","sample_rate":48000,"channels":2,"bit_depth":16},)"
                     R"({"codec":"pcm","sample_rate":48000,"channels":2,"bit_depth":24}])");
    const std::shared_ptr<GroupInput> input = s.open_led({&player});
    ASSERT_TRUE(input);
    constexpr uint64_t TOTAL = 4 * RATE;
    std::thread host([&] {
        EXPECT_EQ(feed(*input, 0, TOTAL, 1), InputState::OPEN);
        input->drain();
    });
    ASSERT_TRUE(player.read_until([&](const Entry&) { return player.chunks().size() >= 20; }));
    std::string state = lead_state();
    state.pop_back();
    state += R"(,"format":{"codec":"pcm","sample_rate":48000,"channels":2,"bit_depth":24}})";
    player.client.send_json(client_state_json(true, state));
    ASSERT_TRUE(player.read_until_type("stream/end"));
    host.join();
    s.recorder.wait_for(Recorder::drained(input->id(), DrainOutcome::PLAYED));

    size_t starts = 0;
    size_t frame_bytes = 4;
    std::optional<uint64_t> expected;
    int64_t next_ts = 0;
    size_t wide = 0;
    std::vector<std::pair<int64_t, int64_t>> sent;
    for (const Entry& entry : player.log) {
        EXPECT_NE(entry.type, "stream/clear");
        if (entry.type == "stream/end") {
            break;
        }
        if (entry.type == "stream/start") {
            ++starts;
            JsonDocument doc;
            ASSERT_EQ(deserializeJson(doc, entry.json), DeserializationError::Ok);
            frame_bytes = doc["payload"]["player"]["bit_depth"].as<size_t>() / 8 * 2;
            continue;
        }
        if (entry.type != "chunk") {
            continue;
        }
        const Bytes& payload = entry.chunk.payload;
        ASSERT_EQ(payload.size() % frame_bytes, 0U);
        const auto frames = static_cast<int64_t>(payload.size() / frame_bytes);
        SCOPED_TRACE(entry.chunk.ts);
        if (next_ts != 0) {
            *expected += skipped_late(entry.chunk, next_ts);
        }
        next_ts = entry.chunk.ts + (frames * 1000000 / RATE);
        wide += frame_bytes == 6 ? 1 : 0;
        for (int64_t f = 0; f < frames; ++f) {
            const uint8_t* at = &payload[static_cast<size_t>(f) * frame_bytes];
            Sample sample = read_frame(at);
            if (frame_bytes == 6) {
                // At 24 bits, each sample is the 16-bit signal shifted up a byte
                const uint8_t shifted[4] = {at[1], at[2], at[4], at[5]};
                ASSERT_EQ(at[0], 0);
                ASSERT_EQ(at[3], 0);
                sample = read_frame(shifted);
            }
            if (!expected) {
                expected = sample.position;
            }
            ASSERT_EQ(sample, (Sample{*expected, 1}));
            ++*expected;
        }
        // I12 under the spec's count since the stream's start, the switch counting nothing
        const int64_t tau = entry.chunk.ts - entry.chunk.send_ahead;
        int64_t counted = HEADER + static_cast<int64_t>(payload.size());
        for (const auto& [end, bytes] : sent) {
            if (end > tau) {
                counted += bytes;
            }
        }
        EXPECT_LE(counted, CAPACITY);
        sent.emplace_back(next_ts, HEADER + static_cast<int64_t>(payload.size()));
    }
    EXPECT_EQ(starts, 2U);
    EXPECT_GT(wide, 0U);
    EXPECT_LT(wide, player.chunks().size());
}

// spec player/v1.md "client/hello player@v1 support object" (an operator-configured format),
// "client/state player object" ("Format preference"); server.h set_player_format(): a player
// listing 16- and 24-bit PCM starts in the operator's format over its own preference, including
// one set before the server first dials it, and in its own preference while the operator's is
// one it does not list.
TEST(ServerPlayback, StartsAPlayerInTheOperatorsFormatBeforeItsOwn) {
    using sendspin::server::StreamCodec;
    using sendspin::server::StreamFormat;
    const StreamFormat pcm24{StreamCodec::PCM, RATE, 2, 24};
    const StreamFormat unlisted{StreamCodec::FLAC, RATE, 2, 16};
    struct Row {
        const char* name;
        std::optional<uint32_t> client_bits;
        std::optional<StreamFormat> operator_format;
        bool before_dial;
        uint32_t bit_depth;
    };
    const Row rows[] = {
        {"Control: no preference", std::nullopt, std::nullopt, false, 16},
        {"the operator's over the client's", 16, pcm24, false, 24},
        {"the operator's, set before the server dials it", std::nullopt, pcm24, true, 24},
        {"the client's over an operator's it does not list", 24, unlisted, false, 24},
    };
    for (const Row& row : rows) {
        SCOPED_TRACE(row.name);
        PlaybackServer s;
        std::string state = lead_state();
        if (row.client_bits) {
            state.pop_back();
            state += R"(,"format":{"codec":"pcm","sample_rate":48000,"channels":2,"bit_depth":)" +
                     std::to_string(*row.client_bits) + "}}";
        }
        const auto set_operator = [&](const std::string& id) {
            EXPECT_TRUE(s.server.set_player_format(id, row.operator_format));
        };
        RawPlayer player(
            s.server, "Raw player", 200000, state,
            R"([{"codec":"pcm","sample_rate":48000,"channels":2,"bit_depth":16},)"
            R"({"codec":"pcm","sample_rate":48000,"channels":2,"bit_depth":24}])",
            true,
            row.before_dial ? std::function<void(const std::string&)>(set_operator) : nullptr);
        if (!row.before_dial && row.operator_format) {
            // Core takes the call before the input opens, which posts after it
            s.recorder.wait_for("connected " + player.id);
            set_operator(player.id);
        }
        const std::shared_ptr<GroupInput> input = s.open_led({&player});
        ASSERT_TRUE(input);
        std::thread host([&] {
            feed(*input, 0, RATE / 5, 1);
            input->drain();
        });
        ASSERT_TRUE(player.read_until_type("stream/end"));
        host.join();
        const auto start = std::find_if(player.log.begin(), player.log.end(),
                                        [](const Entry& e) { return e.type == "stream/start"; });
        ASSERT_NE(start, player.log.end());
        JsonDocument doc;
        ASSERT_EQ(deserializeJson(doc, start->json), DeserializationError::Ok);
        EXPECT_EQ(doc["payload"]["player"]["bit_depth"], row.bit_depth);
    }
}

// spec player/v1.md "Timing parameters": a higher lead mid-run raises the group lead at once, so
// the host's input is admitted further ahead, and the next epoch anchors with it, its first chunk
// at least the new lead after the write that began it. The test lasts as long as the raised lead
// takes to admit, and then to play, the input.
TEST(ServerPlayback, RaisesTheGroupLeadWhenAPlayersLeadRises) {
    PlaybackServer s;
    RawPlayer player(s.server, "Raw player", 100000, lead_state());
    const std::shared_ptr<GroupInput> input = s.open_led({&player});
    ASSERT_TRUE(input);
    std::atomic<bool> stop{false};
    std::thread host([&] {
        uint64_t position = 0;
        while (!stop.load()) {
            feed(*input, position, FRAMES_PER_CELL, 1);
            position += FRAMES_PER_CELL;
        }
    });
    ASSERT_TRUE(player.read_until_type("chunk"));
    // G = max(J_min, 700 ms) + M_pump before; W is one second, as the capacity is less
    constexpr int64_t RAISED_LEAD_MS = 1500;
    constexpr int64_t BEFORE_US = (REQUIRED_LEAD_MS * 1000) + 10000 + 1000000;
    update_state(player, player_state_json(MIN_BUFFER_MS, RAISED_LEAD_MS, 0));
    // Only the raised lead admits this far ahead; sampled with no timeout, so a regression hangs
    // into the suite watchdog
    while (input->latency_us() < BEFORE_US + CELL_US) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    stop = true;
    host.join();

    input->flush();
    const int64_t written_at = s.server.now_us();
    std::thread second([&] {
        EXPECT_EQ(feed(*input, 0, RATE, 2), InputState::OPEN);
        input->drain();
    });
    ASSERT_TRUE(player.read_until([](const Entry& entry) {
        return entry.type == "chunk" && read_frame(entry.chunk.payload.data()).tag == 2;
    }));
    EXPECT_GE(player.log.back().chunk.ts - written_at, (RAISED_LEAD_MS * 1000) + 10000);
    ASSERT_TRUE(player.read_until_type("stream/end"));
    second.join();
    s.recorder.wait_for(Recorder::drained(input->id(), DrainOutcome::PLAYED));
}

// ==============================================================================
// Pause, resume and flush
// ==============================================================================

/// The position of a chunk's first frame
uint64_t first_position(const Entry& entry) {
    return read_frame(entry.chunk.payload.data()).position;
}

// pause() returns the timeline position at now, the same on every player; the stream ends at once
// and the resume plays from that position with a fresh startup lead
TEST(ServerPlayback, ResumesFromThePositionPauseReturned) {
    PlaybackServer s;
    RawPlayer player(s.server, "Raw player", 200000, lead_state());
    const std::shared_ptr<GroupInput> input = s.open_led({&player});
    ASSERT_TRUE(input);
    constexpr uint64_t TOTAL = 3 * RATE;
    std::thread host([&] {
        feed(*input, 0, TOTAL, 1);
        input->drain();
    });
    // Pause once playback has started: a chunk sent after the first one's timestamp, as its
    // transmit time shows
    int64_t played_from = 0;
    ASSERT_TRUE(player.read_until([&](const Entry& entry) {
        if (entry.type != "chunk") {
            return false;
        }
        if (played_from == 0) {
            played_from = entry.chunk.ts;
        }
        return entry.chunk.ts - entry.chunk.send_ahead > played_from + 200000;
    }));
    const uint64_t paused = input->pause();
    EXPECT_GT(paused, 0U);
    EXPECT_EQ(input->pause(), paused);
    ASSERT_TRUE(player.read_until_type("stream/end"));
    const size_t ended = player.log.size();
    input->resume();
    ASSERT_TRUE(player.read_until_type("chunk"));
    const auto start =
        std::find_if(player.log.begin() + static_cast<std::ptrdiff_t>(ended), player.log.end(),
                     [](const Entry& e) { return e.type == "stream/start"; });
    ASSERT_NE(start, player.log.end());
    EXPECT_EQ(first_position(player.log.back()), paused);
    EXPECT_GE(player.log.back().chunk.ts - start->stamp(), J_MIN_US);

    ASSERT_TRUE(player.read_until_type("stream/end"));
    host.join();
    s.recorder.wait_for(Recorder::drained(input->id(), DrainOutcome::PLAYED));
    EXPECT_TRUE(ordered(player.log));
}

// A flush clears the stream, and the next write anchors a new epoch whose first chunk has a fresh
// startup lead from the clear; a drain cancelled by a flush reports FLUSHED
TEST(ServerPlayback, ClearsTheStreamOnAFlushAndStartsTheNewEpochAfterIt) {
    PlaybackServer s;
    RawPlayer player(s.server, "Raw player", 200000, lead_state());
    const std::shared_ptr<GroupInput> input = s.open_led({&player});
    ASSERT_TRUE(input);
    feed(*input, 0, RATE, 1);
    input->drain();
    ASSERT_TRUE(player.read_until_type("chunk"));
    input->flush();
    s.recorder.wait_for(Recorder::drained(input->id(), DrainOutcome::FLUSHED));
    ASSERT_TRUE(player.read_until_type("stream/clear"));
    const int64_t cleared = player.log.back().stamp();
    std::thread host([&] {
        EXPECT_EQ(feed(*input, 0, RATE, 2), InputState::OPEN);
        input->drain();
    });
    ASSERT_TRUE(player.read_until([](const Entry& entry) {
        return entry.type == "chunk" && read_frame(entry.chunk.payload.data()).tag == 2;
    }));
    // A group trigger: at least max(floor, J_min) after the clear's stamp
    EXPECT_GE(player.log.back().chunk.ts - cleared,
              std::max<int64_t>(MIN_BUFFER_MS * 1000, J_MIN_US));
    EXPECT_EQ(first_position(player.log.back()), 0U);
    ASSERT_TRUE(player.read_until_type("stream/end"));
    host.join();
    s.recorder.wait_for(Recorder::drained(input->id(), DrainOutcome::PLAYED));
    EXPECT_TRUE(ordered(player.log));
    EXPECT_EQ(std::count_if(player.log.begin(), player.log.end(),
                            [](const Entry& e) { return e.type == "stream/clear"; }),
              1);
}

// ==============================================================================
// Drain outcomes and the stop latch
// ==============================================================================

// Every drain() gets exactly one on_drained, with the id the call returned and how it resolved
TEST(ServerPlayback, AnswersEveryDrainOnceWithItsOutcome) {
    using Drains = std::vector<DrainId>;
    struct Row {
        const char* name;
        /// May release the input; returns the ids its drain() calls returned
        std::function<Drains(std::shared_ptr<GroupInput>&)> act;
        DrainOutcome outcome;
    };
    const Row rows[] = {
        {"Control: no run drains at once",
         [](std::shared_ptr<GroupInput>& input) { return Drains{input->drain()}; },
         DrainOutcome::PLAYED},
        {"a flush cancels a drain",
         [](std::shared_ptr<GroupInput>& input) {
             feed(*input, 0, FRAMES_PER_CELL * 10, 1);
             const DrainId drain = input->drain();
             input->flush();
             return Drains{drain};
         },
         DrainOutcome::FLUSHED},
        {"stop() discards a drain",
         [](std::shared_ptr<GroupInput>& input) {
             feed(*input, 0, FRAMES_PER_CELL * 10, 1);
             const DrainId drain = input->drain();
             // Refused while draining
             const Bytes frame(4, 0);
             EXPECT_EQ(input->write(frame.data(), 1).state, InputState::DRAINING);
             EXPECT_EQ(input->wait_writable(0), InputState::DRAINING);
             input->stop();
             return Drains{drain};
         },
         DrainOutcome::STOPPED},
        {"a second drain joins the first",
         [](std::shared_ptr<GroupInput>& input) {
             feed(*input, 0, FRAMES_PER_CELL * 10, 1);
             const DrainId first = input->drain();
             const DrainId second = input->drain();
             input->flush();
             return Drains{first, second};
         },
         DrainOutcome::FLUSHED},
        {"a pause keeps the drain for the resumed epoch",
         [](std::shared_ptr<GroupInput>& input) {
             feed(*input, 0, FRAMES_PER_CELL * 10, 1);
             const DrainId drain = input->drain();
             input->pause();
             input->resume();
             return Drains{drain};
         },
         DrainOutcome::PLAYED},
        // A flushed epoch with nothing written has nothing to play
        {"a drain after a flush, with nothing written since",
         [](std::shared_ptr<GroupInput>& input) {
             feed(*input, 0, FRAMES_PER_CELL * 10, 1);
             input->flush();
             return Drains{input->drain()};
         },
         DrainOutcome::PLAYED},
        {"a drain staged while paused after a flush, then resumed",
         [](std::shared_ptr<GroupInput>& input) {
             feed(*input, 0, FRAMES_PER_CELL * 10, 1);
             input->pause();
             input->flush();
             const DrainId drain = input->drain();
             input->resume();
             return Drains{drain};
         },
         DrainOutcome::PLAYED},
        // Releasing the GroupInput resolves a pending drain
        {"releasing the input with a drain pending",
         [](std::shared_ptr<GroupInput>& input) {
             feed(*input, 0, FRAMES_PER_CELL * 10, 1);
             const DrainId drain = input->drain();
             input.reset();
             return Drains{drain};
         },
         DrainOutcome::STOPPED},
    };
    for (const Row& row : rows) {
        SCOPED_TRACE(row.name);
        PlaybackServer s;
        std::shared_ptr<GroupInput> input = s.open();
        ASSERT_TRUE(input);
        const InputId id = input->id();
        const Drains drains = row.act(input);
        s.recorder.wait_for_count(Recorder::drained(id, row.outcome), drains.size());
        // Once the server stops, every callback it was going to deliver has been delivered
        s.server.stop();
        EXPECT_EQ(s.server.start(sendspin::server::test::local_config(), &s.provider, nullptr),
                  StartResult::OK);
        std::vector<std::pair<DrainId, DrainOutcome>> expected;
        for (const DrainId drain : drains) {
            expected.emplace_back(drain, row.outcome);
        }
        EXPECT_EQ(s.recorder.drains(), expected);
    }
}

// spec messaging.md "Server behavior when a client becomes unavailable": the solo player turning
// unavailable stops playback, which latches the input until the host's stop(); the player returning
// starts nothing, and only the host's next write plays again
TEST(ServerPlayback, LatchesTheInputWhenTheServerStopsPlayback) {
    PlaybackServer s;
    RawPlayer player(s.server, "Raw player", 200000, lead_state());
    const std::shared_ptr<GroupInput> input = s.open_led({&player});
    ASSERT_TRUE(input);
    feed(*input, 0, RATE / 2, 1);
    ASSERT_TRUE(player.read_until_type("chunk"));
    const std::string group = std::to_string(input->group().value);
    s.recorder.wait_for("group " + group + " playing");
    const std::string stopped = "group " + group + " stopped";
    const size_t stops = s.recorder.count(stopped);

    player.client.send_json(client_state_json(false, lead_state()));
    s.recorder.wait_for_count(stopped, stops + 1);
    // Seen without a write
    EXPECT_EQ(input->state(), InputState::STOP_LATCHED);
    const Bytes frame(4, 0);
    EXPECT_EQ(input->write(frame.data(), 1).state, InputState::STOP_LATCHED);
    EXPECT_EQ(input->wait_writable(std::numeric_limits<int64_t>::max()), InputState::STOP_LATCHED);
    // A drain while latched resolves at once as STOPPED, and leaves the latch
    input->drain();
    s.recorder.wait_for(Recorder::drained(input->id(), DrainOutcome::STOPPED));
    EXPECT_EQ(input->write(frame.data(), 1).state, InputState::STOP_LATCHED);
    ASSERT_TRUE(player.read_until_type("stream/end"));

    // The player returns while the host is still latched; the host then acknowledges and plays
    player.client.send_json(client_state_json(true, lead_state()));
    EXPECT_EQ(input->write(frame.data(), 1).state, InputState::STOP_LATCHED);
    input->stop();
    std::thread host([&] {
        EXPECT_EQ(feed(*input, 0, RATE / 2, 2), InputState::OPEN);
        input->drain();
    });
    ASSERT_TRUE(player.read_until_type("stream/end"));
    host.join();
    s.recorder.wait_for(Recorder::drained(input->id(), DrainOutcome::PLAYED));
    EXPECT_TRUE(ordered(player.log));
    // One start for the first run and one for the run the host began after its stop(): nothing
    // restarted the player while latched
    EXPECT_EQ(std::count_if(player.log.begin(), player.log.end(),
                            [](const Entry& e) { return e.type == "stream/start"; }),
              2);
    const auto second = std::find_if(player.log.rbegin(), player.log.rend(),
                                     [](const Entry& e) { return e.type == "stream/start"; });
    const auto next_chunk = std::find_if(second.base(), player.log.end(),
                                         [](const Entry& e) { return e.type == "chunk"; });
    ASSERT_NE(next_chunk, player.log.end());
    EXPECT_EQ(read_frame(next_chunk->chunk.payload.data()).tag, 2);
}

// ==============================================================================
// Several players
// ==============================================================================

// Identical audio carries identical timestamps on every player, whatever its format: each chunk's
// timestamp is the epoch's anchor plus its first position's cells
TEST(ServerPlayback, StampsIdenticalAudioIdenticallyOnEveryPlayer) {
    PlaybackServer s;
    RawPlayer shallow(s.server, "Shallow", 200000, lead_state());
    RawPlayer deep(s.server, "Deep", 300000, lead_state(40),
                   R"([{"codec":"pcm","sample_rate":48000,"channels":2,"bit_depth":24}])");
    const std::shared_ptr<GroupInput> input = s.open_led({&shallow, &deep});
    ASSERT_TRUE(input);
    std::thread host([&] {
        feed(*input, 0, RATE, 1);
        input->drain();
    });
    ASSERT_TRUE(shallow.read_until_type("stream/end"));
    ASSERT_TRUE(deep.read_until_type("stream/end"));
    host.join();
    s.recorder.wait_for(Recorder::drained(input->id(), DrainOutcome::PLAYED));

    const std::vector<const Entry*> shallow_chunks = shallow.chunks();
    const std::vector<const Entry*> deep_chunks = deep.chunks();
    ASSERT_FALSE(shallow_chunks.empty());
    ASSERT_FALSE(deep_chunks.empty());
    const auto t0_of = [](const Entry& entry, uint64_t position) {
        return entry.chunk.ts - static_cast<int64_t>(position / FRAMES_PER_CELL) * CELL_US;
    };
    const int64_t t0 = t0_of(*shallow_chunks.front(), first_position(*shallow_chunks.front()));
    for (const Entry* chunk : shallow_chunks) {
        EXPECT_EQ(first_position(*chunk) % FRAMES_PER_CELL, 0U);
        EXPECT_EQ(t0_of(*chunk, first_position(*chunk)), t0);
    }
    for (const Entry* chunk : deep_chunks) {
        // The 24-bit stream is the 16-bit signal shifted up by a byte
        const Bytes& payload = chunk->chunk.payload;
        ASSERT_EQ(payload.size() % 6, 0U);
        const uint8_t frame[4] = {payload[1], payload[2], payload[4], payload[5]};
        const uint64_t position = read_frame(frame).position;
        EXPECT_EQ(payload[0], 0);
        EXPECT_EQ(position % FRAMES_PER_CELL, 0U);
        EXPECT_EQ(t0_of(*chunk, position), t0);
    }
    EXPECT_TRUE(ordered(shallow.log));
}

// spec player/v1.md "Codec framing", "stream/start player object": a player that prefers FLAC in
// its client/state gets it over its first entry. The stream/start carries the STREAMINFO of the
// stream as its codec_header, and each chunk one or more whole frames of one 20 ms cell, numbered
// from 0 at the epoch's first cell, that decode to the host's audio bit for bit, at the timestamps
// a PCM player gets for the same audio.
TEST(ServerPlayback, StreamsFlacInWholeFramesAfterItsCodecHeader) {
    PlaybackServer s;
    std::string prefers_flac = lead_state();
    prefers_flac.insert(prefers_flac.size() - 1,
                        R"(,"format":{"codec":"flac","sample_rate":48000,"channels":2,)"
                        R"("bit_depth":16})");
    RawPlayer flac(s.server, "Flac", 200000, prefers_flac,
                   R"([{"codec":"pcm","sample_rate":48000,"channels":2,"bit_depth":16},)"
                   R"({"codec":"flac","sample_rate":48000,"channels":2,"bit_depth":16}])");
    RawPlayer pcm(s.server, "Pcm", 200000, lead_state());
    const std::shared_ptr<GroupInput> input = s.open_led({&flac, &pcm});
    ASSERT_TRUE(input);
    // Ends 300 frames into a cell; the short last frame that makes is not required sent here, as a
    // late one is skipped (GroupAudioTest.CutsFramesOnTheCellGridThroughADrain cuts it)
    constexpr uint64_t TOTAL = RATE + 300;
    std::thread host([&] {
        feed(*input, 0, TOTAL, 1);
        input->drain();
    });
    ASSERT_TRUE(flac.read_until_type("stream/end"));
    ASSERT_TRUE(pcm.read_until_type("stream/end"));
    host.join();
    s.recorder.wait_for(Recorder::drained(input->id(), DrainOutcome::PLAYED));
    EXPECT_TRUE(ordered(pcm.log));

    const auto start = std::find_if(flac.log.begin(), flac.log.end(),
                                    [](const Entry& e) { return e.type == "stream/start"; });
    ASSERT_NE(start, flac.log.end());
    JsonDocument doc;
    ASSERT_EQ(deserializeJson(doc, start->json), DeserializationError::Ok);
    const JsonObject player = doc["payload"]["player"];
    EXPECT_EQ(player["codec"], "flac");
    EXPECT_EQ(player["sample_rate"], RATE);
    EXPECT_EQ(player["channels"], 2);
    EXPECT_EQ(player["bit_depth"], 16);
    FlacReader reader;
    ASSERT_TRUE(reader.open(player["codec_header"].as<std::string>()));
    EXPECT_EQ(std::string(reader.header.begin(), reader.header.begin() + 4), "fLaC");
    EXPECT_EQ(reader.info().sample_rate(), RATE);
    EXPECT_EQ(reader.info().num_channels(), 2U);
    EXPECT_EQ(reader.info().bits_per_sample(), 16U);
    EXPECT_EQ(reader.info().max_block_size(), FRAMES_PER_CELL);

    // The group's anchor, from the PCM player's stream
    const Entry& reference = *pcm.chunks().front();
    const int64_t t0 = reference.chunk.ts -
                       static_cast<int64_t>(first_position(reference) / FRAMES_PER_CELL) * CELL_US;
    const std::vector<const Entry*> chunks = flac.chunks();
    ASSERT_FALSE(chunks.empty());
    std::optional<uint64_t> expected;
    size_t most_frames = 0;
    for (const Entry* chunk : chunks) {
        if (expected) {
            *expected += skipped_late(
                chunk->chunk, t0 + static_cast<int64_t>(*expected / FRAMES_PER_CELL) * CELL_US);
        }
        const std::optional<std::vector<DecodedFlacFrame>> frames =
            reader.decode(chunk->chunk.payload);
        ASSERT_TRUE(frames);
        ASSERT_FALSE(frames->empty());
        // At most 100 ms of 20 ms frames
        EXPECT_LE(frames->size(), 5U);
        most_frames = std::max(most_frames, frames->size());
        for (const DecodedFlacFrame& frame : *frames) {
            ASSERT_EQ(frame.pcm.size() % 4, 0U);
            const Sample first = read_frame(frame.pcm.data());
            if (!expected) {
                // The stream's first frame is the epoch's first cell
                expected = first.position;
                EXPECT_EQ(chunk->chunk.ts,
                          t0 + static_cast<int64_t>(first.position / FRAMES_PER_CELL) * CELL_US);
            }
            // One cell per frame, numbered by its cell in the epoch
            EXPECT_EQ(first.position % FRAMES_PER_CELL, 0U);
            EXPECT_EQ(frame.number, first.position / FRAMES_PER_CELL);
            for (size_t f = 0; f < frame.pcm.size() / 4; ++f) {
                ASSERT_EQ(read_frame(&frame.pcm[f * 4]), (Sample{*expected, 1}));
                ++*expected;
            }
        }
        // The chunk's timestamp is its first frame's
        const Sample head = read_frame((*frames)[0].pcm.data());
        EXPECT_EQ(chunk->chunk.ts,
                  t0 + static_cast<int64_t>(head.position / FRAMES_PER_CELL) * CELL_US);
    }
    // Chunks of several frames went out
    EXPECT_GT(most_frames, 1U);
}

// A player joining a playing group in a format no member streams gets a new instance from the cell
// the join can still reach, with the group's timeline, and plays the rest of the run in its own bit
// depth
TEST(ServerPlayback, StartsALateJoinerInItsOwnFormatOnTheGroupsTimeline) {
    PlaybackServer s;
    RawPlayer first(s.server, "First", 200000, lead_state());
    const std::shared_ptr<GroupInput> input = s.open_led({&first});
    ASSERT_TRUE(input);
    constexpr uint64_t TOTAL = 3 * RATE;
    std::thread host([&] {
        feed(*input, 0, TOTAL, 1);
        input->drain();
    });
    // Join once the run has played half a second, as a transmit time shows
    int64_t played_from = 0;
    ASSERT_TRUE(first.read_until([&](const Entry& entry) {
        if (entry.type != "chunk") {
            return false;
        }
        if (played_from == 0) {
            played_from = entry.chunk.ts;
        }
        return entry.chunk.ts - entry.chunk.send_ahead > played_from + 500000;
    }));
    RawPlayer joiner(s.server, "Joiner", 300000, lead_state(),
                     R"([{"codec":"pcm","sample_rate":48000,"channels":2,"bit_depth":24}])");
    ASSERT_TRUE(joiner.read_until_type("stream/end"));
    ASSERT_TRUE(first.read_until_type("stream/end"));
    host.join();
    s.recorder.wait_for(Recorder::drained(input->id(), DrainOutcome::PLAYED));
    EXPECT_TRUE(ordered(first.log));

    const auto start = std::find_if(joiner.log.begin(), joiner.log.end(),
                                    [](const Entry& e) { return e.type == "stream/start"; });
    ASSERT_NE(start, joiner.log.end());
    JsonDocument doc;
    ASSERT_EQ(deserializeJson(doc, start->json), DeserializationError::Ok);
    EXPECT_EQ(doc["payload"]["player"]["bit_depth"], 24);
    const std::vector<const Entry*> chunks = joiner.chunks();
    ASSERT_FALSE(chunks.empty());
    // A join trigger: the startup lead from the start's stamp
    EXPECT_GE(chunks.front()->chunk.ts - start->stamp(),
              std::max<int64_t>(MIN_BUFFER_MS * 1000, J_MIN_US));

    // The group's anchor, from the first player's stream
    const Entry& reference = *first.chunks().front();
    const int64_t t0 = reference.chunk.ts -
                       static_cast<int64_t>(first_position(reference) / FRAMES_PER_CELL) * CELL_US;
    std::optional<uint64_t> expected;
    for (const Entry* chunk : chunks) {
        if (expected) {
            *expected += skipped_late(
                chunk->chunk, t0 + static_cast<int64_t>(*expected / FRAMES_PER_CELL) * CELL_US);
        }
        const Bytes& payload = chunk->chunk.payload;
        ASSERT_EQ(payload.size() % 6, 0U);
        for (size_t f = 0; f < payload.size() / 6; ++f) {
            // The 16-bit signal shifted up by a byte
            const uint8_t frame[4] = {payload[(f * 6) + 1], payload[(f * 6) + 2],
                                      payload[(f * 6) + 4], payload[(f * 6) + 5]};
            const Sample sample = read_frame(frame);
            ASSERT_EQ(payload[f * 6], 0);
            if (!expected) {
                // Joined mid-run, on a cell boundary of the group's timeline
                EXPECT_GT(sample.position, 0U);
                EXPECT_EQ(sample.position % FRAMES_PER_CELL, 0U);
                EXPECT_EQ(chunk->chunk.ts,
                          t0 + static_cast<int64_t>(sample.position / FRAMES_PER_CELL) * CELL_US);
                expected = sample.position;
            }
            ASSERT_EQ(sample, (Sample{*expected, 1}));
            ++*expected;
        }
    }
}

// spec messaging.md "Server behavior when a client becomes unavailable": a player of a
// multi-client group that turns unavailable moves to a stopped group of its own, told by a
// group/update before its stream ends, while the others play on
TEST(ServerPlayback, MovesAPlayerThatTurnsUnavailableToAStoppedGroupOfItsOwn) {
    PlaybackServer s;
    RawPlayer stays(s.server, "Stays", 200000, lead_state());
    RawPlayer leaves(s.server, "Leaves", 200000, lead_state());
    const std::shared_ptr<GroupInput> input = s.open_led({&stays, &leaves});
    ASSERT_TRUE(input);
    std::thread host([&] {
        feed(*input, 0, 2 * RATE, 1);
        input->drain();
    });
    ASSERT_TRUE(leaves.read_until_type("chunk"));
    leaves.client.send_json(client_state_json(false, lead_state()));
    ASSERT_TRUE(leaves.read_until_type("stream/end"));
    const auto update = std::find_if(leaves.log.rbegin(), leaves.log.rend(),
                                     [](const Entry& e) { return e.type == "group/update"; });
    ASSERT_NE(update, leaves.log.rend());
    JsonDocument doc;
    ASSERT_EQ(deserializeJson(doc, update->json), DeserializationError::Ok);
    EXPECT_EQ(doc["payload"]["playback_state"], "stopped");
    EXPECT_EQ(doc["payload"]["group_name"], "Leaves");
    EXPECT_NE(doc["payload"]["group_id"].as<std::string>(), std::to_string(input->group().value));

    // The other player is not stopped: its stream runs to the drain's end
    ASSERT_TRUE(stays.read_until_type("stream/end"));
    host.join();
    s.recorder.wait_for(Recorder::drained(input->id(), DrainOutcome::PLAYED));
    EXPECT_GT(stays.chunks().back()->chunk.ts, leaves.chunks().back()->chunk.ts);
    EXPECT_TRUE(ordered(stays.log));
    EXPECT_TRUE(ordered(leaves.log));
    // No auto-rejoin: returning leaves it alone in its stopped group
    leaves.client.send_json(client_state_json(true, lead_state()));
    const std::vector<sendspin::server::GroupInfo> groups = s.server.groups();
    ASSERT_EQ(groups.size(), 2U);
    EXPECT_EQ(groups[1].client_ids, std::vector<std::string>{leaves.id});
    EXPECT_EQ(groups[0].client_ids, std::vector<std::string>{stays.id});
}

// A first client/state reporting `available: false` is an initial value, so the player keeps its
// place in the playing group with no stream, and reporting true then starts its stream as a join.
// messaging.md "client/leave": leaving is handled as turning unavailable, out of the shared group
// into a stopped one of its own with its stream ended, while the other plays on.
TEST(ServerPlayback, StartsAPlayerThatTurnsAvailableAndMovesOneThatLeaves) {
    PlaybackServer s;
    RawPlayer stays(s.server, "Stays", 200000, lead_state());
    const std::shared_ptr<GroupInput> input = s.open_led({&stays});
    ASSERT_TRUE(input);
    std::thread host([&] {
        feed(*input, 0, 2 * RATE, 1);
        input->drain();
    });
    ASSERT_TRUE(stays.read_until_type("chunk"));
    RawPlayer late(s.server, "Late", 200000, lead_state(), PCM16, false);
    // Once its time request is answered its state is taken in: it is still in the playing group
    late.client.send_json(client_time_json(1));
    ASSERT_TRUE(late.read_until_type("server/time"));
    const std::vector<sendspin::server::GroupInfo> groups = s.server.groups();
    const auto playing = std::find_if(
        groups.begin(), groups.end(),
        [&](const sendspin::server::GroupInfo& group) { return group.id == input->group(); });
    ASSERT_NE(playing, groups.end());
    EXPECT_EQ(playing->client_ids, (std::vector<std::string>{stays.id, late.id}));
    late.client.send_json(client_state_json(true, lead_state()));
    ASSERT_TRUE(late.read_until_type("chunk"));
    EXPECT_EQ(std::count_if(late.log.begin(), late.log.end(),
                            [](const Entry& e) { return e.type == "stream/start"; }),
              1);

    late.client.send_json(R"({"type":"client/leave","payload":{}})");
    ASSERT_TRUE(late.read_until_type("stream/end"));
    const auto update = std::find_if(late.log.rbegin(), late.log.rend(),
                                     [](const Entry& e) { return e.type == "group/update"; });
    ASSERT_NE(update, late.log.rend());
    JsonDocument doc;
    ASSERT_EQ(deserializeJson(doc, update->json), DeserializationError::Ok);
    EXPECT_EQ(doc["payload"]["playback_state"], "stopped");
    EXPECT_NE(doc["payload"]["group_id"].as<std::string>(), std::to_string(input->group().value));
    ASSERT_TRUE(stays.read_until_type("stream/end"));
    host.join();
    s.recorder.wait_for(Recorder::drained(input->id(), DrainOutcome::PLAYED));
    EXPECT_TRUE(ordered(late.log));
}

// A write cut short is answered by on_writable once there is room, including for a new input, which
// admits nothing until the server attaches it
TEST(ServerPlayback, TellsTheHostWhenItsInputHasRoom) {
    PlaybackServer s;
    const std::shared_ptr<GroupInput> input = s.open();
    ASSERT_TRUE(input);
    const std::string writable = "writable " + std::to_string(input->group().value) + " " +
                                 std::to_string(input->id().value);
    Bytes block(RATE * 4 * 3);
    WriteResult result = input->write(block.data(), RATE * 3);
    if (result.state == InputState::FULL && result.frames == 0) {
        // Not attached yet: the attachment answers it
        s.recorder.wait_for_count(writable, 1);
        result = input->write(block.data(), RATE * 3);
    }
    const size_t answered = s.recorder.count(writable);
    // A second ahead is admitted, not three
    EXPECT_GT(result.frames, 0U);
    EXPECT_LT(result.frames, RATE * 3);
    EXPECT_EQ(result.state, InputState::FULL);
    s.recorder.wait_for_count(writable, answered + 1);
    EXPECT_GT(input->write(block.data(), RATE * 3).frames, 0U);
}

// spec player/v1.md "client/hello player@v1 support object": a player listing no format the
// input can give (a frame longer than a chunk, a channel count the mix does not reach) gets no
// stream, and on_error names it
TEST(ServerPlayback, ReportsAPlayerWithNoPlayableFormat) {
    PlaybackServer s;
    RawPlayer player(s.server, "Elsewhere", 200000, lead_state(),
                     R"([{"codec":"pcm","sample_rate":44101,"channels":2,"bit_depth":16},)"
                     R"({"codec":"pcm","sample_rate":48000,"channels":6,"bit_depth":16}])");
    const std::shared_ptr<GroupInput> input = s.open();
    ASSERT_TRUE(input);
    s.recorder.wait_for(
        "error " +
        std::to_string(static_cast<int>(sendspin::server::ErrorKind::NO_PLAYABLE_FORMAT)) + " " +
        player.client.url());
    feed(*input, 0, RATE / 2, 1);
    input->drain();
    s.recorder.wait_for(Recorder::drained(input->id(), DrainOutcome::PLAYED));
    // Everything the server sent before the drain ended reaches the player before the close
    player.client.send_json(sendspin::server::test::client_goodbye_json("shutdown"));
    EXPECT_FALSE(player.read_until([](const Entry&) { return false; }));
    EXPECT_TRUE(std::none_of(player.log.begin(), player.log.end(), [](const Entry& e) {
        return e.type == "stream/start" || e.type == "chunk";
    }));
}

// A set_format() that gives a player with no playable format one makes it eligible, which is a
// join: it starts on the playing run, once, and plays on to the drain. Control: before the
// change it is reported as having no playable format.
TEST(ServerPlayback, StartsAPlayerOnceTheInputGivesItAFormat) {
    PlaybackServer s;
    RawPlayer player(s.server, "Stereo", 200000, lead_state());
    s.recorder.wait_for("connected " + player.id);
    InputOptions options;
    // Four channels, which a stereo player is offered no format for
    options.format = InputFormat{RATE, 4, SampleFormat::S16};
    const std::shared_ptr<GroupInput> input =
        s.server.open_input(s.server.groups().at(0).id, options).input;
    ASSERT_TRUE(input);
    s.recorder.wait_for(
        "error " +
        std::to_string(static_cast<int>(sendspin::server::ErrorKind::NO_PLAYABLE_FORMAT)) + " " +
        player.client.url());
    const Bytes silence(size_t{RATE} / 2 * 8, 0);
    for (size_t written = 0; written < RATE / 2;) {
        written += input->write(silence.data() + (written * 8), (RATE / 2) - written).frames;
        if (written < RATE / 2) {
            input->wait_writable(std::numeric_limits<int64_t>::max());
        }
    }
    // The run plays, and the markers its start queued, which find no format for the player, have
    // run: the rename's group/update is queued behind them, so once the player has it they have
    // popped, and a late one cannot start the stream on its own
    const GroupId group = s.server.groups().at(0).id;
    s.recorder.wait_for("group " + std::to_string(group.value) + " playing");
    ASSERT_TRUE(s.server.rename_group(group, "Barrier"));
    ASSERT_TRUE(player.read_until([](const Entry& entry) {
        return entry.type == "group/update" &&
               entry.json.find(R"("group_name":"Barrier")") != std::string::npos;
    }));
    ASSERT_TRUE(input->set_format({RATE, 2, SampleFormat::S16}));
    std::thread host([&] {
        EXPECT_EQ(feed(*input, 0, RATE, 1), InputState::OPEN);
        input->drain();
    });
    // Read through the run's end, which the drain's completion reports with a group/update
    ASSERT_TRUE(player.read_until([](const Entry& entry) {
        return entry.type == "group/update" &&
               entry.json.find(R"("playback_state":"stopped")") != std::string::npos;
    }));
    host.join();
    s.recorder.wait_for(Recorder::drained(input->id(), DrainOutcome::PLAYED));
    EXPECT_EQ(std::count_if(player.log.begin(), player.log.end(),
                            [](const Entry& e) { return e.type == "stream/start"; }),
              1);
    const std::vector<const Entry*> chunks = player.chunks();
    ASSERT_FALSE(chunks.empty());
}

// ==============================================================================
// The host's side of groups and inputs
// ==============================================================================

// on_group_changed reports each playback transition once: a start, a pause, a resume, a drain's
// end and a stop
TEST(ServerPlayback, ReportsEachPlaybackTransitionOnce) {
    PlaybackServer s;
    const std::shared_ptr<GroupInput> input = s.open();
    ASSERT_TRUE(input);
    const std::string group = "group " + std::to_string(input->group().value);
    const std::string playing = group + " playing";
    const std::string stopped = group + " stopped";

    feed(*input, 0, RATE / 2, 1);
    s.recorder.wait_for_count(playing, 1);
    input->pause();
    s.recorder.wait_for_count(stopped, 1);
    input->resume();
    s.recorder.wait_for_count(playing, 2);
    input->drain();
    s.recorder.wait_for(Recorder::drained(input->id(), DrainOutcome::PLAYED));
    s.recorder.wait_for_count(stopped, 2);
    feed(*input, 0, RATE / 2, 2);
    s.recorder.wait_for_count(playing, 3);
    input->stop();
    s.recorder.wait_for_count(stopped, 3);

    // Once the server stops, every event it was going to deliver has been delivered
    s.server.stop();
    EXPECT_EQ(s.server.start(sendspin::server::test::local_config(), &s.provider, nullptr),
              StartResult::OK);
    EXPECT_EQ(s.recorder.count(playing), 3U);
    EXPECT_EQ(s.recorder.count(stopped), 3U);
}

// default_group() is the server's group while it runs, even when a client's own group carries the
// same name
TEST(ServerPlayback, NamesTheServersGroupAsTheDefault) {
    MemoryProvider provider;
    Recorder recorder;
    SendspinServer server;
    EXPECT_FALSE(server.default_group().has_value());
    ServerConfig config = sendspin::server::test::local_config();
    config.name = "Shared name";
    ASSERT_EQ(server.start(config, &provider, &recorder), StartResult::OK);
    const std::optional<GroupId> server_group = server.default_group();
    ASSERT_TRUE(server_group.has_value());

    // Not approved for playback, so it joins a group of its own named after it
    RawClient client;
    ASSERT_TRUE(server.connect_url(client.url()));
    client.activate(player_hello_json("Shared name", PCM16, 100000));
    // groups() lists a group before its first on_group_changed
    const std::string server_group_event = "group " + std::to_string(server_group->value) + " ";
    recorder.wait_for_match([&](const std::string& what) {
        return what.starts_with("group ") && !what.starts_with(server_group_event);
    });
    const std::vector<GroupInfo> groups = server.groups();
    ASSERT_EQ(groups.size(), 2U);
    EXPECT_EQ(groups[0].name, groups[1].name);
    for (const GroupInfo& group : groups) {
        EXPECT_EQ(group.id == server_group, group.client_ids.empty());
    }
    server.stop();
    EXPECT_FALSE(server.default_group().has_value());
}

TEST(ServerPlayback, SaysWhyItCannotOpenAnInput) {
    SendspinServer stopped;
    EXPECT_EQ(stopped.open_input(GroupId{1}, s16_input()).result, OpenInputResult::NOT_RUNNING);

    PlaybackServer s;
    const std::optional<GroupId> group = s.server.default_group();
    ASSERT_TRUE(group.has_value());
    OpenedInput held = s.server.open_input(*group, s16_input());
    ASSERT_EQ(held.result, OpenInputResult::OK);
    ASSERT_TRUE(held.input);

    InputOptions no_rate = s16_input();
    no_rate.format.sample_rate = 0;
    InputOptions live = s16_input();
    live.kind = sendspin::server::InputKind::LIVE;
    live.write_jitter_us = sendspin::server::MAX_WRITE_JITTER_US + 1;
    InputOptions early = live;
    early.write_jitter_us = -1;
    struct Row {
        const char* name;
        GroupId group;
        InputOptions options;
        OpenInputResult result;
    };
    const Row rows[] = {
        {"the group holds an input", *group, s16_input(), OpenInputResult::GROUP_BUSY},
        {"no such group", GroupId{group->value + 100}, s16_input(), OpenInputResult::UNKNOWN_GROUP},
        {"an invalid format", *group, no_rate, OpenInputResult::INVALID_FORMAT},
        {"a live input's jitter past the cap", *group, live, OpenInputResult::INVALID_JITTER},
        {"a negative live jitter", *group, early, OpenInputResult::INVALID_JITTER},
    };
    for (const Row& row : rows) {
        SCOPED_TRACE(row.name);
        const OpenedInput opened = s.server.open_input(row.group, row.options);
        EXPECT_EQ(opened.result, row.result);
        EXPECT_FALSE(opened.input);
    }
    // Control: released, the group takes another, a live one with the largest jitter
    held.input.reset();
    InputOptions at_cap = live;
    at_cap.write_jitter_us = sendspin::server::MAX_WRITE_JITTER_US;
    EXPECT_EQ(s.server.open_input(*group, at_cap).result, OpenInputResult::OK);
}

// Core writes a live input's write-ahead, W_live, so its first write takes 200 ms of input; a
// buffered input's first write takes its write-ahead, 1 s (W_min) with no player to size it
TEST(ServerPlayback, AdmitsALiveInputItsLiveWriteAhead) {
    PlaybackServer s;
    const std::optional<GroupId> group = s.server.default_group();
    ASSERT_TRUE(group.has_value());
    const Bytes two_seconds(2 * RATE * 4, 0);
    for (const bool live : {true, false}) {
        SCOPED_TRACE(live);
        InputOptions options = s16_input();
        if (live) {
            options.kind = sendspin::server::InputKind::LIVE;
        }
        OpenedInput opened = s.server.open_input(*group, options);
        ASSERT_EQ(opened.result, OpenInputResult::OK);
        // Nothing is admitted until core attached the input and wrote its parameters
        ASSERT_EQ(opened.input->wait_writable(std::numeric_limits<int64_t>::max()),
                  InputState::OPEN);
        const WriteResult result = opened.input->write(two_seconds.data(), 2 * RATE);
        EXPECT_EQ(result.frames, live ? RATE / 5 : RATE);
        EXPECT_EQ(result.state, InputState::FULL);
    }
}

// Releasing an input and opening another on the same group at once: the new one attaches and
// plays, whether the released one was still being attached or was playing
TEST(ServerPlayback, OpensAnotherInputRightAfterReleasingOne) {
    PlaybackServer s;
    const std::optional<GroupId> group = s.server.default_group();
    ASSERT_TRUE(group.has_value());
    for (const bool played : {false, true, false, true}) {
        SCOPED_TRACE(played ? "released while playing" : "released at once");
        std::shared_ptr<GroupInput> released = s.server.open_input(*group, s16_input()).input;
        ASSERT_TRUE(released);
        if (played) {
            feed(*released, 0, FRAMES_PER_CELL * 10, 1);
        }
        released.reset();
        const std::shared_ptr<GroupInput> input = s.server.open_input(*group, s16_input()).input;
        ASSERT_TRUE(input);
        EXPECT_EQ(feed(*input, 0, FRAMES_PER_CELL * 10, 2), InputState::OPEN);
        const DrainId drain = input->drain();
        s.recorder.wait_for(Recorder::drained(input->id(), DrainOutcome::PLAYED));
        const auto drains = s.recorder.drains();
        EXPECT_TRUE(std::any_of(drains.begin(), drains.end(), [&](const auto& entry) {
            return entry.first == drain && entry.second == DrainOutcome::PLAYED;
        }));
    }
}

}  // namespace
