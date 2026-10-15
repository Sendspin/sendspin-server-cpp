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

/// @file stalls.h
/// @brief A group's stall count: the rebases and late player frames since its last report, and
/// when the next report is due. A value type.

#pragma once

#include <cstdint>
#include <optional>

namespace sendspin::server::sched {

/// @brief What one stall report carries
struct StallReport {
    /// The rebases the pipelines posted (AnchorShift: the frames about to be produced had too
    /// little lead, whether the pipelines fell behind, the input came late or a joiner raised the
    /// group's lead), and their shifts summed
    uint32_t rebases{0};
    int64_t shifted_us{0};
    /// The player frames that reached their ring at or past their play time, counted per instance,
    /// so a cell late in two formats counts twice
    uint32_t late_frames{0};
};

/// @brief What StallCount::take() decided at now; neither is set when nothing is counted
struct StallTake {
    /// The counts reported, which the count starts again from
    std::optional<StallReport> report;
    /// Set when the counts are held back: when they are due
    std::optional<int64_t> due_us;
};

/// @brief The stalls counted since the last report. A report is due once STALL_REPORT_US has
/// passed since the last, or at once when none was made.
class StallCount {
public:
    void add_rebase(int64_t shifted_us);
    void add_late_frame();
    /// @brief Takes the counts when a report is due at now, else says when it will be
    StallTake take(int64_t now_us);

private:
    StallReport counted_;
    std::optional<int64_t> reported_us_;
};

}  // namespace sendspin::server::sched
