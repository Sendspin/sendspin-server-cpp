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

/// @file timing_log.h
/// @brief TimingLog, server_cli's --timing-log: each client's time-reply hold and connection state
/// appended to a CSV file at an interval

#pragma once

#include "timing_stats.h"

#include <sendspin/server/config.h>
#include <sendspin/server/types.h>

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace sendspin::server::examples {

/// @brief The CSV header line of a timing log, newline included
std::string timing_log_header();

/// @brief One CSV row per client, newline included: unix_s (wall-clock seconds, as
/// `timestamp_unix`), then the client's client_id, name and state (`connected`, `reconnecting` or
/// `disconnected`), and its time replies and their hold in microseconds (min, p50, p99, max) from
/// holds; the hold fields are empty for a client with none
std::string timing_log_rows(double unix_s, const std::vector<ClientInfo>& clients,
                            const std::map<std::string, TimingStats::Percentiles>& holds);

/// @brief Appends, every interval and at stop(), a row per client to a CSV file, with the
/// time-reply holds of that interval only. Fed from the message trace on the dispatch thread;
/// every method may be called from any thread.
class TimingLog {
public:
    /// @param clients the clients a row is written for (SendspinServer::clients()); called on the
    /// log's thread, or the one calling append(), until stop()
    /// @param on_write_failed called on the log's thread the first time its append() fails
    TimingLog(std::string path, int64_t interval_us,
              std::function<std::vector<ClientInfo>()> clients,
              std::function<void()> on_write_failed);
    /// @brief Stops the thread and joins it, with no last rows
    ~TimingLog();
    TimingLog(const TimingLog&) = delete;
    TimingLog& operator=(const TimingLog&) = delete;

    /// @brief Opens the file, writing the header when it is empty, and starts the thread that
    /// appends a row per client every interval
    /// @return false when the file cannot be opened
    bool start();

    /// @brief Stops the thread, joins it and appends the rows of the interval in progress
    void stop();

    /// Takes the message trace, as TimingStats::add()
    void add(const TracedMessage& message);

    /// @brief Appends the rows of the interval that ends now and starts the next
    /// @return false when the file cannot be written
    bool append();

private:
    /// @return whether the thread ran
    bool stop_thread();

    std::string path_;
    int64_t interval_us_;
    std::function<std::vector<ClientInfo>()> clients_;
    std::function<void()> on_write_failed_;
    TimingStats stats_;
    /// Guards the members below and the file's writes
    std::mutex mutex_;
    std::condition_variable wake_;
    bool stopping_{false};
    bool started_{false};
    std::thread thread_;
};

}  // namespace sendspin::server::examples
