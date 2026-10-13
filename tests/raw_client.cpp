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

#include "raw_client.h"

#include "fragmentation.h"
#include "messages.h"
#include <ArduinoJson.h>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/websocket.hpp>
#include <gtest/gtest.h>
#include <noise/protocol.h>

#include <utility>

namespace sendspin::server::test {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
using tcp = asio::ip::tcp;

Bytes bytes_of(std::string_view text) {
    return {text.begin(), text.end()};
}

std::string text_of(const Bytes& bytes) {
    return {bytes.begin(), bytes.end()};
}

std::string client_hello_json(const std::string& name) {
    return R"({"type":"client/hello","payload":{"name":")" + name +
           R"(","supported_roles":["metadata@v1"],"supported_pair_methods":{"pairing_psk":{}},)"
           R"("unpaired_access":{"enabled":true}}})";
}

std::string player_hello_json(const std::string& name, const std::string& formats,
                              uint32_t buffer_capacity) {
    return R"({"type":"client/hello","payload":{"name":")" + name +
           R"(","supported_roles":["player@v1"],"player@v1_support":{"supported_formats":)" +
           formats + R"(,"buffer_capacity":)" + std::to_string(buffer_capacity) +
           R"(},"supported_pair_methods":{"pairing_psk":{}},"unpaired_access":{"enabled":true}}})";
}

std::string source_hello_json(const std::string& name) {
    return R"({"type":"client/hello","payload":{"name":")" + name +
           R"(","supported_roles":["source@v1"],"source@v1_support":{},)"
           R"("supported_pair_methods":{"pairing_psk":{}},"unpaired_access":{"enabled":true}}})";
}

std::string client_state_json(bool available, const std::string& player_json) {
    std::string json = R"({"type":"client/state","payload":{"available":)" +
                       std::string(available ? "true" : "false");
    if (!player_json.empty()) {
        json += R"(,"player":)" + player_json;
    }
    return json + "}}";
}

std::string player_state_json(uint32_t min_buffer_ms, uint32_t required_lead_time_ms,
                              uint32_t output_delay_ms) {
    return R"({"output_delay_ms":)" + std::to_string(output_delay_ms) +
           R"(,"required_lead_time_ms":)" + std::to_string(required_lead_time_ms) +
           R"(,"min_buffer_ms":)" + std::to_string(min_buffer_ms) + R"(,"supported_commands":[]})";
}

std::string client_time_json(int64_t client_transmitted) {
    return R"({"type":"client/time","payload":{"client_transmitted":)" +
           std::to_string(client_transmitted) + "}}";
}

std::string client_goodbye_json(std::string_view reason) {
    return R"({"type":"client/goodbye","payload":{"reason":")" + std::string(reason) + R"("}})";
}

// ==============================================================================
// NoiseResponder
// ==============================================================================

void NoiseResponder::Deleter::operator()(NoiseHandshakeState_s* state) const {
    noise_handshakestate_free(state);
}

void NoiseResponder::Deleter::operator()(NoiseCipherState_s* state) const {
    noise_cipherstate_free(state);
}

NoiseResponder::NoiseResponder(CipherSuite suite) : NoiseResponder(suite, *Identity::generate()) {}

NoiseResponder::NoiseResponder(CipherSuite suite, Identity identity)
    : suite_(suite), identity_(std::move(identity)) {}

NoiseResponder::~NoiseResponder() = default;

std::string NoiseResponder::client_init(const std::string& spacing) const {
    return R"({"type":"client/init",)" + spacing + R"("payload":{"client_id":")" +
           this->identity_.peer_id() + R"(","version":1,"suite":")" +
           std::string(to_wire(this->suite_)) + R"("}})";
}

bool NoiseResponder::read_message_1(const Bytes& prologue, const std::string& server_init,
                                    const std::string& noise_message) {
    JsonDocument init;
    EXPECT_EQ(deserializeJson(init, server_init), DeserializationError::Ok);
    const std::optional<PublicKey> server_key =
        public_key_from_peer_id(init["payload"]["server_id"].as<std::string>());
    const std::optional<Bytes> message = parse_noise_handshake(noise_message);
    if (!server_key || !message) {
        ADD_FAILURE() << "no server key or no Noise message 1";
        return false;
    }
    this->server_key_ = *server_key;
    return this->read_message_1_bytes(prologue, *message);
}

bool NoiseResponder::read_rekey_message_1(const std::string& noise_handshake) {
    const std::optional<Bytes> message = parse_noise_handshake(noise_handshake);
    if (!message) {
        ADD_FAILURE() << "no Noise message 1 in " << noise_handshake;
        return false;
    }
    return this->read_message_1_bytes(
        Bytes(this->handshake_hash_.begin(), this->handshake_hash_.end()), *message);
}

