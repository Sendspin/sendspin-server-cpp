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

#include "timing_log.h"

#include <chrono>
#include <cstdio>
#include <string_view>
#include <utility>

namespace sendspin::server::examples {

namespace {

/// A CSV field: quoted when it holds a comma or a quote, control characters replaced
std::string csv_field(std::string_view text) {
    constexpr unsigned char FIRST_PRINTABLE = 0x20;
    constexpr unsigned char DELETE = 0x7F;
    std::string out;
    bool quote = false;
    for (const char c : text) {
        const auto byte = static_cast<unsigned char>(c);
        if (byte < FIRST_PRINTABLE || byte == DELETE) {
            out += '?';
            continue;
        }
        quote = quote || c == ',' || c == '"';
        if (c == '"') {
            out += '"';
        }
        out += c;
    }
    return quote ? "\"" + out + "\"" : out;
}

const char* state_of(const ClientInfo& client) {
    if (client.connected) {
        return "connected";
    }
    return client.reconnecting ? "reconnecting" : "disconnected";
}

}  // namespace

std::string timing_log_header() {
    return "timestamp_unix,client_id,name,state,time_replies,hold_min_us,hold_p50_us,hold_p99_us,"
           "hold_max_us\n";
}

std::string timing_log_rows(double unix_s, const std::vector<ClientInfo>& clients,
                            const std::map<std::string, TimingStats::Percentiles>& holds) {
    char stamp[32];
    std::snprintf(stamp, sizeof(stamp), "%.6f", unix_s);
    std::string rows;
    for (const ClientInfo& client : clients) {
        rows += stamp;
        rows += ',' + csv_field(client.client_id) + ',' + csv_field(client.name) + ',' +
                state_of(client) + ',';
        const auto found = holds.find(client.client_id);
        if (client.client_id.empty() || found == holds.end() || found->second.count == 0) {
            rows += "0,,,,\n";
            continue;
        }
        const TimingStats::Percentiles& hold = found->second;
        rows += std::to_string(hold.count) + ',' + std::to_string(hold.min) + ',' +
                std::to_string(hold.p50) + ',' + std::to_string(hold.p99) + ',' +
                std::to_string(hold.max) + '\n';
    }
    return rows;
}

TimingLog::TimingLog(std::string path, int64_t interval_us,
                     std::function<std::vector<ClientInfo>()> clients,
                     std::function<void()> on_write_failed)
    : path_(std::move(path)),
      interval_us_(interval_us),
      clients_(std::move(clients)),
      on_write_failed_(std::move(on_write_failed)) {}

TimingLog::~TimingLog() {
    this->stop_thread();
}

bool TimingLog::start() {
    std::FILE* file = std::fopen(this->path_.c_str(), "a");
    if (file == nullptr) {
        return false;
    }
    bool written = true;
    // An append stream's position before its first write is the implementation's choice
    if (std::fseek(file, 0, SEEK_END) == 0 && std::ftell(file) == 0) {
        const std::string header = timing_log_header();
        written = std::fputs(header.c_str(), file) >= 0;
    }
    written = std::fclose(file) == 0 && written;
    if (!written) {
        return false;
    }
    const std::lock_guard<std::mutex> lock(this->mutex_);
    this->started_ = true;
    this->thread_ = std::thread([this] {
        bool failed = false;
        std::unique_lock<std::mutex> lock(this->mutex_);
        while (!this->stopping_) {
            // Due interval_us after the last row
            const bool stopping =
                this->wake_.wait_for(lock, std::chrono::microseconds(this->interval_us_),
                                     [this] { return this->stopping_; });
            if (stopping) {
                break;
            }
            lock.unlock();
            if (!this->append() && !failed && this->on_write_failed_) {
                failed = true;
                this->on_write_failed_();
            }
            lock.lock();
        }
    });
    return true;
}

void TimingLog::stop() {
    if (this->stop_thread()) {
        this->append();
    }
}

bool TimingLog::stop_thread() {
    {
        const std::lock_guard<std::mutex> lock(this->mutex_);
        if (!this->started_) {
            return false;
        }
        this->started_ = false;
        this->stopping_ = true;
    }
    this->wake_.notify_all();
    this->thread_.join();
    return true;
}

void TimingLog::add(const TracedMessage& message) {
    this->stats_.add(message);
}

bool TimingLog::append() {
    const std::vector<ClientInfo> clients = this->clients_();
    const double unix_s =
        std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
    const std::string rows = timing_log_rows(unix_s, clients, this->stats_.take_holds_by_client());
    const std::lock_guard<std::mutex> lock(this->mutex_);
    std::FILE* file = std::fopen(this->path_.c_str(), "a");
    if (file == nullptr) {
        return false;
    }
    const bool written = std::fputs(rows.c_str(), file) >= 0;
    return std::fclose(file) == 0 && written;
}

}  // namespace sendspin::server::examples
