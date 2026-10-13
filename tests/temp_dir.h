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

/// @file temp_dir.h
/// @brief TempDir, a directory of a test's files, removed with them at the end of the test

#pragma once

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>

namespace sendspin::server::test {

/// @brief A new directory under the system's temporary directory, removed with what it holds
class TempDir {
public:
    TempDir() {
        std::string pattern =
            (std::filesystem::temp_directory_path() / "sendspin-test-XXXXXX").string();
        if (::mkdtemp(pattern.data()) == nullptr) {
            ADD_FAILURE() << "cannot create a temporary directory";
        }
        this->path_ = pattern;
    }

    ~TempDir() {
        std::error_code ignored;
        std::filesystem::remove_all(this->path_, ignored);
    }

    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    /// @brief The path of name in the directory
    std::string file(const std::string& name) const {
        return (this->path_ / name).string();
    }

private:
    std::filesystem::path path_;
};

}  // namespace sendspin::server::test
