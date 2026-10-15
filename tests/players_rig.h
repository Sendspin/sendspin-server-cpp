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

/// @file players_rig.h
/// @brief What the in-process player tests run: the host's writes of the test signal in segments,
/// sendspin-cpp players the server dials, what the server sent each player decoded from its trace,
/// what each player played, the checks on both, and the server, its trace and its players together

#pragma once

#include "flac_reader.h"
#include "peer.h"
#include "player_sink.h"
#include "test_ports.h"
#include "test_recorder.h"
#include "wire_messages.h"
#include <ArduinoJson.h>
#include <gtest/gtest.h>

#include <sendspin/client.h>
#include <sendspin/player_role.h>
#include <sendspin/server/server.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <numeric>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace sendspin::server::test {

inline constexpr uint32_t RATE = 48000;
inline constexpr uint32_t CELL = RATE / 50;
inline constexpr int64_t CELL_US = 20000;

/// A frame's duration at rate: the cells that hold a whole number of samples
inline int64_t frame_us(uint32_t rate) {
    return int64_t{50 / std::gcd(rate, 50U)} * CELL_US;
}

/// A stretch of the signal the host writes: positions first to first + count - 1
struct Segment {
    uint32_t first{0};
    uint32_t count{0};

    bool holds(uint32_t position) const {
        return position >= this->first && position - this->first < this->count;
    }
};

/// Writes segment, waiting for room, until seek is set
/// @param fed set to the frames written so far
inline void feed(GroupInput& input, const Signal& signal, const Segment& segment,
                 const std::atomic<bool>* seek = nullptr, uint32_t* fed = nullptr) {
    constexpr uint32_t BLOCK = 960;
    Bytes block(BLOCK * signal.frame_bytes());
    uint32_t written = 0;
    while (written < segment.count) {
        if (seek != nullptr && seek->load()) {
            return;
        }
        const uint32_t n = std::min(BLOCK, segment.count - written);
        for (uint32_t i = 0; i < n; ++i) {
            signal.put(&block[i * signal.frame_bytes()], segment.first + written + i);
        }
        const WriteResult result = input.write(block.data(), n);
        written += static_cast<uint32_t>(result.frames);
        if (fed != nullptr) {
            *fed = written;
        }
        if (result.frames < n) {
            ASSERT_EQ(result.state, InputState::FULL);
            // Short waits, so a seek is seen while the input is full
            input.wait_writable(seek != nullptr ? 20000 : std::numeric_limits<int64_t>::max());
        }
    }
}

/// A sendspin-cpp client with a player role listing formats, listening for the server to dial;
/// its output plays frames of frame_bytes at rate. It asks for 150 ms of buffer unless told
/// otherwise.
class Player {
public:
    /// @param min_buffer_ms, required_lead_ms what its client/state reports
    /// @param buffer_bytes its audio buffer, of which it advertises two thirds as buffer_capacity
    Player(const std::vector<sendspin::AudioSupportedFormatObject>& formats, size_t frame_bytes,
           uint32_t rate, uint16_t min_buffer_ms = 150,
           std::optional<uint16_t> required_lead_ms = std::nullopt,
           size_t buffer_bytes = sendspin::PlayerRoleConfig::DEFAULT_AUDIO_BUFFER_CAPACITY)
        : port_(sendspin::server::test::free_port()),
          client_(sendspin::server::test::peer_config("Player " + std::to_string(this->port_),
                                                      this->port_)) {
        sendspin::PlayerRoleConfig config;
        config.audio_formats = formats;
        config.min_buffer_ms = min_buffer_ms;
        config.required_lead_time_ms = required_lead_ms;
        config.audio_buffer_capacity = buffer_bytes;
        sendspin::PlayerRole& role = this->client_.add_player(config);
        this->sink_ = std::make_unique<Sink>(role, frame_bytes, rate);
        role.set_listener(this->sink_.get());
        this->client_.set_network_provider(&this->network_);
        this->client_.set_unpaired_access_enabled(true);
        EXPECT_TRUE(this->client_.start());
    }

    ~Player() {
        this->client_.stop();
    }

    Player(const Player&) = delete;
    Player& operator=(const Player&) = delete;

    std::string url() const {
        return sendspin::server::test::peer_url(this->port_);
    }

