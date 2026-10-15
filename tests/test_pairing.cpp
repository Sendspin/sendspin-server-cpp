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

/// @file test_pairing.cpp
/// @brief Pairing's decisions as pure functions (pairing.h): which pairing message fits an
/// attempt, the activation's code format, the code an operator types, what a client's pair/abort
/// reports; and the pairing messages' parsing and the trace's redaction (messages.h).

#include "fragmentation.h"
#include "messages.h"
#include "pairing.h"
#include "wire_messages.h"
#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace {

using sendspin::server::ClientPairing;
using sendspin::server::CodeFormat;
using sendspin::server::PairingContext;
using sendspin::server::PairingMessageKind;
using sendspin::server::PairingOffers;
using sendspin::server::PairingStep;
using sendspin::server::PairingVerdict;
using sendspin::server::PairMethod;
namespace wire = sendspin::server::test::wire;

ClientPairing message_of(PairingMessageKind kind, uint32_t index = 0) {
    ClientPairing message;
    message.kind = kind;
    message.pairing_index = index;
    return message;
}

ClientPairing with_commit(ClientPairing message) {
    message.commit_b.emplace();
    return message;
}

ClientPairing with_nonce(ClientPairing message) {
    message.wrapped_nonce_b.emplace();
    return message;
}

ClientPairing with_plain_psk(ClientPairing message) {
    message.long_term_psk.emplace();
    return message;
}

ClientPairing with_wrapped_psk(ClientPairing message) {
    message.wrapped_psk.emplace();
    return message;
}

PairingContext context(std::optional<PairingStep> step, PairMethod method, uint32_t written = 1,
                       uint32_t attempt = 1) {
    PairingContext ctx;
    ctx.step = step;
    ctx.method = method;
    ctx.written_index = written;
    ctx.attempt_index = attempt;
    ctx.round = step && *step != PairingStep::AWAIT_INIT ? 1 : 0;
    return ctx;
}

