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

/// @file thread_setup_host.cpp
/// @brief start_thread() on a host: every thread as std::thread starts it; a thread's token is the
/// address of its own thread_local object

#include "thread_setup.h"

#include <utility>

namespace sendspin::server {

std::thread start_thread(ThreadRole /*role*/, std::function<void()> body) {
    return std::thread(std::move(body));
}

ThreadToken current_thread_token() {
    thread_local char marker = 0;
    return &marker;
}

}  // namespace sendspin::server
