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

/// @file wav_reader.h
/// @brief A minimal RIFF/WAVE parser for the PCM a GroupInput takes, and a WAV file's samples
/// read from the file

#pragma once

#include "audio_source.h"

#include <sendspin/server/types.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>

namespace sendspin::server::examples {

/// @brief Where a WAV file's samples are and how they are laid out
struct WavInfo {
    InputFormat format;
    /// Offset of the first sample in the file
    uint64_t data_offset{0};
    /// Whole frames in the data chunk, or in what the file holds of it
    uint64_t frames{0};
    /// The file ends before the length the data chunk declares
    bool truncated{false};
};

struct WavParse {
    /// nullopt when the file is not a supported WAV file
    std::optional<WavInfo> info;
    /// Why not, when info is nullopt
    std::string error;
};

/// @brief Reads out.size() bytes at offset of a file; false when it cannot
using ReadAt = std::function<bool(uint64_t offset, std::span<uint8_t> out)>;

/// @brief Parses the header of a WAV file of size bytes, read through read: integer PCM of 16 or
/// 24 bits or 32-bit IEEE float (plain or WAVE_FORMAT_EXTENSIBLE), mono or stereo, at any rate.
/// Skips chunks other than `fmt ` and `data`, honoring their odd-length padding; reads only the
/// chunk headers and the `fmt ` chunk.
WavParse parse_wav_header(uint64_t size, const ReadAt& read);

/// @brief parse_wav_header() of a whole WAV file held in memory
WavParse parse_wav(std::span<const uint8_t> file);

/// @brief Opens a WAV file whose samples are read from the file as they are asked for
OpenedAudio open_wav_file(const std::string& path);

}  // namespace sendspin::server::examples
