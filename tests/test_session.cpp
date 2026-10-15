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

/// @file test_session.cpp
/// @brief Session from client/init through activation and time sync, driven in-process by the
/// raw test client's NoiseResponder. No sockets: the test hands each message across.

#include "backoff.h"
#include "clock.h"
#include "crypto/keys.h"
#include "fragmentation.h"
#include "freed_blocks.h"
#include "messages.h"
#include "raw_client.h"
#include "session.h"
#include "trust_fixtures.h"
#include "wire_messages.h"
#include <ArduinoJson.h>
#include <gtest/gtest.h>
#include <noise/protocol.h>

#include <functional>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

using sendspin::server::CipherSuite;
using sendspin::server::Identity;
using sendspin::server::MessageKind;
using sendspin::server::OutboundMessage;
using sendspin::server::Reassembler;
using sendspin::server::ServerActivate;
using sendspin::server::Session;
using sendspin::server::SessionEnd;
using sendspin::server::SessionPhase;
using sendspin::server::SessionSettings;
using sendspin::server::SessionTrace;
using sendspin::server::test::Bytes;
using sendspin::server::test::bytes_of;
using sendspin::server::test::client_hello_json;
using sendspin::server::test::client_time_json;
using sendspin::server::test::NoiseResponder;
using sendspin::server::test::psk_of;
using sendspin::server::test::text_of;
namespace wire = sendspin::server::test::wire;

constexpr int64_t START_US = 1000;
constexpr int64_t TIMEOUT_US = 5000;

SessionSettings test_settings() {
    SessionSettings settings;
    settings.hello.name = "Test server";
    settings.step_timeout_us = TIMEOUT_US;
    return settings;
}

/// A CredentialSource that references one PSK
class FixedCredentials : public sendspin::server::CredentialSource {
public:
    explicit FixedCredentials(sendspin::server::HandshakePsk psk) : psk_(std::move(psk)) {}

    sendspin::server::HandshakePsk handshake_psk(
        const sendspin::server::ClientInit& /*init*/) override {
        return this->psk_;
    }

private:
    sendspin::server::HandshakePsk psk_;
};

// A session and its client. Not movable: the session refers to the identity and settings.
class Harness {
public:
    explicit Harness(CipherSuite suite, SessionSettings settings = test_settings(),
                     SessionTrace* trace = nullptr,
                     sendspin::server::CredentialSource* credentials = nullptr)
        : settings_(std::move(settings)),
          session(this->identity, this->settings_, trace, credentials),
          client(suite) {
        this->session.start(START_US);
    }

    Harness(const Harness&) = delete;
    Harness& operator=(const Harness&) = delete;

    std::vector<OutboundMessage> take_all() {
        std::vector<OutboundMessage> out;
        while (std::optional<OutboundMessage> message = this->session.take_message()) {
            out.push_back(std::move(*message));
        }
        return out;
    }

    // Sends client/init and has the client read Noise message 1 under the spec's prologue
    void exchange_init(int64_t rx_us = START_US) {
        this->client_init = this->client.client_init();
        this->session.on_message(MessageKind::TEXT, bytes_of(this->client_init), rx_us);
        ASSERT_EQ(this->session.phase(), SessionPhase::AWAIT_NOISE_MSG2);
        const std::vector<OutboundMessage> sent = this->take_all();
        ASSERT_EQ(sent.size(), 2U);
        ASSERT_EQ(sent[0].kind, MessageKind::TEXT);
        ASSERT_EQ(sent[1].kind, MessageKind::TEXT);
        this->server_init = text_of(sent[0].bytes);
        this->noise_message_1 = text_of(sent[1].bytes);
        ASSERT_TRUE(this->client.read_message_1(bytes_of(this->client_init + this->server_init),
                                                this->server_init, this->noise_message_1));
    }

    // Through Noise message 2 and the server/hello it releases
    void complete_handshake(int64_t rx_us = START_US) {
        ASSERT_NO_FATAL_FAILURE(this->exchange_init(rx_us));
        this->session.on_message(MessageKind::TEXT, bytes_of(this->client.message_2()), rx_us);
        ASSERT_EQ(this->session.phase(), SessionPhase::AWAIT_CLIENT_HELLO);
        const std::vector<OutboundMessage> sent = this->take_all();
        ASSERT_EQ(sent.size(), 1U);
        ASSERT_EQ(sent[0].kind, MessageKind::BINARY);
        this->server_hello = this->client.decrypt(sent[0].bytes);
        ASSERT_TRUE(this->server_hello);
    }

    // Through client/hello and the initial server/activate, which the client decrypts
    void activate() {
        ASSERT_NO_FATAL_FAILURE(this->complete_handshake());
        this->send(this->client.encrypt_json(client_hello_json()));
        ASSERT_EQ(this->session.phase(), SessionPhase::READY_FOR_ACTIVATION);
        this->session.activate(ServerActivate{false, std::vector<std::string>{}, std::nullopt});
        ASSERT_EQ(this->session.phase(), SessionPhase::ACTIVATING);
        const std::vector<OutboundMessage> sent = this->take_all();
        ASSERT_EQ(sent.size(), 1U);
        ASSERT_EQ(this->session.phase(), SessionPhase::ACTIVE);
        EXPECT_EQ(this->decrypt_json(sent[0]),
                  R"({"type":"server/activate","payload":{"activities":[],"active_roles":[]}})");
    }

    // The JSON a binary server message carries; empty when it is not one
    std::string decrypt_json(const OutboundMessage& message) {
        EXPECT_EQ(message.kind, MessageKind::BINARY);
        const std::optional<Bytes> plaintext = this->client.decrypt(message.bytes);
        if (!plaintext || plaintext->empty() || plaintext->front() != wire::JSON_ID) {
            ADD_FAILURE() << "not an encrypted JSON message";
            return {};
        }
        return {plaintext->begin() + 1, plaintext->end()};
    }

    void send(const Bytes& message, int64_t rx_us = START_US) {
        this->session.on_message(MessageKind::BINARY, message, rx_us);
    }

    // A fragment of a message, encrypted
    Bytes fragment(uint8_t flags, const Bytes& data) {
        Bytes plaintext{sendspin::server::MESSAGE_ID_FRAGMENT, flags};
        plaintext.insert(plaintext.end(), data.begin(), data.end());
        return this->client.encrypt(plaintext);
    }

    Identity identity{*Identity::generate()};

private:
    SessionSettings settings_;

public:
    Session session;
    NoiseResponder client;
    std::string client_init;
    std::string server_init;
    std::string noise_message_1;
    std::optional<Bytes> server_hello;
};

class SessionTest : public testing::TestWithParam<CipherSuite> {
protected:
    static void SetUpTestSuite() {
        ASSERT_EQ(noise_init_framework(), NOISE_ERROR_NONE);
    }
};

TEST_P(SessionTest, CompletesTheHandshakeAndTheHelloExchange) {
    Harness h(GetParam());
    ASSERT_NO_FATAL_FAILURE(h.exchange_init(2000));
    ASSERT_TRUE(h.session.client_init());
    EXPECT_EQ(h.session.client_init()->client_id, h.client.identity().peer_id());
    EXPECT_EQ(h.session.client_init()->rx_us, 2000);
    EXPECT_EQ(h.session.deadline_us(), 2000 + TIMEOUT_US);

    JsonDocument server_init;
    ASSERT_EQ(deserializeJson(server_init, h.server_init), DeserializationError::Ok);
    EXPECT_EQ(server_init["type"], "server/init");
    EXPECT_EQ(server_init["payload"]["server_id"], h.identity.peer_id());
    EXPECT_EQ(server_init["payload"]["version"], 1);

    // connection.md "Pre-Shared Key": the Sentinel, declared as such
    JsonDocument payload_1;
    ASSERT_EQ(deserializeJson(payload_1, h.client.message_1_payload), DeserializationError::Ok);
    EXPECT_EQ(payload_1["psk_id"], sendspin::server::SENTINEL_PSK_ID);
    EXPECT_EQ(payload_1["psk_category"], "sn");

    h.session.on_message(MessageKind::TEXT, bytes_of(h.client.message_2()), 3000);
    ASSERT_EQ(h.session.phase(), SessionPhase::AWAIT_CLIENT_HELLO);
    EXPECT_EQ(h.session.deadline_us(), 3000 + TIMEOUT_US);
    const std::vector<OutboundMessage> sent = h.take_all();
    ASSERT_EQ(sent.size(), 1U);
    ASSERT_EQ(sent[0].kind, MessageKind::BINARY);
    const std::optional<Bytes> hello = h.client.decrypt(sent[0].bytes);
    ASSERT_TRUE(hello);
    ASSERT_EQ(hello->front(), wire::JSON_ID);
    EXPECT_EQ(std::string(hello->begin() + 1, hello->end()),
              sendspin::server::build_server_hello({"Test server", {}, {}}));

    h.send(h.client.encrypt_json(client_hello_json()), 4000);
    ASSERT_EQ(h.session.phase(), SessionPhase::READY_FOR_ACTIVATION);
    ASSERT_TRUE(h.session.client_hello());
    EXPECT_EQ(h.session.client_hello()->name, "Kitchen");
    EXPECT_EQ(h.session.client_hello()->rx_us, 4000);
    EXPECT_FALSE(h.session.deadline_us());
    EXPECT_FALSE(h.session.has_message());
    EXPECT_FALSE(h.session.end());
}

// connection.md "Prologue": the exact bytes received, not a re-encoding
TEST_P(SessionTest, BindsTheRawClientInitBytesIntoThePrologue) {
    Harness h(GetParam());
    const std::string init = h.client.client_init("  ");
    h.session.on_message(MessageKind::TEXT, bytes_of(init), START_US);
    const std::vector<OutboundMessage> sent = h.take_all();
    ASSERT_EQ(sent.size(), 2U);
    const std::string server_init = text_of(sent[0].bytes);
    const std::string message_1 = text_of(sent[1].bytes);

    JsonDocument doc;
    ASSERT_EQ(deserializeJson(doc, init), DeserializationError::Ok);
    std::string reencoded;
    serializeJson(doc, reencoded);
    ASSERT_NE(reencoded, init);
    EXPECT_FALSE(
        h.client.read_message_1(bytes_of(reencoded + server_init), server_init, message_1));
    EXPECT_TRUE(h.client.read_message_1(bytes_of(init + server_init), server_init, message_1));
}

