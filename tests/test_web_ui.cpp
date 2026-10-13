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

/// @file test_web_ui.cpp
/// @brief server_cli's dashboard end to end: a Beast WebSocket client approves a raw player and
/// plays a file to it, and the HTTP port refuses what it must

#include "audio_source.h"
#include "feeder.h"
#include "raw_client.h"
#include "temp_dir.h"
#include "test_recorder.h"
#include "web_ui.h"
#include <ArduinoJson.h>
#include <boost/asio/connect.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/websocket.hpp>
#include <gtest/gtest.h>

#include <sendspin/server/server.h>

#include <cstdint>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
using tcp = asio::ip::tcp;

using sendspin::server::GroupId;
using sendspin::server::SendspinServer;
using sendspin::server::ServerConfig;
using sendspin::server::StartResult;
using sendspin::server::examples::Console;
using sendspin::server::examples::Feeder;
using sendspin::server::examples::WebOptions;
using sendspin::server::examples::WebPlayback;
using sendspin::server::examples::WebPlaybackStatus;
using sendspin::server::examples::WebUi;
using sendspin::server::test::client_state_json;
using sendspin::server::test::MemoryProvider;
using sendspin::server::test::player_hello_json;
using sendspin::server::test::player_state_json;
using sendspin::server::test::RawClient;
using sendspin::server::test::Recorder;
using sendspin::server::test::TempDir;

constexpr const char* PCM16 =
    R"([{"codec":"pcm","sample_rate":48000,"channels":2,"bit_depth":16}])";

/// A 16-bit stereo 48 kHz WAV file of frames frames of silence
void write_wav(const std::string& path, uint32_t frames) {
    constexpr uint32_t RATE = 48000;
    constexpr uint16_t CHANNELS = 2;
    constexpr uint16_t BITS = 16;
    const uint32_t data_bytes = frames * CHANNELS * BITS / 8;
    std::ofstream file(path, std::ios::binary);
    const auto u32 = [&](uint32_t v) {
        for (int i = 0; i < 4; ++i) {
            file.put(static_cast<char>((v >> (8 * i)) & 0xFF));
        }
    };
    const auto u16 = [&](uint16_t v) {
        file.put(static_cast<char>(v & 0xFF));
        file.put(static_cast<char>(v >> 8));
    };
    file << "RIFF";
    u32(36 + data_bytes);
    file << "WAVEfmt ";
    u32(16);
    u16(1);
    u16(CHANNELS);
    u32(RATE);
    u32(RATE * CHANNELS * BITS / 8);
    u16(CHANNELS * BITS / 8);
    u16(BITS);
    file << "data";
    u32(data_bytes);
    file << std::string(data_bytes, '\0');
}

/// Plays a file on a Feeder of server_cli's, as server_cli's playback does for its play command
class FeederPlayback : public WebPlayback {
public:
    explicit FeederPlayback(SendspinServer& server) : server_(server) {}

    bool play(GroupId group, const std::string& path) override {
        sendspin::server::examples::OpenedAudio opened =
            sendspin::server::examples::open_audio_file(path);
        if (!opened.source) {
            return false;
        }
        sendspin::server::InputOptions options;
        options.format = opened.source->info().format;
        sendspin::server::OpenedInput input = this->server_.open_input(group, options);
        if (input.result != sendspin::server::OpenInputResult::OK) {
            return false;
        }
        const std::lock_guard<std::mutex> lock(this->mutex_);
        this->title_ = path.substr(path.find_last_of('/') + 1);
        this->feeders_[group] = std::make_unique<Feeder>(
            std::move(input.input), std::move(opened.source), [](const Feeder::Report&) {}, false);
        return true;
    }
    void queue(GroupId /*group*/, const std::string& /*path*/) override {}
    bool pause(GroupId /*group*/) override {
        return false;
    }
    bool resume(GroupId /*group*/) override {
        return false;
    }
    void stop(GroupId group) override {
        const std::lock_guard<std::mutex> lock(this->mutex_);
        this->feeders_.erase(group);
    }
    bool seek(GroupId /*group*/, double /*seconds*/) override {
        return false;
    }
    bool next(GroupId /*group*/) override {
        return false;
    }
    void set_loop(GroupId /*group*/, bool /*loop*/) override {}
    bool loop() const override {
        return false;
    }
    bool has_live() const override {
        return false;
    }
    bool live(GroupId /*group*/) override {
        return false;
    }
    bool route(GroupId /*group*/, const std::string& /*client_id*/) override {
        return false;
    }
    std::optional<WebPlaybackStatus> status(GroupId group) override {
        const std::lock_guard<std::mutex> lock(this->mutex_);
        if (!this->feeders_.contains(group)) {
            return std::nullopt;
        }
        WebPlaybackStatus status;
        status.phase = "playing";
        status.title = this->title_;
        return status;
    }

