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

#include "transport.h"

#include "clock.h"
#include "inbound_ring.h"
#include "profile.h"
#include "ring.h"
#include "thread_setup.h"
#include "transport_asio.h"
#include "ws_handshake.h"
#include <boost/asio/bind_allocator.hpp>
#include <boost/asio/defer.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ip/v6_only.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/websocket.hpp>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace sendspin::server {

// This transport hands its objects to transport.h and the inbound ring as std::shared_ptr
static_assert(std::is_same_v<SharedPtr<int>, std::shared_ptr<int>>,
              "the host transport builds where SharedPtr is std::shared_ptr");

namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
using tcp = asio::ip::tcp;
using boost::system::error_code;
using WsStream = websocket::stream<beast::tcp_stream>;

/// Largest HTTP upgrade request header accepted
constexpr uint32_t UPGRADE_HEADER_LIMIT = 8192;

/// Beast's websocket::stream::write_buffer_bytes() throws at or below 8
constexpr size_t MIN_WRITE_BUFFER_BYTES = 9;

/// Backoff for an accept that failed for lack of resources (descriptors, buffers)
constexpr int64_t ACCEPT_RETRY_MIN_US = 100000;
constexpr int64_t ACCEPT_RETRY_MAX_US = 1000000;
static_assert(profile::INBOUND_RING_BYTES >= MAX_MESSAGE_BYTES,
              "every message read_message_max admits must fit the inbound ring");

std::chrono::microseconds microseconds(int64_t us) {
    return std::chrono::microseconds(us);
}

bool configure_socket(tcp::socket& socket, const TransportSettings& settings) {
    error_code ec;
    socket.set_option(tcp::no_delay(true), ec);
    if (ec) {
        return false;
    }
#ifdef TCP_NOTSENT_LOWAT
    if (settings.notsent_lowat_bytes > 0) {
        const int value = settings.notsent_lowat_bytes;
        if (::setsockopt(socket.native_handle(), IPPROTO_TCP, TCP_NOTSENT_LOWAT, &value,
                         sizeof(value)) != 0) {
            return false;
        }
    }
#else
#error "TransportSettings::notsent_lowat_bytes needs TCP_NOTSENT_LOWAT"
#endif
    return true;
}

// Whether a socket's room is gated by TCP_NOTSENT_LOWAT (configure_socket()): it then reports
// room only once its unsent bytes fall below the mark, which only a wait sees
bool lowat_gates_room(const TransportSettings& settings) {
#ifdef TCP_NOTSENT_LOWAT
    return settings.notsent_lowat_bytes > 0;
#else
    (void)settings;
    return false;
#endif
}

// Set before the handshake so its timeouts apply to it
void configure_websocket(WsStream& ws, const TransportSettings& settings) {
    websocket::stream_base::timeout timeout{};
    timeout.handshake_timeout = microseconds(settings.handshake_timeout_us);
    if (settings.idle_timeout_us > 0) {
        timeout.idle_timeout = microseconds(settings.idle_timeout_us);
        timeout.keep_alive_pings = true;
    } else {
        timeout.idle_timeout = websocket::stream_base::none();
        timeout.keep_alive_pings = false;
    }
    ws.set_option(timeout);
    websocket::permessage_deflate deflate{};
    deflate.client_enable = false;
    deflate.server_enable = false;
    ws.set_option(deflate);
    ws.read_message_max(settings.read_message_max);
    ws.write_buffer_bytes(settings.write_buffer_bytes);
    ws.auto_fragment(false);
}

CloseReason reason_for(const error_code& ec) {
    if (ec == beast::error::timeout) {
        return CloseReason::TIMED_OUT;
    }
    if (ec == websocket::error::closed || ec == asio::error::eof) {
        return CloseReason::PEER_CLOSED;
    }
    if (ec == websocket::error::message_too_big || ec == websocket::condition::protocol_violation) {
        return CloseReason::PROTOCOL_VIOLATION;
    }
    // A socket call's errno, as the ESP-IDF transport maps its own
    if (ec.category() == asio::error::get_system_category()) {
        return close_reason_for_errno(ec.value());
    }
    return CloseReason::FAILED;
}

std::string_view path_of(std::string_view target) {
    return target.substr(0, target.find('?'));
}

TransportSettings sanitized(TransportSettings settings) {
    settings.write_buffer_bytes = std::max(settings.write_buffer_bytes, MIN_WRITE_BUFFER_BYTES);
    settings.read_message_max = std::min(settings.read_message_max, MAX_MESSAGE_BYTES);
    return settings;
}

std::string endpoint_text(const tcp::socket& socket) {
    error_code ec;
    const tcp::endpoint endpoint = socket.remote_endpoint(ec);
    if (ec) {
        return {};
    }
    asio::ip::address ip = endpoint.address();
    // A dual-stack listener sees an IPv4 peer as an IPv4-mapped IPv6 address
    if (ip.is_v6() && ip.to_v6().is_v4_mapped()) {
        ip = asio::ip::make_address_v4(asio::ip::v4_mapped, ip.to_v6());
    }
    const std::string address = ip.to_string();
    const std::string port = std::to_string(endpoint.port());
    return ip.is_v6() ? "[" + address + "]:" + port : address + ":" + port;
}

class NetConnection;

/// Fixed blocks for the handlers one thread posts to the other: each message's and event's drain,
/// and each command to a connection. Asio recycles a handler's memory on the thread that frees it,
/// so posts that cross threads mostly in one direction (a client sending far more than it reads)
/// would allocate per message. A handler larger than a block, or posted while every block is in
/// use, takes the heap. Any thread; the mutex is a leaf.
class HandlerPool {
public:
    static constexpr size_t BLOCK_BYTES = 128;

    explicit HandlerPool(size_t blocks)
        : blocks_(std::make_unique<Block[]>(blocks)), count_(blocks) {
        for (size_t i = 0; i < blocks; ++i) {
            this->blocks_[i].next = this->free_;
            this->free_ = &this->blocks_[i];
        }
    }

    void* allocate(size_t bytes) {
        if (bytes <= BLOCK_BYTES) {
            const std::lock_guard<std::mutex> lock(this->mutex_);
            if (Block* block = this->free_) {
                this->free_ = block->next;
                return block->bytes;
            }
        }
        return ::operator new(bytes);
    }

    void deallocate(void* p) {
        auto* block = reinterpret_cast<Block*>(p);
        const std::less<const Block*> before;
        if (!before(block, this->blocks_.get()) &&
            before(block, this->blocks_.get() + this->count_)) {
            const std::lock_guard<std::mutex> lock(this->mutex_);
            block->next = this->free_;
            this->free_ = block;
            return;
        }
        ::operator delete(p);
    }

private:
    union Block {
        Block* next;
        alignas(std::max_align_t) unsigned char bytes[BLOCK_BYTES];
    };

    std::unique_ptr<Block[]> blocks_;
    size_t count_;
    std::mutex mutex_;
    Block* free_{nullptr};
};

/// Allocates a posted handler from a HandlerPool, which outlives every handler posted with it
template <typename T>
class PoolAllocator {
public:
    using value_type = T;

    explicit PoolAllocator(HandlerPool* pool) : pool_(pool) {}

