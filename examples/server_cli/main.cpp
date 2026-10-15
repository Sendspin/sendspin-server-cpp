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

/// @file main.cpp
/// @brief server_cli: a command-line host for SendspinServer that advertises the server, dials the
/// clients it finds or is given a URL for, groups them, plays WAV and FLAC files to each group, or
/// raw PCM read from standard input as a live input, and prints the server's events and, on
/// request, every message it sends and receives

#include "audio_source.h"
#include "console.h"
#include "feeder.h"
#include "file_persistence_provider.h"
#include "live_feeder.h"
#include "one_group.h"
#include "timing_log.h"
#include "timing_stats.h"
#include "web_ui.h"
#include <poll.h>
#include <pthread.h>
#include <unistd.h>

#include <sendspin/server/discovery.h>
#include <sendspin/server/server.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

using sendspin::server::ApprovalScope;
using sendspin::server::ArtworkSource;
using sendspin::server::ClientDisconnect;
using sendspin::server::ClientInfo;
using sendspin::server::CodeFormat;
using sendspin::server::Colors;
using sendspin::server::CommandArgs;
using sendspin::server::ControllerCaps;
using sendspin::server::DisconnectReason;
using sendspin::server::DiscoveredClient;
using sendspin::server::DiscoveryProvider;
using sendspin::server::DiscoveryRequest;
using sendspin::server::DiscoverySink;
using sendspin::server::DrainId;
using sendspin::server::DrainOutcome;
using sendspin::server::ErrorKind;
using sendspin::server::GoodbyeReason;
using sendspin::server::GroupId;
using sendspin::server::GroupInfo;
using sendspin::server::GroupInput;
using sendspin::server::InputId;
using sendspin::server::InputState;
using sendspin::server::MAX_VOLUME;
using sendspin::server::Metadata;
using sendspin::server::MetadataProgress;
using sendspin::server::OpenInputResult;
using sendspin::server::PairingOptions;
using sendspin::server::PairingResult;
using sendspin::server::PairMethod;
using sendspin::server::PlaybackState;
using sendspin::server::RgbColor;
using sendspin::server::SampleFormat;
using sendspin::server::SendspinServer;
using sendspin::server::ServerError;
using sendspin::server::SourceSignal;
using sendspin::server::StreamCodec;
using sendspin::server::StreamFormat;
using sendspin::server::TracedMessage;
using sendspin::server::TransportCommand;
using sendspin::server::examples::AudioInfo;
using sendspin::server::examples::AudioSource;
using sendspin::server::examples::Console;
using sendspin::server::examples::Feeder;
using sendspin::server::examples::LiveFeeder;
using sendspin::server::examples::TimingLog;
using sendspin::server::examples::TimingStats;

const char* to_string(DisconnectReason reason) {
    switch (reason) {
        case DisconnectReason::GOODBYE:
            return "goodbye";
        case DisconnectReason::CONNECTION_LOST:
            return "connection lost";
        case DisconnectReason::CLOSED_BY_SERVER:
            return "closed by server";
        case DisconnectReason::SERVER_STOPPED:
            return "server stopped";
        case DisconnectReason::DISCONNECTED_BY_HOST:
            return "disconnected by host";
    }
    return "unknown";
}

const char* to_string(ErrorKind kind) {
    switch (kind) {
        case ErrorKind::DIAL_FAILED:
            return "dial failed";
        case ErrorKind::DISCOVERY_UNAVAILABLE:
            return "mDNS unavailable (on Linux, is avahi-daemon running?); clients connect by "
                   "address and connect <ws-url> dials";
        case ErrorKind::NO_PLAYABLE_FORMAT:
            return "no playable format";
        case ErrorKind::GOODBYE_BEFORE_ACTIVATION:
            return "goodbye before activation";
        case ErrorKind::HANDSHAKE_FAILED:
            return "handshake failed";
        case ErrorKind::PIPELINE_FAILED:
            return "pipeline failed";
        case ErrorKind::PERSISTENCE_FAILED:
            return "client record not saved";
        case ErrorKind::SOURCE_FAILED:
            return "source stream not taken";
    }
    return "unknown";
}

const char* to_string(StreamCodec codec) {
    switch (codec) {
        case StreamCodec::PCM:
            return "pcm";
        case StreamCodec::FLAC:
            return "flac";
        case StreamCodec::OPUS:
            return "opus";
    }
    return "unknown";
}

const char* to_string(GoodbyeReason reason) {
    switch (reason) {
        case GoodbyeReason::ANOTHER_SERVER:
            return "another_server";
        case GoodbyeReason::SHUTDOWN:
            return "shutdown";
        case GoodbyeReason::RESTART:
            return "restart";
        case GoodbyeReason::USER_REQUEST:
            return "user_request";
        case GoodbyeReason::UNAUTHORIZED:
            return "unauthorized";
        case GoodbyeReason::PAIRING_REQUIRED:
            return "pairing_required";
        case GoodbyeReason::CONCURRENT_ATTEMPT:
            return "concurrent_attempt";
        case GoodbyeReason::UNPAIRED:
            return "unpaired";
    }
    return "unknown";
}

const char* to_string(PairingResult result) {
    switch (result) {
        case PairingResult::PAIRED:
            return "paired";
        case PairingResult::CANCELLED:
            return "cancelled";
        case PairingResult::CODE_MISMATCH:
            return "the code did not match";
        case PairingResult::TOKEN_MISMATCH:
            return "the client could not use the token";
        case PairingResult::METHOD_NOT_SUPPORTED:
            return "the client does not support the method";
        case PairingResult::CLIENT_TIMED_OUT:
            return "the client timed out";
        case PairingResult::CLIENT_CANCELLED:
            return "cancelled on the client";
        case PairingResult::CONCURRENT_ATTEMPT:
            return "the client is pairing with another server";
        case PairingResult::TIMED_OUT:
            return "the client did not start";
        case PairingResult::PERSISTENCE_FAILED:
            return "the pairing could not be saved";
        case PairingResult::PROTOCOL_ERROR:
            return "the client broke the protocol";
        case PairingResult::CONNECTION_LOST:
            return "the connection was lost";
        case PairingResult::SERVER_STOPPED:
            return "the server stopped";
    }
    return "unknown";
}

const char* to_string(TransportCommand command) {
    switch (command) {
        case TransportCommand::PLAY:
            return "play";
        case TransportCommand::PAUSE:
            return "pause";
        case TransportCommand::STOP:
            return "stop";
        case TransportCommand::NEXT:
            return "next";
        case TransportCommand::PREVIOUS:
            return "previous";
        case TransportCommand::REPEAT_OFF:
            return "repeat_off";
        case TransportCommand::REPEAT_ONE:
            return "repeat_one";
        case TransportCommand::REPEAT_ALL:
            return "repeat_all";
        case TransportCommand::SHUFFLE:
            return "shuffle";
        case TransportCommand::UNSHUFFLE:
            return "unshuffle";
        case TransportCommand::SEEK:
            return "seek";
        case TransportCommand::SEEK_RELATIVE:
            return "seek_relative";
    }
    return "unknown";
}

/// A device's text as plain printable ASCII: spec pairing.md "client/pair-pending" has it shown as
/// text, so a control character or escape sequence in it is replaced
std::string printable(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    constexpr unsigned char FIRST_PRINTABLE = ' ';
    constexpr unsigned char LAST_PRINTABLE = '~';
    for (const char c : text) {
        const auto byte = static_cast<unsigned char>(c);
        out.push_back(byte >= FIRST_PRINTABLE && byte <= LAST_PRINTABLE ? c : '?');
    }
    return out;
}

/// How the client is reached and whether it is connected now
std::string reach(const ClientInfo& client) {
    std::string text = client.connected ? "connected" : "not connected";
    if (client.accepted) {
        text += ", connected to the server from " + client.address;
        return text;
    }
    text += client.discovered ? ", found by browsing at " : ", dialed at ";
    text += client.url;
    if (!client.connected) {
        text += client.reconnecting ? ", dialed again on its own" : ", dialed again on connect";
    }
    return text;
}

/// The roles the client may be activated with, for a client that has connected
std::string roles(const ClientInfo& client) {
    std::string text;
    for (const std::string& role : client.roles) {
        text += (text.empty() ? "  roles: " : ", ") + role;
    }
    return text;
}

/// Browses through the built-in provider but passes on only the clients whose name (TXT `name`,
/// else the instance name) starts with a prefix, so the server dials no other device it finds
class NameFilter : public DiscoveryProvider, private DiscoverySink {
public:
    NameFilter(std::shared_ptr<DiscoveryProvider> inner, std::string prefix)
        : inner_(std::move(inner)), prefix_(std::move(prefix)) {}

    bool start(const DiscoveryRequest& request, DiscoverySink& sink) override {
        this->sink_ = &sink;
        return this->inner_ && this->inner_->start(request, *this);
    }

    void stop() override {
        this->inner_->stop();
    }

private:
    /// On the provider's discovery thread, as every call of this sink
    void on_client_found(const DiscoveredClient& client) override {
        if (client.name.value_or(client.instance).starts_with(this->prefix_)) {
            this->passed_.insert(client.instance);
            this->sink_->on_client_found(client);
        } else if (this->passed_.erase(client.instance) > 0) {
            this->sink_->on_client_lost(client.instance);
        }
    }

    void on_client_lost(const std::string& instance) override {
        if (this->passed_.erase(instance) > 0) {
            this->sink_->on_client_lost(instance);
        }
    }

    void on_unavailable() override {
        this->passed_.clear();
        this->sink_->on_unavailable();
    }

    void on_available() override {
        this->sink_->on_available();
    }

    std::shared_ptr<DiscoveryProvider> inner_;
    /// The instances passed on, so one renamed out of the prefix is reported lost
    std::set<std::string> passed_;
    std::string prefix_;
    DiscoverySink* sink_{nullptr};
};

/// A client's trust as the operator needs to see it (spec pairing.md "Unpaired Access"): an
/// unapproved client is told apart from paired and approved ones
std::string trust(const ClientInfo& client) {
    if (client.pairing) {
        return "PAIRING";
    }
    if (client.credential_mismatch) {
        return "PAIRING LOST";
    }
    if (client.paired) {
        return "paired";
    }
    if (!client.unpaired_access) {
        return "pairing required";
    }
    if (!client.approval) {
        return "NOT APPROVED";
    }
    return *client.approval == ApprovalScope::PLAYBACK ? "approved" : "approved with source";
}

const char* to_string(DrainOutcome outcome) {
    switch (outcome) {
        case DrainOutcome::PLAYED:
            return "played";
        case DrainOutcome::FLUSHED:
            return "flushed";
        case DrainOutcome::STOPPED:
            return "stopped";
    }
    return "unknown";
}

const char* to_string(SampleFormat format) {
    switch (format) {
        case SampleFormat::S16:
            return "16-bit";
        case SampleFormat::S24:
            return "24-bit";
        case SampleFormat::F32:
            return "32-bit float";
    }
    return "unknown";
}

const char* to_string(InputState state) {
    switch (state) {
        case InputState::OPEN:
            return "open";
        case InputState::FULL:
            return "full";
        case InputState::DRAINING:
            return "draining";
        case InputState::STOP_LATCHED:
            return "stopped by the server";
        case InputState::CLOSED:
            return "closed";
    }
    return "unknown";
}