// connection.md "Failure Handling": server/error with the matching reason, then close
TEST_P(SessionTest, AnswersAnInitFailureWithServerErrorAlone) {
    NoiseResponder client(GetParam());
    struct Row {
        const char* name;
        MessageKind kind;
        std::string text;
        std::optional<sendspin::server::InitError> error;
    };
    const std::string_view suite = sendspin::server::to_wire(GetParam());
    std::string other_suite = client.client_init();
    other_suite.replace(other_suite.find(suite), suite.size(), "25519_Other_SHA256");
    std::string version_2 = client.client_init();
    version_2.replace(version_2.find("\"version\":1"), std::string("\"version\":1").size(),
                      "\"version\":2");
    const Row rows[] = {
        {"Control:", MessageKind::TEXT, client.client_init(), std::nullopt},
        {"binary", MessageKind::BINARY, client.client_init(),
         sendspin::server::InitError::MALFORMED},
        {"not json", MessageKind::TEXT, "hello", sendspin::server::InitError::MALFORMED},
        {"version 2", MessageKind::TEXT, version_2,
         sendspin::server::InitError::UNSUPPORTED_VERSION},
        {"unknown suite", MessageKind::TEXT, other_suite,
         sendspin::server::InitError::UNSUPPORTED_SUITE},
    };
    for (const Row& row : rows) {
        SCOPED_TRACE(row.name);
        Harness h(GetParam());
        h.session.on_message(row.kind, bytes_of(row.text), START_US);
        const std::vector<OutboundMessage> sent = h.take_all();
        if (!row.error) {
            EXPECT_EQ(h.session.phase(), SessionPhase::AWAIT_NOISE_MSG2);
            ASSERT_EQ(sent.size(), 2U);
            EXPECT_NE(text_of(sent[0].bytes).find("server/init"), std::string::npos);
            continue;
        }
        EXPECT_EQ(h.session.phase(), SessionPhase::CLOSED);
        EXPECT_EQ(h.session.end(), SessionEnd::INIT_REJECTED);
        EXPECT_EQ(h.session.init_error(), row.error);
        ASSERT_EQ(sent.size(), 1U);
        EXPECT_EQ(sent[0].kind, MessageKind::TEXT);
        EXPECT_EQ(text_of(sent[0].bytes), sendspin::server::build_server_error(*row.error));
    }
}

// connection.md "Failure Handling": silent failures during the Noise handshake
TEST_P(SessionTest, FailsSilentlyOnAnyBadMessage2) {
    struct Row {
        const char* name;
        std::function<void(Harness&)> setup;
        std::function<std::pair<MessageKind, std::string>(Harness&)> message_2;
    };
    const auto text = [](std::string message) {
        return std::make_pair(MessageKind::TEXT, std::move(message));
    };
    const Row rows[] = {
        {"Control:", nullptr, [&](Harness& h) { return text(h.client.message_2()); }},
        {"payload with a space", nullptr,
         [&](Harness& h) { return text(h.client.message_2("{ }")); }},
        {"empty payload", nullptr, [&](Harness& h) { return text(h.client.message_2("")); }},
        {"another PSK", [](Harness& h) { h.client.psk.fill(0x5A); },
         [&](Harness& h) { return text(h.client.message_2()); }},
        {"tampered", nullptr,
         [&](Harness& h) {
             Bytes bytes = *sendspin::server::parse_noise_handshake(h.client.message_2());
             bytes[bytes.size() / 2] ^= 0x01;
             return text(sendspin::server::build_noise_handshake(bytes));
         }},
        {"binary", nullptr,
         [&](Harness& h) { return std::make_pair(MessageKind::BINARY, h.client.message_2()); }},
        {"another type", nullptr,
         [&](Harness& h) {
             std::string message = h.client.message_2();
             message.replace(message.find("noise/handshake"), 15, "noise/handshook");
             return text(message);
         }},
        {"client/goodbye", nullptr,
         [&](Harness&) {
             return text(R"({"type":"client/goodbye","payload":{"reason":"shutdown"}})");
         }},
    };
    for (const Row& row : rows) {
        SCOPED_TRACE(row.name);
        Harness h(GetParam());
        if (row.setup) {
            row.setup(h);
        }
        ASSERT_NO_FATAL_FAILURE(h.exchange_init());
        const auto [kind, message] = row.message_2(h);
        h.session.on_message(kind, bytes_of(message), START_US);
        if (std::string(row.name) == "Control:") {
            EXPECT_EQ(h.session.phase(), SessionPhase::AWAIT_CLIENT_HELLO);
            EXPECT_TRUE(h.session.has_message());
            continue;
        }
        EXPECT_EQ(h.session.phase(), SessionPhase::CLOSED);
        EXPECT_EQ(h.session.end(), SessionEnd::HANDSHAKE_FAILED);
        EXPECT_FALSE(h.session.has_message());
    }
}

// A value too long for the JSON builders ends the session instead of going out as null
TEST_P(SessionTest, EndsWhenAMessageCannotBeEncoded) {
    using sendspin::server::MAX_JSON_STRING_BYTES;
    for (const size_t length : {MAX_JSON_STRING_BYTES, MAX_JSON_STRING_BYTES + 1}) {
        SCOPED_TRACE(length);
        const bool encodable = length == MAX_JSON_STRING_BYTES;
        SessionSettings settings = test_settings();
        settings.hello.name = std::string(length, 'n');
        Harness name(GetParam(), settings);
        ASSERT_NO_FATAL_FAILURE(name.exchange_init());
        name.session.on_message(MessageKind::TEXT, bytes_of(name.client.message_2()), START_US);
        EXPECT_EQ(name.session.has_message(), encodable);
        EXPECT_EQ(name.session.end(),
                  encodable ? std::nullopt : std::optional(SessionEnd::ENCODE_FAILED));

        Harness role(GetParam());
        ASSERT_NO_FATAL_FAILURE(role.complete_handshake());
        role.send(role.client.encrypt_json(client_hello_json()));
        role.session.activate(ServerActivate{
            false, std::vector<std::string>{std::string(length, 'r')}, std::nullopt});
        EXPECT_EQ(role.session.has_message(), encodable);
        EXPECT_EQ(role.session.phase(),
                  encodable ? SessionPhase::ACTIVATING : SessionPhase::CLOSED);
        EXPECT_EQ(role.session.end(),
                  encodable ? std::nullopt : std::optional(SessionEnd::ENCODE_FAILED));
    }
}

// Every way a client can break the rules in transport mode before the initial server/activate
TEST_P(SessionTest, EndsSilentlyOnTransportModeViolations) {
    struct Row {
        const char* name;
        std::function<void(Harness&)> act;
        std::optional<SessionEnd> end;
    };
    const Bytes hello_json = bytes_of(client_hello_json());
    const Row rows[] = {
        {"Control:", [&](Harness& h) { h.send(h.client.encrypt_json(client_hello_json())); },
         std::nullopt},
        {"cleartext",
         [&](Harness& h) { h.session.on_message(MessageKind::TEXT, hello_json, START_US); },
         SessionEnd::CLEARTEXT_IN_TRANSPORT},
        {"tampered ciphertext",
         [&](Harness& h) {
             Bytes message = h.client.encrypt_json(client_hello_json());
             message[3] ^= 0x01;
             h.send(message);
         },
         SessionEnd::DECRYPT_FAILED},
        {"replayed ciphertext",
         [&](Harness& h) {
             Bytes first = h.fragment(sendspin::server::FRAGMENT_FLAG_FIRST, {0, '{'});
             h.send(first);
             h.send(first);
         },
         SessionEnd::DECRYPT_FAILED},
        {"continuation with none in flight",
         [&](Harness& h) { h.send(h.fragment(sendspin::server::FRAGMENT_FLAG_LAST, {'{'})); },
         SessionEnd::MALFORMED_FRAGMENTS},
        {"empty plaintext", [&](Harness& h) { h.send(h.client.encrypt({})); },
         SessionEnd::MALFORMED_FRAGMENTS},
        {"audio before activation", [&](Harness& h) { h.send(h.client.encrypt({4, 0, 0})); },
         SessionEnd::PROTOCOL_ERROR},
        {"fragmented audio before activation",
         [&](Harness& h) {
             h.send(h.fragment(
                 sendspin::server::FRAGMENT_FLAG_FIRST | sendspin::server::FRAGMENT_FLAG_LAST,
                 {4, 0, 0}));
         },
         SessionEnd::PROTOCOL_ERROR},
        {"client/time before activation",
         [&](Harness& h) {
             h.send(h.client.encrypt_json(
                 R"({"type":"client/time","payload":{"client_transmitted":1}})"));
         },
         SessionEnd::PROTOCOL_ERROR},
        {"malformed client/hello",
         [&](Harness& h) {
             h.send(h.client.encrypt_json(R"({"type":"client/hello","payload":{}})"));
         },
         SessionEnd::PROTOCOL_ERROR},
        {"not json", [&](Harness& h) { h.send(h.client.encrypt_json("{")); },
         SessionEnd::PROTOCOL_ERROR},
        {"a second client/hello",
         [&](Harness& h) {
             h.send(h.client.encrypt_json(client_hello_json()));
             h.send(h.client.encrypt_json(client_hello_json()));
         },
         SessionEnd::PROTOCOL_ERROR},
        {"fragmented message over the cap",
         [&](Harness& h) {
             // Full fragments until the message passes the cap
             constexpr size_t FRAGMENT_DATA = 65000;
             Bytes data(FRAGMENT_DATA, ' ');
             data.front() = sendspin::server::MESSAGE_ID_JSON;
             h.send(h.fragment(sendspin::server::FRAGMENT_FLAG_FIRST, data));
             for (size_t sent = FRAGMENT_DATA;
                  sent <= sendspin::server::DEFAULT_MAX_REASSEMBLED_BYTES; sent += FRAGMENT_DATA) {
                 h.send(h.fragment(0, Bytes(FRAGMENT_DATA, ' ')));
             }
         },
         SessionEnd::MESSAGE_TOO_LARGE},
    };
    for (const Row& row : rows) {
        SCOPED_TRACE(row.name);
        Harness h(GetParam());
        ASSERT_NO_FATAL_FAILURE(h.complete_handshake());
        row.act(h);
        EXPECT_FALSE(h.session.has_message());
        if (!row.end) {
            EXPECT_EQ(h.session.phase(), SessionPhase::READY_FOR_ACTIVATION);
            continue;
        }
        EXPECT_EQ(h.session.phase(), SessionPhase::CLOSED);
        EXPECT_EQ(h.session.end(), row.end);
    }
}