// pairing.md "Messages", "Pairing index" and "Sequence violations"
TEST(JudgePairingMessage, FitsEachMessageToItsStep) {
    constexpr PairMethod DYNAMIC = PairMethod::DYNAMIC_PAIRING_CODE;
    constexpr PairMethod STATIC = PairMethod::STATIC_PAIRING_CODE;
    constexpr PairMethod PSK = PairMethod::PAIRING_PSK;
    constexpr PairingVerdict ACCEPT = PairingVerdict::ACCEPT;
    constexpr PairingVerdict DISCARD = PairingVerdict::DISCARD;
    constexpr PairingVerdict ERROR = PairingVerdict::PROTOCOL_ERROR;
    using enum PairingStep;
    struct Row {
        const char* name;
        PairingContext ctx;
        ClientPairing message;
        PairingVerdict verdict;
    };
    PairingContext declared = context(std::nullopt, DYNAMIC, 1, 0);
    declared.declared_before = true;
    PairingContext pending_seen = context(AWAIT_INIT, STATIC);
    pending_seen.pending_seen = true;
    PairingContext last_round = context(AWAIT_CONFIRM, DYNAMIC);
    last_round.round = sendspin::server::MAX_PAIRING_ROUNDS;
    PairingContext rekeying = context(AWAIT_INIT, PSK);
    rekeying.rekeying = true;
    const Row rows[] = {
        // client/pair-init and client/pair-pending carry the index
        {"Control: dynamic client/pair-init with commit_B", context(AWAIT_INIT, DYNAMIC),
         with_commit(message_of(PairingMessageKind::INIT, 1)), ACCEPT},
        {"Control: static client/pair-init", context(AWAIT_INIT, STATIC),
         message_of(PairingMessageKind::INIT, 1), ACCEPT},
        {"Control: client/pair-pending", context(AWAIT_INIT, STATIC),
         message_of(PairingMessageKind::PENDING, 1), ACCEPT},
        {"an index above the activations written", context(AWAIT_INIT, DYNAMIC),
         with_commit(message_of(PairingMessageKind::INIT, 2)), ERROR},
        {"an index below them, a leftover", context(AWAIT_INIT, DYNAMIC, 2, 2),
         with_commit(message_of(PairingMessageKind::INIT, 1)), DISCARD},
        {"the index of an activation superseded before it was written",
         context(AWAIT_INIT, DYNAMIC, 2, 3), with_commit(message_of(PairingMessageKind::INIT, 2)),
         DISCARD},
        {"no attempt", context(std::nullopt, DYNAMIC, 1, 0),
         with_commit(message_of(PairingMessageKind::INIT, 1)), DISCARD},
        {"a second client/pair-init", context(AWAIT_CODE, DYNAMIC),
         with_commit(message_of(PairingMessageKind::INIT, 1)), ERROR},
        {"dynamic client/pair-init without commit_B", context(AWAIT_INIT, DYNAMIC),
         message_of(PairingMessageKind::INIT, 1), ERROR},
        {"static client/pair-init with commit_B", context(AWAIT_INIT, STATIC),
         with_commit(message_of(PairingMessageKind::INIT, 1)), ERROR},
        {"a second client/pair-pending", pending_seen, message_of(PairingMessageKind::PENDING, 1),
         ERROR},
        // The count restarts at every handshake: index 1 under older keys is not the attempt's
        {"Control: Pairing PSK client/pair-init", context(AWAIT_INIT, PSK),
         message_of(PairingMessageKind::INIT, 1), ACCEPT},
        {"client/pair-init while a re-handshake is pending", rekeying,
         message_of(PairingMessageKind::INIT, 1), DISCARD},
        {"an earlier code attempt's client/pair-init while one is pending", rekeying,
         with_commit(message_of(PairingMessageKind::INIT, 1)), DISCARD},
        {"client/pair-init above the activations written while one is pending", rekeying,
         message_of(PairingMessageKind::INIT, 2), ERROR},
        {"pair/abort while one is pending", rekeying, message_of(PairingMessageKind::ABORT),
         DISCARD},
        // The rounds
        {"Control: client/pair-auth after server/pair-auth", context(AWAIT_AUTH, DYNAMIC),
         message_of(PairingMessageKind::AUTH), ACCEPT},
        {"client/pair-auth before server/pair-auth", context(AWAIT_CODE, DYNAMIC),
         message_of(PairingMessageKind::AUTH), ERROR},
        {"client/pair-auth replayed after server/pair-confirm", context(AWAIT_CONFIRM, DYNAMIC),
         message_of(PairingMessageKind::AUTH), ERROR},
        {"Control: dynamic client/pair-confirm with wrapped_nonce_B",
         context(AWAIT_CONFIRM, DYNAMIC), with_nonce(message_of(PairingMessageKind::CONFIRM)),
         ACCEPT},
        {"Control: static client/pair-confirm", context(AWAIT_CONFIRM, STATIC),
         message_of(PairingMessageKind::CONFIRM), ACCEPT},
        {"dynamic client/pair-confirm without wrapped_nonce_B", context(AWAIT_CONFIRM, DYNAMIC),
         message_of(PairingMessageKind::CONFIRM), ERROR},
        {"static client/pair-confirm with wrapped_nonce_B", context(AWAIT_CONFIRM, STATIC),
         with_nonce(message_of(PairingMessageKind::CONFIRM)), ERROR},
        {"Control: client/pair-retry in the dynamic flow", context(AWAIT_CONFIRM, DYNAMIC),
         message_of(PairingMessageKind::RETRY), ACCEPT},
        {"client/pair-retry in the static flow", context(AWAIT_CONFIRM, STATIC),
         message_of(PairingMessageKind::RETRY), ERROR},
        {"client/pair-retry past the round limit", last_round,
         message_of(PairingMessageKind::RETRY), ERROR},
        // client/pair-finalize
        {"Control: wrapped_psk after a verified code", context(AWAIT_FINALIZE, DYNAMIC),
         with_wrapped_psk(message_of(PairingMessageKind::FINALIZE)), ACCEPT},
        {"Control: a plain PSK in the Pairing PSK Flow", context(AWAIT_FINALIZE, PSK),
         with_plain_psk(message_of(PairingMessageKind::FINALIZE)), ACCEPT},
        {"a plain PSK after a code", context(AWAIT_FINALIZE, STATIC),
         with_plain_psk(message_of(PairingMessageKind::FINALIZE)), ERROR},
        {"wrapped_psk in the Pairing PSK Flow", context(AWAIT_FINALIZE, PSK),
         with_wrapped_psk(message_of(PairingMessageKind::FINALIZE)), ERROR},
        {"client/pair-finalize before the code verified", context(AWAIT_CONFIRM, DYNAMIC),
         with_wrapped_psk(message_of(PairingMessageKind::FINALIZE)), ERROR},
        {"client/pair-finalize while saving", context(PERSISTING, DYNAMIC),
         with_wrapped_psk(message_of(PairingMessageKind::FINALIZE)), ERROR},
        // Leftovers and pair/abort
        {"client/pair-auth with no attempt on a connection that declared pairing", declared,
         message_of(PairingMessageKind::AUTH), DISCARD},
        {"client/pair-auth with no attempt on one that never did",
         context(std::nullopt, DYNAMIC, 0, 0), message_of(PairingMessageKind::AUTH), ERROR},
        {"Control: pair/abort during an attempt", context(AWAIT_AUTH, DYNAMIC),
         message_of(PairingMessageKind::ABORT), ACCEPT},
        {"pair/abort while saving", context(PERSISTING, DYNAMIC),
         message_of(PairingMessageKind::ABORT), ACCEPT},
        {"pair/abort with no attempt", context(std::nullopt, DYNAMIC, 1, 0),
         message_of(PairingMessageKind::ABORT), DISCARD},
        {"pair/abort before a superseding activation is written",
         context(AWAIT_INIT, DYNAMIC, 1, 2), message_of(PairingMessageKind::ABORT), DISCARD},
    };
    for (const Row& row : rows) {
        SCOPED_TRACE(row.name);
        EXPECT_EQ(sendspin::server::judge_pairing_message(row.ctx, row.message), row.verdict);
    }
}

