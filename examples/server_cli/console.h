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

/// @file console.h
/// @brief Console, server_cli's line printer shared by every thread

#pragma once

#include <cstdio>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>

namespace sendspin::server::examples {

/// @brief Prints whole lines from the REPL, the feeder and the server's dispatch thread without
/// interleaving them. Lines carry what clients send (names, the message trace), so control
/// characters are printed as \xNN escapes rather than passed to the terminal.
class Console {
public:
    void print(std::string_view line) {
        std::string out;
        out.reserve(line.size() + 1);
        constexpr unsigned char FIRST_PRINTABLE = 0x20;
        constexpr unsigned char DELETE = 0x7F;
        constexpr unsigned char LOW_NIBBLE = 0x0F;
        constexpr char HEX[] = "0123456789abcdef";
        for (const char c : line) {
            const auto byte = static_cast<unsigned char>(c);
            if (byte < FIRST_PRINTABLE || byte == DELETE) {
                out += "\\x";
                out += HEX[byte >> 4];
                out += HEX[byte & LOW_NIBBLE];
            } else {
                out += c;
            }
        }
        out += '\n';
        const std::lock_guard<std::mutex> lock(this->mutex_);
        std::fputs(out.c_str(), stdout);
        std::fflush(stdout);
        if (this->tap_) {
            out.pop_back();
            this->tap_(out);
        }
    }

    /// @brief Hands every line printed from now on to tap as well, escaped and without its
    /// newline, in the order printed, on the printing thread under the console's lock; nullptr for
    /// none. tap must not print. Once this returns, the tap set before is not called again.
    void set_tap(std::function<void(const std::string&)> tap) {
        const std::lock_guard<std::mutex> lock(this->mutex_);
        this->tap_ = std::move(tap);
    }

private:
    std::mutex mutex_;
    std::function<void(const std::string&)> tap_;
};

}  // namespace sendspin::server::examples
