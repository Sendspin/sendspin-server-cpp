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

#include "ws_handshake.h"

#include "crypto/base64url.h"

#include <algorithm>
#include <cstring>
#include <string>

namespace sendspin::server {

namespace {

/// RFC 6455 section 1.3
constexpr std::string_view ACCEPT_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
constexpr std::string_view CRLF = "\r\n";
constexpr std::string_view BLANK_LINE = "\r\n\r\n";
constexpr size_t SHA1_DIGEST_BYTES = 20;

/// SHA-1 (FIPS 180-4 section 6.1), for Sec-WebSocket-Accept only: its input is public
class Sha1 {
public:
    void update(const uint8_t* data, size_t len) {
        this->total_bytes_ += len;
        while (len > 0) {
            const size_t take = std::min(len, BLOCK - this->buffered_);
            std::memcpy(this->block_.data() + this->buffered_, data, take);
            this->buffered_ += take;
            data += take;
            len -= take;
            if (this->buffered_ == BLOCK) {
                this->process();
                this->buffered_ = 0;
            }
        }
    }

    std::array<uint8_t, SHA1_DIGEST_BYTES> finalize() {
        const uint64_t bits = this->total_bytes_ * 8U;
        constexpr uint8_t PAD = 0x80;
        constexpr size_t LENGTH_BYTES = 8;
        this->block_[this->buffered_++] = PAD;
        if (this->buffered_ > BLOCK - LENGTH_BYTES) {
            std::fill(this->block_.begin() + static_cast<std::ptrdiff_t>(this->buffered_),
                      this->block_.end(), 0);
            this->process();
            this->buffered_ = 0;
        }
        std::fill(this->block_.begin() + static_cast<std::ptrdiff_t>(this->buffered_),
                  this->block_.end() - LENGTH_BYTES, 0);
        for (size_t i = 0; i < LENGTH_BYTES; ++i) {
            this->block_[BLOCK - 1 - i] = static_cast<uint8_t>(bits >> (8U * i));
        }
        this->process();
        std::array<uint8_t, SHA1_DIGEST_BYTES> digest{};
        for (size_t i = 0; i < this->h_.size(); ++i) {
            for (size_t j = 0; j < 4; ++j) {
                digest[(i * 4) + j] = static_cast<uint8_t>(this->h_[i] >> (8U * (3 - j)));
            }
        }
        return digest;
    }

private:
    static constexpr size_t BLOCK = 64;
    static constexpr size_t ROUNDS = 80;
    /// FIPS 180-4 sections 4.1.1 and 4.2.1: a function and a constant for each twenty rounds
    static constexpr size_t STAGE_ROUNDS = 20;
    static constexpr std::array<uint32_t, 4> STAGE_CONSTANTS{0x5A827999, 0x6ED9EBA1, 0x8F1BBCDC,
                                                             0xCA62C1D6};
    /// Section 6.1.2: the message schedule's first words and the earlier words each later one mixes
    static constexpr size_t BLOCK_WORDS = 16;
    static constexpr std::array<size_t, 4> SCHEDULE_TAPS{3, 8, 14, 16};
    static constexpr uint32_t ROTATE_A = 5;
    static constexpr uint32_t ROTATE_B = 30;

    static uint32_t rotl(uint32_t x, uint32_t n) {
        return (x << n) | (x >> (32U - n));
    }

    void process() {
        std::array<uint32_t, ROUNDS> w{};
        for (size_t t = 0; t < BLOCK_WORDS; ++t) {
            w[t] = (uint32_t{this->block_[t * 4]} << 24U) |
                   (uint32_t{this->block_[(t * 4) + 1]} << 16U) |
                   (uint32_t{this->block_[(t * 4) + 2]} << 8U) | this->block_[(t * 4) + 3];
        }
        for (size_t t = BLOCK_WORDS; t < ROUNDS; ++t) {
            w[t] = rotl(w[t - SCHEDULE_TAPS[0]] ^ w[t - SCHEDULE_TAPS[1]] ^
                            w[t - SCHEDULE_TAPS[2]] ^ w[t - SCHEDULE_TAPS[3]],
                        1);
        }
        uint32_t a = this->h_[0];
        uint32_t b = this->h_[1];
        uint32_t c = this->h_[2];
        uint32_t d = this->h_[3];
        uint32_t e = this->h_[4];
        for (size_t t = 0; t < ROUNDS; ++t) {
            const size_t stage = t / STAGE_ROUNDS;
            uint32_t f = b ^ c ^ d;
            if (stage == 0) {
                f = (b & c) | (~b & d);
            } else if (stage == 2) {
                f = (b & c) | (b & d) | (c & d);
            }
            const uint32_t temp = rotl(a, ROTATE_A) + f + e + STAGE_CONSTANTS[stage] + w[t];
            e = d;
            d = c;
            c = rotl(b, ROTATE_B);
            b = a;
            a = temp;
        }
        this->h_[0] += a;
        this->h_[1] += b;
        this->h_[2] += c;
        this->h_[3] += d;
        this->h_[4] += e;
    }

