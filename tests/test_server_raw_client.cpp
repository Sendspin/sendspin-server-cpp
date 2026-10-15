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

/// @file test_server_raw_client.cpp
/// @brief SendspinServer end to end against the raw test client: the handshake in both suites,
/// activation and time sync, and what the host is told when a client breaks the rules, says
/// goodbye or goes away at each step, including the handshake backoff.

#include "fragmentation.h"
#include "messages.h"
#include "raw_client.h"
#include "session.h"
#include "test_recorder.h"
#include "transport.h"
#include <ArduinoJson.h>
#include <boost/asio/error.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/system/system_error.hpp>
#include <gtest/gtest.h>

#include <sendspin/server/server.h>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace {

using sendspin::server::CipherSuite;
using sendspin::server::DisconnectReason;
using sendspin::server::ErrorKind;
using sendspin::server::GoodbyeReason;
using sendspin::server::SendspinServer;
using sendspin::server::ServerConfig;
using sendspin::server::ServerError;
using sendspin::server::StartResult;
using sendspin::server::test::Bytes;
using sendspin::server::test::bytes_of;
using sendspin::server::test::client_goodbye_json;
using sendspin::server::test::client_hello_json;
using sendspin::server::test::client_time_json;
using sendspin::server::test::MemoryProvider;
using sendspin::server::test::NoiseResponder;
using sendspin::server::test::RawClient;
using sendspin::server::test::ReceivedMessage;
using sendspin::server::test::Recorder;
using sendspin::server::test::text_of;

ServerConfig test_config() {
    ServerConfig config = sendspin::server::test::local_config();
    config.name = "Test server";
    return config;
}

/// Sends a message over the WebSocket limit. The server closes on its header, so the rest of the
/// write may meet the closed socket
void send_over_the_limit(RawClient& client) {
    try {
        client.send_binary(Bytes(sendspin::server::MAX_MESSAGE_BYTES + 1, 0));
    } catch (const boost::system::system_error& error) {
        EXPECT_TRUE(error.code() == boost::asio::error::broken_pipe ||
                    error.code() == boost::asio::error::connection_reset)
            << error.what();
    }
}

std::string connected(const std::string& client_id) {
    return "connected " + client_id;
}

std::string disconnected(const std::string& client_id, DisconnectReason reason) {
    return "disconnected " + client_id + " " + std::to_string(static_cast<int>(reason));
}

/// A started server with a recording listener
class RunningServer {
public:
    explicit RunningServer(const ServerConfig& config = test_config()) {
        EXPECT_EQ(this->server.start(config, &this->provider, &this->recorder), StartResult::OK);
    }

    MemoryProvider provider;
    Recorder recorder;
    SendspinServer server;
};

CipherSuite other_suite(CipherSuite suite) {
    return suite == CipherSuite::AESGCM_SHA256 ? CipherSuite::CHACHAPOLY_SHA256
                                               : CipherSuite::AESGCM_SHA256;
}

// ==============================================================================
// The raw client itself
// ==============================================================================

// reset() sends a TCP reset even with nothing unread, where drop() then ends with a FIN
TEST(RawClientSelf, ResetSendsATcpReset) {
    namespace asio = boost::asio;
    namespace websocket = boost::beast::websocket;
    for (const bool reset : {false, true}) {
        SCOPED_TRACE(reset ? "reset" : "Control: drop");
        RawClient client;
        std::thread dialer;
        boost::system::error_code read_error;
        asio::io_context io;
        websocket::stream<asio::ip::tcp::socket> ws(io);
        const sendspin::server::DialTarget target = *sendspin::server::parse_ws_url(client.url());
        dialer = std::thread([&] {
            ws.next_layer().connect({asio::ip::make_address("127.0.0.1"), target.port});
            ws.handshake("127.0.0.1", target.path);
        });
        client.accept();
        dialer.join();
        if (reset) {
            client.reset();
        } else {
            client.drop();
        }
        boost::beast::flat_buffer buffer;
        ws.read(buffer, read_error);
        EXPECT_EQ(read_error == asio::error::connection_reset, reset) << read_error.message();
    }
}