// messaging.md "Communication": an encrypted client/goodbye once the handshake has completed
TEST_P(SessionTest, AcceptsAGoodbyeBeforeAndAfterClientHello) {
    for (const bool after_hello : {false, true}) {
        SCOPED_TRACE(after_hello);
        Harness h(GetParam());
        ASSERT_NO_FATAL_FAILURE(h.complete_handshake());
        if (after_hello) {
            h.send(h.client.encrypt_json(client_hello_json()));
            ASSERT_EQ(h.session.phase(), SessionPhase::READY_FOR_ACTIVATION);
        }
        h.send(h.client.encrypt_json(R"({"type":"client/goodbye","payload":{"reason":"restart"}})"),
               7000);
        EXPECT_EQ(h.session.phase(), SessionPhase::CLOSED);
        EXPECT_EQ(h.session.end(), SessionEnd::GOODBYE);
        ASSERT_TRUE(h.session.goodbye());
        EXPECT_EQ(h.session.goodbye()->reason, sendspin::server::GoodbyeReason::RESTART);
        EXPECT_EQ(h.session.goodbye()->rx_us, 7000);
        EXPECT_FALSE(h.session.has_message());
    }
}

// messaging.md "Receive timestamps": a fragmented message's is its last fragment's
TEST_P(SessionTest, StampsAFragmentedClientHelloWithItsLastFragment) {
    Harness h(GetParam());
    ASSERT_NO_FATAL_FAILURE(h.complete_handshake());
    Bytes json = bytes_of(client_hello_json());
    const Bytes head(json.begin(), json.begin() + 10);
    const Bytes tail(json.begin() + 10, json.end());
    Bytes first{sendspin::server::MESSAGE_ID_JSON};
    first.insert(first.end(), head.begin(), head.end());
    h.send(h.fragment(sendspin::server::FRAGMENT_FLAG_FIRST, first), 5000);
    EXPECT_EQ(h.session.phase(), SessionPhase::AWAIT_CLIENT_HELLO);
    h.send(h.fragment(sendspin::server::FRAGMENT_FLAG_LAST, tail), 6000);
    ASSERT_EQ(h.session.phase(), SessionPhase::READY_FOR_ACTIVATION);
    EXPECT_EQ(h.session.client_hello()->name, "Kitchen");
    EXPECT_EQ(h.session.client_hello()->rx_us, 6000);
}

// connection.md "Failure Handling": a handshake timeout is a silent failure
TEST_P(SessionTest, TimesOutEachHandshakeStep) {
    constexpr int64_t INIT_RX_US = START_US + 1000;
    constexpr int64_t MESSAGE_2_RX_US = START_US + 2000;
    struct Row {
        const char* name;
        std::function<void(Harness&)> reach;
        int64_t deadline;
    };
    const Row rows[] = {
        {"AWAIT_CLIENT_INIT", [](Harness&) {}, START_US + TIMEOUT_US},
        {"AWAIT_NOISE_MSG2", [](Harness& h) { h.exchange_init(INIT_RX_US); },
         INIT_RX_US + TIMEOUT_US},
        {"AWAIT_CLIENT_HELLO",
         [](Harness& h) {
             h.exchange_init(INIT_RX_US);
             // server/hello stays queued: a silent end drops it
             h.session.on_message(MessageKind::TEXT, bytes_of(h.client.message_2()),
                                  MESSAGE_2_RX_US);
         },
         MESSAGE_2_RX_US + TIMEOUT_US},
    };
    for (const Row& row : rows) {
        SCOPED_TRACE(row.name);
        Harness h(GetParam());
        row.reach(h);
        const SessionPhase phase = h.session.phase();
        ASSERT_NE(phase, SessionPhase::CLOSED);
        EXPECT_EQ(h.session.deadline_us(), row.deadline);
        h.session.on_deadline(row.deadline - 1);
        EXPECT_EQ(h.session.phase(), phase);
        h.session.on_deadline(row.deadline);
        EXPECT_EQ(h.session.phase(), SessionPhase::CLOSED);
        EXPECT_EQ(h.session.end(), SessionEnd::TIMED_OUT);
        EXPECT_FALSE(h.session.has_message());
    }

    // Control: no step timeout once the session is ready for activation
    Harness h(GetParam());
    ASSERT_NO_FATAL_FAILURE(h.complete_handshake());
    h.send(h.client.encrypt_json(client_hello_json()));
    EXPECT_FALSE(h.session.deadline_us());
    h.session.on_deadline(std::numeric_limits<int64_t>::max());
    EXPECT_EQ(h.session.phase(), SessionPhase::READY_FOR_ACTIVATION);
}

// messaging.md "Fragmentation": a message larger than one Noise transport message is sent as
// consecutive fragments
TEST_P(SessionTest, FragmentsAnOutboundMessageLargerThanOneTransportMessage) {
    SessionSettings settings = test_settings();
    settings.hello.name = std::string(sendspin::server::MAX_TRANSPORT_PLAINTEXT, 'n');
    Harness h(GetParam(), settings);
    ASSERT_NO_FATAL_FAILURE(h.exchange_init());
    h.session.on_message(MessageKind::TEXT, bytes_of(h.client.message_2()), START_US);
    const std::vector<OutboundMessage> sent = h.take_all();
    ASSERT_EQ(sent.size(), 2U);

    Reassembler reassembler(sendspin::server::DEFAULT_MAX_REASSEMBLED_BYTES,
                            [](uint8_t) { return true; });
    for (size_t i = 0; i < sent.size(); ++i) {
        SCOPED_TRACE(i);
        EXPECT_EQ(sent[i].kind, MessageKind::BINARY);
        EXPECT_LE(sent[i].bytes.size(), sendspin::server::MAX_MESSAGE_BYTES);
        const std::optional<Bytes> plaintext = h.client.decrypt(sent[i].bytes);
        ASSERT_TRUE(plaintext);
        EXPECT_EQ(reassembler.accept(*plaintext), i + 1 < sent.size()
                                                      ? Reassembler::Outcome::PENDING
                                                      : Reassembler::Outcome::MESSAGE);
    }
    const std::span<const uint8_t> message = reassembler.message();
    EXPECT_EQ(std::string(message.begin() + 1, message.end()),
              sendspin::server::build_server_hello({settings.hello.name, {}, {}}));
}

// messaging.md "Communication": nothing but client/goodbye from the client until it has the
// initial server/activate, so nothing is answered before that activation is taken
TEST_P(SessionTest, RefusesApplicationMessagesUntilTheActivationIsTaken) {
    struct Row {
        const char* name;
        std::string json;
        SessionEnd end;
    };
    const Row rows[] = {
        {"client/time", client_time_json(1), SessionEnd::PROTOCOL_ERROR},
        {"client/state", R"({"type":"client/state","payload":{}})", SessionEnd::PROTOCOL_ERROR},
        {"Control: client/goodbye", R"({"type":"client/goodbye","payload":{"reason":"shutdown"}})",
         SessionEnd::GOODBYE},
    };
    for (const Row& row : rows) {
        SCOPED_TRACE(row.name);
        Harness h(GetParam());
        ASSERT_NO_FATAL_FAILURE(h.complete_handshake());
        h.send(h.client.encrypt_json(client_hello_json()));
        h.session.activate(ServerActivate{false, std::vector<std::string>{}, std::nullopt});
        ASSERT_EQ(h.session.phase(), SessionPhase::ACTIVATING);
        h.send(h.client.encrypt_json(row.json));
        EXPECT_EQ(h.session.phase(), SessionPhase::CLOSED);
        EXPECT_EQ(h.session.end(), row.end);
        EXPECT_FALSE(h.session.has_message());
    }
}

// messaging.md "Communication": the ignore rules and time replies start when the activation itself
// is taken, not when the control messages queued behind it are
TEST_P(SessionTest, BecomesActiveWhenTheActivationIsTaken) {
    Harness h(GetParam());
    ASSERT_NO_FATAL_FAILURE(h.complete_handshake());
    h.send(h.client.encrypt_json(client_hello_json()));
    h.session.activate(ServerActivate{false, std::vector<std::string>{}, std::nullopt});
    const std::string follower = R"({"type":"group/update","payload":{}})";
    ASSERT_TRUE(h.session.queue_json(follower));
    const std::optional<OutboundMessage> activation = h.session.take_message();
    ASSERT_TRUE(activation);
    EXPECT_NE(h.decrypt_json(*activation).find("server/activate"), std::string::npos);
    EXPECT_EQ(h.session.phase(), SessionPhase::ACTIVE);

    // A time request the client sends on receiving the activation is answered, ahead of the
    // control message still queued
    h.send(h.client.encrypt_json(client_time_json(3)));
    const std::vector<OutboundMessage> sent = h.take_all();
    ASSERT_EQ(sent.size(), 2U);
    EXPECT_NE(h.decrypt_json(sent[0]).find("server/time"), std::string::npos);
    EXPECT_EQ(h.decrypt_json(sent[1]), follower);
    EXPECT_EQ(h.session.phase(), SessionPhase::ACTIVE);
}

// A server/time reply ends its write (SessionDriver::on_writable()), so no message is built
// between its stamp and its write: it alone is marked so
TEST_P(SessionTest, MarksOnlyATimeReplyAsEndingItsWrite) {
    Harness h(GetParam());
    ASSERT_NO_FATAL_FAILURE(h.activate());
    const std::string control = R"({"type":"group/update","payload":{}})";
    h.session.queue_json_message(control);
    h.send(h.client.encrypt_json(client_time_json(3)));
    const std::vector<OutboundMessage> sent = h.take_all();
    ASSERT_EQ(sent.size(), 2U);
    EXPECT_NE(h.decrypt_json(sent[0]).find("server/time"), std::string::npos);
    EXPECT_TRUE(sent[0].time_reply);
    EXPECT_EQ(h.decrypt_json(sent[1]), control);
    EXPECT_FALSE(sent[1].time_reply);
}

// messaging.md "server/time" and "Transmit timestamps": the receive stamp of the request, and a
// transmit stamp read when the reply is taken, not when it was queued
TEST_P(SessionTest, AnswersClientTimeWithAStampTakenWhenTheReplyIsTaken) {
    Harness h(GetParam());
    ASSERT_NO_FATAL_FAILURE(h.activate());
    h.send(h.client.encrypt_json(client_time_json(42)), 777);
    ASSERT_TRUE(h.session.has_message());
    // A stamp read at queue time would be at most this value
    const int64_t queued_by = sendspin::server::now_us();
    while (sendspin::server::now_us() <= queued_by) {}
    const int64_t before = sendspin::server::now_us();
    const std::optional<OutboundMessage> reply = h.session.take_message();
    const int64_t after = sendspin::server::now_us();
    ASSERT_TRUE(reply);
    EXPECT_FALSE(h.session.has_message());

    JsonDocument doc;
    ASSERT_EQ(deserializeJson(doc, h.decrypt_json(*reply)), DeserializationError::Ok);
    EXPECT_EQ(doc["type"], "server/time");
    EXPECT_EQ(doc["payload"]["client_transmitted"].as<int64_t>(), 42);
    EXPECT_EQ(doc["payload"]["server_received"].as<int64_t>(), 777);
    const int64_t transmitted = doc["payload"]["server_transmitted"].as<int64_t>();
    EXPECT_GE(transmitted, before);
    EXPECT_LE(transmitted, after);
}

