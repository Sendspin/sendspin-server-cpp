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

/// @file test_server_pairing_wire.cpp
/// @brief Pairing on the wire against the raw test client, which plays the client's side with
/// the library's own CPace, code and wrapping functions: the exact messages of a Dynamic Pairing
/// Code attempt in both suites through its re-handshake, and how the server answers messages out
/// of order, malformed or left over, a save that fails and a trace that must not show a secret.

#include "client_records.h"
#include "crypto/base64url.h"
#include "crypto/cpace.h"
#include "crypto/pairing_code.h"
#include "crypto/pairing_token.h"
#include "crypto/psk_wrap.h"
#include "fragmentation.h"
#include "messages.h"
#include "raw_client.h"
#include "silence_feeder.h"
#include "test_recorder.h"
#include "trust_fixtures.h"
#include <ArduinoJson.h>
#include <gtest/gtest.h>

#include <sendspin/persistence_codec.h>
#include <sendspin/server/server.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace {

using sendspin::server::CipherSuite;
using sendspin::server::CodeFormat;
using sendspin::server::CPace;
using sendspin::server::CPaceRole;
using sendspin::server::HandshakeHash;
using sendspin::server::PairingOptions;
using sendspin::server::PairingResult;
using sendspin::server::PairMethod;
using sendspin::server::Psk;
using sendspin::server::SendspinServer;
using sendspin::server::ServerConfig;
using sendspin::server::StartResult;
using sendspin::server::TraceDirection;
using sendspin::server::TracedMessage;
using sendspin::server::test::Bytes;
using sendspin::server::test::bytes_of;
using sendspin::server::test::client_time_json;
using sendspin::server::test::MemoryProvider;
using sendspin::server::test::psk_of;
using sendspin::server::test::RawClient;
using sendspin::server::test::Recorder;
namespace persistence_keys = sendspin::server::persistence_keys;

/// A client/hello offering every method: the dynamic code on a display in both formats
std::string pairing_hello_json() {
    return R"({"type":"client/hello","payload":{"name":"Kitchen","supported_roles":["metadata@v1"],)"
           R"("supported_pair_methods":{"pairing_psk":{},"static_pairing_code":{},)"
           R"("dynamic_pairing_code":{"out_channels":["display"],"formats":["digits","qr_code"]}},)"
           R"("unpaired_access":{"enabled":true}}})";
}

template <size_t N>
std::string b64(const std::array<uint8_t, N>& bytes) {
    return sendspin::server::b64url_encode(bytes.data(), bytes.size());
}

/// The base64url field of a JSON message's payload, decoded into N bytes
template <size_t N>
std::array<uint8_t, N> field_of(const std::string& json, const char* field) {
    JsonDocument doc;
    EXPECT_EQ(deserializeJson(doc, json), DeserializationError::Ok) << json;
    const std::optional<std::vector<uint8_t>> bytes =
        sendspin::server::b64url_decode(doc["payload"][field].as<std::string>());
    std::array<uint8_t, N> out{};
    EXPECT_TRUE(bytes && bytes->size() == N) << json;
    if (bytes && bytes->size() == N) {
        std::copy(bytes->begin(), bytes->end(), out.begin());
    }
    return out;
}

void append_be32(std::vector<uint8_t>& out, uint32_t value) {
    for (int shift = 24; shift >= 0; shift -= 8) {
        out.push_back(static_cast<uint8_t>((value >> shift) & 0xFF));
    }
}

/// The traced plaintexts, as text
class Trace {
public:
    void add(const TracedMessage& message) {
        const std::lock_guard<std::mutex> lock(this->mutex_);
        this->texts_.emplace_back(message.plaintext.begin(), message.plaintext.end());
    }

    std::vector<std::string> texts() const {
        const std::lock_guard<std::mutex> lock(this->mutex_);
        return this->texts_;
    }

private:
    mutable std::mutex mutex_;
    std::vector<std::string> texts_;
};

/// A MemoryProvider whose saves can be held at the provider until released, so a test acts while
/// a save is in flight; its blobs are read under its lock
class GatedProvider : public MemoryProvider {
public:
    bool save_blob(const std::string& key, const uint8_t* data, size_t len) override {
        std::unique_lock<std::mutex> lock(this->mutex_);
        ++this->arrived_;
        this->changed_.notify_all();
        this->changed_.wait(lock, [this] { return !this->held_; });
        return MemoryProvider::save_blob(key, data, len);
    }

    /// Holds the saves that arrive from now on; returns the count arrived so far
    size_t hold() {
        const std::lock_guard<std::mutex> lock(this->mutex_);
        this->held_ = true;
        return this->arrived_;
    }

    /// Waits until count saves arrived, with no timeout
    void wait_arrived(size_t count) {
        std::unique_lock<std::mutex> lock(this->mutex_);
        this->changed_.wait(lock, [&] { return this->arrived_ >= count; });
    }

    void release() {
        const std::lock_guard<std::mutex> lock(this->mutex_);
        this->held_ = false;
        this->changed_.notify_all();
    }

    std::optional<Bytes> blob(const std::string& key) {
        const std::lock_guard<std::mutex> lock(this->mutex_);
        const auto it = this->blobs.find(key);
        return it == this->blobs.end() ? std::nullopt : std::optional(it->second);
    }

    std::optional<sendspin::server::ClientRecord> record(size_t slot) {
        const std::optional<Bytes> bytes = this->blob(persistence_keys::client_record_key(slot));
        return bytes ? sendspin::server::decode_client_record(*bytes) : std::nullopt;
    }

private:
    std::mutex mutex_;
    std::condition_variable changed_;
    size_t arrived_{0};
    bool held_{false};
};

/// A server with a recorder and a trace, which the raw client connects to
struct Server {
    /// prepare adjusts the config and seeds the provider before the start
    explicit Server(const std::function<void(ServerConfig&, GatedProvider&)>& prepare = {}) {
        ServerConfig config = sendspin::server::test::local_config();
        config.name = "Wire server";
        config.trace = [this](const TracedMessage& message) {
            this->trace.add(message);
            this->recorder.trace(message);
        };
        config.log = [this](sendspin::server::LogLevel /*level*/, std::string_view line) {
            const std::lock_guard<std::mutex> lock(this->log_mutex);
            this->log.emplace_back(line);
        };
        config.log_level = sendspin::server::LogLevel::INFO;
        const sendspin::server::Identity identity = *sendspin::server::Identity::generate();
        this->provider.blobs[persistence_keys::SERVER_KEY] =
            Bytes(identity.private_bytes.begin(), identity.private_bytes.end());
        if (prepare) {
            prepare(config, this->provider);
        }
        EXPECT_EQ(this->server.start(config, &this->provider, &this->recorder), StartResult::OK);
    }

    GatedProvider provider;
    Recorder recorder;
    Trace trace;
    std::mutex log_mutex;
    std::vector<std::string> log;
    SendspinServer server;
};

/// The client's side of one Dynamic Pairing Code attempt (pairing.md "Dynamic Pairing Code
/// Flow"), as CPace role B
class ClientSide {
public:
    ClientSide(RawClient& client, uint32_t index)
        : client_(client), index_(index), nonce_b_(sendspin::server::pairing_generate_nonce()) {}

