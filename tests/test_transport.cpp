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

/// @file test_transport.cpp
/// @brief The WebSocket transport over loopback against Beast peers on their own threads. The
/// server side runs on the test thread's io_context; a peer reports back by posting to it.

#include "clock.h"
#include "profile.h"
#include "test_ports.h"
#include "transport.h"
#include "transport_asio.h"
#include <arpa/inet.h>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/write.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/websocket.hpp>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/ioctl.h>
#if defined(__linux__)
#include <linux/sockios.h>
#endif
#include <sys/socket.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
using tcp = asio::ip::tcp;
using boost::system::error_code;

using sendspin::server::CloseReason;
using sendspin::server::Connection;
using sendspin::server::ConnectionHandler;
using sendspin::server::dial;
using sendspin::server::DialTarget;
using sendspin::server::Listener;
using sendspin::server::MessageKind;
using sendspin::server::now_us;
using sendspin::server::parse_ws_url;
using sendspin::server::PendingDial;
using sendspin::server::TransportSettings;
namespace close_code = sendspin::server::close_code;

using PeerSocket = websocket::stream<tcp::socket>;

const asio::ip::address LOOPBACK = asio::ip::make_address("127.0.0.1");

std::vector<uint8_t> bytes_of(std::string_view text) {
    return {text.begin(), text.end()};
}

std::vector<uint8_t> pattern(size_t size) {
    std::vector<uint8_t> out(size);
    for (size_t i = 0; i < size; ++i) {
        out[i] = static_cast<uint8_t>((i * 7) + (i >> 8));
    }
    return out;
}

std::vector<uint8_t> buffer_bytes(const beast::flat_buffer& buffer) {
    const auto data = buffer.cdata();
    const auto* begin = static_cast<const uint8_t*>(data.data());
    return {begin, begin + data.size()};
}

// A peer thread with its own synchronous Beast objects. Its result reaches the test thread
// through a handler posted to the server io_context, so done() is read on the test thread only.
class Peer {
public:
    Peer(asio::io_context& server_io, std::function<void()> body)
        : server_io_(server_io), thread_([this, body = std::move(body)] {
              std::string error;
              try {
                  body();
              } catch (const std::exception& e) {
                  error = e.what();
              }
              asio::post(this->server_io_, [this, error = std::move(error)] {
                  this->error_ = error;
                  this->done_ = true;
              });
          }) {}

    ~Peer() {
        this->thread_.join();
    }

    Peer(const Peer&) = delete;
    Peer& operator=(const Peer&) = delete;

    bool done() const {
        return this->done_;
    }

    const std::string& error() const {
        return this->error_;
    }

private:
    asio::io_context& server_io_;
    std::string error_;
    bool done_{false};
    std::thread thread_;
};

// Holds a peer thread until opened. An OpenOnExit declared after the Peer opens it at the
// latest, so a failed assertion cannot leave ~Peer joining a thread that waits on it.
class Gate {
public:
    void wait() {
        this->future_.wait();
    }

    void open() {
        if (!this->opened_) {
            this->opened_ = true;
            this->promise_.set_value();
        }
    }

private:
    std::promise<void> promise_;
    std::shared_future<void> future_{promise_.get_future().share()};
    bool opened_{false};
};

class OpenOnExit {
public:
    explicit OpenOnExit(Gate& gate) : gate_(gate) {}
    ~OpenOnExit() {
        this->gate_.open();
    }
    OpenOnExit(const OpenOnExit&) = delete;
    OpenOnExit& operator=(const OpenOnExit&) = delete;

private:
    Gate& gate_;
};

// Sends queued messages under the write invariant, as many to a write as it takes (at most
// per_write), and records every event
class TestHandler : public ConnectionHandler {
public:
    struct Message {
        MessageKind kind;
        std::vector<uint8_t> payload;
        int64_t rx_us;
    };

    explicit TestHandler(Connection& connection) : connection_(&connection) {}

    void send(MessageKind kind, std::vector<uint8_t> payload) {
        this->queue_.emplace_back(kind, std::move(payload));
        if (!this->busy_) {
            this->busy_ = this->connection_->wait_writable();
        }
    }

    void on_message(MessageKind kind, std::span<const uint8_t> payload, int64_t rx_us) override {
        this->messages.push_back({kind, {payload.begin(), payload.end()}, rx_us});
        if (this->message_hook) {
            this->message_hook();
        }
    }

    void on_writable() override {
        ++this->writable;
        if (this->writable_hook) {
            this->writable_hook();
            return;
        }
        if (this->queue_.empty()) {
            this->busy_ = false;
            return;
        }
        size_t taken = 0;
        while (!this->queue_.empty() && taken < this->per_write && this->connection_->can_write()) {
            auto [kind, payload] = std::move(this->queue_.front());
            this->queue_.pop_front();
            EXPECT_TRUE(this->connection_->write(kind, payload));
            ++taken;
        }
        this->write_sizes.push_back(taken);
        ++this->writes_started;
    }

    void on_write_done() override {
        // A writable_hook's writes are not counted
        if (static_cast<size_t>(this->write_done) < this->write_sizes.size()) {
            this->messages_done += this->write_sizes[this->write_done];
        }
        ++this->write_done;
        this->busy_ = false;
        if (!this->queue_.empty()) {
            this->busy_ = this->connection_->wait_writable();
        }
    }

    void on_closed(CloseReason reason) override {
        this->closes.push_back(reason);
        this->messages_at_close.push_back(this->messages.size());
    }

    std::vector<Message> messages;
    std::vector<CloseReason> closes;
    std::vector<size_t> messages_at_close;
    int writable{0};
    int writes_started{0};
    int write_done{0};
    // The messages of each write started, and of those completed
    std::vector<size_t> write_sizes;
    size_t messages_done{0};
    size_t per_write{SIZE_MAX};
    std::function<void()> message_hook;
    std::function<void()> writable_hook;

private:
    Connection* connection_;
    std::deque<std::pair<MessageKind, std::vector<uint8_t>>> queue_;
    bool busy_{false};
};

class TransportTest : public testing::Test {
protected:
    // Blocks in the io_context until done() holds; a regression hangs into the suite watchdog
    template <typename Predicate>
    void run_until(Predicate done) {
        while (!done()) {
            this->io.run_one();
        }
    }

    // Runs the io_context's handlers for about duration_us
    void run_for(int64_t duration_us) {
        asio::steady_timer timer(this->io, std::chrono::microseconds(duration_us));
        bool elapsed = false;
        timer.async_wait([&elapsed](const error_code& /*ec*/) { elapsed = true; });
        this->run_until([&] { return elapsed; });
    }

    // Starts a listener on an ephemeral loopback port that keeps every accepted connection
    std::unique_ptr<Listener> listen() {
        auto listener = std::make_unique<Listener>(
            this->loop, this->settings,
            [this](Connection connection) { this->accepted.push_back(std::move(connection)); });
        EXPECT_TRUE(listener->listen("127.0.0.1", 0));
        EXPECT_NE(listener->port(), 0);
        return listener;
    }

    // Dials and waits for the outcome
    std::optional<Connection> dial_and_wait(const DialTarget& target) {
        bool finished = false;
        std::optional<Connection> result;
        PendingDial pending =
            dial(this->loop, target, this->settings, [&](sendspin::server::DialResult outcome) {
                finished = true;
                if (Connection* connection = std::get_if<Connection>(&outcome)) {
                    result = std::move(*connection);
                } else {
                    this->dial_failure = std::get<CloseReason>(outcome);
                }
            });
        this->run_until([&] { return finished; });
        return result;
    }

    // Posts to the test thread's io_context from a peer thread
    void signal(bool& flag) {
        asio::post(this->io, [&flag] { flag = true; });
    }

    sendspin::server::EventLoop loop;
    // The loop's io_context, which holds work until the loop is shut down
    asio::io_context& io{loop.impl().io()};
    TransportSettings settings;
    std::vector<Connection> accepted;
    CloseReason dial_failure{CloseReason::FAILED};
};

// A synchronous client peer connected to the listener at port
void connect_peer(PeerSocket& ws, uint16_t port, const std::string& path = "/sendspin",
                  int receive_buffer = 0) {
    ws.next_layer().open(tcp::v4());
    if (receive_buffer > 0) {
        ws.next_layer().set_option(asio::socket_base::receive_buffer_size(receive_buffer));
    }
    ws.next_layer().connect({LOOPBACK, port});
    ws.handshake("127.0.0.1", path);
}

// Whether socket, connecting to endpoint without blocking, completes its TCP handshake within
// wait_ms
bool connects_within(tcp::socket& socket, const tcp::endpoint& endpoint, int wait_ms) {
    socket.open(endpoint.protocol());
    socket.non_blocking(true);
    // Asio's own connect waits for the outcome even on a non-blocking socket
    if (::connect(socket.native_handle(), endpoint.data(), endpoint.size()) == 0) {
        return true;
    }
    if (errno != EINPROGRESS) {
        return false;
    }
    pollfd entry{socket.native_handle(), POLLOUT, 0};
    int error = 0;
    socklen_t length = sizeof(error);
    return poll(&entry, 1, wait_ms) == 1 &&
           getsockopt(socket.native_handle(), SOL_SOCKET, SO_ERROR, &error, &length) == 0 &&
           error == 0;
}

// A synchronous server peer: accepts one connection and returns the upgrade request
http::request<http::string_body> accept_peer(tcp::acceptor& acceptor, PeerSocket& ws) {
    acceptor.accept(ws.next_layer());
    beast::flat_buffer buffer;
    http::request<http::string_body> request;
    http::read(ws.next_layer(), buffer, request);
    ws.accept(request);
    return request;
}

struct RawFrame {
    bool fin;
    uint8_t opcode;
    bool masked;
    std::vector<uint8_t> payload;
};

