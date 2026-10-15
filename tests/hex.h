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

/// @file hex.h
/// @brief Lowercase hex text of bytes and bytes of hex text, for the known-answer tests

#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace sendspin::server::test {

/// @brief size bytes from data as lowercase hex
inline std::string hex(const uint8_t* data, size_t size) {
    std::string out;
    for (size_t i = 0; i < size; ++i) {
        char byte[3];
        std::snprintf(byte, sizeof(byte), "%02x", data[i]);
        out += byte;
    }
    return out;
}

/// @brief A contiguous container's bytes as lowercase hex
template <typename Bytes>
std::string hex(const Bytes& bytes) {
    return hex(bytes.data(), bytes.size());
}

/// @brief The bytes of hex text; an odd last digit is dropped
inline std::vector<uint8_t> from_hex(std::string_view text) {
    std::vector<uint8_t> out;
    for (size_t i = 0; i + 1 < text.size(); i += 2) {
        out.push_back(static_cast<uint8_t>(std::stoi(std::string(text.substr(i, 2)), nullptr, 16)));
    }
    return out;
}

/// @brief The first N bytes of hex text, zero-filled past its end
template <size_t N>
std::array<uint8_t, N> from_hex_array(std::string_view text) {
    const std::vector<uint8_t> bytes = from_hex(text);
    std::array<uint8_t, N> out{};
    std::memcpy(out.data(), bytes.data(), std::min(N, bytes.size()));
    return out;
}

}  // namespace sendspin::server::test