    std::string init_json() const {
        return R"({"type":"client/pair-init","payload":{"pairing_index":)" +
               std::to_string(this->index_) + R"(,"commit_B":")" +
               b64(*sendspin::server::pairing_code_commit(this->nonce_b_.data(),
                                                          this->nonce_b_.size())) +
               R"("}})";
    }

    /// The code the client shows once server/pair-init gave nonce_A
    std::string code(const std::string& server_init) {
        this->nonce_a_ = field_of<sendspin::server::PAIRING_NONCE_SIZE>(server_init, "nonce_A");
        const HandshakeHash& h = this->client_.noise.handshake_hash();
        return sendspin::server::pairing_code_digits(*sendspin::server::pairing_code_digest(
            h.data(), h.size(), this->nonce_a_.data(), this->nonce_a_.size(), this->nonce_b_.data(),
            this->nonce_b_.size()));
    }

    /// The round's client/pair-auth, answering server/pair-auth under code
    std::string auth_json(const std::string& server_auth, const std::string& code, uint32_t round) {
        std::vector<uint8_t> sid = bytes_of("sendspin-pair-pake-v1");
        const HandshakeHash& h = this->client_.noise.handshake_hash();
        sid.insert(sid.end(), h.begin(), h.end());
        append_be32(sid, this->index_);
        append_be32(sid, round);
        this->cpace_ = std::make_unique<CPace>();
        EXPECT_TRUE(this->cpace_->start(CPaceRole::RESPONDER,
                                        sendspin::server::pairing_code_digits_prs(code), sid, {},
                                        bytes_of("client"), bytes_of("server")));
        const auto share = field_of<sendspin::server::CPACE_SHARE_SIZE>(server_auth, "pake_msg_1");
        EXPECT_TRUE(this->cpace_->derive(share.data(), share.size()));
        return R"({"type":"client/pair-auth","payload":{"pake_msg_2":")" +
               b64(this->cpace_->public_share()) + R"("}})";
    }

    /// Whether server/pair-confirm's server_kc verifies
    bool verify(const std::string& server_confirm) const {
        const auto tag = field_of<sendspin::server::CPACE_TAG_SIZE>(server_confirm, "server_kc");
        return this->cpace_->verify(tag.data(), tag.size());
    }

    /// client/pair-confirm, wrapping nonce_B, or nonce when given
    std::string confirm_json(
        const char* aead,
        const std::optional<std::array<uint8_t, sendspin::server::PAIRING_NONCE_SIZE>>& nonce =
            std::nullopt) const {
        const auto wrapped = sendspin::server::wrap_value(sendspin::server::NONCE_WRAP_LABEL, aead,
                                                          this->cpace_->sid(), *this->cpace_->isk(),
                                                          nonce.value_or(this->nonce_b_));
        return R"({"type":"client/pair-confirm","payload":{"client_kc":")" +
               b64(*this->cpace_->tag()) + R"(","wrapped_nonce_B":")" + b64(*wrapped) + R"("}})";
    }

    std::string finalize_json(const char* aead, const Psk& long_term) const {
        const auto wrapped =
            sendspin::server::wrap_value(sendspin::server::PSK_WRAP_LABEL, aead,
                                         this->cpace_->sid(), *this->cpace_->isk(), long_term);
        return R"({"type":"client/pair-finalize","payload":{"wrapped_psk":")" + b64(*wrapped) +
               R"("}})";
    }

private:
    RawClient& client_;
    uint32_t index_;
    std::array<uint8_t, sendspin::server::PAIRING_NONCE_SIZE> nonce_b_;
    std::array<uint8_t, sendspin::server::PAIRING_NONCE_SIZE> nonce_a_{};
    std::unique_ptr<CPace> cpace_;
};

std::string code_needed(const RawClient& client) {
    return Recorder::code_needed(client.noise.identity().peer_id(),
                                 PairMethod::DYNAMIC_PAIRING_CODE, CodeFormat::DIGITS);
}

/// Waits for client's pairing result, with no timeout, and checks it is the only one so far and
/// was delivered on the dispatch thread (the thread the provider was loaded on)
PairingResult await_result(Server& s, const RawClient& client) {
    const std::string prefix = "pairing result " + client.noise.identity().peer_id() + " ";
    s.recorder.wait_for_match([&](const std::string& what) { return what.starts_with(prefix); });
    std::vector<Recorder::Entry> results;
    for (const Recorder::Entry& entry : s.recorder.entries()) {
        if (entry.what.starts_with("pairing ") || entry.what.starts_with("code needed ")) {
            EXPECT_EQ(entry.thread, s.provider.thread) << entry.what;
        }
        if (entry.what.starts_with(prefix)) {
            results.push_back(entry);
        }
    }
    EXPECT_EQ(results.size(), 1U);
    return static_cast<PairingResult>(std::stoi(results.front().what.substr(prefix.size())));
}

/// Whether client's pairing reported a result
bool has_result(Server& s, const RawClient& client) {
    const std::string prefix = "pairing result " + client.noise.identity().peer_id() + " ";
    const std::vector<Recorder::Entry> entries = s.recorder.entries();
    return std::any_of(entries.begin(), entries.end(), [&](const Recorder::Entry& entry) {
        return entry.what.starts_with(prefix);
    });
}

/// A client record a test seeds into slot 0
void seed_record(GatedProvider& provider, const std::string& client_id,
                 std::optional<sendspin::server::ApprovalScope> approval,
                 std::optional<Psk> long_term) {
    sendspin::server::ClientRecord record{client_id, approval};
    record.long_term_psk = long_term;
    sendspin::server::ClientRecordBlob blob{};
    ASSERT_TRUE(sendspin::server::encode_client_record(record, blob));
    provider.blobs[persistence_keys::client_record_key(0)] = Bytes(blob.begin(), blob.end());
}

/// Connects client, activates it with the pairing hello and begins a Dynamic Pairing Code
/// pairing, through its pairing activation
void begin_dynamic(Server& s, RawClient& client) {
    ASSERT_TRUE(s.server.connect_url(client.url()));
    ASSERT_NO_FATAL_FAILURE(client.activate(pairing_hello_json()));
    ASSERT_TRUE(s.server.begin_pairing(
        client.noise.identity().peer_id(),
        PairingOptions{PairMethod::DYNAMIC_PAIRING_CODE, CodeFormat::DIGITS}));
    // messaging.md "server/activate": 'pairing' with its method and format
    EXPECT_EQ(client.receive_until("server/activate"),
              R"({"type":"server/activate","payload":{"activities":["pairing"],)"
              R"("pairing":{"method":"dynamic_pairing_code","format":"digits"}}})");
}

/// Whether the connection still answers: a time request gets its reply
void expect_open(RawClient& client) {
    client.send_json(client_time_json(1));
    EXPECT_FALSE(client.receive_until("server/time").empty());
}

/// The client's side of a re-handshake to psk: message 1, then message 2 under it
void rekey_to(RawClient& client, const Psk& psk) {
    client.noise.psk = psk;
    const std::string message_1 = client.receive_until("noise/handshake");
    ASSERT_TRUE(client.noise.read_rekey_message_1(message_1)) << message_1;
    client.send_binary(client.noise.rekey_message_2());
}

/// A Dynamic Pairing Code attempt from client/pair-init through client/pair-confirm, the code
/// entered right
void pair_through_confirm(Server& s, RawClient& client, ClientSide& side) {
    client.send_json(side.init_json());
    const std::string code = side.code(client.receive_until("server/pair-init"));
    s.recorder.wait_for(code_needed(client));
    ASSERT_TRUE(s.server.submit_pairing_code(client.noise.identity().peer_id(), code));
    client.send_json(side.auth_json(client.receive_until("server/pair-auth"), code, 1));
    ASSERT_TRUE(side.verify(client.receive_until("server/pair-confirm")));
    client.send_json(side.confirm_json(sendspin::server::AEAD_CHACHAPOLY));
}

