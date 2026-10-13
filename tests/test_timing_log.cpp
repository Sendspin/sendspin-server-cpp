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

/// @file test_timing_log.cpp
/// @brief server_cli's --timing-log over traced messages built in memory: a row per client per
/// interval, with that interval's time-reply holds and the client's state, appended to a CSV file

#include "temp_dir.h"
#include "timing_log.h"
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

using sendspin::server::ClientInfo;
using sendspin::server::TraceDirection;
using sendspin::server::TracedMessage;
using sendspin::server::examples::TimingLog;
using sendspin::server::test::TempDir;

/// An hour: no row comes from the interval while a test runs
constexpr int64_t HOUR_US = int64_t{3600} * 1000000;
constexpr int64_t RECEIVED_US = 1'000'000;

/// A traced server/time reply on connection to client_id, held hold_us
TracedMessage server_time(uint64_t connection, const std::string& client_id, int64_t hold_us) {
    const std::string json =
        R"({"type":"server/time","payload":{"client_transmitted":5,"server_received":)" +
        std::to_string(RECEIVED_US) + R"(,"server_transmitted":)" +
        std::to_string(RECEIVED_US + hold_us) + "}}";
    TracedMessage message;
    message.connection_id = connection;
    message.client_id = client_id;
    message.direction = TraceDirection::OUTBOUND;
    message.binary = true;
    message.stamp_us = RECEIVED_US + hold_us;
    message.plaintext.push_back(0);
    message.plaintext.insert(message.plaintext.end(), json.begin(), json.end());
    return message;
}

ClientInfo client(const std::string& client_id, const std::string& name, bool connected,
                  bool reconnecting) {
    ClientInfo info;
    info.client_id = client_id;
    info.name = name;
    info.connected = connected;
    info.reconnecting = reconnecting;
    return info;
}

/// The file's lines, each split at its commas
std::vector<std::vector<std::string>> read_csv(const std::string& path) {
    std::vector<std::vector<std::string>> rows;
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
        std::vector<std::string>& fields = rows.emplace_back();
        std::istringstream split(line);
        std::string field;
        while (std::getline(split, field, ',')) {
            fields.push_back(field);
        }
        if (!line.empty() && line.back() == ',') {
            fields.emplace_back();
        }
    }
    return rows;
}

// Each append() writes a row per client with the holds since the one before: a client's replies
// over both its connections, none for one with none, and the state of each; stop() writes the
// interval in progress. A second log on the same file appends with no second header, and one
// destroyed without stop() appends nothing.
TEST(TimingLog, AppendsARowPerClientPerInterval) {
    TempDir dir;
    const std::string path = dir.file("timing.csv");
    const std::vector<ClientInfo> clients = {
        client("aaa", "Kitchen", true, false),
        client("bbb", "Den", false, true),
        client("ccc", "Porch", false, false),
    };
    {
        TimingLog log(path, HOUR_US, [&] { return clients; }, {});
        ASSERT_TRUE(log.start());
        for (const int64_t hold : {40, 10, 30, 20}) {
            log.add(server_time(1, "aaa", hold));
        }
        // A second connection of the same client
        log.add(server_time(2, "aaa", 50));
        ASSERT_TRUE(log.append());
        log.add(server_time(2, "aaa", 7));
        log.add(server_time(3, "bbb", 9));
        log.stop();
    }
    {
        TimingLog again(path, HOUR_US, [&] { return clients; }, {});
        ASSERT_TRUE(again.start());
        again.stop();
    }
    {
        // Its destructor writes nothing
        TimingLog unstopped(path, HOUR_US, [&] { return clients; }, {});
        ASSERT_TRUE(unstopped.start());
    }

    const std::vector<std::vector<std::string>> rows = read_csv(path);
    ASSERT_EQ(rows.size(), 1U + (3 * 3));
    EXPECT_EQ(rows[0], (std::vector<std::string>{"timestamp_unix", "client_id", "name", "state",
                                                 "time_replies", "hold_min_us", "hold_p50_us",
                                                 "hold_p99_us", "hold_max_us"}));
    for (size_t i = 1; i < rows.size(); ++i) {
        ASSERT_EQ(rows[i].size(), 9U) << i;
        // Seconds since the epoch, with microseconds
        EXPECT_EQ(rows[i][0].size() - rows[i][0].find('.'), 7U) << rows[i][0];
    }
    // The first interval
    EXPECT_EQ(rows[1], (std::vector<std::string>{rows[1][0], "aaa", "Kitchen", "connected", "5",
                                                 "10", "30", "50", "50"}));
    EXPECT_EQ(rows[2], (std::vector<std::string>{rows[1][0], "bbb", "Den", "reconnecting", "0", "",
                                                 "", "", ""}));
    EXPECT_EQ(rows[3], (std::vector<std::string>{rows[1][0], "ccc", "Porch", "disconnected", "0",
                                                 "", "", "", ""}));
    // stop()'s interval holds only what came after the first
    EXPECT_EQ(rows[4], (std::vector<std::string>{rows[4][0], "aaa", "Kitchen", "connected", "1",
                                                 "7", "7", "7", "7"}));
    EXPECT_EQ(rows[5], (std::vector<std::string>{rows[4][0], "bbb", "Den", "reconnecting", "1", "9",
                                                 "9", "9", "9"}));
    // The second log's interval had nothing
    EXPECT_EQ(rows[7][4], "0");
}

TEST(TimingLog, QuotesANameThatHoldsACommaOrAQuote) {
    const std::vector<ClientInfo> clients = {client("aaa", R"(Living "room", left)", true, false)};
    EXPECT_EQ(sendspin::server::examples::timing_log_rows(1.5, clients, {}),
              "1.500000,aaa,\"Living \"\"room\"\", left\",connected,0,,,,\n");
}

TEST(TimingLog, StartFailsForAFileItCannotWrite) {
    TempDir dir;
    TimingLog log(dir.file("missing/timing.csv"), HOUR_US, [] { return std::vector<ClientInfo>{}; },
                  {});
    EXPECT_FALSE(log.start());
}

// The log's thread appends a row per client each interval with no append() call, and reports a
// file it can no longer write once
TEST(TimingLog, AppendsEveryIntervalOnItsOwnThread) {
    TempDir dir;
    const std::string path = dir.file("timing.csv");
    const std::vector<ClientInfo> clients = {client("aaa", "Kitchen", true, false)};
    std::atomic<int> failures{0};
    TimingLog log(path, 10000, [&] { return clients; }, [&] { ++failures; });
    ASSERT_TRUE(log.start());
    // Sampled with no timeout: the suite watchdog catches a thread that never appends
    while (read_csv(path).size() < 1 + 3) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    EXPECT_EQ(failures.load(), 0);
    // A directory where the file was: every append() fails from now on
    std::filesystem::remove(path);
    std::filesystem::create_directory(path);
    while (failures.load() == 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    // A few intervals later, still reported once
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    log.stop();
    EXPECT_EQ(failures.load(), 1);
}

}  // namespace