// ==============================================================================
// A well-behaved client
// ==============================================================================

class ServerRawClientSuites : public testing::TestWithParam<CipherSuite> {};

// connection.md "Cipher Suites" and messaging.md "Communication": the handshake runs in the suite
// the client declares, then the server activates the client and answers its time requests
TEST_P(ServerRawClientSuites, CompletesTheHandshakeActivationAndTimeSync) {
    RunningServer s;
    RawClient client(GetParam());
    ASSERT_TRUE(s.server.connect_url(client.url()));
    client.accept();
    client.send_client_init();
    ASSERT_NO_FATAL_FAILURE(client.receive_message_1());

    // Control for the suite: message 1 does not authenticate under the other one, with the same
    // keys and prologue
    NoiseResponder other(other_suite(GetParam()), *sendspin::server::Identity::from_private_bytes(
                                                      client.noise.identity().private_bytes));
    EXPECT_FALSE(other.read_message_1(bytes_of(client.client_init + client.server_init),
                                      client.server_init, client.noise_message_1));

    client.send_message_2();
    JsonDocument hello;
    ASSERT_EQ(deserializeJson(hello, client.receive_json()), DeserializationError::Ok);
    EXPECT_EQ(hello["type"], "server/hello");
    EXPECT_EQ(hello["payload"]["name"], "Test server");

    client.send_json(client_hello_json("Raw client"));
    EXPECT_EQ(client.receive_json(),
              R"({"type":"server/activate","payload":{"activities":[],"active_roles":[]}})");
    // messaging.md "group/update": every field, promptly after the first activation. A client
    // not approved for playback is alone in a stopped group named after it.
    JsonDocument update;
    ASSERT_EQ(deserializeJson(update, client.receive_json()), DeserializationError::Ok);
    EXPECT_EQ(update["type"], "group/update");
    EXPECT_EQ(update["payload"]["playback_state"], "stopped");
    EXPECT_EQ(update["payload"]["group_name"], "Raw client");
    const std::vector<sendspin::server::GroupInfo> groups = s.server.groups();
    ASSERT_EQ(groups.size(), 2U);
    EXPECT_EQ(update["payload"]["group_id"], std::to_string(groups[1].id.value));
    const std::string id = client.noise.identity().peer_id();
    s.recorder.wait_for(connected(id));
    const std::vector<sendspin::server::ClientInfo> clients = s.server.clients();
    ASSERT_EQ(clients.size(), 1U);
    EXPECT_EQ(clients[0].client_id, id);
    EXPECT_EQ(clients[0].name, "Raw client");
    EXPECT_EQ(clients[0].url, client.url());

    // messaging.md "server/time": the reply's stamps fall between two readings of the server's
    // clock taken around the exchange, in order
    const int64_t before = s.server.now_us();
    client.send_json(client_time_json(1234));
    JsonDocument time;
    ASSERT_EQ(deserializeJson(time, client.receive_json()), DeserializationError::Ok);
    const int64_t after = s.server.now_us();
    EXPECT_EQ(time["type"], "server/time");
    EXPECT_EQ(time["payload"]["client_transmitted"].as<int64_t>(), 1234);
    const int64_t received = time["payload"]["server_received"].as<int64_t>();
    const int64_t transmitted = time["payload"]["server_transmitted"].as<int64_t>();
    EXPECT_LE(before, received);
    EXPECT_LE(received, transmitted);
    EXPECT_LE(transmitted, after);

    client.send_json(client_goodbye_json("shutdown"));
    client.expect_silent_close();
    s.recorder.wait_for(disconnected(id, DisconnectReason::GOODBYE));
    // messaging.md "client/goodbye": `shutdown` is not redialed on the server's own
    ASSERT_EQ(s.server.clients().size(), 1U);
    EXPECT_FALSE(s.server.clients()[0].connected);
    EXPECT_FALSE(s.server.clients()[0].reconnecting);
}

