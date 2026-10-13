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

/// @file no_discovery.cpp
/// @brief platform_discovery() in a build without discovery (SENDSPIN_SERVER_ENABLE_DISCOVERY off,
/// or a platform with no backend)

#include "sendspin/server/discovery.h"

namespace sendspin::server {

std::shared_ptr<DiscoveryProvider> platform_discovery() {
    return nullptr;
}

}  // namespace sendspin::server
