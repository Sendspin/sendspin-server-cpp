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

/// @file test_pairing_crypto.cpp
/// @brief The pairing crypto copied from sendspin-cpp (src/ORIGIN.md): CPace-X25519-SHA512, the
/// pairing code's commitment, digest and presentations, pairing tokens and PSK wrapping, against
/// known answers.
///
/// Vectors: the spec's own (pairing.md "Pairing PSK Flow" and "QR-code emission" tokens), the
/// draft-irtf-cfrg-cpace-21 testvectors.json G_25519 and X25519_points sets, and the known
/// answers of sendspin-cpp's tests/test_cpace.cpp, test_pairing_code.cpp, test_psk_wrap.cpp at
/// commit 91038b72892596b9ec0cccfec1b9e6ebc58ba148, which that repository derived from the
/// aiosendspin Python reference.
///
/// White-box: the fixed-scalar CPace tests overwrite CPace::scalar_ and public_share_, since
/// start() samples its scalar (see tests/CMakeLists.txt).

#include "crypto/cpace.h"
#include "crypto/crypto.h"
#include "crypto/field25519.h"
#include "crypto/pairing_code.h"
#include "crypto/pairing_token.h"
#include "crypto/psk_wrap.h"
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

using sendspin::server::CPace;
using sendspin::server::CPACE_SHARE_SIZE;
using sendspin::server::CPACE_TAG_SIZE;
using sendspin::server::CPaceRole;

std::string hex(const uint8_t* data, size_t size) {
    std::string out;
    for (size_t i = 0; i < size; ++i) {
        char byte[3];
        std::snprintf(byte, sizeof(byte), "%02x", data[i]);
        out += byte;
    }
    return out;
}

template <typename Bytes>
std::string hex(const Bytes& bytes) {
    return hex(bytes.data(), bytes.size());
}

std::vector<uint8_t> from_hex(std::string_view text) {
    std::vector<uint8_t> out;
    for (size_t i = 0; i + 1 < text.size(); i += 2) {
        out.push_back(static_cast<uint8_t>(std::stoi(std::string(text.substr(i, 2)), nullptr, 16)));
    }
    return out;
}

template <size_t N>
std::array<uint8_t, N> from_hex_array(std::string_view text) {
    const std::vector<uint8_t> bytes = from_hex(text);
    std::array<uint8_t, N> out{};
    std::memcpy(out.data(), bytes.data(), std::min(N, bytes.size()));
    return out;
}

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

// ==============================================================================
// CPace building blocks (sendspin-cpp test_cpace.cpp)
// ==============================================================================

TEST(CPacePrimitives, EncodesLengthsAndConcatenations) {
    struct Row {
        const char* name;
        std::vector<uint8_t> data;
        std::string expected;
    };
    std::string long_expected = "8001";
    for (int i = 0; i < 128; ++i) {
        long_expected += "78";
    }
    const Row rows[] = {
        {"empty", {}, "00"},
        {"one byte", {0x41}, "0141"},
        {"the DSI", bytes_of("CPace255"), "084350616365323535"},
        {"128 bytes take two length bytes", std::vector<uint8_t>(128, 0x78), long_expected},
    };
    for (const Row& row : rows) {
        SCOPED_TRACE(row.name);
        EXPECT_EQ(hex(sendspin::server::cpace_prepend_len(row.data.data(), row.data.size())),
                  row.expected);
    }
    const std::vector<uint8_t> hello = bytes_of("hello");
    const std::vector<uint8_t> world = bytes_of("world");
    EXPECT_EQ(hex(sendspin::server::cpace_lv_cat({{hello.data(), 5}, {world.data(), 5}})),
              "0568656c6c6f05776f726c64");
}

