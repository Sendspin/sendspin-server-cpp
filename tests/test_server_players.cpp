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

/// @file test_server_players.cpp
/// @brief Playback end to end with in-process sendspin-cpp players: what each player's sync task
/// writes to its audio output is the host's PCM, bit for bit, in order, whether it streams PCM or
/// FLAC; and Opus, lossy, close to the PCM player's audio at the same timestamps.

#include "flac_reader.h"
#include "opus_reader.h"
#include "player_sink.h"
#include "resampler_reference.h"
#include "test_ports.h"
#include "test_recorder.h"
#include <ArduinoJson.h>
#include <gtest/gtest.h>

#include <sendspin/client.h>
#include <sendspin/player_role.h>
#include <sendspin/server/server.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <functional>
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
#include <thread>
#include <utility>
#include <vector>

namespace {

using sendspin::server::DrainOutcome;
using sendspin::server::GroupId;
using sendspin::server::GroupInfo;
using sendspin::server::GroupInput;
using sendspin::server::InputFormat;
using sendspin::server::InputOptions;
using sendspin::server::InputState;
using sendspin::server::SampleFormat;
using sendspin::server::SendspinServer;
using sendspin::server::ServerConfig;
using sendspin::server::StartResult;
using sendspin::server::StreamCodec;
using sendspin::server::StreamFormat;
using sendspin::server::TraceDirection;
using sendspin::server::TracedMessage;
using sendspin::server::WriteResult;
using sendspin::server::test::AlwaysReady;
using sendspin::server::test::client_clock_us;
using sendspin::server::test::FlacReader;
using sendspin::server::test::MemoryProvider;
using sendspin::server::test::Recorder;
using sendspin::server::test::ReferenceResampler;
using sendspin::server::test::Signal;
using sendspin::server::test::Sink;
using Bytes = std::vector<uint8_t>;
using Codec = sendspin::SendspinCodecFormat;

constexpr uint32_t RATE = 48000;
constexpr uint32_t CELL = RATE / 50;
constexpr int64_t CELL_US = 20000;

/// A frame's duration at rate: the cells that hold a whole number of samples
int64_t frame_us(uint32_t rate) {
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
void feed(GroupInput& input, const Signal& signal, const Segment& segment,
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
        : port_(sendspin::server::test::free_port()), client_(make_config(this->port_)) {
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
        return "ws://127.0.0.1:" + std::to_string(this->port_) + "/sendspin";
    }

    sendspin::SendspinClient& client() {
        return this->client_;
    }

    const Sink& sink() const {
        return *this->sink_;
    }

private:
    static sendspin::SendspinClientConfig make_config(uint16_t port) {
        sendspin::SendspinClientConfig config;
        config.name = "Player " + std::to_string(port);
        config.server_port = port;
        config.time_burst_interval_ms = 50;
        return config;
    }

    uint16_t port_;
    AlwaysReady network_;
    sendspin::SendspinClient client_;
    std::unique_ptr<Sink> sink_;
};

/// Runs every player's main loop on this thread until done() holds, with no timeout
template <typename Predicate>
void run_until(std::vector<std::unique_ptr<Player>>& players, Predicate done) {
    while (!done()) {
        for (const std::unique_ptr<Player>& player : players) {
            player->client().loop();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
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
                           message.plaintext.size() > 13 && message.plaintext[0] == 4;
        if (!chunk) {
            this->recorder_.trace(message);
        }
        if (message.direction == TraceDirection::INBOUND) {
            // A JSON message after the handshake is binary, behind its message ID 0
            const size_t offset = message.binary ? 1 : 0;
            JsonDocument doc;
            if (message.plaintext.size() > offset &&
                (!message.binary || message.plaintext.front() == 0) &&
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
        if (message.plaintext[0] == 0) {
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
        for (size_t i = 1; i < 9; ++i) {
            info.ts = (info.ts << 8) | message.plaintext[i];
        }
        const std::span<const uint8_t> payload(message.plaintext.data() + 13,
                                               message.plaintext.size() - 13);
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
std::vector<Output> decode(const Signal& signal, const Bytes& output) {
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
constexpr int64_t START_FLOOR_US = 150000;
/// The latest a stream's first chunk may be after its trigger: the group lead (160 ms for these
/// players) when a group trigger finds the epoch's first frame, else the floor and one 40 ms
/// frame, with room left for a frame that the pump, run late on a loaded machine, finds late
constexpr int64_t START_CEILING_US = 500000;

/// What the server sent one client: the first chunk after each stream/start or stream/clear between
/// START_FLOOR_US and START_CEILING_US after that trigger, and every other one right where the
/// chunk before it ended, so no audio is skipped, repeated or reordered. The timeline has no shift
/// in these tests; a gap only where its last frame had passed by the next chunk's stamp. How far
/// ahead of its time a chunk left is when the pump ran, which a loaded machine delays, so it is not
/// checked here: PlayerStreamPacking checks the hold a player's min_buffer_ms sets, and SchedCursor
/// the skip of a frame already late. That the pump wakes for a held chunk at its room_time
/// (Session::take_message() through next_wake(), which is unit-tested) is covered end to end by
/// nothing.
void expect_sent_whole(const std::vector<ChunkInfo>& chunks) {
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
void expect_heard(const std::vector<Output>& frames, const std::vector<ChunkInfo>& chunks,
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
void expect_played(const std::vector<Output>& frames, const std::vector<ChunkInfo>& chunks,
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
std::map<size_t, int64_t> expect_one_anchor_per_segment(
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

// ==============================================================================
// Players of each format in one group
// ==============================================================================

class ServerPlayersByDepth : public testing::TestWithParam<uint32_t> {};

// spec player/v1.md "PCM Encoding Convention", "Codec framing" and "Playback Synchronization": two
// sendspin-cpp players in the server's group, one streaming PCM and one FLAC (listed first), play
// the host's audio through their sync tasks; what reaches each output is the written audio in
// order, every frame bit for bit except the player's own corrections. Both got the same
// timestamps for the same audio, and the FLAC player got chunks of several frames, numbered by
// their cell in the epoch.
TEST_P(ServerPlayersByDepth, PlayTheHostsAudioBitForBitInPcmAndFlac) {
    const Signal signal(GetParam());
    Rig s(signal);
    const auto depth = static_cast<uint8_t>(signal.bits());
    const Player& pcm = s.add({{Codec::PCM, 2, RATE, depth}});
    const Player& flac = s.add({{Codec::FLAC, 2, RATE, depth}, {Codec::PCM, 2, RATE, depth}});

    const std::shared_ptr<GroupInput> input = s.open();
    ASSERT_TRUE(input);
    // 16-bit positions stay below 65,536
    const Segment segment{0, signal.bits() == 16 ? 65000U : 2 * RATE};
    std::thread host([&] {
        feed(*input, signal, segment);
        input->drain();
    });
    const std::string drained = Recorder::drained(input->id(), DrainOutcome::PLAYED);
    s.run_until_drained({drained});
    host.join();

    const std::lock_guard<std::mutex> lock(s.trace.mutex);
    EXPECT_FALSE(s.trace.failed);
    const std::string pcm_id = s.client_id(pcm);
    const std::string flac_id = s.client_id(flac);
    EXPECT_EQ(s.trace.codecs[pcm_id], "pcm");
    EXPECT_EQ(s.trace.codecs[flac_id], "flac");
    for (const Player* player : {&pcm, &flac}) {
        SCOPED_TRACE(player->url());
        expect_played(decode(signal, player->sink().played()), s.trace.chunks[s.client_id(*player)],
                      {segment});
    }
    expect_one_anchor_per_segment(s.trace.chunks, {segment});
    // The same first position carries the same timestamp in both formats
    size_t shared = 0;
    for (const ChunkInfo& chunk : s.trace.chunks[flac_id]) {
        EXPECT_EQ(chunk.number, chunk.position / CELL);
        const std::vector<ChunkInfo>& other = s.trace.chunks[pcm_id];
        const auto same = std::find_if(other.begin(), other.end(), [&](const ChunkInfo& c) {
            return c.position == chunk.position;
        });
        if (same != other.end()) {
            EXPECT_EQ(same->ts, chunk.ts);
            ++shared;
        }
    }
    EXPECT_GT(shared, 0U);
    const std::vector<ChunkInfo>& flac_chunks = s.trace.chunks[flac_id];
    EXPECT_TRUE(std::any_of(flac_chunks.begin(), flac_chunks.end(),
                            [](const ChunkInfo& chunk) { return chunk.frames > 1; }));
}

INSTANTIATE_TEST_SUITE_P(Depths, ServerPlayersByDepth, testing::Values(16U, 24U),
                         [](const testing::TestParamInfo<uint32_t>& info) {
                             return "Bits" + std::to_string(info.param);
                         });

// A FLAC stream's chunks carry n_target frames, whether the player's buffer frees room one
// completed chunk at a time (a 250,000-byte buffer, about a second) or has room past the
// write-ahead (the default buffer), since a chunk short of n_target waits for its frames or room
// while its first frame has the slack. Most chunks past the first 500 ms of the stream's timeline
// hold five 20 ms frames.
TEST(ServerPlayersPacking, FillFlacChunksToTheTarget) {
    struct Row {
        const char* name;
        uint32_t bits;
        size_t buffer_bytes;
        uint32_t frames;
    };
    const Row rows[] = {
        // Four seconds, played in real time: most of the stream goes out after its first 500 ms,
        // one completed chunk's room at a time, so the median is the steady state's
        {"room-bound", 24, 250000, 4 * RATE},
        // Three seconds, played in real time: the 24-bit stream runs past its first 500 ms with
        // room to spare, where a cursor that keeps up with production finds one frame at a time
        {"room past the write-ahead", 24, sendspin::PlayerRoleConfig::DEFAULT_AUDIO_BUFFER_CAPACITY,
         3 * RATE},
    };
    for (const Row& row : rows) {
        SCOPED_TRACE(row.name);
        const Signal signal(row.bits);
        Rig s(signal);
        const auto depth = static_cast<uint8_t>(signal.bits());
        const Player& flac = s.add({{Codec::FLAC, 2, RATE, depth}, {Codec::PCM, 2, RATE, depth}},
                                   RATE, 2, 150, std::nullopt, row.buffer_bytes);
        const std::shared_ptr<GroupInput> input = s.open();
        ASSERT_TRUE(input);
        const Segment segment{0, row.frames};
        std::thread host([&] {
            feed(*input, signal, segment);
            input->drain();
        });
        const std::string drained = Recorder::drained(input->id(), DrainOutcome::PLAYED);
        s.run_until_drained({drained});
        host.join();

        const std::lock_guard<std::mutex> lock(s.trace.mutex);
        EXPECT_FALSE(s.trace.failed);
        const std::vector<ChunkInfo>& chunks = s.trace.chunks[s.client_id(flac)];
        ASSERT_FALSE(chunks.empty());
        expect_played(decode(signal, flac.sink().played()), chunks, {segment});
        std::vector<size_t> frames;
        for (const ChunkInfo& chunk : chunks) {
            if (chunk.ts - chunks.front().ts >= 500000) {
                frames.push_back(chunk.frames);
            }
        }
        ASSERT_FALSE(frames.empty());
        std::sort(frames.begin(), frames.end());
        // n_target, 5 cells
        EXPECT_EQ(frames[frames.size() / 2], 5U);
    }
}

// ==============================================================================
// FLAC frame numbers
// ==============================================================================

// A sendspin-cpp player decodes FLAC chunks of several frames whose numbers jump for a late joiner
// (its first frame continues the group's instance mid-stream) and restart at 0 after a seek (a new
// epoch resets the encoder). Both players play the host's audio bit for bit through the join and
// the seek.
TEST(ServerPlayers, PlayFlacThroughALateJoinAndASeek) {
    const Signal signal(24);
    Rig s(signal);
    const std::vector<sendspin::AudioSupportedFormatObject> formats = {{Codec::FLAC, 2, RATE, 24}};
    const Player& first = s.add(formats);

    const std::shared_ptr<GroupInput> input = s.open();
    ASSERT_TRUE(input);
    // Longer than the test runs before the seek, however slowly
    const Segment before{0, 60 * RATE};
    const Segment after{4000000, 2 * RATE};
    std::atomic<bool> seek{false};
    std::thread host([&] {
        feed(*input, signal, before, &seek);
        while (!seek.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        input->flush();
        feed(*input, signal, after);
        input->drain();
    });
    run_until(s.players, [&] { return first.sink().audible() > RATE / 5; });
    const Player& joiner = s.add(formats);
    run_until(s.players, [&] { return joiner.sink().audible() > RATE / 5; });
    seek = true;
    const std::string drained = Recorder::drained(input->id(), DrainOutcome::PLAYED);
    s.run_until_drained({drained});
    host.join();

    const std::lock_guard<std::mutex> lock(s.trace.mutex);
    EXPECT_FALSE(s.trace.failed);
    const std::vector<Segment> segments = {before, after};
    expect_one_anchor_per_segment(s.trace.chunks, segments);
    for (const Player* player : {&first, &joiner}) {
        SCOPED_TRACE(player->url());
        const std::vector<ChunkInfo>& chunks = s.trace.chunks[s.client_id(*player)];
        expect_played(decode(signal, player->sink().played()), chunks, segments);
        ASSERT_FALSE(chunks.empty());
        // Each epoch numbers its frames from its first cell
        for (const ChunkInfo& chunk : chunks) {
            const uint32_t from = after.holds(chunk.position) ? after.first : before.first;
            EXPECT_EQ(chunk.number, (chunk.position - from) / CELL) << chunk.position;
        }
        EXPECT_TRUE(std::any_of(chunks.begin(), chunks.end(),
                                [](const ChunkInfo& chunk) { return chunk.frames > 1; }));
        EXPECT_TRUE(std::any_of(chunks.begin(), chunks.end(), [&](const ChunkInfo& chunk) {
            return after.holds(chunk.position);
        }));
    }
    // The joiner's first frame continues the instance's numbering
    EXPECT_GT(s.trace.chunks[s.client_id(joiner)].front().number, 0U);
}

// ==============================================================================
// Resampled and mixed streams
// ==============================================================================

constexpr uint32_t RATE_44 = 44100;

/// One signal frame's two 24-bit samples
std::pair<int32_t, int32_t> samples_of(const Signal& signal, uint32_t position) {
    std::array<uint8_t, 6> frame{};
    signal.put(frame.data(), position);
    const auto value = [&](size_t at) {
        return static_cast<int32_t>(frame[at] | (uint32_t{frame[at + 1]} << 8) |
                                    (uint32_t{frame[at + 2]} << 16));
    };
    return {value(0), value(3)};
}

/// A float at 24 bits as the quantize stage writes it: rounded, clipped, little-endian
void put24(uint8_t* out, double sample) {
    const auto value = static_cast<int32_t>(std::clamp<int64_t>(
        std::llround(std::clamp(sample, -2.0, 2.0) * 8388608.0), -8388608, 8388607));
    out[0] = static_cast<uint8_t>(value);
    out[1] = static_cast<uint8_t>(value >> 8);
    out[2] = static_cast<uint8_t>(value >> 16);
}

/// The 24-bit frames micro-resampler gives for input, floats of channels interleaved, run
/// directly as the pipelines configure it: primed, fed, flushed and cut to round(count * out / in)
/// frames
Bytes resampled_frames(const std::vector<float>& input, uint32_t channels, uint32_t in_rate,
                       uint32_t out_rate) {
    const size_t count = input.size() / channels;
    auto resampler = std::make_unique<ReferenceResampler>(in_rate, out_rate, channels, count);
    EXPECT_TRUE(resampler->valid());
    std::vector<float> out((count * out_rate / in_rate + 2) * channels);
    size_t consumed = 0;
    size_t generated = 0;
    EXPECT_TRUE(resampler->prime(input.data(), count));
    EXPECT_TRUE(resampler->process_block(input.data(), count, out.data(), out.size() / channels,
                                         consumed, generated));
    std::vector<float> tail(out.size());
    size_t total = generated;
    while (!resampler->is_flushed()) {
        size_t flushed = 0;
        EXPECT_TRUE(resampler->flush(tail.data(), tail.size() / channels, flushed));
        out.resize(std::max(out.size(), (total + flushed) * channels));
        std::copy(tail.begin(), tail.begin() + static_cast<std::ptrdiff_t>(flushed * channels),
                  out.begin() + static_cast<std::ptrdiff_t>(total * channels));
        total += flushed;
        if (flushed == 0) {
            break;
        }
    }
    const uint64_t frames = ((uint64_t{count} * 2 * out_rate) + in_rate) / (2 * uint64_t{in_rate});
    EXPECT_GE(total, frames);
    Bytes reference(frames * channels * 3);
    for (uint64_t i = 0; i < frames * channels; ++i) {
        put24(&reference[i * 3], out[i]);
    }
    return reference;
}

/// The signal's frames of a segment as the stages' floats: each 24-bit sample over 2^23
std::vector<float> signal_floats(const Signal& signal, const Segment& segment) {
    std::vector<float> input(size_t{segment.count} * 2);
    for (uint32_t i = 0; i < segment.count; ++i) {
        const auto [left, right] = samples_of(signal, segment.first + i);
        input[i * 2] = static_cast<float>(left) / 8388608.0F;
        input[(i * 2) + 1] = static_cast<float>(right) / 8388608.0F;
    }
    return input;
}

/// The 24-bit stereo frames micro-resampler gives for a segment of the 24-bit signal
Bytes resampled_reference(const Signal& signal, const Segment& segment, uint32_t in_rate,
                          uint32_t out_rate) {
    return resampled_frames(signal_floats(signal, segment), 2, in_rate, out_rate);
}

/// The 24-bit mono frames the channel mix gives for a segment of the 24-bit signal: each frame's
/// average, rounded
Bytes mixed_reference(const Signal& signal, const Segment& segment) {
    Bytes reference(size_t{segment.count} * 3);
    for (uint32_t i = 0; i < segment.count; ++i) {
        const auto [left, right] = samples_of(signal, segment.first + i);
        put24(&reference[i * 3], (static_cast<double>(left) + right) / 2.0 / 8388608.0);
    }
    return reference;
}

/// Every chunk is the reference at the place its timestamp gives on the segment's anchor, sample
/// for sample
void expect_samples_match(const std::vector<ChunkInfo>& chunks, const Bytes& reference,
                          size_t frame_bytes, int64_t anchor, uint32_t rate) {
    ASSERT_FALSE(chunks.empty());
    for (const ChunkInfo& chunk : chunks) {
        SCOPED_TRACE(chunk.ts);
        const int64_t offset_us = chunk.ts - anchor;
        ASSERT_GE(offset_us, 0);
        // A cell boundary, a whole sample at the one-cell rates these streams have
        ASSERT_EQ(offset_us % 20000, 0);
        const auto at = static_cast<size_t>(offset_us * rate / 1000000) * frame_bytes;
        ASSERT_LE(at + chunk.pcm.size(), reference.size());
        EXPECT_TRUE(std::equal(chunk.pcm.begin(), chunk.pcm.end(),
                               reference.begin() + static_cast<std::ptrdiff_t>(at)));
    }
}

/// Every chunk of a converted stream is the reference at the place its timestamp gives
/// (expect_samples_match()), and the stream was sent whole (expect_sent_whole())
void expect_chunks_match(const std::vector<ChunkInfo>& chunks, const Bytes& reference,
                         size_t frame_bytes, int64_t anchor, uint32_t rate) {
    expect_sent_whole(chunks);
    expect_samples_match(chunks, reference, frame_bytes, anchor, rate);
}

/// A converted stream's output plays the reference in order from wherever it started, but for
/// sendspin-cpp's corrections (expect_played()): a frame not in the reference, which blends its
/// neighbors, then the reference again at most four frames on; silence; and a re-alignment that
/// skips ahead, taken where two frames in a row continue the reference, since a single frame can
/// match an unrelated one. How much of the end plays is real time (expect_heard()).
void expect_follows(const Bytes& played, const Bytes& reference, size_t frame_bytes) {
    const auto frame_at = [&](const Bytes& bytes, size_t index) {
        return std::span<const uint8_t>(bytes.data() + (index * frame_bytes), frame_bytes);
    };
    const auto silent = [](std::span<const uint8_t> frame) {
        return std::all_of(frame.begin(), frame.end(), [](uint8_t byte) { return byte == 0; });
    };
    const auto same = [](std::span<const uint8_t> a, std::span<const uint8_t> b) {
        return std::equal(a.begin(), a.end(), b.begin(), b.end());
    };
    size_t begin = 0;
    size_t end = played.size() / frame_bytes;
    while (begin < end && silent(frame_at(played, begin))) {
        ++begin;
    }
    while (end > begin && silent(frame_at(played, end - 1))) {
        --end;
    }
    const size_t total = reference.size() / frame_bytes;
    ASSERT_LT(begin + 1, end) << "nothing played";
    // Where played frame i and the one after it continue the reference, at or after from
    const auto realign = [&](size_t i, size_t from) -> std::optional<size_t> {
        for (size_t at = from; at + 1 < total; ++at) {
            if (same(frame_at(reference, at), frame_at(played, i)) &&
                same(frame_at(reference, at + 1), frame_at(played, i + 1))) {
                return at;
            }
        }
        return std::nullopt;
    };
    // The start, where two frames in a row are the reference; a correction or a silence fill at
    // the very start may come before it, as it may anywhere
    std::optional<size_t> start;
    size_t first = begin;
    size_t passed = 0;
    for (; first + 1 < end && passed < 3 && !(start = realign(first, 0)); ++first) {
        passed += silent(frame_at(played, first)) ? 0 : 1;
    }
    ASSERT_TRUE(start) << "the first frames played follow no correction";
    size_t at = *start;
    size_t unmatched = 0;
    for (size_t i = first + 1; i < end; ++i) {
        const std::span<const uint8_t> frame = frame_at(played, i);
        if (silent(frame)) {
            continue;
        }
        size_t step = 1;
        while (step <= 4 && (at + step >= total || !same(frame_at(reference, at + step), frame))) {
            ++step;
        }
        if (step <= 4) {
            unmatched = 0;
            at += step;
            continue;
        }
        if (const std::optional<size_t> ahead = i + 1 < end ? realign(i, at + 1) : std::nullopt) {
            unmatched = 0;
            at = *ahead;
            continue;
        }
        ASSERT_LT(++unmatched, 2U) << "frame " << i << " follows no correction";
    }
}

class ServerPlayersResampled : public testing::TestWithParam<Codec> {};

// A 44.1 kHz input plays to a sendspin-cpp player at 44.1 kHz and one at 48 kHz in one group, as
// PCM or FLAC. The 44.1 kHz player plays the signal bit for bit. Every 48 kHz chunk sits on the
// same anchor, its cells' timestamps identical to the 44.1 kHz chunks' for the same cells, and
// holds micro-resampler's output over the whole input, run directly, at the place its timestamp
// gives; its player plays that output in order.
TEST_P(ServerPlayersResampled, PlayA44100InputAt48000) {
    const Signal signal(24);
    Rig s(signal, RATE_44);
    const Player& native = s.add({{Codec::PCM, 2, RATE_44, 24}}, RATE_44);
    const Player& resampled = s.add({{GetParam(), 2, RATE, 24}});

    const std::shared_ptr<GroupInput> input = s.open();
    ASSERT_TRUE(input);
    const Segment segment{0, 2 * RATE_44};
    std::thread host([&] {
        feed(*input, signal, segment);
        input->drain();
    });
    const std::string drained = Recorder::drained(input->id(), DrainOutcome::PLAYED);
    s.run_until_drained({drained});
    host.join();

    const std::lock_guard<std::mutex> lock(s.trace.mutex);
    EXPECT_FALSE(s.trace.failed);
    const std::string native_id = s.client_id(native);
    const std::string resampled_id = s.client_id(resampled);
    EXPECT_EQ(s.trace.codecs[resampled_id], GetParam() == Codec::FLAC ? "flac" : "pcm");
    const std::vector<ChunkInfo>& native_chunks = s.trace.chunks[native_id];
    expect_played(decode(signal, native.sink().played()), native_chunks, {segment}, RATE_44);
    std::map<size_t, int64_t> anchors =
        expect_one_anchor_per_segment({{native_id, native_chunks}}, {segment}, RATE_44);
    ASSERT_EQ(anchors.size(), 1U);

    const Bytes reference = resampled_reference(signal, segment, RATE_44, RATE);
    const std::vector<ChunkInfo>& resampled_chunks = s.trace.chunks[resampled_id];
    expect_chunks_match(resampled_chunks, reference, 6, anchors[0], RATE);
    // The same cell, the same timestamp
    size_t shared = 0;
    for (const ChunkInfo& chunk : resampled_chunks) {
        shared += static_cast<size_t>(
            std::count_if(native_chunks.begin(), native_chunks.end(),
                          [&](const ChunkInfo& other) { return other.ts == chunk.ts; }));
    }
    EXPECT_GT(shared, 0U);
    expect_follows(resampled.sink().played(), reference, 6);
}

INSTANTIATE_TEST_SUITE_P(Codecs, ServerPlayersResampled, testing::Values(Codec::PCM, Codec::FLAC),
                         [](const testing::TestParamInfo<Codec>& info) {
                             return info.param == Codec::FLAC ? "Flac" : "Pcm";
                         });

// A stereo input plays to a mono sendspin-cpp player, in FLAC, as the average of each frame's
// channels, on the stereo player's timestamps
TEST(ServerPlayersMixed, PlayAStereoInputToAMonoPlayer) {
    const Signal signal(24);
    Rig s(signal);
    const Player& stereo = s.add({{Codec::PCM, 2, RATE, 24}});
    const Player& mono = s.add({{Codec::FLAC, 1, RATE, 24}}, RATE, 1);

    const std::shared_ptr<GroupInput> input = s.open();
    ASSERT_TRUE(input);
    const Segment segment{0, 2 * RATE};
    std::thread host([&] {
        feed(*input, signal, segment);
        input->drain();
    });
    const std::string drained = Recorder::drained(input->id(), DrainOutcome::PLAYED);
    s.run_until_drained({drained});
    host.join();

    const std::lock_guard<std::mutex> lock(s.trace.mutex);
    EXPECT_FALSE(s.trace.failed);
    const std::string stereo_id = s.client_id(stereo);
    const std::string mono_id = s.client_id(mono);
    EXPECT_EQ(s.trace.codecs[mono_id], "flac");
    std::map<size_t, int64_t> anchors =
        expect_one_anchor_per_segment({{stereo_id, s.trace.chunks[stereo_id]}}, {segment});
    ASSERT_EQ(anchors.size(), 1U);
    const Bytes reference = mixed_reference(signal, segment);
    expect_chunks_match(s.trace.chunks[mono_id], reference, 3, anchors[0], RATE);
    expect_follows(mono.sink().played(), reference, 3);
}

// ==============================================================================
// Two-cell frames and input segments
// ==============================================================================

constexpr uint32_t RATE_11 = 11025;

// spec player/v1.md "PCM Encoding Convention", "Codec framing": an 11,025 Hz input plays to a PCM
// and a FLAC sendspin-cpp player at that rate, in frames of two cells (441 samples, 40 ms) on the
// even cells: each plays the host's audio bit for bit, both got the same timestamps for the same
// audio, and the FLAC chunks hold frames numbered by their pair of cells
TEST(ServerPlayersTwoCells, PlayAn11025HzInputBitForBitInPcmAndFlac) {
    const Signal signal(24);
    Rig s(signal, RATE_11);
    const Player& pcm = s.add({{Codec::PCM, 2, RATE_11, 24}}, RATE_11);
    const Player& flac = s.add({{Codec::FLAC, 2, RATE_11, 24}}, RATE_11);

    const std::shared_ptr<GroupInput> input = s.open();
    ASSERT_TRUE(input);
    const Segment segment{0, 3 * RATE_11};
    std::thread host([&] {
        feed(*input, signal, segment);
        input->drain();
    });
    const std::string drained = Recorder::drained(input->id(), DrainOutcome::PLAYED);
    s.run_until_drained({drained});
    host.join();

    const std::lock_guard<std::mutex> lock(s.trace.mutex);
    EXPECT_FALSE(s.trace.failed);
    const std::string pcm_id = s.client_id(pcm);
    const std::string flac_id = s.client_id(flac);
    EXPECT_EQ(s.trace.codecs[pcm_id], "pcm");
    EXPECT_EQ(s.trace.codecs[flac_id], "flac");
    for (const Player* player : {&pcm, &flac}) {
        SCOPED_TRACE(player->url());
        expect_played(decode(signal, player->sink().played()), s.trace.chunks[s.client_id(*player)],
                      {segment}, RATE_11);
    }
    std::map<size_t, int64_t> anchors =
        expect_one_anchor_per_segment(s.trace.chunks, {segment}, RATE_11);
    ASSERT_EQ(anchors.size(), 1U);
    for (const ChunkInfo& chunk : s.trace.chunks[flac_id]) {
        // An even cell, and the frame's number counts pairs of cells
        EXPECT_EQ((chunk.ts - anchors[0]) % 40000, 0) << chunk.ts;
        EXPECT_EQ(chunk.number, chunk.position / 441) << chunk.position;
    }
    const std::vector<ChunkInfo>& flac_chunks = s.trace.chunks[flac_id];
    EXPECT_TRUE(std::any_of(flac_chunks.begin(), flac_chunks.end(),
                            [](const ChunkInfo& chunk) { return chunk.frames > 1; }));
}

/// The 24-bit value feed_channels() writes for position
uint32_t channel_value(uint32_t position) {
    return position & 0x7FFFFFU;
}

/// Writes count 24-bit frames of channels channels from position first on, every channel of each
/// its position's value, waiting for room
void feed_channels(GroupInput& input, uint32_t first, uint32_t count, uint32_t channels = 1) {
    constexpr uint32_t BLOCK = 960;
    Bytes block(size_t{BLOCK} * 3 * channels);
    for (uint32_t written = 0; written < count;) {
        const uint32_t n = std::min(BLOCK, count - written);
        for (uint32_t i = 0; i < n * channels; ++i) {
            const uint32_t value = channel_value(first + written + (i / channels));
            block[i * 3] = static_cast<uint8_t>(value);
            block[(i * 3) + 1] = static_cast<uint8_t>(value >> 8);
            block[(i * 3) + 2] = static_cast<uint8_t>(value >> 16);
        }
        const WriteResult result = input.write(block.data(), n);
        written += static_cast<uint32_t>(result.frames);
        if (result.frames < n) {
            ASSERT_EQ(result.state, InputState::FULL);
            input.wait_writable(std::numeric_limits<int64_t>::max());
        }
    }
}

// Three seconds of a 44.1 kHz stereo input, then set_format() to 48 kHz mono and a second more,
// play through one run to a PCM player at 44.1 kHz stereo and a FLAC player at 48 kHz mono. Neither
// gets a second stream/start: each keeps its format and its stream, and the server sent each one's
// chunks without a gap, on one anchor, the same cell carrying the same timestamp for both. Each
// stream holds, segment by segment, what its pipeline makes of the input: the signal as written,
// its channels averaged and resampled, the mono samples as written, or duplicated into both
// channels and resampled, each resampled segment micro-resampler's output over that segment alone;
// and each player plays it in order across the change.
TEST(ServerPlayersSegments, PlayAFormatChangeWithinARunOnEveryPlayer) {
    const Signal signal(24);
    Rig s(signal, RATE_44, false);
    const Player& stereo = s.add({{Codec::PCM, 2, RATE_44, 24}}, RATE_44);
    const Player& mono = s.add({{Codec::FLAC, 1, RATE, 24}}, RATE, 1);

    const std::shared_ptr<GroupInput> input = s.open();
    ASSERT_TRUE(input);
    // Three seconds end on a cell, so the boundary's output counts are exact at both rates
    const Segment first{0, 3 * RATE_44};
    constexpr uint32_t SECOND = RATE;
    std::thread host([&] {
        feed(*input, signal, first);
        // A failure still drains, so the test ends
        const bool changed = input->set_format({RATE, 1, SampleFormat::S24});
        EXPECT_TRUE(changed);
        if (changed) {
            feed_channels(*input, first.count, SECOND);
        }
        input->drain();
    });
    const std::string drained = Recorder::drained(input->id(), DrainOutcome::PLAYED);
    s.run_until_drained({drained});
    host.join();

    // The mono samples as the stages' floats
    std::vector<float> written(SECOND);
    for (uint32_t i = 0; i < SECOND; ++i) {
        written[i] = static_cast<float>(channel_value(first.count + i)) / 8388608.0F;
    }
    // The stereo player: the signal, then the mono samples in both channels, resampled
    Bytes stereo_reference(size_t{first.count} * 6);
    for (uint32_t i = 0; i < first.count; ++i) {
        signal.put(&stereo_reference[size_t{i} * 6], i);
    }
    std::vector<float> both;
    for (const float sample : written) {
        both.insert(both.end(), 2, sample);
    }
    const Bytes second_stereo = resampled_frames(both, 2, RATE, RATE_44);
    stereo_reference.insert(stereo_reference.end(), second_stereo.begin(), second_stereo.end());
    // The mono player: the signal's channels averaged and resampled, then the mono samples
    std::vector<float> averaged(first.count);
    const std::vector<float> first_floats = signal_floats(signal, first);
    for (uint32_t i = 0; i < first.count; ++i) {
        averaged[i] = static_cast<float>(
            (static_cast<double>(first_floats[i * 2]) + first_floats[(i * 2) + 1]) / 2.0);
    }
    Bytes mono_reference = resampled_frames(averaged, 1, RATE_44, RATE);
    ASSERT_EQ(mono_reference.size(), size_t{3} * RATE * 3);
    for (const float sample : written) {
        const size_t at = mono_reference.size();
        mono_reference.resize(at + 3);
        put24(&mono_reference[at], sample);
    }

    const std::lock_guard<std::mutex> lock(s.trace.mutex);
    EXPECT_FALSE(s.trace.failed);
    const std::string stereo_id = s.client_id(stereo);
    const std::string mono_id = s.client_id(mono);
    EXPECT_EQ(s.trace.streams[stereo_id], 1U);
    EXPECT_EQ(s.trace.streams[mono_id], 1U);
    EXPECT_EQ(s.trace.codecs[mono_id], "flac");
    const std::vector<ChunkInfo>& stereo_chunks = s.trace.chunks[stereo_id];
    const std::vector<ChunkInfo>& mono_chunks = s.trace.chunks[mono_id];
    ASSERT_FALSE(stereo_chunks.empty());
    // The anchor, from the first stereo chunk's position in the signal
    const std::optional<uint32_t> position = signal.get(stereo_chunks.front().pcm.data());
    ASSERT_TRUE(position);
    ASSERT_EQ(*position % 882, 0U);
    const int64_t anchor = stereo_chunks.front().ts - (int64_t{*position} * 1000000 / RATE_44);
    expect_chunks_match(stereo_chunks, stereo_reference, 6, anchor, RATE_44);
    expect_chunks_match(mono_chunks, mono_reference, 3, anchor, RATE);
    // Both streams went on past the change
    const int64_t change = anchor + 3000000;
    EXPECT_GT(stereo_chunks.back().ts, change);
    EXPECT_GT(mono_chunks.back().ts, change);
    expect_follows(stereo.sink().played(), stereo_reference, 6);
    expect_follows(mono.sink().played(), mono_reference, 3);
}

// A set_format() to four channels, which no format of a stereo player is offered for, leaves the
// player its stream and format: one stream/start, and the four-channel segment folded onto its two
// channels, here each the average of two equal channels, so the played audio is the signal and then
// the written values, without a gap. The change comes once the stream has started: a stream not yet
// started is selected for the head's format, for which this player is offered none.
TEST(ServerPlayersSegments, KeepAStreamThroughAnInputItsFormatsAreNotOfferedFor) {
    const Signal signal(24);
    Rig s(signal, RATE_44, false);
    const Player& stereo = s.add({{Codec::PCM, 2, RATE_44, 24}}, RATE_44);
    const std::string stereo_id = s.client_id(stereo);

    const std::shared_ptr<GroupInput> input = s.open();
    ASSERT_TRUE(input);
    const Segment first{0, RATE_44};
    constexpr uint32_t HALF = RATE_44 / 2;
    std::atomic<bool> started{false};
    std::thread host([&] {
        feed(*input, signal, first);
        while (!started.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        const bool changed = input->set_format({RATE_44, 4, SampleFormat::S24});
        EXPECT_TRUE(changed);
        if (changed) {
            feed_channels(*input, first.count, HALF, 4);
        }
        input->drain();
    });
    run_until(s.players, [&] {
        const std::lock_guard<std::mutex> lock(s.trace.mutex);
        return s.trace.streams[stereo_id] > 0;
    });
    started = true;
    const std::string drained = Recorder::drained(input->id(), DrainOutcome::PLAYED);
    s.run_until_drained({drained});
    host.join();

    Bytes reference(size_t{first.count + HALF} * 6);
    for (uint32_t i = 0; i < first.count; ++i) {
        signal.put(&reference[size_t{i} * 6], i);
    }
    for (uint32_t i = 0; i < HALF; ++i) {
        const uint32_t value = channel_value(first.count + i);
        for (size_t byte = 0; byte < 6; ++byte) {
            reference[(size_t{first.count + i} * 6) + byte] =
                static_cast<uint8_t>(value >> (8 * (byte % 3)));
        }
    }

    const std::lock_guard<std::mutex> lock(s.trace.mutex);
    EXPECT_FALSE(s.trace.failed);
    EXPECT_EQ(s.trace.streams[stereo_id], 1U);
    const std::vector<ChunkInfo>& chunks = s.trace.chunks[stereo_id];
    ASSERT_FALSE(chunks.empty());
    const std::optional<uint32_t> position = signal.get(chunks.front().pcm.data());
    ASSERT_TRUE(position);
    const int64_t anchor = chunks.front().ts - (int64_t{*position} * 1000000 / RATE_44);
    expect_chunks_match(chunks, reference, 6, anchor, RATE_44);
    EXPECT_GT(chunks.back().ts, anchor + 1000000);
    expect_follows(stereo.sink().played(), reference, 6);
}

// A 44.1 kHz stereo input set_format() to 48 kHz mono at 1.0123 s, a boundary on no cell. A player
// wanting 44.1 kHz that joins once the head is at 48 kHz, while the 44.1 kHz segment still plays,
// has an instance at the playing segment's rate: it joins that epoch at once, with no resampler
// before the boundary, and gets the signal as written, then the mono samples in both channels
// resampled. The 48 kHz player that played from the start gets the signal resampled and cut at the
// boundary's output index, then the mono samples in both channels.
TEST(ServerPlayersSegments, StartAJoinerAtThePlayingSegmentsRate) {
    const Signal signal(24);
    Rig s(signal, RATE_44, false);
    const Player& first = s.add({{Codec::PCM, 2, RATE, 24}}, RATE);

    const std::shared_ptr<GroupInput> input = s.open();
    ASSERT_TRUE(input);
    const Segment before{0, 44643};
    constexpr uint32_t SECOND = RATE;
    std::atomic<bool> changed{false};
    std::atomic<bool> joined{false};
    std::thread host([&] {
        feed(*input, signal, before);
        const bool accepted = input->set_format({RATE, 1, SampleFormat::S24});
        EXPECT_TRUE(accepted);
        changed.store(true);
        if (accepted) {
            // Held until the joiner is in, so the 44.1 kHz segment plays while it joins
            while (!joined.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            feed_channels(*input, before.count, SECOND);
        }
        input->drain();
    });
    run_until(s.players, [&] { return changed.load(); });
    const Player& joiner = s.add({{Codec::PCM, 2, RATE_44, 24}}, RATE_44);
    joined.store(true);
    const std::string drained = Recorder::drained(input->id(), DrainOutcome::PLAYED);
    s.run_until_drained({drained});
    host.join();

    std::vector<float> both;
    for (uint32_t i = 0; i < SECOND; ++i) {
        both.insert(both.end(), 2,
                    static_cast<float>(channel_value(before.count + i)) / 8388608.0F);
    }
    // The joiner: the signal as written, then the mono samples resampled from the boundary, at
    // output index 44,643 exactly
    Bytes joiner_reference(size_t{before.count} * 6);
    for (uint32_t i = 0; i < before.count; ++i) {
        signal.put(&joiner_reference[size_t{i} * 6], i);
    }
    const Bytes after_44 = resampled_frames(both, 2, RATE, RATE_44);
    joiner_reference.insert(joiner_reference.end(), after_44.begin(), after_44.end());
    // The first player: the signal resampled to round(44,643 * 48,000 / 44,100) = 48,591 frames,
    // then the mono samples in both channels
    Bytes first_reference = resampled_reference(signal, before, RATE_44, RATE);
    ASSERT_EQ(first_reference.size(), size_t{48591} * 6);
    for (uint32_t i = 0; i < SECOND; ++i) {
        const uint32_t value = channel_value(before.count + i);
        for (size_t byte = 0; byte < 6; ++byte) {
            first_reference.push_back(static_cast<uint8_t>(value >> (8 * (byte % 3))));
        }
    }

    const std::lock_guard<std::mutex> lock(s.trace.mutex);
    EXPECT_FALSE(s.trace.failed);
    const std::string first_id = s.client_id(first);
    const std::string joiner_id = s.client_id(joiner);
    EXPECT_EQ(s.trace.streams[first_id], 1U);
    EXPECT_EQ(s.trace.streams[joiner_id], 1U);
    const std::vector<ChunkInfo>& joiner_chunks = s.trace.chunks[joiner_id];
    ASSERT_FALSE(joiner_chunks.empty());
    // The joiner's first chunk is the signal: it joined while the 44.1 kHz segment played
    const std::optional<uint32_t> position = signal.get(joiner_chunks.front().pcm.data());
    ASSERT_TRUE(position);
    ASSERT_EQ(*position % 882, 0U);
    const int64_t anchor = joiner_chunks.front().ts - (int64_t{*position} * 1000000 / RATE_44);
    expect_chunks_match(joiner_chunks, joiner_reference, 6, anchor, RATE_44);
    expect_chunks_match(s.trace.chunks[first_id], first_reference, 6, anchor, RATE);
    expect_follows(first.sink().played(), first_reference, 6);
    EXPECT_GT(joiner.sink().audible(), 0U);
}

/// A 44.1 kHz stereo input playing to a 44.1 kHz player, 2.5 s or more (on a cell) then a
/// set_format() to 48 kHz and a second of it, and a player wanting 48 kHz that joins while the 44.1
/// kHz segment plays, before the set_format() or after it. Joining before it, the first segment
/// goes on, a whole number of 100 ms steps, until the joiner has been sent some of it, however long
/// that takes. Joining after it, the join must land while the staged first segment still plays,
/// which no write can extend: that segment is 5 s, and the first player's 3 MB buffer has the
/// server admit all of it at once, so the join has about 4.5 s on any runner. Played in real time,
/// about four seconds, or six and a half staged.
void play_a_joiner_through_a_segment_change(bool staged_first) {
    const Signal signal(24);
    Rig s(signal, RATE_44, false);
    const Player& first =
        s.add({{Codec::PCM, 2, RATE_44, 24}}, RATE_44, 2, 150, std::nullopt,
              staged_first ? 3000000 : sendspin::PlayerRoleConfig::DEFAULT_AUDIO_BUFFER_CAPACITY);

    const std::shared_ptr<GroupInput> input = s.open();
    ASSERT_TRUE(input);
    // Five cells, a whole number of samples at both rates
    constexpr uint32_t STEP = RATE_44 / 10;
    const uint32_t least = staged_first ? 220500 : 110250;
    uint32_t first_track = 0;
    std::atomic<bool> next_track{staged_first};
    std::atomic<bool> staged{false};
    std::thread host([&] {
        while (first_track < least || !next_track.load()) {
            feed(*input, signal, Segment{first_track, STEP});
            first_track += STEP;
        }
        const bool changed = input->set_format({RATE, 2, SampleFormat::S24});
        EXPECT_TRUE(changed);
        staged = true;
        if (changed) {
            feed(*input, signal, Segment{first_track, RATE});
        }
        input->drain();
    });
    run_until(s.players, [&] { return first.sink().audible() > RATE_44 / 5; });
    if (staged_first) {
        run_until(s.players, [&] { return staged.load(); });
    }
    const Player& joiner = s.add({{Codec::PCM, 2, RATE, 24}});
    // It is sent the first track before the second is staged
    run_until(s.players, [&] { return s.has_chunks(joiner); });
    next_track = true;
    const std::string drained = Recorder::drained(input->id(), DrainOutcome::PLAYED);
    s.run_until_drained({drained});
    host.join();
    const Segment before{0, first_track};
    const Segment after{first_track, RATE};

    // The first track resampled, cut at the boundary's output index (exactly 120,000 frames for
    // 2.5 s), then the second as written
    Bytes reference = resampled_reference(signal, before, RATE_44, RATE);
    ASSERT_EQ(reference.size(), size_t{before.count} * RATE / RATE_44 * 6);
    for (uint32_t i = 0; i < after.count; ++i) {
        reference.resize(reference.size() + 6);
        signal.put(&reference[reference.size() - 6], after.first + i);
    }
    const std::lock_guard<std::mutex> lock(s.trace.mutex);
    EXPECT_FALSE(s.trace.failed);
    const std::string first_id = s.client_id(first);
    const std::string joiner_id = s.client_id(joiner);
    EXPECT_EQ(s.trace.streams[first_id], 1U);
    EXPECT_EQ(s.trace.streams[joiner_id], 1U);
    const std::vector<ChunkInfo>& first_chunks = s.trace.chunks[first_id];
    ASSERT_FALSE(first_chunks.empty());
    const std::optional<uint32_t> position = signal.get(first_chunks.front().pcm.data());
    ASSERT_TRUE(position);
    const int64_t anchor = first_chunks.front().ts - (int64_t{*position} * 1000000 / RATE_44);
    const int64_t boundary = anchor + (int64_t{before.count} * 1000000 / RATE_44);
    const std::vector<ChunkInfo>& joiner_chunks = s.trace.chunks[joiner_id];
    ASSERT_FALSE(joiner_chunks.empty());
    // It joined while the first track played, and its stream went on into the second
    EXPECT_GT(joiner_chunks.front().ts, anchor);
    EXPECT_LT(joiner_chunks.front().ts, boundary);
    EXPECT_GE(joiner_chunks.back().ts, boundary);
    expect_chunks_match(joiner_chunks, reference, 6, anchor, RATE);
    expect_follows(joiner.sink().played(), reference, 6);
}

// A player wanting 48 kHz that joins while a 44.1 kHz input plays starts at once, its instance
// catching up through the resampler from the staging ring's history, and when the host
// set_format()s to 48 kHz for the next track of a gapless queue its stream goes on into it with no
// second stream/start: micro-resampler's output over the first track, cut at the boundary, then the
// second track as written, on the first player's timeline, and it plays that in order
TEST(ServerPlayersLateJoin, PlayAJoinerThroughTheNextSegmentAtItsRate) {
    play_a_joiner_through_a_segment_change(false);
}

// As above, the joiner joining once the 48 kHz segment is staged and the 44.1 kHz one still plays:
// selected for the head's format, it starts at once all the same, through the resampler
TEST(ServerPlayersLateJoin, PlayAJoinerThroughAStagedSegmentAtItsRate) {
    play_a_joiner_through_a_segment_change(true);
}

// A player needing 48 kHz that joins a playing 44.1 kHz input gets an instance of its own at once,
// which catches up from the staging ring's history: its first chunk comes within its startup bound
// of its stream/start (expect_sent_whole()), every chunk is micro-resampler's output over the whole
// input, bit for bit, at the place its timestamp gives on the first player's anchor, the same cell
// carrying the same timestamp for both, and it plays that output in order. Control: a joiner at the
// input's rate joins the same epoch on an instance already running.
TEST(ServerPlayersLateJoin, CatchUpAJoinerNeedingTheResampler) {
    const Signal signal(24);
    Rig s(signal, RATE_44);
    const Player& first = s.add({{Codec::PCM, 2, RATE_44, 24}}, RATE_44);

    const std::shared_ptr<GroupInput> input = s.open();
    ASSERT_TRUE(input);
    const Segment segment{0, 4 * RATE_44};
    std::thread host([&] {
        feed(*input, signal, segment);
        input->drain();
    });
    run_until(s.players, [&] { return first.sink().audible() > RATE_44 / 5; });
    const Player& resampled = s.add({{Codec::PCM, 2, RATE, 24}});
    const Player& same = s.add({{Codec::PCM, 2, RATE_44, 24}}, RATE_44);
    const std::string drained = Recorder::drained(input->id(), DrainOutcome::PLAYED);
    s.run_until_drained({drained});
    host.join();

    const std::lock_guard<std::mutex> lock(s.trace.mutex);
    EXPECT_FALSE(s.trace.failed);
    const std::string first_id = s.client_id(first);
    const std::string same_id = s.client_id(same);
    const std::string resampled_id = s.client_id(resampled);
    for (const Player* player : {&first, &same}) {
        SCOPED_TRACE(player->url());
        expect_played(decode(signal, player->sink().played()), s.trace.chunks[s.client_id(*player)],
                      {segment}, RATE_44);
    }
    std::map<size_t, int64_t> anchors = expect_one_anchor_per_segment(
        {{first_id, s.trace.chunks[first_id]}, {same_id, s.trace.chunks[same_id]}}, {segment},
        RATE_44);
    ASSERT_EQ(anchors.size(), 1U);
    EXPECT_GT(s.trace.chunks[same_id].front().position, 0U);

    EXPECT_EQ(s.trace.streams[resampled_id], 1U);
    EXPECT_EQ(s.trace.codecs[resampled_id], "pcm");
    const std::vector<ChunkInfo>& resampled_chunks = s.trace.chunks[resampled_id];
    ASSERT_FALSE(resampled_chunks.empty());
    // Joined mid-run
    EXPECT_GT(resampled_chunks.front().ts, anchors[0]);
    const Bytes reference = resampled_reference(signal, segment, RATE_44, RATE);
    expect_chunks_match(resampled_chunks, reference, 6, anchors[0], RATE);
    const std::vector<ChunkInfo>& first_chunks = s.trace.chunks[first_id];
    EXPECT_TRUE(
        std::any_of(resampled_chunks.begin(), resampled_chunks.end(), [&](const ChunkInfo& chunk) {
            return std::any_of(first_chunks.begin(), first_chunks.end(),
                               [&](const ChunkInfo& other) { return other.ts == chunk.ts; });
        }));
    expect_follows(resampled.sink().played(), reference, 6);
}

// ==============================================================================
// A format switch for one player
// ==============================================================================

/// The buffer of a player whose stream switches, small enough that its cursor runs about a
/// second ahead, so the switch lands mid-track
constexpr size_t SWITCHING_BUFFER_BYTES = 450000;

/// The index of the first chunk after the one switch in place in chunks, where is_new turns true
/// and stays true; 0 when there is none
size_t switch_index(const std::vector<ChunkInfo>& chunks,
                    const std::function<bool(const ChunkInfo&)>& is_new) {
    const auto at = std::find_if(chunks.begin(), chunks.end(), is_new);
    EXPECT_TRUE(std::all_of(at, chunks.end(), is_new)) << "switched back";
    return static_cast<size_t>(at - chunks.begin());
}

// spec player/v1.md "stream/start player object", "Player Buffer Accounting": the host sets FLAC
// (set_player_format()) for a player streaming PCM that lists FLAC too, mid-track. Its stream
// switches in place: one stream/start in the new format with no stream/end before it, and its
// chunks go on without a gap or a repeat, PCM up to a frame boundary and FLAC from it, on the
// anchor of a player that streams PCM throughout; it plays the host's audio bit for bit across the
// change.
TEST(ServerPlayersSwitch, SwitchAPlayerFromPcmToFlacInPlace) {
    const Signal signal(24);
    Rig s(signal);
    s.add({{Codec::PCM, 2, RATE, 24}});
    const Player& switching = s.add({{Codec::PCM, 2, RATE, 24}, {Codec::FLAC, 2, RATE, 24}}, RATE,
                                    2, 150, std::nullopt, SWITCHING_BUFFER_BYTES);
    const std::string id = s.client_id(switching);

    const std::shared_ptr<GroupInput> input = s.open();
    ASSERT_TRUE(input);
    const Segment segment{0, 4 * RATE};
    std::thread host([&] {
        feed(*input, signal, segment);
        input->drain();
    });
    run_until(s.players, [&] { return s.has_chunks(switching); });
    EXPECT_TRUE(s.server.set_player_format(id, StreamFormat{StreamCodec::FLAC, RATE, 2, 24}));
    const std::string drained = Recorder::drained(input->id(), DrainOutcome::PLAYED);
    s.run_until_drained({drained});
    host.join();

    const std::lock_guard<std::mutex> lock(s.trace.mutex);
    EXPECT_FALSE(s.trace.failed);
    EXPECT_EQ(s.trace.streams[id], 1U);
    EXPECT_EQ(s.trace.codecs[id], "flac");
    const std::vector<ChunkInfo>& chunks = s.trace.chunks[id];
    const size_t at = switch_index(chunks, [](const ChunkInfo& chunk) { return chunk.flac; });
    EXPECT_GT(at, 0U);
    EXPECT_LT(at, chunks.size());
    expect_played(decode(signal, switching.sink().played()), chunks, {segment});
    EXPECT_EQ(expect_one_anchor_per_segment(s.trace.chunks, {segment}).size(), 1U);
}

// A player streaming a 44.1 kHz input at 44.1 kHz, which lists 48 kHz too, is set to 48 kHz
// mid-track (set_player_format()). Its stream switches in place to an instance that catches up
// through the resampler: no gap at the switch, the 44.1 kHz chunks the signal as written on the
// anchor of a player at 44.1 kHz throughout, and the 48 kHz chunks micro-resampler's output over
// the whole input, bit for bit, at the place their timestamps give. Its sink plays at 44.1 kHz, so
// what it plays is not checked.
TEST(ServerPlayersSwitch, SwitchAPlayerInPlaceToAResampledRate) {
    const Signal signal(24);
    Rig s(signal, RATE_44);
    const Player& other = s.add({{Codec::PCM, 2, RATE_44, 24}}, RATE_44);
    const Player& switching = s.add({{Codec::PCM, 2, RATE_44, 24}, {Codec::PCM, 2, RATE, 24}},
                                    RATE_44, 2, 150, std::nullopt, SWITCHING_BUFFER_BYTES);
    const std::string id = s.client_id(switching);

    const std::shared_ptr<GroupInput> input = s.open();
    ASSERT_TRUE(input);
    const Segment segment{0, 4 * RATE_44};
    std::thread host([&] {
        feed(*input, signal, segment);
        input->drain();
    });
    run_until(s.players, [&] { return s.has_chunks(switching); });
    EXPECT_TRUE(s.server.set_player_format(id, StreamFormat{StreamCodec::PCM, RATE, 2, 24}));
    const std::string drained = Recorder::drained(input->id(), DrainOutcome::PLAYED);
    s.run_until_drained({drained});
    host.join();

    const std::lock_guard<std::mutex> lock(s.trace.mutex);
    EXPECT_FALSE(s.trace.failed);
    EXPECT_EQ(s.trace.streams[id], 1U);
    const std::vector<ChunkInfo>& chunks = s.trace.chunks[id];
    const size_t at =
        switch_index(chunks, [](const ChunkInfo& chunk) { return chunk.rate == RATE; });
    EXPECT_GT(at, 0U);
    ASSERT_LT(at, chunks.size());
    expect_sent_whole(chunks);
    const std::string other_id = s.client_id(other);
    std::map<size_t, int64_t> anchors = expect_one_anchor_per_segment(
        {{other_id, s.trace.chunks[other_id]},
         {id, std::vector<ChunkInfo>(chunks.begin(),
                                     chunks.begin() + static_cast<std::ptrdiff_t>(at))}},
        {segment}, RATE_44);
    ASSERT_EQ(anchors.size(), 1U);
    expect_samples_match(
        std::vector<ChunkInfo>(chunks.begin() + static_cast<std::ptrdiff_t>(at), chunks.end()),
        resampled_reference(signal, segment, RATE_44, RATE), 6, anchors[0], RATE);
}

// ==============================================================================
// Groups
// ==============================================================================

/// Whether groups() lists client_id in group
bool listed_in(const SendspinServer& server, GroupId group, const std::string& client_id) {
    const std::vector<GroupInfo> groups = server.groups();
    return std::any_of(groups.begin(), groups.end(), [&](const GroupInfo& info) {
        return info.id == group && std::find(info.client_ids.begin(), info.client_ids.end(),
                                             client_id) != info.client_ids.end();
    });
}

/// Runs the players until the server has sent player a chunk of its stream-th stream, then until
/// it has played half a second more, which the audio of its group before, cleared by its
/// stream/end, cannot fill
void hear_stream(Rig& s, const Player& player, const std::string& id, size_t stream) {
    run_until(s.players, [&] {
        const std::lock_guard<std::mutex> lock(s.trace.mutex);
        const std::vector<ChunkInfo>& chunks = s.trace.chunks[id];
        return !chunks.empty() && chunks.back().stream == stream;
    });
    const size_t heard = player.sink().audible();
    run_until(s.players, [&] { return player.sink().audible() > heard + (RATE / 2); });
}

// spec messaging.md "group/update": two groups play their own inputs at once to sendspin-cpp
// players. A player the host moves from one to the other mid-track gets a stream/end there and a
// join in the other, on its timeline: each of its chunks carries the timestamp the other group's
// player gets for the same audio, and it plays nothing of its first group until moved back, when it
// joins that group again on its timeline. Each player plays its groups' audio in order and nothing
// else. Both inputs play, in real time, until the player is back in its first group, so that both
// moves land mid-track however long they take.
TEST(ServerPlayersGroups, PlayTwoGroupsAndMoveAPlayerBetweenThem) {
    const Signal signal(24);
    Rig s(signal);
    const std::vector<sendspin::AudioSupportedFormatObject> pcm = {{Codec::PCM, 2, RATE, 24}};
    const Player& living = s.add(pcm);
    const Player& moved = s.add({{Codec::FLAC, 2, RATE, 24}});
    const Player& kitchen_player = s.add(pcm);
    // Listed in clients() once the server took each one's client/state, just after tracing it
    run_until(s.players, [&] {
        const std::vector<sendspin::server::ClientInfo> clients = s.server.clients();
        return std::count_if(clients.begin(), clients.end(),
                             [](const auto& client) { return client.connected; }) == 3;
    });
    const std::string living_id = s.client_id(living);
    const std::string moved_id = s.client_id(moved);
    const std::string kitchen_id = s.client_id(kitchen_player);
    const std::optional<GroupId> kitchen = s.server.create_group("Kitchen", kitchen_id);
    ASSERT_TRUE(kitchen);
    run_until(s.players, [&] { return listed_in(s.server, *kitchen, kitchen_id); });

    const std::shared_ptr<GroupInput> living_input = s.open();
    const std::shared_ptr<GroupInput> kitchen_input = s.open(*kitchen);
    ASSERT_TRUE(living_input && kitchen_input);
    // Longer than the test runs; each input's segment is what its host wrote until then
    const Segment longest_a{0, 60 * RATE};
    const Segment longest_b{4000000, 60 * RATE};
    std::atomic<bool> back{false};
    uint32_t fed_a = 0;
    uint32_t fed_b = 0;
    std::thread host_a([&] {
        feed(*living_input, signal, longest_a, &back, &fed_a);
        living_input->drain();
    });
    std::thread host_b([&] {
        feed(*kitchen_input, signal, longest_b, &back, &fed_b);
        kitchen_input->drain();
    });
    run_until(s.players, [&] {
        return std::all_of(s.players.begin(), s.players.end(),
                           [](const auto& player) { return player->sink().audible() > RATE / 5; });
    });
    ASSERT_TRUE(s.server.move_client(moved_id, *kitchen));
    hear_stream(s, moved, moved_id, 2);
    ASSERT_TRUE(s.server.move_client(moved_id, *s.server.default_group()));
    hear_stream(s, moved, moved_id, 3);
    back = true;
    const std::string drained_a = Recorder::drained(living_input->id(), DrainOutcome::PLAYED);
    const std::string drained_b = Recorder::drained(kitchen_input->id(), DrainOutcome::PLAYED);
    s.run_until_drained({drained_a, drained_b});
    host_a.join();
    host_b.join();
    const Segment a{longest_a.first, fed_a};
    const Segment b{longest_b.first, fed_b};

    const std::lock_guard<std::mutex> lock(s.trace.mutex);
    EXPECT_FALSE(s.trace.failed);
    EXPECT_EQ(s.trace.streams[moved_id], 3U);
    const std::vector<ChunkInfo>& moved_chunks = s.trace.chunks[moved_id];
    // Its streams carry the living room's audio, the kitchen's, then the living room's
    std::vector<ChunkInfo> moved_a;
    std::vector<ChunkInfo> moved_b;
    std::optional<uint32_t> back_at;
    for (const ChunkInfo& chunk : moved_chunks) {
        EXPECT_TRUE((chunk.stream == 2 ? b : a).holds(chunk.position))
            << chunk.stream << " " << chunk.position;
        (chunk.stream == 2 ? moved_b : moved_a).push_back(chunk);
        if (chunk.stream == 3 && !back_at) {
            back_at = chunk.position;
        }
    }
    ASSERT_TRUE(back_at);
    ASSERT_FALSE(moved_b.empty());
    expect_one_anchor_per_segment({{living_id, s.trace.chunks[living_id]}, {moved_id, moved_a}},
                                  {a});
    expect_one_anchor_per_segment({{kitchen_id, s.trace.chunks[kitchen_id]}, {moved_id, moved_b}},
                                  {b});
    expect_played(decode(signal, living.sink().played()), s.trace.chunks[living_id], {a});
    expect_played(decode(signal, kitchen_player.sink().played()), s.trace.chunks[kitchen_id], {b});
    const Segment before{a.first, *back_at - a.first};
    const Segment after{*back_at, a.first + a.count - *back_at};
    expect_played(decode(signal, moved.sink().played()), moved_chunks, {before, b, after});
}

// ==============================================================================
// Live inputs
// ==============================================================================

/// The live tests' blocks: 10 ms, as a capture device delivers them
constexpr uint32_t LIVE_BLOCK = RATE / 100;
/// `L_p` of these players' pipelines: one 20 ms frame and the default write jitter `J_in`
constexpr int64_t LIVE_LATENCY_US = 20000 + sendspin::server::DEFAULT_WRITE_JITTER_US;
/// `G` of a live group of players that ask for 150 ms: `max(J_min, 150 ms) + L_p + M_pump`
constexpr int64_t LIVE_LEAD_US = 150000 + LIVE_LATENCY_US + 10000;
/// The lead a live rebase restores: `G_eff - L_p + J_in`, for group lead lead_us
constexpr int64_t on_time_lead(int64_t lead_us) {
    return lead_us - LIVE_LATENCY_US + sendspin::server::DEFAULT_WRITE_JITTER_US;
}
/// `W_live`
constexpr int64_t LIVE_AHEAD_US = 200000;

/// Writes segment as a capture device delivers it: a 10 ms block at a time, at real time's pace
/// from the first write on, each block taken whole. A block written more than two blocks late (a
/// stall, or a thread held up on a loaded machine) restarts the pace from there, so the input falls
/// that far behind rather than catching up at once. Waits for the server to attach the input first.
/// @param before called with the samples written so far before each block; it may stall, pause or
/// resume
/// @param stop ends the writes early once set
/// @param fed set to the samples written so far
void feed_live(GroupInput& input, const Signal& signal, const Segment& segment,
               const std::function<void(uint32_t)>& before = {},
               const std::atomic<bool>* stop = nullptr, uint32_t* fed = nullptr) {
    using Clock = std::chrono::steady_clock;
    constexpr auto BLOCK_TIME = std::chrono::milliseconds(10);
    ASSERT_EQ(input.wait_writable(std::numeric_limits<int64_t>::max()), InputState::OPEN);
    Bytes block(LIVE_BLOCK * signal.frame_bytes());
    Clock::time_point due = Clock::now();
    for (uint32_t written = 0; written < segment.count && (stop == nullptr || !stop->load());) {
        if (before) {
            before(written);
        }
        std::this_thread::sleep_until(due);
        const uint32_t n = std::min(LIVE_BLOCK, segment.count - written);
        for (uint32_t i = 0; i < n; ++i) {
            signal.put(&block[i * signal.frame_bytes()], segment.first + written + i);
        }
        const WriteResult result = input.write(block.data(), n);
        // A live writer on time is never cut short, paused or not
        EXPECT_EQ(result.frames, n) << static_cast<int>(result.state);
        written += n;
        if (fed != nullptr) {
            *fed = written;
        }
        due += BLOCK_TIME;
        if (Clock::now() - due > 2 * BLOCK_TIME) {
            due = Clock::now();
        }
    }
}

/// A chunk whose timestamp lies past where the chunk before it ended, and by how much
struct Gap {
    size_t chunk{0};
    int64_t us{0};
};

/// What the server sent one client of a live input: every chunk within the live horizon from its
/// stamp, `G + W_live` (a later send only shortens it); the first after each stream/start or
/// stream/clear at least floor_us after that trigger; and every other one carrying the input from
/// where the chunk before it ended, at that chunk's end or later on the timeline: a live rebase
/// moves the timeline forward and keeps the audio. Audio is missing only where it was late when the
/// pump ran. How far ahead of its time a chunk left is not checked (expect_sent_whole()).
/// @param lead_us the group lead, for the horizon
/// @return where the timeline moved forward
std::vector<Gap> expect_sent_live(const std::vector<ChunkInfo>& chunks, int64_t floor_us,
                                  int64_t lead_us) {
    std::vector<Gap> gaps;
    for (size_t i = 0; i < chunks.size(); ++i) {
        const ChunkInfo& chunk = chunks[i];
        SCOPED_TRACE(chunk.ts);
        EXPECT_LE(chunk.ts - chunk.stamp_us, lead_us + LIVE_AHEAD_US) << "past the live horizon";
        const bool first =
            i == 0 || chunks[i - 1].stream != chunk.stream || chunks[i - 1].clears != chunk.clears;
        if (first) {
            EXPECT_GE(chunk.ts - chunk.trigger_us, floor_us);
            continue;
        }
        const ChunkInfo& before = chunks[i - 1];
        const int64_t duration_us = int64_t{before.samples} * 1000000;
        EXPECT_EQ(duration_us % before.rate, 0) << "a chunk of part of a cell before another";
        const int64_t ended = before.ts + (duration_us / before.rate);
        if (chunk.position != before.position + before.samples) {
            // The pump skips a frame already late when it runs; a shift at the chunk after it still
            // shows as a gap past where the skipped audio would have played
            EXPECT_GT(chunk.position, before.position + before.samples) << "audio sent again";
            const int64_t due =
                before.ts + (int64_t{chunk.position - before.position} * 1000000 / before.rate);
            // Its last frame late when the chunk was sent
            EXPECT_LE(due - frame_us(before.rate), chunk.stamp_us) << "audio skipped";
            if (chunk.ts > due) {
                gaps.push_back({i, chunk.ts - due});
            }
            continue;
        }
        EXPECT_GE(chunk.ts, ended) << "the timeline ran back";
        if (chunk.ts > ended) {
            gaps.push_back({i, chunk.ts - ended});
        }
    }
    return gaps;
}

/// The time each 48 kHz cell a chunk carries starts at: a chunk plays its frames back to back
void add_cell_times(const ChunkInfo& chunk, std::map<uint32_t, int64_t>& times) {
    for (uint32_t at = 0; at < chunk.samples; at += CELL) {
        times.emplace(chunk.position + at, chunk.ts + (int64_t{at} * 1000000 / RATE));
    }
}

/// The cells two 48 kHz players both got carry the same timestamp, shifts included, however their
/// chunks group them
/// @return how many cells they shared
size_t expect_same_timestamps(const std::vector<ChunkInfo>& one,
                              const std::vector<ChunkInfo>& other) {
    std::map<uint32_t, int64_t> times;
    for (const ChunkInfo& chunk : one) {
        add_cell_times(chunk, times);
    }
    std::map<uint32_t, int64_t> other_times;
    for (const ChunkInfo& chunk : other) {
        add_cell_times(chunk, other_times);
    }
    size_t shared = 0;
    for (const auto& [position, ts] : other_times) {
        const auto it = times.find(position);
        if (it != times.end()) {
            EXPECT_EQ(it->second, ts) << position;
            ++shared;
        }
    }
    return shared;
}

// A player joins a live group asking for 400 ms (min_buffer_ms; its required lead of 2 s does not
// count for a live input), which raises the group lead from 200 to 450 ms. The next frame shifts
// the timeline by the deficit, so the player already streaming hears one gap of about 250 ms and no
// audio is lost; the frame at the gap leaves with at most the new on-time lead, and the joiner
// starts within a second of its trigger and plays on in sync. The input, written in real time, goes
// on until each stream has carried half a second of it, so the join lands mid-stream however
// slowly the writes come.
TEST(ServerPlayersLive, RaiseTheGroupLeadForAJoinerWithOneShift) {
    constexpr int64_t RAISED_US = 400000 + LIVE_LATENCY_US + 10000;
    const Signal signal(24);
    Rig s(signal);
    const std::vector<sendspin::AudioSupportedFormatObject> formats = {{Codec::PCM, 2, RATE, 24}};
    const Player& first = s.add(formats);
    const std::shared_ptr<GroupInput> input = s.open_live();
    ASSERT_TRUE(input);
    // Longer than the test runs; the segment is what the host wrote until then
    const Segment longest{0, 60 * RATE};
    std::atomic<bool> stop{false};
    uint32_t fed = 0;
    std::thread host([&] {
        feed_live(*input, signal, longest, {}, &stop, &fed);
        input->drain();
    });
    // Whether the server has sent player chunks reaching half a second past its first
    const auto carried_half_a_second = [&](const Player& player) {
        const std::string id = s.client_id(player);
        const std::lock_guard<std::mutex> lock(s.trace.mutex);
        const std::vector<ChunkInfo>& chunks = s.trace.chunks[id];
        return !chunks.empty() && chunks.back().position - chunks.front().position >= RATE / 2;
    };
    run_until(s.players, [&] { return carried_half_a_second(first); });
    const int64_t joined_us = s.server.now_us();
    const Player& joiner = s.add(formats, RATE, 2, 400, 2000);
    run_until(s.players, [&] { return carried_half_a_second(joiner); });
    stop = true;
    const std::string drained = Recorder::drained(input->id(), DrainOutcome::PLAYED);
    s.run_until_drained({drained});
    host.join();
    const Segment segment{longest.first, fed};

    const std::lock_guard<std::mutex> lock(s.trace.mutex);
    EXPECT_FALSE(s.trace.failed);
    const std::vector<ChunkInfo>& first_chunks = s.trace.chunks[s.client_id(first)];
    const std::vector<ChunkInfo>& joiner_chunks = s.trace.chunks[s.client_id(joiner)];
    const std::vector<Gap> gaps = expect_sent_live(first_chunks, START_FLOOR_US, RAISED_US);
    expect_sent_live(joiner_chunks, 400000, RAISED_US);
    int64_t moved_us = 0;
    std::optional<Gap> raised;
    for (const Gap& gap : gaps) {
        if (first_chunks[gap.chunk].stamp_us > joined_us) {
            moved_us += gap.us;
            raised = raised.value_or(gap);
        }
    }
    // The deficit: the 250 ms the lead rose by, less the first block's head start and a margin
    // for the threads' timing
    EXPECT_GE(moved_us, RAISED_US - LIVE_LEAD_US - 60000);
    ASSERT_TRUE(raised);
    const ChunkInfo& shifted = first_chunks[raised->chunk];
    EXPECT_LE(shifted.ts - shifted.stamp_us, on_time_lead(RAISED_US));
    ASSERT_FALSE(joiner_chunks.empty());
    // Unless the frame before it was late when it was sent: the pump skips late frames
    const ChunkInfo& joined = joiner_chunks.front();
    if (joined.ts - CELL_US > joined.stamp_us) {
        EXPECT_LT(joined.ts - joined.trigger_us, 1000000);
    }
    EXPECT_GT(expect_same_timestamps(first_chunks, joiner_chunks), 0U);
    expect_heard(decode(signal, first.sink().played()), first_chunks, {segment});
    expect_heard(decode(signal, joiner.sink().played()), joiner_chunks, {segment});
}

// The host stalls for half a second. When its writes resume, the next frame's lead is far under
// G_eff - L_p - M_pump, so the timeline shifts there: each player hears one gap of about the stall
// and none of the audio is lost, the frame at the gap leaving with at most the on-time lead, `G_eff
// - L_p + J_in`, rather than the full group lead a buffered input restores. One player streams PCM
// and one FLAC, with the same timestamp for the same audio on both, and the drain completes played.
// Two seconds of input and the stall, played in real time.
TEST(ServerPlayersLive, ShiftPastAHostStallAndPlayOn) {
    static constexpr int64_t STALL_US = 500000;
    const Signal signal(24);
    Rig s(signal);
    const Player& pcm = s.add({{Codec::PCM, 2, RATE, 24}});
    const Player& flac = s.add({{Codec::FLAC, 2, RATE, 24}});
    const std::shared_ptr<GroupInput> input = s.open_live();
    ASSERT_TRUE(input);
    const Segment segment{0, 2 * RATE};
    std::thread host([&] {
        feed_live(*input, signal, segment, [](uint32_t written) {
            if (written == RATE) {
                std::this_thread::sleep_for(std::chrono::microseconds(STALL_US));
            }
        });
        input->drain();
    });
    const std::string drained = Recorder::drained(input->id(), DrainOutcome::PLAYED);
    s.run_until_drained({drained});
    host.join();

    const std::lock_guard<std::mutex> lock(s.trace.mutex);
    EXPECT_FALSE(s.trace.failed);
    EXPECT_EQ(s.trace.codecs[s.client_id(flac)], "flac");
    for (const Player* player : {&pcm, &flac}) {
        SCOPED_TRACE(player->url());
        const std::vector<ChunkInfo>& chunks = s.trace.chunks[s.client_id(*player)];
        const std::vector<Gap> gaps = expect_sent_live(chunks, START_FLOOR_US, LIVE_LEAD_US);
        // The stall, less the first block's 10 ms head start and a margin for the threads' timing
        const auto stall = std::find_if(gaps.begin(), gaps.end(),
                                        [](const Gap& gap) { return gap.us >= STALL_US - 100000; });
        ASSERT_NE(stall, gaps.end());
        const ChunkInfo& shifted = chunks[stall->chunk];
        EXPECT_LE(shifted.ts - shifted.stamp_us, on_time_lead(LIVE_LEAD_US));
        expect_heard(decode(signal, player->sink().played()), chunks, {segment});
    }
    EXPECT_GT(
        expect_same_timestamps(s.trace.chunks[s.client_id(pcm)], s.trace.chunks[s.client_id(flac)]),
        0U);
}

// A live input paused for 0.7 s takes every write while paused, and its resume continues from the
// newest input: each player's stream ends at the pause and starts again with the first sample
// written after the resume (or some cells later, when the trigger runs late), never with what was
// written while paused. About 2.5 seconds of input, played in real time.
TEST(ServerPlayersLive, ResumeAPausedLiveInputAtItsHead) {
    constexpr uint32_t PAUSE_AT = (3 * RATE) / 4;
    constexpr uint32_t RESUME_AT = PAUSE_AT + ((7 * RATE) / 10);
    const Signal signal(24);
    Rig s(signal);
    const Player& pcm = s.add({{Codec::PCM, 2, RATE, 24}});
    const Player& flac = s.add({{Codec::FLAC, 2, RATE, 24}});
    const std::shared_ptr<GroupInput> input = s.open_live();
    ASSERT_TRUE(input);
    const Segment segment{0, RESUME_AT + RATE};
    uint64_t paused = 0;
    std::thread host([&] {
        feed_live(*input, signal, segment, [&](uint32_t written) {
            if (written == PAUSE_AT) {
                paused = input->pause();
            } else if (written == RESUME_AT) {
                input->resume();
            }
        });
        input->drain();
    });
    const std::string drained = Recorder::drained(input->id(), DrainOutcome::PLAYED);
    s.run_until_drained({drained});
    host.join();
    EXPECT_LE(paused, PAUSE_AT);

    const std::lock_guard<std::mutex> lock(s.trace.mutex);
    EXPECT_FALSE(s.trace.failed);
    const Segment before{0, RESUME_AT};
    const Segment after{RESUME_AT, segment.count - RESUME_AT};
    for (const Player* player : {&pcm, &flac}) {
        SCOPED_TRACE(player->url());
        const std::string id = s.client_id(*player);
        EXPECT_EQ(s.trace.streams[id], 2U);
        const std::vector<ChunkInfo>& chunks = s.trace.chunks[id];
        expect_sent_live(chunks, START_FLOOR_US, LIVE_LEAD_US);
        const auto resumed = std::find_if(chunks.begin(), chunks.end(),
                                          [](const ChunkInfo& chunk) { return chunk.stream == 2; });
        ASSERT_NE(resumed, chunks.end());
        EXPECT_GE(resumed->position, RESUME_AT);
        EXPECT_EQ((resumed->position - RESUME_AT) % CELL, 0U);
        for (const ChunkInfo& chunk : chunks) {
            EXPECT_EQ(chunk.stream == 2, after.holds(chunk.position)) << chunk.position;
        }
        expect_heard(decode(signal, player->sink().played()), chunks, {before, after});
    }
}

// ==============================================================================
// Opus
// ==============================================================================

/// The 16-bit samples of a PCM stream's chunks, in order, from the first chunk's timestamp; each
/// chunk where the one before it ended
std::vector<int16_t> pcm_timeline(const std::vector<ChunkInfo>& chunks) {
    std::vector<int16_t> line;
    for (const ChunkInfo& chunk : chunks) {
        const int64_t at = int64_t{static_cast<int64_t>(line.size() / 2)} * 1000000 / RATE;
        EXPECT_EQ(chunk.ts, chunks.front().ts + at);
        const size_t first = line.size();
        line.resize(first + (chunk.pcm.size() / 2));
        std::memcpy(line.data() + first, chunk.pcm.data(), chunk.pcm.size());
    }
    return line;
}

// spec player/v1.md "Codec framing": with ServerConfig::enable_opus, a sendspin-cpp player listing
// Opus first streams it beside a PCM player in one group, played through a drain: one packet a
// chunk, each where the one before it ended, on the PCM player's cells; decoded in order, the
// packets give the audio the PCM player gets at the same timestamps, at no lag (lossy, so compared
// by SNR), and the Opus player plays it
TEST(ServerPlayersOpus, PlayOpusInSyncWithAPcmPlayer) {
    const Signal signal(16);
    Rig s(signal, RATE, false, true);
    const Player& pcm = s.add({{Codec::PCM, 2, RATE, 16}});
    const Player& opus = s.add({{Codec::OPUS, 2, RATE, 16}, {Codec::PCM, 2, RATE, 16}});
    const std::shared_ptr<GroupInput> input = s.open();
    ASSERT_TRUE(input);
    constexpr uint64_t SAMPLES = 3 * RATE;
    std::thread host([&] {
        for (uint64_t written = 0; written < SAMPLES;) {
            const Bytes block = sendspin::server::test::tone_frames(written, CELL, RATE);
            const WriteResult result = input->write(block.data(), CELL);
            written += result.frames;
            if (result.frames < CELL) {
                input->wait_writable(std::numeric_limits<int64_t>::max());
            }
        }
        input->drain();
    });
    const std::string drained = Recorder::drained(input->id(), DrainOutcome::PLAYED);
    s.run_until_drained({drained});
    host.join();

    const std::lock_guard<std::mutex> lock(s.trace.mutex);
    const std::string pcm_id = s.client_id(pcm);
    const std::string opus_id = s.client_id(opus);
    EXPECT_EQ(s.trace.codecs[pcm_id], "pcm");
    EXPECT_EQ(s.trace.codecs[opus_id], "opus");
    const std::vector<ChunkInfo>& pcm_chunks = s.trace.chunks[pcm_id];
    const std::vector<ChunkInfo>& opus_chunks = s.trace.chunks[opus_id];
    ASSERT_FALSE(pcm_chunks.empty());
    ASSERT_GT(opus_chunks.size(), 50U);
    const std::vector<int16_t> reference = pcm_timeline(pcm_chunks);

    sendspin::server::test::OpusReader reader(RATE, 2);
    std::vector<int16_t> decoded;
    for (size_t i = 0; i < opus_chunks.size(); ++i) {
        EXPECT_EQ(opus_chunks[i].ts, opus_chunks.front().ts + (int64_t{20000} * i));
        const std::optional<std::vector<int16_t>> samples = reader.decode(opus_chunks[i].pcm);
        ASSERT_TRUE(samples);
        ASSERT_EQ(samples->size(), size_t{CELL} * 2);
        decoded.insert(decoded.end(), samples->begin(), samples->end());
    }
    // The same cells: from the later of the two starts and five cells on, clear of a start
    const int64_t pcm_start = pcm_chunks.front().ts;
    const int64_t opus_start = opus_chunks.front().ts;
    EXPECT_EQ((opus_start - pcm_start) % 20000, 0);
    const int64_t from_us = std::max(pcm_start, opus_start) + (5 * 20000);
    const auto pcm_from = static_cast<size_t>((from_us - pcm_start) / 20000 * CELL * 2);
    const auto opus_from = static_cast<size_t>((from_us - opus_start) / 20000 * CELL * 2);
    ASSERT_LT(pcm_from + (RATE * 2), reference.size());
    ASSERT_LT(opus_from + (RATE * 2), decoded.size());
    const std::span<const int16_t> heard(decoded.data() + opus_from, decoded.size() - opus_from);
    const std::span<const int16_t> wanted(reference.data() + pcm_from,
                                          std::min(reference.size() - pcm_from, heard.size()));
    EXPECT_EQ(sendspin::server::test::best_lag(wanted, heard, 480, 2), 0);
    EXPECT_GT(sendspin::server::test::snr_db(wanted, heard), 20.0);
    // It played: how much is real time, which a player behind its clock drops (expect_heard())
    EXPECT_GT(opus.sink().audible(), 0U);
}

}  // namespace