bool NoiseResponder::read_message_1_bytes(const Bytes& prologue, const Bytes& noise_message) {
    NoiseHandshakeState* raw = nullptr;
    const std::string protocol = "Noise_KKpsk2_" + std::string(to_wire(this->suite_));
    EXPECT_EQ(noise_handshakestate_new_by_name(&raw, protocol.c_str(), NOISE_ROLE_RESPONDER),
              NOISE_ERROR_NONE);
    this->handshake_.reset(raw);
    EXPECT_EQ(
        noise_dhstate_set_keypair_private(noise_handshakestate_get_local_keypair_dh(raw),
                                          this->identity_.private_bytes.data(), X25519_KEY_SIZE),
        NOISE_ERROR_NONE);
    EXPECT_EQ(noise_dhstate_set_public_key(noise_handshakestate_get_remote_public_key_dh(raw),
                                           this->server_key_.data(), X25519_KEY_SIZE),
              NOISE_ERROR_NONE);
    EXPECT_EQ(noise_handshakestate_set_prologue(raw, prologue.data(), prologue.size()),
              NOISE_ERROR_NONE);
    EXPECT_EQ(noise_handshakestate_set_pre_shared_key(raw, this->psk.data(), this->psk.size()),
              NOISE_ERROR_NONE);
    EXPECT_EQ(noise_handshakestate_start(raw), NOISE_ERROR_NONE);

    Bytes input = noise_message;
    Bytes payload(input.size());
    NoiseBuffer in;
    noise_buffer_set_input(in, input.data(), input.size());
    NoiseBuffer out;
    noise_buffer_set_output(out, payload.data(), payload.size());
    if (noise_handshakestate_read_message(raw, &in, &out) != NOISE_ERROR_NONE) {
        return false;
    }
    payload.resize(out.size);
    this->message_1_payload = text_of(payload);
    return true;
}

Bytes NoiseResponder::write_message_2(const std::string& payload) {
    Bytes payload_bytes = bytes_of(payload);
    // noise-c refuses a null buffer, even an empty one
    payload_bytes.reserve(1);
    Bytes message(256);
    NoiseBuffer in;
    noise_buffer_set_input(in, payload_bytes.data(), payload_bytes.size());
    NoiseBuffer out;
    noise_buffer_set_output(out, message.data(), message.size());
    EXPECT_EQ(noise_handshakestate_write_message(this->handshake_.get(), &out, &in),
              NOISE_ERROR_NONE);
    message.resize(out.size);
    EXPECT_EQ(
        noise_handshakestate_get_handshake_hash(
            this->handshake_.get(), this->handshake_hash_.data(), this->handshake_hash_.size()),
        NOISE_ERROR_NONE);
    return message;
}

std::string NoiseResponder::message_2(const std::string& payload) {
    const Bytes message = this->write_message_2(payload);
    NoiseCipherState* send = nullptr;
    NoiseCipherState* receive = nullptr;
    EXPECT_EQ(noise_handshakestate_split(this->handshake_.get(), &send, &receive),
              NOISE_ERROR_NONE);
    this->send_.reset(send);
    this->receive_.reset(receive);
    return build_noise_handshake(message);
}

Bytes NoiseResponder::rekey_message_2(const std::string& payload) {
    const Bytes message = this->write_message_2(payload);
    // messaging.md "noise/handshake": message 2 still travels under the keys before it
    Bytes encrypted = this->encrypt_json(build_noise_handshake(message));
    NoiseCipherState* send = nullptr;
    NoiseCipherState* receive = nullptr;
    EXPECT_EQ(noise_handshakestate_split(this->handshake_.get(), &send, &receive),
              NOISE_ERROR_NONE);
    this->send_.reset(send);
    this->receive_.reset(receive);
    return encrypted;
}

Bytes NoiseResponder::encrypt(Bytes plaintext) {
    constexpr size_t TAG_SIZE = 16;
    const size_t size = plaintext.size();
    plaintext.resize(size + TAG_SIZE);
    NoiseBuffer buffer;
    noise_buffer_set_inout(buffer, plaintext.data(), size, plaintext.size());
    EXPECT_EQ(noise_cipherstate_encrypt(this->send_.get(), &buffer), NOISE_ERROR_NONE);
    return plaintext;
}

Bytes NoiseResponder::encrypt_json(const std::string& json) {
    Bytes plaintext{MESSAGE_ID_JSON};
    plaintext.insert(plaintext.end(), json.begin(), json.end());
    return this->encrypt(std::move(plaintext));
}

