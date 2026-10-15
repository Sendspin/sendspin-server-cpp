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

/// @file soak.cpp
/// @brief The soak: one server with two groups, in-process sendspin-cpp players of several
/// formats and a raw player, driven by a seeded random script of host and player events for
/// SENDSPIN_SERVER_SOAK_SECONDS. The message trace is checked as it arrives (each stream
/// contiguous and on time, each player's buffer accounting within its capacity, the same audio at
/// the same timestamp for players of one format in one group), and each sendspin-cpp player's
/// output for underruns between events. Skipped unless the variable is set; its own entry point,
/// whose watchdog follows the requested duration.

#include "image_codec.h"
#include "peer.h"
#include "player_sink.h"
#include "raw_client.h"
#include "soak_wire_checker.h"
#include "test_ports.h"
#include "test_recorder.h"
#include <gtest/gtest.h>
#include <stb_image_write.h>

#include <sendspin/artwork_role.h>
#include <sendspin/client.h>
#include <sendspin/color_role.h>
#include <sendspin/player_role.h>
#include <sendspin/server/server.h>
#include <sendspin/visualizer_role.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <exception>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using sendspin::server::ClientInfo;
using sendspin::server::GroupId;
using sendspin::server::GroupInfo;
using sendspin::server::GroupInput;
using sendspin::server::InputFormat;
using sendspin::server::InputKind;
using sendspin::server::InputOptions;
using sendspin::server::InputState;
using sendspin::server::OpenedInput;
using sendspin::server::SampleFormat;
using sendspin::server::SendspinServer;
using sendspin::server::ServerConfig;
using sendspin::server::ServerError;
using sendspin::server::StartResult;
using sendspin::server::StreamCodec;
using sendspin::server::StreamFormat;
using sendspin::server::TraceDirection;
using sendspin::server::TracedMessage;
using sendspin::server::WriteResult;
using sendspin::server::test::AlwaysReady;
using sendspin::server::test::client_clock_us;
using sendspin::server::test::RawClient;
using sendspin::server::test::WireChecker;
using Bytes = std::vector<uint8_t>;
using Codec = sendspin::SendspinCodecFormat;

// ==============================================================================
// Settings
// ==============================================================================

/// The soak's length from SENDSPIN_SERVER_SOAK_SECONDS; nullopt skips it
std::optional<int64_t> soak_seconds() {
    const char* value = std::getenv("SENDSPIN_SERVER_SOAK_SECONDS");
    if (value == nullptr || *value == '\0') {
        return std::nullopt;
    }
    const long long seconds = std::strtoll(value, nullptr, 10);
    return seconds > 0 ? std::optional<int64_t>(seconds) : std::nullopt;
}

/// The script's seed from SENDSPIN_SERVER_SOAK_SEED, 1 by default
uint64_t soak_seed() {
    const char* value = std::getenv("SENDSPIN_SERVER_SOAK_SEED");
    return value == nullptr ? 1U : std::strtoull(value, nullptr, 10);
}

constexpr int64_t SECOND_US = 1000000;
/// The write-stall timeout the soak's server runs with: short, so the raw player's longest stalls
/// pass it
constexpr int64_t WRITE_STALL_US = 5 * SECOND_US;
/// How much longer than WRITE_STALL_US the raw player's longest stalls last: its socket buffers
/// take a few seconds of its audio before the server's writes stop
constexpr int64_t LONG_STALL_MARGIN_US = 7 * SECOND_US;
/// How long before an event and after it a player's output is not checked for underruns: the
/// event's own silence and resynchronization, a startup lead included
constexpr int64_t QUIET_BEFORE_US = SECOND_US;
constexpr int64_t QUIET_AFTER_US = 5 * SECOND_US;
/// A run of silent output frames this long counts as a resynchronization's silence; shorter runs
/// can be audio that rounds to silence
constexpr size_t SILENT_RUN_FRAMES = 4;
/// A run at least this long must have exercised every kind of event that structures the script
constexpr int64_t MIN_FULL_SCRIPT_S = 120;

// ==============================================================================
// The host's images
// ==============================================================================

void append_bytes(void* context, void* data, int size) {
    auto* out = static_cast<std::vector<uint8_t>*>(context);
    const auto* bytes = static_cast<const uint8_t*>(data);
    out->insert(out->end(), bytes, bytes + size);
}

/// A width by height PNG of one gray level
std::vector<uint8_t> solid_png(int width, int height, uint8_t level) {
    const std::vector<uint8_t> pixels(static_cast<size_t>(width) * height * 3, level);
    std::vector<uint8_t> png;
    stbi_write_png_to_func(&append_bytes, &png, width, height, 3, pixels.data(), width * 3);
    return png;
}

// ==============================================================================
// The host's audio
// ==============================================================================

/// A 24-bit sample of the input at position on channel: noise at half scale, never silence
int32_t sample_at(uint64_t position, uint32_t channel) {
    uint32_t x = static_cast<uint32_t>(position) * 0x9E3779B1U + channel * 0x85EBCA77U;
    x ^= x >> 15;
    x *= 0x2C1B3C6DU;
    x ^= x >> 12;
    int32_t value = static_cast<int32_t>(x) >> 10;
    if (value > -512 && value < 512) {
        value += 1024;
    }
    return value;
}

void put_frames(Bytes& block, uint64_t first, size_t frames, uint32_t channels) {
    block.resize(frames * channels * 3);
    for (size_t i = 0; i < frames; ++i) {
        for (uint32_t c = 0; c < channels; ++c) {
            const int32_t value = sample_at(first + i, c);
            uint8_t* out = &block[(i * channels + c) * 3];
            out[0] = static_cast<uint8_t>(value);
            out[1] = static_cast<uint8_t>(value >> 8);
            out[2] = static_cast<uint8_t>(value >> 16);
        }
    }
}

// ==============================================================================
// The event log
// ==============================================================================

/// One scripted event: when it began and ended on the server's clock, and the group or client it
/// touched (empty for neither)
struct Event {
    std::string kind;
    int64_t begin_us{0};
    int64_t end_us{0};
    std::string group;
    std::string client;
};

/// Events in the order the script applied them; the script's thread writes, the analysis reads
/// once the script ended
class EventLog {
public:
    void add(Event event) {
        const std::lock_guard<std::mutex> lock(this->mutex_);
        ++this->counts_[event.kind];
        this->events_.push_back(std::move(event));
    }

    /// Ends the last event of kind in group at end_us
    void end_last(const std::string& kind, const std::string& group, int64_t end_us) {
        const std::lock_guard<std::mutex> lock(this->mutex_);
        for (auto it = this->events_.rbegin(); it != this->events_.rend(); ++it) {
            if (it->kind == kind && it->group == group) {
                it->end_us = end_us;
                return;
            }
        }
    }

    std::vector<Event> events() const {
        const std::lock_guard<std::mutex> lock(this->mutex_);
        return this->events_;
    }

    std::map<std::string, size_t> counts() const {
        const std::lock_guard<std::mutex> lock(this->mutex_);
        return this->counts_;
    }

private:
    mutable std::mutex mutex_;
    std::vector<Event> events_;
    std::map<std::string, size_t> counts_;
};

// ==============================================================================
// A sendspin-cpp player's output, checked for underruns
// ==============================================================================

/// What a player's output did in one second of the soak
struct Second {
    size_t played{0};
    /// Frames played that are not silence
    size_t audible{0};
    /// Frames due while the output had nothing to play, during a stream that had begun playing
    size_t starved{0};
    /// Frames in runs of SILENT_RUN_FRAMES or more silent ones
    size_t silent{0};
    /// The longest the output's own thread took between two of its 5 ms ticks
    int64_t max_tick_us{0};
};

/// A real-time output like tests/player_sink.h's Sink that keeps no audio: it counts, per second
/// of the soak, what it played, the frames it lacked during a stream, and runs of silence. Its
/// rate and frame size follow the stream the main loop reads from the player.
class SoakSink : public sendspin::PlayerRoleListener {
public:
    /// @param origin_us the soak's start on client_clock_us()
    SoakSink(sendspin::PlayerRole& role, int64_t origin_us, size_t seconds)
        : role_(role),
          origin_us_(origin_us),
          seconds_(seconds + 60),
          thread_([this] { this->run(); }) {}