const char* to_string(Feeder::Phase phase) {
    switch (phase) {
        case Feeder::Phase::FEEDING:
            return "playing";
        case Feeder::Phase::DRAINING:
            return "playing out";
        case Feeder::Phase::FINISHED:
            return "finished";
        case Feeder::Phase::STOPPED:
            return "stopped by the server";
        case Feeder::Phase::CLOSED:
            return "input closed";
    }
    return "unknown";
}

constexpr int64_t US_PER_MS = 1000;
constexpr int DECIMAL_BASE = 10;
constexpr double US_PER_S = 1e6;
constexpr double BYTES_PER_KB = 1000.0;

/// frames at rate, as seconds with two decimals
std::string seconds(uint64_t frames, uint32_t rate) {
    char text[32];
    std::snprintf(text, sizeof(text), "%.2f s", static_cast<double>(frames) / rate);
    return text;
}

std::string describe(const GroupInfo& group) {
    return "group " + std::to_string(group.id.value) + " (" + group.name +
           "): " + (group.playback_state == PlaybackState::PLAYING ? "playing" : "stopped") + ", " +
           std::to_string(group.client_ids.size()) + " member(s)";
}

/// @brief The file playing in each group, if any. Every call into a feeder holds the mutex, so a
/// command and the dispatch thread's on_drained() never meet a feeder being destroyed.
class NowPlaying {
public:
    /// Stops and joins the group's feeder replaced, outside the lock
    void replace(GroupId group, std::unique_ptr<Feeder> feeder) {
        std::unique_ptr<Feeder> old;
        {
            const std::lock_guard<std::mutex> lock(this->mutex_);
            std::unique_ptr<Feeder>& slot = this->feeders_[group];
            old = std::exchange(slot, std::move(feeder));
            if (!slot) {
                this->feeders_.erase(group);
            }
        }
    }

    /// Stops and joins every feeder, outside the lock
    void clear() {
        std::map<GroupId, std::unique_ptr<Feeder>> old;
        {
            const std::lock_guard<std::mutex> lock(this->mutex_);
            old.swap(this->feeders_);
        }
    }

    /// @return false when nothing is playing in the group
    template <typename Fn>
    bool with(GroupId group, Fn&& fn) {
        const std::lock_guard<std::mutex> lock(this->mutex_);
        const auto it = this->feeders_.find(group);
        if (it == this->feeders_.end()) {
            return false;
        }
        std::forward<Fn>(fn)(*it->second);
        return true;
    }

private:
    std::mutex mutex_;
    std::map<GroupId, std::unique_ptr<Feeder>> feeders_;
};

/// One trace line: the direction, the connection, and the message's JSON, or the size of a
/// binary message other than JSON
std::string describe(const TracedMessage& message) {
    std::ostringstream line;
    line << (message.direction == sendspin::server::TraceDirection::INBOUND ? "<< " : ">> ") << "["
         << message.connection_id << "] ";
    std::string_view body(reinterpret_cast<const char*>(message.plaintext.data()),
                          message.plaintext.size());
    // A binary message's first byte is its message ID; ID 0 carries JSON
    if (message.binary && !body.empty() && body.front() == 0) {
        body.remove_prefix(1);
    } else if (message.binary) {
        line << "binary message, " << body.size() << " bytes";
        return line.str();
    }
    line << body;
    return line.str();
}

const char* to_string(OpenInputResult result) {
    switch (result) {
        case OpenInputResult::OK:
            return "ok";
        case OpenInputResult::NOT_RUNNING:
            return "the server is not running";
        case OpenInputResult::UNKNOWN_GROUP:
            return "no such group";
        case OpenInputResult::INVALID_FORMAT:
            return "a format the server does not take";
        case OpenInputResult::INVALID_JITTER:
            return "a write jitter the server does not take";
        case OpenInputResult::GROUP_BUSY:
            return "the group holds another input";
    }
    return "unknown";
}

const char* to_string(sendspin::server::RouteSourceResult result) {
    using sendspin::server::RouteSourceResult;
    switch (result) {
        case RouteSourceResult::OK:
            return "ok";
        case RouteSourceResult::NOT_RUNNING:
            return "the server is not running";
        case RouteSourceResult::INVALID_CLIENT:
            return "not a client_id";
        case RouteSourceResult::UNKNOWN_GROUP:
            return "no such group";
        case RouteSourceResult::INVALID_JITTER:
            return "a write jitter the server does not take";
        case RouteSourceResult::GROUP_BUSY:
            return "the group holds another input";
        case RouteSourceResult::INVALID_LATENCY:
            return "a least latency the server does not take";
    }
    return "unknown";
}

/// A stream's format, as "pcm 48000 Hz, 2 channel(s), 16 bits"
std::string format_text(const StreamFormat& format) {
    return std::string(to_string(format.codec)) + " " + std::to_string(format.sample_rate) +
           " Hz, " + std::to_string(format.channels) + " channel(s), " +
           std::to_string(format.bit_depth) + " bits";
}

/// The group, as groups() lists it now
std::optional<GroupInfo> group_info(const SendspinServer& server, GroupId id) {
    for (GroupInfo& group : server.groups()) {
        if (group.id == id) {
            return std::move(group);
        }
    }
    return std::nullopt;
}

/// path with a leading `~` or `~/` replaced by $HOME, as a shell would
std::string expand_home(const std::string& path) {
    const char* home = std::getenv("HOME");
    if (home == nullptr || path.empty() || path[0] != '~' || (path.size() > 1 && path[1] != '/')) {
        return path;
    }
    return std::string(home) + path.substr(1);
}

constexpr int64_t MS_PER_S = 1000;
/// --timing-interval: its default and least value, in seconds
constexpr double DEFAULT_TIMING_INTERVAL_S = 5.0;
constexpr double MIN_TIMING_INTERVAL_S = 0.1;
/// --timing-interval's largest value, a day
constexpr double MAX_TIMING_INTERVAL_S = 86400.0;

/// The files playing in each group, with the controller commands and the metadata they give the
/// group. Called from the REPL and, for controllers' commands, from the dispatch thread.
class Playback {
public:
    Playback(Console& console, NowPlaying& now_playing)
        : console_(console), now_playing_(now_playing) {}

    /// Before the server starts
    void attach(SendspinServer& server) {
        this->server_ = &server;
    }

    /// Before the server starts: the feeder of standard input (--stdin), which pause, resume, stop
    /// and status act on in the group it plays to
    void attach_live(LiveFeeder& live) {
        this->live_ = &live;
    }

    /// Plays standard input to group as a live input, stopping it where it played before
    /// @return whether it plays
    bool live(GroupId group);

    /// A drain's outcome, for the feeder of standard input
    void on_live_drained(InputId input, DrainId drain, DrainOutcome outcome) {
        if (this->live_ != nullptr) {
            this->live_->on_drained(input, drain, outcome);
        }
    }

    /// The live feeder's status in group; nullopt when it plays elsewhere or not at all
    std::optional<LiveFeeder::Status> live_status(GroupId group) const {
        return this->live_ != nullptr ? this->live_->status(group) : std::nullopt;
    }

    /// A live feeder's report, on its thread, the dispatch thread (a drain's outcome) or the
    /// thread that stopped it (LiveFeeder::detach()); it calls no feeder
    void on_live_event(GroupId group, LiveFeeder::Event event) {
        // GroupId{} when no input was attached
        const std::string prefix =
            group.value == 0 ? "" : "group " + std::to_string(group.value) + ": ";
        switch (event) {
            case LiveFeeder::Event::END_OF_INPUT:
                this->console_.print(prefix + "end of standard input, draining");
                break;
            case LiveFeeder::Event::PLAYED:
                this->console_.print(prefix + "finished playing standard input");
                break;
            case LiveFeeder::Event::STOPPED_BY_SERVER:
                this->console_.print(prefix +
                                     "the server stopped playback; resume to play standard input");
                break;
            case LiveFeeder::Event::CLOSED:
                this->console_.print(prefix + "the input closed; play standard input with: live");
                break;
            case LiveFeeder::Event::ENDED:
                this->console_.print(prefix + "standard input ended with nothing left to play");
                break;
        }
    }

    /// Routes client_id's source into group (SendspinServer::route_source()), releasing what
    /// plays there first
    /// @return whether it is routed
    bool route(GroupId group, const std::string& client_id);

    /// The client whose source group plays; nullopt when none does
    std::optional<std::string> routed(GroupId group) {
        const std::lock_guard<std::mutex> lock(this->mutex_);
        const auto it = this->routes_.find(group);
        return it != this->routes_.end() ? std::optional(it->second.client_id) : std::nullopt;
    }

    /// The routed source's input in group, for its status; nullptr when none plays there
    std::shared_ptr<GroupInput> route_input(GroupId group) {
        const std::lock_guard<std::mutex> lock(this->mutex_);
        const auto it = this->routes_.find(group);
        return it != this->routes_.end() ? it->second.input : nullptr;
    }

    /// Opens the file typed and plays it on group: a group playing a file jumps to it on its
    /// input (GroupInput::flush(), set_format()); otherwise a feeder on a new input
    /// @return whether it plays
    bool play(GroupId group, const std::string& typed);

    /// Opens the file typed and adds it after the group's last file, on the same run; plays it
    /// when nothing plays
    void queue(GroupId group, const std::string& typed);

    /// Before the server starts: whether files loop (--loop)
    void set_default_loop(bool loop) {
        this->loop_.store(loop);
    }

    /// Whether files loop (Feeder::set_loop()): in group's feeder, and in every feeder made from
    /// now on
    void set_loop(GroupId group, bool loop) {
        this->loop_.store(loop);
        this->now_playing_.with(group, [&](Feeder& feeder) { feeder.set_loop(loop); });
        this->console_.print(loop ? "files loop" : "files play once");
    }

    /// @return whether it paused
    bool pause(GroupId group) {
        if (const std::shared_ptr<GroupInput> input = this->route_input(group)) {
            // A live input: it keeps taking the source's audio while paused, and resumes at the
            // newest
            input->pause();
            this->console_.print("paused the routed source");
            return true;
        }
        if (this->live_ != nullptr && this->live_->status(group)) {
            // A live input keeps taking what is read while paused and resumes at the newest
            const bool paused = this->live_->pause(group).has_value();
            this->console_.print(paused ? "paused standard input"
                                        : "standard input is not playing");
            return paused;
        }
        std::optional<Feeder::Spot> paused;
        uint32_t rate = 0;
        this->now_playing_.with(group, [&](Feeder& feeder) {
            paused = feeder.pause();
            if (paused) {
                rate = feeder.info(paused->file).format.sample_rate;
            }
        });
        this->console_.print(paused ? "paused at " + seconds(paused->frame, rate) +
                                          this->in(group, paused->file)
                                    : "nothing to pause");
        return paused.has_value();
    }

    /// @return whether it resumed
    bool resume(GroupId group) {
        if (const std::shared_ptr<GroupInput> input = this->route_input(group)) {
            input->resume();
            this->console_.print("resumed the routed source");
            return true;
        }
        if (this->live_ != nullptr && this->live_->status(group)) {
            const bool resumed = this->live_->resume(group);
            this->console_.print(resumed ? "resumed standard input"
                                         : "standard input is not paused");
            return resumed;
        }
        bool resumed = false;
        this->now_playing_.with(group, [&](Feeder& feeder) { resumed = feeder.resume(); });
        this->console_.print(resumed ? "resumed" : "not paused");
        return resumed;
    }

