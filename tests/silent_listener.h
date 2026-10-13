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

/// @file silent_listener.h
/// @brief A loopback listener for the server's dials that answers only when the test says so, to
/// hold dials in their upgrade or their handshake

#pragma once

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/websocket.hpp>

#include <cstdint>
#include <memory>
#include <string>

namespace sendspin::server::test {

/// @brief Accepts the server's dials and never answers them on its own, so a dial stays in its
/// WebSocket upgrade until the test completes or closes it
class SilentListener {
public:
    using Socket = boost::asio::ip::tcp::socket;
    using WebSocket = boost::beast::websocket::stream<Socket>;

    SilentListener() : acceptor_(io_, {boost::asio::ip::make_address("127.0.0.1"), 0}) {}

    std::string url(const std::string& path) const {
        return "ws://127.0.0.1:" + std::to_string(this->port()) + path;
    }

    uint16_t port() const {
        return this->acceptor_.local_endpoint().port();
    }

    /// @brief Waits with no timeout for the next dial
    std::unique_ptr<Socket> accept() {
        auto socket = std::make_unique<Socket>(this->io_);
        this->acceptor_.accept(*socket);
        return socket;
    }

    /// @brief Whether a dial is waiting to be accepted, without waiting for one
    bool dial_pending() {
        Socket socket(this->io_);
        boost::system::error_code ec;
        this->acceptor_.non_blocking(true);
        this->acceptor_.accept(socket, ec);
        this->acceptor_.non_blocking(false);
        return !ec;
    }

private:
    boost::asio::io_context io_;
    boost::asio::ip::tcp::acceptor acceptor_;
};

/// @brief The upgrade request a dial sent on socket
inline boost::beast::http::request<boost::beast::http::empty_body> upgrade_request(
    SilentListener::Socket& socket) {
    boost::beast::flat_buffer buffer;
    boost::beast::http::request<boost::beast::http::empty_body> request;
    boost::beast::http::read(socket, buffer, request);
    return request;
}

/// @brief Completes the upgrade of a dial whose request was read, so the dial succeeds and its
/// connection waits in the handshake for the client's client/init
inline std::unique_ptr<SilentListener::WebSocket> complete_upgrade(
    std::unique_ptr<SilentListener::Socket> socket,
    const boost::beast::http::request<boost::beast::http::empty_body>& request) {
    auto ws = std::make_unique<SilentListener::WebSocket>(std::move(*socket));
    ws->accept(request);
    return ws;
}

}  // namespace sendspin::server::test
