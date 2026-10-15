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

/// @file ws_link.h
/// @brief WsLink: one ESP-IDF WebSocket connection's state shared by its three tasks. The receive
/// task (the connection's own reader task for a dialed connection, httpd's for an accepted one)
/// stamps and assembles inbound messages and pushes them into the loop's inbound ring, and reports
/// the transport's close; the connection's writer task performs each write (a batch of frames, in
/// one send), wait for room, ping and the close, one at a time, so a slow peer blocks only its own
/// writer, and raises their outcomes in the ring; the core task asks for them, and the ring
/// delivers to the endpoint it binds. Both roles frame on the socket alike; a dialed connection's
/// frames are masked (RFC 6455 section 5.3). A dialed connection's writer also starts its reader
/// task, and the writer tears the transport down last, so the receive task never outlives the
/// link.

#pragma once

#include "esp_loop.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "inbound_hold.h"
#include "inbound_ring.h"
#include "lwip/sockets.h"
#include "shared.h"
#include "transport.h"
#include "ws_batch.h"
#include "ws_frame.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace sendspin::server {

/// @brief How often a writer waiting for room checks for a close or a ping
constexpr int WS_ROOM_POLL_MS = 100;
/// @brief The longest a send waits for room at a time before it checks for a close asked for; the
/// longest the receive task waits to answer a close
constexpr int WS_SEND_SLICE_MS = 200;
/// @brief A link keeps up to this much of its message buffer once the message is pushed; a larger
/// one's buffer is freed
constexpr size_t WS_KEPT_MESSAGE_BYTES = 2048;

class WsLink : public InboundProducer, public EnableSharedFromThis<WsLink> {
public:
    /// @param loop held for the link's life: the loop's destructor closes it, so a link holding it
    /// last destroys only what the loop left
    /// @param masks a dialed connection's: every frame it sends is masked with a fresh key
    /// (RFC 6455 section 5.3) from a keystream the writer seeds from the hardware RNG as it starts
    WsLink(SharedPtr<LoopCore> loop, const TransportSettings& settings, bool masks);
    ~WsLink() override;
    WsLink(const WsLink&) = delete;
    WsLink& operator=(const WsLink&) = delete;

    // ---- Core task ----

    /// @brief Starts the writer task (ThreadRole::LINK_WRITER), which for a dialed connection dials
    /// first
    /// @return false when the task could not be created
    bool start();

    /// @brief Routes the link's messages and events to endpoint through the inbound ring, and its
    /// opening to wake, which it signals at once. Once, before start().
    void bind(Wakeup::Handle wake, SharedPtr<InboundEndpoint> endpoint);

    /// @brief Core started the connection: messages are pushed from now on, the first that came
    /// before it included
    void start_inbound();

    void request_room();
    /// @brief While no write is in flight: adds payload, copied (and masked as it is), as one frame
    /// of the next write
    void stage_write(MessageKind kind, std::span<const uint8_t> payload);
    /// @brief Drops the frames staged since the last write, which is then never requested
    void drop_staged();
    /// @brief Starts the write of the frames staged since the last
    void request_write();
    void request_ping();
    /// @brief Final: ends the connection with a close frame carrying code, or drops it with
    /// nullopt, once the operation in progress returns. A message held for the ring, and every
    /// later one, is dropped.
    void request_close(std::optional<uint16_t> code);

    /// @brief Any task: drops the message held and every later one, and resumes a receive task
    /// holding one
    void drop_inbound();

    /// @brief The WebSocket connection is open: dialed and upgraded, or accepted and upgraded
    bool connected() const {
        return this->connected_.load(std::memory_order_acquire);
    }

    /// @brief The first reason the transport reported for its close
    std::optional<CloseReason> closed() const;

    /// @brief The peer's address and port, once connected()
    const std::string& remote_endpoint() const {
        return this->remote_endpoint_;
    }

    /// @brief now_us() at the last inbound frame of any kind, control frames included
    int64_t last_rx_us() const {
        return this->last_rx_us_.load(std::memory_order_acquire);
    }

protected:
    // ---- Receive task ----