std::optional<Bytes> NoiseResponder::decrypt(Bytes message) {
    NoiseBuffer buffer;
    noise_buffer_set_inout(buffer, message.data(), message.size(), message.size());
    if (noise_cipherstate_decrypt(this->receive_.get(), &buffer) != NOISE_ERROR_NONE) {
        return std::nullopt;
    }
    message.resize(buffer.size);
    return message;
}

// ==============================================================================
// RawClient
// ==============================================================================

class RawClient::Impl {
public:
    Impl() : acceptor(io, {asio::ip::make_address("127.0.0.1"), 0}) {}

    asio::io_context io;
    tcp::acceptor acceptor;
    std::optional<websocket::stream<tcp::socket>> ws;
};

RawClient::RawClient(CipherSuite suite) : noise(suite), impl_(std::make_unique<Impl>()) {}

RawClient::RawClient(CipherSuite suite, Identity identity)
    : noise(suite, std::move(identity)), impl_(std::make_unique<Impl>()) {}

RawClient::~RawClient() = default;

std::string RawClient::url(const std::string& path) const {
    return "ws://127.0.0.1:" + std::to_string(this->impl_->acceptor.local_endpoint().port()) + path;
}

uint16_t RawClient::port() const {
    return this->impl_->acceptor.local_endpoint().port();
}

std::string RawClient::accept() {
    this->impl_->ws.reset();
    this->closed_by_server_ = false;
    tcp::socket socket(this->impl_->io);
    this->impl_->acceptor.non_blocking(false);
    this->impl_->acceptor.accept(socket);
    beast::flat_buffer buffer;
    http::request<http::empty_body> request;
    http::read(socket, buffer, request);
    this->impl_->ws.emplace(std::move(socket));
    websocket::stream<tcp::socket>& ws = *this->impl_->ws;
    // One frame per message, as the spec's peers send them
    ws.auto_fragment(false);
    ws.accept(request);
    return std::string(request.target());
}

bool RawClient::dial_pending() {
    tcp::socket socket(this->impl_->io);
    beast::error_code ec;
    this->impl_->acceptor.non_blocking(true);
    this->impl_->acceptor.accept(socket, ec);
    this->impl_->acceptor.non_blocking(false);
    return !ec;
}

unsigned RawClient::dial(uint16_t port, const std::string& path) {
    this->impl_->ws.reset();
    this->closed_by_server_ = false;
    tcp::socket socket(this->impl_->io);
    // The server's listener is dual-stack on the IPv6 wildcard; on macOS another process may hold
    // the same port on 127.0.0.1 alone, so the IPv6 loopback reaches this server for certain
    socket.connect({asio::ip::make_address("::1"), port});
    this->impl_->ws.emplace(std::move(socket));
    websocket::stream<tcp::socket>& ws = *this->impl_->ws;
    // One frame per message, as the spec's peers send them
    ws.auto_fragment(false);
    websocket::response_type response;
    beast::error_code ec;
    ws.handshake(response, "[::1]:" + std::to_string(port), path, ec);
    if (ec) {
        this->impl_->ws.reset();
    }
    return response.result_int();
}

void RawClient::send_text(std::string_view text) {
    websocket::stream<tcp::socket>& ws = *this->impl_->ws;
    ws.text(true);
    ws.write(asio::buffer(text.data(), text.size()));
}

void RawClient::send_binary(const Bytes& bytes) {
    websocket::stream<tcp::socket>& ws = *this->impl_->ws;
    ws.binary(true);
    ws.write(asio::buffer(bytes));
}

std::optional<ReceivedMessage> RawClient::receive() {
    websocket::stream<tcp::socket>& ws = *this->impl_->ws;
    beast::flat_buffer buffer;
    beast::error_code ec;
    ws.read(buffer, ec);
    if (ec) {
        this->closed_by_server_ = ec == websocket::error::closed;
        return std::nullopt;
    }
    const auto data = buffer.cdata();
    const auto* begin = static_cast<const uint8_t*>(data.data());
    return ReceivedMessage{ws.got_text(), Bytes(begin, begin + data.size())};
}

void RawClient::close() {
    websocket::stream<tcp::socket>& ws = *this->impl_->ws;
    beast::error_code ec;
    ws.close(websocket::close_code::normal, ec);
    // The server's close frame ends the read
    while (!ec) {
        beast::flat_buffer buffer;
        ws.read(buffer, ec);
    }
}

void RawClient::drop() {
    beast::error_code ignored;
    this->impl_->ws->next_layer().close(ignored);
}

