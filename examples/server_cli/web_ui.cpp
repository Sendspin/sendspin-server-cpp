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

/// @file web_ui.cpp
/// @brief The dashboard's HTTP and WebSocket server on Boost.Beast, its listener and its pushes

#include "web_ui.h"

#include "dashboard_html.h"
#include "one_group.h"
#include "timing_stats.h"
#include <ArduinoJson.h>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/websocket.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <map>
#include <thread>
#include <utility>
#include <vector>

namespace sendspin::server::examples {

namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
using tcp = asio::ip::tcp;

/// Browsers connected at once; one more is refused
constexpr size_t MAX_SESSIONS = 16;
/// Connections open at once, browsers and page loads; one more is closed at accept
constexpr size_t MAX_CONNECTIONS = 64;
/// An HTTP request's header; a longer one is refused
constexpr uint32_t MAX_HEADER_BYTES = 8192;
/// Messages waiting to be written to one browser; a browser that falls this far behind is closed
constexpr size_t MAX_QUEUED_MESSAGES = 256;
/// Log lines kept for a browser that connects
constexpr size_t MAX_LOG_LINES = 500;
/// The time to send an HTTP request's header, and then an upload's body, before the connection is
/// closed
constexpr auto HEADER_TIMEOUT = std::chrono::seconds(10);
constexpr auto BODY_TIMEOUT = std::chrono::seconds(60);
/// How often the state goes out unprompted, for the playback positions
constexpr auto STATE_INTERVAL = std::chrono::seconds(1);
/// How often a timing point is taken, and how many are kept: 10 minutes
constexpr auto TIMING_INTERVAL = std::chrono::seconds(2);
constexpr size_t MAX_TIMING_POINTS = 300;
constexpr uint16_t MAX_PORT = 65535;
constexpr int DECIMAL_BASE = 10;

std::string serialize(const JsonDocument& doc) {
    std::string text;
    serializeJson(doc, text);
    return text;
}

const char* to_string(PairingResult result) {
    switch (result) {
        case PairingResult::PAIRED:
            return "paired";
        case PairingResult::CANCELLED:
            return "cancelled";
        case PairingResult::CODE_MISMATCH:
            return "code_mismatch";
        case PairingResult::TOKEN_MISMATCH:
            return "token_mismatch";
        case PairingResult::METHOD_NOT_SUPPORTED:
            return "method_not_supported";
        case PairingResult::CLIENT_TIMED_OUT:
            return "client_timed_out";
        case PairingResult::CLIENT_CANCELLED:
            return "client_cancelled";
        case PairingResult::CONCURRENT_ATTEMPT:
            return "concurrent_attempt";
        case PairingResult::TIMED_OUT:
            return "timed_out";
        case PairingResult::PERSISTENCE_FAILED:
            return "persistence_failed";
        case PairingResult::PROTOCOL_ERROR:
            return "protocol_error";
        case PairingResult::CONNECTION_LOST:
            return "connection_lost";
        case PairingResult::SERVER_STOPPED:
            return "server_stopped";
    }
    return "unknown";
}

}  // namespace

// ==============================================================================
// Options
// ==============================================================================

std::optional<std::pair<std::string, uint16_t>> parse_web_address(std::string_view text) {
    std::string_view host = "127.0.0.1";
    std::string_view port = text;
    const size_t colon = text.rfind(':');
    if (colon != std::string_view::npos) {
        host = text.substr(0, colon);
        port = text.substr(colon + 1);
        if (host.size() >= 2 && host.front() == '[' && host.back() == ']') {
            host = host.substr(1, host.size() - 2);
        } else if (host.find(':') != std::string_view::npos) {
            // An IPv6 address takes brackets, so its colons are not the port's
            return std::nullopt;
        }
        if (host == "localhost") {
            host = "127.0.0.1";
        }
    }
    const std::string digits(port);
    char* end = nullptr;
    const unsigned long value = std::strtoul(digits.c_str(), &end, DECIMAL_BASE);
    boost::system::error_code error;
    asio::ip::make_address(std::string(host), error);
    const bool numeric = !digits.empty() && std::all_of(digits.begin(), digits.end(), [](char c) {
        return std::isdigit(static_cast<unsigned char>(c)) != 0;
    });
    if (!numeric || *end != '\0' || value > MAX_PORT || error) {
        return std::nullopt;
    }
    return std::make_pair(std::string(host), static_cast<uint16_t>(value));
}