    /// @brief One frame's payload is about to arrive: opens a message with a text or binary frame,
    /// or continues one with a continuation frame (RFC 6455 section 5.4)
    /// @return false when the message is dropped (the link is closing) or broke the rules
    /// (reported closed)
    bool begin_frame(uint8_t opcode, size_t payload_bytes);
    /// @brief The place of the payload of the frame begun last, until end_frame(); null for an
    /// empty one
    uint8_t* frame_data() {
        return this->assembly_.frame_bytes() > 0
                   ? this->hold_.message().data() + this->assembly_.message_bytes()
                   : nullptr;
    }
    /// @brief The frame begun last is complete; a final frame offers its message to the ring,
    /// stamped with rx_us, and stops receiving while it is held (hold_receiving()). A message
    /// ws_message_valid() refuses fails the connection with close_code::INVALID_PAYLOAD instead.
    /// @return false when the connection failed or receiving could not stop: the connection is
    /// closing, and the receive task ends it
    bool end_frame(bool fin, int64_t rx_us);

    /// @brief Receive task, as the message of generation is held: stops receiving until it is
    /// released (a session's pause in httpd; a dialed connection's reader task waits)
    /// @return false when it could not stop receiving
    virtual bool hold_receiving(uint64_t generation) = 0;

    void note_rx(int64_t rx_us) {
        this->last_rx_us_.store(rx_us, std::memory_order_release);
    }

    /// @brief The connection's WebSocket is open; remote is its peer
    void report_connected(std::string remote);
    /// @brief Any task: the first report wins; raised in the ring after the message held or being
    /// pushed, if one is
    void report_closed(CloseReason reason);

    /// @brief Raises an event in the ring for the bound endpoint
    void raise(InboundEndpoint::Event event);

    LoopCore& loop() const {
        return *this->loop_;
    }

    void signal_core();
    void wake_writer();

    /// @brief Any task
    bool close_requested() const {
        return (this->requests_.load(std::memory_order_acquire) & REQUEST_CLOSE) != 0;
    }

    // ---- Receive task ----

    /// @brief A ping's payload, for the pong the writer sends (ask_pong())
    void store_ping(const uint8_t* payload, size_t len);
    /// @brief The peer's close: a close frame echoing its status code answers it (RFC 6455 section
    /// 5.5.1), unless a close was sent already, and it is reported closed. A frame the writer has
    /// begun cannot be interrupted; past WS_SEND_SLICE_MS in all, waiting for it and sending, the
    /// close goes unanswered.
    /// @return false when the payload is malformed: fail_protocol() with the code
    /// ws_close_payload_fault() gives instead
    bool answer_close(const uint8_t* payload, size_t len);
    /// @brief The peer broke the protocol: a close frame with code, bounded as answer_close()'s,
    /// fails the connection (RFC 6455 section 7.1.7), reported closed as PROTOCOL_VIOLATION
    void fail_protocol(uint16_t code = close_code::PROTOCOL_ERROR);
    /// @brief Receive task: asks the writer to call send_pong()
    void ask_pong();

    // ---- Writer task: the transport's own operations ----

    /// @brief Before anything else: a dialed connection dials here
    /// @return false when the connection did not open (reported closed) or a close was asked for
    virtual bool open() {
        return true;
    }
    /// @brief Closes the transport, with a close frame when code is set (send_close()); may wait.
    /// The receive task makes no call after it returns.
    virtual void shut(std::optional<uint16_t> code) = 0;

    /// @brief A close frame carrying code
    bool send_close(uint16_t code);

    /// @brief Any task: shuts the socket down with how (SHUT_WR, SHUT_RDWR) unless it is closed
    void shutdown_socket(int how);

    /// @brief The masking key for the next frame sent from keys, or nullopt for an accepted
    /// connection's
    std::optional<WsMaskKey> mask_key(WsMaskKeys& keys) const;

    const TransportSettings settings_;
    std::string remote_endpoint_;

    /// The connection's socket: set before the writer's first use (an accepted connection's at
    /// construction, a dialed one's before report_connected()); closed by httpd for an accepted
    /// connection, by the writer's shut() for a dialed one
    int fd_{-1};
    /// Set under socket_mutex_ once fd_ is closed
    std::atomic<bool> fd_closed_{false};
    /// Held only for a call that moves bytes or shuts the socket, so a close waits no longer than
    /// that
    std::mutex socket_mutex_;

private:
    static constexpr uint32_t REQUEST_ROOM = 1U << 0U;
    static constexpr uint32_t REQUEST_WRITE = 1U << 1U;
    static constexpr uint32_t REQUEST_PING = 1U << 2U;
    static constexpr uint32_t REQUEST_PONG = 1U << 3U;
    static constexpr uint32_t REQUEST_CLOSE = 1U << 4U;

