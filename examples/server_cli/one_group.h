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

/// @file one_group.h
/// @brief gather_into_server_group(), server_cli's --one-group

#pragma once

#include <sendspin/server/server.h>

namespace sendspin::server::examples {

/// @brief Moves every client SendspinServer::groups() lists in a group other than the server's
/// into the server's group (SendspinServer::move_client()), whatever moved it out: a host's move,
/// a controller's switch, or client/leave or unavailability. Called from the listener's
/// on_group_changed() and on_client_connected(), it keeps every client in the one group, across
/// reconnects; a client held with no roles (not approved, say) stays in a group of its own.
void gather_into_server_group(SendspinServer& server);

}  // namespace sendspin::server::examples