INSTANTIATE_TEST_SUITE_P(Suites, ServerRawClientSuites,
                         testing::Values(CipherSuite::CHACHAPOLY_SHA256,
                                         CipherSuite::AESGCM_SHA256),
                         [](const testing::TestParamInfo<CipherSuite>& info) {
                             return info.param == CipherSuite::AESGCM_SHA256
                                        ? std::string("AESGCM")
                                        : std::string("ChaChaPoly");
                         });

// ==============================================================================
// A client that breaks the rules before activation
// ==============================================================================

// connection.md "Failure Handling": an invalid client/init is answered with server/error, whose
// reason follows the spec's order, and the server closes; the host hears of a failed handshake
TEST(ServerRawClient, AnswersAnInitRejectionWithServerErrorThenCloses) {
    struct Row {
        const char* name;
        bool binary;
        std::function<std::string(const RawClient&)> init;
        const char* reason;  // nullptr for none
    };
    const auto edit = [](std::string from, std::string to) {
        return [from, to](const RawClient& client) {
            std::string init = client.noise.client_init();
            init.replace(init.find(from), from.size(), to);
            return init;
        };
    };
    const std::string suite(sendspin::server::to_wire(CipherSuite::CHACHAPOLY_SHA256));
    const Row rows[] = {
        {"Control: a valid client/init", false,
         [](const RawClient& client) { return client.noise.client_init(); }, nullptr},
        {"version 2", false, edit(R"("version":1)", R"("version":2)"), "unsupported_version"},
        {"version 2 with an unknown suite", false,
         [&](const RawClient& client) {
             std::string init = edit(R"("version":1)", R"("version":2)")(client);
             return init.replace(init.find(suite), suite.size(), "25519_Other_SHA256");
         },
         "unsupported_version"},
        {"an unknown suite", false, edit(suite, "25519_Other_SHA256"), "unsupported_suite"},
        {"no client_id", false, edit(R"("client_id")", R"("client_ix")"), "malformed"},
        {"not JSON", false, [](const RawClient&) { return std::string("hello"); }, "malformed"},
        {"a binary message", true,
         [](const RawClient& client) { return client.noise.client_init(); }, "malformed"},
        {"client/goodbye", false, [](const RawClient&) { return client_goodbye_json("shutdown"); },
         "malformed"},
    };
    RunningServer s;
    for (const Row& row : rows) {
        SCOPED_TRACE(row.name);
        RawClient client;
        ASSERT_TRUE(s.server.connect_url(client.url()));
        client.accept();
        const std::string init = row.init(client);
        if (row.binary) {
            client.send_binary(bytes_of(init));
        } else {
            client.send_text(init);
        }
        const std::optional<ReceivedMessage> answer = client.receive();
        ASSERT_TRUE(answer);
        EXPECT_TRUE(answer->text);
        JsonDocument doc;
        ASSERT_EQ(deserializeJson(doc, text_of(answer->bytes)), DeserializationError::Ok);
        if (row.reason == nullptr) {
            EXPECT_EQ(doc["type"], "server/init");
            client.drop();
            continue;
        }
        EXPECT_EQ(doc["type"], "server/error");
        EXPECT_EQ(doc["payload"]["reason"], row.reason);
        client.expect_silent_close();
        EXPECT_TRUE(client.closed_by_server());
        const ServerError error = s.recorder.wait_for_error(client.url());
        EXPECT_EQ(error.kind, ErrorKind::HANDSHAKE_FAILED);
        EXPECT_EQ(error.client_id, "");
    }
}