    sendspin::SendspinClient& client() {
        return this->client_;
    }

    const Sink& sink() const {
        return *this->sink_;
    }

private:
    uint16_t port_;
    AlwaysReady network_;
    sendspin::SendspinClient client_;
    std::unique_ptr<Sink> sink_;
};

/// Runs every player's main loop on this thread until done() holds, with no timeout
template <typename Predicate>
void run_until(std::vector<std::unique_ptr<Player>>& players, Predicate done) {
    sendspin::server::test::run_all_until(players, done);
}

// ==============================================================================
// What the server sent each player, from its trace
// ==============================================================================

/// One chunk: its timestamp and the stamp it was sent with, its samples per channel and their
/// rate, its first position when its stream carries the signal as written, whether it is FLAC and
/// then its first frame's number and its frame count, how many stream/start and stream/clear
/// messages its client had been sent before it, and the stamp of the last of those
struct ChunkInfo {
    int64_t ts{0};
    int64_t stamp_us{0};
    uint32_t position{0};
    bool flac{false};
    uint64_t number{0};
    size_t frames{1};
    size_t stream{0};
    size_t clears{0};
    int64_t trigger_us{0};
    uint32_t samples{0};
    uint32_t rate{0};
    Bytes pcm;
};

/// Decodes each chunk the server sends, PCM or FLAC, through the codec_header of its stream/start.
/// A stream at the input's rate with two channels carries the signal, so its chunks decode to
/// positions, unless the input changes format; another stream's chunks keep only their samples.
class StreamTrace {
public:
    /// @param positions whether a stream at the input's rate and channel count carries the signal
    StreamTrace(const Signal& signal, uint32_t input_rate, Recorder& recorder,
                bool positions = true)
        : signal_(signal), input_rate_(input_rate), recorder_(recorder), positions_(positions) {}