/// The server dials client, which takes the connection with its client/state; the server reports
/// it connected
void connect(Server& s, RawClient& client) {
    ASSERT_TRUE(s.server.connect_url(client.url()));
    ASSERT_NO_FATAL_FAILURE(client.activate(pairing_hello_json()));
    client.send_json(sendspin::server::test::client_state_json(true));
    s.recorder.wait_for("connected " + client.noise.identity().peer_id());
}

/// The client leaves with `shutdown`, so the server dials it again only when asked
void leave(Server& s, RawClient& client, size_t disconnects) {
    client.send_json(sendspin::server::test::client_goodbye_json("shutdown"));
    client.read_until_closed();
    s.recorder.wait_for_disconnects(disconnects);
}

class PairingWire : public testing::TestWithParam<CipherSuite> {};

// pairing.md "Dynamic Pairing Code Flow" message by message, through a second round after a wrong
// code ("Rounds"), the persisted record before server/pair-finalize, and the re-handshake to the
// new long-term PSK with the activation it froze (connection.md "Re-handshake"); the wrapped values
// use the suite's AEAD ("Wrapping")
TEST_P(PairingWire, PairsWithADynamicCodeMessageByMessage) {
    Server s;
    RawClient client(GetParam());
    ASSERT_NO_FATAL_FAILURE(begin_dynamic(s, client));
    const char* aead = GetParam() == CipherSuite::AESGCM_SHA256 ? sendspin::server::AEAD_AESGCM
                                                                : sendspin::server::AEAD_CHACHAPOLY;
    ClientSide side(client, 1);
    client.send_json(side.init_json());
    const std::string server_init = client.receive_json();
    ASSERT_EQ(server_init.find(R"({"type":"server/pair-init","payload":{"nonce_A":")"), 0U)
        << server_init;
    const std::string code = side.code(server_init);
    s.recorder.wait_for(code_needed(client));

    // Round 1 under a wrong code: server_kc fails the client's check
    std::string wrong = code;
    wrong.back() = wrong.back() == '0' ? '1' : '0';
    ASSERT_TRUE(s.server.submit_pairing_code(client.noise.identity().peer_id(), wrong));
    std::string server_auth = client.receive_json();
    ASSERT_EQ(server_auth.find(R"({"type":"server/pair-auth","payload":{"pake_msg_1":")"), 0U)
        << server_auth;
    client.send_json(side.auth_json(server_auth, code, 1));
    std::string server_confirm = client.receive_json();
    ASSERT_EQ(server_confirm.find(R"({"type":"server/pair-confirm","payload":{"server_kc":")"), 0U)
        << server_confirm;
    EXPECT_FALSE(side.verify(server_confirm));
    client.send_json(R"({"type":"client/pair-retry","payload":{}})");
    // Round 2 carries no nonce_A
    EXPECT_EQ(client.receive_json(), R"({"type":"server/pair-init","payload":{}})");
    s.recorder.wait_for_count(code_needed(client), 2);
    ASSERT_TRUE(s.server.submit_pairing_code(client.noise.identity().peer_id(), code));
    server_auth = client.receive_json();
    client.send_json(side.auth_json(server_auth, code, 2));
    server_confirm = client.receive_json();
    ASSERT_TRUE(side.verify(server_confirm));
    client.send_json(side.confirm_json(aead));
    const Psk long_term = psk_of(0x70);
    client.send_json(side.finalize_json(aead, long_term));

    EXPECT_EQ(client.receive_json(), R"({"type":"server/pair-finalize","payload":{}})");
    // pairing.md "server/pair-finalize": the record was durable first
    const std::optional<sendspin::server::ClientRecord> record = s.provider.record(0);
    ASSERT_TRUE(record && record->long_term_psk);
    EXPECT_EQ(*record->long_term_psk, long_term);
    EXPECT_EQ(await_result(s, client), PairingResult::PAIRED);

    client.noise.psk = long_term;
    const std::string message_1 = client.receive_json();
    ASSERT_TRUE(client.noise.read_rekey_message_1(message_1)) << message_1;
    EXPECT_NE(client.noise.message_1_payload.find(R"("psk_category":"lt")"), std::string::npos);
    client.send_binary(client.noise.rekey_message_2());
    // The frozen activation, without 'pairing', first under the new keys; the paired client
    // has its roles
    EXPECT_EQ(client.receive_json(), R"({"type":"server/activate","payload":{"activities":[],)"
                                     R"("active_roles":["metadata@v1"]}})");
    ASSERT_NO_FATAL_FAILURE(expect_open(client));

    // Pairing it again runs under the Sentinel: the re-handshake to it freezes the pairing
    // activation, the first of the new handshake, so its pairing_index is 1 again (pairing.md
    // "Pairing index")
    ASSERT_TRUE(s.server.begin_pairing(
        client.noise.identity().peer_id(),
        PairingOptions{PairMethod::DYNAMIC_PAIRING_CODE, CodeFormat::DIGITS}));
    client.noise.psk = sendspin::server::SENTINEL_PSK;
    const std::string sentinel_1 = client.receive_until("noise/handshake");
    ASSERT_TRUE(client.noise.read_rekey_message_1(sentinel_1)) << sentinel_1;
    client.send_binary(client.noise.rekey_message_2());
    EXPECT_EQ(client.receive_json(),
              R"({"type":"server/activate","payload":{"activities":["pairing"],"active_roles":[],)"
              R"("pairing":{"method":"dynamic_pairing_code","format":"digits"}}})");
    ClientSide again(client, 1);
    client.send_json(again.init_json());
    EXPECT_EQ(client.receive_until("server/pair-init").find(R"({"type":"server/pair-init")"), 0U);
}

INSTANTIATE_TEST_SUITE_P(Suites, PairingWire,
                         testing::Values(CipherSuite::CHACHAPOLY_SHA256,
                                         CipherSuite::AESGCM_SHA256),
                         [](const testing::TestParamInfo<CipherSuite>& info) {
                             return info.param == CipherSuite::AESGCM_SHA256 ? "AesGcm"
                                                                             : "ChaChaPoly";
                         });

// pairing.md "Sequence violations" and "Pairing index": a message out of its step, one whose
// pairing_index is above the activations written, and one with a field of the wrong size or a
// missing one close the connection silently and end the attempt with PROTOCOL_ERROR; a
// pairing_index below the written one is a leftover, dropped, after which the attempt goes on
TEST(PairingWireViolations, ClosesOnAViolationAndDropsALeftover) {
    struct Row {
        const char* name;
        /// Runs the rows' step: from the pairing activation, sends what the row tests
        std::function<void(RawClient&, ClientSide&)> send;
        bool closes;
    };
    const std::string commit_33 = std::string(R"({"type":"client/pair-init","payload":{)") +
                                  R"("pairing_index":1,"commit_B":")" + std::string(44, 'A') +
                                  R"("}})";
    const Row rows[] = {
        {"Control: client/pair-init at index 1",
         [](RawClient& client, ClientSide& side) { client.send_json(side.init_json()); }, false},
        {"client/pair-init at index 2, above the activations written",
         [](RawClient& client, ClientSide& /*side*/) {
             ClientSide ahead(client, 2);
             client.send_json(ahead.init_json());
         },
         true},
        {"client/pair-init at index 0, a leftover",
         [](RawClient& client, ClientSide& side) {
             ClientSide stale(client, 0);
             client.send_json(stale.init_json());
             client.send_json(side.init_json());
         },
         false},
        {"client/pair-auth before client/pair-init",
         [](RawClient& client, ClientSide& /*side*/) {
             client.send_json(R"({"type":"client/pair-auth","payload":{"pake_msg_2":")" +
                              std::string(43, 'A') + R"("}})");
         },
         true},
        {"client/pair-init with a 33-byte commit_B",
         [commit_33](RawClient& client, ClientSide& /*side*/) { client.send_json(commit_33); },
         true},
        {"client/pair-init without pairing_index",
         [](RawClient& client, ClientSide& /*side*/) {
             client.send_json(R"({"type":"client/pair-init","payload":{}})");
         },
         true},
        {"client/pair-finalize before any round",
         [](RawClient& client, ClientSide& side) {
             client.send_json(side.init_json());
             client.receive_until("server/pair-init");
             client.send_json(R"({"type":"client/pair-finalize","payload":{"long_term_psk":")" +
                              b64(psk_of(1)) + R"("}})");
         },
         true},
    };
    for (const Row& row : rows) {
        SCOPED_TRACE(row.name);
        Server s;
        RawClient client;
        ASSERT_NO_FATAL_FAILURE(begin_dynamic(s, client));
        ClientSide side(client, 1);
        row.send(client, side);
        if (row.closes) {
            client.read_until_closed();
            EXPECT_EQ(await_result(s, client), PairingResult::PROTOCOL_ERROR);
        } else {
            EXPECT_EQ(client.receive_json().find(R"({"type":"server/pair-init")"), 0U);
            ASSERT_NO_FATAL_FAILURE(expect_open(client));
            EXPECT_FALSE(has_result(s, client));
        }
    }
}