    ~SoakSink() override {
        {
            const std::lock_guard<std::mutex> lock(this->mutex_);
            this->stopping_ = true;
        }
        this->changed_.notify_all();
        this->thread_.join();
    }

    SoakSink(const SoakSink&) = delete;
    SoakSink& operator=(const SoakSink&) = delete;

    /// On the main loop: the stream's format, as the player reports it
    void set_format(uint32_t rate, size_t frame_bytes) {
        const std::lock_guard<std::mutex> lock(this->mutex_);
        if (rate == this->rate_ && frame_bytes == this->frame_bytes_) {
            return;
        }
        this->rate_ = rate;
        this->frame_bytes_ = frame_bytes;
        this->buffer_.clear();
        this->audible_since_start_ = false;
        this->changed_.notify_all();
    }

    void on_stream_start() override {
        const std::lock_guard<std::mutex> lock(this->mutex_);
        this->streaming_ = true;
        this->audible_since_start_ = false;
    }

    void on_stream_end() override {
        const std::lock_guard<std::mutex> lock(this->mutex_);
        this->streaming_ = false;
    }

    size_t on_audio_write(uint8_t* data, size_t length, uint32_t timeout_ms) override {
        std::unique_lock<std::mutex> lock(this->mutex_);
        this->changed_.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&] {
            return this->stopping_ || this->frame_bytes_ == 0 ||
                   this->buffer_.size() + this->frame_bytes_ <= this->buffer_bytes();
        });
        if (this->frame_bytes_ == 0) {
            return 0;
        }
        size_t room = this->buffer_bytes() - std::min(this->buffer_bytes(), this->buffer_.size());
        room = std::min(room, length);
        room -= room % this->frame_bytes_;
        this->buffer_.insert(this->buffer_.end(), data, data + room);
        return room;
    }

    std::vector<Second> seconds() const {
        const std::lock_guard<std::mutex> lock(this->mutex_);
        return this->seconds_;
    }

private:
    size_t buffer_bytes() const {
        return size_t{this->rate_} / 10 * this->frame_bytes_;
    }

    Second* second_at(int64_t now) {
        const int64_t index = (now - this->origin_us_) / SECOND_US;
        if (index < 0 || static_cast<size_t>(index) >= this->seconds_.size()) {
            return nullptr;
        }
        return &this->seconds_[static_cast<size_t>(index)];
    }

    void run() {
        constexpr int64_t TICK_US = 5000;
        int64_t last = client_clock_us();
        int64_t owed_us = 0;
        std::unique_lock<std::mutex> lock(this->mutex_);
        while (!this->stopping_) {
            this->changed_.wait_for(lock, std::chrono::microseconds(TICK_US));
            const int64_t now = client_clock_us();
            owed_us += now - last;
            if (Second* tick = this->second_at(now); tick != nullptr) {
                tick->max_tick_us = std::max(tick->max_tick_us, now - last);
            }
            last = now;
            if (this->rate_ == 0 || this->frame_bytes_ == 0) {
                owed_us = 0;
                continue;
            }
            const auto due = static_cast<size_t>(owed_us * this->rate_ / SECOND_US);
            owed_us -= static_cast<int64_t>(due) * SECOND_US / this->rate_;
            const size_t frames = std::min(due, this->buffer_.size() / this->frame_bytes_);
            Second* second = this->second_at(now);
            if (second != nullptr && this->streaming_ && this->audible_since_start_) {
                second->starved += due - frames;
            }
            if (frames == 0) {
                continue;
            }
            for (size_t f = 0; f < frames; ++f) {
                const auto frame =
                    this->buffer_.begin() + static_cast<std::ptrdiff_t>(f * this->frame_bytes_);
                const bool silent =
                    std::all_of(frame, frame + static_cast<std::ptrdiff_t>(this->frame_bytes_),
                                [](uint8_t byte) { return byte == 0; });
                if (silent) {
                    ++this->silent_run_;
                    if (second != nullptr && this->audible_since_start_) {
                        if (this->silent_run_ == SILENT_RUN_FRAMES) {
                            second->silent += SILENT_RUN_FRAMES;
                        } else if (this->silent_run_ > SILENT_RUN_FRAMES) {
                            ++second->silent;
                        }
                    }
                } else {
                    this->silent_run_ = 0;
                    this->audible_since_start_ = true;
                    if (second != nullptr) {
                        ++second->audible;
                    }
                }
            }
            if (second != nullptr) {
                second->played += frames;
            }
            this->buffer_.erase(
                this->buffer_.begin(),
                this->buffer_.begin() + static_cast<std::ptrdiff_t>(frames * this->frame_bytes_));
            this->changed_.notify_all();
            lock.unlock();
            this->role_.notify_audio_played(static_cast<uint32_t>(frames), now);
            lock.lock();
        }
    }

    sendspin::PlayerRole& role_;
    int64_t origin_us_;
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    uint32_t rate_{0};
    size_t frame_bytes_{0};
    Bytes buffer_;
    std::vector<Second> seconds_;
    size_t silent_run_{0};
    bool streaming_{false};
    bool audible_since_start_{false};
    bool stopping_{false};
    std::thread thread_;
};

// ==============================================================================
// Players
// ==============================================================================

/// A sendspin-cpp player's configuration
struct Profile {
    std::string name;
    std::vector<sendspin::AudioSupportedFormatObject> formats;
    uint16_t min_buffer_ms{150};
    size_t buffer_bytes{sendspin::PlayerRoleConfig::DEFAULT_AUDIO_BUFFER_CAPACITY};
    uint16_t output_delay_ms{0};
    /// It also has the visualizer role
    bool visualizer{false};
};

/// The profiles players join with: PCM at 48 and 44.1 kHz (the second resampled from a 48 kHz
/// input), FLAC, mono, Opus, and PCM with the visualizer role. The first two share a format, for
/// the same-audio check.
std::vector<Profile> profiles() {
    return {
        {"pcm48",
         {{Codec::PCM, 2, 48000, 24}, {Codec::FLAC, 2, 48000, 24}, {Codec::PCM, 2, 44100, 24}},
         150,
         sendspin::PlayerRoleConfig::DEFAULT_AUDIO_BUFFER_CAPACITY,
         0},
        {"pcm48-delayed",
         {{Codec::PCM, 2, 48000, 24}, {Codec::FLAC, 2, 48000, 24}},
         300,
         400000,
         60},
        {"flac48-16", {{Codec::FLAC, 2, 48000, 16}, {Codec::PCM, 2, 48000, 16}}, 250, 300000, 0},
        {"pcm44", {{Codec::PCM, 2, 44100, 24}, {Codec::FLAC, 2, 44100, 24}}, 200, 250000, 120},
        {"mono", {{Codec::FLAC, 1, 48000, 24}, {Codec::PCM, 1, 48000, 24}}, 150, 600000, 30},
        {"opus48", {{Codec::OPUS, 2, 48000, 16}, {Codec::PCM, 2, 48000, 16}}, 150, 200000, 0},
        {"viz48", {{Codec::PCM, 2, 48000, 16}}, 150, 300000, 0, true},
    };
}

/// The side of the album channel every sendspin-cpp player declares
constexpr uint32_t ART_SIDE = 96;

/// What the players' artwork and color roles received, over every player of the run
struct ArtCounts {
    std::atomic<size_t> images{0};
    /// Images that did not decode as a JPEG of ART_SIDE by ART_SIDE
    std::atomic<size_t> wrong{0};
    std::atomic<size_t> colors{0};
};

/// What the visualizers received, over every player of the run
struct VisualizerCounts {
    std::atomic<size_t> starts{0};
    std::atomic<size_t> frames{0};
};

/// A player's visualizer listener: frames on its drain thread, the stream's start on its main
/// loop
class VisualizerCounter : public sendspin::VisualizerRoleListener {
public:
    explicit VisualizerCounter(VisualizerCounts& counts) : counts_(counts) {}

