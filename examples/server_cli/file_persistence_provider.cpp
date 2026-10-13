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

#include "file_persistence_provider.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <system_error>
#include <utility>

namespace sendspin::server::examples {

namespace {

/// Writes all of data to fd, retrying after signals
bool write_all(int fd, const uint8_t* data, size_t len) {
    while (len > 0) {
        const ssize_t written = ::write(fd, data, len);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        data += written;
        len -= static_cast<size_t>(written);
    }
    return true;
}

/// Makes a rename into directory durable
bool sync_directory(const std::string& directory) {
    const int fd = ::open(directory.c_str(), O_RDONLY);
    if (fd < 0) {
        return false;
    }
    const bool synced = ::fsync(fd) == 0;
    ::close(fd);
    return synced;
}

}  // namespace

FilePersistenceProvider::FilePersistenceProvider(std::string directory)
    : directory_(std::move(directory)) {}

std::string FilePersistenceProvider::path_of(const std::string& key) const {
    return (std::filesystem::path(this->directory_) / key).string();
}

std::optional<std::vector<uint8_t>> FilePersistenceProvider::load_blob(const std::string& key) {
    const std::string path = this->path_of(key);
    std::error_code ec;
    if (!std::filesystem::exists(path, ec) && !ec) {
        return std::nullopt;
    }
    // A blob that exists but cannot be read loads as empty, which the server refuses, rather than
    // as missing, which would have it replaced
    std::ifstream file(path, std::ios::binary);
    if (ec || !file) {
        std::fprintf(stderr, "cannot read %s\n", path.c_str());
        return std::vector<uint8_t>{};
    }
    std::vector<uint8_t> blob{std::istreambuf_iterator<char>(file),
                              std::istreambuf_iterator<char>()};
    if (file.bad()) {
        std::fprintf(stderr, "cannot read %s\n", path.c_str());
        return std::vector<uint8_t>{};
    }
    return blob;
}

bool FilePersistenceProvider::save_blob(const std::string& key, const uint8_t* data, size_t len) {
    std::error_code ec;
    std::filesystem::create_directories(this->directory_, ec);
    if (ec) {
        std::fprintf(stderr, "cannot create %s: %s\n", this->directory_.c_str(),
                     ec.message().c_str());
        return false;
    }
    // Written beside the blob, then renamed over it, so a crash leaves the old blob or the new one
    const std::string path = this->path_of(key);
    const std::string temporary = path + ".tmp";
    const int fd = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR);
    if (fd < 0) {
        return false;
    }
    // open() applies the mode only to a new file; a leftover one may be looser
    const bool written =
        ::fchmod(fd, S_IRUSR | S_IWUSR) == 0 && write_all(fd, data, len) && ::fsync(fd) == 0;
    if (::close(fd) != 0 || !written) {
        ::unlink(temporary.c_str());
        return false;
    }
    if (std::rename(temporary.c_str(), path.c_str()) != 0) {
        ::unlink(temporary.c_str());
        return false;
    }
    return sync_directory(this->directory_);
}

}  // namespace sendspin::server::examples