    /// The writer task's body, holding the link until it ends
    static void writer_entry(SharedPtr<WsLink> self);
    void writer_main();
    void request(uint32_t bits);
    /// Sends the pings and pongs asked for; false once a close is asked for
    bool serve_control();
    void wait_for_room();

    /// > 0 when the socket has room, 0 when timeout_ms passed, < 0 on an error, with why in
    /// failure
    int poll_room(int timeout_ms, CloseReason& failure);
    /// The batch's frames in one send, as it holds them
    /// @return true once sent; else false, with why the send failed in failure, or failure unset
    /// when a close was sent, which the task that sent it reports
    bool send_batch(const WsBatch& batch, std::optional<CloseReason>& failure);
    bool send_ping();
    /// Answers the last ping stored (store_ping())
    bool send_pong();
    /// One whole frame, waiting for the frame lock while the receive task answers a close
    bool send_frame(uint8_t opcode, const uint8_t* data, size_t len);
    /// Receive task: a close frame carrying body, within WS_SEND_SLICE_MS of waiting for the frame
    /// lock and sending
    void close_within_slice(const uint8_t* body, size_t len);
    /// Under frame_mutex_: writes one control frame (send_parts()), masked when the link masks
    bool write_frame(uint8_t opcode, const uint8_t* data, size_t len, int64_t deadline_us);
    /// Under frame_mutex_: sends whole frames until they are all sent, deadline_us passes or a
    /// close is asked for. The socket mutex is held only while lwIP copies what fits. A frame left
    /// part written breaks the stream: every later frame, a close included, is refused. A failure
    /// sets send_failure_.
    bool send_parts(iovec* parts, size_t count, int64_t deadline_us);

    const SharedPtr<LoopCore> loop_;
    /// Where messages and events go: set by bind(), before the writer starts and before
    /// hold_.bind() and hold_.start(), so every push and raise sees it
    SharedPtr<InboundEndpoint> endpoint_;

    /// Set once by bind() before wake_bound_ is; a receive task reads it only after
    Wakeup::Handle wake_;
    std::atomic<bool> wake_bound_{false};
    /// Null once the writer task has ended. A wake_writer() between reading it and notifying is
    /// counted in waking_writer_, and the writer task ends only once that count is 0, so it is
    /// never deleted under a notification
    std::atomic<TaskHandle_t> writer_{nullptr};
    std::atomic<int> waking_writer_{0};

    std::atomic<uint32_t> requests_{0};
    /// Core task, then the writer task once REQUEST_WRITE is seen, which clears it as the write
    /// completes; core clears a batch it never requests (drop_staged())
    WsBatch write_batch_;
    /// Core task, then the writer task once REQUEST_CLOSE is seen
    std::optional<uint16_t> close_code_;
    const bool masks_;
    /// Seeded by the writer before open(), so before any room it reports and before the reader
    /// starts; then core_keys_ is the core task's (stage_write()) and frame_keys_ is guarded by
    /// frame_mutex_ (write_frame())
    WsMaskKeys core_keys_;
    WsMaskKeys frame_keys_;
    /// Held for one whole frame, so frames from the writer and the receive task never interleave;
    /// taken before socket_mutex_
    std::timed_mutex frame_mutex_;
    /// Guarded by frame_mutex_: a close frame was sent or begun
    bool close_sent_{false};
    /// Guarded by frame_mutex_: a frame was left part written
    bool frame_broken_{false};
    /// Guarded by frame_mutex_: why the last send_parts() failed
    CloseReason send_failure_{CloseReason::FAILED};
    /// Guarded by socket_mutex_, which no one holds long: the last ping's payload
    std::array<uint8_t, WS_CONTROL_PAYLOAD_MAX> pong_{};
    size_t pong_bytes_{0};
    std::atomic<bool> connected_{false};
    /// -1 while open, else the CloseReason
    std::atomic<int> closed_{-1};
    std::atomic<int64_t> last_rx_us_{0};

protected:
    /// The message being assembled and held, and the connection's close (InboundHold)
    InboundHold hold_;

private:
    /// Receive task only: the message being assembled in hold_'s buffer
    WsAssembly assembly_;
    bool dropping_{false};
};

/// @brief "192.0.2.1:8927" or "[2001:db8::1]:8927" for a connected socket's peer, an IPv4-mapped
/// IPv6 address as IPv4; empty when unknown
std::string peer_endpoint(int fd);

}  // namespace sendspin::server