TEST(CPacePrimitives, BuildsTheGeneratorStringAndGenerator) {
    const std::vector<uint8_t> prs = bytes_of("1234");
    const std::vector<uint8_t> sid = bytes_of("test-session-id-1234567890123456");
    const std::vector<uint8_t> string = sendspin::server::cpace_generator_string(
        prs.data(), prs.size(), nullptr, 0, sid.data(), sid.size());
    EXPECT_EQ(hex(string), "0843506163653235350431323334710000000000000000000000000000000000"
                           "0000000000000000000000000000000000000000000000000000000000000000"
                           "0000000000000000000000000000000000000000000000000000000000000000"
                           "0000000000000000000000000000000000000000000000000000000000000000"
                           "0020746573742d73657373696f6e2d69642d3132333435363738393031323334"
                           "3536");
    EXPECT_EQ(hex(sendspin::server::cpace_calculate_generator(prs.data(), prs.size(), nullptr, 0,
                                                              sid.data(), sid.size())),
              "8fb13a51e2f4d62c9d5d6ae2390b41009cd26e6535d28248df9b5984a7a1de70");
}

TEST(CPacePrimitives, DecodesUWithTheTopBitCleared) {
    std::array<uint8_t, 32> value{};
    for (size_t i = 0; i < 31; ++i) {
        value[i] = static_cast<uint8_t>(i);
    }
    value[31] = 0xff;
    const std::array<uint8_t, 32> decoded =
        sendspin::server::cpace_decode_u(value.data(), value.size());
    EXPECT_EQ(decoded[31], 0x7f);
    EXPECT_TRUE(std::equal(decoded.begin(), decoded.begin() + 31, value.begin()));
}

TEST(CPacePrimitives, MapsWithElligator2) {
    struct Row {
        const char* r_le;
        const char* point;
    };
    const Row rows[] = {
        {"efcdab9078563412efcdab9078563412efcdab9078563412efcdab9078563412",
         "406682120bd67c0502cb7afed2849918c58c0bc36e1cbdbc2184d407f5a9db19"},
        {"0000000000000000000000000000000000000000000000000000000000000000",
         "0000000000000000000000000000000000000000000000000000000000000000"},
        {"0100000000000000000000000000000000000000000000000000000000000000",
         "9cdb525555555555555555555555555555555555555555555555555555555555"},
        {"ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff",
         "6edfd66733be3f52309983805b9fcdf8fe48c1640e026e7d36e3fb2305933908"},
        {"dededededededededededededededededededededededededededededededede",
         "42d8f3889968f3caa814ca11b2bc880b35c9c0897f193a1aec578043a43e3948"},
        {"0102030405060708010203040506070801020304050607080102030405060708",
         "bde03792c4903abf825e98667c9d4c4945665581b0e2306ad19e10c564e04773"},
        {"aa55aa55aa55aa55aa55aa55aa55aa55aa55aa55aa55aa55aa55aa55aa55aa55",
         "803fcc2dbcf575f784907707db8080a2657230a71c9caa4a87a57beccfdb323f"},
    };
    for (const Row& row : rows) {
        SCOPED_TRACE(row.r_le);
        EXPECT_EQ(hex(sendspin::server::cpace_elligator2(from_hex_array<32>(row.r_le))), row.point);
    }
}

