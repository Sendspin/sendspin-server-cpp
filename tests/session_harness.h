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

/// @file session_harness.h
/// @brief A Session and the raw test client's NoiseResponder driving it in-process, and the
/// fixture its tests run under for each cipher suite

#pragma once

#include "fragmentation.h"
#include "messages.h"
#include "raw_client.h"
#include "session.h"
#include "wire_messages.h"
#include <gtest/gtest.h>
#include <noise/protocol.h>

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace sendspin::server::test {

/// The session's start, and the receive stamp a message gets by default
inline constexpr int64_t START_US = 1000;
/// Each handshake step's timeout
inline constexpr int64_t TIMEOUT_US = 5000;

/// The settings a Harness runs with by default
inline SessionSettings test_settings() {
    SessionSettings settings;
    settings.hello.name = "Test server";
    settings.step_timeout_us = TIMEOUT_US;
    return settings;
}

/// A session and its client. Not movable: the session refers to the identity and settings.
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

/// Each test runs under both cipher suites
class SessionTest : public testing::TestWithParam<CipherSuite> {
protected:
    static void SetUpTestSuite() {
        ASSERT_EQ(noise_init_framework(), NOISE_ERROR_NONE);
    }
};

}  // namespace sendspin::server::test
