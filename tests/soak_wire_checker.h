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

/// @file soak_wire_checker.h
/// @brief The soak's check of what the server sent each player, from its message trace

#pragma once

#include "flac_reader.h"
#include <ArduinoJson.h>

#include <sendspin/server/config.h>

#include <algorithm>
#include <cstdint>
#include <deque>
#include <iterator>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <tuple>
#include <vector>

namespace sendspin::server::test {

/// A stream's format as its stream/start announced it
struct WireFormat {
    bool flac{false};
    uint32_t rate{0};
    uint32_t channels{0};
    uint32_t bits{0};
    bool opus{false};

    bool operator==(const WireFormat&) const = default;
};

/// Checks every player's traffic as the trace delivers it, on the dispatch thread: within a stream,
/// each chunk begins where the one before it ended, or later only where the frames between were
/// already late when the pump ran or the group's input is live (a live rebase); every chunk leaves
/// before its completion would count it out (`ts - od > stamp`); the first after a trigger at least
/// the player's floor past it, and within FIRST_CHUNK_WITHIN_US of it; the bytes counted by the
/// spec's accounting rule never pass the player's `buffer_capacity`, replayed from the trace (`ts +
/// duration - od > stamp`, reset by stream/clear and stream/end); and players streaming one format
/// in one group get the same audio for the same cell timestamp. The replay counts a chunk from its
/// stamp, which is before its write ends, so it does not model the spec's "transmission is
/// unfinished" term. Visualization messages go only within a visualizer stream, each before its
/// timestamp, in timestamps that never decrease within a stream (a stream/clear or a stream/start
/// from empty excepted), and within the visualizer's `buffer_capacity`, counted while `ts > stamp`
/// (spec roles/visualizer/v1.md).
class WireChecker {
public:
    static constexpr int64_t SECOND_US = 1000000;
    /// `J_min`
    static constexpr int64_t J_MIN_US = 100000;
    /// The longest a stream may wait for its first chunk after its trigger
    static constexpr int64_t FIRST_CHUNK_WITHIN_US = 2 * SECOND_US;
    /// How long after a group's input stops being live a gap still counts as a live rebase: one
    /// decided just before the switch reaches the wire a lead later
    static constexpr int64_t LIVE_MARGIN_US = 3 * SECOND_US;

    struct Stats {
        size_t chunks{0};
        size_t bytes{0};
        size_t streams{0};
        size_t clears{0};
        size_t switches{0};
        int64_t min_lead_us{INT64_MAX};
        int64_t max_lead_us{INT64_MIN};
        int64_t min_first_lead_us{INT64_MAX};
        /// Gaps where the skipped frames were late when the pump ran, outside live stretches
        size_t late_skips{0};
        size_t live_gaps{0};
        /// Streams whose first chunk came more than FIRST_CHUNK_WITHIN_US after the trigger, or
        /// never
        size_t late_starts{0};
        size_t shared_cells{0};
        /// Largest share of buffer_capacity the replayed count reached, in percent
        int64_t max_fill_percent{0};
        /// Largest output_delay_ms the client reported, in microseconds
        int64_t max_od_us{0};
        size_t visualizer_messages{0};
        size_t visualizer_streams{0};
        /// Largest share of the visualizer's buffer_capacity its count reached, in percent
        int64_t visualizer_max_fill_percent{0};
    };

    /// An active stream, or a live stretch of a group's input, on the server's clock
    struct Interval {
        int64_t begin_us{0};
        int64_t end_us{INT64_MAX};
    };

    /// A forward jump in a stream's timestamps: its chunk's stamp and timestamp, its size and its
    /// kind
    struct Gap {
        int64_t stamp_us{0};
        int64_t ts{0};
        int64_t us{0};
        std::string kind;
    };

    void on_message(const TracedMessage& message) {
        const std::lock_guard<std::mutex> lock(this->mutex_);
        if (message.direction == TraceDirection::INBOUND) {
            this->on_inbound(message);
            return;
        }
        if (!message.binary || message.plaintext.empty()) {
            return;
        }
        if (message.plaintext[0] == 0) {
            this->on_json(message);
        } else if (message.plaintext[0] == CHUNK_ID && message.plaintext.size() > HEADER_BYTES) {
            this->on_chunk(message);
        } else if (message.plaintext[0] >= FIRST_VISUALIZER_ID &&
                   message.plaintext[0] <= LAST_VISUALIZER_ID) {
            this->on_visualizer(message);
        }
    }

