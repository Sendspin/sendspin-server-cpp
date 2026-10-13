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

#include "pairing.h"

#include "crypto/crypto.h"
#include "crypto/pairing_token.h"

#include <algorithm>
#include <utility>

namespace sendspin::server {

namespace {

constexpr std::string_view PAKE_SID_LABEL = "sendspin-pair-pake-v1";
/// ADa and ADb (pairing.md "PAKE")
constexpr std::string_view PAKE_AD_SERVER = "server";
constexpr std::string_view PAKE_AD_CLIENT = "client";

std::vector<uint8_t> bytes_of(std::string_view text) {
    return {text.begin(), text.end()};
}

void append_be32(std::vector<uint8_t>& out, uint32_t value) {
    for (int shift = 24; shift >= 0; shift -= 8) {
        out.push_back(static_cast<uint8_t>((value >> shift) & 0xFF));
    }
}

void wipe(std::vector<uint8_t>& bytes) {
    secure_zero(bytes.data(), bytes.size());
    bytes.clear();
}

bool is_index_message(PairingMessageKind kind) {
    return kind == PairingMessageKind::PENDING || kind == PairingMessageKind::INIT;
}

}  // namespace

PairingOffers pairing_offers(const PairMethods& methods) {
    PairingOffers offers;
    offers.pairing_psk = methods.pairing_psk.has_value();
    offers.static_pairing_code = methods.static_pairing_code.has_value();
    if (methods.dynamic_pairing_code) {
        offers.dynamic_digits = (methods.dynamic_pairing_code->formats & pair_code::DIGITS) != 0;
        offers.dynamic_qr_code = (methods.dynamic_pairing_code->formats & pair_code::QR_CODE) != 0;
    }
    return offers;
}

bool offers_method(const PairingOffers& offers, PairMethod method) {
    switch (method) {
        case PairMethod::PAIRING_PSK:
            return offers.pairing_psk;
        case PairMethod::STATIC_PAIRING_CODE:
            return offers.static_pairing_code;
        case PairMethod::DYNAMIC_PAIRING_CODE:
            break;
    }
    return offers.dynamic_digits || offers.dynamic_qr_code;
}

std::optional<CodeFormat> dynamic_code_format(const PairingOffers& offers,
                                              std::optional<CodeFormat> requested,
                                              bool operator_can_scan_qr) {
    const bool qr = offers.dynamic_qr_code && operator_can_scan_qr;
    if (requested) {
        if ((*requested == CodeFormat::QR_CODE && qr) ||
            (*requested == CodeFormat::DIGITS && offers.dynamic_digits)) {
            return requested;
        }
        return std::nullopt;
    }
    if (qr) {
        return CodeFormat::QR_CODE;
    }
    return offers.dynamic_digits ? std::optional(CodeFormat::DIGITS) : std::nullopt;
}

PskCategory attempt_psk(PairMethod method) {
    return method == PairMethod::PAIRING_PSK ? PskCategory::PAIRING : PskCategory::SENTINEL;
}

PairingVerdict judge_pairing_message(const PairingContext& ctx, const ClientPairing& message) {
    const bool dynamic = ctx.method == PairMethod::DYNAMIC_PAIRING_CODE;
    if (is_index_message(message.kind)) {
        // pairing.md "Pairing index"
        if (message.pairing_index > ctx.written_index) {
            return PairingVerdict::PROTOCOL_ERROR;
        }
        if (message.pairing_index < ctx.written_index || !ctx.step ||
            ctx.attempt_index != ctx.written_index) {
            return PairingVerdict::DISCARD;
        }
        if (*ctx.step != PairingStep::AWAIT_INIT) {
            return PairingVerdict::PROTOCOL_ERROR;
        }
        if (message.kind == PairingMessageKind::PENDING) {
            return ctx.pending_seen ? PairingVerdict::PROTOCOL_ERROR : PairingVerdict::ACCEPT;
        }
        // pairing.md "client/pair-init": commit_B in the Dynamic Pairing Code Flow only
        return message.commit_b.has_value() == dynamic ? PairingVerdict::ACCEPT
                                                       : PairingVerdict::PROTOCOL_ERROR;
    }
    // pairing.md "Entering and leaving pairing": in flight from an attempt that ended
    const bool leftover = !ctx.step || *ctx.step == PairingStep::AWAIT_INIT;
    if (message.kind == PairingMessageKind::ABORT) {
        // pairing.md "pair/abort": one after the attempt ended has no effect
        const bool current = ctx.step && ctx.attempt_index == ctx.written_index;
        return current ? PairingVerdict::ACCEPT : PairingVerdict::DISCARD;
    }
    if (leftover) {
        return ctx.declared_before ? PairingVerdict::DISCARD : PairingVerdict::PROTOCOL_ERROR;
    }
    const PairingStep step = *ctx.step;
    bool fits = false;
    switch (message.kind) {
        case PairingMessageKind::AUTH:
            fits = step == PairingStep::AWAIT_AUTH;
            break;
        case PairingMessageKind::RETRY:
            // pairing.md "Rounds": dynamic only, and the client aborts at the round limit
            fits = dynamic && step == PairingStep::AWAIT_CONFIRM && ctx.round < MAX_PAIRING_ROUNDS;
            break;
        case PairingMessageKind::CONFIRM:
            fits = ctx.method != PairMethod::PAIRING_PSK && step == PairingStep::AWAIT_CONFIRM &&
                   message.wrapped_nonce_b.has_value() == dynamic;
            break;
        case PairingMessageKind::FINALIZE:
            fits = step == PairingStep::AWAIT_FINALIZE &&
                   message.long_term_psk.has_value() == (ctx.method == PairMethod::PAIRING_PSK);
            break;
        case PairingMessageKind::PENDING:
        case PairingMessageKind::INIT:
        case PairingMessageKind::ABORT:
            break;
    }
    return fits ? PairingVerdict::ACCEPT : PairingVerdict::PROTOCOL_ERROR;
}

std::optional<std::vector<uint8_t>> code_prs(std::string_view input, PairMethod method,
                                             std::optional<CodeFormat> format) {
    if (method == PairMethod::PAIRING_PSK) {
        return std::nullopt;
    }
    if (method == PairMethod::DYNAMIC_PAIRING_CODE && format == CodeFormat::QR_CODE) {
        const std::optional<PairingCodeToken> token = decode_pairing_code_token(input);
        if (!token) {
            return std::nullopt;
        }
        return std::vector<uint8_t>(token->code.begin(), token->code.end());
    }
    // pairing.md "Pairing Code Presentation": separators never enter the code
    std::vector<uint8_t> digits;
    digits.reserve(input.size());
    for (const char c : input) {
        if (c == ' ' || c == '-') {
            continue;
        }
        if (c < '0' || c > '9') {
            wipe(digits);
            return std::nullopt;
        }
        digits.push_back(static_cast<uint8_t>(c));
    }
    const size_t expected = method == PairMethod::STATIC_PAIRING_CODE ? STATIC_PAIRING_CODE_DIGITS
                                                                      : DYNAMIC_PAIRING_CODE_DIGITS;
    if (digits.size() != expected) {
        wipe(digits);
        return std::nullopt;
    }
    return digits;
}

CodeFormat code_format_for(PairMethod method, std::optional<CodeFormat> format) {
    if (method == PairMethod::PAIRING_PSK) {
        return CodeFormat::TOKEN;
    }
    return format.value_or(CodeFormat::DIGITS);
}

const char* aead_name(CipherSuite suite) {
    return suite == CipherSuite::AESGCM_SHA256 ? AEAD_AESGCM : AEAD_CHACHAPOLY;
}

PairingResult result_of_abort(std::optional<PairAbortReason> reason) {
    if (!reason) {
        return PairingResult::CLIENT_CANCELLED;
    }
    switch (*reason) {
        case PairAbortReason::ATTEMPT_TIMEOUT:
            return PairingResult::CLIENT_TIMED_OUT;
        case PairAbortReason::CONCURRENT_ATTEMPT:
            return PairingResult::CONCURRENT_ATTEMPT;
        case PairAbortReason::METHOD_NOT_SUPPORTED:
            return PairingResult::METHOD_NOT_SUPPORTED;
        case PairAbortReason::PAIRING_CODE_MISMATCH:
            return PairingResult::CODE_MISMATCH;
        case PairAbortReason::USER_CANCELLED:
            break;
    }
    return PairingResult::CLIENT_CANCELLED;
}

// ==============================================================================
// PairingAttempt
// ==============================================================================

PairingAttempt::PairingAttempt(PairMethod method, std::optional<CodeFormat> format, uint32_t index,
                               bool declared_before)
    : method_(method), format_(format), index_(index), declared_before_(declared_before) {}

PairingAttempt::~PairingAttempt() {
    wipe(this->prs_);
    wipe(this->round_prs_);
    secure_zero(this->nonce_a_.data(), this->nonce_a_.size());
}

PairingContext PairingAttempt::context(uint32_t written_index) const {
    PairingContext ctx;
    ctx.step = this->step_;
    ctx.method = this->method_;
    ctx.written_index = written_index;
    ctx.attempt_index = this->index_;
    ctx.declared_before = this->declared_before_;
    ctx.pending_seen = this->pending_seen_;
    ctx.round = this->round_;
    return ctx;
}

std::vector<uint8_t> PairingAttempt::sid() const {
    std::vector<uint8_t> sid = bytes_of(PAKE_SID_LABEL);
    sid.insert(sid.end(), this->h_.begin(), this->h_.end());
    append_be32(sid, this->index_);
    append_be32(sid, this->round_);
    return sid;
}

std::optional<std::string> PairingAttempt::on_init(const ClientPairing& init,
                                                   const HandshakeHash& h) {
    this->h_ = h;
    if (this->method_ == PairMethod::PAIRING_PSK) {
        // pairing.md "Pairing PSK Flow": client/pair-finalize follows at once
        this->step_ = PairingStep::AWAIT_FINALIZE;
        return std::nullopt;
    }
    this->step_ = PairingStep::AWAIT_CODE;
    if (this->method_ == PairMethod::STATIC_PAIRING_CODE) {
        // pairing.md "PAKE": always round 1 in the Static Pairing Code Flow
        this->round_ = 1;
        return std::nullopt;
    }
    // pairing.md "Dynamic Pairing Code Flow": the first round carries nonce_A
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access): judge_pairing_message() requires it
    this->commit_b_ = *init.commit_b;
    this->nonce_a_ = pairing_generate_nonce();
    this->round_ = 1;
    // A code entered before the round's server/pair-init cannot be this attempt's
    wipe(this->prs_);
    return build_server_pair_init(this->nonce_a_);
}

