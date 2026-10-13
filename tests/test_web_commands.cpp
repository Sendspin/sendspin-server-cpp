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

/// @file test_web_commands.cpp
/// @brief server_cli's dashboard without its transport: the commands a browser sends, the request
/// checks, the media directory's confinement, the trace it watches and the text it sends

#include "temp_dir.h"
#include "test_recorder.h"
#include "web_commands.h"
#include "web_ui.h"
#include <ArduinoJson.h>
#include <gtest/gtest.h>

#include <sendspin/server/server.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace {

using sendspin::server::GroupId;
using sendspin::server::SendspinServer;
using sendspin::server::StartResult;
using sendspin::server::StreamCodec;
using sendspin::server::StreamFormat;
using sendspin::server::TraceDirection;
using sendspin::server::TracedMessage;
using sendspin::server::examples::ClientWatch;
using sendspin::server::examples::MAX_COMMAND_BYTES;
using sendspin::server::examples::MediaDir;
using sendspin::server::examples::WebCommands;
using sendspin::server::examples::WebPlayback;
using sendspin::server::examples::WebPlaybackStatus;
using sendspin::server::test::MemoryProvider;
using sendspin::server::test::TempDir;

/// Records each playback call as "name group [argument]"
class FakePlayback : public WebPlayback {
public:
    bool play(GroupId group, const std::string& path) override {
        this->calls.push_back("play " + std::to_string(group.value) + " " + path);
        return true;
    }
    void queue(GroupId group, const std::string& path) override {
        this->calls.push_back("queue " + std::to_string(group.value) + " " + path);
    }
    bool pause(GroupId group) override {
        this->calls.push_back("pause " + std::to_string(group.value));
        return this->succeeds;
    }
    bool resume(GroupId group) override {
        this->calls.push_back("resume " + std::to_string(group.value));
        return this->succeeds;
    }
    void stop(GroupId group) override {
        this->calls.push_back("stop " + std::to_string(group.value));
    }
    bool seek(GroupId group, double seconds) override {
        this->calls.push_back("seek " + std::to_string(group.value) + " " +
                              std::to_string(seconds));
        return this->succeeds;
    }
    bool next(GroupId group) override {
        this->calls.push_back("next " + std::to_string(group.value));
        return this->succeeds;
    }
    void set_loop(GroupId group, bool loop) override {
        this->calls.push_back("loop " + std::to_string(group.value) + (loop ? " on" : " off"));
    }
    bool loop() const override {
        return false;
    }
    bool has_live() const override {
        return false;
    }
    bool live(GroupId /*group*/) override {
        return false;
    }
    bool route(GroupId group, const std::string& client_id) override {
        this->calls.push_back("route " + std::to_string(group.value) + " " + client_id);
        return this->succeeds;
    }
    std::optional<WebPlaybackStatus> status(GroupId /*group*/) override {
        return std::nullopt;
    }

    std::vector<std::string> calls;
    /// What pause, resume, seek and next report: false when they find nothing to act on
    bool succeeds{true};
};

/// A running server with no clients, the dashboard's commands over it and a media directory
class CommandsTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_EQ(
            this->server.start(sendspin::server::test::local_config(), &this->provider, nullptr),
            StartResult::OK);
        this->group = std::to_string(this->server.default_group()->value);
    }

    void TearDown() override {
        this->server.stop();
    }

    /// The result of text, run with media when given
    JsonDocument run(const std::string& text, const MediaDir* media = nullptr,
                     bool one_group_fixed = false) {
        WebCommands commands(this->server, this->playback, media, this->watch, this->one_group,
                             one_group_fixed);
        JsonDocument result;
        EXPECT_EQ(deserializeJson(result, commands.run(text)), DeserializationError::Ok);
        EXPECT_EQ(result["type"], "result");
        return result;
    }

    MemoryProvider provider;
    SendspinServer server;
    FakePlayback playback;
    ClientWatch watch;
    std::atomic<bool> one_group{false};
    std::string group;
};