// Reads one WebSocket frame straight off the socket (RFC 6455 section 5.2)
RawFrame read_raw_frame(tcp::socket& socket) {
    std::array<uint8_t, 2> head{};
    asio::read(socket, asio::buffer(head));
    uint64_t length = head[1] & 0x7FU;
    if (length == 126) {
        std::array<uint8_t, 2> extended{};
        asio::read(socket, asio::buffer(extended));
        length = (uint64_t{extended[0]} << 8U) | extended[1];
    } else if (length == 127) {
        std::array<uint8_t, 8> extended{};
        asio::read(socket, asio::buffer(extended));
        length = 0;
        for (const uint8_t byte : extended) {
            length = (length << 8U) | byte;
        }
    }
    RawFrame frame{(head[0] & 0x80U) != 0, static_cast<uint8_t>(head[0] & 0x0FU),
                   (head[1] & 0x80U) != 0, std::vector<uint8_t>(length)};
    std::array<uint8_t, 4> mask{};
    if (frame.masked) {
        asio::read(socket, asio::buffer(mask));
    }
    asio::read(socket, asio::buffer(frame.payload));
    if (frame.masked) {
        for (size_t i = 0; i < frame.payload.size(); ++i) {
            frame.payload[i] ^= mask[i % 4];
        }
    }
    return frame;
}

uint16_t port_of(const sockaddr_storage& address) {
    if (address.ss_family == AF_INET) {
        return ntohs(reinterpret_cast<const sockaddr_in&>(address).sin_port);
    }
    if (address.ss_family == AF_INET6) {
        return ntohs(reinterpret_cast<const sockaddr_in6&>(address).sin6_port);
    }
    return 0;
}

// The transport does not expose its sockets, so find one by its two loopback ports
int find_socket(uint16_t local_port, uint16_t peer_port) {
    constexpr int MAX_FD = 4096;
    for (int fd = 0; fd < MAX_FD; ++fd) {
        sockaddr_storage local{};
        sockaddr_storage peer{};
        socklen_t local_length = sizeof(local);
        socklen_t peer_length = sizeof(peer);
        if (getsockname(fd, reinterpret_cast<sockaddr*>(&local), &local_length) != 0 ||
            getpeername(fd, reinterpret_cast<sockaddr*>(&peer), &peer_length) != 0) {
            continue;
        }
        if (port_of(local) == local_port && port_of(peer) == peer_port) {
            return fd;
        }
    }
    return -1;
}

// The server's end of an accepted connection: the listener's port and the peer's
int server_socket(const Connection& connection, uint16_t listen_port) {
    const std::string& endpoint = connection.remote_endpoint();
    const auto peer_port =
        static_cast<uint16_t>(std::stoi(endpoint.substr(endpoint.rfind(':') + 1)));
    return find_socket(listen_port, peer_port);
}

// Bytes the kernel holds for fd that no read has taken
int unread_bytes(int fd) {
    int bytes = 0;
    EXPECT_EQ(ioctl(fd, FIONREAD, &bytes), 0);
    return bytes;
}

// Bytes fd's kernel send queue holds, sent but not acknowledged or not yet sent
int unsent_bytes(int fd) {
    int bytes = 0;
#if defined(__APPLE__)
    socklen_t length = sizeof(bytes);
    EXPECT_EQ(getsockopt(fd, SOL_SOCKET, SO_NWRITE, &bytes, &length), 0);
#else
    EXPECT_EQ(ioctl(fd, SIOCOUTQ, &bytes), 0);
#endif
    return bytes;
}