std::optional<bool> WebOptions::parse(int argc, char** argv, int& i) {
    const std::string_view arg = argv[i];
    if (arg != "--web" && arg != "--media-dir") {
        return std::nullopt;
    }
    if (i + 1 >= argc) {
        return false;
    }
    const char* value = argv[++i];
    if (arg == "--media-dir") {
        this->media_dir = value;
        return true;
    }
    const std::optional<std::pair<std::string, uint16_t>> address = parse_web_address(value);
    if (!address) {
        return false;
    }
    this->host = address->first;
    this->port = address->second;
    return true;
}

void print_web_usage() {
    std::printf("  --web <[address:]port>\n"
                "                     serve a dashboard in the browser at this address (default\n"
                "                     127.0.0.1); it has no password, so with an address beyond\n"
                "                     loopback (--web 0.0.0.0:8080 for a phone) anyone on the\n"
                "                     network can control the server\n"
                "  --media-dir <dir>  the directory the dashboard lists and plays files from\n");
}

// ==============================================================================
// The dashboard
// ==============================================================================

class WebUi::Impl {
public:
    class Session;
    class HttpSession;

    /// Counts one connection while it lives; its destructor may run on any thread once the
    /// dashboard's thread has stopped
    class Slot {
    public:
        explicit Slot(Impl& impl) : impl_(impl) {
            ++this->impl_.connections_;
        }
        ~Slot() {
            --this->impl_.connections_;
        }
        Slot(const Slot&) = delete;
        Slot& operator=(const Slot&) = delete;

    private:
        Impl& impl_;
    };

    /// The listener the server calls: the host's first, then the browsers are told
    class Tee : public ServerListener {
    public:
        Tee(Impl& impl, ServerListener& inner) : impl_(impl), inner_(inner) {}

        void on_client_seen(const ClientInfo& client) override {
            this->inner_.on_client_seen(client);
            this->impl_.changed();
        }
        void on_client_connected(const ClientInfo& client) override {
            this->inner_.on_client_connected(client);
            this->impl_.gather();
            this->impl_.changed();
        }
        void on_client_disconnected(const ClientInfo& client,
                                    const ClientDisconnect& disconnect) override {
            this->inner_.on_client_disconnected(client, disconnect);
            this->impl_.changed();
        }
        void on_approval_needed(const ClientInfo& client) override {
            this->inner_.on_approval_needed(client);
            this->impl_.changed();
        }
        void on_pairing_required(const ClientInfo& client) override {
            this->inner_.on_pairing_required(client);
            this->impl_.changed();
        }
        void on_pairing_pending(const ClientInfo& client, std::string_view message) override {
            this->inner_.on_pairing_pending(client, message);
            JsonDocument doc;
            pairing_event(doc, client, "pending");
            doc["message"] = message;
            this->impl_.push(serialize(doc));
            this->impl_.changed();
        }
        void on_pairing_code_needed(const ClientInfo& client, PairMethod method,
                                    CodeFormat format) override {
            this->inner_.on_pairing_code_needed(client, method, format);
            JsonDocument doc;
            pairing_event(doc, client, "code_needed");
            doc["method"] = method == PairMethod::PAIRING_PSK           ? "psk"
                            : method == PairMethod::STATIC_PAIRING_CODE ? "static"
                                                                        : "dynamic";
            doc["format"] = format == CodeFormat::TOKEN     ? "token"
                            : format == CodeFormat::QR_CODE ? "qr"
                                                            : "digits";
            this->impl_.push(serialize(doc));
            this->impl_.changed();
        }
        void on_pairing_result(const ClientInfo& client, PairingResult result) override {
            this->inner_.on_pairing_result(client, result);
            JsonDocument doc;
            pairing_event(doc, client, "result");
            doc["result"] = to_string(result);
            this->impl_.push(serialize(doc));
            this->impl_.changed();
        }
        void on_credential_mismatch(const ClientInfo& client) override {
            this->inner_.on_credential_mismatch(client);
            this->impl_.changed();
        }
        void on_newer_client_detected(const ClientInfo& client,
                                      std::string_view identifier) override {
            this->inner_.on_newer_client_detected(client, identifier);
        }
        void on_transport_command(GroupId group, TransportCommand command,
                                  const CommandArgs& args) override {
            this->inner_.on_transport_command(group, command, args);
            this->impl_.changed();
        }
        void on_error(const ServerError& error) override {
            this->inner_.on_error(error);
            this->impl_.changed();
        }
        void on_group_changed(const GroupInfo& group) override {
            this->inner_.on_group_changed(group);
            this->impl_.gather();
            this->impl_.changed();
        }
        void on_writable(GroupId group, InputId input) override {
            this->inner_.on_writable(group, input);
        }
        void on_drained(GroupId group, InputId input, DrainId drain,
                        DrainOutcome outcome) override {
            this->inner_.on_drained(group, input, drain, outcome);
            this->impl_.changed();
        }