// A command is a JSON object with an id and a cmd; the result carries the id when there is one
// and says why it failed. Each rejected row leaves the playback and the server untouched.
TEST_F(CommandsTest, RejectsMalformedCommandsAndUnknownIds) {
    const std::string g = this->group;
    struct Row {
        const char* name;
        std::string text;
        /// Empty for success
        const char* error;
        /// The playback call a success makes
        const char* call;
    };
    const std::vector<Row> rows{
        {"Control: a transport command", R"({"id":7,"cmd":"pause","group":)" + g + "}", "",
         "pause"},
        {"Control: a volume in range",
         R"({"id":7,"cmd":"volume","group":)" + g + R"(,"volume":100})", "", ""},
        {"not JSON", "{", "not a JSON object", ""},
        {"an array", "[1]", "not a JSON object", ""},
        {"no id", R"({"cmd":"pause","group":)" + g + "}", "an id from 0 to 4294967295 is required",
         ""},
        {"a negative id", R"({"id":-1,"cmd":"pause","group":)" + g + "}",
         "an id from 0 to 4294967295 is required", ""},
        {"a string id", R"({"id":"7","cmd":"pause","group":)" + g + "}",
         "an id from 0 to 4294967295 is required", ""},
        {"no cmd", R"({"id":7,"group":)" + g + "}", "a cmd is required", ""},
        {"an unknown cmd", R"({"id":7,"cmd":"reboot"})", "unknown command", ""},
        {"an unknown group", R"({"id":7,"cmd":"pause","group":999999})", "no such group", ""},
        {"a group as a string", R"({"id":7,"cmd":"pause","group":")" + g + R"("})", "no such group",
         ""},
        {"an unknown client", R"({"id":7,"cmd":"approve","client":"nobody"})", "no such client",
         ""},
        {"an empty client", R"({"id":7,"cmd":"unpair","client":""})", "a client is required", ""},
        {"a volume past 100", R"({"id":7,"cmd":"volume","group":)" + g + R"(,"volume":101})",
         "a volume from 0 to 100 is required", ""},
        {"a negative seek", R"({"id":7,"cmd":"seek","group":)" + g + R"(,"seconds":-1})",
         "a position in seconds is required", ""},
        {"a color that is not hex",
         R"({"id":7,"cmd":"colors","group":)" + g + R"(,"colors":{"primary":"12345g"}})",
         "colors must map background_dark, background_light, primary, accent, on_dark or "
         "on_light to rrggbb",
         ""},
        {"an unknown color",
         R"({"id":7,"cmd":"colors","group":)" + g +
             R"(,"colors":{"primary":"123456","glow":"123456"}})",
         "colors must map background_dark, background_light, primary, accent, on_dark or "
         "on_light to rrggbb",
         ""},
        {"a format with an unknown codec",
         R"({"id":7,"cmd":"format","client":"x","format":{"codec":"mp3","sample_rate":48000,)"
         R"("channels":2,"bit_depth":16}})",
         "the format needs codec (pcm, flac or opus), sample_rate, channels and bit_depth", ""},
        {"a pairing method the REPL does not take",
         R"({"id":7,"cmd":"pair","client":"x","method":"nfc"})",
         "the method is psk, static, digits or qr, or none for the code the client shows", ""},
        {"play with no media directory",
         R"({"id":7,"cmd":"play","group":)" + g + R"(,"path":"a.wav"})",
         "server_cli was not started with --media-dir", ""},
        {"live with no standard input", R"({"id":7,"cmd":"live","group":)" + g + "}",
         "server_cli was not started with --stdin", ""},
    };
    for (const Row& row : rows) {
        SCOPED_TRACE(row.name);
        this->playback.calls.clear();
        const JsonDocument result = this->run(row.text);
        const bool has_id = std::string(row.error) != "not a JSON object" &&
                            std::string(row.error).find("an id") == std::string::npos;
        EXPECT_EQ(result["id"].isNull(), !has_id);
        if (has_id) {
            EXPECT_EQ(result["id"], 7);
        }
        if (std::string(row.error).empty()) {
            EXPECT_TRUE(result["ok"].as<bool>()) << result["error"].as<std::string>();
            if (std::string(row.call).empty()) {
                EXPECT_TRUE(this->playback.calls.empty());
            } else {
                EXPECT_EQ(this->playback.calls,
                          std::vector<std::string>{std::string(row.call) + " " + g});
            }
        } else {
            EXPECT_FALSE(result["ok"].as<bool>());
            EXPECT_EQ(result["error"], row.error);
            EXPECT_TRUE(this->playback.calls.empty());
        }
    }
}

