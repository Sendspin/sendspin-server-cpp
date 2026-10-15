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

/// @file group_params.h
/// @brief GroupParams, the group parameters core writes into a group's host view (HostView)

#pragma once

#include <cstdint>

namespace sendspin::server {

/// @brief The group parameters core writes into the view (HostView)
struct GroupParams {
    /// `G_next`: the group lead the next anchor uses
    int64_t group_lead_us{0};
    /// `od_max`: the largest output delay over the eligible members
    int64_t output_delay_max_us{0};
    /// `W`: the write-ahead
    int64_t write_ahead_us{0};

    bool operator==(const GroupParams&) const = default;
};

}  // namespace sendspin::server