    private:
        /// The fields every pairing message carries
        static void pairing_event(JsonDocument& doc, const ClientInfo& client, const char* event) {
            doc["type"] = "pairing";
            doc["event"] = event;
            doc["client_id"] = client.client_id;
            doc["name"] = client.name;
        }

        Impl& impl_;
        ServerListener& inner_;
    };

    Impl(WebOptions options, Console& console, WebPlayback& playback, ServerListener& inner)
        : options_(std::move(options)),
          console_(console),
          playback_(playback),
          tee_(*this, inner),
          io_(std::make_unique<asio::io_context>()) {}

    ~Impl() {
        this->stop();
        // The sessions and timers before the io_context, whose destruction destroys the handlers
        // that still hold sessions
        this->sessions_.clear();
        this->work_.reset();
        this->acceptor_.reset();
        this->state_timer_.reset();
        this->timing_timer_.reset();
        this->io_.reset();
    }

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    void attach(SendspinServer& server, ServerConfig& config, bool one_group) {
        this->server_ = &server;
        this->server_name_ = config.name;
        this->one_group_.store(one_group);
        this->one_group_fixed_ = one_group;
        config.trace = [inner = std::move(config.trace), this](const TracedMessage& message) {
            if (inner) {
                inner(message);
            }
            this->timing_.add(message);
            this->watch_.add(message);
        };
    }

    ServerListener* listener() {
        return &this->tee_;
    }

    bool start();
    void stop();

    uint16_t port() const {
        return this->port_.load();
    }

    // The members below run on the dashboard's thread

    /// A browser's command: runs it and answers that browser, then pushes the state
    void on_command(Session& session, const std::string& text);
    /// A browser connected: its snapshot and state
    void add_session(const std::shared_ptr<Session>& session);
    void remove_session(const Session* session);
    bool sessions_full() const {
        return this->sessions_.size() >= MAX_SESSIONS;
    }
    /// An HTTP request other than an upgrade or an upload, after its header
    http::response<http::string_body> respond(
        const http::request<http::vector_body<uint8_t>>& req) const;
    /// An upload's checks, after its header and before its body
    /// @return the refusal; nullopt when its body is to be read
    std::optional<http::response<http::string_body>> check_upload(
        const http::request<http::vector_body<uint8_t>>& req) const;
    /// An upload whose check_upload() passed, with its body
    http::response<http::string_body> upload(const http::request<http::vector_body<uint8_t>>& req);
    bool allowed(const http::request<http::vector_body<uint8_t>>& req) const {
        const auto origin = req[http::field::origin];
        const auto host = req[http::field::host];
        return request_allowed(std::string_view(origin.data(), origin.size()),
                               std::string_view(host.data(), host.size()), this->loopback_only_);
    }

private:
    /// From any thread: pushes a message to every browser
    void push(std::string message) {
        asio::post(*this->io_, [this, message = std::move(message)] { this->broadcast(message); });
    }

