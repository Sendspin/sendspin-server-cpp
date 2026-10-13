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

/// @file thread_esp.h
/// @brief The heap capabilities a task's stack is allocated with on ESP-IDF, for the tasks the
/// library creates itself and those it configures in an IDF component (thread_setup_esp.cpp)

#pragma once

#include "memory.h"

#include <cstddef>
#include <cstdint>

namespace sendspin::server {

/// @brief PSRAM for PREFER_EXTERNAL when the build allows task stacks there
/// (CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM) and PSRAM has stack_bytes free; internal RAM
/// otherwise. A free size does not promise a block that large: start_thread() falls back to
/// internal RAM when the stack cannot be allocated, httpd does not start.
uint32_t stack_caps(MemoryLocation location, size_t stack_bytes);

}  // namespace sendspin::server