// messaging.md "server/activate" and pairing.md "QR-code emission": qr_code only for an operator
// interface that scans
TEST(DynamicCodeFormat, PicksAFormatTheClientOffersAndTheOperatorCanUse) {
    struct Row {
        const char* name;
        bool digits;
        bool qr;
        std::optional<CodeFormat> requested;
        bool scan;
        std::optional<CodeFormat> format;
    };
    const Row rows[] = {
        {"Control: digits by default", true, true, std::nullopt, false, CodeFormat::DIGITS},
        {"qr_code when the operator scans", true, true, std::nullopt, true, CodeFormat::QR_CODE},
        {"digits when only they are offered", true, false, std::nullopt, true, CodeFormat::DIGITS},
        {"nothing for qr_code alone and no scanning", false, true, std::nullopt, false,
         std::nullopt},
        {"the requested digits", true, true, CodeFormat::DIGITS, true, CodeFormat::DIGITS},
        {"no requested qr_code without scanning", true, true, CodeFormat::QR_CODE, false,
         std::nullopt},
        {"no requested digits the client lacks", false, true, CodeFormat::DIGITS, true,
         std::nullopt},
        {"no dynamic code offered", false, false, std::nullopt, true, std::nullopt},
    };
    for (const Row& row : rows) {
        SCOPED_TRACE(row.name);
        PairingOffers offers;
        offers.dynamic_digits = row.digits;
        offers.dynamic_qr_code = row.qr;
        EXPECT_EQ(sendspin::server::dynamic_code_format(offers, row.requested, row.scan),
                  row.format);
    }
}

// pairing.md "Pairing Code Presentation": separators never enter the code; the PRS is the
// digits for a static or digits code, the token's bytes for qr_code
TEST(CodePrs, TakesTheCodeAnOperatorTypes) {
    struct Row {
        const char* name;
        std::string input;
        PairMethod method;
        std::optional<CodeFormat> format;
        std::optional<std::string> prs;
    };
    constexpr PairMethod DYNAMIC = PairMethod::DYNAMIC_PAIRING_CODE;
    constexpr PairMethod STATIC = PairMethod::STATIC_PAIRING_CODE;
    const Row rows[] = {
        {"Control: six digits", "123456", DYNAMIC, CodeFormat::DIGITS, "123456"},
        {"digits with a space", "123 456", DYNAMIC, CodeFormat::DIGITS, "123456"},
        {"Control: eight static digits", "12345678", STATIC, std::nullopt, "12345678"},
        {"a static code with a hyphen", "1234-5678", STATIC, std::nullopt, "12345678"},
        {"five digits", "12345", DYNAMIC, CodeFormat::DIGITS, std::nullopt},
        {"eight digits for a dynamic code", "12345678", DYNAMIC, CodeFormat::DIGITS, std::nullopt},
        {"six digits for a static code", "123456", STATIC, std::nullopt, std::nullopt},
        {"a letter", "12345a", DYNAMIC, CodeFormat::DIGITS, std::nullopt},
        {"empty", "", DYNAMIC, CodeFormat::DIGITS, std::nullopt},
        {"a Pairing PSK attempt takes no code", "123456", PairMethod::PAIRING_PSK, std::nullopt,
         std::nullopt},
        {"Control: a version-1 token for qr_code", "SP:14DQ6FY7E4XTOP9HJ5LV6Z3PO57YPD4XT6T97N5Y",
         DYNAMIC, CodeFormat::QR_CODE,
         std::string("\xe0\xe1\xe2\xe3\xe4\xe5\xe6\xe7\xe8\xe9\xea"
                     "\xeb\xec\xed\xee\xef\xf0\xf1\xf2\xf3\xf4\xf5"
                     "\xf6\xf7")},
        {"digits for qr_code", "123456", DYNAMIC, CodeFormat::QR_CODE, std::nullopt},
    };
    for (const Row& row : rows) {
        SCOPED_TRACE(row.name);
        const std::optional<std::vector<uint8_t>> prs =
            sendspin::server::code_prs(row.input, row.method, row.format);
        ASSERT_EQ(prs.has_value(), row.prs.has_value());
        if (prs) {
            EXPECT_EQ(std::string(prs->begin(), prs->end()), *row.prs);
        }
    }
}

