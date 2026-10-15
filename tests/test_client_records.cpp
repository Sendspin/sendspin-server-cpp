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

/// @file test_client_records.cpp
/// @brief The client record codec, the slot bookkeeping, loading from a provider and the wiping of
/// a long-term PSK a record drops

#include "client_records.h"
#include "crypto/keys.h"
#include "dispatcher.h"
#include "log.h"
#include "test_recorder.h"
#include <gtest/gtest.h>
#include <noise/protocol.h>

#include <sendspin/server/persistence_keys.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

using sendspin::server::ApprovalScope;
using sendspin::server::ClientRecord;
using sendspin::server::ClientRecords;
namespace persistence_keys = sendspin::server::persistence_keys;

std::string new_client_id() {
    EXPECT_EQ(noise_init_framework(), NOISE_ERROR_NONE);
    return sendspin::server::Identity::generate()->peer_id();
}

std::vector<uint8_t> blob_of(const ClientRecord& record) {
    sendspin::server::ClientRecordBlob blob{};
    EXPECT_TRUE(sendspin::server::encode_client_record(record, blob));
    return {blob.begin(), blob.end()};
}

ClientRecord record_with(const std::string& id, std::optional<ApprovalScope> approval,
                         std::optional<uint8_t> psk_byte, std::string name,
                         std::vector<std::string> identifiers) {
    ClientRecord record{id, approval};
    if (psk_byte) {
        record.long_term_psk.emplace();
        record.long_term_psk->fill(*psk_byte);
    }
    record.name = std::move(name);
    record.newer_identifiers = std::move(identifiers);
    return record;
}

// The offsets of persistence_keys.h's layout
constexpr size_t VERSION = 0;
constexpr size_t KEY = 1;
constexpr size_t APPROVAL = 33;
constexpr size_t PSK_FLAG = 34;
constexpr size_t PSK = 35;
constexpr size_t NAME_LENGTH = 67;
constexpr size_t NAME = 68;
constexpr size_t IDENTIFIER_COUNT = NAME + persistence_keys::RECORD_NAME_BYTES;
constexpr size_t IDENTIFIERS = IDENTIFIER_COUNT + 1;

// persistence_keys.h: every field round-trips, at its limits too
TEST(ClientRecordCodec, RoundTripsEveryField) {
    const std::string id = new_client_id();
    const std::string longest_name(persistence_keys::RECORD_NAME_BYTES, 'n');
    const std::vector<std::string> most_identifiers(
        persistence_keys::RECORD_IDENTIFIERS,
        std::string(persistence_keys::RECORD_IDENTIFIER_BYTES, 'i'));
    const ClientRecord records[] = {
        {id, std::nullopt},  // Control: an empty record
        {id, ApprovalScope::PLAYBACK},
        {id, ApprovalScope::PLAYBACK_AND_SOURCE},
        record_with(id, std::nullopt, 0x5A, "Kitchen", {"player@v2"}),
        record_with(id, ApprovalScope::PLAYBACK, 0x01, longest_name, most_identifiers),
    };
    for (const ClientRecord& record : records) {
        SCOPED_TRACE(record.name);
        const std::vector<uint8_t> blob = blob_of(record);
        ASSERT_EQ(blob.size(), persistence_keys::CLIENT_RECORD_SIZE);
        EXPECT_EQ(blob[VERSION], persistence_keys::CLIENT_RECORD_VERSION);
        EXPECT_EQ(blob[APPROVAL],
                  record.approval ? (*record.approval == ApprovalScope::PLAYBACK ? 1 : 2) : 0);
        EXPECT_EQ(blob[PSK_FLAG], record.long_term_psk ? 1 : 0);
        EXPECT_EQ(sendspin::server::decode_client_record(blob), record);
    }
}

