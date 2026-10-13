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

/// @file test_arduinojson.cpp
/// @brief ArduinoJson builds with the configuration the library uses (64-bit integers, std::string)

#include <ArduinoJson.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <string>

static_assert(ARDUINOJSON_USE_LONG_LONG == 1 && ARDUINOJSON_ENABLE_STD_STRING == 1,
              "the ArduinoJson target must carry the library's configuration");

namespace {

TEST(ArduinoJson, RoundTripsA64BitTimestamp) {
    constexpr int64_t SERVER_RECEIVED_US = 1'700'000'000'123'456;
    JsonDocument out;
    out["type"] = "server/time";
    out["payload"]["server_received"] = SERVER_RECEIVED_US;
    std::string text;
    serializeJson(out, text);

    JsonDocument in;
    ASSERT_EQ(deserializeJson(in, text), DeserializationError::Ok);
    EXPECT_EQ(in["type"].as<std::string>(), "server/time");
    ASSERT_TRUE(in["payload"]["server_received"].is<int64_t>());
    EXPECT_EQ(in["payload"]["server_received"].as<int64_t>(), SERVER_RECEIVED_US);
}

}  // namespace
