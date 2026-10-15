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

/// @file test_server_log.cpp
/// @brief The log a host receives through ServerConfig::log: the levels it filters by, no key
/// material in any line, and each line's place among the dispatch thread's calls. Lines are
/// counted by level, never matched by wording.

#include "crypto/base64url.h"
#include "crypto/keys.h"
#include "log.h"
#include "raw_client.h"
#include "test_ports.h"
#include "test_recorder.h"
#include <gtest/gtest.h>

#include <sendspin/server/persistence_keys.h>
#include <sendspin/server/server.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <future>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace {

using sendspin::server::DisconnectReason;
using sendspin::server::ErrorKind;
using sendspin::server::Identity;
using sendspin::server::LogLevel;
using sendspin::server::SendspinServer;
using sendspin::server::ServerConfig;
using sendspin::server::StartResult;
using sendspin::server::test::client_goodbye_json;
using sendspin::server::test::LocalServer;
using sendspin::server::test::RawClient;
using sendspin::server::test::Recorder;

/// Collects the lines ServerConfig::log receives
class LogRecorder {
public:
    struct Line {
        LogLevel level;
        std::string text;
    };

    sendspin::server::LogFn sink() {
        return [this](LogLevel level, std::string_view text) {
            const std::lock_guard<std::mutex> lock(this->mutex_);
            this->lines_.push_back({level, std::string(text)});
        };
    }

    std::vector<Line> lines() const {
        const std::lock_guard<std::mutex> lock(this->mutex_);
        return this->lines_;
    }

    size_t count(LogLevel level) const {
        const std::lock_guard<std::mutex> lock(this->mutex_);
        return static_cast<size_t>(
            std::count_if(this->lines_.begin(), this->lines_.end(),
                          [level](const Line& line) { return line.level == level; }));
    }

private:
    mutable std::mutex mutex_;
    std::vector<Line> lines_;
};

std::string error_entry(ErrorKind kind, const std::string& url = "") {
    return "error " + std::to_string(static_cast<int>(kind)) + " " + url;
}

std::string disconnected(const std::string& client_id) {
    return "disconnected " + client_id + " " +
           std::to_string(static_cast<int>(DisconnectReason::GOODBYE));
}

/// A client that connects, is activated with no roles (not approved) and says goodbye, then one
/// whose handshake fails, then a dial that cannot connect. Each line is posted before the listener
/// call that reports the same change, so once the recorder saw the last call, the log holds every
/// line.
void connect_fail_and_dial(SendspinServer& server, Recorder& recorder) {
    RawClient client;
    const std::string id = client.noise.identity().peer_id();
    ASSERT_EQ(client.dial(server.listen_port()), 101U);
    ASSERT_NO_FATAL_FAILURE(client.complete_handshake());
    client.send_json(sendspin::server::test::client_hello_json());
    EXPECT_NE(client.receive_until("server/activate"), "");
    recorder.wait_for("connected " + id);
    client.send_json(client_goodbye_json("shutdown"));
    recorder.wait_for(disconnected(id));

    RawClient failing;
    ASSERT_EQ(failing.dial(server.listen_port()), 101U);
    failing.drop();
    recorder.wait_for(error_entry(ErrorKind::HANDSHAKE_FAILED));

    const sendspin::server::test::UnlistenedPort unlistened;
    const std::string unreachable =
        "ws://127.0.0.1:" + std::to_string(unlistened.port()) + "/sendspin";
    ASSERT_TRUE(server.connect_url(unreachable));
    EXPECT_EQ(recorder.wait_for_error(unreachable).kind, ErrorKind::DIAL_FAILED);
}

// A failed dial is a warning; a connection and its end, an accepted connection's failed
// handshake, which any peer can repeat, and a dial are information. A level logs its own lines and
// those before it, and OFF none.
TEST(ServerLog, FiltersByLevel) {
    struct Row {
        const char* name;
        LogLevel level;
        size_t warnings;
        size_t infos;
    };
    const Row rows[] = {
        {"off", LogLevel::OFF, 0, 0},
        {"errors only", LogLevel::ERROR, 0, 0},
        {"Control: warnings, the default", LogLevel::WARN, 1, 0},
        {"Control: information", LogLevel::INFO, 1, 4},
    };
    for (const Row& row : rows) {
        SCOPED_TRACE(row.name);
        LogRecorder log;
        ServerConfig config = sendspin::server::test::local_config();
        config.log = log.sink();
        config.log_level = row.level;
        LocalServer s;
        ASSERT_EQ(s.start(config), StartResult::OK);
        ASSERT_NO_FATAL_FAILURE(connect_fail_and_dial(s.server, s.recorder));
        EXPECT_EQ(log.count(LogLevel::ERROR), 0U);
        EXPECT_EQ(log.count(LogLevel::WARN), row.warnings);
        EXPECT_EQ(log.count(LogLevel::INFO), row.infos);
    }
}

std::string hex(const uint8_t* data, size_t len, const char* digits) {
    std::string out;
    for (size_t i = 0; i < len; ++i) {
        out += digits[data[i] >> 4];
        out += digits[data[i] & 0x0f];
    }
    return out;
}

/// Every text form a key could take in a line
std::vector<std::string> encodings(const uint8_t* data, size_t len) {
    return {
        hex(data, len, "0123456789abcdef"),
        hex(data, len, "0123456789ABCDEF"),
        sendspin::server::b64url_encode(data, len),
        sendspin::server::base64_encode(data, len),
        std::string(reinterpret_cast<const char*>(data), len),
    };
}

