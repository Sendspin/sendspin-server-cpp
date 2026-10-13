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

/// @file transport.h
/// @brief WebSocket connections, accepted and dialed, that deliver whole messages, write
/// readiness and close events, and the event loop they run on. Everything here is called on the
/// loop's thread (the core thread), which receives every event, and is not thread-safe, except
/// EventLoop::post() and Wakeup::signal(). The sockets are read and written on threads of the
/// transport's own, so a message's receive stamp never waits for core: on the host, a network
/// thread per loop on Asio and Beast (transport.cpp), started by its first Listener or dial() and
/// stopped with it; on ESP-IDF, FreeRTOS tasks on lwIP's sockets and esp_http_server (src/esp/),
/// with the differences noted below.

#pragma once

#include "shared.h"
#include "task_queue.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace sendspin::server {

class EventLoop;

/// @brief The type of a WebSocket data message
enum class MessageKind : uint8_t { TEXT, BINARY };

/// @brief Why a connection closed, as reported to ConnectionHandler::on_closed()
enum class CloseReason : uint8_t {
    PEER_CLOSED,    ///< The peer closed the WebSocket or dropped the TCP connection
    TIMED_OUT,      ///< A connect, handshake or idle timeout (keep-alive pings unanswered)
    WRITE_STALLED,  ///< A wait for room or a write stayed outstanding for write_stall_us
    /// The peer broke the WebSocket protocol or sent a message over read_message_max
    PROTOCOL_VIOLATION,
    FAILED,  ///< Anything else
};

/// @brief WebSocket close codes the server sends (RFC 6455 section 7.4.1)
namespace close_code {
constexpr uint16_t NORMAL = 1000;
constexpr uint16_t GOING_AWAY = 1001;
constexpr uint16_t PROTOCOL_ERROR = 1002;
constexpr uint16_t INVALID_PAYLOAD = 1007;
}  // namespace close_code

/// @brief The largest Noise message (Noise Protocol Framework section 3), and so the largest
/// WebSocket message either side sends
constexpr size_t MAX_MESSAGE_BYTES = 65535;

/// @brief The largest write whose completion is room for the next (wait_writable()); a chunk may
/// fill the send buffer, and a wait after it lets a time reply arriving meanwhile go first
constexpr size_t ROOM_AFTER_WRITE_MAX_BYTES = 1024;

/// @brief The most messages one write carries: four analysis instants' visualizer messages, one per
/// type the server streams (visualizer_type::STREAMABLE), or fewer beside control traffic
constexpr size_t WRITE_BATCH_MESSAGES = 16;
/// @brief A message joins a write only while the messages before it total fewer bytes: four
/// instants at the largest spectrum (MAX_DISPLAY_BINS) fit, encrypted, and a chunk goes last. Past
/// ROOM_AFTER_WRITE_MAX_BYTES, a write's completion is not room for the next.
constexpr size_t WRITE_BATCH_BYTES = 4096;

/// @brief Whether a write holding count messages of bytes in all takes another
constexpr bool write_batch_has_room(size_t count, size_t bytes) {
    return count < WRITE_BATCH_MESSAGES && bytes < WRITE_BATCH_BYTES;
}

constexpr int64_t DEFAULT_HANDSHAKE_TIMEOUT_US = 30000000;
constexpr int64_t DEFAULT_IDLE_TIMEOUT_US = 60000000;
constexpr int64_t DEFAULT_CONNECT_TIMEOUT_US = 10000000;
/// @brief The default write_stall_us
constexpr int64_t DEFAULT_WRITE_STALL_US = 15000000;