    template <typename U>
    PoolAllocator(const PoolAllocator<U>& other)  // NOLINT(google-explicit-constructor)
        : pool_(other.pool()) {}

    T* allocate(size_t n) {
        return static_cast<T*>(this->pool_->allocate(n * sizeof(T)));
    }

    void deallocate(T* p, size_t /*n*/) {
        this->pool_->deallocate(p);
    }

    HandlerPool* pool() const {
        return this->pool_;
    }

    template <typename U>
    bool operator==(const PoolAllocator<U>& other) const {
        return this->pool_ == other.pool();
    }

private:
    HandlerPool* pool_;
};

// ==============================================================================
// The network thread
// ==============================================================================

/// The network thread of one core io_context, and its io_context, which runs every socket of the
/// connections, listeners and dials made on that core io_context: all their I/O, with Beast's
/// control frames and handshakes, so a receive stamp never waits for core. Created by the first
/// Listener or dial() on the core io_context, and shut down with it.
///
/// Core holds a network object only weakly and reaches it by posting to the network io_context;
/// the network side owns it, through its outstanding operations and hold(), so every socket is
/// destroyed while the network io_context exists. Messages and events go to core through the
/// inbound ring, every other result through a post to the core io_context.
class NetworkService : public asio::execution_context::service {
public:
    static asio::execution_context::id id;  // NOLINT(readability-identifier-naming): Asio's name

    explicit NetworkService(asio::execution_context& owner)
        : asio::execution_context::service(owner),
          core_io_(static_cast<asio::io_context&>(owner)),
          inbound_(
              profile::INBOUND_RING_BYTES, profile::INBOUND_RING_MESSAGES,
              [this](uint64_t place) {
                  this->to_core([this, place] { this->inbound_.drain(place); });
              },
              [this] { this->to_network([this] { this->push_parked(); }); }),
          thread_(start_thread(ThreadRole::NETWORK, [this] { this->run(); })) {}

    // Also without shutdown(): use_service() destroys a service that lost a race to be created
    ~NetworkService() override {
        this->stop_all();
    }

    NetworkService(const NetworkService&) = delete;
    NetworkService& operator=(const NetworkService&) = delete;

    static NetworkService& of(asio::io_context& core_io) {
        return asio::use_service<NetworkService>(static_cast<asio::execution_context&>(core_io));
    }

    asio::io_context& core_io() {
        return this->core_io_;
    }

    asio::io_context& net_io() {
        return *this->net_io_;
    }

    InboundRing& inbound() {
        return this->inbound_;
    }

    /// Posts fn to core, its handler in the pool. From the network thread.
    template <typename Fn>
    void to_core(Fn&& fn) {
        asio::post(this->core_io_,
                   asio::bind_allocator(PoolAllocator<void>(&this->pool_), std::forward<Fn>(fn)));
    }

    /// Posts fn to the network thread, its handler in the pool; nothing once the network
    /// io_context is gone with the core one's services, as a Connection that outlived them would
    /// post. From core, or the thread that calls Listener::listen().
    template <typename Fn>
    void to_network(Fn&& fn) {
        if (this->net_io_) {
            asio::post(*this->net_io_, asio::bind_allocator(PoolAllocator<void>(&this->pool_),
                                                            std::forward<Fn>(fn)));
        }
    }

    /// Network thread: keeps an object alive until release(), while no operation of its own does;
    /// end() closes it if it is still held when core's loop has ended (finish())
    void hold(std::shared_ptr<void> object, std::function<void()> end) {
        const void* key = object.get();
        this->held_.emplace(key, Held{std::move(object), std::move(end)});
    }

    /// Network thread, from a caller that holds the object too
    void release(const void* object) {
        this->held_.erase(object);
    }

    /// Network thread: a connection whose message the ring refused; the parked connections push
    /// again in the order they parked once the ring has room
    void park(std::shared_ptr<NetConnection> connection) {
        this->parked_.push(std::move(connection));
    }

    /// Core thread, as its loop ends: lets the network io_context run until no operation is
    /// outstanding or deadline_us has passed, then stops the network thread. A post to the network
    /// io_context afterwards is never run.
    void finish(int64_t deadline_us) {
        // Core takes nothing more, so what is still held (a connection or a dial's outcome posted
        // to core too late) is closed rather than kept open, by its timers, to the deadline
        this->to_network([this] { this->end_held(); });
        this->work_.reset();
        {
            std::unique_lock<std::mutex> lock(this->mutex_);
            this->stopped_cv_.wait_for(lock,
                                       microseconds(std::max<int64_t>(deadline_us - now_us(), 0)),
                                       [this] { return this->stopped_; });
        }
        this->stop_thread();
    }

private:
    void run() {
        this->net_io_->run();
        {
            const std::lock_guard<std::mutex> lock(this->mutex_);
            this->stopped_ = true;
        }
        this->stopped_cv_.notify_all();
    }

    void push_parked();

    void end_held() {
        std::unordered_map<const void*, Held> held;
        held.swap(this->held_);
        for (auto& [key, entry] : held) {
            entry.end();
        }
    }

    void stop_thread() {
        this->work_.reset();
        this->net_io_->stop();
        if (this->thread_.joinable()) {
            this->thread_.join();
        }
    }

    void shutdown() override {
        this->stop_all();
    }

    // At most once: the network thread first, then what it held, then the network io_context,
    // whose destruction destroys the handlers still queued and the objects they hold
    void stop_all() {
        if (!this->net_io_) {
            return;
        }
        this->stop_thread();
        this->inbound_.clear();
        this->parked_.clear();
        this->held_.clear();
        this->net_io_.reset();
    }

    asio::io_context& core_io_;
    /// Before the network io_context, and kept after stop_all(): the core io_context's handlers
    /// that hold blocks go after this service's shutdown
    HandlerPool pool_{2 * profile::INBOUND_RING_MESSAGES};
    std::unique_ptr<asio::io_context> net_io_{std::make_unique<asio::io_context>()};
    std::optional<asio::executor_work_guard<asio::io_context::executor_type>> work_{
        net_io_->get_executor()};
    InboundRing inbound_;
    struct Held {
        std::shared_ptr<void> object;
        std::function<void()> end;
    };

    /// Network thread only
    std::unordered_map<const void*, Held> held_;
    /// Network thread only: each connection parks at most once, and reads nothing more until it
    /// pushes; room for eight at first
    Ring<std::shared_ptr<NetConnection>> parked_{8, Ring<std::shared_ptr<NetConnection>>::UNLIMITED,
                                                 MemoryLocation::PREFER_EXTERNAL};
    /// Guards stopped_, which the network thread sets as its run() returns
    std::mutex mutex_;
    std::condition_variable stopped_cv_;
    bool stopped_{false};
    std::thread thread_;
};

asio::execution_context::id NetworkService::id;

}  // namespace

// ==============================================================================
// Connection
// ==============================================================================

/// The core thread's half of a connection: the write invariant, the room after a write, the events
/// the network half raises and the batch it writes from. Holds no I/O object, so it may outlive
/// the network io_context.
class Connection::Impl : public InboundEndpoint, public std::enable_shared_from_this<Impl> {
public:
    Impl(NetworkService& service, const TransportSettings& settings)
        : service_(service), room_after_write_(!lowat_gates_room(settings)) {}

