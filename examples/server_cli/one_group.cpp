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

#include "one_group.h"

#include <optional>
#include <string>
#include <vector>

namespace sendspin::server::examples {

void gather_into_server_group(SendspinServer& server) {
    const std::optional<GroupId> target = server.default_group();
    if (!target) {
        return;
    }
    for (const GroupInfo& group : server.groups()) {
        if (group.id == *target) {
            continue;
        }
        for (const std::string& client_id : group.client_ids) {
            // false only once the server stops, which leaves nothing to gather
            (void)server.move_client(client_id, *target);
        }
    }
}

}  // namespace sendspin::server::examples