    /// Stops every feeder while the server runs
    void clear() {
        const std::lock_guard<std::mutex> lock(this->mutex_);
        this->feeders_.clear();
    }

private:
    SendspinServer& server_;
    std::mutex mutex_;
    std::string title_;
    std::map<GroupId, std::unique_ptr<Feeder>> feeders_;
};

/// A server with the dashboard on 127.0.0.1, port 0, browsing media
class Dashboard {
public:
    explicit Dashboard(const std::string& media_dir) : playback(server) {
        WebOptions options;
        options.host = "127.0.0.1";
        options.media_dir = media_dir;
        this->web = std::make_unique<WebUi>(options, this->console, this->playback, this->recorder);
        ServerConfig config = sendspin::server::test::local_config();
        config.name = "Dashboard";
        this->web->attach(this->server, config, false);
        EXPECT_EQ(this->server.start(config, &this->provider, this->web->listener()),
                  StartResult::OK);
        EXPECT_TRUE(this->web->start());
    }

    ~Dashboard() {
        this->web->stop();
        this->playback.clear();
        this->server.stop();
    }

    Dashboard(const Dashboard&) = delete;
    Dashboard& operator=(const Dashboard&) = delete;

    // Declared before the server, which calls them until it is destroyed
    Console console;
    Recorder recorder;
    MemoryProvider provider;
    std::unique_ptr<WebUi> web;
    SendspinServer server;
    FeederPlayback playback;
};

/// A browser's WebSocket to the dashboard, read with no timeout
class Browser {
public:
    /// @param host the Host the upgrade names; empty for 127.0.0.1 and the port
    explicit Browser(uint16_t port, const std::string& origin = "", const std::string& host = "")
        : ws_(io_) {
        beast::get_lowest_layer(this->ws_).connect(
            tcp::endpoint(asio::ip::make_address("127.0.0.1"), port));
        if (!origin.empty()) {
            this->ws_.set_option(
                websocket::stream_base::decorator([origin](websocket::request_type& request) {
                    request.set(http::field::origin, origin);
                }));
        }
        this->ws_.handshake(host.empty() ? "127.0.0.1:" + std::to_string(port) : host, "/ws",
                            this->error_);
    }

    /// Whether the upgrade was refused
    bool refused() const {
        return static_cast<bool>(this->error_);
    }

    /// The next message, or nullopt once the dashboard closed the session
    std::optional<JsonDocument> read() {
        beast::flat_buffer buffer;
        beast::error_code error;
        this->ws_.read(buffer, error);
        if (error) {
            return std::nullopt;
        }
        JsonDocument doc;
        EXPECT_EQ(deserializeJson(doc, beast::buffers_to_string(buffer.data())),
                  DeserializationError::Ok);
        return doc;
    }

    /// Reads until a message for which match holds
    JsonDocument read_until(const std::function<bool(const JsonDocument&)>& match) {
        for (;;) {
            std::optional<JsonDocument> doc = this->read();
            if (!doc) {
                ADD_FAILURE() << "the dashboard closed the session";
                return JsonDocument();
            }
            if (match(*doc)) {
                return std::move(*doc);
            }
        }
    }

    void send(const std::string& text) {
        this->ws_.text(true);
        this->ws_.write(asio::buffer(text));
    }

    /// Sends a command and reads until its result
    JsonDocument command(const std::string& text, int id) {
        this->send(text);
        return this->read_until(
            [&](const JsonDocument& doc) { return doc["type"] == "result" && doc["id"] == id; });
    }

private:
    asio::io_context io_;
    websocket::stream<beast::tcp_stream> ws_;
    beast::error_code error_;
};

