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

/// @file pairing.h
/// @brief One pairing attempt as the server, CPace role A (spec pairing.md): the decisions as pure
/// functions (what a client offers, which PSK an attempt runs under, whether a pairing message
/// fits the attempt's step) and PairingAttempt, the attempt's state and crypto. Core thread only.

#pragma once

#include "crypto/cpace.h"
#include "crypto/keys.h"
#include "crypto/pairing_code.h"
#include "crypto/psk_wrap.h"
#include "messages.h"
#include "noise.h"
#include "sendspin/server/types.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace sendspin::server {

/// @brief Rounds of one Dynamic Pairing Code attempt: the client aborts rather than retry at this
/// count (spec pairing.md "Rounds"), so a retry past it is a protocol error
constexpr uint32_t MAX_PAIRING_ROUNDS = 20;

/// @brief How long the server waits on the client within an attempt: for client/pair-auth,
/// client/pair-confirm or client/pair-finalize. Above the client's own attempt timeout (2 minutes
/// recommended, spec pairing.md "Entering and leaving pairing"), so it ends only a silent client.
constexpr int64_t PAIRING_CLIENT_STEP_TIMEOUT_US = 150'000'000;

/// @brief The methods and formats client/hello offers, for ClientInfo
PairingOffers pairing_offers(const PairMethods& methods);

/// @brief Whether offers include method
bool offers_method(const PairingOffers& offers, PairMethod method);

/// @brief The `format` of a Dynamic Pairing Code activation (messaging.md "server/activate"):
/// the requested one when the client offers it, else QR_CODE when the client offers it and the
/// operator can scan, else DIGITS when offered; never QR_CODE unless the operator can scan.
/// nullopt when nothing fits.
std::optional<CodeFormat> dynamic_code_format(const PairingOffers& offers,
                                              std::optional<CodeFormat> requested,
                                              bool operator_can_scan_qr);

/// @brief The PSK an attempt with method runs under (pairing.md "Pairing PSK Flow", "Dynamic
/// Pairing Code Flow", "Static Pairing Code Flow"): the pairing PSK for PAIRING_PSK, the Sentinel
/// for a code
PskCategory attempt_psk(PairMethod method);

/// @brief Where an attempt stands, from its pairing activation on
enum class PairingStep : uint8_t {
    /// The pairing activation is queued or written; client/pair-init starts the attempt
    AWAIT_INIT,
    /// A code method waits for the operator's code: before server/pair-auth
    AWAIT_CODE,
    /// server/pair-auth went out
    AWAIT_AUTH,
    /// server/pair-confirm went out
    AWAIT_CONFIRM,
    /// The client's confirmation verified, or a Pairing PSK attempt started:
    /// client/pair-finalize is next
    AWAIT_FINALIZE,
    /// The long-term PSK is being saved (pairing.md "server/pair-finalize")
    PERSISTING,
};

/// @brief What the server does with a pairing message
enum class PairingVerdict : uint8_t {
    ACCEPT,
    /// Dropped silently: a leftover of an attempt that ended, or of a superseded activation
    /// (pairing.md "Entering and leaving pairing", "Pairing index")
    DISCARD,
    /// pairing.md "Sequence violations": close silently, persist nothing
    PROTOCOL_ERROR,
};

/// @brief The attempt a pairing message is judged against
struct PairingContext {
    /// nullopt when no attempt runs on the connection
    std::optional<PairingStep> step;
    PairMethod method{PairMethod::PAIRING_PSK};
    /// The pairing activations the client has received since the last Noise handshake
    uint32_t written_index{0};
    /// The index of the attempt's activation
    uint32_t attempt_index{0};
    /// The connection declared `'pairing'` before this attempt's activation, so a message without
    /// a pairing_index may belong to an earlier attempt
    bool declared_before{false};
    bool pending_seen{false};
    /// The rounds run, 0 before the first server/pair-init
    uint32_t round{0};
};

/// @brief Whether message fits ctx (pairing.md "Messages", "Pairing index", "Sequence
/// violations"). A pairing_index above the activations written is a protocol error and one below
/// is a leftover; one between them answers an activation superseded before it was written. Each
/// message fits one step, with the fields of its method (commit_B and wrapped_nonce_B in the
/// Dynamic Pairing Code Flow only, the plain PSK in client/pair-finalize for Pairing PSK only); a
/// pair/abort ends any step. Without an attempt, or before its client/pair-init, a message
/// without an index is a leftover once the connection declared pairing before. Two choices the
/// spec leaves open: a pairing_index of 0 is below any count an attempt has, so a leftover; and
/// a pair/abort with a reason this server does not recognize still ends the attempt
/// (result_of_abort()), since the client has ended it.
PairingVerdict judge_pairing_message(const PairingContext& ctx, const ClientPairing& message);

