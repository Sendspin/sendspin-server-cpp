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

/// @file file_persistence_provider.h
/// @brief A PersistenceProvider that keeps each blob in its own file in one directory

#pragma once

#include <sendspin/server/server.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace sendspin::server::examples {

/// @brief Stores the blob under `key` in `<directory>/<key>`, readable by the owner only. Each
/// save replaces its file atomically and durably, so commit() has nothing left to do. A blob file
/// that exists but cannot be read loads as empty, never as missing.
class FilePersistenceProvider : public PersistenceProvider {
public:
    /// @param directory created on the first save when missing
    explicit FilePersistenceProvider(std::string directory);

    std::optional<std::vector<uint8_t>> load_blob(const std::string& key) override;
    bool save_blob(const std::string& key, const uint8_t* data, size_t len) override;

private:
    std::string path_of(const std::string& key) const;

    std::string directory_;
};

}  // namespace sendspin::server::examples