// messaging.md "Communication": after the initial server/activate, unrecognized types, unknown
// payload fields and unimplemented binary IDs are ignored, while recognized messages keep their
// validation, direction and sequencing rules, and fragments their malformed-sequence rules
TEST_P(SessionTest, AppliesTheIgnoreRulesOnceActive) {
    using sendspin::server::FRAGMENT_FLAG_FIRST;
    using sendspin::server::FRAGMENT_FLAG_LAST;
    struct Row {
        const char* name;
        std::function<void(Harness&)> act;
        std::optional<SessionEnd> end;
    };
    const auto json = [](const std::string& text) {
        return [text](Harness& h) { h.send(h.client.encrypt_json(text)); };
    };
    const Row rows[] = {
        {"Control: nothing", [](Harness&) {}, std::nullopt},
        {"an unrecognized type", json(R"({"type":"client/telepathy","payload":{"x":1}})"),
         std::nullopt},
        {"an unrecognized server type", json(R"({"type":"server/telepathy","payload":{}})"),
         std::nullopt},
        {"client/state", json(R"({"type":"client/state","payload":{"available":true,"x":{}}})"),
         std::nullopt},
        // messaging.md "server/activate": an inactive role's object is ignored, valid or not
        {"client/state with an invalid object of an inactive role",
         json(R"({"type":"client/state","payload":{"available":false,"player":{"volume":7}}})"),
         std::nullopt},
        {"client/command", json(R"({"type":"client/command","payload":{}})"), std::nullopt},
        {"client/leave", json(R"({"type":"client/leave","payload":{}})"), std::nullopt},
        {"an unimplemented source role ID", [](Harness& h) { h.send(h.client.encrypt({13, 1})); },
         std::nullopt},
        {"an application-specific binary ID", [](Harness& h) { h.send(h.client.encrypt({200})); },
         std::nullopt},
        {"a fragmented binary message",
         [](Harness& h) {
             h.send(h.fragment(FRAGMENT_FLAG_FIRST, {200, 1}));
             h.send(h.fragment(FRAGMENT_FLAG_LAST, {2}));
         },
         std::nullopt},
        {"not json", json("{"), SessionEnd::PROTOCOL_ERROR},
        {"no payload object", json(R"({"type":"client/telepathy"})"), SessionEnd::PROTOCOL_ERROR},
        {"an invalid client/time", json(R"({"type":"client/time","payload":{}})"),
         SessionEnd::PROTOCOL_ERROR},
        {"a client/state without available", json(R"({"type":"client/state","payload":{}})"),
         SessionEnd::PROTOCOL_ERROR},
        {"a second client/hello", json(client_hello_json()), SessionEnd::PROTOCOL_ERROR},
        {"a server message",
         json(R"({"type":"server/time","payload":{"client_transmitted":1,"server_received":2,)"
              R"("server_transmitted":3}})"),
         SessionEnd::PROTOCOL_ERROR},
        {"an unsolicited noise/handshake",
         json(R"({"type":"noise/handshake","payload":{"data":"AAAA"}})"),
         SessionEnd::PROTOCOL_ERROR},
        {"a continuation with none in flight",
         [](Harness& h) { h.send(h.fragment(FRAGMENT_FLAG_LAST, {'{'})); },
         SessionEnd::MALFORMED_FRAGMENTS},
        {"cleartext",
         [](Harness& h) {
             h.session.on_message(MessageKind::TEXT, bytes_of(client_time_json(1)), START_US);
         },
         SessionEnd::CLEARTEXT_IN_TRANSPORT},
    };
    for (const Row& row : rows) {
        SCOPED_TRACE(row.name);
        Harness h(GetParam());
        ASSERT_NO_FATAL_FAILURE(h.activate());
        row.act(h);
        if (row.end) {
            EXPECT_EQ(h.session.phase(), SessionPhase::CLOSED);
            EXPECT_EQ(h.session.end(), row.end);
            EXPECT_FALSE(h.session.has_message());
            continue;
        }
        // Nothing answered the ignored message (a client/state's marker emits nothing), and the
        // session still answers client/time
        EXPECT_TRUE(h.take_all().empty());
        EXPECT_FALSE(h.session.has_message());
        h.send(h.client.encrypt_json(client_time_json(9)));
        const std::vector<OutboundMessage> sent = h.take_all();
        ASSERT_EQ(sent.size(), 1U);
        EXPECT_NE(h.decrypt_json(sent[0]).find(R"("client_transmitted":9)"), std::string::npos);
        EXPECT_EQ(h.session.phase(), SessionPhase::ACTIVE);
    }
}

// messaging.md "client/goodbye": recorded, and the server ends the connection
TEST_P(SessionTest, EndsOnAGoodbyeOnceActive) {
    Harness h(GetParam());
    ASSERT_NO_FATAL_FAILURE(h.activate());
    h.send(h.client.encrypt_json(client_time_json(1)));
    h.send(
        h.client.encrypt_json(R"({"type":"client/goodbye","payload":{"reason":"another_server"}})"),
        8000);
    EXPECT_EQ(h.session.phase(), SessionPhase::CLOSED);
    EXPECT_EQ(h.session.end(), SessionEnd::GOODBYE);
    ASSERT_TRUE(h.session.goodbye());
    EXPECT_EQ(h.session.goodbye()->reason, sendspin::server::GoodbyeReason::ANOTHER_SERVER);
    EXPECT_EQ(h.session.goodbye()->rx_us, 8000);
    // The pending time reply is dropped with the connection
    EXPECT_FALSE(h.session.has_message());
}

// A client that floods time requests the server cannot write is closed
TEST_P(SessionTest, ClosesWhenTheTimeQueueOverflows) {
    SessionSettings settings = test_settings();
    settings.queue_caps.time_entries = 2;
    Harness h(GetParam(), settings);
    ASSERT_NO_FATAL_FAILURE(h.activate());
    h.send(h.client.encrypt_json(client_time_json(1)));
    h.send(h.client.encrypt_json(client_time_json(2)));
    EXPECT_EQ(h.session.phase(), SessionPhase::ACTIVE);
    h.send(h.client.encrypt_json(client_time_json(3)));
    EXPECT_EQ(h.session.phase(), SessionPhase::CLOSED);
    EXPECT_EQ(h.session.end(), SessionEnd::QUEUE_OVERFLOW);
    EXPECT_FALSE(h.session.has_message());
}

class RecordingTrace : public SessionTrace {
public:
    struct Entry {
        bool outbound;
        MessageKind kind;
        std::string text;  // A binary plaintext without its ID byte
        int64_t stamp_us;
    };

    void on_inbound(MessageKind kind, std::span<const uint8_t> plaintext, int64_t rx_us) override {
        this->record(false, kind, plaintext, rx_us);
    }

    void on_outbound(MessageKind kind, std::span<const uint8_t> plaintext, int64_t stamp_us,
                     size_t /*audio_bytes*/) override {
        this->record(true, kind, plaintext, stamp_us);
    }

    // A Session never reports writes; its SessionDriver does
    void on_written(int64_t /*done_us*/) override {}

    std::vector<Entry> entries;

private:
    void record(bool outbound, MessageKind kind, std::span<const uint8_t> plaintext,
                int64_t stamp_us) {
        if (kind == MessageKind::BINARY) {
            EXPECT_EQ(plaintext.front(), wire::JSON_ID);
            plaintext = plaintext.subspan(1);
        }
        this->entries.push_back({outbound, kind, {plaintext.begin(), plaintext.end()}, stamp_us});
    }
};

std::string type_of(const std::string& json) {
    JsonDocument doc;
    EXPECT_EQ(deserializeJson(doc, json), DeserializationError::Ok) << json;
    return doc["type"].as<std::string>();
}

// The message trace: every message's plaintext in both directions, in order, inbound with its
// receive stamp and outbound with the stamp the message carries
TEST_P(SessionTest, TracesThePlaintextOfEveryMessage) {
    RecordingTrace trace;
    Harness h(GetParam(), test_settings(), &trace);
    ASSERT_NO_FATAL_FAILURE(h.exchange_init(1100));
    h.session.on_message(MessageKind::TEXT, bytes_of(h.client.message_2()), 1200);
    // The client decrypts each message, so its receive nonce follows the server's
    for (const OutboundMessage& message : h.take_all()) {
        h.decrypt_json(message);
    }
    h.send(h.client.encrypt_json(client_hello_json()), 1300);
    h.session.activate(ServerActivate{false, std::vector<std::string>{}, std::nullopt});
    for (const OutboundMessage& message : h.take_all()) {
        h.decrypt_json(message);
    }
    h.send(h.client.encrypt_json(client_time_json(5)), 1400);
    const std::vector<OutboundMessage> reply = h.take_all();
    ASSERT_EQ(reply.size(), 1U);

    struct Expected {
        bool outbound;
        MessageKind kind;
        const char* type;
        int64_t rx_us;  // inbound only
    };
    const Expected expected[] = {
        {false, MessageKind::TEXT, "client/init", 1100},
        {true, MessageKind::TEXT, "server/init", 0},
        {true, MessageKind::TEXT, "noise/handshake", 0},
        {false, MessageKind::TEXT, "noise/handshake", 1200},
        {true, MessageKind::BINARY, "server/hello", 0},
        {false, MessageKind::BINARY, "client/hello", 1300},
        {true, MessageKind::BINARY, "server/activate", 0},
        {false, MessageKind::BINARY, "client/time", 1400},
        {true, MessageKind::BINARY, "server/time", 0},
    };
    ASSERT_EQ(trace.entries.size(), std::size(expected));
    for (size_t i = 0; i < trace.entries.size(); ++i) {
        SCOPED_TRACE(i);
        const RecordingTrace::Entry& entry = trace.entries[i];
        EXPECT_EQ(entry.outbound, expected[i].outbound);
        EXPECT_EQ(entry.kind, expected[i].kind);
        EXPECT_EQ(type_of(entry.text), expected[i].type);
        if (!entry.outbound) {
            EXPECT_EQ(entry.stamp_us, expected[i].rx_us);
        }
    }
    EXPECT_EQ(trace.entries[0].text, h.client_init);
    // The traced reply is the plaintext the client decrypts, stamped with its own value
    const RecordingTrace::Entry& time = trace.entries.back();
    EXPECT_EQ(time.text, h.decrypt_json(reply[0]));
    JsonDocument doc;
    ASSERT_EQ(deserializeJson(doc, time.text), DeserializationError::Ok);
    EXPECT_EQ(doc["payload"]["server_transmitted"].as<int64_t>(), time.stamp_us);
}