    void on_loudness(int64_t /*client_timestamp*/, uint16_t /*loudness*/) override {
        ++this->counts_.frames;
    }

    void on_spectrum(int64_t /*client_timestamp*/, const std::vector<uint16_t>& /*bins*/) override {
        ++this->counts_.frames;
    }

    void on_peak(int64_t /*client_timestamp*/, uint8_t /*strength*/) override {
        ++this->counts_.frames;
    }

    void on_visualizer_stream_start(
        const sendspin::ServerVisualizerStreamObject& /*stream*/) override {
        ++this->counts_.starts;
    }

private:
    VisualizerCounts& counts_;
};

/// A player's artwork and color listener: images on its decode thread, colors on its main loop
class ArtListener : public sendspin::ArtworkRoleListener, public sendspin::ColorRoleListener {
public:
    explicit ArtListener(ArtCounts& counts) : counts_(counts) {}

    void on_image_decode(uint8_t /*slot*/, const uint8_t* data, size_t length,
                         sendspin::SendspinImageFormat /*format*/) override {
        const std::optional<sendspin::server::DecodedImage> image =
            sendspin::server::decode_image(std::span<const uint8_t>(data, length));
        ++this->counts_.images;
        if (!image || image->width != ART_SIDE || image->height != ART_SIDE) {
            ++this->counts_.wrong;
        }
    }

    void on_color(const sendspin::ServerColorStateObject& /*color*/) override {
        ++this->counts_.colors;
    }

private:
    ArtCounts& counts_;
};

/// A sendspin-cpp client with a player role, an album artwork channel, the color role and, for
/// its profile, the visualizer role, listening for the server to dial
class CppPlayer {
public:
    CppPlayer(const Profile& profile, int64_t origin_us, size_t seconds, ArtCounts& art,
              VisualizerCounts& visualizer)
        : profile_(profile),
          port_(sendspin::server::test::free_port()),
          client_(sendspin::server::test::peer_config(
              profile.name + " " + std::to_string(this->port_), this->port_)),
          art_(art),
          visualizer_(visualizer) {
        if (profile.visualizer) {
            sendspin::VisualizerRoleConfig config;
            config.support.buffer_capacity = 8192;
            config.stream.types = {sendspin::VisualizerDataType::LOUDNESS,
                                   sendspin::VisualizerDataType::SPECTRUM,
                                   sendspin::VisualizerDataType::PEAK};
            config.stream.rate_max = 30;
            config.stream.spectrum =
                sendspin::VisualizerSpectrumConfig{.n_disp_bins = 32,
                                                   .scale = sendspin::VisualizerSpectrumScale::MEL,
                                                   .f_min = 40,
                                                   .f_max = 16000};
            this->client_.add_visualizer(config).set_listener(&this->visualizer_);
        }
        sendspin::ArtworkRoleConfig artwork;
        sendspin::ImageSlotPreference album;
        album.source = sendspin::SendspinImageSource::ALBUM;
        album.format = sendspin::SendspinImageFormat::JPEG;
        album.width = ART_SIDE;
        album.height = ART_SIDE;
        artwork.preferred_formats.push_back(album);
        this->client_.add_artwork(artwork).set_listener(&this->art_);
        this->client_.add_color().set_listener(&this->art_);
        sendspin::PlayerRoleConfig config;
        config.audio_formats = profile.formats;
        config.min_buffer_ms = profile.min_buffer_ms;
        config.audio_buffer_capacity = profile.buffer_bytes;
        config.initial_output_delay_ms = profile.output_delay_ms;
        this->role_ = &this->client_.add_player(config);
        this->sink_ = std::make_unique<SoakSink>(*this->role_, origin_us, seconds);
        this->role_->set_listener(this->sink_.get());
        this->client_.set_network_provider(&this->network_);
        this->client_.set_unpaired_access_enabled(true);
        // Reported in client/state, and changed by the script, only when adjustable
        this->role_->set_output_delay_adjustable(true);
        EXPECT_TRUE(this->client_.start());
    }

    ~CppPlayer() {
        this->client_.stop();
    }

    CppPlayer(const CppPlayer&) = delete;
    CppPlayer& operator=(const CppPlayer&) = delete;

    std::string url() const {
        return sendspin::server::test::peer_url(this->port_);
    }

    /// On the main loop: runs the client's loop and hands the stream's format to the sink
    void loop() {
        this->client_.loop();
        const sendspin::ServerPlayerStreamObject& stream = this->role_->get_current_stream_params();
        if (stream.is_complete()) {
            this->sink_->set_format(*stream.sample_rate,
                                    size_t{*stream.channels} * (*stream.bit_depth / 8));
        }
    }

    sendspin::PlayerRole& role() {
        return *this->role_;
    }

    const SoakSink& sink() const {
        return *this->sink_;
    }

    const Profile& profile() const {
        return this->profile_;
    }

    std::string client_id;

private:
    Profile profile_;
    uint16_t port_;
    AlwaysReady network_;
    sendspin::SendspinClient client_;
    ArtListener art_;
    VisualizerCounter visualizer_;
    sendspin::PlayerRole* role_{nullptr};
    std::unique_ptr<SoakSink> sink_;
};

/// A raw player on its own thread: PCM at 48 kHz, 16 bits; it changes its client/state timing on
/// command and stops reading for a while, past the server's write-stall timeout at times, after
/// which the server closes it and dials it again
class RawPlayer {
public:
    /// About ten seconds of its audio: more than the socket buffers take while it stops reading, so
    /// the server's writes stall
    static constexpr uint32_t CAPACITY = 2000000;
    /// A receive buffer the system does not grow, so that a stall soon leaves the server no room
    static constexpr int RECEIVE_BUFFER_BYTES = 65536;

    RawPlayer(SendspinServer& server, std::atomic<size_t>& closes)
        : server_(server),
          closes_(closes),
          url_(this->client_.url()),
          thread_([this] { this->run(); }) {}

    ~RawPlayer() {
        this->stopping_ = true;
        this->thread_.join();
    }

    RawPlayer(const RawPlayer&) = delete;
    RawPlayer& operator=(const RawPlayer&) = delete;

    void set_timing(uint32_t min_buffer_ms, uint32_t required_lead_ms, uint32_t output_delay_ms) {
        const std::lock_guard<std::mutex> lock(this->mutex_);
        this->state_ = sendspin::server::test::client_state_json(
            true, sendspin::server::test::player_state_json(min_buffer_ms, required_lead_ms,
                                                            output_delay_ms));
    }

    void stall(int64_t us) {
        this->stall_us_ = us;
    }

    const std::string& url() const {
        return this->url_;
    }

private:
    void run() {
        const std::string hello = sendspin::server::test::player_hello_json(
            "Raw", R"([{"codec":"pcm","sample_rate":48000,"channels":2,"bit_depth":16}])",
            CAPACITY);
        this->set_timing(150, 0, 0);
        this->client_.fix_receive_buffer(RECEIVE_BUFFER_BYTES);
        EXPECT_TRUE(this->server_.connect_url(this->url_));
        while (!this->stopping_) {
            this->client_.activate(hello);
            try {
                {
                    const std::lock_guard<std::mutex> lock(this->mutex_);
                    this->client_.send_json(this->state_);
                    this->sent_ = this->state_;
                }
                this->serve();
            } catch (const std::exception& /*closed*/) {
                // A write to a connection the server closed
            }
            if (!this->stopping_) {
                ++this->closes_;
                // The server dials a dropped client again; an operator's dial hurries it
                this->server_.connect_url(this->url_);
            }
        }
    }