// A command longer than MAX_COMMAND_BYTES is refused before it is parsed; one at the limit runs.
// Control: the same command padded to exactly the limit.
TEST_F(CommandsTest, RefusesACommandPastTheSizeLimit) {
    const std::string command = R"({"id":3,"cmd":"stop","group":)" + this->group + "}";
    for (const size_t size : {MAX_COMMAND_BYTES, MAX_COMMAND_BYTES + 1}) {
        SCOPED_TRACE(size == MAX_COMMAND_BYTES ? "Control: at the limit" : "past the limit");
        this->playback.calls.clear();
        // JSON whitespace before the object keeps it valid
        const std::string text = std::string(size - command.size(), ' ') + command;
        ASSERT_EQ(text.size(), size);
        const JsonDocument result = this->run(text);
        if (size == MAX_COMMAND_BYTES) {
            EXPECT_TRUE(result["ok"].as<bool>());
            EXPECT_EQ(this->playback.calls.size(), 1U);
        } else {
            EXPECT_FALSE(result["ok"].as<bool>());
            EXPECT_EQ(result["error"], "the command is too large");
            EXPECT_TRUE(this->playback.calls.empty());
        }
    }
}

// A transport command the playback finds nothing to act on fails with why. Control: one it acts
// on.
TEST_F(CommandsTest, ReportsATransportCommandThatDoesNothing) {
    const std::string g = this->group;
    const std::vector<std::pair<std::string, const char*>> rows{
        {R"({"id":1,"cmd":"pause","group":)" + g + "}", "nothing to pause"},
        {R"({"id":1,"cmd":"resume","group":)" + g + "}", "nothing is paused"},
        {R"({"id":1,"cmd":"seek","group":)" + g + R"(,"seconds":2})",
         "nothing to seek; the log says why"},
        {R"({"id":1,"cmd":"next","group":)" + g + "}", "nothing to skip; the log says why"},
    };
    for (const bool succeeds : {true, false}) {
        SCOPED_TRACE(succeeds ? "Control: the playback acts" : "nothing to act on");
        this->playback.succeeds = succeeds;
        for (const auto& [text, error] : rows) {
            SCOPED_TRACE(text);
            const JsonDocument result = this->run(text);
            EXPECT_EQ(result["ok"].as<bool>(), succeeds);
            if (!succeeds) {
                EXPECT_EQ(result["error"], error);
            }
        }
    }
}

