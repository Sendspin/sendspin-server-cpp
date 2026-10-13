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

/// @file test_messages.cpp
/// @brief The handshake, hello, state and stream messages: client/init validation in the order of
/// spec connection.md "Failure Handling", the builders, and client/hello, client/goodbye,
/// client/state and the source role's messages' validation.

#include "crypto/base64url.h"
#include "crypto/keys.h"
#include "messages.h"
#include <ArduinoJson.h>
#include <gtest/gtest.h>

#include <limits>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace {

using sendspin::server::CipherSuite;
using sendspin::server::ClientGoodbye;
using sendspin::server::ClientHello;
using sendspin::server::ClientInit;
using sendspin::server::ClientMessage;
using sendspin::server::ClientTime;
using sendspin::server::Codec;
using sendspin::server::GoodbyeReason;
using sendspin::server::Identity;
using sendspin::server::InitError;
using sendspin::server::MalformedMessage;
using sendspin::server::OtherMessage;

std::string client_id() {
    static const std::string id = Identity::generate()->peer_id();
    return id;
}

JsonDocument parse(const std::string& text) {
    JsonDocument doc;
    EXPECT_EQ(deserializeJson(doc, text), DeserializationError::Ok) << text;
    return doc;
}

// ==============================================================================
// client/init
// ==============================================================================

TEST(ClientInitValidation, ChecksEnvelopeThenVersionThenSuiteThenTheRest) {
    const std::string id = client_id();
    const std::string good_id = "\"client_id\":\"" + id + "\"";
    const std::string chacha = "\"suite\":\"25519_ChaChaPoly_SHA256\"";
    auto init = [](const std::string& payload) {
        return R"({"type":"client/init","payload":{)" + payload + "}}";
    };
    const std::string zero_key = "\"client_id\":\"" + std::string(42, 'A') + "A\"";
    struct Row {
        std::string text;
        std::optional<InitError> error;
        CipherSuite suite{CipherSuite::CHACHAPOLY_SHA256};
    };
    const Row rows[] = {
        // Control:
        {init(good_id + ",\"version\":1," + chacha), std::nullopt},
        {init(good_id + ",\"version\":1,\"suite\":\"25519_AESGCM_SHA256\""), std::nullopt,
         CipherSuite::AESGCM_SHA256},
        // The parser is lenient; the prologue binds the raw bytes either way
        {init(good_id + ",\"version\":1," + chacha) + " trailing", std::nullopt},
        {"{'type':'client/init','payload':{'client_id':'" + id +
             "','version':1,'suite':'25519_ChaChaPoly_SHA256'}}",
         std::nullopt},
        // Unknown fields are ignored
        {init(good_id + ",\"version\":1," + chacha + ",\"extra\":[1]"), std::nullopt},
        // The envelope
        {"not json", InitError::MALFORMED},
        {"[1]", InitError::MALFORMED},
        {R"({"payload":{"version":1}})", InitError::MALFORMED},
        {R"({"type":7,"payload":{"version":1}})", InitError::MALFORMED},
        {R"({"type":"client/hello","payload":{"version":1}})", InitError::MALFORMED},
        {R"({"type":"client/init"})", InitError::MALFORMED},
        {R"({"type":"client/init","payload":[]})", InitError::MALFORMED},
        {R"({"type":"client/init","payload":"version 2"})", InitError::MALFORMED},
        // An integer version other than 1, whatever the other fields hold
        {init("\"version\":2"), InitError::UNSUPPORTED_VERSION},
        {init("\"version\":0,\"suite\":42,\"client_id\":false"), InitError::UNSUPPORTED_VERSION},
        {init("\"version\":-1," + chacha + "," + good_id), InitError::UNSUPPORTED_VERSION},
        {init("\"version\":18446744073709551615"), InitError::UNSUPPORTED_VERSION},
        // A version that is not an integer
        {init(good_id + ",\"version\":\"1\"," + chacha), InitError::MALFORMED},
        {init(good_id + ",\"version\":1.5," + chacha), InitError::MALFORMED},
        {init(good_id + ",\"version\":true," + chacha), InitError::MALFORMED},
        {init(good_id + "," + chacha), InitError::MALFORMED},
        // The suite, before the remaining fields
        {init("\"version\":1,\"suite\":\"25519_Other_SHA256\""), InitError::UNSUPPORTED_SUITE},
        {init(good_id + ",\"version\":1,\"suite\":7"), InitError::MALFORMED},
        {init(good_id + ",\"version\":1"), InitError::MALFORMED},
        // The client_id
        {init("\"version\":1," + chacha), InitError::MALFORMED},
        {init("\"version\":1," + chacha + ",\"client_id\":7"), InitError::MALFORMED},
        {init("\"version\":1," + chacha + ",\"client_id\":\"" + id.substr(1) + "\""),
         InitError::MALFORMED},
        {init("\"version\":1," + chacha + ",\"client_id\":\"" + id + "=\""), InitError::MALFORMED},
        // A small-order key (connection.md "Identities")
        {init("\"version\":1," + chacha + "," + zero_key), InitError::MALFORMED},
    };
    for (const Row& row : rows) {
        SCOPED_TRACE(row.text);
        const std::variant<ClientInit, InitError> result =
            sendspin::server::validate_client_init(row.text, 1234);
        if (row.error) {
            ASSERT_TRUE(std::holds_alternative<InitError>(result));
            EXPECT_EQ(std::get<InitError>(result), *row.error);
        } else {
            ASSERT_TRUE(std::holds_alternative<ClientInit>(result));
            const ClientInit& init = std::get<ClientInit>(result);
            EXPECT_EQ(init.client_id, id);
            EXPECT_EQ(init.client_key, sendspin::server::public_key_from_peer_id(id));
            EXPECT_EQ(init.suite, row.suite);
            EXPECT_EQ(init.rx_us, 1234);
        }
    }
}

// ==============================================================================
// Builders
// ==============================================================================

TEST(HandshakeBuilders, CarryTheSpecFields) {
    const JsonDocument init = parse(sendspin::server::build_server_init(client_id()));
    EXPECT_EQ(init["type"], "server/init");
    EXPECT_EQ(init["payload"]["server_id"], client_id());
    EXPECT_EQ(init["payload"]["version"], 1);
    EXPECT_EQ(init["payload"].size(), 2U);

    struct ErrorRow {
        InitError error;
        const char* reason;
    };
    const ErrorRow errors[] = {
        {InitError::UNSUPPORTED_VERSION, "unsupported_version"},
        {InitError::UNSUPPORTED_SUITE, "unsupported_suite"},
        {InitError::MALFORMED, "malformed"},
    };
    for (const ErrorRow& row : errors) {
        const JsonDocument error = parse(sendspin::server::build_server_error(row.error));
        EXPECT_EQ(error["type"], "server/error");
        EXPECT_EQ(error["payload"]["reason"], row.reason);
        EXPECT_EQ(error["payload"].size(), 1U);
    }

    const JsonDocument payload = parse(sendspin::server::build_message_1_payload(
        sendspin::server::SENTINEL_PSK_ID, sendspin::server::PskCategory::SENTINEL));
    EXPECT_EQ(payload["psk_id"], sendspin::server::SENTINEL_PSK_ID);
    EXPECT_EQ(payload["psk_category"], "sn");
}

TEST(NoiseHandshakeMessage, RoundTripsAndRejectsOtherShapes) {
    const std::vector<uint8_t> bytes{0x00, 0xFB, 0xFF, 0x10};
    const std::string message = sendspin::server::build_noise_handshake(bytes);
    EXPECT_EQ(parse(message)["type"], "noise/handshake");
    EXPECT_EQ(sendspin::server::parse_noise_handshake(message), bytes);  // Control:

    const char* rejected[] = {
        "{",
        R"({"type":"noise/handshake","payload":{}})",
        R"({"type":"noise/handshake","payload":{"data":""}})",
        R"({"type":"noise/handshake","payload":{"data":7}})",
        R"({"type":"noise/handshake","payload":{"data":"AP+/EA"}})",
        R"({"type":"server/hello","payload":{"data":"APv_EA"}})",
    };
    for (const char* text : rejected) {
        SCOPED_TRACE(text);
        EXPECT_FALSE(sendspin::server::parse_noise_handshake(text));
    }
}

