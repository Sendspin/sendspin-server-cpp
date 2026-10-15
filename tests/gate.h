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

/// @file gate.h
/// @brief Gate, which a job waits at until the test opens it

#pragma once

#include <future>

namespace sendspin::server::test {

/// @brief A gate a job waits at until the test opens it, and which tells the test the job reached
/// it
class Gate {
public:
    /// On the job's thread
    void pass() {
        this->reached_.set_value();
        this->open_.get_future().wait();
    }

    void wait_reached() {
        this->reached_.get_future().wait();
    }

    void open() {
        this->open_.set_value();
    }

private:
    std::promise<void> reached_;
    std::promise<void> open_;
};

}  // namespace sendspin::server::test