// messaging.md "client/goodbye": a goodbye before activation is reported with its reason, and
// any other end before activation as a failed handshake. Only the session decides, so a client
// that closes right after its goodbye is still reported as leaving with it.
TEST_P(SessionTest, ReportsAGoodbyeBeforeActivationWithItsReason) {
    using sendspin::server::ErrorKind;
    using sendspin::server::GoodbyeReason;
    struct Row {
        const char* name;
        std::string json;
        ErrorKind kind;
        std::optional<GoodbyeReason> reason;
    };
    const Row rows[] = {
        {"a goodbye", R"({"type":"client/goodbye","payload":{"reason":"restart"}})",
         ErrorKind::GOODBYE_BEFORE_ACTIVATION, GoodbyeReason::RESTART},
        {"a goodbye with an unrecognized reason",
         R"({"type":"client/goodbye","payload":{"reason":"moving"}})",
         ErrorKind::GOODBYE_BEFORE_ACTIVATION, std::nullopt},
        {"Control: a protocol error", R"({"type":"client/state","payload":{}})",
         ErrorKind::HANDSHAKE_FAILED, std::nullopt},
        {"Control: the session still open, as when the transport closed first", "",
         ErrorKind::HANDSHAKE_FAILED, std::nullopt},
    };
    for (const Row& row : rows) {
        SCOPED_TRACE(row.name);
        Harness h(GetParam());
        ASSERT_NO_FATAL_FAILURE(h.complete_handshake());
        if (!row.json.empty()) {
            h.send(h.client.encrypt_json(row.json));
        }
        const sendspin::server::ServerError error =
            sendspin::server::error_before_activation("ws://kitchen/sendspin", h.session.outcome());
        EXPECT_EQ(error.kind, row.kind);
        EXPECT_EQ(error.goodbye_reason, row.reason);
        EXPECT_EQ(error.url, "ws://kitchen/sendspin");
        EXPECT_EQ(error.client_id, h.client.identity().peer_id());
    }
}

// ==============================================================================
// Trust: the PSK message 1 references, the Sentinel Fallback and re-handshakes
// ==============================================================================

// connection.md "Pre-Shared Key": message 1 references the PSK the credentials choose, by its
// psk_id and category, and a client holding it keys the session with it. "Sentinel Fallback": a
// client that answers a long-term or pairing PSK under the Sentinel completes the handshake, an
// authenticated credential-mismatch signal; any other PSK fails it silently, the Sentinel's own
// reference included.
TEST_P(SessionTest, MatchesTheReferencedPskOrFallsBackToTheSentinel) {
    using sendspin::server::HandshakePsk;
    using sendspin::server::PskCategory;
    using sendspin::server::PskMatch;
    struct Row {
        const char* name;
        HandshakePsk referenced;
        sendspin::server::Psk client_psk;
        std::optional<PskMatch> match;
    };
    const sendspin::server::Psk& sentinel = sendspin::server::SENTINEL_PSK;
    const Row rows[] = {
        {"Control: the Sentinel", {}, sentinel, PskMatch{PskCategory::SENTINEL, std::nullopt}},
        {"Control: a long-term PSK the client holds",
         {PskCategory::LONG_TERM, psk_of(1)},
         psk_of(1),
         PskMatch{PskCategory::LONG_TERM, std::nullopt}},
        {"Control: a pairing PSK the client holds",
         {PskCategory::PAIRING, psk_of(2)},
         psk_of(2),
         PskMatch{PskCategory::PAIRING, std::nullopt}},
        {"a long-term PSK answered under the Sentinel",
         {PskCategory::LONG_TERM, psk_of(1)},
         sentinel,
         PskMatch{PskCategory::SENTINEL, PskCategory::LONG_TERM}},
        {"a pairing PSK answered under the Sentinel",
         {PskCategory::PAIRING, psk_of(2)},
         sentinel,
         PskMatch{PskCategory::SENTINEL, PskCategory::PAIRING}},
        {"a long-term PSK answered under another",
         {PskCategory::LONG_TERM, psk_of(1)},
         psk_of(3),
         std::nullopt},
        {"the Sentinel answered under another", {}, psk_of(3), std::nullopt},
    };
    for (const Row& row : rows) {
        SCOPED_TRACE(row.name);
        FixedCredentials credentials(row.referenced);
        Harness h(GetParam(), test_settings(), nullptr, &credentials);
        h.client.psk = row.client_psk;
        ASSERT_NO_FATAL_FAILURE(h.exchange_init());
        JsonDocument payload_1;
        ASSERT_EQ(deserializeJson(payload_1, h.client.message_1_payload), DeserializationError::Ok);
        EXPECT_EQ(payload_1["psk_id"], sendspin::server::psk_id_for(row.referenced.psk));
        EXPECT_EQ(payload_1["psk_category"], sendspin::server::to_wire(row.referenced.category));
        h.session.on_message(MessageKind::TEXT, bytes_of(h.client.message_2()), START_US);
        if (!row.match) {
            EXPECT_EQ(h.session.end(), SessionEnd::HANDSHAKE_FAILED);
            EXPECT_TRUE(h.take_all().empty());
            continue;
        }
        ASSERT_EQ(h.session.phase(), SessionPhase::AWAIT_CLIENT_HELLO);
        EXPECT_EQ(h.session.psk_match(), *row.match);
        EXPECT_EQ(h.session.next_psk_match(), *row.match);
        // The keys work both ways: server/hello decrypts, and so does a client/hello
        const std::vector<OutboundMessage> sent = h.take_all();
        ASSERT_EQ(sent.size(), 1U);
        EXPECT_NE(h.decrypt_json(sent[0]).find(R"("type":"server/hello")"), std::string::npos);
        h.send(h.client.encrypt_json(client_hello_json()));
        EXPECT_EQ(h.session.phase(), SessionPhase::READY_FOR_ACTIVATION);
    }
}

// connection.md "Re-handshake": message 1 travels as an encrypted noise/handshake under the current
// keys with `h` as the prologue; nothing is written until message 2, old-key messages are taken in
// meanwhile (a time request keeps its receive stamp), and the frozen server/activate is the first
// message under the new keys, ahead of the time reply and anything queued during the freeze. The
// re-handshake is pending from rekey() until the frozen activation is taken.
TEST_P(SessionTest, RekeysWithTheFrozenActivationFirstUnderTheNewKeys) {
    using sendspin::server::HandshakePsk;
    using sendspin::server::PskCategory;
    using sendspin::server::PskMatch;
    Harness h(GetParam());
    ASSERT_NO_FATAL_FAILURE(h.activate());
    const sendspin::server::HandshakeHash prior_h = h.client.handshake_hash();
    EXPECT_FALSE(h.session.rekey_pending());
    h.session.rekey(HandshakePsk{PskCategory::LONG_TERM, psk_of(4)},
                    ServerActivate{true, std::nullopt, std::nullopt});
    EXPECT_TRUE(h.session.rekey_pending());
    // The activations queued from now go out under the new PSK
    EXPECT_EQ(h.session.next_psk_match(), (PskMatch{PskCategory::LONG_TERM, std::nullopt}));
    EXPECT_EQ(h.session.psk_match(), (PskMatch{PskCategory::SENTINEL, std::nullopt}));
    std::vector<OutboundMessage> sent = h.take_all();
    ASSERT_EQ(sent.size(), 1U);
    const std::string message_1 = h.decrypt_json(sent[0]);
    h.client.psk = psk_of(4);
    ASSERT_TRUE(h.client.read_rekey_message_1(message_1)) << message_1;
    JsonDocument payload_1;
    ASSERT_EQ(deserializeJson(payload_1, h.client.message_1_payload), DeserializationError::Ok);
    EXPECT_EQ(payload_1["psk_id"], sendspin::server::psk_id_for(psk_of(4)));
    EXPECT_EQ(payload_1["psk_category"], "lt");
    ASSERT_TRUE(h.session.deadline_us());
    EXPECT_TRUE(h.session.rekey_pending());

    // The freeze: a time request under the old keys and a queued message wait
    constexpr int64_t TIME_RX_US = 7000;
    h.send(h.client.encrypt_json(client_time_json(55)), TIME_RX_US);
    h.session.queue_json_message(R"({"type":"group/update","payload":{}})");
    EXPECT_FALSE(h.session.has_message());
    EXPECT_FALSE(h.session.poll().wake_us);

    h.send(h.client.rekey_message_2());
    ASSERT_EQ(h.session.phase(), SessionPhase::ACTIVE);
    EXPECT_EQ(h.session.psk_match(), (PskMatch{PskCategory::LONG_TERM, std::nullopt}));
    EXPECT_FALSE(h.session.deadline_us());
    EXPECT_NE(h.client.handshake_hash(), prior_h);
    EXPECT_TRUE(h.session.rekey_pending());
    sent = h.take_all();
    EXPECT_FALSE(h.session.rekey_pending());
    ASSERT_EQ(sent.size(), 3U);
    EXPECT_EQ(h.decrypt_json(sent[0]),
              R"({"type":"server/activate","payload":{"activities":["playback"]}})");
    JsonDocument time;
    ASSERT_EQ(deserializeJson(time, h.decrypt_json(sent[1])), DeserializationError::Ok);
    EXPECT_EQ(time["type"], "server/time");
    EXPECT_EQ(time["payload"]["server_received"], TIME_RX_US);
    EXPECT_NE(h.decrypt_json(sent[2]).find("group/update"), std::string::npos);
    // The client's messages under the new keys are taken in
    h.send(h.client.encrypt_json(client_time_json(56)));
    EXPECT_EQ(h.session.phase(), SessionPhase::ACTIVE);
    EXPECT_EQ(h.take_all().size(), 1U);
}