    /// On the dispatch thread
    void on_message(const TracedMessage& message) {
        const bool chunk = message.direction == TraceDirection::OUTBOUND && message.binary &&
                           message.plaintext.size() > wire::AUDIO_CHUNK_HEADER &&
                           message.plaintext[0] == wire::AUDIO_CHUNK_ID;
        if (!chunk) {
            this->recorder_.trace(message);
        }
        if (message.direction == TraceDirection::INBOUND) {
            // A JSON message after the handshake is binary, behind its message ID 0
            const size_t offset = message.binary ? 1 : 0;
            JsonDocument doc;
            if (message.plaintext.size() > offset &&
                (!message.binary || message.plaintext.front() == wire::JSON_ID) &&
                deserializeJson(doc, message.plaintext.data() + offset,
                                message.plaintext.size() - offset) == DeserializationError::Ok &&
                doc["type"] == "client/state") {
                const std::lock_guard<std::mutex> lock(this->mutex);
                this->states.insert(message.client_id);
            }
            return;
        }
        if (message.direction == TraceDirection::INBOUND || !message.binary ||
            message.plaintext.empty()) {
            return;
        }
        const std::lock_guard<std::mutex> lock(this->mutex);
        if (message.plaintext[0] == wire::JSON_ID) {
            JsonDocument doc;
            if (deserializeJson(doc, message.plaintext.data() + 1, message.plaintext.size() - 1) !=
                DeserializationError::Ok) {
                return;
            }
            if (doc["type"] == "stream/clear") {
                ++this->clears_[message.client_id];
                this->triggers_[message.client_id] = message.stamp_us;
            }
            // The server lists the roles a stream/end ends
            if (doc["type"] == "stream/end") {
                for (const JsonVariantConst role : doc["payload"]["roles"].as<JsonArrayConst>()) {
                    if (role == "player") {
                        this->active_.erase(message.client_id);
                    }
                }
            }
            if (doc["type"] == "stream/start" && !doc["payload"]["player"].isNull()) {
                const std::string codec = doc["payload"]["player"]["codec"].as<std::string>();
                this->codecs[message.client_id] = codec;
                // A start while the stream is active announces a format in place: the stream goes
                // on, with no trigger
                if (this->active_.insert(message.client_id).second) {
                    ++this->streams[message.client_id];
                    this->triggers_[message.client_id] = message.stamp_us;
                }
                const JsonObjectConst player = doc["payload"]["player"];
                this->rates_[message.client_id] = player["sample_rate"].as<uint32_t>();
                this->frame_bytes_[message.client_id] =
                    player["channels"].as<uint32_t>() * player["bit_depth"].as<uint32_t>() / 8;
                this->as_written_[message.client_id] =
                    this->positions_ &&
                    doc["payload"]["player"]["sample_rate"].as<uint32_t>() == this->input_rate_ &&
                    doc["payload"]["player"]["channels"].as<uint32_t>() == 2;
                this->readers_.erase(message.client_id);
                if (codec == "flac" &&
                    !this->readers_[message.client_id].open(
                        doc["payload"]["player"]["codec_header"].as<std::string>())) {
                    this->failed = true;
                }
            }
            return;
        }
        if (!chunk) {
            return;
        }
        ChunkInfo info;
        info.stamp_us = message.stamp_us;
        info.stream = this->streams[message.client_id];
        info.clears = this->clears_[message.client_id];
        info.trigger_us = this->triggers_[message.client_id];
        info.rate = this->rates_[message.client_id];
        info.ts = wire::timestamp_at(message.plaintext);
        const std::span<const uint8_t> payload =
            wire::payload_after(message.plaintext, wire::AUDIO_CHUNK_HEADER);
        const auto reader = this->readers_.find(message.client_id);
        if (reader == this->readers_.end()) {
            info.pcm.assign(payload.begin(), payload.end());
        } else {
            const std::optional<std::vector<sendspin::server::test::DecodedFlacFrame>> frames =
                reader->second.decode(payload);
            if (!frames || frames->empty()) {
                this->failed = true;
                return;
            }
            info.flac = true;
            info.number = frames->front().number;
            info.frames = frames->size();
            for (const sendspin::server::test::DecodedFlacFrame& frame : *frames) {
                info.pcm.insert(info.pcm.end(), frame.pcm.begin(), frame.pcm.end());
            }
        }
        if (this->as_written_[message.client_id]) {
            const std::optional<uint32_t> position = this->signal_.get(info.pcm.data());
            if (!position) {
                this->failed = true;
                return;
            }
            info.position = *position;
        }
        info.samples =
            static_cast<uint32_t>(info.pcm.size() / this->frame_bytes_[message.client_id]);
        this->chunks[message.client_id].push_back(std::move(info));
    }

    std::mutex mutex;
    /// The codec of each client's last stream/start
    std::map<std::string, std::string> codecs;
    /// The stream/start messages each client was sent
    std::map<std::string, size_t> streams;
    /// The clients whose client/state came in
    std::set<std::string> states;
    std::map<std::string, std::vector<ChunkInfo>> chunks;
    /// A codec_header or a chunk did not decode to the signal
    bool failed{false};

private:
    const Signal& signal_;
    uint32_t input_rate_;
    Recorder& recorder_;
    bool positions_;
    std::map<std::string, FlacReader> readers_;
    /// Whether each client's stream carries the signal as written
    std::map<std::string, bool> as_written_;
    /// The clients with a player stream, between its stream/start and stream/end
    std::set<std::string> active_;
    /// Each client's stream/clear messages, the stamp of its last stream/start or stream/clear,
    /// and its stream's rate and bytes per frame of decoded samples
    std::map<std::string, size_t> clears_;
    std::map<std::string, int64_t> triggers_;
    std::map<std::string, uint32_t> rates_;
    std::map<std::string, uint32_t> frame_bytes_;
};

// ==============================================================================
// What a player played
// ==============================================================================

/// One frame of a player's output
struct Output {
    enum class Kind : uint8_t { POSITION, ALTERED, SILENT };
    Kind kind{Kind::POSITION};
    uint32_t position{0};
};

