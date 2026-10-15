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

/// @file test_session_internals.cpp
/// @brief A pairing message's secret wiped from the session's buffers, spare capacity included,
/// however the message or the session ends, and from a reassembly buffer's storage past a shorter
/// message. Compiled with -fno-access-control (tests/CMakeLists.txt).

#include "fragmentation.h"
#include "freed_blocks.h"
#include "session.h"
#include "session_harness.h"
#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

using sendspin::server::Reassembler;
using sendspin::server::SessionEnd;
using sendspin::server::SessionSettings;
using sendspin::server::test::Bytes;
using sendspin::server::test::bytes_of;
using sendspin::server::test::Harness;
using sendspin::server::test::SessionTest;
using sendspin::server::test::test_settings;

// Whether a vector's bytes, read to its capacity, hold needle. The spare capacity is outside the
// container's annotation, so the scan is not instrumented and calls nothing that is.
__attribute__((no_sanitize("address"))) bool holds_to_capacity(const std::vector<uint8_t>& bytes,
                                                               std::string_view needle) {
    const uint8_t* data = bytes.data();
    for (size_t start = 0; start + needle.size() <= bytes.capacity(); ++start) {
        size_t matched = 0;
        while (matched < needle.size() &&
               data[start + matched] == static_cast<uint8_t>(needle[matched])) {
            ++matched;
        }
        if (matched == needle.size()) {
            return true;
        }
    }
    return false;
}

// pairing.md "client/pair-finalize": a pair-finalize carrying both long_term_psk and wrapped_psk
// fails validation, and its PSK is wiped from the decrypted message and the reassembly buffer,
// spare capacity included, as a valid one's is (the Control), and as it is from a body whose type
// cannot be read and from a fragment sequence that fails or outgrows the cap with the PSK buffered.
// The first fragment's check that the scan finds the PSK is the Control of the scan.
TEST_P(SessionTest, WipesTheSecretOfAPairingMessageWhetherOrNotItPassesValidation) {
    using sendspin::server::FRAGMENT_FLAG_FIRST;
    using sendspin::server::FRAGMENT_FLAG_LAST;
    using sendspin::server::FRAGMENT_FLAGS_RESERVED;
    const std::string psk = "QUJDREVGR0hJSktMTU5PUFFSU1RVVldYWVphYmNkZWY";
    const std::string valid_head =
        R"({"type":"client/pair-finalize","payload":{"long_term_psk":")" + psk;
    const std::string valid_tail = R"("}})";
    const std::string head = valid_head + R"(","wrapped_psk":"AAAA)";
    const std::string tail = R"("}})";
    struct Case {
        const char* name;
        std::string head;
        std::string tail;
        // Whether head and tail go as two fragments rather than one message
        bool fragmented;
        // The second fragment's flags: LAST completes the message, a reserved bit fails it
        uint8_t second_flags;
        std::optional<SessionEnd> end;
        // The cap on a reassembled message: the head fits, head and tail do not
        size_t max_reassembled_bytes{sendspin::server::DEFAULT_MAX_REASSEMBLED_BYTES};
    };
    const std::vector<Case> cases{
        {"Control: a valid one in one message", valid_head, valid_tail, false, 0, std::nullopt},
        {"Control: a valid one in two fragments", valid_head, valid_tail, true, FRAGMENT_FLAG_LAST,
         std::nullopt},
        {"in one message", head, tail, false, 0, SessionEnd::PROTOCOL_ERROR},
        {"in a body that is not JSON, its type unread", head, "", false, 0,
         SessionEnd::PROTOCOL_ERROR},
        {"in two fragments, the PSK in the first", head, tail, true, FRAGMENT_FLAG_LAST,
         SessionEnd::PROTOCOL_ERROR},
        {"in a fragment sequence that fails", head, tail, true, FRAGMENT_FLAGS_RESERVED,
         SessionEnd::MALFORMED_FRAGMENTS},
        {"in a fragment sequence that outgrows the cap", head, tail, true, FRAGMENT_FLAG_LAST,
         SessionEnd::MESSAGE_TOO_LARGE, head.size()},
    };
    for (const Case& c : cases) {
        SCOPED_TRACE(c.name);
        SessionSettings settings = test_settings();
        settings.max_reassembled_bytes = c.max_reassembled_bytes;
        Harness h(GetParam(), settings);
        ASSERT_NO_FATAL_FAILURE(h.activate());
        if (c.fragmented) {
            Bytes first{sendspin::server::MESSAGE_ID_JSON};
            const Bytes head_bytes = bytes_of(c.head);
            first.insert(first.end(), head_bytes.begin(), head_bytes.end());
            h.send(h.fragment(FRAGMENT_FLAG_FIRST, first));
            EXPECT_TRUE(holds_to_capacity(h.session.scratch_, psk));
            EXPECT_TRUE(holds_to_capacity(h.session.reassembler_.buffer_, psk));
            h.send(h.fragment(c.second_flags, bytes_of(c.tail)));
        } else {
            h.send(h.client.encrypt_json(c.head + c.tail));
        }
        EXPECT_EQ(h.session.end(), c.end);
        EXPECT_EQ(h.session.take_pairing_messages().size(), c.end ? 0U : 1U);
        EXPECT_FALSE(holds_to_capacity(h.session.scratch_, psk));
        EXPECT_FALSE(holds_to_capacity(h.session.reassembler_.buffer_, psk));
    }
}