    /// From any thread: the state changed; one push of it follows, however many changes come first
    void changed() {
        if (!this->state_pending_.exchange(true)) {
            asio::post(*this->io_, [this] {
                this->state_pending_.store(false);
                this->broadcast(this->commands_->state());
            });
        }
    }

    /// On the dispatch thread, as server_cli's --one-group: while the dashboard's one-group mode
    /// is on and --one-group was not given, whose listener gathers instead
    void gather() {
        if (this->one_group_.load() && !this->one_group_fixed_) {
            gather_into_server_group(*this->server_);
        }
    }

    void broadcast(const std::string& message);
    void accept();
    void tick_state();
    void tick_timing();
    void on_log(const std::string& line);
    std::string snapshot() const;

    WebOptions options_;
    Console& console_;
    WebPlayback& playback_;
    Tee tee_;
    SendspinServer* server_{nullptr};
    std::string server_name_;
    std::atomic<bool> one_group_{false};
    bool one_group_fixed_{false};
    TimingStats timing_;
    ClientWatch watch_;
    std::optional<MediaDir> media_;
    std::unique_ptr<WebCommands> commands_;
    bool loopback_only_{true};
    std::atomic<uint16_t> port_{0};
    std::atomic<bool> state_pending_{false};

    std::unique_ptr<asio::io_context> io_;
    std::optional<asio::executor_work_guard<asio::io_context::executor_type>> work_;
    std::unique_ptr<tcp::acceptor> acceptor_;
    std::unique_ptr<asio::steady_timer> state_timer_;
    std::unique_ptr<asio::steady_timer> timing_timer_;
    std::thread thread_;

    // On the dashboard's thread only
    std::vector<std::shared_ptr<Session>> sessions_;
    size_t connections_{0};
    std::deque<std::string> log_;
    /// Each timing point's JSON object, oldest first
    std::deque<std::string> timing_points_;
};

/// @brief One browser's WebSocket: reads commands one at a time and writes a queue of messages
class WebUi::Impl::Session : public std::enable_shared_from_this<Session> {
public:
    Session(tcp::socket socket, Impl& impl) : ws_(std::move(socket)), impl_(impl), slot_(impl) {}

    void start(const http::request<http::vector_body<uint8_t>>& request) {
        this->ws_.set_option(websocket::stream_base::timeout::suggested(beast::role_type::server));
        this->ws_.read_message_max(MAX_COMMAND_BYTES);
        this->ws_.text(true);
        this->ws_.async_accept(request, [self = this->shared_from_this()](beast::error_code ec) {
            if (!ec) {
                self->impl_.add_session(self);
                self->read();
            }
        });
    }

    /// Queues message; a browser too far behind is closed
    void send(const std::shared_ptr<const std::string>& message) {
        if (this->closed_) {
            return;
        }
        if (this->queue_.size() >= MAX_QUEUED_MESSAGES) {
            this->close();
            return;
        }
        this->queue_.push_back(message);
        if (this->queue_.size() == 1) {
            this->write();
        }
    }