TEST(Message2Payload, IsExactlyTheTwoBytesOfAnEmptyObject) {
    struct Row {
        std::string payload;
        bool valid;
    };
    const Row rows[] = {
        {"{}", true},  // Control:
        {"{ }", false}, {"{}\n", false}, {"", false}, {R"({"a":1})", false},
    };
    for (const Row& row : rows) {
        SCOPED_TRACE(row.payload);
        const std::vector<uint8_t> bytes(row.payload.begin(), row.payload.end());
        EXPECT_EQ(sendspin::server::is_message_2_payload(bytes), row.valid);
    }
}

TEST(ServerHelloBuilder, OmitsLanguagesWhenEmpty) {
    const JsonDocument bare =
        parse(*sendspin::server::build_server_hello({"Kitchen \"server\"", {}, {}}));
    EXPECT_EQ(bare["type"], "server/hello");
    EXPECT_EQ(bare["payload"]["name"], "Kitchen \"server\"");
    EXPECT_FALSE(bare["payload"]["languages"].is<JsonVariantConst>());
    EXPECT_EQ(bare["payload"].size(), 1U);

    const JsonDocument with_languages =
        parse(*sendspin::server::build_server_hello({"Kitchen", {"ca", "en"}, {}}));
    ASSERT_EQ(with_languages["payload"]["languages"].size(), 2U);
    EXPECT_EQ(with_languages["payload"]["languages"][0], "ca");
    EXPECT_EQ(with_languages["payload"]["languages"][1], "en");
}

// ==============================================================================
// client/hello
// ==============================================================================

// A complete client/hello; each table row edits one part of it
JsonDocument full_hello() {
    return parse(R"({"type":"client/hello","payload":{
        "name":"Living Room",
        "device_info":{"product_name":"Speaker","manufacturer":"Acme",
                       "software_version":"1.2","mac_address":"aa:bb:cc:dd:ee:ff","extra":1},
        "supported_roles":["player@v2","player@v1","source@v1","visualizer@v1","metadata@v1",
                           "_acme_lamp@v1","player@_experimental"],
        "player@v1_support":{"supported_formats":[
            {"codec":"flac","channels":2,"sample_rate":48000,"bit_depth":24},
            {"codec":"future","channels":2,"sample_rate":48000,"bit_depth":16},
            {"codec":"pcm","channels":1,"sample_rate":44100,"bit_depth":16}],
            "buffer_capacity":1000000},
        "source@v1_support":{"features":{"line_sense":true}},
        "visualizer@v1_support":{"buffer_capacity":4096},
        "supported_pair_methods":{
            "pairing_psk":{"locations":["device","moon"]},
            "dynamic_pairing_code":{"out_channels":["display"],"formats":["qr_code","hologram"]},
            "future_method":{"anything":true}},
        "unpaired_access":{"enabled":true}}})");
}

ClientMessage parse_hello(const JsonDocument& doc) {
    std::string text;
    serializeJson(doc, text);
    return sendspin::server::parse_client_message(text, 99);
}

TEST(ClientHelloValidation, RecordsRolesSupportObjectsAndPairMethods) {
    const ClientMessage message = parse_hello(full_hello());
    ASSERT_TRUE(std::holds_alternative<ClientHello>(message));
    const ClientHello& hello = std::get<ClientHello>(message);
    EXPECT_EQ(hello.name, "Living Room");
    EXPECT_EQ(hello.rx_us, 99);
    EXPECT_EQ(hello.device_info.product_name, "Speaker");
    EXPECT_EQ(hello.device_info.manufacturer, "Acme");
    EXPECT_EQ(hello.device_info.software_version, "1.2");
    EXPECT_EQ(hello.device_info.mac_address, "aa:bb:cc:dd:ee:ff");
    EXPECT_EQ(hello.supported_roles,
              (std::vector<std::string>{"player@v2", "player@v1", "source@v1", "visualizer@v1",
                                        "metadata@v1", "_acme_lamp@v1", "player@_experimental"}));

    ASSERT_TRUE(hello.player_v1_support);
    // The unrecognized codec's entry is left out; the order is kept
    EXPECT_EQ(hello.player_v1_support->supported_formats,
              (std::vector<sendspin::server::AudioFormat>{{Codec::FLAC, 2, 48000, 24},
                                                          {Codec::PCM, 1, 44100, 16}}));
    EXPECT_EQ(hello.player_v1_support->buffer_capacity, 1000000U);
    ASSERT_TRUE(hello.source_v1_support);
    EXPECT_TRUE(hello.source_v1_support->line_sense);
    ASSERT_TRUE(hello.visualizer_v1_support);
    EXPECT_EQ(hello.visualizer_v1_support->buffer_capacity, 4096U);

    ASSERT_TRUE(hello.pair_methods.pairing_psk);
    EXPECT_EQ(hello.pair_methods.pairing_psk->locations, sendspin::server::pair_location::DEVICE);
    EXPECT_FALSE(hello.pair_methods.static_pairing_code);
    ASSERT_TRUE(hello.pair_methods.dynamic_pairing_code);
    EXPECT_EQ(hello.pair_methods.dynamic_pairing_code->out_channels,
              sendspin::server::pair_code::DISPLAY);
    EXPECT_EQ(hello.pair_methods.dynamic_pairing_code->formats,
              sendspin::server::pair_code::QR_CODE);
    EXPECT_TRUE(hello.unpaired_access_enabled);

    // Not the application-specific role or version
    EXPECT_EQ(hello.unrecognized_identifiers,
              (std::vector<std::string>{"player@v2", "future_method"}));
}

TEST(ClientHelloValidation, RejectsAHelloWithoutItsRequiredFields) {
    struct Row {
        const char* name;
        const char* path_a;
        const char* path_b;  // nullptr: edit payload[path_a]
        const char* json;    // replacement value, or nullptr to remove
    };
    const Row rows[] = {
        {"name missing", "name", nullptr, nullptr},
        {"name not a string", "name", nullptr, "7"},
        {"supported_roles missing", "supported_roles", nullptr, nullptr},
        {"a role not a string", "supported_roles", nullptr, R"(["player@v1",1])"},
        {"supported_pair_methods missing", "supported_pair_methods", nullptr, nullptr},
        {"supported_pair_methods not an object", "supported_pair_methods", nullptr, "[]"},
        {"unpaired_access missing", "unpaired_access", nullptr, nullptr},
        {"enabled missing", "unpaired_access", "enabled", nullptr},
        {"enabled not a boolean", "unpaired_access", "enabled", "1"},
    };
    {
        SCOPED_TRACE("Control:");
        EXPECT_TRUE(std::holds_alternative<ClientHello>(parse_hello(full_hello())));
    }
    for (const Row& row : rows) {
        SCOPED_TRACE(row.name);
        JsonDocument doc = full_hello();
        JsonVariant parent = doc["payload"];
        const char* key = row.path_a;
        if (row.path_b != nullptr) {
            parent = doc["payload"][row.path_a];
            key = row.path_b;
        }
        if (row.json == nullptr) {
            parent.as<JsonObject>().remove(key);
        } else {
            JsonDocument value = parse(row.json);
            parent[key] = value;
        }
        const ClientMessage message = parse_hello(doc);
        ASSERT_TRUE(std::holds_alternative<MalformedMessage>(message));
        EXPECT_EQ(std::get<MalformedMessage>(message).type, "client/hello");
    }
}