    bool is_open() const {
        return !this->closed_;
    }

    const std::string& remote_endpoint() const {
        return this->remote_endpoint_;
    }

    void start(ConnectionHandler& handler);
    bool wait_writable();
    bool can_write() const {
        return !this->closed_ && this->room_ &&
               write_batch_has_room(this->batch_count_, this->write_buffer_.size());
    }
    bool write(MessageKind kind, std::span<const uint8_t> payload);
    void close(uint16_t code);

    // For destructors: a close that cannot be posted leaves the network half to the network
    // io_context's end
    void close_noexcept() noexcept {
        try {
            this->close(close_code::GOING_AWAY);
        } catch (...) {
            this->closed_ = true;
        }
    }

    /// Set by the network half before it hands the connection to core
    std::weak_ptr<NetConnection> net_;
    std::string remote_endpoint_;
    struct BatchFrame {
        MessageKind kind;
        size_t bytes;
    };
    /// Written by core for a write, read by the network half until the write completes: the
    /// payloads back to back, and each frame's kind and size
    std::vector<uint8_t> write_buffer_;
    std::array<BatchFrame, WRITE_BATCH_MESSAGES> batch_frames_{};
    /// The frames of the write being built or in flight; the batch is cleared once it completes
    size_t batch_count_{0};

protected:
    void deliver_message(MessageKind kind, std::span<const uint8_t> payload,
                         int64_t rx_us) override {
        if (!this->closed_ && this->handler_ != nullptr) {
            this->handler_->on_message(kind, payload, rx_us);
        }
    }

    void deliver_writable() override {
        this->wait_in_flight_ = false;
        if (this->closed_) {
            return;
        }
        this->room_ = true;
        this->handler_->on_writable();
        this->room_ = false;
        this->start_write();
    }

    void deliver_write_done() override {
        this->write_in_flight_ = false;
        this->clear_batch();
        if (this->closed_) {
            return;
        }
        this->wrote_ = this->small_write_;
        this->in_write_done_ = true;
        this->handler_->on_write_done();
        this->in_write_done_ = false;
        if (std::exchange(this->room_pending_, false)) {
            this->deliver_writable();
        }
    }

    void deliver_closed(CloseReason reason) override {
        if (this->closed_) {
            return;
        }
        this->closed_ = true;
        this->room_ = false;
        ConnectionHandler* handler = std::exchange(this->handler_, nullptr);
        if (handler != nullptr) {
            handler->on_closed(reason);
        }
    }

private:
    /// Starts the write of the messages on_writable() took, unless it took none or closed
    void start_write();

    void clear_batch() {
        this->write_buffer_.clear();
        this->batch_count_ = 0;
    }

    /// Runs fn with the network half on the network thread, unless it is gone by then
    template <typename Fn>
    void to_network(Fn fn) {
        this->service_.to_network([weak = this->net_, fn = std::move(fn)] {
            if (const std::shared_ptr<NetConnection> net = weak.lock()) {
                fn(*net);
            }
        });
    }

    NetworkService& service_;
    ConnectionHandler* handler_{nullptr};
    // A completed write leaves room (no TCP_NOTSENT_LOWAT gating it)
    bool room_after_write_;
    // The last write, no larger than ROOM_AFTER_WRITE_MAX_BYTES, completed and no wait has started
    // since
    bool wrote_{false};
    bool small_write_{false};
    // on_write_done() is running; a wait it starts after a completed write is reported once it
    // returns
    bool in_write_done_{false};
    bool room_pending_{false};
    bool closed_{false};
    bool wait_in_flight_{false};
    bool write_in_flight_{false};
    // True only while on_writable() runs
    bool room_{false};
};

namespace {

/// The network thread's half of a connection: its WebSocket stream and every operation on it,
/// the receive stamps, the write stall and the closing handshake, all on the network thread
class NetConnection : public std::enable_shared_from_this<NetConnection> {
public:
    NetConnection(NetworkService& service, beast::tcp_stream stream,
                  const TransportSettings& settings)
        : service_(service),
          ws_(std::move(stream)),
          timer_(ws_.get_executor()),
          core_(std::make_shared<Connection::Impl>(service, settings)),
          write_stall_us_(settings.write_stall_us),
          close_timeout_us_(settings.handshake_timeout_us) {
        configure_websocket(this->ws_, settings);
    }

    /// The connection for core, linked to this half
    static std::shared_ptr<NetConnection> create(NetworkService& service, beast::tcp_stream stream,
                                                 const TransportSettings& settings) {
        auto net = std::make_shared<NetConnection>(service, std::move(stream), settings);
        net->core_->net_ = net;
        return net;
    }

    WsStream& ws() {
        return this->ws_;
    }

    const std::shared_ptr<Connection::Impl>& core() const {
        return this->core_;
    }

    // Called once the socket is connected, before core has the connection
    void remember_remote_endpoint() {
        this->core_->remote_endpoint_ = endpoint_text(beast::get_lowest_layer(this->ws_).socket());
    }

    void start() {
        if (!this->closed_) {
            this->read_next();
        }
    }

    void wait_writable() {
        if (this->closed_) {
            return;
        }
        this->wait_in_flight_ = true;
        this->start_io();
        beast::get_lowest_layer(this->ws_).socket().async_wait(
            tcp::socket::wait_write,
            [self = this->shared_from_this()](const error_code& ec) { self->on_wait(ec); });
    }

    /// Writes the core half's batch of frames, each a message of its own, one after another
    void write(size_t frames) {
        if (this->closed_) {
            return;
        }
        this->write_in_flight_ = true;
        this->write_frame(0, 0, frames);
    }

    void close(uint16_t code) {
        if (this->closed_) {
            return;
        }
        this->closed_ = true;
        this->parked_ = false;
        this->service_.release(this);
        // A close frame would queue behind the write in flight, which may never complete
        if (this->write_in_flight_ || !this->ws_.is_open()) {
            this->drop();
            return;
        }
        // Bounds the closing handshake for a peer that neither reads nor answers
        this->arm_timer(this->close_timeout_us_, &NetConnection::drop);
        this->ws_.async_close(
            websocket::close_reason(code),
            [self = this->shared_from_this()](const error_code& /*ec*/) { self->drop(); });
    }

    // For a connection core never received: a closing handshake that cannot start drops the
    // socket instead
    void close_noexcept() noexcept {
        try {
            this->close(close_code::GOING_AWAY);
        } catch (...) {
            this->closed_ = true;
            error_code ignored;
            beast::get_lowest_layer(this->ws_).socket().close(ignored);
        }
    }