// The commands reach the server as the REPL's do: a rename shows in groups(); and the one-group
// mode the dashboard set cannot be cleared when --one-group set it. Control: cleared when the
// dashboard set it.
TEST_F(CommandsTest, CommandsActOnTheServer) {
    JsonDocument result =
        this->run(R"({"id":1,"cmd":"rename_group","group":)" + this->group + R"(,"name":"Den"})");
    ASSERT_TRUE(result["ok"].as<bool>());
    // The rename runs on core; a regression hangs into the suite watchdog
    while (this->server.groups().front().name != "Den") {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    // route: the playback routes the client's source into the group
    result = this->run(R"({"id":3,"cmd":"route","client":"abc","group":)" + this->group + "}");
    EXPECT_TRUE(result["ok"].as<bool>());
    EXPECT_EQ(this->playback.calls, std::vector<std::string>{"route " + this->group + " abc"});

    for (const bool fixed : {true, false}) {
        SCOPED_TRACE(fixed ? "--one-group given" : "Control: set by the dashboard");
        this->one_group = true;
        result = this->run(R"({"id":2,"cmd":"one_group","on":false})", nullptr, fixed);
        EXPECT_EQ(result["ok"].as<bool>(), !fixed);
        EXPECT_EQ(this->one_group.load(), fixed);
    }
}

/// A media directory with a file and a subdirectory, beside a sibling directory whose name starts
/// with the media directory's and a directory outside it, with links leading in and out
class MediaTest : public ::testing::Test {
protected:
    void SetUp() override {
        namespace fs = std::filesystem;
        this->root = this->dir.file("media");
        fs::create_directories(this->root + "/sub");
        fs::create_directories(this->dir.file("media2"));
        fs::create_directories(this->dir.file("outside"));
        for (const std::string& file :
             {this->root + "/a.wav", this->root + "/sub/b.flac", this->root + "/notes.txt",
              this->root + "/.hidden.wav", this->dir.file("media2/c.wav"),
              this->dir.file("outside/d.wav")}) {
            std::ofstream(file) << "x";
        }
        fs::create_symlink(this->dir.file("outside"), this->root + "/link_out");
        fs::create_symlink(this->root + "/sub/b.flac", this->root + "/link_in.flac");
        this->media = MediaDir::open(this->root);
        ASSERT_TRUE(this->media);
    }

    TempDir dir;
    std::string root;
    std::optional<MediaDir> media;
};

// Nothing outside the media directory resolves: not by "..", an absolute path, a link leading
// out, or a sibling whose name only starts with the root's. Control: paths inside, a link leading
// inside, and ".." that stays inside.
TEST_F(MediaTest, ResolvesOnlyPathsInsideTheRoot) {
    namespace fs = std::filesystem;
    const fs::path root = fs::canonical(this->root);
    struct Row {
        const char* relative;
        /// Empty when refused
        std::string resolved;
    };
    const std::vector<Row> rows{
        {"", root.string()},
        {"a.wav", (root / "a.wav").string()},
        {"sub/b.flac", (root / "sub/b.flac").string()},
        {"sub/../a.wav", (root / "a.wav").string()},
        {"link_in.flac", (root / "sub/b.flac").string()},
        {"../media2/c.wav", ""},
        {"../outside/d.wav", ""},
        {"sub/../../outside/d.wav", ""},
        {"link_out/d.wav", ""},
        {"link_out", ""},
        {"/etc/hosts", ""},
        {"missing.wav", ""},
    };
    for (const Row& row : rows) {
        SCOPED_TRACE(row.relative);
        const std::optional<fs::path> resolved = this->media->resolve(row.relative);
        EXPECT_EQ(resolved ? resolved->string() : "", row.resolved);
    }
}

// A listing holds directories first, then WAV and FLAC files, by name: no hidden file, no other
// file, no link leading out; a path outside is not listed
TEST_F(MediaTest, ListsDirectoriesAndAudioFilesInside) {
    const std::optional<MediaDir::Listing> listing = this->media->list("");
    ASSERT_TRUE(listing);
    std::vector<std::string> names;
    for (const MediaDir::Entry& entry : listing->entries) {
        names.push_back(entry.name + (entry.directory ? "/" : ""));
    }
    EXPECT_EQ(names, (std::vector<std::string>{"sub/", "a.wav", "link_in.flac"}));
    EXPECT_FALSE(listing->truncated);
    EXPECT_FALSE(this->media->list(".."));
    EXPECT_FALSE(this->media->list("link_out"));
    EXPECT_FALSE(this->media->list("a.wav"));
}

// play and queue take only a file inside the media directory, which reaches the playback as its
// resolved path. Control: a file inside.
TEST_F(MediaTest, PlaysOnlyFilesInside) {
    MemoryProvider provider;
    SendspinServer server;
    ASSERT_EQ(server.start(sendspin::server::test::local_config(), &provider, nullptr),
              StartResult::OK);
    FakePlayback playback;
    ClientWatch watch;
    std::atomic<bool> one_group{false};
    WebCommands commands(server, playback, &*this->media, watch, one_group, false);
    const std::string group = std::to_string(server.default_group()->value);
    const std::string resolved = std::filesystem::canonical(this->root + "/sub/b.flac").string();
    struct Row {
        const char* path;
        const char* cmd;
        bool ok;
    };
    for (const Row& row : std::vector<Row>{{"sub/b.flac", "play", true},
                                           {"sub/b.flac", "queue", true},
                                           {"../outside/d.wav", "play", false},
                                           {"link_out/d.wav", "queue", false},
                                           {"sub", "play", false},
                                           {"/etc/hosts", "play", false}}) {
        SCOPED_TRACE(std::string(row.cmd) + " " + row.path);
        playback.calls.clear();
        JsonDocument result;
        ASSERT_EQ(deserializeJson(result, commands.run(R"({"id":1,"cmd":")" + std::string(row.cmd) +
                                                       R"(","group":)" + group + R"(,"path":")" +
                                                       row.path + R"("})")),
                  DeserializationError::Ok);
        EXPECT_EQ(result["ok"].as<bool>(), row.ok) << result["error"].as<std::string>();
        if (row.ok) {
            EXPECT_EQ(playback.calls, std::vector<std::string>{std::string(row.cmd) + " " + group +
                                                               " " + resolved});
        } else {
            EXPECT_EQ(result["error"], "not a file in the media directory");
            EXPECT_TRUE(playback.calls.empty());
        }
    }
    server.stop();
}