    /// Continues the file being written from target_s seconds, clamped to its length
    /// @return whether it seeked
    bool seek(GroupId group, double target_s) {
        return this->seek_to(group, [target_s](Feeder& /*feeder*/, const AudioInfo& info) {
            const uint32_t rate = info.format.sample_rate;
            // Clamped in seconds, so the cast stays in range
            const double length = static_cast<double>(info.frames) / rate;
            return static_cast<uint64_t>(std::min(target_s, length) * rate);
        });
    }

    /// Skips to the file after the one playing: seeks the file being written to its start when it
    /// is that file, else to its end, where the feeder goes on as at the end of a file (the next
    /// file, the first again when looping, or the drain)
    /// @return whether it seeked
    bool next(GroupId group) {
        return this->seek_to(group, [](Feeder& feeder, const AudioInfo& info) {
            return feeder.status().playing.file == feeder.writing() ? info.frames : uint64_t{0};
        });
    }

    /// Continues the file being written from the frame pick chooses, both under NowPlaying's lock,
    /// so no command moves the feeder in between; its own thread may still begin the next file
    /// @return whether it seeked
    template <typename Pick>
    bool seek_to(GroupId group, Pick&& pick) {
        if (this->routed(group)) {
            this->console_.print("a routed source is live and cannot seek");
            return false;
        }
        if (this->live_ != nullptr && this->live_->status(group)) {
            this->console_.print("standard input is live and cannot seek");
            return false;
        }
        std::optional<bool> seeked;
        uint32_t rate = 0;
        uint64_t frame = 0;
        int64_t at_us = 0;
        size_t file = 0;
        this->now_playing_.with(group, [&](Feeder& feeder) {
            file = feeder.writing();
            const AudioInfo info = feeder.info(file);
            rate = info.format.sample_rate;
            frame = pick(feeder, info);
            seeked = feeder.seek(frame);
            at_us = feeder.time_of(frame);
        });
        if (!seeked) {
            this->console_.print("nothing to seek");
        } else if (!*seeked) {
            this->console_.print("the input is closed; play the file again");
        } else {
            this->console_.print("seeking to " + seconds(frame, rate) + this->in(group, file));
            this->publish_metadata(group, file, frame * MS_PER_S / rate, at_us);
        }
        return seeked.value_or(false);
    }

    void stop(GroupId group) {
        if (this->release_route(group)) {
            this->console_.print("stopped the routed source; route it again with: route <client>");
            this->set_caps(group, {});
            return;
        }
        const std::optional<LiveFeeder::Status> live = this->live_status(group);
        if (live && this->live_->detach(group)) {
            // Once standard input ended there is nothing to play again
            this->console_.print(live->ended ? "stopped standard input"
                                             : "stopped standard input; play it again with: live");
            this->set_caps(group, {});
            return;
        }
        const bool playing = this->now_playing_.with(group, [](Feeder&) {});
        this->now_playing_.replace(group, nullptr);
        this->console_.print(playing ? "stopped" : "nothing playing");
        this->set_caps(group, {});
    }

    /// A controller's command in group, on the dispatch thread
    void on_transport_command(GroupId group, TransportCommand command, const CommandArgs& args) {
        switch (command) {
            case TransportCommand::PLAY:
                this->resume(group);
                break;
            case TransportCommand::PAUSE:
                this->pause(group);
                break;
            case TransportCommand::STOP:
                this->stop(group);
                break;
            case TransportCommand::PREVIOUS:
                this->seek(group, 0.0);
                break;
            case TransportCommand::SEEK:
            case TransportCommand::SEEK_RELATIVE:
                if (args.position_ms) {
                    this->seek(group, static_cast<double>(*args.position_ms) / MS_PER_S);
                }
                break;
            default:
                break;
        }
    }

    /// " in <title>" for a group's file, by its index among those played since the last play
    std::string in(GroupId group, size_t file) {
        const std::lock_guard<std::mutex> lock(this->mutex_);
        const std::vector<Track>& tracks = this->tracks_[group];
        return file < tracks.size() ? " in " + tracks[file].title : "";
    }

    /// The title of a group's file, by its index among those played since the last play
    std::string title(GroupId group, size_t file) {
        const std::lock_guard<std::mutex> lock(this->mutex_);
        const std::vector<Track>& tracks = this->tracks_[group];
        return file < tracks.size() ? tracks[file].title : "";
    }

    bool loop() const {
        return this->loop_.load();
    }

private:
    /// The title and length of a file a group plays
    struct Track {
        std::string title;
        uint32_t duration_ms{0};
    };

    /// A WAV or FLAC file opened
    struct Loaded {
        std::string path;
        std::shared_ptr<AudioSource> source;
        AudioInfo info;
    };

    /// Opens the file typed, printing why when it cannot
    std::optional<Loaded> load(const std::string& typed);

    /// A source routed into a group, and the input it plays into
    struct Route {
        std::string client_id;
        std::shared_ptr<GroupInput> input;
    };

    /// Releases the input of the source routed into group, which ends the route
    /// @return whether one was routed there
    bool release_route(GroupId group) {
        std::shared_ptr<GroupInput> input;
        {
            const std::lock_guard<std::mutex> lock(this->mutex_);
            const auto it = this->routes_.find(group);
            if (it == this->routes_.end()) {
                return false;
            }
            input = std::move(it->second.input);
            this->routes_.erase(it);
        }
        return true;
    }

    /// Sets the group's controller caps; a group gone meanwhile has no controllers to offer them
    void set_caps(GroupId group, const ControllerCaps& caps) {
        (void)this->server_->set_controller_caps(group, caps);
    }

    /// The group's file's metadata at position_ms, taking effect at at_us, and its controller
    /// caps
    void publish_metadata(GroupId group, size_t file, uint64_t position_ms, int64_t at_us) {
        Metadata metadata;
        ControllerCaps caps;
        {
            const std::lock_guard<std::mutex> lock(this->mutex_);
            const std::vector<Track>& tracks = this->tracks_[group];
            if (file >= tracks.size()) {
                return;
            }
            metadata.title = tracks[file].title;
            metadata.progress =
                MetadataProgress{static_cast<uint32_t>(position_ms), tracks[file].duration_ms,
                                 sendspin::server::NORMAL_PLAYBACK_SPEED};
            caps.seek_max_ms = tracks[file].duration_ms;
        }
        caps.commands = {TransportCommand::PLAY, TransportCommand::PAUSE,
                         TransportCommand::STOP, TransportCommand::PREVIOUS,
                         TransportCommand::SEEK, TransportCommand::SEEK_RELATIVE};
        this->set_caps(group, caps);
        // As for the caps, a group gone meanwhile has no members to show the metadata
        (void)this->server_->set_metadata(group, metadata, at_us);
    }

    /// A feeder's report, on its thread or the dispatch thread; it calls no feeder
    void on_report(GroupId group, const Feeder::Report& report) {
        const std::string prefix = "group " + std::to_string(group.value) + ": ";
        switch (report.event) {
            case Feeder::Event::NEXT_FILE:
                if (!this->announce(group, report.file)) {
                    break;
                }
                this->console_.print(prefix + "now writing" + this->in(group, report.file) +
                                     ", playing from " + std::to_string(report.at_us / US_PER_MS) +
                                     " ms on the server's clock");
                this->publish_metadata(group, report.file, 0, report.at_us);
                break;
            case Feeder::Event::END_OF_FILE:
                this->console_.print(prefix + "end of the last file, draining");
                break;
            case Feeder::Event::PLAYED:
                this->console_.print(prefix + "finished playing");
                break;
            case Feeder::Event::STOPPED_BY_SERVER:
                this->console_.print(prefix + "the server stopped playback; seek to play again");
                break;
            case Feeder::Event::CLOSED:
                this->console_.print(prefix + "the input closed");
                break;
        }
    }

    /// Whether a NEXT_FILE of file in group is printed and its metadata published: a loop of one
    /// short file begins it again many times a second, so the same file is announced at most
    /// once a second
    bool announce(GroupId group, size_t file) {
        constexpr auto SAME_FILE_INTERVAL = std::chrono::seconds(1);
        const auto now = std::chrono::steady_clock::now();
        const std::lock_guard<std::mutex> lock(this->mutex_);
        const auto [it, inserted] = this->announced_.try_emplace(group, file, now);
        if (!inserted && it->second.first == file && now - it->second.second < SAME_FILE_INTERVAL) {
            return false;
        }
        it->second = {file, now};
        return true;
    }

    /// A track for a parsed file
    static Track track_of(const Loaded& loaded) {
        return {
            loaded.path.substr(loaded.path.find_last_of('/') + 1),
            static_cast<uint32_t>(loaded.info.frames * MS_PER_S / loaded.info.format.sample_rate)};
    }

    void print_playing(const Loaded& loaded, GroupId group, const char* how) {
        const AudioInfo& info = loaded.info;
        this->console_.print(std::string(how) + " " + loaded.path + " in group " +
                             std::to_string(group.value) + ": " +
                             std::to_string(info.format.sample_rate) + " Hz, " +
                             std::to_string(info.format.channels) + " channel(s), " +
                             to_string(info.format.sample_format) + ", " +
                             seconds(info.frames, info.format.sample_rate) +
                             (info.truncated ? " (the file is shorter than its header says)" : ""));
    }

    Console& console_;
    NowPlaying& now_playing_;
    SendspinServer* server_{nullptr};
    LiveFeeder* live_{nullptr};
    std::atomic<bool> loop_{false};
    /// Guards only the members below
    std::mutex mutex_;
    /// Each group's files since its last play, by their index in its feeder
    std::map<GroupId, std::vector<Track>> tracks_;
    /// Each group's file last announced (announce()), and when
    std::map<GroupId, std::pair<size_t, std::chrono::steady_clock::time_point>> announced_;
    /// The source each group plays (route())
    std::map<GroupId, Route> routes_;
};

std::optional<Playback::Loaded> Playback::load(const std::string& typed) {
    Loaded loaded;
    loaded.path = expand_home(typed);
    sendspin::server::examples::OpenedAudio opened =
        sendspin::server::examples::open_audio_file(loaded.path);
    if (!opened.source) {
        this->console_.print(loaded.path + ": " + opened.error);
        return std::nullopt;
    }
    loaded.info = opened.source->info();
    loaded.source = std::move(opened.source);
    return loaded;
}

bool Playback::play(GroupId group, const std::string& typed) {
    std::optional<Loaded> loaded = this->load(typed);
    if (!loaded) {
        return false;
    }
    {
        const std::lock_guard<std::mutex> lock(this->mutex_);
        this->tracks_[group] = {track_of(*loaded)};
    }
    // A track jump on the group's input, which keeps its run (spec messaging.md "stream/clear")
    bool jumped = false;
    int64_t start_us = 0;
    this->now_playing_.with(group, [&](Feeder& feeder) {
        jumped = feeder.play(loaded->source);
        start_us = feeder.time_of(0);
    });
    if (jumped) {
        this->print_playing(*loaded, group, "jumping to");
        this->publish_metadata(group, 0, 0, start_us);
        return true;
    }
    // The group allows one input at a time: release the previous one before opening the next
    this->release_route(group);
    this->now_playing_.replace(group, nullptr);
    sendspin::server::InputOptions options;
    options.format = loaded->info.format;
    sendspin::server::OpenedInput opened = this->server_->open_input(group, options);
    if (opened.result != OpenInputResult::OK) {
        this->console_.print("cannot play " + loaded->path + ": " + to_string(opened.result));
        return false;
    }
    this->print_playing(*loaded, group, "playing");
    // Before the first write, time_of(0) is provisional: about when the first sample plays
    start_us = opened.input->time_of(0);
    this->now_playing_.replace(
        group, std::make_unique<Feeder>(
                   std::move(opened.input), std::move(loaded->source),
                   [this, group](const Feeder::Report& report) { this->on_report(group, report); },
                   this->loop_.load()));
    this->publish_metadata(group, 0, 0, start_us);
    return true;
}

