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

/// @file test_pairing_crypto_internals.cpp
/// @brief CPace runs with fixed scalars against sendspin-cpp's known answers and the
/// draft-irtf-cfrg-cpace-21 G_25519 vectors. Compiled with -fno-access-control
/// (tests/CMakeLists.txt): the scalar is overwritten, since start() samples it from the CSPRNG.

#include "crypto/cpace.h"
#include "hex.h"
#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace {

using sendspin::server::CPace;
using sendspin::server::CPACE_SHARE_SIZE;
using sendspin::server::CPACE_TAG_SIZE;
using sendspin::server::CPaceRole;
using sendspin::server::test::from_hex;
using sendspin::server::test::from_hex_array;
using sendspin::server::test::hex;

std::vector<uint8_t> bytes_of(std::string_view text) {
    return {text.begin(), text.end()};
}

/// Replaces a started CPace's scalar with a fixed one and recomputes its share, leaving it as if
/// start() had sampled that scalar; gen is the generator of the side's (prs, ci, sid)
bool force_scalar(CPace& side, const std::array<uint8_t, 32>& scalar,
                  const std::array<uint8_t, 32>& gen) {
    side.scalar_ = scalar;
    return sendspin::server::x25519_scalar_mult(side.scalar_.data(), gen.data(),
                                                side.public_share_.data());
}

// sendspin-cpp test_cpace.cpp FixedScalarDrivesProductionDeriveAndTag: the shares, ISK and tags
// of a run with fixed scalars, through derive(), isk(), tag() and verify()
TEST(CPaceRun, MatchesTheFixedScalarKnownAnswers) {
    const std::vector<uint8_t> prs = bytes_of("1234");
    const std::vector<uint8_t> sid = bytes_of("test-session-id-1234567890123456");
    const std::vector<uint8_t> none;
    CPace a;
    CPace b;
    ASSERT_TRUE(a.start(CPaceRole::INITIATOR, prs, sid, none, none, none));
    ASSERT_TRUE(b.start(CPaceRole::RESPONDER, prs, sid, none, none, none));
    const auto gen = sendspin::server::cpace_calculate_generator(prs.data(), prs.size(), nullptr, 0,
                                                                 sid.data(), sid.size());
    std::array<uint8_t, 32> scalar_a{};
    std::array<uint8_t, 32> scalar_b{};
    scalar_a.fill(0x10);
    scalar_b.fill(0x20);
    ASSERT_TRUE(force_scalar(a, scalar_a, gen));
    ASSERT_TRUE(force_scalar(b, scalar_b, gen));
    EXPECT_EQ(hex(a.public_share()),
              "57551a92c995ea9b792f4e89a18c34459491c1484deddbb1498c29b499286c0f");
    EXPECT_EQ(hex(b.public_share()),
              "a057a00be47920856f4ae0574971590e9fb0b5d97b89dba12237957dd8ec577f");
    ASSERT_TRUE(a.derive(b.public_share().data(), CPACE_SHARE_SIZE));
    ASSERT_TRUE(b.derive(a.public_share().data(), CPACE_SHARE_SIZE));
    const std::string isk = "fab2965a5e084943c7d286b66a998daf6af5e370040aec16f544f3e8dbc9ba0f"
                            "87149a1a88bc73f0f5c3f9a8fa2cfc78ee88a53941614c69b68c827665f53d3b";
    EXPECT_EQ(hex(*a.isk()), isk);
    EXPECT_EQ(hex(*b.isk()), isk);
    const std::string ta = "fe47abf1649b3f91ce6e3b38870ac3947a209a1dc1dab2c88db77791e9432426"
                           "111e9555562a6f7a41a14079de95617f1396d82c8066f19c8aa486e7f70b8ef0";
    const std::string tb = "806b304d1cc3f232ed45afd6d2dc7e08ffd69aeb57ca3c78de75c5d280024a6c"
                           "263ec2c6da7414899a91d085b826d63ca320bf50f7b49d985a3db00c8c891e45";
    EXPECT_EQ(hex(*a.tag()), ta);
    EXPECT_EQ(hex(*b.tag()), tb);
    EXPECT_TRUE(b.verify(from_hex(ta).data(), CPACE_TAG_SIZE));
    EXPECT_TRUE(a.verify(from_hex(tb).data(), CPACE_TAG_SIZE));
}

// draft-irtf-cfrg-cpace-21 testvectors.json, G_25519: the generator, shares, K and ISK_IR
TEST(CPaceRun, MatchesTheDraft21Vectors) {
    const std::vector<uint8_t> prs = from_hex("50617373776f7264");
    const std::vector<uint8_t> ci = from_hex("0b415f696e69746961746f720b425f726573706f6e646572");
    const std::vector<uint8_t> sid = from_hex("7e4b4791d6a8ef019b936c79fb7f2c57");
    const std::vector<uint8_t> ada = from_hex("414461");
    const std::vector<uint8_t> adb = from_hex("414462");
    const auto gen = sendspin::server::cpace_calculate_generator(prs.data(), prs.size(), ci.data(),
                                                                 ci.size(), sid.data(), sid.size());
    EXPECT_EQ(hex(gen), "d04bf6d41f6a289632a2e929fa29bebd51092512a7829fdde7d314b62f05a73f");
    CPace a;
    CPace b;
    ASSERT_TRUE(a.start(CPaceRole::INITIATOR, prs, sid, ci, ada, adb));
    ASSERT_TRUE(b.start(CPaceRole::RESPONDER, prs, sid, ci, adb, ada));
    const auto scalar_a =
        from_hex_array<32>("21b4f4bd9e64ed355c3eb676a28ebedaf6d8f17bdc365995b319097153044080");
    const auto scalar_b =
        from_hex_array<32>("848b0779ff415f0af4ea14df9dd1d3c29ac41d836c7808896c4eba19c51ac40a");
    ASSERT_TRUE(force_scalar(a, scalar_a, gen));
    ASSERT_TRUE(force_scalar(b, scalar_b, gen));
    EXPECT_EQ(hex(a.public_share()),
              "1d13c89278cdadd826f6d8d7f887701430f8380ddc17611cdd6dc989ce0c9f32");
    EXPECT_EQ(hex(b.public_share()),
              "248cccf6d5cdc3646f0ad593f9e6cef4e69d4945f8372e623512ecea32185623");
    std::array<uint8_t, 32> k{};
    ASSERT_TRUE(
        sendspin::server::x25519_scalar_mult(scalar_a.data(), b.public_share().data(), k.data()));
    EXPECT_EQ(hex(k), "5b067effbdc0b2a0e1d907b21ebb25cfedb96a852179a847c37e43ee71322c6b");
    ASSERT_TRUE(a.derive(b.public_share().data(), CPACE_SHARE_SIZE));
    ASSERT_TRUE(b.derive(a.public_share().data(), CPACE_SHARE_SIZE));
    const std::string isk = "6e19b875f7a561d6b3ca3dbb9ef42ac55de3e717881018204b8922b4d5e53bb2"
                            "aa82c300bea7b65d2b671da71922ddf6472301b79bc270adfa8bf413285f2263";
    EXPECT_EQ(hex(*a.isk()), isk);
    EXPECT_EQ(hex(*b.isk()), isk);
}

}  // namespace
