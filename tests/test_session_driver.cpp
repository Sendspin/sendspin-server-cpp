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

/// @file test_session_driver.cpp
/// @brief SessionDriver over loopback: the server dials a sendspin-cpp client through the hello
/// exchange, a raw Beast peer on its own thread sees server/error before the close (traced with its
/// completed write) and a silent close on a timeout, and a goodbye that arrives while a write is in
/// flight ends the session, and an active session's messages go out as many to a write as are
/// ready, but for a time reply, which ends its write. White-box: reads SessionDriver::writing_ and
/// the session's fragment progress (see tests/CMakeLists.txt).

#include "backoff.h"
#include "clock.h"
#include "crypto/keys.h"
#include "fragmentation.h"
#include "messages.h"
#include "raw_client.h"
#include "session_driver.h"
#include "test_ports.h"
#include "transport.h"
#include "transport_asio.h"
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <gtest/gtest.h>
#include <noise/protocol.h>

#include <sendspin/client.h>
#include <sendspin/metadata_role.h>
#include <sendspin/player_role.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <variant>
#include <vector>

namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace websocket = beast::websocket;
using tcp = asio::ip::tcp;

using sendspin::server::Connection;
using sendspin::server::Identity;
using sendspin::server::SessionDriver;
using sendspin::server::SessionEnd;
using sendspin::server::SessionObserver;
using sendspin::server::SessionPhase;
using sendspin::server::SessionSettings;

class RecordingObserver : public SessionObserver {
public:
    void on_ready(SessionDriver& /*driver*/) override {
        ++this->ready;
    }

    void on_closed(SessionDriver& /*driver*/) override {
        ++this->closed;
    }

    int ready{0};
    int closed{0};
};

class AlwaysReady : public sendspin::SendspinNetworkProvider {
public:
    bool is_network_ready() override {
        return true;
    }
};

class SessionDriverTest : public testing::Test {
protected:
    static void SetUpTestSuite() {
        ASSERT_EQ(noise_init_framework(), NOISE_ERROR_NONE);
    }

    SessionDriverTest() {
        this->settings.hello.name = "Driver test server";
    }

    // Runs the io_context until done() holds; a regression hangs into the suite watchdog
    template <typename Predicate>
    void run_until(Predicate done) {
        while (!done()) {
            this->io.run_one();
        }
    }

    // Starts a driver on each accepted connection
    std::unique_ptr<sendspin::server::Listener> listen() {
        auto listener = std::make_unique<sendspin::server::Listener>(
            this->loop, sendspin::server::TransportSettings{}, [this](Connection connection) {
                this->driver = std::make_unique<SessionDriver>(this->loop, std::move(connection),
                                                               this->identity, this->settings,
                                                               this->observer, this->trace);
                this->driver->start();
            });
        EXPECT_TRUE(listener->listen("127.0.0.1", 0));
        return listener;
    }

    sendspin::server::EventLoop loop;
    // The loop's io_context, which holds work until the loop is shut down
    asio::io_context& io{loop.impl().io()};
    Identity identity{*Identity::generate()};
    SessionSettings settings;
    RecordingObserver observer;
    sendspin::server::SessionTrace* trace{nullptr};
    std::unique_ptr<SessionDriver> driver;
};

// Each traced message and completed write, in order: "in", "out" or "written"
class OrderTrace : public sendspin::server::SessionTrace {
public:
    void on_inbound(sendspin::server::MessageKind /*kind*/, std::span<const uint8_t> /*plaintext*/,
                    int64_t /*rx_us*/) override {
        this->events.emplace_back("in");
    }

    void on_outbound(sendspin::server::MessageKind /*kind*/, std::span<const uint8_t> plaintext,
                     int64_t stamp_us, size_t /*audio_bytes*/) override {
        const std::string_view text(reinterpret_cast<const char*>(plaintext.data()),
                                    plaintext.size());
        this->events.emplace_back(
            text.find(R"("type":"server/time")") != std::string_view::npos ? "out time" : "out");
        this->last_out_us = stamp_us;
    }

    void on_written(int64_t done_us) override {
        this->events.emplace_back("written");
        this->written_after_out = done_us >= this->last_out_us;
    }

    std::vector<std::string> events;
    int64_t last_out_us{0};
    bool written_after_out{false};
};

// A raw WebSocket client on its own thread. What it saw reaches the test thread through a
// handler posted to the io_context.
class RawPeer {
public:
    struct Result {
        std::vector<std::string> texts;
        bool closed_by_server{false};
        std::string error;
    };