    /// Marks group's input live from now_us on, or buffered
    void set_live(const std::string& group, bool live, int64_t now_us) {
        const std::lock_guard<std::mutex> lock(this->mutex_);
        std::vector<Interval>& intervals = this->live_[group];
        if (live) {
            intervals.push_back({now_us, INT64_MAX});
        } else if (!intervals.empty() && intervals.back().end_us == INT64_MAX) {
            intervals.back().end_us = now_us;
        }
    }

    /// Ends the check at now_us: a stream still waiting for its first chunk then is a late start
    void finish(int64_t now_us) {
        const std::lock_guard<std::mutex> lock(this->mutex_);
        for (auto& [id, client] : this->clients_) {
            this->check_start(client, now_us);
        }
    }

    std::vector<std::string> anomalies() const {
        const std::lock_guard<std::mutex> lock(this->mutex_);
        return this->anomalies_;
    }

    /// How many anomalies of each kind
    std::map<std::string, size_t> anomaly_kinds() const {
        const std::lock_guard<std::mutex> lock(this->mutex_);
        return this->anomaly_kinds_;
    }

    size_t anomaly_count() const {
        const std::lock_guard<std::mutex> lock(this->mutex_);
        return this->anomaly_count_;
    }

    std::map<std::string, Stats> stats() const {
        const std::lock_guard<std::mutex> lock(this->mutex_);
        std::map<std::string, Stats> stats;
        for (const auto& [id, client] : this->clients_) {
            stats[id] = client.stats;
        }
        return stats;
    }

    std::vector<Interval> streams_of(const std::string& client_id) const {
        const std::lock_guard<std::mutex> lock(this->mutex_);
        const auto it = this->clients_.find(client_id);
        return it == this->clients_.end() ? std::vector<Interval>{} : it->second.intervals;
    }

    std::vector<Gap> gaps_of(const std::string& client_id) const {
        const std::lock_guard<std::mutex> lock(this->mutex_);
        const auto it = this->clients_.find(client_id);
        return it == this->clients_.end() ? std::vector<Gap>{} : it->second.gaps;
    }

    /// Mismatched cells excused as two epochs' cells on the same microsecond: one of the two
    /// clients was triggered between them
    size_t ambiguous_cells() const {
        const std::lock_guard<std::mutex> lock(this->mutex_);
        return this->ambiguous_;
    }

private:
    using Bytes = std::vector<uint8_t>;

    static constexpr uint8_t CHUNK_ID = 4;
    /// The visualizer role's binary message types (spec messaging.md "Binary Message ID
    /// Structure"), and a visualization message's type byte and timestamp
    static constexpr uint8_t FIRST_VISUALIZER_ID = 16;
    static constexpr uint8_t LAST_VISUALIZER_ID = 23;
    static constexpr size_t VISUALIZER_HEADER_BYTES = 9;
    /// Message ID, timestamp and send_ahead
    static constexpr size_t HEADER_BYTES = 13;
    static constexpr size_t MAX_ANOMALIES_KEPT = 40;
    /// A cell is kept past the write-ahead's 10 s, while every player of the group can still get it
    static constexpr int64_t CELL_KEPT_US = 30 * SECOND_US;
    static constexpr uint32_t CELLS_PER_SECOND = 50;
    static constexpr int64_t CELL_US = SECOND_US / CELLS_PER_SECOND;

    struct Entry {
        int64_t completion_us{0};
        int64_t bytes{0};
    };

    /// A client's visualizer stream as its messages told it
    struct VisualizerSide {
        uint32_t capacity{0};
        bool active{false};
        int64_t last_ts{INT64_MIN};
        /// Each message sent since the last reset: its ts and size
        std::deque<Entry> ledger;
    };

    struct Client {
        uint32_t capacity{0};
        int64_t od_us{0};
        int64_t min_buffer_us{0};
        std::string group;
        bool active{false};
        WireFormat format;
        std::optional<FlacReader> flac;
        int64_t trigger_us{0};
        int64_t trigger_od_us{0};
        int64_t trigger_min_buffer_us{0};
        /// The last trigger, stream/end or new session
        int64_t last_change_us{INT64_MIN};
        bool awaiting_first{false};
        /// Where the chunk before ended
        std::optional<int64_t> previous_end;
        std::deque<Entry> ledger;
        std::vector<Interval> intervals;
        std::vector<Gap> gaps;
        VisualizerSide visualizer;
        Stats stats;
    };

