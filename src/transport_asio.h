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

/// @file transport_asio.h
/// @brief The host's EventLoop::Impl: the io_context the core thread runs, for the Asio transport
/// (transport.cpp) and for host code that runs its own Asio objects on that thread

#pragma once

#include "transport.h"
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>

#include <cstdint>
#include <memory>

namespace sendspin::server {

class EventLoop::Impl {
public:
    boost::asio::io_context& io() {
        return *this->io_;
    }

    /// @brief For a holder that may outlive the loop (Wakeup)
    const std::shared_ptr<boost::asio::io_context>& shared_io() {
        return this->io_;
    }

    void run();

    void shutdown(int64_t grace_us);

private:
    std::shared_ptr<boost::asio::io_context> io_{std::make_shared<boost::asio::io_context>()};
    boost::asio::executor_work_guard<boost::asio::io_context::executor_type> work_{
        io_->get_executor()};
    int64_t grace_us_{0};
};

}  // namespace sendspin::server