TEST(ClientHelloValidation, TreatsAnInvalidOptionalObjectAsAbsent) {
    struct Row {
        const char* name;
        const char* key;
        const char* json;  // replacement value, or nullptr to remove
        bool player;
        bool source;
        bool visualizer;
    };
    const Row rows[] = {
        {"Control:", "metadata", "1", true, true, true},
        {"player object missing", "player@v1_support", nullptr, false, true, true},
        {"no formats", "player@v1_support", R"({"supported_formats":[],"buffer_capacity":1})",
         false, true, true},
        {"zero capacity", "player@v1_support",
         R"({"supported_formats":[{"codec":"pcm","channels":2,"sample_rate":48000,"bit_depth":16}],
             "buffer_capacity":0})",
         false, true, true},
        {"zero channels", "player@v1_support",
         R"({"supported_formats":[{"codec":"pcm","channels":0,"sample_rate":48000,"bit_depth":16}],
             "buffer_capacity":1})",
         false, true, true},
        {"entry not an object", "player@v1_support",
         R"({"supported_formats":["pcm"],"buffer_capacity":1})", false, true, true},
        {"bit_depth missing", "player@v1_support",
         R"({"supported_formats":[{"codec":"pcm","channels":2,"sample_rate":48000}],
             "buffer_capacity":1})",
         false, true, true},
        {"source features not an object", "source@v1_support", R"({"features":1})", true, false,
         true},
        {"line_sense not a boolean", "source@v1_support", R"({"features":{"line_sense":1}})", true,
         false, true},
        {"source object missing", "source@v1_support", nullptr, true, false, true},
        {"visualizer capacity negative", "visualizer@v1_support", R"({"buffer_capacity":-1})", true,
         true, false},
    };
    for (const Row& row : rows) {
        SCOPED_TRACE(row.name);
        JsonDocument doc = full_hello();
        if (row.json == nullptr) {
            doc["payload"].as<JsonObject>().remove(row.key);
        } else {
            JsonDocument value = parse(row.json);
            doc["payload"][row.key] = value;
        }
        const ClientMessage message = parse_hello(doc);
        ASSERT_TRUE(std::holds_alternative<ClientHello>(message));
        const ClientHello& hello = std::get<ClientHello>(message);
        EXPECT_EQ(hello.player_v1_support.has_value(), row.player);
        EXPECT_EQ(hello.source_v1_support.has_value(), row.source);
        EXPECT_EQ(hello.visualizer_v1_support.has_value(), row.visualizer);
    }
}

TEST(ClientHelloValidation, RecordsSupportObjectsOnlyForListedRoles) {
    JsonDocument doc = full_hello();
    JsonDocument roles = parse(R"(["metadata@v1"])");
    doc["payload"]["supported_roles"] = roles;
    const ClientMessage message = parse_hello(doc);
    ASSERT_TRUE(std::holds_alternative<ClientHello>(message));
    const ClientHello& hello = std::get<ClientHello>(message);
    EXPECT_FALSE(hello.player_v1_support);
    EXPECT_FALSE(hello.source_v1_support);
    EXPECT_FALSE(hello.visualizer_v1_support);
}

TEST(ClientHelloValidation, AppliesThePairMethodDescriptorIgnoreRules) {
    struct Row {
        const char* name;
        const char* methods;
        bool pairing_psk;
        bool static_code;
        bool dynamic_code;
        std::vector<std::string> unrecognized;
    };
    const Row rows[] = {
        {"Control:",
         R"({"pairing_psk":{},"static_pairing_code":{"locations":["leaflet"]}})",
         true,
         true,
         false,
         {"player@v2"}},
        {"dynamic code with no recognized format",
         R"({"pairing_psk":{},"dynamic_pairing_code":{"out_channels":["display"],"formats":["x"]}})",
         true,
         false,
         false,
         {"player@v2", "dynamic_pairing_code"}},
        {"dynamic code with no recognized channel",
         R"({"pairing_psk":{},"dynamic_pairing_code":{"out_channels":["x"],"formats":["digits"]}})",
         true,
         false,
         false,
         {"player@v2", "dynamic_pairing_code"}},
        {"locations not an array",
         R"({"pairing_psk":{"locations":"device"}})",
         false,
         false,
         false,
         {"player@v2"}},
        {"an unrecognized key is left unvalidated",
         R"({"pairing_psk":{},"x_method":7})",
         true,
         false,
         false,
         {"player@v2", "x_method"}},
    };
    for (const Row& row : rows) {
        SCOPED_TRACE(row.name);
        JsonDocument doc = full_hello();
        JsonDocument methods = parse(row.methods);
        doc["payload"]["supported_pair_methods"] = methods;
        const ClientMessage message = parse_hello(doc);
        ASSERT_TRUE(std::holds_alternative<ClientHello>(message));
        const ClientHello& hello = std::get<ClientHello>(message);
        EXPECT_EQ(hello.pair_methods.pairing_psk.has_value(), row.pairing_psk);
        EXPECT_EQ(hello.pair_methods.static_pairing_code.has_value(), row.static_code);
        EXPECT_EQ(hello.pair_methods.dynamic_pairing_code.has_value(), row.dynamic_code);
        EXPECT_EQ(hello.unrecognized_identifiers, row.unrecognized);
    }
}

// ==============================================================================
// Other inbound messages
// ==============================================================================

TEST(ClientMessages, ParseGoodbyeAndTimeAndPassOtherTypesThrough) {
    struct Row {
        const char* text;
        int kind;  // variant index: 0 malformed, 1 hello, 2 goodbye, 3 time, or OtherMessage's
        std::optional<GoodbyeReason> reason;
        int64_t client_transmitted;
    };
    const Row rows[] = {
        {R"({"type":"client/goodbye","payload":{"reason":"restart"}})", 2, GoodbyeReason::RESTART,
         0},
        {R"({"type":"client/goodbye","payload":{"reason":"another_server"}})", 2,
         GoodbyeReason::ANOTHER_SERVER, 0},
        {R"({"type":"client/goodbye","payload":{"reason":"unpaired"}})", 2, GoodbyeReason::UNPAIRED,
         0},
        {R"({"type":"client/goodbye","payload":{"reason":"moving"}})", 2, std::nullopt, 0},
        {R"({"type":"client/goodbye","payload":{}})", 0, std::nullopt, 0},
        {R"({"type":"client/goodbye","payload":{"reason":3}})", 0, std::nullopt, 0},
        // messaging.md "client/time"; unrecognized payload fields are ignored
        {R"({"type":"client/time","payload":{"client_transmitted":5}})", 3, std::nullopt, 5},
        {R"({"type":"client/time","payload":{"client_transmitted":-9,"x":1}})", 3, std::nullopt,
         -9},
        {R"({"type":"client/time","payload":{}})", 0, std::nullopt, 0},
        {R"({"type":"client/time","payload":{"client_transmitted":"5"}})", 0, std::nullopt, 0},
        {R"({"type":"client/time","payload":{"client_transmitted":5.5}})", 0, std::nullopt, 0},
        {R"({"type":"client/time","payload":{"client_transmitted":9223372036854775808}})", 0,
         std::nullopt, 0},
        {R"({"type":"client/time"})", 0, std::nullopt, 0},
        {R"({"type":"client/unknown","payload":{"x":1}})",
         static_cast<int>(ClientMessage(OtherMessage{}).index()), std::nullopt, 0},
        {R"({"payload":{}})", 0, std::nullopt, 0},
        {"[]", 0, std::nullopt, 0},
        {"{", 0, std::nullopt, 0},
    };
    for (const Row& row : rows) {
        SCOPED_TRACE(row.text);
        const ClientMessage message = sendspin::server::parse_client_message(row.text, 5);
        ASSERT_EQ(static_cast<int>(message.index()), row.kind);
        if (const ClientGoodbye* goodbye = std::get_if<ClientGoodbye>(&message)) {
            EXPECT_EQ(goodbye->reason, row.reason);
            EXPECT_EQ(goodbye->rx_us, 5);
        }
        if (const ClientTime* time = std::get_if<ClientTime>(&message)) {
            EXPECT_EQ(time->client_transmitted, row.client_transmitted);
            EXPECT_EQ(time->rx_us, 5);
        }
        if (const OtherMessage* other = std::get_if<OtherMessage>(&message)) {
            EXPECT_EQ(other->type, "client/unknown");
            EXPECT_EQ(other->rx_us, 5);
        }
    }
}