void Playback::queue(GroupId group, const std::string& typed) {
    std::optional<Loaded> loaded = this->load(typed);
    if (!loaded) {
        return;
    }
    size_t after = 0;
    {
        const std::lock_guard<std::mutex> lock(this->mutex_);
        std::vector<Track>& tracks = this->tracks_[group];
        after = tracks.size();
        tracks.push_back(track_of(*loaded));
    }
    bool queued = false;
    this->now_playing_.with(group, [&](Feeder& feeder) { queued = feeder.queue(loaded->source); });
    if (queued) {
        this->print_playing(*loaded, group,
                            ("queued after " + std::to_string(after) + " file(s):").c_str());
        return;
    }
    {
        const std::lock_guard<std::mutex> lock(this->mutex_);
        this->tracks_[group].pop_back();
    }
    // Nothing plays there
    this->play(group, typed);
}

bool Playback::live(GroupId group) {
    if (this->live_ == nullptr) {
        this->console_.print("start server_cli with --stdin to play standard input");
        return false;
    }
    // The group allows one input at a time: release a file's or a source's before opening the
    // live one
    this->release_route(group);
    this->now_playing_.replace(group, nullptr);
    sendspin::server::InputOptions options;
    options.format = this->live_->format();
    options.kind = sendspin::server::InputKind::LIVE;
    sendspin::server::OpenedInput opened = this->server_->open_input(group, options);
    if (opened.result != OpenInputResult::OK) {
        this->console_.print(std::string("cannot play standard input: ") +
                             to_string(opened.result));
        return false;
    }
    if (!this->live_->attach(std::move(opened.input))) {
        this->console_.print("standard input has ended");
        return false;
    }
    // A live input has no position to seek to
    ControllerCaps caps;
    caps.commands = {TransportCommand::PLAY, TransportCommand::PAUSE, TransportCommand::STOP};
    this->set_caps(group, caps);
    this->console_.print("playing standard input live in group " + std::to_string(group.value));
    return true;
}

bool Playback::route(GroupId group, const std::string& client_id) {
    // The group allows one input at a time: release a file's or standard input's first
    this->release_route(group);
    this->now_playing_.replace(group, nullptr);
    if (this->live_ != nullptr && this->live_->status(group)) {
        (void)this->live_->detach(group);
    }
    sendspin::server::RoutedSource routed = this->server_->route_source(client_id, group);
    if (routed.result != sendspin::server::RouteSourceResult::OK) {
        this->console_.print(std::string("cannot route the source: ") + to_string(routed.result));
        return false;
    }
    {
        const std::lock_guard<std::mutex> lock(this->mutex_);
        this->routes_[group] = {client_id, std::move(routed.input)};
    }
    // A live input has no position to seek to; stop ends the route
    ControllerCaps caps;
    caps.commands = {TransportCommand::PLAY, TransportCommand::PAUSE, TransportCommand::STOP};
    this->set_caps(group, caps);
    this->console_.print("routing the source of " + client_id + " into group " +
                         std::to_string(group.value) + "; it streams whenever it is ready");
    return true;
}

/// Prints the server's events as they arrive, on its dispatch thread
class EventPrinter : public sendspin::server::ServerListener {
public:
    EventPrinter(Console& console, NowPlaying& now_playing, Playback& playback)
        : console_(console), now_playing_(now_playing), playback_(playback) {}

    /// Before the server starts: keeps every client in the server's group (--one-group)
    void gather_into(SendspinServer& server) {
        this->gather_ = &server;
    }

    void on_client_seen(const ClientInfo& client) override {
        this->console_.print("discovered: " + client.name + " at " + client.url + ", dialing it");
    }

    void on_client_connected(const ClientInfo& client) override {
        this->console_.print("connected: " + client.name + " (" + client.client_id + ") " +
                             (client.accepted ? "from " + client.address : "at " + client.url) +
                             ", " + trust(client));
        if (this->gather_ != nullptr) {
            sendspin::server::examples::gather_into_server_group(*this->gather_);
        }
    }

    void on_client_disconnected(const ClientInfo& client,
                                const ClientDisconnect& disconnect) override {
        std::string line = "disconnected: " + client.name + " (" + client.client_id +
                           "): " + to_string(disconnect.reason);
        if (disconnect.reason == DisconnectReason::GOODBYE) {
            line += std::string(", reason ") + (disconnect.goodbye_reason
                                                    ? to_string(*disconnect.goodbye_reason)
                                                    : "unrecognized");
        }
        this->console_.print(line);
    }

    void on_source_ready(const ClientInfo& client) override {
        this->console_.print("source ready: " + client.name + " (" + client.client_id +
                             "); route it into the selected group with: route " + client.client_id);
    }

    void on_source_started(const ClientInfo& client, GroupId group,
                           const StreamFormat& format) override {
        this->console_.print("source " + client.name + " streams " + format_text(format) +
                             " into group " + std::to_string(group.value));
    }

    void on_source_stopped(const ClientInfo& client, GroupId group) override {
        this->console_.print("source " + client.name + " stopped streaming into group " +
                             std::to_string(group.value));
    }

    void on_source_signal(const ClientInfo& client, SourceSignal signal) override {
        this->console_.print("source " + client.name + " signal " +
                             (signal == SourceSignal::PRESENT ? "present" : "absent"));
    }

    void on_approval_needed(const ClientInfo& client) override {
        this->console_.print("approval needed: " + client.name + " (" + client.client_id +
                             ") is NOT APPROVED and plays nothing; approve it with: approve " +
                             client.client_id);
    }

    void on_pairing_required(const ClientInfo& client) override {
        this->console_.print("pairing required: " + client.name + " (" + client.client_id +
                             ") does not enable unpaired access, so it plays nothing; pair it "
                             "with: pair " +
                             client.client_id);
    }

    void on_credential_mismatch(const ClientInfo& client) override {
        this->console_.print("pairing lost: " + client.name + " (" + client.client_id +
                             ") could not use its pairing with this server and plays nothing "
                             "until it is paired again with: pair " +
                             client.client_id);
    }

    void on_pairing_pending(const ClientInfo& client, std::string_view message) override {
        this->console_.print("pairing waits on " + client.name + (message.empty() ? "" : ": ") +
                             printable(message));
    }

    void on_pairing_code_needed(const ClientInfo& client, PairMethod method,
                                CodeFormat format) override {
        std::string what;
        if (method == PairMethod::PAIRING_PSK) {
            what = "the pairing token it shows: token ";
        } else if (format == CodeFormat::QR_CODE) {
            what = "the token under the QR code it shows: code ";
        } else if (method == PairMethod::STATIC_PAIRING_CODE) {
            what = "its 8-digit code: code ";
        } else {
            what = "the 6-digit code it shows: code ";
        }
        this->console_.print("pairing " + client.name + ": enter " + what + client.client_id +
                             " <...>");
    }

    void on_pairing_result(const ClientInfo& client, PairingResult result) override {
        this->console_.print("pairing " + client.name + " (" + client.client_id +
                             "): " + to_string(result));
    }

    void on_newer_client_detected(const ClientInfo& client, std::string_view identifier) override {
        this->console_.print("newer client: " + client.name + " (" + client.client_id +
                             ") offers " + std::string(identifier) +
                             ", which this server does not implement");
    }

    void on_transport_command(GroupId group, TransportCommand command,
                              const CommandArgs& args) override {
        std::string line = "controller " + args.client_id + " in group " +
                           std::to_string(group.value) + ": " + to_string(command);
        if (args.position_ms) {
            line += " to " + std::to_string(*args.position_ms) + " ms";
        }
        if (args.offset_ms) {
            line += " by " + std::to_string(*args.offset_ms) + " ms";
        }
        this->console_.print(line);
        this->playback_.on_transport_command(group, command, args);
    }

    void on_error(const ServerError& error) override {
        std::string line = std::string("error: ") + to_string(error.kind);
        if (!error.url.empty()) {
            line += " at " + error.url;
        }
        if (!error.client_id.empty()) {
            line += " (" + error.client_id + ")";
        }
        if (error.kind == ErrorKind::GOODBYE_BEFORE_ACTIVATION) {
            line += std::string(", reason ") +
                    (error.goodbye_reason ? to_string(*error.goodbye_reason) : "unrecognized");
        }
        if (error.format) {
            line += std::string(", format ") + to_string(error.format->codec) + " " +
                    std::to_string(error.format->sample_rate) + " Hz, " +
                    std::to_string(error.format->channels) + " channels, " +
                    std::to_string(error.format->bit_depth) + " bits";
        }
        this->console_.print(line);
    }

    void on_group_changed(const GroupInfo& group) override {
        this->console_.print(describe(group));
        if (this->gather_ != nullptr) {
            sendspin::server::examples::gather_into_server_group(*this->gather_);
        }
    }

    void on_drained(GroupId group, InputId input, DrainId drain, DrainOutcome outcome) override {
        this->console_.print("group " + std::to_string(group.value) + ": drained " +
                             std::to_string(drain.value) + ": " + to_string(outcome));
        this->now_playing_.with(group, [&](Feeder& feeder) {
            if (feeder.input_id() == input) {
                feeder.on_drained(drain, outcome);
            }
        });
        this->playback_.on_live_drained(input, drain, outcome);
    }

private:
    Console& console_;
    NowPlaying& now_playing_;
    Playback& playback_;
    SendspinServer* gather_{nullptr};
};

void print_status(const SendspinServer& server, GroupId selected, Console& console,
                  NowPlaying& now_playing, Playback& playback) {
    const std::optional<GroupInfo> group = group_info(server, selected);
    if (group) {
        console.print("status: " + describe(*group));
    } else {
        console.print("status: group " + std::to_string(selected.value) +
                      " is gone; select another with: use <group>");
    }
    if (const std::shared_ptr<GroupInput> input = playback.route_input(selected)) {
        console.print("status: the source of " + playback.routed(selected).value_or("") +
                      ", live, latency " + std::to_string(input->latency_us() / US_PER_MS) +
                      " ms, input " + to_string(input->state()));
        return;
    }
    if (const std::optional<LiveFeeder::Status> live = playback.live_status(selected)) {
        console.print(std::string("status: standard input, live") +
                      (live->stopped  ? ", stopped by the server"
                       : live->paused ? ", paused"
                                      : "") +
                      (live->ended ? ", ended" : "") + ", " + std::to_string(live->written) +
                      " samples written, " + std::to_string(live->dropped) + " dropped, latency " +
                      std::to_string(live->latency_us / US_PER_MS) + " ms, input " +
                      to_string(live->input_state));
        return;
    }
    std::optional<Feeder::Status> status;
    uint32_t playing_rate = 0;
    uint32_t writing_rate = 0;
    now_playing.with(selected, [&](Feeder& feeder) {
        status = feeder.status();
        playing_rate = feeder.info(status->playing.file).format.sample_rate;
        writing_rate = feeder.info(status->writing).format.sample_rate;
    });
    if (!status) {
        console.print("no file playing");
        return;
    }
    console.print(std::string("status: ") + to_string(status->phase) +
                  (status->paused ? ", paused" : "") + ", at " +
                  seconds(status->playing.frame, playing_rate) +
                  playback.in(selected, status->playing.file) + ", written to " +
                  seconds(status->written_to, writing_rate) + " of " +
                  seconds(status->total, writing_rate) + playback.in(selected, status->writing) +
                  ", latency " + std::to_string(status->latency_us / US_PER_MS) + " ms, input " +
                  to_string(status->input_state));
}

