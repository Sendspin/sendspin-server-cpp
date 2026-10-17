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

/// @file clock.h
/// @brief The server clock every timestamp is taken from

#pragma once

#include <cstdint>

namespace sendspin::server {

/// @brief Monotonic microseconds at the clock's own rate, never slewed by NTP:
/// `CLOCK_MONOTONIC_RAW` on Linux, `mach_continuous_time` on macOS (which keeps counting while the
/// system sleeps), the esp_timer's `CLOCK_MONOTONIC` on ESP-IDF
int64_t now_us();

}  // namespace sendspin::server