// Only what fits the fixed layout encodes; a failed encode leaves no PSK in the blob
TEST(ClientRecordCodec, RefusesWhatDoesNotFit) {
    const std::string id = new_client_id();
    struct Row {
        const char* name;
        ClientRecord record;
        bool encodes;
    };
    const Row rows[] = {
        {"Control: a paired record", record_with(id, std::nullopt, 7, "n", {"x"}), true},
        {"not a client id", record_with("not a client id", std::nullopt, 7, "n", {}), false},
        {"a long name",
         record_with(id, std::nullopt, 7, std::string(persistence_keys::RECORD_NAME_BYTES + 1, 'n'),
                     {}),
         false},
        {"a long identifier",
         record_with(id, std::nullopt, 7, "n",
                     {std::string(persistence_keys::RECORD_IDENTIFIER_BYTES + 1, 'i')}),
         false},
        {"too many identifiers",
         record_with(id, std::nullopt, 7, "n",
                     std::vector<std::string>(persistence_keys::RECORD_IDENTIFIERS + 1, "i")),
         false},
    };
    for (const Row& row : rows) {
        SCOPED_TRACE(row.name);
        sendspin::server::ClientRecordBlob blob{};
        blob.fill(0xFF);
        EXPECT_EQ(sendspin::server::encode_client_record(row.record, blob), row.encodes);
        if (!row.encodes) {
            EXPECT_EQ(blob, sendspin::server::ClientRecordBlob{});
        }
    }
}

// persistence_keys.h: a blob of another size or version, or with any value outside the layout,
// holds no record
TEST(ClientRecordCodec, RejectsBlobsOutsideTheLayout) {
    const std::string id = new_client_id();
    const std::vector<uint8_t> valid =
        blob_of(record_with(id, ApprovalScope::PLAYBACK, 7, "ab", {"x"}));
    ASSERT_TRUE(sendspin::server::decode_client_record(valid));  // Control
    const std::vector<uint8_t> unpaired =
        blob_of(record_with(id, std::nullopt, std::nullopt, "", {}));
    ASSERT_TRUE(sendspin::server::decode_client_record(unpaired));  // Control
    const auto edit = [](std::vector<uint8_t> blob, size_t offset, uint8_t value) {
        blob[offset] = value;
        return blob;
    };
    std::vector<uint8_t> zero_psk = valid;
    std::fill(zero_psk.begin() + PSK, zero_psk.begin() + PSK + 32, 0);
    const std::vector<uint8_t> short_blob(valid.begin() + KEY, valid.begin() + APPROVAL + 1);
    struct Row {
        const char* name;
        std::vector<uint8_t> blob;
    };
    const Row rows[] = {
        {"version 0", edit(valid, VERSION, 0)},
        {"a later version", edit(valid, VERSION, persistence_keys::CLIENT_RECORD_VERSION + 1)},
        {"another approval byte", edit(valid, APPROVAL, 3)},
        {"another PSK flag", edit(valid, PSK_FLAG, 2)},
        {"a PSK flagged absent that is not zeros", edit(valid, PSK_FLAG, 0)},
        {"a PSK flagged present that is zeros", zero_psk},
        {"a name past its field",
         edit(valid, NAME_LENGTH, persistence_keys::RECORD_NAME_BYTES + 1)},
        {"a byte past the name", edit(valid, NAME + 2, 'c')},
        {"too many identifiers",
         edit(valid, IDENTIFIER_COUNT, persistence_keys::RECORD_IDENTIFIERS + 1)},
        {"an identifier past the count", edit(edit(valid, IDENTIFIER_COUNT, 0), IDENTIFIERS, 1)},
        {"a byte past an identifier", edit(valid, IDENTIFIERS + 2, 'y')},
        {"one byte short", std::vector<uint8_t>(valid.begin(), valid.end() - 1)},
        {"one byte long", edit(std::vector<uint8_t>(valid.size() + 1), 0, 0)},
        {"a free slot's zeros", std::vector<uint8_t>(persistence_keys::CLIENT_RECORD_SIZE, 0)},
        {"a 33-byte blob", short_blob},
    };
    for (const Row& row : rows) {
        SCOPED_TRACE(row.name);
        EXPECT_FALSE(sendspin::server::decode_client_record(row.blob));
    }
}

// A record keeps what fits of a client/hello: the name cut before a character that would not
// fit whole, and the identifiers that fit, the first ones
TEST(ClientRecordDetails, KeepsWhatFits) {
    // Two-byte characters straddle the limit
    std::string name(persistence_keys::RECORD_NAME_BYTES - 1, 'n');
    name += "\xC3\xA9";
    const std::string fitting(persistence_keys::RECORD_IDENTIFIER_BYTES, 'f');
    const std::string too_long(persistence_keys::RECORD_IDENTIFIER_BYTES + 1, 'l');
    const std::vector<std::string> identifiers{too_long, "a", fitting, "b", "c", "d"};
    ClientRecord record{new_client_id(), std::nullopt};
    sendspin::server::set_details(record, name, identifiers);
    EXPECT_EQ(record.name, std::string(persistence_keys::RECORD_NAME_BYTES - 1, 'n'));
    EXPECT_EQ(record.newer_identifiers, (std::vector<std::string>{"a", fitting, "b", "c"}));
    // Control: a name that fits is kept whole
    sendspin::server::set_details(record,
                                  "K\xC3\xBC"
                                  "che",
                                  {});
    EXPECT_EQ(record.name, "K\xC3\xBC"
                           "che");
    EXPECT_TRUE(record.newer_identifiers.empty());
    EXPECT_TRUE(sendspin::server::decode_client_record(blob_of(record)));
}