/// microseconds as milliseconds with two decimals
std::string ms(int64_t us) {
    char text[32];
    std::snprintf(text, sizeof(text), "%.2f", static_cast<double>(us) / US_PER_MS);
    return text;
}

/// "p50 a / p99 b / max c ms", or "none" with no values
std::string describe(const TimingStats::Percentiles& values, bool with_p99) {
    if (values.count == 0) {
        return "none";
    }
    if (!with_p99) {
        return "min " + ms(values.min) + " / p50 " + ms(values.p50) + " ms";
    }
    return "p50 " + ms(values.p50) + " / p99 " + ms(values.p99) + " / max " + ms(values.max) +
           " ms";
}

void print_timing(const SendspinServer& server, const TimingStats& stats, bool was_reset,
                  Console& console) {
    const std::vector<TimingStats::ConnectionTiming> timings = stats.report(server.now_us());
    if (timings.empty()) {
        console.print(was_reset ? "timing: nothing sent since the reset" : "timing: nothing sent");
    }
    const std::vector<ClientInfo> clients = server.clients();
    for (const TimingStats::ConnectionTiming& timing : timings) {
        std::string name = "not connected";
        for (const ClientInfo& client : clients) {
            if (client.client_id == timing.client_id) {
                name = client.name;
            }
        }
        const double seconds = static_cast<double>(timing.window_us) / US_PER_S;
        const double per_second = seconds > 0 ? 1.0 / seconds : 0.0;
        char rates[128];
        std::snprintf(rates, sizeof(rates), "%.1f chunks/s, %.1f kB/s over %.1f s",
                      static_cast<double>(timing.chunks) * per_second,
                      static_cast<double>(timing.chunk_bytes) * per_second / BYTES_PER_KB, seconds);
        console.print("timing [" + std::to_string(timing.connection_id) + "] " + name + " (" +
                      timing.client_id + "): " + std::to_string(timing.hold_us.count) +
                      " time replies, hold " + describe(timing.hold_us, true) + "; audio " +
                      std::to_string(timing.chunks) + " chunks, " + rates + ", send_ahead " +
                      describe(timing.send_ahead_us, false));
    }
}

/// The bytes of a file; nullopt when it cannot be read
/// A file's bytes, or nullopt when it cannot be read or is larger than max_bytes
std::optional<std::vector<uint8_t>> read_file(const std::string& path, size_t max_bytes) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        return std::nullopt;
    }
    const std::streamoff size = file.tellg();
    if (size < 0 || static_cast<uint64_t>(size) > max_bytes) {
        return std::nullopt;
    }
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    file.seekg(0);
    if (!file.read(reinterpret_cast<char*>(bytes.data()), size)) {
        return std::nullopt;
    }
    return bytes;
}

/// `art` and `artist`: the group's image for source from a JPEG or PNG file, or none for "off"
void set_art(SendspinServer& server, GroupId group, ArtworkSource source, const std::string& path,
             Console& console) {
    std::vector<uint8_t> image;
    if (path != "off") {
        std::optional<std::vector<uint8_t>> bytes =
            read_file(path, sendspin::server::artwork_limits::MAX_IMAGE_BYTES);
        if (!bytes) {
            console.print("cannot read " + path + ", or it is larger than 32 MiB");
            return;
        }
        image = std::move(*bytes);
    }
    if (server.set_artwork(group, source, image, server.now_us())) {
        return;
    }
    const std::vector<GroupInfo> groups = server.groups();
    if (std::none_of(groups.begin(), groups.end(),
                     [&](const GroupInfo& info) { return info.id == group; })) {
        console.print("the group is gone; use selects another");
    } else {
        console.print("not a JPEG or PNG image the server takes: " + path);
    }
}

/// `colors`: fields as name=rrggbb, or "off" for none
std::optional<Colors> parse_colors(const std::string& text) {
    Colors colors;
    if (text == "off") {
        return colors;
    }
    std::istringstream fields(text);
    std::string field;
    const std::array<std::pair<std::string_view, std::optional<RgbColor> Colors::*>, 6> names{{
        {"background_dark", &Colors::background_dark},
        {"background_light", &Colors::background_light},
        {"primary", &Colors::primary},
        {"accent", &Colors::accent},
        {"on_dark", &Colors::on_dark},
        {"on_light", &Colors::on_light},
    }};
    constexpr size_t HEX_DIGITS = 6;
    constexpr int HEX_BASE = 16;
    constexpr int BYTE_BITS = 8;
    constexpr unsigned BYTE_MASK = 0xFF;
    while (fields >> field) {
        const size_t equals = field.find('=');
        if (equals == std::string::npos || field.size() - equals - 1 != HEX_DIGITS) {
            return std::nullopt;
        }
        const std::string hex = field.substr(equals + 1);
        // strtoul() alone would take a sign or a 0x
        if (!std::all_of(hex.begin(), hex.end(), [](char c) {
                return std::isxdigit(static_cast<unsigned char>(c)) != 0;
            })) {
            return std::nullopt;
        }
        char* end = nullptr;
        const unsigned long value = std::strtoul(hex.c_str(), &end, HEX_BASE);
        const auto name = std::find_if(names.begin(), names.end(), [&](const auto& entry) {
            return entry.first == std::string_view(field).substr(0, equals);
        });
        if (*end != '\0' || name == names.end()) {
            return std::nullopt;
        }
        colors.*(name->second) =
            RgbColor{static_cast<uint8_t>((value >> (2 * BYTE_BITS)) & BYTE_MASK),
                     static_cast<uint8_t>((value >> BYTE_BITS) & BYTE_MASK),
                     static_cast<uint8_t>(value & BYTE_MASK)};
    }
    return colors;
}

void print_help(Console& console) {
    console.print("commands:");
    console.print(
        "  connect <ws-url>   dial a client, e.g. connect ws://192.168.1.20:8928/sendspin");
    console.print("  connect <client>   dial a client clients lists as not connected, by its");
    console.print("                     client_id or the start of its name");
    console.print("  clients            list the clients, connected or not");
    console.print("  approve <client> [source]");
    console.print("                     approve a client (its client_id or name) to play, and");
    console.print("                     with source its source role too");
    console.print("  revoke <client>    revoke a client's approval");
    console.print("  pair <client> [psk|static|digits|qr]");
    console.print("                     pair a client with a method it offers (default: the code");
    console.print("                     it shows); the server asks for the code or token");
    console.print("  code <client> <code>");
    console.print("                     enter the code (or the token under a QR code) a client");
    console.print("                     shows, or its static code");
    console.print("  token <client> <token>");
    console.print("                     pair a client with the pairing token it shows");
    console.print("  cancel <client>    cancel a client's pairing");
    console.print("  unpair <client>    remove a client's pairing");
    console.print("  groups             list the groups and their members; * marks the group");
    console.print("                     the commands below act on (the server's at first)");
    console.print("  group <name> <client>");
    console.print("                     create a group with the client in it");
    console.print("  move <client> <group>");
    console.print("                     move a client into a group (its id or the start of its");
    console.print("                     name); a group left empty with no file playing goes");
    console.print("  rename <group> <name>");
    console.print("                     rename a group");
    console.print("  format <client> <pcm|flac|opus>:<rate>:<channels>:<bits>|off");
    console.print("                     stream to a client in this format where it lists it, in");
    console.print("                     place of its own preference; off goes back to its own");
    console.print("  use <group>        act on this group with the commands below");
    console.print("  play <file>        play a WAV or FLAC file to the group; one playing there");
    console.print("                     jumps to it on the same run");
    console.print("  queue <file>       play a WAV or FLAC file after the group's last one,");
    console.print("                     gaplessly");
    console.print("  loop on|off        go on from the group's first file after its last, on the");
    console.print("                     same run, rather than stop (--loop sets it at start)");
    console.print("  live               play standard input (--stdin) to the group, live");
    console.print("  route <client>     play a client's source role into the group, live, while");
    console.print("                     it is ready; stop ends it");
    console.print("  pause, resume      pause and resume the group's file or standard input,");
    console.print("                     which resumes at the newest it read");
    console.print("  seek <seconds>     continue the group's file from this position");
    console.print("  stop               stop the group's file or standard input");
    console.print("  volume <0-" + std::to_string(MAX_VOLUME) + ">     set the group's volume");
    console.print("  mute on|off        mute or unmute the group");
    console.print("  art <file>|off     show a JPEG or PNG file as the group's album art, which");
    console.print("                     also sets its colors from the image, or none");
    console.print("  artist <file>|off  show a JPEG or PNG file as the group's artist image");
    console.print("  colors <name>=<rrggbb> ...|off");
    console.print(
        "                     set the group's colors (background_dark, background_light,");
    console.print("                     primary, accent, on_dark, on_light), or none");
    console.print("  status             show the group, its file's position and the latency");
    console.print("  trace on|off       print every message sent and received");
    console.print("  timing [reset]     show each connection's time-reply hold and audio rate");
    console.print("                     since the last reset, or reset them");
    console.print("  help               show this list");
    console.print("  quit               stop the server and exit");
}

void print_usage(const char* program) {
    std::printf("usage: %s [options] <state-directory> [server-name]\n", program);
    sendspin::server::examples::print_web_usage();
    std::printf("  --approve-all      approve every client without a record of the operator's\n"
                "                     decision, so each that enables unpaired access joins the\n"
                "                     server's group and plays what `play` writes\n"
                "  --port <port>      accept clients on this port (default 8927; 0 for any)\n"
                "  --no-advertise     do not advertise the server for clients to connect to\n"
                "  --no-browse        do not browse for clients to dial\n"
                "  --only <prefix>    dial only the clients found whose name starts with prefix\n"
                "  --qr               ask a client that can show a QR code for it; type the\n"
                "                     token printed under it\n"
                "  --opus             stream Opus to the players that list it\n"
                "  --log-level <lvl>  print the server's log up to this level: off, error,\n"
                "                     warn (default) or info\n"
                "  --connect <ws-url> dial a client at start, as the connect command does; may\n"
                "                     be given more than once\n"
                "  --play <file>      play a WAV or FLAC file to the server's group at start\n"
                "  --loop             loop files: after the last, go on from the first on the\n"
                "                     same run, gaplessly (the loop command changes it)\n"
                "  --one-group        keep every client in the server's group, moving back\n"
                "                     each one that ends up in another\n"
                "  --timing-log <csv> append each client's time-reply hold and connection state\n"
                "                     to a CSV file every --timing-interval seconds\n"
                "  --timing-interval <seconds>\n"
                "                     the timing log's interval, 0.1 to 86400 (default 5)\n"
                "  --stdin <rate>:<channels>:<s16|s24|f32>\n"
                "                     play raw interleaved little-endian PCM read from standard\n"
                "                     input to the server's group as a live input, read as it\n"
                "                     arrives (deliver it in real time; what the input refuses is\n"
                "                     dropped); commands are then read from the terminal, and\n"
                "                     with none it plays until standard input ends or the server\n"
                "                     stops it\n"
                "Commands are read from standard input; when it ends and is not a terminal,\n"
                "server_cli runs until SIGINT or SIGTERM, which stop it cleanly.\n");
}