// field25519: a fold-back carry out of limb 0 ripples into the higher limbs, and the final
// reduction subtracts p twice, since 2^256 = 2p + 38
TEST(FieldArithmetic, ReducesCarriesAndResultsCanonically) {
    namespace fp = sendspin::server::field25519;
    constexpr uint64_t ONES = 0xFFFFFFFFFFFFFFFFULL;
    const fp::Fp all_ones = {{ONES, ONES, ONES, ONES}};
    const fp::Fp all_ones_minus_one = {{ONES - 1, ONES, ONES, ONES}};
    EXPECT_EQ(hex(fp::fp_to_le(fp::fp_mul(all_ones, all_ones))),
              "5905000000000000000000000000000000000000000000000000000000000000");
    EXPECT_EQ(hex(fp::fp_to_le(fp::fp_mul(all_ones, all_ones_minus_one))),
              "3405000000000000000000000000000000000000000000000000000000000000");
    const fp::Fp a = {{0x2DE0BF53061137D5ULL, ONES, ONES, 0x7FFFFFFFFFFFFFFFULL}};
    EXPECT_EQ(hex(fp::fp_to_le(fp::fp_scale(a, 486662ULL))),
              "5d178d0000000000a9e7f9ffffffffffffffffffffffffffffffffffffffff7f");
    const auto x =
        from_hex_array<32>("5171d8daf80eb7e4b7c102a6f8c63218d0bb218db264c644bfd6fd97c75f6312");
    const auto inverse =
        from_hex_array<32>("a5aa1eb682b5de7c2d01e7936be026ea2db1f101dd883d3724f04d2461516960");
    const std::string one = "0100000000000000000000000000000000000000000000000000000000000000";
    EXPECT_EQ(
        hex(fp::fp_to_le(fp::fp_mul(fp::fp_from_le(x.data()), fp::fp_from_le(inverse.data())))),
        one);
    for (uint64_t seed = 1; seed <= 200; ++seed) {
        std::array<uint8_t, 32> bytes{};
        uint64_t s = seed * 0x9E3779B97F4A7C15ULL;
        for (uint8_t& byte : bytes) {
            s ^= s >> 30;
            s *= 0xBF58476D1CE4E5B9ULL;
            byte = static_cast<uint8_t>(s >> 24);
        }
        bytes[31] &= 0x7F;
        const fp::Fp value = fp::fp_from_le(bytes.data());
        EXPECT_EQ(hex(fp::fp_to_le(fp::fp_mul(value, fp::fp_inv(value)))), one) << seed;
    }
}

// RFC 7748 section 5.2's two test vectors; the second's u-coordinate has its top bit set, which
// decodeUCoordinate masks
TEST(CPacePrimitives, MatchesTheRfc7748Vectors) {
    struct Row {
        const char* scalar;
        const char* u;
        const char* out;
    };
    const Row rows[] = {
        {"a546e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449ac4",
         "e6db6867583030db3594c1a424b15f7c726624ec26b3353b10a903a6d0ab1c4c",
         "c3da55379de9c6908e94ea4df28d084f32eccf03491c71f754b4075577a28552"},
        {"4b66e9d4d1b4673c5ad22691957d6af5c11b6421e0ea01d42ca4169e7918ba0d",
         "e5210f12786811d3f4b7959d0538ae2c31dbe7106fc03c3efc4cd549c715a493",
         "95cbde9476e8907d7aade45cb4b873f88b595a68799fa152e6f8f7647aac7957"},
    };
    for (const Row& row : rows) {
        SCOPED_TRACE(row.out);
        const auto scalar = from_hex_array<32>(row.scalar);
        const auto u = from_hex_array<32>(row.u);
        std::array<uint8_t, 32> out{};
        ASSERT_TRUE(sendspin::server::x25519_scalar_mult(scalar.data(), u.data(), out.data()));
        EXPECT_EQ(hex(out), row.out);
    }
}

TEST(CPacePrimitives, MultipliesWithAClampedScalar) {
    std::array<uint8_t, 32> scalar{};
    for (size_t i = 0; i < scalar.size(); ++i) {
        scalar[i] = static_cast<uint8_t>(i);
    }
    const std::array<uint8_t, 32> base{9};
    std::array<uint8_t, 32> out{};
    ASSERT_TRUE(sendspin::server::x25519_scalar_mult(scalar.data(), base.data(), out.data()));
    EXPECT_EQ(hex(out), "8f40c5adb68f25624ae5b214ea767a6ec94d829d3d7b5e1ad1ba6f3e2138285f");
}

// ==============================================================================
// CPace runs
// ==============================================================================

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