/// Whether psk's bytes are anywhere in the storage of record's long_term_psk, held or not
bool psk_bytes_in(const ClientRecord& record, const sendspin::server::Psk& psk) {
    std::array<uint8_t, sizeof(record.long_term_psk)> storage{};
    std::memcpy(storage.data(), static_cast<const void*>(&record.long_term_psk), storage.size());
    return std::search(storage.begin(), storage.end(), psk.begin(), psk.end()) != storage.end();
}

// A record that drops its long-term PSK wipes it: assigned a record without one, by copy or by
// move, or reset. White-box: reads the bytes the optional leaves behind. Control: the PSK is
// found while held.
TEST(ClientRecordSecrets, WipesAPskTheRecordDrops) {
    struct Row {
        const char* name;
        void (*drop)(ClientRecord& record);
        bool held;
    };
    const Row rows[] = {
        {"Control: nothing dropped", [](ClientRecord& /*record*/) {}, true},
        {"copy-assigned a record without one",
         [](ClientRecord& record) {
             const ClientRecord without{record.client_id, std::nullopt};
             record = without;
         },
         false},
        {"move-assigned a record without one",
         [](ClientRecord& record) { record = ClientRecord{record.client_id, std::nullopt}; },
         false},
        {"reset", [](ClientRecord& record) { record.long_term_psk.reset(); }, false},
    };
    sendspin::server::Psk psk{};
    for (size_t i = 0; i < psk.size(); ++i) {
        psk[i] = static_cast<uint8_t>(0xC0 + i);
    }
    for (const Row& row : rows) {
        SCOPED_TRACE(row.name);
        ClientRecord record{"id", std::nullopt};
        record.long_term_psk = psk;
        row.drop(record);
        EXPECT_EQ(record.long_term_psk.has_value(), row.held);
        EXPECT_EQ(psk_bytes_in(record, psk), row.held);
    }
}

// persistence_keys.h: slots fill from 0 up, a slot that held no record is reused first, and past
// MAX_CLIENT_RECORDS a record is held unsaved
TEST(ClientRecords, AssignsSlotsDenselyAndHoldsWhatDoesNotFit) {
    const std::string a = new_client_id();
    const std::string b = new_client_id();
    const std::string c = new_client_id();
    const std::string d = new_client_id();
    ClientRecords records;
    records.add_loaded(0, {a, ApprovalScope::PLAYBACK});
    records.add_free(1);
    records.add_loaded(2, {b, std::nullopt});
    // A second record for one client leaves its slot free
    records.add_loaded(3, {a, std::nullopt});
    ASSERT_NE(records.find(a), nullptr);
    EXPECT_EQ(records.find(a)->approval, ApprovalScope::PLAYBACK);
    EXPECT_EQ(records.find(c), nullptr);

    // Control: an existing record keeps its slot
    EXPECT_EQ(records.set({b, ApprovalScope::PLAYBACK}), 2U);
    EXPECT_EQ(records.find(b)->approval, ApprovalScope::PLAYBACK);
    EXPECT_EQ(records.set({c, ApprovalScope::PLAYBACK}), 1U);
    EXPECT_EQ(records.set({d, std::nullopt}), 3U);
    EXPECT_EQ(records.set({new_client_id(), std::nullopt}), 4U);
    EXPECT_EQ(records.size(), 5U);

    ClientRecords full;
    full.add_loaded(persistence_keys::MAX_CLIENT_RECORDS - 1, {a, std::nullopt});
    EXPECT_EQ(full.set({b, ApprovalScope::PLAYBACK}), std::nullopt);
    EXPECT_EQ(full.slot_of(b), std::nullopt);
    EXPECT_EQ(full.slot_of(a), persistence_keys::MAX_CLIENT_RECORDS - 1);
    ASSERT_NE(full.find(b), nullptr);
    EXPECT_EQ(full.find(b)->approval, ApprovalScope::PLAYBACK);
}

