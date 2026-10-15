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

/// @file raw_roles.h
/// @brief For tests that drive the roles with the raw test client: the client/hello, client/state
/// and controller command it sends, the dial through its client/hello, and readers of the JSON
/// messages it receives

#pragma once

#include "raw_client.h"
#include <ArduinoJson.h>
#include <gtest/gtest.h>

#include <sendspin/server/server.h>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace sendspin::server::test {

/// A client/hello listing roles, with the player's support object when it lists player@v1
inline std::string hello(const std::string& name, const std::vector<std::string>& roles,
                         bool unpaired) {
    JsonDocument doc;
    doc["type"] = "client/hello";
    JsonObject payload = doc["payload"].to<JsonObject>();
    payload["name"] = name;
    JsonArray listed = payload["supported_roles"].to<JsonArray>();
    for (const std::string& role : roles) {
        listed.add(role);
    }
    if (std::find(roles.begin(), roles.end(), "player@v1") != roles.end()) {
        JsonObject format = payload["player@v1_support"]["supported_formats"].add<JsonObject>();
        format["codec"] = "pcm";
        format["sample_rate"] = 48000;
        format["channels"] = 2;
        format["bit_depth"] = 16;
        payload["player@v1_support"]["buffer_capacity"] = 200000;
    }
    payload["supported_pair_methods"]["pairing_psk"].to<JsonObject>();
    payload["unpaired_access"]["enabled"] = unpaired;
    std::string out;
    serializeJson(doc, out);
    return out;
}

/// A client/state with a player object offering commands, reporting volume and muted when set
inline std::string player_state(bool available, const std::vector<std::string>& commands,
                                std::optional<uint32_t> volume, std::optional<bool> muted) {
    JsonDocument doc;
    doc["type"] = "client/state";
    doc["payload"]["available"] = available;
    JsonObject player = doc["payload"]["player"].to<JsonObject>();
    player["output_delay_ms"] = 0;
    player["required_lead_time_ms"] = 0;
    player["min_buffer_ms"] = 0;
    JsonArray offered = player["supported_commands"].to<JsonArray>();
    for (const std::string& command : commands) {
        offered.add(command);
    }
    if (volume) {
        player["volume"] = *volume;
    }
    if (muted) {
        player["muted"] = *muted;
    }
    std::string out;
    serializeJson(doc, out);
    return out;
}

inline std::string controller_command(const std::string& object) {
    return R"({"type":"client/command","payload":{"controller":)" + object + "}}";
}

/// The next JSON message, parsed
inline JsonDocument next(RawClient& client) {
    JsonDocument doc;
    const std::string json = client.receive_json();
    EXPECT_FALSE(json.empty()) << "the connection closed";
    deserializeJson(doc, json);
    return doc;
}

/// The next JSON message of type, skipping others
inline JsonDocument next(RawClient& client, std::string_view type) {
    for (;;) {
        const std::string json = client.receive_json();
        if (json.empty()) {
            ADD_FAILURE() << "the connection closed while waiting for " << type;
            return JsonDocument();
        }
        JsonDocument doc;
        deserializeJson(doc, json);
        if (doc["type"].as<std::string_view>() == type) {
            return doc;
        }
    }
}

inline std::string payload(const JsonDocument& doc) {
    std::string out;
    serializeJson(doc["payload"], out);
    return out;
}

inline std::string type(const JsonDocument& doc) {
    return doc["type"].as<std::string>();
}

/// Dials client and runs the handshake through client/hello
inline void connect(SendspinServer& server, RawClient& client, const std::string& hello_json) {
    ASSERT_TRUE(server.connect_url(client.url()));
    ASSERT_NO_FATAL_FAILURE(client.handshake());
    client.send_json(hello_json);
}

inline std::string client_id_of(const RawClient& client) {
    return client.noise.identity().peer_id();
}

inline std::string group_id(const GroupId& id) {
    return std::to_string(id.value);
}

}  // namespace sendspin::server::test
