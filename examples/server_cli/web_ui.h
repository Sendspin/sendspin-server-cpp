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

/// @file web_ui.h
/// @brief WebUi, server_cli's --web dashboard: one HTTP port serving the page and a WebSocket
/// that carries its commands and the server's state and events (web_protocol.md)

#pragma once

#include "console.h"
#include "web_commands.h"

#include <sendspin/server/server.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace sendspin::server::examples {

/// @brief The dashboard's flags: --web <[host:]port> and --media-dir <dir>
struct WebOptions {
    /// --web's address and port; unset for no dashboard
    std::optional<std::string> host;
    uint16_t port{0};
    /// --media-dir; unset for no file browser
    std::optional<std::string> media_dir;

    /// @brief Takes argv[i] when it is one of the dashboard's flags, and its value
    /// @return nullopt when argv[i] is not one; false for a malformed one; true when taken, with i
    /// at its value
    std::optional<bool> parse(int argc, char** argv, int& i);
};

/// @brief Prints the dashboard's flags for server_cli's usage
void print_web_usage();

/// @brief --web's value, `<port>` (on 127.0.0.1) or `<address>:<port>` with a numeric IPv4 or
/// bracketed IPv6 address, or localhost
/// @return the address and port; nullopt when it does not parse
std::optional<std::pair<std::string, uint16_t>> parse_web_address(std::string_view text);

/// @brief The dashboard. Its io_context runs on a thread of its own, which runs every command and
/// pushes the server's state, listener events, log lines and timing to every browser connected.
/// There is no authentication: bound beyond loopback, anyone who reaches the port controls the
/// server.
class WebUi {
public:
    /// @param console every line printed is sent to the browsers' event log from start() on
    /// @param playback the host's playback, which the dashboard drives
    /// @param inner the host's listener, which every listener call reaches first
    WebUi(WebOptions options, Console& console, WebPlayback& playback, ServerListener& inner);
    /// @brief stop()
    ~WebUi();
    WebUi(const WebUi&) = delete;
    WebUi& operator=(const WebUi&) = delete;

    /// @brief Before the server starts: wraps config's message trace to watch timing and the
    /// clients' formats and volumes, and takes the server the dashboard acts on
    /// @param one_group --one-group was given; the dashboard keeps every client in the server's
    /// group while it is set and cannot clear it
    void attach(SendspinServer& server, ServerConfig& config, bool one_group);

    /// @brief The listener the server takes in place of inner: it calls inner, then tells the
    /// browsers. Valid until the WebUi is destroyed, so the WebUi outlives the server.
    ServerListener* listener();

    /// @brief Once the server runs: binds the port and starts the dashboard's thread, and prints
    /// its address
    /// @return false, printing why, when the media directory is not one or the port cannot be bound
    bool start();

    /// @brief Stops the dashboard's thread and closes every connection to it; call it before the
    /// server is destroyed. Idempotent.
    void stop();

    /// @brief The port bound; 0 before start()
    uint16_t port() const;

    /// @brief Calls stop() when it goes out of scope; declared after the server, so the dashboard
    /// stops acting on it before it goes
    class Stopper {
    public:
        explicit Stopper(WebUi* web) : web_(web) {}
        ~Stopper() {
            if (this->web_ != nullptr) {
                this->web_->stop();
            }
        }
        Stopper(const Stopper&) = delete;
        Stopper& operator=(const Stopper&) = delete;

    private:
        WebUi* web_;
    };

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace sendspin::server::examples
