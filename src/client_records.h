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

/// @file client_records.h
/// @brief The client records: each client's approval for unpaired access, its long-term PSK once
/// paired, its last name and the identifiers that show it speaks a newer revision, held on core and
/// saved through the PersistenceProvider as fixed-size blobs in numbered slots
/// (persistence_keys.h). A blob of another size holds no record, so a layout that changes size
/// drops the records saved before it. The codec and the slot bookkeeping are pure.

#pragma once

#include "crypto/keys.h"
#include "log.h"
#include "sendspin/server/persistence_keys.h"
#include "sendspin/server/server.h"
#include "sendspin/server/types.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace sendspin::server {

/// @brief A Psk that wipes itself when destroyed, so an optional holding one wipes it when reset
/// or assigned an empty one
struct WipedPsk : Psk {
    WipedPsk() : Psk{} {}
    // NOLINTNEXTLINE(google-explicit-constructor,hicpp-explicit-conversions): holds any Psk
    WipedPsk(const Psk& psk) : Psk(psk) {}
    ~WipedPsk();
    WipedPsk(const WipedPsk&) = default;
    WipedPsk& operator=(const WipedPsk&) = default;
    WipedPsk(WipedPsk&&) noexcept = default;
    WipedPsk& operator=(WipedPsk&&) noexcept = default;
};

/// @brief A client's record. Every copy wipes its long-term PSK when it drops it.
struct ClientRecord {
    std::string client_id;
    /// nullopt: not approved, or the operator revoked the approval
    std::optional<ApprovalScope> approval;
    /// Set once the client is paired (pairing.md "Pairing Records")
    std::optional<WipedPsk> long_term_psk;
    /// The name of its last client/hello, cut to persistence_keys::RECORD_NAME_BYTES at a
    /// character boundary
    std::string name;
    /// The client/hello identifiers this server does not recognize (ClientHello::
    /// unrecognized_identifiers), the first persistence_keys::RECORD_IDENTIFIERS of at most
    /// persistence_keys::RECORD_IDENTIFIER_BYTES each
    std::vector<std::string> newer_identifiers;

    ClientRecord() = default;
    ClientRecord(std::string client_id_in, std::optional<ApprovalScope> approval_in)
        : client_id(std::move(client_id_in)), approval(approval_in) {}

    bool operator==(const ClientRecord&) const = default;
};

using ClientRecordBlob = std::array<uint8_t, persistence_keys::CLIENT_RECORD_SIZE>;

/// @brief Sets the name and identifiers a record keeps of a client/hello: the name cut at a
/// character boundary, the identifiers that fit, in order
void set_details(ClientRecord& record, std::string_view name,
                 std::span<const std::string> identifiers);

/// @brief Writes record's blob into blob, which then holds the PSK: wipe it once saved
/// @return false, with blob all zeros, when client_id is not a client's, or the name or the
/// identifiers do not fit
bool encode_client_record(const ClientRecord& record, ClientRecordBlob& blob);

/// @return nullopt for a blob of another size, a key no client_id names (all zeros included),
/// another approval byte, a PSK flag other than 0 or 1, a PSK present but all zeros or absent but
/// not zeros, a length past its field, or nonzero bytes past a field's length
std::optional<ClientRecord> decode_client_record(std::span<const uint8_t> blob);

/// @brief The records by client_id, each with the slot it is saved in
class ClientRecords {
public:
    /// @brief The record loaded from slot. A second record for one client_id leaves its slot free
    /// for reuse.
    void add_loaded(size_t slot, ClientRecord record);

    /// @brief A slot whose key is absent or holds no record, which a new record reuses
    void add_free(size_t slot);

    /// @return nullptr when the client has no record
    const ClientRecord* find(std::string_view client_id) const;

    /// @brief The record's slot; nullopt for a client with no record or one held unsaved
    std::optional<size_t> slot_of(std::string_view client_id) const;

    /// @brief Replaces client_id's record, creating it when it has none
    /// @return the slot to save the record in; nullopt when a new record finds no slot below
    /// persistence_keys::MAX_CLIENT_RECORDS, so it is held unsaved
    std::optional<size_t> set(const ClientRecord& record);

    /// @brief Removes client_id's record
    /// @return the slot it was saved in, now free; nullopt for none, or one held unsaved
    std::optional<size_t> remove(std::string_view client_id);

    size_t size() const {
        return this->records_.size();
    }

private:
    struct Entry {
        /// nullopt for a record held unsaved
        std::optional<size_t> slot;
        ClientRecord record;
    };

    std::map<std::string, Entry, std::less<>> records_;
    /// Slots below next_slot_ that hold no record
    std::vector<size_t> free_;
    /// The first slot never written
    size_t next_slot_{0};
};

/// @brief Loads every slot below persistence_keys::MAX_CLIENT_RECORDS; call on the dispatch
/// thread. An all-zero blob is a removed record's free slot; any other blob that holds no record
/// is logged as an error and its slot reused.
ClientRecords load_client_records(PersistenceProvider& provider, const Logger& log);

}  // namespace sendspin::server