// Rekeys queued together re-handshake in turn, each to its own PSK, the second under the keys the
// first made
TEST_P(SessionTest, RekeysQueuedTogetherEachUseTheirOwnPsk) {
    using sendspin::server::HandshakePsk;
    using sendspin::server::PskCategory;
    Harness h(GetParam());
    ASSERT_NO_FATAL_FAILURE(h.activate());
    const ServerActivate activation{true, std::nullopt, std::nullopt};
    h.session.rekey(HandshakePsk{PskCategory::LONG_TERM, psk_of(4)}, activation);
    h.session.rekey(HandshakePsk{PskCategory::LONG_TERM, psk_of(5)}, activation);
    std::vector<OutboundMessage> sent = h.take_all();
    ASSERT_EQ(sent.size(), 1U);
    h.client.psk = psk_of(4);
    ASSERT_TRUE(h.client.read_rekey_message_1(h.decrypt_json(sent[0])));
    JsonDocument payload;
    ASSERT_EQ(deserializeJson(payload, h.client.message_1_payload), DeserializationError::Ok);
    EXPECT_EQ(payload["psk_id"], sendspin::server::psk_id_for(psk_of(4)));
    h.send(h.client.rekey_message_2());

    // The first frozen activation, then the second rekey's message 1
    sent = h.take_all();
    ASSERT_EQ(sent.size(), 2U);
    EXPECT_EQ(h.decrypt_json(sent[0]),
              R"({"type":"server/activate","payload":{"activities":["playback"]}})");
    h.client.psk = psk_of(5);
    ASSERT_TRUE(h.client.read_rekey_message_1(h.decrypt_json(sent[1])));
    ASSERT_EQ(deserializeJson(payload, h.client.message_1_payload), DeserializationError::Ok);
    EXPECT_EQ(payload["psk_id"], sendspin::server::psk_id_for(psk_of(5)));
    h.send(h.client.rekey_message_2());
    EXPECT_EQ(h.session.phase(), SessionPhase::ACTIVE);
    EXPECT_EQ(h.take_all().size(), 1U);
}

// pairing.md "Pairing index": the pairing activations written since the last Noise handshake,
// counted as each is taken for writing; a re-handshake restarts the count, its frozen activation
// the first of the new handshake. A pairing message is handed over parsed; a malformed one is a
// protocol error ("Protocol Errors").
TEST_P(SessionTest, CountsPairingActivationsAndHandsOverPairingMessages) {
    using sendspin::server::HandshakePsk;
    using sendspin::server::PairingParams;
    using sendspin::server::PairMethod;
    using sendspin::server::PskCategory;
    Harness h(GetParam());
    ASSERT_NO_FATAL_FAILURE(h.activate());
    const ServerActivate pairing{false, std::nullopt,
                                 PairingParams{PairMethod::STATIC_PAIRING_CODE, std::nullopt}};
    const ServerActivate plain{false, std::nullopt, std::nullopt};
    EXPECT_EQ(h.session.pairing_index(), 0U);
    h.session.activate(pairing);
    h.session.activate(plain);
    h.session.activate(pairing);
    EXPECT_EQ(h.session.queued_pairing_index(), 2U);
    EXPECT_EQ(h.session.pairing_index(), 0U);
    const std::vector<OutboundMessage> sent = h.take_all();
    ASSERT_EQ(sent.size(), 3U);
    EXPECT_EQ(h.decrypt_json(sent[0]),
              R"({"type":"server/activate","payload":{"activities":["pairing"],)"
              R"("pairing":{"method":"static_pairing_code"}}})");
    EXPECT_EQ(h.decrypt_json(sent[1]), R"({"type":"server/activate","payload":{"activities":[]}})");
    h.decrypt_json(sent[2]);
    EXPECT_EQ(h.session.pairing_index(), 2U);

    h.send(h.client.encrypt_json(R"({"type":"client/pair-init","payload":{"pairing_index":2}})"));
    const std::vector<sendspin::server::ClientPairing> taken = h.session.take_pairing_messages();
    ASSERT_EQ(taken.size(), 1U);
    EXPECT_EQ(taken[0].pairing_index, 2U);
    EXPECT_TRUE(h.session.take_pairing_messages().empty());

    h.session.rekey(HandshakePsk{PskCategory::LONG_TERM, psk_of(4)}, pairing);
    EXPECT_EQ(h.session.queued_pairing_index(), 1U);
    const std::vector<OutboundMessage> message_1 = h.take_all();
    ASSERT_EQ(message_1.size(), 1U);
    h.client.psk = psk_of(4);
    ASSERT_TRUE(h.client.read_rekey_message_1(h.decrypt_json(message_1[0])));
    h.send(h.client.rekey_message_2());
    EXPECT_EQ(h.take_all().size(), 1U);
    EXPECT_EQ(h.session.pairing_index(), 1U);

    h.send(h.client.encrypt_json(R"({"type":"client/pair-init","payload":{}})"));
    EXPECT_EQ(h.session.end(), SessionEnd::PROTOCOL_ERROR);
}

// Whether a vector's bytes, read to its capacity, hold needle. The spare capacity is outside the
// container's annotation, so the scan is not instrumented and calls nothing that is.
__attribute__((no_sanitize("address"))) bool holds_to_capacity(const std::vector<uint8_t>& bytes,
                                                               std::string_view needle) {
    const uint8_t* data = bytes.data();
    for (size_t start = 0; start + needle.size() <= bytes.capacity(); ++start) {
        size_t matched = 0;
        while (matched < needle.size() &&
               data[start + matched] == static_cast<uint8_t>(needle[matched])) {
            ++matched;
        }
        if (matched == needle.size()) {
            return true;
        }
    }
    return false;
}

// pairing.md "client/pair-finalize": a pair-finalize carrying both long_term_psk and wrapped_psk
// fails validation, and its PSK is wiped from the decrypted message and the reassembly buffer,
// spare capacity included, as a valid one's is (the Control), and as it is from a body whose type
// cannot be read and from a fragment sequence that fails or outgrows the cap with the PSK buffered.
// The first fragment's check that the scan finds the PSK is the Control of the scan.
TEST_P(SessionTest, WipesTheSecretOfAPairingMessageWhetherOrNotItPassesValidation) {
    using sendspin::server::FRAGMENT_FLAG_FIRST;
    using sendspin::server::FRAGMENT_FLAG_LAST;
    using sendspin::server::FRAGMENT_FLAGS_RESERVED;
    const std::string psk = "QUJDREVGR0hJSktMTU5PUFFSU1RVVldYWVphYmNkZWY";
    const std::string valid_head =
        R"({"type":"client/pair-finalize","payload":{"long_term_psk":")" + psk;
    const std::string valid_tail = R"("}})";
    const std::string head = valid_head + R"(","wrapped_psk":"AAAA)";
    const std::string tail = R"("}})";
    struct Case {
        const char* name;
        std::string head;
        std::string tail;
        // Whether head and tail go as two fragments rather than one message
        bool fragmented;
        // The second fragment's flags: LAST completes the message, a reserved bit fails it
        uint8_t second_flags;
        std::optional<SessionEnd> end;
        // The cap on a reassembled message: the head fits, head and tail do not
        size_t max_reassembled_bytes{sendspin::server::DEFAULT_MAX_REASSEMBLED_BYTES};
    };
    const std::vector<Case> cases{
        {"Control: a valid one in one message", valid_head, valid_tail, false, 0, std::nullopt},
        {"Control: a valid one in two fragments", valid_head, valid_tail, true, FRAGMENT_FLAG_LAST,
         std::nullopt},
        {"in one message", head, tail, false, 0, SessionEnd::PROTOCOL_ERROR},
        {"in a body that is not JSON, its type unread", head, "", false, 0,
         SessionEnd::PROTOCOL_ERROR},
        {"in two fragments, the PSK in the first", head, tail, true, FRAGMENT_FLAG_LAST,
         SessionEnd::PROTOCOL_ERROR},
        {"in a fragment sequence that fails", head, tail, true, FRAGMENT_FLAGS_RESERVED,
         SessionEnd::MALFORMED_FRAGMENTS},
        {"in a fragment sequence that outgrows the cap", head, tail, true, FRAGMENT_FLAG_LAST,
         SessionEnd::MESSAGE_TOO_LARGE, head.size()},
    };
    for (const Case& c : cases) {
        SCOPED_TRACE(c.name);
        SessionSettings settings = test_settings();
        settings.max_reassembled_bytes = c.max_reassembled_bytes;
        Harness h(GetParam(), settings);
        ASSERT_NO_FATAL_FAILURE(h.activate());
        if (c.fragmented) {
            Bytes first{sendspin::server::MESSAGE_ID_JSON};
            const Bytes head_bytes = bytes_of(c.head);
            first.insert(first.end(), head_bytes.begin(), head_bytes.end());
            h.send(h.fragment(FRAGMENT_FLAG_FIRST, first));
            EXPECT_TRUE(holds_to_capacity(h.session.scratch_, psk));
            EXPECT_TRUE(holds_to_capacity(h.session.reassembler_.buffer_, psk));
            h.send(h.fragment(c.second_flags, bytes_of(c.tail)));
        } else {
            h.send(h.client.encrypt_json(c.head + c.tail));
        }
        EXPECT_EQ(h.session.end(), c.end);
        EXPECT_EQ(h.session.take_pairing_messages().size(), c.end ? 0U : 1U);
        EXPECT_FALSE(holds_to_capacity(h.session.scratch_, psk));
        EXPECT_FALSE(holds_to_capacity(h.session.reassembler_.buffer_, psk));
    }
}

// A client/pair-finalize whose tag fails leaves no PSK in the decrypted message's buffer: AES-GCM
// decrypts in place before its tag check (ChaChaPoly checks first), and the buffer is wiped
TEST_P(SessionTest, WipesAPairingMessageWhoseTagFails) {
    const std::string psk = "QUJDREVGR0hJSktMTU5PUFFSU1RVVldYWVphYmNkZWY";
    Harness h(GetParam());
    ASSERT_NO_FATAL_FAILURE(h.activate());
    Bytes message = h.client.encrypt_json(
        R"({"type":"client/pair-finalize","payload":{"long_term_psk":")" + psk + R"("}})");
    message.back() ^= 0x01;
    h.send(message);
    EXPECT_EQ(h.session.end(), SessionEnd::DECRYPT_FAILED);
    EXPECT_FALSE(holds_to_capacity(h.session.scratch_, psk));
}

// A session that ends while a pairing message's first fragment is pending leaves its PSK in
// neither buffer, whether the host closes it or the next message fails to decrypt. The scan finds
// the PSK before the end: the Control.
TEST_P(SessionTest, WipesAPendingPairingFragmentAtTheEnd) {
    using sendspin::server::FRAGMENT_FLAG_FIRST;
    const std::string psk = "QUJDREVGR0hJSktMTU5PUFFSU1RVVldYWVphYmNkZWY";
    struct Case {
        const char* name;
        std::function<void(Harness&)> end_session;
        SessionEnd end;
    };
    const std::vector<Case> cases{
        {"closed by the host", [](Harness& h) { h.session.close_by_host(); },
         SessionEnd::CLOSED_BY_HOST},
        {"a message that fails to decrypt", [](Harness& h) { h.send(Bytes(32, 0)); },
         SessionEnd::DECRYPT_FAILED},
    };
    for (const Case& c : cases) {
        SCOPED_TRACE(c.name);
        Harness h(GetParam());
        ASSERT_NO_FATAL_FAILURE(h.activate());
        Bytes first{sendspin::server::MESSAGE_ID_JSON};
        const Bytes head =
            bytes_of(R"({"type":"client/pair-finalize","payload":{"long_term_psk":")" + psk);
        first.insert(first.end(), head.begin(), head.end());
        h.send(h.fragment(FRAGMENT_FLAG_FIRST, first));
        EXPECT_TRUE(holds_to_capacity(h.session.scratch_, psk));
        EXPECT_TRUE(holds_to_capacity(h.session.reassembler_.buffer_, psk));
        c.end_session(h);
        EXPECT_EQ(h.session.end(), c.end);
        EXPECT_FALSE(holds_to_capacity(h.session.scratch_, psk));
        EXPECT_FALSE(holds_to_capacity(h.session.reassembler_.buffer_, psk));
    }
}

