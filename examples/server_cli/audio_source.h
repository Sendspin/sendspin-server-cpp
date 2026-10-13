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

/// @file audio_source.h
/// @brief AudioSource, the PCM of an audio file read a block at a time, and open_audio_file()

#pragma once

#include <sendspin/server/types.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace sendspin::server::examples {

/// @brief The format and length of an audio file's PCM
struct AudioInfo {
    InputFormat format;
    /// Whole frames the file holds
    uint64_t frames{0};
    /// The file ends before the length its header declares
    bool truncated{false};
};

/// @brief The bytes one frame of format takes
size_t frame_bytes(const InputFormat& format);

/// @brief A file's PCM, read from any frame on without holding the file in memory. Used by one
/// thread at a time.
class AudioSource {
public:
    virtual ~AudioSource() = default;

    virtual AudioInfo info() const = 0;

    /// @brief Reads up to frames whole frames, from frame on, into out, which holds frames *
    /// frame_bytes(info().format) bytes; a frame other than where the last read ended seeks
    /// @return the frames read: fewer only at the end of the file or when it cannot be read
    virtual size_t read(uint64_t frame, uint8_t* out, size_t frames) = 0;
};

struct OpenedAudio {
    /// nullptr when the file cannot be read or is not a supported WAV or FLAC file
    std::shared_ptr<AudioSource> source;
    /// Why not, when source is nullptr
    std::string error;
};

/// @brief Opens a WAV (parse_wav()) or FLAC (FlacFile) file, told apart by its first bytes
OpenedAudio open_audio_file(const std::string& path);

}  // namespace sendspin::server::examples