// messaging.md "Communication": unrecognized types are ignored; recognized ones keep their
// direction and sequencing rules
TEST(UnhandledRule, IgnoresWhatTheClientMaySendAndClosesOnWhatItMayNot) {
    using sendspin::server::UnhandledRule;
    struct Row {
        const char* type;
        UnhandledRule rule;
    };
    const Row rows[] = {
        {"Control: client/state", UnhandledRule::IGNORE},
        {"client/command", UnhandledRule::IGNORE},
        {"client/leave", UnhandledRule::IGNORE},
        {"client-stream/start", UnhandledRule::IGNORE},
        {"client/pair-init", UnhandledRule::IGNORE},
        {"pair/abort", UnhandledRule::IGNORE},
        {"client/unknown", UnhandledRule::IGNORE},
        {"server/unknown", UnhandledRule::IGNORE},
        {"", UnhandledRule::IGNORE},
        {"client/hello", UnhandledRule::CLOSE},
        {"client/init", UnhandledRule::CLOSE},
        {"noise/handshake", UnhandledRule::CLOSE},
        {"server/init", UnhandledRule::CLOSE},
        {"server/error", UnhandledRule::CLOSE},
        {"server/hello", UnhandledRule::CLOSE},
        {"server/activate", UnhandledRule::CLOSE},
        {"server/time", UnhandledRule::CLOSE},
        {"server/state", UnhandledRule::CLOSE},
        {"server/command", UnhandledRule::CLOSE},
        {"server/unpair", UnhandledRule::CLOSE},
        {"server/pair-auth", UnhandledRule::CLOSE},
        {"server/pair-confirm", UnhandledRule::CLOSE},
        {"server/pair-finalize", UnhandledRule::CLOSE},
        {"server/pair-init", UnhandledRule::CLOSE},
        {"stream/start", UnhandledRule::CLOSE},
        {"stream/clear", UnhandledRule::CLOSE},
        {"stream/end", UnhandledRule::CLOSE},
        {"group/update", UnhandledRule::CLOSE},
    };
    for (const Row& row : rows) {
        std::string_view type = row.type;
        if (type.starts_with("Control: ")) {
            type.remove_prefix(std::string_view("Control: ").size());
        }
        SCOPED_TRACE(type);
        EXPECT_EQ(sendspin::server::rule_for_unhandled(type), row.rule);
    }
}

// ==============================================================================
// server/activate and server/time
// ==============================================================================

// messaging.md "server/activate"
TEST(ServerActivateBuilder, WritesActivitiesAndOmitsRolesOnlyWhenAsked) {
    using sendspin::server::ServerActivate;
    struct Row {
        const char* name;
        ServerActivate activate;
        const char* json;
    };
    using sendspin::server::CodeFormat;
    using sendspin::server::PairingParams;
    using sendspin::server::PairMethod;
    const Row rows[] = {
        {"initial, no activities",
         {false, std::vector<std::string>{}, std::nullopt},
         R"({"type":"server/activate","payload":{"activities":[],"active_roles":[]}})"},
        {"playback with roles",
         {true, std::vector<std::string>{"player@v1", "metadata@v1"}, std::nullopt},
         R"({"type":"server/activate","payload":{"activities":["playback"],)"
         R"("active_roles":["player@v1","metadata@v1"]}})"},
        {"roles kept",
         {false, std::nullopt, std::nullopt},
         R"({"type":"server/activate","payload":{"activities":[]}})"},
        // messaging.md "server/activate": `pairing` with its method, and `format` for a dynamic
        // code only
        {"pairing PSK",
         {false, std::vector<std::string>{}, PairingParams{PairMethod::PAIRING_PSK, std::nullopt}},
         R"({"type":"server/activate","payload":{"activities":["pairing"],"active_roles":[],)"
         R"("pairing":{"method":"pairing_psk"}}})"},
        {"static code",
         {false, std::nullopt, PairingParams{PairMethod::STATIC_PAIRING_CODE, std::nullopt}},
         R"({"type":"server/activate","payload":{"activities":["pairing"],)"
         R"("pairing":{"method":"static_pairing_code"}}})"},
        {"dynamic code alongside playback",
         {true, std::nullopt, PairingParams{PairMethod::DYNAMIC_PAIRING_CODE, CodeFormat::QR_CODE}},
         R"({"type":"server/activate","payload":{"activities":["playback","pairing"],)"
         R"("pairing":{"method":"dynamic_pairing_code","format":"qr_code"}}})"},
        {"dynamic code in digits",
         {false, std::nullopt, PairingParams{PairMethod::DYNAMIC_PAIRING_CODE, CodeFormat::DIGITS}},
         R"({"type":"server/activate","payload":{"activities":["pairing"],)"
         R"("pairing":{"method":"dynamic_pairing_code","format":"digits"}}})"},
    };
    for (const Row& row : rows) {
        SCOPED_TRACE(row.name);
        EXPECT_EQ(sendspin::server::build_server_activate(row.activate), row.json);
    }
}

// A value the JSON library cannot hold would go out as null; the builders refuse it instead
TEST(JsonBuilders, RefuseAValueTooLongToEncode) {
    using sendspin::server::MAX_JSON_STRING_BYTES;
    const std::string longest(MAX_JSON_STRING_BYTES, 'x');
    const std::string too_long(MAX_JSON_STRING_BYTES + 1, 'x');
    struct Row {
        const char* name;
        std::optional<std::string> json;
        bool built;
    };
    const Row rows[] = {
        {"Control: a name at the limit", sendspin::server::build_server_hello({longest, {}, {}}),
         true},
        {"a name past it", sendspin::server::build_server_hello({too_long, {}, {}}), false},
        {"Control: a language at the limit",
         sendspin::server::build_server_hello({"Kitchen", {"en", longest}, {}}), true},
        {"a language past it",
         sendspin::server::build_server_hello({"Kitchen", {"en", too_long}, {}}), false},
        {"Control: a role at the limit",
         sendspin::server::build_server_activate(
             {false, std::vector<std::string>{longest}, std::nullopt}),
         true},
        {"a role past it",
         sendspin::server::build_server_activate(
             {false, std::vector<std::string>{too_long}, std::nullopt}),
         false},
    };
    for (const Row& row : rows) {
        SCOPED_TRACE(row.name);
        EXPECT_EQ(row.json.has_value(), row.built);
        if (row.json) {
            EXPECT_EQ(row.json->find("null"), std::string::npos);
        }
    }
}

// messaging.md "server/time": the request's own value and receive stamp, and the transmit stamp
TEST(ServerTimeBuilder, AnswersTheStampedRequest) {
    struct Row {
        ClientTime request;
        int64_t transmitted_us;
        const char* json;
    };
    const Row rows[] = {
        {{123, 456},
         789,
         R"({"type":"server/time","payload":{"client_transmitted":123,)"
         R"("server_received":456,"server_transmitted":789}})"},
        // Above 2^53, so a double could not carry them
        {{-5, 9007199254740993},
         9007199254740995,
         R"({"type":"server/time","payload":{"client_transmitted":-5,)"
         R"("server_received":9007199254740993,"server_transmitted":9007199254740995}})"},
    };
    for (const Row& row : rows) {
        SCOPED_TRACE(row.json);
        EXPECT_EQ(sendspin::server::build_server_time(row.request, row.transmitted_us), row.json);
    }
}

// ==============================================================================
// client/state
// ==============================================================================

