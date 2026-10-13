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

/// @file timing_stats.h
/// @brief TimingStats, server_cli's per-connection time-reply and audio-chunk figures, taken from
/// the message trace

#pragma once

#include <sendspin/server/config.h>

#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace sendspin::server::examples {

/// @brief Collects, per connection since the last reset, the server's hold of each time reply
/// (`server_transmitted - server_received` of every outbound server/time) and the audio chunks
/// sent with their `send_ahead`. Fed from the message trace on the dispatch thread; every method
/// may be called from any thread.
class TimingStats {
public:
    struct Percentiles {
        size_t count{0};
        int64_t p50{0};
        int64_t p99{0};
        int64_t min{0};
        int64_t max{0};
    };

    struct ConnectionTiming {
        uint64_t connection_id{0};
        std::string client_id;
        /// Microseconds from the request's receive stamp to the reply's stamp
        Percentiles hold_us;
        uint64_t chunks{0};
        /// Plaintext bytes of the chunks, headers included
        uint64_t chunk_bytes{0};
        Percentiles send_ahead_us;
        /// From the reset, or the connection's first message after it, to the report
        int64_t window_us{0};
    };

    void add(const TracedMessage& message);

    /// Forgets every figure; windows start at now_us, read from SendspinServer::now_us()
    void reset(int64_t now_us);

    /// @return one entry per connection seen since the reset, by connection id
    std::vector<ConnectionTiming> report(int64_t now_us) const;

    /// @brief Forgets every figure, as reset() does
    /// @return each client's time-reply holds forgotten, over every connection of it, by client_id
    std::map<std::string, Percentiles> take_holds_by_client();

private:
    struct Connection {
        std::string client_id;
        int64_t first_stamp_us{0};
        std::vector<int64_t> holds_us;
        uint64_t chunks{0};
        uint64_t chunk_bytes{0};
        std::vector<int64_t> send_ahead_us;
    };

    mutable std::mutex mutex_;
    int64_t since_us_{0};
    std::map<uint64_t, Connection> connections_;
};

}  // namespace sendspin::server::examples