// connection.md "Failure Handling": a bad Noise message 2, cleartext after transport mode and an
// AEAD failure are silent failures, and the rules before activation (messaging.md
// "Communication", "Fragmentation") end the connection without a message too
TEST(ServerRawClient, FailsSilentlyOnABadMessageBeforeActivation) {
    using sendspin::server::FRAGMENT_FLAG_LAST;
    struct Row {
        const char* name;
        /// Sent once Noise message 1 is read; false for once server/hello is
        bool replaces_message_2;
        std::function<void(RawClient&)> act;
    };
    const Row rows[] = {
        {"Control: client/hello", false, [](RawClient& c) { c.send_json(client_hello_json()); }},
        {"message 2 tampered", true,
         [](RawClient& c) {
             Bytes bytes = *sendspin::server::parse_noise_handshake(c.noise.message_2());
             bytes[bytes.size() / 2] ^= 0x01;
             c.send_text(sendspin::server::build_noise_handshake(bytes));
         }},
        {"message 2 under another PSK", true,
         [](RawClient& c) {
             // The PSK enters the responder's state when it reads message 1
             c.noise.psk.fill(0x5A);
             ASSERT_TRUE(c.noise.read_message_1(bytes_of(c.client_init + c.server_init),
                                                c.server_init, c.noise_message_1));
             c.send_message_2();
         }},
        {"message 2 with a payload other than {}", true,
         [](RawClient& c) { c.send_message_2("{ }"); }},
        {"garbage handshake data", true,
         [](RawClient& c) {
             c.send_text(sendspin::server::build_noise_handshake(Bytes(48, 0x41)));
         }},
        {"message 2 as a binary message", true,
         [](RawClient& c) { c.send_binary(bytes_of(c.noise.message_2())); }},
        {"a cleartext client/goodbye", true,
         [](RawClient& c) { c.send_text(client_goodbye_json("shutdown")); }},
        {"cleartext in transport mode", false,
         [](RawClient& c) { c.send_text(client_hello_json()); }},
        {"tampered ciphertext", false,
         [](RawClient& c) {
             Bytes message = c.noise.encrypt_json(client_hello_json());
             message[3] ^= 0x01;
             c.send_binary(message);
         }},
        {"a continuation with no fragmented message in flight", false,
         [](RawClient& c) { c.send_fragment(FRAGMENT_FLAG_LAST, {'{'}); }},
        {"client/time before client/hello", false,
         [](RawClient& c) { c.send_json(client_time_json(1)); }},
        {"client/state before client/hello", false,
         [](RawClient& c) { c.send_json(R"({"type":"client/state","payload":{}})"); }},
        {"audio before activation", false,
         [](RawClient& c) { c.send_binary(c.noise.encrypt({4, 0})); }},
        {"a message over the WebSocket limit", false, send_over_the_limit},
    };
    RunningServer s;
    for (const Row& row : rows) {
        SCOPED_TRACE(row.name);
        RawClient client;
        ASSERT_TRUE(s.server.connect_url(client.url()));
        if (row.replaces_message_2) {
            client.accept();
            client.send_client_init();
            ASSERT_NO_FATAL_FAILURE(client.receive_message_1());
        } else {
            ASSERT_NO_FATAL_FAILURE(client.handshake());
        }
        row.act(client);
        const std::string id = client.noise.identity().peer_id();
        if (std::string(row.name).starts_with("Control:")) {
            EXPECT_NE(client.receive_json().find("server/activate"), std::string::npos);
            s.recorder.wait_for(connected(id));
            client.drop();
            continue;
        }
        client.expect_silent_close();
        const ServerError error = s.recorder.wait_for_error(client.url());
        EXPECT_EQ(error.kind, ErrorKind::HANDSHAKE_FAILED);
        EXPECT_EQ(error.client_id, id);
        EXPECT_FALSE(s.recorder.contains(connected(id)));
    }
}