// Blocks, running no handler of the server's io_context, until the server's end has taken every
// byte the peer wrote: once the peer's last message is one the inbound ring refuses, the network
// thread holds it. A network thread that drops it instead goes on reading the messages after it,
// which the tests check for.
void wait_taken(int server_fd, int peer_fd) {
    while (unread_bytes(server_fd) > 0 || unsent_bytes(peer_fd) > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

int tcp_option(int fd, int option) {
    int value = 0;
    socklen_t length = sizeof(value);
    EXPECT_EQ(getsockopt(fd, IPPROTO_TCP, option, &value, &length), 0);
    return value;
}

// ==============================================================================
// Accept and dial
// ==============================================================================

TEST_F(TransportTest, AcceptedConnectionRoundTripsTextAndBinary) {
    auto listener = this->listen();
    const uint16_t port = listener->port();
    const std::vector<uint8_t> binary{0x00, 0x01, 0x7F, 0x80, 0xFF};
    int64_t peer_sent_us = 0;
    bool peer_got_text = false;
    bool peer_got_binary = false;
    std::vector<uint8_t> peer_text;
    std::vector<uint8_t> peer_binary;
    uint16_t peer_port = 0;

    Peer peer(this->io, [&] {
        asio::io_context peer_io;
        PeerSocket ws(peer_io);
        connect_peer(ws, port);
        peer_port = ws.next_layer().local_endpoint().port();
        peer_sent_us = now_us();
        ws.text(true);
        ws.write(asio::buffer(std::string("hello")));
        ws.binary(true);
        ws.write(asio::buffer(binary));
        beast::flat_buffer buffer;
        ws.read(buffer);
        peer_got_text = ws.got_text();
        peer_text = buffer_bytes(buffer);
        buffer.clear();
        ws.read(buffer);
        peer_got_binary = ws.got_binary();
        peer_binary = buffer_bytes(buffer);
        ws.close(websocket::close_code::normal);
    });

    this->run_until([&] { return !this->accepted.empty(); });
    Connection& connection = this->accepted.front();
    TestHandler handler(connection);
    handler.message_hook = [&] {
        const TestHandler::Message& message = handler.messages.back();
        handler.send(message.kind, message.payload);
    };
    connection.start(handler);
    this->run_until([&] { return peer.done(); });
    const int64_t after_us = now_us();

    ASSERT_EQ(peer.error(), "");
    ASSERT_EQ(handler.messages.size(), 2U);
    EXPECT_EQ(handler.messages[0].kind, MessageKind::TEXT);
    EXPECT_EQ(handler.messages[0].payload, bytes_of("hello"));
    EXPECT_EQ(handler.messages[1].kind, MessageKind::BINARY);
    EXPECT_EQ(handler.messages[1].payload, binary);
    for (const TestHandler::Message& message : handler.messages) {
        EXPECT_GE(message.rx_us, peer_sent_us);
        EXPECT_LE(message.rx_us, after_us);
    }
    EXPECT_EQ(connection.remote_endpoint(), "127.0.0.1:" + std::to_string(peer_port));
    EXPECT_TRUE(peer_got_text);
    EXPECT_EQ(peer_text, bytes_of("hello"));
    EXPECT_TRUE(peer_got_binary);
    EXPECT_EQ(peer_binary, binary);
}

TEST_F(TransportTest, DialedConnectionRoundTripsTextAndBinary) {
    struct Row {
        const char* name;
        bool by_address;
        /// For by_address
        const char* host;
        const char* host_header;
    };
    // RFC 6874 section 4: a zone stays out of the Host header
    for (const Row row : {Row{"url", false, "", "127.0.0.1"},
                          Row{"addresses", true, "sendspin.invalid", "sendspin.invalid"},
                          Row{"zoned host", true, "fe80::1%lo0", "[fe80::1]"}}) {
        SCOPED_TRACE(row.name);
        asio::io_context peer_io;
        tcp::acceptor acceptor(peer_io, {LOOPBACK, 0});
        const uint16_t port = acceptor.local_endpoint().port();
        std::string target_seen;
        std::string host_seen;

        Peer peer(this->io, [&] {
            PeerSocket ws(peer_io);
            const auto request = accept_peer(acceptor, ws);
            target_seen = std::string(request.target());
            host_seen = std::string(request[http::field::host]);
            // Echo two messages with their type
            for (int i = 0; i < 2; ++i) {
                beast::flat_buffer buffer;
                ws.read(buffer);
                ws.text(ws.got_text());
                ws.write(buffer.data());
            }
            beast::flat_buffer buffer;
            error_code ec;
            ws.read(buffer, ec);
        });

        DialTarget target;
        if (row.by_address) {
            // A host never resolved: only the address list can reach the peer
            target.host = row.host;
            target.port = port;
            target.path = "/sendspin";
            target.addresses = {"127.0.0.1"};
        } else {
            target = *parse_ws_url("ws://127.0.0.1:" + std::to_string(port) + "/sendspin");
        }
        std::optional<Connection> connection = this->dial_and_wait(target);
        ASSERT_TRUE(connection.has_value());
        EXPECT_EQ(connection->remote_endpoint(), "127.0.0.1:" + std::to_string(port));
        TestHandler handler(*connection);
        connection->start(handler);
        handler.send(MessageKind::TEXT, bytes_of("time"));
        handler.send(MessageKind::BINARY, {9, 8, 7});
        this->run_until([&] { return handler.messages.size() == 2; });
        connection->close(close_code::NORMAL);
        this->run_until([&] { return peer.done(); });

        ASSERT_EQ(peer.error(), "");
        EXPECT_EQ(target_seen, "/sendspin");
        EXPECT_EQ(host_seen, std::string(row.host_header) + ":" + std::to_string(port));
        EXPECT_EQ(handler.messages[0].kind, MessageKind::TEXT);
        EXPECT_EQ(handler.messages[0].payload, bytes_of("time"));
        EXPECT_EQ(handler.messages[1].kind, MessageKind::BINARY);
        EXPECT_EQ(handler.messages[1].payload, (std::vector<uint8_t>{9, 8, 7}));
    }
}

TEST_F(TransportTest, UpgradeToAnotherPathIsRejectedWith404) {
    auto listener = this->listen();
    const uint16_t port = listener->port();
    http::status wrong_path_status{};
    error_code wrong_path_error;
    bool control_connected = false;

    Peer peer(this->io, [&] {
        asio::io_context peer_io;
        {
            PeerSocket ws(peer_io);
            ws.next_layer().connect({LOOPBACK, port});
            websocket::response_type response;
            ws.handshake(response, "127.0.0.1", "/other", wrong_path_error);
            wrong_path_status = response.result();
        }
        // Control: the configured path, with a query string, is accepted
        PeerSocket ws(peer_io);
        connect_peer(ws, port, "/sendspin?client=1");
        control_connected = true;
    });
    this->run_until([&] { return peer.done(); });

    ASSERT_EQ(peer.error(), "");
    EXPECT_EQ(wrong_path_error, websocket::error::upgrade_declined);
    EXPECT_EQ(wrong_path_status, http::status::not_found);
    EXPECT_TRUE(control_connected);
    // The rejected upgrade came first and was never delivered
    this->run_until([&] { return !this->accepted.empty(); });
    EXPECT_EQ(this->accepted.size(), 1U);
}

TEST_F(TransportTest, RequestThatIsNotAnUpgradeGets400OnAnyPath) {
    auto listener = this->listen();
    const uint16_t port = listener->port();
    std::vector<http::status> statuses;

    Peer peer(this->io, [&] {
        asio::io_context peer_io;
        for (const char* path : {"/sendspin", "/other"}) {
            tcp::socket socket(peer_io);
            socket.connect({LOOPBACK, port});
            http::request<http::empty_body> request(http::verb::get, path, 11);
            request.set(http::field::host, "127.0.0.1");
            http::write(socket, request);
            beast::flat_buffer buffer;
            http::response<http::string_body> response;
            http::read(socket, buffer, response);
            statuses.push_back(response.result());
        }
    });
    this->run_until([&] { return peer.done(); });

    ASSERT_EQ(peer.error(), "");
    EXPECT_EQ(statuses,
              (std::vector<http::status>{http::status::bad_request, http::status::bad_request}));
    EXPECT_TRUE(this->accepted.empty());
}

TEST_F(TransportTest, ListenerCloseDropsUpgradesInProgress) {
    // Longer than the suite watchdog, so only the drop can end the peer's read
    this->settings.handshake_timeout_us = 600000000;
    auto listener = this->listen();
    const uint16_t port = listener->port();
    bool peer_connected = false;
    uint16_t peer_port = 0;
    error_code peer_read_error;

    Peer peer(this->io, [&] {
        asio::io_context peer_io;
        tcp::socket socket(peer_io);
        socket.connect({LOOPBACK, port});
        peer_port = socket.local_endpoint().port();
        this->signal(peer_connected);
        // No upgrade request: the server is left waiting for one
        std::array<uint8_t, 1> byte{};
        asio::read(socket, asio::buffer(byte), peer_read_error);
    });
    this->run_until([&] { return peer_connected; });
    // Once the server side of the socket exists, its accept completion is queued; run it
    this->run_until([&] { return find_socket(port, peer_port) >= 0; });
    while (this->io.poll() > 0) {}
    listener->close();
    this->run_until([&] { return peer.done(); });

    ASSERT_EQ(peer.error(), "");
    EXPECT_EQ(peer_read_error, asio::error::eof);
    EXPECT_TRUE(this->accepted.empty());
}

// ==============================================================================
// Receive stamps and the hand-off to core
// ==============================================================================

// messaging.md "Receive timestamps": the stamp is read when the message's last byte is in, by the
// thread that reads the socket, however late core gets to the message. Core runs nothing until the
// peer has the pong to a ping it sent after the message, which the server sends only once the read
// that stamped the message has completed, so a stamp read on core would come after it.
TEST_F(TransportTest, TheReceiveStampIsReadWhileCoreIsBusy) {
    auto listener = this->listen();
    const uint16_t port = listener->port();
    int64_t peer_sent_us = 0;
    Gate ponged;

    Peer peer(this->io, [&] {
        // Opens at the latest when the peer ends, so a peer that fails cannot hold the test
        const OpenOnExit open_ponged(ponged);
        asio::io_context peer_io;
        PeerSocket ws(peer_io);
        connect_peer(ws, port);
        ws.control_callback([&](websocket::frame_type kind, beast::string_view /*payload*/) {
            if (kind == websocket::frame_type::pong) {
                ponged.open();
            }
        });
        peer_sent_us = now_us();
        ws.text(true);
        ws.write(asio::buffer(std::string("time")));
        ws.ping({});
        // Takes the pong, then waits for the server's close
        beast::flat_buffer buffer;
        error_code ec;
        ws.read(buffer, ec);
    });
    this->run_until([&] { return !this->accepted.empty(); });
    Connection& connection = this->accepted.front();
    TestHandler handler(connection);
    connection.start(handler);
    ponged.wait();
    const int64_t core_resumed_us = now_us();
    this->run_until([&] { return !handler.messages.empty() || !handler.closes.empty(); });
    connection.close(close_code::NORMAL);
    this->run_until([&] { return peer.done(); });

    ASSERT_EQ(peer.error(), "");
    ASSERT_EQ(handler.messages.size(), 1U);
    EXPECT_EQ(handler.messages[0].payload, bytes_of("time"));
    EXPECT_GE(handler.messages[0].rx_us, peer_sent_us);
    EXPECT_LT(handler.messages[0].rx_us, core_resumed_us);
}

constexpr size_t HELD_MESSAGE_BYTES = 60000;

/// The messages the inbound ring takes before it refuses one of HELD_MESSAGE_BYTES
size_t in_ring() {
    return sendspin::server::profile::INBOUND_RING_BYTES / HELD_MESSAGE_BYTES;
}

std::vector<uint8_t> numbered(size_t index) {
    std::vector<uint8_t> out = pattern(HELD_MESSAGE_BYTES);
    out[0] = static_cast<uint8_t>(index);
    return out;
}

// While core runs nothing, the network thread reads until the inbound ring refuses a message and
// then stops reading that connection, leaving the rest to TCP. Once core drains the ring, every
// message arrives once and in order, and the peer's close after the last of them.
TEST_F(TransportTest, MessagesPastTheInboundRingWaitAndArriveInOrder) {
    // The ring's, then the one it refuses, then more behind it
    const size_t held = in_ring() + 1;
    const size_t messages = (3 * in_ring()) + 1;
    auto listener = this->listen();
    const uint16_t port = listener->port();
    Gate written;
    Gate rest;
    int peer_fd = -1;

    Peer peer(this->io, [&] {
        const OpenOnExit open_written(written);
        asio::io_context peer_io;
        PeerSocket ws(peer_io);
        connect_peer(ws, port);
        peer_fd = ws.next_layer().native_handle();
        ws.binary(true);
        for (size_t i = 0; i < held; ++i) {
            ws.write(asio::buffer(numbered(i)));
        }
        written.open();
        rest.wait();
        for (size_t i = held; i < messages; ++i) {
            ws.write(asio::buffer(numbered(i)));
        }
        ws.close(websocket::close_code::normal);
    });
    const OpenOnExit open_rest(rest);
    this->run_until([&] { return !this->accepted.empty(); });
    Connection& connection = this->accepted.front();
    TestHandler handler(connection);
    connection.start(handler);
    const int fd = server_socket(connection, port);
    ASSERT_GE(fd, 0);
    written.wait();
    wait_taken(fd, peer_fd);
    rest.open();
    this->run_until([&] { return peer.done() && !handler.closes.empty(); });

    ASSERT_EQ(peer.error(), "");
    ASSERT_EQ(handler.messages.size(), messages);
    for (size_t i = 0; i < messages; ++i) {
        EXPECT_EQ(handler.messages[i].payload, numbered(i)) << "message " << i;
    }
    EXPECT_EQ(handler.closes, std::vector<CloseReason>{CloseReason::PEER_CLOSED});
    EXPECT_EQ(handler.messages_at_close, std::vector<size_t>{messages});
}

// A write that stalls while a message waits for room in the ring ends the connection, and the
// close reaches core after that message: everything the network thread read is delivered first.
// The write starts only once the message is held, from an on_writable() that holds core meanwhile.
TEST_F(TransportTest, AStallWhileHeldBackClosesAfterTheHeldMessage) {
    this->settings.write_stall_us = 200000;
    // One write far larger than the socket buffers stays outstanding
    this->settings.notsent_lowat_bytes = 0;
    const size_t held = in_ring() + 1;
    auto listener = this->listen();
    const uint16_t port = listener->port();
    Gate send;
    Gate written;
    Gate probe;
    Gate closed;
    int peer_fd = -1;

    Peer peer(this->io, [&] {
        const OpenOnExit open_written(written);
        const OpenOnExit open_closed(closed);
        asio::io_context peer_io;
        PeerSocket ws(peer_io);
        connect_peer(ws, port, "/sendspin", 16384);
        peer_fd = ws.next_layer().native_handle();
        send.wait();
        ws.binary(true);
        for (size_t i = 0; i < held; ++i) {
            ws.write(asio::buffer(numbered(i)));
        }
        written.open();
        probe.wait();
        // Reads nothing; sends until the server's drop fails a write
        error_code ec;
        for (size_t i = held; !ec; ++i) {
            ws.write(asio::buffer(numbered(i)), ec);
        }
    });
    const OpenOnExit open_send(send);
    const OpenOnExit open_probe(probe);
    this->run_until([&] { return !this->accepted.empty(); });
    Connection& connection = this->accepted.front();
    TestHandler handler(connection);
    connection.start(handler);
    const int fd = server_socket(connection, port);
    ASSERT_GE(fd, 0);
    bool write_started = false;
    handler.writable_hook = [&] {
        send.open();
        written.wait();
        wait_taken(fd, peer_fd);
        write_started = connection.write(MessageKind::BINARY, pattern(16 * 1024 * 1024));
    };
    ASSERT_TRUE(connection.wait_writable());
    this->run_until([&] { return handler.writable == 1; });
    ASSERT_TRUE(write_started);
    probe.open();
    // Core runs nothing from here until the peer saw the server drop the connection
    closed.wait();
    this->run_until([&] { return !handler.closes.empty(); });
    this->run_until([&] { return peer.done(); });

    ASSERT_EQ(peer.error(), "");
    EXPECT_EQ(handler.closes, std::vector<CloseReason>{CloseReason::WRITE_STALLED});
    ASSERT_EQ(handler.messages.size(), held) << "the ring's and the held one";
    for (size_t i = 0; i < handler.messages.size(); ++i) {
        EXPECT_EQ(handler.messages[i].payload, numbered(i)) << "message " << i;
    }
    EXPECT_EQ(handler.messages_at_close, std::vector<size_t>{held});
}

// Closed by core while a message waits for room in the ring, a connection sends its close frame
// and reports nothing more: neither the ring's messages nor the held one, nor a close
TEST_F(TransportTest, ACloseWhileHeldBackReportsNothingMore) {
    const size_t held = in_ring() + 1;
    auto listener = this->listen();
    const uint16_t port = listener->port();
    Gate written;
    int peer_fd = -1;
    uint16_t peer_close_code = 0;

    Peer peer(this->io, [&] {
        const OpenOnExit open_written(written);
        asio::io_context peer_io;
        PeerSocket ws(peer_io);
        connect_peer(ws, port);
        peer_fd = ws.next_layer().native_handle();
        ws.binary(true);
        for (size_t i = 0; i < held; ++i) {
            ws.write(asio::buffer(numbered(i)));
        }
        written.open();
        beast::flat_buffer buffer;
        error_code ec;
        ws.read(buffer, ec);
        peer_close_code = ws.reason().code;
    });
    this->run_until([&] { return !this->accepted.empty(); });
    Connection& connection = this->accepted.front();
    TestHandler handler(connection);
    connection.start(handler);
    const int fd = server_socket(connection, port);
    ASSERT_GE(fd, 0);
    written.wait();
    wait_taken(fd, peer_fd);
    constexpr uint16_t APPLICATION_CODE = 4002;
    connection.close(APPLICATION_CODE);
    this->run_until([&] { return peer.done(); });
    while (this->io.poll() > 0) {}

    ASSERT_EQ(peer.error(), "");
    EXPECT_EQ(peer_close_code, APPLICATION_CODE);
    EXPECT_TRUE(handler.messages.empty()) << handler.messages.size();
    EXPECT_TRUE(handler.closes.empty());
}

// Connections the ring held back push again in the order they were held: one that filled the
// ring first, then those whose messages came while the ring refused, more of them than the room
// the network thread's queue of held-back connections starts with (eight)
TEST_F(TransportTest, HeldBackConnectionsResumeInTheOrderTheyWereHeld) {
    constexpr size_t LATER = 9;
    const size_t held = in_ring() + 1;
    auto listener = this->listen();
    const uint16_t port = listener->port();
    Gate first_written;
    Gate first_close;
    int first_fd = -1;

    Peer first(this->io, [&] {
        const OpenOnExit open_written(first_written);
        asio::io_context peer_io;
        PeerSocket ws(peer_io);
        connect_peer(ws, port);
        first_fd = ws.next_layer().native_handle();
        ws.binary(true);
        for (size_t i = 0; i < held; ++i) {
            ws.write(asio::buffer(numbered(i)));
        }
        first_written.open();
        first_close.wait();
        ws.close(websocket::close_code::normal);
    });
    const OpenOnExit open_first_close(first_close);
    this->run_until([&] { return this->accepted.size() == 1; });
    // Each later peer connects, then sends its message when told to and reads until closed
    std::vector<std::unique_ptr<Gate>> send(LATER);
    std::vector<std::unique_ptr<Gate>> written(LATER);
    std::vector<int> later_fds(LATER, -1);
    std::vector<std::unique_ptr<Peer>> later;
    std::vector<std::unique_ptr<OpenOnExit>> open_sends;
    for (size_t n = 0; n < LATER; ++n) {
        send[n] = std::make_unique<Gate>();
        written[n] = std::make_unique<Gate>();
        later.push_back(std::make_unique<Peer>(this->io, [&, n] {
            const OpenOnExit open_written(*written[n]);
            asio::io_context peer_io;
            PeerSocket ws(peer_io);
            connect_peer(ws, port);
            later_fds[n] = ws.next_layer().native_handle();
            send[n]->wait();
            ws.text(true);
            ws.write(asio::buffer("later " + std::to_string(n)));
            written[n]->open();
            beast::flat_buffer buffer;
            error_code ec;
            ws.read(buffer, ec);
        }));
        open_sends.push_back(std::make_unique<OpenOnExit>(*send[n]));
        this->run_until([&] { return this->accepted.size() == n + 2; });
    }
    std::vector<std::string> order;
    std::vector<std::unique_ptr<TestHandler>> handlers;
    for (size_t c = 0; c < this->accepted.size(); ++c) {
        handlers.push_back(std::make_unique<TestHandler>(this->accepted[c]));
        handlers.back()->message_hook = [&order, c] {
            order.push_back(c == 0 ? std::string("first") : "later " + std::to_string(c - 1));
        };
        this->accepted[c].start(*handlers.back());
    }
    const int first_server_fd = server_socket(this->accepted[0], port);
    ASSERT_GE(first_server_fd, 0);
    // Core runs nothing until the first holds its last message back and each later message, in
    // turn, was taken and refused
    first_written.wait();
    wait_taken(first_server_fd, first_fd);
    for (size_t n = 0; n < LATER; ++n) {
        const int server_fd = server_socket(this->accepted[n + 1], port);
        ASSERT_GE(server_fd, 0);
        send[n]->open();
        written[n]->wait();
        wait_taken(server_fd, later_fds[n]);
    }
    first_close.open();
    this->run_until([&] { return first.done() && !handlers[0]->closes.empty(); });
    for (size_t n = 0; n < LATER; ++n) {
        this->accepted[n + 1].close(close_code::NORMAL);
    }
    this->run_until([&] {
        return std::all_of(later.begin(), later.end(),
                           [](const std::unique_ptr<Peer>& peer) { return peer->done(); });
    });

    ASSERT_EQ(first.error(), "");
    for (const std::unique_ptr<Peer>& peer : later) {
        ASSERT_EQ(peer->error(), "");
    }
    std::vector<std::string> expected(held, "first");
    for (size_t n = 0; n < LATER; ++n) {
        expected.push_back("later " + std::to_string(n));
    }
    EXPECT_EQ(order, expected);
}

// ==============================================================================
// Socket and WebSocket settings
// ==============================================================================

TEST_F(TransportTest, MessageLargerThanTheWriteBufferIsOneFrame) {
    // Beast fragments at the write buffer size when auto_fragment is on
    this->settings.write_buffer_bytes = 16384;
    const std::vector<uint8_t> message = pattern(40000);
    for (const bool dialed : {false, true}) {
        SCOPED_TRACE(dialed ? "dialed (masked)" : "accepted");
        this->accepted.clear();
        asio::io_context peer_io;
        tcp::acceptor acceptor(peer_io, {LOOPBACK, 0});
        auto listener = this->listen();
        const uint16_t port = listener->port();
        bool peer_ready = false;
        RawFrame frame{};

        Peer peer(this->io, [&] {
            PeerSocket ws(peer_io);
            if (dialed) {
                accept_peer(acceptor, ws);
            } else {
                connect_peer(ws, port);
            }
            this->signal(peer_ready);
            frame = read_raw_frame(ws.next_layer());
        });

        std::optional<Connection> dialed_connection;
        if (dialed) {
            dialed_connection = this->dial_and_wait(*parse_ws_url(
                "ws://127.0.0.1:" + std::to_string(acceptor.local_endpoint().port()) + "/"));
            ASSERT_TRUE(dialed_connection.has_value());
        } else {
            this->run_until([&] { return !this->accepted.empty(); });
        }
        Connection& connection = dialed ? *dialed_connection : this->accepted.front();
        TestHandler handler(connection);
        connection.start(handler);
        // The peer reads raw frames, so nothing may follow its handshake response unread
        this->run_until([&] { return peer_ready; });
        handler.send(MessageKind::BINARY, message);
        this->run_until([&] { return peer.done(); });

        ASSERT_EQ(peer.error(), "");
        EXPECT_TRUE(frame.fin);
        EXPECT_EQ(frame.opcode, 0x2);
        EXPECT_EQ(frame.masked, dialed);
        EXPECT_EQ(frame.payload, message);
    }
}

TEST_F(TransportTest, WriteBufferBelowBeastsMinimumIsRaised) {
    // Beast throws for 8 bytes or fewer, which would escape io_context::run()
    this->settings.write_buffer_bytes = 0;
    asio::io_context peer_io;
    tcp::acceptor acceptor(peer_io, {LOOPBACK, 0});
    auto listener = this->listen();
    const uint16_t port = listener->port();
    std::vector<uint8_t> dialed_received;

    Peer peer(this->io, [&] {
        PeerSocket accepted_peer(peer_io);
        connect_peer(accepted_peer, port);
        PeerSocket dialed_peer(peer_io);
        accept_peer(acceptor, dialed_peer);
        beast::flat_buffer buffer;
        dialed_peer.read(buffer);
        dialed_received = buffer_bytes(buffer);
    });
    std::optional<Connection> dialed = this->dial_and_wait(
        *parse_ws_url("ws://127.0.0.1:" + std::to_string(acceptor.local_endpoint().port()) + "/"));
    ASSERT_TRUE(dialed.has_value());
    TestHandler handler(*dialed);
    dialed->start(handler);
    // Masked, so it passes through the write buffer
    handler.send(MessageKind::BINARY, pattern(100));
    this->run_until([&] { return peer.done() && !this->accepted.empty(); });

    ASSERT_EQ(peer.error(), "");
    EXPECT_EQ(dialed_received, pattern(100));
}

TEST_F(TransportTest, InboundMessageOverReadMessageMaxClosesTheConnection) {
    this->settings.read_message_max = 1024;
    auto listener = this->listen();
    const uint16_t port = listener->port();
    error_code peer_error;
    uint16_t peer_close_code = 0;

    Peer peer(this->io, [&] {
        asio::io_context peer_io;
        PeerSocket ws(peer_io);
        connect_peer(ws, port);
        ws.binary(true);
        ws.write(asio::buffer(pattern(1024)));
        ws.write(asio::buffer(pattern(1025)));
        beast::flat_buffer buffer;
        ws.read(buffer, peer_error);
        peer_close_code = ws.reason().code;
    });
    this->run_until([&] { return !this->accepted.empty(); });
    TestHandler handler(this->accepted.front());
    this->accepted.front().start(handler);
    this->run_until([&] { return peer.done() && !handler.closes.empty(); });

    ASSERT_EQ(peer.error(), "");
    // Control: a message at the cap is delivered
    ASSERT_EQ(handler.messages.size(), 1U);
    EXPECT_EQ(handler.messages[0].payload, pattern(1024));
    EXPECT_EQ(handler.closes, std::vector<CloseReason>{CloseReason::PROTOCOL_VIOLATION});
    // Beast's teardown closes the socket after one read, so the rest of the oversized message
    // can be left unread and the close frame lost to a reset
    EXPECT_TRUE(peer_error);
    if (peer_error == websocket::error::closed) {
        EXPECT_EQ(peer_close_code, websocket::close_code::too_big);
    }
}

// RFC 6455 section 5.2: a reserved bit set with no extension negotiated fails the connection
TEST_F(TransportTest, InboundFrameBreakingTheWebSocketProtocolClosesTheConnection) {
    auto listener = this->listen();
    const uint16_t port = listener->port();

    Peer peer(this->io, [&] {
        asio::io_context peer_io;
        PeerSocket ws(peer_io);
        connect_peer(ws, port);
        ws.binary(true);
        ws.write(asio::buffer(pattern(16)));
        // FIN, RSV1 and the binary opcode; masked, with an empty payload
        const std::array<uint8_t, 6> frame{0xC2, 0x80, 1, 2, 3, 4};
        asio::write(ws.next_layer(), asio::buffer(frame));
        beast::flat_buffer buffer;
        error_code ignored;
        ws.read(buffer, ignored);
    });
    this->run_until([&] { return !this->accepted.empty(); });
    TestHandler handler(this->accepted.front());
    this->accepted.front().start(handler);
    this->run_until([&] { return peer.done() && !handler.closes.empty(); });

    ASSERT_EQ(peer.error(), "");
    // Control: the well-formed message before it is delivered
    ASSERT_EQ(handler.messages.size(), 1U);
    EXPECT_EQ(handler.messages[0].payload, pattern(16));
    EXPECT_EQ(handler.closes, std::vector<CloseReason>{CloseReason::PROTOCOL_VIOLATION});
}

TEST_F(TransportTest, AcceptedAndDialedSocketsCarryNoDelayAndNotSentLowWater) {
    struct Row {
        const char* name;
        bool dialed;
        int lowat;
    };
    const Row rows[] = {
        {"accepted", false, 24576},
        {"dialed", true, 24576},
        {"accepted, low-water unset", false, 0},
    };
    for (const Row& row : rows) {
        SCOPED_TRACE(row.name);
        this->settings.notsent_lowat_bytes = row.lowat;
        this->accepted.clear();
        asio::io_context peer_io;
        tcp::acceptor acceptor(peer_io, {LOOPBACK, 0});
        auto listener = this->listen();
        const uint16_t port = listener->port();
        bool ports_known = false;
        uint16_t server_local_port = 0;
        uint16_t server_peer_port = 0;
        int peer_fd = -1;
        Gate release;

        Peer peer(this->io, [&] {
            PeerSocket ws(peer_io);
            if (row.dialed) {
                accept_peer(acceptor, ws);
                server_local_port = ws.next_layer().remote_endpoint().port();
                server_peer_port = acceptor.local_endpoint().port();
            } else {
                connect_peer(ws, port);
                server_local_port = port;
                server_peer_port = ws.next_layer().local_endpoint().port();
            }
            peer_fd = ws.next_layer().native_handle();
            this->signal(ports_known);
            release.wait();
        });
        const OpenOnExit open_release(release);

        std::optional<Connection> dialed_connection;
        if (row.dialed) {
            dialed_connection = this->dial_and_wait(*parse_ws_url(
                "ws://127.0.0.1:" + std::to_string(acceptor.local_endpoint().port()) + "/"));
            ASSERT_TRUE(dialed_connection.has_value());
        } else {
            this->run_until([&] { return !this->accepted.empty(); });
        }
        this->run_until([&] { return ports_known; });

        const int fd = find_socket(server_local_port, server_peer_port);
        ASSERT_GE(fd, 0);
        EXPECT_NE(tcp_option(fd, TCP_NODELAY), 0);
        // Control: the peer's own socket keeps the defaults, so the reads distinguish them
        EXPECT_EQ(tcp_option(peer_fd, TCP_NODELAY), 0);
        if (row.lowat > 0) {
            EXPECT_EQ(tcp_option(fd, TCP_NOTSENT_LOWAT), row.lowat);
            EXPECT_NE(tcp_option(peer_fd, TCP_NOTSENT_LOWAT), row.lowat);
        } else {
            EXPECT_EQ(tcp_option(fd, TCP_NOTSENT_LOWAT), tcp_option(peer_fd, TCP_NOTSENT_LOWAT));
        }
        release.open();
        this->run_until([&] { return peer.done(); });
        ASSERT_EQ(peer.error(), "");
    }
}

// ==============================================================================
// Write invariant and write stall
// ==============================================================================

TEST_F(TransportTest, WritesOnlyWhenRoomWasReportedAndNoWriteIsInFlight) {
    auto listener = this->listen();
    const uint16_t port = listener->port();
    std::vector<std::vector<uint8_t>> peer_messages;
    uint16_t peer_close_code = 0;

    Peer peer(this->io, [&] {
        asio::io_context peer_io;
        PeerSocket ws(peer_io);
        connect_peer(ws, port);
        for (;;) {
            beast::flat_buffer buffer;
            error_code ec;
            ws.read(buffer, ec);
            if (ec) {
                peer_close_code = ws.reason().code;
                return;
            }
            peer_messages.push_back(buffer_bytes(buffer));
        }
    });
    this->run_until([&] { return !this->accepted.empty(); });
    Connection& connection = this->accepted.front();
    TestHandler handler(connection);
    const std::vector<uint8_t> first = bytes_of("first");
    const std::vector<uint8_t> second = bytes_of("second");

    // Not started
    EXPECT_FALSE(connection.wait_writable());
    connection.start(handler);
    // No room reported
    EXPECT_FALSE(connection.write(MessageKind::TEXT, first));
    ASSERT_TRUE(connection.wait_writable());
    // A wait is in flight
    EXPECT_FALSE(connection.wait_writable());
    EXPECT_FALSE(connection.write(MessageKind::TEXT, first));

    bool first_written = false;
    bool wait_during_write_refused = false;
    handler.writable_hook = [&] {
        first_written = connection.write(MessageKind::TEXT, first);
        // The write has begun
        wait_during_write_refused = !connection.wait_writable();
    };
    this->run_until([&] { return handler.writable == 1; });
    EXPECT_TRUE(first_written);
    EXPECT_TRUE(wait_during_write_refused);
    // Outside on_writable(), and with the write in flight
    EXPECT_FALSE(connection.can_write());
    EXPECT_FALSE(connection.write(MessageKind::TEXT, second));
    EXPECT_FALSE(connection.wait_writable());
    this->run_until([&] { return handler.write_done == 1; });

    // Room reported but not used expires with the callback
    handler.writable_hook = [] {};
    ASSERT_TRUE(connection.wait_writable());
    this->run_until([&] { return handler.writable == 2; });
    EXPECT_FALSE(connection.can_write());
    EXPECT_FALSE(connection.write(MessageKind::TEXT, second));

    bool second_written = false;
    handler.writable_hook = [&] { second_written = connection.write(MessageKind::TEXT, second); };
    ASSERT_TRUE(connection.wait_writable());
    this->run_until([&] { return handler.write_done == 2; });
    EXPECT_TRUE(second_written);
    connection.close(close_code::NORMAL);
    this->run_until([&] { return peer.done(); });

    ASSERT_EQ(peer.error(), "");
    EXPECT_EQ(peer_messages, (std::vector<std::vector<uint8_t>>{first, second}));
    EXPECT_EQ(peer_close_code, websocket::close_code::normal);
    EXPECT_TRUE(handler.closes.empty());
}

// The messages one on_writable() writes leave as one write: each a final frame of its own, text or
// binary as written, masked on a dialed connection, in order, and one on_write_done() for all
TEST_F(TransportTest, AWriteCarriesEachMessageAsAFrameOfItsOwn) {
    const std::vector<std::pair<MessageKind, std::vector<uint8_t>>> messages = {
        {MessageKind::TEXT, bytes_of("first")},
        // A 16-bit length
        {MessageKind::BINARY, pattern(300)},
        {MessageKind::BINARY, {}},
        {MessageKind::TEXT, bytes_of("{}")},
        // Past WRITE_BATCH_BYTES with the messages before it, the largest message there is
        {MessageKind::BINARY, pattern(sendspin::server::MAX_MESSAGE_BYTES)},
    };
    for (const bool dialed : {false, true}) {
        SCOPED_TRACE(dialed ? "dialed (masked)" : "accepted");
        this->accepted.clear();
        asio::io_context peer_io;
        tcp::acceptor acceptor(peer_io, {LOOPBACK, 0});
        auto listener = this->listen();
        const uint16_t port = listener->port();
        bool peer_ready = false;
        std::vector<RawFrame> frames;

        Peer peer(this->io, [&] {
            PeerSocket ws(peer_io);
            if (dialed) {
                accept_peer(acceptor, ws);
            } else {
                connect_peer(ws, port);
            }
            this->signal(peer_ready);
            for (size_t i = 0; i < messages.size(); ++i) {
                frames.push_back(read_raw_frame(ws.next_layer()));
            }
        });

        std::optional<Connection> dialed_connection;
        if (dialed) {
            dialed_connection = this->dial_and_wait(*parse_ws_url(
                "ws://127.0.0.1:" + std::to_string(acceptor.local_endpoint().port()) + "/"));
            ASSERT_TRUE(dialed_connection.has_value());
        } else {
            this->run_until([&] { return !this->accepted.empty(); });
        }
        Connection& connection = dialed ? *dialed_connection : this->accepted.front();
        TestHandler handler(connection);
        connection.start(handler);
        // The peer reads raw frames, so nothing may follow its handshake response unread
        this->run_until([&] { return peer_ready; });
        size_t written = 0;
        handler.writable_hook = [&] {
            for (const auto& [kind, payload] : messages) {
                written += connection.write(kind, payload) ? 1 : 0;
            }
        };
        ASSERT_TRUE(connection.wait_writable());
        this->run_until([&] { return peer.done() && handler.write_done == 1; });

        ASSERT_EQ(peer.error(), "");
        EXPECT_EQ(written, messages.size());
        EXPECT_EQ(handler.writable, 1);
        ASSERT_EQ(frames.size(), messages.size());
        for (size_t i = 0; i < messages.size(); ++i) {
            SCOPED_TRACE(i);
            EXPECT_TRUE(frames[i].fin);
            EXPECT_EQ(frames[i].opcode, messages[i].first == MessageKind::TEXT ? 0x1 : 0x2);
            EXPECT_EQ(frames[i].masked, dialed);
            EXPECT_EQ(frames[i].payload, messages[i].second);
        }
    }
}

// A message joins a write while the write holds fewer than WRITE_BATCH_MESSAGES messages and
// WRITE_BATCH_BYTES bytes; the first refused goes in the next write, after the write before it
TEST_F(TransportTest, AWriteTakesMessagesWithinItsBounds) {
    using sendspin::server::WRITE_BATCH_BYTES;
    using sendspin::server::WRITE_BATCH_MESSAGES;
    struct Row {
        const char* name;
        size_t message_size;
        size_t messages;
        // Those the first write takes
        size_t taken;
    };
    const Row rows[] = {
        {"Control: every message within both bounds", 100, 4, 4},
        {"message count", 1, WRITE_BATCH_MESSAGES + 1, WRITE_BATCH_MESSAGES},
        // The message that reaches WRITE_BATCH_BYTES is the write's last
        {"bytes, reached exactly", WRITE_BATCH_BYTES / 4, 6, 4},
        {"bytes, passed", 1000, 6, (WRITE_BATCH_BYTES + 999) / 1000},
        {"bytes, one message past them", WRITE_BATCH_BYTES + 1, 2, 1},
    };
    for (const Row& row : rows) {
        SCOPED_TRACE(row.name);
        this->accepted.clear();
        auto listener = this->listen();
        const uint16_t port = listener->port();
        std::vector<std::vector<uint8_t>> received;

        Peer peer(this->io, [&] {
            asio::io_context peer_io;
            PeerSocket ws(peer_io);
            connect_peer(ws, port);
            for (size_t i = 0; i < row.messages; ++i) {
                beast::flat_buffer buffer;
                ws.read(buffer);
                received.push_back(buffer_bytes(buffer));
            }
        });
        this->run_until([&] { return !this->accepted.empty(); });
        Connection& connection = this->accepted.front();
        TestHandler handler(connection);
        connection.start(handler);
        // Each message distinct, so the peer's order shows
        for (size_t i = 0; i < row.messages; ++i) {
            std::vector<uint8_t> message = pattern(row.message_size);
            message[0] = static_cast<uint8_t>(i);
            handler.send(MessageKind::BINARY, message);
        }
        this->run_until([&] { return peer.done() && handler.messages_done == row.messages; });

        ASSERT_EQ(peer.error(), "");
        std::vector<size_t> writes{row.taken};
        if (row.taken < row.messages) {
            writes.push_back(row.messages - row.taken);
        }
        EXPECT_EQ(handler.write_sizes, writes);
        ASSERT_EQ(received.size(), row.messages);
        for (size_t i = 0; i < row.messages; ++i) {
            EXPECT_EQ(received[i].size(), row.message_size);
            EXPECT_EQ(received[i][0], static_cast<uint8_t>(i)) << i;
        }
    }
}

TEST_F(TransportTest, WriteStallClosesOnlyAPeerThatStopsReading) {
    struct Row {
        const char* name;
        bool peer_reads;
        int lowat;
        size_t message_size;
        int messages;
        // When stalled: a write outstanding, rather than a wait for room
        bool write_stalls{false};
        size_t per_write{SIZE_MAX};
    };
    const Row rows[] = {
        {"Control: peer reads", true, 16384, 65000, 64},
        // With no low-water mark a completed small write is the room for the next, with no wait:
        // writes of WRITE_BATCH_MESSAGES messages of 64 bytes
        {"Control: peer reads, no low-water mark", true, 0, 64, 4000},
        // Each write of small messages completes; the wait for room is what never ends
        {"peer stops reading, wait stalls", false, 16384, 1000, 4000},
        // One message far larger than the socket buffers
        {"peer stops reading, write stalls", false, 0, 16 * 1024 * 1024, 1, true},
        // Small writes with no wait between them, until one fills the socket buffers and stays
        {"peer stops reading, a later small write stalls", false, 0, 1000, 64000, true, 1},
        // Past ROOM_AFTER_WRITE_MAX_BYTES each write is followed by a wait, which never ends: one
        // message to a write, which a socket reporting room always takes whole
        {"peer stops reading, a wait after a larger write stalls", false, 0, 2000, 32000, false, 1},
    };
    for (const Row& row : rows) {
        SCOPED_TRACE(row.name);
        // A reading peer's thread can be starved past a short timer on a loaded machine; that a
        // slow reader outlives it is AHealthyPeerOutlivesTheStallTimeout's to show
        this->settings.write_stall_us = row.peer_reads ? 30000000 : 500000;
        this->settings.notsent_lowat_bytes = row.lowat;
        this->accepted.clear();
        auto listener = this->listen();
        const uint16_t port = listener->port();
        Gate release;
        int peer_received = 0;

        Peer peer(this->io, [&] {
            asio::io_context peer_io;
            PeerSocket ws(peer_io);
            connect_peer(ws, port, "/sendspin", 16384);
            if (!row.peer_reads) {
                release.wait();
                return;
            }
            for (int i = 0; i < row.messages; ++i) {
                beast::flat_buffer buffer;
                ws.read(buffer);
                ++peer_received;
            }
            ws.close(websocket::close_code::normal);
        });
        const OpenOnExit open_release(release);
        this->run_until([&] { return !this->accepted.empty(); });
        Connection& connection = this->accepted.front();
        TestHandler handler(connection);
        handler.per_write = row.per_write;
        connection.start(handler);
        // As fast as room allows
        const std::vector<uint8_t> message = pattern(row.message_size);
        for (int i = 0; i < row.messages; ++i) {
            handler.send(MessageKind::BINARY, message);
        }

        if (row.peer_reads) {
            this->run_until([&] { return peer.done() && !handler.closes.empty(); });
            EXPECT_EQ(handler.messages_done, static_cast<size_t>(row.messages));
            EXPECT_EQ(peer_received, row.messages);
            EXPECT_EQ(handler.closes, std::vector<CloseReason>{CloseReason::PEER_CLOSED});
        } else {
            this->run_until([&] { return !handler.closes.empty(); });
            EXPECT_EQ(handler.closes, std::vector<CloseReason>{CloseReason::WRITE_STALLED});
            EXPECT_FALSE(connection.is_open());
            EXPECT_LT(handler.messages_done, static_cast<size_t>(row.messages));
            EXPECT_EQ(handler.writes_started, handler.write_done + (row.write_stalls ? 1 : 0));
            EXPECT_EQ(handler.write_done > 0, row.messages > 1);
            release.open();
            this->run_until([&] { return peer.done(); });
        }
        ASSERT_EQ(peer.error(), "");
    }
}

// The write-stall timer stays armed across completed I/O: a peer that keeps reading while the
// connection writes for three times write_stall_us is never closed, under a low-water mark and with
// none (where each small write's completion is the room for the next)
TEST_F(TransportTest, AHealthyPeerOutlivesTheStallTimeout) {
    constexpr int64_t STALL_US = 200000;
    this->settings.write_stall_us = STALL_US;
    for (const int lowat : {16384, 0}) {
        SCOPED_TRACE(lowat);
        this->settings.notsent_lowat_bytes = lowat;
        this->accepted.clear();
        auto listener = this->listen();
        const uint16_t port = listener->port();
        Peer peer(this->io, [&] {
            asio::io_context peer_io;
            PeerSocket ws(peer_io);
            connect_peer(ws, port);
            for (;;) {
                beast::flat_buffer buffer;
                error_code ec;
                ws.read(buffer, ec);
                if (ec) {
                    return;
                }
            }
        });
        this->run_until([&] { return !this->accepted.empty(); });
        Connection& connection = this->accepted.front();
        TestHandler handler(connection);
        connection.start(handler);
        const std::vector<uint8_t> message = pattern(1000);
        size_t sent = 0;
        const int64_t end = now_us() + (3 * STALL_US);
        this->run_until([&] {
            while (sent - handler.messages_done < 16) {
                handler.send(MessageKind::BINARY, message);
                ++sent;
            }
            return now_us() >= end || !handler.closes.empty();
        });
        EXPECT_TRUE(handler.closes.empty());
        EXPECT_TRUE(connection.is_open());
        EXPECT_GT(handler.messages_done, 100U);
        connection.close(close_code::NORMAL);
        this->run_until([&] { return peer.done(); });
        ASSERT_EQ(peer.error(), "");
    }
}

// Neither a completed write nor an on_writable() that wrote nothing leaves a stall deadline
// behind, with a low-water mark and with none (where the room after the write is reported in a
// deferred handler)
TEST_F(TransportTest, CompletedWaitsAndWritesLeaveNoStallDeadline) {
    this->settings.write_stall_us = 200000;
    for (const int lowat : {16384, 0}) {
        SCOPED_TRACE(lowat);
        this->settings.notsent_lowat_bytes = lowat;
        this->accepted.clear();
        auto listener = this->listen();
        const uint16_t port = listener->port();
        int peer_received = 0;

        Peer peer(this->io, [&] {
            asio::io_context peer_io;
            PeerSocket ws(peer_io);
            connect_peer(ws, port);
            for (;;) {
                beast::flat_buffer buffer;
                error_code ec;
                ws.read(buffer, ec);
                if (ec) {
                    return;
                }
                ++peer_received;
            }
        });
        this->run_until([&] { return !this->accepted.empty(); });
        Connection& connection = this->accepted.front();
        TestHandler handler(connection);
        connection.start(handler);
        // Sits idle for twice the stall timeout
        auto idle = [this] { this->run_for(2 * 200000); };

        handler.send(MessageKind::BINARY, bytes_of("one"));
        this->run_until([&] { return handler.write_done == 1; });
        idle();
        EXPECT_TRUE(handler.closes.empty()) << "after a completed write";

        bool writable = false;
        handler.writable_hook = [&] { writable = true; };
        ASSERT_TRUE(connection.wait_writable());
        this->run_until([&] { return writable; });
        idle();
        EXPECT_TRUE(handler.closes.empty()) << "after an on_writable() that wrote nothing";
        EXPECT_TRUE(connection.is_open());

        connection.close(close_code::NORMAL);
        this->run_until([&] { return peer.done(); });
        ASSERT_EQ(peer.error(), "");
        EXPECT_EQ(peer_received, 1);
    }
}

// ==============================================================================
// Close
// ==============================================================================

TEST_F(TransportTest, ExplicitCloseSendsTheCodeAndReportsNothing) {
    auto listener = this->listen();
    const uint16_t port = listener->port();
    error_code peer_error;
    uint16_t peer_close_code = 0;

    Peer peer(this->io, [&] {
        asio::io_context peer_io;
        PeerSocket ws(peer_io);
        connect_peer(ws, port);
        beast::flat_buffer buffer;
        ws.read(buffer, peer_error);
        peer_close_code = ws.reason().code;
    });
    this->run_until([&] { return !this->accepted.empty(); });
    Connection& connection = this->accepted.front();
    // Deleted right after close(): any later event would be a use after free under ASan
    auto handler = std::make_unique<TestHandler>(connection);
    connection.start(*handler);
    ASSERT_TRUE(connection.wait_writable());
    constexpr uint16_t APPLICATION_CODE = 4001;
    connection.close(APPLICATION_CODE);
    handler.reset();
    EXPECT_FALSE(connection.is_open());
    EXPECT_FALSE(connection.wait_writable());
    this->run_until([&] { return peer.done(); });
    while (this->io.poll() > 0) {}

    ASSERT_EQ(peer.error(), "");
    EXPECT_EQ(peer_error, websocket::error::closed);
    EXPECT_EQ(peer_close_code, APPLICATION_CODE);
}

// A close inside on_writable() drops the messages written there: the peer gets the close frame
// alone, and nothing is reported
TEST_F(TransportTest, CloseInsideOnWritableDropsTheMessagesWrittenThere) {
    auto listener = this->listen();
    const uint16_t port = listener->port();
    error_code peer_error;
    uint16_t peer_close_code = 0;

    Peer peer(this->io, [&] {
        asio::io_context peer_io;
        PeerSocket ws(peer_io);
        connect_peer(ws, port);
        beast::flat_buffer buffer;
        ws.read(buffer, peer_error);
        peer_close_code = ws.reason().code;
    });
    this->run_until([&] { return !this->accepted.empty(); });
    Connection& connection = this->accepted.front();
    TestHandler handler(connection);
    bool written = false;
    handler.writable_hook = [&] {
        written = connection.write(MessageKind::TEXT, bytes_of("one")) &&
                  connection.write(MessageKind::BINARY, pattern(100));
        connection.close(close_code::NORMAL);
    };
    connection.start(handler);
    ASSERT_TRUE(connection.wait_writable());
    this->run_until([&] { return peer.done(); });
    while (this->io.poll() > 0) {}

    ASSERT_EQ(peer.error(), "");
    EXPECT_TRUE(written);
    EXPECT_EQ(peer_error, websocket::error::closed);
    EXPECT_EQ(peer_close_code, websocket::close_code::normal);
    EXPECT_EQ(handler.write_done, 0);
    EXPECT_TRUE(handler.closes.empty());
}

TEST_F(TransportTest, PeerCloseIsReportedOnceAndEndsTheConnection) {
    auto listener = this->listen();
    const uint16_t port = listener->port();
    Peer peer(this->io, [&] {
        asio::io_context peer_io;
        PeerSocket ws(peer_io);
        connect_peer(ws, port);
        ws.close(websocket::close_code::normal);
    });
    this->run_until([&] { return !this->accepted.empty(); });
    Connection& connection = this->accepted.front();
    TestHandler handler(connection);
    connection.start(handler);
    this->run_until([&] { return peer.done() && !handler.closes.empty(); });
    while (this->io.poll() > 0) {}

    ASSERT_EQ(peer.error(), "");
    EXPECT_EQ(handler.closes, std::vector<CloseReason>{CloseReason::PEER_CLOSED});
    EXPECT_FALSE(connection.is_open());
    EXPECT_FALSE(connection.wait_writable());
}

// A reset's ECONNRESET reaches reason_for() as an Asio system error, which
// close_reason_for_errno() maps
TEST_F(TransportTest, PeerResetIsReportedAsPeerClosed) {
    auto listener = this->listen();
    const uint16_t port = listener->port();
    Peer peer(this->io, [&] {
        asio::io_context peer_io;
        PeerSocket ws(peer_io);
        connect_peer(ws, port);
        // Linger 0: the close sends RST, not FIN
        tcp::socket& socket = beast::get_lowest_layer(ws);
        socket.set_option(asio::socket_base::linger(true, 0));
        socket.close();
    });
    this->run_until([&] { return !this->accepted.empty(); });
    Connection& connection = this->accepted.front();
    TestHandler handler(connection);
    connection.start(handler);
    this->run_until([&] { return peer.done() && !handler.closes.empty(); });
    while (this->io.poll() > 0) {}

    ASSERT_EQ(peer.error(), "");
    EXPECT_EQ(handler.closes, std::vector<CloseReason>{CloseReason::PEER_CLOSED});
}

TEST_F(TransportTest, CloseInsideOnMessageStopsEveryLaterEvent) {
    auto listener = this->listen();
    const uint16_t port = listener->port();
    uint16_t peer_close_code = 0;

    Peer peer(this->io, [&] {
        asio::io_context peer_io;
        PeerSocket ws(peer_io);
        connect_peer(ws, port);
        ws.text(true);
        ws.write(asio::buffer(std::string("one")));
        ws.write(asio::buffer(std::string("two")));
        beast::flat_buffer buffer;
        error_code ec;
        ws.read(buffer, ec);
        peer_close_code = ws.reason().code;
    });
    this->run_until([&] { return !this->accepted.empty(); });
    Connection& connection = this->accepted.front();
    TestHandler handler(connection);
    handler.message_hook = [&] { connection.close(close_code::NORMAL); };
    connection.start(handler);
    this->run_until([&] { return peer.done(); });
    while (this->io.poll() > 0) {}

    ASSERT_EQ(peer.error(), "");
    ASSERT_EQ(handler.messages.size(), 1U);
    EXPECT_EQ(handler.messages[0].payload, bytes_of("one"));
    EXPECT_TRUE(handler.closes.empty());
    EXPECT_EQ(peer_close_code, websocket::close_code::normal);
}

TEST_F(TransportTest, DroppingAConnectionWithAWaitInFlightIsSafe) {
    this->settings.notsent_lowat_bytes = 16384;
    auto listener = this->listen();
    const uint16_t port = listener->port();
    Gate release;
    error_code peer_error;

    Peer peer(this->io, [&] {
        asio::io_context peer_io;
        PeerSocket ws(peer_io);
        connect_peer(ws, port, "/sendspin", 16384);
        release.wait();
        // Drain until the server's close shows up
        for (;;) {
            beast::flat_buffer buffer;
            ws.read(buffer, peer_error);
            if (peer_error) {
                return;
            }
        }
    });
    const OpenOnExit open_release(release);
    this->run_until([&] { return !this->accepted.empty(); });
    auto handler = std::make_unique<TestHandler>(this->accepted.front());
    this->accepted.front().start(*handler);
    // Small messages under the low-water mark: each write completes and the wait for room is
    // what stays outstanding once the socket fills
    constexpr int MESSAGES = 4000;
    for (int i = 0; i < MESSAGES; ++i) {
        handler->send(MessageKind::BINARY, pattern(1000));
    }
    // Until the socket stays full: no write completes for a while
    int last_done = -1;
    while (handler->write_done != last_done) {
        last_done = handler->write_done;
        this->run_for(100000);
    }
    ASSERT_LT(handler->write_done, MESSAGES);
    ASSERT_EQ(handler->writes_started, handler->write_done);
    // The handle and the handler go away with the wait outstanding
    this->accepted.clear();
    handler.reset();
    release.open();
    this->run_until([&] { return peer.done(); });
    while (this->io.poll() > 0) {}

    ASSERT_EQ(peer.error(), "");
    EXPECT_TRUE(peer_error);
}

TEST_F(TransportTest, DroppingAConnectionWithAWriteInFlightDropsTcp) {
    // No low-water mark and one message far larger than the socket buffers: the write itself
    // stays outstanding
    this->settings.notsent_lowat_bytes = 0;
    // Longer than the suite watchdog, so a close frame queued behind the write would never be
    // cut short by the closing-handshake bound
    this->settings.handshake_timeout_us = 600000000;
    auto listener = this->listen();
    const uint16_t port = listener->port();
    Gate release;
    error_code peer_error;

    Peer peer(this->io, [&] {
        asio::io_context peer_io;
        PeerSocket ws(peer_io);
        ws.read_message_max(64U * 1024U * 1024U);
        connect_peer(ws, port, "/sendspin", 16384);
        release.wait();
        for (;;) {
            beast::flat_buffer buffer;
            ws.read(buffer, peer_error);
            if (peer_error) {
                return;
            }
        }
    });
    const OpenOnExit open_release(release);
    this->run_until([&] { return !this->accepted.empty(); });
    auto handler = std::make_unique<TestHandler>(this->accepted.front());
    this->accepted.front().start(*handler);
    handler->send(MessageKind::BINARY, pattern(16 * 1024 * 1024));
    this->run_until([&] { return handler->writes_started == 1; });
    while (this->io.poll() > 0) {}
    ASSERT_GT(handler->writes_started, handler->write_done);
    this->accepted.clear();
    handler.reset();
    release.open();
    this->run_until([&] { return peer.done(); });
    while (this->io.poll() > 0) {}

    ASSERT_EQ(peer.error(), "");
    // The TCP connection ended without a close frame, mid-message
    EXPECT_NE(peer_error, websocket::error::closed);
    EXPECT_TRUE(peer_error == asio::error::eof || peer_error == asio::error::connection_reset)
        << peer_error.message();
}

TEST_F(TransportTest, UnansweredKeepAlivePingsTimeOut) {
    this->settings.idle_timeout_us = 200000;
    auto listener = this->listen();
    const uint16_t port = listener->port();
    Gate release;

    Peer peer(this->io, [&] {
        asio::io_context peer_io;
        PeerSocket ws(peer_io);
        connect_peer(ws, port);
        // Never reads, so the server's pings go unanswered
        release.wait();
    });
    const OpenOnExit open_release(release);
    this->run_until([&] { return !this->accepted.empty(); });
    TestHandler handler(this->accepted.front());
    this->accepted.front().start(handler);
    this->run_until([&] { return !handler.closes.empty(); });
    release.open();
    this->run_until([&] { return peer.done(); });

    ASSERT_EQ(peer.error(), "");
    EXPECT_EQ(handler.closes, std::vector<CloseReason>{CloseReason::TIMED_OUT});
}

TEST_F(TransportTest, KeepAlivePingsHoldASilentPeerThatAnswersThem) {
    this->settings.idle_timeout_us = 200000;
    auto listener = this->listen();
    const uint16_t port = listener->port();
    bool pinged_past_idle_timeout = false;
    uint16_t peer_close_code = 0;

    Peer peer(this->io, [&] {
        asio::io_context peer_io;
        PeerSocket ws(peer_io);
        connect_peer(ws, port);
        int pings = 0;
        // Pings come at half the idle timeout, so the third arrives after a whole one
        ws.control_callback([&](websocket::frame_type kind, beast::string_view /*payload*/) {
            if (kind == websocket::frame_type::ping && ++pings == 3) {
                this->signal(pinged_past_idle_timeout);
            }
        });
        // Reading answers each ping; the peer sends nothing else
        beast::flat_buffer buffer;
        error_code ec;
        ws.read(buffer, ec);
        peer_close_code = ws.reason().code;
    });
    this->run_until([&] { return !this->accepted.empty(); });
    TestHandler handler(this->accepted.front());
    this->accepted.front().start(handler);
    this->run_until([&] { return pinged_past_idle_timeout || !handler.closes.empty(); });
    this->accepted.front().close(close_code::NORMAL);
    this->run_until([&] { return peer.done(); });

    ASSERT_EQ(peer.error(), "");
    EXPECT_TRUE(pinged_past_idle_timeout);
    EXPECT_TRUE(handler.closes.empty());
    EXPECT_EQ(peer_close_code, websocket::close_code::normal);
}

// ==============================================================================
// Dial failures
// ==============================================================================

TEST_F(TransportTest, DialFailuresAreReportedWithTheirReason) {
    // A port with nothing listening
    uint16_t closed_port = 0;
    {
        asio::io_context probe_io;
        tcp::acceptor probe(probe_io, {LOOPBACK, 0});
        closed_port = probe.local_endpoint().port();
    }
    EXPECT_FALSE(
        this->dial_and_wait(*parse_ws_url("ws://127.0.0.1:" + std::to_string(closed_port) + "/"))
            .has_value());
    EXPECT_EQ(this->dial_failure, CloseReason::FAILED);

    // A peer that accepts the TCP connection and never answers the upgrade
    this->settings.handshake_timeout_us = 200000;
    asio::io_context peer_io;
    tcp::acceptor acceptor(peer_io, {LOOPBACK, 0});
    Peer peer(this->io, [&] {
        tcp::socket socket(peer_io);
        acceptor.accept(socket);
        std::array<uint8_t, 256> buffer{};
        error_code ec;
        while (!ec) {
            socket.read_some(asio::buffer(buffer), ec);
        }
    });
    EXPECT_FALSE(
        this->dial_and_wait(*parse_ws_url("ws://127.0.0.1:" +
                                          std::to_string(acceptor.local_endpoint().port()) + "/"))
            .has_value());
    EXPECT_EQ(this->dial_failure, CloseReason::TIMED_OUT);
    this->run_until([&] { return peer.done(); });
    ASSERT_EQ(peer.error(), "");
}

TEST_F(TransportTest, EachAddressOfADialGetsItsOwnConnectTimeout) {
    // A listener whose accept queue is full and never drains drops a new connect's SYN, so the
    // connect gets no answer. Each probe that completes fills the queue further (macOS takes a
    // backlog of 0 as the system's maximum); half a second tells a loaded machine's slow loopback
    // connect from a dropped one.
    asio::io_context peer_io;
    tcp::acceptor silent(peer_io);
    silent.open(tcp::v4());
    silent.bind({LOOPBACK, 0});
    silent.listen(1);
    const tcp::endpoint silent_endpoint = silent.local_endpoint();
    std::vector<std::unique_ptr<tcp::socket>> probes;
    bool full = false;
    while (!full && probes.size() < 64) {
        probes.push_back(std::make_unique<tcp::socket>(peer_io));
        full = !connects_within(*probes.back(), silent_endpoint, 500);
    }
    ASSERT_TRUE(full);

    // The listener that answers, on the same port over IPv6. The connect timeout also bounds the
    // connect that must succeed, so it leaves a loaded machine a second.
    this->settings.connect_timeout_us = 1000000;
    Listener listener(this->loop, this->settings, [this](Connection connection) {
        this->accepted.push_back(std::move(connection));
    });
    ASSERT_TRUE(listener.listen("::1", silent_endpoint.port()));
    DialTarget target;
    target.host = "127.0.0.1";
    target.port = silent_endpoint.port();
    target.path = "/sendspin";

    // Control: the silent address alone times out in its connect
    target.addresses = {"127.0.0.1"};
    EXPECT_FALSE(this->dial_and_wait(target).has_value());
    EXPECT_EQ(this->dial_failure, CloseReason::TIMED_OUT);

    // Its timeout leaves the next address a connect of its own
    target.addresses = {"127.0.0.1", "::1"};
    const std::optional<Connection> connection = this->dial_and_wait(target);
    ASSERT_TRUE(connection.has_value());
    EXPECT_EQ(connection->remote_endpoint().rfind("[::1]:", 0), 0U)
        << connection->remote_endpoint();
}

TEST_F(TransportTest, ARefusedAddressMovesTheDialToTheNextOfAnotherFamily) {
    // Nothing listens on 127.0.0.1 at the IPv6 listener's port
    const sendspin::server::test::UnlistenedPort unlistened;
    Listener listener(this->loop, this->settings, [this](Connection connection) {
        this->accepted.push_back(std::move(connection));
    });
    ASSERT_TRUE(listener.listen("::1", unlistened.port()));
    DialTarget target;
    target.host = "127.0.0.1";
    target.port = unlistened.port();
    target.path = "/sendspin";
    target.addresses = {"127.0.0.1", "::1"};
    const std::optional<Connection> connection = this->dial_and_wait(target);
    ASSERT_TRUE(connection.has_value());
    EXPECT_EQ(connection->remote_endpoint().rfind("[::1]:", 0), 0U)
        << connection->remote_endpoint();
}

TEST_F(TransportTest, DroppingAPendingDialClosesItWithoutACallback) {
    // Longer than the suite watchdog, so only the cancel can end the peer's read
    this->settings.handshake_timeout_us = 600000000;
    asio::io_context peer_io;
    tcp::acceptor acceptor(peer_io, {LOOPBACK, 0});
    bool peer_accepted = false;
    error_code peer_error;
    Peer peer(this->io, [&] {
        tcp::socket socket(peer_io);
        acceptor.accept(socket);
        this->signal(peer_accepted);
        std::array<uint8_t, 256> buffer{};
        while (!peer_error) {
            socket.read_some(asio::buffer(buffer), peer_error);
        }
    });
    bool called = false;
    std::optional<PendingDial> pending = dial(
        this->loop,
        *parse_ws_url("ws://127.0.0.1:" + std::to_string(acceptor.local_endpoint().port()) + "/"),
        this->settings, [&](sendspin::server::DialResult /*outcome*/) { called = true; });
    this->run_until([&] { return peer_accepted; });
    pending.reset();
    this->run_until([&] { return peer.done(); });
    while (this->io.poll() > 0) {}

    ASSERT_EQ(peer.error(), "");
    EXPECT_EQ(peer_error, asio::error::eof);
    EXPECT_FALSE(called);
}

// ==============================================================================
// Lifetime
// ==============================================================================

// The core loop (its io_context) may go while an upgrade, a dial and a connection core never
// started are in progress on the network thread: it takes the network thread with it, and every
// socket the transport held is closed, which each peer sees as the end of its connection
TEST(TransportLifetime, EverySocketClosesWithTheIoContext) {
    asio::io_context peer_io;
    // Takes the dial's TCP connection into its backlog and never answers it
    tcp::acceptor silent(peer_io, {LOOPBACK, 0});
    PeerSocket upgraded(peer_io);
    tcp::socket idle(peer_io);
    tcp::socket dialed(peer_io);
    {
        sendspin::server::EventLoop loop;
        asio::io_context& io = loop.impl().io();
        TransportSettings settings;
        std::optional<Connection> kept;
        Listener listener(loop, settings,
                          [&](Connection connection) { kept.emplace(std::move(connection)); });
        ASSERT_TRUE(listener.listen("127.0.0.1", 0));
        const uint16_t port = listener.port();
        std::string peer_error;
        std::thread handshake([&] {
            try {
                connect_peer(upgraded, port);
            } catch (const std::exception& e) {
                peer_error = e.what();
            }
        });
        while (!kept) {
            io.run_one();
        }
        handshake.join();
        ASSERT_EQ(peer_error, "");
        // An upgrade in progress: admitted, and no request
        idle.connect({LOOPBACK, port});
        while (listener.upgrades_in_progress() == 0) {
            io.run_one();
        }
        const PendingDial pending = dial(
            loop,
            *parse_ws_url("ws://127.0.0.1:" + std::to_string(silent.local_endpoint().port()) + "/"),
            settings, [](sendspin::server::DialResult /*outcome*/) {});
        silent.accept(dialed);
        kept.reset();
    }

    beast::flat_buffer buffer;
    error_code upgraded_error;
    upgraded.read(buffer, upgraded_error);
    EXPECT_TRUE(upgraded_error) << "the upgraded connection";
    std::array<uint8_t, 1> byte{};
    error_code idle_error;
    asio::read(idle, asio::buffer(byte), idle_error);
    EXPECT_TRUE(idle_error) << "the upgrade in progress";
    std::array<uint8_t, 4096> request{};
    error_code dialed_error;
    while (!dialed_error) {
        dialed.read_some(asio::buffer(request), dialed_error);
    }
    EXPECT_TRUE(dialed_error == asio::error::eof || dialed_error == asio::error::connection_reset)
        << dialed_error.message();
}

}  // namespace