// draft-irtf-cfrg-cpace-21 testvectors.json, X25519_points: shares that yield K = 0 are refused,
// and so are a share of the wrong size, a second derive() and a derive() before start(). The
// draft's five further points with bit 255 set are weak only for an X25519 that does not mask that
// bit; RFC 7748 decodeUCoordinate masks it, so they are ordinary points here.
TEST(CPaceRun, RefusesInvalidShares) {
    struct Row {
        const char* name;
        std::vector<uint8_t> share;
        bool accepted;
    };
    std::vector<Row> rows;
    for (const char* point : {
             "0000000000000000000000000000000000000000000000000000000000000000",
             "0100000000000000000000000000000000000000000000000000000000000000",
             "ecffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f",
             "e0eb7a7c3b41b8ae1656e3faf19fc46ada098deb9c32b1fd866205165f49b800",
             "5f9c95bca3508c24b1d0b1559c83ef5b04445cc4581c8e86d8224eddd09f1157",
             "edffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f",
             "eeffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f",
         }) {
        rows.push_back({point, from_hex(point), false});
    }
    for (const char* point : {
             "daffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff",
             "dbffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff",
             "d9ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff",
             "cdeb7a7c3b41b8ae1656e3faf19fc46ada098deb9c32b1fd866205165f49b880",
             "4c9c95bca3508c24b1d0b1559c83ef5b04445cc4581c8e86d8224eddd09f11d7",
         }) {
        rows.push_back({point, from_hex(point), true});
    }
    rows.push_back({"16 bytes", std::vector<uint8_t>(16, 0x09), false});
    rows.push_back({"Control: a valid share",
                    from_hex("248cccf6d5cdc3646f0ad593f9e6cef4e69d4945f8372e623512ecea32185623"),
                    true});
    const std::vector<uint8_t> prs = bytes_of("pinpin");
    const std::vector<uint8_t> sid(32, 0xAA);
    for (const Row& row : rows) {
        SCOPED_TRACE(row.name);
        CPace side;
        ASSERT_TRUE(side.start(CPaceRole::INITIATOR, prs, sid, {}, {}, {}));
        EXPECT_EQ(side.derive(row.share.data(), row.share.size()), row.accepted);
    }

    CPace peer;
    ASSERT_TRUE(peer.start(CPaceRole::INITIATOR, prs, sid, {}, {}, {}));
    CPace unstarted;
    EXPECT_FALSE(unstarted.derive(peer.public_share().data(), CPACE_SHARE_SIZE));
    EXPECT_FALSE(unstarted.tag());
    CPace twice;
    ASSERT_TRUE(twice.start(CPaceRole::RESPONDER, prs, sid, {}, {}, {}));
    ASSERT_TRUE(twice.derive(peer.public_share().data(), CPACE_SHARE_SIZE));
    EXPECT_FALSE(twice.derive(peer.public_share().data(), CPACE_SHARE_SIZE));
}

// Sampled scalars: the same code makes tags that verify each way, a different code tags that do
// not, and a tag of the wrong length never verifies
TEST(CPaceRun, VerifiesOnlyTheSameCode) {
    struct Row {
        const char* name;
        const char* code_b;
        size_t tag_size;
        bool verifies;
    };
    const Row rows[] = {
        {"Control: the same code", "123456", CPACE_TAG_SIZE, true},
        {"another code", "123457", CPACE_TAG_SIZE, false},
        {"a short tag", "123456", CPACE_TAG_SIZE - 1, false},
    };
    const std::vector<uint8_t> sid(32, 0xBB);
    for (const Row& row : rows) {
        SCOPED_TRACE(row.name);
        CPace a;
        CPace b;
        ASSERT_TRUE(a.start(CPaceRole::INITIATOR, bytes_of("123456"), sid, {}, bytes_of("server"),
                            bytes_of("client")));
        ASSERT_TRUE(b.start(CPaceRole::RESPONDER, bytes_of(row.code_b), sid, {}, bytes_of("client"),
                            bytes_of("server")));
        ASSERT_TRUE(a.derive(b.public_share().data(), CPACE_SHARE_SIZE));
        ASSERT_TRUE(b.derive(a.public_share().data(), CPACE_SHARE_SIZE));
        EXPECT_EQ(b.verify(a.tag()->data(), row.tag_size), row.verifies);
        EXPECT_EQ(a.verify(b.tag()->data(), row.tag_size), row.verifies);
    }
}

// ==============================================================================
// The pairing code (sendspin-cpp test_pairing_code.cpp)
// ==============================================================================

struct DigestInputs {
    std::array<uint8_t, 32> h{};
    std::array<uint8_t, 32> nonce_a{};
    std::array<uint8_t, 32> nonce_b{};
};

