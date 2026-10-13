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

#include "timing_stats.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace sendspin::server::examples {

namespace {

/// Binary message IDs (messaging.md "Binary Message ID Structure")
constexpr uint8_t MESSAGE_ID_JSON = 0;
constexpr uint8_t MESSAGE_ID_AUDIO_CHUNK = 4;
/// An audio chunk's header: its ID, the big-endian timestamp and the big-endian send_ahead
constexpr size_t SEND_AHEAD_OFFSET = 9;
constexpr size_t CHUNK_HEADER_BYTES = 13;
constexpr unsigned BITS_PER_BYTE = 8;
constexpr double P50 = 0.50;
constexpr double P99 = 0.99;

/// The integer that follows `"key":` in json, or nullopt
std::optional<int64_t> json_integer(std::string_view json, std::string_view key) {
    const std::string quoted = "\"" + std::string(key) + "\":";
    const size_t at = json.find(quoted);
    if (at == std::string_view::npos) {
        return std::nullopt;
    }
    const std::string_view rest = json.substr(at + quoted.size());
    int64_t value = 0;
    const auto [end, error] = std::from_chars(rest.data(), rest.data() + rest.size(), value);
    if (error != std::errc{}) {
        return std::nullopt;
    }
    return value;
}

/// Nearest rank: the smallest value with at least fraction of the values at or below it
int64_t percentile(const std::vector<int64_t>& sorted, double fraction) {
    const auto n = static_cast<double>(sorted.size());
    const auto rank = static_cast<size_t>(std::max(1.0, std::ceil(fraction * n)));
    return sorted[std::min(rank, sorted.size()) - 1];
}

TimingStats::Percentiles summarize(std::vector<int64_t> values) {
    TimingStats::Percentiles out;
    out.count = values.size();
    if (values.empty()) {
        return out;
    }
    std::sort(values.begin(), values.end());
    out.p50 = percentile(values, P50);
    out.p99 = percentile(values, P99);
    out.min = values.front();
    out.max = values.back();
    return out;
}

}  // namespace

void TimingStats::add(const TracedMessage& message) {
    if (message.direction != TraceDirection::OUTBOUND || !message.binary ||
        message.plaintext.empty()) {
        return;
    }
    const std::lock_guard<std::mutex> lock(this->mutex_);
    auto [it, inserted] = this->connections_.try_emplace(message.connection_id);
    Connection& connection = it->second;
    if (inserted) {
        connection.first_stamp_us = message.stamp_us;
    }
    if (connection.client_id.empty()) {
        connection.client_id = message.client_id;
    }
    const std::vector<uint8_t>& bytes = message.plaintext;
    if (bytes.front() == MESSAGE_ID_AUDIO_CHUNK && bytes.size() >= CHUNK_HEADER_BYTES) {
        uint32_t send_ahead = 0;
        for (size_t i = SEND_AHEAD_OFFSET; i < CHUNK_HEADER_BYTES; ++i) {
            send_ahead = (send_ahead << BITS_PER_BYTE) | bytes[i];
        }
        ++connection.chunks;
        connection.chunk_bytes += bytes.size();
        connection.send_ahead_us.push_back(send_ahead);
        return;
    }
    if (bytes.front() != MESSAGE_ID_JSON) {
        return;
    }
    const std::string_view json(reinterpret_cast<const char*>(bytes.data()) + 1, bytes.size() - 1);
    if (json.find("\"type\":\"server/time\"") == std::string_view::npos) {
        return;
    }
    const std::optional<int64_t> received = json_integer(json, "server_received");
    const std::optional<int64_t> transmitted = json_integer(json, "server_transmitted");
    if (received && transmitted) {
        connection.holds_us.push_back(*transmitted - *received);
    }
}

void TimingStats::reset(int64_t now_us) {
    const std::lock_guard<std::mutex> lock(this->mutex_);
    this->connections_.clear();
    this->since_us_ = now_us;
}

std::vector<TimingStats::ConnectionTiming> TimingStats::report(int64_t now_us) const {
    std::vector<ConnectionTiming> out;
    const std::lock_guard<std::mutex> lock(this->mutex_);
    for (const auto& [id, connection] : this->connections_) {
        ConnectionTiming& timing = out.emplace_back();
        timing.connection_id = id;
        timing.client_id = connection.client_id;
        timing.hold_us = summarize(connection.holds_us);
        timing.chunks = connection.chunks;
        timing.chunk_bytes = connection.chunk_bytes;
        timing.send_ahead_us = summarize(connection.send_ahead_us);
        timing.window_us = now_us - std::max(this->since_us_, connection.first_stamp_us);
    }
    return out;
}

std::map<std::string, TimingStats::Percentiles> TimingStats::take_holds_by_client() {
    std::map<uint64_t, Connection> connections;
    {
        const std::lock_guard<std::mutex> lock(this->mutex_);
        connections.swap(this->connections_);
    }
    std::map<std::string, std::vector<int64_t>> holds;
    for (const auto& [id, connection] : connections) {
        if (!connection.client_id.empty()) {
            std::vector<int64_t>& values = holds[connection.client_id];
            values.insert(values.end(), connection.holds_us.begin(), connection.holds_us.end());
        }
    }
    std::map<std::string, Percentiles> out;
    for (auto& [client_id, values] : holds) {
        out[client_id] = summarize(std::move(values));
    }
    return out;
}

}  // namespace sendspin::server::examples