    /// Ends the session at once: the reads and writes pending complete with an error
    void close() {
        if (this->closed_) {
            return;
        }
        this->closed_ = true;
        beast::error_code ignored;
        beast::get_lowest_layer(this->ws_).socket().close(ignored);
        this->impl_.remove_session(this);
    }

private:
    void read() {
        this->ws_.async_read(this->buffer_, [self = this->shared_from_this()](beast::error_code ec,
                                                                              size_t /*bytes*/) {
            if (ec || !self->ws_.got_text()) {
                self->close();
                return;
            }
            const std::string text = beast::buffers_to_string(self->buffer_.data());
            self->buffer_.consume(self->buffer_.size());
            self->impl_.on_command(*self, text);
            self->read();
        });
    }

    void write() {
        this->ws_.async_write(
            asio::buffer(*this->queue_.front()),
            [self = this->shared_from_this()](beast::error_code ec, size_t /*bytes*/) {
                if (ec) {
                    self->close();
                    return;
                }
                self->queue_.pop_front();
                if (!self->queue_.empty() && !self->closed_) {
                    self->write();
                }
            });
    }

    websocket::stream<beast::tcp_stream> ws_;
    beast::flat_buffer buffer_;
    Impl& impl_;
    Slot slot_;
    std::deque<std::shared_ptr<const std::string>> queue_;
    bool closed_{false};
};

/// @brief One HTTP request: the page, an image upload, or the upgrade to a Session
class WebUi::Impl::HttpSession : public std::enable_shared_from_this<HttpSession> {
public:
    HttpSession(tcp::socket socket, Impl& impl)
        : stream_(std::move(socket)), impl_(impl), slot_(impl) {}

    void start() {
        this->parser_.header_limit(MAX_HEADER_BYTES);
        // An upload's limit; the parser checks a Content-Length against it with the header, and
        // no other request's body is read
        this->parser_.body_limit(artwork_limits::MAX_IMAGE_BYTES);
        this->stream_.expires_after(HEADER_TIMEOUT);
        http::async_read_header(
            this->stream_, this->buffer_, this->parser_,
            [self = this->shared_from_this()](beast::error_code ec, size_t /*bytes*/) {
                if (ec == http::error::body_limit) {
                    self->too_large();
                } else if (!ec) {
                    self->on_header();
                }
            });
    }

private:
    void on_header() {
        const http::request<http::vector_body<uint8_t>>& req = this->parser_.get();
        if (websocket::is_upgrade(req)) {
            if (req.target() != "/ws" || !this->impl_.allowed(req)) {
                this->reply(http::status::forbidden, "forbidden\n");
                return;
            }
            if (this->impl_.sessions_full()) {
                this->reply(http::status::service_unavailable, "too many browsers connected\n");
                return;
            }
            this->stream_.expires_never();
            std::make_shared<Session>(this->stream_.release_socket(), this->impl_)->start(req);
            return;
        }
        if (req.method() == http::verb::post && req.target().starts_with("/artwork")) {
            // Every check before the body, so only the dashboard's own page makes the server
            // hold one
            if (std::optional<http::response<http::string_body>> refused =
                    this->impl_.check_upload(req)) {
                this->send(std::move(*refused));
                return;
            }
            this->stream_.expires_after(BODY_TIMEOUT);
            http::async_read(
                this->stream_, this->buffer_, this->parser_,
                [self = this->shared_from_this()](beast::error_code ec, size_t /*bytes*/) {
                    if (ec == http::error::body_limit) {
                        self->too_large();
                    } else if (!ec) {
                        self->send(self->impl_.upload(self->parser_.get()));
                    }
                });
            return;
        }
        this->send(this->impl_.respond(req));
    }

    void too_large() {
        this->reply(http::status::payload_too_large, "the image is larger than 32 MiB\n");
    }

    void reply(http::status status, std::string body) {
        http::response<http::string_body> response{status, this->parser_.get().version()};
        response.set(http::field::content_type, "text/plain; charset=utf-8");
        response.body() = std::move(body);
        this->send(std::move(response));
    }