// pairing.md "Sequence violations" and "Server verification": a client/pair-auth replayed once
// server/pair-confirm went out, a client/pair-confirm without wrapped_nonce_B in the Dynamic
// Pairing Code Flow, and one whose nonce_B does not open commit_B close the connection silently
TEST(PairingWireViolations, ClosesOnAReplayedAuthAndAConfirmThatDoesNotBind) {
    enum class Breach : uint8_t { REPLAYED_AUTH, NO_NONCE, ANOTHER_NONCE };
    for (const Breach breach : {Breach::REPLAYED_AUTH, Breach::NO_NONCE, Breach::ANOTHER_NONCE}) {
        SCOPED_TRACE(static_cast<int>(breach));
        Server s;
        RawClient client;
        ASSERT_NO_FATAL_FAILURE(begin_dynamic(s, client));
        ClientSide side(client, 1);
        client.send_json(side.init_json());
        const std::string code = side.code(client.receive_json());
        s.recorder.wait_for(code_needed(client));
        ASSERT_TRUE(s.server.submit_pairing_code(client.noise.identity().peer_id(), code));
        const std::string auth = side.auth_json(client.receive_json(), code, 1);
        client.send_json(auth);
        ASSERT_TRUE(side.verify(client.receive_json()));
        if (breach == Breach::REPLAYED_AUTH) {
            client.send_json(auth);
        } else if (breach == Breach::ANOTHER_NONCE) {
            client.send_json(side.confirm_json(sendspin::server::AEAD_CHACHAPOLY,
                                               sendspin::server::pairing_generate_nonce()));
        } else {
            const std::string confirm = side.confirm_json(sendspin::server::AEAD_CHACHAPOLY);
            JsonDocument doc;
            ASSERT_EQ(deserializeJson(doc, confirm), DeserializationError::Ok);
            doc["payload"].remove("wrapped_nonce_B");
            std::string without;
            serializeJson(doc, without);
            client.send_json(without);
        }
        client.read_until_closed();
        EXPECT_EQ(await_result(s, client), PairingResult::PROTOCOL_ERROR);
        EXPECT_FALSE(s.provider.blob(persistence_keys::client_record_key(0)));
    }
}

// pairing.md "Entering and leaving pairing": the operator's cancel sends pair/abort
// `user_cancelled` and an activation without 'pairing'; the cancelled attempt's messages are
// leftovers then, dropped, and so is client/pair-init of a superseded activation once the next
// pairing activation is written
TEST(PairingWireLeftovers, DropsMessagesOfACancelledAttempt) {
    Server s;
    RawClient client;
    ASSERT_NO_FATAL_FAILURE(begin_dynamic(s, client));
    ClientSide side(client, 1);
    client.send_json(side.init_json());
    client.receive_until("server/pair-init");
    ASSERT_TRUE(s.server.cancel_pairing(client.noise.identity().peer_id()));
    EXPECT_EQ(client.receive_json(),
              R"({"type":"pair/abort","payload":{"reason":"user_cancelled"}})");
    EXPECT_EQ(client.receive_json(), R"({"type":"server/activate","payload":{"activities":[]}})");
    EXPECT_EQ(await_result(s, client), PairingResult::CANCELLED);
    client.send_json(R"({"type":"client/pair-auth","payload":{"pake_msg_2":")" +
                     std::string(43, 'A') + R"("}})");
    client.send_json(R"({"type":"client/pair-retry","payload":{}})");
    ASSERT_NO_FATAL_FAILURE(expect_open(client));

    ASSERT_TRUE(s.server.begin_pairing(
        client.noise.identity().peer_id(),
        PairingOptions{PairMethod::DYNAMIC_PAIRING_CODE, CodeFormat::DIGITS}));
    client.receive_until("server/activate");
    // The first activation's client/pair-init, sent before the second arrived
    client.send_json(side.init_json());
    ASSERT_NO_FATAL_FAILURE(expect_open(client));
    ClientSide second(client, 2);
    client.send_json(second.init_json());
    EXPECT_EQ(client.receive_json().find(R"({"type":"server/pair-init")"), 0U);
}

/// The Pairing PSK Flow's client/pair-init for the first activation of a handshake
const char* const PSK_INIT = R"({"type":"client/pair-init","payload":{"pairing_index":1}})";

/// The Pairing PSK Flow's client/pair-finalize, carrying long_term as it is
std::string psk_finalize_json(const Psk& long_term) {
    return R"({"type":"client/pair-finalize","payload":{"long_term_psk":")" + b64(long_term) +
           R"("}})";
}

/// The pairing token of client with pairing_psk
std::string token_of(const RawClient& client, const Psk& pairing_psk) {
    return sendspin::server::format_pairing_token(client.noise.identity().public_bytes,
                                                  pairing_psk);
}

/// The client's side of a Pairing PSK attempt from its pairing activation, the re-handshake to the
/// pairing PSK done: client/pair-init and client/pair-finalize with long_term, which the server
/// saves in slot 0 and reports PAIRED, once
void pair_with_pairing_psk(Server& s, RawClient& client, const Psk& long_term) {
    const std::string activation = client.receive_until("server/activate");
    EXPECT_NE(activation.find(R"("method":"pairing_psk")"), std::string::npos) << activation;
    client.send_json(PSK_INIT);
    client.send_json(psk_finalize_json(long_term));
    EXPECT_EQ(client.receive_until("server/pair-finalize"),
              R"({"type":"server/pair-finalize","payload":{}})");
    const std::optional<sendspin::server::ClientRecord> record = s.provider.record(0);
    ASSERT_TRUE(record && record->long_term_psk);
    EXPECT_EQ(*record->long_term_psk, long_term);
    const std::string paired =
        Recorder::pairing_result(client.noise.identity().peer_id(), PairingResult::PAIRED);
    s.recorder.wait_for(paired);
    EXPECT_EQ(s.recorder.count(paired), 1U);
}