// A pairing message's first fragment, its PSK in it, followed by a longer one, grows both the
// decrypted message's buffer and the reassembly buffer; neither frees a block holding the PSK.
// The scan's Control is SecureReserve.WipesTheBlockItLeaves.
TEST_P(SessionTest, GrowsItsBuffersWithoutFreeingAPairingSecret) {
    using sendspin::server::FRAGMENT_FLAG_FIRST;
    if (!FreedBlockScan::available()) {
        GTEST_SKIP() << "needs the sanitizer's free hook";
    }
    const std::string psk = "QUJDREVGR0hJSktMTU5PUFFSU1RVVldYWVphYmNkZWY";
    Harness h(GetParam());
    ASSERT_NO_FATAL_FAILURE(h.activate());
    Bytes first{sendspin::server::MESSAGE_ID_JSON};
    const Bytes head =
        bytes_of(R"({"type":"client/pair-finalize","payload":{"long_term_psk":")" + psk);
    first.insert(first.end(), head.begin(), head.end());
    h.send(h.fragment(FRAGMENT_FLAG_FIRST, first));
    ASSERT_TRUE(holds_to_capacity(h.session.scratch_, psk));
    ASSERT_TRUE(holds_to_capacity(h.session.reassembler_.buffer_, psk));
    const size_t scratch_capacity = h.session.scratch_.capacity();
    const size_t buffer_capacity = h.session.reassembler_.buffer_.capacity();
    const Bytes longer = h.fragment(0, Bytes(4 * scratch_capacity, 'a'));
    {
        const FreedBlockScan scan(psk);
        h.send(longer);
        EXPECT_EQ(scan.holding(), 0);
    }
    EXPECT_FALSE(h.session.end());
    EXPECT_GT(h.session.scratch_.capacity(), scratch_capacity);
    EXPECT_GT(h.session.reassembler_.buffer_.capacity(), buffer_capacity);
}

// A session destroyed while a pairing message's first fragment is pending, with no end before,
// frees neither buffer holding the PSK. The scan's Control is SecureReserve.WipesTheBlockItLeaves.
TEST_P(SessionTest, WipesAPendingPairingFragmentWhenDestroyed) {
    using sendspin::server::FRAGMENT_FLAG_FIRST;
    if (!FreedBlockScan::available()) {
        GTEST_SKIP() << "needs the sanitizer's free hook";
    }
    const std::string psk = "QUJDREVGR0hJSktMTU5PUFFSU1RVVldYWVphYmNkZWY";
    std::optional<Harness> h;
    h.emplace(GetParam());
    ASSERT_NO_FATAL_FAILURE(h->activate());
    Bytes first{sendspin::server::MESSAGE_ID_JSON};
    const Bytes head =
        bytes_of(R"({"type":"client/pair-finalize","payload":{"long_term_psk":")" + psk);
    first.insert(first.end(), head.begin(), head.end());
    h->send(h->fragment(FRAGMENT_FLAG_FIRST, first));
    ASSERT_TRUE(holds_to_capacity(h->session.scratch_, psk));
    ASSERT_TRUE(holds_to_capacity(h->session.reassembler_.buffer_, psk));
    ASSERT_FALSE(h->session.end());
    const FreedBlockScan scan(psk);
    h.reset();
    EXPECT_EQ(scan.holding(), 0);
}

// A wipe reaches the reassembly buffer's storage past a shorter message, where a longer one
// written since the last wipe left its bytes. Before the wipe the scan finds them: the Control.
TEST(Reassembler, WipesTheStoragePastTheMessage) {
    using sendspin::server::FRAGMENT_FLAG_FIRST;
    using sendspin::server::FRAGMENT_FLAG_LAST;
    const std::string secret = "QUJDREVGR0hJSktMTU5PUFFSU1RVVldYWVphYmNkZWY";
    Reassembler reassembler(sendspin::server::DEFAULT_MAX_REASSEMBLED_BYTES,
                            [](uint8_t) { return true; });
    const auto whole = [](const std::string& text) {
        Bytes plaintext{sendspin::server::MESSAGE_ID_FRAGMENT,
                        FRAGMENT_FLAG_FIRST | FRAGMENT_FLAG_LAST,
                        sendspin::server::MESSAGE_ID_JSON};
        plaintext.insert(plaintext.end(), text.begin(), text.end());
        return plaintext;
    };
    ASSERT_EQ(reassembler.accept(whole("{\"longer\":\"" + secret + "\"}")),
              Reassembler::Outcome::MESSAGE);
    ASSERT_EQ(reassembler.accept(whole("{}")), Reassembler::Outcome::MESSAGE);
    EXPECT_TRUE(holds_to_capacity(reassembler.buffer_, secret));
    reassembler.wipe();
    EXPECT_FALSE(holds_to_capacity(reassembler.buffer_, secret));
}

// A session that ends between message 2 and the frozen activation's write, here on a client/goodbye
// under the new keys, writes nothing more
TEST_P(SessionTest, EndsBeforeTheFrozenActivationWithNothingMoreWritten) {
    using sendspin::server::HandshakePsk;
    using sendspin::server::PskCategory;
    Harness h(GetParam());
    ASSERT_NO_FATAL_FAILURE(h.activate());
    h.session.rekey(HandshakePsk{PskCategory::LONG_TERM, psk_of(4)},
                    ServerActivate{true, std::nullopt, std::nullopt});
    const std::vector<OutboundMessage> sent = h.take_all();
    ASSERT_EQ(sent.size(), 1U);
    h.client.psk = psk_of(4);
    ASSERT_TRUE(h.client.read_rekey_message_1(h.decrypt_json(sent[0])));
    h.send(h.client.rekey_message_2());
    h.send(h.client.encrypt_json(R"({"type":"client/goodbye","payload":{"reason":"shutdown"}})"));
    EXPECT_EQ(h.session.end(), SessionEnd::GOODBYE);
    EXPECT_FALSE(h.session.has_message());
    EXPECT_TRUE(h.take_all().empty());
}

// connection.md "Sentinel Fallback" applies to the initial handshake only: a re-handshake's
// message 2 under any PSK but the referenced one, the Sentinel included, fails silently, as do a
// malformed message 2 and one that does not carry `{}`; so does a re-handshake whose message 2
// never comes
TEST_P(SessionTest, FailsARekeySilently) {
    using sendspin::server::HandshakePsk;
    using sendspin::server::PskCategory;
    struct Row {
        const char* name;
        sendspin::server::Psk client_psk;
        std::function<Bytes(NoiseResponder&)> message_2;
        bool fails;
    };
    const auto valid = [](NoiseResponder& client) { return client.rekey_message_2(); };
    const Row rows[] = {
        {"Control: under the referenced PSK", psk_of(4), valid, false},
        {"under the Sentinel", sendspin::server::SENTINEL_PSK, valid, true},
        {"under another PSK", psk_of(9), valid, true},
        {"a payload other than {}", psk_of(4),
         [](NoiseResponder& client) { return client.rekey_message_2("{ }"); }, true},
        {"no data", psk_of(4),
         [](NoiseResponder& client) {
             return client.encrypt_json(R"({"type":"noise/handshake","payload":{}})");
         },
         true},
    };
    for (const Row& row : rows) {
        SCOPED_TRACE(row.name);
        Harness h(GetParam());
        ASSERT_NO_FATAL_FAILURE(h.activate());
        h.session.rekey(HandshakePsk{PskCategory::LONG_TERM, psk_of(4)},
                        ServerActivate{false, std::nullopt, std::nullopt});
        const std::vector<OutboundMessage> sent = h.take_all();
        ASSERT_EQ(sent.size(), 1U);
        // Message 1 does not depend on the PSK, so the client reads it under its own
        h.client.psk = row.client_psk;
        ASSERT_TRUE(h.client.read_rekey_message_1(h.decrypt_json(sent[0])));
        h.send(row.message_2(h.client));
        EXPECT_EQ(h.session.end(),
                  row.fails ? std::optional(SessionEnd::HANDSHAKE_FAILED) : std::nullopt);
        EXPECT_EQ(h.take_all().size(), row.fails ? 0U : 1U);
    }

    // connection.md "Failure Handling": the step timeout bounds the wait for message 2
    Harness h(GetParam());
    ASSERT_NO_FATAL_FAILURE(h.activate());
    EXPECT_FALSE(h.session.deadline_us());
    h.session.rekey(HandshakePsk{}, ServerActivate{false, std::nullopt, std::nullopt});
    ASSERT_EQ(h.take_all().size(), 1U);
    const std::optional<int64_t> deadline = h.session.deadline_us();
    ASSERT_TRUE(deadline);
    h.session.on_deadline(*deadline - 1);
    EXPECT_FALSE(h.session.end());
    h.session.on_deadline(*deadline);
    EXPECT_EQ(h.session.end(), SessionEnd::TIMED_OUT);
}

// ==============================================================================
// The source role
// ==============================================================================

SessionSettings source_settings() {
    SessionSettings settings = test_settings();
    settings.hello.source_codecs = {sendspin::server::Codec::PCM, sendspin::server::Codec::FLAC};
    return settings;
}

/// A client/state as the driver takes it in: the marker it calls for follows
void send_state(Harness& h, const std::string& payload) {
    h.send(h.client.encrypt_json(R"({"type":"client/state","payload":)" + payload + "}"));
    if (h.session.take_state_changed()) {
        h.session.queue_state_marker();
    }
}

/// Through an activation with the source role and the client/state source object that follows it
void activate_source(Harness& h) {
    ASSERT_NO_FATAL_FAILURE(h.complete_handshake());
    h.send(h.client.encrypt_json(sendspin::server::test::source_hello_json("Turntable")));
    h.session.activate(ServerActivate{true, std::vector<std::string>{"source@v1"}, std::nullopt});
    const std::vector<OutboundMessage> sent = h.take_all();
    ASSERT_EQ(sent.size(), 1U);
    // Read, so the client's nonce follows the server's
    EXPECT_NE(h.decrypt_json(sent[0]).find("server/activate"), std::string::npos);
    send_state(h, R"({"available":true,"source":{}})");
    EXPECT_TRUE(h.take_all().empty()) << "no start for a source not routed";
}