DigestInputs counting_inputs() {
    DigestInputs in;
    in.h.fill(0xAB);
    for (size_t i = 0; i < 32; ++i) {
        in.nonce_a[i] = static_cast<uint8_t>(i);
        in.nonce_b[i] = static_cast<uint8_t>(i + 32);
    }
    return in;
}

std::array<uint8_t, 32> digest_of(const DigestInputs& in) {
    return *sendspin::server::pairing_code_digest(in.h.data(), in.h.size(), in.nonce_a.data(),
                                                  in.nonce_a.size(), in.nonce_b.data(),
                                                  in.nonce_b.size());
}

TEST(PairingCode, MatchesTheKnownAnswers) {
    std::array<uint8_t, 32> nonce{};
    for (size_t i = 0; i < nonce.size(); ++i) {
        nonce[i] = static_cast<uint8_t>(i);
    }
    const auto commitment = sendspin::server::pairing_code_commit(nonce.data(), nonce.size());
    ASSERT_TRUE(commitment);
    EXPECT_EQ(hex(*commitment), "ea08c0aee3c421ace702f31591b3d213e8c371a8a8e3b0be3fd405ed841755a3");
    EXPECT_TRUE(sendspin::server::pairing_code_verify_commit(
        nonce.data(), nonce.size(), commitment->data(), commitment->size()));
    nonce[0] ^= 0xFF;
    EXPECT_FALSE(sendspin::server::pairing_code_verify_commit(
        nonce.data(), nonce.size(), commitment->data(), commitment->size()));

    const std::array<uint8_t, 32> digest = digest_of(counting_inputs());
    EXPECT_EQ(hex(digest), "ff533978a64a628c9c3bd5de32338fa37cb79825e85f544433136c13e3e7edf7");
    EXPECT_EQ(sendspin::server::pairing_code_digits(digest), "564919");
    EXPECT_EQ(hex(sendspin::server::pairing_code_qr_bytes(digest)),
              "ff533978a64a628c9c3bd5de32338fa37cb79825e85f5444");
    const std::array<uint8_t, 32> zero_digest = digest_of(DigestInputs{});
    EXPECT_EQ(hex(zero_digest), "ecdf16f52ca727f063d6d97ba61b5111948f00256a98520891952fba99caf882");
    EXPECT_EQ(sendspin::server::pairing_code_digits(zero_digest), "593986");

    // Six digits, zero-padded, from the whole digest as one big-endian integer
    std::array<uint8_t, 32> small{};
    small[31] = 5;
    EXPECT_EQ(sendspin::server::pairing_code_digits(small), "000005");
    std::array<uint8_t, 32> high{};
    high[0] = 1;
    EXPECT_EQ(sendspin::server::pairing_code_digits(high), "662656");

    // The nonces' order is part of the digest
    DigestInputs swapped = counting_inputs();
    std::swap(swapped.nonce_a, swapped.nonce_b);
    EXPECT_NE(digest_of(swapped), digest);
}

TEST(PairingCode, RefusesInputsOfTheWrongLength) {
    struct Row {
        const char* name;
        size_t hash;
        size_t nonce_a;
        size_t nonce_b;
        bool accepted;
    };
    const Row rows[] = {
        {"Control: every length right", 32, 32, 32, true},
        {"a short hash", 16, 32, 32, false},
        {"a short nonce_A", 32, 16, 32, false},
        {"a short nonce_B", 32, 32, 16, false},
    };
    const std::array<uint8_t, 32> bytes{};
    for (const Row& row : rows) {
        SCOPED_TRACE(row.name);
        EXPECT_EQ(sendspin::server::pairing_code_digest(bytes.data(), row.hash, bytes.data(),
                                                        row.nonce_a, bytes.data(), row.nonce_b)
                      .has_value(),
                  row.accepted);
        EXPECT_EQ(sendspin::server::pairing_code_commit(bytes.data(), row.nonce_a).has_value(),
                  row.nonce_a == 32);
    }
}

// ==============================================================================
// Pairing tokens (pairing.md "Pairing Token")
// ==============================================================================

