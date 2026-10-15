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

/// @file test_noise_initiator.cpp
/// @brief The cipher suite names of spec connection.md "Cipher Suites", as client/init carries them

#include "noise_types.h"
#include <gtest/gtest.h>

#include <optional>

namespace {

using sendspin::server::CipherSuite;

TEST(CipherSuiteNames, MapTheClientInitSuiteNames) {
    struct Row {
        const char* name;
        std::optional<CipherSuite> suite;
    };
    const Row rows[] = {
        {"25519_ChaChaPoly_SHA256", CipherSuite::CHACHAPOLY_SHA256},  // Control:
        {"25519_AESGCM_SHA256", CipherSuite::AESGCM_SHA256},
        {"Noise_KKpsk2_25519_ChaChaPoly_SHA256", std::nullopt},
        {"25519_chachapoly_sha256", std::nullopt},
        {"", std::nullopt},
    };
    for (const Row& row : rows) {
        SCOPED_TRACE(row.name);
        const std::optional<CipherSuite> suite = sendspin::server::cipher_suite_from_wire(row.name);
        EXPECT_EQ(suite, row.suite);
        if (suite) {
            EXPECT_EQ(sendspin::server::to_wire(*suite), row.name);
        }
    }
}

}  // namespace