// pairing.md "pair/abort": the reasons a client gives, and one this server does not know
TEST(ResultOfAbort, ReportsTheClientsReason) {
    using sendspin::server::PairAbortReason;
    using sendspin::server::PairingResult;
    struct Row {
        std::optional<PairAbortReason> reason;
        PairingResult result;
    };
    const Row rows[] = {
        {PairAbortReason::ATTEMPT_TIMEOUT, PairingResult::CLIENT_TIMED_OUT},
        {PairAbortReason::CONCURRENT_ATTEMPT, PairingResult::CONCURRENT_ATTEMPT},
        {PairAbortReason::METHOD_NOT_SUPPORTED, PairingResult::METHOD_NOT_SUPPORTED},
        {PairAbortReason::PAIRING_CODE_MISMATCH, PairingResult::CODE_MISMATCH},
        {PairAbortReason::USER_CANCELLED, PairingResult::CLIENT_CANCELLED},
        {std::nullopt, PairingResult::CLIENT_CANCELLED},
    };
    for (const Row& row : rows) {
        EXPECT_EQ(sendspin::server::result_of_abort(row.reason), row.result);
    }
}

// pairing.md "Messages": each pairing message's fields, of exactly their length in unpadded
// base64url; a missing or malformed one makes the message malformed ("Protocol Errors")
TEST(ParseClientPairing, ValidatesEachMessagesFields) {
    const std::string share(43, 'A');
    const std::string tag_86 = std::string(86, 'A');
    const std::string sealed(64, 'A');
    const std::string psk(43, 'A');
    struct Row {
        const char* name;
        std::string json;
        bool valid;
    };
    const auto msg = [](const std::string& type, const std::string& payload) {
        return R"({"type":")" + type + R"(","payload":{)" + payload + "}}";
    };
    const Row rows[] = {
        {"Control: client/pair-init", msg("client/pair-init", R"("pairing_index":1)"), true},
        {"Control: client/pair-init with commit_B",
         msg("client/pair-init", R"("pairing_index":1,"commit_B":")" + share + R"(")"), true},
        {"client/pair-init without pairing_index", msg("client/pair-init", ""), false},
        {"a negative pairing_index", msg("client/pair-init", R"("pairing_index":-1)"), false},
        {"a 33-byte commit_B",
         msg("client/pair-init",
             R"("pairing_index":1,"commit_B":")" + std::string(44, 'A') + R"(")"),
         false},
        {"a padded commit_B",
         msg("client/pair-init", R"("pairing_index":1,"commit_B":")" + share + R"(=")"), false},
        {"Control: client/pair-pending with a message",
         msg("client/pair-pending", R"("pairing_index":1,"message":"Press the button")"), true},
        {"a message that is not a string",
         msg("client/pair-pending", R"("pairing_index":1,"message":5)"), false},
        {"Control: client/pair-auth", msg("client/pair-auth", R"("pake_msg_2":")" + share + R"(")"),
         true},
        {"a short pake_msg_2",
         msg("client/pair-auth", R"("pake_msg_2":")" + std::string(42, 'A') + R"(")"), false},
        {"Control: client/pair-confirm",
         msg("client/pair-confirm", R"("client_kc":")" + tag_86 + R"(")"), true},
        {"a short client_kc",
         msg("client/pair-confirm", R"("client_kc":")" + std::string(85, 'A') + R"(")"), false},
        {"a 47-byte wrapped_nonce_B",
         msg("client/pair-confirm", R"("client_kc":")" + tag_86 + R"(","wrapped_nonce_B":")" +
                                        std::string(63, 'A') + R"(")"),
         false},
        {"Control: client/pair-finalize with wrapped_psk",
         msg("client/pair-finalize", R"("wrapped_psk":")" + sealed + R"(")"), true},
        {"Control: client/pair-finalize with long_term_psk",
         msg("client/pair-finalize", R"("long_term_psk":")" + psk + R"(")"), true},
        {"client/pair-finalize with both",
         msg("client/pair-finalize",
             R"("wrapped_psk":")" + sealed + R"(","long_term_psk":")" + psk + R"(")"),
         false},
        {"client/pair-finalize with neither", msg("client/pair-finalize", ""), false},
        {"Control: pair/abort", msg("pair/abort", R"("reason":"user_cancelled")"), true},
        {"Control: pair/abort with a reason this server does not know",
         msg("pair/abort", R"("reason":"later_reason")"), true},
        {"pair/abort without a reason", msg("pair/abort", ""), false},
        {"Control: client/pair-retry", msg("client/pair-retry", ""), true},
    };
    for (const Row& row : rows) {
        SCOPED_TRACE(row.name);
        const sendspin::server::ClientMessage parsed =
            sendspin::server::parse_client_message(row.json, 0);
        EXPECT_EQ(std::holds_alternative<ClientPairing>(parsed), row.valid);
        EXPECT_EQ(std::holds_alternative<sendspin::server::MalformedMessage>(parsed), !row.valid);
    }

    // pairing.md "client/pair-pending": the operator text is cut to 200 characters
    const std::string long_text(300, 'x');
    const sendspin::server::ClientMessage pending = sendspin::server::parse_client_message(
        msg("client/pair-pending", R"("pairing_index":1,"message":")" + long_text + R"(")"), 0);
    ASSERT_TRUE(std::holds_alternative<ClientPairing>(pending));
    EXPECT_EQ(std::get<ClientPairing>(pending).message, std::string(200, 'x'));
}

