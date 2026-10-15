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

/// @file test_session_driver_internals.cpp
/// @brief SessionDriver over loopback: a goodbye that arrives while a write is stalled ends the
/// session. Compiled with -fno-access-control (tests/CMakeLists.txt).

#include "backoff.h"
#include "crypto/keys.h"
#include "fragmentation.h"
#include "raw_client.h"
#include "session_driver.h"
#include "transport.h"
#include <gtest/gtest.h>
#include <noise/protocol.h>

#include <chrono>
#include <cstddef>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <variant>

namespace {

using sendspin::server::Connection;
using sendspin::server::Identity;
using sendspin::server::SessionDriver;
using sendspin::server::SessionEnd;
using sendspin::server::SessionObserver;
using sendspin::server::SessionSettings;

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
                 sendspin::server::error_before_activation("url", driver.session().outcome()),
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
