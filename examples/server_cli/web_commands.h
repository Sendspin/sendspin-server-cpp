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

/// @file web_commands.h
/// @brief The dashboard's logic without its transport: the commands a browser sends, the state it
/// is shown, the media directory it browses and the checks a request passes
/// (web_protocol.md has the messages)

#pragma once

#include <sendspin/server/server.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace sendspin::server::examples {

/// The largest command a browser may send, in bytes; a longer WebSocket message closes the session
constexpr size_t MAX_COMMAND_BYTES = size_t{16} * 1024;
/// The most entries a media directory listing holds; the rest, last by name, are left out
constexpr size_t MAX_LISTED_ENTRIES = 2000;

/// @brief What the dashboard shows of a group's playback
struct WebPlaybackStatus {
    /// A file, or a live input: standard input (--stdin) or a routed source
    bool live{false};
    /// A routed source's client_id; empty for a file or standard input
    std::string source;
    /// As server_cli's status prints it, such as "playing" or "playing out"
    std::string phase;
    bool paused{false};
    /// A file: the one playing, its index among those played since the last play, and where
    std::string title;
    size_t file{0};
    double position_s{0.0};
    double duration_s{0.0};
    int64_t latency_ms{0};
    /// Standard input: the samples written and dropped, and whether its data ended
    uint64_t written{0};
    uint64_t dropped{0};
    bool ended{false};
};

/// @brief The playback calls the dashboard makes, which the host carries out as its own commands
/// do; called on the dashboard's thread
class WebPlayback {
public:
    virtual ~WebPlayback() = default;
    /// @param path an absolute path inside the media directory
    virtual bool play(GroupId group, const std::string& path) = 0;
    virtual void queue(GroupId group, const std::string& path) = 0;
    /// @return false when there is nothing to pause
    virtual bool pause(GroupId group) = 0;
    /// @return false when nothing is paused
    virtual bool resume(GroupId group) = 0;
    virtual void stop(GroupId group) = 0;
    /// @return false when there is nothing to seek, or standard input plays
    virtual bool seek(GroupId group, double seconds) = 0;
    /// @brief Skips to the file after the one playing (web_protocol.md has what that is)
    /// @return false when there is nothing to skip, or standard input plays
    virtual bool next(GroupId group) = 0;
    virtual void set_loop(GroupId group, bool loop) = 0;
    virtual bool loop() const = 0;
    /// @brief Whether standard input can be played (--stdin)
    virtual bool has_live() const = 0;
    virtual bool live(GroupId group) = 0;
    /// @brief Routes a client's source into the group (SendspinServer::route_source())
    /// @return false when it cannot be routed there
    virtual bool route(GroupId group, const std::string& client_id) = 0;
    /// @return nullopt when nothing plays in the group
    virtual std::optional<WebPlaybackStatus> status(GroupId group) = 0;
};

/// @brief Whether a browser's request may act on the server: its Origin, when it sends one, must
/// be the server it was sent to (Host), which turns away a page of another site that the operator
/// visits (cross-site request forgery). Host must name this machine as only it can be named, which
/// turns away a page of another site that resolves its own name to this machine (DNS rebinding):
/// with loopback_only, a loopback address or localhost; otherwise also any IP address or a `.local`
/// name. A request with no Origin comes from outside a browser and is allowed.
bool request_allowed(std::string_view origin, std::string_view host, bool loopback_only);

/// @brief Where an image upload goes
struct ArtworkTarget {
    GroupId group;
    ArtworkSource source{ArtworkSource::ALBUM};
};

/// @brief An image upload's target, `/artwork?group=<decimal id>&source=<album|artist>`
/// @return nullopt for another path, or a group or source missing or not one of those
std::optional<ArtworkTarget> parse_artwork_target(std::string_view target);

/// @brief text with each byte that does not begin a well-formed UTF-8 sequence replaced by U+FFFD:
/// a browser closes a WebSocket whose text message is not UTF-8, and client names, device text
/// and log lines come from the network
std::string valid_utf8(std::string_view text);

/// @brief The directory the dashboard browses and plays from (--media-dir): nothing outside it is
/// listed or played
class MediaDir {
public:
    struct Entry {
        std::string name;
        bool directory{false};
    };

    struct Listing {
        std::vector<Entry> entries;
        /// More than MAX_LISTED_ENTRIES entries were found
        bool truncated{false};
    };

    /// @return nullopt when root is not a directory
    static std::optional<MediaDir> open(const std::string& root);

    /// @brief The path of relative, a path below the root with '/' between its parts, symbolic
    /// links resolved
    /// @return nullopt for an absolute path, one that is missing or one that leads outside the
    /// root, by ".." or a symbolic link
    std::optional<std::filesystem::path> resolve(std::string_view relative) const;

    /// @brief The directories and WAV and FLAC files in relative, directories first, each by name
    /// @return nullopt when relative does not resolve to a directory
    std::optional<Listing> list(std::string_view relative) const;

    const std::filesystem::path& root() const {
        return this->root_;
    }

private:
    explicit MediaDir(std::filesystem::path root) : root_(std::move(root)) {}

    std::filesystem::path root_;
};

/// @brief What the message trace shows of each client that the server does not report: the
/// player format of its stream and the volume and mute its client/state reports. Fed on the
/// dispatch thread; read from any thread.
class ClientWatch {
public:
    struct Seen {
        /// The player stream's format, from its stream/start until its stream/end
        std::optional<StreamFormat> format;
        std::optional<uint32_t> volume;
        std::optional<bool> muted;
    };

    void add(const TracedMessage& message);

    /// @return by client_id
    std::map<std::string, Seen> snapshot() const;

private:
    mutable std::mutex mutex_;
    std::map<std::string, Seen> seen_;
};

/// @brief Runs a browser's commands against the server and the host's playback, and builds the
/// state the browser is shown. Called on the dashboard's thread.
class WebCommands {
public:
    /// @param media nullptr without a media directory
    /// @param one_group the --one-group of the dashboard, which the one_group command sets
    /// @param one_group_fixed --one-group was given, which the dashboard cannot turn off
    WebCommands(SendspinServer& server, WebPlayback& playback, const MediaDir* media,
                const ClientWatch& watch, std::atomic<bool>& one_group, bool one_group_fixed);

    /// @brief Runs one command, a JSON object of at most MAX_COMMAND_BYTES
    /// @return the result message, as web_protocol.md "result"
    std::string run(std::string_view text);

    /// @brief The clients, groups and playback as the browser shows them (web_protocol.md "state")
    std::string state() const;

private:
    SendspinServer& server_;
    WebPlayback& playback_;
    const MediaDir* media_;
    const ClientWatch& watch_;
    std::atomic<bool>& one_group_;
    bool one_group_fixed_;
};

}  // namespace sendspin::server::examples