// No line holds a private or public key, the PSK, a client_id or the server_id, through a
// client that connects and one the server dials, each activated for playback, time-synced and
// gone with a goodbye
TEST(ServerLog, LogsNoKeyMaterial) {
    LogRecorder log;
    ServerConfig config = sendspin::server::test::local_config();
    config.log = log.sink();
    config.log_level = LogLevel::INFO;
    config.approve_all_clients = true;
    LocalServer s;
    ASSERT_EQ(s.start(config), StartResult::OK);

    const std::string hello = sendspin::server::test::player_hello_json(
        "Player", R"([{"codec":"pcm","sample_rate":48000,"channels":2,"bit_depth":16}])", 200000);
    RawClient accepted;
    RawClient dialed;
    ASSERT_EQ(accepted.dial(s.server.listen_port()), 101U);
    ASSERT_NO_FATAL_FAILURE(accepted.complete_handshake());
    ASSERT_TRUE(s.server.connect_url(dialed.url()));
    ASSERT_NO_FATAL_FAILURE(dialed.handshake());
    for (RawClient* client : {&accepted, &dialed}) {
        client->send_json(hello);
        EXPECT_NE(client->receive_until("server/activate"), "");
        client->send_json(sendspin::server::test::client_state_json(
            true, sendspin::server::test::player_state_json(0, 0, 0)));
        client->send_json(sendspin::server::test::client_time_json(1));
        EXPECT_NE(client->receive_until("server/time"), "");
        client->send_json(client_goodbye_json("shutdown"));
        s.recorder.wait_for(disconnected(client->noise.identity().peer_id()));
    }

    const std::vector<uint8_t>& server_key =
        s.provider.blobs.at(sendspin::server::persistence_keys::SERVER_KEY);
    std::array<uint8_t, sendspin::server::X25519_KEY_SIZE> server_private{};
    std::copy(server_key.begin(), server_key.end(), server_private.begin());
    const std::optional<Identity> server_identity = Identity::from_private_bytes(server_private);
    ASSERT_TRUE(server_identity);
    std::vector<std::string> secrets{s.server.server_id()};
    const auto add = [&secrets](const uint8_t* data, size_t len) {
        const std::vector<std::string> forms = encodings(data, len);
        secrets.insert(secrets.end(), forms.begin(), forms.end());
    };
    add(server_identity->private_bytes.data(), server_identity->private_bytes.size());
    add(server_identity->public_bytes.data(), server_identity->public_bytes.size());
    add(sendspin::server::SENTINEL_PSK.data(), sendspin::server::SENTINEL_PSK.size());
    for (const RawClient* client : {&accepted, &dialed}) {
        const Identity& identity = client->noise.identity();
        secrets.push_back(identity.peer_id());
        add(identity.private_bytes.data(), identity.private_bytes.size());
        add(identity.public_bytes.data(), identity.public_bytes.size());
    }

    const std::vector<LogRecorder::Line> lines = log.lines();
    // Each connection and its end, and the dial
    EXPECT_GE(lines.size(), 5U);
    for (const LogRecorder::Line& line : lines) {
        for (const std::string& secret : secrets) {
            EXPECT_EQ(line.text.find(secret), std::string::npos) << line.text;
        }
    }
}

// Logger::log() calls the sink at once on the dispatch thread, so a line logged there keeps its
// place before the tasks queued behind the one logging it; from any other thread it is posted
// behind what is queued. Each line's level says where it was logged.
TEST(ServerLog, KeepsALineLoggedOnTheDispatchThreadInItsPlace) {
    LogRecorder recorder;
    {
        sendspin::server::Dispatcher dispatcher;
        const sendspin::server::Logger log(dispatcher, recorder.sink(), LogLevel::INFO);
        std::promise<void> release;
        const std::shared_future<void> released = release.get_future().share();
        dispatcher.post([&log, released] {
            released.wait();
            SS_LOGE(log, "on the dispatch thread");
        });
        dispatcher.post([&log] { SS_LOGI(log, "on the dispatch thread, queued behind it"); });
        // The test thread's line goes behind both tasks
        SS_LOGW(log, "on the test thread");
        release.set_value();
        // The tasks use log, which goes before the dispatcher
        dispatcher.finish();
        dispatcher.join();
    }
    std::vector<LogLevel> order;
    for (const LogRecorder::Line& line : recorder.lines()) {
        order.push_back(line.level);
    }
    EXPECT_EQ(order, (std::vector<LogLevel>{LogLevel::ERROR, LogLevel::INFO, LogLevel::WARN}));
}

// A URL is logged without what may hold a secret: its userinfo, query and fragment
TEST(ServerLog, LogsAUrlWithoutUserinfoOrQuery) {
    struct Row {
        const char* name;
        const char* url;
        const char* logged;
    };
    const Row rows[] = {
        {"Control: a plain URL", "ws://host:8927/sendspin", "ws://host:8927/sendspin"},
        {"userinfo", "ws://user:secret@host:8927/sendspin", "ws://host:8927/sendspin"},
        {"a query", "ws://host/sendspin?token=secret", "ws://host/sendspin"},
        {"a query without a path", "ws://host?token=secret", "ws://host"},
        {"a fragment", "ws://host/sendspin#secret", "ws://host/sendspin"},
        {"a control character", "ws://ho\nst/", "ws://ho?st/"},
    };
    for (const Row& row : rows) {
        SCOPED_TRACE(row.name);
        EXPECT_EQ(sendspin::server::url_for_log(row.url), row.logged);
    }
}

}  // namespace