// connection.md "Failure Handling": a client that stops answering during the handshake is closed
// silently once the step timeout passes
TEST(ServerRawClient, TimesOutAClientThatStopsAnswering) {
    constexpr int64_t STEP_TIMEOUT_US = 1000000;
    struct Row {
        const char* name;
        std::function<void(RawClient&)> reach;
        bool init_accepted;
    };
    const Row rows[] = {
        {"never sends client/init", [](RawClient& c) { c.accept(); }, false},
        {"never sends message 2",
         [](RawClient& c) {
             c.accept();
             c.send_client_init();
             c.receive_message_1();
         },
         true},
        {"never sends client/hello", [](RawClient& c) { c.handshake(); }, true},
    };
    ServerConfig config = test_config();
    config.timing.handshake_step_timeout_us = STEP_TIMEOUT_US;
    RunningServer s(config);
    for (const Row& row : rows) {
        SCOPED_TRACE(row.name);
        RawClient client;
        ASSERT_TRUE(s.server.connect_url(client.url()));
        // The step's limit starts no earlier than this
        const int64_t start = s.server.now_us();
        ASSERT_NO_FATAL_FAILURE(row.reach(client));
        client.expect_silent_close();
        EXPECT_GE(s.server.now_us() - start, STEP_TIMEOUT_US);
        const ServerError error = s.recorder.wait_for_error(client.url());
        EXPECT_EQ(error.kind, ErrorKind::HANDSHAKE_FAILED);
        EXPECT_EQ(error.client_id, row.init_accepted ? client.noise.identity().peer_id() : "");
    }
}

// ==============================================================================
// Goodbyes and closes
// ==============================================================================

// A client that goes away at any step: before activation the host hears of a failed handshake,
// after it of a lost connection, whether the client closes the WebSocket or drops the TCP
// connection
TEST(ServerRawClient, ReportsAClientThatClosesAtEachStep) {
    struct Step {
        const char* name;
        std::function<void(RawClient&)> reach;
        bool connected;
    };
    const Step steps[] = {
        {"before client/init", [](RawClient& c) { c.accept(); }, false},
        {"after client/init",
         [](RawClient& c) {
             c.accept();
             c.send_client_init();
             c.receive_message_1();
         },
         false},
        {"after message 2", [](RawClient& c) { c.handshake(); }, false},
        {"after the activation", [](RawClient& c) { c.activate(); }, true},
    };
    RunningServer s;
    for (const Step& step : steps) {
        for (const bool drop : {false, true}) {
            SCOPED_TRACE(std::string(step.name) + (drop ? ", dropped" : ", closed"));
            RawClient client;
            ASSERT_TRUE(s.server.connect_url(client.url()));
            ASSERT_NO_FATAL_FAILURE(step.reach(client));
            if (drop) {
                client.drop();
            } else {
                client.close();
            }
            const std::string id = client.noise.identity().peer_id();
            if (step.connected) {
                s.recorder.wait_for(disconnected(id, DisconnectReason::CONNECTION_LOST));
                continue;
            }
            EXPECT_EQ(s.recorder.wait_for_error(client.url()).kind, ErrorKind::HANDSHAKE_FAILED);
        }
    }
}

// messaging.md "client/goodbye": an encrypted goodbye is accepted once the Noise handshake is
// complete; before activation the host hears of it with its reason, after it as a disconnect
TEST(ServerRawClient, ReportsAGoodbyeAtEachPhase) {
    struct Row {
        const char* name;
        bool after_hello;
        const char* reason;
        std::optional<GoodbyeReason> parsed;
    };
    const Row rows[] = {
        {"before client/hello", false, "restart", GoodbyeReason::RESTART},
        {"before client/hello, an unrecognized reason", false, "moving", std::nullopt},
        // The server reports the client connected on its client/hello, so the goodbye follows a
        // connection whether or not the client has read its activation
        {"after client/hello", true, "another_server", GoodbyeReason::ANOTHER_SERVER},
    };
    RunningServer s;
    for (const Row& row : rows) {
        SCOPED_TRACE(row.name);
        RawClient client;
        ASSERT_TRUE(s.server.connect_url(client.url()));
        ASSERT_NO_FATAL_FAILURE(client.handshake());
        if (row.after_hello) {
            client.send_json(client_hello_json());
        }
        client.send_json(client_goodbye_json(row.reason));
        const std::string id = client.noise.identity().peer_id();
        if (row.after_hello) {
            s.recorder.wait_for(disconnected(id, DisconnectReason::GOODBYE));
            EXPECT_TRUE(s.recorder.contains(connected(id)));
            continue;
        }
        client.expect_silent_close();
        const ServerError error = s.recorder.wait_for_error(client.url());
        EXPECT_EQ(error.kind, ErrorKind::GOODBYE_BEFORE_ACTIVATION);
        EXPECT_EQ(error.goodbye_reason, row.parsed);
        EXPECT_EQ(error.client_id, id);
    }
}