    struct CellKey {
        std::string group;
        WireFormat format;
        int64_t ts{0};

        bool operator<(const CellKey& other) const {
            return std::tie(this->group, this->format.flac, this->format.opus, this->format.rate,
                            this->format.channels, this->format.bits, this->ts) <
                   std::tie(other.group, other.format.flac, other.format.opus, other.format.rate,
                            other.format.channels, other.format.bits, other.ts);
        }
    };

    struct CellValue {
        uint64_t hash{0};
        std::string client;
        int64_t stamp_us{0};
    };

    /// The JSON of a message after its message ID
    static bool parse(const TracedMessage& message, JsonDocument& doc) {
        return message.plaintext.size() > 1 &&
               deserializeJson(doc, message.plaintext.data() + 1, message.plaintext.size() - 1) ==
                   DeserializationError::Ok;
    }

    /// Whether a stream/clear or stream/end names role; one naming none names every stream
    static bool names(const JsonDocument& doc, const char* name) {
        const JsonVariantConst roles = doc["payload"]["roles"];
        if (roles.isNull()) {
            return true;
        }
        // A range-for: ArduinoJson's iterator has no iterator_category for std::any_of
        for (const JsonVariantConst role : roles.as<JsonArrayConst>()) {
            if (role == name) {
                return true;
            }
        }
        return false;
    }

    static bool names_player(const JsonDocument& doc) {
        return names(doc, "player");
    }

    void anomaly(const std::string& client, const char* kind, const std::string& what) {
        ++this->anomaly_count_;
        ++this->anomaly_kinds_[kind];
        if (this->anomalies_.size() < MAX_ANOMALIES_KEPT) {
            this->anomalies_.push_back(client.substr(0, 8) + ": " + what);
        }
    }

    void on_inbound(const TracedMessage& message) {
        if (!message.binary || message.plaintext.empty() || message.plaintext[0] != 0) {
            return;
        }
        JsonDocument doc;
        if (!parse(message, doc)) {
            return;
        }
        Client& client = this->clients_[message.client_id];
        if (doc["type"] == "client/hello") {
            // A new connection: a new session, with a new ledger (spec player/v1.md "Player
            // Buffer Accounting")
            this->end_stream(client, message.stamp_us);
            client.capacity = doc["payload"]["player@v1_support"]["buffer_capacity"] | 0U;
            client.visualizer = {};
            client.visualizer.capacity =
                doc["payload"]["visualizer@v1_support"]["buffer_capacity"] | 0U;
            client.od_us = 0;
            client.min_buffer_us = 0;
            return;
        }
        if (doc["type"] == "client/state" && !doc["payload"]["player"].isNull()) {
            const JsonVariantConst player = doc["payload"]["player"];
            client.od_us = int64_t{player["output_delay_ms"] | 0U} * 1000;
            client.min_buffer_us = int64_t{player["min_buffer_ms"] | 0U} * 1000;
            client.stats.max_od_us = std::max(client.stats.max_od_us, client.od_us);
        }
    }

    /// A stream waiting for its first chunk at now_us past FIRST_CHUNK_WITHIN_US is a late start,
    /// counted once
    void check_start(Client& client, int64_t now_us) {
        if (client.awaiting_first && now_us - client.trigger_us > FIRST_CHUNK_WITHIN_US) {
            ++client.stats.late_starts;
            client.awaiting_first = false;
        }
    }

    void end_stream(Client& client, int64_t stamp_us) {
        this->check_start(client, stamp_us);
        if (client.active && !client.intervals.empty()) {
            client.intervals.back().end_us = stamp_us;
        }
        client.active = false;
        client.previous_end.reset();
        client.ledger.clear();
        client.awaiting_first = false;
        client.last_change_us = stamp_us;
    }

    void trigger(Client& client, int64_t stamp_us) {
        this->check_start(client, stamp_us);
        client.previous_end.reset();
        client.ledger.clear();
        client.awaiting_first = true;
        client.trigger_us = stamp_us;
        client.trigger_od_us = client.od_us;
        client.trigger_min_buffer_us = client.min_buffer_us;
        client.last_change_us = stamp_us;
    }