/// @brief Settings for every connection a Listener accepts or dial() makes
struct TransportSettings {
    /// Request path an accepted upgrade must name (the query string is ignored). An upgrade to
    /// another path gets 404; a request that is not an upgrade gets 400 whatever its path (on
    /// ESP-IDF, 404 for another path either way).
    std::string path{"/sendspin"};
    /// Beast's write buffer size; a masked message (on a dialed connection) up to this size goes
    /// to the socket in one send. Raised to Beast's minimum when below it. Ignored on ESP-IDF,
    /// where every message is one frame and a write's frames go in one send.
    size_t write_buffer_bytes{MAX_MESSAGE_BYTES};
    /// Largest inbound message, nonzero and at most MAX_MESSAGE_BYTES (a larger value is lowered
    /// to it); a larger message closes the connection
    size_t read_message_max{MAX_MESSAGE_BYTES};
    /// `TCP_NOTSENT_LOWAT` in bytes, or 0 for the system default. Linux reports room only once
    /// the unsent bytes fall below half of it; macOS below all of it. Ignored on ESP-IDF, whose
    /// lwIP has no such option: room for a write is the send buffer's, and a completed small
    /// write leaves room (wait_writable()).
    int notsent_lowat_bytes{32768};
    /// Limit for the HTTP upgrade, the WebSocket handshake and the closing handshake; positive.
    /// On ESP-IDF a dial's connect and upgrade share one limit, this and connect_timeout_us
    /// together.
    int64_t handshake_timeout_us{DEFAULT_HANDSHAKE_TIMEOUT_US};
    /// Close after this long without any inbound frame, with keep-alive pings sent at half of
    /// it; 0 for no idle timeout
    int64_t idle_timeout_us{DEFAULT_IDLE_TIMEOUT_US};
    /// Limit for a dial's TCP connect; positive
    int64_t connect_timeout_us{DEFAULT_CONNECT_TIMEOUT_US};
    /// Close when a wait for room or a write stays outstanding this long; positive
    int64_t write_stall_us{DEFAULT_WRITE_STALL_US};
};

/// @brief Receives a started Connection's events, on the loop's thread. Any of them may
/// call the connection's methods or destroy it.
class ConnectionHandler {
public:
    virtual ~ConnectionHandler() = default;

    /// @brief One complete inbound message, valid only during the call
    /// @param rx_us now_us() read first by the thread reading the socket once the message's last
    /// byte is in (spec messaging.md "Receive timestamps"), however late core gets to it
    virtual void on_message(MessageKind kind, std::span<const uint8_t> payload, int64_t rx_us) = 0;

    /// @brief The socket has room for one write, of the messages write() takes during this call
    virtual void on_writable() = 0;

    /// @brief Every message of the write is fully handed to the kernel
    virtual void on_write_done() = 0;

    /// @brief The connection failed or the peer closed it. The last event; close() reports none.
    virtual void on_closed(CloseReason reason) = 0;
};

/// @brief One WebSocket connection, made by a Listener or dial(). The handle owns it: dropping
/// the handle closes it with close_code::GOING_AWAY, and no event follows. Outstanding
/// operations keep the internals alive until they complete.
///
/// Write invariant: wait_writable() asks for one on_writable(); write() is accepted only inside
/// on_writable(), for as long as can_write() holds, and the messages it takes there go out as one
/// write once on_writable() returns, each its own WebSocket frame, in order; the next
/// wait_writable() is accepted only after on_write_done(). The connection queues nothing past
/// that write.
///
/// No method calls the handler synchronously; every event comes from a handler on the loop.
class Connection {
public:
    class Impl;

    explicit Connection(SharedPtr<Impl> impl);
    ~Connection();
    Connection(Connection&& other) noexcept;
    Connection& operator=(Connection&& other) noexcept;
    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    /// @brief Starts reading and routes events to handler, which must outlive the connection
    /// or a close(). Called once; no method may be called on a moved-from handle.
    void start(ConnectionHandler& handler);

    /// @brief Waits for the socket to report room, then calls on_writable(). Write-stall timed.
    /// After a completed write of at most ROOM_AFTER_WRITE_MAX_BYTES with no `TCP_NOTSENT_LOWAT`
    /// gating room (notsent_lowat_bytes 0, or ESP-IDF's lwIP), the write left room: on_writable()
    /// follows with no wait on the socket, once on_write_done() has returned when called from it,
    /// else in a handler of its own.
    /// @return false when closed, not started, or refused by the write invariant
    bool wait_writable();

