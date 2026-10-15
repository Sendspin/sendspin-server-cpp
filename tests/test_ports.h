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

/// @file test_ports.h
/// @brief Loopback ports for test peers

#pragma once

#include <arpa/inet.h>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdint>

namespace sendspin::server::test {

/// @brief A TCP socket bound to an ephemeral loopback port
inline int bound_loopback_socket() {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    EXPECT_GE(fd, 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    EXPECT_EQ(::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)), 0);
    return fd;
}

/// @brief The address a socket is bound to
inline sockaddr_in local_address(int fd) {
    sockaddr_in address{};
    socklen_t length = sizeof(address);
    EXPECT_EQ(::getsockname(fd, reinterpret_cast<sockaddr*>(&address), &length), 0);
    return address;
}

/// @brief A loopback port that nothing listens on while the object lives, so a dial to it is
/// refused at once, and that no bind to port 0 is given meanwhile
///
/// The port's socket is connected to a listener of its own: macOS drops a connection request to a
/// port whose socket is only bound, so a dial would wait out its connect timeout.
class UnlistenedPort {
public:
    UnlistenedPort() {
        const int listener = bound_loopback_socket();
        EXPECT_EQ(::listen(listener, 1), 0);
        const sockaddr_in listener_address = local_address(listener);
        this->held_ = bound_loopback_socket();
        this->port_ = ntohs(local_address(this->held_).sin_port);
        const int connected =
            ::connect(this->held_, reinterpret_cast<const sockaddr*>(&listener_address),
                      sizeof(listener_address));
        EXPECT_EQ(connected, 0);
        // Nothing waits to be accepted after a failed connect
        if (connected == 0) {
            this->accepted_ = ::accept(listener, nullptr, nullptr);
            EXPECT_GE(this->accepted_, 0);
        }
        ::close(listener);
    }
    ~UnlistenedPort() {
        this->release();
    }
    UnlistenedPort(const UnlistenedPort&) = delete;
    UnlistenedPort& operator=(const UnlistenedPort&) = delete;

    uint16_t port() const {
        return this->port_;
    }

    /// @brief Frees the port, for a peer that sets SO_REUSEADDR (as the sendspin-cpp peer's
    /// listener does) to listen on it next
    void release() {
        ::close(this->accepted_);
        ::close(this->held_);
        this->held_ = -1;
        this->accepted_ = -1;
    }

private:
    int held_{-1};
    int accepted_{-1};
    uint16_t port_{0};
};

/// @brief A loopback port that was free a moment ago, for a peer that cannot bind an ephemeral
/// port itself and report it back; another process may take it before the peer listens
inline uint16_t free_port() {
    const int fd = bound_loopback_socket();
    const uint16_t port = ntohs(local_address(fd).sin_port);
    ::close(fd);
    return port;
}

}  // namespace sendspin::server::test
