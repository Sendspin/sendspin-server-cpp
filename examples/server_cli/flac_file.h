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

/// @file flac_file.h
/// @brief open_flac_file(): a FLAC file's PCM, decoded with micro-flac as it is read

#pragma once

#include "audio_source.h"

#include <string>

namespace sendspin::server::examples {

/// @brief Opens a native FLAC file (RFC 9639) of 16-bit or 24-bit samples, mono or stereo, whose
/// STREAMINFO gives its length. It is decoded a frame at a time as it is read: what it holds is
/// one frame's samples and a read buffer, whatever the file's length. A seek starts decoding at
/// the nearest SEEKTABLE point at or before the frame when the file has one, else at the first
/// frame (or goes on from where decoding is when the frame lies ahead), and decodes forward to the
/// frame.
OpenedAudio open_flac_file(const std::string& path);

}  // namespace sendspin::server::examples