    /// @brief Whether write() would take a message now: inside on_writable(), while the write
    /// has room under write_batch_has_room()
    bool can_write() const;

    /// @brief Adds one message, as one WebSocket frame, to the write on_writable() starts as it
    /// returns; the payload is copied. Write-stall timed.
    /// @return false when closed or refused by the write invariant
    bool write(MessageKind kind, std::span<const uint8_t> payload);

    /// @brief Closes with a WebSocket close frame carrying code, or drops the TCP connection
    /// when a write is in flight. Inside on_writable(), the messages written there are dropped.
    /// No event follows.
    void close(uint16_t code);

    /// @brief False once closed by either side
    bool is_open() const;

    /// @brief The peer's address and port, as "192.0.2.1:8927" or "[2001:db8::1]:8927"; empty
    /// when unknown
    const std::string& remote_endpoint() const;

private:
    SharedPtr<Impl> impl_;
};

/// @brief Accepts WebSocket upgrades on one TCP port
class Listener {
public:
    class Impl;

    /// @brief Called with each upgraded connection, not started
    using AcceptFn = std::function<void(Connection connection)>;
    /// @brief Asked on the loop's thread at each TCP accept whether to take the connection; one
    /// refused is closed at once
    using AdmitFn = std::function<bool()>;

    /// @param admit empty to take every connection
    Listener(EventLoop& loop, TransportSettings settings, AcceptFn on_accept,
             AdmitFn admit = nullptr);
    /// @brief Calls close()
    ~Listener();
    Listener(const Listener&) = delete;
    Listener& operator=(const Listener&) = delete;

    /// @brief Binds and starts accepting. Called once, or again after it failed; close() is
    /// final.
    /// @param address a literal IPv4 or IPv6 address; the IPv6 wildcard `::` takes IPv4 peers too.
    /// On ESP-IDF only the wildcard of the family lwIP was built for binds.
    /// @param port 0 for an ephemeral port, then read back with port() (not on ESP-IDF)
    /// @return false when the address is invalid or binding fails
    bool listen(const std::string& address, uint16_t port);

    /// @brief The bound port, or 0 before a successful listen()
    uint16_t port() const;

    /// @brief Whether the last listen() failed because the system has no address of its family,
    /// such as IPv6 turned off
    bool family_unavailable() const;

    /// @brief Stops accepting and drops the upgrades in progress; delivered connections stay open
    void close();

    /// @brief Accepted TCP connections whose WebSocket upgrade is in progress
    size_t upgrades_in_progress() const;

private:
    SharedPtr<Impl> impl_;
};

/// @brief The platform's internal RAM, which a connection's tasks and lwIP's buffers take
struct InternalHeap {
    size_t free_bytes{0};
    /// 0 when free_bytes is below profile::INTERNAL_RAM_RESERVE_BYTES, where it is not read
    size_t largest_block_bytes{0};
};

/// @brief Any thread: the internal heap now, or nullopt where the platform has none to run short
/// of (a host)
std::optional<InternalHeap> internal_heap();

/// @brief Where dial() connects
struct DialTarget {
    /// Host for the `Host` header; resolved when addresses is empty
    std::string host;
    uint16_t port{0};
    /// Request target, starting with '/'
    std::string path{"/"};
    /// Literal IP addresses, tried in order
    std::vector<std::string> addresses;
};

/// @brief Parses `ws://host[:port][/path]`. The host may be a bracketed IPv6 literal in a form
/// is_ipv6_text() takes (`url_text.h`) on every platform, its zone identifier raw or
/// percent-encoded (RFC 6874) and kept as `address%zone`; a raw zone starting with "25" reads as
/// encoded, so it is written encoded (`%2525` for index 25). A build without IPv6 refuses every
/// bracketed host, and lwIP (ESP-IDF) an IPv4 tail in other forms than `::ffff:a.b.c.d` or after
/// six groups. The port defaults to 80 and the path to "/".
/// @return nullopt for any other scheme, a fragment (RFC 6455 section 3), a host or path holding
/// a character RFC 3986 does not allow there, or another malformed URL
std::optional<DialTarget> parse_ws_url(std::string_view url);