// messaging.md "client/state" and roles/player/v1.md "client/state player object": `available`
// is required; a player object present is validated whole, and an invalid one is marked rather
// than dropping the message, since the role may be inactive
TEST(ClientStateValidation, ValidatesAvailableAndThePlayerObject) {
    using sendspin::server::ClientState;
    using sendspin::server::PlayerState;
    const std::string player =
        R"("output_delay_ms":40,"required_lead_time_ms":300,"min_buffer_ms":100)";
    const auto state = [](const std::string& payload) {
        return R"({"type":"client/state","payload":)" + payload + "}";
    };
    enum class Outcome : uint8_t { MALFORMED, NO_PLAYER, PLAYER, INVALID_PLAYER };
    struct Row {
        const char* name;
        std::string text;
        Outcome outcome;
    };
    const Row rows[] = {
        {"Control: a full player object",
         state(R"({"available":true,"player":{)" + player +
               R"(,"supported_commands":["volume","mute","dance"],"volume":30,"muted":false,)"
               R"("format":{"codec":"pcm","channels":2,"sample_rate":48000,"bit_depth":24}}})"),
         Outcome::PLAYER},
        {"Control: no player object", state(R"({"available":false})"), Outcome::NO_PLAYER},
        {"no available", state(R"({"player":{}})"), Outcome::MALFORMED},
        {"available not a boolean", state(R"({"available":1})"), Outcome::MALFORMED},
        {"output delay past 5000",
         state(R"({"available":true,"player":{"output_delay_ms":5001,)"
               R"("required_lead_time_ms":0,"min_buffer_ms":0,"supported_commands":[]}})"),
         Outcome::INVALID_PLAYER},
        {"a negative minimum buffer",
         state(R"({"available":true,"player":{"output_delay_ms":0,"required_lead_time_ms":0,)"
               R"("min_buffer_ms":-1,"supported_commands":[]}})"),
         Outcome::INVALID_PLAYER},
        {"no supported_commands", state(R"({"available":true,"player":{)" + player + "}}"),
         Outcome::INVALID_PLAYER},
        {"volume offered and not reported",
         state(R"({"available":true,"player":{)" + player +
               R"(,"supported_commands":["volume"]}})"),
         Outcome::INVALID_PLAYER},
        {"volume past 100",
         state(R"({"available":true,"player":{)" + player +
               R"(,"supported_commands":[],"volume":101}})"),
         Outcome::INVALID_PLAYER},
        {"a format without a sample rate",
         state(
             R"({"available":true,"player":{)" + player +
             R"(,"supported_commands":[],"format":{"codec":"pcm","channels":2,"bit_depth":16}}})"),
         Outcome::INVALID_PLAYER},
    };
    for (const Row& row : rows) {
        SCOPED_TRACE(row.name);
        const ClientMessage message = sendspin::server::parse_client_message(row.text, 7);
        if (row.outcome == Outcome::MALFORMED) {
            EXPECT_TRUE(std::holds_alternative<MalformedMessage>(message));
            continue;
        }
        ASSERT_TRUE(std::holds_alternative<ClientState>(message));
        const ClientState& parsed = std::get<ClientState>(message);
        EXPECT_EQ(parsed.rx_us, 7);
        EXPECT_EQ(parsed.player.has_value(), row.outcome == Outcome::PLAYER);
        EXPECT_EQ(parsed.player_malformed, row.outcome == Outcome::INVALID_PLAYER);
    }

    const ClientMessage full = sendspin::server::parse_client_message(rows[0].text, 7);
    const PlayerState& parsed = *std::get<ClientState>(full).player;
    EXPECT_TRUE(std::get<ClientState>(full).available);
    EXPECT_EQ(parsed.output_delay_ms, 40U);
    EXPECT_EQ(parsed.required_lead_time_ms, 300U);
    EXPECT_EQ(parsed.min_buffer_ms, 100U);
    // The unrecognized command is left out
    EXPECT_EQ(parsed.supported_commands,
              sendspin::server::player_command::VOLUME | sendspin::server::player_command::MUTE);
    EXPECT_EQ(parsed.volume, 30U);
    EXPECT_EQ(parsed.muted, false);
    EXPECT_EQ(parsed.format, (sendspin::server::AudioFormat{Codec::PCM, 2, 48000, 24}));

    // A preferred format with a codec this server does not know is no preference
    const ClientMessage unknown = sendspin::server::parse_client_message(
        state(R"({"available":true,"player":{)" + player +
              R"(,"supported_commands":[],"format":{"codec":"mp3","channels":2,)"
              R"("sample_rate":48000,"bit_depth":16}}})"),
        7);
    ASSERT_TRUE(std::get<ClientState>(unknown).player);
    EXPECT_FALSE(std::get<ClientState>(unknown).player->format);
}

// roles/visualizer/v1.md "client/state visualizer object": `types` an array of strings, an
// unrecognized type adding none; `rate_max` a positive integer; a `spectrum` object of three
// integers and a known scale, required when 'spectrum' is listed. An invalid object is marked, the
// rest of the message kept; whether a valid spectrum can be streamed is the negotiation's
TEST(ClientStateValidation, ValidatesTheVisualizerObject) {
    using sendspin::server::ClientState;
    using sendspin::server::SpectrumConfig;
    using sendspin::server::SpectrumScale;
    namespace type = sendspin::server::visualizer_type;
    const auto state = [](const std::string& visualizer) {
        return R"({"type":"client/state","payload":{"available":true,"visualizer":)" + visualizer +
               "}}";
    };
    const std::string spectrum = R"("spectrum":{"n_disp_bins":32,"scale":"mel","f_min":40,)"
                                 R"("f_max":16000})";
    struct Row {
        const char* name;
        std::string text;
        bool valid;
        /// A valid object whose spectrum the server cannot stream
        bool unusable{false};
    };
    const Row rows[] = {
        {"Control: every type with a spectrum",
         state(R"({"types":["loudness","beat","f_peak","spectrum","peak","shimmer"],)"
               R"("rate_max":30,)" +
               spectrum + "}"),
         true},
        {"Control: no types", state(R"({"types":[],"rate_max":1})"), true},
        {"Control: a spectrum without its type, and no bins",
         state(R"({"types":["peak"],"rate_max":60,"spectrum":{"n_disp_bins":0,"scale":"lin",)"
               R"("f_min":0,"f_max":0}})"),
         true},
        {"spectrum without a spectrum object", state(R"({"types":["spectrum"],"rate_max":30})"),
         false},
        {"no types", state(R"({"rate_max":30})"), false},
        {"a type not a string", state(R"({"types":["loudness",1],"rate_max":30})"), false},
        {"rate_max 0", state(R"({"types":["loudness"],"rate_max":0})"), false},
        {"no rate_max", state(R"({"types":["loudness"]})"), false},
        {"an unknown scale, a spectrum it cannot stream",
         state(R"({"types":["spectrum"],"rate_max":30,"spectrum":{"n_disp_bins":8,)"
               R"("scale":"bark","f_min":0,"f_max":8000}})"),
         true, true},
        {"a negative f_min, a spectrum it cannot stream",
         state(R"({"types":["spectrum"],"rate_max":30,"spectrum":{"n_disp_bins":8,)"
               R"("scale":"lin","f_min":-1,"f_max":8000}})"),
         true, true},
        {"n_disp_bins past 32 bits, a spectrum it cannot stream",
         state(R"({"types":["spectrum"],"rate_max":30,"spectrum":{"n_disp_bins":4294967296,)"
               R"("scale":"lin","f_min":0,"f_max":8000}})"),
         true, true},
        {"a scale not a string",
         state(R"({"types":["spectrum"],"rate_max":30,"spectrum":{"n_disp_bins":8,)"
               R"("scale":2,"f_min":0,"f_max":8000}})"),
         false},
        {"n_disp_bins not an integer",
         state(R"({"types":["spectrum"],"rate_max":30,"spectrum":{"n_disp_bins":8.5,)"
               R"("scale":"lin","f_min":0,"f_max":8000}})"),
         false},
        {"no f_max",
         state(R"({"types":["spectrum"],"rate_max":30,"spectrum":{"n_disp_bins":8,)"
               R"("scale":"lin","f_min":0}})"),
         false},
        {"not an object", state("[]"), false},
    };
    for (const Row& row : rows) {
        SCOPED_TRACE(row.name);
        const ClientMessage message = sendspin::server::parse_client_message(row.text, 7);
        ASSERT_TRUE(std::holds_alternative<ClientState>(message));
        const ClientState& parsed = std::get<ClientState>(message);
        EXPECT_TRUE(parsed.available);
        EXPECT_EQ(parsed.visualizer.has_value(), row.valid);
        EXPECT_EQ(parsed.visualizer_malformed, !row.valid);
        if (parsed.visualizer) {
            EXPECT_EQ(parsed.visualizer->spectrum_unusable, row.unusable);
            if (row.unusable) {
                EXPECT_FALSE(parsed.visualizer->spectrum.has_value());
            }
        }
    }
    const ClientMessage full = sendspin::server::parse_client_message(rows[0].text, 7);
    const sendspin::server::VisualizerRequest& request = *std::get<ClientState>(full).visualizer;
    EXPECT_EQ(request.types,
              type::LOUDNESS | type::BEAT | type::F_PEAK | type::SPECTRUM | type::PEAK);
    EXPECT_EQ(request.rate_max, 30U);
    EXPECT_EQ(request.spectrum, (SpectrumConfig{32, SpectrumScale::MEL, 40, 16000}));
}

