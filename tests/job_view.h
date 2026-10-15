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

/// @file job_view.h
/// @brief job_view_of(), a copy of what the group audio threads read of a host view

#pragma once

#include "host_view.h"

namespace sendspin::server::test {

/// @brief A copy of view's JobView, by read_job_view() into fresh storage
inline JobView job_view_of(const HostView& view) {
    JobView job;
    view.read_job_view(job);
    return job;
}

}  // namespace sendspin::server::test
