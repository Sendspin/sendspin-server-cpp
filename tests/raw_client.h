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

/// @file raw_client.h
/// @brief The raw test client: a Sendspin client written on raw noise-c (NoiseResponder) and,
/// for the server to dial, on raw Beast (RawClient). It follows the protocol or breaks it one
/// step at a time, as a test scripts it.

#pragma once

#include "crypto/keys.h"
#include "noise.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

struct NoiseHandshakeState_s;
struct NoiseCipherState_s;

namespace sendspin::server::test {

using Bytes = std::vector<uint8_t>;

Bytes bytes_of(std::string_view text);
std::string text_of(const Bytes& bytes);

/// @brief A client/hello the server accepts
std::string client_hello_json(const std::string& name = "Kitchen");

/// @brief A client/hello listing player@v1 with these `supported_formats` entries (a JSON array)
/// and buffer_capacity, unpaired access enabled
std::string player_hello_json(const std::string& name, const std::string& formats,
                              uint32_t buffer_capacity);

/// @brief A client/hello listing source@v1 and its support object, unpaired access enabled
std::string source_hello_json(const std::string& name);

/// @brief A client/state; with player_json, the player object it carries
std::string client_state_json(bool available, const std::string& player_json = "");

/// @brief A client/state player object with these timing values and no commands
std::string player_state_json(uint32_t min_buffer_ms, uint32_t required_lead_time_ms,
                              uint32_t output_delay_ms);

std::string client_time_json(int64_t client_transmitted);

std::string client_goodbye_json(std::string_view reason);

/// @brief The client side of the Noise handshake and transport ciphers on raw noise-c, as the
/// KKpsk2 responder. Reports a noise-c call that should not fail as a test failure.
class NoiseResponder {
public:
    explicit NoiseResponder(CipherSuite suite);
    NoiseResponder(CipherSuite suite, Identity identity);
    ~NoiseResponder();
    NoiseResponder(const NoiseResponder&) = delete;
    NoiseResponder& operator=(const NoiseResponder&) = delete;

    /// @brief A valid client/init declaring this suite, with spacing after the type member
    std::string client_init(const std::string& spacing = "") const;

    /// @brief Reads Noise message 1 from noise_message under prologue, with the server key from
    /// server_init
    /// @return false when message 1 does not authenticate
    bool read_message_1(const Bytes& prologue, const std::string& server_init,
                        const std::string& noise_message);

    /// @brief The noise/handshake text carrying Noise message 2 with payload, after which the
    /// responder is in transport mode
    std::string message_2(const std::string& payload = "{}");

    /// @brief Reads a re-handshake's Noise message 1 from the noise/handshake JSON it travels in,
    /// under the prologue `h` of the last handshake (connection.md "Re-handshake")
    /// @return false when message 1 does not authenticate
    bool read_rekey_message_1(const std::string& noise_handshake);

    /// @brief The re-handshake's message 2 with payload, as a noise/handshake JSON message
    /// encrypted under the current keys, after which the new keys are in place
    Bytes rekey_message_2(const std::string& payload = "{}");

    /// @brief The handshake hash `h` of the last handshake
    const HandshakeHash& handshake_hash() const {
        return this->handshake_hash_;
    }

    Bytes encrypt(Bytes plaintext);

    /// @brief A JSON message, ID byte first, encrypted
    Bytes encrypt_json(const std::string& json);

    /// @return nullopt when the message does not authenticate
    std::optional<Bytes> decrypt(Bytes message);

    const Identity& identity() const {
        return this->identity_;
    }

    CipherSuite suite() const {
        return this->suite_;
    }

    /// The PSK message 2 is written under
    Psk psk{SENTINEL_PSK};
    /// Message 1's payload, once read
    std::string message_1_payload;

private:
    struct Deleter {
        void operator()(NoiseHandshakeState_s* state) const;
        void operator()(NoiseCipherState_s* state) const;
    };

    /// Starts a responder handshake under prologue and reads message 1 from noise_message
    bool read_message_1_bytes(const Bytes& prologue, const Bytes& noise_message);
    /// Writes message 2 and splits into new ciphers, which it returns
    Bytes write_message_2(const std::string& payload);