/// @brief A dial's outcome: the connection, not started, or why the dial failed
using DialResult = std::variant<Connection, CloseReason>;

/// @brief Called once with the dial's outcome
using DialFn = std::function<void(DialResult result)>;

/// @brief An outstanding dial. Dropping it cancels the dial.
class PendingDial {
public:
    class Impl;

    explicit PendingDial(SharedPtr<Impl> impl);
    ~PendingDial();
    PendingDial(PendingDial&& other) noexcept;
    PendingDial& operator=(PendingDial&& other) noexcept;
    PendingDial(const PendingDial&) = delete;
    PendingDial& operator=(const PendingDial&) = delete;

    /// @brief Abandons the dial; the DialFn is not called afterwards
    void cancel();

private:
    SharedPtr<Impl> impl_;
};

/// @brief Connects to target and performs the WebSocket handshake. Resolving a host name uses
/// the system resolver, which cannot be cancelled or timed out; pass addresses to avoid it.
[[nodiscard]] PendingDial dial(EventLoop& loop, const DialTarget& target,
                               const TransportSettings& settings, DialFn on_done);

/// @brief The loop the core thread runs. run() returns only after shutdown().
class EventLoop {
public:
    /// The implementation's state, which the transport's own classes reach through impl()
    class Impl;

    EventLoop();
    ~EventLoop();
    EventLoop(const EventLoop&) = delete;
    EventLoop& operator=(const EventLoop&) = delete;

    Impl& impl() {
        return *this->impl_;
    }

    /// @brief Runs handlers on the calling thread until shutdown(), then until every operation
    /// still in progress on the transport's threads (a closing handshake, say) has completed or
    /// grace_us has passed, and on the host stops the network thread
    void run();

    /// @brief Queues fn, a closure called with no arguments, to run on the thread in run(). Any
    /// thread; once run() has returned, fn is destroyed with the loop and never called.
    template <class F>
    void post(F&& fn) {
        std::decay_t<F> held(std::forward<F>(fn));
        this->post_task(TaskRef(held));
    }

    /// @brief Ends run(). From a handler on the loop's thread.
    /// @param grace_us how long run() waits for the operations still in progress
    void shutdown(int64_t grace_us);

private:
    /// Moves the closure into the loop's queue
    void post_task(TaskRef task);

    std::unique_ptr<Impl> impl_;
};

/// @brief Runs a function on the loop when another thread signals, once per batch of
/// signals: a signal while a run is pending adds nothing. signal() never allocates, so a real-time
/// thread may call it.
class Wakeup {
public:
    class Impl;
    /// @brief Signals the Wakeup from any thread; after the Wakeup is destroyed, or the loop
    /// is, a signal does nothing
    using Handle = SharedPtr<Impl>;

    /// @param loop held weakly: a signal that finds it destroyed does nothing
    /// @param on_wake run on the loop's thread
    Wakeup(EventLoop& loop, std::function<void()> on_wake);
    /// @brief A pending run is discarded, and so is every later signal. On the loop's thread.
    ~Wakeup();
    Wakeup(const Wakeup&) = delete;
    Wakeup& operator=(const Wakeup&) = delete;

    Handle handle() const {
        return this->impl_;
    }

    /// @brief Any thread
    void signal();

    /// @brief Any thread; a null handle does nothing
    static void signal(const Handle& handle);

private:
    SharedPtr<Impl> impl_;
};

/// @brief A one-shot timer on the loop. Dropping it, cancel() and a new start() each
/// discard the pending call, even one already due.
class Timer {
public:
    class Impl;

    explicit Timer(EventLoop& loop);
    ~Timer();
    Timer(const Timer&) = delete;
    Timer& operator=(const Timer&) = delete;

    /// @brief Calls on_fire from the loop once now_us() reaches about at_us, replacing any
    /// pending call. The wait runs on the steady clock, which can fall behind now_us().
    void start(int64_t at_us, std::function<void()> on_fire);

    void cancel();

private:
    SharedPtr<Impl> impl_;
};

}  // namespace sendspin::server