// pairing.md "Pairing index", "Entering and leaving pairing": the count restarts at every
// handshake, so a pairing activation written before a re-handshake counts index 1 under the old
// keys too. A Pairing PSK attempt begun after a cancelled code attempt starts behind a
// re-handshake to the pairing PSK. Sent under the Sentinel's keys before message 2, the cancelled
// attempt's client/pair-init, with its commit_B, and a client/pair-finalize are dropped: they are
// leftovers, and prove no pairing PSK. Control: the same messages after the re-handshake pair.
TEST(PairingWireLeftovers, DropsAttemptMessagesSentBeforeItsReHandshake) {
    Server s;
    RawClient client;
    ASSERT_NO_FATAL_FAILURE(begin_dynamic(s, client));
    const std::string id = client.noise.identity().peer_id();
    ASSERT_TRUE(s.server.cancel_pairing(id));
    EXPECT_EQ(client.receive_until("server/activate"),
              R"({"type":"server/activate","payload":{"activities":[]}})");
    s.recorder.wait_for(Recorder::pairing_result(id, PairingResult::CANCELLED));

    const Psk pairing_psk = psk_of(0x40);
    ASSERT_TRUE(s.server.submit_pairing_token(id, token_of(client, pairing_psk)));
    const std::string message_1 = client.receive_until("noise/handshake");
    const ClientSide cancelled(client, 1);
    client.send_json(cancelled.init_json());
    client.send_json(PSK_INIT);
    client.send_json(psk_finalize_json(psk_of(0x60)));
    client.noise.psk = pairing_psk;
    ASSERT_TRUE(client.noise.read_rekey_message_1(message_1)) << message_1;
    client.send_binary(client.noise.rekey_message_2());
    ASSERT_NO_FATAL_FAILURE(pair_with_pairing_psk(s, client, psk_of(0x70)));
}

// pairing.md "Pairing index": a paired client's Pairing PSK attempt that a second token replaces
// leaves two re-handshakes queued, back to the long-term PSK and then to the pairing PSK. Under
// the pairing PSK the first attempt's activation counted index 1, so its client/pair-init, a
// client/pair-finalize and its pair/abort sent before those re-handshakes fit the new attempt's
// step and PSK; they are dropped as sent before its activation. Control: the init and finalize
// after them pair.
TEST(PairingWireLeftovers, DropsAttemptMessagesSentBeforeTheReHandshakesAReplacementQueued) {
    RawClient client;
    const std::string id = client.noise.identity().peer_id();
    const Psk long_term = psk_of(0x30);
    Server s([&](ServerConfig& /*config*/, GatedProvider& provider) {
        seed_record(provider, id, sendspin::server::ApprovalScope::PLAYBACK, long_term);
    });
    client.noise.psk = long_term;
    ASSERT_NO_FATAL_FAILURE(connect(s, client));
    const Psk pairing_psk = psk_of(0x40);
    ASSERT_TRUE(s.server.submit_pairing_token(id, token_of(client, pairing_psk)));
    ASSERT_NO_FATAL_FAILURE(rekey_to(client, pairing_psk));
    const std::string activation = client.receive_until("server/activate");
    EXPECT_NE(activation.find(R"("method":"pairing_psk")"), std::string::npos) << activation;

    // The second token cancels the first attempt and begins the next in one step
    ASSERT_TRUE(s.server.submit_pairing_token(id, token_of(client, pairing_psk)));
    EXPECT_EQ(client.receive_until("pair/abort"),
              R"({"type":"pair/abort","payload":{"reason":"user_cancelled"}})");
    client.send_json(PSK_INIT);
    client.send_json(psk_finalize_json(psk_of(0x60)));
    client.send_json(R"({"type":"pair/abort","payload":{"reason":"user_cancelled"}})");
    ASSERT_NO_FATAL_FAILURE(rekey_to(client, long_term));
    EXPECT_EQ(client.receive_until("server/activate").find(R"("activities":["pairing"])"),
              std::string::npos);
    ASSERT_NO_FATAL_FAILURE(rekey_to(client, pairing_psk));
    ASSERT_NO_FATAL_FAILURE(pair_with_pairing_psk(s, client, psk_of(0x70)));
}

// pairing.md "server/pair-finalize": sent only once the record is persisted. A save that fails
// ends the attempt with an activation without 'pairing' in its place, nothing claims the client
// is paired, and a client that had no record has none after: under approve_all_clients it stays
// approved, rather than held by a record without an approval
TEST(PairingWirePersistence, SendsNoFinalizeWhenTheSaveFails) {
    for (const bool approve_all : {false, true}) {
        SCOPED_TRACE(approve_all ? "approve_all_clients" : "Control: no approval");
        Server s([&](ServerConfig& config, GatedProvider& provider) {
            config.approve_all_clients = approve_all;
            provider.refuse_save = true;
        });
        RawClient client;
        ASSERT_NO_FATAL_FAILURE(connect(s, client));
        ASSERT_TRUE(s.server.begin_pairing(
            client.noise.identity().peer_id(),
            PairingOptions{PairMethod::DYNAMIC_PAIRING_CODE, CodeFormat::DIGITS}));
        const std::string pairing = client.receive_until("server/activate");
        ASSERT_NE(pairing.find("pairing"), std::string::npos) << pairing;
        ClientSide side(client, 1);
        ASSERT_NO_FATAL_FAILURE(pair_through_confirm(s, client, side));
        client.send_json(side.finalize_json(sendspin::server::AEAD_CHACHAPOLY, psk_of(0x70)));
        const std::string ending = client.receive_json();
        EXPECT_NE(ending.find(R"("type":"server/activate")"), std::string::npos) << ending;
        EXPECT_EQ(ending.find("pairing"), std::string::npos) << ending;
        EXPECT_EQ(await_result(s, client), PairingResult::PERSISTENCE_FAILED);
        ASSERT_NO_FATAL_FAILURE(expect_open(client));
        const std::vector<sendspin::server::ClientInfo> clients = s.server.clients();
        ASSERT_EQ(clients.size(), 1U);
        EXPECT_FALSE(clients[0].paired);
        EXPECT_FALSE(clients[0].pairing);
        EXPECT_EQ(
            clients[0].approval,
            approve_all ? std::optional(sendspin::server::ApprovalScope::PLAYBACK) : std::nullopt);
    }
}

// SendspinServer::approve() while the pairing's long-term PSK is being saved stays when a
// pair/abort then puts the pairing back: only the PSK goes
TEST(PairingWirePersistence, KeepsAnApprovalGivenDuringTheSave) {
    Server s;
    RawClient client;
    const std::string id = client.noise.identity().peer_id();
    ASSERT_NO_FATAL_FAILURE(begin_dynamic(s, client));
    ClientSide side(client, 1);
    ASSERT_NO_FATAL_FAILURE(pair_through_confirm(s, client, side));
    const size_t arrived = s.provider.hold();
    client.send_json(side.finalize_json(sendspin::server::AEAD_CHACHAPOLY, psk_of(0x70)));
    s.provider.wait_arrived(arrived + 1);
    ASSERT_TRUE(s.server.approve(id, sendspin::server::ApprovalScope::PLAYBACK));
    // The dispatch thread is held in the save, so core's snapshot shows the approval
    while (s.server.clients().empty() || !s.server.clients()[0].approval) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    client.send_json(R"({"type":"pair/abort","payload":{"reason":"user_cancelled"}})");
    ASSERT_NO_FATAL_FAILURE(expect_open(client));
    s.provider.release();
    EXPECT_EQ(await_result(s, client), PairingResult::CLIENT_CANCELLED);
    const std::optional<sendspin::server::ClientRecord> record = s.provider.record(0);
    ASSERT_TRUE(record);
    EXPECT_FALSE(record->long_term_psk);
    EXPECT_EQ(record->approval, sendspin::server::ApprovalScope::PLAYBACK);
    const std::vector<sendspin::server::ClientInfo> clients = s.server.clients();
    ASSERT_EQ(clients.size(), 1U);
    EXPECT_FALSE(clients[0].paired);
    EXPECT_EQ(clients[0].approval, sendspin::server::ApprovalScope::PLAYBACK);
}