// messaging.md "client/goodbye": a client that closes right after its goodbye, however it closes,
// is reported as leaving with the goodbye. test_session_driver.cpp has the goodbye arrive while a
// write is in flight.
TEST(ServerRawClient, ReportsAGoodbyeFollowedByAnImmediateClose) {
    enum class Close : uint8_t { RESET, HALF_CLOSE, CLOSE_FRAME };
    struct Row {
        const char* name;
        Close close;
    };
    const Row rows[] = {
        {"a reset", Close::RESET},
        {"a half-close", Close::HALF_CLOSE},
        {"a close frame", Close::CLOSE_FRAME},
    };
    RunningServer s;
    for (const Row& row : rows) {
        SCOPED_TRACE(row.name);
        RawClient client;
        ASSERT_TRUE(s.server.connect_url(client.url()));
        ASSERT_NO_FATAL_FAILURE(client.handshake());
        client.send_json(client_goodbye_json("restart"));
        switch (row.close) {
            case Close::RESET:
                client.reset();
                break;
            case Close::HALF_CLOSE:
                client.half_close();
                break;
            case Close::CLOSE_FRAME:
                client.close();
                break;
        }
        const ServerError error = s.recorder.wait_for_error(client.url());
        EXPECT_EQ(error.kind, ErrorKind::GOODBYE_BEFORE_ACTIVATION);
        EXPECT_EQ(error.goodbye_reason, GoodbyeReason::RESTART);
    }
}

// ==============================================================================
// A client that breaks the rules once active
// ==============================================================================

// messaging.md "Communication": once active, an unrecognized message is ignored while a broken
// rule still closes the connection, which the host hears of as closed by the server
TEST(ServerRawClient, ClosesAnActiveClientThatBreaksTheRules) {
    using sendspin::server::FRAGMENT_FLAG_FIRST;
    using sendspin::server::FRAGMENT_FLAG_LAST;
    struct Row {
        const char* name;
        std::function<void(RawClient&)> act;
        bool closes;
    };
    const Row rows[] = {
        {"Control: an unrecognized type",
         [](RawClient& c) { c.send_json(R"({"type":"client/telepathy","payload":{}})"); }, false},
        {"cleartext", [](RawClient& c) { c.send_text(client_time_json(1)); }, true},
        {"tampered ciphertext",
         [](RawClient& c) {
             Bytes message = c.noise.encrypt_json(client_time_json(1));
             message[3] ^= 0x01;
             c.send_binary(message);
         },
         true},
        {"a continuation with no fragmented message in flight",
         [](RawClient& c) { c.send_fragment(FRAGMENT_FLAG_LAST, {'{'}); }, true},
        {"a fragmented message over the reassembly cap",
         [](RawClient& c) {
             constexpr size_t FRAGMENT_DATA = 65000;
             Bytes data(FRAGMENT_DATA, ' ');
             data.front() = sendspin::server::MESSAGE_ID_JSON;
             c.send_fragment(FRAGMENT_FLAG_FIRST, data);
             for (size_t sent = FRAGMENT_DATA;
                  sent <= sendspin::server::DEFAULT_MAX_REASSEMBLED_BYTES; sent += FRAGMENT_DATA) {
                 c.send_fragment(0, Bytes(FRAGMENT_DATA, ' '));
             }
         },
         true},
        {"a second client/hello", [](RawClient& c) { c.send_json(client_hello_json()); }, true},
        {"a message over the WebSocket limit", send_over_the_limit, true},
    };
    RunningServer s;
    for (const Row& row : rows) {
        SCOPED_TRACE(row.name);
        RawClient client;
        ASSERT_TRUE(s.server.connect_url(client.url()));
        ASSERT_NO_FATAL_FAILURE(client.activate());
        row.act(client);
        const std::string id = client.noise.identity().peer_id();
        if (!row.closes) {
            // Nothing answered it, and the next time request is answered
            client.send_json(client_time_json(9));
            EXPECT_NE(client.receive_json().find(R"("client_transmitted":9)"), std::string::npos);
            client.send_json(client_goodbye_json("shutdown"));
            s.recorder.wait_for(disconnected(id, DisconnectReason::GOODBYE));
            continue;
        }
        client.expect_silent_close();
        s.recorder.wait_for(disconnected(id, DisconnectReason::CLOSED_BY_SERVER));
    }
}