    // Reads every message as it comes until the connection closes or the soak stops, keeping a
    // time request outstanding so that one always comes
    void serve() {
        constexpr int64_t TIME_EVERY_US = 100000;
        int64_t transmitted = 0;
        int64_t last_request = client_clock_us();
        this->client_.send_json(sendspin::server::test::client_time_json(++transmitted));
        while (!this->stopping_) {
            if (!this->client_.receive()) {
                return;
            }
            {
                const std::lock_guard<std::mutex> lock(this->mutex_);
                if (this->state_ != this->sent_) {
                    this->client_.send_json(this->state_);
                    this->sent_ = this->state_;
                }
            }
            const int64_t stall = this->stall_us_.exchange(0);
            if (stall > 0) {
                std::this_thread::sleep_for(std::chrono::microseconds(stall));
            }
            if (client_clock_us() - last_request >= TIME_EVERY_US) {
                last_request = client_clock_us();
                this->client_.send_json(sendspin::server::test::client_time_json(++transmitted));
            }
        }
    }

    SendspinServer& server_;
    std::atomic<size_t>& closes_;
    RawClient client_;
    std::string url_;
    std::mutex mutex_;
    std::string state_;
    std::string sent_;
    std::atomic<int64_t> stall_us_{0};
    std::atomic<bool> stopping_{false};
    std::thread thread_;
};

// ==============================================================================
// A group's host
// ==============================================================================

/// The host feeding one group: a gapless playlist of tracks at 48, 44.1 and 32 kHz, now and then
/// mono, written as far ahead as the input admits, with seeks and pauses on command; or a live
/// input written at real time's pace in 10 ms blocks, with stalls on command
class Host {
public:
    enum class Command : uint8_t { SEEK, PAUSE, RESUME, GO_LIVE, GO_BUFFERED, STALL };

    struct Counts {
        size_t tracks{0};
        size_t format_changes{0};
        size_t refused_formats{0};
        size_t latched{0};
        size_t live_dropped{0};
        uint64_t frames{0};
    };

    Host(SendspinServer& server, GroupId group, uint64_t seed)
        : server_(server), group_(group), random_(seed), thread_([this] { this->run(); }) {}

    ~Host() {
        {
            const std::lock_guard<std::mutex> lock(this->mutex_);
            this->stopping_ = true;
        }
        this->thread_.join();
    }

    Host(const Host&) = delete;
    Host& operator=(const Host&) = delete;

    void command(Command command, int64_t stall_us = 0) {
        const std::lock_guard<std::mutex> lock(this->mutex_);
        this->commands_.emplace_back(command, stall_us);
    }

    Counts counts() const {
        const std::lock_guard<std::mutex> lock(this->mutex_);
        return this->counts_;
    }

private:
    std::shared_ptr<GroupInput> open(InputKind kind, const InputFormat& format) {
        InputOptions options;
        options.format = format;
        options.kind = kind;
        for (int attempt = 0; attempt < 500; ++attempt) {
            const OpenedInput opened = this->server_.open_input(this->group_, options);
            if (opened.input) {
                return opened.input;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        ADD_FAILURE() << "could not open an input on group " << this->group_.value;
        return nullptr;
    }

    InputFormat next_format() {
        constexpr std::array<uint32_t, 3> RATES = {48000, 44100, 32000};
        const uint32_t rate = RATES[this->random_() % RATES.size()];
        const uint8_t channels = this->random_() % 8 == 0 ? 1 : 2;
        return {rate, channels, SampleFormat::S24};
    }

    void run() {
        InputFormat format{48000, 2, SampleFormat::S24};
        std::shared_ptr<GroupInput> input = this->open(InputKind::BUFFERED, format);
        uint64_t position = 0;
        uint64_t track_left = 0;
        bool paused = false;
        Bytes block;
        using Clock = std::chrono::steady_clock;
        Clock::time_point due = Clock::now();
        for (;;) {
            std::vector<std::pair<Command, int64_t>> commands;
            {
                const std::lock_guard<std::mutex> lock(this->mutex_);
                if (this->stopping_) {
                    break;
                }
                commands.swap(this->commands_);
            }
            for (const auto& [command, stall_us] : commands) {
                switch (command) {
                    case Command::SEEK:
                        if (input) {
                            input->flush();
                        }
                        track_left = 0;
                        position += 1000003;
                        break;
                    case Command::PAUSE:
                        if (input && !paused) {
                            input->pause();
                            paused = true;
                        }
                        break;
                    case Command::RESUME:
                        if (input && paused) {
                            input->resume();
                            paused = false;
                        }
                        break;
                    case Command::GO_LIVE:
                        input.reset();
                        format = {48000, 2, SampleFormat::S24};
                        input = this->open(InputKind::LIVE, format);
                        this->live_ = true;
                        paused = false;
                        due = Clock::now();
                        break;
                    case Command::GO_BUFFERED:
                        input.reset();
                        format = {48000, 2, SampleFormat::S24};
                        input = this->open(InputKind::BUFFERED, format);
                        this->live_ = false;
                        paused = false;
                        track_left = 0;
                        break;
                    case Command::STALL:
                        std::this_thread::sleep_for(std::chrono::microseconds(stall_us));
                        due = Clock::now();
                        break;
                }
            }
            if (!input) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }
            if (this->live_) {
                // A capture device's 10 ms blocks, at real time's pace; a write it refuses is
                // dropped (GroupInput::write())
                std::this_thread::sleep_until(due);
                const size_t frames = format.sample_rate / 100;
                put_frames(block, position, frames, format.channels);
                const WriteResult result = input->write(block.data(), frames);
                position += frames;
                if (result.state == InputState::STOP_LATCHED) {
                    input->stop();
                }
                {
                    const std::lock_guard<std::mutex> lock(this->mutex_);
                    this->counts_.frames += result.frames;
                    this->counts_.live_dropped += frames - result.frames;
                    this->counts_.latched += result.state == InputState::STOP_LATCHED ? 1 : 0;
                }
                due += std::chrono::milliseconds(10);
                if (Clock::now() - due > std::chrono::milliseconds(20)) {
                    due = Clock::now();
                }
                continue;
            }
            if (paused) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }
            if (track_left == 0) {
                // The next track, gapless (GroupInput::set_format())
                const InputFormat next = this->next_format();
                const bool changes = next != format;
                const bool changed = changes && input->set_format(next);
                if (changed) {
                    format = next;
                }
                track_left = uint64_t{format.sample_rate} * (3 + (this->random_() % 8));
                const std::lock_guard<std::mutex> lock(this->mutex_);
                ++this->counts_.tracks;
                this->counts_.format_changes += changed ? 1 : 0;
                this->counts_.refused_formats += changes && !changed ? 1 : 0;
            }
            const size_t frames = std::min<uint64_t>(format.sample_rate / 50, track_left);
            put_frames(block, position, frames, format.channels);
            const WriteResult result = input->write(block.data(), frames);
            position += result.frames;
            track_left -= result.frames;
            {
                const std::lock_guard<std::mutex> lock(this->mutex_);
                this->counts_.frames += result.frames;
            }
            if (result.frames < frames) {
                if (result.state == InputState::STOP_LATCHED) {
                    input->stop();
                    const std::lock_guard<std::mutex> lock(this->mutex_);
                    ++this->counts_.latched;
                } else if (result.state == InputState::CLOSED) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                } else {
                    input->wait_writable(20000);
                }
            }
        }
    }

    SendspinServer& server_;
    GroupId group_;
    std::mt19937_64 random_;
    mutable std::mutex mutex_;
    std::vector<std::pair<Command, int64_t>> commands_;
    Counts counts_;
    bool stopping_{false};
    // The host's thread only
    bool live_{false};
    std::thread thread_;
};

// ==============================================================================
// Listener
// ==============================================================================

/// Counts the listener calls the soak reports, and keeps the errors
class SoakListener : public sendspin::server::ServerListener {
public:
    void on_client_connected(const ClientInfo& /*client*/) override {
        ++this->connected;
    }

    void on_client_disconnected(const ClientInfo& /*client*/,
                                const sendspin::server::ClientDisconnect& disconnect) override {
        const std::lock_guard<std::mutex> lock(this->mutex);
        ++this->disconnects[static_cast<int>(disconnect.reason)];
    }

    void on_error(const ServerError& error) override {
        const std::lock_guard<std::mutex> lock(this->mutex);
        ++this->errors[static_cast<int>(error.kind)];
    }

    std::atomic<size_t> connected{0};
    std::mutex mutex;
    std::map<int, size_t> disconnects;
    std::map<int, size_t> errors;
};