// The trace never shows a sealed value or a long-term PSK (pairing.md "Wrapping",
// "client/pair-finalize"), whatever escapes spell its key; a message without one is traced as it is
TEST(RedactPairingSecrets, ReplacesEverySecretField) {
    struct Row {
        const char* name;
        std::string json;
        std::optional<std::string> redacted;
    };
    const Row rows[] = {
        {"Control: client/pair-auth has no secret",
         R"({"type":"client/pair-auth","payload":{"pake_msg_2":"abc"}})", std::nullopt},
        {"wrapped_psk", R"({"type":"client/pair-finalize","payload":{"wrapped_psk":"secret"}})",
         R"({"type":"client/pair-finalize","payload":{"wrapped_psk":"<redacted>"}})"},
        {"long_term_psk", R"({"type":"client/pair-finalize","payload":{"long_term_psk":"secret"}})",
         R"({"type":"client/pair-finalize","payload":{"long_term_psk":"<redacted>"}})"},
        {"wrapped_nonce_B",
         R"({"type":"client/pair-confirm","payload":{"client_kc":"tag","wrapped_nonce_B":"s"}})",
         R"({"type":"client/pair-confirm","payload":{"client_kc":"tag","wrapped_nonce_B":"<redacted>"}})"},
        {"unparsable text naming a field", R"({"wrapped_psk":"secret")", "<redacted>"},
        {"a key spelled with an escape",
         R"({"type":"client/pair-finalize","payload":{"wrapped\u005fpsk":"secret"}})",
         R"({"type":"client/pair-finalize","payload":{"wrapped_psk":"<redacted>"}})"},
        {"Control: an escape and no secret",
         R"({"type":"client/pair-pending","payload":{"pairing_index":1,"message":"a\"b"}})",
         std::nullopt},
        {"unparsable text with an escape", R"({"x":"\)", "<redacted>"},
    };
    for (const Row& row : rows) {
        SCOPED_TRACE(row.name);
        std::vector<uint8_t> plaintext{sendspin::server::MESSAGE_ID_JSON};
        plaintext.insert(plaintext.end(), row.json.begin(), row.json.end());
        const std::optional<std::vector<uint8_t>> redacted =
            sendspin::server::redact_pairing_secrets(plaintext);
        ASSERT_EQ(redacted.has_value(), row.redacted.has_value());
        if (redacted) {
            ASSERT_FALSE(redacted->empty());
            EXPECT_EQ(redacted->front(), wire::JSON_ID);
            EXPECT_EQ(std::string(redacted->begin() + 1, redacted->end()), *row.redacted);
        }
    }
}

}  // namespace