    /// Writes the response and closes the connection: one request per connection
    void send(http::response<http::string_body> response) {
        response.keep_alive(false);
        response.set(http::field::cache_control, "no-store");
        response.set("X-Content-Type-Options", "nosniff");
        response.prepare_payload();
        auto shared = std::make_shared<http::response<http::string_body>>(std::move(response));
        http::async_write(
            this->stream_, *shared,
            [self = this->shared_from_this(), shared](beast::error_code /*ec*/, size_t /*bytes*/) {
                beast::error_code ignored;
                self->stream_.socket().shutdown(tcp::socket::shutdown_send, ignored);
            });
    }

    beast::tcp_stream stream_;
    beast::flat_buffer buffer_;
    http::request_parser<http::vector_body<uint8_t>> parser_;
    Impl& impl_;
    Slot slot_;
};

bool WebUi::Impl::start() {
    if (this->options_.media_dir) {
        this->media_ = MediaDir::open(*this->options_.media_dir);
        if (!this->media_) {
            this->console_.print("dashboard: --media-dir is not a directory: " +
                                 *this->options_.media_dir);
            return false;
        }
    }
    this->commands_ = std::make_unique<WebCommands>(
        *this->server_, this->playback_, this->media_ ? &*this->media_ : nullptr, this->watch_,
        this->one_group_, this->one_group_fixed_);
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access): start() is called with --web given
    const std::string& host = *this->options_.host;
    beast::error_code error;
    const asio::ip::address address = asio::ip::make_address(host, error);
    const tcp::endpoint endpoint(address, this->options_.port);
    this->acceptor_ = std::make_unique<tcp::acceptor>(*this->io_);
    if (!error) {
        this->acceptor_->open(endpoint.protocol(), error);
    }
    if (!error) {
        this->acceptor_->set_option(asio::socket_base::reuse_address(true), error);
    }
    if (!error) {
        this->acceptor_->bind(endpoint, error);
    }
    if (!error) {
        this->acceptor_->listen(asio::socket_base::max_listen_connections, error);
    }
    if (error) {
        this->console_.print("dashboard: cannot listen on " + host + ":" +
                             std::to_string(this->options_.port) + ": " + error.message());
        return false;
    }
    this->loopback_only_ = address.is_loopback();
    this->port_.store(this->acceptor_->local_endpoint().port());
    this->state_timer_ = std::make_unique<asio::steady_timer>(*this->io_);
    this->timing_timer_ = std::make_unique<asio::steady_timer>(*this->io_);
    this->work_.emplace(this->io_->get_executor());
    this->console_.set_tap([this](const std::string& line) {
        asio::post(*this->io_, [this, text = std::string(line)] { this->on_log(text); });
    });
    this->accept();
    this->tick_state();
    this->tick_timing();
    this->thread_ = std::thread([this] { this->io_->run(); });
    const std::string where =
        (address.is_v6() ? "[" + host + "]" : host) + ":" + std::to_string(this->port());
    if (this->loopback_only_) {
        this->console_.print("dashboard: http://" + where + "/ (this machine only)");
    } else {
        this->console_.print("dashboard: http://" + where +
                             "/ WARNING: it has no password; anyone on the network who can "
                             "reach this port can control the server");
    }
    return true;
}

void WebUi::Impl::stop() {
    if (!this->thread_.joinable()) {
        return;
    }
    this->console_.set_tap(nullptr);
    this->io_->stop();
    this->thread_.join();
}

void WebUi::Impl::accept() {
    this->acceptor_->async_accept([this](beast::error_code ec, tcp::socket socket) {
        if (ec == asio::error::operation_aborted) {
            return;
        }
        if (!ec && this->connections_ < MAX_CONNECTIONS) {
            std::make_shared<HttpSession>(std::move(socket), *this)->start();
        }
        this->accept();
    });
}

void WebUi::Impl::tick_state() {
    this->state_timer_->expires_after(STATE_INTERVAL);
    this->state_timer_->async_wait([this](beast::error_code ec) {
        if (ec) {
            return;
        }
        if (!this->sessions_.empty()) {
            this->broadcast(this->commands_->state());
        }
        this->tick_state();
    });
}