// A client/pair-finalize whose tag fails leaves no PSK in the decrypted message's buffer: AES-GCM
// decrypts in place before its tag check (ChaChaPoly checks first), and the buffer is wiped
TEST_P(SessionTest, WipesAPairingMessageWhoseTagFails) {
    const std::string psk = "QUJDREVGR0hJSktMTU5PUFFSU1RVVldYWVphYmNkZWY";
    Harness h(GetParam());
    ASSERT_NO_FATAL_FAILURE(h.activate());
    Bytes message = h.client.encrypt_json(
        R"({"type":"client/pair-finalize","payload":{"long_term_psk":")" + psk + R"("}})");
    message.back() ^= 0x01;
    h.send(message);
    EXPECT_EQ(h.session.end(), SessionEnd::DECRYPT_FAILED);
    EXPECT_FALSE(holds_to_capacity(h.session.scratch_, psk));
}

// A session that ends while a pairing message's first fragment is pending leaves its PSK in
// neither buffer, whether the host closes it or the next message fails to decrypt. The scan finds
// the PSK before the end: the Control.
TEST_P(SessionTest, WipesAPendingPairingFragmentAtTheEnd) {
    using sendspin::server::FRAGMENT_FLAG_FIRST;
    const std::string psk = "QUJDREVGR0hJSktMTU5PUFFSU1RVVldYWVphYmNkZWY";
    struct Case {
        const char* name;
        std::function<void(Harness&)> end_session;
        SessionEnd end;
    };
    const std::vector<Case> cases{
        {"closed by the host", [](Harness& h) { h.session.close_by_host(); },
         SessionEnd::CLOSED_BY_HOST},
        {"a message that fails to decrypt", [](Harness& h) { h.send(Bytes(32, 0)); },
         SessionEnd::DECRYPT_FAILED},
    };
    for (const Case& c : cases) {
        SCOPED_TRACE(c.name);
        Harness h(GetParam());
        ASSERT_NO_FATAL_FAILURE(h.activate());
        Bytes first{sendspin::server::MESSAGE_ID_JSON};
        const Bytes head =
            bytes_of(R"({"type":"client/pair-finalize","payload":{"long_term_psk":")" + psk);
        first.insert(first.end(), head.begin(), head.end());
        h.send(h.fragment(FRAGMENT_FLAG_FIRST, first));
        EXPECT_TRUE(holds_to_capacity(h.session.scratch_, psk));
        EXPECT_TRUE(holds_to_capacity(h.session.reassembler_.buffer_, psk));
        c.end_session(h);
        EXPECT_EQ(h.session.end(), c.end);
        EXPECT_FALSE(holds_to_capacity(h.session.scratch_, psk));
        EXPECT_FALSE(holds_to_capacity(h.session.reassembler_.buffer_, psk));
    }
}