/// A player's output with the silence before and after the stream left out
inline std::vector<Output> decode(const Signal& signal, const Bytes& output) {
    const size_t size = signal.frame_bytes();
    size_t begin = 0;
    size_t end = output.size() - (output.size() % size);
    const auto silent = [&](size_t offset) {
        return std::all_of(output.begin() + static_cast<std::ptrdiff_t>(offset),
                           output.begin() + static_cast<std::ptrdiff_t>(offset + size),
                           [](uint8_t byte) { return byte == 0; });
    };
    while (begin < end && silent(begin)) {
        begin += size;
    }
    while (end > begin && silent(end - size)) {
        end -= size;
    }
    std::vector<Output> frames;
    for (size_t offset = begin; offset < end; offset += size) {
        if (silent(offset)) {
            frames.push_back({Output::Kind::SILENT, 0});
        } else if (const std::optional<uint32_t> position = signal.get(&output[offset])) {
            frames.push_back({Output::Kind::POSITION, *position});
        } else {
            frames.push_back({Output::Kind::ALTERED, 0});
        }
    }
    return frames;
}

/// The players' startup floor, max(min_buffer_ms + output_delay_ms, J_min): each player asks for
/// 150 ms and no output delay
inline constexpr int64_t START_FLOOR_US = 150000;
/// The latest a stream's first chunk may be after its trigger: the group lead (160 ms for these
/// players) when a group trigger finds the epoch's first frame, else the floor and one 40 ms
/// frame, with room left for a frame that the pump, run late on a loaded machine, finds late
inline constexpr int64_t START_CEILING_US = 500000;

/// What the server sent one client: the first chunk after each stream/start or stream/clear between
/// START_FLOOR_US and START_CEILING_US after that trigger, and every other one right where the
/// chunk before it ended, so no audio is skipped, repeated or reordered. The timeline has no shift
/// in these tests; a gap only where its last frame had passed by the next chunk's stamp. How far
/// ahead of its time a chunk left is when the pump ran, which a loaded machine delays, so it is not
/// checked here: PlayerStreamPacking checks the hold a player's min_buffer_ms sets, and SchedCursor
/// the skip of a frame already late. That the pump wakes for a held chunk at its room_time
/// (Session::take_message() through next_wake(), which is unit-tested) is covered end to end by
/// nothing.
inline void expect_sent_whole(const std::vector<ChunkInfo>& chunks) {
    for (size_t i = 0; i < chunks.size(); ++i) {
        const ChunkInfo& chunk = chunks[i];
        SCOPED_TRACE(chunk.ts);
        const bool first =
            i == 0 || chunks[i - 1].stream != chunk.stream || chunks[i - 1].clears != chunk.clears;
        // The pump skips a frame already late when it runs (sched::prepare_cursor()), as when core
        // stalls on a loaded machine, so audio may be missing only where it was late
        if (first) {
            EXPECT_GE(chunk.ts - chunk.trigger_us, START_FLOOR_US);
            if (chunk.ts - frame_us(chunk.rate) > chunk.stamp_us) {
                EXPECT_LE(chunk.ts - chunk.trigger_us, START_CEILING_US);
            }
            continue;
        }
        const ChunkInfo& before = chunks[i - 1];
        const int64_t duration_us = int64_t{before.samples} * 1000000;
        ASSERT_EQ(duration_us % before.rate, 0) << "a chunk of part of a cell before another";
        const int64_t expected = before.ts + (duration_us / before.rate);
        if (chunk.ts > expected && chunk.ts - frame_us(before.rate) <= chunk.stamp_us) {
            continue;
        }
        EXPECT_EQ(chunk.ts, expected) << "a gap in the chunks";
    }
}

