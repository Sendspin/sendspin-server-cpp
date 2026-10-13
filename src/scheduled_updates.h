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

/// @file scheduled_updates.h
/// @brief A server/state object's current state and at most one scheduled update, as the
/// metadata and color roles define them (spec roles/metadata/v1.md "Scheduled metadata updates",
/// roles/color/v1.md "Scheduled color updates"), and the states each step sends. Pure.

#pragma once

#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace sendspin::server {

/// @brief The most a scheduled update is sent ahead of its timestamp (spec roles/metadata/v1.md
/// "Server rules for scheduled metadata", roles/color/v1.md "Server rules for scheduled colors")
constexpr int64_t SCHEDULE_AHEAD_US = 20000000;

/// @brief State is a role's server/state object with an `int64_t timestamp_us`; a
/// value-initialized one with its timestamp set is the empty state
template <typename State>
class ScheduledUpdates {
public:
    /// @brief The current state: the last set at a past or present time, or an empty one at
    /// now_us when none was
    State current(int64_t now_us) const {
        if (this->current_) {
            return *this->current_;
        }
        State empty{};
        empty.timestamp_us = now_us;
        return empty;
    }

    /// @brief The current state set, if any
    const std::optional<State>& current_set() const {
        return this->current_;
    }

    /// @brief The scheduled update while its members hold it, which a joiner gets after current()
    std::optional<State> sent_pending() const {
        if (this->pending_ && this->pending_->sent) {
            return this->pending_->state;
        }
        return std::nullopt;
    }

    /// @brief Sets state from its timestamp, returning what to send in order. Past or present: it
    /// is current and the scheduled update is dropped, as members drop it on receiving it. To
    /// come: it replaces the scheduled update, and is sent once its timestamp is at most
    /// SCHEDULE_AHEAD_US away; a sent update it replaces before then is cancelled by resending the
    /// current state.
    std::vector<State> set(State state, int64_t now_us) {
        std::vector<State> sends = this->advance(now_us);
        if (state.timestamp_us <= now_us) {
            this->current_ = std::move(state);
            this->pending_.reset();
            sends.push_back(*this->current_);
            return sends;
        }
        if (this->pending_ && this->pending_->sent &&
            state.timestamp_us - now_us > SCHEDULE_AHEAD_US) {
            sends.push_back(this->current(now_us));
        }
        this->pending_ = Pending{std::move(state), false};
        std::vector<State> due = this->advance(now_us);
        sends.insert(sends.end(), due.begin(), due.end());
        return sends;
    }

    /// @brief Drops the scheduled update; one already sent is cancelled by resending the current
    /// state
    std::vector<State> cancel(int64_t now_us) {
        std::vector<State> sends = this->advance(now_us);
        if (this->pending_) {
            if (this->pending_->sent) {
                sends.push_back(this->current(now_us));
            }
            this->pending_.reset();
        }
        return sends;
    }

    /// @brief Makes state current without sending it, dropping the scheduled update
    void replace(State state) {
        this->current_ = std::move(state);
        this->pending_.reset();
    }

    /// @brief Promotes a scheduled update whose time has come and sends one that came within
    /// SCHEDULE_AHEAD_US
    std::vector<State> advance(int64_t now_us) {
        std::vector<State> sends;
        if (!this->pending_) {
            return sends;
        }
        if (this->pending_->state.timestamp_us <= now_us) {
            // Its time came: members applied it, sent or not, as the current state
            if (!this->pending_->sent) {
                sends.push_back(this->pending_->state);
            }
            this->current_ = std::move(this->pending_->state);
            this->pending_.reset();
            return sends;
        }
        if (!this->pending_->sent &&
            this->pending_->state.timestamp_us - now_us <= SCHEDULE_AHEAD_US) {
            this->pending_->sent = true;
            sends.push_back(this->pending_->state);
        }
        return sends;
    }

    /// @brief When advance() has something to do; nullopt for never
    std::optional<int64_t> next_due_us() const {
        if (!this->pending_) {
            return std::nullopt;
        }
        if (this->pending_->sent) {
            return this->pending_->state.timestamp_us;
        }
        return this->pending_->state.timestamp_us - SCHEDULE_AHEAD_US;
    }

private:
    struct Pending {
        State state;
        bool sent{false};
    };

    std::optional<State> current_;
    std::optional<Pending> pending_;
};

}  // namespace sendspin::server