constexpr std::string_view SPEC_PSK_TOKEN =
    "SP:0AAAQEAYEAUDAOCAJBIFQYDIOB4IBCEQTCQKRMFYYDENBWHA5DYP6BYPC4PSOLZXH5DU6V97M5XXO74HR6LZ7"
    "J5PW674PT6X37T6757Y";
constexpr std::string_view SPEC_CODE_TOKEN = "SP:14DQ6FY7E4XTOP9HJ5LV6Z3PO57YPD4XT6T97N5Y";

sendspin::server::PublicKey spec_client_key() {
    sendspin::server::PublicKey key{};
    for (size_t i = 0; i < key.size(); ++i) {
        key[i] = static_cast<uint8_t>(i);
    }
    return key;
}

sendspin::server::Psk spec_pairing_psk() {
    sendspin::server::Psk psk{};
    for (size_t i = 0; i < psk.size(); ++i) {
        psk[i] = static_cast<uint8_t>(0xE0 + i);
    }
    return psk;
}

std::array<uint8_t, sendspin::server::QR_PAIRING_CODE_SIZE> spec_qr_code() {
    std::array<uint8_t, sendspin::server::QR_PAIRING_CODE_SIZE> code{};
    for (size_t i = 0; i < code.size(); ++i) {
        code[i] = static_cast<uint8_t>(0xE0 + i);
    }
    return code;
}

// pairing.md "Pairing PSK Flow" and "QR-code emission": the spec's two tokens, verbatim
TEST(PairingToken, FormatsTheSpecVectors) {
    EXPECT_EQ(sendspin::server::format_pairing_token(spec_client_key(), spec_pairing_psk()),
              SPEC_PSK_TOKEN);
    EXPECT_EQ(sendspin::server::format_pairing_code_token(spec_qr_code()), SPEC_CODE_TOKEN);
}

// pairing.md "Pairing Token": an operator's copy is trimmed and read without regard to case, the
// "SP:" prefix may be left off, and '9' reads as '2'; the version must match and the payload be
// whole
TEST(PairingToken, DecodesWhatAnOperatorTypes) {
    struct Row {
        const char* name;
        std::string input;
        bool decodes;
    };
    std::string lower(SPEC_PSK_TOKEN);
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](char c) { return static_cast<char>(std::tolower(c)); });
    std::string with_two(SPEC_PSK_TOKEN);
    std::replace(with_two.begin(), with_two.end(), '9', '2');
    const Row rows[] = {
        {"Control: the spec token", std::string(SPEC_PSK_TOKEN), true},
        {"lower case", lower, true},
        {"surrounding whitespace", " \t" + std::string(SPEC_PSK_TOKEN) + "\n", true},
        {"without SP:", std::string(SPEC_PSK_TOKEN.substr(3)), true},
        {"2 where the token shows 9", with_two, true},
        {"the version-1 token", std::string(SPEC_CODE_TOKEN), false},
        {"a character outside the alphabet", std::string(SPEC_PSK_TOKEN) + "1", false},
        {"one character short", std::string(SPEC_PSK_TOKEN.substr(0, SPEC_PSK_TOKEN.size() - 1)),
         false},
        {"eight characters short", std::string(SPEC_PSK_TOKEN.substr(0, SPEC_PSK_TOKEN.size() - 8)),
         false},
        {"empty", "", false},
    };
    for (const Row& row : rows) {
        SCOPED_TRACE(row.name);
        const std::optional<sendspin::server::PairingPskToken> token =
            sendspin::server::decode_pairing_psk_token(row.input);
        ASSERT_EQ(token.has_value(), row.decodes);
        if (token) {
            EXPECT_EQ(token->client_key, spec_client_key());
            EXPECT_EQ(token->pairing_psk, spec_pairing_psk());
        }
    }
    const std::optional<sendspin::server::PairingCodeToken> code =
        sendspin::server::decode_pairing_code_token(" sp:" +
                                                    std::string(SPEC_CODE_TOKEN.substr(3)));
    ASSERT_TRUE(code);
    EXPECT_EQ(code->code, spec_qr_code());
    EXPECT_FALSE(sendspin::server::decode_pairing_code_token(SPEC_PSK_TOKEN));
}