    /// Pushes the message the ring refused; false when it is refused again
    bool push_parked() {
        if (!this->parked_) {
            return true;
        }
        const auto data = this->read_buffer_.cdata();
        if (!this->service_.inbound().push(
                this->core_, this->parked_kind_,
                std::span(static_cast<const uint8_t*>(data.data()), data.size()),
                this->parked_rx_us_)) {
            return false;
        }
        this->parked_ = false;
        this->read_buffer_.clear();
        if (this->pending_close_) {
            this->service_.inbound().raise(this->core_, InboundEndpoint::Event::CLOSED,
                                           *this->pending_close_);
        } else if (!this->closed_) {
            this->read_next();
        }
        return true;
    }

private:
    void read_next() {
        this->read_in_flight_ = true;
        this->ws_.async_read(this->read_buffer_, [self = this->shared_from_this()](
                                                     const error_code& ec, size_t /*bytes*/) {
            const int64_t rx_us = now_us();
            self->on_read(ec, rx_us);
        });
    }

    void on_read(const error_code& ec, int64_t rx_us) {
        this->read_in_flight_ = false;
        if (this->closed_) {
            return;
        }
        if (ec) {
            this->fail(reason_for(ec));
            return;
        }
        const auto data = this->read_buffer_.cdata();
        const MessageKind kind = this->ws_.got_text() ? MessageKind::TEXT : MessageKind::BINARY;
        if (!this->service_.inbound().push(
                this->core_, kind, std::span(static_cast<const uint8_t*>(data.data()), data.size()),
                rx_us)) {
            // Held in the read buffer, with no read until it is pushed: TCP holds the rest back
            this->parked_ = true;
            this->parked_kind_ = kind;
            this->parked_rx_us_ = rx_us;
            this->service_.park(this->shared_from_this());
            return;
        }
        this->read_buffer_.clear();
        this->read_next();
    }

    void on_wait(const error_code& ec) {
        this->wait_in_flight_ = false;
        if (this->closed_) {
            return;
        }
        if (ec) {
            this->fail_unless_read_reports(ec);
            return;
        }
        this->service_.inbound().raise(this->core_, InboundEndpoint::Event::WRITABLE);
    }

    // Beast writes each frame on its own, as its write lock, which its pongs and closing handshake
    // share, admits only one message at a time
    void write_frame(size_t index, size_t offset, size_t frames) {
        const Connection::Impl::BatchFrame frame = this->core_->batch_frames_[index];
        this->ws_.text(frame.kind == MessageKind::TEXT);
        // The stall deadline covers the whole write, as on ESP-IDF
        if (index == 0) {
            this->start_io();
        }
        this->ws_.async_write(
            asio::buffer(this->core_->write_buffer_.data() + offset, frame.bytes),
            [self = this->shared_from_this(), next = index + 1, end = offset + frame.bytes, frames](
                const error_code& ec, size_t) { self->on_write(ec, next, end, frames); });
    }

    void on_write(const error_code& ec, size_t next, size_t offset, size_t frames) {
        if (this->closed_) {
            this->write_in_flight_ = false;
            return;
        }
        if (ec) {
            this->write_in_flight_ = false;
            this->fail_unless_read_reports(ec);
            return;
        }
        if (next < frames) {
            this->write_frame(next, offset, frames);
            return;
        }
        this->write_in_flight_ = false;
        this->service_.inbound().raise(this->core_, InboundEndpoint::Event::WRITE_DONE);
    }

    // The write stall: one wait or write outstanding for write_stall_us_. The timer is armed when
    // I/O starts with none armed and checks the I/O outstanding when it fires, so I/O that keeps
    // completing arms it about once per write_stall_us_, not once per wait and write.
    void start_io() {
        this->io_started_us_ = now_us();
        if (!this->stall_armed_) {
            this->arm_timer(this->write_stall_us_, &NetConnection::on_stall);
            this->stall_armed_ = true;
        }
    }

    void on_stall() {
        this->stall_armed_ = false;
        if (this->closed_ || (!this->wait_in_flight_ && !this->write_in_flight_)) {
            return;
        }
        const int64_t outstanding_us = now_us() - this->io_started_us_;
        if (outstanding_us >= this->write_stall_us_) {
            this->fail(CloseReason::WRITE_STALLED);
            return;
        }
        this->arm_timer(this->write_stall_us_ - outstanding_us, &NetConnection::on_stall);
        this->stall_armed_ = true;
    }

    // Beast closes the socket itself when a read fails or times out, which aborts a wait or write
    // outstanding: the read's completion follows with the reason
    void fail_unless_read_reports(const error_code& ec) {
        if (ec == asio::error::operation_aborted && this->read_in_flight_) {
            return;
        }
        this->fail(reason_for(ec));
    }

    // The close reaches core after every message read before it, a parked one included
    void fail(CloseReason reason) {
        this->closed_ = true;
        this->drop();
        this->service_.release(this);
        if (this->parked_) {
            this->pending_close_ = reason;
            return;
        }
        this->service_.inbound().raise(this->core_, InboundEndpoint::Event::CLOSED, reason);
    }

    // Closes the socket; every outstanding operation completes with an error
    void drop() {
        this->disarm_timer();
        beast::get_lowest_layer(this->ws_).close();
    }

    // One timer serves the write stall and the closing-handshake bound. The generation discards
    // a completion already queued when the timer was re-armed or disarmed.
    void arm_timer(int64_t after_us, void (NetConnection::*on_fire)()) {
        const uint64_t generation = ++this->timer_generation_;
        this->stall_armed_ = false;
        this->timer_.expires_after(microseconds(after_us));
        this->timer_.async_wait(
            [self = this->shared_from_this(), generation, on_fire](const error_code& ec) {
                if (!ec && generation == self->timer_generation_) {
                    ((*self).*on_fire)();
                }
            });
    }

    void disarm_timer() {
        ++this->timer_generation_;
        this->stall_armed_ = false;
        this->timer_.cancel();
    }

    NetworkService& service_;
    WsStream ws_;
    asio::steady_timer timer_;
    beast::flat_buffer read_buffer_;
    std::shared_ptr<Connection::Impl> core_;
    int64_t write_stall_us_;
    int64_t close_timeout_us_;
    uint64_t timer_generation_{0};
    // When the wait or write outstanding started
    int64_t io_started_us_{0};
    // The timer is armed for the write stall
    bool stall_armed_{false};
    bool closed_{false};
    bool read_in_flight_{false};
    bool wait_in_flight_{false};
    bool write_in_flight_{false};
    // The read buffer holds a message the inbound ring refused
    bool parked_{false};
    MessageKind parked_kind_{MessageKind::BINARY};
    int64_t parked_rx_us_{0};
    // A failure while parked, raised once the parked message is pushed
    std::optional<CloseReason> pending_close_;
};

void NetworkService::push_parked() {
    while (!this->parked_.empty()) {
        if (!this->parked_.front()->push_parked()) {
            return;
        }
        this->parked_.pop_front();
    }
}

}  // namespace

void Connection::Impl::start(ConnectionHandler& handler) {
    this->handler_ = &handler;
    if (!this->closed_) {
        this->to_network([](NetConnection& net) { net.start(); });
    }
}