/// The --stdin value's format, `<rate>:<channels>:<s16|s24|f32>`; nullopt when it does not parse
std::optional<sendspin::server::InputFormat> parse_stdin_format(const std::string& text) {
    std::istringstream in(text);
    std::string rate;
    std::string channels;
    std::string sample;
    if (!std::getline(in, rate, ':') || !std::getline(in, channels, ':') ||
        !std::getline(in, sample) || sample.find(':') != std::string::npos) {
        return std::nullopt;
    }
    char* end = nullptr;
    const unsigned long r = std::strtoul(rate.c_str(), &end, DECIMAL_BASE);
    if (rate.empty() || *end != '\0') {
        return std::nullopt;
    }
    const unsigned long c = std::strtoul(channels.c_str(), &end, DECIMAL_BASE);
    if (channels.empty() || *end != '\0') {
        return std::nullopt;
    }
    constexpr std::array<std::pair<std::string_view, SampleFormat>, 3> SAMPLES{{
        {"s16", SampleFormat::S16},
        {"s24", SampleFormat::S24},
        {"f32", SampleFormat::F32},
    }};
    const auto found = std::find_if(SAMPLES.begin(), SAMPLES.end(),
                                    [&](const auto& entry) { return entry.first == sample; });
    // The server checks the range when the input opens; the channel count must fit its field
    constexpr unsigned long MAX_RATE = 1000000;
    if (found == SAMPLES.end() || r == 0 || r > MAX_RATE || c == 0 || c > UINT8_MAX) {
        return std::nullopt;
    }
    return sendspin::server::InputFormat{static_cast<uint32_t>(r), static_cast<uint8_t>(c),
                                         found->second};
}

/// The format command's format, `<pcm|flac|opus>:<rate>:<channels>:<bits>`; nullopt when it does
/// not parse
std::optional<StreamFormat> parse_player_format(const std::string& text) {
    std::istringstream in(text);
    std::string codec;
    std::string rate;
    std::string channels;
    std::string bits;
    if (!std::getline(in, codec, ':') || !std::getline(in, rate, ':') ||
        !std::getline(in, channels, ':') || !std::getline(in, bits) ||
        bits.find(':') != std::string::npos) {
        return std::nullopt;
    }
    std::array<unsigned long, 3> values{};
    const std::array<const std::string*, 3> fields{&rate, &channels, &bits};
    for (size_t i = 0; i < fields.size(); ++i) {
        char* end = nullptr;
        values[i] = std::strtoul(fields[i]->c_str(), &end, DECIMAL_BASE);
        if (fields[i]->empty() || *end != '\0') {
            return std::nullopt;
        }
    }
    // The server checks the rest against what the client lists; the counts must fit their fields
    const std::array<std::pair<std::string_view, StreamCodec>, 3> codecs{{
        {"pcm", StreamCodec::PCM},
        {"flac", StreamCodec::FLAC},
        {"opus", StreamCodec::OPUS},
    }};
    const auto found = std::find_if(codecs.begin(), codecs.end(),
                                    [&](const auto& entry) { return entry.first == codec; });
    if (found == codecs.end() || values[0] > UINT32_MAX || values[1] > UINT8_MAX ||
        values[2] > UINT8_MAX) {
        return std::nullopt;
    }
    return StreamFormat{found->second, static_cast<uint32_t>(values[0]),
                        static_cast<uint8_t>(values[1]), static_cast<uint8_t>(values[2])};
}

/// The --log-level values
constexpr std::array<std::pair<std::string_view, sendspin::server::LogLevel>, 4> LOG_LEVELS{{
    {"off", sendspin::server::LogLevel::OFF},
    {"error", sendspin::server::LogLevel::ERROR},
    {"warn", sendspin::server::LogLevel::WARN},
    {"info", sendspin::server::LogLevel::INFO},
}};

/// The --log-level value's level; nullopt for another word
std::optional<sendspin::server::LogLevel> parse_log_level(std::string_view text) {
    for (const auto& [name, level] : LOG_LEVELS) {
        if (name == text) {
            return level;
        }
    }
    return std::nullopt;
}

/// The level as --log-level spells it, for the prefix of a log line
std::string_view log_level_name(sendspin::server::LogLevel level) {
    for (const auto& [name, value] : LOG_LEVELS) {
        if (value == level) {
            return name;
        }
    }
    return {};
}

/// The pair command's method word: the code the client shows when empty; nullopt for another word
std::optional<PairingOptions> pairing_options(std::string_view word) {
    if (word.empty()) {
        return PairingOptions{PairMethod::DYNAMIC_PAIRING_CODE, std::nullopt};
    }
    if (word == "psk") {
        return PairingOptions{PairMethod::PAIRING_PSK, std::nullopt};
    }
    if (word == "static") {
        return PairingOptions{PairMethod::STATIC_PAIRING_CODE, std::nullopt};
    }
    if (word == "digits") {
        return PairingOptions{PairMethod::DYNAMIC_PAIRING_CODE, CodeFormat::DIGITS};
    }
    if (word == "qr") {
        return PairingOptions{PairMethod::DYNAMIC_PAIRING_CODE, CodeFormat::QR_CODE};
    }
    return std::nullopt;
}

/// The client_id of the client whose client_id is typed, else the one client whose name starts
/// with it; empty when none or several match
std::string find_client(const SendspinServer& server, const std::string& typed) {
    std::string found;
    int matches = 0;
    for (const ClientInfo& client : server.clients()) {
        if (!client.client_id.empty() && client.client_id == typed) {
            return client.client_id;
        }
        if (client.name.starts_with(typed)) {
            found = client.client_id;
            ++matches;
        }
    }
    return matches == 1 ? found : std::string();
}

/// The group whose id is typed, else the one group whose name starts with it
std::optional<GroupId> find_group(const SendspinServer& server, const std::string& typed) {
    std::optional<GroupId> found;
    int matches = 0;
    for (const GroupInfo& group : server.groups()) {
        if (std::to_string(group.id.value) == typed) {
            return group.id;
        }
        if (!typed.empty() && group.name.starts_with(typed)) {
            found = group.id;
            ++matches;
        }
    }
    return matches == 1 ? found : std::nullopt;
}

/// Each group with its members by name, the one the playback commands act on marked
void print_groups(const SendspinServer& server, GroupId selected, Console& console) {
    const std::vector<ClientInfo> clients = server.clients();
    for (const GroupInfo& group : server.groups()) {
        std::string line = group.id == selected ? "* " : "  ";
        line += describe(group);
        const char* separator = ": ";
        for (const std::string& client_id : group.client_ids) {
            const auto client =
                std::find_if(clients.begin(), clients.end(),
                             [&](const ClientInfo& info) { return info.client_id == client_id; });
            line += separator;
            line += client != clients.end() ? client->name : client_id;
            separator = ", ";
        }
        console.print(line);
    }
}

/// @brief A request to stop server_cli, from a signal: wakes the command reader through a pipe and
/// the waits on it through a condition variable
class StopRequest {
public:
    StopRequest() {
        if (::pipe(this->pipe_.data()) != 0) {
            this->pipe_ = {-1, -1};
        }
    }

    ~StopRequest() {
        for (const int fd : this->pipe_) {
            if (fd >= 0) {
                ::close(fd);
            }
        }
    }

    StopRequest(const StopRequest&) = delete;
    StopRequest& operator=(const StopRequest&) = delete;

    void request() {
        {
            const std::lock_guard<std::mutex> lock(this->mutex_);
            this->requested_ = true;
        }
        this->changed_.notify_all();
        if (this->pipe_[1] >= 0) {
            const char byte = 1;
            // A full pipe already wakes the reader
            [[maybe_unused]] const ssize_t written = ::write(this->pipe_[1], &byte, 1);
        }
    }

    /// Waits with no timeout for request()
    void wait() {
        std::unique_lock<std::mutex> lock(this->mutex_);
        this->changed_.wait(lock, [this] { return this->requested_; });
    }

    /// Readable once request() was called
    int fd() const {
        return this->pipe_[0];
    }

private:
    std::array<int, 2> pipe_{-1, -1};
    std::mutex mutex_;
    std::condition_variable changed_;
    bool requested_{false};
};

/// @brief Takes SIGINT and SIGTERM: blocks them in the constructing thread, and so in every
/// thread it starts afterwards (construct it before any other thread starts), and waits for them on
/// a thread of its own, which calls on_stop at the first and exits at once at the second
class SignalStop {
public:
    explicit SignalStop(std::function<void()> on_stop) {
        sigemptyset(&this->signals_);
        sigaddset(&this->signals_, SIGINT);
        sigaddset(&this->signals_, SIGTERM);
        // Sent by the destructor to end the thread; ignored from anyone else
        sigaddset(&this->signals_, SIGUSR1);
        pthread_sigmask(SIG_BLOCK, &this->signals_, nullptr);
        this->thread_ = std::thread([this, on_stop = std::move(on_stop)] {
            bool stopping = false;
            for (;;) {
                int signal = 0;
                if (sigwait(&this->signals_, &signal) != 0 ||
                    (signal == SIGUSR1 && this->ending_.load())) {
                    return;
                }
                if (signal == SIGUSR1) {
                    continue;
                }
                if (stopping) {
                    std::_Exit(EXIT_FAILURE);
                }
                stopping = true;
                on_stop();
            }
        });
    }

    ~SignalStop() {
        this->ending_.store(true);
        pthread_kill(this->thread_.native_handle(), SIGUSR1);
        this->thread_.join();
    }

    SignalStop(const SignalStop&) = delete;
    SignalStop& operator=(const SignalStop&) = delete;

private:
    sigset_t signals_{};
    std::atomic<bool> ending_{false};
    std::thread thread_;
};

/// @brief Reads command lines from a file descriptor, and returns early once a stop is requested
class CommandReader {
public:
    enum class Result : uint8_t { LINE, END, STOPPED };

    CommandReader(int fd, const StopRequest& stop) : fd_(fd), stop_(stop) {}