// Trace redaction (pairing.md "Wrapping", "client/pair-finalize"): the trace shows the sealed
// values and a plain long-term PSK as "<redacted>", whether client/pair-finalize arrived whole or
// in fragments, and no trace entry or log line holds the PSK or the code
TEST(PairingWireTrace, RedactsSecretsFromTheTrace) {
    for (const bool fragmented : {false, true}) {
        SCOPED_TRACE(fragmented ? "fragmented" : "whole");
        Server s;
        RawClient client;
        ASSERT_NO_FATAL_FAILURE(begin_dynamic(s, client));
        ClientSide side(client, 1);
        client.send_json(side.init_json());
        const std::string code = side.code(client.receive_json());
        s.recorder.wait_for(code_needed(client));
        ASSERT_TRUE(s.server.submit_pairing_code(client.noise.identity().peer_id(), code));
        client.send_json(side.auth_json(client.receive_json(), code, 1));
        ASSERT_TRUE(side.verify(client.receive_json()));
        client.send_json(side.confirm_json(sendspin::server::AEAD_CHACHAPOLY));
        const Psk long_term = psk_of(0x70);
        const std::string finalize =
            side.finalize_json(sendspin::server::AEAD_CHACHAPOLY, long_term);
        if (fragmented) {
            // messaging.md "Fragmentation": two fragments of one message
            Bytes message{sendspin::server::MESSAGE_ID_JSON};
            message.insert(message.end(), finalize.begin(), finalize.end());
            const auto half = static_cast<std::ptrdiff_t>(message.size() / 2);
            client.send_fragment(sendspin::server::FRAGMENT_FLAG_FIRST,
                                 Bytes(message.begin(), message.begin() + half));
            client.send_fragment(sendspin::server::FRAGMENT_FLAG_LAST,
                                 Bytes(message.begin() + half, message.end()));
        } else {
            client.send_json(finalize);
        }
        EXPECT_EQ(client.receive_json(), R"({"type":"server/pair-finalize","payload":{}})");
        EXPECT_EQ(await_result(s, client), PairingResult::PAIRED);

        const std::vector<std::string> texts = s.trace.texts();
        const auto traced = std::find_if(texts.begin(), texts.end(), [](const std::string& text) {
            return text.find("client/pair-finalize") != std::string::npos;
        });
        ASSERT_NE(traced, texts.end());
        EXPECT_NE(traced->find(R"("wrapped_psk":"<redacted>")"), std::string::npos) << *traced;
        const auto confirm = std::find_if(texts.begin(), texts.end(), [](const std::string& text) {
            return text.find("client/pair-confirm") != std::string::npos;
        });
        ASSERT_NE(confirm, texts.end());
        EXPECT_NE(confirm->find(R"("wrapped_nonce_B":"<redacted>")"), std::string::npos)
            << *confirm;
        JsonDocument finalize_doc;
        ASSERT_EQ(deserializeJson(finalize_doc, finalize), DeserializationError::Ok);
        const std::string wrapped = finalize_doc["payload"]["wrapped_psk"].as<std::string>();
        std::vector<std::string> all = texts;
        {
            const std::lock_guard<std::mutex> lock(s.log_mutex);
            all.insert(all.end(), s.log.begin(), s.log.end());
        }
        const std::string psk_text = b64(long_term);
        for (const std::string& text : all) {
            EXPECT_EQ(text.find(psk_text), std::string::npos) << text;
            EXPECT_EQ(text.find(wrapped), std::string::npos) << text;
            EXPECT_EQ(text.find(code), std::string::npos) << text;
        }
    }
}

// pairing.md "Entering and leaving pairing": an activation in place of server/pair-finalize
// "persists nothing and discards any received PSK", and an approval is discarded only on a
// successful pairing. A pair/abort while the PSK is being saved puts back the record the paired
// client had, its approval with it, and the connection goes back to that long-term PSK.
TEST(PairingWirePersistence, PutsBackTheRecordWhenAnAbortArrivesDuringTheSave) {
    RawClient client;
    const std::string id = client.noise.identity().peer_id();
    const Psk old_psk = psk_of(0x30);
    Server s([&](ServerConfig& /*config*/, GatedProvider& provider) {
        seed_record(provider, id, sendspin::server::ApprovalScope::PLAYBACK, old_psk);
    });
    client.noise.psk = old_psk;
    ASSERT_NO_FATAL_FAILURE(connect(s, client));
    ASSERT_TRUE(s.server.begin_pairing(
        id, PairingOptions{PairMethod::DYNAMIC_PAIRING_CODE, CodeFormat::DIGITS}));
    ASSERT_NO_FATAL_FAILURE(rekey_to(client, sendspin::server::SENTINEL_PSK));
    const std::string pairing = client.receive_until("server/activate");
    EXPECT_NE(pairing.find(R"("activities":["pairing"])"), std::string::npos) << pairing;
    ClientSide side(client, 1);
    ASSERT_NO_FATAL_FAILURE(pair_through_confirm(s, client, side));

    const size_t arrived = s.provider.hold();
    client.send_json(side.finalize_json(sendspin::server::AEAD_CHACHAPOLY, psk_of(0x70)));
    s.provider.wait_arrived(arrived + 1);
    client.send_json(R"({"type":"pair/abort","payload":{"reason":"user_cancelled"}})");
    // Taken in after the abort, so core has the abort once the reply comes
    ASSERT_NO_FATAL_FAILURE(expect_open(client));
    s.provider.release();

    ASSERT_NO_FATAL_FAILURE(rekey_to(client, old_psk));
    EXPECT_NE(client.noise.message_1_payload.find(R"("psk_category":"lt")"), std::string::npos);
    const std::string activation = client.receive_until("server/activate");
    EXPECT_EQ(activation.find("pairing"), std::string::npos) << activation;
    EXPECT_EQ(await_result(s, client), PairingResult::CLIENT_CANCELLED);
    const std::optional<sendspin::server::ClientRecord> record = s.provider.record(0);
    ASSERT_TRUE(record && record->long_term_psk);
    EXPECT_EQ(*record->long_term_psk, old_psk);
    EXPECT_EQ(record->approval, sendspin::server::ApprovalScope::PLAYBACK);
    const std::vector<sendspin::server::ClientInfo> clients = s.server.clients();
    ASSERT_EQ(clients.size(), 1U);
    EXPECT_TRUE(clients[0].paired);
    EXPECT_EQ(clients[0].approval, sendspin::server::ApprovalScope::PLAYBACK);
    ASSERT_NO_FATAL_FAILURE(expect_open(client));
}

// The same when the connection goes during the save: the client never had server/pair-finalize,
// so the record the save created goes again (persistence_keys.h: an all-zero blob)
TEST(PairingWirePersistence, PutsBackNothingWhenTheConnectionGoesDuringTheSave) {
    Server s;
    RawClient client;
    ASSERT_NO_FATAL_FAILURE(begin_dynamic(s, client));
    ClientSide side(client, 1);
    ASSERT_NO_FATAL_FAILURE(pair_through_confirm(s, client, side));
    const size_t arrived = s.provider.hold();
    client.send_json(side.finalize_json(sendspin::server::AEAD_CHACHAPOLY, psk_of(0x70)));
    s.provider.wait_arrived(arrived + 1);
    client.drop();
    // The dispatch thread is held in the save, so core's snapshot shows the close
    while (s.server.clients().empty() || s.server.clients()[0].connected) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    s.provider.release();
    EXPECT_EQ(await_result(s, client), PairingResult::CONNECTION_LOST);
    const std::optional<Bytes> blob = s.provider.blob(persistence_keys::client_record_key(0));
    ASSERT_TRUE(blob);
    EXPECT_TRUE(std::all_of(blob->begin(), blob->end(), [](uint8_t byte) { return byte == 0; }));
    const std::vector<sendspin::server::ClientInfo> clients = s.server.clients();
    ASSERT_EQ(clients.size(), 1U);
    EXPECT_FALSE(clients[0].paired);
}