bool Connection::Impl::wait_writable() {
    if (this->closed_ || this->handler_ == nullptr || this->wait_in_flight_ ||
        this->write_in_flight_ || this->batch_count_ > 0) {
        return false;
    }
    this->room_ = false;
    this->wait_in_flight_ = true;
    // A write that completed leaves room, unless a low-water mark gates it: the room is reported
    // with no wait on the socket and no stall to watch, once the write's completion handler has
    // returned from on_write_done(), or else in a handler of its own
    if (std::exchange(this->wrote_, false) && this->room_after_write_) {
        if (this->in_write_done_) {
            this->room_pending_ = true;
        } else {
            asio::defer(this->service_.core_io(),
                        [self = this->shared_from_this()] { self->deliver_writable(); });
        }
        return true;
    }
    this->to_network([](NetConnection& net) { net.wait_writable(); });
    return true;
}

bool Connection::Impl::write(MessageKind kind, std::span<const uint8_t> payload) {
    if (!this->can_write()) {
        return false;
    }
    this->write_buffer_.insert(this->write_buffer_.end(), payload.begin(), payload.end());
    this->batch_frames_[this->batch_count_++] = {kind, payload.size()};
    return true;
}

void Connection::Impl::start_write() {
    if (this->batch_count_ == 0) {
        return;
    }
    // After a close inside on_writable(): the network close it posted would make the network half
    // ignore the write, but close_noexcept() may have set closed_ without posting one
    if (this->closed_) {
        this->clear_batch();
        return;
    }
    this->small_write_ = this->write_buffer_.size() <= ROOM_AFTER_WRITE_MAX_BYTES;
    this->write_in_flight_ = true;
    this->to_network([frames = this->batch_count_](NetConnection& net) { net.write(frames); });
}

void Connection::Impl::close(uint16_t code) {
    if (this->closed_) {
        return;
    }
    this->closed_ = true;
    this->room_ = false;
    this->to_network([code](NetConnection& net) { net.close(code); });
}

Connection::Connection(SharedPtr<Impl> impl) : impl_(std::move(impl)) {}

Connection::~Connection() {
    if (this->impl_) {
        this->impl_->close_noexcept();
    }
}

Connection::Connection(Connection&& other) noexcept = default;

Connection& Connection::operator=(Connection&& other) noexcept {
    if (this != &other) {
        if (this->impl_) {
            this->impl_->close_noexcept();
        }
        this->impl_ = std::move(other.impl_);
    }
    return *this;
}

void Connection::start(ConnectionHandler& handler) {
    this->impl_->start(handler);
}

bool Connection::wait_writable() {
    return this->impl_->wait_writable();
}

bool Connection::can_write() const {
    return this->impl_->can_write();
}

bool Connection::write(MessageKind kind, std::span<const uint8_t> payload) {
    return this->impl_->write(kind, payload);
}

void Connection::close(uint16_t code) {
    this->impl_->close(code);
}

bool Connection::is_open() const {
    return this->impl_->is_open();
}

const std::string& Connection::remote_endpoint() const {
    return this->impl_->remote_endpoint();
}

// ==============================================================================
// Listener
// ==============================================================================

namespace {

class Upgrade;

/// Network thread: ends an admitted upgrade, with the connection for core or nullptr when it failed
using UpgradeEndFn = std::function<void(Upgrade* upgrade, std::shared_ptr<Connection::Impl>)>;

// An accepted socket from the TCP accept until its WebSocket handshake completes, on the network
// thread
class Upgrade : public std::enable_shared_from_this<Upgrade> {
public:
    Upgrade(NetworkService& service, tcp::socket socket, const TransportSettings& settings,
            UpgradeEndFn on_end)
        : service_(service),
          stream_(std::move(socket)),
          settings_(settings),
          on_end_(std::move(on_end)) {}

    // Once core admitted it
    void start() {
        this->started_ = true;
        if (!configure_socket(this->stream_.socket(), this->settings_)) {
            this->drop();
            return;
        }
        this->parser_.header_limit(UPGRADE_HEADER_LIMIT);
        this->stream_.expires_after(microseconds(this->settings_.handshake_timeout_us));
        http::async_read(this->stream_, this->buffer_, this->parser_,
                         [self = this->shared_from_this()](const error_code& ec, size_t /*bytes*/) {
                             self->on_request(ec);
                         });
    }

    void drop() {
        if (this->dropped_) {
            return;
        }
        this->dropped_ = true;
        if (this->connection_) {
            this->connection_->close_noexcept();
        } else if (!this->stream_moved_) {
            this->stream_.close();
        }
        this->end(nullptr);
    }

private:
    void on_request(const error_code& ec) {
        if (ec || this->dropped_) {
            this->drop();
            return;
        }
        const auto& request = this->parser_.get();
        if (!websocket::is_upgrade(request)) {
            this->reject(http::status::bad_request, request.version());
            return;
        }
        if (path_of(request.target()) != this->settings_.path) {
            this->reject(http::status::not_found, request.version());
            return;
        }
        this->stream_.expires_never();
        this->stream_moved_ = true;
        this->connection_ =
            NetConnection::create(this->service_, std::move(this->stream_), this->settings_);
        this->connection_->remember_remote_endpoint();
        this->connection_->ws().async_accept(
            request, [self = this->shared_from_this()](const error_code& accept_ec) {
                self->on_accepted(accept_ec);
            });
    }

    void reject(http::status status, unsigned version) {
        this->response_.version(version);
        this->response_.result(status);
        this->response_.keep_alive(false);
        this->response_.prepare_payload();
        http::async_write(this->stream_, this->response_,
                          [self = this->shared_from_this()](const error_code& /*ec*/,
                                                            size_t /*bytes*/) { self->drop(); });
    }

    void on_accepted(const error_code& ec) {
        if (this->dropped_) {
            return;
        }
        if (ec) {
            this->drop();
            return;
        }
        this->dropped_ = true;
        const std::shared_ptr<NetConnection> connection = std::exchange(this->connection_, nullptr);
        this->service_.hold(connection, [connection] { connection->close_noexcept(); });
        this->end(connection->core());
    }

    void end(std::shared_ptr<Connection::Impl> core) {
        if (this->started_ && this->on_end_) {
            const UpgradeEndFn on_end = std::move(this->on_end_);
            this->on_end_ = nullptr;
            on_end(this, std::move(core));
        }
    }

    NetworkService& service_;
    beast::tcp_stream stream_;
    beast::flat_buffer buffer_;
    http::request_parser<http::empty_body> parser_;
    http::response<http::empty_body> response_;
    std::shared_ptr<NetConnection> connection_;
    TransportSettings settings_;
    UpgradeEndFn on_end_;
    bool started_{false};
    bool dropped_{false};
    bool stream_moved_{false};
};

class NetListener;

}  // namespace

/// The core thread's half of a listener: the admission, the count of upgrades in progress and the
/// delivery of upgraded connections
class Listener::Impl : public std::enable_shared_from_this<Impl> {
public:
    Impl(asio::io_context& io, TransportSettings settings, AcceptFn on_accept, AdmitFn admit)
        : service_(NetworkService::of(io)),
          settings_(sanitized(std::move(settings))),
          on_accept_(std::move(on_accept)),
          admit_(std::move(admit)) {}

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    bool listen(const std::string& address, uint16_t port);

    bool family_unavailable() const {
        return this->family_unavailable_;
    }

    uint16_t port() const {
        return this->port_;
    }

    void close();

    size_t upgrades_in_progress() const {
        return this->upgrades_;
    }

