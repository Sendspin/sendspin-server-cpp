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

/// @file thread_setup.h
/// @brief start_thread(): the library's threads, each started with its role's placement where the
/// platform has one, and current_thread_token() (thread_setup_host.cpp, esp/thread_setup_esp.cpp,
/// chosen by CMake)

#pragma once

#include <functional>
#include <thread>

namespace sendspin::server {

/// @brief The library's threads, each with its own stack, core and priority on ESP-IDF; NETWORK
/// exists on a host only, LINK_WRITER (a connection's writer task) on ESP-IDF only
enum class ThreadRole { CORE, NETWORK, DISPATCH, AUDIO_WORKER, ARTWORK, LINK_WRITER };

/// @brief Starts a thread running body with role's placement. The calling thread's own setting
/// for the threads it starts is left as it was, so a host's threads never inherit a role's.
std::thread start_thread(ThreadRole role, std::function<void()> body);

/// @brief Names a running thread; never null
using ThreadToken = const void*;

/// @brief The calling thread's token, distinct from every other running thread's. Any thread, and
/// on ESP-IDF any task, which std::this_thread::get_id() is not: it asserts on a task that is not a
/// pthread (an application's main loop task, esp_timer's task). A token may be reused once its
/// thread ends.
ThreadToken current_thread_token();

}  // namespace sendspin::server