    void on_json(const TracedMessage& message) {
        JsonDocument doc;
        if (!parse(message, doc)) {
            return;
        }
        Client& client = this->clients_[message.client_id];
        const JsonVariantConst type = doc["type"];
        this->on_visualizer_json(client, doc);
        if (type == "group/update") {
            const std::string group = doc["payload"]["group_id"] | std::string{};
            if (!group.empty()) {
                client.group = group;
            }
        } else if (type == "stream/start" && !doc["payload"]["player"].isNull()) {
            const JsonVariantConst player = doc["payload"]["player"];
            client.format = {player["codec"] == "flac", player["sample_rate"] | 0U,
                             player["channels"] | 0U, player["bit_depth"] | 0U,
                             player["codec"] == "opus"};
            client.flac.reset();
            if (client.format.flac) {
                client.flac.emplace();
                if (!client.flac->open(player["codec_header"] | std::string{})) {
                    this->anomaly(message.client_id, "codec_header",
                                  "a codec_header that does not open");
                    client.flac.reset();
                }
            }
            if (client.active) {
                // In place, the timeline and the ledger go on
                ++client.stats.switches;
                return;
            }
            client.active = true;
            ++client.stats.streams;
            client.intervals.push_back({message.stamp_us, INT64_MAX});
            this->trigger(client, message.stamp_us);
        } else if (type == "stream/clear" && names_player(doc)) {
            ++client.stats.clears;
            this->trigger(client, message.stamp_us);
        } else if (type == "stream/end" && names_player(doc)) {
            this->end_stream(client, message.stamp_us);
        }
    }

    /// A stream message's effect on the visualizer's stream: a stream/start from empty starts it,
    /// one in place keeps its count and its order; a stream/clear resets both; a stream/end ends it
    static void on_visualizer_json(Client& client, const JsonDocument& doc) {
        VisualizerSide& visualizer = client.visualizer;
        const JsonVariantConst type = doc["type"];
        if (type == "stream/start" && !doc["payload"]["visualizer"].isNull()) {
            if (!visualizer.active) {
                visualizer.active = true;
                visualizer.last_ts = INT64_MIN;
                visualizer.ledger.clear();
                ++client.stats.visualizer_streams;
            }
        } else if ((type == "stream/clear" || type == "stream/end") && names(doc, "visualizer")) {
            visualizer.active = type == "stream/clear" && visualizer.active;
            visualizer.last_ts = INT64_MIN;
            visualizer.ledger.clear();
        }
    }

    void on_visualizer(const TracedMessage& message) {
        Client& client = this->clients_[message.client_id];
        VisualizerSide& visualizer = client.visualizer;
        const std::string& id = message.client_id;
        if (message.plaintext.size() < VISUALIZER_HEADER_BYTES) {
            this->anomaly(id, "visualizer size", "a visualization message under its header");
            return;
        }
        int64_t ts = 0;
        for (size_t i = 1; i < VISUALIZER_HEADER_BYTES; ++i) {
            ts = (ts << 8) | message.plaintext[i];
        }
        const int64_t stamp = message.stamp_us;
        const auto bytes = static_cast<int64_t>(message.plaintext.size());
        if (!visualizer.active) {
            this->anomaly(id, "visualizer outside a stream",
                          "a visualization message outside a stream");
            return;
        }
        ++client.stats.visualizer_messages;
        if (ts <= stamp) {
            this->anomaly(id, "visualizer late",
                          "a visualization message sent late: ts " + std::to_string(ts) +
                              " stamp " + std::to_string(stamp));
        }
        if (ts < visualizer.last_ts) {
            this->anomaly(id, "visualizer order", "a visualization timestamp that decreased");
        }
        visualizer.last_ts = ts;
        while (!visualizer.ledger.empty() && visualizer.ledger.front().completion_us <= stamp) {
            visualizer.ledger.pop_front();
        }
        int64_t count = 0;
        for (const Entry& entry : visualizer.ledger) {
            count += entry.bytes;
        }
        if (visualizer.capacity == 0 || count + bytes > visualizer.capacity) {
            this->anomaly(id, "visualizer over capacity",
                          "visualizer over capacity: " + std::to_string(count + bytes) + " > " +
                              std::to_string(visualizer.capacity));
        } else {
            client.stats.visualizer_max_fill_percent =
                std::max(client.stats.visualizer_max_fill_percent,
                         (count + bytes) * 100 / visualizer.capacity);
        }
        visualizer.ledger.push_back({ts, bytes});
    }