/// The state's entry of client_id, or null
JsonObjectConst client_in(const JsonDocument& state, const std::string& client_id) {
    for (JsonObjectConst client : state["clients"].as<JsonArrayConst>()) {
        if (client["client_id"] == client_id) {
            return client;
        }
    }
    return {};
}

// A browser connects and gets the snapshot, then the state; it approves a player that waits for
// approval and plays a file of the media directory to the server's group, and sees the group
// playing it, while the player gets its stream
TEST(WebUi, ApprovesAPlayerAndPlaysAFileFromTheBrowser) {
    TempDir media;
    // 30 s of audio, so the group still plays when the state shows it however slow the run
    constexpr uint32_t FRAMES = 30 * 48000;
    write_wav(media.file("tone.wav"), FRAMES);
    Dashboard dashboard(media.file(""));
    Browser browser(dashboard.web->port());
    const JsonDocument snapshot =
        browser.read_until([](const JsonDocument& doc) { return doc["type"] == "snapshot"; });
    EXPECT_EQ(snapshot["server"]["name"], "Dashboard");
    EXPECT_EQ(snapshot["server"]["server_id"], dashboard.server.server_id());
    browser.read_until([](const JsonDocument& doc) { return doc["type"] == "state"; });

    RawClient player;
    const std::string id = player.noise.identity().peer_id();
    ASSERT_TRUE(dashboard.server.connect_url(player.url()));
    player.activate(player_hello_json("Kitchen", PCM16, 1000000));
    // Held for approval: no roles, so it counts as connected at once
    const JsonDocument held = browser.read_until([&](const JsonDocument& doc) {
        const JsonObjectConst client = client_in(doc, id);
        return doc["type"] == "state" && !client.isNull() && client["connected"] == true &&
               client["approved"] == false;
    });
    // The roles it may be activated with, which the dashboard shows
    const JsonArrayConst roles = client_in(held, id)["roles"];
    ASSERT_EQ(roles.size(), 1U);
    EXPECT_EQ(roles[0], "player@v1");

    JsonDocument result =
        browser.command(R"({"id":1,"cmd":"approve","client":")" + id + R"("})", 1);
    ASSERT_TRUE(result["ok"].as<bool>()) << result["error"].as<std::string>();
    player.receive_until("server/activate");
    player.send_json(client_state_json(true, player_state_json(150, 0, 0)));
    const uint64_t group = dashboard.server.default_group()->value;
    browser.read_until([&](const JsonDocument& doc) {
        const JsonObjectConst client = client_in(doc, id);
        return doc["type"] == "state" && !client.isNull() && client["approved"] == true &&
               client["group"] == group;
    });

    result = browser.command(
        R"({"id":2,"cmd":"play","group":)" + std::to_string(group) + R"(,"path":"tone.wav"})", 2);
    ASSERT_TRUE(result["ok"].as<bool>()) << result["error"].as<std::string>();
    player.receive_until("stream/start");
    const JsonDocument playing = browser.read_until([&](const JsonDocument& doc) {
        if (doc["type"] != "state") {
            return false;
        }
        for (JsonObjectConst entry : doc["groups"].as<JsonArrayConst>()) {
            if (entry["id"] == group && entry["playing"] == true) {
                return true;
            }
        }
        return false;
    });
    for (JsonObjectConst entry : playing["groups"].as<JsonArrayConst>()) {
        if (entry["id"] == group) {
            EXPECT_EQ(entry["playback"]["title"], "tone.wav");
            EXPECT_EQ(entry["members"][0], id);
        }
    }
}

// The WebSocket upgrade is refused to a page of another site, and to a name that is not loopback
// while the dashboard is bound to loopback, even with a matching Origin (DNS rebinding). Control:
// no Origin, and the dashboard's own.
TEST(WebUi, RefusesUpgradesFromOtherOrigins) {
    TempDir media;
    Dashboard dashboard(media.file(""));
    const uint16_t port = dashboard.web->port();
    const std::string own = "http://127.0.0.1:" + std::to_string(port);
    const std::string rebound = "evil.example:" + std::to_string(port);
    struct Row {
        const char* name;
        std::string origin;
        std::string host;
        bool refused;
    };
    for (const Row& row :
         {Row{"Control: no Origin", "", "", false}, Row{"Control: own", own, "", false},
          Row{"another site", "http://evil.example", "", true},
          Row{"another port", "http://127.0.0.1:1", "", true},
          Row{"a rebound name", "http://" + rebound, rebound, true}}) {
        SCOPED_TRACE(row.name);
        Browser browser(port, row.origin, row.host);
        EXPECT_EQ(browser.refused(), row.refused);
        if (!row.refused) {
            const std::optional<JsonDocument> first = browser.read();
            ASSERT_TRUE(first);
            EXPECT_EQ((*first)["type"], "snapshot");
        }
    }
}