/// Checks a player's output and the chunks the server sent it, for segments of the signal written
/// in their order (a segment after the first follows a seek or a resume): each chunk on a frame
/// boundary of its segment. The output holds some of every segment, in order, never going back to
/// an earlier one, each frame of the signal bit for bit, its positions rising within a segment,
/// but for one blended frame at a time.
///
/// Where the output enters a segment, and what it skips or fills with silence within one, depends
/// on timing alone. The server's trigger may go out after the slack its group lead leaves, so the
/// stream starts at a later cell, in sync; a live rebase's gap plays as silence. The sync task
/// blends a frame in or out at chunk boundaries (SyncTask::soft_sync_insert_frame(),
/// soft_sync_drop_frame()), and when it starts, re-aligns after a seek, or loses sync because its
/// threads ran late, it drops the late prefix of a chunk or fills with silence
/// (SyncTask::handle_synchronize_audio()). The startup lead is checked from the stamps with raw
/// players. How much of the end it plays depends on its threads keeping real time: one behind its
/// clock at the stream/end drops what it has not played. So does how much of the end the server
/// sends, since a frame late when the pump runs is skipped; that a stream runs through its last
/// frame is checked with the clock passed in (PlayerStreamSwitch).
/// @param rate the signal's sample rate
inline void expect_heard(const std::vector<Output>& frames, const std::vector<ChunkInfo>& chunks,
                         const std::vector<Segment>& segments, uint32_t rate = RATE) {
    const auto segment_of = [&](uint32_t position) -> size_t {
        const auto it = std::find_if(segments.begin(), segments.end(), [&](const Segment& segment) {
            return segment.holds(position);
        });
        return static_cast<size_t>(it - segments.begin());
    };
    // A frame's samples: the whole number the fewest cells hold at the rate
    const uint32_t frame = rate / std::gcd(rate, 50U);
    for (const ChunkInfo& chunk : chunks) {
        const size_t segment = segment_of(chunk.position);
        ASSERT_LT(segment, segments.size()) << chunk.position;
        EXPECT_EQ((chunk.position - segments[segment].first) % frame, 0U) << chunk.position;
    }
    std::vector<size_t> heard(segments.size(), 0);
    std::optional<uint32_t> previous;
    size_t altered = 0;
    for (const Output& frame : frames) {
        if (frame.kind == Output::Kind::ALTERED) {
            ++altered;
            continue;
        }
        if (frame.kind == Output::Kind::SILENT) {
            continue;
        }
        // Not a SCOPED_TRACE: a second of output is 48,000 frames
        const uint32_t position = frame.position;
        const size_t segment = segment_of(position);
        ASSERT_LT(segment, segments.size()) << "a frame of no segment: " << position;
        if (!previous) {
            EXPECT_EQ(segment, 0U) << position;
        } else if (segment != segment_of(*previous)) {
            ASSERT_GT(segment, segment_of(*previous)) << "back to an earlier segment: " << position;
        } else {
            ASSERT_GT(position, *previous);
        }
        EXPECT_LE(altered, 1U) << position;
        previous = position;
        altered = 0;
        ++heard[segment];
    }
    // A drop in the last chunk leaves its blended frame last
    EXPECT_LE(altered, 1U);
    for (size_t i = 0; i < segments.size(); ++i) {
        EXPECT_GT(heard[i], 0U) << "segment " << i << " not heard";
    }
}

/// expect_heard(), with the server having sent each segment whole from where its stream entered
/// it (expect_sent_whole())
inline void expect_played(const std::vector<Output>& frames, const std::vector<ChunkInfo>& chunks,
                          const std::vector<Segment>& segments, uint32_t rate = RATE) {
    expect_sent_whole(chunks);
    expect_heard(frames, chunks, segments, rate);
}

/// The server, its trace and its players
class Rig {
public:
    /// @param input_rate the rate of the input open() opens
    /// @param positions whether chunks at the input's rate and channel count decode to positions
    /// @param opus ServerConfig::enable_opus
    explicit Rig(const Signal& signal, uint32_t input_rate = RATE, bool positions = true,
                 bool opus = false)
        : signal(signal),
          input_rate(input_rate),
          trace(signal, input_rate, this->recorder, positions) {
        ServerConfig config = sendspin::server::test::local_config();
        config.name = "Players";
        config.approve_all_clients = true;
        config.enable_opus = opus;
        config.trace = [this](const TracedMessage& message) { this->trace.on_message(message); };
        EXPECT_EQ(this->server.start(config, &this->provider, &this->recorder), StartResult::OK);
    }

    /// Adds a player, which the server dials, and runs every player until the server has its
    /// client/state
    /// @param rate, channels its output's, which its stream must have
    /// @param min_buffer_ms, required_lead_ms what its client/state reports
    /// @param buffer_bytes its audio buffer
    Player& add(const std::vector<sendspin::AudioSupportedFormatObject>& formats,
                uint32_t rate = RATE, uint32_t channels = 2, uint16_t min_buffer_ms = 150,
                std::optional<uint16_t> required_lead_ms = std::nullopt,
                size_t buffer_bytes = sendspin::PlayerRoleConfig::DEFAULT_AUDIO_BUFFER_CAPACITY) {
        this->players.push_back(
            std::make_unique<Player>(formats, this->signal.bits() / 8 * channels, rate,
                                     min_buffer_ms, required_lead_ms, buffer_bytes));
        const std::string url = this->players.back()->url();
        EXPECT_TRUE(this->server.connect_url(url));
        // Its own client/state, not one another player sends meanwhile
        run_until(this->players, [&] {
            const std::optional<std::string> id = this->find_client_id(url);
            const std::lock_guard<std::mutex> lock(this->trace.mutex);
            return id && this->trace.states.contains(*id);
        });
        return *this->players.back();
    }