    bool live_at(const std::string& group, int64_t at_us) const {
        const auto it = this->live_.find(group);
        if (it == this->live_.end()) {
            return false;
        }
        return std::any_of(it->second.begin(), it->second.end(), [&](const Interval& interval) {
            return at_us >= interval.begin_us &&
                   (interval.end_us == INT64_MAX || at_us <= interval.end_us + LIVE_MARGIN_US);
        });
    }

    void on_chunk(const TracedMessage& message) {
        Client& client = this->clients_[message.client_id];
        const std::string& id = message.client_id;
        int64_t ts = 0;
        for (size_t i = 1; i < 1 + sizeof(int64_t); ++i) {
            ts = (ts << 8) | message.plaintext[i];
        }
        const int64_t stamp = message.stamp_us;
        const auto bytes = static_cast<int64_t>(message.plaintext.size());
        const std::span<const uint8_t> payload(message.plaintext.data() + HEADER_BYTES,
                                               message.plaintext.size() - HEADER_BYTES);
        if (!client.active || client.format.rate == 0) {
            this->anomaly(id, "outside a stream", "a chunk outside a stream");
            return;
        }

        // The chunk's samples per channel, and its PCM for the same-audio check: for Opus, one
        // packet of one cell (spec player/v1.md "Codec framing"), whose bytes stand for the cell,
        // since a packet's decoding depends on the packets its player decoded before
        Bytes pcm;
        size_t frame_bytes = size_t{client.format.channels} * (client.format.bits / 8);
        size_t cell_bytes = size_t{client.format.rate / CELLS_PER_SECOND} * frame_bytes;
        int64_t samples = 0;
        if (client.format.opus) {
            pcm.assign(payload.begin(), payload.end());
            samples = client.format.rate / CELLS_PER_SECOND;
            cell_bytes = pcm.size();
        } else if (client.format.flac) {
            if (!client.flac) {
                return;
            }
            const auto frames = client.flac->decode(payload);
            if (!frames || frames->empty()) {
                this->anomaly(id, "FLAC decode", "a FLAC chunk that does not decode");
                return;
            }
            for (const auto& frame : *frames) {
                pcm.insert(pcm.end(), frame.pcm.begin(), frame.pcm.end());
            }
            frame_bytes =
                size_t{client.flac->info().num_channels()} * client.flac->info().bytes_per_sample();
            cell_bytes = size_t{client.format.rate / CELLS_PER_SECOND} * frame_bytes;
        } else {
            pcm.assign(payload.begin(), payload.end());
        }
        if (!client.format.opus) {
            if (frame_bytes == 0 || pcm.size() % frame_bytes != 0) {
                this->anomaly(id, "part of a frame", "a chunk of part of a frame");
                return;
            }
            samples = static_cast<int64_t>(pcm.size() / frame_bytes);
        }
        const int64_t rate = client.format.rate;
        const int64_t duration = ((samples * SECOND_US) + rate - 1) / rate;

        Stats& stats = client.stats;
        ++stats.chunks;
        stats.bytes += static_cast<size_t>(bytes);
        const int64_t lead = ts - stamp;
        stats.min_lead_us = std::min(stats.min_lead_us, lead);
        stats.max_lead_us = std::max(stats.max_lead_us, lead);

        // On time: its completion still counts it when it leaves
        if (ts - client.od_us <= stamp) {
            this->anomaly(id, "late",
                          "sent late: ts " + std::to_string(ts) + " od " +
                              std::to_string(client.od_us) + " stamp " + std::to_string(stamp));
        }

        // Accounting: the replay counts no more than the ledger did, since its now was read before
        // this stamp. An entry whose completion has passed can never count again, at any output
        // delay; one excluded by the delay alone counts again when the delay falls.
        while (!client.ledger.empty() && client.ledger.front().completion_us <= stamp) {
            client.ledger.pop_front();
        }
        int64_t count = 0;
        for (const Entry& entry : client.ledger) {
            if (entry.completion_us - client.od_us > stamp) {
                count += entry.bytes;
            }
        }
        if (client.capacity == 0) {
            this->anomaly(id, "no capacity", "a chunk before the client/hello's buffer_capacity");
        } else {
            if (count + bytes > client.capacity) {
                this->anomaly(id, "over capacity",
                              "over capacity: " + std::to_string(count + bytes) + " > " +
                                  std::to_string(client.capacity));
            }
            stats.max_fill_percent =
                std::max(stats.max_fill_percent, (count + bytes) * 100 / client.capacity);
        }
        client.ledger.push_back({ts + duration, bytes});

        // The first after a trigger, then contiguity
        if (client.awaiting_first) {
            this->check_start(client, stamp);
            client.awaiting_first = false;
            const int64_t floor =
                std::max(client.trigger_min_buffer_us + client.trigger_od_us, J_MIN_US);
            const int64_t first_lead = ts - client.trigger_us;
            stats.min_first_lead_us = std::min(stats.min_first_lead_us, first_lead);
            if (first_lead < floor) {
                this->anomaly(id, "under the floor",
                              "first chunk " + std::to_string(first_lead) +
                                  " us after its trigger, under the floor " +
                                  std::to_string(floor));
            }
        } else if (client.previous_end) {
            const int64_t expected = *client.previous_end;
            if (ts < expected) {
                this->anomaly(id, "overlap",
                              "a chunk overlapping the one before by " +
                                  std::to_string(expected - ts) + " us");
            } else if (ts > expected) {
                if (this->live_at(client.group, stamp)) {
                    ++stats.live_gaps;
                    client.gaps.push_back({stamp, ts, ts - expected, "live"});
                } else if (expected - client.od_us <= stamp) {
                    ++stats.late_skips;
                    client.gaps.push_back({stamp, ts, ts - expected, "late"});
                } else {
                    this->anomaly(id, "gap",
                                  "a gap of " + std::to_string(ts - expected) +
                                      " us in a buffered stream at stamp " + std::to_string(stamp));
                }
            }
        }
        client.previous_end = ts + duration;

        this->check_cells(client, id, ts, stamp, pcm, cell_bytes);
    }

