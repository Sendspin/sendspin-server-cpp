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

/// @file base64url.h
/// @brief Base64url (RFC 4648 section 5): the `-`/`_` alphabet without `=` padding; and standard
/// Base64 (RFC 4648 section 4) with padding, for encoding

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace sendspin::server {

/// @brief Encodes bytes without padding
std::string b64url_encode(const uint8_t* data, size_t len);

/// @brief Encodes bytes in standard Base64, padding included
std::string base64_encode(const uint8_t* data, size_t len);

/// @brief Decodes base64url, tolerating trailing `=` padding
/// @return nullopt for any character outside the alphabet or an impossible length
std::optional<std::vector<uint8_t>> b64url_decode(std::string_view text);

}  // namespace sendspin::server