void RawClient::reset() {
    beast::error_code ignored;
    tcp::socket& socket = this->impl_->ws->next_layer();
    socket.set_option(asio::socket_base::linger(true, 0), ignored);
    socket.close(ignored);
}

void RawClient::half_close() {
    beast::error_code ignored;
    this->impl_->ws->next_layer().shutdown(tcp::socket::shutdown_send, ignored);
}

void RawClient::fix_receive_buffer(int bytes) {
    // Accepted sockets inherit it, with the window scale it calls for
    this->impl_->acceptor.set_option(asio::socket_base::receive_buffer_size(bytes));
}

void RawClient::send_client_init() {
    this->client_init = this->noise.client_init();
    this->send_text(this->client_init);
}

void RawClient::receive_message_1() {
    const std::optional<ReceivedMessage> init = this->receive();
    const std::optional<ReceivedMessage> message_1 = this->receive();
    ASSERT_TRUE(init && message_1);
    ASSERT_TRUE(init->text && message_1->text);
    this->server_init = text_of(init->bytes);
    this->noise_message_1 = text_of(message_1->bytes);
    ASSERT_TRUE(this->noise.read_message_1(bytes_of(this->client_init + this->server_init),
                                           this->server_init, this->noise_message_1));
}

void RawClient::send_message_2(const std::string& payload) {
    this->send_text(this->noise.message_2(payload));
}

void RawClient::send_json(const std::string& json) {
    this->send_binary(this->noise.encrypt_json(json));
}

void RawClient::send_fragment(uint8_t flags, const Bytes& data) {
    Bytes plaintext{MESSAGE_ID_FRAGMENT, flags};
    plaintext.insert(plaintext.end(), data.begin(), data.end());
    this->send_binary(this->noise.encrypt(std::move(plaintext)));
}

std::string RawClient::receive_json() {
    const std::optional<ReceivedMessage> message = this->receive();
    if (!message || message->text) {
        ADD_FAILURE() << "expected an encrypted message";
        return {};
    }
    const std::optional<Bytes> plaintext = this->noise.decrypt(message->bytes);
    if (!plaintext || plaintext->empty() || plaintext->front() != MESSAGE_ID_JSON) {
        ADD_FAILURE() << "not an encrypted JSON message";
        return {};
    }
    return {plaintext->begin() + 1, plaintext->end()};
}

std::optional<Bytes> RawClient::receive_plaintext() {
    const std::optional<ReceivedMessage> message = this->receive();
    if (!message) {
        return std::nullopt;
    }
    if (message->text) {
        ADD_FAILURE() << "expected an encrypted message";
        return Bytes{};
    }
    std::optional<Bytes> plaintext = this->noise.decrypt(message->bytes);
    if (!plaintext || plaintext->empty()) {
        ADD_FAILURE() << "a message that does not decrypt";
        return Bytes{};
    }
    return plaintext;
}

std::string RawClient::receive_until(std::string_view type) {
    const std::string marker = R"("type":")" + std::string(type) + R"(")";
    for (;;) {
        const std::optional<Bytes> plaintext = this->receive_plaintext();
        if (!plaintext) {
            return {};
        }
        if (plaintext->empty() || plaintext->front() != MESSAGE_ID_JSON) {
            continue;
        }
        std::string json(plaintext->begin() + 1, plaintext->end());
        if (json.find(marker) != std::string::npos) {
            return json;
        }
    }
}

void RawClient::expect_silent_close() {
    const std::optional<ReceivedMessage> message = this->receive();
    EXPECT_FALSE(message) << "unexpected message: "
                          << (message ? text_of(message->bytes) : std::string());
}

void RawClient::read_until_closed() {
    while (this->receive()) {}
}

void RawClient::complete_handshake() {
    this->send_client_init();
    ASSERT_NO_FATAL_FAILURE(this->receive_message_1());
    this->send_message_2();
    this->server_hello = this->receive_json();
    ASSERT_NE(this->server_hello.find(R"("type":"server/hello")"), std::string::npos)
        << this->server_hello;
}

void RawClient::handshake() {
    this->accept();
    this->complete_handshake();
}

void RawClient::activate(const std::string& hello) {
    ASSERT_NO_FATAL_FAILURE(this->handshake());
    this->send_json(hello);
    const std::string activation = this->receive_json();
    ASSERT_NE(activation.find(R"("type":"server/activate")"), std::string::npos) << activation;
    // messaging.md "group/update": promptly after the first activation
    const std::string update = this->receive_json();
    ASSERT_NE(update.find(R"("type":"group/update")"), std::string::npos) << update;
}

}  // namespace sendspin::server::test