    /// @brief Waits for the next line, the end of the input or a stop
    Result next(std::string& line) {
        for (;;) {
            const size_t newline = this->buffer_.find('\n');
            if (newline != std::string::npos) {
                line = this->buffer_.substr(0, newline);
                this->buffer_.erase(0, newline + 1);
                if (!line.empty() && line.back() == '\r') {
                    line.pop_back();
                }
                return Result::LINE;
            }
            if (this->ended_) {
                if (this->buffer_.empty()) {
                    return Result::END;
                }
                line = std::exchange(this->buffer_, {});
                return Result::LINE;
            }
            std::array<pollfd, 2> fds{{{this->fd_, POLLIN, 0}, {this->stop_.fd(), POLLIN, 0}}};
            if (::poll(fds.data(), fds.size(), -1) < 0) {
                this->ended_ = errno != EINTR;
                continue;
            }
            if (fds[1].revents != 0) {
                return Result::STOPPED;
            }
            if (fds[0].revents == 0) {
                continue;
            }
            constexpr size_t READ_BYTES = 512;
            std::array<char, READ_BYTES> chunk{};
            const ssize_t got = ::read(this->fd_, chunk.data(), chunk.size());
            if (got > 0) {
                this->buffer_.append(chunk.data(), static_cast<size_t>(got));
            } else if (got == 0 || errno != EINTR) {
                this->ended_ = true;
            }
        }
    }

private:
    int fd_;
    const StopRequest& stop_;
    std::string buffer_;
    bool ended_{false};
};

/// The dashboard's playback (--web), carried out as the REPL's commands are
class WebPlaybackAdapter : public sendspin::server::examples::WebPlayback {
public:
    WebPlaybackAdapter(Playback& playback, NowPlaying& now_playing, bool has_live)
        : playback_(playback), now_playing_(now_playing), has_live_(has_live) {}

    bool play(GroupId group, const std::string& path) override {
        return this->playback_.play(group, path);
    }
    void queue(GroupId group, const std::string& path) override {
        this->playback_.queue(group, path);
    }
    bool pause(GroupId group) override {
        return this->playback_.pause(group);
    }
    bool resume(GroupId group) override {
        return this->playback_.resume(group);
    }
    void stop(GroupId group) override {
        this->playback_.stop(group);
    }
    bool seek(GroupId group, double seconds) override {
        return this->playback_.seek(group, seconds);
    }
    bool next(GroupId group) override {
        return this->playback_.next(group);
    }
    void set_loop(GroupId group, bool loop) override {
        this->playback_.set_loop(group, loop);
    }
    bool loop() const override {
        return this->playback_.loop();
    }
    bool has_live() const override {
        return this->has_live_;
    }
    bool live(GroupId group) override {
        return this->playback_.live(group);
    }
    bool route(GroupId group, const std::string& client_id) override {
        return this->playback_.route(group, client_id);
    }
    std::optional<sendspin::server::examples::WebPlaybackStatus> status(GroupId group) override {
        sendspin::server::examples::WebPlaybackStatus out;
        if (const std::shared_ptr<GroupInput> input = this->playback_.route_input(group)) {
            out.live = true;
            out.source = this->playback_.routed(group).value_or("");
            out.phase = std::string("input ") + to_string(input->state());
            out.latency_ms = input->latency_us() / US_PER_MS;
            return out;
        }
        if (const std::optional<LiveFeeder::Status> live = this->playback_.live_status(group)) {
            out.live = true;
            out.phase = live->stopped ? "stopped by the server"
                                      : std::string("input ") + to_string(live->input_state);
            out.paused = live->paused;
            out.ended = live->ended;
            out.written = live->written;
            out.dropped = live->dropped;
            out.latency_ms = live->latency_us / US_PER_MS;
            return out;
        }
        std::optional<Feeder::Status> status;
        AudioInfo info;
        this->now_playing_.with(group, [&](Feeder& feeder) {
            status = feeder.status();
            info = feeder.info(status->playing.file);
        });
        if (!status) {
            return std::nullopt;
        }
        const double rate = info.format.sample_rate;
        out.phase = to_string(status->phase);
        out.paused = status->paused;
        out.file = status->playing.file;
        out.title = this->playback_.title(group, status->playing.file);
        out.position_s = static_cast<double>(status->playing.frame) / rate;
        out.duration_s = static_cast<double>(info.frames) / rate;
        out.latency_ms = status->latency_us / US_PER_MS;
        return out;
    }

private:
    Playback& playback_;
    NowPlaying& now_playing_;
    bool has_live_;
};