// ==============================================================================
// The soak
// ==============================================================================

std::string group_key(GroupId group) {
    return std::to_string(group.value);
}

/// The script's state on its thread
class Soak {
public:
    Soak(int64_t seconds, uint64_t seed) : seconds_(seconds), random_(seed) {
        // A wide, a tall and a square image
        this->images_ = {solid_png(300, 200, 0xC0), solid_png(100, 300, 0x40),
                         solid_png(64, 64, 0x80)};
        ServerConfig config = sendspin::server::test::local_config();
        config.name = "Soak";
        config.approve_all_clients = true;
        config.enable_opus = true;
        config.timing.write_stall_us = WRITE_STALL_US;
        config.trace = [this](const TracedMessage& message) { this->wire_.on_message(message); };
        EXPECT_EQ(this->server_.start(config, &this->provider_, &this->listener_), StartResult::OK);
        this->origin_client_us_ = client_clock_us();
        this->origin_server_us_ = this->server_.now_us();
    }

    ~Soak() {
        this->raw_.reset();
        this->hosts_.clear();
        this->players_.clear();
        this->server_.stop();
    }

    Soak(const Soak&) = delete;
    Soak& operator=(const Soak&) = delete;

    void run() {
        const std::vector<Profile> all = profiles();
        // Every profile once, then the second group with the first player as its member
        for (const Profile& profile : all) {
            this->join(profile);
        }
        this->run_for(2 * SECOND_US);
        const std::optional<GroupId> home = this->server_.default_group();
        ASSERT_TRUE(home);
        this->groups_.push_back(*home);
        const std::string first = this->players_.front()->client_id;
        ASSERT_FALSE(first.empty());
        const std::optional<GroupId> kitchen = this->server_.create_group("Kitchen", first);
        ASSERT_TRUE(kitchen);
        this->groups_.push_back(*kitchen);
        this->run_until([&] { return this->members(*kitchen) == 1; });
        // Each group shows its own image from the start, so every move delivers one
        for (size_t i = 0; i < this->groups_.size(); ++i) {
            EXPECT_TRUE(this->server_.set_artwork(this->groups_[i],
                                                  sendspin::server::ArtworkSource::ALBUM,
                                                  this->images_[i], this->server_.now_us()));
        }
        this->raw_ = std::make_unique<RawPlayer>(this->server_, this->raw_closes_);
        for (const GroupId group : this->groups_) {
            this->hosts_.push_back(std::make_unique<Host>(this->server_, group, this->random_()));
        }
        this->run_for(3 * SECOND_US);

        const int64_t end = this->origin_server_us_ + (this->seconds_ * SECOND_US);
        int64_t next = this->server_.now_us();
        while (this->server_.now_us() < end) {
            this->run_for(50000);
            const int64_t now = this->server_.now_us();
            if ((now - this->origin_server_us_) / SECOND_US != this->marked_s_) {
                // A mark in the output, beside the players' log lines
                this->marked_s_ = (now - this->origin_server_us_) / SECOND_US;
                std::printf("[soak %lld s]\n", static_cast<long long>(this->marked_s_));
                std::fflush(stdout);
            }
            this->resume_due(now);
            if (now >= next) {
                this->step(now);
                next = now + 1500000 + static_cast<int64_t>(this->random_() % 2500000);
            }
        }
        // Back to the steady state, so the last events' streams end cleanly
        this->resume_due(INT64_MAX);
        if (this->live_until_) {
            this->go_buffered(this->server_.now_us());
        }
        this->run_for(3 * SECOND_US);
        this->ended_server_us_ = this->server_.now_us();
    }

    /// Ends the script's players and hosts, keeping what the analysis reads
    void finish() {
        for (const std::unique_ptr<CppPlayer>& player : this->players_) {
            if (!player->client_id.empty()) {
                this->outputs_.push_back(
                    {player->client_id, player->sink().seconds(), this->server_.now_us()});
            }
        }
        for (const std::unique_ptr<Host>& host : this->hosts_) {
            this->hosts_counts_.push_back(host->counts());
        }
        this->raw_.reset();
        this->hosts_.clear();
        this->players_.clear();
        this->server_.stop();
    }