/// A source audio message, encrypted: the ID, the big-endian capture time, then audio
Bytes source_chunk(Harness& h, int64_t capture_us, const Bytes& audio) {
    Bytes plaintext{sendspin::server::MESSAGE_ID_SOURCE_AUDIO};
    for (int shift = 56; shift >= 0; shift -= 8) {
        plaintext.push_back(static_cast<uint8_t>(static_cast<uint64_t>(capture_us) >> shift));
    }
    plaintext.insert(plaintext.end(), audio.begin(), audio.end());
    return h.client.encrypt(plaintext);
}

constexpr const char* PCM_START =
    R"({"type":"client-stream/start","payload":{"source":{"codec":"pcm","channels":2,)"
    R"("sample_rate":48000,"bit_depth":16}}})";

/// The items a feed holds, oldest first, with their bytes
std::vector<std::pair<sendspin::server::SourceItem, Bytes>> drain(
    sendspin::server::SourceFeed& feed) {
    std::vector<std::pair<sendspin::server::SourceItem, Bytes>> items;
    sendspin::server::SourceItem item;
    std::span<const uint8_t> bytes;
    while (feed.front(item, bytes)) {
        items.emplace_back(item, Bytes(bytes.begin(), bytes.end()));
        feed.pop();
    }
    return items;
}

// roles/source/v1.md "Source command semantics": a routed source is sent start once its role is
// active and its client/state is in, and stop once unrouted; its input stream's start, chunks
// (whole or fragmented, with their capture times and receive stamps) and end go to the route's
// feed in order, named by the route's connection and the stream; after the stop its chunks are
// discarded
TEST_P(SessionTest, StartsARoutedSourceAndFeedsItsStream) {
    using sendspin::server::SourceCommand;
    using sendspin::server::SourceItem;
    Harness h(GetParam(), source_settings());
    ASSERT_NO_FATAL_FAILURE(activate_source(h));
    sendspin::server::SourceFeed feed(4096, 16);
    h.session.set_source_route(&feed, 7);
    std::vector<OutboundMessage> sent = h.take_all();
    ASSERT_EQ(sent.size(), 1U);
    EXPECT_EQ(h.decrypt_json(sent[0]),
              sendspin::server::build_source_command(SourceCommand::START));

    h.send(h.client.encrypt_json(PCM_START), 10);
    h.send(source_chunk(h, 123456789, {1, 2, 3, 4}), 20);
    // The ID, then the first seven bytes of the capture time; the last byte and the audio follow
    const Bytes first{sendspin::server::MESSAGE_ID_SOURCE_AUDIO, 0, 0, 0, 0, 0, 0, 0};
    h.send(h.fragment(sendspin::server::FRAGMENT_FLAG_FIRST, first), 30);
    h.send(h.fragment(sendspin::server::FRAGMENT_FLAG_LAST, {9, 5, 6, 7, 8}), 40);
    h.send(h.client.encrypt_json(R"({"type":"client-stream/end","payload":{}})"), 50);
    EXPECT_EQ(h.session.phase(), SessionPhase::ACTIVE);

    const auto items = drain(feed);
    ASSERT_EQ(items.size(), 4U);
    EXPECT_EQ(items[0].first.kind, SourceItem::Kind::START);
    EXPECT_EQ(items[0].first.connection, 7U);
    EXPECT_EQ(items[0].first.stream, 1U);
    EXPECT_EQ(items[0].first.format.sample_rate, 48000U);
    EXPECT_EQ(items[1].first.kind, SourceItem::Kind::CHUNK);
    EXPECT_EQ(items[1].first.capture_us, 123456789);
    EXPECT_EQ(items[1].first.rx_us, 20);
    EXPECT_EQ(items[1].second, (Bytes{1, 2, 3, 4}));
    EXPECT_EQ(items[2].first.capture_us, 9);
    EXPECT_EQ(items[2].first.rx_us, 40) << "the last fragment's stamp";
    EXPECT_EQ(items[2].second, (Bytes{5, 6, 7, 8}));
    EXPECT_EQ(items[3].first.kind, SourceItem::Kind::END);

    // The source ended the stream its start opened: stopped, and not asked again
    sent = h.take_all();
    ASSERT_EQ(sent.size(), 1U);
    EXPECT_EQ(h.decrypt_json(sent[0]), sendspin::server::build_source_command(SourceCommand::STOP));

    // Routed again: started again; unrouted: stopped, and the chunks in flight discarded
    h.session.set_source_route(nullptr, 7);
    h.session.set_source_route(&feed, 7);
    sent = h.take_all();
    ASSERT_EQ(sent.size(), 1U);
    EXPECT_EQ(h.decrypt_json(sent[0]),
              sendspin::server::build_source_command(SourceCommand::START));
    h.send(h.client.encrypt_json(PCM_START));
    h.session.set_source_route(nullptr, 7);
    sent = h.take_all();
    ASSERT_EQ(sent.size(), 1U);
    EXPECT_EQ(h.decrypt_json(sent[0]), sendspin::server::build_source_command(SourceCommand::STOP));
    h.send(source_chunk(h, 1, {1, 2, 3, 4}));
    EXPECT_EQ(drain(feed).size(), 1U) << "the second stream's start alone";
    EXPECT_EQ(h.session.phase(), SessionPhase::ACTIVE);
}

// roles/source/v1.md "Default streaming behavior" and "Source Audio Chunks (Binary)": the
// connection closes on an unsolicited client-stream/start, a codec server/hello does not list, a
// chunk with no stream open or while the client is unavailable, a chunk with no audio, and a
// malformed source object while the role is active
TEST_P(SessionTest, ClosesOnSourceProtocolErrors) {
    struct Row {
        const char* name;
        bool start_sent;
        std::function<void(Harness&)> act;
        bool closes;
    };
    const auto json = [](const std::string& text) {
        return [text](Harness& h) { h.send(h.client.encrypt_json(text)); };
    };
    const auto opened = [](const std::function<void(Harness&)>& then) {
        return [then](Harness& h) {
            h.send(h.client.encrypt_json(PCM_START));
            then(h);
        };
    };
    const Row rows[] = {
        {"Control: a stream answering a start", true, json(PCM_START), false},
        {"Control: an end with no stream open", true,
         json(R"({"type":"client-stream/end","payload":{}})"), false},
        {"Control: a chunk of an open stream", true,
         opened([](Harness& h) { h.send(source_chunk(h, 1, {0, 0, 0, 0})); }), false},
        {"an unsolicited start", false, json(PCM_START), true},
        {"a codec not listed", true,
         json(R"({"type":"client-stream/start","payload":{"source":{"codec":"opus",)"
              R"("channels":2,"sample_rate":48000}}})"),
         true},
        {"a malformed start", true,
         json(R"({"type":"client-stream/start","payload":{"source":{"codec":"pcm"}}})"), true},
        {"a chunk with no stream open", true,
         [](Harness& h) { h.send(source_chunk(h, 1, {0, 0, 0, 0})); }, true},
        {"Control: a chunk with no audio, PCM's whole frames", true,
         opened([](Harness& h) { h.send(source_chunk(h, 1, {})); }), false},
        {"a capture time past the bound", true, opened([](Harness& h) {
             h.send(source_chunk(h, sendspin::server::MAX_SOURCE_TIMESTAMP_US + 1, {0, 0, 0, 0}));
         }),
         true},
        {"a chunk while unavailable", true, opened([](Harness& h) {
             send_state(h, R"({"available":false,"source":{}})");
             h.send(source_chunk(h, 1, {0, 0, 0, 0}));
         }),
         true},
        {"a malformed source object", true,
         [](Harness& h) { send_state(h, R"({"available":true,"source":7})"); }, true},
    };
    for (const Row& row : rows) {
        SCOPED_TRACE(row.name);
        Harness h(GetParam(), source_settings());
        ASSERT_NO_FATAL_FAILURE(activate_source(h));
        sendspin::server::SourceFeed feed(4096, 16);
        if (row.start_sent) {
            h.session.set_source_route(&feed, 1);
            ASSERT_EQ(h.take_all().size(), 1U);
        }
        row.act(h);
        EXPECT_EQ(h.session.end(),
                  row.closes ? std::optional(SessionEnd::PROTOCOL_ERROR) : std::nullopt);
    }
}

// A stream the server cannot take (8-bit PCM here) is stopped, reported as failed and never as
// opened, and its chunks discarded
TEST_P(SessionTest, StopsASourceStreamItCannotTake) {
    using sendspin::server::SourceEvent;
    Harness h(GetParam(), source_settings());
    ASSERT_NO_FATAL_FAILURE(activate_source(h));
    sendspin::server::SourceFeed feed(4096, 16);
    h.session.set_source_route(&feed, 1);
    const std::vector<OutboundMessage> start = h.take_all();
    ASSERT_EQ(start.size(), 1U);
    EXPECT_NE(h.decrypt_json(start[0]).find("start"), std::string::npos);
    (void)h.session.take_source_events();
    h.send(h.client.encrypt_json(
        R"({"type":"client-stream/start","payload":{"source":{"codec":"pcm","channels":2,)"
        R"("sample_rate":48000,"bit_depth":8}}})"));
    h.send(source_chunk(h, 1, {0, 0}));
    const std::vector<OutboundMessage> sent = h.take_all();
    ASSERT_EQ(sent.size(), 1U);
    EXPECT_EQ(h.decrypt_json(sent[0]),
              sendspin::server::build_source_command(sendspin::server::SourceCommand::STOP));
    EXPECT_TRUE(drain(feed).empty());
    const std::vector<SourceEvent> events = h.session.take_source_events();
    ASSERT_EQ(events.size(), 1U);
    EXPECT_EQ(events[0].kind, SourceEvent::Kind::FAILED);
    EXPECT_EQ(events[0].format.bit_depth, 8U);
    EXPECT_FALSE(h.session.end());
}

INSTANTIATE_TEST_SUITE_P(Suites, SessionTest,
                         testing::Values(CipherSuite::CHACHAPOLY_SHA256,
                                         CipherSuite::AESGCM_SHA256),
                         [](const testing::TestParamInfo<CipherSuite>& info) {
                             return info.param == CipherSuite::AESGCM_SHA256
                                        ? std::string("AESGCM")
                                        : std::string("ChaChaPoly");
                         });

}  // namespace
