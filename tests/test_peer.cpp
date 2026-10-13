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

/// @file test_peer.cpp
/// @brief The sendspin-cpp test peer builds against this project's noise_c, micro_flac and
/// ArduinoJson.

#include <gtest/gtest.h>

#include <sendspin/client.h>

namespace {

TEST(TestPeer, ClientWithAPlayerConstructsUnconnected) {
    sendspin::SendspinClientConfig config;
    config.name = "sendspin-server test peer";
    sendspin::SendspinClient client(config);
    // The player pulls in the peer's FLAC decoder, and so micro-flac
    client.add_player(sendspin::PlayerRoleConfig{});
    EXPECT_FALSE(client.is_connected());
}

}  // namespace