// ==============================================================================
// group/update and stream lifecycle
// ==============================================================================

// messaging.md "group/update", "stream/start", "stream/clear", "stream/end" and roles/player/v1.md
// "stream/start player object"
TEST(StreamBuilders, CarryTheSpecFields) {
    using sendspin::server::AudioFormat;
    using sendspin::server::GroupUpdate;
    const std::string_view player[] = {"player"};
    EXPECT_EQ(sendspin::server::build_group_update(GroupUpdate{true, "7", "Kitchen"}),
              R"({"type":"group/update","payload":{"playback_state":"playing","group_id":"7",)"
              R"("group_name":"Kitchen"}})");
    EXPECT_EQ(sendspin::server::build_group_update(GroupUpdate{false, "7", "Kitchen"}),
              R"({"type":"group/update","payload":{"playback_state":"stopped","group_id":"7",)"
              R"("group_name":"Kitchen"}})");
    EXPECT_FALSE(sendspin::server::build_group_update(
        GroupUpdate{true, "7", std::string(sendspin::server::MAX_JSON_STRING_BYTES + 1, 'x')}));
    using sendspin::server::StreamStart;
    EXPECT_EQ(sendspin::server::build_stream_start(
                  9007199254740993,
                  StreamStart{AudioFormat{Codec::PCM, 2, 48000, 24}, std::nullopt, std::nullopt}),
              R"({"type":"stream/start","payload":{"server_transmitted":9007199254740993,)"
              R"("player":{"codec":"pcm","sample_rate":48000,"channels":2,"bit_depth":24}}})");
    // roles/player/v1.md "stream/start player object": the codec_header when the codec has one
    EXPECT_EQ(
        sendspin::server::build_stream_start(5, StreamStart{AudioFormat{Codec::FLAC, 2, 44100, 16},
                                                            std::string("ZkxhQw=="), std::nullopt}),
        R"({"type":"stream/start","payload":{"server_transmitted":5,"player":{"codec":)"
        R"("flac","sample_rate":44100,"channels":2,"bit_depth":16,)"
        R"("codec_header":"ZkxhQw=="}}})");
    // roles/visualizer/v1.md "stream/start visualizer object": the types in their message order,
    // the rate, and the spectrum echoed exactly, beside a player object in one message
    namespace type = sendspin::server::visualizer_type;
    const sendspin::server::VisualizerConfig visualizer{
        type::PEAK | type::SPECTRUM | type::LOUDNESS, 30,
        sendspin::server::SpectrumConfig{64, sendspin::server::SpectrumScale::LOG, 20, 16000}};
    EXPECT_EQ(sendspin::server::build_stream_start(
                  7, StreamStart{AudioFormat{Codec::PCM, 1, 48000, 16}, std::nullopt, visualizer}),
              R"({"type":"stream/start","payload":{"server_transmitted":7,"player":{"codec":)"
              R"("pcm","sample_rate":48000,"channels":1,"bit_depth":16},"visualizer":{"types":)"
              R"(["loudness","spectrum","peak"],"rate_max":30,"spectrum":{"n_disp_bins":64,)"
              R"("scale":"log","f_min":20,"f_max":16000}}}})");
    // Control: no spectrum object without the type
    EXPECT_EQ(
        sendspin::server::build_stream_start(
            8, StreamStart{std::nullopt, std::nullopt,
                           sendspin::server::VisualizerConfig{type::F_PEAK, 50, std::nullopt}}),
        R"({"type":"stream/start","payload":{"server_transmitted":8,"visualizer":{"types":)"
        R"(["f_peak"],"rate_max":50}}})");
    const std::string_view both[] = {"player", "visualizer"};
    EXPECT_EQ(sendspin::server::build_stream_clear(13, both),
              R"({"type":"stream/clear","payload":{"server_transmitted":13,)"
              R"("roles":["player","visualizer"]}})");
    EXPECT_EQ(sendspin::server::build_stream_clear(12, player),
              R"({"type":"stream/clear","payload":{"server_transmitted":12,"roles":["player"]}})");
    EXPECT_EQ(sendspin::server::build_stream_end(player),
              R"({"type":"stream/end","payload":{"roles":["player"]}})");
}

// ==============================================================================
// client/command, client/leave, server/state and server/command
// ==============================================================================

// messaging.md "client/command" and roles/controller/v1.md "client/command controller object": a
// recognized command whose parameter is missing or of the wrong type (volume within 0-100) is
// kept and marked malformed, since one not offered is ignored rather than invalid; an
// unrecognized command is kept as one; a seek's position is not checked here, since out of range
// it is ignored. An object without a command string is malformed.
TEST(ClientCommandValidation, ValidatesTheControllerObjectOnItsOwn) {
    using sendspin::server::ClientCommand;
    using sendspin::server::ControllerCommand;
    enum class Outcome : uint8_t { REQUEST, BAD_PARAMETER, INVALID, NONE };
    struct Row {
        const char* name;
        std::string controller;
        Outcome outcome;
        std::optional<ControllerCommand> command;
    };
    const auto command = [](const std::string& controller) {
        return controller.empty()
                   ? std::string(R"({"type":"client/command","payload":{}})")
                   : R"({"type":"client/command","payload":{"controller":)" + controller + "}}";
    };
    using C = ControllerCommand;
    const Row rows[] = {
        {"Control: play", R"({"command":"play"})", Outcome::REQUEST, C::PLAY},
        {"Control: volume", R"({"command":"volume","volume":40})", Outcome::REQUEST, C::VOLUME},
        {"Control: mute", R"({"command":"mute","mute":true})", Outcome::REQUEST, C::MUTE},
        {"Control: seek past the range", R"({"command":"seek","position_ms":-5})", Outcome::REQUEST,
         C::SEEK},
        {"Control: seek_relative", R"({"command":"seek_relative","offset_ms":-15000})",
         Outcome::REQUEST, C::SEEK_RELATIVE},
        {"Control: switch", R"({"command":"switch"})", Outcome::REQUEST, C::SWITCH},
        {"an unrecognized command", R"({"command":"rewind"})", Outcome::REQUEST, std::nullopt},
        {"no controller object", "", Outcome::NONE, std::nullopt},
        {"volume without its parameter", R"({"command":"volume"})", Outcome::BAD_PARAMETER,
         C::VOLUME},
        {"volume past 100", R"({"command":"volume","volume":101})", Outcome::BAD_PARAMETER,
         C::VOLUME},
        {"volume negative", R"({"command":"volume","volume":-1})", Outcome::BAD_PARAMETER,
         C::VOLUME},
        {"mute not a boolean", R"({"command":"mute","mute":1})", Outcome::BAD_PARAMETER, C::MUTE},
        {"seek without a position", R"({"command":"seek"})", Outcome::BAD_PARAMETER, C::SEEK},
        {"seek_relative with a fraction", R"({"command":"seek_relative","offset_ms":1.5})",
         Outcome::BAD_PARAMETER, C::SEEK_RELATIVE},
        {"no command", R"({"volume":3})", Outcome::INVALID, std::nullopt},
        {"a command not a string", R"({"command":1})", Outcome::INVALID, std::nullopt},
        {"not an object", R"("play")", Outcome::INVALID, std::nullopt},
    };
    for (const Row& row : rows) {
        SCOPED_TRACE(row.name);
        const ClientMessage message =
            sendspin::server::parse_client_message(command(row.controller), 9);
        ASSERT_TRUE(std::holds_alternative<ClientCommand>(message));
        const ClientCommand& parsed = std::get<ClientCommand>(message);
        EXPECT_EQ(parsed.rx_us, 9);
        EXPECT_EQ(parsed.controller_malformed, row.outcome == Outcome::INVALID);
        ASSERT_EQ(parsed.controller.has_value(),
                  row.outcome == Outcome::REQUEST || row.outcome == Outcome::BAD_PARAMETER);
        if (parsed.controller) {
            EXPECT_EQ(parsed.controller->command, row.command);
            EXPECT_EQ(parsed.controller->malformed, row.outcome == Outcome::BAD_PARAMETER);
        }
    }
    const ClientMessage volume =
        sendspin::server::parse_client_message(command(R"({"command":"volume","volume":40})"), 9);
    EXPECT_EQ(std::get<ClientCommand>(volume).controller->volume, 40U);
    const ClientMessage seek = sendspin::server::parse_client_message(
        command(R"({"command":"seek_relative","offset_ms":-15000})"), 9);
    EXPECT_EQ(std::get<ClientCommand>(seek).controller->offset_ms, -15000);

    // messaging.md "client/leave": no payload fields
    const ClientMessage leave =
        sendspin::server::parse_client_message(R"({"type":"client/leave","payload":{}})", 4);
    ASSERT_TRUE(std::holds_alternative<sendspin::server::ClientLeave>(leave));
    EXPECT_EQ(std::get<sendspin::server::ClientLeave>(leave).rx_us, 4);
}