// A browser's request acts only from a page of the dashboard's own origin, and only under a name
// no other site's page can be reached under: bound to loopback a loopback name, beyond it also an
// IP address or a .local name; a request with no Origin is not a browser's
TEST(WebRequest, AllowsOnlyTheDashboardsOrigin) {
    struct Row {
        const char* name;
        const char* origin;
        const char* host;
        bool loopback_only;
        bool allowed;
    };
    const std::vector<Row> rows{
        {"Control: the page's origin", "http://127.0.0.1:8080", "127.0.0.1:8080", true, true},
        {"Control: no Origin", "", "127.0.0.1:8080", true, true},
        {"Control: names compare without case", "http://LocalHost:8080", "localhost:8080", true,
         true},
        {"Control: IPv6 loopback", "http://[::1]:8080", "[::1]:8080", true, true},
        {"Control: a .local name when bound beyond loopback", "http://speakers.local:8080",
         "speakers.local:8080", false, true},
        {"Control: an IP address when bound beyond loopback", "http://192.168.1.20:8080",
         "192.168.1.20:8080", false, true},
        {"a rebound name beyond loopback", "http://evil.example:8080", "evil.example:8080", false,
         false},
        {"a rebound name beyond loopback, no Origin", "", "evil.example:8080", false, false},
        {"another site", "http://evil.example", "127.0.0.1:8080", true, false},
        {"another port", "http://127.0.0.1:9090", "127.0.0.1:8080", true, false},
        {"another scheme", "https://127.0.0.1:8080", "127.0.0.1:8080", true, false},
        {"an opaque origin", "null", "127.0.0.1:8080", true, false},
        {"a rebound name on loopback", "http://evil.example:8080", "evil.example:8080", true,
         false},
        {"no Host", "", "", false, false},
    };
    for (const Row& row : rows) {
        SCOPED_TRACE(row.name);
        EXPECT_EQ(
            sendspin::server::examples::request_allowed(row.origin, row.host, row.loopback_only),
            row.allowed);
    }
}

// An upload names its group by decimal id and its source as album or artist, in any order
TEST(WebRequest, ParsesTheArtworkTarget) {
    using sendspin::server::ArtworkSource;
    struct Row {
        const char* target;
        std::optional<std::pair<uint64_t, ArtworkSource>> parsed;
    };
    const std::vector<Row> rows{
        {"/artwork?group=3&source=album", std::make_pair(uint64_t{3}, ArtworkSource::ALBUM)},
        {"/artwork?source=artist&group=12", std::make_pair(uint64_t{12}, ArtworkSource::ARTIST)},
        {"/artwork?group=3", std::nullopt},
        {"/artwork?group=&source=album", std::nullopt},
        {"/artwork?group=-3&source=album", std::nullopt},
        {"/artwork?group=+3&source=album", std::nullopt},
        {"/artwork?group=3x&source=album", std::nullopt},
        {"/artwork?group=99999999999999999999999&source=album", std::nullopt},
        {"/artwork?group=3&source=cover", std::nullopt},
        {"/artworks?group=3&source=album", std::nullopt},
        {"/other?group=3&source=album", std::nullopt},
    };
    for (const Row& row : rows) {
        SCOPED_TRACE(row.target);
        const std::optional<sendspin::server::examples::ArtworkTarget> target =
            sendspin::server::examples::parse_artwork_target(row.target);
        ASSERT_EQ(target.has_value(), row.parsed.has_value());
        if (target) {
            EXPECT_EQ(target->group.value, row.parsed->first);
            EXPECT_EQ(target->source, row.parsed->second);
        }
    }
}

