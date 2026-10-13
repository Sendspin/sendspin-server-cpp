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

#include "client_records.h"

#include "crypto/base64url.h"
#include "crypto/crypto.h"
#include "crypto/keys.h"

#include <algorithm>
#include <utility>

namespace sendspin::server {

namespace {

constexpr uint8_t APPROVAL_NONE = 0;
constexpr uint8_t APPROVAL_PLAYBACK = 1;
constexpr uint8_t APPROVAL_PLAYBACK_AND_SOURCE = 2;

namespace layout {
constexpr size_t APPROVAL = X25519_KEY_SIZE;
constexpr size_t PSK_FLAG = APPROVAL + 1;
constexpr size_t PSK = PSK_FLAG + 1;
constexpr size_t NAME_LENGTH = PSK + PSK_SIZE;
constexpr size_t NAME = NAME_LENGTH + 1;
constexpr size_t IDENTIFIER_COUNT = NAME + persistence_keys::RECORD_NAME_BYTES;
constexpr size_t IDENTIFIERS = IDENTIFIER_COUNT + 1;
constexpr size_t IDENTIFIER_FIELD = 1 + persistence_keys::RECORD_IDENTIFIER_BYTES;
static_assert(IDENTIFIERS + persistence_keys::RECORD_IDENTIFIERS * IDENTIFIER_FIELD ==
              persistence_keys::CLIENT_RECORD_SIZE);
}  // namespace layout

/// Writes text as a length byte and the text padded with zeros to capacity
bool put_text(ClientRecordBlob& blob, size_t offset, size_t capacity, std::string_view text) {
    if (text.size() > capacity) {
        return false;
    }
    blob[offset] = static_cast<uint8_t>(text.size());
    std::copy(text.begin(), text.end(), blob.begin() + static_cast<std::ptrdiff_t>(offset + 1));
    return true;
}

/// Reads a field put_text() wrote; nullopt for a length past capacity or nonzero padding
std::optional<std::string> get_text(std::span<const uint8_t> blob, size_t offset, size_t capacity) {
    const size_t length = blob[offset];
    if (length > capacity) {
        return std::nullopt;
    }
    const std::span<const uint8_t> field = blob.subspan(offset + 1, capacity);
    if (std::any_of(field.begin() + static_cast<std::ptrdiff_t>(length), field.end(),
                    [](uint8_t byte) { return byte != 0; })) {
        return std::nullopt;
    }
    return std::string(field.begin(), field.begin() + static_cast<std::ptrdiff_t>(length));
}

bool all_zero(std::span<const uint8_t> bytes) {
    return std::all_of(bytes.begin(), bytes.end(), [](uint8_t byte) { return byte == 0; });
}

/// The longest prefix of text of at most capacity bytes that ends at a UTF-8 character boundary
std::string_view cut(std::string_view text, size_t capacity) {
    if (text.size() <= capacity) {
        return text;
    }
    constexpr uint8_t CONTINUATION_MASK = 0xC0;
    constexpr uint8_t CONTINUATION = 0x80;
    size_t end = capacity;
    while (end > 0 && (static_cast<uint8_t>(text[end]) & CONTINUATION_MASK) == CONTINUATION) {
        --end;
    }
    return text.substr(0, end);
}

}  // namespace

WipedPsk::~WipedPsk() {
    secure_zero(this->data(), this->size());
}

void set_details(ClientRecord& record, std::string_view name,
                 std::span<const std::string> identifiers) {
    record.name = std::string(cut(name, persistence_keys::RECORD_NAME_BYTES));
    record.newer_identifiers.clear();
    for (const std::string& identifier : identifiers) {
        if (record.newer_identifiers.size() == persistence_keys::RECORD_IDENTIFIERS) {
            break;
        }
        if (identifier.size() <= persistence_keys::RECORD_IDENTIFIER_BYTES) {
            record.newer_identifiers.push_back(identifier);
        }
    }
}

bool encode_client_record(const ClientRecord& record, ClientRecordBlob& blob) {
    blob.fill(0);
    const std::optional<PublicKey> key = public_key_from_peer_id(record.client_id);
    if (!key || record.newer_identifiers.size() > persistence_keys::RECORD_IDENTIFIERS) {
        return false;
    }
    std::copy(key->begin(), key->end(), blob.begin());
    uint8_t approval = APPROVAL_NONE;
    if (record.approval) {
        approval = *record.approval == ApprovalScope::PLAYBACK ? APPROVAL_PLAYBACK
                                                               : APPROVAL_PLAYBACK_AND_SOURCE;
    }
    blob[layout::APPROVAL] = approval;
    if (record.long_term_psk) {
        blob[layout::PSK_FLAG] = 1;
        std::copy(record.long_term_psk->begin(), record.long_term_psk->end(),
                  blob.begin() + layout::PSK);
    }
    bool fits =
        put_text(blob, layout::NAME_LENGTH, persistence_keys::RECORD_NAME_BYTES, record.name);
    blob[layout::IDENTIFIER_COUNT] = static_cast<uint8_t>(record.newer_identifiers.size());
    for (size_t i = 0; i < record.newer_identifiers.size(); ++i) {
        fits = fits &&
               put_text(blob, layout::IDENTIFIERS + i * layout::IDENTIFIER_FIELD,
                        persistence_keys::RECORD_IDENTIFIER_BYTES, record.newer_identifiers[i]);
    }
    if (!fits) {
        secure_zero(blob.data(), blob.size());
    }
    return fits;
}

std::optional<ClientRecord> decode_client_record(std::span<const uint8_t> blob) {
    if (blob.size() != persistence_keys::CLIENT_RECORD_SIZE) {
        return std::nullopt;
    }
    ClientRecord record;
    record.client_id = b64url_encode(blob.data(), X25519_KEY_SIZE);
    if (!public_key_from_peer_id(record.client_id)) {
        return std::nullopt;
    }
    switch (blob[layout::APPROVAL]) {
        case APPROVAL_NONE:
            break;
        case APPROVAL_PLAYBACK:
            record.approval = ApprovalScope::PLAYBACK;
            break;
        case APPROVAL_PLAYBACK_AND_SOURCE:
            record.approval = ApprovalScope::PLAYBACK_AND_SOURCE;
            break;
        default:
            return std::nullopt;
    }
    const std::span<const uint8_t> psk = blob.subspan(layout::PSK, PSK_SIZE);
    switch (blob[layout::PSK_FLAG]) {
        case 0:
            if (!all_zero(psk)) {
                return std::nullopt;
            }
            break;
        case 1:
            if (all_zero(psk)) {
                return std::nullopt;
            }
            record.long_term_psk.emplace();
            std::copy(psk.begin(), psk.end(), record.long_term_psk->begin());
            break;
        default:
            return std::nullopt;
    }
    std::optional<std::string> name =
        get_text(blob, layout::NAME_LENGTH, persistence_keys::RECORD_NAME_BYTES);
    const size_t count = blob[layout::IDENTIFIER_COUNT];
    if (!name || count > persistence_keys::RECORD_IDENTIFIERS) {
        return std::nullopt;
    }
    record.name = std::move(*name);
    for (size_t i = 0; i < persistence_keys::RECORD_IDENTIFIERS; ++i) {
        const size_t offset = layout::IDENTIFIERS + i * layout::IDENTIFIER_FIELD;
        std::optional<std::string> identifier =
            get_text(blob, offset, persistence_keys::RECORD_IDENTIFIER_BYTES);
        // Fields past the count are all zeros
        if (!identifier || (i >= count && blob[offset] != 0)) {
            return std::nullopt;
        }
        if (i < count) {
            record.newer_identifiers.push_back(std::move(*identifier));
        }
    }
    return record;
}

void ClientRecords::add_loaded(size_t slot, ClientRecord record) {
    this->next_slot_ = std::max(this->next_slot_, slot + 1);
    std::string client_id = record.client_id;
    if (!this->records_.try_emplace(std::move(client_id), Entry{slot, std::move(record)}).second) {
        this->free_.push_back(slot);
    }
}

void ClientRecords::add_free(size_t slot) {
    this->next_slot_ = std::max(this->next_slot_, slot + 1);
    this->free_.push_back(slot);
}

const ClientRecord* ClientRecords::find(std::string_view client_id) const {
    const auto it = this->records_.find(client_id);
    return it == this->records_.end() ? nullptr : &it->second.record;
}

std::optional<size_t> ClientRecords::slot_of(std::string_view client_id) const {
    const auto it = this->records_.find(client_id);
    return it == this->records_.end() ? std::nullopt : it->second.slot;
}

std::optional<size_t> ClientRecords::set(const ClientRecord& record) {
    const auto it = this->records_.find(record.client_id);
    if (it != this->records_.end()) {
        it->second.record = record;
        return it->second.slot;
    }
    std::optional<size_t> slot;
    if (!this->free_.empty()) {
        // The lowest free slot, so that slots stay dense
        const auto lowest = std::min_element(this->free_.begin(), this->free_.end());
        slot = *lowest;
        this->free_.erase(lowest);
    } else if (this->next_slot_ < persistence_keys::MAX_CLIENT_RECORDS) {
        slot = this->next_slot_++;
    }
    this->records_.emplace(record.client_id, Entry{slot, record});
    return slot;
}

std::optional<size_t> ClientRecords::remove(std::string_view client_id) {
    const auto it = this->records_.find(client_id);
    if (it == this->records_.end()) {
        return std::nullopt;
    }
    const std::optional<size_t> slot = it->second.slot;
    this->records_.erase(it);
    if (slot) {
        this->free_.push_back(*slot);
    }
    return slot;
}

ClientRecords load_client_records(PersistenceProvider& provider, const Logger& log) {
    ClientRecords records;
    for (size_t slot = 0; slot < persistence_keys::MAX_CLIENT_RECORDS; ++slot) {
        std::optional<std::vector<uint8_t>> blob =
            provider.load_blob(persistence_keys::client_record_key(slot));
        // A slot whose save failed is absent while later ones hold records
        std::optional<ClientRecord> record;
        if (blob) {
            record = decode_client_record(*blob);
            const bool erased = all_zero(*blob);
            secure_zero(blob->data(), blob->size());
            if (!record && !erased) {
                SS_LOGE(log, "client record slot %zu holds no valid record and is reused", slot);
            }
        }
        if (record) {
            records.add_loaded(slot, std::move(*record));
        } else {
            records.add_free(slot);
        }
    }
    return records;
}

}  // namespace sendspin::server