    /// Core thread: a TCP connection the network thread accepted; whether to upgrade it
    bool admit() {
        if (this->closed_ || (this->admit_ && !this->admit_())) {
            return false;
        }
        ++this->upgrades_;
        return true;
    }

    /// Core thread: an admitted upgrade ended, with the connection or nullptr
    void upgrade_ended(std::shared_ptr<Connection::Impl> core) {
        if (!this->closed_) {
            --this->upgrades_;
        }
        if (!core) {
            return;
        }
        Connection connection(std::move(core));
        if (!this->closed_) {
            this->on_accept_(std::move(connection));
        }
    }

private:
    NetworkService& service_;
    TransportSettings settings_;
    /// Released to the network thread by close()
    std::shared_ptr<NetListener> net_;
    AcceptFn on_accept_;
    AdmitFn admit_;
    size_t upgrades_{0};
    uint16_t port_{0};
    bool closed_{false};
    bool family_unavailable_{false};
};

namespace {

/// The network thread's half of a listener: the acceptor and the upgrades. listen() runs on the
/// thread that calls Listener::listen(), before any operation on the acceptor starts.
class NetListener : public std::enable_shared_from_this<NetListener> {
public:
    NetListener(NetworkService& service, TransportSettings settings,
                std::weak_ptr<Listener::Impl> core)
        : service_(service),
          acceptor_(service.net_io()),
          retry_timer_(service.net_io()),
          settings_(std::move(settings)),
          core_(std::move(core)) {}

    error_code listen(const asio::ip::address& ip, uint16_t port) {
        error_code ec;
        const tcp::endpoint endpoint(ip, port);
        this->acceptor_.open(endpoint.protocol(), ec);
        if (!ec) {
            this->acceptor_.set_option(tcp::acceptor::reuse_address(true), ec);
        }
        // The IPv6 wildcard takes IPv4 peers too, whatever the system's default
        if (!ec && ip.is_v6() && ip.is_unspecified()) {
            this->acceptor_.set_option(asio::ip::v6_only(false), ec);
        }
        if (!ec) {
            this->acceptor_.bind(endpoint, ec);
        }
        if (!ec) {
            this->acceptor_.listen(asio::socket_base::max_listen_connections, ec);
        }
        if (ec) {
            error_code ignored;
            this->acceptor_.close(ignored);
        }
        return ec;
    }

    uint16_t port() const {
        error_code ec;
        const tcp::endpoint endpoint = this->acceptor_.local_endpoint(ec);
        return ec ? 0 : endpoint.port();
    }

    void accept_next() {
        this->acceptor_.async_accept(
            [self = this->shared_from_this()](const error_code& ec, tcp::socket socket) {
                if (self->closed_ || ec == asio::error::operation_aborted) {
                    return;
                }
                if (!ec) {
                    self->retry_us_ = ACCEPT_RETRY_MIN_US;
                    self->offer(std::move(socket));
                    self->accept_next();
                    return;
                }
                // Out of descriptors or buffers: retrying at once would spin
                self->retry_timer_.expires_after(microseconds(self->retry_us_));
                self->retry_us_ = std::min(self->retry_us_ * 2, ACCEPT_RETRY_MAX_US);
                self->retry_timer_.async_wait([self](const error_code& timer_ec) {
                    if (!timer_ec && !self->closed_) {
                        self->accept_next();
                    }
                });
            });
    }

    void close() {
        this->closed_ = true;
        error_code ignored;
        this->acceptor_.close(ignored);
        this->retry_timer_.cancel();
        for (const std::shared_ptr<Upgrade>& upgrade : std::exchange(this->upgrades_, {})) {
            upgrade->drop();
        }
    }

private:
    // Core admits the socket or refuses it, which closes it at once. What core runs holds the
    // network objects weakly.
    void offer(tcp::socket socket) {
        auto upgrade = std::make_shared<Upgrade>(
            this->service_, std::move(socket), this->settings_,
            [weak = this->weak_from_this()](Upgrade* ended,
                                            std::shared_ptr<Connection::Impl> core) {
                if (const std::shared_ptr<NetListener> self = weak.lock()) {
                    self->on_upgrade_end(ended, std::move(core));
                }
            });
        this->upgrades_.push_back(upgrade);
        this->service_.to_core([service = &this->service_, core = this->core_,
                                weak = this->weak_from_this(),
                                weak_upgrade = std::weak_ptr<Upgrade>(upgrade)] {
            const std::shared_ptr<Listener::Impl> listener = core.lock();
            const bool admitted = listener && listener->admit();
            service->to_network([weak, weak_upgrade, admitted] {
                const std::shared_ptr<NetListener> self = weak.lock();
                const std::shared_ptr<Upgrade> upgrade = weak_upgrade.lock();
                if (self && upgrade && !self->closed_) {
                    self->on_admission(upgrade, admitted);
                }
            });
        });
    }

    void on_admission(const std::shared_ptr<Upgrade>& upgrade, bool admitted) {
        if (admitted) {
            upgrade->start();
            return;
        }
        this->forget(upgrade.get());
        upgrade->drop();
    }

    void forget(const Upgrade* upgrade) {
        std::erase_if(this->upgrades_, [upgrade](const std::shared_ptr<Upgrade>& held) {
            return held.get() == upgrade;
        });
    }

    void on_upgrade_end(Upgrade* ended, std::shared_ptr<Connection::Impl> core_connection) {
        this->forget(ended);
        this->service_.to_core(
            [weak = this->core_, core_connection = std::move(core_connection)]() mutable {
                if (const std::shared_ptr<Listener::Impl> core = weak.lock()) {
                    core->upgrade_ended(std::move(core_connection));
                } else if (core_connection) {
                    const Connection dropped(std::move(core_connection));
                }
            });
    }

    NetworkService& service_;
    tcp::acceptor acceptor_;
    asio::steady_timer retry_timer_;
    TransportSettings settings_;
    std::weak_ptr<Listener::Impl> core_;
    int64_t retry_us_{ACCEPT_RETRY_MIN_US};
    /// The upgrades offered to core or in progress
    std::vector<std::shared_ptr<Upgrade>> upgrades_;
    bool closed_{false};
};

}  // namespace

bool Listener::Impl::listen(const std::string& address, uint16_t port) {
    if (this->closed_) {
        return false;
    }
    error_code ec;
    const asio::ip::address ip = asio::ip::make_address(address, ec);
    if (ec) {
        return false;
    }
    if (!this->net_) {
        this->net_ =
            std::make_shared<NetListener>(this->service_, this->settings_, this->weak_from_this());
    }
    ec = this->net_->listen(ip, port);
    // EAFNOSUPPORT from open() or bind(), or EADDRNOTAVAIL from a bind on a system with the
    // family but no address of it
    this->family_unavailable_ = ec == boost::system::errc::address_family_not_supported ||
                                ec == boost::system::errc::address_not_available;
    if (ec) {
        return false;
    }
    this->port_ = this->net_->port();
    this->service_.to_network([net = this->net_] { net->accept_next(); });
    return true;
}

void Listener::Impl::close() {
    if (this->closed_) {
        return;
    }
    this->closed_ = true;
    this->upgrades_ = 0;
    if (this->net_) {
        this->service_.to_network([net = std::move(this->net_)] { net->close(); });
    }
}