    RawPeer(asio::io_context& io, uint16_t port, std::optional<std::string> first_message)
        : thread_([this, &io, port, first_message = std::move(first_message)] {
              Result result;
              try {
                  asio::io_context peer_io;
                  websocket::stream<tcp::socket> ws(peer_io);
                  ws.next_layer().connect({asio::ip::make_address("127.0.0.1"), port});
                  ws.handshake("127.0.0.1", "/sendspin");
                  if (first_message) {
                      ws.text(true);
                      ws.write(asio::buffer(*first_message));
                  }
                  for (;;) {
                      beast::flat_buffer buffer;
                      beast::error_code ec;
                      ws.read(buffer, ec);
                      if (ec == websocket::error::closed) {
                          result.closed_by_server = true;
                          break;
                      }
                      if (ec) {
                          result.error = ec.message();
                          break;
                      }
                      result.texts.push_back(beast::buffers_to_string(buffer.data()));
                  }
              } catch (const std::exception& e) {
                  result.error = e.what();
              }
              asio::post(io, [this, result = std::move(result)] { this->result_ = result; });
          }) {}

    ~RawPeer() {
        this->thread_.join();
    }

    RawPeer(const RawPeer&) = delete;
    RawPeer& operator=(const RawPeer&) = delete;

    const std::optional<Result>& result() const {
        return this->result_;
    }

private:
    std::optional<Result> result_;
    std::thread thread_;
};

// connection.md "Failure Handling": server/error, then the close
TEST_F(SessionDriverTest, WritesServerErrorBeforeClosing) {
    const std::unique_ptr<sendspin::server::Listener> listener = this->listen();
    RawPeer peer(this->io, listener->port(), R"({"type":"client/init","payload":{"version":2}})");
    this->run_until([&] { return peer.result().has_value(); });

    EXPECT_EQ(peer.result()->error, "");
    EXPECT_TRUE(peer.result()->closed_by_server);
    ASSERT_EQ(peer.result()->texts.size(), 1U);
    EXPECT_EQ(peer.result()->texts[0], sendspin::server::build_server_error(
                                           sendspin::server::InitError::UNSUPPORTED_VERSION));
    ASSERT_TRUE(this->driver);
    EXPECT_EQ(this->driver->session().end(), SessionEnd::INIT_REJECTED);
    EXPECT_FALSE(this->driver->transport_close());
    EXPECT_EQ(this->observer.closed, 1);
    EXPECT_EQ(this->observer.ready, 0);
}

// The trace hears of each completed write after the message it wrote, so a host can tell what was
// on the wire before a message it received
TEST_F(SessionDriverTest, TracesEachCompletedWriteAfterItsMessage) {
    OrderTrace order;
    this->trace = &order;
    const std::unique_ptr<sendspin::server::Listener> listener = this->listen();
    RawPeer peer(this->io, listener->port(), R"({"type":"client/init","payload":{"version":2}})");
    this->run_until([&] { return peer.result().has_value() && this->observer.closed == 1; });

    ASSERT_EQ(peer.result()->texts.size(), 1U);
    EXPECT_EQ(order.events, (std::vector<std::string>{"in", "out", "written"}));
    EXPECT_TRUE(order.written_after_out);
}

// connection.md "Failure Handling": a handshake timeout closes without a message
TEST_F(SessionDriverTest, ClosesSilentlyWhenTheClientInitNeverComes) {
    this->settings.step_timeout_us = 1000;
    const std::unique_ptr<sendspin::server::Listener> listener = this->listen();
    RawPeer peer(this->io, listener->port(), std::nullopt);
    this->run_until([&] { return peer.result().has_value(); });

    EXPECT_EQ(peer.result()->error, "");
    EXPECT_TRUE(peer.result()->closed_by_server);
    EXPECT_TRUE(peer.result()->texts.empty());
    ASSERT_TRUE(this->driver);
    EXPECT_EQ(this->driver->session().end(), SessionEnd::TIMED_OUT);
    EXPECT_EQ(this->observer.closed, 1);
}