// --web takes a port, on 127.0.0.1, or a numeric address and a port
TEST(WebRequest, ParsesTheWebAddress) {
    struct Row {
        const char* text;
        std::optional<std::pair<std::string, uint16_t>> parsed;
    };
    const std::vector<Row> rows{
        {"8080", std::make_pair(std::string("127.0.0.1"), uint16_t{8080})},
        {"0.0.0.0:8080", std::make_pair(std::string("0.0.0.0"), uint16_t{8080})},
        {"[::1]:0", std::make_pair(std::string("::1"), uint16_t{0})},
        {"localhost:80", std::make_pair(std::string("127.0.0.1"), uint16_t{80})},
        {"", std::nullopt},
        {"80a", std::nullopt},
        {"+80", std::nullopt},
        {"65536", std::nullopt},
        {"::1:80", std::nullopt},
        {"speakers.local:80", std::nullopt},
    };
    for (const Row& row : rows) {
        SCOPED_TRACE(row.text);
        EXPECT_EQ(sendspin::server::examples::parse_web_address(row.text), row.parsed);
    }
}

// Text sent to a browser is UTF-8 (RFC 3629): each byte that does not begin a well-formed
// sequence becomes U+FFFD
TEST(WebText, ReplacesWhatIsNotUtf8) {
    const std::string bad = "\xEF\xBF\xBD";
    struct Row {
        const char* name;
        std::string text;
        std::string sent;
    };
    const std::vector<Row> rows{
        {"Control: ASCII", "Kitchen", "Kitchen"},
        {"Control: two, three and four bytes", "\xC3\xA9\xE2\x82\xAC\xF0\x9F\x8E\xB5",
         "\xC3\xA9\xE2\x82\xAC\xF0\x9F\x8E\xB5"},
        {"a lone continuation byte", "a\x80z", "a" + bad + "z"},
        {"a sequence cut short", "a\xE2\x82", "a" + bad + bad},
        {"an overlong form", "\xC0\xAF", bad + bad},
        {"a surrogate", "\xED\xA0\x80", bad + bad + bad},
        {"past U+10FFFF", "\xF4\x90\x80\x80", bad + bad + bad + bad},
    };
    for (const Row& row : rows) {
        SCOPED_TRACE(row.name);
        EXPECT_EQ(sendspin::server::examples::valid_utf8(row.text), row.sent);
    }
}

TracedMessage traced(TraceDirection direction, const std::string& json, bool binary = true,
                     uint8_t id = 0) {
    TracedMessage message;
    message.client_id = "c";
    message.direction = direction;
    message.binary = binary;
    if (binary) {
        message.plaintext.push_back(id);
    }
    message.plaintext.insert(message.plaintext.end(), json.begin(), json.end());
    return message;
}

// The trace shows a player's format from its stream/start until a stream/end that ends the
// player's stream, and its volume and mute from its client/state
TEST(WebClientWatch, FollowsFormatVolumeAndMute) {
    ClientWatch watch;
    const std::string start =
        R"({"type":"stream/start","payload":{"player":{"codec":"flac","sample_rate":48000,)"
        R"("channels":2,"bit_depth":24}}})";
    const StreamFormat flac{StreamCodec::FLAC, 48000, 2, 24};
    watch.add(traced(TraceDirection::OUTBOUND, start));
    EXPECT_EQ(watch.snapshot()["c"].format, flac);
    // An artwork stream's end leaves the player's
    watch.add(traced(TraceDirection::OUTBOUND,
                     R"({"type":"stream/end","payload":{"roles":["artwork"]}})"));
    EXPECT_EQ(watch.snapshot()["c"].format, flac);
    watch.add(traced(TraceDirection::OUTBOUND,
                     R"({"type":"stream/end","payload":{"roles":["player"]}})"));
    EXPECT_FALSE(watch.snapshot()["c"].format);
    // A binary message of another ID is not JSON
    watch.add(traced(TraceDirection::OUTBOUND, start, true, 4));
    EXPECT_FALSE(watch.snapshot()["c"].format);

    watch.add(traced(TraceDirection::INBOUND,
                     R"({"type":"client/state","payload":{"player":{"volume":30,"muted":true}}})"));
    EXPECT_EQ(watch.snapshot()["c"].volume, 30U);
    EXPECT_EQ(watch.snapshot()["c"].muted, true);
}

}  // namespace