void WebUi::Impl::tick_timing() {
    this->timing_timer_->expires_after(TIMING_INTERVAL);
    this->timing_timer_->async_wait([this](beast::error_code ec) {
        if (ec) {
            return;
        }
        JsonDocument point;
        point["t"] = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();
        JsonObject clients = point["clients"].to<JsonObject>();
        for (const auto& [client_id, holds] : this->timing_.take_holds_by_client()) {
            JsonObject entry = clients[client_id].to<JsonObject>();
            entry["n"] = holds.count;
            entry["p50_us"] = holds.p50;
            entry["p99_us"] = holds.p99;
            entry["max_us"] = holds.max;
        }
        std::string text = serialize(point);
        JsonDocument message;
        message["type"] = "timing";
        message["point"] = serialized(text);
        this->broadcast(serialize(message));
        this->timing_points_.push_back(std::move(text));
        if (this->timing_points_.size() > MAX_TIMING_POINTS) {
            this->timing_points_.pop_front();
        }
        this->tick_timing();
    });
}

void WebUi::Impl::on_log(const std::string& line) {
    this->log_.push_back(line);
    if (this->log_.size() > MAX_LOG_LINES) {
        this->log_.pop_front();
    }
    JsonDocument doc;
    doc["type"] = "log";
    doc["line"] = line;
    this->broadcast(serialize(doc));
}

void WebUi::Impl::broadcast(const std::string& message) {
    const auto shared = std::make_shared<const std::string>(valid_utf8(message));
    // A send may close its session, which leaves sessions_
    const std::vector<std::shared_ptr<Session>> sessions = this->sessions_;
    for (const std::shared_ptr<Session>& session : sessions) {
        session->send(shared);
    }
}

std::string WebUi::Impl::snapshot() const {
    JsonDocument doc;
    doc["type"] = "snapshot";
    JsonObject server = doc["server"].to<JsonObject>();
    server["name"] = this->server_name_;
    server["server_id"] = this->server_->server_id();
    server["listen_port"] = this->server_->listen_port();
    server["loopback_only"] = this->loopback_only_;
    server["max_image_bytes"] = artwork_limits::MAX_IMAGE_BYTES;
    doc["timing_interval_s"] =
        std::chrono::duration_cast<std::chrono::seconds>(TIMING_INTERVAL).count();
    JsonArray timing = doc["timing"].to<JsonArray>();
    for (const std::string& point : this->timing_points_) {
        timing.add(serialized(point));
    }
    JsonArray log = doc["log"].to<JsonArray>();
    for (const std::string& line : this->log_) {
        log.add(line);
    }
    return serialize(doc);
}

void WebUi::Impl::add_session(const std::shared_ptr<Session>& session) {
    // Upgrades accepted together pass the check before the upgrade; the cap holds here
    if (this->sessions_full()) {
        session->close();
        return;
    }
    this->sessions_.push_back(session);
    session->send(std::make_shared<const std::string>(valid_utf8(this->snapshot())));
    session->send(std::make_shared<const std::string>(valid_utf8(this->commands_->state())));
}

void WebUi::Impl::remove_session(const Session* session) {
    std::erase_if(this->sessions_,
                  [&](const std::shared_ptr<Session>& entry) { return entry.get() == session; });
}

void WebUi::Impl::on_command(Session& session, const std::string& text) {
    session.send(std::make_shared<const std::string>(valid_utf8(this->commands_->run(text))));
    this->changed();
}