// The server dials a sendspin-cpp client, which runs on its own threads, and both complete the
// handshake and the hello exchange
TEST_F(SessionDriverTest, CompletesTheHelloExchangeWithASendspinCppClient) {
    sendspin::SendspinClientConfig config;
    config.name = "sendspin-cpp peer";
    config.server_port = sendspin::server::test::free_port();
    sendspin::SendspinClient client(config);
    sendspin::PlayerRoleConfig player;
    // One format, assigned by count: GCC 13 -Warray-bounds misreads an initializer-list assignment
    player.audio_formats.assign(
        1, sendspin::AudioSupportedFormatObject{sendspin::SendspinCodecFormat::PCM, 2, 48000, 16});
    client.add_player(player);
    client.add_metadata();
    AlwaysReady network;
    client.set_network_provider(&network);
    client.set_unpaired_access_enabled(true);
    ASSERT_TRUE(client.start());

    std::optional<Connection> connection;
    sendspin::server::DialTarget target;
    target.host = "127.0.0.1";
    target.port = config.server_port;
    target.path = "/sendspin";
    target.addresses = {"127.0.0.1"};
    const sendspin::server::PendingDial pending =
        sendspin::server::dial(this->loop, target, sendspin::server::TransportSettings{},
                               [&](sendspin::server::DialResult result) {
                                   ASSERT_TRUE(std::holds_alternative<Connection>(result));
                                   this->driver = std::make_unique<SessionDriver>(
                                       this->loop, std::move(std::get<Connection>(result)),
                                       this->identity, this->settings, this->observer);
                                   this->driver->start();
                               });

    // The client's main loop runs on this thread too
    while (this->observer.ready == 0 && this->observer.closed == 0) {
        client.loop();
        this->io.run_for(std::chrono::milliseconds(2));
    }

    ASSERT_EQ(this->observer.ready, 1);
    EXPECT_EQ(this->observer.closed, 0);
    const sendspin::server::Session& session = this->driver->session();
    EXPECT_EQ(session.phase(), SessionPhase::READY_FOR_ACTIVATION);
    ASSERT_TRUE(session.client_init());
    EXPECT_EQ(session.client_init()->client_id, client.client_id());
    // The client sends client/hello only once it has decrypted and accepted server/hello
    ASSERT_TRUE(session.client_hello());
    const sendspin::server::ClientHello& hello = *session.client_hello();
    EXPECT_EQ(hello.name, "sendspin-cpp peer");
    EXPECT_NE(std::find(hello.supported_roles.begin(), hello.supported_roles.end(), "player@v1"),
              hello.supported_roles.end());
    EXPECT_NE(std::find(hello.supported_roles.begin(), hello.supported_roles.end(), "metadata@v1"),
              hello.supported_roles.end());
    ASSERT_TRUE(hello.player_v1_support);
    ASSERT_EQ(hello.player_v1_support->supported_formats.size(), 1U);
    EXPECT_EQ(hello.player_v1_support->supported_formats[0],
              (sendspin::server::AudioFormat{sendspin::server::Codec::PCM, 2, 48000, 16}));
    EXPECT_TRUE(hello.pair_methods.pairing_psk);
    EXPECT_TRUE(hello.unpaired_access_enabled);
    EXPECT_TRUE(hello.unrecognized_identifiers.empty());

    client.stop();
}

// A driver the server dials to a RawClient, run on a core thread of its own, through an activation
// with no roles. on_core() runs a step on the core thread and waits for it.
class ActiveDriver {
public:
    class Observer : public SessionObserver {
    public:
        void on_ready(SessionDriver& /*driver*/) override {
            this->ready = true;
        }

        void on_closed(SessionDriver& /*driver*/) override {}

        bool ready{false};
    };

    explicit ActiveDriver(SessionSettings settings) : settings_(std::move(settings)) {
        this->core_thread_ = std::thread([this] { this->loop_.run(); });
    }

    ~ActiveDriver() {
        this->loop_.post([this] {
            this->driver_.reset();
            this->pending_.reset();
            this->loop_.shutdown(0);
        });
        this->core_thread_.join();
    }

    ActiveDriver(const ActiveDriver&) = delete;
    ActiveDriver& operator=(const ActiveDriver&) = delete;

    template <typename Fn>
    auto on_core(Fn fn) {
        std::packaged_task<decltype(fn())()> task(std::move(fn));
        auto result = task.get_future();
        this->loop_.post([&task] { task(); });
        return result.get();
    }

