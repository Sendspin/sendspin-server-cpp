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

/// @file byte_order.h
/// @brief Big-endian integers, the byte order of the binary messages' fields

#pragma once

#include <climits>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace sendspin::server {

/// @brief Writes value's sizeof(T) bytes at out, most significant first
template <std::unsigned_integral T>
void put_be(uint8_t* out, T value) {
    for (size_t i = 0; i < sizeof(T); ++i) {
        out[i] = static_cast<uint8_t>(value >> (CHAR_BIT * (sizeof(T) - 1 - i)));
    }
}

/// @brief Appends value's sizeof(T) bytes to out, most significant first
template <std::unsigned_integral T>
void append_be(std::vector<uint8_t>& out, T value) {
    for (size_t i = sizeof(T); i > 0; --i) {
        out.push_back(static_cast<uint8_t>(value >> (CHAR_BIT * (i - 1))));
    }
}

/// @brief The unsigned value of the sizeof(T) bytes at in, most significant first
template <std::unsigned_integral T>
T read_be(const uint8_t* in) {
    T value = 0;
    for (size_t i = 0; i < sizeof(T); ++i) {
        value = static_cast<T>((value << CHAR_BIT) | in[i]);
    }
    return value;
}

}  // namespace sendspin::server