// messaging.md "server/state", roles/controller/v1.md "server/state controller object",
// roles/metadata/v1.md "server/state metadata object" and roles/player/v1.md "server/command
// player object"
TEST(StateBuilders, CarryTheSpecFields) {
    using sendspin::server::command_bit;
    using sendspin::server::ControllerCommand;
    using sendspin::server::ControllerState;
    using sendspin::server::Metadata;
    using sendspin::server::MetadataProgress;
    using sendspin::server::MetadataState;
    using sendspin::server::RepeatMode;
    using sendspin::server::ServerState;
    ControllerState controller;
    controller.supported_commands = command_bit(ControllerCommand::PLAY) |
                                    command_bit(ControllerCommand::SEEK) |
                                    command_bit(ControllerCommand::SWITCH);
    controller.volume = 35;
    controller.muted = true;
    controller.repeat = RepeatMode::ONE;
    controller.shuffle = true;
    controller.seek_max_ms = 180000;
    EXPECT_EQ(sendspin::server::build_server_state({controller, std::nullopt, std::nullopt}),
              R"({"type":"server/state","payload":{"controller":{"supported_commands":)"
              R"(["play","switch","seek"],"volume":35,"muted":true,"repeat":"one",)"
              R"("shuffle":true,"seek_max_ms":180000}}})");
    // seek_max_ms only with seek
    EXPECT_EQ(sendspin::server::build_server_state({ControllerState{}, std::nullopt, std::nullopt}),
              R"({"type":"server/state","payload":{"controller":{"supported_commands":[],)"
              R"("volume":100,"muted":false,"repeat":"off","shuffle":false}}})");

    Metadata metadata;
    metadata.title = "Song";
    metadata.album = "Album";
    metadata.year = 1999;
    metadata.track = 3;
    metadata.progress = MetadataProgress{1500, 200000, 1000};
    EXPECT_EQ(sendspin::server::build_server_state(
                  {std::nullopt, MetadataState{77, metadata}, std::nullopt}),
              R"({"type":"server/state","payload":{"metadata":{"timestamp":77,"title":"Song",)"
              R"("album":"Album","year":1999,"track":3,"progress":{"track_progress":1500,)"
              R"("track_duration":200000,"playback_speed":1000}}}})");
    // A state with no position omits progress, which clears the client's position
    EXPECT_EQ(sendspin::server::build_server_state(
                  {std::nullopt, MetadataState{5, Metadata{}}, std::nullopt}),
              R"({"type":"server/state","payload":{"metadata":{"timestamp":5}}})");
    Metadata long_title;
    long_title.title = std::string(sendspin::server::MAX_JSON_STRING_BYTES + 1, 'x');
    EXPECT_FALSE(sendspin::server::build_server_state(
        {std::nullopt, MetadataState{5, long_title}, std::nullopt}));

    EXPECT_EQ(sendspin::server::build_player_volume_command(42),
              R"({"type":"server/command","payload":{"player":{"command":"volume","volume":42}}})");
    EXPECT_EQ(sendspin::server::build_player_mute_command(false),
              R"({"type":"server/command","payload":{"player":{"command":"mute","mute":false}}})");
}

// ==============================================================================
// The source role
// ==============================================================================

// roles/source/v1.md "server/hello source@v1 support object": the codecs the server accepts, left
// out with none (the bare hello above has one payload member)
TEST(ServerHelloBuilder, ListsTheSourceCodecs) {
    const JsonDocument hello =
        parse(*sendspin::server::build_server_hello({"Kitchen", {}, {Codec::PCM, Codec::FLAC}}));
    const JsonVariantConst codecs = hello["payload"]["source@v1_support"]["supported_codecs"];
    ASSERT_EQ(codecs.size(), 2U);
    EXPECT_EQ(codecs[0], "pcm");
    EXPECT_EQ(codecs[1], "flac");
}

/// `fLaC` and a STREAMINFO block of rate, channels and depth (RFC 9639 section 8.2), the last
/// metadata block unless more follow
std::vector<uint8_t> flac_header(uint32_t rate, uint32_t channels, uint32_t depth) {
    std::vector<uint8_t> header{'f', 'L', 'a', 'C', 0x80, 0, 0, 34};
    std::vector<uint8_t> info(34, 0);
    info[0] = 0x10;  // min and max block size 4,096
    info[2] = 0x10;
    info[10] = static_cast<uint8_t>(rate >> 12);
    info[11] = static_cast<uint8_t>(rate >> 4);
    info[12] =
        static_cast<uint8_t>(((rate & 0x0F) << 4) | ((channels - 1) << 1) | ((depth - 1) >> 4));
    info[13] = static_cast<uint8_t>(((depth - 1) & 0x0F) << 4);
    header.insert(header.end(), info.begin(), info.end());
    return header;
}

std::string base64(const std::vector<uint8_t>& bytes) {
    return sendspin::server::base64_encode(bytes.data(), bytes.size());
}