    /// Prints the report and checks the outcome
    void report() {
        this->wire_.finish(this->ended_server_us_);
        const std::map<std::string, WireChecker::Stats> stats = this->wire_.stats();
        const std::vector<Event> events = this->events_.events();
        std::printf("\n==== Soak report: %lld s, seed %llu ====\n",
                    static_cast<long long>(this->seconds_),
                    static_cast<unsigned long long>(soak_seed()));
        std::printf("Events applied: %zu\n", events.size());
        for (const auto& [kind, count] : this->events_.counts()) {
            std::printf("  %-22s %zu\n", kind.c_str(), count);
        }
        for (size_t i = 0; i < this->hosts_counts_.size(); ++i) {
            const Host::Counts& c = this->hosts_counts_[i];
            std::printf(
                "Host %zu: %zu tracks, %zu set_format changes (%zu refused), %llu frames written, "
                "%zu live frames dropped, %zu stop latches\n",
                i, c.tracks, c.format_changes, c.refused_formats,
                static_cast<unsigned long long>(c.frames), c.live_dropped, c.latched);
        }
        std::printf("Raw player closed by the server and dialed again: %zu\n",
                    this->raw_closes_.load());
        std::printf("%-10s %-14s %7s %9s %5s %5s %4s %10s %10s %10s %5s %5s %7s %7s %5s %6s\n",
                    "client", "profile", "chunks", "kbytes", "strm", "clear", "swch", "min lead",
                    "max lead", "min first", "late", "live", "nostart", "shared", "fill%",
                    "max od");
        size_t total_chunks = 0;
        for (const auto& [id, s] : stats) {
            total_chunks += s.chunks;
            std::printf("%-10s %-14s %7zu %9zu %5zu %5zu %4zu %10.1f %10.1f %10.1f %5zu %5zu %7zu "
                        "%7zu %5lld %6lld\n",
                        id.substr(0, 10).c_str(), this->profile_of(id).c_str(), s.chunks,
                        s.bytes / 1000, s.streams, s.clears, s.switches,
                        s.chunks ? static_cast<double>(s.min_lead_us) / 1000 : 0.0,
                        s.chunks ? static_cast<double>(s.max_lead_us) / 1000 : 0.0,
                        s.min_first_lead_us == INT64_MAX
                            ? 0.0
                            : static_cast<double>(s.min_first_lead_us) / 1000,
                        s.late_skips, s.live_gaps, s.late_starts, s.shared_cells,
                        static_cast<long long>(s.max_fill_percent),
                        static_cast<long long>(s.max_od_us / 1000));
        }
        // A mismatch is excused only when one of the two players was triggered between the two
        // cells: two epochs' cells on the same microsecond, about one chance in 20,000 per epoch
        // change, so it is reported rather than failed
        std::printf("Chunks: %zu; cells compared across players of one format and group: %zu "
                    "(%zu ambiguous across epochs)\n",
                    total_chunks, shared_cells(stats), this->wire_.ambiguous_cells());
        {
            const std::lock_guard<std::mutex> lock(this->listener_.mutex);
            std::printf("Connected: %zu; disconnects by reason:", this->listener_.connected.load());
            for (const auto& [reason, count] : this->listener_.disconnects) {
                std::printf(" %d:%zu", reason, count);
            }
            std::printf("; errors by kind:");
            for (const auto& [kind, count] : this->listener_.errors) {
                std::printf(" %d:%zu", kind, count);
            }
            std::printf("\n");
        }
        std::printf("Artwork: %zu images decoded, %zu not a %u by %u JPEG; %zu colors received\n",
                    this->art_.images.load(), this->art_.wrong.load(), ART_SIDE, ART_SIDE,
                    this->art_.colors.load());
        size_t visualizer_messages = 0;
        for (const auto& [id, s] : stats) {
            visualizer_messages += s.visualizer_messages;
        }
        std::printf("Visualizer: %zu messages sent, %zu streams started and %zu frames delivered "
                    "on the sendspin-cpp visualizers\n",
                    visualizer_messages, this->visualizer_counts_.starts.load(),
                    this->visualizer_counts_.frames.load());
        EXPECT_GT(visualizer_messages, 0U);
        EXPECT_GT(this->visualizer_counts_.frames.load(), 0U);
        EXPECT_EQ(this->art_.wrong.load(), 0U);
        EXPECT_GE(this->art_.images.load(), this->players_.size());
        EXPECT_GT(this->art_.colors.load(), 0U);
        const Underruns underruns = this->underruns(events);
        std::printf("Underruns: %zu of %zu quiet player-seconds checked (%zu frames starved); "
                    "%zu seconds with a resync's silence (%zu frames)\n",
                    underruns.seconds, underruns.checked, underruns.starved, underruns.resynced,
                    underruns.silent);
        std::printf(
            "Outputs' threads: longest tick %lld ms; %zu checked seconds with one over 15 ms\n",
            static_cast<long long>(underruns.max_tick_us / 1000), underruns.late_ticks);
        for (const std::string& line : underruns.where) {
            std::printf("  %s\n", line.c_str());
        }
        const std::vector<std::string> anomalies = this->wire_.anomalies();
        std::printf("Wire anomalies: %zu", this->wire_.anomaly_count());
        for (const auto& [kind, count] : this->wire_.anomaly_kinds()) {
            std::printf("; %s: %zu", kind.c_str(), count);
        }
        std::printf("\n");
        for (const std::string& anomaly : anomalies) {
            std::printf("  %s\n", anomaly.c_str());
        }
        std::fflush(stdout);

        EXPECT_GE(this->max_players_, 3U);
        EXPECT_GT(total_chunks, 0U);
        EXPECT_GT(shared_cells(stats), 0U);
        EXPECT_EQ(this->wire_.anomaly_count(), 0U);
        EXPECT_GT(underruns.checked, 0U);
        EXPECT_EQ(underruns.seconds, 0U);
        this->expect_outcomes(stats, events);
    }

private:
    /// What the script must have exercised, and what the server and the players must have
    /// reported
    void expect_outcomes(const std::map<std::string, WireChecker::Stats>& stats,
                         const std::vector<Event>& events) {
        // A sendspin-cpp player gets its first chunk within FIRST_CHUNK_WITHIN_US of each trigger
        // and has no late audio skipped outside a live stretch; the raw player's stalls hold back
        // both by design
        bool delayed = false;
        for (const auto& [id, s] : stats) {
            if (id == this->raw_id_) {
                continue;
            }
            EXPECT_EQ(s.late_starts, 0U) << id;
            EXPECT_EQ(s.late_skips, 0U) << id;
            delayed = delayed || s.max_od_us > 0;
        }
        EXPECT_TRUE(delayed) << "no sendspin-cpp player reported an output delay";
        {
            const std::lock_guard<std::mutex> lock(this->listener_.mutex);
            EXPECT_EQ(this->listener_.disconnects.count(
                          static_cast<int>(sendspin::server::DisconnectReason::CLOSED_BY_SERVER)),
                      0U);
            for (const auto& [kind, count] : this->listener_.errors) {
                EXPECT_EQ(kind, static_cast<int>(sendspin::server::ErrorKind::DIAL_FAILED))
                    << count << " errors of kind " << kind;
            }
        }
        // A scripted stall past the write-stall timeout that ended in time closed the raw
        // player's connection
        const bool closing = std::any_of(events.begin(), events.end(), [&](const Event& event) {
            return event.kind == "raw-stall-closing" &&
                   event.end_us + (2 * SECOND_US) <= this->ended_server_us_;
        });
        if (closing) {
            EXPECT_GT(this->raw_closes_.load(), 0U);
        }
        // Enough of the script ran
        const std::map<std::string, size_t> counts = this->events_.counts();
        EXPECT_GE(events.size(), static_cast<size_t>(this->seconds_ / 5));
        if (this->seconds_ >= MIN_FULL_SCRIPT_S) {
            for (const char* kind : {"move", "seek", "pause", "live-begin", "raw-stall-closing"}) {
                EXPECT_TRUE(counts.contains(kind)) << "no " << kind << " event";
            }
        }
    }

    /// A sendspin-cpp player's output, kept when it left or the script ended
    struct Output {
        std::string client_id;
        std::vector<Second> seconds;
        int64_t left_us{0};
    };

    struct Underruns {
        int64_t max_tick_us{0};
        size_t late_ticks{0};
        size_t checked{0};
        /// Seconds whose output ran out of audio during the stream
        size_t seconds{0};
        /// Seconds in which the player filled a timing error with silence (a hard resync)
        size_t resynced{0};
        size_t starved{0};
        size_t silent{0};
        std::vector<std::string> where;
    };

    static size_t shared_cells(const std::map<std::string, WireChecker::Stats>& stats) {
        size_t shared = 0;
        for (const auto& [id, s] : stats) {
            shared += s.shared_cells;
        }
        return shared;
    }

    std::string profile_of(const std::string& id) const {
        if (this->raw_id_ == id) {
            return "raw";
        }
        for (const auto& [client, profile] : this->profiles_by_id_) {
            if (client == id) {
                return profile;
            }
        }
        return "?";
    }

    /// Each second a sendspin-cpp player streamed throughout, in one group, with no event touching
    /// it or its group and no live rebase landing from QUIET_BEFORE_US before to QUIET_AFTER_US
    /// after, must have played audio with no starved frame; else it is an underrun. Runs of silence
    /// there are the player's resynchronizations, counted but not failed: its sync task refills a
    /// timing error over 5 ms with silence, and its own threads and this output's run on a loaded
    /// host, where the server's part is checked from the wire.
    Underruns underruns(const std::vector<Event>& events) {
        Underruns result;
        const int64_t offset = this->origin_server_us_;
        for (const auto& [client_id, seconds, left_us] : this->outputs_) {
            const std::vector<WireChecker::Interval> streams = this->wire_.streams_of(client_id);
            const std::vector<WireChecker::Gap> gaps = this->wire_.gaps_of(client_id);
            for (size_t s = 0; s < seconds.size(); ++s) {
                const int64_t begin = offset + (static_cast<int64_t>(s) * SECOND_US);
                const int64_t end = begin + SECOND_US;
                if (end + QUIET_BEFORE_US > left_us) {
                    break;
                }
                const auto stream = std::find_if(
                    streams.begin(), streams.end(), [&](const WireChecker::Interval& interval) {
                        return interval.begin_us <= begin - QUIET_AFTER_US &&
                               interval.end_us >= end + QUIET_BEFORE_US;
                    });
                // A move ends the stream and starts another: the snapshot shows the group the
                // events name, within a pump of the change
                const std::optional<std::string> group = this->group_throughout(
                    client_id, begin - QUIET_AFTER_US, end + QUIET_BEFORE_US);
                if (stream == streams.end() || !group) {
                    continue;
                }
                const bool touched =
                    std::any_of(events.begin(), events.end(), [&](const Event& event) {
                        if (!event.client.empty() && event.client != client_id &&
                            event.group.empty()) {
                            return false;
                        }
                        if (!event.group.empty() && event.group != *group &&
                            event.client != client_id) {
                            return false;
                        }
                        const int64_t after =
                            event.end_us == INT64_MAX ? INT64_MAX : event.end_us + QUIET_AFTER_US;
                        return event.begin_us - QUIET_BEFORE_US < end && after > begin;
                    });
                // A live rebase plays as silence where it lands
                const bool gap =
                    std::any_of(gaps.begin(), gaps.end(), [&](const WireChecker::Gap& g) {
                        return g.kind == "live" && g.ts - QUIET_BEFORE_US < end &&
                               g.ts + QUIET_AFTER_US > begin;
                    });
                if (touched || gap) {
                    continue;
                }
                ++result.checked;
                const Second& second = seconds[s];
                result.max_tick_us = std::max(result.max_tick_us, second.max_tick_us);
                if (second.max_tick_us > 15000) {
                    ++result.late_ticks;
                }
                result.starved += second.starved;
                result.silent += second.silent;
                if (second.silent > 0) {
                    ++result.resynced;
                }
                // An output that ran out, or played nothing audible all second
                const bool underrun = second.starved > 0 || second.audible == 0;
                if (underrun || second.silent > 0) {
                    if (underrun) {
                        ++result.seconds;
                    }
                    if (result.where.size() < 20) {
                        std::string line =
                            client_id.substr(0, 10) + " in group " + *group + " at " +
                            std::to_string(s) + " s: " + std::to_string(second.starved) +
                            " starved, " + std::to_string(second.audible) + " audible, " +
                            std::to_string(second.silent) + " silent; output tick max " +
                            std::to_string(second.max_tick_us / 1000) + " ms, " +
                            std::to_string(s > 0 ? seconds[s - 1].max_tick_us / 1000 : 0) +
                            " ms before; events near:";
                        for (const Event& event : events) {
                            if (event.begin_us > begin - 10 * SECOND_US &&
                                event.begin_us < end + 10 * SECOND_US) {
                                line += " " + event.kind + "@" +
                                        std::to_string((event.begin_us - offset) / 1000) + "ms[" +
                                        event.group + "/" + event.client.substr(0, 4) + "]";
                            }
                        }
                        line += "; gaps near:";
                        for (const WireChecker::Gap& gap : gaps) {
                            if (gap.stamp_us > begin - 10 * SECOND_US &&
                                gap.stamp_us < end + 10 * SECOND_US) {
                                line += " " + gap.kind + " " + std::to_string(gap.us / 1000) +
                                        "ms@" + std::to_string((gap.stamp_us - offset) / 1000) +
                                        "ms";
                            }
                        }
                        result.where.push_back(line);
                    }
                }
            }
        }
        return result;
    }