    // Dials, completes the handshake and hello exchange, and activates
    void activate() {
        this->on_core([this] {
            this->pending_.emplace(sendspin::server::dial(
                this->loop_, *sendspin::server::parse_ws_url(this->client.url()),
                sendspin::server::TransportSettings{}, [this](sendspin::server::DialResult result) {
                    this->driver_ = std::make_unique<SessionDriver>(
                        this->loop_, std::move(std::get<Connection>(result)), this->identity_,
                        this->settings_, this->observer_, &this->trace);
                    this->driver_->start();
                }));
        });
        this->client.handshake();
        this->client.send_json(sendspin::server::test::client_hello_json());
        while (!this->on_core([this] { return this->observer_.ready; })) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        this->on_core([this] {
            this->driver_->activate(
                sendspin::server::ServerActivate{false, std::vector<std::string>{}, std::nullopt});
        });
        const std::string activation = this->client.receive_json();
        ASSERT_NE(activation.find("server/activate"), std::string::npos) << activation;
        // Until the activation's write has completed and nothing more is ready
        while (!this->on_core([this] {
            return !this->driver_->writing_ && !this->driver_->waiting_ &&
                   !this->driver_->session().has_message();
        })) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    // Queues each JSON message, then pumps
    void queue(const std::vector<std::string>& messages) {
        this->on_core([&] {
            for (const std::string& json : messages) {
                ASSERT_TRUE(this->driver_->session().queue_json(json));
            }
            this->driver_->pump();
        });
    }

    // The trace's events from index on, once there are at least count of them
    std::vector<std::string> events_from(size_t index, size_t count) {
        for (;;) {
            std::vector<std::string> events = this->on_core([this] { return this->trace.events; });
            if (events.size() >= index + count) {
                return {events.begin() + static_cast<std::ptrdiff_t>(index), events.end()};
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    size_t event_count() {
        return this->on_core([this] { return this->trace.events.size(); });
    }

    sendspin::server::test::RawClient client;
    // Core thread only
    OrderTrace trace;

private:
    sendspin::server::EventLoop loop_;
    const Identity identity_{*Identity::generate()};
    const SessionSettings settings_;
    Observer observer_;
    std::unique_ptr<SessionDriver> driver_;
    std::optional<sendspin::server::PendingDial> pending_;
    std::thread core_thread_;
};

std::vector<std::string> numbered_messages(size_t count) {
    std::vector<std::string> messages;
    for (size_t i = 0; i < count; ++i) {
        messages.push_back(R"({"type":"test/numbered","payload":{"n":)" + std::to_string(i) + "}}");
    }
    return messages;
}

// Messages ready together go out in one write, each encrypted as a message of its own, which the
// client decrypts in order
TEST(SessionDriverWrites, AWriteCarriesEveryMessageReadyAsItStarts) {
    ASSERT_EQ(noise_init_framework(), NOISE_ERROR_NONE);
    ActiveDriver active{SessionSettings{}};
    ASSERT_NO_FATAL_FAILURE(active.activate());
    const size_t before = active.event_count();
    const std::vector<std::string> messages = numbered_messages(4);
    active.queue(messages);
    for (const std::string& expected : messages) {
        EXPECT_EQ(active.client.receive_json(), expected);
    }

    const std::vector<std::string> one_write = {"out", "out", "out", "out", "written"};
    EXPECT_EQ(active.events_from(before, one_write.size()), one_write);
}

// messaging.md "Transmit timestamps": a time reply ends its write, so no message is built between
// its stamp and its write, while the messages ready beside it follow in the next write. The reply
// is taken with them ready: the client sends its request while a message larger than the socket
// buffers keeps the server writing, and reads only after.
TEST(SessionDriverWrites, ATimeReplyEndsItsWrite) {
    ASSERT_EQ(noise_init_framework(), NOISE_ERROR_NONE);
    constexpr size_t BIG_BYTES = size_t{16} * 1024 * 1024;
    SessionSettings settings;
    settings.queue_caps.control_bytes = 2 * BIG_BYTES;
    ActiveDriver active{settings};
    active.client.fix_receive_buffer(16384);
    ASSERT_NO_FATAL_FAILURE(active.activate());
    const size_t before = active.event_count();
    std::vector<std::string> messages{R"({"type":"test/big","payload":")" +
                                      std::string(BIG_BYTES, 'b') + R"("})"};
    const std::vector<std::string> numbered = numbered_messages(4);
    messages.insert(messages.end(), numbered.begin(), numbered.end());
    active.queue(messages);
    active.client.send_json(sendspin::server::test::client_time_json(1));
    EXPECT_NE(active.client.receive_until("server/time"), "");
    for (const std::string& expected : numbered) {
        EXPECT_EQ(active.client.receive_json(), expected);
    }

    const std::vector<std::string> events = active.events_from(before, 0);
    const auto reply = std::find(events.begin(), events.end(), "out time");
    ASSERT_NE(reply, events.end());
    ASSERT_NE(reply + 1, events.end());
    EXPECT_EQ(*(reply + 1), "written");
    // The numbered messages, after the reply
    EXPECT_EQ(std::count(reply, events.end(), "out"), 4);
}

// messaging.md "client/goodbye": a goodbye that arrives while a write is stalled, from a client
// that closes right after it without reading, ends the session before the transport can report
// the close, so the connection is reported as the goodbye
TEST(SessionDriverGoodbye, EndsWithAGoodbyeThatArrivesWhileAWriteIsStalled) {
    ASSERT_EQ(noise_init_framework(), NOISE_ERROR_NONE);
    struct Closed {
        std::optional<SessionEnd> end;
        std::optional<sendspin::server::CloseReason> transport;
        sendspin::server::ServerError error;
        bool writing{false};
    };
    class Observer : public SessionObserver {
    public:
        void on_ready(SessionDriver& /*driver*/) override {
            ADD_FAILURE() << "ready before client/hello";
        }

        void on_closed(SessionDriver& driver) override {
            this->closed.set_value(
                {driver.session().end(), driver.transport_close(),
                 sendspin::server::error_before_activation("url", driver.session()),
                 driver.writing_});
        }

        std::promise<Closed> closed;
    };

    for (const bool half_close : {true, false}) {
        SCOPED_TRACE(half_close ? "half-close" : "close frame");
        sendspin::server::EventLoop loop;
        const Identity identity = *Identity::generate();
        SessionSettings settings;
        // server/hello far larger than the socket buffers can grow to, in fragments larger than
        // the room the socket reports under the default low-water mark, so that a write stays in
        // flight. Each language tag stays within ArduinoJson's string limit.
        constexpr size_t TAG_BYTES = 60000;
        constexpr size_t TAGS = 280;
        settings.hello.languages.assign(TAGS, std::string(TAG_BYTES, 'l'));
        settings.queue_caps.control_bytes = 2 * TAGS * TAG_BYTES;
        settings.max_fragment_plaintext = sendspin::server::MAX_TRANSPORT_PLAINTEXT;
        sendspin::server::TransportSettings transport;
        transport.notsent_lowat_bytes = 0;
        Observer observer;
        std::unique_ptr<SessionDriver> driver;
        std::optional<sendspin::server::PendingDial> pending;
        std::thread core_thread([&] { loop.run(); });
        // Ends the loop and joins its thread on every exit, a failed assertion's included
        struct Stop {
            std::function<void()> stop;
            ~Stop() {
                this->stop();
            }
        };
        const Stop stop{[&] {
            loop.post([&] {
                driver.reset();
                pending.reset();
                loop.shutdown(0);
            });
            core_thread.join();
        }};

        sendspin::server::test::RawClient client;
        loop.post([&] {
            pending.emplace(
                sendspin::server::dial(loop, *sendspin::server::parse_ws_url(client.url()),
                                       transport, [&](sendspin::server::DialResult result) {
                                           driver = std::make_unique<SessionDriver>(
                                               loop, std::move(std::get<Connection>(result)),
                                               identity, settings, observer);
                                           driver->start();
                                       }));
        });
        client.accept();
        client.send_client_init();
        ASSERT_NO_FATAL_FAILURE(client.receive_message_1());
        client.send_message_2();

        // The client reads nothing, so once the socket buffers stop growing the writes stall: a
        // write in flight (macOS) or a wait for room (Linux) seen twice, with no fragment taken in
        // between
        struct Progress {
            bool stalled{false};
            size_t taken{0};
        };
        const auto inspect = [&] {
            std::promise<Progress> inspected;
            loop.post([&] {
                Progress progress;
                if (driver) {
                    const auto& fragmenting = driver->session().queues_.fragmenting_;
                    progress = {driver->writing_ || driver->waiting_,
                                fragmenting ? fragmenting->offset : 0};
                }
                inspected.set_value(progress);
            });
            return inspected.get_future().get();
        };
        for (;;) {
            const Progress first = inspect();
            std::this_thread::sleep_for(std::chrono::milliseconds(first.stalled ? 200 : 1));
            const Progress second = inspect();
            if (first.stalled && second.stalled && first.taken == second.taken) {
                break;
            }
        }
        client.send_json(sendspin::server::test::client_goodbye_json("restart"));
        if (half_close) {
            client.half_close();
        } else {
            client.close();
        }

        const Closed closed = observer.closed.get_future().get();
        EXPECT_EQ(closed.end, SessionEnd::GOODBYE);
        if (half_close) {
            // The driver closes the connection itself only once no write is in flight, so a write
            // still in flight at the close leaves it to the transport, after the goodbye
            EXPECT_EQ(closed.transport.has_value(), closed.writing);
        }
        EXPECT_EQ(closed.error.kind, sendspin::server::ErrorKind::GOODBYE_BEFORE_ACTIVATION);
        EXPECT_EQ(closed.error.goodbye_reason, sendspin::server::GoodbyeReason::RESTART);
    }
}

}  // namespace