std::string PairingAttempt::on_retry() {
    ++this->round_;
    this->step_ = PairingStep::AWAIT_CODE;
    wipe(this->prs_);
    return build_server_pair_init(std::nullopt);
}

void PairingAttempt::set_code(std::vector<uint8_t> prs) {
    wipe(this->prs_);
    this->prs_ = std::move(prs);
}

std::optional<std::string> PairingAttempt::start_round() {
    wipe(this->round_prs_);
    this->round_prs_ = std::move(this->prs_);
    this->prs_.clear();
    // pairing.md "PAKE": CI empty, ADa "server", ADb "client"
    if (!this->cpace_.start(CPaceRole::INITIATOR, this->round_prs_, this->sid(), {},
                            bytes_of(PAKE_AD_SERVER), bytes_of(PAKE_AD_CLIENT))) {
        return std::nullopt;
    }
    this->step_ = PairingStep::AWAIT_AUTH;
    return build_server_pair_auth(this->cpace_.public_share());
}

std::optional<std::string> PairingAttempt::on_auth(const ClientPairing& auth) {
    // pairing.md "Protocol Errors": a share of the wrong length or a low-order point
    if (!this->cpace_.derive(auth.pake_msg_2.data(), auth.pake_msg_2.size())) {
        return std::nullopt;
    }
    const std::optional<std::array<uint8_t, CPACE_TAG_SIZE>> tag = this->cpace_.tag();
    if (!tag) {
        return std::nullopt;
    }
    this->step_ = PairingStep::AWAIT_CONFIRM;
    return build_server_pair_confirm(*tag);
}