    void loop_players() {
        for (const std::unique_ptr<CppPlayer>& player : this->players_) {
            player->loop();
        }
    }

    template <typename Predicate>
    void run_until(Predicate done) {
        sendspin::server::test::poll_until([this] { this->loop_players(); }, done);
    }

    void run_for(int64_t us) {
        const int64_t until = this->server_.now_us() + us;
        this->run_until([&] {
            this->resolve_ids();
            return this->server_.now_us() >= until;
        });
    }

    /// Notes each client's group as groups() lists it, when it changes
    void note_groups() {
        const int64_t now = this->server_.now_us();
        for (const GroupInfo& info : this->server_.groups()) {
            for (const std::string& client : info.client_ids) {
                std::vector<std::pair<int64_t, std::string>>& groups = this->membership_[client];
                if (groups.empty() || groups.back().second != group_key(info.id)) {
                    groups.emplace_back(now, group_key(info.id));
                }
            }
        }
    }

    /// client_id's group throughout [begin_us, end_us), or none when it changed or is not known
    std::optional<std::string> group_throughout(const std::string& client_id, int64_t begin_us,
                                                int64_t end_us) const {
        const auto it = this->membership_.find(client_id);
        if (it == this->membership_.end()) {
            return std::nullopt;
        }
        std::optional<std::string> group;
        for (const auto& [at, name] : it->second) {
            if (at <= begin_us) {
                group = name;
            } else if (at < end_us) {
                return std::nullopt;
            }
        }
        return group;
    }

    void resolve_ids() {
        this->note_groups();
        const std::vector<ClientInfo> clients = this->server_.clients();
        for (const std::unique_ptr<CppPlayer>& player : this->players_) {
            if (!player->client_id.empty()) {
                continue;
            }
            for (const ClientInfo& client : clients) {
                if (client.url == player->url() && client.connected) {
                    player->client_id = client.client_id;
                    this->profiles_by_id_.emplace_back(client.client_id, player->profile().name);
                }
            }
        }
        if (this->raw_ && this->raw_id_.empty()) {
            for (const ClientInfo& client : clients) {
                if (client.url == this->raw_->url()) {
                    this->raw_id_ = client.client_id;
                }
            }
        }
    }

    size_t members(GroupId group) const {
        for (const GroupInfo& info : this->server_.groups()) {
            if (info.id == group) {
                return info.client_ids.size();
            }
        }
        return 0;
    }

    std::optional<GroupId> group_of(const std::string& client_id) const {
        for (const GroupInfo& info : this->server_.groups()) {
            if (std::find(info.client_ids.begin(), info.client_ids.end(), client_id) !=
                info.client_ids.end()) {
                return info.id;
            }
        }
        return std::nullopt;
    }

    void join(const Profile& profile) {
        auto player = std::make_unique<CppPlayer>(profile, this->origin_client_us_,
                                                  static_cast<size_t>(this->seconds_) + 30,
                                                  this->art_, this->visualizer_counts_);
        EXPECT_TRUE(this->server_.connect_url(player->url()));
        this->players_.push_back(std::move(player));
        this->max_players_ = std::max(this->max_players_, this->players_.size());
        this->run_until([&] {
            this->resolve_ids();
            return !this->players_.back()->client_id.empty();
        });
        this->note({"join", this->server_.now_us(), this->server_.now_us(), "",
                    this->players_.back()->client_id});
    }

    /// The player leaves: its output's record is kept for the analysis
    void leave(size_t index) {
        CppPlayer& player = *this->players_[index];
        const std::optional<GroupId> group = group_of(player.client_id);
        if (!group || (*group != this->groups_[0] && this->members(*group) <= 1)) {
            return;
        }
        const int64_t now = this->server_.now_us();
        this->note({"leave", now, now, "", player.client_id});
        this->outputs_.push_back({player.client_id, player.sink().seconds(), now});
        this->players_.erase(this->players_.begin() + static_cast<std::ptrdiff_t>(index));
    }

    /// Logs event, and marks it in the output beside the players' log lines
    void note(Event event) {
        std::printf("[soak %.3f s: %s group %s client %s]\n",
                    static_cast<double>(event.begin_us - this->origin_server_us_) / SECOND_US,
                    event.kind.c_str(), event.group.c_str(), event.client.substr(0, 10).c_str());
        std::fflush(stdout);
        this->events_.add(std::move(event));
    }

    /// The group of client_id while its input is live, where a player's lead or pipeline latency
    /// moves the whole group's timeline; else none
    std::string live_scope(const std::string& client_id) const {
        const std::optional<GroupId> group = group_of(client_id);
        return this->live_until_ && group && *group == this->groups_[1] ? group_key(*group)
                                                                        : std::string{};
    }

    void go_buffered(int64_t now) {
        this->hosts_[1]->command(Host::Command::GO_BUFFERED);
        this->wire_.set_live(group_key(this->groups_[1]), false, now);
        this->note({"live-end", now, now, group_key(this->groups_[1]), ""});
        this->live_until_.reset();
    }

    void resume_due(int64_t now) {
        for (auto it = this->resume_at_.begin(); it != this->resume_at_.end();) {
            if (it->second > now) {
                ++it;
                continue;
            }
            const size_t index = it->first;
            this->hosts_[index]->command(Host::Command::RESUME);
            this->events_.end_last("pause", group_key(this->groups_[index]),
                                   std::min(now, this->server_.now_us()));
            it = this->resume_at_.erase(it);
        }
        if (this->live_until_ && *this->live_until_ <= now) {
            this->go_buffered(this->server_.now_us());
        }
    }