// A pairing message's first fragment, its PSK in it, followed by a longer one, grows both the
// decrypted message's buffer and the reassembly buffer; neither frees a block holding the PSK.
// The scan's Control is SecureReserve.WipesTheBlockItLeaves.
TEST_P(SessionTest, GrowsItsBuffersWithoutFreeingAPairingSecret) {
    using sendspin::server::FRAGMENT_FLAG_FIRST;
    if (!FreedBlockScan::available()) {
        GTEST_SKIP() << "needs the sanitizer's free hook";
    }
    const std::string psk = "QUJDREVGR0hJSktMTU5PUFFSU1RVVldYWVphYmNkZWY";
    Harness h(GetParam());
    ASSERT_NO_FATAL_FAILURE(h.activate());
    Bytes first{sendspin::server::MESSAGE_ID_JSON};
    const Bytes head =
        bytes_of(R"({"type":"client/pair-finalize","payload":{"long_term_psk":")" + psk);
    first.insert(first.end(), head.begin(), head.end());
    h.send(h.fragment(FRAGMENT_FLAG_FIRST, first));
    ASSERT_TRUE(holds_to_capacity(h.session.scratch_, psk));
    ASSERT_TRUE(holds_to_capacity(h.session.reassembler_.buffer_, psk));
    const size_t scratch_capacity = h.session.scratch_.capacity();
    const size_t buffer_capacity = h.session.reassembler_.buffer_.capacity();
    const Bytes longer = h.fragment(0, Bytes(4 * scratch_capacity, 'a'));
    {
        const FreedBlockScan scan(psk);
        h.send(longer);
        EXPECT_EQ(scan.holding(), 0);
    }
    EXPECT_FALSE(h.session.end());
    EXPECT_GT(h.session.scratch_.capacity(), scratch_capacity);
    EXPECT_GT(h.session.reassembler_.buffer_.capacity(), buffer_capacity);
}

// A session destroyed while a pairing message's first fragment is pending, with no end before,
// frees neither buffer holding the PSK. The scan's Control is SecureReserve.WipesTheBlockItLeaves.
TEST_P(SessionTest, WipesAPendingPairingFragmentWhenDestroyed) {
    using sendspin::server::FRAGMENT_FLAG_FIRST;
    if (!FreedBlockScan::available()) {
        GTEST_SKIP() << "needs the sanitizer's free hook";
    }
    const std::string psk = "QUJDREVGR0hJSktMTU5PUFFSU1RVVldYWVphYmNkZWY";
    std::optional<Harness> h;
    h.emplace(GetParam());
    ASSERT_NO_FATAL_FAILURE(h->activate());
    Bytes first{sendspin::server::MESSAGE_ID_JSON};
    const Bytes head =
        bytes_of(R"({"type":"client/pair-finalize","payload":{"long_term_psk":")" + psk);
    first.insert(first.end(), head.begin(), head.end());
    h->send(h->fragment(FRAGMENT_FLAG_FIRST, first));
    ASSERT_TRUE(holds_to_capacity(h->session.scratch_, psk));
    ASSERT_TRUE(holds_to_capacity(h->session.reassembler_.buffer_, psk));
    ASSERT_FALSE(h->session.end());
    const FreedBlockScan scan(psk);
    h.reset();
    EXPECT_EQ(scan.holding(), 0);
}

// A wipe reaches the reassembly buffer's storage past a shorter message, where a longer one
// written since the last wipe left its bytes. Before the wipe the scan finds them: the Control.
TEST(Reassembler, WipesTheStoragePastTheMessage) {
    using sendspin::server::FRAGMENT_FLAG_FIRST;
    using sendspin::server::FRAGMENT_FLAG_LAST;
    const std::string secret = "QUJDREVGR0hJSktMTU5PUFFSU1RVVldYWVphYmNkZWY";
    Reassembler reassembler(sendspin::server::DEFAULT_MAX_REASSEMBLED_BYTES,
                            [](uint8_t) { return true; });
    const auto whole = [](const std::string& text) {
        Bytes plaintext{sendspin::server::MESSAGE_ID_FRAGMENT,
                        FRAGMENT_FLAG_FIRST | FRAGMENT_FLAG_LAST,
                        sendspin::server::MESSAGE_ID_JSON};
        plaintext.insert(plaintext.end(), text.begin(), text.end());
        return plaintext;
    };
    ASSERT_EQ(reassembler.accept(whole("{\"longer\":\"" + secret + "\"}")),
              Reassembler::Outcome::MESSAGE);
    ASSERT_EQ(reassembler.accept(whole("{}")), Reassembler::Outcome::MESSAGE);
    EXPECT_TRUE(holds_to_capacity(reassembler.buffer_, secret));
    reassembler.wipe();
    EXPECT_FALSE(holds_to_capacity(reassembler.buffer_, secret));
}

}  // namespace