// A command past MAX_COMMAND_BYTES closes the browser's session; one within it is answered.
// Control: a command at the limit.
TEST(WebUi, ClosesASessionThatSendsTooMuch) {
    TempDir media;
    Dashboard dashboard(media.file(""));
    const std::string command = R"({"id":5,"cmd":"bogus"})";
    for (const size_t size : {sendspin::server::examples::MAX_COMMAND_BYTES,
                              sendspin::server::examples::MAX_COMMAND_BYTES + 1}) {
        const bool control = size == sendspin::server::examples::MAX_COMMAND_BYTES;
        SCOPED_TRACE(control ? "Control: at the limit" : "past the limit");
        Browser browser(dashboard.web->port());
        browser.send(std::string(size - command.size(), ' ') + command);
        bool answered = false;
        while (std::optional<JsonDocument> doc = browser.read()) {
            if ((*doc)["type"] == "result") {
                answered = true;
                break;
            }
        }
        EXPECT_EQ(answered, control);
    }
}

/// The status of an HTTP request to the dashboard
unsigned http_status(uint16_t port, http::request<http::string_body> request) {
    asio::io_context io;
    beast::tcp_stream stream(io);
    stream.connect(tcp::endpoint(asio::ip::make_address("127.0.0.1"), port));
    request.set(http::field::host, "127.0.0.1:" + std::to_string(port));
    http::write(stream, request);
    beast::flat_buffer buffer;
    http::response<http::string_body> response;
    beast::error_code error;
    http::read(stream, buffer, response, error);
    return error ? 0 : response.result_int();
}

// An image upload is refused from its header, before its body: past artwork_limits::
// MAX_IMAGE_BYTES by its Content-Length, without the dashboard's header, from another site, with
// a malformed target or an unknown group. Control: the page, and an upload that is not an image,
// refused by the server once its body is read.
TEST(WebUi, RefusesUploadsItMustNot) {
    TempDir media;
    Dashboard dashboard(media.file(""));
    const uint16_t port = dashboard.web->port();
    const std::string target =
        "/artwork?group=" + std::to_string(dashboard.server.default_group()->value) +
        "&source=album";
    const auto upload = [&](size_t length, bool header, const std::string& origin,
                            const std::string& to) {
        http::request<http::string_body> request{http::verb::post, to, 11};
        if (header) {
            request.set("X-Sendspin-Dashboard", "1");
        }
        if (!origin.empty()) {
            request.set(http::field::origin, origin);
        }
        request.body() = "not an image";
        request.prepare_payload();
        // Claimed, not sent: the dashboard answers from the header
        request.set(http::field::content_length, std::to_string(length));
        return request;
    };
    const std::string own = "http://127.0.0.1:" + std::to_string(port);
    constexpr size_t SMALL = 12;
    const size_t too_large = sendspin::server::artwork_limits::MAX_IMAGE_BYTES + 1;
    EXPECT_EQ(http_status(port, http::request<http::string_body>{http::verb::get, "/", 11}), 200U);
    EXPECT_EQ(http_status(port, upload(SMALL, true, own, target)), 422U);
    EXPECT_EQ(http_status(port, upload(too_large, true, own, target)), 413U);
    // Refused from the header, before the body: these claim a body they never send, which a read
    // of it would wait for until the test watchdog
    const size_t claimed = sendspin::server::artwork_limits::MAX_IMAGE_BYTES;
    EXPECT_EQ(http_status(port, upload(claimed, false, own, target)), 403U);
    EXPECT_EQ(http_status(port, upload(claimed, true, "http://evil.example", target)), 403U);
    EXPECT_EQ(http_status(port, upload(claimed, true, own, "/artwork?group=x&source=album")), 400U);
    EXPECT_EQ(http_status(port, upload(claimed, true, own, "/artwork?group=999999&source=album")),
              404U);
}

}  // namespace