PairingAttempt::Confirmation PairingAttempt::on_confirm(const ClientPairing& confirm,
                                                        CipherSuite suite) {
    // pairing.md "Server verification", in this order: client_kc, the opening of commit_B, the
    // code's binding
    if (!this->cpace_.verify(confirm.client_kc.data(), confirm.client_kc.size())) {
        return Confirmation::CODE_MISMATCH;
    }
    if (this->method_ == PairMethod::DYNAMIC_PAIRING_CODE) {
        // judge_pairing_message() requires wrapped_nonce_B in the Dynamic Pairing Code Flow
        if (!confirm.wrapped_nonce_b) {
            return Confirmation::PROTOCOL_ERROR;
        }
        std::optional<std::array<uint8_t, CPACE_ISK_SIZE>> isk = this->cpace_.isk();
        std::optional<std::array<uint8_t, WRAPPED_PLAINTEXT_SIZE>> nonce_b;
        if (isk) {
            nonce_b = unwrap_value(NONCE_WRAP_LABEL, aead_name(suite), this->cpace_.sid(), *isk,
                                   *confirm.wrapped_nonce_b);
            secure_zero(isk->data(), isk->size());
        }
        bool bound =
            nonce_b && pairing_code_verify_commit(nonce_b->data(), nonce_b->size(),
                                                  this->commit_b_.data(), this->commit_b_.size());
        if (bound) {
            std::optional<std::array<uint8_t, PAIRING_DIGEST_SIZE>> digest =
                pairing_code_digest(this->h_.data(), this->h_.size(), this->nonce_a_.data(),
                                    this->nonce_a_.size(), nonce_b->data(), nonce_b->size());
            std::vector<uint8_t> derived;
            if (digest && this->format_ == CodeFormat::QR_CODE) {
                const std::array<uint8_t, QR_PAIRING_CODE_SIZE> code =
                    pairing_code_qr_bytes(*digest);
                derived.assign(code.begin(), code.end());
            } else if (digest) {
                derived = pairing_code_digits_prs(pairing_code_digits(*digest));
            }
            bound = !derived.empty() && derived.size() == this->round_prs_.size() &&
                    constant_time_equal(derived.data(), this->round_prs_.data(), derived.size());
            wipe(derived);
            if (digest) {
                secure_zero(digest->data(), digest->size());
            }
        }
        if (nonce_b) {
            secure_zero(nonce_b->data(), nonce_b->size());
        }
        if (!bound) {
            return Confirmation::PROTOCOL_ERROR;
        }
    }
    // The code has done its part: only the CPace output is needed from here
    wipe(this->round_prs_);
    this->step_ = PairingStep::AWAIT_FINALIZE;
    return Confirmation::VERIFIED;
}

std::optional<HandshakePsk> PairingAttempt::on_finalize(const ClientPairing& finalize,
                                                        CipherSuite suite) {
    if (finalize.long_term_psk) {
        this->step_ = PairingStep::PERSISTING;
        return HandshakePsk{PskCategory::LONG_TERM, finalize.long_term_psk->psk};
    }
    std::optional<std::array<uint8_t, CPACE_ISK_SIZE>> isk = this->cpace_.isk();
    // judge_pairing_message() requires one of the two
    if (!isk || !finalize.wrapped_psk) {
        return std::nullopt;
    }
    std::optional<std::array<uint8_t, WRAPPED_PLAINTEXT_SIZE>> psk = unwrap_value(
        PSK_WRAP_LABEL, aead_name(suite), this->cpace_.sid(), *isk, *finalize.wrapped_psk);
    secure_zero(isk->data(), isk->size());
    if (!psk) {
        return std::nullopt;
    }
    HandshakePsk unwrapped{PskCategory::LONG_TERM, *psk};
    secure_zero(psk->data(), psk->size());
    this->step_ = PairingStep::PERSISTING;
    return unwrapped;
}

}  // namespace sendspin::server
