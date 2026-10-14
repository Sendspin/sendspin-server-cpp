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

/// @file persistence_keys.h
/// @brief The keys and blob sizes PersistenceProvider is called with

#pragma once

#include <cstddef>
#include <string>

namespace sendspin::server::persistence_keys {

/// @brief The server's static X25519 private key, raw. The server_id derives from it, so clients
/// pair with it: keep it in the host's backup set. start() fails rather than replace a blob of
/// another size, or of all zeros or all 0xFF bytes, as an erased store reads.
inline constexpr const char* SERVER_KEY = "server_key";
inline constexpr size_t SERVER_KEY_SIZE = 32;

/// @brief Prefix of the client record keys; see client_record_key()
inline constexpr const char* CLIENT_RECORD_PREFIX = "client_";
/// @brief The longest client name a client record keeps, in bytes
inline constexpr size_t RECORD_NAME_BYTES = 64;
/// @brief How many unrecognized client/hello identifiers a client record keeps
inline constexpr size_t RECORD_IDENTIFIERS = 4;
/// @brief The longest unrecognized identifier a client record keeps, in bytes
inline constexpr size_t RECORD_IDENTIFIER_BYTES = 32;
/// @brief A client record, in this order: the client's X25519 public key, the key its
/// `client_id` encodes (32 bytes); one byte of approval (0 none, 1 ApprovalScope::PLAYBACK, 2
/// ApprovalScope::PLAYBACK_AND_SOURCE); one byte that is 1 when a long-term PSK follows and 0
/// when the next 32 bytes are zeros; the long-term PSK (32 bytes); the name's length, then the
/// name padded with zeros to RECORD_NAME_BYTES; the identifier count, then RECORD_IDENTIFIERS
/// fields each of a length byte and RECORD_IDENTIFIER_BYTES bytes padded with zeros. A blob of
/// all zeros is a removed record (SendspinServer::unpair()). A blob of another size or with a
/// value outside these holds no record, so records saved in an earlier layout are dropped at
/// start, each logged. The blob holds the long-term PSK: store it as a secret.
inline constexpr size_t CLIENT_RECORD_SIZE = 32 + 1 + 1 + 32 + 1 + RECORD_NAME_BYTES + 1 +
                                             RECORD_IDENTIFIERS * (1 + RECORD_IDENTIFIER_BYTES);
/// @brief The most client records the server keeps
inline constexpr size_t MAX_CLIENT_RECORDS = 1024;

/// @brief The key of client record slot `slot`: CLIENT_RECORD_PREFIX followed by the decimal
/// slot number, such as `client_0`. start() loads every slot below MAX_CLIENT_RECORDS; an absent
/// key or an all-zero blob is a free slot, and the lowest free slot takes the next record.
inline std::string client_record_key(size_t slot) {
    return std::string(CLIENT_RECORD_PREFIX) + std::to_string(slot);
}

}  // namespace sendspin::server::persistence_keys