/// @brief A code the operator entered, as CPace's PRS for an attempt's method and format
/// (pairing.md "PAKE", "Pairing Code Presentation"): digits with spaces and hyphens stripped,
/// 8 for a static code and 6 for a dynamic one, or a version-1 token for QR_CODE. nullopt for
/// input of another shape. The caller wipes it.
std::optional<std::vector<uint8_t>> code_prs(std::string_view input, PairMethod method,
                                             std::optional<CodeFormat> format);

/// @brief What the operator enters next for an attempt
CodeFormat code_format_for(PairMethod method, std::optional<CodeFormat> format);

/// @brief noise-c's name of the suite's AEAD, for wrapping (pairing.md "Wrapping")
const char* aead_name(CipherSuite suite);

/// @brief The pairing result a client's pair/abort reports; a reason the server does not
/// recognize ends the attempt as the client cancelling it
PairingResult result_of_abort(std::optional<PairAbortReason> reason);

/// @brief One attempt's state and crypto, from its pairing activation to its end. It holds the
/// attempt's secrets (the code, the CPace run) and wipes them when destroyed.
class PairingAttempt {
public:
    /// @param index the pairing_index of its activation
    /// @param declared_before the connection declared `'pairing'` before this activation
    PairingAttempt(PairMethod method, std::optional<CodeFormat> format, uint32_t index,
                   bool declared_before);
    ~PairingAttempt();
    PairingAttempt(const PairingAttempt&) = delete;
    PairingAttempt& operator=(const PairingAttempt&) = delete;

    PairMethod method() const {
        return this->method_;
    }

    std::optional<CodeFormat> format() const {
        return this->format_;
    }

    PairingStep step() const {
        return this->step_;
    }

    uint32_t round() const {
        return this->round_;
    }

    /// @brief The context judge_pairing_message() takes, with the activations written
    PairingContext context(uint32_t written_index) const;

    /// @brief A client/pair-pending was accepted
    void note_pending() {
        this->pending_seen_ = true;
    }

    /// @brief The accepted client/pair-init, with the handshake hash it binds to: Pairing PSK
    /// goes on to wait for client/pair-finalize, a code method for the operator's code
    /// @return the first server/pair-init for the Dynamic Pairing Code Flow, else nullopt
    std::optional<std::string> on_init(const ClientPairing& init, const HandshakeHash& h);

    /// @brief A client/pair-retry: the next round's server/pair-init, without nonce_A
    std::string on_retry();

    /// @brief Keeps the operator's code until the attempt needs it; replaces an earlier one
    void set_code(std::vector<uint8_t> prs);

    /// @brief Whether a code is kept
    bool has_code() const {
        return !this->prs_.empty();
    }

    /// @brief In AWAIT_CODE with a code: runs the round's CPace start and consumes the code
    /// @return the server/pair-auth; nullopt when CPace fails, which ends the attempt
    std::optional<std::string> start_round();

    /// @brief The accepted client/pair-auth
    /// @return the server/pair-confirm; nullopt for a share CPace refuses, a protocol error
    std::optional<std::string> on_auth(const ClientPairing& auth);

    /// @brief How a client/pair-confirm checks out (pairing.md "Server verification")
    enum class Confirmation : uint8_t {
        VERIFIED,
        /// client_kc failed: pair/abort `pairing_code_mismatch`
        CODE_MISMATCH,
        /// The opening of commit_B or the code's binding failed: a protocol error
        PROTOCOL_ERROR,
    };

    /// @brief The accepted client/pair-confirm, checked in the spec's order
    Confirmation on_confirm(const ClientPairing& confirm, CipherSuite suite);

    /// @brief The long-term PSK of the accepted client/pair-finalize: carried as it is for
    /// Pairing PSK, unwrapped under the attempt's CPace output for a code. Enters PERSISTING.
    /// @return nullopt for a wrapped_psk that fails to decrypt, a protocol error
    std::optional<HandshakePsk> on_finalize(const ClientPairing& finalize, CipherSuite suite);

private:
    /// @brief The CPace sid of the current round (pairing.md "PAKE")
    std::vector<uint8_t> sid() const;

    PairMethod method_;
    std::optional<CodeFormat> format_;
    uint32_t index_;
    bool declared_before_;
    PairingStep step_{PairingStep::AWAIT_INIT};
    bool pending_seen_{false};
    uint32_t round_{0};
    HandshakeHash h_{};
    std::array<uint8_t, PAIRING_NONCE_SIZE> nonce_a_{};
    std::array<uint8_t, PAIRING_COMMIT_SIZE> commit_b_{};
    /// The operator's code, as PRS, until a round takes it; then the round's
    std::vector<uint8_t> prs_;
    std::vector<uint8_t> round_prs_;
    CPace cpace_;
};

}  // namespace sendspin::server