    /// One random event
    void step(int64_t now) {
        const uint64_t roll = this->random_() % 100;
        const size_t g = this->random_() % this->groups_.size();
        const GroupId group = this->groups_[g];
        const std::string group_name = group_key(group);
        if (roll < 8) {
            if (this->players_.size() < 6) {
                const std::vector<Profile> all = profiles();
                this->join(all[this->random_() % all.size()]);
            }
        } else if (roll < 16) {
            if (this->players_.size() > 3) {
                this->leave(this->random_() % this->players_.size());
            }
        } else if (roll < 30) {
            CppPlayer& player = *this->players_[this->random_() % this->players_.size()];
            const std::optional<GroupId> from = group_of(player.client_id);
            if (from && (*from == this->groups_[0] || this->members(*from) > 1)) {
                const GroupId to = *from == this->groups_[0] ? this->groups_[1] : this->groups_[0];
                if (this->server_.move_client(player.client_id, to)) {
                    this->note({"move", now, now, group_key(*from), player.client_id});
                    this->note({"move", now, now, group_key(to), player.client_id});
                }
            }
        } else if (roll < 40) {
            if ((g != 1 || !this->live_until_) && !this->resume_at_.contains(g)) {
                this->hosts_[g]->command(Host::Command::SEEK);
                this->note({"seek", now, now, group_name, ""});
            }
        } else if (roll < 48) {
            if (!this->resume_at_.contains(g)) {
                this->hosts_[g]->command(Host::Command::PAUSE);
                this->resume_at_[g] =
                    now + 500000 + static_cast<int64_t>(this->random_() % 2000000);
                this->note({"pause", now, INT64_MAX, group_name, ""});
            }
        } else if (roll < 58) {
            CppPlayer& player = *this->players_[this->random_() % this->players_.size()];
            const auto delay = static_cast<uint16_t>(this->random_() % 300);
            player.role().update_output_delay(delay);
            this->note(
                {"output-delay", now, now, this->live_scope(player.client_id), player.client_id});
        } else if (roll < 66) {
            CppPlayer& player = *this->players_[this->random_() % this->players_.size()];
            const std::vector<sendspin::AudioSupportedFormatObject>& formats =
                player.profile().formats;
            const size_t pick = this->random_() % (formats.size() + 1);
            std::optional<StreamFormat> format;
            if (pick < formats.size()) {
                const sendspin::AudioSupportedFormatObject& f = formats[pick];
                format = StreamFormat{f.codec == Codec::FLAC ? StreamCodec::FLAC : StreamCodec::PCM,
                                      f.sample_rate, f.channels, f.bit_depth};
            }
            if (this->server_.set_player_format(player.client_id, format)) {
                this->note({"set-player-format", now, now, this->live_scope(player.client_id),
                            player.client_id});
            }
        } else if (roll < 74) {
            const uint32_t min_buffer = 100 + static_cast<uint32_t>(this->random_() % 900);
            const uint32_t lead = static_cast<uint32_t>(this->random_() % 1000);
            const uint32_t od = static_cast<uint32_t>(this->random_() % 300);
            this->raw_->set_timing(min_buffer, lead, od);
            // A lead the raw player raises reaches its whole group
            const std::optional<GroupId> raw_group = group_of(this->raw_id_);
            this->note({"raw-timing", now, now, raw_group ? group_key(*raw_group) : std::string{},
                        this->raw_id_});
        } else if (roll < 80) {
            if (!this->live_until_ && !this->resume_at_.contains(1)) {
                this->hosts_[1]->command(Host::Command::GO_LIVE);
                this->wire_.set_live(group_key(this->groups_[1]), true, now);
                this->live_until_ =
                    now + 20000000 + static_cast<int64_t>(this->random_() % 20000000);
                this->note({"live-begin", now, now, group_key(this->groups_[1]), ""});
            } else if (this->live_until_) {
                const int64_t stall = 200000 + static_cast<int64_t>(this->random_() % 600000);
                this->hosts_[1]->command(Host::Command::STALL, stall);
                this->note({"live-stall", now, now + stall, group_key(this->groups_[1]), ""});
            }
        } else if (roll < 88) {
            // Neither is an event for the underrun check: the output is not scaled, and artwork
            // and colors carry no audio
            const uint64_t pick = this->random_() % 4;
            if (pick == 0) {
                this->server_.set_group_volume(group, static_cast<uint32_t>(this->random_() % 101));
                this->note({"volume", now, now, "-", ""});
            } else if (pick == 1) {
                this->server_.set_group_mute(group, this->random_() % 2 == 0);
                this->note({"mute", now, now, "-", ""});
            } else if (pick == 2) {
                // An image now or up to 30 s ahead, so some are announced ahead and some replaced
                // before they are; none at times
                const size_t image = this->random_() % (this->images_.size() + 1);
                const int64_t at = now + static_cast<int64_t>(this->random_() % 3) *
                                             static_cast<int64_t>(this->random_() % 15000000);
                const std::vector<uint8_t> none;
                EXPECT_TRUE(this->server_.set_artwork(
                    group, sendspin::server::ArtworkSource::ALBUM,
                    image < this->images_.size() ? this->images_[image] : none, at));
                this->note({"artwork", now, now, "-", ""});
            } else {
                sendspin::server::Colors colors;
                colors.primary = sendspin::server::RgbColor{static_cast<uint8_t>(this->random_()),
                                                            static_cast<uint8_t>(this->random_()),
                                                            static_cast<uint8_t>(this->random_())};
                colors.background_dark = colors.primary;
                EXPECT_TRUE(this->server_.set_colors(group, colors, now));
                this->note({"colors", now, now, "-", ""});
            }
        } else {
            // A player that stops reading; past the write-stall timeout at times
            // The first of every four
            const bool past_timeout = this->raw_stalls_++ % 4 == 0;
            const int64_t stall = past_timeout
                                      ? WRITE_STALL_US + LONG_STALL_MARGIN_US
                                      : 300000 + static_cast<int64_t>(this->random_() % 2500000);
            this->raw_->stall(stall);
            this->note({past_timeout ? "raw-stall-closing" : "raw-stall", now, now + stall, "",
                        this->raw_id_});
        }
    }

    int64_t seconds_;
    std::mt19937_64 random_;
    int64_t origin_client_us_{0};
    int64_t origin_server_us_{0};
    int64_t ended_server_us_{0};
    // Declared before the server, whose trace and listener call them until it stops
    WireChecker wire_;
    /// Declared before the players, which count into it
    ArtCounts art_;
    VisualizerCounts visualizer_counts_;
    std::vector<std::vector<uint8_t>> images_;
    SoakListener listener_;
    EventLog events_;
    sendspin::server::test::MemoryProvider provider_;
    SendspinServer server_;
    std::vector<GroupId> groups_;
    std::vector<std::unique_ptr<CppPlayer>> players_;
    std::vector<std::pair<std::string, std::string>> profiles_by_id_;
    std::vector<Output> outputs_;
    std::map<std::string, std::vector<std::pair<int64_t, std::string>>> membership_;
    std::unique_ptr<RawPlayer> raw_;
    std::string raw_id_;
    std::atomic<size_t> raw_closes_{0};
    std::vector<std::unique_ptr<Host>> hosts_;
    std::vector<Host::Counts> hosts_counts_;
    std::map<size_t, int64_t> resume_at_;
    std::optional<int64_t> live_until_;
    size_t max_players_{0};
    int64_t marked_s_{0};
    size_t raw_stalls_{0};
};

TEST(Soak, PlaysThroughAScriptOfEvents) {
    const std::optional<int64_t> seconds = soak_seconds();
    if (!seconds) {
        GTEST_SKIP() << "set SENDSPIN_SERVER_SOAK_SECONDS to run the soak";
    }
    std::printf("Soak: %lld s, seed %llu\n", static_cast<long long>(*seconds),
                static_cast<unsigned long long>(soak_seed()));
    std::fflush(stdout);
    Soak soak(*seconds, soak_seed());
    soak.run();
    soak.finish();
    soak.report();
}

}  // namespace

// The ASan runtime's stack-use-after-return detection slows the real-time paths under GCC, as in
// tests/main.cpp
extern "C" const char* __asan_default_options() {
    return "detect_stack_use_after_return=0";
}

int main(int argc, char** argv) {
    testing::InitGoogleTest(&argc, argv);
    // A hang aborts the run: the script's length, its setup and teardown, and a margin
    const int64_t budget_s = soak_seconds().value_or(0) + 180;
    std::thread([budget_s] {
        std::this_thread::sleep_for(std::chrono::seconds(budget_s));
        std::fprintf(stderr, "\nSoak watchdog: still running after %lld s\n",
                     static_cast<long long>(budget_s));
        std::fflush(stderr);
        std::abort();
    }).detach();
    return RUN_ALL_TESTS();
}
