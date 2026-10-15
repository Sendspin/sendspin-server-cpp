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

/// @file fragmentation.h
/// @brief Sendspin fragmentation of decrypted binary messages (spec messaging.md
/// "Fragmentation"): inbound reassembly and outbound splitting. Plaintexts here start with the
/// message ID byte.

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace sendspin::server {

/// @brief Message ID of a JSON body (messaging.md "Binary Message ID Structure")
constexpr uint8_t MESSAGE_ID_JSON = 0;
/// @brief Message ID of a fragment
constexpr uint8_t MESSAGE_ID_FRAGMENT = 1;

/// @brief Size of the AEAD tag encryption appends to each Noise transport message, in both cipher
/// suites
constexpr size_t TRANSPORT_TAG_BYTES = 16;

/// @brief Largest plaintext one Noise transport message carries: 65535 bytes less the tag, message
/// ID included
constexpr size_t MAX_TRANSPORT_PLAINTEXT = 65535 - TRANSPORT_TAG_BYTES;

/// @brief Reserves room in an outbound plaintext for plaintext_bytes and the tag, so that
/// TransportCiphers::encrypt() appends the tag without reallocating
inline void reserve_plaintext(std::vector<uint8_t>& plaintext, size_t plaintext_bytes) {
    plaintext.reserve(plaintext_bytes + TRANSPORT_TAG_BYTES);
}

/// @brief Fragment flags: bit 0 marks the last fragment, bit 1 the first, bits 2-7 are reserved
constexpr uint8_t FRAGMENT_FLAG_LAST = 0x01;
constexpr uint8_t FRAGMENT_FLAG_FIRST = 0x02;
constexpr uint8_t FRAGMENT_FLAGS_RESERVED = 0xFC;

/// @brief Inbound reassembly: a single buffer and in-flight `orig_type`, closing the connection
/// on every malformed sequence the spec lists
class Reassembler {
public:
    enum class Outcome : uint8_t {
        PENDING,    ///< A fragment was taken; the message is not complete
        MESSAGE,    ///< message() holds a complete message, ID first
        DISCARDED,  ///< A fragmented message whose ID is not buffered ended; it has no content
        MALFORMED,  ///< A malformed sequence or an empty plaintext; close the connection
        TOO_LARGE,  ///< A fragmented message outgrew max_message_bytes; close the connection
    };

    /// @param max_message_bytes cap on a reassembled message, its ID byte excluded
    /// @param is_buffered whether a fragmented message with this `orig_type` is reassembled;
    /// another type is tracked to its last fragment and DISCARDED, as the ignore rules allow
    Reassembler(size_t max_message_bytes, bool (*is_buffered)(uint8_t message_id));

    /// @brief Takes one decrypted plaintext
    Outcome accept(std::span<const uint8_t> plaintext);

    /// @brief After MESSAGE: the message, ID first. Valid until the next accept(); an unfragmented
    /// message points into the plaintext accept() was given.
    std::span<const uint8_t> message() const {
        return this->message_;
    }

    /// @brief Zeros the reassembly buffer, after a message that may carry a secret
    void wipe();

private:
    Outcome fail(Outcome outcome);

    std::vector<uint8_t> buffer_;
    std::span<const uint8_t> message_;
    size_t max_message_bytes_;
    bool (*is_buffered_)(uint8_t);
    bool in_progress_{false};
    bool discarding_{false};
};

/// @brief Whether a plaintext (ID first) needs more than one Noise transport message
constexpr bool needs_fragments(size_t plaintext_size) {
    return plaintext_size > MAX_TRANSPORT_PLAINTEXT;
}

/// @brief Smallest fragment plaintext write_fragment() is given: room for the headers and data
constexpr size_t MIN_FRAGMENT_PLAINTEXT = 1024;

/// @brief Writes the fragment of a message that starts at offset into out, replacing its
/// contents. The first fragment is the one at offset 0 and carries orig_type.
/// @param payload the message without its ID byte
/// @param max_plaintext the fragment's plaintext size, headers included, unless the payload ends
/// first; from MIN_FRAGMENT_PLAINTEXT to MAX_TRANSPORT_PLAINTEXT
/// @return the offset of the next fragment; payload.size() after the last
size_t write_fragment(uint8_t orig_type, std::span<const uint8_t> payload, size_t offset,
                      size_t max_plaintext, std::vector<uint8_t>& out);

}  // namespace sendspin::server