    std::array<uint32_t, 5> h_{0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    std::array<uint8_t, BLOCK> block_{};
    size_t buffered_{0};
    uint64_t total_bytes_{0};
};

char lower(char c) {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

bool equals_ignoring_case(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (size_t i = 0; i < a.size(); ++i) {
        if (lower(a[i]) != lower(b[i])) {
            return false;
        }
    }
    return true;
}

/// Without the optional whitespace around a header value (RFC 9110 section 5.6.3)
std::string_view trimmed(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) {
        text.remove_suffix(1);
    }
    return text;
}

/// Whether the comma-separated list holds token, in any case
bool lists_token(std::string_view list, std::string_view token) {
    while (true) {
        const size_t comma = list.find(',');
        if (equals_ignoring_case(trimmed(list.substr(0, comma)), token)) {
            return true;
        }
        if (comma == std::string_view::npos) {
            return false;
        }
        list.remove_prefix(comma + 1);
    }
}

/// "HTTP/1.1 101", then a reason phrase or nothing
bool switching_protocols(std::string_view status_line) {
    constexpr std::string_view STATUS = "HTTP/1.1 101";
    return status_line.starts_with(STATUS) &&
           (status_line.size() == STATUS.size() || status_line[STATUS.size()] == ' ');
}

}  // namespace

std::string ws_handshake_key(const std::array<uint8_t, WS_KEY_NONCE_BYTES>& nonce) {
    return base64_encode(nonce.data(), nonce.size());
}

std::string ws_accept_value(std::string_view key) {
    Sha1 sha1;
    sha1.update(reinterpret_cast<const uint8_t*>(key.data()), key.size());
    sha1.update(reinterpret_cast<const uint8_t*>(ACCEPT_GUID.data()), ACCEPT_GUID.size());
    const std::array<uint8_t, SHA1_DIGEST_BYTES> digest = sha1.finalize();
    return base64_encode(digest.data(), digest.size());
}

std::string ws_upgrade_request(const DialTarget& target, std::string_view key) {
    // RFC 3986 section 3.2.2: an IPv6 literal is bracketed
    const bool bracketed = target.host.find(':') != std::string::npos;
    std::string request;
    request += "GET ";
    request += target.path;
    request += " HTTP/1.1\r\nHost: ";
    request += bracketed ? "[" + target.host + "]" : target.host;
    request += ":" + std::to_string(target.port);
    request += "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: ";
    request += key;
    request += "\r\nSec-WebSocket-Version: 13\r\n\r\n";
    return request;
}

WsUpgradeCheck check_upgrade_response(std::string_view received, std::string_view key) {
    WsUpgradeCheck check;
    const size_t blank = received.substr(0, WS_UPGRADE_RESPONSE_MAX).find(BLANK_LINE);
    if (blank == std::string_view::npos || blank + BLANK_LINE.size() > WS_UPGRADE_RESPONSE_MAX) {
        if (received.size() >= WS_UPGRADE_RESPONSE_MAX) {
            check.status = WsUpgradeStatus::REJECTED;
        }
        return check;
    }
    check.status = WsUpgradeStatus::REJECTED;
    // The status line and each header line, each ending in CRLF
    std::string_view lines = received.substr(0, blank + CRLF.size());
    size_t end = lines.find(CRLF);
    if (!switching_protocols(lines.substr(0, end))) {
        return check;
    }
    lines.remove_prefix(end + CRLF.size());
    const std::string accept = ws_accept_value(key);
    bool upgrade = false;
    bool connection = false;
    bool accepted = false;
    while (!lines.empty()) {
        end = lines.find(CRLF);
        const std::string_view line = lines.substr(0, end);
        lines.remove_prefix(end + CRLF.size());
        const size_t colon = line.find(':');
        if (colon == std::string_view::npos || colon == 0) {
            return check;
        }
        const std::string_view name = line.substr(0, colon);
        const std::string_view value = trimmed(line.substr(colon + 1));
        if (equals_ignoring_case(name, "Upgrade")) {
            upgrade = equals_ignoring_case(value, "websocket");
        } else if (equals_ignoring_case(name, "Connection")) {
            connection = lists_token(value, "Upgrade");
        } else if (equals_ignoring_case(name, "Sec-WebSocket-Accept")) {
            if (value != accept) {
                return check;
            }
            accepted = true;
        } else if (equals_ignoring_case(name, "Sec-WebSocket-Extensions") ||
                   equals_ignoring_case(name, "Sec-WebSocket-Protocol")) {
            return check;
        }
    }
    if (upgrade && connection && accepted) {
        check.status = WsUpgradeStatus::ACCEPTED;
        check.response_bytes = blank + BLANK_LINE.size();
    }
    return check;
}

}  // namespace sendspin::server