    CipherSuite suite_;
    Identity identity_;
    PublicKey server_key_{};
    HandshakeHash handshake_hash_{};
    std::unique_ptr<NoiseHandshakeState_s, Deleter> handshake_;
    std::unique_ptr<NoiseCipherState_s, Deleter> send_;
    std::unique_ptr<NoiseCipherState_s, Deleter> receive_;
};

/// @brief One WebSocket message the server sent
struct ReceivedMessage {
    bool text{false};
    Bytes bytes;
};

/// @brief A Sendspin client that listens on a loopback port for the server to dial, or dials the
/// server's listener itself. Every method runs one blocking step on the calling thread; a read
/// waits with no timeout, so a regression hangs into the suite watchdog. The script steps report a
/// deviation by the server as a test failure.
class RawClient {
public:
    explicit RawClient(CipherSuite suite = CipherSuite::CHACHAPOLY_SHA256);
    /// @brief A client with identity's static key, as a second connection of the same client
    RawClient(CipherSuite suite, Identity identity);
    ~RawClient();
    RawClient(const RawClient&) = delete;
    RawClient& operator=(const RawClient&) = delete;

    /// @brief The URL the server dials to reach this client
    std::string url(const std::string& path = "/sendspin") const;

    /// @brief The loopback port the server dials
    uint16_t port() const;

    // ------------------------------------------------------------------------------
    // Transport
    // ------------------------------------------------------------------------------

    /// @brief Accepts the next dial and its WebSocket upgrade, dropping any earlier connection
    /// @return the request path of the upgrade
    std::string accept();

    /// @brief Whether a dial was waiting to be accepted, without waiting for one; a waiting dial
    /// is accepted and dropped
    bool dial_pending();

    /// @brief Connects to the server on the IPv6 loopback at port and requests path, dropping any
    /// earlier connection, as a client that connects to the server does
    /// @return the HTTP status of the server's response: 101 when it accepted the upgrade
    unsigned dial(uint16_t port, const std::string& path = "/sendspin");

    void send_text(std::string_view text);
    void send_binary(const Bytes& bytes);

    /// @brief The next message, or nullopt once the connection closed (closed_by_server() says
    /// how)
    std::optional<ReceivedMessage> receive();

    /// @brief Whether the server's close frame ended the connection
    bool closed_by_server() const {
        return this->closed_by_server_;
    }

    /// @brief Closes with a WebSocket close frame and waits for the server's
    void close();

    /// @brief Closes the TCP connection with no close frame; unread data makes it a reset
    void drop();

    /// @brief Resets the TCP connection (SO_LINGER with a zero timeout), whatever is unread
    void reset();

    /// @brief Ends the TCP connection's sending direction (a FIN), still able to read
    void half_close();

    /// @brief Sets the receive buffer (SO_RCVBUF) of the connections accepted from now on, which
    /// also stops the system from growing it, so that a server writing while the client does not
    /// read soon has no room
    void fix_receive_buffer(int bytes);

    // ------------------------------------------------------------------------------
    // Protocol steps
    // ------------------------------------------------------------------------------

    /// @brief Sends a valid client/init
    void send_client_init();

    /// @brief Receives server/init and noise/handshake and reads Noise message 1 under the
    /// spec's prologue
    void receive_message_1();

    /// @brief Sends Noise message 2 with payload; the client is in transport mode after it
    void send_message_2(const std::string& payload = "{}");

    /// @brief Encrypts and sends a JSON message
    void send_json(const std::string& json);

    /// @brief Encrypts and sends a fragment of a message (messaging.md "Fragmentation")
    void send_fragment(uint8_t flags, const Bytes& data);

    /// @brief The JSON of the next message, which must be an encrypted JSON message; empty when
    /// it is not, or the connection closed
    std::string receive_json();

    /// @brief The plaintext of the next message, message ID first, which must be encrypted;
    /// nullopt once the connection closed
    std::optional<Bytes> receive_plaintext();

    /// @brief The JSON of the next encrypted JSON message of type, skipping every other message;
    /// empty once the connection closed
    std::string receive_until(std::string_view type);

    /// @brief Expects the connection to close with no further message
    void expect_silent_close();

    /// @brief Reads and discards every message until the connection closes
    void read_until_closed();

    /// @brief client/init through server/hello on the accepted connection
    void complete_handshake();

    /// @brief accept(), then complete_handshake()
    void handshake();

    /// @brief handshake(), then client/hello through the initial server/activate and the
    /// group/update after it
    void activate(const std::string& hello = client_hello_json());

    NoiseResponder noise;
    std::string client_init;
    std::string server_init;
    std::string noise_message_1;
    std::string server_hello;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
    bool closed_by_server_{false};
};

}  // namespace sendspin::server::test