    /// Opens an input to group, the server's group by default
    std::shared_ptr<GroupInput> open(std::optional<GroupId> group = std::nullopt) {
        InputOptions options;
        options.format = InputFormat{this->input_rate, 2, this->signal.sample_format()};
        return this->server.open_input(group.value_or(*this->server.default_group()), options)
            .input;
    }

    /// Opens a live input to the server's group, with the default write jitter
    std::shared_ptr<GroupInput> open_live() {
        InputOptions options;
        options.format = InputFormat{this->input_rate, 2, this->signal.sample_format()};
        options.kind = sendspin::server::InputKind::LIVE;
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access): set while the server runs
        return this->server.open_input(*this->server.default_group(), options).input;
    }

    std::optional<std::string> find_client_id(const std::string& url) const {
        for (const sendspin::server::ClientInfo& client : this->server.clients()) {
            if (client.url == url) {
                return client.client_id;
            }
        }
        return std::nullopt;
    }

    std::string client_id(const Player& player) const {
        const std::optional<std::string> id = this->find_client_id(player.url());
        if (!id) {
            ADD_FAILURE() << "no client at " << player.url();
        }
        return id.value_or(std::string{});
    }

    /// Runs every player until the host heard each of drained and every player played out its
    /// stream: a drain completes by the server's clock, which can be before a player's output
    /// plays its last frames.
    void run_until_drained(std::initializer_list<std::string> drained) {
        run_until(this->players, [&] {
            return std::all_of(
                       drained.begin(), drained.end(),
                       [&](const std::string& event) { return this->recorder.contains(event); }) &&
                   std::all_of(this->players.begin(), this->players.end(),
                               [](const auto& player) { return player->sink().played_out(); });
        });
    }

    /// Whether the server has sent player a chunk
    bool has_chunks(const Player& player) {
        const std::string id = this->client_id(player);
        const std::lock_guard<std::mutex> lock(this->trace.mutex);
        const auto it = this->trace.chunks.find(id);
        return it != this->trace.chunks.end() && !it->second.empty();
    }

    const Signal& signal;
    uint32_t input_rate;
    MemoryProvider provider;
    Recorder recorder;
    StreamTrace trace;
    SendspinServer server;
    std::vector<std::unique_ptr<Player>> players;
};

/// Within each segment, every chunk's timestamp is one anchor plus its position's offset in the
/// segment, exactly, the same anchor for every player
/// @return the anchor of each segment with a chunk, by its index
inline std::map<size_t, int64_t> expect_one_anchor_per_segment(
    const std::map<std::string, std::vector<ChunkInfo>>& chunks,
    const std::vector<Segment>& segments, uint32_t rate = RATE) {
    std::map<size_t, int64_t> anchors;
    for (const auto& [client, list] : chunks) {
        SCOPED_TRACE(client);
        for (const ChunkInfo& chunk : list) {
            const auto segment =
                std::find_if(segments.begin(), segments.end(),
                             [&](const Segment& s) { return s.holds(chunk.position); });
            EXPECT_NE(segment, segments.end());
            if (segment == segments.end()) {
                continue;
            }
            const int64_t offset_us = int64_t{chunk.position - segment->first} * 1000000;
            EXPECT_EQ(offset_us % rate, 0) << chunk.position;
            const int64_t anchor = chunk.ts - (offset_us / rate);
            const auto index = static_cast<size_t>(segment - segments.begin());
            const auto [it, added] = anchors.emplace(index, anchor);
            EXPECT_EQ(it->second, anchor) << chunk.position;
        }
    }
    return anchors;
}

}  // namespace sendspin::server::test