int run(int argc, char** argv) {
    bool approve_all = false;
    bool scan_qr = false;
    bool opus = false;
    bool advertise = true;
    bool browse = true;
    std::optional<std::string> only;
    std::optional<sendspin::server::InputFormat> stdin_format;
    bool one_group = false;
    bool loop = false;
    std::optional<std::string> play_file;
    std::vector<std::string> connect_urls;
    std::optional<std::string> timing_log_path;
    double timing_interval_s = DEFAULT_TIMING_INTERVAL_S;
    long port = sendspin::server::config_defaults::LISTEN_PORT;
    sendspin::server::LogLevel log_level = sendspin::server::LogLevel::WARN;
    std::vector<std::string> positional;
    sendspin::server::examples::WebOptions web_options;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            print_usage(argv[0]);
            return 0;
        }
        constexpr long MAX_PORT = 65535;
        if (arg == "--approve-all") {
            approve_all = true;
        } else if (arg == "--qr") {
            scan_qr = true;
        } else if (arg == "--opus") {
            opus = true;
        } else if (arg == "--no-advertise") {
            advertise = false;
        } else if (arg == "--no-browse") {
            browse = false;
        } else if (arg == "--only" && i + 1 < argc) {
            only = argv[++i];
        } else if (arg == "--one-group") {
            one_group = true;
        } else if (arg == "--loop") {
            loop = true;
        } else if (arg == "--play" && i + 1 < argc) {
            play_file = argv[++i];
        } else if (arg == "--connect" && i + 1 < argc) {
            connect_urls.emplace_back(argv[++i]);
        } else if (arg == "--timing-log" && i + 1 < argc) {
            timing_log_path = expand_home(argv[++i]);
        } else if (arg == "--timing-interval" && i + 1 < argc) {
            char* end = nullptr;
            const char* text = argv[++i];
            timing_interval_s = std::strtod(text, &end);
            if (end == text || *end != '\0' || !std::isfinite(timing_interval_s) ||
                timing_interval_s < MIN_TIMING_INTERVAL_S ||
                timing_interval_s > MAX_TIMING_INTERVAL_S) {
                print_usage(argv[0]);
                return 2;
            }
        } else if (arg == "--stdin" && i + 1 < argc) {
            stdin_format = parse_stdin_format(argv[++i]);
            if (!stdin_format) {
                print_usage(argv[0]);
                return 2;
            }
        } else if (arg == "--log-level" && i + 1 < argc) {
            const std::optional<sendspin::server::LogLevel> level = parse_log_level(argv[++i]);
            if (!level) {
                print_usage(argv[0]);
                return 2;
            }
            log_level = *level;
        } else if (arg == "--port" && i + 1 < argc) {
            char* end = nullptr;
            const char* text = argv[++i];
            port = std::strtol(text, &end, DECIMAL_BASE);
            if (end == text || *end != '\0' || port < 0 || port > MAX_PORT) {
                print_usage(argv[0]);
                return 2;
            }
        } else if (const std::optional<bool> web_flag = web_options.parse(argc, argv, i)) {
            if (!*web_flag) {
                print_usage(argv[0]);
                return 2;
            }
        } else if (arg.starts_with("-")) {
            print_usage(argv[0]);
            return 2;
        } else {
            positional.emplace_back(arg);
        }
    }
    // --play and --stdin would both play to the server's group from the start
    if (positional.empty() || positional.size() > 2 || (play_file && stdin_format)) {
        print_usage(argv[0]);
        return 2;
    }
    // Before any other thread starts. A signal ends the command reader and the waits below.
    StopRequest stop;
    std::mutex live_mutex;
    std::condition_variable live_changed;
    bool live_over = false;
    const SignalStop signals([&] {
        stop.request();
        {
            const std::lock_guard<std::mutex> lock(live_mutex);
            live_over = true;
        }
        live_changed.notify_all();
    });
    // With standard input playing, the feeder reads the audio from a duplicate of it and commands
    // come from the terminal, reopened as standard input; with no terminal, standard input plays
    // until it ends. The duplicate stays open until exit, after the feeder.
    int audio_fd = -1;
    bool interactive = true;
    if (stdin_format) {
        audio_fd = ::dup(STDIN_FILENO);
        if (audio_fd < 0) {
            std::fprintf(stderr, "cannot read standard input: %s\n", std::strerror(errno));
            return 1;
        }
        interactive = std::freopen("/dev/tty", "r", stdin) != nullptr;
    }
    Console console;
    NowPlaying now_playing;
    Playback playback(console, now_playing);
    EventPrinter printer(console, now_playing, playback);
    // Reads standard input from the start, so its producer never blocks. Declared before the
    // server, so it outlives the dispatch thread that hands it drain outcomes.
    std::unique_ptr<LiveFeeder> live;
    if (stdin_format) {
        live = std::make_unique<LiveFeeder>(
            audio_fd, *stdin_format, [&, interactive](GroupId group, LiveFeeder::Event event) {
                playback.on_live_event(group, event);
                // Without a terminal nothing but a controller can resume a stop by the server, so
                // it ends server_cli as the end of the data does
                if (event == LiveFeeder::Event::PLAYED || event == LiveFeeder::Event::CLOSED ||
                    event == LiveFeeder::Event::ENDED ||
                    (event == LiveFeeder::Event::STOPPED_BY_SERVER && !interactive)) {
                    {
                        const std::lock_guard<std::mutex> lock(live_mutex);
                        live_over = true;
                    }
                    live_changed.notify_all();
                }
            });
        playback.attach_live(*live);
    }
    // The dashboard (--web), declared before the server, whose listener it is
    WebPlaybackAdapter web_playback(playback, now_playing, live != nullptr);
    std::unique_ptr<sendspin::server::examples::WebUi> web;
    if (web_options.host) {
        web = std::make_unique<sendspin::server::examples::WebUi>(web_options, console,
                                                                  web_playback, printer);
    }
    sendspin::server::examples::FilePersistenceProvider provider(positional[0]);

    std::atomic<bool> tracing{false};
    // Outlive the server, whose dispatch thread feeds them until the destructor joins
    TimingStats timing;
    std::unique_ptr<TimingLog> timing_log;
    bool timing_was_reset = false;
    sendspin::server::ServerConfig config;
    config.name = positional.size() == 2 ? positional[1] : "server_cli";
    config.approve_all_clients = approve_all;
    config.operator_can_scan_qr = scan_qr;
    config.enable_opus = opus;
    config.listen_port = static_cast<uint16_t>(port);
    config.advertise = advertise;
    config.browse = browse;
    if (only) {
        config.discovery =
            std::make_shared<NameFilter>(sendspin::server::platform_discovery(), *only);
    }
    // On the dispatch thread, as the listener's lines, so the two keep their order
    config.log = [&console](sendspin::server::LogLevel level, std::string_view line) {
        console.print(std::string(log_level_name(level)) + ": " + std::string(line));
    };
    config.log_level = log_level;
    config.trace = [&console, &tracing, &timing, &timing_log](const TracedMessage& message) {
        timing.add(message);
        if (timing_log) {
            timing_log->add(message);
        }
        if (tracing.load()) {
            console.print(describe(message));
        }
    };

    SendspinServer server;
    playback.attach(server);
    playback.set_default_loop(loop);
    if (one_group) {
        printer.gather_into(server);
    }
    if (web) {
        web->attach(server, config, one_group);
    }
    if (timing_log_path) {
        constexpr double US_PER_SECOND = 1e6;
        timing_log = std::make_unique<TimingLog>(
            *timing_log_path, static_cast<int64_t>(timing_interval_s * US_PER_SECOND),
            [&server] { return server.clients(); },
            [&console] { console.print("cannot append to the timing log; it stays as it is"); });
    }
    switch (server.start(config, &provider, web ? web->listener() : &printer)) {
        case sendspin::server::StartResult::OK:
            break;
        case sendspin::server::StartResult::INVALID_CONFIG:
            std::fprintf(stderr, "cannot start: the server name is too long\n");
            return 1;
        case sendspin::server::StartResult::BIND_FAILED:
            std::fprintf(stderr, "cannot start: port %ld is in use\n", port);
            return 1;
        case sendspin::server::StartResult::DISCOVERY_FAILED:
            std::fprintf(stderr, "cannot start: mDNS discovery could not start\n");
            return 1;
        default:
            std::fprintf(stderr,
                         "cannot start: the server key in %s could not be loaded or saved\n",
                         positional[0].c_str());
            return 1;
    }
    console.print(std::string("sendspin-server-cpp, server_id ") + server.server_id() +
                  ", clients connect on port " + std::to_string(server.listen_port()) +
                  (advertise ? " (advertised)" : "") + ". Type help for the commands.");

    // The group the playback commands act on
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access): set while the server runs
    GroupId selected = *server.default_group();
    // Ends a start that cannot go on, stopping the timing log's thread while the server lives
    const auto fail = [&] {
        if (web) {
            web->stop();
        }
        if (timing_log) {
            timing_log->stop();
        }
        now_playing.clear();
        server.stop();
        return 1;
    };
    // Stops the dashboard before the server goes, whatever the return
    const sendspin::server::examples::WebUi::Stopper web_stopper(web.get());
    if (web && !web->start()) {
        return fail();
    }
    if (timing_log && !timing_log->start()) {
        console.print("cannot write the timing log " + *timing_log_path);
        return fail();
    }
    for (const std::string& url : connect_urls) {
        if (!server.connect_url(url)) {
            console.print("not a ws:// URL: " + url);
            return fail();
        }
    }
    if (play_file && !playback.play(selected, *play_file)) {
        return fail();
    }
    if (live && !playback.live(selected) && !interactive) {
        const std::lock_guard<std::mutex> lock(live_mutex);
        live_over = true;
    }
    if (live && !interactive) {
        console.print(
            "no terminal to read commands from: playing standard input until it ends or the "
            "server stops it");
        std::unique_lock<std::mutex> lock(live_mutex);
        live_changed.wait(lock, [&] { return live_over; });
    }
    // Commands come from standard input. At its end a terminal's user is done; a script, or
    // </dev/null, leaves server_cli running until a signal.
    CommandReader reader(STDIN_FILENO, stop);
    const bool terminal = ::isatty(STDIN_FILENO) != 0;
    std::string line;
    while (interactive) {
        const CommandReader::Result read = reader.next(line);
        if (read == CommandReader::Result::STOPPED) {
            break;
        }
        if (read == CommandReader::Result::END) {
            if (!terminal) {
                console.print("no more commands: running until SIGINT or SIGTERM");
                stop.wait();
            }
            break;
        }
        std::istringstream words(line);
        std::string command;
        std::string argument;
        words >> command >> argument;
        if (command.empty()) {
            continue;
        }
        // The rest of the line, which a code may split with a space
        std::string rest;
        std::getline(words >> std::ws, rest);
        // The argument and the rest: a path with spaces, or a list
        std::string arguments = argument;
        if (!rest.empty()) {
            arguments += ' ';
            arguments += rest;
        }
        if (command == "quit") {
            break;
        }
        if (command == "connect" && argument.starts_with("ws://")) {
            if (!server.connect_url(argument)) {
                console.print("not a ws:// URL: " + argument);
            }
        } else if (command == "connect" && !argument.empty()) {
            const std::string client_id = find_client(server, argument);
            if (client_id.empty() || !server.connect(client_id)) {
                console.print("not one client clients lists, or not reached yet: " + argument);
            }
        } else if (command == "clients") {
            const auto clients = server.clients();
            if (clients.empty()) {
                console.print("no clients");
            }
            for (const ClientInfo& client : clients) {
                const std::string id =
                    client.client_id.empty() ? "(not reached yet)" : client.client_id;
                console.print(id + "  " + client.name + "  " + reach(client) +
                              (client.client_id.empty() ? "" : "  " + trust(client)) +
                              roles(client));
            }
        } else if ((command == "approve" || command == "revoke") && !argument.empty()) {
            // A connected client's name stands for its client_id
            std::string client_id = argument;
            for (const ClientInfo& client : server.clients()) {
                if (client.name == argument) {
                    client_id = client.client_id;
                }
            }
            const ApprovalScope scope =
                rest == "source" ? ApprovalScope::PLAYBACK_AND_SOURCE : ApprovalScope::PLAYBACK;
            const bool done = command == "approve" ? server.approve(client_id, scope)
                                                   : server.revoke_approval(client_id);
            if (done) {
                std::string line = command;
                line += "d ";
                line += client_id;
                console.print(line);
            } else {
                console.print("not a client_id or a connected client's name: " + argument);
            }
        } else if (command == "pair" && !argument.empty()) {
            const std::optional<PairingOptions> options = pairing_options(rest);
            const std::string client_id = find_client(server, argument);
            if (!options) {
                print_help(console);
            } else if (client_id.empty() || !server.begin_pairing(client_id, *options)) {
                console.print("not a client offering that method: " + argument);
            }
        } else if (command == "code" && !argument.empty() && !rest.empty()) {
            const std::string client_id = find_client(server, argument);
            if (client_id.empty() || !server.submit_pairing_code(client_id, rest)) {
                console.print("not a client being paired: " + argument);
            }
        } else if (command == "token" && !argument.empty() && !rest.empty()) {
            const std::string client_id = find_client(server, argument);
            if (client_id.empty() || !server.submit_pairing_token(client_id, rest)) {
                console.print("not that client's pairing token, or a client offering it: " +
                              argument);
            }
        } else if ((command == "cancel" || command == "unpair") && !argument.empty()) {
            const std::string client_id = find_client(server, argument);
            const bool done =
                !client_id.empty() &&
                (command == "cancel" ? server.cancel_pairing(client_id) : server.unpair(client_id));
            if (!done) {
                console.print("not a client_id or a client's name: " + argument);
            }
        } else if (command == "groups") {
            print_groups(server, selected, console);
        } else if (command == "group" && !argument.empty() && !rest.empty()) {
            const std::string client_id = find_client(server, rest);
            const std::optional<GroupId> created =
                client_id.empty() ? std::nullopt : server.create_group(argument, client_id);
            if (created) {
                const std::string id = std::to_string(created->value);
                std::string text = "creating group ";
                text += id;
                text += " (";
                text += argument;
                text += ") for ";
                text += rest;
                text += "; select it with: use ";
                text += id;
                console.print(text);
            } else {
                console.print("not a client_id or a client's name: " + rest);
            }
        } else if (command == "move" && !argument.empty() && !rest.empty()) {
            const std::string client_id = find_client(server, argument);
            const std::optional<GroupId> group = find_group(server, rest);
            if (client_id.empty() || !group || !server.move_client(client_id, *group)) {
                std::string text = "not one client and one group: ";
                text += argument;
                text += ", " + rest;
                console.print(text);
            }
        } else if (command == "format" && !argument.empty() && !rest.empty()) {
            const std::string client_id = find_client(server, argument);
            const std::optional<StreamFormat> format = parse_player_format(rest);
            if (rest != "off" && !format) {
                print_help(console);
            } else if (client_id.empty() || !server.set_player_format(client_id, format)) {
                console.print("not a client_id or a client's name: " + argument);
            }
        } else if (command == "rename" && !argument.empty() && !rest.empty()) {
            const std::optional<GroupId> group = find_group(server, argument);
            if (!group || !server.rename_group(*group, rest)) {
                console.print("not one group: " + argument);
            }
        } else if (command == "use" && !argument.empty()) {
            const std::optional<GroupId> group = find_group(server, argument);
            if (group) {
                selected = *group;
                console.print("playback commands act on group " + std::to_string(group->value));
            } else {
                console.print("not one group: " + argument);
            }
        } else if (command == "play" && !argument.empty()) {
            playback.play(selected, argument);
        } else if (command == "queue" && !argument.empty()) {
            playback.queue(selected, argument);
        } else if (command == "live") {
            playback.live(selected);
        } else if (command == "route" && !argument.empty()) {
            const std::string client_id = find_client(server, argument);
            if (client_id.empty()) {
                console.print("not a client_id or a client's name: " + argument);
            } else {
                playback.route(selected, client_id);
            }
        } else if (command == "pause") {
            playback.pause(selected);
        } else if (command == "resume") {
            playback.resume(selected);
        } else if (command == "seek" && !argument.empty()) {
            char* end = nullptr;
            const double target = std::strtod(argument.c_str(), &end);
            if (end == argument.c_str() || *end != '\0' || !std::isfinite(target) || target < 0.0) {
                console.print("not a position in seconds: " + argument);
                continue;
            }
            playback.seek(selected, target);
        } else if (command == "stop") {
            playback.stop(selected);
        } else if (command == "volume" && !argument.empty()) {
            char* end = nullptr;
            const long volume = std::strtol(argument.c_str(), &end, DECIMAL_BASE);
            if (end == argument.c_str() || *end != '\0' || volume < 0 ||
                volume > long{MAX_VOLUME}) {
                console.print("not a volume from 0 to " + std::to_string(MAX_VOLUME) + ": " +
                              argument);
                continue;
            }
            if (!server.set_group_volume(selected, static_cast<uint32_t>(volume))) {
                console.print("no such group");
            }
        } else if (command == "mute" && (argument == "on" || argument == "off")) {
            if (!server.set_group_mute(selected, argument == "on")) {
                console.print("no such group");
            }
        } else if ((command == "art" || command == "artist") && !argument.empty()) {
            set_art(server, selected,
                    command == "art" ? ArtworkSource::ALBUM : ArtworkSource::ARTIST, arguments,
                    console);
        } else if (command == "colors" && !argument.empty()) {
            const std::optional<Colors> colors = parse_colors(arguments);
            if (!colors) {
                print_help(console);
            } else if (!server.set_colors(selected, *colors, server.now_us())) {
                console.print("no such group");
            }
        } else if (command == "status") {
            print_status(server, selected, console, now_playing, playback);
        } else if (command == "loop" && (argument == "on" || argument == "off")) {
            playback.set_loop(selected, argument == "on");
        } else if (command == "trace" && (argument == "on" || argument == "off")) {
            tracing.store(argument == "on");
        } else if (command == "timing" && argument.empty()) {
            print_timing(server, timing, timing_was_reset, console);
        } else if (command == "timing" && argument == "reset") {
            timing.reset(server.now_us());
            timing_was_reset = true;
            console.print("timing reset");
        } else {
            print_help(console);
        }
    }
    if (web) {
        web->stop();
    }
    now_playing.clear();
    // Its last rows while the clients are still listed
    if (timing_log) {
        timing_log->stop();
    }
    // Reports the connected clients as stopped; the destructor then waits for the threads
    server.stop();
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        return run(argc, argv);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "server_cli: %s\n", e.what());
        return 1;
    }
}