Listener::Listener(EventLoop& loop, TransportSettings settings, AcceptFn on_accept, AdmitFn admit)
    : impl_(std::make_shared<Impl>(loop.impl().io(), std::move(settings), std::move(on_accept),
                                   std::move(admit))) {}

Listener::~Listener() {
    this->impl_->close();
}

bool Listener::listen(const std::string& address, uint16_t port) {
    return this->impl_->listen(address, port);
}

uint16_t Listener::port() const {
    return this->impl_->port();
}

bool Listener::family_unavailable() const {
    return this->impl_->family_unavailable();
}

void Listener::close() {
    this->impl_->close();
}

size_t Listener::upgrades_in_progress() const {
    return this->impl_->upgrades_in_progress();
}

// ==============================================================================
// Dialing
// ==============================================================================

namespace {

class NetDial;

/// What a dial delivers to core: its connection or why it failed
using DialOutcome = std::variant<std::shared_ptr<Connection::Impl>, CloseReason>;

}  // namespace

/// The core thread's half of a dial: the DialFn, called at most once
class PendingDial::Impl {
public:
    Impl(NetworkService& service, DialFn on_done)
        : service_(service), on_done_(std::move(on_done)) {}

    void cancel();

    /// Core thread: the dial's outcome; a connection that comes after cancel() is closed
    void complete(DialOutcome outcome) {
        std::shared_ptr<Connection::Impl>* connection =
            std::get_if<std::shared_ptr<Connection::Impl>>(&outcome);
        if (this->done_) {
            if (connection != nullptr) {
                const Connection dropped(std::move(*connection));
            }
            return;
        }
        this->done_ = true;
        const DialFn on_done = std::move(this->on_done_);
        if (connection != nullptr) {
            on_done(Connection(std::move(*connection)));
        } else {
            on_done(std::get<CloseReason>(outcome));
        }
    }

    /// Set by dial() before the network thread starts it
    std::weak_ptr<NetDial> net_;

private:
    NetworkService& service_;
    DialFn on_done_;
    bool done_{false};
};

namespace {

/// The network thread's half of a dial: the resolve, the connect and the WebSocket handshake.
/// Constructed on core with no operation started; held on the network thread from start() on.
class NetDial : public std::enable_shared_from_this<NetDial> {
public:
    NetDial(NetworkService& service, DialTarget target, const TransportSettings& settings,
            std::weak_ptr<PendingDial::Impl> core)
        : service_(service),
          resolver_(service.net_io()),
          connection_(
              NetConnection::create(service, beast::tcp_stream(service.net_io()), settings)),
          target_(std::move(target)),
          settings_(settings),
          core_(std::move(core)) {}

    void start() {
        this->service_.hold(this->shared_from_this(), [this] { this->cancel(); });
        if (!this->target_.addresses.empty()) {
            std::vector<tcp::endpoint> endpoints;
            for (const std::string& address : this->target_.addresses) {
                error_code ec;
                const asio::ip::address ip = asio::ip::make_address(address, ec);
                if (!ec) {
                    endpoints.emplace_back(ip, this->target_.port);
                }
            }
            // An empty list fails in connect_next()
            this->connect(std::move(endpoints));
            return;
        }
        this->resolver_.async_resolve(
            this->target_.host, std::to_string(this->target_.port),
            [self = this->shared_from_this()](const error_code& ec,
                                              const tcp::resolver::results_type& results) {
                if (self->done_) {
                    return;
                }
                if (ec) {
                    self->finish_failed(reason_for(ec));
                    return;
                }
                std::vector<tcp::endpoint> endpoints;
                for (const auto& entry : results) {
                    endpoints.push_back(entry.endpoint());
                }
                self->connect(std::move(endpoints));
            });
    }

    void cancel() {
        if (this->done_) {
            return;
        }
        this->done_ = true;
        this->resolver_.cancel();
        this->connection_->close_noexcept();
        this->service_.release(this);
    }

private:
    void connect(std::vector<tcp::endpoint> endpoints) {
        this->endpoints_ = std::move(endpoints);
        this->connect_next(0, CloseReason::FAILED);
    }

    /// The endpoint at index, within a connect_timeout_us of its own, as the ESP-IDF dial does: a
    /// silent address does not use up the time of the ones after it. Past the last endpoint the
    /// dial fails with the last attempt's failure.
    void connect_next(size_t index, CloseReason failure) {
        if (index >= this->endpoints_.size()) {
            this->finish_failed(failure);
            return;
        }
        beast::tcp_stream& stream = beast::get_lowest_layer(this->connection_->ws());
        // A socket whose connect failed does not connect again, and the next endpoint may be of
        // the other address family
        error_code ignored;
        stream.socket().close(ignored);
        stream.expires_after(microseconds(this->settings_.connect_timeout_us));
        stream.async_connect(this->endpoints_[index],
                             [self = this->shared_from_this(), index](const error_code& ec) {
                                 self->on_connect(index, ec);
                             });
    }

    void on_connect(size_t index, const error_code& ec) {
        if (this->done_) {
            return;
        }
        if (ec) {
            this->connect_next(index + 1, reason_for(ec));
            return;
        }
        // The WebSocket stream's own timeouts take over from here
        beast::tcp_stream& stream = beast::get_lowest_layer(this->connection_->ws());
        stream.expires_never();
        this->connection_->remember_remote_endpoint();
        if (!configure_socket(stream.socket(), this->settings_)) {
            this->finish_failed(CloseReason::FAILED);
            return;
        }
        this->connection_->ws().async_handshake(
            ws_host_value(this->target_), this->target_.path,
            [self = this->shared_from_this()](const error_code& handshake_ec) {
                self->on_handshake(handshake_ec);
            });
    }

    void on_handshake(const error_code& ec) {
        if (this->done_) {
            return;
        }
        if (ec) {
            this->finish_failed(reason_for(ec));
            return;
        }
        this->service_.hold(this->connection_,
                            [connection = this->connection_] { connection->close_noexcept(); });
        this->finish(this->connection_->core());
    }

    void finish_failed(CloseReason reason) {
        this->connection_->close(close_code::GOING_AWAY);
        this->finish(reason);
    }

    void finish(DialOutcome outcome) {
        this->done_ = true;
        this->service_.to_core([weak = this->core_, outcome = std::move(outcome)] {
            if (const std::shared_ptr<PendingDial::Impl> core = weak.lock()) {
                core->complete(outcome);
            } else if (const auto* connection =
                           std::get_if<std::shared_ptr<Connection::Impl>>(&outcome)) {
                const Connection dropped(*connection);
            }
        });
        this->service_.release(this);
    }

    NetworkService& service_;
    tcp::resolver resolver_;
    std::shared_ptr<NetConnection> connection_;
    std::vector<tcp::endpoint> endpoints_;
    DialTarget target_;
    TransportSettings settings_;
    std::weak_ptr<PendingDial::Impl> core_;
    bool done_{false};
};

}  // namespace

void PendingDial::Impl::cancel() {
    if (this->done_) {
        return;
    }
    this->done_ = true;
    this->on_done_ = nullptr;
    this->service_.to_network([weak = this->net_] {
        if (const std::shared_ptr<NetDial> net = weak.lock()) {
            net->cancel();
        }
    });
}