// persistence_keys::client_record_key(): every slot is read, so a record after a slot whose save
// failed (absent) loads; an absent or unreadable slot is reused first, and an unreadable one is
// logged as an error
TEST(ClientRecords, LoadsEverySlotPastAGap) {
    const std::string a = new_client_id();
    const std::string b = new_client_id();
    const std::string c = new_client_id();
    sendspin::server::test::MemoryProvider provider;
    provider.blobs[persistence_keys::client_record_key(0)] = blob_of({a, ApprovalScope::PLAYBACK});
    provider.blobs[persistence_keys::client_record_key(1)] = {1, 2, 3};
    provider.blobs[persistence_keys::client_record_key(2)] = blob_of({b, std::nullopt});
    provider.blobs[persistence_keys::client_record_key(4)] = blob_of({c, ApprovalScope::PLAYBACK});
    ClientRecords records;
    std::vector<sendspin::server::LogLevel> logged;
    {
        sendspin::server::Dispatcher dispatcher;
        const sendspin::server::Logger log(
            dispatcher,
            [&logged](sendspin::server::LogLevel level, std::string_view /*line*/) {
                logged.push_back(level);
            },
            sendspin::server::LogLevel::INFO);
        records = sendspin::server::load_client_records(provider, log);
    }
    // The unreadable slot is an error; an absent one is not
    EXPECT_EQ(logged, std::vector<sendspin::server::LogLevel>{sendspin::server::LogLevel::ERROR});
    EXPECT_EQ(records.size(), 3U);
    ASSERT_NE(records.find(a), nullptr);
    EXPECT_EQ(records.find(a)->approval, ApprovalScope::PLAYBACK);
    ASSERT_NE(records.find(b), nullptr);
    EXPECT_FALSE(records.find(b)->approval);
    ASSERT_NE(records.find(c), nullptr);
    EXPECT_EQ(records.find(c)->approval, ApprovalScope::PLAYBACK);
    // The unreadable slot, then the absent one, then the one after the last record
    EXPECT_EQ(records.set({new_client_id(), std::nullopt}), 1U);
    EXPECT_EQ(records.set({new_client_id(), std::nullopt}), 3U);
    EXPECT_EQ(records.set({new_client_id(), std::nullopt}), 5U);
}

// SendspinServer::unpair(): a removed record frees its slot, which persistence_keys.h's all-zero
// blob marks, and a load takes that blob for a free slot without an error
TEST(ClientRecords, FreesTheSlotOfARemovedRecord) {
    const std::string a = new_client_id();
    const std::string b = new_client_id();
    ClientRecords records;
    records.add_loaded(0, {a, ApprovalScope::PLAYBACK});
    records.add_loaded(1, {b, std::nullopt});
    EXPECT_EQ(records.remove(a), 0U);
    EXPECT_EQ(records.find(a), nullptr);
    EXPECT_EQ(records.remove(a), std::nullopt);
    EXPECT_EQ(records.set({new_client_id(), std::nullopt}), 0U);
    // A record held unsaved has no slot to free
    ClientRecords full;
    full.add_loaded(persistence_keys::MAX_CLIENT_RECORDS - 1, {a, std::nullopt});
    ASSERT_EQ(full.set({b, std::nullopt}), std::nullopt);
    EXPECT_EQ(full.remove(b), std::nullopt);
    EXPECT_EQ(full.find(b), nullptr);

    sendspin::server::test::MemoryProvider provider;
    provider.blobs[persistence_keys::client_record_key(0)] =
        std::vector<uint8_t>(persistence_keys::CLIENT_RECORD_SIZE, 0);
    provider.blobs[persistence_keys::client_record_key(1)] = blob_of({b, std::nullopt});
    std::vector<sendspin::server::LogLevel> logged;
    ClientRecords loaded;
    {
        sendspin::server::Dispatcher dispatcher;
        const sendspin::server::Logger log(
            dispatcher,
            [&logged](sendspin::server::LogLevel level, std::string_view /*line*/) {
                logged.push_back(level);
            },
            sendspin::server::LogLevel::INFO);
        loaded = sendspin::server::load_client_records(provider, log);
    }
    EXPECT_TRUE(logged.empty());
    EXPECT_EQ(loaded.size(), 1U);
    EXPECT_EQ(loaded.set({new_client_id(), std::nullopt}), 0U);
}

}  // namespace