// ==============================================================================
// Backoff
// ==============================================================================

// messaging.md "client/goodbye": after a failed handshake, a dial to the same URL waits out the
// backoff however often it is asked for, while a goodbye before activation does not count. Seen
// through dial order: two URLs that reach one client, the one that failed asked for first.
TEST(ServerRawClient, DefersTheNextDialAfterAFailedHandshake) {
    struct Row {
        const char* name;
        std::function<void(RawClient&)> fail;
        ErrorKind kind;
        bool deferred;
    };
    const Row rows[] = {
        {"a failed handshake",
         [](RawClient& c) {
             c.send_text(R"({"type":"client/init","payload":{"version":2}})");
             c.receive();
         },
         ErrorKind::HANDSHAKE_FAILED, true},
        {"Control: a goodbye before activation",
         [](RawClient& c) {
             c.complete_handshake();
             c.send_json(client_goodbye_json("restart"));
         },
         ErrorKind::GOODBYE_BEFORE_ACTIVATION, false},
    };
    // Far longer than the test needs to ask for both dials after the failure is reported
    constexpr int64_t BACKOFF_MIN_US = 1000000;
    ServerConfig config = test_config();
    config.timing.handshake_backoff_min_us = BACKOFF_MIN_US;
    config.timing.handshake_backoff_max_us = 2 * BACKOFF_MIN_US;
    RunningServer s(config);
    for (const Row& row : rows) {
        SCOPED_TRACE(row.name);
        RawClient client;
        const std::string failing = client.url("/failing");
        const std::string fresh = client.url("/fresh");
        ASSERT_TRUE(s.server.connect_url(failing));
        EXPECT_EQ(client.accept(), "/failing");
        // The failure is recorded no earlier than this
        const int64_t failed_after = s.server.now_us();
        ASSERT_NO_FATAL_FAILURE(row.fail(client));
        EXPECT_EQ(s.recorder.wait_for_error(failing).kind, row.kind);

        ASSERT_TRUE(s.server.connect_url(failing));
        ASSERT_TRUE(s.server.connect_url(failing));
        ASSERT_TRUE(s.server.connect_url(fresh));
        EXPECT_EQ(client.accept(), row.deferred ? "/fresh" : "/failing");
        const std::string second = client.accept();
        if (!row.deferred) {
            EXPECT_EQ(second, "/fresh");
            continue;
        }
        EXPECT_EQ(second, "/failing");
        EXPECT_GE(s.server.now_us() - failed_after, BACKOFF_MIN_US);
        // The deferred dial is a full connection, and the repeated request made no other
        ASSERT_NO_FATAL_FAILURE(client.complete_handshake());
        client.send_json(client_hello_json());
        EXPECT_NE(client.receive_json().find("server/activate"), std::string::npos);
        s.recorder.wait_for(connected(client.noise.identity().peer_id()));
        EXPECT_FALSE(client.dial_pending());
    }
}

}  // namespace