// roles/source/v1.md "client-stream/start": codec, channels, sample_rate and bit_depth (ignored for
// Opus), a codec_header in standard Base64 that FLAC requires and the others have none of, and a
// codec the server lists; a FLAC header's STREAMINFO agrees with the format it is announced with
TEST(ClientStreamStartValidation, ValidatesTheSourceObject) {
    using sendspin::server::AudioFormat;
    using sendspin::server::ClientStreamStart;
    const std::string flac = base64(flac_header(48000, 2, 16));
    struct Row {
        const char* name;
        std::string source;
        std::optional<AudioFormat> format;
    };
    const Row rows[] = {
        {"Control: pcm", R"({"codec":"pcm","channels":2,"sample_rate":48000,"bit_depth":24})",
         AudioFormat{Codec::PCM, 2, 48000, 24}},
        {"Control: flac with its header",
         R"({"codec":"flac","channels":2,"sample_rate":48000,"bit_depth":16,"codec_header":")" +
             flac + R"("})",
         AudioFormat{Codec::FLAC, 2, 48000, 16}},
        {"Control: opus without bit_depth", R"({"codec":"opus","channels":1,"sample_rate":48000})",
         AudioFormat{Codec::OPUS, 1, 48000, 0}},
        {"Control: opus ignores its bit_depth",
         R"({"codec":"opus","channels":2,"sample_rate":48000,"bit_depth":"x"})",
         AudioFormat{Codec::OPUS, 2, 48000, 0}},
        {"no source object", "", std::nullopt},
        {"an unknown codec", R"({"codec":"mp3","channels":2,"sample_rate":48000,"bit_depth":16})",
         std::nullopt},
        {"no channels", R"({"codec":"pcm","sample_rate":48000,"bit_depth":16})", std::nullopt},
        {"zero channels", R"({"codec":"pcm","channels":0,"sample_rate":48000,"bit_depth":16})",
         std::nullopt},
        {"no sample_rate", R"({"codec":"pcm","channels":2,"bit_depth":16})", std::nullopt},
        {"no bit_depth for pcm", R"({"codec":"pcm","channels":2,"sample_rate":48000})",
         std::nullopt},
        {"a codec_header for pcm",
         R"({"codec":"pcm","channels":2,"sample_rate":48000,"bit_depth":16,"codec_header":"AAAA"})",
         std::nullopt},
        {"a codec_header for opus",
         R"({"codec":"opus","channels":2,"sample_rate":48000,"codec_header":"AAAA"})",
         std::nullopt},
        {"flac without its header",
         R"({"codec":"flac","channels":2,"sample_rate":48000,"bit_depth":16})", std::nullopt},
        {"a flac header not in whole Base64 quads",
         R"({"codec":"flac","channels":2,"sample_rate":48000,"bit_depth":16,"codec_header":")" +
             flac.substr(0, flac.size() - 1) + R"("})",
         std::nullopt},
        {"a flac header for another rate",
         R"({"codec":"flac","channels":2,"sample_rate":44100,"bit_depth":16,"codec_header":")" +
             flac + R"("})",
         std::nullopt},
        {"a flac header for another depth",
         R"({"codec":"flac","channels":2,"sample_rate":48000,"bit_depth":24,"codec_header":")" +
             flac + R"("})",
         std::nullopt},
        {"a flac header that is not one",
         R"({"codec":"flac","channels":2,"sample_rate":48000,"bit_depth":16,"codec_header":")" +
             base64({'O', 'g', 'g', 'S', 0, 0, 0, 0}) + R"("})",
         std::nullopt},
    };
    for (const Row& row : rows) {
        SCOPED_TRACE(row.name);
        const std::string json =
            R"({"type":"client-stream/start","payload":{)" +
            (row.source.empty() ? std::string() : R"("source":)" + row.source) + "}}";
        const ClientMessage message = sendspin::server::parse_client_message(json, 9);
        if (!row.format) {
            EXPECT_TRUE(std::holds_alternative<MalformedMessage>(message));
            continue;
        }
        ASSERT_TRUE(std::holds_alternative<ClientStreamStart>(message));
        const ClientStreamStart& start = std::get<ClientStreamStart>(message);
        EXPECT_EQ(start.format, *row.format);
        EXPECT_EQ(start.rx_us, 9);
        EXPECT_EQ(start.codec_header.empty(), row.format->codec != Codec::FLAC);
    }
    const ClientMessage end =
        sendspin::server::parse_client_message(R"({"type":"client-stream/end","payload":{}})", 4);
    ASSERT_TRUE(std::holds_alternative<sendspin::server::ClientStreamEnd>(end));
    EXPECT_EQ(std::get<sendspin::server::ClientStreamEnd>(end).rx_us, 4);
}

// roles/source/v1.md "client/state source object": `signal` 'present' or 'absent', optional; one
// this server does not know reads as unreported, and a source object that is not one is malformed
TEST(ClientStateValidation, ValidatesTheSourceObject) {
    using sendspin::server::ClientState;
    using sendspin::server::SourceSignal;
    struct Row {
        const char* name;
        std::string source;
        bool malformed;
        std::optional<SourceSignal> signal;
    };
    const Row rows[] = {
        {"Control: empty", "{}", false, std::nullopt},
        {"Control: present", R"({"signal":"present"})", false, SourceSignal::PRESENT},
        {"Control: absent", R"({"signal":"absent"})", false, SourceSignal::ABSENT},
        {"an unknown signal", R"({"signal":"humming"})", false, std::nullopt},
        {"a signal that is not a string", R"({"signal":1})", true, std::nullopt},
        {"not an object", "[]", true, std::nullopt},
    };
    for (const Row& row : rows) {
        SCOPED_TRACE(row.name);
        const ClientMessage message = sendspin::server::parse_client_message(
            R"({"type":"client/state","payload":{"available":true,"source":)" + row.source + "}}",
            1);
        ASSERT_TRUE(std::holds_alternative<ClientState>(message));
        const ClientState& state = std::get<ClientState>(message);
        EXPECT_EQ(state.source_malformed, row.malformed);
        EXPECT_EQ(state.source.has_value(), !row.malformed);
        if (state.source) {
            EXPECT_EQ(state.source->signal, row.signal);
        }
    }
}

// roles/source/v1.md "Source Audio Chunks (Binary)": the ID, a big-endian int64 capture time, then
// audio, which may be empty; a message shorter than its header, or a capture time past
// MAX_SOURCE_TIMESTAMP_US either way, is not a chunk
TEST(SourceChunk, ReadsTheTimestampAndTheAudio) {
    using sendspin::server::MAX_SOURCE_TIMESTAMP_US;
    const std::vector<uint8_t> message{12,   0x00, 0x00, 0x01, 0x02, 0x03,
                                       0x04, 0x05, 0x06, 0xAA, 0xBB};
    const std::optional<sendspin::server::SourceChunk> chunk =
        sendspin::server::parse_source_chunk(message);
    ASSERT_TRUE(chunk);
    EXPECT_EQ(chunk->capture_us, 0x010203040506);
    EXPECT_EQ(std::vector<uint8_t>(chunk->audio.begin(), chunk->audio.end()),
              (std::vector<uint8_t>{0xAA, 0xBB}));
    const std::optional<sendspin::server::SourceChunk> empty =
        sendspin::server::parse_source_chunk(std::span(message).first(9));
    ASSERT_TRUE(empty);
    EXPECT_TRUE(empty->audio.empty());
    EXPECT_FALSE(sendspin::server::parse_source_chunk(std::span(message).first(8)));

    const auto at = [](int64_t capture_us) {
        std::vector<uint8_t> bytes{12};
        for (int shift = 56; shift >= 0; shift -= 8) {
            bytes.push_back(static_cast<uint8_t>(static_cast<uint64_t>(capture_us) >> shift));
        }
        return sendspin::server::parse_source_chunk(bytes).has_value();
    };
    EXPECT_TRUE(at(MAX_SOURCE_TIMESTAMP_US)) << "Control";
    EXPECT_TRUE(at(-MAX_SOURCE_TIMESTAMP_US)) << "Control";
    EXPECT_FALSE(at(MAX_SOURCE_TIMESTAMP_US + 1));
    EXPECT_FALSE(at(-MAX_SOURCE_TIMESTAMP_US - 1));
    EXPECT_FALSE(at(std::numeric_limits<int64_t>::min()));
}

// roles/source/v1.md "server/command source object"
TEST(StateBuilders, BuildTheSourceCommands) {
    using sendspin::server::SourceCommand;
    EXPECT_EQ(sendspin::server::build_source_command(SourceCommand::START),
              R"({"type":"server/command","payload":{"source":{"command":"start"}}})");
    EXPECT_EQ(sendspin::server::build_source_command(SourceCommand::STOP),
              R"({"type":"server/command","payload":{"source":{"command":"stop"}}})");
}

}  // namespace