// How an attempt that does not complete ends (types.h PairingResult): the server's wait for
// client/pair-init (pairing.md "Entering and leaving pairing", "Servers SHOULD apply their own
// timeout"), the server stopping and the connection going, each reported once
TEST(PairingWireEnds, ReportsHowAnAttemptEnded) {
    struct Row {
        const char* name;
        bool short_start_timeout;
        std::function<void(Server&, RawClient&)> act;
        PairingResult result;
    };
    const auto start = [](RawClient& client) {
        ClientSide side(client, 1);
        client.send_json(side.init_json());
        client.receive_until("server/pair-init");
    };
    const Row rows[] = {
        {"Control: the operator cancels", false,
         [&](Server& s, RawClient& client) {
             start(client);
             ASSERT_TRUE(s.server.cancel_pairing(client.noise.identity().peer_id()));
         },
         PairingResult::CANCELLED},
        {"the client never sends client/pair-init", true,
         [](Server& /*s*/, RawClient& client) {
             // The activation that ends the pairing
             const std::string activation = client.receive_until("server/activate");
             EXPECT_EQ(activation.find("pairing"), std::string::npos) << activation;
         },
         PairingResult::TIMED_OUT},
        {"the server stops", false,
         [&](Server& s, RawClient& client) {
             start(client);
             s.server.stop();
         },
         PairingResult::SERVER_STOPPED},
        {"the connection goes", false,
         [&](Server& /*s*/, RawClient& client) {
             start(client);
             client.drop();
         },
         PairingResult::CONNECTION_LOST},
    };
    for (const Row& row : rows) {
        SCOPED_TRACE(row.name);
        Server s([&](ServerConfig& config, GatedProvider& /*provider*/) {
            if (row.short_start_timeout) {
                config.timing.pairing_start_timeout_us = 200000;
            }
        });
        RawClient client;
        ASSERT_NO_FATAL_FAILURE(begin_dynamic(s, client));
        ASSERT_NO_FATAL_FAILURE(row.act(s, client));
        EXPECT_EQ(await_result(s, client), row.result);
    }
}

// A client not connected is dialed for pairing, its first activation the pairing one. A paired
// client dialed so, under the Sentinel, goes back to its long-term PSK when the pairing is
// cancelled.
TEST(PairingWireDial, PairsAClientNotConnectedOnADialForPairing) {
    for (const bool paired : {false, true}) {
        SCOPED_TRACE(paired ? "a paired client" : "an unpaired client");
        RawClient client;
        const std::string id = client.noise.identity().peer_id();
        const Psk old_psk = psk_of(0x30);
        Server s([&](ServerConfig& /*config*/, GatedProvider& provider) {
            if (paired) {
                seed_record(provider, id, std::nullopt, old_psk);
            }
        });
        client.noise.psk = paired ? old_psk : sendspin::server::SENTINEL_PSK;
        ASSERT_NO_FATAL_FAILURE(connect(s, client));
        ASSERT_NO_FATAL_FAILURE(leave(s, client, 1));

        ASSERT_TRUE(s.server.begin_pairing(
            id, PairingOptions{PairMethod::DYNAMIC_PAIRING_CODE, CodeFormat::DIGITS}));
        client.noise.psk = sendspin::server::SENTINEL_PSK;
        client.accept();
        ASSERT_NO_FATAL_FAILURE(client.complete_handshake());
        client.send_json(pairing_hello_json());
        const std::string first = client.receive_until("server/activate");
        EXPECT_NE(first.find(R"("activities":["pairing"])"), std::string::npos) << first;
        EXPECT_NE(first.find(R"("pairing":{"method":"dynamic_pairing_code","format":"digits"})"),
                  std::string::npos)
            << first;
        ClientSide side(client, 1);
        client.send_json(side.init_json());
        EXPECT_EQ(client.receive_until("server/pair-init").find(R"({"type":"server/pair-init")"),
                  0U);
        ASSERT_TRUE(s.server.cancel_pairing(id));
        EXPECT_EQ(client.receive_until("pair/abort"),
                  R"({"type":"pair/abort","payload":{"reason":"user_cancelled"}})");
        if (paired) {
            // The re-handshake comes next, ahead of the activation it freezes
            const std::string message_1 = client.receive_json();
            ASSERT_NE(message_1.find(R"("type":"noise/handshake")"), std::string::npos)
                << message_1;
            client.noise.psk = old_psk;
            ASSERT_TRUE(client.noise.read_rekey_message_1(message_1)) << message_1;
            client.send_binary(client.noise.rekey_message_2());
            EXPECT_NE(client.noise.message_1_payload.find(R"("psk_category":"lt")"),
                      std::string::npos);
        }
        const std::string activation = client.receive_until("server/activate");
        EXPECT_EQ(activation.find("pairing"), std::string::npos) << activation;
        EXPECT_EQ(await_result(s, client), PairingResult::CANCELLED);
        ASSERT_NO_FATAL_FAILURE(expect_open(client));
    }
}

// A dial for pairing that never completes (the client takes the TCP connection and never answers
// the upgrade) does not hold the pairing past ServerTiming::pairing_start_timeout_us
TEST(PairingWireDial, EndsADialForPairingThatNeverCompletes) {
    Server s([](ServerConfig& config, GatedProvider& /*provider*/) {
        config.timing.pairing_start_timeout_us = 300000;
    });
    RawClient client;
    ASSERT_TRUE(s.server.connect_url(client.url()));
    ASSERT_NO_FATAL_FAILURE(client.activate(pairing_hello_json()));
    ASSERT_NO_FATAL_FAILURE(leave(s, client, 1));
    ASSERT_TRUE(s.server.begin_pairing(
        client.noise.identity().peer_id(),
        PairingOptions{PairMethod::DYNAMIC_PAIRING_CODE, CodeFormat::DIGITS}));
    EXPECT_EQ(await_result(s, client), PairingResult::TIMED_OUT);
}

// cancel_connect_url() while a dial for pairing is in progress abandons the dial, and the pairing,
// with no connection to come, ends at once as CONNECTION_LOST rather than at its start timeout
TEST(PairingWireDial, EndsAPairingWhoseDialTheHostCancels) {
    Server s;
    RawClient client;
    ASSERT_TRUE(s.server.connect_url(client.url()));
    ASSERT_NO_FATAL_FAILURE(client.activate(pairing_hello_json()));
    ASSERT_NO_FATAL_FAILURE(leave(s, client, 1));
    ASSERT_TRUE(s.server.begin_pairing(
        client.noise.identity().peer_id(),
        PairingOptions{PairMethod::DYNAMIC_PAIRING_CODE, CodeFormat::DIGITS}));
    ASSERT_TRUE(s.server.cancel_connect_url(client.url()));
    EXPECT_EQ(await_result(s, client), PairingResult::CONNECTION_LOST);
}

// cancel_connect_url() once the dial for pairing is in its handshake closes that connection, which
// the client never took: the pairing ends as CONNECTION_LOST, and the host's own close is no
// on_error()
TEST(PairingWireDial, EndsAPairingWhoseDialTheHostCancelsInItsHandshake) {
    Server s;
    RawClient client;
    ASSERT_TRUE(s.server.connect_url(client.url()));
    ASSERT_NO_FATAL_FAILURE(client.activate(pairing_hello_json()));
    ASSERT_NO_FATAL_FAILURE(leave(s, client, 1));
    ASSERT_TRUE(s.server.begin_pairing(
        client.noise.identity().peer_id(),
        PairingOptions{PairMethod::DYNAMIC_PAIRING_CODE, CodeFormat::DIGITS}));
    client.accept();
    client.send_client_init();
    client.receive_message_1();
    ASSERT_TRUE(s.server.cancel_connect_url(client.url()));
    client.read_until_closed();
    EXPECT_EQ(await_result(s, client), PairingResult::CONNECTION_LOST);
    // The stop delivers every event posted before it
    s.server.stop();
    EXPECT_EQ(s.recorder.count_errors(sendspin::server::ErrorKind::HANDSHAKE_FAILED), 0U);
}