PendingDial::PendingDial(SharedPtr<Impl> impl) : impl_(std::move(impl)) {}

PendingDial::~PendingDial() {
    if (this->impl_) {
        this->impl_->cancel();
    }
}

PendingDial::PendingDial(PendingDial&& other) noexcept = default;

PendingDial& PendingDial::operator=(PendingDial&& other) noexcept {
    if (this != &other) {
        if (this->impl_) {
            this->impl_->cancel();
        }
        this->impl_ = std::move(other.impl_);
    }
    return *this;
}

void PendingDial::cancel() {
    this->impl_->cancel();
}

PendingDial dial(EventLoop& loop, const DialTarget& target, const TransportSettings& settings,
                 DialFn on_done) {
    NetworkService& service = NetworkService::of(loop.impl().io());
    auto impl = std::make_shared<PendingDial::Impl>(service, std::move(on_done));
    auto net = std::make_shared<NetDial>(service, target, sanitized(settings), impl);
    impl->net_ = net;
    service.to_network([net = std::move(net)] { net->start(); });
    return PendingDial(std::move(impl));
}

// A host's connections take ordinary heap
std::optional<InternalHeap> internal_heap() {
    return std::nullopt;
}

// ==============================================================================
// EventLoop
// ==============================================================================

void EventLoop::Impl::run() {
    this->io_->run();
    this->io_->restart();
    const int64_t deadline_us = now_us() + this->grace_us_;
    // Returns early once no operation is outstanding
    this->io_->run_for(microseconds(this->grace_us_));
    if (asio::has_service<NetworkService>(*this->io_)) {
        NetworkService::of(*this->io_).finish(deadline_us);
    }
}

void EventLoop::Impl::shutdown(int64_t grace_us) {
    this->grace_us_ = grace_us;
    this->work_.reset();
    this->io_->stop();
}

EventLoop::EventLoop() : impl_(std::make_unique<Impl>()) {}

EventLoop::~EventLoop() = default;

void EventLoop::run() {
    this->impl_->run();
}

void EventLoop::post_task(TaskRef task) {
    asio::post(this->impl_->io(), HeapTask(task));
}

void EventLoop::shutdown(int64_t grace_us) {
    this->impl_->shutdown(grace_us);
}

// ==============================================================================
// Timer
// ==============================================================================

class Timer::Impl : public std::enable_shared_from_this<Impl> {
public:
    explicit Impl(asio::io_context& io) : timer_(io) {}

    void start(int64_t at_us, std::function<void()> on_fire) {
        const uint64_t generation = ++this->generation_;
        this->on_fire_ = std::move(on_fire);
        this->timer_.expires_after(microseconds(std::max<int64_t>(at_us - now_us(), 0)));
        // A weak reference: the timer's owner may drop it before the wait completes
        this->timer_.async_wait([weak = this->weak_from_this(), generation](const error_code& ec) {
            const std::shared_ptr<Impl> self = weak.lock();
            if (!ec && self && generation == self->generation_) {
                std::function<void()> on_fire = std::move(self->on_fire_);
                on_fire();
            }
        });
    }

    void cancel() {
        ++this->generation_;
        this->on_fire_ = nullptr;
        this->timer_.cancel();
    }

private:
    asio::steady_timer timer_;
    std::function<void()> on_fire_;
    uint64_t generation_{0};
};

Timer::Timer(EventLoop& loop) : impl_(std::make_shared<Impl>(loop.impl().io())) {}

Timer::~Timer() {
    this->impl_->cancel();
}

void Timer::start(int64_t at_us, std::function<void()> on_fire) {
    this->impl_->start(at_us, std::move(on_fire));
}

void Timer::cancel() {
    this->impl_->cancel();
}

// ==============================================================================
// Wakeup
// ==============================================================================

/// One run is pending at a time, so its posted operation takes the one preallocated block
class Wakeup::Impl {
public:
    /// The largest posted operation the block holds; asio's for this handler is far smaller
    static constexpr size_t BLOCK_BYTES = 256;

    Impl(const std::shared_ptr<asio::io_context>& io, std::function<void()> on_wake)
        : io_(io), on_wake_(std::move(on_wake)) {}

    /// Hands out the block, keeping its Impl alive until the block is returned
    template <typename T>
    class BlockAllocator {
    public:
        using value_type = T;

        explicit BlockAllocator(std::shared_ptr<Impl> owner) : owner_(std::move(owner)) {}

        template <typename U>
        BlockAllocator(const BlockAllocator<U>& other)  // NOLINT(google-explicit-constructor)
            : owner_(other.owner()) {}

        T* allocate(size_t n) {
            if (n * sizeof(T) <= BLOCK_BYTES && !this->owner_->block_used_.exchange(true)) {
                return reinterpret_cast<T*>(this->owner_->block_);
            }
            return std::allocator<T>().allocate(n);
        }

        void deallocate(T* p, size_t n) {
            if (reinterpret_cast<unsigned char*>(p) == this->owner_->block_) {
                this->owner_->block_used_.store(false);
                return;
            }
            std::allocator<T>().deallocate(p, n);
        }

        const std::shared_ptr<Impl>& owner() const {
            return this->owner_;
        }

        template <typename U>
        bool operator==(const BlockAllocator<U>& other) const {
            return this->owner_ == other.owner();
        }

    private:
        std::shared_ptr<Impl> owner_;
    };

    static void signal(const std::shared_ptr<Impl>& self) {
        if (self->pending_.exchange(true)) {
            return;
        }
        // Held for the post, so the io_context outlives it even when its loop is destroyed now
        const std::shared_ptr<asio::io_context> io = self->io_.lock();
        if (!io) {
            return;
        }
        // The handler and the allocator copy a shared_ptr, which counts without allocating
        asio::post(*io, asio::bind_allocator(BlockAllocator<void>(self), [self] {
            if (self->cancelled_) {
                return;
            }
            self->pending_.store(false);
            self->on_wake_();
        }));
    }

    /// Core thread only. A run still pending, or posted by a signal in progress, does nothing,
    /// and pending_ stays set, so later signals post nothing.
    void cancel() {
        this->pending_.store(true);
        this->cancelled_ = true;
        this->on_wake_ = nullptr;
    }

private:
    std::weak_ptr<asio::io_context> io_;
    /// Core thread only
    bool cancelled_{false};
    /// Core thread only
    std::function<void()> on_wake_;
    std::atomic<bool> pending_{false};
    std::atomic<bool> block_used_{false};
    alignas(std::max_align_t) unsigned char block_[BLOCK_BYTES]{};
};

Wakeup::Wakeup(EventLoop& loop, std::function<void()> on_wake)
    : impl_(std::make_shared<Impl>(loop.impl().shared_io(), std::move(on_wake))) {}

Wakeup::~Wakeup() {
    this->impl_->cancel();
}

void Wakeup::signal() {
    Impl::signal(this->impl_);
}

void Wakeup::signal(const Handle& handle) {
    if (handle) {
        Impl::signal(handle);
    }
}

}  // namespace sendspin::server