// ==============================================================================
// PSK wrapping (sendspin-cpp test_psk_wrap.cpp; pairing.md "Wrapping")
// ==============================================================================

std::vector<uint8_t> wrap_sid(uint32_t round) {
    std::vector<uint8_t> sid = bytes_of("sendspin-pair-pake-v1");
    for (uint8_t i = 0; i < 32; ++i) {
        sid.push_back(i);
    }
    for (const uint32_t counter : {uint32_t{1}, round}) {
        for (int shift = 24; shift >= 0; shift -= 8) {
            sid.push_back(static_cast<uint8_t>((counter >> shift) & 0xFF));
        }
    }
    return sid;
}

std::array<uint8_t, sendspin::server::CPACE_ISK_SIZE> counting_isk() {
    std::array<uint8_t, sendspin::server::CPACE_ISK_SIZE> isk{};
    for (size_t i = 0; i < isk.size(); ++i) {
        isk[i] = static_cast<uint8_t>(i);
    }
    return isk;
}

TEST(PskWrap, DerivesTheKnownWrapKeys) {
    EXPECT_EQ(hex(*sendspin::server::derive_wrap_key(sendspin::server::PSK_WRAP_LABEL, wrap_sid(1),
                                                     counting_isk())),
              "79069f5664638a5263d07188893fb3e38dd1983b603a100e5dccb28ec327967a");
    EXPECT_EQ(hex(*sendspin::server::derive_wrap_key(sendspin::server::NONCE_WRAP_LABEL,
                                                     wrap_sid(1), counting_isk())),
              "7172ccfe4f3d6d71bddb9731344518bbe4b41a2c676a0832f1426056747c31c9");
}

// A value wrapped under one (label, AEAD, sid) unwraps under the same, in either suite's AEAD,
// and under no other
TEST(PskWrap, UnwrapsOnlyUnderTheSameInputs) {
    struct Row {
        const char* name;
        const char* aead;
        std::string_view unwrap_label;
        const char* unwrap_aead;
        uint32_t unwrap_round;
        bool unwraps;
    };
    using sendspin::server::AEAD_AESGCM;
    using sendspin::server::AEAD_CHACHAPOLY;
    using sendspin::server::NONCE_WRAP_LABEL;
    using sendspin::server::PSK_WRAP_LABEL;
    const Row rows[] = {
        {"Control: ChaChaPoly", AEAD_CHACHAPOLY, PSK_WRAP_LABEL, AEAD_CHACHAPOLY, 1, true},
        {"Control: AESGCM", AEAD_AESGCM, PSK_WRAP_LABEL, AEAD_AESGCM, 1, true},
        {"the other label", AEAD_CHACHAPOLY, NONCE_WRAP_LABEL, AEAD_CHACHAPOLY, 1, false},
        {"the other AEAD", AEAD_CHACHAPOLY, PSK_WRAP_LABEL, AEAD_AESGCM, 1, false},
        {"another round's sid", AEAD_CHACHAPOLY, PSK_WRAP_LABEL, AEAD_CHACHAPOLY, 2, false},
    };
    std::array<uint8_t, 32> value{};
    for (size_t i = 0; i < value.size(); ++i) {
        value[i] = static_cast<uint8_t>(0xA0 + i);
    }
    for (const Row& row : rows) {
        SCOPED_TRACE(row.name);
        const std::optional<sendspin::server::WrappedValue> wrapped = sendspin::server::wrap_value(
            PSK_WRAP_LABEL, row.aead, wrap_sid(1), counting_isk(), value);
        ASSERT_TRUE(wrapped);
        const auto unwrapped =
            sendspin::server::unwrap_value(row.unwrap_label, row.unwrap_aead,
                                           wrap_sid(row.unwrap_round), counting_isk(), *wrapped);
        ASSERT_EQ(unwrapped.has_value(), row.unwraps);
        if (unwrapped) {
            EXPECT_EQ(*unwrapped, value);
        }
    }
}

}  // namespace