// A dial for a pairing that ended before the dial completed is an idle dial: its first activation
// declares no activities, and the later activation rule then declares `playback` for the group that
// plays
TEST(PairingWireDial, TreatsADialWhosePairingEndedAsAnIdleOne) {
    Server s([](ServerConfig& config, GatedProvider& /*provider*/) {
        config.approve_all_clients = true;
        config.timing.pairing_start_timeout_us = 300000;
    });
    RawClient client;
    ASSERT_NO_FATAL_FAILURE(connect(s, client));
    ASSERT_NO_FATAL_FAILURE(leave(s, client, 1));
    const sendspin::server::OpenedInput opened = s.server.open_input(
        *s.server.default_group(), sendspin::server::InputOptions{sendspin::server::InputFormat{
                                       48000, 2, sendspin::server::SampleFormat::S16}});
    ASSERT_TRUE(opened.input);
    const sendspin::server::test::SilenceFeeder feeder(opened.input);
    ASSERT_TRUE(s.server.begin_pairing(
        client.noise.identity().peer_id(),
        PairingOptions{PairMethod::DYNAMIC_PAIRING_CODE, CodeFormat::DIGITS}));
    EXPECT_EQ(await_result(s, client), PairingResult::TIMED_OUT);

    client.accept();
    ASSERT_NO_FATAL_FAILURE(client.complete_handshake());
    client.send_json(pairing_hello_json());
    const std::string first = client.receive_json();
    EXPECT_NE(first.find(R"("activities":[])"), std::string::npos) << first;
    EXPECT_EQ(first.find("pairing"), std::string::npos) << first;
    EXPECT_EQ(client.receive_json(),
              R"({"type":"server/activate","payload":{"activities":["playback"]}})");
}

// pairing.md "pair/abort" `concurrent_attempt`: a client that refuses a connection for pairing
// alone is dialed again after the reconnect wait, and pairs then; the refusal ends nothing
TEST(PairingWireDial, DialsAgainAfterAConcurrentAttempt) {
    Server s([](ServerConfig& config, GatedProvider& /*provider*/) {
        config.timing.handshake_backoff_min_us = 50000;
    });
    RawClient client;
    const std::string id = client.noise.identity().peer_id();
    ASSERT_TRUE(s.server.connect_url(client.url()));
    ASSERT_NO_FATAL_FAILURE(client.activate(pairing_hello_json()));
    ASSERT_NO_FATAL_FAILURE(leave(s, client, 1));
    ASSERT_TRUE(s.server.begin_pairing(
        id, PairingOptions{PairMethod::DYNAMIC_PAIRING_CODE, CodeFormat::DIGITS}));
    client.accept();
    ASSERT_NO_FATAL_FAILURE(client.complete_handshake());
    client.send_json(pairing_hello_json());
    client.receive_until("server/activate");
    client.send_json(R"({"type":"pair/abort","payload":{"reason":"concurrent_attempt"}})");
    client.drop();

    client.accept();
    ASSERT_NO_FATAL_FAILURE(client.complete_handshake());
    client.send_json(pairing_hello_json());
    const std::string again = client.receive_until("server/activate");
    EXPECT_NE(again.find(R"("activities":["pairing"])"), std::string::npos) << again;
    EXPECT_FALSE(has_result(s, client));
    ClientSide side(client, 1);
    client.send_json(side.init_json());
    EXPECT_EQ(client.receive_until("server/pair-init").find(R"({"type":"server/pair-init")"), 0U);
    ASSERT_TRUE(s.server.cancel_pairing(id));
    EXPECT_EQ(await_result(s, client), PairingResult::CANCELLED);
}

// pairing.md "Protocol Errors" end the attempt on its own connection only: another connection of
// the client that breaks the pairing protocol is closed, and the attempt goes on
TEST(PairingWireViolations, LeavesTheAttemptWhenAnotherConnectionBreaksTheProtocol) {
    Server s;
    RawClient client;
    ASSERT_NO_FATAL_FAILURE(begin_dynamic(s, client));
    RawClient other(CipherSuite::CHACHAPOLY_SHA256, *sendspin::server::Identity::from_private_bytes(
                                                        client.noise.identity().private_bytes));
    ASSERT_TRUE(s.server.connect_url(other.url()));
    ASSERT_NO_FATAL_FAILURE(other.activate(pairing_hello_json()));
    other.send_json(R"({"type":"client/pair-auth","payload":{"pake_msg_2":")" +
                    std::string(43, 'A') + R"("}})");
    other.read_until_closed();

    // The server closed it, so core had its message: the pairing still runs, as clients() shows
    // before any result is posted
    const std::vector<sendspin::server::ClientInfo> clients = s.server.clients();
    ASSERT_EQ(clients.size(), 1U);
    ASSERT_TRUE(clients[0].pairing);
    ClientSide side(client, 1);
    client.send_json(side.init_json());
    EXPECT_EQ(client.receive_until("server/pair-init").find(R"({"type":"server/pair-init")"), 0U);
    EXPECT_FALSE(has_result(s, client));
}

// SendspinServer::unpair(): a record whose client is not connected is removed (an all-zero blob),
// and one whose client is pairing ends that pairing first and tells the session with
// server/unpair
TEST(PairingWireUnpair, UnpairsAClientOfflineOrPairing) {
    for (const bool connected : {false, true}) {
        SCOPED_TRACE(connected ? "while pairing" : "not connected");
        RawClient client;
        const std::string id = client.noise.identity().peer_id();
        const Psk old_psk = psk_of(0x30);
        Server s([&](ServerConfig& /*config*/, GatedProvider& provider) {
            seed_record(provider, id, std::nullopt, old_psk);
        });
        size_t arrived = s.provider.hold();
        s.provider.release();
        if (connected) {
            client.noise.psk = old_psk;
            ASSERT_NO_FATAL_FAILURE(connect(s, client));
            ASSERT_TRUE(s.server.begin_pairing(
                id, PairingOptions{PairMethod::DYNAMIC_PAIRING_CODE, CodeFormat::DIGITS}));
            ASSERT_NO_FATAL_FAILURE(rekey_to(client, sendspin::server::SENTINEL_PSK));
            client.receive_until("server/activate");
            arrived = s.provider.hold();
            s.provider.release();
        }
        ASSERT_TRUE(s.server.unpair(id));
        s.provider.wait_arrived(arrived + 1);
        if (connected) {
            EXPECT_EQ(await_result(s, client), PairingResult::CANCELLED);
            EXPECT_EQ(client.receive_until("pair/abort"),
                      R"({"type":"pair/abort","payload":{"reason":"user_cancelled"}})");
            ASSERT_NO_FATAL_FAILURE(rekey_to(client, old_psk));
            EXPECT_EQ(client.receive_until("server/unpair"),
                      R"({"type":"server/unpair","payload":{}})");
        }
        const std::optional<Bytes> blob = s.provider.blob(persistence_keys::client_record_key(0));
        ASSERT_TRUE(blob);
        EXPECT_TRUE(
            std::all_of(blob->begin(), blob->end(), [](uint8_t byte) { return byte == 0; }));
    }
}

}  // namespace