    // One format's cells in one group are one instance's, whoever gets them
    void check_cells(Client& client, const std::string& id, int64_t ts, int64_t stamp,
                     const Bytes& pcm, size_t cell_bytes) {
        if (client.group.empty() || client.format.rate % CELLS_PER_SECOND != 0) {
            return;
        }
        if (cell_bytes == 0 || pcm.size() % cell_bytes != 0) {
            return;
        }
        constexpr uint64_t FNV_OFFSET = 1469598103934665603ULL;
        constexpr uint64_t FNV_PRIME = 1099511628211ULL;
        for (size_t offset = 0, i = 0; offset < pcm.size(); offset += cell_bytes, ++i) {
            uint64_t hash = FNV_OFFSET;
            for (size_t b = offset; b < offset + cell_bytes; ++b) {
                hash = (hash ^ pcm[b]) * FNV_PRIME;
            }
            const CellKey key{client.group, client.format,
                              ts + (static_cast<int64_t>(i) * CELL_US)};
            const auto [it, added] = this->cells_.emplace(key, CellValue{hash, id, stamp});
            if (added || it->second.client == id) {
                continue;
            }
            if (it->second.hash == hash) {
                ++client.stats.shared_cells;
                continue;
            }
            // A cell of another epoch can fall on the same microsecond only when one of the two
            // clients was triggered between the two cells
            const int64_t stored = it->second.stamp_us;
            const auto other = this->clients_.find(it->second.client);
            if (client.last_change_us > stored ||
                (other != this->clients_.end() && other->second.last_change_us > stored)) {
                ++this->ambiguous_;
                continue;
            }
            this->anomaly(id, "different audio",
                          "different audio at cell " + std::to_string(key.ts) +
                              " from another player of its format");
        }
        if (++this->since_prune_ >= PRUNE_EVERY) {
            this->since_prune_ = 0;
            for (auto it = this->cells_.begin(); it != this->cells_.end();) {
                it = it->second.stamp_us < stamp - CELL_KEPT_US ? this->cells_.erase(it)
                                                                : std::next(it);
            }
        }
    }

    static constexpr size_t PRUNE_EVERY = 512;

    mutable std::mutex mutex_;
    std::map<std::string, Client> clients_;
    std::map<std::string, std::vector<Interval>> live_;
    std::map<CellKey, CellValue> cells_;
    size_t since_prune_{0};
    size_t ambiguous_{0};
    std::vector<std::string> anomalies_;
    size_t anomaly_count_{0};
    std::map<std::string, size_t> anomaly_kinds_;
};

}  // namespace sendspin::server::test