http::response<http::string_body> WebUi::Impl::respond(
    const http::request<http::vector_body<uint8_t>>& req) const {
    http::response<http::string_body> response{http::status::ok, req.version()};
    if (!this->allowed(req)) {
        response.result(http::status::forbidden);
        response.set(http::field::content_type, "text/plain; charset=utf-8");
        response.body() = "forbidden\n";
        return response;
    }
    const std::string_view target(req.target().data(), req.target().size());
    if (req.method() == http::verb::get && (target == "/" || target == "/index.html")) {
        response.set(http::field::content_type, "text/html; charset=utf-8");
        // The page's own script and styles, and its WebSocket back to this server
        response.set("Content-Security-Policy",
                     "default-src 'none'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; "
                     "connect-src 'self'; img-src 'self' data:; base-uri 'none'; "
                     "form-action 'none'; frame-ancestors 'none'");
        response.set("Referrer-Policy", "no-referrer");
        response.body().assign(reinterpret_cast<const char*>(DASHBOARD_HTML), DASHBOARD_HTML_SIZE);
        return response;
    }
    response.result(http::status::not_found);
    response.set(http::field::content_type, "text/plain; charset=utf-8");
    response.body() = "not found\n";
    return response;
}

namespace {

/// An upload's answer: {"ok":true}, or {"ok":false,"error":error}
http::response<http::string_body> upload_answer(unsigned version, http::status status,
                                                const char* error) {
    http::response<http::string_body> response{status, version};
    response.set(http::field::content_type, "application/json");
    JsonDocument doc;
    doc["ok"] = error == nullptr;
    if (error != nullptr) {
        doc["error"] = error;
    }
    response.body() = serialize(doc);
    return response;
}

}  // namespace

std::optional<http::response<http::string_body>> WebUi::Impl::check_upload(
    const http::request<http::vector_body<uint8_t>>& req) const {
    if (!this->allowed(req)) {
        return upload_answer(req.version(), http::status::forbidden, "forbidden");
    }
    // A form of another site cannot set this header; a script of another site needs the
    // cross-origin permission the dashboard never grants
    if (req["X-Sendspin-Dashboard"] != "1") {
        return upload_answer(req.version(), http::status::forbidden,
                             "the X-Sendspin-Dashboard header is required");
    }
    const std::optional<ArtworkTarget> target =
        parse_artwork_target(std::string_view(req.target().data(), req.target().size()));
    if (!target) {
        return upload_answer(req.version(), http::status::bad_request,
                             "group and source (album or artist) are required");
    }
    const std::vector<GroupInfo> groups = this->server_->groups();
    if (std::none_of(groups.begin(), groups.end(),
                     [&](const GroupInfo& info) { return info.id == target->group; })) {
        return upload_answer(req.version(), http::status::not_found, "no such group");
    }
    return std::nullopt;
}

http::response<http::string_body> WebUi::Impl::upload(
    const http::request<http::vector_body<uint8_t>>& req) {
    // check_upload() parsed it
    const std::optional<ArtworkTarget> target =
        parse_artwork_target(std::string_view(req.target().data(), req.target().size()));
    if (!target || !this->server_->set_artwork(target->group, target->source, req.body(),
                                               this->server_->now_us())) {
        return upload_answer(req.version(), http::status::unprocessable_entity,
                             "not a JPEG or PNG image the server takes, or the group is gone");
    }
    return upload_answer(req.version(), http::status::ok, nullptr);
}

// ==============================================================================
// WebUi
// ==============================================================================

WebUi::WebUi(WebOptions options, Console& console, WebPlayback& playback, ServerListener& inner)
    : impl_(std::make_unique<Impl>(std::move(options), console, playback, inner)) {}

WebUi::~WebUi() = default;

void WebUi::attach(SendspinServer& server, ServerConfig& config, bool one_group) {
    this->impl_->attach(server, config, one_group);
}

ServerListener* WebUi::listener() {
    return this->impl_->listener();
}

bool WebUi::start() {
    return this->impl_->start();
}

void WebUi::stop() {
    this->impl_->stop();
}

uint16_t WebUi::port() const {
    return this->impl_->port();
}

}  // namespace sendspin::server::examples
